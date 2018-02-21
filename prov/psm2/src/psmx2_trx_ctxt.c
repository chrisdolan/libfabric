/*
 * Copyright (c) 2013-2018 Intel Corporation. All rights reserved.
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

#include "psmx2.h"

static int psmx2_trx_ctxt_cnt = 0;

/*
 * Tx/Rx context disconnect protocol:
 *
 * TRX_CTXT disconnect REQ:
 *	args[0].u32w0	cmd
 *
 * Before a PSM2 endpoint is closed, a TRX_CTXT disconnect REQ is sent to
 * all connected peers. Each peer then calls psm2_ep_disconnet() to clean
 * up the local connection state. This allows a future endpoint with the
 * same epid to connect to the same peers.
 */

struct disconnect_args {
	psm2_ep_t	ep;
	psm2_epaddr_t	epaddr;
};

static void *disconnect_func(void *args)
{
	struct disconnect_args *disconn = args;
	psm2_error_t errors;

	FI_INFO(&psmx2_prov, FI_LOG_CORE,
		"psm2_ep: %p, epaddr: %p\n", disconn->ep, disconn->epaddr);

	psm2_ep_disconnect(disconn->ep, 1, &disconn->epaddr, NULL,
			   &errors, 5*1e9);
	free(args);
	return NULL;
}

static int psmx2_peer_match(struct dlist_entry *item, const void *arg)
{
	struct psmx2_epaddr_context *peer;

	peer = container_of(item, struct psmx2_epaddr_context, entry);
	return  (peer->epaddr == arg);
}

int psmx2_am_trx_ctxt_handler(psm2_am_token_t token, psm2_amarg_t *args,
				  int nargs, void *src, uint32_t len,
				  void *hctx)
{
	psm2_epaddr_t epaddr;
	int err = 0;
	int cmd;
	struct disconnect_args *disconn;
	pthread_t disconnect_thread;
	struct psmx2_trx_ctxt *trx_ctxt;
	trx_ctxt = (struct psmx2_trx_ctxt *)hctx;

	psm2_am_get_source(token, &epaddr);
	cmd = PSMX2_AM_GET_OP(args[0].u32w0);

	switch(cmd) {
	case PSMX2_AM_REQ_TRX_CTXT_DISCONNECT:
		/*
		 * we can't call psm2_ep_disconnect from the AM
		 * handler. instead, create a thread to do the work.
		 * the performance of this operation is not important.
		 */
		disconn = malloc(sizeof(*disconn));
		if (disconn) {
			psmx2_lock(&trx_ctxt->peer_lock, 2);
			dlist_remove_first_match(&trx_ctxt->peer_list,
						 psmx2_peer_match, epaddr);
			psmx2_unlock(&trx_ctxt->peer_lock, 2);
			disconn->ep = trx_ctxt->psm2_ep;
			disconn->epaddr = epaddr;
			pthread_create(&disconnect_thread, NULL,
				       disconnect_func, disconn);
			pthread_detach(disconnect_thread);
		}
		break;

	default:
		err = -FI_EINVAL;
		break;
	}

	return err;
}

void psmx2_trx_ctxt_disconnect_peers(struct psmx2_trx_ctxt *trx_ctxt)
{
	struct dlist_entry *item, *tmp;
	struct psmx2_epaddr_context *peer;
	struct dlist_entry peer_list;
	psm2_amarg_t arg;

	arg.u32w0 = PSMX2_AM_REQ_TRX_CTXT_DISCONNECT;

	/* use local peer_list to avoid entering AM handler while holding the lock */
	dlist_init(&peer_list);
	psmx2_lock(&trx_ctxt->peer_lock, 2);
	dlist_foreach_safe(&trx_ctxt->peer_list, item, tmp) {
		dlist_remove(item);
		dlist_insert_before(item, &peer_list);
	}
	psmx2_unlock(&trx_ctxt->peer_lock, 2);

	dlist_foreach_safe(&peer_list, item, tmp) {
		peer = container_of(item, struct psmx2_epaddr_context, entry);
		FI_INFO(&psmx2_prov, FI_LOG_CORE, "epaddr: %p\n", peer->epaddr);
		psm2_am_request_short(peer->epaddr, PSMX2_AM_TRX_CTXT_HANDLER,
				      &arg, 1, NULL, 0, 0, NULL, NULL);
		psm2_epaddr_setctxt(peer->epaddr, NULL);
		free(peer);
	}
}

void psmx2_trx_ctxt_free(struct psmx2_trx_ctxt *trx_ctxt)
{
	int err;

	if (!trx_ctxt)
		return;

	FI_INFO(&psmx2_prov, FI_LOG_CORE, "epid: %016lx\n", trx_ctxt->psm2_epid);

	if (psmx2_env.disconnect)
		psmx2_trx_ctxt_disconnect_peers(trx_ctxt);

	if (trx_ctxt->am_initialized)
		psmx2_am_fini(trx_ctxt);

#if 0
	/* AM messages could arrive after MQ is finalized, causing segfault
	 * when trying to dereference the MQ pointer. There is no mechanism
	 * to properly shutdown AM. The workaround is to keep MQ valid.
	 */
	psm2_mq_finalize(trx_ctxt->psm2_mq);
#endif

	/* workaround for:
	 * Assertion failure at psm2_ep.c:1059: ep->mctxt_master == ep
	 */
	if (psmx2_env.delay)
		sleep(psmx2_env.delay);

	if (psmx2_env.timeout)
		err = psm2_ep_close(trx_ctxt->psm2_ep, PSM2_EP_CLOSE_GRACEFUL,
				    (int64_t) psmx2_env.timeout * 1000000000LL);
	else
		err = PSM2_EP_CLOSE_TIMEOUT;

	if (err != PSM2_OK)
		psm2_ep_close(trx_ctxt->psm2_ep, PSM2_EP_CLOSE_FORCE, 0);

	util_buf_pool_destroy(trx_ctxt->am_req_pool);
	fastlock_destroy(&trx_ctxt->am_req_pool_lock);
	fastlock_destroy(&trx_ctxt->poll_lock);
	fastlock_destroy(&trx_ctxt->peer_lock);
	free(trx_ctxt);
}

