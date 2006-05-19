/* -*- c-basic-offset: 4 -*- */
/*
 * proclikefs.c -- /proc-like file system infrastructure; allow file systems
 * to be unmounted even while active
 * Eddie Kohler
 *
 * Copyright (c) 2002-2003 International Computer Science Institute
 * Copyright (c) 2005 Regents of the University of California
 *
 * This source code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#undef CLICK_LINUXMODULE
#include <linux/version.h>
#include <linux/config.h>
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#ifdef CONFIG_MODVERSIONS
# define MODVERSIONS
# if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
#  include <linux/modversions.h>
# endif
#endif
#include <linux/module.h>
#include "proclikefs.h"
#include <linux/string.h>
#include <linux/slab.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
# include <linux/locks.h>
#endif
#include <linux/file.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
# include <linux/namei.h>
#endif

#ifndef MOD_DEC_USE_COUNT
# define MOD_DEC_USE_COUNT	module_put(THIS_MODULE)
#endif


#if 0
# define DEBUG(args...) do { printk("<1>proclikefs: " args); printk("\n"); } while (0)
#else
# define DEBUG(args...) /* nada */
#endif

struct proclikefs_file_operations {
    struct file_operations pfo_op;
    struct proclikefs_file_operations *pfo_next;
};

struct proclikefs_inode_operations {
    struct inode_operations pio_op;
    struct proclikefs_inode_operations *pio_next;
};

struct proclikefs_file_system {
    struct file_system_type fs;
    struct list_head fs_list;
    atomic_t nsuper;
    int live;
    spinlock_t lock;
    struct proclikefs_file_operations *pfs_pfo;
    struct proclikefs_inode_operations *pfs_pio;
    char name[1];
};

static LIST_HEAD(fs_list);
static spinlock_t fslist_lock;
extern spinlock_t inode_lock;
extern spinlock_t sb_lock;

static struct super_operations proclikefs_null_super_operations;
static struct inode_operations proclikefs_null_root_inode_operations;

EXPORT_SYMBOL(proclikefs_register_filesystem);
EXPORT_SYMBOL(proclikefs_reinitialize_supers);
EXPORT_SYMBOL(proclikefs_unregister_filesystem);
EXPORT_SYMBOL(proclikefs_read_super);
EXPORT_SYMBOL(proclikefs_put_super);
EXPORT_SYMBOL(proclikefs_new_file_operations);
EXPORT_SYMBOL(proclikefs_new_inode_operations);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
static struct super_block *
proclikefs_null_read_super(struct super_block *sb, void *data, int silent)
{
    DEBUG("null_read_super");
    sb->s_dev = 0;
    return 0;
}
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
static struct dentry *
proclikefs_null_root_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *namei)
{
    return (struct dentry *)(ERR_PTR(-ENOENT));
}
#else
static struct dentry *
proclikefs_null_root_lookup(struct inode *dir, struct dentry *dentry)
{
    return (struct dentry *)(ERR_PTR(-ENOENT));
}
#endif

struct proclikefs_file_system *
proclikefs_register_filesystem(const char *name, int fs_flags,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
	struct super_block *(*get_sb) (struct file_system_type *, int, const char *, void *)
#else
	struct super_block *(*read_super) (struct super_block *, void *, int)
#endif
	)
{
    struct proclikefs_file_system *newfs = 0;
    struct list_head *next;
    int newfs_is_new = 0;
    
    if (!name)
	return 0;
    
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
    if (!try_module_get(THIS_MODULE)) {
	printk("<1>proclikefs: error using module\n");
	return 0;
    }
#else 
    MOD_INC_USE_COUNT;
#endif

    spin_lock(&fslist_lock);
    
    for (next = fs_list.next; next != &fs_list; next = next->next) {
	newfs = list_entry(next, struct proclikefs_file_system, fs_list);
	if (strcmp(name, newfs->name) == 0) {
	    if (newfs->live > 0) { /* active filesystem with that name */
		spin_unlock(&fslist_lock);
		MOD_DEC_USE_COUNT;
		return 0;
	    } else
		break;
	}
    }

    if (!newfs) {
	newfs = kmalloc(sizeof(struct proclikefs_file_system) + strlen(name), GFP_ATOMIC);
	if (!newfs) {		/* out of memory */
	    spin_unlock(&fslist_lock);
	    MOD_DEC_USE_COUNT;
	    return 0;
	}
	newfs->pfs_pfo = 0;
	newfs->pfs_pio = 0;
	list_add(&newfs->fs_list, &fs_list);
	strcpy(newfs->name, name);
	spin_lock_init(&newfs->lock);
	atomic_set(&newfs->nsuper, 0);
	newfs->fs.name = newfs->name;
	newfs->fs.next = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 0)
	newfs->fs.owner = THIS_MODULE;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 10)
	INIT_LIST_HEAD(&newfs->fs.fs_supers);
