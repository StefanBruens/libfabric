/*
 * Copyright (c) 2013-2017 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <malloc.h>
#include <rdma/rdma_cma.h>
#include <fi_list.h>

#include "../fi_verbs.h"
#include "verbs_utils.h"
#include "verbs_rdm.h"
#include "verbs_queuing.h"

extern struct fi_provider fi_ibv_prov;
extern struct util_buf_pool* fi_ibv_rdm_request_pool;

static struct ibv_mr *
fi_ibv_rdm_alloc_and_reg(struct fi_ibv_rdm_ep *ep,
			 void **buf, size_t size)
{
	if (!ofi_memalign((void**)buf,
			  FI_IBV_RDM_BUF_ALIGNMENT, size)) {
		memset(*buf, 0, size);
		return ibv_reg_mr(ep->domain->pd, *buf, size,
				  IBV_ACCESS_LOCAL_WRITE |
				  IBV_ACCESS_REMOTE_WRITE);
	}
	return NULL;
}

static ssize_t
fi_ibv_rdm_dereg_and_free(struct ibv_mr **mr, char **buff)
{
	ssize_t ret = FI_SUCCESS;
	if (ibv_dereg_mr(*mr)) {
		VERBS_INFO_ERRNO(FI_LOG_AV, "ibv_dereg_mr failed\n", errno);
		ret = -errno;
	}

	*mr = NULL;
	free(*buff);
	*buff = NULL;

	return ret;
}

static inline ssize_t
fi_ibv_rdm_batch_repost_receives(struct fi_ibv_rdm_conn *conn,
				 struct fi_ibv_rdm_ep *ep, int num_to_post)
{
	const size_t idx = (conn->cm_role == FI_VERBS_CM_SELF) ? 1 : 0;
	struct ibv_recv_wr *bad_wr = NULL;
	struct ibv_recv_wr wr[num_to_post];
	struct ibv_sge sge[num_to_post];
	int i, last = num_to_post - 1;

	/* IBV_WR_SEND opcode specific */
	assert((num_to_post % ep->n_buffs) == 0);

	assert(ep->eopcode == IBV_WR_SEND ||
	       ep->eopcode == IBV_WR_RDMA_WRITE_WITH_IMM);

	if (ep->eopcode == IBV_WR_SEND) {
		if (last >= 0) {
			sge[last].addr = (uint64_t)(uintptr_t)
				fi_ibv_rdm_get_rbuf(conn, ep,
						    last % ep->n_buffs);
			sge[last].length = ep->buff_len;
			sge[last].lkey = conn->r_mr->lkey;

			wr[last].wr_id = (uintptr_t) conn;
			wr[last].next = NULL;
			wr[last].sg_list = &sge[last];
			wr[last].num_sge = 1;
		}
		for (i = num_to_post - 2; i >= 0; i--) {
			sge[i].addr = (uint64_t)(uintptr_t)
				fi_ibv_rdm_get_rbuf(conn, ep,
						    i % ep->n_buffs);
			sge[i].length = ep->buff_len;
			sge[i].lkey = conn->r_mr->lkey;

			wr[i].wr_id = (uintptr_t)conn;
			wr[i].next = &wr[i + 1];
			wr[i].sg_list = &sge[i];
			wr[i].num_sge = 1;
		}
	} else {
		if (last >= 0) {
			wr[last].wr_id = (uintptr_t) conn;
			wr[last].next = NULL;
			wr[last].sg_list = &sge[last];
			wr[last].num_sge = 1;
		}
		for (i = num_to_post - 2; i >= 0; i--) {
			wr[i].wr_id = (uintptr_t)conn;
			wr[i].next = &wr[i + 1];
			wr[i].sg_list = &sge[i];
			wr[i].num_sge = 1;
		}
	}

	if (ibv_post_recv(conn->qp[idx], wr, &bad_wr) == 0) {
		conn->recv_preposted += num_to_post;
		return num_to_post;
	}

	VERBS_INFO(FI_LOG_EP_DATA, "Failed to post recv\n");
	return -FI_ENOMEM;
}

ssize_t fi_ibv_rdm_repost_receives(struct fi_ibv_rdm_conn *conn,
				   struct fi_ibv_rdm_ep *ep, int num_to_post)
{
	assert(num_to_post > 0);
	const ssize_t batch_size = ep->n_buffs * 10;

