// SPDX-License-Identifier: GPL-2.0-only
/*
 * vnetif - Virtual Network Interface Driver
 *
 * Net device operations and in-kernel packet processing:
 *   - ARP reply for Ethernet/IPv4 ARP requests  (RFC 826)
 *   - ICMP echo reply for IPv4 ping              (RFC 792)
 *   - Bridge slave forwarding for remote peers
 *   - ethtool driver info and statistics
 *
 * Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <linux/if_arp.h>
#include <linux/in.h>
#include <net/ip.h>

#include "vnetif.h"

/* -------------------------------------------------------------------------
 * ethtool support
 * ---------------------------------------------------------------------- */

static const char vnetif_ethtool_stat_names[][ETH_GSTRING_LEN] = {
	"tx_packets",
	"tx_bytes",
	"rx_packets",
	"rx_bytes",
	"tx_dropped",
};

#define VNETIF_N_STATS ARRAY_SIZE(vnetif_ethtool_stat_names)

static void vnetif_ethtool_get_drvinfo(struct net_device *dev,
				       struct ethtool_drvinfo *info)
{
	strscpy(info->driver,   VNETIF_DRV_NAME,    sizeof(info->driver));
	strscpy(info->version,  VNETIF_DRV_VERSION, sizeof(info->version));
	strscpy(info->bus_info, "virtual",          sizeof(info->bus_info));
}

static int vnetif_ethtool_get_sset_count(struct net_device *dev, int sset)
{
	return (sset == ETH_SS_STATS) ? (int)VNETIF_N_STATS : -EOPNOTSUPP;
}

static void vnetif_ethtool_get_strings(struct net_device *dev,
				       u32 sset, u8 *data)
{
	if (sset == ETH_SS_STATS)
		memcpy(data, vnetif_ethtool_stat_names,
		       sizeof(vnetif_ethtool_stat_names));
}

static void vnetif_ethtool_get_stats(struct net_device *dev,
				     struct ethtool_stats *stats, u64 *data)
{
	data[0] = dev->stats.tx_packets;
	data[1] = dev->stats.tx_bytes;
	data[2] = dev->stats.rx_packets;
	data[3] = dev->stats.rx_bytes;
	data[4] = dev->stats.tx_dropped;
}

static const struct ethtool_ops vnetif_ethtool_ops = {
	.get_drvinfo       = vnetif_ethtool_get_drvinfo,
	.get_sset_count    = vnetif_ethtool_get_sset_count,
	.get_strings       = vnetif_ethtool_get_strings,
	.get_ethtool_stats = vnetif_ethtool_get_stats,
};

/* -------------------------------------------------------------------------
 * Packet classification
 * ---------------------------------------------------------------------- */

static bool vnetif_pkt_is_arp_request(struct sk_buff *skb,
				      const struct net_device *dev)
{
	const struct vnetif_priv     *priv = netdev_priv(dev);
	const struct ethhdr          *eth;
	const struct arphdr          *arp;
	const struct vnetif_arp_ipv4 *payload;
	size_t min_len = sizeof(struct ethhdr) + sizeof(struct arphdr) +
			 sizeof(struct vnetif_arp_ipv4);

	if (!priv->ipv4 || skb->len < min_len)
		return false;

	if (!pskb_may_pull(skb, min_len))
		return false;

	skb_reset_mac_header(skb);
	skb_set_network_header(skb, sizeof(struct ethhdr));

	eth = eth_hdr(skb);
	if (eth->h_proto != htons(ETH_P_ARP))
		return false;

	arp = (const struct arphdr *)skb_network_header(skb);
	if (arp->ar_hrd != htons(ARPHRD_ETHER) ||
	    arp->ar_pro != htons(ETH_P_IP)      ||
	    arp->ar_hln != ETH_ALEN             ||
	    arp->ar_pln != 4                    ||
	    arp->ar_op  != htons(ARPOP_REQUEST))
		return false;

	payload = (const struct vnetif_arp_ipv4 *)(arp + 1);
	return payload->tip == priv->ipv4;
}

