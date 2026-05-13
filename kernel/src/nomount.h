#ifndef _LINUX_NOMOUNT_H
#define _LINUX_NOMOUNT_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/atomic.h>
#include <linux/ioctl.h>

#define NOMOUNT_MAGIC_CODE 0x4E /* 'N' */
#define NOMOUNT_VERSION    2
#define NOMOUNT_HASH_BITS  12
#define NOMOUNT_UID_HASH_BITS 4
#define NM_FLAG_IS_DIR        (1 << 7)
#define NOMOUNT_MAGIC_POS 0x7000000000000000ULL
#define NOMOUNT_IOC_ADD_RULE    _IOW(NOMOUNT_MAGIC_CODE, 1, struct nomount_ioctl_data)
#define NOMOUNT_IOC_DEL_RULE    _IOW(NOMOUNT_MAGIC_CODE, 2, struct nomount_ioctl_data)
#define NOMOUNT_IOC_CLEAR_ALL   _IO(NOMOUNT_MAGIC_CODE,  3)
#define NOMOUNT_IOC_GET_VERSION _IOR(NOMOUNT_MAGIC_CODE, 4, int)
#define NOMOUNT_IOC_ADD_UID     _IOW(NOMOUNT_MAGIC_CODE, 5, unsigned int)
#define NOMOUNT_IOC_DEL_UID     _IOW(NOMOUNT_MAGIC_CODE, 6, unsigned int)
#define NOMOUNT_IOC_GET_LIST    _IOR(NOMOUNT_MAGIC_CODE, 7, int)

static DEFINE_HASHTABLE(nomount_dirs_ht,           NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_vpath,    NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_real_ino, NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_rules_by_v_ino,    NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_basenames_ht,      NOMOUNT_HASH_BITS);
static DEFINE_HASHTABLE(nomount_uid_ht,            NOMOUNT_UID_HASH_BITS);
static LIST_HEAD(nomount_rules_list);
static LIST_HEAD(nomount_private_dirs_list);
static DEFINE_MUTEX(nomount_write_mutex);

struct nomount_ioctl_data {
    u64 virtual_path;
    u64 real_path;
    u32 flags;
};

struct nomount_rule {
    unsigned long v_ino;
    dev_t v_dev;
    unsigned long real_ino;
    dev_t real_dev;
    char *virtual_path;
    char *real_path;
    const char *basename;
    size_t vp_len;
    size_t rp_len;
    size_t b_len;
    long v_fs_type;
    u32 v_hash;
    u32 flags;
    struct hlist_node v_ino_node;
    struct hlist_node real_ino_node;
    struct hlist_node vpath_node;
    struct hlist_node basename_node;
    struct list_head list;
    struct list_head cleanup_list;
};

struct nomount_dir_node {
    struct hlist_node node;      
    char *dir_path;              
    size_t dir_path_len;
    unsigned long dir_ino;
    bool is_private;
    struct list_head private_list;
    struct list_head cleanup_list;
    struct list_head children_names; 
    unsigned long next_child_index; /* next v_index to assign */
};

struct nomount_child_name {
    struct list_head list;
    struct list_head cleanup_list;   
    unsigned short name_len;       
    unsigned char d_type;
    unsigned long fake_ino;      /* deterministic fake inode for injected entries */
    unsigned long v_index;       /* stable injected index used for d_off mapping */
    char name[]; /* Flexible array: must be the last member! */
};

struct nomount_uid_node {
    uid_t uid;
    struct hlist_node node;
    struct list_head list;
    struct list_head cleanup_list;
};

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
