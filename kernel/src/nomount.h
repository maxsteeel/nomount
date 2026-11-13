#ifndef _LINUX_NOMOUNT_H
#define _LINUX_NOMOUNT_H

#include <linux/types.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/magic.h>
#include <linux/key-type.h>
#include <linux/highmem.h>
#include <linux/version.h>
#include <linux/compat.h>

#define NOMOUNT_BASE_VERSION "23"
#ifdef NOMOUNT_COMMIT_COUNT
    #define NOMOUNT_VERSION ""NOMOUNT_BASE_VERSION" ("NOMOUNT_COMMIT_COUNT")"
#else
    #define NOMOUNT_VERSION NOMOUNT_BASE_VERSION
#endif

#define NM_MAGIC_SIG 0x4E4F4D4F554E54ULL /* "NOMOUNT" in hex */
#define NM_FLAG_IS_DIR      (1 << 0)
#define NM_FLAG_VIRTUAL_DIR (1 << 1)
#define NM_FLAG_WHITEOUT    (1 << 2)
#define NM_FLAG_OPAQUE      (1 << 3)

/* flags for cleanup */
#define NM_CLEAR_UIDS  (1 << 0)
#define NM_CLEAR_RULES (1 << 1)
#define NM_CLEAR_EXIT  (1 << 2)

/* logs */
#define nm_debug(fmt, ...) printk(KERN_DEBUG "NoMount: [DEBUG] " fmt, ##__VA_ARGS__)
#define nm_info(fmt, ...) printk(KERN_INFO "NoMount: " fmt, ##__VA_ARGS__)
#define nm_warn(fmt, ...) printk(KERN_WARNING "NoMount: [WARN] " fmt, ##__VA_ARGS__)
#define nm_err(fmt, ...)  printk(KERN_ERR "NoMount: [ERROR] " fmt, ##__VA_ARGS__)

static struct nm_uid_array __rcu *nm_uids = NULL;
static DEFINE_HASHTABLE(nm_leaves_ht, 6);
static DEFINE_MUTEX(nm_mutex);
static LIST_HEAD(nm_sb_list);
static struct nm_rule_array *nm_retired_arrays;
static struct nm_leaf *nm_retired_leaves;

/* * Helpers to dynamically calculate the memory address of the strings / structs */
#define nm_get_child_name(leaf) ((leaf)->paths + (leaf)->v_len - (leaf)->child_len)

struct nm_dir_ops {
    const struct inode_operations *orig_iop;
    const struct file_operations *orig_fop;
    struct nm_dir_node *dir_node;
    const struct dentry_operations *orig_dops;

    struct inode_operations fake_iop;
    struct file_operations fake_fop;
    struct dentry_operations fake_dops;
    struct rcu_head rcu;
};

struct nm_sop {
    struct super_operations fake_sop; /* MUST be exactly at offset 0 */
    const struct super_operations *orig_sop;
    const struct xattr_handler **orig_xattr;
    const struct xattr_handler **fake_xattr;
    struct super_block *sb;
    struct rcu_head rcu;
    struct list_head list;
};

struct nm_inode_info {
    struct path r_path;
    union { struct nm_dir_node *dir_node; struct nm_leaf *leaf; const void *node; };
    struct file *r_file;
    bool opaque;
    bool backing_child;
    struct rcu_head rcu;
};

struct nm_child {
    struct nm_leaf *leaf;
    u32 hash;
};

struct nm_child_array {
    struct rcu_head rcu;
    int count;
    u16 bucket_slots;
    u32 starts[] __aligned(4);
};

struct nm_dir_node {
    struct rcu_head rcu;
    struct nm_child_array __rcu *children;
    int readdir_count;
    atomic_t refs;
    struct dentry *pinned_dentry;
    struct nm_leaf *owner;
};

struct nm_rule {
    struct path r_path;
    unsigned int target_uid;
    unsigned int uid_end;
    unsigned int deny_start;
    unsigned int deny_end;
    u16 flags;
};

