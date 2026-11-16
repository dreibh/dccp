// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Self-contained INET/INET6 connection, hashtable, and time-wait helpers
 * bound directly to dccp_hashinfo for Linux >= 6.16.
 */

#include <linux/dccp.h>
#include <linux/export.h>
#include <linux/jhash.h>
#include <linux/random.h>
#include <linux/sched/signal.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <net/flow.h>
#include <net/inet_dscp.h>
#include <net/inet6_hashtables.h>
#include <net/ip6_route.h>
#include <net/route.h>
#include <net/xfrm.h>

#include "dccp.h"

int dccp_hashinfo2_init_mod(struct inet_hashinfo *h)
{
	unsigned int i;

	h->lhash2 = kvmalloc_array(INET_LHTABLE_SIZE,
				   sizeof(struct inet_listen_hashbucket),
				   GFP_KERNEL);
	if (!h->lhash2)
		return -ENOMEM;

	h->lhash2_mask = INET_LHTABLE_SIZE - 1;
	for (i = 0; i < INET_LHTABLE_SIZE; i++) {
		spin_lock_init(&h->lhash2[i].lock);
		INIT_HLIST_NULLS_HEAD(&h->lhash2[i].nulls_head,
				      i + LISTENING_NULLS_BASE);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(dccp_hashinfo2_init_mod);

int dccp_ehash_locks_alloc(struct inet_hashinfo *hashinfo)
{
	unsigned int i, size = 256;

	hashinfo->ehash_locks = kvmalloc_array(size, sizeof(spinlock_t),
					       GFP_KERNEL);
	if (!hashinfo->ehash_locks)
		return -ENOMEM;

	hashinfo->ehash_locks_mask = size - 1;
	for (i = 0; i < size; i++)
		spin_lock_init(&hashinfo->ehash_locks[i]);
	return 0;
}
EXPORT_SYMBOL_GPL(dccp_ehash_locks_alloc);

static u32 dccp_sk_ehashfn(const struct sock *sk)
{
	const struct inet_sock *inet = inet_sk(sk);
	const struct net *net = sock_net(sk);

#if IS_ENABLED(CONFIG_IPV6)
	if (sk->sk_family == AF_INET6 &&
	    !ipv6_addr_v4mapped(&sk->sk_v6_daddr))
		return inet6_ehashfn(net, &sk->sk_v6_rcv_saddr,
				     inet->inet_num, &sk->sk_v6_daddr,
				     inet->inet_dport);
#endif
	return inet_ehashfn(net, inet->inet_rcv_saddr, inet->inet_num,
			    inet->inet_daddr, inet->inet_dport);
}

static bool dccp_ehash_entry_match(const struct sock *a, const struct sock *b)
{
	if (!net_eq(sock_net(a), sock_net(b)) ||
	    a->sk_family != b->sk_family ||
	    a->sk_portpair != b->sk_portpair ||
	    a->sk_bound_dev_if != b->sk_bound_dev_if)
		return false;

#if IS_ENABLED(CONFIG_IPV6)
	if (a->sk_family == AF_INET6)
		return ipv6_addr_equal(&a->sk_v6_daddr, &b->sk_v6_daddr) &&
		       ipv6_addr_equal(&a->sk_v6_rcv_saddr,
				       &b->sk_v6_rcv_saddr);
#endif
	return a->sk_addrpair == b->sk_addrpair;
}

bool dccp_ehash_insert(struct sock *sk, struct sock *osk, bool *found_dup_sk)
{
	struct inet_ehash_bucket *head;
	struct hlist_nulls_node *node;
	struct sock *esk;
	spinlock_t *lock;
	u32 hash;

	hash = dccp_sk_ehashfn(sk);
	sk->sk_hash = hash;

	head = inet_ehash_bucket(&dccp_hashinfo, hash);
	lock = inet_ehash_lockp(&dccp_hashinfo, hash);

	spin_lock_bh(lock);
	if (osk) {
		if (sk_unhashed(osk)) {
			spin_unlock_bh(lock);
			if (found_dup_sk)
				*found_dup_sk = false;
			return false;
		}
		__sk_nulls_del_node_init_rcu(osk);
	} else if (found_dup_sk) {
		sk_nulls_for_each(esk, node, &head->chain) {
			if (esk->sk_hash == hash &&
			    dccp_ehash_entry_match(esk, sk)) {
				*found_dup_sk = true;
				spin_unlock_bh(lock);
				return false;
			}
		}
		*found_dup_sk = false;
	}

	hlist_nulls_add_head_rcu(&sk->sk_nulls_node, &head->chain);
	spin_unlock_bh(lock);
	return true;
}
EXPORT_SYMBOL_GPL(dccp_ehash_insert);

bool dccp_ehash_nolisten(struct sock *sk, struct sock *osk, bool *found_dup_sk)
{
	bool ok = dccp_ehash_insert(sk, osk, found_dup_sk);

	if (ok) {
		sock_hold(sk);
	} else {
		inet_sk_set_state(sk, DCCP_CLOSED);
		sock_set_flag(sk, SOCK_DEAD);
	}
	return ok;
}
EXPORT_SYMBOL_GPL(dccp_ehash_nolisten);

static struct inet_listen_hashbucket *dccp_lhash2_bucket_sk(const struct sock *sk)
{
	const struct net *net = sock_net(sk);
	const u16 hnum = inet_sk(sk)->inet_num;
	u32 hash;

#if IS_ENABLED(CONFIG_IPV6)
	if (sk->sk_family == AF_INET6)
		hash = ipv6_portaddr_hash(net, &sk->sk_v6_rcv_saddr, hnum);
	else
#endif
		hash = ipv4_portaddr_hash(net, inet_sk(sk)->inet_rcv_saddr,
					  hnum);

	return inet_lhash2_bucket(&dccp_hashinfo, hash);
}

int dccp_inet_hash(struct sock *sk)
{
	struct inet_listen_hashbucket *ilb2;

	if (sk->sk_state != DCCP_LISTEN) {
		dccp_ehash_nolisten(sk, NULL, NULL);
		return 0;
	}

	ilb2 = dccp_lhash2_bucket_sk(sk);
	spin_lock_bh(&ilb2->lock);
	if (sk_unhashed(sk)) {
		sock_hold(sk);
		hlist_nulls_add_head_rcu(&sk->sk_nulls_node, &ilb2->nulls_head);
	}
	spin_unlock_bh(&ilb2->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(dccp_inet_hash);

void dccp_inet_unhash(struct sock *sk)
{
	if (sk_unhashed(sk))
		return;

	if (sk->sk_state == DCCP_LISTEN) {
		struct inet_listen_hashbucket *ilb2 = dccp_lhash2_bucket_sk(sk);

		spin_lock_bh(&ilb2->lock);
		if (!sk_unhashed(sk)) {
			__sk_nulls_del_node_init_rcu(sk);
			sock_put(sk);
		}
		spin_unlock_bh(&ilb2->lock);
	} else {
		spinlock_t *lock = inet_ehash_lockp(&dccp_hashinfo, sk->sk_hash);

		spin_lock_bh(lock);
		if (!sk_unhashed(sk)) {
			__sk_nulls_del_node_init_rcu(sk);
			sock_put(sk);
		}
		spin_unlock_bh(lock);
	}
}
EXPORT_SYMBOL_GPL(dccp_inet_unhash);

static struct inet_bind_hashbucket *
dccp_bhash2_bucket(const struct net *net, const struct sock *sk,
		   const struct inet_bind2_bucket *tb2)
{
	u32 hash;

#if IS_ENABLED(CONFIG_IPV6)
	if (sk->sk_family == AF_INET6)
		hash = ipv6_portaddr_hash(net, &tb2->v6_rcv_saddr, tb2->port);
	else
#endif
		hash = ipv4_portaddr_hash(net, tb2->rcv_saddr, tb2->port);
	return &dccp_hashinfo.bhash2[hash & (dccp_hashinfo.bhash_size - 1)];
}

void dccp_put_port(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct net *net = sock_net(sk);
	const unsigned short snum = inet_sk(sk)->inet_num;
	struct inet_bind_hashbucket *head;
	struct inet_bind_bucket *tb;
	struct inet_bind2_bucket *tb2;

	if (!icsk->icsk_bind_hash)
		return;

	head = &dccp_hashinfo.bhash[inet_bhashfn(net, snum,
						 dccp_hashinfo.bhash_size)];
	local_bh_disable();
	spin_lock(&head->lock);
	tb = icsk->icsk_bind_hash;
	tb2 = icsk->icsk_bind2_hash;
	__sk_del_bind_node(sk);
	icsk->icsk_bind_hash = NULL;
	icsk->icsk_bind2_hash = NULL;
	inet_sk(sk)->inet_num = 0;

	if (tb2 && hlist_empty(&tb2->owners)) {
		struct inet_bind_hashbucket *head2 =
			dccp_bhash2_bucket(net, sk, tb2);

		spin_lock(&head2->lock);
		__hlist_del(&tb2->node);
		spin_unlock(&head2->lock);
		__hlist_del(&tb2->bhash_node);
		kmem_cache_free(dccp_hashinfo.bind2_bucket_cachep, tb2);
	}
	if (tb && hlist_empty(&tb->bhash2)) {
		__hlist_del(&tb->node);
		kmem_cache_free(dccp_hashinfo.bind_bucket_cachep, tb);
	}
	spin_unlock(&head->lock);
	local_bh_enable();
}
EXPORT_SYMBOL_GPL(dccp_put_port);

int dccp_inherit_port(const struct sock *sk, struct sock *child)
{
	struct inet_connection_sock *icsk = inet_csk(child);
	struct inet_bind_bucket *tb = inet_csk(sk)->icsk_bind_hash;
	const unsigned short port = inet_sk(child)->inet_num;
	struct net *net = sock_net(child);
	struct inet_bind_hashbucket *head, *head2;
	struct inet_bind2_bucket *tb2 = NULL, *iter;
	int l3mdev;

	if (unlikely(!tb))
		return -ENOENT;

	l3mdev = inet_sk_bound_l3mdev(child);
	head = &dccp_hashinfo.bhash[inet_bhashfn(net, port,
						 dccp_hashinfo.bhash_size)];
	head2 = inet_bhashfn_portaddr(&dccp_hashinfo, child, net, port);

	spin_lock(&head->lock);
	spin_lock(&head2->lock);
	hlist_for_each_entry(iter, &head2->chain, node) {
		if (!net_eq(ib2_net(iter), net) || iter->port != port ||
		    iter->l3mdev != l3mdev)
			continue;
#if IS_ENABLED(CONFIG_IPV6)
		if (child->sk_family == AF_INET6) {
			if (ipv6_addr_equal(&iter->v6_rcv_saddr,
					    &child->sk_v6_rcv_saddr)) {
				tb2 = iter;
				break;
			}
		} else
#endif
		if (iter->rcv_saddr == child->sk_rcv_saddr) {
			tb2 = iter;
			break;
		}
	}

	if (!tb2) {
		tb2 = kmem_cache_alloc(dccp_hashinfo.bind2_bucket_cachep,
				       GFP_ATOMIC);
		if (!tb2) {
			spin_unlock(&head2->lock);
			spin_unlock(&head->lock);
			return -ENOMEM;
		}
		write_pnet(&tb2->ib_net, net);
		tb2->l3mdev = l3mdev;
		tb2->port = port;
#if IS_ENABLED(CONFIG_IPV6)
		if (child->sk_family == AF_INET6) {
			tb2->addr_type =
				ipv6_addr_type(&child->sk_v6_rcv_saddr);
			tb2->v6_rcv_saddr = child->sk_v6_rcv_saddr;
		} else
#endif
		{
			tb2->rcv_saddr = child->sk_rcv_saddr;
		}
		tb2->fastreuse = 0;
		tb2->fastreuseport = 0;
		INIT_HLIST_HEAD(&tb2->owners);
		hlist_add_head(&tb2->node, &head2->chain);
		hlist_add_head(&tb2->bhash_node, &tb->bhash2);
	}

	sk_add_bind_node(child, &tb2->owners);
	icsk->icsk_bind_hash = tb;
	icsk->icsk_bind2_hash = tb2;
	spin_unlock(&head2->lock);
	spin_unlock(&head->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(dccp_inherit_port);

int dccp_csk_listen_start(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct inet_sock *inet = inet_sk(sk);
	int err;

	memset(&icsk->icsk_accept_queue, 0, sizeof(icsk->icsk_accept_queue));
	spin_lock_init(&icsk->icsk_accept_queue.rskq_lock);

	sk->sk_ack_backlog = 0;
	inet_csk_delack_init(sk);

	err = sk->sk_prot->get_port(sk, inet->inet_num);
	if (!err) {
		inet->inet_sport = htons(inet->inet_num);
		sk_dst_reset(sk);
		inet_sk_set_state(sk, DCCP_LISTEN);
		err = sk->sk_prot->hash(sk);
		if (likely(!err))
			return 0;
	}

	inet_sk_set_state(sk, DCCP_CLOSED);
	return err;
}
EXPORT_SYMBOL_GPL(dccp_csk_listen_start);

void dccp_csk_listen_stop(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct request_sock_queue *queue = &icsk->icsk_accept_queue;
	struct request_sock *req;
	unsigned int i;

	while ((req = reqsk_queue_remove(queue, sk)) != NULL) {
		struct sock *child = req->sk;

		local_bh_disable();
		bh_lock_sock(child);
		WARN_ON(sock_owned_by_user(child));
		sock_hold(child);

		dccp_csk_prepare_forced_close(child);
		dccp_done(child);

		bh_unlock_sock(child);
		local_bh_enable();
		sock_put(child);

		dccp_reqsk_put(req);
	}

	if (!dccp_hashinfo.ehash)
		return;

	for (i = 0; i <= dccp_hashinfo.ehash_mask; i++) {
		struct inet_ehash_bucket *head = &dccp_hashinfo.ehash[i];
		spinlock_t *lock = inet_ehash_lockp(&dccp_hashinfo, i);
		struct hlist_nulls_node *node;

		spin_lock_bh(lock);
		node = head->chain.first;
		while (!is_a_nulls(node)) {
			struct hlist_nulls_node *next = node->next;
			struct sock *esk = container_of(node, struct sock,
							__sk_common.skc_nulls_node);

			if (esk->sk_state == DCCP_NEW_SYN_RECV &&
			    inet_reqsk(esk)->rsk_listener == sk) {
				struct request_sock *ereq = inet_reqsk(esk);

				__sk_nulls_del_node_init_rcu(esk);
				reqsk_queue_removed(queue, ereq);
				if (timer_delete(&ereq->rsk_timer))
					dccp_reqsk_put(ereq);
				dccp_reqsk_put(ereq);
			}
			node = next;
		}
		spin_unlock_bh(lock);
	}
}
EXPORT_SYMBOL_GPL(dccp_csk_listen_stop);

struct sock *dccp_csk_accept(struct sock *sk, struct proto_accept_arg *arg)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct request_sock_queue *queue = &icsk->icsk_accept_queue;
	struct request_sock *req;
	struct sock *newsk;
	int error;

	lock_sock(sk);

	error = -EINVAL;
	if (sk->sk_state != DCCP_LISTEN)
		goto out_err;

	if (reqsk_queue_empty(queue)) {
		long timeo = sock_rcvtimeo(sk, arg->flags & O_NONBLOCK);
		DEFINE_WAIT(wait);

		error = -EAGAIN;
		if (!timeo)
			goto out_err;

		for (;;) {
			prepare_to_wait_exclusive(sk_sleep(sk), &wait,
						  TASK_INTERRUPTIBLE);
			release_sock(sk);
			if (reqsk_queue_empty(queue))
				timeo = schedule_timeout(timeo);
			sched_annotate_sleep();
			lock_sock(sk);
			finish_wait(sk_sleep(sk), &wait);

			if (!reqsk_queue_empty(queue))
				break;
			if (sk->sk_state != DCCP_LISTEN) {
				error = -EINVAL;
				goto out_err;
			}
			if (signal_pending(current)) {
				error = sock_intr_errno(timeo);
				goto out_err;
			}
			if (!timeo) {
				error = -EAGAIN;
				goto out_err;
			}
		}
	}

	req = reqsk_queue_remove(queue, sk);
	newsk = req->sk;
	arg->is_empty = reqsk_queue_empty(queue);
	arg->err = 0;

	release_sock(sk);
	dccp_reqsk_put(req);
	return newsk;

out_err:
	arg->err = error;
	release_sock(sk);
	return NULL;
}
EXPORT_SYMBOL_GPL(dccp_csk_accept);

int dccp_hash_connect(struct inet_timewait_death_row *death_row,
		      struct sock *sk)
{
	struct inet_sock *inet = inet_sk(sk);
	struct net *net = sock_net(sk);
	int low, high, range, i;
	u32 offset;

	if (inet->inet_num != 0) {
		inet->inet_sport = htons(inet->inet_num);
		return dccp_ehash_nolisten(sk, NULL, NULL) ? 0 : -EADDRNOTAVAIL;
	}

	inet_get_local_port_range(net, &low, &high);
	range = high - low + 1;
	if (range <= 0)
		return -EADDRNOTAVAIL;

	offset = get_random_u32() % range;
	for (i = 0; i < range; i++) {
		u16 port = low + ((offset + i) % range);
		bool found_dup = false;

		if (sk->sk_prot->get_port(sk, port) != 0)
			continue;

		inet->inet_sport = htons(port);
		if (dccp_ehash_nolisten(sk, NULL, &found_dup))
			return 0;

		dccp_put_port(sk);
		if (!found_dup)
			break;
	}

	inet->inet_sport = 0;
	return -EADDRNOTAVAIL;
}
EXPORT_SYMBOL_GPL(dccp_hash_connect);

int dccp_bhash2_update_saddr(struct sock *sk, void *saddr, int family)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct inet_sock *inet = inet_sk(sk);
	u16 port = inet->inet_num;
	bool rebind = (icsk->icsk_bind2_hash != NULL);
	int err = 0;

	if (rebind)
		dccp_put_port(sk);

#if IS_ENABLED(CONFIG_IPV6)
	if (family == AF_INET6) {
		sk->sk_v6_rcv_saddr = *(struct in6_addr *)saddr;
	} else
#endif
	{
		__be32 addr = *(__be32 *)saddr;

		inet->inet_saddr = addr;
		sk_rcv_saddr_set(sk, addr);
	}

	if (rebind)
		err = sk->sk_prot->get_port(sk, port);

	return err;
}
EXPORT_SYMBOL_GPL(dccp_bhash2_update_saddr);

void dccp_bhash2_reset_saddr(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct inet_sock *inet = inet_sk(sk);
	u16 port = inet->inet_num;
	bool rebind = (icsk->icsk_bind2_hash != NULL);

	if (sk->sk_userlocks & SOCK_BINDADDR_LOCK)
		return;

	if (rebind)
		dccp_put_port(sk);

	inet->inet_saddr = 0;
	sk_rcv_saddr_set(sk, 0);
#if IS_ENABLED(CONFIG_IPV6)
	if (sk->sk_family == AF_INET6)
		memset(&sk->sk_v6_rcv_saddr, 0, sizeof(sk->sk_v6_rcv_saddr));
#endif

	if (rebind && port)
		sk->sk_prot->get_port(sk, port);
}
EXPORT_SYMBOL_GPL(dccp_bhash2_reset_saddr);

struct request_sock *dccp_reqsk_alloc(const struct request_sock_ops *ops,
				      struct sock *sk_listener,
				      bool attach_listener)
{
	struct request_sock *req;

	req = kzalloc(ops->obj_size, GFP_ATOMIC);
	if (!req)
		return NULL;

	req->rsk_ops = ops;
	req_to_sk(req)->sk_prot = sk_listener->sk_prot;
	req_to_sk(req)->__sk_common.skc_nulls_node.pprev = NULL;
	req_to_sk(req)->sk_state = DCCP_NEW_SYN_RECV;
	write_pnet(&req_to_sk(req)->sk_net, sock_net(sk_listener));
	refcount_set(&req->rsk_refcnt, 0);

	if (attach_listener) {
		sock_hold(sk_listener);
		req->rsk_listener = sk_listener;
	}
	return req;
}
EXPORT_SYMBOL_GPL(dccp_reqsk_alloc);

void dccp_reqsk_free(struct request_sock *req)
{
	if (req->rsk_ops->destructor)
		req->rsk_ops->destructor(req);
	if (req->rsk_listener)
		sock_put(req->rsk_listener);
	kfree(req->saved_syn);
	kfree(req);
}
EXPORT_SYMBOL_GPL(dccp_reqsk_free);

void dccp_reqsk_put(struct request_sock *req)
{
	if (refcount_dec_and_test(&req->rsk_refcnt))
		dccp_reqsk_free(req);
}
EXPORT_SYMBOL_GPL(dccp_reqsk_put);

struct sock *dccp_csk_clone_lock(const struct sock *sk,
				 const struct request_sock *req,
				 const gfp_t priority)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	const struct inet_sock *inet = inet_sk(sk);
	struct inet_connection_sock *newicsk;
	struct inet_sock *newinet;
	struct sock *newsk;

	newsk = sk_alloc(sock_net(sk), sk->sk_family, priority, sk->sk_prot, 0);
	if (!newsk)
		return NULL;

	sock_init_data(NULL, newsk);

	newsk->sk_type		= sk->sk_type;
	newsk->sk_protocol	= sk->sk_protocol;
	newsk->sk_destruct	= sk->sk_destruct;
	newsk->sk_backlog_rcv	= sk->sk_backlog_rcv;
	newsk->sk_rcvbuf	= sk->sk_rcvbuf;
	newsk->sk_sndbuf	= sk->sk_sndbuf;
	newsk->sk_rcvlowat	= sk->sk_rcvlowat;
	newsk->sk_rcvtimeo	= sk->sk_rcvtimeo;
	newsk->sk_sndtimeo	= sk->sk_sndtimeo;
	newsk->sk_bound_dev_if	= ireq->ir_iif;
	newsk->sk_mark		= ireq->ir_mark;
	newsk->sk_priority	= READ_ONCE(sk->sk_priority);
	newsk->sk_reuse		= sk->sk_reuse;
	newsk->sk_reuseport	= sk->sk_reuseport;
	newsk->sk_ipv6only	= sk->sk_ipv6only;
	newsk->sk_userlocks	= sk->sk_userlocks & ~SOCK_BINDPORT_LOCK;
	newsk->sk_uid		= sk->sk_uid;
	sock_reset_flag(newsk, SOCK_DONE);
	sock_reset_flag(newsk, SOCK_DEAD);

	newinet = inet_sk(newsk);
	newinet->inet_num	= ireq->ir_num;
	newinet->inet_sport	= htons(ireq->ir_num);
	newinet->inet_dport	= ireq->ir_rmt_port;
	newinet->inet_daddr	= ireq->ir_rmt_addr;
	newinet->inet_saddr	= ireq->ir_loc_addr;
	newinet->inet_rcv_saddr	= ireq->ir_loc_addr;
	newinet->tos		= READ_ONCE(inet->tos);
	newinet->uc_ttl		= inet->uc_ttl;
	newinet->pmtudisc	= inet->pmtudisc;
	newinet->inet_flags	= inet->inet_flags;

#if IS_ENABLED(CONFIG_IPV6)
	if (req->rsk_ops->family == AF_INET6) {
		newsk->sk_v6_daddr = ireq->ir_v6_rmt_addr;
		newsk->sk_v6_rcv_saddr = ireq->ir_v6_loc_addr;
	} else if (sk->sk_family == AF_INET6) {
		ipv6_addr_set_v4mapped(ireq->ir_rmt_addr,
				       &newsk->sk_v6_daddr);
		ipv6_addr_set_v4mapped(ireq->ir_loc_addr,
				       &newsk->sk_v6_rcv_saddr);
	}
#endif

	newicsk = inet_csk(newsk);
	newicsk->icsk_af_ops = inet_csk(sk)->icsk_af_ops;
	newicsk->icsk_bind_hash = NULL;
	newicsk->icsk_bind2_hash = NULL;
	spin_lock_init(&newicsk->icsk_accept_queue.rskq_lock);
	inet_csk_delack_init(newsk);

	inet_sk_set_state(newsk, DCCP_RESPOND);
	bh_lock_sock(newsk);
	return newsk;
}
EXPORT_SYMBOL_GPL(dccp_csk_clone_lock);

void dccp_csk_reqsk_queue_drop(struct sock *sk, struct request_sock *req)
{
	spinlock_t *lock = inet_ehash_lockp(&dccp_hashinfo, req->rsk_hash);

	spin_lock_bh(lock);
	if (!sk_unhashed(req_to_sk(req))) {
		__sk_nulls_del_node_init_rcu(req_to_sk(req));
		reqsk_queue_removed(&inet_csk(sk)->icsk_accept_queue, req);
		dccp_reqsk_put(req);
	}
	spin_unlock_bh(lock);

	if (timer_delete(&req->rsk_timer))
		dccp_reqsk_put(req);
}
EXPORT_SYMBOL_GPL(dccp_csk_reqsk_queue_drop);

void dccp_csk_reqsk_queue_drop_and_put(struct sock *sk,
				       struct request_sock *req)
{
	dccp_csk_reqsk_queue_drop(sk, req);
	dccp_reqsk_put(req);
}
EXPORT_SYMBOL_GPL(dccp_csk_reqsk_queue_drop_and_put);

struct sock *dccp_csk_complete_hashdance(struct sock *sk, struct sock *child,
					 struct request_sock *req,
					 bool own_req)
{
	struct request_sock_queue *queue = &inet_csk(sk)->icsk_accept_queue;

	if (!own_req) {
		bh_unlock_sock(child);
		sock_put(child);
		return NULL;
	}

	dccp_csk_reqsk_queue_drop(sk, req);

	spin_lock_bh(&queue->rskq_lock);
	if (unlikely(sk->sk_state != DCCP_LISTEN)) {
		spin_unlock_bh(&queue->rskq_lock);
		dccp_csk_prepare_forced_close(child);
		dccp_done(child);
		bh_unlock_sock(child);
		sock_put(child);
		dccp_reqsk_put(req);
		return NULL;
	}

	req->sk = child;
	req->dl_next = NULL;
	if (queue->rskq_accept_head == NULL)
		WRITE_ONCE(queue->rskq_accept_head, req);
	else
		queue->rskq_accept_tail->dl_next = req;
	queue->rskq_accept_tail = req;
	sk_acceptq_added(sk);
	spin_unlock_bh(&queue->rskq_lock);

	return child;
}
EXPORT_SYMBOL_GPL(dccp_csk_complete_hashdance);

void dccp_csk_prepare_forced_close(struct sock *sk)
{
	sock_orphan(sk);
	inet_sk(sk)->inet_num = 0;
}
EXPORT_SYMBOL_GPL(dccp_csk_prepare_forced_close);

void dccp_csk_destroy_sock(struct sock *sk)
{
	WARN_ON(sk->sk_state != DCCP_CLOSED);
	WARN_ON(!sk_unhashed(sk));

	if (sk->sk_prot->destroy)
		sk->sk_prot->destroy(sk);

	sk_stream_kill_queues(sk);
	xfrm_sk_free_policy(sk);
	sock_put(sk);
}
EXPORT_SYMBOL_GPL(dccp_csk_destroy_sock);

void dccp_sk_stream_wait_close(struct sock *sk, long timeout)
{
	if (timeout) {
		DEFINE_WAIT(wait);

		do {
			prepare_to_wait(sk_sleep(sk), &wait,
					TASK_INTERRUPTIBLE);
			if (sk_wait_event(sk, &timeout,
					  !((1 << sk->sk_state) &
					    (DCCPF_CLOSING |
					     DCCPF_ACTIVE_CLOSEREQ |
					     DCCPF_PASSIVE_CLOSE |
					     DCCPF_PASSIVE_CLOSEREQ)),
					  &wait))
				break;
		} while (!signal_pending(current) && timeout);

		finish_wait(sk_sleep(sk), &wait);
	}
}
EXPORT_SYMBOL_GPL(dccp_sk_stream_wait_close);

void dccp_twsk_put(struct inet_timewait_sock *tw)
{
	if (refcount_dec_and_test(&tw->tw_refcnt))
		kfree(tw);
}
EXPORT_SYMBOL_GPL(dccp_twsk_put);

static void dccp_twsk_timer_handler(struct timer_list *t)
{
	struct inet_timewait_sock *tw = timer_container_of(tw, t, tw_timer);
	spinlock_t *lock = inet_ehash_lockp(&dccp_hashinfo, tw->tw_hash);

	spin_lock_bh(lock);
	if (!hlist_nulls_unhashed(&tw->tw_node)) {
		__hlist_nulls_del(&tw->tw_node);
		dccp_twsk_put(tw);
	}
	spin_unlock_bh(lock);

	dccp_twsk_put(tw);
}

struct inet_timewait_sock *dccp_twsk_alloc(const struct sock *sk,
					   struct inet_timewait_death_row *dr,
					   const int state)
{
	const struct inet_sock *inet = inet_sk(sk);
	struct inet_timewait_sock *tw;

	tw = kzalloc(sk->sk_prot->twsk_prot->twsk_obj_size, GFP_ATOMIC);
	if (!tw)
		return NULL;

	tw->tw_family		= sk->sk_family;
	tw->tw_state		= state;
	tw->tw_substate		= state;
	tw->tw_reuse		= sk->sk_reuse;
	tw->tw_reuseport	= sk->sk_reuseport;
	tw->tw_ipv6only		= sk->sk_ipv6only;
	tw->tw_bound_dev_if	= sk->sk_bound_dev_if;
	tw->tw_hash		= sk->sk_hash;
	tw->tw_prot		= sk->sk_prot;
	tw->tw_daddr		= inet->inet_daddr;
	tw->tw_rcv_saddr	= inet->inet_rcv_saddr;
	tw->tw_dport		= inet->inet_dport;
	tw->tw_sport		= inet->inet_sport;
	tw->tw_num		= inet->inet_num;
	tw->tw_dr		= dr;
	tw->tw_mark		= READ_ONCE(sk->sk_mark);
	tw->tw_priority		= READ_ONCE(sk->sk_priority);
	tw->tw_tos		= READ_ONCE(inet->tos);
	twsk_net_set(tw, sock_net(sk));
	refcount_set(&tw->tw_refcnt, 1);
	INIT_HLIST_NODE(&tw->tw_bind_node);
	tw->tw_node.pprev	= NULL;

	return tw;
}
EXPORT_SYMBOL_GPL(dccp_twsk_alloc);

void dccp_twsk_hashdance_schedule(struct inet_timewait_sock *tw,
				  struct sock *sk,
				  struct inet_hashinfo *hashinfo,
				  int timeo)
{
	struct inet_ehash_bucket *head = inet_ehash_bucket(hashinfo, tw->tw_hash);
	spinlock_t *lock = inet_ehash_lockp(hashinfo, tw->tw_hash);

	timer_setup(&tw->tw_timer, dccp_twsk_timer_handler, TIMER_PINNED);
	/* Two active refs: one for ehash and one for tw_timer. */
	refcount_set(&tw->tw_refcnt, 2);

	spin_lock_bh(lock);
	if (!sk_unhashed(sk)) {
		__sk_nulls_del_node_init_rcu(sk);
		sock_put(sk);
	}
	hlist_nulls_add_head_rcu(&tw->tw_node, &head->chain);
	spin_unlock_bh(lock);

	mod_timer(&tw->tw_timer, jiffies + timeo);
}
EXPORT_SYMBOL_GPL(dccp_twsk_hashdance_schedule);

void dccp_twsk_purge(struct inet_hashinfo *hashinfo)
{
	unsigned int i;

	if (!hashinfo->ehash)
		return;

	for (i = 0; i <= hashinfo->ehash_mask; i++) {
		struct inet_ehash_bucket *head = &hashinfo->ehash[i];
		spinlock_t *lock = inet_ehash_lockp(hashinfo, i);
		struct hlist_nulls_node *node;

		spin_lock_bh(lock);
		node = head->chain.first;
		while (!is_a_nulls(node)) {
			struct hlist_nulls_node *next = node->next;
			struct sock *sk = container_of(node, struct sock,
						       __sk_common.skc_nulls_node);

			if (sk->sk_state == DCCP_TIME_WAIT) {
				struct inet_timewait_sock *tw = inet_twsk(sk);

				__hlist_nulls_del(&tw->tw_node);
				if (timer_delete(&tw->tw_timer))
					dccp_twsk_put(tw);
				dccp_twsk_put(tw);
			}
			node = next;
		}
		spin_unlock_bh(lock);
	}
}
EXPORT_SYMBOL_GPL(dccp_twsk_purge);

void dccp_csk_init_xmit_timers(struct sock *sk,
			       void (*retransmit_handler)(struct timer_list *),
			       void (*delack_handler)(struct timer_list *),
			       void (*keepalive_handler)(struct timer_list *))
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	timer_setup(&sk->tcp_retransmit_timer, retransmit_handler, TIMER_PINNED);
	timer_setup(&icsk->icsk_delack_timer, delack_handler, TIMER_PINNED);
	timer_setup(&icsk->icsk_keepalive_timer, keepalive_handler, TIMER_PINNED);
	icsk->icsk_pending = 0;
	icsk->icsk_ack.pending = 0;
}
EXPORT_SYMBOL_GPL(dccp_csk_init_xmit_timers);

void dccp_csk_clear_xmit_timers(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	icsk->icsk_pending = 0;
	icsk->icsk_ack.pending = 0;
	sk_stop_timer(sk, &sk->tcp_retransmit_timer);
	sk_stop_timer(sk, &icsk->icsk_delack_timer);
	sk_stop_timer(sk, &icsk->icsk_keepalive_timer);
}
EXPORT_SYMBOL_GPL(dccp_csk_clear_xmit_timers);

int dccp_inet_sk_rebuild_header(struct sock *sk)
{
	struct inet_sock *inet = inet_sk(sk);
	struct rtable *rt = (struct rtable *)__sk_dst_check(sk, 0);
	__be32 daddr;
	struct ip_options_rcu *inet_opt;
	int err;

	if (rt)
		return 0;

	rcu_read_lock();
	inet_opt = rcu_dereference(inet->inet_opt);
	daddr = inet->inet_daddr;
	if (inet_opt && inet_opt->opt.srr)
		daddr = inet_opt->opt.faddr;
	rt = ip_route_output_ports(sock_net(sk), &inet->cork.fl.u.ip4, sk,
				   daddr, inet->inet_saddr,
				   inet->inet_dport, inet->inet_sport,
				   sk->sk_protocol,
				   inet_sk_dscp(inet),
				   sk->sk_bound_dev_if);
	rcu_read_unlock();

	if (!IS_ERR(rt)) {
		sk_setup_caps(sk, &rt->dst);
		return 0;
	}

	err = PTR_ERR(rt);
	sk->sk_route_caps = 0;
	if (!inet_opt || !inet_opt->opt.srr)
		WRITE_ONCE(sk->sk_err_soft, -err);
	return err;
}
EXPORT_SYMBOL_GPL(dccp_inet_sk_rebuild_header);

struct dst_entry *dccp_csk_route_req(const struct sock *sk, struct flowi4 *fl4,
				     const struct request_sock *req)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	struct net *net = sock_net(sk);
	struct ip_options_rcu *opt;
	struct rtable *rt;

	rcu_read_lock();
	opt = rcu_dereference(ireq->ireq_opt);
	flowi4_init_output(fl4, ireq->ir_iif, ireq->ir_mark,
			   inet_sk_dscp(inet_sk(sk)), RT_SCOPE_UNIVERSE,
			   sk->sk_protocol, inet_sk_flowi_flags(sk),
			   (opt && opt->opt.srr) ? opt->opt.faddr :
						   ireq->ir_rmt_addr,
			   ireq->ir_loc_addr, ireq->ir_rmt_port,
			   htons(ireq->ir_num), sk->sk_uid);
	security_req_classify_flow(req, flowi4_to_flowi_common(fl4));
	rt = ip_route_output_flow(net, fl4, sk);
	rcu_read_unlock();

	if (IS_ERR(rt))
		return NULL;
	return &rt->dst;
}
EXPORT_SYMBOL_GPL(dccp_csk_route_req);

struct dst_entry *dccp_csk_route_child_sock(const struct sock *sk,
					    struct sock *newsk,
					    const struct request_sock *req)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	struct net *net = read_pnet(&ireq->ireq_net);
	struct inet_sock *newinet = inet_sk(newsk);
	struct ip_options_rcu *opt;
	struct flowi4 *fl4;
	struct rtable *rt;

	opt = rcu_dereference(ireq->ireq_opt);
	fl4 = &newinet->cork.fl.u.ip4;

	flowi4_init_output(fl4, ireq->ir_iif, ireq->ir_mark,
			   inet_sk_dscp(inet_sk(sk)), RT_SCOPE_UNIVERSE,
			   sk->sk_protocol, inet_sk_flowi_flags(sk),
			   (opt && opt->opt.srr) ? opt->opt.faddr :
						   ireq->ir_rmt_addr,
			   ireq->ir_loc_addr, ireq->ir_rmt_port,
			   htons(ireq->ir_num), sk->sk_uid);
	security_req_classify_flow(req, flowi4_to_flowi_common(fl4));
	rt = ip_route_output_flow(net, fl4, sk);
	if (IS_ERR(rt))
		return NULL;

	if (opt && opt->opt.is_strictroute && rt->rt_uses_gateway) {
		ip_rt_put(rt);
		return NULL;
	}
	return &rt->dst;
}
EXPORT_SYMBOL_GPL(dccp_csk_route_child_sock);

