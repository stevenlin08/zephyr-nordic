/*
 * Copyright (c) 2016 Intel Corporation.
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

#if defined(CONFIG_NETWORK_IP_STACK_DEBUG_IF)
#define SYS_LOG_DOMAIN "net/if"
#define NET_DEBUG 1
#endif

#include <init.h>
#include <nanokernel.h>
#include <sections.h>
#include <string.h>
#include <net/net_core.h>
#include <net/nbuf.h>
#include <net/net_if.h>
#include <net/arp.h>

#include "net_private.h"

/* net_if dedicated section limiters */
extern struct net_if __net_if_start[];
extern struct net_if __net_if_end[];

static void net_if_tx_fiber(struct net_if *iface)
{
	struct net_if_api *api = (struct net_if_api *)iface->dev->driver_api;

	NET_DBG("Starting TX fiber (stack %d bytes) for driver %p",
		sizeof(iface->tx_fiber_stack), api);

	while (1) {
		struct net_buf *buf;

		/* Get next packet from application - wait if necessary */
		buf = nano_fifo_get(&iface->tx_queue, TICKS_UNLIMITED);

		NET_DBG("Processing (buf %p, data len %u) network packet",
			buf, net_buf_frags_len(buf->frags));

		if (api && api->send) {

#if defined(CONFIG_NET_IPV4)
			if (iface->capabilities & NET_CAP_ARP) {
				buf = net_arp_prepare(buf);
				if (!buf) {
					/* Packet was discarded */
					continue;
				}

				/* ARP packet will be sent now and the actual
				 * packet is sent after the ARP reply has been
				 * received.
				 */
			}
#endif /* CONFIG_NET_IPV4 */

			if (api->send(iface, buf) < 0) {
				net_nbuf_unref(buf);
			}
		} else {
			net_nbuf_unref(buf);
		}

		net_analyze_stack("TX fiber", iface->tx_fiber_stack,
				  sizeof(iface->tx_fiber_stack));
	}
}

static inline void init_tx_queue(struct net_if *iface)
{
	nano_fifo_init(&iface->tx_queue);

	fiber_start(iface->tx_fiber_stack, sizeof(iface->tx_fiber_stack),
		    (nano_fiber_entry_t)net_if_tx_fiber, (int)iface, 0, 7, 0);
}

struct net_if *net_if_get_by_link_addr(struct net_linkaddr *ll_addr)
{
	struct net_if *iface;

	for (iface = __net_if_start; iface != __net_if_end; iface++) {
		if (!memcmp(iface->link_addr.addr, ll_addr->addr,
			    ll_addr->len)) {
			return iface;
		}
	}

	return NULL;
}

struct net_if_addr *net_if_ipv6_addr_lookup(struct in6_addr *addr)
{
#if defined(CONFIG_NET_IPV6)
	struct net_if *iface;

	for (iface = __net_if_start; iface != __net_if_end; iface++) {
		int i;

		for (i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
			if (!iface->ipv6.unicast[i].is_used ||
			    iface->ipv6.unicast[i].address.family != AF_INET6) {
				continue;
			}

			if (net_is_ipv6_prefix(addr->s6_addr,
				iface->ipv6.unicast[i].address.in6_addr.s6_addr,
					       128)) {
				return &iface->ipv6.unicast[i];
			}
		}
	}
#endif

	return NULL;
}

struct net_if_addr *net_if_ipv6_addr_add(struct net_if *iface,
					 struct in6_addr *addr,
					 enum net_addr_type addr_type,
					 uint32_t vlifetime)
{
#if defined(CONFIG_NET_IPV6)
	int i;

	for (i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
		if (iface->ipv6.unicast[i].is_used) {
			continue;
		}

		iface->ipv6.unicast[i].is_used = true;
		iface->ipv6.unicast[i].address.family = AF_INET6;
		iface->ipv6.unicast[i].addr_type = addr_type;
		memcpy(&iface->ipv6.unicast[i].address.in6_addr, addr, 16);

		/* FIXME - set the mcast addr for this node */

		if (vlifetime) {
			iface->ipv6.unicast[i].is_infinite = false;

			/* FIXME - set the timer */
		} else {
			iface->ipv6.unicast[i].is_infinite = true;
		}

		NET_DBG("[%d] interface %p address %s type %s added", i, iface,
			net_sprint_ipv6_addr(addr),
			net_addr_type2str(addr_type));

		return &iface->ipv6.unicast[i];
	}
#endif

	return NULL;
}

struct net_if_mcast_addr *net_if_ipv6_maddr_add(struct net_if *iface,
						struct in6_addr *addr)
{
#if defined(CONFIG_NET_IPV6)
	int i;

