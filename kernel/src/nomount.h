#ifndef _LINUX_NOMOUNT_H
#define _LINUX_NOMOUNT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/limits.h>
#include <linux/atomic.h>
#include <linux/uidgid.h>
#include <linux/stat.h>
#include <linux/ioctl.h>
#include <linux/rcupdate.h>
#include <linux/bitmap.h>
#include <linux/xattr.h>
#include <linux/version.h>

#include <asm/local.h>

#define NOMOUNT_MAGIC_CODE 0x4E /* 'N' */
#define NOMOUNT_VERSION    2
#define NOMOUNT_HASH_BITS  12
#define NM_FLAG_ACTIVE        (1 << 0)
#define NM_FLAG_IS_DIR        (1 << 7)
#define NOMOUNT_MAGIC_POS 0x7000000
#define NOMOUNT_IOC_MAGIC  NOMOUNT_MAGIC_CODE
#define NOMOUNT_IOC_ADD_RULE    _IOW(NOMOUNT_IOC_MAGIC, 1, struct nomount_ioctl_data)
#define NOMOUNT_IOC_DEL_RULE    _IOW(NOMOUNT_IOC_MAGIC, 2, struct nomount_ioctl_data)
#define NOMOUNT_IOC_CLEAR_ALL   _IO(NOMOUNT_IOC_MAGIC, 3)
#define NOMOUNT_IOC_GET_VERSION _IOR(NOMOUNT_IOC_MAGIC, 4, int)
#define NOMOUNT_IOC_ADD_UID     _IOW(NOMOUNT_IOC_MAGIC, 5, unsigned int)
#define NOMOUNT_IOC_DEL_UID     _IOW(NOMOUNT_IOC_MAGIC, 6, unsigned int)
#define NOMOUNT_IOC_GET_LIST _IOR(NOMOUNT_IOC_MAGIC, 7, int)
#define MAX_LIST_BUFFER_SIZE (1024 * 1024)
#define NM_MAX_PARENTS 16
#define NM_RECURSION_SHIFT 29
#define NM_RECURSION_MASK  (0x7UL << NM_RECURSION_SHIFT)
#define NOMOUNT_BLOOM_BITS 20
#define NOMOUNT_BLOOM_SIZE (1 << NOMOUNT_BLOOM_BITS)

atomic_t nomount_enabled = ATOMIC_INIT(0);
EXPORT_SYMBOL(nomount_enabled);
#define NOMOUNT_DISABLED() (atomic_read(&nomount_enabled) == 0)

struct linux_dirent {
    unsigned long   d_ino;
    unsigned long   d_off;
    unsigned short  d_reclen;
    char        d_name[];
};

static DEFINE_HASHTABLE(nomount_dirs_ht, NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_uid_ht, NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_vpath, NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_real_ino, NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_v_ino,    NOMOUNT_HASH_BITS);
static LIST_HEAD(nomount_rules_list);
static DEFINE_MUTEX(nomount_write_mutex);
extern struct cred init_cred;
struct kstatfs;

/* filter bloom logic */
static DECLARE_BITMAP(nomount_bloom_paths, NOMOUNT_BLOOM_SIZE);
static DECLARE_BITMAP(nomount_bloom_inos, NOMOUNT_BLOOM_SIZE);

struct nomount_ioctl_data {
    char __user *virtual_path;
    char __user *real_path;
    unsigned int flags;
    unsigned long real_ino;
    dev_t real_dev;
};

struct nomount_rule {
    unsigned long v_ino;
    unsigned long real_ino;
    dev_t v_dev;
    dev_t real_dev;
    char *virtual_path;
    char *real_path;
    size_t vp_len;
    long v_fs_type;
    u32 v_hash;
    u32 flags;
    kuid_t v_uid;
    kgid_t v_gid;
    struct hlist_node v_ino_node;
    struct hlist_node real_ino_node;
    struct hlist_node vpath_node;
    struct list_head list;
    struct list_head cleanup_list;
    struct rcu_head rcu; 
};

struct nomount_dir_node {
    struct hlist_node node;      
    char *dir_path;              
    unsigned long dir_ino;
    struct list_head cleanup_list;
    struct list_head children_names; 
    unsigned long next_child_index; /* next v_index to assign */
    struct rcu_head rcu;
};

struct nomount_child_name {
    struct list_head list;
    char *name;                  
    unsigned char d_type;
    unsigned long fake_ino;      /* deterministic fake inode for injected entries */
    unsigned long v_index;       /* stable injected index used for d_off mapping */
    struct rcu_head rcu;
};

struct nomount_uid_node {
    uid_t uid;
    struct hlist_node node;
    struct list_head list;
    struct list_head cleanup_list;
    struct rcu_head rcu;
};

/* Wrapper for vfs_getxattr across different kernel versions */
static inline ssize_t nm_vfs_getxattr(struct dentry *dentry, const char *name, void *value, size_t size)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    /* Kernel 6.3+ uses mnt_idmap */
    return vfs_getxattr(&nop_mnt_idmap, dentry, name, value, size);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    /* Kernel 5.12 to 6.2 uses user_namespace */
    return vfs_getxattr(&init_user_ns, dentry, name, value, size);
#else
    /* Kernel < 5.12 uses the classic signature */
    return vfs_getxattr(dentry, name, value, size);
#endif
}

/* Wrapper for vfs_setxattr across different kernel versions */
static inline int nm_vfs_setxattr(struct dentry *dentry, const char *name, const void *value, size_t size, int flags)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    return vfs_setxattr(&nop_mnt_idmap, dentry, name, value, size, flags);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    return vfs_setxattr(&init_user_ns, dentry, name, value, size, flags);
#else
    return vfs_setxattr(dentry, name, value, size, flags);
#endif
}

static inline void nm_enter(void) {
    unsigned long flags = current->flags;
    unsigned long level = (flags & NM_RECURSION_MASK) >> NM_RECURSION_SHIFT;

    if (level < 7) {
        level++;
        current->flags = (flags & ~NM_RECURSION_MASK) | (level << NM_RECURSION_SHIFT);
    }
}

static inline void nm_exit(void) {
    unsigned long flags = current->flags;
    unsigned long level = (flags & NM_RECURSION_MASK) >> NM_RECURSION_SHIFT;

    if (level > 0) {
        level--;
        current->flags = (flags & ~NM_RECURSION_MASK) | (level << NM_RECURSION_SHIFT);
    }
}

static inline bool nm_is_recursive(void) {
    unsigned long level = (current->flags & NM_RECURSION_MASK) >> NM_RECURSION_SHIFT;
    return (level > 5);
}

#endif /* _LINUX_NOMOUNT_H */

