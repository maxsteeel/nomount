/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NM_KPM_SYMS_H
#define _NM_KPM_SYMS_H

enum nm_kpm_sym {

    NMS_d_add, NMS_d_drop, NMS_d_lookup, NMS_d_splice_alias, NMS_d_parent_ino,
    NMS_dput, NMS_path_get, NMS_path_put, NMS_kern_path, NMS_full_name_hash,
    NMS_shrink_dcache_sb,

    NMS_new_inode, NMS_iput, NMS_igrab, NMS_clear_inode, NMS_notify_change,
    NMS_truncate_inode_pages_final, NMS_generic_fillattr, NMS_vfs_getattr_nosec,

    NMS_dentry_open, NMS_fput, NMS_generic_read_dir, NMS_vfs_llseek,
    NMS_vfs_copy_file_range, NMS_shmem_file_setup, NMS_generic_file_mmap,

    NMS___vfs_getxattr, NMS___vfs_setxattr, NMS_xattr_full_name,

    NMS___kmalloc, NMS_kfree, NMS_kmem_cache_create, NMS_kmem_cache_alloc,
    NMS_kmem_cache_free, NMS_kmem_cache_destroy,

    NMS_idr_alloc, NMS_idr_find, NMS_idr_remove, NMS_idr_get_next,
    NMS_idr_destroy, NMS_idr_init, NMS_rb_insert_color, NMS_rb_erase, NMS_rb_next,
    NMS_radix_tree_tagged,

    NMS_call_rcu, NMS_synchronize_rcu, NMS_rcu_barrier, NMS_kvfree_call_rcu,
    NMS__raw_spin_lock, NMS__raw_spin_unlock,
    NMS___rcu_read_lock, NMS___rcu_read_unlock,
    NMS___srcu_read_lock, NMS___srcu_read_unlock, NMS_synchronize_srcu,
    NMS_init_srcu_struct, NMS_cleanup_srcu_struct,
    NMS_down_read, NMS_up_read, NMS_down_write, NMS_up_write,

    NMS_register_key_type, NMS_unregister_key_type,

    NMS__printk, NMS_capable, NMS_strlen, NMS_memcmp,
    NMS_get_user_pages_fast, NMS___folio_put,
    NMS_static_key_enable, NMS_static_key_disable,

    NMS___list_add_valid_or_report, NMS_alt_cb_patch_nops,

    NM_KPM_SYM_COUNT
};

extern void *nm_kpm_sym[NM_KPM_SYM_COUNT];

int  nm_kpm_engine_init(void);
void nm_kpm_engine_exit(void);
long nm_kpm_engine_ctl(const char *args);

#endif
