/*
 * Copyright (c) 2013-2015 Intel Corporation, Inc.  All rights reserved.
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

#include "fi_verbs.h"


#define VERBS_IB_PREFIX "IB-0x"
#define VERBS_IWARP_FABRIC "Ethernet-iWARP"
#define VERBS_ANY_FABRIC "Any RDMA fabric"

#define VERBS_CAPS (FI_MSG | FI_RMA | FI_ATOMICS | FI_READ | FI_WRITE | \
		FI_SEND | FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE)
#define VERBS_MODE (FI_LOCAL_MR)
#define VERBS_TX_OP_FLAGS (FI_INJECT | FI_COMPLETION | FI_TRANSMIT_COMPLETE)
#define VERBS_TX_OP_FLAGS_IWARP (FI_INJECT | FI_COMPLETION)
#define VERBS_TX_MODE VERBS_MODE
#define VERBS_RX_MODE (FI_LOCAL_MR | FI_RX_CQ_DATA)
#define VERBS_MSG_ORDER (FI_ORDER_RAR | FI_ORDER_RAW | FI_ORDER_RAS | \
		FI_ORDER_WAW | FI_ORDER_WAS | FI_ORDER_SAW | FI_ORDER_SAS )


static char def_tx_ctx_size[16] = "384";
static char def_rx_ctx_size[16] = "384";
static char def_tx_iov_limit[16] = "4";
static char def_rx_iov_limit[16] = "4";
static char def_inject_size[16] = "64";

const struct fi_fabric_attr verbs_fabric_attr = {
	.prov_version		= VERBS_PROV_VERS,
};

const struct fi_domain_attr verbs_domain_attr = {
	.threading		= FI_THREAD_SAFE,
	.control_progress	= FI_PROGRESS_AUTO,
	.data_progress		= FI_PROGRESS_AUTO,
	.mr_mode		= FI_MR_BASIC,
	.mr_key_size		= sizeof_field(struct ibv_sge, lkey),
	.cq_data_size		= sizeof_field(struct ibv_send_wr, imm_data),
	.tx_ctx_cnt		= 1024,
	.rx_ctx_cnt		= 1024,
	.max_ep_tx_ctx		= 1,
	.max_ep_rx_ctx		= 1,
};

const struct fi_ep_attr verbs_ep_attr = {
	.type			= FI_EP_MSG,
	.protocol_version	= 1,
	.msg_prefix_size	= 0,
	.max_order_war_size	= 0,
	.mem_tag_format		= 0,
	.tx_ctx_cnt		= 1,
	.rx_ctx_cnt		= 1,
};

const struct fi_rx_attr verbs_rx_attr = {
	.caps			= VERBS_CAPS,
	.mode			= VERBS_RX_MODE,
	.msg_order		= VERBS_MSG_ORDER,
	.total_buffered_recv	= 0,
};

const struct fi_tx_attr verbs_tx_attr = {
	.caps			= VERBS_CAPS,
	.mode			= VERBS_TX_MODE,
	.op_flags		= VERBS_TX_OP_FLAGS,
	.msg_order		= VERBS_MSG_ORDER,
	.inject_size		= 0,
	.rma_iov_limit		= 1,
};

static struct fi_info *verbs_info = NULL;
static pthread_mutex_t verbs_info_lock = PTHREAD_MUTEX_INITIALIZER;

int fi_ibv_check_fabric_attr(const struct fi_fabric_attr *attr,
			     const struct fi_info *info)
{
	if (attr->name && strcmp(attr->name, info->fabric_attr->name)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE, "Unknown fabric name\n");
		return -FI_ENODATA;
	}

	if (attr->prov_version > info->fabric_attr->prov_version) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Unsupported provider version\n");
		return -FI_ENODATA;
	}

	return 0;
}

int fi_ibv_check_domain_attr(const struct fi_domain_attr *attr,
			     const struct fi_info *info)
{
	if (attr->name && strcmp(attr->name, info->domain_attr->name)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE, "Unknown domain name\n");
		return -FI_ENODATA;
	}

	switch (attr->threading) {
	case FI_THREAD_UNSPEC:
	case FI_THREAD_SAFE:
	case FI_THREAD_FID:
	case FI_THREAD_DOMAIN:
	case FI_THREAD_COMPLETION:
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Invalid threading model\n");
		return -FI_ENODATA;
	}

	switch (attr->control_progress) {
	case FI_PROGRESS_UNSPEC:
	case FI_PROGRESS_AUTO:
	case FI_PROGRESS_MANUAL:
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given control progress mode not supported\n");
		return -FI_ENODATA;
	}

	switch (attr->data_progress) {
	case FI_PROGRESS_UNSPEC:
	case FI_PROGRESS_AUTO:
	case FI_PROGRESS_MANUAL:
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given data progress mode not supported!\n");
		return -FI_ENODATA;
	}

	switch (attr->mr_mode) {
	case FI_MR_UNSPEC:
	case FI_MR_BASIC:
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"MR mode not supported\n");
		return -FI_ENODATA;
	}

	if (attr->mr_key_size > info->domain_attr->mr_key_size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"MR key size too large\n");
		return -FI_ENODATA;
	}

	if (attr->cq_data_size > info->domain_attr->cq_data_size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"CQ data size too large\n");
		return -FI_ENODATA;
	}

	if (attr->cq_cnt > info->domain_attr->cq_cnt) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"cq_cnt exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->ep_cnt > info->domain_attr->ep_cnt) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"ep_cnt exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->max_ep_tx_ctx > info->domain_attr->max_ep_tx_ctx) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"domain_attr: max_ep_tx_ctx exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->max_ep_rx_ctx > info->domain_attr->max_ep_rx_ctx) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"domain_attr: max_ep_rx_ctx exceeds supported size\n");
		return -FI_ENODATA;
	}

	return 0;
}

int fi_ibv_check_ep_attr(const struct fi_ep_attr *attr,
			 const struct fi_info *info)
{
	switch (attr->type) {
	case FI_EP_UNSPEC:
	case FI_EP_MSG:
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Unsupported endpoint type\n");
		return -FI_ENODATA;
	}

	switch (attr->protocol) {
	case FI_PROTO_UNSPEC:
	case FI_PROTO_RDMA_CM_IB_RC:
	case FI_PROTO_IWARP:
	case FI_PROTO_IB_UD:
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Unsupported protocol\n");
		return -FI_ENODATA;
	}

	if (attr->protocol_version > 1) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Unsupported protocol version\n");
		return -FI_ENODATA;
	}

	if (attr->max_msg_size > info->ep_attr->max_msg_size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Max message size too large\n");
		return -FI_ENODATA;
	}

	if (attr->max_order_raw_size > info->ep_attr->max_order_raw_size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"max_order_raw_size exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->max_order_war_size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"max_order_war_size exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->max_order_waw_size > info->ep_attr->max_order_waw_size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"max_order_waw_size exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->tx_ctx_cnt > info->domain_attr->max_ep_tx_ctx) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"tx_ctx_cnt exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->rx_ctx_cnt > info->domain_attr->max_ep_rx_ctx) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"rx_ctx_cnt exceeds supported size\n");
		return -FI_ENODATA;
	}

	return 0;
}

int fi_ibv_check_rx_attr(const struct fi_rx_attr *attr,
			 const struct fi_info *hints, const struct fi_info *info)
{
	uint64_t compare_mode, check_mode;

	if (attr->caps & ~(info->rx_attr->caps)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given rx_attr->caps not supported\n");
		return -FI_ENODATA;
	}

	compare_mode = attr->mode ? attr->mode : hints->mode;
	check_mode = (hints->caps & FI_RMA) ?
		     info->rx_attr->mode : VERBS_MODE;
	if ((compare_mode & check_mode) != check_mode) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given rx_attr->mode not supported\n");
		return -FI_ENODATA;
	}

	if (attr->op_flags & ~(info->rx_attr->op_flags)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given rx_attr->op_flags not supported\n");
		return -FI_ENODATA;
	}

	if (attr->msg_order & ~(info->rx_attr->msg_order)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given rx_attr->msg_order not supported\n");
		return -FI_ENODATA;
	}

	if (attr->size > info->rx_attr->size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given rx_attr->size is greater than supported\n");
		return -FI_ENODATA;
	}

	if (attr->total_buffered_recv > info->rx_attr->total_buffered_recv) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given rx_attr->total_buffered_recv exceeds supported size\n");
		return -FI_ENODATA;
	}

	if (attr->iov_limit > info->rx_attr->iov_limit) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given rx_attr->iov_limit greater than supported\n");
		return -FI_ENODATA;
	}

	return 0;
}

int fi_ibv_check_tx_attr(const struct fi_tx_attr *attr,
			 const struct fi_info *hints, const struct fi_info *info)
{
	if (attr->caps & ~(info->tx_attr->caps)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given tx_attr->caps not supported\n");
		return -FI_ENODATA;
	}

	if (((attr->mode ? attr->mode : hints->mode) &
				info->tx_attr->mode) != info->tx_attr->mode) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given tx_attr->mode not supported\n");
		return -FI_ENODATA;
	}

	if (attr->op_flags & ~(info->tx_attr->op_flags)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given tx_attr->op_flags not supported\n");
		return -FI_ENODATA;
	}

	if (attr->msg_order & ~(info->tx_attr->msg_order)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given tx_attr->msg_order not supported\n");
		return -FI_ENODATA;
	}

	if (attr->size > info->tx_attr->size) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given tx_attr->size is greater than supported\n");
		return -FI_ENODATA;
	}

	if (attr->iov_limit > info->tx_attr->iov_limit) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given tx_attr->iov_limit greater than supported\n");
		return -FI_ENODATA;
	}

	if (attr->rma_iov_limit > info->tx_attr->rma_iov_limit) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Given tx_attr->rma_iov_limit greater than supported\n");
		return -FI_ENODATA;
	}

	return 0;
}

static int fi_ibv_check_hints(const struct fi_info *hints,
		const struct fi_info *info)
{
	int ret;

	if (hints->caps & ~(info->caps)) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Unsupported capabilities\n");
		return -FI_ENODATA;
	}

	if ((hints->mode & info->mode) != info->mode) {
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE,
			"Required hints mode bits not set. Expected:0x%llx"
			" Given:0x%llx\n", info->mode, hints->mode);
		return -FI_ENODATA;
	}

	if (hints->fabric_attr) {
		ret = fi_ibv_check_fabric_attr(hints->fabric_attr, info);
		if (ret)
			return ret;
	}

	if (hints->domain_attr) {
		ret = fi_ibv_check_domain_attr(hints->domain_attr, info);
		if (ret)
			return ret;
	}

	if (hints->ep_attr) {
		ret = fi_ibv_check_ep_attr(hints->ep_attr, info);
		if (ret)
			return ret;
	}

	if (hints->rx_attr) {
		ret = fi_ibv_check_rx_attr(hints->rx_attr, hints, info);
		if (ret)
			return ret;
	}

	if (hints->tx_attr) {
		ret = fi_ibv_check_tx_attr(hints->tx_attr, hints, info);
		if (ret)
			return ret;
	}

	return 0;
}

int fi_ibv_fi_to_rai(const struct fi_info *fi, uint64_t flags,
		     struct rdma_addrinfo *rai)
{
	memset(rai, 0, sizeof *rai);
	if (flags & FI_SOURCE)
		rai->ai_flags = RAI_PASSIVE;
	if (flags & FI_NUMERICHOST)
		rai->ai_flags |= RAI_NUMERICHOST;

	rai->ai_qp_type = IBV_QPT_RC;
	rai->ai_port_space = RDMA_PS_TCP;

	if (!fi)
		return 0;

	switch(fi->addr_format) {
	case FI_SOCKADDR_IN:
		rai->ai_family = AF_INET;
		rai->ai_flags |= RAI_FAMILY;
		break;
	case FI_SOCKADDR_IN6:
		rai->ai_family = AF_INET6;
		rai->ai_flags |= RAI_FAMILY;
		break;
	case FI_SOCKADDR_IB:
		rai->ai_family = AF_IB;
		rai->ai_flags |= RAI_FAMILY;
		break;
	case FI_SOCKADDR:
		if (fi->src_addrlen) {
			rai->ai_family = ((struct sockaddr *)fi->src_addr)->sa_family;
			rai->ai_flags |= RAI_FAMILY;
		} else if (fi->dest_addrlen) {
			rai->ai_family = ((struct sockaddr *)fi->dest_addr)->sa_family;
			rai->ai_flags |= RAI_FAMILY;
		}
		break;
	case FI_FORMAT_UNSPEC:
		break;
	default:
		VERBS_INFO(FI_LOG_FABRIC, "Unknown fi->addr_format\n");
	}

	if (fi->src_addrlen) {
		if (!(rai->ai_src_addr = malloc(fi->src_addrlen)))
			return -FI_ENOMEM;
		memcpy(rai->ai_src_addr, fi->src_addr, fi->src_addrlen);
		rai->ai_src_len = fi->src_addrlen;
	}
	if (fi->dest_addrlen) {
		if (!(rai->ai_dst_addr = malloc(fi->dest_addrlen)))
			return -FI_ENOMEM;
		memcpy(rai->ai_dst_addr, fi->dest_addr, fi->dest_addrlen);
		rai->ai_dst_len = fi->dest_addrlen;
	}

	return 0;
}

static int fi_ibv_rai_to_fi(struct rdma_addrinfo *rai, struct fi_info *fi)
{
	switch(rai->ai_family) {
	case AF_INET:
		fi->addr_format = FI_SOCKADDR_IN;
		break;
	case AF_INET6:
		fi->addr_format = FI_SOCKADDR_IN6;
		break;
	case AF_IB:
		fi->addr_format = FI_SOCKADDR_IB;
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE, "Unknown rai->ai_family\n");
	}

	if (rai->ai_src_len) {
 		if (!(fi->src_addr = malloc(rai->ai_src_len)))
 			return -FI_ENOMEM;
 		memcpy(fi->src_addr, rai->ai_src_addr, rai->ai_src_len);
 		fi->src_addrlen = rai->ai_src_len;
 	}
 	if (rai->ai_dst_len) {
		if (!(fi->dest_addr = malloc(rai->ai_dst_len)))
			return -FI_ENOMEM;
 		memcpy(fi->dest_addr, rai->ai_dst_addr, rai->ai_dst_len);
 		fi->dest_addrlen = rai->ai_dst_len;
 	}

 	return 0;
}

static inline int fi_ibv_get_qp_cap(struct ibv_context *ctx,
		struct ibv_device_attr *device_attr,
		struct fi_info *info)
{
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_qp_init_attr init_attr;
	int ret = 0;

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "ibv_alloc_pd", errno);
		return -errno;
	}

	cq = ibv_create_cq(ctx, 1, NULL, NULL, 0);
	if (!cq) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "ibv_create_cq", errno);
		ret = -errno;
		goto err1;
	}


	/* TODO: serialize access to string buffers */
	fi_read_file(FI_CONF_DIR, "def_tx_ctx_size",
			def_tx_ctx_size, sizeof def_tx_ctx_size);
	fi_read_file(FI_CONF_DIR, "def_rx_ctx_size",
			def_rx_ctx_size, sizeof def_rx_ctx_size);
	fi_read_file(FI_CONF_DIR, "def_tx_iov_limit",
			def_tx_iov_limit, sizeof def_tx_iov_limit);
	fi_read_file(FI_CONF_DIR, "def_rx_iov_limit",
			def_rx_iov_limit, sizeof def_rx_iov_limit);
	fi_read_file(FI_CONF_DIR, "def_inject_size",
			def_inject_size, sizeof def_inject_size);

	memset(&init_attr, 0, sizeof init_attr);
	init_attr.send_cq = cq;
	init_attr.recv_cq = cq;
	init_attr.cap.max_send_wr = atoi(def_tx_ctx_size);
	init_attr.cap.max_recv_wr = atoi(def_rx_ctx_size);
	init_attr.cap.max_send_sge = MIN(atoi(def_tx_iov_limit), device_attr->max_sge);
	init_attr.cap.max_recv_sge = MIN(atoi(def_rx_iov_limit), device_attr->max_sge);
	init_attr.cap.max_inline_data = atoi(def_inject_size);
	init_attr.qp_type = IBV_QPT_RC;

	qp = ibv_create_qp(pd, &init_attr);
	if (!qp) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "ibv_create_qp", errno);
		ret = -errno;
		goto err2;
	}

	info->tx_attr->inject_size	= init_attr.cap.max_inline_data;
	info->tx_attr->iov_limit 	= init_attr.cap.max_send_sge;
	info->tx_attr->size	 	= init_attr.cap.max_send_wr;

	info->rx_attr->iov_limit 	= init_attr.cap.max_recv_sge;
	info->rx_attr->size	 	= init_attr.cap.max_recv_wr;

	ibv_destroy_qp(qp);