struct dst_entry *dccp_csk_update_pmtu(struct sock *sk, u32 mtu)
{
	struct dst_entry *dst = __sk_dst_check(sk, 0);

	if (!dst) {
		if (inet_csk(sk)->icsk_af_ops->rebuild_header(sk))
			return NULL;
		dst = __sk_dst_get(sk);
	}
	if (dst)
		dst->ops->update_pmtu(dst, sk, NULL, mtu, true);
	return dst;
}
EXPORT_SYMBOL_GPL(dccp_csk_update_pmtu);

int dccp_ip_build_and_send_pkt(struct sk_buff *skb, const struct sock *sk,
			       __be32 saddr, __be32 daddr,
			       struct ip_options_rcu *opt, u8 tos)
{
	struct rtable *rt = skb_rtable(skb);
	struct net *net = sock_net(sk);
	unsigned int optlen = opt ? opt->opt.optlen : 0;
	struct iphdr *iph;
	int ttl;

	ttl = READ_ONCE(inet_sk(sk)->uc_ttl);
	if (ttl < 0)
		ttl = dst_metric_raw(&rt->dst, RTAX_HOPLIMIT);
	if (ttl <= 0)
		ttl = READ_ONCE(net->ipv4.sysctl_ip_default_ttl);

	skb_push(skb, sizeof(struct iphdr) + optlen);
	skb_reset_network_header(skb);
	iph = ip_hdr(skb);
	iph->version  = 4;
	iph->ihl      = (sizeof(struct iphdr) + optlen) >> 2;
	iph->tos      = tos;
	iph->ttl      = ttl;
	iph->daddr    = (opt && opt->opt.srr) ? opt->opt.faddr : daddr;
	iph->saddr    = saddr;
	iph->protocol = sk->sk_protocol;
	if (ip_dont_fragment(sk, &rt->dst)) {
		iph->frag_off = htons(IP_DF);
		iph->id = 0;
	} else {
		iph->frag_off = 0;
		__ip_select_ident(net, iph, 1);
	}
	if (optlen) {
		memcpy(iph + 1, opt->opt.__data, optlen);
		if (opt->opt.srr)
			memcpy((u8 *)iph + opt->opt.srr - 1 +
			       ((u8 *)iph)[opt->opt.srr - 1] - 4,
			       &daddr, 4);
	}