	ssize_t rest = num_to_post - (num_to_post % ep->n_buffs);
	ssize_t count = 0;
	while (rest) {
		const ssize_t batch = MIN(rest, batch_size);
		const ssize_t ret = 
			fi_ibv_rdm_batch_repost_receives(conn, ep, batch);

		if (ret < 0) {
			return ret;
		}

		count += ret;
		rest -= ret;

		assert(ret == batch);
	}

	return count;
}

static ssize_t
fi_ibv_rdm_prepare_conn_memory(struct fi_ibv_rdm_ep *ep,
			       struct fi_ibv_rdm_conn *conn)
{
	assert(conn->s_mr == NULL);
	assert(conn->r_mr == NULL);

	const size_t size = ep->buff_len * ep->n_buffs;
	conn->s_mr = fi_ibv_rdm_alloc_and_reg(ep,
				(void **) &conn->sbuf_mem_reg, size);
	if (!conn->s_mr) {
		assert(conn->s_mr);
		goto s_err;
	}

	conn->r_mr = fi_ibv_rdm_alloc_and_reg(ep,
				(void **) &conn->rbuf_mem_reg, size);
	if (!conn->r_mr) {
		assert(conn->r_mr);
		goto r_err;
	}

	conn->ack_mr = ibv_reg_mr(ep->domain->pd, &conn->sbuf_ack_status,
		sizeof(conn->sbuf_ack_status),
		IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

	if (!conn->ack_mr) {
		assert(conn->ack_mr);
		goto ack_err;
	}

	conn->rma_mr = fi_ibv_rdm_alloc_and_reg(ep,
				(void **) &conn->rmabuf_mem_reg, size);
	if (!conn->rma_mr) {
		assert(conn->rma_mr);
		goto rma_err;
	}

	fi_ibv_rdm_buffer_lists_init(conn, ep);

	return FI_SUCCESS;

/* Error handling */
rma_err:
	free(conn->rmabuf_mem_reg);
ack_err: /* 
	  * Ack buffer is a part of connection structure, freeing is not needed
	  */
r_err:
	free(conn->rbuf_mem_reg);
s_err:
	free(conn->sbuf_mem_reg);

	/* The is a lack of host or HCA memory */
	return -FI_ENOMEM;
}

static inline void
fi_ibv_rdm_tagged_init_qp_attributes(struct ibv_qp_init_attr *qp_attr,
				     struct fi_ibv_rdm_ep *ep)
{
	assert(ep->scq && ep->rcq);
	memset(qp_attr, 0, sizeof(*qp_attr));
	qp_attr->send_cq = ep->scq;
	qp_attr->recv_cq = ep->rcq;
	qp_attr->qp_type = IBV_QPT_RC;
	qp_attr->cap.max_send_wr = ep->sq_wr_depth;
	qp_attr->cap.max_recv_wr = ep->rq_wr_depth;
	qp_attr->cap.max_send_sge = 1;
	qp_attr->cap.max_recv_sge = 1;
	qp_attr->cap.max_inline_data = ep->max_inline_rc;

}

static inline int
fi_ibv_rdm_pack_cm_params(struct rdma_conn_param *cm_params,
			  struct fi_ibv_rdm_conn *conn,
			  struct fi_ibv_rdm_ep *ep)
{
	char *p;

	memset(cm_params, 0, sizeof(struct rdma_conn_param));
	cm_params->responder_resources = 2;
	cm_params->initiator_depth = 2;

	cm_params->private_data_len = FI_IBV_RDM_DFLT_ADDRLEN;

	if ((conn->cm_role != FI_VERBS_CM_SELF) && (conn->r_mr && conn->s_mr)) {
		cm_params->private_data_len += sizeof(conn->r_mr->rkey);
		cm_params->private_data_len += sizeof(conn->remote_rbuf_mem_reg);
		cm_params->private_data_len += sizeof(conn->s_mr->rkey);
		cm_params->private_data_len += sizeof(conn->remote_sbuf_mem_reg);
	}

	cm_params->private_data = calloc(1, cm_params->private_data_len);
	if (!cm_params->private_data)
		return -FI_ENOMEM;