struct psmx2_trx_ctxt *psmx2_trx_ctxt_alloc(struct psmx2_fid_domain *domain,
					    struct psmx2_ep_name *src_addr,
					    int sep_ctxt_idx)
{
	struct psmx2_trx_ctxt *trx_ctxt;
	struct psm2_ep_open_opts opts;
	int should_retry = 0;
	int err;

	if (psmx2_trx_ctxt_cnt >= psmx2_env.max_trx_ctxt) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE,
			"number of Tx/Rx contexts exceeds limit (%d).\n",
			psmx2_env.max_trx_ctxt);
		return NULL;
	}

	trx_ctxt = calloc(1, sizeof(*trx_ctxt));
	if (!trx_ctxt) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE,
			"failed to allocate trx_ctxt.\n");
		return NULL;
	}

	err = util_buf_pool_create(&trx_ctxt->am_req_pool,
				   sizeof(struct psmx2_am_request),
				   sizeof(void *),
				   0, /* max_cnt: unlimited */
				   64); /* chunk_cnt */
	if (err) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE,
			"failed to allocate am_req_pool.\n");
		goto err_out;
	}

	psm2_ep_open_opts_get_defaults(&opts);
	FI_INFO(&psmx2_prov, FI_LOG_CORE,
		"uuid: %s\n", psmx2_uuid_to_string(domain->fabric->uuid));

	opts.unit = src_addr ? src_addr->unit : PSMX2_DEFAULT_UNIT;
	opts.port = src_addr ? src_addr->port : PSMX2_DEFAULT_PORT;
	FI_INFO(&psmx2_prov, FI_LOG_CORE,
		"ep_open_opts: unit=%d port=%u\n", opts.unit, opts.port);

	if (opts.unit < 0 && sep_ctxt_idx >= 0) {
		should_retry = 1;
		opts.unit = sep_ctxt_idx % psmx2_env.num_devunits;
		FI_INFO(&psmx2_prov, FI_LOG_CORE,
			"sep %d: ep_open_opts: unit=%d\n", sep_ctxt_idx, opts.unit);
	}

	err = psm2_ep_open(domain->fabric->uuid, &opts,
			   &trx_ctxt->psm2_ep, &trx_ctxt->psm2_epid);
	if (err != PSM2_OK) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE,
			"psm2_ep_open returns %d, errno=%d\n", err, errno);
		if (!should_retry) {
			err = psmx2_errno(err);
			goto err_out_destroy_pool;
		}

		/* When round-robin fails, retry w/o explicit assignment */
		opts.unit = -1;
		err = psm2_ep_open(domain->fabric->uuid, &opts,
				   &trx_ctxt->psm2_ep, &trx_ctxt->psm2_epid);
		if (err != PSM2_OK) {
			FI_WARN(&psmx2_prov, FI_LOG_CORE,
				"psm2_ep_open retry returns %d, errno=%d\n", err, errno);
			err = psmx2_errno(err);
			goto err_out_destroy_pool;
		}
	}

	FI_INFO(&psmx2_prov, FI_LOG_CORE,
		"epid: 0x%016lx\n", trx_ctxt->psm2_epid);

	err = psm2_mq_init(trx_ctxt->psm2_ep, PSM2_MQ_ORDERMASK_ALL,
			   NULL, 0, &trx_ctxt->psm2_mq);
	if (err != PSM2_OK) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE,
			"psm2_mq_init returns %d, errno=%d\n", err, errno);
		err = psmx2_errno(err);
		goto err_out_close_ep;
	}

	fastlock_init(&trx_ctxt->peer_lock);
	fastlock_init(&trx_ctxt->poll_lock);
	fastlock_init(&trx_ctxt->am_req_pool_lock);
	fastlock_init(&trx_ctxt->rma_queue.lock);
	fastlock_init(&trx_ctxt->trigger_queue.lock);
	dlist_init(&trx_ctxt->peer_list);
	slist_init(&trx_ctxt->rma_queue.list);
	slist_init(&trx_ctxt->trigger_queue.list);
	trx_ctxt->id = psmx2_trx_ctxt_cnt++;
	trx_ctxt->domain = domain;

	return trx_ctxt;

err_out_close_ep:
	if (psm2_ep_close(trx_ctxt->psm2_ep, PSM2_EP_CLOSE_GRACEFUL,
			  (int64_t) psmx2_env.timeout * 1000000000LL) != PSM2_OK)
		psm2_ep_close(trx_ctxt->psm2_ep, PSM2_EP_CLOSE_FORCE, 0);

err_out_destroy_pool:
	util_buf_pool_destroy(trx_ctxt->am_req_pool);

err_out:
	free(trx_ctxt);
	return NULL;
}