	if (!net_is_ipv6_addr_mcast(addr)) {
		NET_DBG("Address %s is not a multicast address.",
			net_sprint_ipv6_addr(addr));
		return NULL;
	}

	for (i = 0; i < NET_IF_MAX_IPV6_MADDR; i++) {
		if (iface->ipv6.mcast[i].is_used) {
			continue;
		}

		iface->ipv6.mcast[i].is_used = true;
		memcpy(&iface->ipv6.mcast[i].address.in6_addr, addr, 16);

		NET_DBG("[%d] interface %p address %s added", i, iface,
			net_sprint_ipv6_addr(addr));

		return &iface->ipv6.mcast[i];
	}
#endif

	return NULL;
}

struct net_if_mcast_addr *net_if_ipv6_maddr_lookup(struct in6_addr *maddr)
{
#if defined(CONFIG_NET_IPV6)
	struct net_if *iface;

	for (iface = __net_if_start; iface != __net_if_end; iface++) {
		int i;

		for (i = 0; i < NET_IF_MAX_IPV6_MADDR; i++) {
			if (!iface->ipv6.mcast[i].is_used ||
			    iface->ipv6.mcast[i].address.family != AF_INET6) {
				continue;
			}

			if (net_is_ipv6_prefix(maddr->s6_addr,
				iface->ipv6.mcast[i].address.in6_addr.s6_addr,
					       128)) {
				return &iface->ipv6.mcast[i];
			}
		}
	}
#endif

	return NULL;
}

struct in6_addr *net_if_ipv6_unspecified_addr(void)
{
#if defined(CONFIG_NET_IPV6)
	static struct in6_addr addr = IN6ADDR_ANY_INIT;

	return &addr;
#else
	return NULL;
#endif
}

const struct in_addr *net_if_ipv4_broadcast_addr(void)
{
#if defined(CONFIG_NET_IPV4)
	static const struct in_addr addr = { { { 255, 255, 255, 255 } } };

	return &addr;
#else
	return NULL;
#endif
}

bool net_if_ipv4_addr_mask_cmp(struct net_if *iface,
			       struct in_addr *addr)
{
#if defined(CONFIG_NET_IPV4)
	uint32_t subnet = ntohl(addr->s_addr[0]) &
			ntohl(iface->ipv4.netmask.s_addr[0]);
	int i;

	for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (!iface->ipv4.unicast[i].is_used ||
		    iface->ipv4.unicast[i].address.family != AF_INET) {
				continue;
		}
		if ((ntohl(iface->ipv4.unicast[i].address.in_addr.s_addr[0]) &
		     ntohl(iface->ipv4.netmask.s_addr[0])) == subnet) {
			return true;
		}
	}
#endif

	return false;
}

struct in6_addr *net_if_ipv6_get_ll(struct net_if *iface,
				    enum net_addr_state addr_state)
{
	int i;

	for (i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
		if (!iface->ipv6.unicast[i].is_used ||
		    (addr_state != NET_ADDR_ANY_STATE &&
		     iface->ipv6.unicast[i].addr_state != addr_state) ||
		    iface->ipv6.unicast[i].address.family != AF_INET6) {
			continue;
		}
		if (net_is_ipv6_ll_addr(&iface->ipv6.unicast[i].address.in6_addr)) {
			return &iface->ipv6.unicast[i].address.in6_addr;
		}
	}

	return NULL;
}

static inline uint8_t get_length(struct in6_addr *src, struct in6_addr *dst)
{
	uint8_t j, k, xor;
	uint8_t len = 0;

	for (j = 0; j < 16; j++) {
		if (src->s6_addr[j] == dst->s6_addr[j]) {
			len += 8;
		} else {
			xor = src->s6_addr[j] ^ dst->s6_addr[j];
			for (k = 0; k < 8; k++) {
				if (!(xor & 0x80)) {
					len++;
					xor <<= 1;
				} else {
					break;
				}
			}
			break;
		}
	}

	return len;
}

static inline bool is_proper_ipv6_address(struct net_if_addr *addr)
{
	if (addr->is_used && addr->addr_state == NET_ADDR_PREFERRED &&
	    addr->address.family == AF_INET6 &&
	    !net_is_ipv6_ll_addr(&addr->address.in6_addr)) {
		return true;
	}

	return false;
}

