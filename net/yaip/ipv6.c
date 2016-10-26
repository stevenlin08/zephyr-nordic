/** @file
 * @brief ICMPv6 related functions
 */

/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef CONFIG_NET_DEBUG_IPV6
#define SYS_LOG_DOMAIN "net/ipv6"
#define NET_DEBUG 1

/* By default this prints too much data, set the value to 1 to see
 * neighbor cache contents.
 */
#define NET_DEBUG_NBR 0
#endif

#include <errno.h>
#include <net/net_core.h>
#include <net/nbuf.h>
#include <net/net_stats.h>
#include <net/net_context.h>
#include "net_private.h"
#include "icmpv6.h"
#include "ipv6.h"
#include "nbr.h"
#include "6lo.h"

#if defined(CONFIG_NET_IPV6_ND)

#define MAX_MULTICAST_SOLICIT 3
#define MAX_UNICAST_SOLICIT   3

extern void net_neighbor_data_remove(struct net_nbr *nbr);
extern void net_neighbor_table_clear(struct net_nbr_table *table);

enum net_nbr_state {
	NET_NBR_INCOMPLETE,
	NET_NBR_REACHABLE,
	NET_NBR_STALE,
	NET_NBR_DELAY,
	NET_NBR_PROBE,
};

struct net_nbr_data {
	struct in6_addr addr;
	struct nano_delayed_work reachable;
	struct nano_delayed_work send_ns;
	uint8_t ns_count;
	bool is_router;
	enum net_nbr_state state;
	uint16_t link_metric;
	struct net_buf *pending;
};

NET_NBR_POOL_INIT(net_neighbor_pool, CONFIG_NET_IPV6_MAX_NEIGHBORS,
		  sizeof(struct net_nbr_data), net_neighbor_data_remove);

NET_NBR_TABLE_INIT(NET_NBR_GLOBAL, neighbor, net_neighbor_pool,
		   net_neighbor_table_clear);

static inline bool net_is_solicited(struct net_buf *buf)
{
	return NET_ICMPV6_NA_BUF(buf)->flags & NET_ICMPV6_NA_FLAG_SOLICITED;
}

static inline bool net_is_router(struct net_buf *buf)
{
	return NET_ICMPV6_NA_BUF(buf)->flags & NET_ICMPV6_NA_FLAG_ROUTER;
}

static inline bool net_is_override(struct net_buf *buf)
{
	return NET_ICMPV6_NA_BUF(buf)->flags & NET_ICMPV6_NA_FLAG_OVERRIDE;
}

static inline struct net_nbr *get_nbr(int idx)
{
	return &net_neighbor_pool[idx].nbr;
}

static inline struct net_nbr_data *net_nbr_data(struct net_nbr *nbr)
{
	return (struct net_nbr_data *)nbr->data;
}

static inline struct net_nbr *get_nbr_from_data(struct net_nbr_data *data)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (nbr->data == (uint8_t *)data) {
			return nbr;
		}
	}

	return NULL;
}

#if NET_DEBUG_NBR
void nbr_print(void)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (!nbr->ref) {
			continue;
		}

		NET_DBG("[%d] %p %d/%d/%d/%d/%d pending %p iface %p idx %d "
			"ll %s addr %s",
			i, nbr, nbr->ref, net_nbr_data(nbr)->ns_count,
			net_nbr_data(nbr)->is_router,
			net_nbr_data(nbr)->state,
			net_nbr_data(nbr)->link_metric,
			net_nbr_data(nbr)->pending,
			nbr->iface, nbr->idx,
			nbr->idx == NET_NBR_LLADDR_UNKNOWN ? "?" :
			net_sprint_ll_addr(
				net_nbr_get_lladdr(nbr->idx)->addr,
				net_nbr_get_lladdr(nbr->idx)->len),
			net_sprint_ipv6_addr(&net_nbr_data(nbr)->addr));
	}
}
#else
#define nbr_print(...)
#endif

static struct net_nbr *nbr_lookup(struct net_nbr_table *table,
				  struct net_if *iface,
				  struct in6_addr *addr)
{
	int i;

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (!nbr->ref) {
			continue;
		}

		if (nbr->iface == iface &&
		    net_ipv6_addr_cmp(&net_nbr_data(nbr)->addr, addr)) {
			return nbr;
		}
	}

	return NULL;
}

static inline void nbr_clear_ns_pending(struct net_nbr_data *data)
{
	int ret;

	ret = nano_delayed_work_cancel(&data->send_ns);
	if (ret < 0) {
		NET_DBG("Cannot cancel NS work (%d)", ret);
	}

	net_nbuf_unref(data->pending);

	data->pending = NULL;
}

static inline void nbr_free(struct net_nbr *nbr)
{
	NET_DBG("nbr %p", nbr);

	nbr_clear_ns_pending(net_nbr_data(nbr));

	nano_delayed_work_cancel(&net_nbr_data(nbr)->reachable);

	net_nbr_unref(nbr);
}

static struct net_nbr *nbr_add(struct net_buf *buf,
			       struct in6_addr *addr,
			       struct net_linkaddr *lladdr,
			       bool is_router,
			       enum net_nbr_state state)
{
	struct net_nbr *nbr = net_nbr_get(&net_neighbor.table);

	if (!nbr) {
		return NULL;
	}

	if (net_nbr_link(nbr, net_nbuf_iface(buf), lladdr)) {
		nbr_free(nbr);
		return NULL;
	}

	net_ipaddr_copy(&net_nbr_data(nbr)->addr, addr);
	net_nbr_data(nbr)->state = state;
	net_nbr_data(nbr)->is_router = is_router;

	NET_DBG("nbr %p state %d router %d IPv6 %s ll %s",
		nbr, state, is_router,
		net_sprint_ipv6_addr(addr),
		net_sprint_ll_addr(lladdr->addr, lladdr->len));

	return nbr;
}

static struct net_nbr *nbr_new(struct net_if *iface,
			       struct in6_addr *addr,
			       enum net_nbr_state state)
{
	struct net_nbr *nbr = net_nbr_get(&net_neighbor.table);

	if (!nbr) {
		return NULL;
	}

	nbr->idx = NET_NBR_LLADDR_UNKNOWN;
	nbr->iface = iface;

	net_ipaddr_copy(&net_nbr_data(nbr)->addr, addr);
	net_nbr_data(nbr)->state = state;
	net_nbr_data(nbr)->pending = NULL;

	NET_DBG("nbr %p iface %p state %d IPv6 %s",
		nbr, iface, state, net_sprint_ipv6_addr(addr));

	return nbr;
}

void net_neighbor_data_remove(struct net_nbr *nbr)
{
	NET_DBG("Neighbor %p removed", nbr);

	return;
}

void net_neighbor_table_clear(struct net_nbr_table *table)
{
	NET_DBG("Neighbor table %p cleared", table);
}

struct in6_addr *net_ipv6_nbr_lookup_by_index(struct net_if *iface,
					      uint8_t idx)
{
	int i;

	if (idx == NET_NBR_LLADDR_UNKNOWN) {
		return NULL;
	}

	for (i = 0; i < CONFIG_NET_IPV6_MAX_NEIGHBORS; i++) {
		struct net_nbr *nbr = get_nbr(i);

		if (!nbr->ref) {
			continue;
		}

		if (iface && nbr->iface != iface) {
			continue;
		}

		if (nbr->idx == idx) {
			return &net_nbr_data(nbr)->addr;
		}
	}

	return NULL;
}
#endif /* CONFIG_NET_IPV6_ND */

