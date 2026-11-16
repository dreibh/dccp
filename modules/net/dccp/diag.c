// SPDX-License-Identifier: GPL-2.0-only
/*
 *  net/dccp/diag.c
 *
 *  An implementation of the DCCP protocol
 *  Arnaldo Carvalho de Melo <acme@mandriva.com>
 */

#include <linux/module.h>
#include <linux/inet_diag.h>
#include <net/inet_hashtables.h>
#include <net/inet6_hashtables.h>
#include <net/netlink.h>
#include <net/sock.h>

#include "ccid.h"
#include "dccp.h"

static void dccp_get_info(struct sock *sk, struct tcp_info *info)
{
	struct dccp_sock *dp = dccp_sk(sk);
	const struct inet_connection_sock *icsk = inet_csk(sk);

	memset(info, 0, sizeof(*info));

	info->tcpi_state	= sk->sk_state;
	info->tcpi_retransmits	= icsk->icsk_retransmits;
	info->tcpi_probes	= icsk->icsk_probes_out;
	info->tcpi_backoff	= icsk->icsk_backoff;
	info->tcpi_pmtu		= icsk->icsk_pmtu_cookie;

	if (dp->dccps_hc_rx_ackvec != NULL)
		info->tcpi_options |= TCPI_OPT_SACK;

	if (dp->dccps_hc_rx_ccid != NULL)
		ccid_hc_rx_get_info(dp->dccps_hc_rx_ccid, sk, info);

	if (dp->dccps_hc_tx_ccid != NULL)
		ccid_hc_tx_get_info(dp->dccps_hc_tx_ccid, sk, info);
}

static void dccp_diag_get_info(struct sock *sk, struct inet_diag_msg *r,
			       void *_info)
{
	r->idiag_rqueue = r->idiag_wqueue = 0;

	if (_info != NULL)
		dccp_get_info(sk, _info);
}

static void dccp_diag_fill_sockid(struct inet_diag_msg *r,
				  const struct sock *sk)
{
	r->idiag_family = sk->sk_family;
	r->idiag_state = sk->sk_state;
	r->id.idiag_sport = htons(sk->sk_num);
	r->id.idiag_dport = sk->sk_dport;
	r->id.idiag_if = sk->sk_bound_dev_if;
	r->id.idiag_cookie[0] = (u32)(unsigned long)sk;
	r->id.idiag_cookie[1] = (u32)(((unsigned long)sk >> 31) >> 1);

#if IS_ENABLED(CONFIG_IPV6)
	if (sk->sk_family == AF_INET6) {
		*(struct in6_addr *)r->id.idiag_src = sk->sk_v6_rcv_saddr;
		*(struct in6_addr *)r->id.idiag_dst = sk->sk_v6_daddr;
		return;
	}
#endif
	r->id.idiag_src[0] = sk->sk_rcv_saddr;
	r->id.idiag_dst[0] = sk->sk_daddr;
}

static int dccp_twsk_diag_fill(struct inet_timewait_sock *tw,
			       struct sk_buff *skb,
			       struct netlink_callback *cb,
			       const struct inet_diag_req_v2 *req,
			       u16 nlmsg_flags, bool net_admin)
{
	struct inet_diag_msg *r;
	struct nlmsghdr *nlh;
	long tmo;

	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			cb->nlh->nlmsg_type, sizeof(*r), nlmsg_flags);
	if (!nlh)
		return -EMSGSIZE;

	r = nlmsg_data(nlh);
	memset(r, 0, sizeof(*r));
	dccp_diag_fill_sockid(r, (const struct sock *)tw);

	tmo = tw->tw_timer.expires - jiffies;
	if (tmo < 0)
		tmo = 0;

	r->idiag_timer = 5; /* INET_DIAG_TIME_WAIT */
	r->idiag_expires = jiffies_to_msecs(tmo);

	if (net_admin && (req->idiag_ext & (1 << (INET_DIAG_MARK - 1)))) {
		if (nla_put_u32(skb, INET_DIAG_MARK, tw->tw_mark) < 0) {
			nlmsg_cancel(skb, nlh);
			return -EMSGSIZE;
		}
	}

	nlmsg_end(skb, nlh);
	return 0;
}

