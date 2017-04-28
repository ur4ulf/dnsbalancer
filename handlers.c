/* vim: set tabstop=4:softtabstop=4:shiftwidth=4:noexpandtab */

/*
 * dnsbalancer - daemon to balance UDP DNS requests over DNS servers
 * Initially created under patronage of Lanet Network
 * Programmed by Oleksandr Natalenko <oleksandr@natalenko.name>, 2015-2017
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "context.h"
#include "dns.h"
#include "pfcq.h"
#include "rb.h"
#include "utils.h"

#include "handlers.h"

int ds_wrk_acpt_handler(struct ds_fe_sk* _fe_sk, struct ds_wrk_ctx* _data)
{
	struct ds_wrk_tsk* tsk = NULL;

	pfcq_counter_inc(&_data->ctx->in_flight);

	tsk = pfcq_alloc(sizeof(struct ds_wrk_tsk));
	tsk->buf = pfcq_alloc(_data->ctx->max_pkt_size);
	tsk->addr.family = _fe_sk->fe->addr.family;
	tsk->buf_size = ds_recvfrom(_fe_sk->sk, tsk->buf, _data->ctx->max_pkt_size,
								&tsk->addr);
	if (unlikely(tsk->buf_size == -1))
	{
		ds_tsk_free(tsk);
		goto err;
	}

	tsk->type = DS_TSK_REG;
	// save frontend address separately
	// in case response will be redirected
	// through new context on reload
	tsk->orig_fe_addr = _fe_sk->fe->addr;
	tsk->orig_fe_sk = _fe_sk;

	TAILQ_INSERT_TAIL(&_data->prep_queue, tsk, tailq);
	ds_produce_u64(_data->ev_prep_fd);

	goto out;

err:
	pfcq_counter_dec(&_data->ctx->in_flight);

out:
	return 0;
}

int ds_wrk_prep_handler(int _fd, struct ds_wrk_ctx* _data)
{
	struct ds_wrk_tsk* tsk = NULL;

	ds_consume_u64(_fd);

	tsk = TAILQ_FIRST(&_data->prep_queue);
	TAILQ_REMOVE(&_data->prep_queue, tsk, tailq);

	// TODO: filtering/acls

	ds_tsk_get_fwd(tsk, _data->fwd_sk_set);
	if (unlikely(!tsk->fe_fwd))
	{
		ds_tsk_free(tsk);
		goto err;
	}

	if (unlikely(ds_tsk_buf_parse(_data, tsk, DS_PKT_REQ) == -1))
	{
		ds_tsk_free(tsk);
		goto err;
	}

	TAILQ_INSERT_TAIL(&_data->fwd_queue, tsk, tailq);
	ds_produce_u64(_data->ev_fwd_fd);

	goto out;

err:
	pfcq_counter_dec(&_data->ctx->in_flight);

out:
	return 0;
}

int ds_wrk_fwd_handler(int _fd, struct ds_wrk_ctx* _data)
{
	struct ds_wrk_tsk* tsk = NULL;

	ds_consume_u64(_fd);

	tsk = TAILQ_FIRST(&_data->fwd_queue);
	TAILQ_REMOVE(&_data->fwd_queue, tsk, tailq);

	if (unlikely(ds_send(tsk->fwd_sk->sk, tsk->buf, tsk->buf_size) == -1))
	{
		// TODO: stats
		goto out;
	}

	rb_insert(_data->tracking, (void*)tsk);

out:
	pfcq_counter_dec(&_data->ctx->in_flight);

	return 0;
}

int ds_wrk_obt_handler(struct ds_fwd_sk* _fwd_sk, struct ds_wrk_ctx* _data)
{
	struct ds_wrk_tsk* tsk = NULL;
	struct ds_wrk_tsk* found = NULL;
	struct ds_wrk_ctx* wrk_next = NULL;

	pfcq_counter_inc(&_data->ctx->in_flight);

	tsk = pfcq_alloc(sizeof(struct ds_wrk_tsk));
	tsk->buf = pfcq_alloc(_data->ctx->max_pkt_size);
	tsk->buf_size = ds_recv(_fwd_sk->sk, tsk->buf, _data->ctx->max_pkt_size);
	if (unlikely(tsk->buf_size == -1))
	{
		ds_tsk_free(tsk);
		goto err;
	}

	if (unlikely(ds_tsk_buf_parse(_data, tsk, DS_PKT_REP)) == -1)
	{
		ds_tsk_free(tsk);
		goto err;
	}

	tsk->fwd_sk = _fwd_sk;

	found = rb_find(_data->tracking, tsk);
	if (unlikely(!found))
	{
		ds_tsk_free(tsk);
		goto err;
	}

	rb_delete(_data->tracking, found);

	tsk->type = found->type;
	memcpy(tsk->buf, &found->orig_id, sizeof(uint16_t));
	tsk->addr = found->addr;
	tsk->orig_fe_sk = found->orig_fe_sk;
	tsk->orig_fe_addr = found->orig_fe_addr;
	tsk->fe_fwd = found->fe_fwd;

	pfcq_counter_reset(&found->epoch);
	ds_tsk_free(found);

	tsk->redirected = _data->ctx->redirect;
	// TODO: choose worker, RR likely
	wrk_next = tsk->redirected ? _data->ctx->ctx_next->wrks[0] : _data;

	if (likely(tsk->type == DS_TSK_REG))
	{
		// response to client query
		pfcq_spin_lock(&wrk_next->rep_queue_lock);
		TAILQ_INSERT_TAIL(&wrk_next->rep_queue, tsk, tailq);
		pfcq_spin_unlock(&wrk_next->rep_queue_lock);
		ds_produce_u64(wrk_next->ev_rep_fd);
	} else if (likely(tsk->type == DS_TSK_WDT))
	{
		// response to watchdog
		pfcq_spin_lock(&wrk_next->wdt_rep_queue_lock);
		TAILQ_INSERT_TAIL(&wrk_next->wdt_rep_queue, tsk, tailq);
		pfcq_spin_unlock(&wrk_next->wdt_rep_queue_lock);
		ds_produce_u64(wrk_next->ev_wdt_rep_fd);
	} else
		panic("Unknown task type");

	if (tsk->redirected)
	{
		pfcq_counter_dec(&_data->ctx->in_flight);
		pfcq_counter_inc(&_data->ctx->ctx_next->in_flight);
	}

	goto out;

err:
	pfcq_counter_dec(&_data->ctx->in_flight);

out:
	return 0;
}

int ds_wrk_rep_handler(int _fd, struct ds_wrk_ctx* _data)
{
	int sk = -1;
	struct ds_wrk_tsk* tsk = NULL;

	ds_consume_u64(_fd);

	pfcq_spin_lock(&_data->rep_queue_lock);
	tsk = TAILQ_FIRST(&_data->rep_queue);
	TAILQ_REMOVE(&_data->rep_queue, tsk, tailq);
	pfcq_spin_unlock(&_data->rep_queue_lock);

	if (unlikely(tsk->redirected))
	{
		struct rb_traverser iter;
		struct ds_fe_sk* cur = NULL;
		bool found = false;

		rb_t_init(&iter, _data->fe_sk_set);
		cur = rb_t_first(&iter, _data->fe_sk_set);
		do {
			if (pfcq_net_addr_cmp(&cur->fe->addr, &tsk->orig_fe_addr))
			{
				sk = cur->sk;
				found = true;
				break;
			}
		} while (likely((cur = rb_t_next(&iter)) != NULL));

		if (!found)
			goto out;
	} else
	{
		sk = tsk->orig_fe_sk->sk;
	}

	if (unlikely(ds_sendto(sk, tsk->buf, tsk->buf_size, &tsk->addr) == -1))
	{
		// TODO: stats
		__noop;
	}

out:
	ds_tsk_free(tsk);

	pfcq_counter_dec(&_data->ctx->in_flight);

	return 0;
}

int ds_wrk_exit_handler(int _fd, struct ds_wrk_ctx* _data)
{
	struct rb_traverser iter;
	struct ds_fe_sk* cur_fe_sk = NULL;

	ds_consume_u64(_fd);

	verbose("[ctx: %p, wrk: %zu/%#lx] exiting...\n", (void*)_data->ctx, _data->index, _data->id);

	rb_t_init(&iter, _data->fe_sk_set);
	cur_fe_sk = rb_t_first(&iter, _data->fe_sk_set);
	do {
		ds_close(cur_fe_sk->sk);
	} while (likely((cur_fe_sk = rb_t_next(&iter)) != NULL));

	ds_epoll_del_fd(_data->wrk_fd, _data->ctx->wdt_fd);

	_data->poll_timeo = _data->ctx->poll_timeo;

	return 0;
}

static uint64_t __ds_epoch_diff_ns(struct pfcq_counter* _epoch1,
								   struct pfcq_counter* _epoch2,
								   uint64_t _epoch_size)
{
	uint64_t e1 = 0;
	uint64_t e2 = 0;

	e1 = (uint64_t)pfcq_counter_get(_epoch1);
	e2 = (uint64_t)pfcq_counter_get(_epoch2);
	return (e2 - e1) * _epoch_size;
}

int ds_wrk_gc_handler(int _fd, struct ds_wrk_ctx* _data)
{
	struct pfcq_counter now;
	struct rb_traverser iter;
	struct rb_traverser iter_f;
	struct ds_wrk_tsk* cur = NULL;
	struct ds_wrk_tsk* cur_f = NULL;
	struct rb_table* found = NULL;

	pfcq_zero(&now, sizeof(struct timespec));
	pfcq_zero(&iter, sizeof(struct rb_traverser));
	pfcq_zero(&iter_f, sizeof(struct rb_traverser));

	ds_consume_u64(_fd);

	pfcq_counter_init(&now);
	pfcq_counter_set(&now, pfcq_counter_get(&_data->ctx->epoch));

	rb_t_init(&iter, _data->tracking);
	cur = rb_t_first(&iter, _data->tracking);
	if (likely(cur))
	{
		found = rb_create(ds_tsk_cmp, NULL, &ds_rb_allocator);

		do {
			if (unlikely(
				__ds_epoch_diff_ns(&cur->epoch,
								   &_data->ctx->epoch,
								   _data->ctx->epoch_size) > _data->ctx->req_ttl))
			{
				rb_insert(found, cur);
			}
		} while (likely((cur = rb_t_next(&iter)) != NULL));

		rb_t_init(&iter_f, found);
		cur_f = rb_t_first(&iter_f, found);
		if (likely(cur_f))
		{
			do {
				rb_delete(_data->tracking, cur_f);
			} while (likely((cur_f = rb_t_next(&iter_f)) != NULL));
		}
		rb_destroy(found, ds_rb_tsk_free);
	}

	return 0;
}

int ds_wrk_wdt_req_handler(int _fd, struct ds_wrk_ctx* _data)
{
	ldns_pkt* pkt = NULL;
	ldns_rr* rr = NULL;
	int push_res = -1;
	size_t buf_size = 0;
	struct ds_wrk_tsk* tsk = NULL;
	uint8_t* buf = NULL;
	struct ds_fwd_sk* cur_fwd_wdt_sk = NULL;
	struct rb_traverser iter;

	if (unlikely(ds_try_consume_u64(_fd) == -1 && errno == EAGAIN))
		return 0;

	rb_t_init(&iter, _data->fwd_wdt_sk_set);
	cur_fwd_wdt_sk = rb_t_first(&iter, _data->fwd_wdt_sk_set);
	do
	{
		if (unlikely(pfcq_counter_reset_if_gt(&cur_fwd_wdt_sk->fwd->wdt_pending,
											  cur_fwd_wdt_sk->fwd->wdt_tries)))
		{
			if (unlikely(cur_fwd_wdt_sk->fwd->alive))
			{
				verbose("Forwarder %s became unreachable\n", cur_fwd_wdt_sk->fwd->name);
				cur_fwd_wdt_sk->fwd->alive = false;
			}
		}

		pfcq_counter_inc(&_data->ctx->in_flight);

		pkt = ldns_pkt_new();
		if (unlikely(!pkt))
			goto fail;

		ldns_pkt_set_random_id(pkt);
		ldns_pkt_set_qr(pkt, 0);
		ldns_pkt_set_opcode(pkt, LDNS_PACKET_QUERY);
		ldns_pkt_set_tc(pkt, 0);
		ldns_pkt_set_rd(pkt, 1);
		if (unlikely(ldns_rr_new_question_frm_str(&rr, cur_fwd_wdt_sk->fwd->wdt_query, NULL, NULL) != LDNS_STATUS_OK))
			goto fail;
		push_res = ldns_pkt_push_rr(pkt, LDNS_SECTION_QUESTION, rr);
		if (unlikely(push_res != LDNS_STATUS_OK && push_res != LDNS_STATUS_EMPTY_LABEL))
			goto fail;

		tsk = pfcq_alloc(sizeof(struct ds_wrk_tsk));
		tsk->buf = pfcq_alloc(_data->ctx->max_pkt_size);
		if (unlikely(ldns_pkt2wire(&buf, pkt, &buf_size) != LDNS_STATUS_OK))
			goto fail;
		ldns_pkt_free(pkt);
		memcpy(tsk->buf, buf, buf_size);
		free(buf);
		tsk->buf_size = buf_size;

		tsk->type = DS_TSK_WDT;
		tsk->fwd_sk = cur_fwd_wdt_sk;

		if (unlikely(ds_tsk_buf_parse(_data, tsk, DS_PKT_REQ) == -1))
		{
			ds_tsk_free(tsk);
			goto fail;
		}

		pfcq_counter_inc(&tsk->fwd_sk->fwd->wdt_pending);

		TAILQ_INSERT_TAIL(&_data->fwd_queue, tsk, tailq);
		ds_produce_u64(_data->ev_fwd_fd);

		goto ok;

fail:
		pfcq_counter_dec(&_data->ctx->in_flight);

ok:
		continue;
	} while (likely((cur_fwd_wdt_sk = rb_t_next(&iter)) != NULL));

	return 0;
}

int ds_wrk_wdt_rep_handler(int _fd, struct ds_wrk_ctx* _data)
{
	struct ds_wrk_tsk* tsk = NULL;

	ds_consume_u64(_fd);

	pfcq_spin_lock(&_data->wdt_rep_queue_lock);
	tsk = TAILQ_FIRST(&_data->wdt_rep_queue);
	TAILQ_REMOVE(&_data->wdt_rep_queue, tsk, tailq);
	pfcq_spin_unlock(&_data->wdt_rep_queue_lock);

	pfcq_counter_reset(&tsk->fwd_sk->fwd->wdt_pending);
	if (unlikely(!tsk->fwd_sk->fwd->alive))
	{
		tsk->fwd_sk->fwd->alive = true;
		verbose("Forwarder %s became reachable\n", tsk->fwd_sk->fwd->name);
	}

	ds_tsk_free(tsk);

	pfcq_counter_dec(&_data->ctx->in_flight);

	return 0;
}

int ds_wrk_tk_handler(int _fd, struct ds_wrk_ctx* _data)
{
	if (unlikely(ds_try_consume_u64(_fd) == -1 && errno == EAGAIN))
		return 0;

	pfcq_counter_inc(&_data->ctx->epoch);

	return 0;
}
