/*
 * Copyright (c) 2018 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
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
#include <rdma/fi_errno.h>
#include <ofi_prov.h>
#include "tcpx.h"

#include <sys/types.h>
#include <ofi_util.h>
#include <unistd.h>


void tcpx_srx_xfer_release(struct tcpx_rx_ctx *srx_ctx,
			   struct tcpx_xfer_entry *xfer_entry)
{
	if (xfer_entry->ep->cur_rx_entry == xfer_entry)
		xfer_entry->ep->cur_rx_entry = NULL;

	fastlock_acquire(&srx_ctx->lock);
	util_buf_release(srx_ctx->buf_pool, xfer_entry);
	fastlock_release(&srx_ctx->lock);
}

static inline void tcpx_srx_recv_init(struct tcpx_xfer_entry *recv_entry,
				      uint64_t base_flags, void *context)
{
	recv_entry->flags = base_flags | FI_MSG | FI_RECV;
	recv_entry->context = context;
}

static inline void tcpx_srx_recv_init_iov(struct tcpx_xfer_entry *recv_entry,
					  size_t count, const struct iovec *iov)
{
	recv_entry->iov_cnt = count;
	memcpy(&recv_entry->iov[0], iov, count * sizeof(*iov));
}

struct tcpx_xfer_entry *
tcpx_srx_dequeue(struct tcpx_rx_ctx *srx_ctx)
{
	struct tcpx_xfer_entry *xfer_entry;

	fastlock_acquire(&srx_ctx->lock);
	if (!slist_empty(&srx_ctx->rx_queue)) {
		xfer_entry = container_of(slist_remove_head(&srx_ctx->rx_queue),
					  struct tcpx_xfer_entry, entry);
	} else {
		xfer_entry = NULL;
	}
	fastlock_release(&srx_ctx->lock);
	return xfer_entry;
}

static ssize_t tcpx_srx_recvmsg(struct fid_ep *ep, const struct fi_msg *msg,
				uint64_t flags)
{
	struct tcpx_xfer_entry *recv_entry;
	struct tcpx_rx_ctx *srx_ctx;
	ssize_t ret = FI_SUCCESS;

	srx_ctx = container_of(ep, struct tcpx_rx_ctx, rx_fid);
	assert(msg->iov_count <= TCPX_IOV_LIMIT);

	fastlock_acquire(&srx_ctx->lock);
	recv_entry = util_buf_alloc(srx_ctx->buf_pool);
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	tcpx_srx_recv_init(recv_entry, flags, msg->context);
	tcpx_srx_recv_init_iov(recv_entry, msg->iov_count, msg->msg_iov);

	slist_insert_tail(&recv_entry->entry, &srx_ctx->rx_queue);
unlock:
	fastlock_release(&srx_ctx->lock);
	return ret;
}

static ssize_t tcpx_srx_recv(struct fid_ep *ep, void *buf, size_t len, void *desc,
			     fi_addr_t src_addr, void *context)
{
	struct tcpx_xfer_entry *recv_entry;
	struct tcpx_rx_ctx *srx_ctx;
	ssize_t ret = FI_SUCCESS;

	srx_ctx = container_of(ep, struct tcpx_rx_ctx, rx_fid);

	fastlock_acquire(&srx_ctx->lock);
	recv_entry = util_buf_alloc(srx_ctx->buf_pool);
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	tcpx_srx_recv_init(recv_entry, 0, context);
	recv_entry->iov_cnt = 1;
	recv_entry->iov[0].iov_base = buf;
	recv_entry->iov[0].iov_len = len;

	slist_insert_tail(&recv_entry->entry, &srx_ctx->rx_queue);
unlock:
	fastlock_release(&srx_ctx->lock);
	return ret;
}

static ssize_t tcpx_srx_recvv(struct fid_ep *ep, const struct iovec *iov, void **desc,
			      size_t count, fi_addr_t src_addr, void *context)
{
	struct tcpx_xfer_entry *recv_entry;
	struct tcpx_rx_ctx *srx_ctx;
	ssize_t ret = FI_SUCCESS;

	srx_ctx = container_of(ep, struct tcpx_rx_ctx, rx_fid);
	assert(count <= TCPX_IOV_LIMIT);

	fastlock_acquire(&srx_ctx->lock);
	recv_entry = util_buf_alloc(srx_ctx->buf_pool);
	if (!recv_entry) {
		ret = -FI_EAGAIN;
		goto unlock;
	}

	tcpx_srx_recv_init(recv_entry, 0, context);
	tcpx_srx_recv_init_iov(recv_entry, count, iov);

	slist_insert_tail(&recv_entry->entry, &srx_ctx->rx_queue);
unlock:
	fastlock_release(&srx_ctx->lock);
	return ret;
}

struct fi_ops_msg tcpx_srx_msg_ops = {
	.size = sizeof(struct fi_ops_msg),
	.recv = tcpx_srx_recv,
	.recvv = tcpx_srx_recvv,
	.recvmsg = tcpx_srx_recvmsg,
	.send = fi_no_msg_send,
	.sendv = fi_no_msg_sendv,
	.sendmsg = fi_no_msg_sendmsg,
	.inject = fi_no_msg_inject,
	.senddata = fi_no_msg_senddata,
	.injectdata = fi_no_msg_injectdata,
};
