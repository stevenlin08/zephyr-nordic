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

#ifdef CONFIG_NETWORK_IP_STACK_DEBUG_ICMPV6
#define SYS_LOG_DOMAIN "net/icmpv6"
#define NET_DEBUG 1
#endif

#include <errno.h>
#include <misc/slist.h>
#include <net/net_core.h>
#include <net/nbuf.h>
#include <net/net_if.h>
#include <net/net_stats.h>
#include "net_private.h"
#include "icmpv6.h"

static sys_slist_t handlers;

void net_icmpv6_register_handler(struct net_icmpv6_handler *handler)
{
	sys_slist_prepend(&handlers, &handler->node);
}

static enum net_verdict handle_echo_request(struct net_buf *buf)
{
	/* Note that we send the same data buffers back and just swap
	 * the addresses etc.
	 */
#if NET_DEBUG > 0
	char out[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx")];

	snprintf(out, sizeof(out),
		 net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->dst));
	NET_DBG("Received Echo Request from %s to %s",
		net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src), out);
#endif /* NET_DEBUG > 0 */

	if (net_is_ipv6_addr_mcast(&NET_IPV6_BUF(buf)->dst)) {
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst,
				&NET_IPV6_BUF(buf)->src);

		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				net_if_ipv6_select_src_addr(net_nbuf_iface(buf),
						&NET_IPV6_BUF(buf)->dst));
	} else {
		struct in6_addr addr;

		net_ipaddr_copy(&addr, &NET_IPV6_BUF(buf)->src);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->src,
				&NET_IPV6_BUF(buf)->dst);
		net_ipaddr_copy(&NET_IPV6_BUF(buf)->dst, &addr);
	}

	NET_IPV6_BUF(buf)->hop_limit =
		net_if_ipv6_get_hop_limit(net_nbuf_iface(buf));

	NET_ICMP_BUF(buf)->type = NET_ICMPV6_ECHO_REPLY;
	NET_ICMP_BUF(buf)->code = 0;
	NET_ICMP_BUF(buf)->chksum = 0;
	NET_ICMP_BUF(buf)->chksum = ~net_calc_chksum_icmpv6(buf);

#if NET_DEBUG > 0
	snprintf(out, sizeof(out),
		 net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->dst));
	NET_DBG("Sending Echo Reply from %s to %s",
		net_sprint_ipv6_addr(&NET_IPV6_BUF(buf)->src), out);
#endif /* NET_DEBUG > 0 */

	if (net_send_data(buf) < 0) {
		NET_STATS(++net_stats.icmp.drop);
		return NET_DROP;
	}

	NET_STATS(++net_stats.icmp.sent);

	return NET_OK;
}

enum net_verdict net_icmpv6_input(struct net_buf *buf, uint16_t len,
				  uint8_t type, uint8_t code)
{
	sys_snode_t *node;
	struct net_icmpv6_handler *cb;

	SYS_SLIST_FOR_EACH_NODE(&handlers, node) {
		cb = (struct net_icmpv6_handler *)node;

		if (!cb) {
			continue;
		}

		if (cb->type == type && (cb->code == code || cb->code == 0)) {
			NET_STATS(++net_stats.icmp.recv);
			return cb->handler(buf);
		}
	}

	return NET_DROP;
}

static struct net_icmpv6_handler echo_request_handler = {
	.type = NET_ICMPV6_ECHO_REQUEST,
	.code = 0,
	.handler = handle_echo_request,
};

void net_icmpv6_init(void)
{
	static bool is_initialized;

	if (is_initialized) {
		return;
	}

	net_icmpv6_register_handler(&echo_request_handler);

	is_initialized = true;
}