	p = (char *) cm_params->private_data;
	memcpy(p, &ep->my_addr, FI_IBV_RDM_DFLT_ADDRLEN);
	p += FI_IBV_RDM_DFLT_ADDRLEN;

	if ((conn->cm_role != FI_VERBS_CM_SELF) && (conn->r_mr && conn->s_mr)) {
		memcpy(p, &conn->r_mr->rkey, sizeof(conn->r_mr->rkey));
		p += sizeof(conn->r_mr->rkey);
		memcpy(p, &conn->rbuf_mem_reg, sizeof(conn->rbuf_mem_reg));
		p += sizeof(conn->rbuf_mem_reg);

		memcpy(p, &conn->s_mr->rkey, sizeof(conn->s_mr->rkey));
		p += sizeof(conn->s_mr->rkey);
		memcpy(p, &conn->sbuf_mem_reg, sizeof(conn->sbuf_mem_reg));
		p += sizeof(conn->sbuf_mem_reg);
	}

	return FI_SUCCESS;
}


static inline void
fi_ibv_rdm_unpack_cm_params(struct rdma_conn_param *cm_param,
			  struct fi_ibv_rdm_conn *conn,
			  struct fi_ibv_rdm_ep *ep)
{
	char *p = (char *)cm_param->private_data;

	if (conn->cm_role == FI_VERBS_CM_SELF) {
		if (conn->r_mr && conn->s_mr) {
			memcpy(&conn->addr, &ep->my_addr,
				FI_IBV_RDM_DFLT_ADDRLEN);
			conn->remote_rbuf_rkey = conn->r_mr->rkey;
			conn->remote_rbuf_mem_reg = conn->r_mr->addr;

			conn->remote_sbuf_rkey = conn->s_mr->rkey;
			conn->remote_sbuf_mem_reg = conn->s_mr->addr;

			conn->remote_sbuf_head = (struct fi_ibv_rdm_buf *)
				conn->remote_sbuf_mem_reg;
		}
	} else {
		if (conn->state == FI_VERBS_CONN_ALLOCATED) {
			memcpy(&conn->addr, p, FI_IBV_RDM_DFLT_ADDRLEN);
		}
		p += FI_IBV_RDM_DFLT_ADDRLEN;

		conn->remote_rbuf_rkey = *(uint32_t *) (p);
		p += sizeof(conn->r_mr->rkey);
		conn->remote_rbuf_mem_reg = *(char **)(p);
		p += sizeof(conn->remote_rbuf_mem_reg);

		conn->remote_sbuf_rkey = *(uint32_t *) (p);
		p += sizeof(conn->s_mr->rkey);
		conn->remote_sbuf_mem_reg = *(char **)(p);
		p += sizeof(conn->remote_sbuf_mem_reg);

		conn->remote_sbuf_head = (struct fi_ibv_rdm_buf *)
			conn->remote_sbuf_mem_reg;
	}
}

static ssize_t
fi_ibv_rdm_process_addr_resolved(struct rdma_cm_id *id,
				 struct fi_ibv_rdm_ep *ep)
{
	ssize_t ret = FI_SUCCESS;
	struct ibv_qp_init_attr qp_attr;
	struct fi_ibv_rdm_conn *conn = id->context;

	VERBS_INFO(FI_LOG_AV, "ADDR_RESOLVED conn %p, addr %s:%u\n",
		   conn, inet_ntoa(conn->addr.sin_addr),
		   ntohs(conn->addr.sin_port));

	assert(id->verbs == ep->domain->verbs);

	fi_ibv_rdm_tagged_init_qp_attributes(&qp_attr, ep);
	if (rdma_create_qp(id, ep->domain->pd, &qp_attr)) {
		VERBS_INFO_ERRNO(FI_LOG_AV,
				 "rdma_create_qp failed\n", errno);
		return -errno;
	}

	if (conn->cm_role == FI_VERBS_CM_PASSIVE)
		goto resolve_route;

	conn->qp[0] = id->qp;
	assert(conn->id[0] == id);
	if (conn->cm_role == FI_VERBS_CM_SELF)
		goto resolve_route;

	ret = fi_ibv_rdm_prepare_conn_memory(ep, conn);
	if (ret != FI_SUCCESS)
			goto err;