static int dccp_req_diag_fill(struct request_sock *reqsk,
			      struct sk_buff *skb,
			      struct netlink_callback *cb,
			      const struct inet_diag_req_v2 *req,
			      u16 nlmsg_flags, bool net_admin)
{
	struct inet_request_sock *ireq = inet_rsk(reqsk);
	struct inet_diag_msg *r;
	struct nlmsghdr *nlh;
	long tmo;

	nlh = nlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			cb->nlh->nlmsg_type, sizeof(*r), nlmsg_flags);
	if (!nlh)
		return -EMSGSIZE;

	r = nlmsg_data(nlh);
	memset(r, 0, sizeof(*r));
	dccp_diag_fill_sockid(r, req_to_sk(reqsk));

	tmo = reqsk->rsk_timer.expires - jiffies;
	if (tmo < 0)
		tmo = 0;

	r->idiag_timer = 2; /* INET_DIAG_SYN_RECV */
	r->idiag_retrans = reqsk->num_retrans;
	r->idiag_expires = jiffies_to_msecs(tmo);

	if (net_admin && (req->idiag_ext & (1 << (INET_DIAG_MARK - 1)))) {
		if (nla_put_u32(skb, INET_DIAG_MARK, ireq->ir_mark) < 0) {
			nlmsg_cancel(skb, nlh);
			return -EMSGSIZE;
		}
	}

	nlmsg_end(skb, nlh);
	return 0;
}

static int dccp_diag_fill_any(struct sock *sk, struct sk_buff *skb,
			      struct netlink_callback *cb,
			      const struct inet_diag_req_v2 *r,
			      u16 nlmsg_flags, bool net_admin)
{
	if (sk->sk_state == DCCP_TIME_WAIT)
		return dccp_twsk_diag_fill(inet_twsk(sk), skb, cb, r,
					   nlmsg_flags, net_admin);

	if (sk->sk_state == DCCP_NEW_SYN_RECV)
		return dccp_req_diag_fill(inet_reqsk(sk), skb, cb, r,
					  nlmsg_flags, net_admin);

	return inet_sk_diag_fill(sk, inet_csk(sk), skb, cb, r,
				 nlmsg_flags, net_admin);
}

