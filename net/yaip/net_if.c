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

#include <init.h>
#include <nanokernel.h>
#include <sections.h>
#include <misc/sys_log.h>
#include <net/net_if.h>

/* net_if dedicated section limiters */
extern struct net_if __net_if_start[];
extern struct net_if __net_if_end[];

/* Stack for the TX fiber */
#ifndef CONFIG_NET_TX_STACK_SIZE
#define CONFIG_NET_TX_STACK_SIZE 1024
#endif
static char __noinit __stack tx_fiber_stack[CONFIG_NET_TX_STACK_SIZE];

static void net_if_tx_fiber(struct net_if *iface)
{
	SYS_LOG_DBG("Starting TX fiber (stack %d bytes)\n",
		    sizeof(tx_fiber_stack));

	while (1) {
		struct net_buf *buf;

		/* Get next packet from application - wait if necessary */
		buf = nano_fifo_get(&iface->tx_queue, TICKS_UNLIMITED);

		SYS_LOG_DBG("Processing (buf %p, len %u) network packet\n",
			    buf, buf->len);

		/* FIXME - Do something with the packet */
	}
}

static inline void init_tx_queue(struct net_if *iface)
{
	nano_fifo_init(&iface->tx_queue);

	fiber_start(tx_fiber_stack, sizeof(tx_fiber_stack),
		    (nano_fiber_entry_t)net_if_tx_fiber, (int)iface, 0, 7, 0);
}

static int net_if_init(struct device *unused)
{
	struct net_if_api *api;
	struct net_if *iface;

	ARG_UNUSED(unused);

	for (iface = __net_if_start; iface != __net_if_end; iface++) {
		api = (struct net_if_api *) iface->dev->driver_api;

		if (api && api->init) {
			api->init(iface);
			init_tx_queue(iface);
		}
	}

	return 0;
}

SYS_INIT(net_if_init, NANOKERNEL, CONFIG_NET_YAIP_INIT_PRIO);