struct net_buf *net_ipv6_create_raw(struct net_buf *buf,
				    uint16_t reserve,
				    const struct in6_addr *src,
				    const struct in6_addr *dst,
				    struct net_if *iface,
				    uint8_t next_header)
{
	struct net_buf *header;

	header = net_nbuf_get_reserve_data(reserve);

	net_buf_frag_insert(buf, header);

	NET_IPV6_BUF(buf)->vtc = 0x60;
	NET_IPV6_BUF(buf)->tcflow = 0;
	NET_IPV6_BUF(buf)->flow = 0;

	NET_IPV6_BUF(buf)->nexthdr = 0;
	NET_IPV6_BUF(buf)->hop_limit = net_if_ipv6_get_hop_limit(iface);

	net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst, dst);
	net_ipaddr_copy(&NET_IPV6_BUF(buf)->src, src);

	NET_IPV6_BUF(buf)->nexthdr = next_header;

	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));
	net_nbuf_set_family(buf, AF_INET6);

	net_buf_add(header, sizeof(struct net_ipv6_hdr));

	return buf;
}

struct net_buf *net_ipv6_create(struct net_context *context,
				struct net_buf *buf,
				const struct in6_addr *addr)
{
	uint8_t nexthdr;

#if defined(CONFIG_NET_UDP)
	if (net_context_get_ip_proto(context) == IPPROTO_UDP) {
		nexthdr = IPPROTO_UDP;
	}
#endif /* CONFIG_NET_UDP */

	NET_ASSERT(((struct sockaddr_in6_ptr *)&context->local)->sin6_addr);

	return net_ipv6_create_raw(buf,
				   net_nbuf_ll_reserve(buf),
				   ((struct sockaddr_in6_ptr *)
				    &context->local)->sin6_addr,
				   addr,
				   net_context_get_iface(context),
				   nexthdr);
}

struct net_buf *net_ipv6_finalize_raw(struct net_buf *buf,
				      uint8_t next_header)
{
	/* Set the length of the IPv6 header */
	size_t total_len;

	net_nbuf_compact(buf);

	total_len = net_buf_frags_len(buf->frags);

	total_len -= sizeof(struct net_ipv6_hdr);

	NET_IPV6_BUF(buf)->len[0] = total_len / 256;
	NET_IPV6_BUF(buf)->len[1] = total_len - NET_IPV6_BUF(buf)->len[0] * 256;

#if defined(CONFIG_NET_UDP)
	if (next_header == IPPROTO_UDP) {
		NET_UDP_BUF(buf)->chksum = 0;
		NET_UDP_BUF(buf)->chksum = ~net_calc_chksum_udp(buf);
	} else
#endif

#if defined(CONFIG_NET_TCP)
	if (next_header == IPPROTO_TCP) {
		NET_TCP_BUF(buf)->chksum = 0;
		NET_TCP_BUF(buf)->chksum = ~net_calc_chksum_tcp(buf);
	} else
#endif

	if (next_header == IPPROTO_ICMPV6) {
		NET_ICMP_BUF(buf)->chksum = 0;
		NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum(buf,
							     IPPROTO_ICMPV6);
	}

	return buf;
}

struct net_buf *net_ipv6_finalize(struct net_context *context,
				  struct net_buf *buf)
{
	return net_ipv6_finalize_raw(buf,
				     net_context_get_ip_proto(context));
}

#if defined(CONFIG_NET_IPV6_DAD)
int net_ipv6_start_dad(struct net_if *iface, struct net_if_addr *ifaddr)
{
	return net_ipv6_send_ns(iface, NULL, NULL, NULL,
				&ifaddr->address.in6_addr, true);
}

static inline bool dad_failed(struct net_if *iface, struct in6_addr *addr)
{
	if (net_is_ipv6_ll_addr(addr)) {
		NET_ERR("DAD failed, no ll IPv6 address!");
		return false;
	}

	net_if_ipv6_addr_rm(iface, addr);

	return true;
}
#endif /* CONFIG_NET_IPV6_DAD */

#if NET_DEBUG > 0
static inline void dbg_update_neighbor_lladdr(struct net_linkaddr *new_lladdr,
				struct net_linkaddr_storage *old_lladdr,
				struct in6_addr *addr)
{
	char out[sizeof("xx:xx:xx:xx:xx:xx:xx:xx")];

	snprintf(out, sizeof(out), net_sprint_ll_addr(old_lladdr->addr,
						      old_lladdr->len));

	NET_DBG("Updating neighbor %s lladdr %s (was %s)",
		net_sprint_ipv6_addr(addr),
		net_sprint_ll_addr(new_lladdr->addr, new_lladdr->len),
		out);
}

static inline void dbg_update_neighbor_lladdr_raw(uint8_t *new_lladdr,
				struct net_linkaddr_storage *old_lladdr,
				struct in6_addr *addr)
{
	struct net_linkaddr lladdr = {
		.len = old_lladdr->len,
		.addr = new_lladdr,
	};

	dbg_update_neighbor_lladdr(&lladdr, old_lladdr, addr);
}
#else
#define dbg_update_neighbor_lladdr(...)
#define dbg_update_neighbor_lladdr_raw(...)
#endif /* NET_DEBUG */

#if defined(CONFIG_NET_IPV6_ND)
#define NS_REPLY_TIMEOUT (sys_clock_ticks_per_sec * 1)

static void ns_reply_timeout(struct nano_work *work)
{
	/* We did not receive reply to a sent NS */
	struct net_nbr_data *data = CONTAINER_OF(work,
						 struct net_nbr_data,
						 send_ns);

	struct net_nbr *nbr = get_nbr_from_data(data);

	if (!data) {
		NET_DBG("NS timeout but no nbr data");
		return;
	}

	if (!data->pending) {
		/* Silently return, this is not an error as the work
		 * cannot be cancelled in certain cases.
		 */
		return;
	}

	NET_DBG("NS nbr %p pending %p timeout to %s", nbr, data->pending,
		net_sprint_ipv6_addr(&NET_IPV6_BUF(data->pending)->dst));

	/* To unref when pending variable was set */
	net_nbuf_unref(data->pending);

	/* To unref the original buf allocation */
	net_nbuf_unref(data->pending);

	data->pending = NULL;

	net_nbr_unref(nbr);
}

struct net_buf *net_ipv6_prepare_for_send(struct net_buf *buf)
{
	struct net_nbr *nbr;

	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			 &NET_IPV6_BUF(buf)->dst);

	NET_DBG("Neighbor lookup %p (%d) iface %p addr %s", nbr,
		nbr ? nbr->idx : NET_NBR_LLADDR_UNKNOWN,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->dst));

	if (nbr && nbr->idx != NET_NBR_LLADDR_UNKNOWN) {
		struct net_linkaddr_storage *lladdr;

		lladdr = net_nbr_get_lladdr(nbr->idx);

		net_nbuf_ll_dst(buf)->addr = lladdr->addr;
		net_nbuf_ll_dst(buf)->len = lladdr->len;

		NET_DBG("Neighbor %p addr %s", nbr,
			net_sprint_ll_addr(lladdr->addr, lladdr->len));

		return buf;
	}

	/* We need to send NS and wait for NA before sending the packet. */
	if (net_ipv6_send_ns(net_nbuf_iface(buf),
			     buf,
			     &NET_IPV6_BUF(buf)->src,
			     NULL,
			     &NET_IPV6_BUF(buf)->dst,
			     false) < 0) {
		/* In case of an error, the NS send function will unref
		 * the buf.
		 */
		return NULL;
	}

	NET_DBG("Buf %p will be sent later", buf);

	return NULL;
}

struct net_nbr *net_ipv6_nbr_lookup(struct net_if *iface,
				    struct in6_addr *addr)
{
	return nbr_lookup(&net_neighbor.table, iface, addr);
}

static inline uint8_t get_llao_len(struct net_if *iface)
{
	if (iface->link_addr.len == 6) {
		return 8;
	} else if (iface->link_addr.len == 8) {
		return 16;
	}

	/* What else could it be? */
	NET_ASSERT_INFO(0, "Invalid link address length %d",
			iface->link_addr.len);

	return 0;
}

static inline void set_llao(struct net_linkaddr *lladdr,
			    uint8_t *llao, uint8_t llao_len, uint8_t type)
{
	llao[NET_ICMPV6_OPT_TYPE_OFFSET] = type;
	llao[NET_ICMPV6_OPT_LEN_OFFSET] = llao_len >> 3;

	memcpy(&llao[NET_ICMPV6_OPT_DATA_OFFSET], lladdr->addr, lladdr->len);

	memset(&llao[NET_ICMPV6_OPT_DATA_OFFSET + lladdr->len], 0,
	       llao_len - lladdr->len - 2);
}