struct nm_rule_array {
    struct nm_rule_array *next;
    int count;
    int uid_count;
    struct nm_rule rules[];
};

struct nm_leaf {
    struct path anchor;
    struct nm_rule_array __rcu *rules;
    struct nm_dir_node *this_dir;
    struct nm_dir_node *parent_dir;
    unsigned long v_ino;
    u32 v_hash;
    u16 v_len;
    u16 child_len;
    bool native_file;
    u16 child_hash;
    atomic_t refs;
    union { struct hlist_node ht_node; struct nm_leaf *next; struct rcu_head rcu; };
    char paths[];
};

struct nm_rule_info {
    u16 flags;
    bool backing_child;
    unsigned long v_ino;
    unsigned long key;
    struct path r_path;
    struct nm_dir_node *this_dir;
    struct nm_leaf *leaf;
};

struct nm_uid_array {
    struct rcu_head rcu;
    int count;
    u64 bloom_mask[4];
    uid_t uids[];
};

static inline struct nm_child *nm_entries(struct nm_child_array *arr)
{
    uintptr_t p = (uintptr_t)arr->starts + arr->bucket_slots * sizeof(u32);
    return (struct nm_child *)((p + 7) & ~(uintptr_t)7);
}

static inline u16 nm_child_slots_for(int count)
{
    if (count <= 2)   return 0;
    if (count <= 16)  return 9;
    if (count <= 64)  return 33;
    if (count <= 256) return 129;
    if (count <= 1024) return 257;
    return 513;
}

static inline u32 nm_child_bucket(struct nm_child_array *arr, u32 hash)
{
    return (hash >> 5) & (arr->bucket_slots - 2);
}

/*** Operaction Vectors ***/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static const struct file_operations nm_file_fops_mmap_prepare;
#endif
static const struct file_operations nm_file_fops;
static const struct inode_operations nm_file_iops;
static const struct file_operations nm_dir_fops;
static const struct inode_operations nm_dir_iops;
static const struct dentry_operations nm_dops;

/*** forward declarations ***/
extern int vfs_path_lookup(struct dentry *, struct vfsmount *, const char *, unsigned int, struct path *);
static struct dentry *nm_hijacked_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);
static int nm_hijacked_iterate_dir(struct file *file, struct dir_context *ctx);
static void nm_hijacked_evict_inode(struct inode *inode);
static void nm_hijack_dentry_ops(struct inode *dir, struct dentry *dentry, bool injected);
static void nm_dir_put(struct nm_dir_node *dir_node);
static void nm_leaf_put(struct nm_leaf *leaf);
static void nm_shutdown_release(void);
static int nm_shutdown_sync_fs(struct super_block *sb, int wait);
static int nm_del_rule_locked(const char *path, u16 len, unsigned int uid, unsigned int uid_end, unsigned int deny_start, unsigned int deny_end);
static void nm_clear_state(int clear_flags);

/* =====================================================================
 * NoMount VFS Offset Protocol
 * =====================================================================
 * 64-bit layout: [ 16-bit 'nm' ][ 16-bit FRAG ][ 32-bit ID ]
 * 32-bit layout: [ 16-bit 'nm' ][ 16-bit ID ]
 */
#define NM_SIG_16 0x6E6DULL /* "nm" in hex */
static inline bool nm_is_virtual_pos(loff_t pos) {
#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) return (pos & 0xFFFF0000ULL) == (NM_SIG_16 << 16);
#endif
    return (pos & 0xFFFF000000000000ULL) == (NM_SIG_16 << 48);
}

static inline loff_t nm_pack_pos(u32 id, u32 frag) {
#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) return (NM_SIG_16 << 16) | (id & 0xFFFF);
#endif
    return ((loff_t)NM_SIG_16 << 48) | ((loff_t)(frag & 0xFFFF) << 32) | id;
}

static inline u32 nm_unpack_frag(loff_t pos) {
#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) return 0;
#endif
    return (u32)((pos >> 32) & 0xFFFF);
}

