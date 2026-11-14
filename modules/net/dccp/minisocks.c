// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  net/dccp/minisocks.c
 *
 *  An implementation of the DCCP protocol
 *  Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 */

#include <linux/dccp.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/timer.h>

#include <net/inet_hashtables.h>
#include <net/sock.h>
#include <net/xfrm.h>
#include <net/inet_timewait_sock.h>
#include <net/rstreason.h>

#include "ackvec.h"
#include "ccid.h"
#include "dccp.h"
#include "feat.h"

struct inet_timewait_death_row dccp_death_row = {
	.tw_refcount = REFCOUNT_INIT(1),
	.sysctl_max_tw_buckets = NR_FILE * 2,
	.hashinfo	= &dccp_hashinfo,
};

EXPORT_SYMBOL_GPL(dccp_death_row);

void dccp_time_wait(struct sock *sk, int state, int timeo)
{
	struct inet_timewait_sock *tw;

	tw = dccp_twsk_alloc(sk, &dccp_death_row, state);

	if (tw != NULL) {
		const struct inet_connection_sock *icsk = inet_csk(sk);
		const int rto = (icsk->icsk_rto << 2) - (icsk->icsk_rto >> 1);
#if IS_ENABLED(CONFIG_IPV6)
		if (tw->tw_family == PF_INET6) {
			tw->tw_v6_daddr = sk->sk_v6_daddr;
			tw->tw_v6_rcv_saddr = sk->sk_v6_rcv_saddr;
			tw->tw_ipv6only = sk->sk_ipv6only;
		}
#endif

		/* Get the TIME_WAIT timeout firing. */
		if (timeo < rto)
			timeo = rto;

		if (state == DCCP_TIME_WAIT)
			timeo = DCCP_TIMEWAIT_LEN;

		/* Linkage updates.
		 * Note that access to tw after this point is illegal.
		 */
		dccp_twsk_hashdance_schedule(tw, sk, &dccp_hashinfo, timeo);
	} else {
		/* Sorry, if we're out of memory, just CLOSE this
		 * socket up.  We've got bigger problems than
		 * non-graceful socket closings.
		 */
		DCCP_WARN("time wait bucket table overflow\n");
	}

	dccp_done(sk);
}

struct sock *dccp_create_openreq_child(const struct sock *sk,
				       const struct request_sock *req,
				       const struct sk_buff *skb)
{
	/*
	 * Step 3: Process LISTEN state
	 *
	 *   (* Generate a new socket and switch to that socket *)
	 *   Set S := new socket for this port pair
	 */
	struct sock *newsk = dccp_csk_clone_lock(sk, req, GFP_ATOMIC);

	if (newsk != NULL) {
		struct dccp_request_sock *dreq = dccp_rsk(req);
		struct inet_connection_sock *newicsk = inet_csk(newsk);
		struct dccp_sock *newdp = dccp_sk(newsk);

		newdp->dccps_role	    = DCCP_ROLE_SERVER;
		newdp->dccps_hc_rx_ackvec   = NULL;
		newdp->dccps_service_list   = NULL;
		newdp->dccps_hc_rx_ccid     = NULL;
		newdp->dccps_hc_tx_ccid     = NULL;
		newdp->dccps_service	    = dreq->dreq_service;
		newdp->dccps_timestamp_echo = dreq->dreq_timestamp_echo;
		newdp->dccps_timestamp_time = dreq->dreq_timestamp_time;
		newicsk->icsk_rto	    = DCCP_TIMEOUT_INIT;

		INIT_LIST_HEAD(&newdp->dccps_featneg);
		/*
		 * Step 3: Process LISTEN state
		 *
		 *    Choose S.ISS (initial seqno) or set from Init Cookies
		 *    Initialize S.GAR := S.ISS
		 *    Set S.ISR, S.GSR from packet (or Init Cookies)
		 *
		 *    Setting AWL/AWH and SWL/SWH happens as part of the feature
		 *    activation below, as these windows all depend on the local
		 *    and remote Sequence Window feature values (7.5.2).
		 */
		newdp->dccps_iss = dreq->dreq_iss;
		newdp->dccps_gss = dreq->dreq_gss;
		newdp->dccps_gar = newdp->dccps_iss;
		newdp->dccps_isr = dreq->dreq_isr;
		newdp->dccps_gsr = dreq->dreq_gsr;

		/*
		 * Activate features: initialise CCIDs, sequence windows etc.
		 */
		if (dccp_feat_activate_values(newsk, &dreq->dreq_featneg)) {
			bh_unlock_sock(newsk);
			sk_free(newsk);
			return NULL;
		}
		dccp_init_xmit_timers(newsk);

		__DCCP_INC_STATS(DCCP_MIB_PASSIVEOPENS);
	}
	return newsk;
}

