/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * vnetif - Virtual Network Interface Driver
 *
 * Internal shared header: types, constants, and cross-file declarations.
 *
 * Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
 */

#ifndef _VNETIF_H
#define _VNETIF_H

#include <linux/netdevice.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/types.h>

/* -------------------------------------------------------------------------
 * Driver identity
 * ---------------------------------------------------------------------- */

#define VNETIF_DRV_NAME     "vnetif"
#define VNETIF_DRV_VERSION  "1.0"
#define VNETIF_IF_FMT       "vnet%d"

/* -------------------------------------------------------------------------
 * Buffer and device limits
 * ---------------------------------------------------------------------- */

#define VNETIF_CMD_BUF_MAX    64
#define VNETIF_STATUS_BUF_MAX 4096
#define VNETIF_CIDR_SUFFIX    "/24"
#define VNETIF_MAX_MTU        65535

/* -------------------------------------------------------------------------
 * Data structures
 * ---------------------------------------------------------------------- */

/**
 * struct vnetif_arp_ipv4 - ARP payload layout for Ethernet/IPv4 (RFC 826)
 */
struct vnetif_arp_ipv4 {
	unsigned char sha[ETH_ALEN]; /* sender hardware address */
	__be32        sip;           /* sender IP address       */
	unsigned char tha[ETH_ALEN]; /* target hardware address */
	__be32        tip;           /* target IP address       */
} __packed;

/**
 * struct vnetif_priv - per-device private data (embedded in net_device)
 * @id:   sequential index assigned at creation time
 * @ipv4: configured IPv4 address in network byte order; 0 if unset
 */
struct vnetif_priv {
	int    id;
	__be32 ipv4;
};

/**
 * struct vnetif_entry - node in the global device list
 * @netdev: the associated network device
 * @node:   list linkage
 */
struct vnetif_entry {
	struct net_device *netdev;
	struct list_head   node;
};

/* -------------------------------------------------------------------------
 * Global state  (defined in vnetif_core.c)
 * ---------------------------------------------------------------------- */

extern struct list_head vnetif_devlist;
extern struct mutex     vnetif_devlock;
extern int              vnetif_next_id;

/* -------------------------------------------------------------------------
 * vnetif_core.c — device and address management
 * ---------------------------------------------------------------------- */

struct vnetif_entry *vnetif_dev_find(const char *name);
int  vnetif_dev_create(void);
int  vnetif_dev_destroy(const char *name);
void vnetif_dev_destroy_all(void);
int  vnetif_dev_bring_up(struct net_device *dev);
int  vnetif_dev_set_mtu(const char *ifname, int mtu);
int  vnetif_addr_validate(const char *ifname, const char *ip_str,
			   char *ifname_out, size_t ifname_out_len);
int  vnetif_addr_set(const char *ifname, const char *ip_str);
int  vnetif_addr_apply(const char *ifname, const char *ip_str);

/* -------------------------------------------------------------------------
 * vnetif_netdev.c — net_device setup
 * ---------------------------------------------------------------------- */

void vnetif_netdev_setup(struct net_device *dev);

/* -------------------------------------------------------------------------
 * vnetif_proc.c — procfs interface
 * ---------------------------------------------------------------------- */

int  vnetif_proc_init(void);
void vnetif_proc_exit(void);

#endif /* _VNETIF_H */