static void setup_headers(struct net_buf *buf, uint8_t nd6_len,
			  uint8_t icmp_type)
{
	NET_IPV6_BUF(buf)->vtc = 0x60;
	NET_IPV6_BUF(buf)->tcflow = 0;
	NET_IPV6_BUF(buf)->flow = 0;
	NET_IPV6_BUF(buf)->len[0] = 0;
	NET_IPV6_BUF(buf)->len[1] = NET_ICMPH_LEN + nd6_len;

	NET_IPV6_BUF(buf)->nexthdr = IPPROTO_ICMPV6;
	NET_IPV6_BUF(buf)->hop_limit = NET_IPV6_ND_HOP_LIMIT;

	NET_ICMP_BUF(buf)->type = icmp_type;
	NET_ICMP_BUF(buf)->code = 0;
}

static inline void handle_ns_neighbor(struct net_buf *buf,
				      struct net_icmpv6_nd_opt_hdr *hdr)
{
	struct net_nbr *nbr;
	struct net_linkaddr lladdr = {
		.len = 8 * hdr->len - 2,
		.addr = (uint8_t *)hdr + 2,
	};

	/**
	 * IEEE802154 lladdress is 8 bytes long, so it requires
	 * 2 * 8 bytes - 2 - padding.
	 * The formula above needs to be adjusted.
	 */
	if (net_nbuf_ll_src(buf)->len < lladdr.len) {
		lladdr.len = net_nbuf_ll_src(buf)->len;
	}

	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			 &NET_IPV6_BUF(buf)->src);

	NET_DBG("Neighbor lookup %p iface %p addr %s", nbr,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));

	if (!nbr) {
		nbr_print();

		nbr = nbr_new(net_nbuf_iface(buf),
			      &NET_IPV6_BUF(buf)->src, NET_NBR_INCOMPLETE);
		if (nbr) {
			NET_DBG("Added %s to nbr cache",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));
		} else {
			NET_ERR("Could not add neighbor %s",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));

			return;
		}
	}

	if (net_nbr_link(nbr, net_nbuf_iface(buf), &lladdr) == -EALREADY) {
		/* Update the lladdr if the node was already known */
		struct net_linkaddr_storage *cached_lladdr;

		cached_lladdr = net_nbr_get_lladdr(nbr->idx);

		if (memcmp(cached_lladdr->addr, lladdr.addr, lladdr.len)) {

			dbg_update_neighbor_lladdr(&lladdr,
						   cached_lladdr,
						   &NET_IPV6_BUF(buf)->src);

			cached_lladdr->len = lladdr.len;
			memcpy(cached_lladdr->addr, lladdr.addr, lladdr.len);

			net_nbr_data(nbr)->state = NET_NBR_STALE;
		} else {
			if (net_nbr_data(nbr)->state == NET_NBR_INCOMPLETE) {
				net_nbr_data(nbr)->state = NET_NBR_STALE;
			}
		}
	}
}

#if NET_DEBUG
#define dbg_addr(action, pkt_str, src, dst)				\
	do {								\
		char out[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx")];	\
									\
		snprintf(out, sizeof(out), net_sprint_ipv6_addr(dst));	\
									\
		NET_DBG("%s %s from %s to %s", action,			\
			pkt_str, net_sprint_ipv6_addr(src), out);	\
									\
	} while (0)

#define dbg_addr_recv(pkt_str, src, dst)	\
	dbg_addr("Received", pkt_str, src, dst)

#define dbg_addr_sent(pkt_str, src, dst)	\
	dbg_addr("Sent", pkt_str, src, dst)

#define dbg_addr_with_tgt(action, pkt_str, src, dst, target)		\
	do {								\
		char out[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx")];	\
		char tgt[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx")];	\
									\
		snprintf(out, sizeof(out), net_sprint_ipv6_addr(dst));	\
		snprintf(tgt, sizeof(tgt), net_sprint_ipv6_addr(target)); \
									\
		NET_DBG("%s %s from %s to %s, target %s", action,	\
			pkt_str, net_sprint_ipv6_addr(src), out, tgt);	\
									\
	} while (0)

#define dbg_addr_recv_tgt(pkt_str, src, dst, tgt)		\
	dbg_addr_with_tgt("Received", pkt_str, src, dst, tgt)

#define dbg_addr_sent_tgt(pkt_str, src, dst, tgt)		\
	dbg_addr_with_tgt("Sent", pkt_str, src, dst, tgt)
#else
#define dbg_addr(...)
#define dbg_addr_recv(...)
#define dbg_addr_sent(...)

#define dbg_addr_with_tgt(...)
#define dbg_addr_recv_tgt(...)
#define dbg_addr_sent_tgt(...)
#endif

static enum net_verdict handle_ns_input(struct net_buf *buf)
{
	uint16_t total_len = net_buf_frags_len(buf);
	struct net_icmpv6_nd_opt_hdr *hdr;
	struct net_if_addr *ifaddr;
	uint8_t flags = 0, llao_len, prev_opt_len = 0;
	size_t left_len;

	dbg_addr_recv_tgt("Neighbor Solicitation",
			  &NET_IPV6_BUF(buf)->src,
			  &NET_IPV6_BUF(buf)->dst,
			  &NET_ICMPV6_NS_BUF(buf)->tgt);

	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.recv);

	if ((total_len < (sizeof(struct net_ipv6_hdr) +
			  sizeof(struct net_icmp_hdr) +
			  sizeof(struct net_icmpv6_ns_hdr))) ||
	    (NET_ICMP_BUF(buf)->code != 0) ||
	    (NET_IPV6_BUF(buf)->hop_limit != NET_IPV6_ND_HOP_LIMIT) ||
	    net_is_ipv6_addr_mcast(&NET_ICMPV6_NS_BUF(buf)->tgt)) {
		NET_DBG("Preliminary check failed %u/%u, code %u, hop %u",
			total_len, (sizeof(struct net_ipv6_hdr) +
				    sizeof(struct net_icmp_hdr) +
				    sizeof(struct net_icmpv6_ns_hdr)),
			NET_ICMP_BUF(buf)->code, NET_IPV6_BUF(buf)->hop_limit);
		goto drop;
	}

	net_nbuf_set_ext_opt_len(buf, sizeof(struct net_icmpv6_ns_hdr));
	hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);

	/* The parsing gets tricky if the ND struct is split
	 * between two fragments. FIXME later.
	 */
	if (buf->frags->len < ((uint8_t *)hdr - buf->frags->data)) {
		NET_DBG("NS struct split between fragments");
		goto drop;
	}

	left_len = buf->frags->len - (sizeof(struct net_ipv6_hdr) +
				      sizeof(struct net_icmp_hdr));

	while (net_nbuf_ext_opt_len(buf) < left_len &&
	       left_len < buf->frags->len) {

		if (!hdr->len) {
			break;
		}

		switch (hdr->type) {
		case NET_ICMPV6_ND_OPT_SLLAO:
			if (net_is_ipv6_addr_unspecified(
				    &NET_IPV6_BUF(buf)->src)) {
				goto drop;
			}

			handle_ns_neighbor(buf, hdr);
			break;

		default:
			NET_DBG("Unknown ND option 0x%x", hdr->type);
			break;
		}

		prev_opt_len = net_nbuf_ext_opt_len(buf);

		net_nbuf_set_ext_opt_len(buf, net_nbuf_ext_opt_len(buf) +
					 (hdr->len << 3));

		if (prev_opt_len == net_nbuf_ext_opt_len(buf)) {
			NET_ERR("Corrupted NS message");
			goto drop;
		}

		hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);
	}

	ifaddr = net_if_ipv6_addr_lookup_by_iface(net_nbuf_iface(buf),
						  &NET_ICMPV6_NS_BUF(buf)->tgt);
	if (!ifaddr) {
		NET_DBG("No such interface address %s",
			net_sprint_ipv6_addr(&NET_ICMPV6_NS_BUF(buf)->tgt));
		goto drop;
	}