static inline struct in6_addr *net_if_ipv6_get_best_match(struct net_if *iface,
							  struct in6_addr *dst,
							  uint8_t *best_so_far)
{
#if defined(CONFIG_NET_IPV6)
	struct in6_addr *src = NULL;
	uint8_t i, len;

	for (i = 0; i < NET_IF_MAX_IPV6_ADDR; i++) {
		if (!is_proper_ipv6_address(&iface->ipv6.unicast[i])) {
			continue;
		}

		len = get_length(dst,
				 &iface->ipv6.unicast[i].address.in6_addr);
		if (len >= *best_so_far) {
			*best_so_far = len;
			src = &iface->ipv6.unicast[i].address.in6_addr;
		}
	}

	return src;
#else
	return NULL;
#endif
}

struct in6_addr *net_if_ipv6_select_src_addr(struct net_if *dst_iface,
					     struct in6_addr *dst)
{
#if defined(CONFIG_NET_IPV6)
	struct in6_addr *src = NULL;
	uint8_t best_match = 0;
	struct net_if *iface;

	if (!net_is_ipv6_ll_addr(dst) && !net_is_ipv6_addr_mcast(dst)) {

		for (iface = __net_if_start;
		     !dst_iface && iface != __net_if_end;
		     iface++) {
			struct in6_addr *addr;

			addr = net_if_ipv6_get_best_match(iface, dst,
							  &best_match);
			if (addr) {
				src = addr;
			}
		}

		/* If caller has supplied interface, then use that */
		if (dst_iface) {
			src = net_if_ipv6_get_best_match(dst_iface, dst,
							 &best_match);
		}

	} else {
		for (iface = __net_if_start;
		     !dst_iface && iface != __net_if_end;
		     iface++) {
			struct in6_addr *addr;

			addr = net_if_ipv6_get_ll(iface, NET_ADDR_PREFERRED);
			if (addr) {
				src = addr;
				break;
			}
		}

		if (dst_iface) {
			src = net_if_ipv6_get_ll(dst_iface, NET_ADDR_PREFERRED);
		}
	}

	if (!src) {
		return net_if_ipv6_unspecified_addr();
	}

	return src;
#else
	return NULL;
#endif
}

struct net_if_addr *net_if_ipv4_addr_lookup(struct in_addr *addr)
{
#if defined(CONFIG_NET_IPV4)
	struct net_if *iface;

	for (iface = __net_if_start; iface != __net_if_end; iface++) {
		int i;

		for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			if (!iface->ipv4.unicast[i].is_used ||
			    iface->ipv4.unicast[i].address.family != AF_INET) {
				continue;
			}

			if (addr->s4_addr32[0] ==
			    iface->ipv4.unicast[i].address.in_addr.s_addr[0]) {
				return &iface->ipv4.unicast[i];
			}
		}
	}
#endif

	return NULL;
}

struct net_if_addr *net_if_ipv4_addr_add(struct net_if *iface,
					 struct in_addr *addr,
					 enum net_addr_type addr_type,
					 uint32_t vlifetime)
{
#if defined(CONFIG_NET_IPV4)
	int i;

	for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (iface->ipv4.unicast[i].is_used) {
			continue;
		}

		iface->ipv4.unicast[i].is_used = true;
		iface->ipv4.unicast[i].address.family = AF_INET;
		iface->ipv4.unicast[i].address.in_addr.s4_addr32[0] =
						addr->s4_addr32[0];
		iface->ipv4.unicast[i].addr_type = addr_type;

		if (vlifetime) {
			iface->ipv4.unicast[i].is_infinite = false;

			/* FIXME - set the timer */
		} else {
			iface->ipv4.unicast[i].is_infinite = true;
			iface->ipv4.unicast[i].addr_state = NET_ADDR_PREFERRED;
		}

		NET_DBG("[%d] interface %p address %s type %s added", i, iface,
			net_sprint_ipv4_addr(addr),
			net_addr_type2str(addr_type));

		return &iface->ipv4.unicast[i];
	}
#endif

	return NULL;
}

struct net_if *net_if_get_default(void)
{
	return __net_if_start;
}

int net_if_init(void)
{
	struct net_if_api *api;
	struct net_if *iface;

	for (iface = __net_if_start; iface != __net_if_end; iface++) {
		api = (struct net_if_api *) iface->dev->driver_api;

		if (api && api->init) {
			api->init(iface);

			if (api->capabilities) {
				iface->capabilities = api->capabilities(iface);
			} else {
				iface->capabilities = 0;
			}

			init_tx_queue(iface);
		}

#if defined(CONFIG_NET_IPV4)
		if (iface->capabilities & NET_CAP_ARP) {
			net_arp_init();
		}
#endif

#if defined(CONFIG_NET_IPV6)
		iface->hop_limit = CONFIG_NET_INITIAL_HOP_LIMIT;
#endif
	}

	return 0;
}
