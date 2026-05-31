// SPDX-License-Identifier: GPL-2.0-only
/*
 * vnetif - Virtual Network Interface Driver
 *
 * Core: module lifecycle, virtual device management, IPv4 address
 * configuration via usermode helper, and MTU management.
 *
 * Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <linux/rtnetlink.h>
#include <linux/kmod.h>

#include "vnetif.h"

/* -------------------------------------------------------------------------
 * Module parameters
 * ---------------------------------------------------------------------- */

static unsigned int max_interfaces = 16;
module_param(max_interfaces, uint, 0444);
MODULE_PARM_DESC(max_interfaces,
		 "Maximum number of virtual interfaces (default: 16)");

/* -------------------------------------------------------------------------
 * Global state
 * ---------------------------------------------------------------------- */

LIST_HEAD(vnetif_devlist);
DEFINE_MUTEX(vnetif_devlock);
int vnetif_next_id;

/* -------------------------------------------------------------------------
 * Device list helpers — caller must hold vnetif_devlock
 * ---------------------------------------------------------------------- */

struct vnetif_entry *vnetif_dev_find(const char *name)
{
	struct vnetif_entry *e;

	list_for_each_entry(e, &vnetif_devlist, node) {
		if (strcmp(e->netdev->name, name) == 0)
			return e;
	}
	return NULL;
}

int vnetif_dev_bring_up(struct net_device *dev)
{
	int ret;

	rtnl_lock();
	ret = dev_open(dev, NULL);
	rtnl_unlock();

	if (ret)
		pr_err("failed to bring %s up: %d\n", dev->name, ret);

	return ret;
}

int vnetif_dev_create(void)
{
	struct net_device   *netdev;
	struct vnetif_entry *entry;
	struct vnetif_priv  *priv;
	int ret;

	if ((unsigned int)vnetif_next_id >= max_interfaces) {
		pr_warn("interface limit (%u) reached\n", max_interfaces);
		return -ENOSPC;
	}

	netdev = alloc_netdev(sizeof(struct vnetif_priv), VNETIF_IF_FMT,
			      NET_NAME_UNKNOWN, vnetif_netdev_setup);
	if (!netdev)
		return -ENOMEM;

	priv       = netdev_priv(netdev);
	priv->id   = vnetif_next_id++;
	priv->ipv4 = 0;

	ret = register_netdev(netdev);
	if (ret) {
		free_netdev(netdev);
		return ret;
	}

	ret = vnetif_dev_bring_up(netdev);
	if (ret) {
		unregister_netdev(netdev);
		free_netdev(netdev);
		return ret;
	}

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		unregister_netdev(netdev);
		free_netdev(netdev);
		return -ENOMEM;
	}

	entry->netdev = netdev;
	list_add_tail(&entry->node, &vnetif_devlist);

	pr_info("created %s\n", netdev->name);
	return 0;
}

int vnetif_dev_destroy(const char *name)
{
	struct vnetif_entry *entry;

	entry = vnetif_dev_find(name);
	if (!entry)
		return -ENOENT;

	list_del(&entry->node);
	unregister_netdev(entry->netdev);
	free_netdev(entry->netdev);
	kfree(entry);

	pr_info("destroyed %s\n", name);
	return 0;
}

void vnetif_dev_destroy_all(void)
{
	struct vnetif_entry *entry, *tmp;

	list_for_each_entry_safe(entry, tmp, &vnetif_devlist, node) {
		list_del(&entry->node);
		unregister_netdev(entry->netdev);
		free_netdev(entry->netdev);
		kfree(entry);
	}
}

/*
 * vnetif_dev_set_mtu — change an interface MTU from procfs context.
 *
 * Must be called WITHOUT vnetif_devlock held: dev_set_mtu() requires
 * rtnl_lock(), and we must not nest rtnl_lock inside our mutex.
 * We hold the device reference across the lock boundary to prevent
 * concurrent destruction.
 */
int vnetif_dev_set_mtu(const char *ifname, int mtu)
{
	struct vnetif_entry *entry;
	struct net_device   *netdev = NULL;
	int ret;

	mutex_lock(&vnetif_devlock);
	entry = vnetif_dev_find(ifname);
	if (entry) {
		netdev = entry->netdev;
		dev_hold(netdev);
	}
	mutex_unlock(&vnetif_devlock);

	if (!netdev)
		return -ENOENT;

	rtnl_lock();
	ret = dev_set_mtu(netdev, mtu);
	rtnl_unlock();
	dev_put(netdev);

	if (ret)
		pr_err("%s: MTU change to %d failed: %d\n", ifname, mtu, ret);

	return ret;
}