#if !defined(CONFIG_NET_IPV6_DAD)
	if (net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src)) {
		goto drop;
	}

#else /* CONFIG_NET_IPV6_DAD */

	/* Do DAD */
	if (net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src)) {

		if (!net_is_ipv6_addr_solicited_node(&NET_IPV6_BUF(buf)->dst)) {
			NET_DBG("Not solicited node addr %s",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->dst));
			goto drop;
		}

		if (ifaddr->addr_state == NET_ADDR_TENTATIVE) {
			NET_DBG("DAD failed for %s iface %p",
				net_sprint_ipv6_addr(&ifaddr->address.in6_addr));
			dad_failed(net_nbuf_iface(buf),
				   &ifaddr->address.in6_addr);
			goto drop;
		}

		/* We reuse the received buffer to send the NA */
		net_ipv6_addr_create_ll_allnodes_mcast(&NET_IPV6_BUF(buf)->dst);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				net_if_ipv6_select_src_addr(net_nbuf_iface(buf),
						&NET_IPV6_BUF(buf)->dst));
		flags = NET_ICMPV6_NA_FLAG_OVERRIDE;
		goto send_na;
	}
#endif /* CONFIG_NET_IPV6_DAD */

	if (net_is_my_ipv6_addr(&NET_IPV6_BUF(buf)->src)) {
		NET_DBG("Duplicate IPv6 %s address",
			net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));
		goto drop;
	}

	/* Address resolution */
	if (net_is_ipv6_addr_solicited_node(&NET_IPV6_BUF(buf)->dst)) {
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst,
				&NET_IPV6_BUF(buf)->src);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				&NET_ICMPV6_NS_BUF(buf)->tgt);
		flags = NET_ICMPV6_NA_FLAG_SOLICITED |
			NET_ICMPV6_NA_FLAG_OVERRIDE;
		goto send_na;
	}

	/* Neighbor Unreachability Detection (NUD) */
	if (net_if_ipv6_addr_lookup_by_iface(net_nbuf_iface(buf),
					     &NET_IPV6_BUF(buf)->dst)) {
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst,
				&NET_IPV6_BUF(buf)->src);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				&NET_ICMPV6_NS_BUF(buf)->tgt);
		flags = NET_ICMPV6_NA_FLAG_SOLICITED |
			NET_ICMPV6_NA_FLAG_OVERRIDE;
		goto send_na;
	} else {
		NET_DBG("NUD failed");
		goto drop;
	}

send_na:
	llao_len = get_llao_len(net_nbuf_iface(buf));

	net_nbuf_set_ext_len(buf, 0);

	setup_headers(buf, sizeof(struct net_icmpv6_na_hdr) + llao_len,
		      NET_ICMPV6_NA);

	net_ipaddr_copy(&NET_ICMPV6_NA_BUF(buf)->tgt,
			&ifaddr->address.in6_addr);

	set_llao(&net_nbuf_iface(buf)->link_addr,
		 net_nbuf_icmp_data(buf) + sizeof(struct net_icmp_hdr) +
					sizeof(struct net_icmpv6_na_hdr),
		 llao_len, NET_ICMPV6_ND_OPT_TLLAO);

	NET_ICMPV6_NA_BUF(buf)->flags = flags;

	NET_ICMP_BUF(buf)->chksum = 0;
	NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum_icmpv6(buf);

	net_nbuf_set_len(buf->frags, NET_IPV6ICMPH_LEN +
			 sizeof(struct net_icmpv6_na_hdr) +
			 llao_len);

	if (net_send_data(buf) < 0) {
		goto drop;
	}

	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.sent);

	return NET_OK;

drop:
	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.drop);
	return NET_DROP;
}

static void nd_reachable_timeout(struct nano_work *work)
{
	struct net_nbr_data *data = CONTAINER_OF(work,
						 struct net_nbr_data,
						 send_ns);

	struct net_nbr *nbr = get_nbr_from_data(data);

	if (!data || !nbr) {
		NET_DBG("ND reachable timeout but no nbr data");
		return;
	}

	switch (data->state) {

	case NET_NBR_INCOMPLETE:
		if (data->ns_count >= MAX_MULTICAST_SOLICIT) {
			nbr_free(nbr);
		} else {
			data->ns_count++;

			NET_DBG("nbr %p incomplete count %u", nbr,
				data->ns_count);

			net_ipv6_send_ns(nbr->iface, NULL, NULL, NULL,
					 &data->addr, false);
		}
		break;

	case NET_NBR_REACHABLE:
		data->state = NET_NBR_STALE;

		NET_DBG("nbr %p moving %s state to STALE (%d)",
			nbr, net_sprint_ipv6_addr(&data->addr), data->state);
		break;

	case NET_NBR_STALE:
		NET_DBG("nbr %p removing stale address %s",
			nbr, net_sprint_ipv6_addr(&data->addr));
		nbr_free(nbr);
		break;

	case NET_NBR_DELAY:
		data->state = NET_NBR_PROBE;
		data->ns_count = 0;

		NET_DBG("nbr %p moving %s state to PROBE (%d)",
			nbr, net_sprint_ipv6_addr(&data->addr), data->state);
		break;

	case NET_NBR_PROBE:
		if (data->ns_count >= MAX_UNICAST_SOLICIT) {
			struct net_if_router *router;

			router = net_if_ipv6_router_lookup(nbr->iface,
							   &data->addr);
			if (router && !router->is_infinite) {
				NET_DBG("nbr %p address %s PROBE ended",
					nbr, net_sprint_ipv6_addr(&data->addr),
					data->state);

				net_if_router_rm(router);
				nbr_free(nbr);
			}
		} else {
			data->ns_count++;

			NET_DBG("nbr %p probe count %u", nbr,
				data->ns_count);

			net_ipv6_send_ns(nbr->iface, NULL, NULL, NULL,
					 &data->addr, false);
		}
		break;
	}
}

static inline void set_reachable_timeout(struct net_if *iface,
					 struct net_nbr *nbr)
{
	uint32_t time;

	time = MSEC(net_if_ipv6_get_reachable_time(iface));

	NET_ASSERT_INFO(time, "Zero reachable timeout!");

	nano_delayed_work_init(&net_nbr_data(nbr)->reachable,
			       nd_reachable_timeout);

	nano_delayed_work_submit(&net_nbr_data(nbr)->reachable, time);
}