	skb->priority = READ_ONCE(sk->sk_priority);
	skb->mark = READ_ONCE(sk->sk_mark);
	iph->tot_len = htons(skb->len);
	ip_send_check(iph);

	return ip_local_out(net, (struct sock *)sk, skb);
}
EXPORT_SYMBOL_GPL(dccp_ip_build_and_send_pkt);

#if IS_ENABLED(CONFIG_IPV6)
int dccp_inet6_sk_rebuild_header(struct sock *sk)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct dst_entry *dst = __sk_dst_check(sk, np->dst_cookie);
	struct in6_addr *final_p, final;
	struct flowi6 fl6;

	if (dst)
		return 0;

	memset(&fl6, 0, sizeof(fl6));
	fl6.flowi6_proto = sk->sk_protocol;
	fl6.daddr = sk->sk_v6_daddr;
	fl6.saddr = np->saddr;
	fl6.flowlabel = np->flow_label;
	fl6.flowi6_oif = sk->sk_bound_dev_if;
	fl6.flowi6_mark = READ_ONCE(sk->sk_mark);
	fl6.fl6_dport = inet_sk(sk)->inet_dport;
	fl6.fl6_sport = inet_sk(sk)->inet_sport;
	fl6.flowi6_uid = sk->sk_uid;
	security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));

	rcu_read_lock();
	final_p = fl6_update_dst(&fl6, rcu_dereference(np->opt), &final);
	rcu_read_unlock();

	dst = ip6_dst_lookup_flow(sock_net(sk), sk, &fl6, final_p);
	if (IS_ERR(dst)) {
		sk->sk_route_caps = 0;
		return PTR_ERR(dst);
	}
	ip6_dst_store(sk, dst, NULL, NULL);
	return 0;
}
EXPORT_SYMBOL_GPL(dccp_inet6_sk_rebuild_header);