static void dccp_diag_dump(struct sk_buff *skb, struct netlink_callback *cb,
			   const struct inet_diag_req_v2 *r)
{
	const struct inet_diag_dump_data *cb_data = cb->data;
	bool net_admin = netlink_capable(cb->skb, CAP_NET_ADMIN);
	struct net *net = sock_net(skb->sk);
	int num = 0, s_num, i;

	if (cb->args[2] == 0) {
		if (!(r->idiag_states & TCPF_LISTEN) || !dccp_hashinfo.lhash2)
			goto skip_listen;

		s_num = cb->args[1];
		for (i = cb->args[0]; i <= dccp_hashinfo.lhash2_mask; i++) {
			struct inet_listen_hashbucket *ilb2 =
				&dccp_hashinfo.lhash2[i];
			struct hlist_nulls_node *node;
			struct sock *sk;

			num = 0;
			spin_lock_bh(&ilb2->lock);
			sk_nulls_for_each(sk, node, &ilb2->nulls_head) {
				if (!net_eq(sock_net(sk), net))
					continue;
				if (r->sdiag_family != AF_UNSPEC &&
				    sk->sk_family != r->sdiag_family)
					continue;
				if (r->id.idiag_sport &&
				    htons(sk->sk_num) != r->id.idiag_sport)
					continue;
				if (r->id.idiag_dport)
					continue;
				if (num < s_num) {
					num++;
					continue;
				}
				if (!inet_diag_bc_sk(cb_data, sk)) {
					num++;
					continue;
				}
				if (inet_sk_diag_fill(sk, inet_csk(sk), skb, cb,
						      r, NLM_F_MULTI,
						      net_admin) < 0) {
					spin_unlock_bh(&ilb2->lock);
					goto done;
				}
				num++;
			}
			spin_unlock_bh(&ilb2->lock);
			s_num = 0;
		}
skip_listen:
		cb->args[2] = 1;
		cb->args[0] = 0;
		cb->args[1] = 0;
	}

	if (!(r->idiag_states & ~TCPF_LISTEN) || !dccp_hashinfo.ehash) {
		i = dccp_hashinfo.ehash_mask + 1;
		num = 0;
		goto done;
	}

	s_num = cb->args[1];
	for (i = cb->args[0]; i <= dccp_hashinfo.ehash_mask; i++) {
		struct inet_ehash_bucket *head = &dccp_hashinfo.ehash[i];
		spinlock_t *lock = inet_ehash_lockp(&dccp_hashinfo, i);
		struct hlist_nulls_node *node;
		struct sock *sk;

		num = 0;
		spin_lock_bh(lock);
		sk_nulls_for_each(sk, node, &head->chain) {
			if (!net_eq(sock_net(sk), net))
				continue;
			if (r->sdiag_family != AF_UNSPEC &&
			    sk->sk_family != r->sdiag_family)
				continue;
			if (!(r->idiag_states & (1 << sk->sk_state)))
				continue;
			if (r->id.idiag_sport &&
			    htons(sk->sk_num) != r->id.idiag_sport)
				continue;
			if (r->id.idiag_dport &&
			    sk->sk_dport != r->id.idiag_dport)
				continue;
			if (num < s_num) {
				num++;
				continue;
			}
			if (!inet_diag_bc_sk(cb_data, sk)) {
				num++;
				continue;
			}
			if (dccp_diag_fill_any(sk, skb, cb, r, NLM_F_MULTI,
					       net_admin) < 0) {
				spin_unlock_bh(lock);
				goto done;
			}
			num++;
		}
		spin_unlock_bh(lock);
		s_num = 0;
	}

done:
	cb->args[0] = i;
	cb->args[1] = num;
}