	ret = fi_ibv_rdm_repost_receives(conn, ep, ep->rq_wr_depth);
	if (ret < 0) {
		VERBS_INFO(FI_LOG_AV, "repost receives failed\n");
		goto err;
	} else {
		ret = FI_SUCCESS;
	}

resolve_route:
	if (rdma_resolve_route(id, FI_IBV_RDM_CM_RESOLVEADDR_TIMEOUT)) {
		VERBS_INFO(FI_LOG_AV, "rdma_resolve_route failed\n");
		ret = -FI_EHOSTUNREACH;
		goto err;
	}

	return ret;
err:
	rdma_destroy_qp(id);
	return ret;
}

static ssize_t
fi_ibv_rdm_process_connect_request(struct rdma_cm_event *event,
				   struct fi_ibv_rdm_ep *ep)
{
	struct ibv_qp_init_attr qp_attr;
	struct rdma_conn_param cm_params;
	struct fi_ibv_rdm_av_entry *av_entry = NULL;
	struct rdma_cm_id *id = event->id;
	struct fi_ibv_rdm_conn *conn;
	ssize_t ret = FI_SUCCESS;

	char *p = (char *) event->param.conn.private_data;

	if (ep->is_closing) {
		int rej_message = 0xdeadbeef;
		if (rdma_reject(id, &rej_message, sizeof(rej_message))) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_reject\n", errno);
			ret = -errno;
			if (rdma_destroy_id(id)) {
				VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_id\n",
						 errno);
				ret = (ret == FI_SUCCESS) ? -errno : ret;
			}
		}
		assert(ret == FI_SUCCESS);
		return ret;
	}

	HASH_FIND(hh, ep->domain->rdm_cm->av_hash, p,
		  FI_IBV_RDM_DFLT_ADDRLEN, av_entry);

	if (!av_entry) {
		ret = ofi_memalign((void**)&av_entry,
				   FI_IBV_RDM_MEM_ALIGNMENT,
				   sizeof(*av_entry));
		if (ret)
			return -ret;
		memset(av_entry, 0, sizeof(*av_entry));
		memcpy(&av_entry->addr, p, FI_IBV_RDM_DFLT_ADDRLEN);

		pthread_mutex_init(&av_entry->conn_lock, NULL);

		ret = ofi_memalign((void**)&conn,
				   FI_IBV_RDM_MEM_ALIGNMENT,
				   sizeof(*conn));
		if (ret) {
			pthread_mutex_destroy(&av_entry->conn_lock);
			ofi_freealign(av_entry);
			return -ret;
		}

		memset(conn, 0, sizeof(*conn));
		conn->av_entry = av_entry;
		conn->ep = ep;
		conn->state = FI_VERBS_CONN_ALLOCATED;
		dlist_init(&conn->postponed_requests_head);
		ofi_atomic_initialize32(&conn->sends_outgoing, 0);
		fi_ibv_rdm_unpack_cm_params(&event->param.conn, conn, ep);
		fi_ibv_rdm_conn_init_cm_role(conn, ep);
		HASH_ADD(hh, av_entry->conn_hash, ep,
			 sizeof(struct fi_ibv_rdm_ep *), conn);

		VERBS_INFO(FI_LOG_AV, "CONN REQUEST, NOT found in hash, "
			   "new conn %p %d, addr %s:%u, HASH ADD\n",
			   conn, conn->cm_role, inet_ntoa(conn->addr.sin_addr),
			   ntohs(conn->addr.sin_port));

		HASH_ADD(hh, ep->domain->rdm_cm->av_hash, addr,
			 FI_IBV_RDM_DFLT_ADDRLEN, av_entry);
	} else {
		pthread_mutex_lock(&av_entry->conn_lock);
		HASH_FIND(hh, av_entry->conn_hash, &ep,
			  sizeof(struct fi_ibv_rdm_ep *), conn);
		if (!conn) {
			ret = ofi_memalign((void**)&conn,
					   FI_IBV_RDM_MEM_ALIGNMENT,
					   sizeof(*conn));
			if (ret) {
				pthread_mutex_unlock(&av_entry->conn_lock);
				return -ret;
			}
			memset(conn, 0, sizeof(*conn));
			conn->ep = ep;
			conn->av_entry = av_entry;
			dlist_init(&conn->postponed_requests_head);
			conn->state = FI_VERBS_CONN_ALLOCATED;
			memcpy(&conn->addr, &av_entry->addr, FI_IBV_RDM_DFLT_ADDRLEN);
			ofi_atomic_initialize32(&conn->sends_outgoing, 0);
			HASH_ADD(hh, av_entry->conn_hash, ep,
				 sizeof(struct fi_ibv_rdm_ep *), conn);
		}
		pthread_mutex_unlock(&av_entry->conn_lock);
		fi_ibv_rdm_conn_init_cm_role(conn, ep);
		if (conn->cm_role != FI_VERBS_CM_ACTIVE) {
			/*
			 * Do it before rdma_create_qp since that call would
			 * modify event->param.conn.private_data buffer
			 */
			fi_ibv_rdm_unpack_cm_params(&event->param.conn, conn,
						    ep);
		}

		VERBS_INFO(FI_LOG_AV,
			   "CONN REQUEST,  FOUND in hash, conn %p %d, addr %s:%u\n",
			   conn, conn->cm_role, inet_ntoa(conn->addr.sin_addr),
			   ntohs(conn->addr.sin_port));
	}

	if (conn->cm_role == FI_VERBS_CM_ACTIVE) {
		int rej_message = 0xdeadbeef;
		if (rdma_reject(id, &rej_message, sizeof(rej_message))) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_reject\n", errno);
			ret = -errno;
			if (rdma_destroy_id(id)) {
				VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_id\n",
						 errno);
				ret = (ret == FI_SUCCESS) ? -errno : ret;
			}
		}
		if (conn->state == FI_VERBS_CONN_ALLOCATED) {
			ret = fi_ibv_rdm_start_connection(ep, conn);
			if (ret != FI_SUCCESS)
				goto err;
		}
	} else {
		assert(conn->state == FI_VERBS_CONN_ALLOCATED ||
		       conn->state == FI_VERBS_CONN_STARTED);

		const size_t idx = 
			(conn->cm_role == FI_VERBS_CM_PASSIVE) ? 0 : 1;

		conn->state = FI_VERBS_CONN_STARTED;

		assert (conn->id[idx] == NULL);
		conn->id[idx] = id;

		ret = fi_ibv_rdm_prepare_conn_memory(ep, conn);
		if (ret != FI_SUCCESS)
			goto err;

		fi_ibv_rdm_tagged_init_qp_attributes(&qp_attr, ep);
		if (rdma_create_qp(id, ep->domain->pd, &qp_attr)) {
			ret = -errno;
			goto err;
		}
		conn->qp[idx] = id->qp;

		ret = fi_ibv_rdm_repost_receives(conn, ep, ep->rq_wr_depth);
		if (ret < 0) {
			VERBS_INFO(FI_LOG_AV, "repost receives failed\n");
			goto err;
		} else {
			ret = FI_SUCCESS;
		}

		id->context = conn;

		ret = fi_ibv_rdm_pack_cm_params(&cm_params, conn, ep);
		if (ret) {
			VERBS_INFO(FI_LOG_AV, "Packing of CM parameters fails, "
				   "ret = %zd\n", ret);
			goto err;
		}

		if (rdma_accept(id, &cm_params)) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_accept\n", errno);
			ret = -errno;
			goto err;
		}
		if (cm_params.private_data)
			free((void *) cm_params.private_data);
	}

	return ret;
