/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _DCCP_COMPAT_H
#define _DCCP_COMPAT_H

#include <linux/version.h>
#include <net/sock.h>

/*
 * Linux 7.1 (commit 8341c989ac77, "net: remove addr_len argument of recvmsg()
 * handlers") dropped the unused int *addr_len parameter from struct proto::recvmsg.
 */
#ifndef HAVE_4ARG_PROTO_RECVMSG
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 0)
#define HAVE_4ARG_PROTO_RECVMSG 1
#endif
#endif

#ifdef HAVE_4ARG_PROTO_RECVMSG
#define DCCP_RECVMSG_OP dccp_recvmsg
#else
int dccp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int flags);

static inline int dccp_recvmsg_compat(struct sock *sk, struct msghdr *msg,
				      size_t len, int flags, int *addr_len)
{
	return dccp_recvmsg(sk, msg, len, flags);
}
#define DCCP_RECVMSG_OP dccp_recvmsg_compat
#endif

/*
 * Linux 7.1 made CONFIG_IPV6 built-in only and removed ipv6_stub along with
 * <net/ipv6_stubs.h>. On 7.0.x, route ip6_dst_lookup_flow() through ipv6_stub.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 1, 0)
#if IS_ENABLED(CONFIG_IPV6)
#include <net/ipv6_stubs.h>

static inline struct dst_entry *
dccp_ip6_dst_lookup_flow(struct net *net, const struct sock *sk,
			 struct flowi6 *fl6, const struct in6_addr *final_dst)
{
	return ipv6_stub->ipv6_dst_lookup_flow(net, sk, fl6, final_dst);
}
#define ip6_dst_lookup_flow(net, sk, fl6, final_dst) \
	dccp_ip6_dst_lookup_flow((net), (sk), (fl6), (final_dst))
#endif /* CONFIG_IPV6 */
#endif /* < 7.1.0 */

#endif /* _DCCP_COMPAT_H */