err2:
	ibv_destroy_cq(cq);
err1:
	ibv_dealloc_pd(pd);

	return ret;
}

static int fi_ibv_get_device_attrs(struct ibv_context *ctx, struct fi_info *info)
{
	struct ibv_device_attr device_attr;
	struct ibv_port_attr port_attr;
	int ret = 0;

	ret = ibv_query_device(ctx, &device_attr);
	if (ret) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "ibv_query_device", errno);
		return -errno;
	}

	info->domain_attr->cq_cnt 		= device_attr.max_cq;
	info->domain_attr->ep_cnt 		= device_attr.max_qp;
	info->domain_attr->tx_ctx_cnt 		= MIN(info->domain_attr->tx_ctx_cnt, device_attr.max_qp);
	info->domain_attr->rx_ctx_cnt 		= MIN(info->domain_attr->rx_ctx_cnt, device_attr.max_qp);
	info->domain_attr->max_ep_tx_ctx 	= device_attr.max_qp;
	info->domain_attr->max_ep_rx_ctx 	= device_attr.max_qp;

	ret = fi_ibv_get_qp_cap(ctx, &device_attr, info);
	if (ret)
		return ret;

	ret = ibv_query_port(ctx, 1, &port_attr);
	if (ret) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "ibv_query_port", errno);
		return -errno;
	}

	info->ep_attr->max_msg_size 		= port_attr.max_msg_sz;
	info->ep_attr->max_order_raw_size 	= port_attr.max_msg_sz;
	info->ep_attr->max_order_waw_size	= port_attr.max_msg_sz;

	return 0;
}

