#ifndef _LINUX_NOMOUNT_H
#define _LINUX_NOMOUNT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>
#include <linux/rwsem.h>
#include <net/sock.h>
#include <net/genetlink.h>
#include <linux/version.h>

#define NOMOUNT_VERSION    2
#define NOMOUNT_HASH_BITS  12
#define NOMOUNT_UID_HASH_BITS 4
#define NM_FLAG_IS_DIR        (1 << 7)

static DEFINE_HASHTABLE(nomount_dirs_ht,           NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_vpath,    NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_real_ino, NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_v_ino,    NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_basenames_ht,      NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_uid_ht,            NOMOUNT_UID_HASH_BITS);
static LIST_HEAD(nomount_rules_list);
static LIST_HEAD(nomount_private_dirs_list);
static DEFINE_MUTEX(nomount_write_mutex);
static DECLARE_RWSEM(nomount_dirs_rwsem);


struct nomount_rule {
    struct hlist_node v_ino_node;
    struct hlist_node real_ino_node;
    struct hlist_node vpath_node;
    struct hlist_node basename_node;
    struct list_head list;
    char *virtual_path;
    char *real_path;
    const char *basename;
    unsigned long v_ino;
    unsigned long real_ino;
    long v_fs_type;
    dev_t v_dev;
    dev_t real_dev;
    u32 v_hash;
    u32 flags;
    u16 vp_len;
    u16 rp_len;
    u16 b_len;
};

struct nomount_dir_node {
    struct hlist_node node;      
    struct list_head private_list;
    struct list_head children_names; 
    char *dir_path;              
    unsigned long dir_ino;
    u32 next_child_index;
    u16 dir_path_len;
    bool is_private;
};

struct nomount_child_name {
    struct list_head list;
    unsigned long fake_ino;
    u32 v_index;
    u16 name_len;
    u8 d_type;
    char name[]; /* Flexible array: must be the last member! */
};

struct nomount_uid_node {
    struct hlist_node node;
    uid_t uid;
};

/* ========================================================================= */
/* NETLINK GENERIC PROTOCOL DEFINITIONS */
/* ========================================================================= */

#define NOMOUNT_GENL_NAME "nomount"
#define NOMOUNT_GENL_VERSION 1

/* Commands */
enum {
    NOMOUNT_CMD_UNSPEC = 0,
    NOMOUNT_CMD_GET_VERSION,
    NOMOUNT_CMD_ADD_RULE,
    NOMOUNT_CMD_DEL_RULE,
    NOMOUNT_CMD_CLEAR_ALL,
    NOMOUNT_CMD_ADD_UID,
    NOMOUNT_CMD_DEL_UID,
    NOMOUNT_CMD_GET_LIST,
    __NOMOUNT_CMD_MAX,
};
#define NOMOUNT_CMD_MAX (__NOMOUNT_CMD_MAX - 1)

/* Attributes */
enum {
    NOMOUNT_ATTR_UNSPEC = 0,
    NOMOUNT_ATTR_VIRTUAL_PATH,  /* String (NLA_NUL_STRING) */
    NOMOUNT_ATTR_REAL_PATH,     /* String (NLA_NUL_STRING) */
    NOMOUNT_ATTR_FLAGS,         /* u32 (NLA_U32) */
    NOMOUNT_ATTR_UID,           /* u32 (NLA_U32) */
    NOMOUNT_ATTR_VERSION,       /* u32 (NLA_U32) */
    NOMOUNT_ATTR_PAYLOAD,       /* Binary payload for GET_LIST (NLA_BINARY) */
    __NOMOUNT_ATTR_MAX,
};

#define NOMOUNT_ATTR_MAX (__NOMOUNT_ATTR_MAX - 1)

/* * Compat macros for Generic Netlink Policy API changes.
 * Linux 4.20 moved the policy pointer from genl_ops to genl_family.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 2, 0)
#define NM_OPS_POLICY(p)    .policy = (p),
#define NM_FAMILY_POLICY(p)
#else
#define NM_OPS_POLICY(p)
#define NM_FAMILY_POLICY(p) .policy = (p),
#endif

/*
 * Recursion tracking for nomount operations. 
 * We use a lockless, fixed-size array of atomic counters.
 * To minimize collisions in heavily threaded environments,
 * we hash the memory address of the task_struct (current) instead of the PID.
 */

#define NM_RECURSION_BINS 4096
static atomic_t nm_rec_counters[NM_RECURSION_BINS];

static inline int nm_get_bin(void) {
    return (hash_ptr(current, ilog2(NM_RECURSION_BINS))) & (NM_RECURSION_BINS - 1);
}

static inline void nm_enter(void) {
    atomic_inc(&nm_rec_counters[nm_get_bin()]);
}

static inline void nm_exit(void) {
    atomic_dec(&nm_rec_counters[nm_get_bin()]);
}

static inline bool nm_is_recursive(void) {
    return atomic_read(&nm_rec_counters[nm_get_bin()]) > 5; // Threshold to detect recursion, can be tuned
}

#endif /* _LINUX_NOMOUNT_H */