static inline bool handle_na_neighbor(struct net_buf *buf,
				      struct net_icmpv6_nd_opt_hdr *hdr,
				      uint8_t *tllao)
{
	bool lladdr_changed = false;
	struct net_nbr *nbr;
	struct net_linkaddr_storage *cached_lladdr;
	struct net_buf *pending;

	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			 &NET_ICMPV6_NS_BUF(buf)->tgt);

	NET_DBG("Neighbor lookup %p iface %p addr %s", nbr,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(&NET_ICMPV6_NS_BUF(buf)->tgt));

	if (!nbr) {
		nbr_print();

		NET_DBG("No such neighbor found, msg discarded");
		return false;
	}

	if (nbr->idx == NET_NBR_LLADDR_UNKNOWN) {
		struct net_linkaddr lladdr;

		if (!tllao) {
			NET_DBG("No target link layer address.");
			return false;
		}

		lladdr.len = net_nbuf_iface(buf)->link_addr.len;
		lladdr.addr = &tllao[NET_ICMPV6_OPT_DATA_OFFSET];

		if (net_nbr_link(nbr, net_nbuf_iface(buf), &lladdr)) {
			nbr_free(nbr);
			return false;
		}

		NET_DBG("nbr %p state %d IPv6 %s ll %s",
			nbr, net_nbr_data(nbr)->state,
			net_sprint_ipv6_addr(&NET_ICMPV6_NS_BUF(buf)->tgt),
			net_sprint_ll_addr(lladdr.addr, lladdr.len));
	}

	cached_lladdr = net_nbr_get_lladdr(nbr->idx);
	if (!cached_lladdr) {
		NET_DBG("No lladdr but index defined");
		return false;
	}

	if (tllao) {
		lladdr_changed = memcmp(&tllao[NET_ICMPV6_OPT_DATA_OFFSET],
					cached_lladdr->addr,
					cached_lladdr->len);
	}

	/* Update the cached address if we do not yet known it */
	if (net_nbr_data(nbr)->state == NET_NBR_INCOMPLETE) {
		if (!tllao) {
			return false;
		}

		if (lladdr_changed) {
			dbg_update_neighbor_lladdr_raw(
				&tllao[NET_ICMPV6_OPT_DATA_OFFSET],
				cached_lladdr,
				&NET_ICMPV6_NS_BUF(buf)->tgt);

			memcpy(cached_lladdr->addr,
			       &tllao[NET_ICMPV6_OPT_DATA_OFFSET],
			       cached_lladdr->len);
		}

		if (net_is_solicited(buf)) {
			net_nbr_data(nbr)->state = NET_NBR_REACHABLE;
			net_nbr_data(nbr)->ns_count = 0;

			set_reachable_timeout(net_nbuf_iface(buf), nbr);
		} else {
			net_nbr_data(nbr)->state = NET_NBR_STALE;
		}

		net_nbr_data(nbr)->is_router = net_is_router(buf);

		goto send_pending;
	}

	/* We do not update the address if override bit is not set
	 * and we have a valid address in the cache.
	 */
	if (!net_is_override(buf) && lladdr_changed) {
		if (net_nbr_data(nbr)->state == NET_NBR_REACHABLE) {
			net_nbr_data(nbr)->state = NET_NBR_STALE;
		}

		return false;
	}

	if (net_is_override(buf) ||
	    (!net_is_override(buf) && tllao && !lladdr_changed)) {

		if (lladdr_changed) {
			dbg_update_neighbor_lladdr_raw(
				&tllao[NET_ICMPV6_OPT_DATA_OFFSET],
				cached_lladdr,
				&NET_ICMPV6_NS_BUF(buf)->tgt);

			memcpy(cached_lladdr->addr,
			       &tllao[NET_ICMPV6_OPT_DATA_OFFSET],
			       cached_lladdr->len);
		}

		if (net_is_solicited(buf)) {
			net_nbr_data(nbr)->state = NET_NBR_REACHABLE;

			set_reachable_timeout(net_nbuf_iface(buf), nbr);
		} else {
			if (lladdr_changed) {
				net_nbr_data(nbr)->state = NET_NBR_STALE;
			}
		}
	}

	if (net_nbr_data(nbr)->is_router && !net_is_router(buf)) {
		/* Update the routing if the peer is no longer
		 * a router.
		 */
		/* FIXME */
	}

	net_nbr_data(nbr)->is_router = net_is_router(buf);

send_pending:
	/* Next send any pending messages to the peer. */
	pending = net_nbr_data(nbr)->pending;

	if (pending) {
		NET_DBG("Sending pending %p to %s lladdr %s", pending,
			net_sprint_ipv6_addr(&NET_IPV6_BUF(pending)->dst),
			net_sprint_ll_addr(cached_lladdr->addr,
					   cached_lladdr->len));

		if (net_send_data(pending) < 0) {
			net_nbuf_unref(pending);
			nbr_clear_ns_pending(net_nbr_data(nbr));
		} else {
			net_nbr_data(nbr)->pending = NULL;
		}
	}

	return true;
}

static enum net_verdict handle_na_input(struct net_buf *buf)
{
	uint16_t total_len = net_buf_frags_len(buf);
	struct net_icmpv6_nd_opt_hdr *hdr;
	struct net_if_addr *ifaddr;
	uint8_t *tllao = NULL;
	uint8_t prev_opt_len = 0;
	size_t left_len;

	dbg_addr_recv_tgt("Neighbor Advertisement",
			  &NET_IPV6_BUF(buf)->src,
			  &NET_IPV6_BUF(buf)->dst,
			  &NET_ICMPV6_NS_BUF(buf)->tgt);

	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.recv);

	if ((total_len < (sizeof(struct net_ipv6_hdr) +
			  sizeof(struct net_icmp_hdr) +
			  sizeof(struct net_icmpv6_na_hdr) +
			  sizeof(struct net_icmpv6_nd_opt_hdr))) ||
	    (NET_ICMP_BUF(buf)->code != 0) ||
	    (NET_IPV6_BUF(buf)->hop_limit != NET_IPV6_ND_HOP_LIMIT) ||
	    net_is_ipv6_addr_mcast(&NET_ICMPV6_NS_BUF(buf)->tgt) ||
	    (net_is_solicited(buf) &&
	     net_is_ipv6_addr_mcast(&NET_IPV6_BUF(buf)->dst))) {
		goto drop;
	}

	net_nbuf_set_ext_opt_len(buf, sizeof(struct net_icmpv6_na_hdr));
	hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);

	/* The parsing gets tricky if the ND struct is split
	 * between two fragments. FIXME later.
	 */
	if (buf->frags->len < ((uint8_t *)hdr - buf->frags->data)) {
		NET_DBG("NA struct split between fragments");
		goto drop;
	}

	left_len = buf->frags->len - (sizeof(struct net_ipv6_hdr) +
				      sizeof(struct net_icmp_hdr));

	while (net_nbuf_ext_opt_len(buf) < left_len &&
	       left_len < buf->frags->len) {

		if (!hdr->len) {
			break;
		}

		switch (hdr->type) {
		case NET_ICMPV6_ND_OPT_TLLAO:
			tllao = (uint8_t *)hdr;
			break;

		default:
			NET_DBG("Unknown ND option 0x%x", hdr->type);
			break;
		}

		prev_opt_len = net_nbuf_ext_opt_len(buf);

		net_nbuf_set_ext_opt_len(buf, net_nbuf_ext_opt_len(buf) +
					 (hdr->len << 3));

		if (prev_opt_len == net_nbuf_ext_opt_len(buf)) {
			NET_ERR("Corrupted NA message");
			goto drop;
		}

		hdr = NET_ICMPV6_ND_OPT_HDR_BUF(buf);
	}

	ifaddr = net_if_ipv6_addr_lookup_by_iface(net_nbuf_iface(buf),
						  &NET_ICMPV6_NA_BUF(buf)->tgt);
	if (ifaddr) {
		NET_DBG("Interface %p already has address %s",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&NET_ICMPV6_NA_BUF(buf)->tgt));

#if defined(CONFIG_NET_IPV6_DAD)
		if (ifaddr->addr_state == NET_ADDR_TENTATIVE) {
			dad_failed(net_nbuf_iface(buf),
				   &NET_ICMPV6_NA_BUF(buf)->tgt);
		}
#endif /* CONFIG_NET_IPV6_DAD */

		goto drop;
	}

	if (!handle_na_neighbor(buf, hdr, tllao)) {
		goto drop;
	}

	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.sent);
	return NET_OK;

drop:
	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.drop);
	return NET_DROP;
}