struct dst_entry *dccp_inet6_csk_route_req(const struct sock *sk,
					   struct dst_entry *dst,
					   struct flowi6 *fl6,
					   const struct request_sock *req,
					   u8 proto)
{
	const struct inet_request_sock *ireq = inet_rsk(req);
	const struct ipv6_pinfo *np = inet6_sk(sk);
	struct in6_addr *final_p, final;

	memset(fl6, 0, sizeof(*fl6));
	fl6->flowi6_proto = proto;
	fl6->daddr = ireq->ir_v6_rmt_addr;
	rcu_read_lock();
	final_p = fl6_update_dst(fl6, rcu_dereference(np->opt), &final);
	rcu_read_unlock();
	fl6->saddr = ireq->ir_v6_loc_addr;
	fl6->flowi6_oif = ireq->ir_iif;
	fl6->flowi6_mark = ireq->ir_mark;
	fl6->fl6_dport = ireq->ir_rmt_port;
	fl6->fl6_sport = htons(ireq->ir_num);
	fl6->flowi6_uid = sk->sk_uid;
	security_req_classify_flow(req, flowi6_to_flowi_common(fl6));

	dst = ip6_dst_lookup_flow(sock_net(sk), sk, fl6, final_p);
	if (IS_ERR(dst))
		return NULL;
	return dst;
}
EXPORT_SYMBOL_GPL(dccp_inet6_csk_route_req);