static inline u32 nm_unpack_pos(loff_t pos) {
#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) return pos & 0xFFFF;
#endif
    return (u32)pos;
}

/* --- UIDs Array RCU Management --- */
static __always_inline void nm_uid_bloom_set(struct nm_uid_array *arr, uid_t uid)
{
    arr->bloom_mask[(uid >> 6) & 3] |= 1ULL << (uid & 63);
}
static __always_inline bool nm_uid_bloom_test(struct nm_uid_array *arr, uid_t uid)
{
    return !!(arr->bloom_mask[(uid >> 6) & 3] & (1ULL << (uid & 63)));
}

static inline int nm_uid_add(uid_t target)
{
    struct nm_uid_array *old, *new_arr;
    int count = 0;

    if ((old = rcu_dereference_protected(nm_uids, lockdep_is_held(&nm_mutex)))) {
        for (int i = 0; i < (count = old->count); i++) if (old->uids[i] == target) return -EEXIST;
    }

    if (!(new_arr = kmalloc(sizeof(*new_arr) + (count + 1) * sizeof(uid_t), GFP_KERNEL)))
        return -ENOMEM;

    new_arr->count = count + 1;
    if (old) {
        memcpy(new_arr->uids, old->uids, count * sizeof(uid_t));
        memcpy(new_arr->bloom_mask, old->bloom_mask, sizeof(new_arr->bloom_mask));
    } else {
        memset(new_arr->bloom_mask, 0, sizeof(new_arr->bloom_mask));
    }
    new_arr->uids[count] = target;
    nm_uid_bloom_set(new_arr, target);
    rcu_assign_pointer(nm_uids, new_arr);
    if (old) kfree_rcu(old, rcu);
    return 0;
}

static inline int nm_uid_del(uid_t target)
{
    struct nm_uid_array *old, *new_arr = NULL;
    int count, target_idx = -1;

    if (!(old = rcu_dereference_protected(nm_uids, lockdep_is_held(&nm_mutex)))) return -ENOENT;
    for (int i = 0; i < (count = old->count); i++) if (old->uids[i] == target) { target_idx = i; break; }
    if (target_idx < 0) return -ENOENT;

    if (count > 1) {
        if (!(new_arr = kmalloc(sizeof(*new_arr) + (count - 1) * sizeof(uid_t), GFP_KERNEL))) return -ENOMEM;
        new_arr->count = count - 1;
        if (target_idx > 0) memcpy(new_arr->uids, old->uids, target_idx * sizeof(uid_t));
        if (target_idx < count - 1) memcpy(new_arr->uids + target_idx, old->uids + target_idx + 1, (count - target_idx - 1) * sizeof(uid_t));
        memset(new_arr->bloom_mask, 0, sizeof(new_arr->bloom_mask));
        for (int i = 0; i < new_arr->count; i++)
            nm_uid_bloom_set(new_arr, new_arr->uids[i]);
    }

    rcu_assign_pointer(nm_uids, new_arr);
    kfree_rcu(old, rcu);
    return 0;
}

/* ============================ */
/* NOMOUNT PAYLOAD PROTOCOL     */
/* ============================ */

enum {
    NM_CMD_UNSPEC = 0,
    NM_CMD_GET_VERSION,
    NM_CMD_ADD_RULE,
    NM_CMD_DEL_RULE,
    NM_CMD_ADD_UID,
    NM_CMD_DEL_UID,
    NM_CMD_CLEAR_ALL,
    NM_CMD_CLEAR_RULES,
    NM_CMD_CLEAR_UIDS,
    NM_CMD_GET_LIST,
    NM_CMD_GET_UIDS,
    NM_CMD_BLOCK_ISOLATED_UIDS,
    NM_CMD_GET_ISOLATED_STATE,
};

struct nm_payload {
    u64 magic;
    u32 cmd;
    u32 target_uid;
    int status;
    u32 arg1;
    u32 data_size;
    char buffer[4068];
} __attribute__((packed));