int net_ipv6_send_ns(struct net_if *iface,
		     struct net_buf *pending,
		     struct in6_addr *src,
		     struct in6_addr *dst,
		     struct in6_addr *tgt,
		     bool is_my_address)
{
	struct net_buf *buf, *frag;
	struct net_nbr *nbr;
	uint8_t llao_len;

	buf = net_nbuf_get_reserve_tx(0);

	NET_ASSERT_INFO(buf, "Out of TX buffers");

	frag = net_nbuf_get_reserve_data(net_if_get_ll_reserve(iface, dst));

	NET_ASSERT_INFO(frag, "Out of DATA buffers");

	net_buf_frag_add(buf, frag);

	net_nbuf_set_ll_reserve(buf, net_buf_headroom(frag));
	net_nbuf_set_iface(buf, iface);
	net_nbuf_set_family(buf, AF_INET6);
	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));

	net_nbuf_ll_clear(buf);

	llao_len = get_llao_len(net_nbuf_iface(buf));

	setup_headers(buf, sizeof(struct net_icmpv6_ns_hdr) + llao_len,
		      NET_ICMPV6_NS);

	if (!dst) {
		net_ipv6_addr_create_solicited_node(tgt,
						    &NET_IPV6_BUF(buf)->dst);
	} else {
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst, dst);
	}

	NET_ICMPV6_NS_BUF(buf)->reserved = 0;

	net_ipaddr_copy(&NET_ICMPV6_NS_BUF(buf)->tgt, tgt);

	if (is_my_address) {
		/* DAD */
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				net_if_ipv6_unspecified_addr());

		NET_IPV6_BUF(buf)->len[1] -= llao_len;

		net_buf_add(frag,
			    sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_ns_hdr));
	} else {
		if (src) {
			net_ipaddr_copy(&NET_IPV6_BUF(buf)->src, src);
		} else {
			net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
					net_if_ipv6_select_src_addr(
						net_nbuf_iface(buf),
						&NET_IPV6_BUF(buf)->dst));
		}

		if (net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src)) {
			NET_DBG("No source address for NS");
			goto drop;
		}

		set_llao(&net_nbuf_iface(buf)->link_addr,
			 net_nbuf_icmp_data(buf) +
			 sizeof(struct net_icmp_hdr) +
			 sizeof(struct net_icmpv6_ns_hdr),
			 llao_len, NET_ICMPV6_ND_OPT_SLLAO);

		net_buf_add(frag,
			    sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_ns_hdr) + llao_len);
	}

	NET_ICMP_BUF(buf)->chksum = 0;
	NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum_icmpv6(buf);

	nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			 &NET_ICMPV6_NS_BUF(buf)->tgt);
	if (!nbr) {
		nbr_print();

		nbr = nbr_new(net_nbuf_iface(buf),
			      &NET_ICMPV6_NS_BUF(buf)->tgt,
			      NET_NBR_INCOMPLETE);
		if (!nbr) {
			NET_DBG("Could not create new neighbor %s",
				net_sprint_ipv6_addr(
						&NET_ICMPV6_NS_BUF(buf)->tgt));
			goto drop;
		}
	}

	if (pending) {
		if (!net_nbr_data(nbr)->pending) {
			net_nbr_data(nbr)->pending = net_nbuf_ref(pending);
		} else {
			NET_DBG("Buffer %p already pending for "
				"operation. Discarding pending %p and buf %p",
				net_nbr_data(nbr)->pending, pending, buf);
			net_nbuf_unref(pending);
			goto drop;
		}

		NET_DBG("Setting timeout %d for NS", NS_REPLY_TIMEOUT);

		nano_delayed_work_init(&net_nbr_data(nbr)->send_ns,
				       ns_reply_timeout);
		nano_delayed_work_submit(&net_nbr_data(nbr)->send_ns,
					 NS_REPLY_TIMEOUT);
	}

	dbg_addr_sent_tgt("Neighbor Solicitation",
			  &NET_IPV6_BUF(buf)->src,
			  &NET_IPV6_BUF(buf)->dst,
			  &NET_ICMPV6_NS_BUF(buf)->tgt);

	if (net_send_data(buf) < 0) {
		goto drop;
	}

	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.sent);

	return 0;

drop:
	net_nbuf_unref(buf);
	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.drop);
	return -EINVAL;
}

int net_ipv6_send_rs(struct net_if *iface)
{
	struct net_buf *buf, *frag;
	bool unspec_src;
	uint8_t llao_len = 0;

	buf = net_nbuf_get_reserve_tx(0);

	frag = net_nbuf_get_reserve_data(
		net_if_get_ll_reserve(iface, &NET_IPV6_BUF(buf)->dst));

	net_buf_frag_add(buf, frag);

	net_nbuf_set_ll_reserve(buf, net_buf_headroom(frag));
	net_nbuf_set_iface(buf, iface);
	net_nbuf_set_family(buf, AF_INET6);
	net_nbuf_set_ip_hdr_len(buf, sizeof(struct net_ipv6_hdr));

	net_nbuf_ll_clear(buf);

	net_ipv6_addr_create_ll_allnodes_mcast(&NET_IPV6_BUF(buf)->dst);

	net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
			net_if_ipv6_select_src_addr(iface,
						    &NET_IPV6_BUF(buf)->dst));

	unspec_src = net_is_ipv6_addr_unspecified(&NET_IPV6_BUF(buf)->src);
	if (!unspec_src) {
		llao_len = get_llao_len(net_nbuf_iface(buf));
	}

	setup_headers(buf, sizeof(struct net_icmpv6_rs_hdr) + llao_len,
		      NET_ICMPV6_RS);

	if (!unspec_src) {
		set_llao(&net_nbuf_iface(buf)->link_addr,
			 net_nbuf_icmp_data(buf) +
			 sizeof(struct net_icmp_hdr) +
			 sizeof(struct net_icmpv6_rs_hdr),
			 llao_len, NET_ICMPV6_ND_OPT_SLLAO);

		net_buf_add(frag, sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_rs_hdr) +
			    llao_len);
	} else {
		net_buf_add(frag, sizeof(struct net_ipv6_hdr) +
			    sizeof(struct net_icmp_hdr) +
			    sizeof(struct net_icmpv6_rs_hdr));
	}

	NET_ICMP_BUF(buf)->chksum = 0;
	NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum_icmpv6(buf);

	dbg_addr_sent("Router Solicitation",
		      &NET_IPV6_BUF(buf)->src,
		      &NET_IPV6_BUF(buf)->dst);

	if (net_send_data(buf) < 0) {
		goto drop;
	}

	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.sent);

	return 0;

drop:
	net_nbuf_unref(buf);
	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.drop);
	return -EINVAL;
}

int net_ipv6_start_rs(struct net_if *iface)
{
	return net_ipv6_send_rs(iface);
}

static inline struct net_buf *handle_ra_neighbor(struct net_buf *buf,
						 struct net_buf *frag,
						 uint8_t len,
						 uint16_t offset, uint16_t *pos,
						 struct net_nbr **nbr)

{
	struct net_linkaddr lladdr;
	uint8_t padding;

	if (!nbr) {
		return NULL;
	}

	if (net_nbuf_ll_src(buf)->len < lladdr.len) {
		lladdr.len = net_nbuf_ll_src(buf)->len;
	}

	frag = net_nbuf_read(frag, offset, pos, lladdr.len, lladdr.addr);
	if (!frag && offset) {
		return NULL;
	}

	padding = len * 8 - 2 - lladdr.len;
	if (padding) {
		frag = net_nbuf_read(frag, *pos, pos, padding, NULL);
		if (!frag && *pos) {
			return NULL;
		}
	}

	*nbr = nbr_lookup(&net_neighbor.table, net_nbuf_iface(buf),
			  &NET_IPV6_BUF(buf)->src);

	NET_DBG("Neighbor lookup %p iface %p addr %s", *nbr,
		net_nbuf_iface(buf),
		net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src));

	if (!*nbr) {
		nbr_print();

		*nbr = nbr_add(buf, &NET_IPV6_BUF(buf)->src, &lladdr,
			       true, NET_NBR_STALE);
		if (!*nbr) {
			NET_ERR("Could not add router neighbor %s [%s]",
				net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src),
				net_sprint_ll_addr(lladdr.addr, lladdr.len));
			return NULL;
		}
	}

	if (net_nbr_link(*nbr, net_nbuf_iface(buf), &lladdr) == -EALREADY) {
		/* Update the lladdr if the node was already known */
		struct net_linkaddr_storage *cached_lladdr;

		cached_lladdr = net_nbr_get_lladdr((*nbr)->idx);

		if (memcmp(cached_lladdr->addr, lladdr.addr, lladdr.len)) {

			dbg_update_neighbor_lladdr(&lladdr,
						   cached_lladdr,
						   &NET_IPV6_BUF(buf)->src);

			cached_lladdr->len = lladdr.len;
			memcpy(cached_lladdr->addr, lladdr.addr, lladdr.len);

			net_nbr_data(*nbr)->state = NET_NBR_STALE;
		} else {
			if (net_nbr_data(*nbr)->state == NET_NBR_INCOMPLETE) {
				net_nbr_data(*nbr)->state = NET_NBR_STALE;
			}
		}
	}

	net_nbr_data(*nbr)->is_router = true;

	return frag;
}