#endif
	newfs_is_new = 1;
    }

    newfs->fs.fs_flags = fs_flags;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
    newfs->fs.get_sb = get_sb;
    newfs->fs.kill_sb = kill_anon_super;
#else
    newfs->fs.read_super = read_super;
#endif
    newfs->live = 1;
    DEBUG("pfs[%p]: created filesystem %s", newfs, name);

    if (newfs_is_new) {
	int err = register_filesystem(&newfs->fs);
	if (err != 0)
	    printk("<1>proclikefs: error %d while initializing pfs[%p] (%s)\n", -err, newfs, name);
    }

    spin_unlock(&fslist_lock);
    return newfs;
}

void
proclikefs_reinitialize_supers(struct proclikefs_file_system *pfs,
			       void (*reread_super) (struct super_block *))
{
    struct super_block *sb;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 10)
	struct list_head *p;
#endif
    spin_lock(&fslist_lock);
    /* transfer superblocks */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 10)
    spin_lock(&sb_lock);
    for (p = pfs->fs.fs_supers.next; p != &pfs->fs.fs_supers; p = p->next) {
	sb = list_entry(p, struct super_block, s_instances);
	if (sb->s_type == &pfs->fs)
	    (*reread_super)(sb);
	else
	    printk("<1>proclikefs: confusion\n");
    }
    spin_unlock(&sb_lock);
#else
    for (sb = sb_entry(super_blocks.next); sb != sb_entry(&super_blocks); 
	 sb = sb_entry(sb->s_list.next))
	if (sb->s_type == &pfs->fs)
	    (*reread_super)(sb);
#endif
    spin_unlock(&fslist_lock);
}

static void
proclikefs_kill_super(struct super_block *sb, struct file_operations *dummy)
{
    struct dentry *dentry_tree;
    struct list_head *p;

    DEBUG("killing files");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
    file_list_lock();
    list_for_each(p, &sb->s_files) {
	struct file *filp = list_entry(p, struct file, f_u.fu_list);
	filp->f_op = dummy;
    }
    file_list_unlock();
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 0)
    file_list_lock();
    for (p = sb->s_files.next; p != &sb->s_files; p = p->next) {
	struct file *filp = list_entry(p, struct file, f_list);
	filp->f_op = dummy;
    }
    file_list_unlock();
#else
    (void) dummy;
    (void) p;
#endif

    lock_super(sb);

    sb->s_op = &proclikefs_null_super_operations;
    /* will not create new dentries any more */

    /* clear out dentries, starting from the root */
    /* Develop a linked list corresponding to depth-first search, through
       the d_fsdata fields. */
    /* XXX locking? */
    
    DEBUG("killing dentries");
    dentry_tree = sb->s_root;
    if (dentry_tree) {
	/* Do not d_drop(root) */
	dentry_tree->d_fsdata = 0;
    }
    while (dentry_tree) {
	struct list_head *next;
	struct dentry *active = dentry_tree;
	/* Process this dentry, move to next */
	active->d_op = 0;
	dentry_tree = (struct dentry *)active->d_fsdata;
	/* Prepend children to dentry_tree */
	next = active->d_subdirs.next;
	while (next != &active->d_subdirs) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
	    struct dentry *child = list_entry(next, struct dentry, d_u.d_child);
#else
	    struct dentry *child = list_entry(next, struct dentry, d_child);
#endif
	    next = next->next;
	    d_drop(child);
	    child->d_fsdata = (void *)dentry_tree;
	    dentry_tree = child;
	}
    }

    /* But the root inode can't be a dead inode */
    sb->s_root->d_inode->i_op = &proclikefs_null_root_inode_operations;

    unlock_super(sb);
    DEBUG("done killing super");
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 16)
static int bad_follow_link(struct dentry *dent, struct nameidata *nd)
{
# if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
	nd_set_link(nd, ERR_PTR(-EIO));
	return 0;
# else
	return vfs_follow_link(nd, ERR_PTR(-EIO));
# endif
}
#endif