/*
 * USNIC plugs into the verbs framework, but is not a usable device.
 * Manually check for devices and fail gracefully if none are present.
 * This avoids the lower libraries (libibverbs and librdmacm) from
 * reporting error messages to stderr.
 */
static int fi_ibv_have_device(void)
{
	struct ibv_device **devs;
	struct ibv_context *verbs;
	int i, ret = 0;

	devs = ibv_get_device_list(NULL);
	if (!devs)
		return 0;

	for (i = 0; devs[i]; i++) {
		verbs = ibv_open_device(devs[i]);
		if (verbs) {
			ibv_close_device(verbs);
			ret = 1;
			break;
		}
	}

	ibv_free_device_list(devs);
	return ret;
}

static int fi_ibv_get_info_ctx(struct ibv_context *ctx, struct fi_info **info)
{
	struct fi_info *fi;
	union ibv_gid gid;
	size_t name_len;
	int ret;

	if (!(fi = fi_allocinfo()))
		return -FI_ENOMEM;

	fi->caps		= VERBS_CAPS;
	fi->mode		= VERBS_MODE;
	fi->handle		= NULL;
	*(fi->tx_attr)		= verbs_tx_attr;
	*(fi->rx_attr)		= verbs_rx_attr;
	*(fi->ep_attr)		= verbs_ep_attr;
	*(fi->domain_attr)	= verbs_domain_attr;
	*(fi->fabric_attr)	= verbs_fabric_attr;

	ret = fi_ibv_get_device_attrs(ctx, fi);
	if (ret)
		goto err;

	switch (ctx->device->transport_type) {
	case IBV_TRANSPORT_IB:
		if(ibv_query_gid(ctx, 1, 0, &gid)) {
			VERBS_INFO_ERRNO(FI_LOG_FABRIC, "ibv_query_gid", errno);
			ret = -errno;
			goto err;
		}

		name_len =  strlen(VERBS_IB_PREFIX) + INET6_ADDRSTRLEN;

		if (!(fi->fabric_attr->name = calloc(1, name_len + 1))) {
			ret = -FI_ENOMEM;
			goto err;
		}

		snprintf(fi->fabric_attr->name, name_len, VERBS_IB_PREFIX "%lx",
			 gid.global.subnet_prefix);

		fi->ep_attr->protocol = FI_PROTO_RDMA_CM_IB_RC;
		break;
	case IBV_TRANSPORT_IWARP:
		fi->fabric_attr->name = strdup(VERBS_IWARP_FABRIC);
		if (!fi->fabric_attr->name) {
			ret = -FI_ENOMEM;
			goto err;
		}

		fi->ep_attr->protocol = FI_PROTO_IWARP;
		fi->tx_attr->op_flags = VERBS_TX_OP_FLAGS_IWARP;
		break;
	default:
		FI_INFO(&fi_ibv_prov, FI_LOG_CORE, "Unknown transport type\n");
		ret = -FI_ENODATA;
		goto err;
	}

	if (!(fi->domain_attr->name = strdup(ctx->device->name))) {
		ret = -FI_ENOMEM;
		goto err;
	}

	*info = fi;
	return 0;
err:
	fi_freeinfo(fi);
	return ret;
}