static bool vnetif_pkt_is_icmp_echo(struct sk_buff *skb,
				    const struct net_device *dev)
{
	const struct vnetif_priv *priv = netdev_priv(dev);
	const struct ethhdr      *eth;
	const struct iphdr       *iph;
	const struct icmphdr     *icmph;
	size_t ip_hdr_len;

	if (!priv->ipv4)
		return false;

	if (!pskb_may_pull(skb, sizeof(struct ethhdr) + sizeof(struct iphdr)))
		return false;

	skb_reset_mac_header(skb);
	skb_set_network_header(skb, sizeof(struct ethhdr));

	eth = eth_hdr(skb);
	if (eth->h_proto != htons(ETH_P_IP))
		return false;

	iph = (const struct iphdr *)skb_network_header(skb);
	if (iph->version != 4 || iph->ihl < 5 || iph->protocol != IPPROTO_ICMP)
		return false;

	ip_hdr_len = (size_t)iph->ihl * 4;
	if (!pskb_may_pull(skb, sizeof(struct ethhdr) + ip_hdr_len +
				 sizeof(struct icmphdr)))
		return false;

	/* Re-fetch after pskb_may_pull may have reallocated the head. */
	iph   = (const struct iphdr *)skb_network_header(skb);
	icmph = (const struct icmphdr *)((const u8 *)iph + ip_hdr_len);

	return icmph->type == ICMP_ECHO && iph->daddr == priv->ipv4;
}

/* -------------------------------------------------------------------------
 * In-place reply builders
 * ---------------------------------------------------------------------- */

static int vnetif_pkt_build_arp_reply(struct sk_buff *skb,
				      const struct net_device *dev)
{
	const struct vnetif_priv *priv = netdev_priv(dev);
	struct ethhdr            *eth;
	struct arphdr            *arp;
	struct vnetif_arp_ipv4   *payload;
	unsigned char requester_mac[ETH_ALEN];
	__be32        requester_ip;

	if (skb->len < sizeof(struct ethhdr) + sizeof(struct arphdr) +
		       sizeof(struct vnetif_arp_ipv4))
		return -EINVAL;

	skb_reset_mac_header(skb);
	skb_set_network_header(skb, sizeof(struct ethhdr));

	eth     = eth_hdr(skb);
	arp     = (struct arphdr *)skb_network_header(skb);
	payload = (struct vnetif_arp_ipv4 *)(arp + 1);

	memcpy(requester_mac, payload->sha, ETH_ALEN);
	requester_ip = payload->sip;

	memcpy(eth->h_dest,   requester_mac, ETH_ALEN);
	memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);

	arp->ar_op = htons(ARPOP_REPLY);

	memcpy(payload->sha, dev->dev_addr, ETH_ALEN);
	payload->sip = priv->ipv4;
	memcpy(payload->tha, requester_mac,  ETH_ALEN);
	payload->tip = requester_ip;

	return 0;
}

static int vnetif_pkt_build_icmp_reply(struct sk_buff *skb,
				       const struct net_device *dev)
{
	const struct vnetif_priv *priv = netdev_priv(dev);
	struct ethhdr  *eth;
	struct iphdr   *iph;
	struct icmphdr *icmph;
	size_t ip_hdr_len, icmp_len;
	__be32 tmp;

	skb_reset_mac_header(skb);
	skb_set_network_header(skb, sizeof(struct ethhdr));

	eth        = eth_hdr(skb);
	iph        = (struct iphdr *)skb_network_header(skb);
	ip_hdr_len = (size_t)iph->ihl * 4;

	if (skb->len < sizeof(struct ethhdr) + ip_hdr_len +
		       sizeof(struct icmphdr))
		return -EINVAL;

	icmph = (struct icmphdr *)((u8 *)iph + ip_hdr_len);

	memcpy(eth->h_dest,   eth->h_source, ETH_ALEN);
	memcpy(eth->h_source, dev->dev_addr, ETH_ALEN);

	tmp        = iph->saddr;
	iph->saddr = priv->ipv4;
	iph->daddr = tmp;
	iph->ttl   = 64;
	iph->check  = 0;
	ip_send_check(iph);

	icmp_len        = ntohs(iph->tot_len) - ip_hdr_len;
	icmph->type     = ICMP_ECHOREPLY;
	icmph->code     = 0;
	icmph->checksum = 0;
	icmph->checksum = ip_compute_csum(icmph, icmp_len);

	return 0;
}

/* -------------------------------------------------------------------------
 * Reply delivery
 * ---------------------------------------------------------------------- */