EXPORT_SYMBOL_GPL(dccp_create_openreq_child);

static int dccp_rtx_syn_ack(const struct sock *parent, struct request_sock *req)
{
	const struct dccp_request_sock_ops *ops;
	int err;

	ops = container_of(req->rsk_ops, const struct dccp_request_sock_ops, ops);
	err = ops->rtx_response(parent, req);
	if (!err)
		req->num_retrans++;
	return err;
}

static void dccp_reqsk_timer_handler(struct timer_list *t)
{
	struct request_sock *req = timer_container_of(req, t, rsk_timer);
	struct sock *sk_listener = req->rsk_listener;
	struct inet_connection_sock *icsk = inet_csk(sk_listener);
	int max_retries = icsk->icsk_syn_retries ? : sysctl_dccp_request_retries;
	unsigned long timeo;

	if (sk_unhashed(req_to_sk(req))) {
		reqsk_put(req);
		return;
	}

	if (inet_sk_state_load(sk_listener) != DCCP_LISTEN)
		goto drop;

	if (req->num_retrans >= max_retries)
		goto drop;

	dccp_rtx_syn_ack(sk_listener, req);
	timeo = min_t(unsigned long, DCCP_TIMEOUT_INIT << req->num_retrans,
		      DCCP_RTO_MAX);
	mod_timer(&req->rsk_timer, jiffies + timeo);
	return;

drop:
	dccp_csk_reqsk_queue_drop_and_put(sk_listener, req);
}

bool dccp_reqsk_queue_hash_add(struct sock *sk, struct request_sock *req,
			       unsigned long timeout)
{
	bool found_dup_sk = false;

	timer_setup(&req->rsk_timer, dccp_reqsk_timer_handler, TIMER_PINNED);

	/*
	 * Three references: one for the caller, one for the ehash table,
	 * and one for rsk_timer.
	 */
	refcount_set(&req->rsk_refcnt, 3);
	reqsk_queue_added(&inet_csk(sk)->icsk_accept_queue);

	if (unlikely(!dccp_ehash_insert(req_to_sk(req), NULL, &found_dup_sk))) {
		refcount_set(&req->rsk_refcnt, 1);
		return false;
	}

	mod_timer(&req->rsk_timer, jiffies + timeout);
	return true;
}
EXPORT_SYMBOL_GPL(dccp_reqsk_queue_hash_add);

/*
 * Process an incoming packet for RESPOND sockets represented
 * as an request_sock.
 */
struct sock *dccp_check_req(struct sock *sk, struct sk_buff *skb,
			    struct request_sock *req)
{
	struct sock *child = NULL;
	struct dccp_request_sock *dreq = dccp_rsk(req);
	bool own_req;

	/* TCP/DCCP listeners became lockless.
	 * DCCP stores complex state in its request_sock, so we need
	 * a protection for them, now this code runs without being protected
	 * by the parent (listener) lock.
	 */
	spin_lock_bh(&dreq->dreq_lock);

	/* Check for retransmitted REQUEST */
	if (dccp_hdr(skb)->dccph_type == DCCP_PKT_REQUEST) {

		if (after48(DCCP_SKB_CB(skb)->dccpd_seq, dreq->dreq_gsr)) {
			dccp_pr_debug("Retransmitted REQUEST\n");
			dreq->dreq_gsr = DCCP_SKB_CB(skb)->dccpd_seq;
			/*
			 * Send another RESPONSE packet
			 * To protect against Request floods, increment retrans
			 * counter (backoff, monitored by dccp_response_timer).
			 */
			dccp_rtx_syn_ack(sk, req);
		}
		/* Network Duplicate, discard packet */
		goto out;
	}