int fi_ibv_init_info(void)
{
	struct ibv_context **ctx_list;
	struct fi_info *fi = NULL, *tail = NULL;
	int ret = 0, i, num_devices;

	if (verbs_info)
		return 0;

	pthread_mutex_lock(&verbs_info_lock);
	if (verbs_info)
		goto unlock;

	if (!fi_ibv_have_device()) {
		VERBS_INFO(FI_LOG_FABRIC, "No RDMA devices found\n");
		ret = -FI_ENODATA;
		goto unlock;
	}

	ctx_list = rdma_get_devices(&num_devices);
	if (!num_devices) {
		VERBS_INFO_ERRNO(FI_LOG_FABRIC, "rdma_get_devices", errno);
		ret = -errno;
		goto unlock;
	}

	for (i = 0; i < num_devices; i++) {
		ret = fi_ibv_get_info_ctx(ctx_list[i], &fi);
		if (!ret) {
			if (!verbs_info)
				verbs_info = fi;
			else
				tail->next = fi;
			tail = fi;
		}
	}

	ret = verbs_info ? 0 : ret;

	rdma_free_devices(ctx_list);
unlock:
	pthread_mutex_unlock(&verbs_info_lock);
	return ret;
}

void fi_ibv_update_info(const struct fi_info *hints, struct fi_info *info)
{
	if (hints) {
		if (hints->ep_attr) {
			if (hints->ep_attr->tx_ctx_cnt)
				info->ep_attr->tx_ctx_cnt = hints->ep_attr->tx_ctx_cnt;
			if (hints->ep_attr->rx_ctx_cnt)
				info->ep_attr->rx_ctx_cnt = hints->ep_attr->rx_ctx_cnt;
		}

		if (hints->tx_attr)
			info->tx_attr->op_flags = hints->tx_attr->op_flags;

		if (hints->rx_attr)
			info->rx_attr->op_flags = hints->rx_attr->op_flags;

		if (hints->handle)
			info->handle = hints->handle;
	} else {
		info->tx_attr->op_flags = 0;
		info->rx_attr->op_flags = 0;
	}
}


