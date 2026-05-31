// SPDX-License-Identifier: GPL-2.0-only
/*
 * vnetif - Virtual Network Interface Driver
 *
 * Procfs control interface — /proc/vnetif/ directory:
 *
 *   status   (r--) list all interfaces and their IPv4 addresses
 *   create   (-w-) create a new interface (any write triggers it)
 *   destroy  (-w-) destroy an interface by name
 *   setaddr  (-w-) assign an IPv4 address: "IFNAME IP"
 *   setmtu   (-w-) change interface MTU:   "IFNAME MTU"
 *
 * Copyright (C) 2026 Ridha MASTOURI <mastouri.rida@gmail.com>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/in.h>

#include "vnetif.h"

static struct proc_dir_entry *vnetif_proc_dir;
static struct proc_dir_entry *vnetif_proc_status;
static struct proc_dir_entry *vnetif_proc_create_ent;
static struct proc_dir_entry *vnetif_proc_destroy_ent;
static struct proc_dir_entry *vnetif_proc_setaddr_ent;
static struct proc_dir_entry *vnetif_proc_setmtu_ent;

/* -------------------------------------------------------------------------
 * /proc/vnetif/status
 * ---------------------------------------------------------------------- */

static ssize_t vnetif_proc_status_read(struct file *file, char __user *ubuf,
				       size_t count, loff_t *ppos)
{
	struct vnetif_entry *entry;
	struct vnetif_priv  *priv;
	char   *kbuf;
	size_t  len = 0;
	ssize_t ret;

	if (*ppos != 0)
		return 0;

	kbuf = kzalloc(VNETIF_STATUS_BUF_MAX, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	mutex_lock(&vnetif_devlock);
	list_for_each_entry(entry, &vnetif_devlist, node) {
		priv = netdev_priv(entry->netdev);
		if (priv->ipv4)
			len += scnprintf(kbuf + len, VNETIF_STATUS_BUF_MAX - len,
					 "%-16s mtu=%-6u %pI4\n",
					 entry->netdev->name,
					 entry->netdev->mtu,
					 &priv->ipv4);
		else
			len += scnprintf(kbuf + len, VNETIF_STATUS_BUF_MAX - len,
					 "%-16s mtu=%-6u -\n",
					 entry->netdev->name,
					 entry->netdev->mtu);
		if (len >= VNETIF_STATUS_BUF_MAX - 1)
			break;
	}
	mutex_unlock(&vnetif_devlock);

	ret = simple_read_from_buffer(ubuf, count, ppos, kbuf, len);
	kfree(kbuf);
	return ret;
}

static const struct proc_ops vnetif_status_ops = {
	.proc_read = vnetif_proc_status_read,
};

/* -------------------------------------------------------------------------
 * /proc/vnetif/create
 * ---------------------------------------------------------------------- */

static ssize_t vnetif_proc_create_write(struct file *file,
					const char __user *ubuf,
					size_t count, loff_t *ppos)
{
	int ret;

	mutex_lock(&vnetif_devlock);
	ret = vnetif_dev_create();
	mutex_unlock(&vnetif_devlock);

	return ret ? (ssize_t)ret : (ssize_t)count;
}

static const struct proc_ops vnetif_create_ops = {
	.proc_write = vnetif_proc_create_write,
};

/* -------------------------------------------------------------------------
 * /proc/vnetif/destroy
 * ---------------------------------------------------------------------- */

static ssize_t vnetif_proc_destroy_write(struct file *file,
					 const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	char kbuf[VNETIF_CMD_BUF_MAX];
	int  ret;

	if (!count)
		return 0;
	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	strim(kbuf);
	if (!kbuf[0])
		return -EINVAL;

	mutex_lock(&vnetif_devlock);
	ret = vnetif_dev_destroy(kbuf);
	mutex_unlock(&vnetif_devlock);

	return ret ? (ssize_t)ret : (ssize_t)count;
}

static const struct proc_ops vnetif_destroy_ops = {
	.proc_write = vnetif_proc_destroy_write,
};

/* -------------------------------------------------------------------------
 * /proc/vnetif/setaddr
 * ---------------------------------------------------------------------- */

static ssize_t vnetif_proc_setaddr_write(struct file *file,
					 const char __user *ubuf,
					 size_t count, loff_t *ppos)
{
	char kbuf[VNETIF_CMD_BUF_MAX];
	char ifname[IFNAMSIZ] = {};
	char ip_str[16]       = {};
	int  ret;

	if (!count)
		return 0;
	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	strim(kbuf);

	if (sscanf(kbuf, "%15s %15s", ifname, ip_str) != 2)
		return -EINVAL;

	mutex_lock(&vnetif_devlock);
	ret = vnetif_addr_validate(ifname, ip_str, ifname, sizeof(ifname));
	mutex_unlock(&vnetif_devlock);
	if (ret)
		return ret;

	ret = vnetif_addr_apply(ifname, ip_str);
	if (ret)
		return ret;

	mutex_lock(&vnetif_devlock);
	ret = vnetif_addr_set(ifname, ip_str);
	mutex_unlock(&vnetif_devlock);

	return ret ? (ssize_t)ret : (ssize_t)count;
}

static const struct proc_ops vnetif_setaddr_ops = {
	.proc_write = vnetif_proc_setaddr_write,
};

/* -------------------------------------------------------------------------
 * /proc/vnetif/setmtu
 * ---------------------------------------------------------------------- */

static ssize_t vnetif_proc_setmtu_write(struct file *file,
					const char __user *ubuf,
					size_t count, loff_t *ppos)
{
	char kbuf[VNETIF_CMD_BUF_MAX];
	char ifname[IFNAMSIZ] = {};
	int  mtu, ret;

	if (!count)
		return 0;
	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	strim(kbuf);

	if (sscanf(kbuf, "%15s %d", ifname, &mtu) != 2)
		return -EINVAL;

	/* vnetif_dev_set_mtu acquires the locks itself (requires rtnl). */
	ret = vnetif_dev_set_mtu(ifname, mtu);

	return ret ? (ssize_t)ret : (ssize_t)count;
}

static const struct proc_ops vnetif_setmtu_ops = {
	.proc_write = vnetif_proc_setmtu_write,
};

/* -------------------------------------------------------------------------
 * Init / exit
 * ---------------------------------------------------------------------- */

int vnetif_proc_init(void)
{
	vnetif_proc_dir = proc_mkdir(VNETIF_DRV_NAME, NULL);
	if (!vnetif_proc_dir)
		return -ENOMEM;

	vnetif_proc_status = proc_create("status", 0444, vnetif_proc_dir,
					 &vnetif_status_ops);
	if (!vnetif_proc_status)
		goto err_rmdir;

	vnetif_proc_create_ent = proc_create("create", 0222, vnetif_proc_dir,
					     &vnetif_create_ops);
	if (!vnetif_proc_create_ent)
		goto err_rm_status;

	vnetif_proc_destroy_ent = proc_create("destroy", 0222, vnetif_proc_dir,
					      &vnetif_destroy_ops);
	if (!vnetif_proc_destroy_ent)
		goto err_rm_create;

	vnetif_proc_setaddr_ent = proc_create("setaddr", 0222, vnetif_proc_dir,
					      &vnetif_setaddr_ops);
	if (!vnetif_proc_setaddr_ent)
		goto err_rm_destroy;

	vnetif_proc_setmtu_ent = proc_create("setmtu", 0222, vnetif_proc_dir,
					     &vnetif_setmtu_ops);
	if (!vnetif_proc_setmtu_ent)
		goto err_rm_setaddr;

	return 0;

err_rm_setaddr:
	proc_remove(vnetif_proc_setaddr_ent);
err_rm_destroy:
	proc_remove(vnetif_proc_destroy_ent);
err_rm_create:
	proc_remove(vnetif_proc_create_ent);
err_rm_status:
	proc_remove(vnetif_proc_status);
err_rmdir:
	proc_remove(vnetif_proc_dir);
	return -ENOMEM;
}

void vnetif_proc_exit(void)
{
	proc_remove(vnetif_proc_setmtu_ent);
	proc_remove(vnetif_proc_setaddr_ent);
	proc_remove(vnetif_proc_destroy_ent);
	proc_remove(vnetif_proc_create_ent);
	proc_remove(vnetif_proc_status);
	proc_remove(vnetif_proc_dir);
}