static netdev_tx_t vnetif_pkt_deliver(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct ethhdr *eth = eth_hdr(skb);

	skb->dev       = dev;
	skb->ip_summed = CHECKSUM_NONE;
	skb->protocol  = eth_type_trans(skb, dev);

	/*
	 * Self-ping: mark PACKET_HOST so the local IP stack accepts the frame
	 * directly; the bridge must not attempt to forward a frame that loops
	 * back to the ingress port.
	 */
	if (ether_addr_equal(eth->h_dest, dev->dev_addr))
		skb->pkt_type = PACKET_HOST;

	netif_rx(skb);
	return NETDEV_TX_OK;
}

/* -------------------------------------------------------------------------
 * Net device operations
 * ---------------------------------------------------------------------- */

static int vnetif_ndo_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int vnetif_ndo_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static void vnetif_ndo_get_stats64(struct net_device *dev,
				   struct rtnl_link_stats64 *stats)
{
	netdev_stats_to_stats64(stats, &dev->stats);
}

static int vnetif_ndo_change_mtu(struct net_device *dev, int new_mtu)
{
	if (new_mtu < (int)ETH_MIN_MTU || new_mtu > VNETIF_MAX_MTU)
		return -EINVAL;
	WRITE_ONCE(dev->mtu, (unsigned int)new_mtu);
	pr_info("%s: MTU changed to %d\n", dev->name, new_mtu);
	return 0;
}

static int vnetif_ndo_set_mac_address(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;
	eth_hw_addr_set(dev, addr->sa_data);
	pr_info("%s: MAC address updated\n", dev->name);
	return 0;
}

static netdev_tx_t vnetif_ndo_xmit(struct sk_buff *skb,
				   struct net_device *dev)
{
	const struct ethhdr *eth;
	struct net_device   *master;
	int ret;

	if (unlikely(!skb)) {
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	skb_reset_mac_header(skb);
	skb_set_network_header(skb, sizeof(struct ethhdr));
	eth = eth_hdr(skb);

	/*
	 * Bridge slave path: forward non-self, non-multicast unicast frames
	 * to the bridge master for standard L2 forwarding.
	 */
	if (!ether_addr_equal(eth->h_dest, dev->dev_addr) &&
	    !is_multicast_ether_addr(eth->h_dest)) {
		rcu_read_lock();
		master = netdev_master_upper_dev_get_rcu(dev);
		if (master)
			dev_hold(master);
		rcu_read_unlock();

		if (master) {
			skb->dev       = master;
			skb->ip_summed = CHECKSUM_NONE;
			skb->protocol  = eth->h_proto;
			dev->stats.tx_packets++;
			dev->stats.tx_bytes += skb->len;
			ret = dev_queue_xmit(skb);
			if (unlikely(ret))
				pr_err("%s: bridge xmit error: %d\n",
				       dev->name, ret);
			dev_put(master);
			return NETDEV_TX_OK;
		}
	}

	if (vnetif_pkt_is_arp_request(skb, dev)) {
		ret = vnetif_pkt_build_arp_reply(skb, dev);
		if (unlikely(ret)) {
			dev_kfree_skb(skb);
			dev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
		dev->stats.tx_packets++;
		dev->stats.tx_bytes   += skb->len;
		dev->stats.rx_packets++;
		dev->stats.rx_bytes   += skb->len;
		return vnetif_pkt_deliver(skb, dev);
	}

	if (vnetif_pkt_is_icmp_echo(skb, dev)) {
		ret = vnetif_pkt_build_icmp_reply(skb, dev);
		if (unlikely(ret)) {
			dev_kfree_skb(skb);
			dev->stats.tx_dropped++;
			return NETDEV_TX_OK;
		}
		dev->stats.tx_packets++;
		dev->stats.tx_bytes   += skb->len;
		dev->stats.rx_packets++;
		dev->stats.rx_bytes   += skb->len;
		return vnetif_pkt_deliver(skb, dev);
	}

	dev_kfree_skb(skb);
	dev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static const struct net_device_ops vnetif_netdev_ops = {
	.ndo_open            = vnetif_ndo_open,
	.ndo_stop            = vnetif_ndo_stop,
	.ndo_start_xmit      = vnetif_ndo_xmit,
	.ndo_get_stats64     = vnetif_ndo_get_stats64,
	.ndo_change_mtu      = vnetif_ndo_change_mtu,
	.ndo_set_mac_address = vnetif_ndo_set_mac_address,
};

void vnetif_netdev_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops  = &vnetif_netdev_ops;
	dev->ethtool_ops = &vnetif_ethtool_ops;
	eth_hw_addr_random(dev);
	netif_carrier_on(dev);
}