err:
	/* ret err code is already set here, just cleanup resources */
	fi_ibv_rdm_conn_cleanup(conn);
	return ret;
}

static ssize_t
fi_ibv_rdm_process_route_resolved(struct rdma_cm_event *event,
				  struct fi_ibv_rdm_ep *ep)
{
	struct fi_ibv_rdm_conn *conn = event->id->context;
	ssize_t ret = FI_SUCCESS;

	struct rdma_conn_param cm_params;
	ret = fi_ibv_rdm_pack_cm_params(&cm_params, conn, ep);
	if (ret) {
		VERBS_INFO(FI_LOG_AV, "Packing of CM parameters fails, "
			   "ret = %zd\n", ret);
		return ret;
	}

	VERBS_INFO(FI_LOG_AV,
		"ROUTE RESOLVED, conn %p, addr %s:%u\n", conn,
		inet_ntoa(conn->addr.sin_addr), ntohs(conn->addr.sin_port));

	if (rdma_connect(event->id, &cm_params)) {
		VERBS_INFO_ERRNO(FI_LOG_AV,
				 "rdma_connect failed\n", errno);
		ret = -errno;

		free((void *)cm_params.private_data);
		assert(0);
	}

	return ret;
}

static ssize_t
fi_ibv_rdm_process_event_established(struct rdma_cm_event *event,
				     struct fi_ibv_rdm_ep *ep)
{
	struct fi_ibv_rdm_conn *conn = 
		(struct fi_ibv_rdm_conn *)event->id->context;