struct nm_rule_hdr {
    u32 flags;
    u32 uid;
    u32 uid_end;
    u32 deny_start;
    u32 deny_end;
    u16 v_len;
    u16 r_len;
} __attribute__((packed));

struct nm_del_hdr {
    u32 uid;
    u32 uid_end;
    u32 deny_start;
    u32 deny_end;
    u16 v_len;
} __attribute__((packed));

/* * Compat macros * */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    #define IDMAP_PATH(path) mnt_idmap((path).mnt),
    #define IDMAP_ARG struct mnt_idmap *idmap,
    #define IDMAP_CALL idmap,
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0)
    #define IDMAP_PATH(path) mnt_user_ns((path).mnt),
    #define IDMAP_ARG struct user_namespace *mnt_userns,
    #define IDMAP_CALL mnt_userns,
#else
    #define IDMAP_PATH(path)/* Nothing */
    #define IDMAP_ARG /* Nothing */
    #define IDMAP_CALL /* Nothing */
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
    #define NM_ACTOR_RET bool
    #define NM_ACTOR_CONTINUE true
#else
    #define NM_ACTOR_RET int
    #define NM_ACTOR_CONTINUE 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
    #define nm_inode_is_new(inode) (inode_state_read_once(inode) & I_NEW)
#else
    #define nm_inode_is_new(inode) (inode->i_state & I_NEW)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0)
#define sb_rdonly(sb) ((sb)->s_flags & MS_RDONLY)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
    #define FLAGS_ARG , int flags
    #define FLAGS_VAL , flags
#else
    #define FLAGS_ARG /* Nothing */
    #define FLAGS_VAL /* Nothing */
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
# define nm_lookup_one_len_unlocked(name, dentry, len) lookup_one_unlocked(&nop_mnt_idmap, &(struct qstr)QSTR_INIT((name), (len)), dentry)
#else
# define nm_lookup_one_len_unlocked lookup_one_len_unlocked
#endif

#ifndef for_each_alias
#define for_each_alias(dentry, inode) \
    hlist_for_each_entry(dentry, &(inode)->i_dentry, d_u.d_alias)
#endif

#ifndef DCACHE_DONTCACHE
# define DCACHE_DONTCACHE 0
#endif

static inline void nm_sync_inode_times(struct inode *v_inode, struct inode *r_inode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    v_inode->i_atime_sec = r_inode->i_atime_sec;
    v_inode->i_atime_nsec = r_inode->i_atime_nsec;
    v_inode->i_mtime_sec = r_inode->i_mtime_sec;
    v_inode->i_mtime_nsec = r_inode->i_mtime_nsec;
    v_inode->i_ctime_sec = r_inode->i_ctime_sec;
    v_inode->i_ctime_nsec = r_inode->i_ctime_nsec;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    v_inode->i_atime = r_inode->i_atime;
    v_inode->i_mtime = r_inode->i_mtime;
    inode_set_ctime_to_ts(v_inode, inode_get_ctime(r_inode));
#else
    v_inode->i_atime = r_inode->i_atime;
    v_inode->i_mtime = r_inode->i_mtime;
    v_inode->i_ctime = r_inode->i_ctime;
#endif
}

static inline int nm_call_iterate(struct file *file, struct dir_context *ctx, const struct file_operations *fop)
{
    if (fop->iterate_shared)
        return fop->iterate_shared(file, ctx);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    else if (fop->iterate)
        return fop->iterate(file, ctx);
#endif
    return -ENOTDIR;
}

static inline struct dentry *nm_hash_and_lookup(struct dentry *dir, struct qstr *n) {
    n->hash = full_name_hash(dir, n->name, n->len);
    return (unlikely(dir->d_flags & DCACHE_OP_HASH) && dir->d_op->d_hash(dir, n) < 0) ? NULL : d_lookup(dir, n);
}

static inline struct nm_dir_ops *nm_get_nm_iop(const struct inode_operations *iop) {
    if (likely(iop) && iop->lookup == nm_hijacked_lookup)
        return container_of(iop, struct nm_dir_ops, fake_iop);
    return NULL;
}