static int return_EIO(void)
{
	return -EIO;
}

void
proclikefs_unregister_filesystem(struct proclikefs_file_system *pfs)
{
    struct super_block *sb;
    struct file *filp;
    struct list_head *p;
    struct proclikefs_file_operations *pfo;
    struct proclikefs_inode_operations *pio;
    
    if (!pfs)
	return;

    DEBUG("unregister_filesystem entry");
    spin_lock(&fslist_lock);

    /* clear out file operations */
    for (pfo = pfs->pfs_pfo; pfo; pfo = pfo->pfo_next) {
	struct file_operations *fo = &pfo->pfo_op;
	fo->llseek = (void *) return_EIO;
	fo->read = (void *) return_EIO;
	fo->write = (void *) return_EIO;
	fo->readdir = (void *) return_EIO;
	fo->poll = (void *) return_EIO;
	fo->ioctl = (void *) return_EIO;
	fo->mmap = (void *) return_EIO;
	fo->open = (void *) return_EIO;
	fo->flush = (void *) return_EIO;
	fo->release = (void *) return_EIO;
	fo->fsync = (void *) return_EIO;
	fo->fasync = (void *) return_EIO;
	fo->lock = (void *) return_EIO;
	fo->readv = (void *) return_EIO;
	fo->writev = (void *) return_EIO;
	fo->sendpage = (void *) return_EIO;
	fo->get_unmapped_area = (void *) return_EIO;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
	fo->aio_read = (void *) return_EIO;
	fo->aio_write = (void *) return_EIO;
	fo->unlocked_ioctl = (void *) return_EIO;
	fo->compat_ioctl = (void *) return_EIO;
	fo->aio_fsync = (void *) return_EIO;
	fo->sendfile = (void *) return_EIO;
	fo->check_flags = (void *) return_EIO;
	fo->flock = (void *) return_EIO;
#endif
    }

    for (pio = pfs->pfs_pio; pio; pio = pio->pio_next) {
	struct inode_operations *io = &pio->pio_op;
	io->create = (void *) return_EIO;
	io->lookup = (void *) return_EIO;
	io->link = (void *) return_EIO;
	io->unlink = (void *) return_EIO;
	io->symlink = (void *) return_EIO;
	io->mkdir = (void *) return_EIO;
	io->rmdir = (void *) return_EIO;
	io->mknod = (void *) return_EIO;
	io->rename = (void *) return_EIO;
	io->readlink = (void *) return_EIO;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 16)
	io->follow_link = 0;
#else
	io->follow_link = bad_follow_link;
#endif
	io->truncate = (void *) return_EIO;
	io->permission = (void *) return_EIO;
	io->setattr = (void *) return_EIO;
	io->getattr = (void *) return_EIO;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 20)
	io->setxattr = (void *) return_EIO;
	io->getxattr = (void *) return_EIO;
	io->listxattr = (void *) return_EIO;
	io->removexattr = (void *) return_EIO;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
	io->put_link = (void *) return_EIO;
#else
	io->revalidate = (void *) return_EIO;
#endif
    }
    
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 0)
    /* file operations cleared out superblock by superblock, below */
    (void) filp;
#else
    /* clear out file operations */
    /* inuse_filps is protected by the single kernel lock */
    /* XXX locking? */
    for (filp = inuse_filps; filp; filp = filp->f_next) {
	struct dentry *dentry = filp->f_dentry;
	if (!dentry)
	    continue;
	inode = dentry->d_inode;
	if (!inode || !inode->i_sb || inode->i_sb->s_type != &pfs->fs)
	    continue;
	filp->f_op = &pfs->pfs_pfo->pfo_op;
    }
#endif
    
    spin_lock(&pfs->lock);

    /* clear out superblock operations */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 10)
    DEBUG("clearing superblocks");
    spin_lock(&sb_lock);
    for (p = pfs->fs.fs_supers.next; p != &pfs->fs.fs_supers; p = p->next) {
	sb = list_entry(p, struct super_block, s_instances);
	proclikefs_kill_super(sb, &pfs->pfs_pfo->pfo_op);
    }
    spin_unlock(&sb_lock);
#else
    for (sb = sb_entry(super_blocks.next); sb != sb_entry(&super_blocks); 
	 sb = sb_entry(sb->s_list.next)) {
	if (sb->s_type != &pfs->fs)
	    continue;
	proclikefs_kill_super(sb, &pfs->pfs_pfo->pfo_op);
    }
    (void) p;
