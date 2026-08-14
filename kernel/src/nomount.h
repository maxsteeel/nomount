#ifndef _LINUX_NOMOUNT_H
#define _LINUX_NOMOUNT_H

#include <linux/types.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/key-type.h>
#include <linux/highmem.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif
#include <linux/jump_label.h>
#include <linux/compat.h>

#define NOMOUNT_VERSION "12"
#define NOMOUNT_MAGIC_SIG 0x4E4F4D4F554E54ULL /* "NOMOUNT" in hex */
#define NM_FLAG_IS_DIR      (1 << 0)
#define NM_FLAG_VIRTUAL_DIR (1 << 1)
#define NM_FLAG_WHITEOUT    (1 << 2)

/* flags for cleanup */
#define NM_CLEAR_UIDS  (1 << 0)
#define NM_CLEAR_RULES (1 << 1)
#define NM_CLEAR_EXIT  (1 << 2)

/* logs */
#define nm_debug(fmt, ...) printk(KERN_DEBUG "NoMount: [DEBUG] " fmt, ##__VA_ARGS__)
#define nm_info(fmt, ...) printk(KERN_INFO "NoMount: " fmt, ##__VA_ARGS__)
#define nm_warn(fmt, ...) printk(KERN_WARNING "NoMount: [WARN] " fmt, ##__VA_ARGS__)
#define nm_err(fmt, ...)  printk(KERN_ERR "NoMount: [ERROR] " fmt, ##__VA_ARGS__)

static struct rb_root_cached nomount_rules_tree = RB_ROOT_CACHED;
static LIST_HEAD(nomount_sb_list);
static DEFINE_IDR(nomount_uid_idr);
static DECLARE_RWSEM(nomount_rwsem);

/* * Helpers to dynamically calculate the memory address of the strings */
#define nm_get_vpath(rule) ((rule)->paths)
#define nm_get_rpath(rule) ((rule)->paths + (rule)->v_len + 1)

struct nm_iop {
    struct inode_operations fake_iop; /* MUST be exactly at offset 0 */
    const struct inode_operations *orig_iop;
    struct dentry_operations fake_dop;
    const struct dentry_operations *orig_dop;
    struct nomount_dir_node *dir_node;
    struct rcu_head rcu;
};

struct nm_fop {
    struct file_operations fake_fop;  /* MUST be exactly at offset 0 */
    const struct file_operations *orig_fop;
    struct nomount_dir_node *dir_node;
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
    struct nomount_dir_node *dir_node;
    unsigned long v_ino;
    u8 flags;
};

#define nm_get_real_inode(v_inode) \
    (((v_inode)->i_private && ((struct nm_inode_info *)(v_inode)->i_private)->r_path.dentry) ? \
        d_backing_inode(((struct nm_inode_info *)(v_inode)->i_private)->r_path.dentry) : NULL)

struct nomount_child_node {
    struct rcu_head rcu;
    u32 name_hash;
    u32 fake_ino;
    int id;
    u8 d_type;
    u8 flags;
    u16 name_len;
    struct nomount_rule *rule;

    /* * FLEXIBLE ARRAY MEMBER:
     * Memory Layout: [ struct ] "children_name\0"
     */
    char name[]; 
};

struct nomount_child_array {
    struct rcu_head rcu;
    int count;
    int capacity;
    u32 *hashes;
    struct nomount_child_node **nodes;
};

struct nomount_dir_node {
    struct rcu_head rcu;
    struct nomount_child_array __rcu *children;
    seqcount_t seq;
    u64 bloom_mask;
    struct inode *v_inode;
    union {
        struct inode *dir_inode;
        struct nomount_rule *owner_rule;
        unsigned long _tag_ptr;
    };
};

struct nomount_rule {
    struct rb_node rb_node;
    struct hlist_node vpath_node;
    struct nomount_dir_node *parent_dir;
    struct nomount_dir_node *this_dir;
    struct path r_path;
    unsigned long v_ino;
    u32 v_hash;
    u16 v_len;
    u8  flags;
    unsigned int target_uid;

    /* * FLEXIBLE ARRAY MEMBER: 
     * Memory Layout: [ struct ] "virtual_path\0real_path\0"
     */
    char paths[]; 
} ____cacheline_aligned;

struct nm_rule_info {
    u32 flags;
    unsigned long v_ino;
    struct path r_path;
    struct nomount_dir_node *this_dir;
};

/*** Operaction Vectors ***/
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static const struct file_operations nm_file_fops_mmap_prepare;
#endif
static const struct file_operations nm_file_fops;
static const struct inode_operations nm_file_iops;
static const struct file_operations nm_dir_fops;
static const struct inode_operations nm_dir_iops;

/*** forward declarations ***/
static struct dentry *nomount_hijacked_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);
static int nomount_hijacked_iterate_dir(struct file *file, struct dir_context *ctx);
static void nomount_hijack_dentry_ops(struct dentry *dentry);
static void nm_free_rule(struct nomount_rule *rule);

