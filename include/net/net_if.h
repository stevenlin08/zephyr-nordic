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

/**
 * @file
 * @brief Public API for network interface
 */

#ifndef __NET_IF_H__
#define __NET_IF_H__

#include <device.h>

#include <net/net_linkaddr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Network Interface structure
 *
 * Used to handle a network interface on top of a device driver instance.
 * There can be many net_if instance against the same device.
 *
 * Such interface is mainly to be used by the link layer, but is also tight
 * to a network context: it then makes the relation with a network context
 * and the network device.
 *
 * Because of the strong relationship between a device driver and such
 * network interface, each net_if should be instanciated by
 */
struct net_if {
	/** The actualy device driver instance the net_if is related to */
	struct device *dev;

	/** The harware link address */
	struct net_linkaddr link_addr;

	/** The hardware MTU */
	uint16_t mtu;
};

/**
 * @brief Get an network interface's device
 * @param iface Pointer to a network interface structure
 * @return a pointer on the device driver instance
 */
static inline struct device *net_if_get_device(struct net_if *iface)
{
	return iface->dev;
}

/**
 * @brief Get an network interface's link address
 * @param iface Pointer to a network interface structure
 * @return a pointer on the network link address
 */
static inline struct net_linkaddr *net_if_get_link_addr(struct net_if *iface)
{
	return &iface->link_addr;
}

/**
 * @brief Set a network interfac's link address
 * @param iface Pointer to a network interface structure
 * @param addr a pointer on a uint8_t buffer representing the address
 * @param len length of the address buffer
 */
static inline void net_if_set_link_addr(struct net_if *iface,
					uint8_t *addr, uint8_t len)
{
	iface->link_addr.addr = addr;
	iface->link_addr.len = len;
}

/**
 * @brief Get an network interface's MTU
 * @param iface Pointer to a network interface structure
 * @return the MTU
 */
static inline uint16_t net_if_get_mtu(struct net_if *iface)
{
	return iface->mtu;
}

struct net_if_api {
	void (*init)(struct net_if *iface);
};

#ifdef CONFIG_NET_YAIP

#define NET_IF_INIT(dev_name, sfx, _mtu)				\
	static struct net_if (__net_if_ ##dev_name _##sfx) __used	\
	__attribute__((__section__(".net_if.data"))) = {		\
		.dev = &(__device_##dev_name),				\
		.mtu = _mtu,						\
	}

#else

#define NET_IF_INIT(...)

#endif /* CONFIG_NET_YAIP */

/* Network device intialization macro */

#define NET_DEVICE_INIT(dev_name, drv_name, init_fn,		\
			data, cfg_info, prio, api, mtu)		\
	DEVICE_AND_API_INIT(dev_name, drv_name, init_fn, data,	\
			    cfg_info, NANOKERNEL, prio, api);	\
	NET_IF_INIT(dev_name, 0, mtu)

#ifdef __cplusplus
}
#endif

#endif /* __NET_IFACE_H__ */