static inline void handle_prefix_onlink(struct net_buf *buf,
			struct net_icmpv6_nd_opt_prefix_info *prefix_info)
{
	struct net_if_ipv6_prefix *prefix;

	prefix = net_if_ipv6_prefix_lookup(net_nbuf_iface(buf),
					   &prefix_info->prefix,
					   prefix_info->len);
	if (!prefix) {
		if (!prefix_info->valid_lifetime) {
			return;
		}

		prefix = net_if_ipv6_prefix_add(net_nbuf_iface(buf),
					&prefix_info->prefix,
					prefix_info->len,
					ntohl(prefix_info->valid_lifetime));
		if (prefix) {
			NET_DBG("Interface %p add prefix %s/%d lifetime %lu",
				net_nbuf_iface(buf),
				net_sprint_ipv6_addr(&prefix_info->prefix),
				prefix_info->prefix_len,
				ntohl(prefix_info->valid_lifetime));
		} else {
			NET_ERR("Prefix %s/%d could not be added to iface %p",
				net_sprint_ipv6_addr(&prefix_info->prefix),
				prefix_info->len,
				net_nbuf_iface(buf));

			return;
		}
	}

	switch (prefix_info->valid_lifetime) {
	case 0:
		NET_DBG("Interface %p delete prefix %s/%d",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&prefix_info->prefix),
			prefix_info->len);

		net_if_ipv6_prefix_rm(net_nbuf_iface(buf),
				      &prefix->prefix,
				      prefix->len);
		break;

	case NET_IPV6_ND_INFINITE_LIFETIME:
		NET_DBG("Interface %p prefix %s/%d infinite",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&prefix->prefix),
			prefix->len);

		net_if_ipv6_prefix_set_lf(prefix, true);
		break;

	default:
		NET_DBG("Interface %p update prefix %s/%d lifetime %lu",
			net_nbuf_iface(buf),
			net_sprint_ipv6_addr(&prefix_info->prefix),
			prefix_info->prefix_len,
			ntohl(prefix_info->valid_lifetime));

		net_if_ipv6_prefix_set_lf(prefix, false);
		net_if_ipv6_prefix_set_timer(prefix,
					ntohl(prefix_info->valid_lifetime));
		break;
	}
}

#define TWO_HOURS (2 * 60 * 60)

static inline uint32_t remaining(struct nano_delayed_work *work)
{
	if (work->timeout.delta_ticks_from_prev < 0) {
		return NET_IPV6_ND_INFINITE_LIFETIME;
	}

	return (uint32_t)work->timeout.delta_ticks_from_prev /
		sys_clock_ticks_per_sec;
}

static inline void submit_work(struct nano_delayed_work *work,
			       uint32_t time_in_sec)
{
	int32_t ticks = (int32_t)(time_in_sec * sys_clock_ticks_per_sec);

	NET_ASSERT_INFO(ticks >= 0, "Too long timeout");

	nano_delayed_work_submit(work, ticks);
}

static inline void handle_prefix_autonomous(struct net_buf *buf,
			struct net_icmpv6_nd_opt_prefix_info *prefix_info)
{
	struct in6_addr addr = { 0 };
	struct net_if_addr *ifaddr;

	net_ipaddr_copy(&addr, &prefix_info->prefix);

	net_ipv6_addr_create_iid(&addr,
				 net_if_get_link_addr(net_nbuf_iface(buf)));

	ifaddr = net_if_ipv6_addr_lookup(&addr, NULL);
	if (ifaddr && ifaddr->addr_type == NET_ADDR_AUTOCONF) {
		if (prefix_info->valid_lifetime ==
		    NET_IPV6_ND_INFINITE_LIFETIME) {
			net_if_addr_set_lf(ifaddr, true);
			return;
		}

		/* RFC 4862 ch 5.5.3 */
		if ((ntohl(prefix_info->valid_lifetime) > TWO_HOURS) ||
			(ntohl(prefix_info->valid_lifetime) >
			 remaining(&ifaddr->lifetime))) {
			NET_DBG("Timer updating for address %s "
				"lifetime %lu secs", &addr,
				ntohl(prefix_info->valid_lifetime));

			submit_work(&ifaddr->lifetime,
				    ntohl(prefix_info->valid_lifetime));
		} else {
			NET_DBG("Timer updating for address %s "
				"lifetime %lu secs", &addr, TWO_HOURS);
			submit_work(&ifaddr->lifetime, TWO_HOURS);
		}
		net_if_addr_set_lf(ifaddr, false);
	} else {
		if (ntohl(prefix_info->valid_lifetime) ==
		    NET_IPV6_ND_INFINITE_LIFETIME) {
			net_if_ipv6_addr_add(net_nbuf_iface(buf),
					     &addr, NET_ADDR_AUTOCONF, 0);
		} else {
			net_if_ipv6_addr_add(net_nbuf_iface(buf),
					&addr, NET_ADDR_AUTOCONF,
					ntohl(prefix_info->valid_lifetime));
		}
	}
}

static inline struct net_buf *handle_ra_prefix(struct net_buf *buf,
					       struct net_buf *frag,
					       uint8_t len,
					       uint16_t offset, uint16_t *pos)
{
	struct net_icmpv6_nd_opt_prefix_info prefix_info;

	prefix_info.type = NET_ICMPV6_ND_OPT_PREFIX_INFO;
	prefix_info.len = len * 8 - 2;

	frag = net_nbuf_read(frag, offset, pos, 1, &prefix_info.prefix_len);
	frag = net_nbuf_read(frag, *pos, pos, 1, &prefix_info.flags);
	frag = net_nbuf_read_be32(frag, *pos, pos, &prefix_info.valid_lifetime);
	frag = net_nbuf_read_be32(frag, *pos, pos,
				  &prefix_info.preferred_lifetime);
	/* Skip reserved bytes */
	frag = net_nbuf_skip(frag, *pos, pos, 4);
	frag = net_nbuf_read(frag, *pos, pos, 16, prefix_info.prefix.s6_addr);
	if (!frag && *pos) {
		return NULL;
	}

	if (prefix_info.valid_lifetime >= prefix_info.preferred_lifetime &&
	    !net_is_ipv6_ll_addr(&prefix_info.prefix)) {

		if (prefix_info.flags & NET_ICMPV6_RA_FLAG_ONLINK) {
			handle_prefix_onlink(buf, &prefix_info);
		}

		if ((prefix_info.flags & NET_ICMPV6_RA_FLAG_AUTONOMOUS) &&
		    prefix_info.valid_lifetime &&
		    (prefix_info.prefix_len == NET_IPV6_DEFAULT_PREFIX_LEN)) {
			handle_prefix_autonomous(buf, &prefix_info);
		}
	}

	return frag;
}