/* =====================================================================
 * NoMount VFS Offset Protocol
 * =====================================================================
 * 64-bit layout: [ 16-bit 'nm' ][ 16-bit 0 ][ 32-bit ID ] 
 * 32-bit layout: [ 16-bit 'nm' ][ 16-bit ID ]
 */
#define NM_SIG_16 0x6E6DULL /* "nm" in hex */
static inline bool nm_is_virtual_pos(loff_t pos) {
#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) return (pos & 0xFFFF0000ULL) == (NM_SIG_16 << 16);
#endif
    return (pos & 0xFFFFFFFF00000000ULL) == (NM_SIG_16 << 48);
}

static inline loff_t nm_pack_pos(int id) {
#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) return (NM_SIG_16 << 16) | (id & 0xFFFF);
#endif
    return (NM_SIG_16 << 48) | (id & 0xFFFFFFFF);
}

static inline int nm_unpack_pos(loff_t pos) {
#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) return (int)(pos & 0xFFFF);
#endif
    return (int)(pos & 0xFFFFFFFF);
}

/** RBTree Protocol ****/
#define nm_cmp(a, b, next_cmp) ((a) < (b) ? -1 : ((a) > (b) ? 1 : (next_cmp)))
static __always_inline struct nomount_rule *nm_tree_search_path(u32 hash, u16 len, const char *path)
{
    struct rb_node *node = nomount_rules_tree.rb_root.rb_node;
    while (node) {
        struct nomount_rule *rule = rb_entry(node, struct nomount_rule, rb_node);
        int cmp = nm_cmp(hash, rule->v_hash, nm_cmp(len, rule->v_len, memcmp(path, nm_get_vpath(rule), len)));
        if (cmp < 0) node = node->rb_left;
        else if (cmp > 0) node = node->rb_right;
        else return rule;
    }
    return NULL;
}

static __always_inline struct nomount_rule *nm_tree_search_exact(u32 hash, u16 len, const char *path, unsigned int uid)
{
    struct rb_node *node = nomount_rules_tree.rb_root.rb_node;
    while (node) {
        struct nomount_rule *rule = rb_entry(node, struct nomount_rule, rb_node);
        int cmp = nm_cmp(hash, rule->v_hash, nm_cmp(len, rule->v_len, nm_cmp(memcmp(path, nm_get_vpath(rule), len), 0, nm_cmp(uid, rule->target_uid, 0))));
        if (cmp < 0) node = node->rb_left;
        else if (cmp > 0) node = node->rb_right;
        else return rule;
    }
    return NULL;
}

static __always_inline void nm_tree_insert(struct nomount_rule *new_rule)
{
    struct rb_node **link = &nomount_rules_tree.rb_root.rb_node, *parent = NULL;
    const char *n_path = nm_get_vpath(new_rule);
    unsigned int n_uid = new_rule->target_uid;
    u32 n_hash = new_rule->v_hash;
    u16 n_len = new_rule->v_len;
    bool leftmost = true;
    int cmp;
    while (*link) {
        struct nomount_rule *rule = rb_entry(*link, struct nomount_rule, rb_node);
        parent = *link;
        cmp = nm_cmp(n_hash, rule->v_hash, nm_cmp(n_len, rule->v_len, nm_cmp(memcmp(n_path, nm_get_vpath(rule), n_len), 0, nm_cmp(n_uid, rule->target_uid, 0))));
        if (cmp < 0) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = false;
        }
    }
    rb_link_node(&new_rule->rb_node, parent, link);
    rb_insert_color_cached(&new_rule->rb_node, &nomount_rules_tree, leftmost);
}

/* ============================ */
/* NOMOUNT PAYLOAD PROTOCOL     */
/* ============================ */

enum {
    NM_CMD_UNSPEC = 0,
    NM_CMD_GET_VERSION,
    NM_CMD_ADD_RULE,
    NM_CMD_DEL_RULE,
    NM_CMD_CLEAR_ALL,
    NM_CMD_ADD_UID,
    NM_CMD_DEL_UID,
    NM_CMD_GET_LIST,
    NM_CMD_GET_UIDS,
    NM_CMD_ADD_RULE_BATCH,
    NM_CMD_CLEAR_RULES,
    NM_CMD_CLEAR_UIDS,
};

struct nm_payload {
    u64 magic;
    u32 cmd;
    u32 flags;
    u32 target_uid;
    u16 v_len;
    u16 r_len;
    int status;
    u32 arg1;
    u32 data_size;
    char buffer[3900];
};

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

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 2, 0)
    #define FLAGS_ARG , int flags
    #define FLAGS_VAL , flags
#else
    #define FLAGS_ARG /* Nothing */
    #define FLAGS_VAL /* Nothing */
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

#endif /* _LINUX_NOMOUNT_H */
