// SPDX-License-Identifier: GPL-2.0
#include <compiler.h>
#include <kpmodule.h>
#include <kallsyms.h>
#include <ktypes.h>
#include <baselib.h>
#include <kputils.h>
#include <log.h>

#include "nm_kpm_syms.h"

KPM_NAME("nomount");
KPM_VERSION("1.0.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("NoMount");
KPM_DESCRIPTION("hookless VFS path redirection");

void *nm_kpm_sym[NM_KPM_SYM_COUNT];

struct nm_sym_ent {
    int idx;
    const char *name;
    const char *alt;
    int optional;
};

static const struct nm_sym_ent nm_syms[] = {
    { NMS_d_add, "d_add", 0, 0 },
    { NMS_d_drop, "d_drop", 0, 0 },
    { NMS_d_lookup, "d_lookup", 0, 0 },
    { NMS_d_splice_alias, "d_splice_alias", 0, 0 },
    { NMS_d_parent_ino, "d_parent_ino", 0, 1 },
    { NMS_dput, "dput", 0, 0 },
    { NMS_path_get, "path_get", 0, 0 },
    { NMS_path_put, "path_put", 0, 0 },
    { NMS_kern_path, "kern_path", 0, 0 },
    { NMS_full_name_hash, "full_name_hash", 0, 0 },
    { NMS_shrink_dcache_sb, "shrink_dcache_sb", 0, 1 },

    { NMS_new_inode, "new_inode", 0, 0 },
    { NMS_iput, "iput", 0, 0 },
    { NMS_igrab, "igrab", 0, 1 },
    { NMS_clear_inode, "clear_inode", 0, 0 },
    { NMS_notify_change, "notify_change", 0, 1 },
    { NMS_truncate_inode_pages_final, "truncate_inode_pages_final", 0, 1 },
    { NMS_generic_fillattr, "generic_fillattr", 0, 0 },
    { NMS_vfs_getattr_nosec, "vfs_getattr_nosec", 0, 1 },

    { NMS_dentry_open, "dentry_open", 0, 0 },
    { NMS_fput, "fput", 0, 0 },
    { NMS_generic_read_dir, "generic_read_dir", 0, 0 },
    { NMS_vfs_llseek, "vfs_llseek", 0, 1 },
    { NMS_vfs_copy_file_range, "vfs_copy_file_range", 0, 1 },
    { NMS_shmem_file_setup, "shmem_file_setup", 0, 1 },
    { NMS_generic_file_mmap, "generic_file_mmap", 0, 0 },

    { NMS___vfs_getxattr, "__vfs_getxattr", 0, 1 },
    { NMS___vfs_setxattr, "__vfs_setxattr", 0, 0 },
    { NMS_xattr_full_name, "xattr_full_name", 0, 1 },

    { NMS___kmalloc, "__kmalloc", "__kmalloc_noprof", 0 },
    { NMS_kfree, "kfree", 0, 0 },
    { NMS_kmem_cache_create, "kmem_cache_create", "__kmem_cache_create_args", 0 },
    { NMS_kmem_cache_alloc, "kmem_cache_alloc", "kmem_cache_alloc_noprof", 0 },
    { NMS_kmem_cache_free, "kmem_cache_free", 0, 0 },
    { NMS_kmem_cache_destroy, "kmem_cache_destroy", 0, 0 },

    { NMS_idr_alloc, "idr_alloc", 0, 0 },
    { NMS_idr_find, "idr_find", 0, 0 },
    { NMS_idr_remove, "idr_remove", 0, 0 },
    { NMS_idr_get_next, "idr_get_next", 0, 0 },
    { NMS_idr_destroy, "idr_destroy", 0, 0 },
    { NMS_idr_init, "idr_init", 0, 1 },
    { NMS_rb_insert_color, "rb_insert_color", 0, 1 },
    { NMS_rb_erase, "rb_erase", 0, 0 },
    { NMS_rb_next, "rb_next", 0, 0 },
    { NMS_radix_tree_tagged, "radix_tree_tagged", 0, 0 },

    { NMS_call_rcu, "call_rcu", 0, 0 },
    { NMS_synchronize_rcu, "synchronize_rcu", 0, 0 },
    { NMS_rcu_barrier, "rcu_barrier", 0, 1 },
    { NMS_kvfree_call_rcu, "kvfree_call_rcu", "kfree_call_rcu", 1 },
    { NMS__raw_spin_lock, "_raw_spin_lock", 0, 0 },
    { NMS___rcu_read_lock, "__rcu_read_lock", 0, 1 },
    { NMS___rcu_read_unlock, "__rcu_read_unlock", 0, 1 },
    { NMS___srcu_read_lock, "__srcu_read_lock", 0, 0 },
    { NMS___srcu_read_unlock, "__srcu_read_unlock", 0, 0 },
    { NMS_synchronize_srcu, "synchronize_srcu", 0, 0 },
    { NMS_init_srcu_struct, "init_srcu_struct", 0, 0 },
    { NMS_cleanup_srcu_struct, "cleanup_srcu_struct", 0, 1 },
    { NMS__raw_spin_unlock, "_raw_spin_unlock", 0, 0 },
    { NMS_down_read, "down_read", 0, 0 },
    { NMS_up_read, "up_read", 0, 0 },
    { NMS_down_write, "down_write", 0, 0 },
    { NMS_up_write, "up_write", 0, 0 },

    { NMS_register_key_type, "register_key_type", 0, 0 },
    { NMS_unregister_key_type, "unregister_key_type", 0, 0 },

    { NMS__printk, "_printk", "printk", 0 },
    { NMS_capable, "capable", 0, 0 },
    { NMS_strlen, "strlen", 0, 1 },
    { NMS_memcmp, "memcmp", 0, 1 },
    { NMS_get_user_pages_fast, "get_user_pages_fast", 0, 1 },
    { NMS___folio_put, "__folio_put", "__put_page", 0 },
    { NMS_static_key_enable, "static_key_enable", 0, 1 },
    { NMS_static_key_disable, "static_key_disable", 0, 1 },
    { NMS___list_add_valid_or_report, "__list_add_valid_or_report", "__list_add_valid", 1 },
    { NMS_alt_cb_patch_nops, "alt_cb_patch_nops", 0, 1 },
};

static long nm_resolve_all(void)
{
    unsigned long i;
    long missing = 0;

    for (i = 0; i < sizeof(nm_syms) / sizeof(nm_syms[0]); i++) {
        const struct nm_sym_ent *e = &nm_syms[i];
        void *a = (void *)kallsyms_lookup_name(e->name);

        if (!a && e->alt)
            a = (void *)kallsyms_lookup_name(e->alt);

        nm_kpm_sym[e->idx] = a;

        if (!a && !e->optional) {
            logke("nomount: cannot resolve %s\n", e->name);
            missing++;
        }
    }
    return missing;
}

static long nm_kpm_init(const char *args, const char *event, void *__user reserved)
{
    long missing;

    (void)args; (void)event; (void)reserved;

    missing = nm_resolve_all();
    if (missing) {
        logke("nomount: %ld required symbol(s) unresolved, not starting\n", missing);
        return -2;
    }
    return nm_kpm_engine_init();
}

static long nm_kpm_ctl0(const char *args, char *__user out_msg, int outlen)
{
    (void)out_msg; (void)outlen;
    return nm_kpm_engine_ctl(args);
}

static long nm_kpm_exit(void *__user reserved)
{
    (void)reserved;
    nm_kpm_engine_exit();
    return 0;
}

KPM_INIT(nm_kpm_init);
KPM_CTL0(nm_kpm_ctl0);
KPM_EXIT(nm_kpm_exit);