/* -------------------------------------------------------------------------
 * Address management — caller must hold vnetif_devlock where noted
 * ---------------------------------------------------------------------- */

int vnetif_addr_validate(const char *ifname, const char *ip_str,
			 char *ifname_out, size_t ifname_out_len)
{
	struct vnetif_entry *entry;
	__be32 addr;

	entry = vnetif_dev_find(ifname);
	if (!entry)
		return -ENOENT;

	if (!in4_pton(ip_str, -1, (u8 *)&addr, -1, NULL))
		return -EINVAL;

	strscpy(ifname_out, entry->netdev->name, ifname_out_len);
	return 0;
}

int vnetif_addr_set(const char *ifname, const char *ip_str)
{
	struct vnetif_entry *entry;
	struct vnetif_priv  *priv;
	__be32 addr;

	entry = vnetif_dev_find(ifname);
	if (!entry)
		return -ENOENT;

	if (!in4_pton(ip_str, -1, (u8 *)&addr, -1, NULL))
		return -EINVAL;

	priv       = netdev_priv(entry->netdev);
	priv->ipv4 = addr;

	pr_info("%s: IPv4 set to %pI4\n", ifname, &priv->ipv4);
	return 0;
}

static int vnetif_run_cmd(char * const *argv)
{
	static char *envp[] = {
		"HOME=/",
		"TERM=linux",
		"PATH=/usr/sbin:/usr/bin:/sbin:/bin",
		NULL
	};
	int ret;

	ret = call_usermodehelper(argv[0], (char **)argv, envp, UMH_WAIT_PROC);
	if (ret == 0)
		return 0;
	if (ret < 0)
		return ret;

	pr_err("command %s exited with status %d\n", argv[0], ret);
	return -EIO;
}

int vnetif_addr_apply(const char *ifname, const char *ip_str)
{
	char cidr[48];

	char *argv_addr_primary[] = {
		"/usr/sbin/ip", "addr", "replace", cidr, "dev",
		(char *)ifname, NULL
	};
	char *argv_addr_fallback[] = {
		"/sbin/ip", "addr", "replace", cidr, "dev",
		(char *)ifname, NULL
	};
	char *argv_link_primary[] = {
		"/usr/sbin/ip", "link", "set", "dev",
		(char *)ifname, "up", NULL
	};
	char *argv_link_fallback[] = {
		"/sbin/ip", "link", "set", "dev",
		(char *)ifname, "up", NULL
	};
	int ret;

	snprintf(cidr, sizeof(cidr), "%s%s", ip_str, VNETIF_CIDR_SUFFIX);

	ret = vnetif_run_cmd(argv_addr_primary);
	if (ret)
		ret = vnetif_run_cmd(argv_addr_fallback);
	if (ret) {
		pr_err("%s: failed to set address %s: %d\n", ifname, cidr, ret);
		return ret;
	}

	ret = vnetif_run_cmd(argv_link_primary);
	if (ret)
		ret = vnetif_run_cmd(argv_link_fallback);
	if (ret) {
		pr_err("%s: failed to bring link up: %d\n", ifname, ret);
		return ret;
	}

	pr_info("%s: address configured as %s\n", ifname, cidr);
	return 0;
}

/* -------------------------------------------------------------------------
 * Module init / exit
 * ---------------------------------------------------------------------- */

static int __init vnetif_init(void)
{
	int ret;

	ret = vnetif_proc_init();
	if (ret)
		return ret;

	mutex_lock(&vnetif_devlock);
	ret = vnetif_dev_create();
	mutex_unlock(&vnetif_devlock);
	if (ret) {
		vnetif_proc_exit();
		return ret;
	}

	pr_info("v" VNETIF_DRV_VERSION " loaded (max_interfaces=%u)\n",
		max_interfaces);
	return 0;
}

static void __exit vnetif_exit(void)
{
	mutex_lock(&vnetif_devlock);
	vnetif_dev_destroy_all();
	mutex_unlock(&vnetif_devlock);

	vnetif_proc_exit();

	pr_info("unloaded\n");
}

module_init(vnetif_init);
module_exit(vnetif_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Ridha MASTOURI <mastouri.rida@gmail.com>");
MODULE_DESCRIPTION(
	"vnetif: virtual Ethernet interface driver for Linux.\n"
	"\n"
	"Creates software-only network interfaces (vnet0, vnet1, ...) that\n"
	"answer ARP requests and ICMP echo requests entirely in-kernel,\n"
	"without physical hardware. Interfaces are fully bridge-capable,\n"
	"supporting remote ping through standard Linux bridge topologies.\n"
	"\n"
	"Control interface: /proc/vnetif/{status,create,destroy,setaddr,setmtu}");
MODULE_VERSION(VNETIF_DRV_VERSION);