	if (conn->state != FI_VERBS_CONN_STARTED &&
	    conn->cm_role != FI_VERBS_CM_SELF)
	{
		VERBS_INFO(FI_LOG_AV, "state = %d, conn %p", conn->state, conn);
		assert(0 && "Wrong state");
		return -FI_ECONNABORTED;
	}

	if (conn->cm_role == FI_VERBS_CM_ACTIVE ||
	    conn->cm_role == FI_VERBS_CM_SELF)
	{
		fi_ibv_rdm_unpack_cm_params(&event->param.conn, conn, ep);
	}

	VERBS_INFO(FI_LOG_AV, "CONN ESTABLISHED, conn %p, addr %s:%u\n",
		   conn, inet_ntoa(conn->addr.sin_addr),
		   ntohs(conn->addr.sin_port));
	
	/* Do not count self twice */
	if (conn->state != FI_VERBS_CONN_ESTABLISHED) {
		ep->num_active_conns++;
		conn->state = FI_VERBS_CONN_ESTABLISHED;
	}
	return FI_SUCCESS;
}

ssize_t fi_ibv_rdm_overall_conn_cleanup(struct fi_ibv_rdm_av_entry *av_entry)
{
	struct fi_ibv_rdm_conn *conn = NULL, *tmp = NULL;
	ssize_t ret = FI_SUCCESS;
	ssize_t err = FI_SUCCESS;

	pthread_mutex_lock(&av_entry->conn_lock);
	HASH_ITER(hh, av_entry->conn_hash, conn, tmp) {
		ret = fi_ibv_rdm_conn_cleanup(conn);
		if (ret) {
			VERBS_INFO(FI_LOG_AV, "Conn cleanup failed (%zd) "
				   "for av_entry = %p", ret, av_entry);
			err = ret;
		}
	}
	pthread_mutex_unlock(&av_entry->conn_lock);

	return err;
}

ssize_t fi_ibv_rdm_conn_cleanup(struct fi_ibv_rdm_conn *conn)
{
	ssize_t ret = FI_SUCCESS;
	ssize_t err = FI_SUCCESS;

	VERBS_DBG(FI_LOG_AV, "conn %p, exp = %zu unexp = %zu\n", conn,
		     conn->exp_counter, conn->unexp_counter);

	errno = 0;
	if (conn->id[0]) {
		rdma_destroy_qp(conn->id[0]);
		if (errno) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_qp\n", errno);
			ret = -errno;
		}

		if (rdma_destroy_id(conn->id[0])) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_id\n", errno);
			if (ret == FI_SUCCESS)
				ret = -errno;
		}
		conn->id[0] = NULL;
	}

	if (conn->id[1]) {
		assert(conn->cm_role == FI_VERBS_CM_SELF);
		rdma_destroy_qp(conn->id[1]);
		if (errno) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_qp\n", errno);
			if (ret == FI_SUCCESS)
				ret = -errno;
		}
		if (rdma_destroy_id(conn->id[1])) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_id\n", errno);
			if (ret == FI_SUCCESS)
				ret = -errno;
		}
		conn->id[1] = NULL;
	}

	if (conn->s_mr) {
		err = fi_ibv_rdm_dereg_and_free(&conn->s_mr, &conn->sbuf_mem_reg);
		if ((err != FI_SUCCESS) && (ret == FI_SUCCESS)) {
			ret = err;
		}
	}
	if (conn->r_mr) {
		err = fi_ibv_rdm_dereg_and_free(&conn->r_mr, &conn->rbuf_mem_reg);
		if ((err != FI_SUCCESS) && (ret == FI_SUCCESS)) {
			ret = err;
		}
	}
	if (conn->ack_mr) {
		if (ibv_dereg_mr(conn->ack_mr)) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "ibv_dereg_mr failed\n",
					 errno);
			if (ret == FI_SUCCESS)
				ret = -errno;
		}
		conn->ack_mr = NULL;
	}

	if (conn->rma_mr) {
		err = fi_ibv_rdm_dereg_and_free(&conn->rma_mr,
						&conn->rmabuf_mem_reg);
		if ((err != FI_SUCCESS) && (ret == FI_SUCCESS)) {
			ret = err;
		}
	}

	ofi_freealign(conn);
	return ret;
}