static struct sock *dccp_diag_find_one(struct net *net,
				       const struct inet_diag_req_v2 *req)
{
	const u16 hnum = ntohs(req->id.idiag_sport);
	const __be16 dport = req->id.idiag_dport;
	const int dif = req->id.idiag_if;
	const __portpair ports = INET_COMBINED_PORTS(dport, hnum);
	const struct hlist_nulls_node *node;
	struct sock *sk = NULL;

	rcu_read_lock();
	if (dport && dccp_hashinfo.ehash) {
		if (req->sdiag_family == AF_INET) {
			const __be32 laddr = req->id.idiag_src[0];
			const __be32 raddr = req->id.idiag_dst[0];
			INET_ADDR_COOKIE(acookie, raddr, laddr);
			unsigned int hash = inet_ehashfn(net, laddr, hnum,
							 raddr, dport);
			unsigned int slot = hash & dccp_hashinfo.ehash_mask;
			struct inet_ehash_bucket *head =
				&dccp_hashinfo.ehash[slot];

			sk_nulls_for_each_rcu(sk, node, &head->chain) {
				if (sk->sk_hash != hash)
					continue;
				if (inet_match(net, sk, acookie, ports,
					       dif, 0)) {
					if (refcount_inc_not_zero(&sk->sk_refcnt))
						goto out;
					break;
				}
			}
		}
#if IS_ENABLED(CONFIG_IPV6)
		else if (req->sdiag_family == AF_INET6) {
			const struct in6_addr *laddr =
				(const struct in6_addr *)req->id.idiag_src;
			const struct in6_addr *raddr =
				(const struct in6_addr *)req->id.idiag_dst;
			unsigned int hash = inet6_ehashfn(net, laddr, hnum,
							  raddr, dport);
			unsigned int slot = hash & dccp_hashinfo.ehash_mask;
			struct inet_ehash_bucket *head =
				&dccp_hashinfo.ehash[slot];

			sk_nulls_for_each_rcu(sk, node, &head->chain) {
				if (sk->sk_hash != hash)
					continue;
				if (inet6_match(net, sk, raddr, laddr, ports,
						dif, 0)) {
					if (refcount_inc_not_zero(&sk->sk_refcnt))
						goto out;
					break;
				}
			}
		}
#endif
	}

	if ((req->idiag_states & TCPF_LISTEN) && dccp_hashinfo.lhash2) {
		unsigned int i;

		for (i = 0; i <= dccp_hashinfo.lhash2_mask; i++) {
			struct inet_listen_hashbucket *ilb2 =
				&dccp_hashinfo.lhash2[i];

			sk_nulls_for_each_rcu(sk, node, &ilb2->nulls_head) {
				if (!net_eq(sock_net(sk), net) ||
				    sk->sk_family != req->sdiag_family ||
				    sk->sk_num != hnum)
					continue;
				if (dif && sk->sk_bound_dev_if != dif)
					continue;
				if (req->sdiag_family == AF_INET &&
				    inet_sk(sk)->inet_rcv_saddr !=
				    req->id.idiag_src[0])
					continue;
#if IS_ENABLED(CONFIG_IPV6)
				if (req->sdiag_family == AF_INET6 &&
				    !ipv6_addr_equal(&sk->sk_v6_rcv_saddr,
						     (const struct in6_addr *)
						     req->id.idiag_src))
					continue;
#endif
				if (refcount_inc_not_zero(&sk->sk_refcnt))
					goto out;
			}
		}
	}
	sk = NULL;
out:
	rcu_read_unlock();
	return sk;
}

static void dccp_diag_put_sk(struct sock *sk)
{
	if (sk->sk_state == DCCP_TIME_WAIT)
		dccp_twsk_put(inet_twsk(sk));
	else if (sk->sk_state == DCCP_NEW_SYN_RECV)
		dccp_reqsk_put(inet_reqsk(sk));
	else
		sock_put(sk);
}

static int dccp_diag_dump_one(struct netlink_callback *cb,
			      const struct inet_diag_req_v2 *req)
{
	struct net *net = sock_net(cb->skb->sk);
	struct sk_buff *rep;
	struct sock *sk;
	int err;

	sk = dccp_diag_find_one(net, req);
	if (!sk)
		return -ENOENT;

	rep = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!rep) {
		dccp_diag_put_sk(sk);
		return -ENOMEM;
	}

	err = dccp_diag_fill_any(sk, rep, cb, req, 0,
				 netlink_capable(cb->skb, CAP_NET_ADMIN));
	dccp_diag_put_sk(sk);
	if (err < 0) {
		kfree_skb(rep);
		return err;
	}

	return nlmsg_unicast(cb->skb->sk, rep, NETLINK_CB(cb->skb).portid);
}

static const struct inet_diag_handler dccp_diag_handler = {
	.owner		 = THIS_MODULE,
	.dump		 = dccp_diag_dump,
	.dump_one	 = dccp_diag_dump_one,
	.idiag_get_info	 = dccp_diag_get_info,
	.idiag_type	 = IPPROTO_DCCP,
	.idiag_info_size = sizeof(struct tcp_info),
};

static int __init dccp_diag_init(void)
{
	return inet_diag_register(&dccp_diag_handler);
}

static void __exit dccp_diag_fini(void)
{
	inet_diag_unregister(&dccp_diag_handler);
}

module_init(dccp_diag_init);
module_exit(dccp_diag_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Arnaldo Carvalho de Melo <acme@mandriva.com>");
MODULE_DESCRIPTION("DCCP inet_diag handler");
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_NETLINK, NETLINK_SOCK_DIAG, 2-33 /* AF_INET - IPPROTO_DCCP */);
