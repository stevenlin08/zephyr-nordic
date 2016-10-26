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
#include <errno.h>
#include <misc/printk.h>

#include <net/nbuf.h>
#include <net/net_ip.h>

#define LL_RESERVE 28

struct ipv6_hdr {
	uint8_t vtc;
	uint8_t tcflow;
	uint16_t flow;
	uint8_t len[2];
	uint8_t nexthdr;
	uint8_t hop_limit;
	struct in6_addr src;
	struct in6_addr dst;
} __packed;

struct udp_hdr {
	uint16_t src_port;
	uint16_t dst_port;
	uint16_t len;
	uint16_t chksum;
} __packed;

struct icmp_hdr {
	uint8_t type;
	uint8_t code;
	uint16_t chksum;
} __packed;

static const char example_data[] =
	"0123456789abcdefghijklmnopqrstuvxyz!#¤%&/()=?"
	"0123456789abcdefghijklmnopqrstuvxyz!#¤%&/()=?"
	"0123456789abcdefghijklmnopqrstuvxyz!#¤%&/()=?"
	"0123456789abcdefghijklmnopqrstuvxyz!#¤%&/()=?"
	"0123456789abcdefghijklmnopqrstuvxyz!#¤%&/()=?"
	"0123456789abcdefghijklmnopqrstuvxyz!#¤%&/()=?";

static int test_ipv6_multi_frags(void)
{
	struct net_buf *buf, *frag;
	struct ipv6_hdr *ipv6;
	struct udp_hdr *udp;
	int bytes, remaining = strlen(example_data), pos = 0;

	/* Example of multi fragment scenario with IPv6 */
	buf = net_nbuf_get_reserve_rx(0);
	frag = net_nbuf_get_reserve_data(LL_RESERVE);

	/* Place the IP + UDP header in the first fragment */
	if (!net_buf_tailroom(frag)) {
		ipv6 = (struct ipv6_hdr *)(frag->data);
		udp = (struct udp_hdr *)((void *)ipv6 + sizeof(*ipv6));
		if (net_buf_tailroom(frag) < sizeof(ipv6)) {
			printk("Not enough space for IPv6 header, "
			       "needed %d bytes, has %d bytes\n",
			       sizeof(ipv6), net_buf_tailroom(frag));
			return -EINVAL;
		}
		net_buf_add(frag, sizeof(ipv6));

		if (net_buf_tailroom(frag) < sizeof(udp)) {
			printk("Not enough space for UDP header, "
			       "needed %d bytes, has %d bytes\n",
			       sizeof(udp), net_buf_tailroom(frag));
			return -EINVAL;
		}

		net_nbuf_appdata(buf) = (void *)udp + sizeof(*udp);
		net_nbuf_appdatalen(buf) = 0;
	}

	net_buf_frag_add(buf, frag);

	/* Put some data to rest of the fragments */
	frag = net_nbuf_get_reserve_data(LL_RESERVE);
	if (net_buf_tailroom(frag) -
	      (CONFIG_NET_NBUF_DATA_SIZE - LL_RESERVE)) {
		printk("Invalid number of bytes available in the buf, "
		       "should be 0 but was %d - %d\n",
		       net_buf_tailroom(frag),
		       CONFIG_NET_NBUF_DATA_SIZE - LL_RESERVE);
		return -EINVAL;
	}

	if (((int)net_buf_tailroom(frag) - remaining) > 0) {
		printk("We should have been out of space now, "
		       "tailroom %d user data len %d\n",
		       net_buf_tailroom(frag),
		       strlen(example_data));
		return -EINVAL;
	}

	while (remaining > 0) {
		int copy;

		bytes = net_buf_tailroom(frag);
		copy = remaining > bytes ? bytes : remaining;
		memcpy(net_buf_add(frag, copy), &example_data[pos], copy);

		printk("Remaining %d left %d copy %d\n", remaining, bytes,
		       copy);

		pos += bytes;
		remaining -= bytes;
		if (net_buf_tailroom(frag) - (bytes - copy)) {
			printk("There should have not been any tailroom left, "
			       "tailroom %d\n",
			       net_buf_tailroom(frag) - (bytes - copy));
			return -EINVAL;
		}

		net_buf_frag_add(buf, frag);
		if (remaining > 0) {
			frag = net_nbuf_get_reserve_data(LL_RESERVE);
		}
	}

	bytes = net_buf_frags_len(buf->frags);
	if (bytes != strlen(example_data)) {
		printk("Invalid number of bytes in message, %d vs %d\n",
		       strlen(example_data), bytes);
		return -EINVAL;
	}

	/* Normally one should not unref the fragment list like this
	 * because it will leave the buf->frags pointing to already
	 * freed fragment.
	 */
	net_nbuf_unref(buf->frags);
	if (!buf->frags) {
		printk("Fragment list should not be empty.\n");
		return -EINVAL;
	}
	buf->frags = NULL; /* to prevent double free */

	net_nbuf_unref(buf);

	return 0;
}