static inline struct nm_dir_ops *nm_get_nm_fop(const struct file_operations *fop) {
    if (unlikely(!fop)) return NULL;
    if (fop->iterate_shared == nm_hijacked_iterate_dir)
        return container_of(fop, struct nm_dir_ops, fake_fop);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    if (fop->iterate == nm_hijacked_iterate_dir)
        return container_of(fop, struct nm_dir_ops, fake_fop);
#endif
    return NULL;
}

static inline struct nm_sop *nm_get_nm_sop(const struct super_operations *sop) {
    if (likely(sop) && (sop->evict_inode == nm_hijacked_evict_inode || sop->sync_fs == nm_shutdown_sync_fs))
        return container_of(sop, struct nm_sop, fake_sop);
    return NULL;
}

#define NM_DOP_INITIALIZING ((const struct dentry_operations *)1L)
static inline const struct dentry_operations *nm_get_orig_dops(struct nm_dir_ops *iop)
{
    const struct dentry_operations *dops;
    if (!iop) return NULL;
    dops = smp_load_acquire(&iop->orig_dops);
    return (dops == NM_DOP_INITIALIZING) ? NULL : dops;
}

static inline int nm_call_orig_revalidate(const struct dentry_operations *dops, struct inode *parent_inode,
                                          const struct qstr *name, struct dentry *dentry, unsigned int flags)
{
    if (!dops || !dops->d_revalidate) return 1;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
    return dops->d_revalidate(parent_inode, name, dentry, flags);
#else
    return dops->d_revalidate(dentry, flags);
#endif
}

static inline bool nm_inode_is_virtual(struct inode *inode) {
    return inode->i_op == &nm_file_iops || inode->i_op == &nm_dir_iops;
}

static bool nm_is_pseudo_fs(struct super_block *sb)
{
    switch (sb->s_magic) {
    case PROC_SUPER_MAGIC:
    case SYSFS_MAGIC:
    case DEVPTS_SUPER_MAGIC:
    case DEBUGFS_MAGIC:
    case TRACEFS_MAGIC:
    case SECURITYFS_MAGIC:
    case CGROUP_SUPER_MAGIC:
#ifdef CGROUP2_SUPER_MAGIC
    case CGROUP2_SUPER_MAGIC:
#endif
    case SELINUX_MAGIC:
    case BPF_FS_MAGIC:
        return true;
    default:
        return false;
    }
}

static bool nm_is_kernfs(struct super_block *sb)
{
    switch (sb->s_magic) {
    case SYSFS_MAGIC:
    case CGROUP_SUPER_MAGIC:
#ifdef CGROUP2_SUPER_MAGIC
    case CGROUP2_SUPER_MAGIC:
#endif
    case TRACEFS_MAGIC:
    case SECURITYFS_MAGIC:
    case BPF_FS_MAGIC:
    case DEBUGFS_MAGIC:
        return true;
    default:
        return false;
    }
}

static bool nm_dir_lookup_tolerates_cross_sb(struct super_block *sb)
{
    switch (sb->s_magic) {
    case F2FS_SUPER_MAGIC:
    case EXT4_SUPER_MAGIC:
#if defined(EROFS_SUPER_MAGIC_V1) && BITS_PER_LONG == 64
    case EROFS_SUPER_MAGIC_V1:
#endif
    case SQUASHFS_MAGIC:
        return true;
    default:
        return false;
    }
}

static inline int nm_drop_dentry(struct dentry *dentry, unsigned int flags, bool weak)
{
    if (flags & LOOKUP_RCU) return -ECHILD;
    d_drop(dentry);
    return weak ? -ESTALE : 0;
}

/* RCU read lock scope guard */
static __always_inline void __nm_rcu_unlock(int *g) { (void)g; rcu_read_unlock(); }
#define rcu_guard() rcu_read_lock(); int __attribute__((cleanup(__nm_rcu_unlock), unused)) __UNIQUE_ID(rcug)

#endif /* _LINUX_NOMOUNT_H */