struct fi_info *fi_ibv_search_verbs_info(const char *fabric_name,
					 const char *domain_name)
{
	struct fi_info *info;

	for (info = verbs_info; info; info = info->next) {
		if ((!domain_name || !strcmp(info->domain_attr->name, domain_name)) &&
			(!fabric_name || !strcmp(info->fabric_attr->name, fabric_name))) {
			return info;
		}
	}

	return NULL;
}

static int fi_ibv_get_matching_info(struct fi_info *check_info,
		struct fi_info *hints, struct rdma_addrinfo *rai,
		struct fi_info **info)
{

	int ret;
	struct fi_info *fi, *tail;

	*info = tail = NULL;

	for (; check_info; check_info = check_info->next) {
		if (hints) {
			ret = fi_ibv_check_hints(hints, check_info);
			if (ret)
				continue;
		}

		if (!(fi = fi_dupinfo(check_info))) {
			ret = -FI_ENOMEM;
			goto err1;
		}

		ret = fi_ibv_rai_to_fi(rai, fi);
		if (ret)
			goto err2;

		fi_ibv_update_info(hints, fi);

		if (!*info)
			*info = fi;
		else
			tail->next = fi;
		tail = fi;
	}

	if (!*info)
		return -FI_ENODATA;

	return 0;
err2:
	fi_freeinfo(fi);
err1:
	fi_freeinfo(*info);
	return ret;
}