#if defined(CONFIG_NET_6LO_CONTEXT)
/* 6lowpan Context Option RFC 6775, 4.2 */
static inline struct net_buf *handle_ra_6co(struct net_buf *buf,
					    struct net_buf *frag,
					    uint8_t len,
					    uint16_t offset, uint16_t *pos)
{
	struct net_icmpv6_nd_opt_6co context;

	context.type = NET_ICMPV6_ND_OPT_6CO;
	context.len = len * 8 - 2;

	frag = net_nbuf_read_u8(frag, offset, pos, &context.context_len);
	frag = net_nbuf_read_u8(frag, *pos, pos, &context.flag);

	/* Skip reserved bytes */
	frag = net_nbuf_skip(frag, *pos, pos, 2);
	frag = net_nbuf_read_be16(frag, *pos, pos, &context.lifetime);

	/* RFC 6775, 4.2 (Length field). Length can be 2 or 3 depending
	 * on the length of context prefix field.
	 */
	if (len == 3) {
		frag = net_nbuf_read(frag, *pos, pos, sizeof(struct in6_addr),
				     context.prefix.s6_addr);
	} else if (len == 2) {
		/* If length is 2 means only 64 bits of context prefix
		 * is available, rest set to zeros.
		 */
		frag = net_nbuf_read(frag, *pos, pos, 8,
				     context.prefix.s6_addr);
		memset(context.prefix.s6_addr + 8, 0, 8);
	}

	if (!frag && *pos) {
		return NULL;
	}

	net_6lo_set_context(net_nbuf_iface(buf), &context);

	return frag;
}
#endif

static enum net_verdict handle_ra_input(struct net_buf *buf)
{
	uint16_t total_len = net_buf_frags_len(buf);
	struct net_nbr *nbr = NULL;
	struct net_if_router *router;
	struct net_buf *frag;
	uint16_t offset;
	uint8_t length;
	uint8_t type;
	uint32_t mtu;

	dbg_addr_recv("Router Advertisement",
		      &NET_IPV6_BUF(buf)->src,
		      &NET_IPV6_BUF(buf)->dst);

	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.recv);

	if ((total_len < (sizeof(struct net_ipv6_hdr) +
			  sizeof(struct net_icmp_hdr) +
			  sizeof(struct net_icmpv6_ra_hdr) +
			  sizeof(struct net_icmpv6_nd_opt_hdr))) ||
	    (NET_ICMP_BUF(buf)->code != 0) ||
	    (NET_IPV6_BUF(buf)->hop_limit != NET_IPV6_ND_HOP_LIMIT) ||
	    !net_is_ipv6_ll_addr(&NET_IPV6_BUF(buf)->src)) {
		goto drop;
	}

	if (NET_ICMPV6_RA_BUF(buf)->cur_hop_limit) {
		net_ipv6_set_hop_limit(net_nbuf_iface(buf),
				       NET_ICMPV6_RA_BUF(buf)->cur_hop_limit);
		NET_DBG("New hop limit %d",
			net_if_ipv6_get_hop_limit(net_nbuf_iface(buf)));
	}

	if (NET_ICMPV6_RA_BUF(buf)->reachable_time &&
	    (net_if_ipv6_get_reachable_time(net_nbuf_iface(buf)) !=
	     ntohl(NET_ICMPV6_RA_BUF(buf)->reachable_time))) {
		net_if_ipv6_set_base_reachable_time(net_nbuf_iface(buf),
			    ntohl(NET_ICMPV6_RA_BUF(buf)->reachable_time));

		net_if_ipv6_set_reachable_time(net_nbuf_iface(buf));
	}

	if (NET_ICMPV6_RA_BUF(buf)->retrans_timer) {
		net_if_ipv6_set_retrans_timer(net_nbuf_iface(buf),
			      ntohl(NET_ICMPV6_RA_BUF(buf)->retrans_timer));
	}

	frag = buf->frags;
	offset = sizeof(struct net_ipv6_hdr) +
		 sizeof(struct net_icmp_hdr) +
		 sizeof(struct net_icmpv6_ra_hdr);

	while (frag) {
		frag = net_nbuf_read(frag, offset, &offset, 1, &type);
		frag = net_nbuf_read(frag, offset, &offset, 1, &length);
		if (!frag) {
			goto drop;
		}

		switch (type) {
		case NET_ICMPV6_ND_OPT_SLLAO:
			frag = handle_ra_neighbor(buf, frag, length, offset,
						  &offset, &nbr);
			if (!frag && offset) {
				goto drop;
			}

			break;
		case NET_ICMPV6_ND_OPT_MTU:
			/* MTU has reserved 2 bytes, so skip it. */
			frag = net_nbuf_skip(frag, offset, &offset, 2);
			frag = net_nbuf_read_be32(frag, offset, &offset, &mtu);
			if (!frag && offset) {
				goto drop;
			}

			net_if_set_mtu(net_nbuf_iface(buf), mtu);

			if (mtu > 0xffff) {
				/* TODO: discard packet? */
				NET_ERR("MTU %lu, max is %d", mtu, 0xffff);
			}

			break;
		case NET_ICMPV6_ND_OPT_PREFIX_INFO:
			frag = handle_ra_prefix(buf, frag, length, offset,
						&offset);
			if (!frag && offset) {
				goto drop;
			}

			break;
#if defined(CONFIG_NET_6LO_CONTEXT)
		case NET_ICMPV6_ND_OPT_6CO:
			/* RFC 6775, 4.2 (Length)*/
			if (!(length == 2 || length == 3)) {
				NET_ERR("Invalid 6CO length %d", length);
				goto drop;
			}

			frag = handle_ra_6co(buf, frag, length, offset,
					     &offset);
			if (!frag && offset) {
				goto drop;
			}

			break;
#endif
#if defined(CONFIG_NET_IPV6_RA_RDNSS)
		case NET_ICMPV6_ND_OPT_RDNSS:
			NET_DBG("RDNSS option skipped");
#endif
		default:
			NET_DBG("Unknown ND option 0x%x", type);
			frag = net_nbuf_skip(frag, offset, &offset,
					     length * 8 - 2);
			if (!frag && offset) {
				goto drop;
			}

			break;
		}
	}

	router = net_if_ipv6_router_lookup(net_nbuf_iface(buf),
					   &NET_IPV6_BUF(buf)->src);
	if (router) {
		if (!NET_ICMPV6_RA_BUF(buf)->router_lifetime) {
			/*TODO: Start rs_timer on iface if no routers
			 * at all available on iface.
			 */
			net_if_router_rm(router);
		} else {
			if (nbr) {
				net_nbr_data(nbr)->is_router = true;
			}

			submit_work(&router->lifetime, (uint32_t)
				ntohs(NET_ICMPV6_RA_BUF(buf)->router_lifetime));
		}
	} else {
		net_if_ipv6_router_add(net_nbuf_iface(buf),
				&NET_IPV6_BUF(buf)->src,
				ntohs(NET_ICMPV6_RA_BUF(buf)->router_lifetime));
	}

	if (nbr && net_nbr_data(nbr)->pending) {
		NET_DBG("Sending pending buf %p to %s",
			net_nbr_data(nbr)->pending,
			net_sprint_ipv6_addr(&NET_IPV6_BUF(
					net_nbr_data(nbr)->pending)->dst));

		if (net_send_data(net_nbr_data(nbr)->pending) < 0) {
			net_nbuf_unref(net_nbr_data(nbr)->pending);
		}

		nbr_clear_ns_pending(net_nbr_data(nbr));
	}

	/* Cancel the RS timer on iface */
	nano_delayed_work_cancel(&net_nbuf_iface(buf)->rs_timer);

	net_nbuf_unref(buf);

	return NET_OK;

drop:
	NET_STATS_IPV6_ND(++net_stats.ipv6_nd.drop);

	return NET_DROP;
}
#endif /* CONFIG_NET_IPV6_ND */

#if defined(CONFIG_NET_IPV6_ND)
static struct net_icmpv6_handler ns_input_handler = {
	.type = NET_ICMPV6_NS,
	.code = 0,
	.handler = handle_ns_input,
};

static struct net_icmpv6_handler na_input_handler = {
	.type = NET_ICMPV6_NA,
	.code = 0,
	.handler = handle_na_input,
};

static struct net_icmpv6_handler ra_input_handler = {
	.type = NET_ICMPV6_RA,
	.code = 0,
	.handler = handle_ra_input,
};
#endif /* CONFIG_NET_IPV6_ND */

void net_ipv6_init(void)
{
#if defined(CONFIG_NET_IPV6_ND)
	net_icmpv6_register_handler(&ns_input_handler);
	net_icmpv6_register_handler(&na_input_handler);
	net_icmpv6_register_handler(&ra_input_handler);
#endif
}