struct dst_entry *dccp_inet6_csk_update_pmtu(struct sock *sk, u32 mtu)
{
	struct dst_entry *dst = __sk_dst_check(sk, inet6_sk(sk)->dst_cookie);

	if (!dst) {
		if (dccp_inet6_sk_rebuild_header(sk))
			return NULL;
		dst = __sk_dst_get(sk);
	}
	if (dst)
		dst->ops->update_pmtu(dst, sk, NULL, mtu, true);
	return dst;
}
EXPORT_SYMBOL_GPL(dccp_inet6_csk_update_pmtu);

bool dccp_ipv6_opt_accepted(const struct sock *sk, const struct sk_buff *skb,
			    const struct inet6_skb_parm *opt)
{
	const struct ipv6_pinfo *np = inet6_sk(sk);

	if (np->rxopt.all) {
		if (((opt->flags & IP6SKB_HOPBYHOP) &&
		     (np->rxopt.bits.hopopts || np->rxopt.bits.ohopopts)) ||
		    (ip6_flowinfo(ipv6_hdr(skb)) &&
		     np->rxopt.bits.rxflow) ||
		    (opt->srcrt &&
		     (np->rxopt.bits.srcrt || np->rxopt.bits.osrcrt)) ||
		    ((opt->dst1 || opt->dst0) &&
		     (np->rxopt.bits.dstopts || np->rxopt.bits.odstopts)))
			return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(dccp_ipv6_opt_accepted);
#endif