static int fi_ibv_rdm_poll_cq(struct fi_ibv_rdm_ep *ep)
{
	int i, ret = 0;
	const int wc_count = ep->fi_scq->read_bunch_size;
	struct ibv_wc wc[wc_count];

	ret = ibv_poll_cq(ep->scq, wc_count, wc);
	for (i = 0; i < ret; ++i)
		if (fi_ibv_rdm_process_send_wc(ep, &wc[i]))
			fi_ibv_rdm_process_err_send_wc(ep, &wc[i]);

	return ret;
}

static ssize_t
fi_ibv_rdm_process_event_disconnected(struct fi_ibv_rdm_ep *ep,
				      struct rdma_cm_event *event)
{
	struct fi_ibv_rdm_conn *conn = event->id->context;
	int ret = 0;

	ep->num_active_conns--;
	conn->state = FI_VERBS_CONN_CLOSED;

	VERBS_INFO(FI_LOG_AV,
		   "Disconnected from conn %p, addr %s:%u\n",
		   conn, inet_ntoa(conn->addr.sin_addr),
		   ntohs(conn->addr.sin_port));

	/* Retrieve CQ entries from send Completion Queue if any  */
	do {
		ret = fi_ibv_rdm_poll_cq(ep);
	} while (ret > 0);

	return FI_SUCCESS;
}

static ssize_t
fi_ibv_rdm_process_event_rejected(struct fi_ibv_rdm_ep *ep,
				  struct rdma_cm_event *event)
{
	struct fi_ibv_rdm_conn *conn = event->id->context;
	ssize_t ret = FI_SUCCESS;
	const int *pdata = event->param.conn.private_data;

	if ((pdata && *pdata == 0xdeadbeef) ||
	    /* 
	     * TODO: this is a workaround of the case when private_data is not
	     * arriving from rdma_reject call on iWarp devices
	     */
	    (conn->cm_role == FI_VERBS_CM_PASSIVE &&
	     event->status == -ECONNREFUSED))
	{
		errno = 0;
		rdma_destroy_qp(event->id);
		if (errno) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_qp failed\n",
					 errno);
			ret = -errno;
		}
		if (rdma_destroy_id(event->id)) {
			VERBS_INFO_ERRNO(FI_LOG_AV, "rdma_destroy_id failed\n",
					 errno);
			if (ret == FI_SUCCESS)
				ret = -errno;
		}
		VERBS_INFO(FI_LOG_AV,
			"Rejected from conn %p, addr %s:%u, cm_role %d, status %d\n",
			conn, inet_ntoa(conn->addr.sin_addr),
			ntohs(conn->addr.sin_port),
			conn->cm_role,
			event->status);
	} else {
		VERBS_INFO(FI_LOG_AV,
			"Unexpected REJECT from conn %p, addr %s:%u, cm_role %d, "
			"msg len %d, msg %x, status %d, err %d\n",
			conn, inet_ntoa(conn->addr.sin_addr),
			ntohs(conn->addr.sin_port),
			conn->cm_role,
			event->param.conn.private_data_len,
			event->param.conn.private_data ?
			*(int *)event->param.conn.private_data : 0,
			event->status, errno);
		conn->state = FI_VERBS_CONN_REJECTED;

	}
	return ret;
}
	       
static inline void
fi_ibv_rdm_process_timewait_exit_event(struct rdma_cm_event *event,
				       struct fi_ibv_rdm_ep *ep)
{
	struct fi_ibv_rdm_conn *conn = event->id->context;
	struct fi_ibv_rdm_request *request = NULL;

	VERBS_INFO(FI_LOG_AV, "Handle TIMEWAIT Exit event "
		   "from conn %p, addr %s:%u\n",
		   conn, inet_ntoa(conn->addr.sin_addr),
		   ntohs(conn->addr.sin_port));