	DCCP_SKB_CB(skb)->dccpd_reset_code = DCCP_RESET_CODE_PACKET_ERROR;

	if (dccp_hdr(skb)->dccph_type != DCCP_PKT_ACK &&
	    dccp_hdr(skb)->dccph_type != DCCP_PKT_DATAACK)
		goto drop;

	/* Invalid ACK */
	if (!between48(DCCP_SKB_CB(skb)->dccpd_ack_seq,
				dreq->dreq_iss, dreq->dreq_gss)) {
		dccp_pr_debug("Invalid ACK number: ack_seq=%llu, "
			      "dreq_iss=%llu, dreq_gss=%llu\n",
			      (unsigned long long)
			      DCCP_SKB_CB(skb)->dccpd_ack_seq,
			      (unsigned long long) dreq->dreq_iss,
			      (unsigned long long) dreq->dreq_gss);
		goto drop;
	}

	if (dccp_parse_options(sk, dreq, skb))
		 goto drop;

	child = inet_csk(sk)->icsk_af_ops->syn_recv_sock(sk, skb, req, NULL,
							 req, &own_req, NULL);
	if (child) {
		child = dccp_csk_complete_hashdance(sk, child, req, own_req);
		goto out;
	}

	DCCP_SKB_CB(skb)->dccpd_reset_code = DCCP_RESET_CODE_TOO_BUSY;
drop:
	if (dccp_hdr(skb)->dccph_type != DCCP_PKT_RESET)
		req->rsk_ops->send_reset(sk, skb, SK_RST_REASON_NOT_SPECIFIED);

	dccp_csk_reqsk_queue_drop(sk, req);
out:
	spin_unlock_bh(&dreq->dreq_lock);
	return child;
}

EXPORT_SYMBOL_GPL(dccp_check_req);

/*
 *  Queue segment on the new socket if the new socket is active,
 *  otherwise we just shortcircuit this and continue with
 *  the new socket.
 */
int dccp_child_process(struct sock *parent, struct sock *child,
		       struct sk_buff *skb)
	__releases(child)
{
	int ret = 0;
	const int state = child->sk_state;

	if (!sock_owned_by_user(child)) {
		ret = dccp_rcv_state_process(child, skb, dccp_hdr(skb),
					     skb->len);

		/* Wakeup parent, send SIGIO */
		if (state == DCCP_RESPOND && child->sk_state != state)
			parent->sk_data_ready(parent);
	} else {
		/* Alas, it is possible again, because we do lookup
		 * in main socket hash table and lock on listening
		 * socket does not protect us more.
		 */
		__sk_add_backlog(child, skb);
	}

	bh_unlock_sock(child);
	sock_put(child);
	return ret;
}

EXPORT_SYMBOL_GPL(dccp_child_process);

void dccp_reqsk_send_ack(const struct sock *sk, struct sk_buff *skb,
			 struct request_sock *rsk)
{
	DCCP_BUG("DCCP-ACK packets are never sent in LISTEN/RESPOND state");
}

EXPORT_SYMBOL_GPL(dccp_reqsk_send_ack);

int dccp_reqsk_init(struct request_sock *req,
		    struct dccp_sock const *dp, struct sk_buff const *skb)
{
	struct dccp_request_sock *dreq = dccp_rsk(req);

	spin_lock_init(&dreq->dreq_lock);
	inet_rsk(req)->ir_rmt_port = dccp_hdr(skb)->dccph_sport;
	inet_rsk(req)->ir_num	   = ntohs(dccp_hdr(skb)->dccph_dport);
	inet_rsk(req)->acked	   = 0;
	dreq->dreq_timestamp_echo  = 0;

	/* inherit feature negotiation options from listening socket */
	return dccp_feat_clone_list(&dp->dccps_featneg, &dreq->dreq_featneg);
}

EXPORT_SYMBOL_GPL(dccp_reqsk_init);
