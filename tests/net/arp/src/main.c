/* main.c - Application main entry point */

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

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <device.h>
#include <init.h>
#include <misc/printk.h>
#include <net/net_core.h>
#include <net/nbuf.h>
#include <net/net_ip.h>
#include <net/arp.h>

#define NET_DEBUG 1
#include "net_private.h"

/* Sample ARP request (60 bytes) */
static const unsigned char pkt1[60] = {
0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x98, 0x4f, /* .......O */
0xee, 0x05, 0x4e, 0x5d, 0x08, 0x06, 0x00, 0x01, /* ..N].... */
0x08, 0x00, 0x06, 0x04, 0x00, 0x01, 0x98, 0x4f, /* .......O */
0xee, 0x05, 0x4e, 0x5d, 0xc0, 0xa8, 0x00, 0x02, /* ..N].... */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, /* ........ */
0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* ........ */
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* ........ */
0x00, 0x00, 0x00, 0x00                          /* .... */
};

static char *app_data = "0123456789";

static const struct net_eth_addr broadcast_eth_addr = {
	{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
static const struct net_eth_addr multicast_eth_addr = {
	{ 0x01, 0x00, 0x5e, 0x01, 0x02, 0x03 } };

struct net_arp_context {
	uint8_t mac_addr[sizeof(struct net_eth_addr)];
	struct net_linkaddr ll_addr;
};

int net_arp_dev_init(struct device *dev)
{
	struct net_arp_context *net_arp_context = dev->driver_data;

	dev->driver_api = NULL;
	net_arp_context = net_arp_context;

	return 0;
}

static uint8_t *net_arp_get_mac(struct device *dev)
{
	struct net_arp_context *context = dev->driver_data;

	if (context->mac_addr[0] == 0x00) {
		/* 10-00-00-00-00 to 10-00-00-00-FF Documentation RFC7042 */
		context->mac_addr[0] = 0x10;
		context->mac_addr[1] = 0x00;
		context->mac_addr[2] = 0x00;
		context->mac_addr[3] = 0x00;
		context->mac_addr[4] = 0x00;
		context->mac_addr[5] = sys_rand32_get();
	}

	return context->mac_addr;
}

static void net_arp_iface_init(struct net_if *iface)
{
	uint8_t *mac = net_arp_get_mac(net_if_get_device(iface));

	net_if_set_link_addr(iface, mac, 8);
}

static uint32_t net_arp_iface_cap(struct net_if *iface)
{
	return NET_CAP_ARP;
}

static struct net_buf *pending_buf;

static struct net_eth_addr hwaddr = { { 0x42, 0x11, 0x69, 0xde, 0xfa, 0xec } };

static int send_status = -EINVAL;

#define NET_ARP_BUF(buf) ((struct net_arp_hdr *)net_nbuf_ip_data(buf))

static int tester_send(struct net_if *iface, struct net_buf *buf)
{
	struct net_eth_hdr *hdr;

	if (!buf->frags) {
		printk("No data to send!");
		return -ENODATA;
	}

	if (net_nbuf_ll_reserve(buf) != sizeof(struct net_eth_hdr)) {
		printk("No ethernet header in buf %p", buf);

		send_status = -EINVAL;
		return send_status;
	}

	hdr = (struct net_eth_hdr *)(net_nbuf_ip_data(buf) -
				     net_nbuf_ll_reserve(buf));

	if (ntohs(hdr->type) == NET_ETH_PTYPE_ARP) {
		struct net_arp_hdr *arp_hdr =
			(struct net_arp_hdr *)(net_nbuf_ip_data(buf) -
					       net_nbuf_ll_reserve(buf));
		if (ntohs(arp_hdr->opcode) == NET_ARP_REPLY) {
			if (buf != pending_buf) {
				printk("Pending data but to be sent is wrong, "
				       "expecting %p but got %p\n",
				       pending_buf, buf);
				return -EINVAL;
			}

			if (memcmp(&hdr->dst, &hwaddr,
				   sizeof(struct net_eth_addr))) {
				char out[sizeof("xx:xx:xx:xx:xx:xx")];
				snprintf(out, sizeof(out),
					 net_sprint_ll_addr(
						 (uint8_t *)&hdr->dst,
						 sizeof(struct net_eth_addr)));
				printk("Invalid hwaddr %s, should be %s\n",
				       out,
				       net_sprint_ll_addr(
					       (uint8_t *)&hwaddr,
					       sizeof(struct net_eth_addr)));
				send_status = -EINVAL;
				return send_status;
			}

		} else if (ntohs(arp_hdr->opcode) == NET_ARP_REQUEST) {
			if (memcmp(&hdr->src, &hwaddr,
				   sizeof(struct net_eth_addr))) {
				char out[sizeof("xx:xx:xx:xx:xx:xx")];
				snprintf(out, sizeof(out),
					 net_sprint_ll_addr(
						 (uint8_t *)&hdr->src,
						 sizeof(struct net_eth_addr)));
				printk("Invalid hwaddr %s, should be %s\n",
				       out,
				       net_sprint_ll_addr(
					       (uint8_t *)&hwaddr,
					       sizeof(struct net_eth_addr)));
				send_status = -EINVAL;
				return send_status;
			}
		}
	}

	printk("Data was sent successfully\n");

	net_nbuf_unref(buf);

	send_status = 0;

	return 0;
}

static inline struct in_addr *if_get_addr(struct net_if *iface)
{
	int i;

	for (i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (iface->ipv4.unicast[i].is_used &&
		    iface->ipv4.unicast[i].address.family == AF_INET &&
		    iface->ipv4.unicast[i].addr_state == NET_ADDR_PREFERRED) {
			return &iface->ipv4.unicast[i].address.in_addr;
		}
	}

	return NULL;
}

static inline struct net_buf *prepare_arp_reply(struct net_if *iface,
						struct net_buf *req,
						struct net_eth_addr *addr)
{
	struct net_buf *buf, *frag;
	struct net_arp_hdr *hdr;

	buf = net_nbuf_get_reserve_tx(0);
	if (!buf) {
		goto fail;
	}

	frag = net_nbuf_get_reserve_data(sizeof(struct net_eth_hdr));
	if (!frag) {
		goto fail;
	}

	net_buf_frag_add(buf, frag);
	net_nbuf_iface(buf) = iface;
	net_nbuf_ll_reserve(buf) = net_buf_headroom(frag);

	hdr = (struct net_arp_hdr *)(net_nbuf_ip_data(buf) -
				     net_buf_headroom(frag));

	hdr->eth_hdr.type = htons(NET_ETH_PTYPE_ARP);

	memset(&hdr->eth_hdr.dst.addr, 0xff, sizeof(struct net_eth_addr));
	memcpy(&hdr->eth_hdr.src.addr, net_if_get_link_addr(iface)->addr,
	       sizeof(struct net_eth_addr));

	hdr->hwtype = htons(NET_ARP_HTYPE_ETH);
	hdr->protocol = htons(NET_ETH_PTYPE_IP);
	hdr->hwlen = sizeof(struct net_eth_addr);
	hdr->protolen = sizeof(struct in_addr);
	hdr->opcode = htons(NET_ARP_REPLY);

	memcpy(&hdr->dst_hwaddr.addr, &hdr->eth_hdr.src.addr,
	       sizeof(struct net_eth_addr));
	memcpy(&hdr->src_hwaddr.addr, addr,
	       sizeof(struct net_eth_addr));

	net_ipaddr_copy(&hdr->dst_ipaddr, &NET_ARP_BUF(req)->src_ipaddr);
	net_ipaddr_copy(&hdr->src_ipaddr, &NET_ARP_BUF(req)->dst_ipaddr);

	net_buf_add(frag, sizeof(struct net_arp_hdr) - net_buf_headroom(frag));

	return buf;

fail:
	net_nbuf_unref(buf);
	return NULL;
}

static inline struct net_buf *prepare_arp_request(struct net_if *iface,
						  struct net_buf *req,
						  struct net_eth_addr *addr)
{
	struct net_buf *buf, *frag;
	struct net_arp_hdr *hdr, *req_hdr;

	buf = net_nbuf_get_reserve_rx(0);
	if (!buf) {
		goto fail;
	}

	frag = net_nbuf_get_reserve_data(sizeof(struct net_eth_hdr));
	if (!frag) {
		goto fail;
	}

	net_buf_frag_add(buf, frag);
	net_nbuf_iface(buf) = iface;
	net_nbuf_ll_reserve(buf) = sizeof(struct net_eth_hdr);

	hdr = (struct net_arp_hdr *)(net_nbuf_ip_data(buf) -
				     net_buf_headroom(frag));
	req_hdr = (struct net_arp_hdr *)(net_nbuf_ip_data(req) -
					 net_nbuf_ll_reserve(req));

	hdr->eth_hdr.type = htons(NET_ETH_PTYPE_ARP);

	memset(&hdr->eth_hdr.dst.addr, 0xff, sizeof(struct net_eth_addr));
	memcpy(&hdr->eth_hdr.src.addr, addr, sizeof(struct net_eth_addr));

	hdr->hwtype = htons(NET_ARP_HTYPE_ETH);
	hdr->protocol = htons(NET_ETH_PTYPE_IP);
	hdr->hwlen = sizeof(struct net_eth_addr);
	hdr->protolen = sizeof(struct in_addr);
	hdr->opcode = htons(NET_ARP_REQUEST);

	memset(&hdr->dst_hwaddr.addr, 0x00, sizeof(struct net_eth_addr));
	memcpy(&hdr->src_hwaddr.addr, addr, sizeof(struct net_eth_addr));

	net_ipaddr_copy(&hdr->src_ipaddr, &req_hdr->src_ipaddr);
	net_ipaddr_copy(&hdr->dst_ipaddr, &req_hdr->dst_ipaddr);

	net_buf_add(frag, sizeof(struct net_arp_hdr) - net_buf_headroom(frag));

	return buf;

fail:
	net_nbuf_unref(buf);
	return NULL;
}

static void setup_eth_header(struct net_if *iface, struct net_buf *buf,
			     struct net_eth_addr *hwaddr, uint16_t type)
{
	struct net_eth_hdr *hdr = (struct net_eth_hdr *)
		(net_nbuf_ip_data(buf) - net_nbuf_ll_reserve(buf));

	memcpy(&hdr->dst.addr, hwaddr, sizeof(struct net_eth_addr));
	memcpy(&hdr->src.addr, net_if_get_link_addr(iface)->addr,
	       sizeof(struct net_eth_addr));

	hdr->type = htons(type);
}

struct net_arp_context net_arp_context_data;

static struct net_if_api net_arp_if_api = {
	.init = net_arp_iface_init,
	.capabilities = net_arp_iface_cap,
	.send = tester_send,
};

NET_DEVICE_INIT(net_arp_test, "net_arp_test",
		net_arp_dev_init, &net_arp_context_data, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
		&net_arp_if_api, 127);

#ifdef CONFIG_MICROKERNEL
void mainloop(void)
#else
void main(void)
#endif
{
	struct net_buf *buf, *buf2;
	struct net_buf *frag;
	struct net_if *iface;
	struct net_if_addr *ifaddr;
	struct net_arp_hdr *arp_hdr;
	struct net_ipv4_hdr *ipv4;
	struct net_eth_hdr *eth_hdr;
	int len;

	struct in_addr dst = { { { 192, 168, 0, 2 } } };
	struct in_addr dst_far = { { { 10, 11, 12, 13 } } };
	struct in_addr dst_far2 = { { { 172, 16, 14, 186 } } };
	struct in_addr src = { { { 192, 168, 0, 1 } } };
	struct in_addr mcast = { { { 224, 1, 2, 3 } } };
	struct in_addr netmask = { { { 255, 255, 255, 0 } } };
	struct in_addr gw = { { { 192, 168, 0, 42 } } };

	net_arp_init();

	iface = net_if_get_default();

	net_if_set_gw(iface, &gw);
	net_if_set_netmask(iface, &netmask);

	/* Broadcast and multicast tests */
	buf = net_nbuf_get_reserve_tx(0);
	if (!buf) {
		printk("Out of mem TX xcast\n");
		return;
	}

	frag = net_nbuf_get_reserve_data(sizeof(struct net_eth_hdr));
	if (!frag) {
		printk("Out of mem DATA xcast\n");
		return;
	}

	net_buf_frag_add(buf, frag);

	net_nbuf_ll_reserve(buf) = net_buf_headroom(frag);
	net_nbuf_iface(buf) = iface;

	ipv4 = (struct net_ipv4_hdr *)net_buf_add(frag,
						  sizeof(struct net_ipv4_hdr));
	net_ipaddr_copy(&ipv4->src, &src);
	net_ipaddr_copy(&ipv4->dst, net_ipv4_broadcast_address());

	buf2 = net_arp_prepare(buf);

	if (buf2 != buf) {
		printk("ARP broadcast buffer different\n");
		return;
	}

	eth_hdr = (struct net_eth_hdr *)(net_nbuf_ip_data(buf) -
					 net_nbuf_ll_reserve(buf));

	if (memcmp(&eth_hdr->dst.addr, &broadcast_eth_addr,
		   sizeof(struct net_eth_addr))) {
		char out[sizeof("xx:xx:xx:xx:xx:xx")];
		snprintf(out, sizeof(out),
			 net_sprint_ll_addr((uint8_t *)&eth_hdr->dst.addr,
					    sizeof(struct net_eth_addr)));
		printk("ETH addr dest invalid %s, should be %s", out,
		       net_sprint_ll_addr((uint8_t *)&broadcast_eth_addr,
					  sizeof(broadcast_eth_addr)));
		return;
	}

	net_ipaddr_copy(&ipv4->dst, &mcast);

	buf2 = net_arp_prepare(buf);

	if (buf2 != buf) {
		printk("ARP multicast buffer different\n");
		return;
	}

	if (memcmp(&eth_hdr->dst.addr, &multicast_eth_addr,
		   sizeof(struct net_eth_addr))) {
		char out[sizeof("xx:xx:xx:xx:xx:xx")];
		snprintf(out, sizeof(out),
			 net_sprint_ll_addr((uint8_t *)&eth_hdr->dst.addr,
					    sizeof(struct net_eth_addr)));
		printk("ETH maddr dest invalid %s, should be %s", out,
		       net_sprint_ll_addr((uint8_t *)&multicast_eth_addr,
					  sizeof(broadcast_eth_addr)));
		return;
	}

	net_nbuf_unref(buf);

	/* Then the unicast test */
	ifaddr = net_if_ipv4_addr_add(iface,
				      &src,
				      NET_ADDR_MANUAL,
				      0);
	ifaddr->addr_state = NET_ADDR_PREFERRED;

	/* Application data for testing */
	buf = net_nbuf_get_reserve_tx(0);
	if (!buf) {
		printk("Out of mem TX\n");
		return;
	}

	frag = net_nbuf_get_reserve_data(sizeof(struct net_eth_hdr));
	if (!frag) {
		printk("Out of mem DATA\n");
		return;
	}

	net_buf_frag_add(buf, frag);

	net_nbuf_ll_reserve(buf) = net_buf_headroom(frag);
	net_nbuf_iface(buf) = iface;

	setup_eth_header(iface, buf, &hwaddr, NET_ETH_PTYPE_IP);

	len = strlen(app_data);

	if (net_nbuf_ll_reserve(buf) != sizeof(struct net_eth_hdr)) {
		printk("LL reserve invalid, should be %d was %d\n",
		       sizeof(struct net_eth_hdr),
		       net_nbuf_ll_reserve(buf));
		return;
	}

	ipv4 = (struct net_ipv4_hdr *)net_buf_add(frag,
						  sizeof(struct net_ipv4_hdr));
	net_ipaddr_copy(&ipv4->src, &src);
	net_ipaddr_copy(&ipv4->dst, &dst);

	memcpy(net_buf_add(frag, len), app_data, len);

	buf2 = net_arp_prepare(buf);

	/* buf2 is the ARP buffer and buf is the IPv4 buffer and it was
	 * stored in ARP table.
	 */
	if (buf2 == buf) {
		/* The buffers cannot be the same as the ARP cache has
		 * still room for the buf.
		 */
		printk("ARP cache should still have free space\n");
		return;
	}

	if (!buf2) {
		printk("ARP buf is empty\n");
		return;
	}

	/* The ARP cache should now have a link to pending net_buf
	 * that is to be sent after we have got an ARP reply.
	 */
	if (!buf->frags) {
		printk("Pending buf fragment is NULL\n");
		return;
	}
	pending_buf = buf;

	/* buf2 should contain the arp header, verify it */
	if (memcmp(net_nbuf_ip_data(buf2), &broadcast_eth_addr,
		   sizeof(struct net_eth_addr))) {
		printk("ARP ETH dest address invalid\n");
		net_hexdump("ETH dest correct", net_nbuf_ip_data(buf2),
			    sizeof(struct net_eth_addr));
		net_hexdump("ETH dest wrong  ", (uint8_t *)&broadcast_eth_addr,
			    sizeof(struct net_eth_addr));
		return;
	}

	if (memcmp(net_nbuf_ip_data(buf2) + sizeof(struct net_eth_addr),
		   iface->link_addr.addr,
		   sizeof(struct net_eth_addr))) {
		printk("ARP ETH source address invalid\n");
		net_hexdump("ETH src correct",
			    iface->link_addr.addr,
			    sizeof(struct net_eth_addr));
		net_hexdump("ETH src wrong  ",
			    net_nbuf_ip_data(buf2) +
						sizeof(struct net_eth_addr),
			    sizeof(struct net_eth_addr));
		return;
	}

	arp_hdr = (struct net_arp_hdr *)net_nbuf_ip_data(buf2);

	if (arp_hdr->eth_hdr.type != htons(NET_ETH_PTYPE_ARP)) {
		printk("ETH type 0x%x, should be 0x%x\n",
		       arp_hdr->eth_hdr.type, htons(NET_ETH_PTYPE_ARP));
		return;
	}

	if (arp_hdr->hwtype != htons(NET_ARP_HTYPE_ETH)) {
		printk("ARP hwtype 0x%x, should be 0x%x\n",
		       arp_hdr->hwtype, htons(NET_ARP_HTYPE_ETH));
		return;
	}

	if (arp_hdr->protocol != htons(NET_ETH_PTYPE_IP)) {
		printk("ARP protocol 0x%x, should be 0x%x\n",
		       arp_hdr->protocol, htons(NET_ETH_PTYPE_IP));
		return;
	}

	if (arp_hdr->hwlen != sizeof(struct net_eth_addr)) {
		printk("ARP hwlen 0x%x, should be 0x%x\n",
		       arp_hdr->hwlen, sizeof(struct net_eth_addr));
		return;
	}

	if (arp_hdr->protolen != sizeof(struct in_addr)) {
		printk("ARP IP addr len 0x%x, should be 0x%x\n",
		       arp_hdr->protolen, sizeof(struct in_addr));
		return;
	}

	if (arp_hdr->opcode != htons(NET_ARP_REQUEST)) {
		printk("ARP opcode 0x%x, should be 0x%x\n",
		       arp_hdr->opcode, htons(NET_ARP_REQUEST));
		return;
	}

	if (!net_ipv4_addr_cmp(&arp_hdr->dst_ipaddr,
			       &NET_IPV4_BUF(buf)->dst)) {
		char out[sizeof("xxx.xxx.xxx.xxx")];
		snprintf(out, sizeof(out),
			 net_sprint_ipv4_addr(&arp_hdr->dst_ipaddr));
		printk("ARP IP dest invalid %s, should be %s", out,
		       net_sprint_ipv4_addr(&NET_IPV4_BUF(buf)->dst));
		return;
	}

	if (!net_ipv4_addr_cmp(&arp_hdr->src_ipaddr,
			       &NET_IPV4_BUF(buf)->src)) {
		char out[sizeof("xxx.xxx.xxx.xxx")];
		snprintf(out, sizeof(out),
			 net_sprint_ipv4_addr(&arp_hdr->src_ipaddr));
		printk("ARP IP src invalid %s, should be %s", out,
		       net_sprint_ipv4_addr(&NET_IPV4_BUF(buf)->src));
		return;
	}

	if (buf->ref != 2) {
		printk("ARP cache should own the original buffer\n");
		return;
	}

	/* Then a case where target is not in the same subnet */
	net_ipaddr_copy(&ipv4->dst, &dst_far);

	buf2 = net_arp_prepare(buf);

	if (buf2 == buf) {
		printk("ARP cache should not find anything\n");
		return;
	}

	if (!buf2) {
		printk("ARP buf2 is empty\n");
		return;
	}

	arp_hdr = (struct net_arp_hdr *)net_nbuf_ip_data(buf2);

	if (!net_ipv4_addr_cmp(&arp_hdr->dst_ipaddr, &iface->ipv4.gw)) {
		char out[sizeof("xxx.xxx.xxx.xxx")];
		snprintf(out, sizeof(out),
			 net_sprint_ipv4_addr(&arp_hdr->dst_ipaddr));
		printk("ARP IP dst invalid %s, should be %s\n", out,
			 net_sprint_ipv4_addr(&iface->ipv4.gw));
		return;
	}

	net_nbuf_unref(buf2);

	/* Try to find the same destination again, this should fail as there
	 * is a pending request in ARP cache.
	 */
	net_ipaddr_copy(&ipv4->dst, &dst_far);

	/* Make sure prepare will not free the buf because it will be
	 * needed in the later test case.
	 */
	net_buf_ref(buf);

	buf2 = net_arp_prepare(buf);
	if (buf2) {
		printk("ARP cache should fail now\n");
		return;
	}

	/* Try to find the different destination, this should fail too
	 * as the cache table should be full.
	 */
	net_ipaddr_copy(&ipv4->dst, &dst_far2);

	/* Make sure prepare will not free the buf because it will be
	 * needed in the next test case.
	 */
	net_buf_ref(buf);

	buf2 = net_arp_prepare(buf);
	if (buf2) {
		printk("ARP cache should fail again\n");
		return;
	}

	/* Restore the original address so that following test case can
	 * work properly.
	 */
	net_ipaddr_copy(&ipv4->dst, &dst);

	/* The arp request packet is now verified, create an arp reply.
	 * The previous value of buf is stored in arp table and is not lost.
	 */
	buf = net_nbuf_get_reserve_rx(0);
	if (!buf) {
		printk("Out of mem RX reply\n");
		return;
	}

	frag = net_nbuf_get_reserve_data(sizeof(struct net_eth_hdr));
	if (!frag) {
		printk("Out of mem DATA reply\n");
		return;
	}

	net_buf_frag_add(buf, frag);

	net_nbuf_ll_reserve(buf) = net_buf_headroom(frag);
	net_nbuf_iface(buf) = iface;

	arp_hdr = (struct net_arp_hdr *)net_nbuf_ip_data(buf);
	net_buf_add(buf, sizeof(struct net_arp_hdr));

	net_ipaddr_copy(&arp_hdr->dst_ipaddr, &dst);
	net_ipaddr_copy(&arp_hdr->src_ipaddr, &src);

	buf2 = prepare_arp_reply(iface, buf, &hwaddr);
	if (!buf2) {
		printk("ARP reply generation failed.");
		return;
	}

	switch (net_arp_input(buf2)) {
	case NET_OK:
		break;
	case NET_DROP:
		break;
	}

	/* Yielding so that network interface TX fiber can proceed. */
	fiber_yield();

	if (send_status < 0) {
		printk("ARP reply was not sent\n");
		return;
	}

	if (buf->ref != 1) {
		printk("ARP cache should no longer own the original buffer\n");
		return;
	}

	net_nbuf_unref(buf);

	/* Then feed in ARP request */
	buf = net_nbuf_get_reserve_rx(0);
	if (!buf) {
		printk("Out of mem RX request\n");
		return;
	}

	frag = net_nbuf_get_reserve_data(sizeof(struct net_eth_hdr));
	if (!frag) {
		printk("Out of mem DATA request\n");
		return;
	}

	net_buf_frag_add(buf, frag);

	net_nbuf_ll_reserve(buf) = sizeof(struct net_eth_hdr);
	net_nbuf_iface(buf) = iface;
	send_status = -EINVAL;

	arp_hdr = (struct net_arp_hdr *)(net_nbuf_ip_data(buf) -
					 net_nbuf_ll_reserve(buf));
	net_buf_add(frag,
		    sizeof(struct net_arp_hdr) - net_nbuf_ll_reserve(buf));

	net_ipaddr_copy(&arp_hdr->dst_ipaddr, &src);
	net_ipaddr_copy(&arp_hdr->src_ipaddr, &dst);
	setup_eth_header(iface, buf, &hwaddr, NET_ETH_PTYPE_ARP);

	buf2 = prepare_arp_request(iface, buf, &hwaddr);
	if (!buf2) {
		printk("ARP request generation failed.");
		return;
	}

	switch (net_arp_input(buf2)) {
	case NET_OK:
		break;
	case NET_DROP:
		break;
	}

	net_nbuf_unref(buf2);

	/* Yielding so that network interface TX fiber can proceed. */
	fiber_yield();

	if (send_status < 0) {
		printk("ARP req was not sent\n");
		return;
	}

	net_nbuf_unref(buf);

	printk("Network ARP checks passed\n");
}