static char buf_orig[200];
static char buf_copy[200];

static void linearize(struct net_buf *buf, char *buffer, int len)
{
	char *ptr = buffer;

	buf = buf->frags;

	while (buf && len > 0) {

		memcpy(ptr, buf->data, buf->len);
		ptr += buf->len;
		len -= buf->len;

		buf = buf->frags;
	}
}

static int test_fragment_copy(void)
{
	struct net_buf *buf, *frag, *new_buf, *new_frag;
	struct ipv6_hdr *ipv6;
	struct udp_hdr *udp;
	size_t orig_len;
	int pos;

	buf = net_nbuf_get_reserve_rx(0);
	frag = net_nbuf_get_reserve_data(LL_RESERVE);

	/* Place the IP + UDP header in the first fragment */
	if (net_buf_tailroom(frag)) {
		ipv6 = (struct ipv6_hdr *)(frag->data);
		udp = (struct udp_hdr *)((void *)ipv6 + sizeof(*ipv6));
		if (net_buf_tailroom(frag) < sizeof(*ipv6)) {
			printk("Not enough space for IPv6 header, "
			       "needed %d bytes, has %d bytes\n",
			       sizeof(ipv6), net_buf_tailroom(frag));
			return -EINVAL;
		}
		net_buf_add(frag, sizeof(*ipv6));

		if (net_buf_tailroom(frag) < sizeof(*udp)) {
			printk("Not enough space for UDP header, "
			       "needed %d bytes, has %d bytes\n",
			       sizeof(udp), net_buf_tailroom(frag));
			return -EINVAL;
		}

		net_buf_add(frag, sizeof(*udp));

		memcpy(net_buf_add(frag, 15), example_data, 15);

		net_nbuf_appdata(buf) = (void *)udp + sizeof(*udp) + 15;
		net_nbuf_appdatalen(buf) = 0;
	}

	net_buf_frag_add(buf, frag);

	orig_len = net_buf_frags_len(buf);

	printk("Total copy data len %d\n", orig_len);

	linearize(buf, buf_orig, sizeof(orig_len));

	/* Then copy a fragment list to a new fragment list */
	new_frag = net_nbuf_copy_all(buf->frags, sizeof(struct ipv6_hdr) +
				     sizeof(struct icmp_hdr));
	if (!new_frag) {
		printk("Cannot copy fragment list.\n");
		return -EINVAL;
	}

	new_buf = net_nbuf_get_reserve_tx(0);
	net_buf_frag_add(new_buf, new_frag);

	printk("Total new data len %d\n", net_buf_frags_len(new_buf));

	if (net_buf_frags_len(buf) != 0) {
		printk("Fragment list missing data, %d bytes not copied\n",
		       net_buf_frags_len(buf));
		return -EINVAL;
	}

	if (net_buf_frags_len(new_buf) != (orig_len + sizeof(struct ipv6_hdr) +
					   sizeof(struct icmp_hdr))) {
		printk("Fragment list missing data, new buf len %d "
		       "should be %d\n", net_buf_frags_len(new_buf),
		       orig_len + sizeof(struct ipv6_hdr) +
		       sizeof(struct icmp_hdr));
		return -EINVAL;
	}

	linearize(new_buf, buf_copy, sizeof(buf_copy));

	if (!memcmp(buf_orig, buf_copy, sizeof(buf_orig))) {
		printk("Buffer copy failed, buffers are same!\n");
		return -EINVAL;
	}

	pos = memcmp(buf_orig, buf_copy + sizeof(struct ipv6_hdr) +
		     sizeof(struct icmp_hdr), sizeof(buf_orig));
	if (pos) {
		printk("Buffer copy failed at pos %d\n", pos);
		return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_MICROKERNEL
void mainloop(void)
#else
void main(void)
#endif
{
	if (test_ipv6_multi_frags() < 0) {
		return;
	}

	if (test_fragment_copy() < 0) {
		return;
	}

	printk("nbuf tests passed\n");
}