int fi_ibv_getinfo(uint32_t version, const char *node, const char *service,
		   uint64_t flags, struct fi_info *hints, struct fi_info **info)
{
	struct rdma_cm_id *id;
	struct rdma_addrinfo *rai;
	struct fi_info *check_info, *fi;
	int ret;

	ret = fi_ibv_init_info();
	if (ret)
		goto err1;

	ret = fi_ibv_create_ep(node, service, flags, hints, &rai, &id);
	if (ret)
		goto err1;

	if (id->verbs) {
		fi = fi_ibv_search_verbs_info(NULL,
				ibv_get_device_name(id->verbs->device));
		if (!fi) {
			FI_WARN(&fi_ibv_prov, FI_LOG_FABRIC, "No matching"
					"verbs_info for given verbs device\n");
			ret = -FI_ENODATA;
			goto err2;
		}

		check_info = fi_dupinfo(fi);
		if (!check_info) {
			ret = -FI_ENOMEM;
			goto err2;
		}

		ret = fi_ibv_get_matching_info(check_info, hints, rai, info);
		fi_freeinfo(check_info);
	} else {
		ret = fi_ibv_get_matching_info(verbs_info, hints, rai, info);
	}

err2:
	rdma_destroy_ep(id);
	rdma_freeaddrinfo(rai);
err1:
	if (!ret || ret == -FI_ENOMEM)
		return ret;
	else
		return -FI_ENODATA;
}

void fi_ibv_free_info(void)
{
	fi_freeinfo(verbs_info);
}