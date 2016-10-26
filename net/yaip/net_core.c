/** @file
 * @brief Network initialization
 *
 * Initialize the network IP stack. Create two fibers, one for reading data
 * from applications (Tx fiber) and one for reading data from IP stack
 * and passing that data to applications (Rx fiber).
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

#if defined(CONFIG_NETWORK_IP_STACK_DEBUG_CORE)
#define SYS_LOG_DOMAIN "net/core"
#define NET_DEBUG 1
#endif

#include <init.h>
#include <nanokernel.h>
#include <toolchain.h>
#include <sections.h>
#include <string.h>
#include <errno.h>

#include <net/net_if.h>
#include <net/nbuf.h>
#include <net/net_core.h>
#include <net/net_stats.h>

#include "net_private.h"

/* Stack for the rx fiber.
 */
#if !defined(CONFIG_NET_RX_STACK_SIZE)
#define CONFIG_NET_RX_STACK_SIZE 1024
#endif
static char __noinit __stack rx_fiber_stack[CONFIG_NET_RX_STACK_SIZE];
static struct nano_fifo rx_queue;
static nano_thread_id_t rx_fiber_id;

#if defined(CONFIG_NET_STATISTICS)
#define PRINT_STAT(fmt, ...) printk(fmt, ##__VA_ARGS__)
struct net_stats net_stats;
#define GET_STAT(s) net_stats.s
#define PRINT_STATISTICS_INTERVAL (30 * sys_clock_ticks_per_sec)
#define net_print_statistics stats /* to make the debug print line shorter */

static inline void stats(void)
{
	static uint32_t next_print;
	uint32_t curr = sys_tick_get_32();

	if (!next_print || (next_print < curr &&
	    (!((curr - next_print) > PRINT_STATISTICS_INTERVAL)))) {
		uint32_t new_print;

#if defined(CONFIG_NET_IPV6)
		PRINT_STAT("IPv6 recv      %d\tsent\t%d\tdrop\t%d\tforwarded\t%d\n",
			 GET_STAT(ipv6.recv),
			 GET_STAT(ipv6.sent),
			 GET_STAT(ipv6.drop),
			 GET_STAT(ipv6.forwarded));
#endif /* CONFIG_NET_IPV6 */

#if defined(CONFIG_NET_IPV4)
		PRINT_STAT("IPv4 recv      %d\tsent\t%d\tdrop\t%d\tforwarded\t%d\n",
			 GET_STAT(ipv4.recv),
			 GET_STAT(ipv4.sent),
			 GET_STAT(ipv4.drop),
			 GET_STAT(ipv4.forwarded));
#endif /* CONFIG_NET_IPV6 */

		PRINT_STAT("IP vhlerr      %d\thblener\t%d\tlblener\t%d\n",
			 GET_STAT(ip_errors.vhlerr),
			 GET_STAT(ip_errors.hblenerr),
			 GET_STAT(ip_errors.lblenerr));
		PRINT_STAT("IP fragerr     %d\tchkerr\t%d\tprotoer\t%d\n",
			 GET_STAT(ip_errors.fragerr),
			 GET_STAT(ip_errors.chkerr),
			 GET_STAT(ip_errors.protoerr));

		PRINT_STAT("ICMP recv      %d\tsent\t%d\tdrop\t%d\n",
			 GET_STAT(icmp.recv),
			 GET_STAT(icmp.sent),
			 GET_STAT(icmp.drop));
		PRINT_STAT("ICMP typeer    %d\tchkerr\t%d\n",
			 GET_STAT(icmp.typeerr),
			 GET_STAT(icmp.chkerr));

#if defined(CONFIG_NET_UDP)
		PRINT_STAT("UDP recv       %d\tsent\t%d\tdrop\t%d\n",
			 GET_STAT(udp.recv),
			 GET_STAT(udp.sent),
			 GET_STAT(udp.drop));
		PRINT_STAT("UDP chkerr     %d\n",
			 GET_STAT(icmp.chkerr));
#endif

		PRINT_STAT("Processing err %d\n", GET_STAT(processing_error));

		new_print = curr + PRINT_STATISTICS_INTERVAL;
		if (new_print > curr) {
			next_print = new_print;
		} else {
			/* Overflow */
			next_print = PRINT_STATISTICS_INTERVAL -
				(0xffffffff - curr);
		}
	}
}
#else /* CONFIG_NET_STATISTICS */
#define net_print_statistics()
#endif /* CONFIG_NET_STATISTICS */

static inline enum net_verdict process_data(struct net_buf *buf)
{
	return NET_DROP;
}

static void net_rx_fiber(int unused1, int unused2)
{
	struct net_buf *buf;

	NET_DBG("Starting RX fiber (stack %d bytes)",
		sizeof(rx_fiber_stack));

	while (1) {
		buf = nano_fifo_get(&rx_queue, TICKS_UNLIMITED);

		net_analyze_stack("RX fiber", rx_fiber_stack,
				  sizeof(rx_fiber_stack));

		NET_DBG("Received buf %p len %d", buf,
			net_buf_frags_len(buf));

		switch (process_data(buf)) {
		case NET_OK:
			NET_DBG("Consumed buf %p", buf);
			break;
		case NET_DROP:
		default:
			NET_DBG("Dropping buf %p", buf);
			net_nbuf_unref(buf);
			break;
		}

		net_print_statistics();
	}
}

static void init_rx_queue(void)
{
	nano_fifo_init(&rx_queue);

	rx_fiber_id = fiber_start(rx_fiber_stack, sizeof(rx_fiber_stack),
				  net_rx_fiber, 0, 0, 8, 0);
}

/* Called by driver when an IP packet has been received */
int net_recv(struct net_if *iface, struct net_buf *buf)
{
	if (!buf->frags) {
		return -ENODATA;
	}

	NET_DBG("fifo %p iface %p buf %p len %d", &rx_queue, iface, buf,
		net_buf_frags_len(buf));

	net_nbuf_iface(buf) = iface;

	nano_fifo_put(&rx_queue, buf);

	fiber_wakeup(rx_fiber_id);

	return 0;
}

static int network_initialization(void)
{

	return 0;
}

static int net_init(struct device *unused)
{
	static bool is_initialized;

	if (is_initialized)
		return -EALREADY;

	is_initialized = true;

	NET_DBG("Priority %d", CONFIG_NET_INIT_PRIO);

	net_nbuf_init();

	init_rx_queue();

	net_if_init();

	net_context_init();

	return network_initialization();
}

SYS_INIT(net_init, NANOKERNEL, CONFIG_NET_INIT_PRIO);