#endif

    pfs->live = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 0)
    pfs->fs.read_super = proclikefs_null_read_super;
#endif
    MOD_DEC_USE_COUNT;

    spin_unlock(&pfs->lock);
    spin_unlock(&fslist_lock);
}

void
proclikefs_read_super(struct super_block *sb)
{
    struct proclikefs_file_system *pfs = (struct proclikefs_file_system *) (sb->s_type);
    atomic_inc(&pfs->nsuper);
    DEBUG("pfs[%p]: read_super for %s", pfs, pfs->fs.name);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
    if (!try_module_get(THIS_MODULE))
	printk("<1>proclikefs: error using module\n");
#else 
    MOD_INC_USE_COUNT;
#endif
}

void
proclikefs_put_super(struct super_block *sb)
{
    struct proclikefs_file_system *pfs = (struct proclikefs_file_system *) (sb->s_type);
    atomic_dec(&pfs->nsuper);
    DEBUG("pfs[%p]: put_super for %s", pfs, pfs->fs.name);
    MOD_DEC_USE_COUNT;
    spin_lock(&fslist_lock);
    if (!pfs->live && atomic_read(&pfs->nsuper) == 0) {
	struct proclikefs_file_operations *pfo;
	struct proclikefs_inode_operations *pio;
	
	list_del(&pfs->fs_list);
	unregister_filesystem(&pfs->fs);
	while ((pfo = pfs->pfs_pfo)) {
	    pfs->pfs_pfo = pfo->pfo_next;
	    kfree(pfo);
	}
	while ((pio = pfs->pfs_pio)) {
	    pfs->pfs_pio = pio->pio_next;
	    kfree(pio);
	}
	kfree(pfs);
    }
    spin_unlock(&fslist_lock);
}

struct file_operations *
proclikefs_new_file_operations(struct proclikefs_file_system *pfs)
{
    struct proclikefs_file_operations *pfo = kmalloc(sizeof(struct proclikefs_file_operations), GFP_ATOMIC);
    
    if (pfo) {
	spin_lock(&fslist_lock);
	pfo->pfo_next = pfs->pfs_pfo;
	pfs->pfs_pfo = pfo;
	spin_unlock(&fslist_lock);
	memset(&pfo->pfo_op, 0, sizeof(struct file_operations));
    }
    return &pfo->pfo_op;
}

struct inode_operations *
proclikefs_new_inode_operations(struct proclikefs_file_system *pfs)
{
    struct proclikefs_inode_operations *pio = kmalloc(sizeof(struct proclikefs_inode_operations), GFP_ATOMIC);
    
    if (pio) {
	spin_lock(&fslist_lock);
	pio->pio_next = pfs->pfs_pio;
	pfs->pfs_pio = pio;
	spin_unlock(&fslist_lock);
	memset(&pio->pio_op, 0, sizeof(struct inode_operations));
    }
    return &pio->pio_op;
}

void
proclikefs_read_inode(struct inode *inode)
{
}

int
init_module(void)
{
    proclikefs_null_super_operations.read_inode = proclikefs_read_inode;
    proclikefs_null_super_operations.put_super = proclikefs_put_super;
    proclikefs_null_root_inode_operations.lookup = proclikefs_null_root_lookup;
    spin_lock_init(&fslist_lock);
    return 0;
}

void
cleanup_module(void)
{
    struct list_head *next;
    spin_lock(&fslist_lock);
    for (next = fs_list.next; next != &fs_list; ) {
	struct proclikefs_file_system *pfs = list_entry(next, struct proclikefs_file_system, fs_list);
	next = next->next;
	if (pfs->live || atomic_read(&pfs->nsuper) != 0)
	    printk("<1>proclikefs: unregistering active FS %s, prepare to die\n", pfs->name);	    
	unregister_filesystem(&pfs->fs);
	kfree(pfs);
    }
    spin_unlock(&fslist_lock);
}

#ifdef MODULE_AUTHOR
MODULE_AUTHOR("Eddie Kohler <kohler@cs.ucla.edu>");
#endif
#ifdef MODULE_DESCRIPTION
MODULE_DESCRIPTION("Proclikefs: allow module unload of mounted filesystems");
#endif
#ifdef MODULE_LICENSE
MODULE_LICENSE("GPL");
#endif