	/* Cleanup posted queue */
	while (NULL !=
		(request = fi_ibv_rdm_take_first_from_posted_queue(ep))) {
		request->context->internal[0] = NULL;
		FI_IBV_RDM_DBG_REQUEST("to_pool: ", request, FI_LOG_DEBUG);
		util_buf_release(fi_ibv_rdm_request_pool, request);
	}
}

static ssize_t
fi_ibv_rdm_process_event(struct rdma_cm_event *event, struct fi_ibv_rdm_ep *ep)
{
	ssize_t ret = FI_SUCCESS;
	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		ret = fi_ibv_rdm_process_addr_resolved(event->id, ep);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		ret = fi_ibv_rdm_process_route_resolved(event, ep);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		ret = fi_ibv_rdm_process_event_established(event, ep);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		ret = fi_ibv_rdm_process_event_disconnected(ep, event);
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		ret = fi_ibv_rdm_process_connect_request(event, ep);
		break;
	case RDMA_CM_EVENT_REJECTED:
		ret = fi_ibv_rdm_process_event_rejected(ep, event);
		break;
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		fi_ibv_rdm_process_timewait_exit_event(event, ep);
		ret = FI_SUCCESS;
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
		ret = -FI_EADDRNOTAVAIL;
		goto print_err;
	case RDMA_CM_EVENT_ROUTE_ERROR:
		ret = -FI_EHOSTUNREACH;
		goto print_err;
	case RDMA_CM_EVENT_CONNECT_ERROR:
		ret = -FI_ECONNREFUSED;
		goto print_err;
	case RDMA_CM_EVENT_UNREACHABLE:
		ret = -FI_EADDRNOTAVAIL;
		goto print_err;
	default:
		ret = -FI_ECONNABORTED;
print_err:
		VERBS_INFO(FI_LOG_AV, "got unexpected rdmacm event, %s\n",
			   rdma_event_str(event->event));
		break;
	}

	return ret;
}

ssize_t fi_ibv_rdm_cm_progress(struct fi_ibv_rdm_ep *ep)
{
	struct rdma_cm_event *event = NULL;
	void *data = NULL;
	ssize_t ret = FI_SUCCESS;

	if (rdma_get_cm_event(ep->domain->rdm_cm->ec, &event)) {
		if(errno == EAGAIN) {
			errno = 0;
			usleep(ep->domain->rdm_cm->cm_progress_timeout);
			return FI_SUCCESS;
		} else {
			VERBS_INFO_ERRNO(FI_LOG_AV,
					 "rdma_get_cm_event failed\n", errno);
			ret = -errno;
		}
	}

	while (ret == FI_SUCCESS && event) {
		pthread_mutex_lock(&ep->domain->rdm_cm->cm_lock);

		struct rdma_cm_event event_copy;
		memcpy(&event_copy, event, sizeof(*event));
		if (event->param.conn.private_data_len) {
			data = malloc(event->param.conn.private_data_len);
			if (!data) {
				pthread_mutex_unlock(&ep->domain->rdm_cm->cm_lock);
				ret = -FI_ENOMEM;
				break;
			}
			memcpy(data, event->param.conn.private_data,
				      event->param.conn.private_data_len);
			event_copy.param.conn.private_data = data;
			event_copy.param.conn.private_data_len =
			    event->param.conn.private_data_len;
		}
		if (rdma_ack_cm_event(event)) {
			VERBS_INFO_ERRNO(FI_LOG_AV,
					 "rdma_get_cm_event failed\n", errno);
			ret = -errno;
		}

		if (ret == FI_SUCCESS){
			ret = fi_ibv_rdm_process_event(&event_copy, ep);
		}

		free(data);
		data = NULL;

		event = NULL;

		pthread_mutex_unlock(&ep->domain->rdm_cm->cm_lock);

		if (ret != FI_SUCCESS) {
			break;
		}

		if(rdma_get_cm_event(ep->domain->rdm_cm->ec, &event)) {
			if(errno == EAGAIN) {
				errno = 0;
				usleep(ep->domain->rdm_cm->cm_progress_timeout);
				break;
			} else {
				VERBS_INFO_ERRNO(FI_LOG_AV,
						 "rdma_get_cm_event failed\n",
						 errno);
				ret = -errno;
			}
		}
	}

	return ret;
}
