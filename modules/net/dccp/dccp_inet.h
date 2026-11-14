/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DCCP_INET_H
#define _DCCP_INET_H

#include <linux/types.h>
#include <net/inet_connection_sock.h>
#include <net/inet_hashtables.h>
#include <net/inet_timewait_sock.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/request_sock.h>
#include <net/sock.h>

int dccp_hashinfo2_init_mod(struct inet_hashinfo *h);
int dccp_ehash_locks_alloc(struct inet_hashinfo *hashinfo);

bool dccp_ehash_insert(struct sock *sk, struct sock *osk, bool *found_dup_sk);
bool dccp_ehash_nolisten(struct sock *sk, struct sock *osk, bool *found_dup_sk);
int dccp_inet_hash(struct sock *sk);
void dccp_inet_unhash(struct sock *sk);

int dccp_csk_listen_start(struct sock *sk);
int dccp_hash_connect(struct inet_timewait_death_row *death_row,
		      struct sock *sk);

int dccp_bhash2_update_saddr(struct sock *sk, void *saddr, int family);
void dccp_bhash2_reset_saddr(struct sock *sk);

struct sock *dccp_csk_clone_lock(const struct sock *sk,
				 const struct request_sock *req,
				 const gfp_t priority);
void dccp_csk_reqsk_queue_drop(struct sock *sk, struct request_sock *req);
void dccp_csk_reqsk_queue_drop_and_put(struct sock *sk,
				       struct request_sock *req);
struct sock *dccp_csk_complete_hashdance(struct sock *sk, struct sock *child,
					 struct request_sock *req,
					 bool own_req);

struct inet_timewait_sock *dccp_twsk_alloc(const struct sock *sk,
					   struct inet_timewait_death_row *dr,
					   const int state);
void dccp_twsk_hashdance_schedule(struct inet_timewait_sock *tw,
				  struct sock *sk,
				  struct inet_hashinfo *hashinfo,
				  int timeo);
void dccp_twsk_put(struct inet_timewait_sock *tw);
void dccp_twsk_purge(struct inet_hashinfo *hashinfo);

void dccp_csk_init_xmit_timers(struct sock *sk,
			       void (*retransmit_handler)(struct timer_list *),
			       void (*delack_handler)(struct timer_list *),
			       void (*keepalive_handler)(struct timer_list *));
void dccp_csk_clear_xmit_timers(struct sock *sk);

struct dst_entry *dccp_csk_route_req(const struct sock *sk, struct flowi4 *fl4,
				     const struct request_sock *req);
struct dst_entry *dccp_csk_update_pmtu(struct sock *sk, u32 mtu);

#if IS_ENABLED(CONFIG_IPV6)
int dccp_inet6_sk_rebuild_header(struct sock *sk);
struct dst_entry *dccp_inet6_csk_route_req(const struct sock *sk,
					   struct dst_entry *dst,
					   struct flowi6 *fl6,
					   const struct request_sock *req,
					   u8 proto);
struct dst_entry *dccp_inet6_csk_update_pmtu(struct sock *sk, u32 mtu);
bool dccp_ipv6_opt_accepted(const struct sock *sk, const struct sk_buff *skb,
			    const struct inet6_skb_parm *opt);
#endif

#endif /* _DCCP_INET_H */
