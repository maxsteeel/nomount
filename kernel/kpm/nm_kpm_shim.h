/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NM_KPM_SHIM_H
#define _NM_KPM_SHIM_H

#include "nm_kpm_syms.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#error "KPM build targets 6.6 and older: KernelPatch does not come up on 6.12+ (boot hangs in its pagetable bring-up), so a module built for it could never load."
#endif

#define NM_SYM(i, type) ((type)nm_kpm_sym[(i)])

#define d_add(d, i)            NM_SYM(NMS_d_add, void (*)(struct dentry *, struct inode *))((d), (i))
#define d_drop(d)              NM_SYM(NMS_d_drop, void (*)(struct dentry *))((d))
#define d_lookup(p, n)         NM_SYM(NMS_d_lookup, struct dentry *(*)(const struct dentry *, const struct qstr *))((p), (n))
#define d_splice_alias(i, d)   NM_SYM(NMS_d_splice_alias, struct dentry *(*)(struct inode *, struct dentry *))((i), (d))
#define d_parent_ino(d)        NM_SYM(NMS_d_parent_ino, ino_t (*)(struct dentry *))((d))
#define dput(d)                NM_SYM(NMS_dput, void (*)(struct dentry *))((d))
#define path_get(p)            NM_SYM(NMS_path_get, void (*)(const struct path *))((p))
#define path_put(p)            NM_SYM(NMS_path_put, void (*)(const struct path *))((p))
#define kern_path(n, f, p)     NM_SYM(NMS_kern_path, int (*)(const char *, unsigned int, struct path *))((n), (f), (p))
#define full_name_hash(s, n, l) NM_SYM(NMS_full_name_hash, unsigned int (*)(const void *, const char *, unsigned int))((s), (n), (l))
#define shrink_dcache_sb(sb)   NM_SYM(NMS_shrink_dcache_sb, void (*)(struct super_block *))((sb))

#define new_inode(sb)          NM_SYM(NMS_new_inode, struct inode *(*)(struct super_block *))((sb))
#define iput(i)                NM_SYM(NMS_iput, void (*)(struct inode *))((i))
#define igrab(i)               NM_SYM(NMS_igrab, struct inode *(*)(struct inode *))((i))
#define clear_inode(i)         NM_SYM(NMS_clear_inode, void (*)(struct inode *))((i))
#define truncate_inode_pages_final(m) \
                               NM_SYM(NMS_truncate_inode_pages_final, void (*)(struct address_space *))((m))

#define dentry_open(p, f, c)   NM_SYM(NMS_dentry_open, struct file *(*)(const struct path *, int, const struct cred *))((p), (f), (c))
#define fput(f)                NM_SYM(NMS_fput, void (*)(struct file *))((f))

#define xattr_full_name(h, n)  NM_SYM(NMS_xattr_full_name, const char *(*)(const struct xattr_handler *, const char *))((h), (n))

#define kfree(p)               NM_SYM(NMS_kfree, void (*)(const void *))((p))
#define kmem_cache_create(...)  ((struct kmem_cache *(*)())nm_kpm_sym[NMS_kmem_cache_create])(__VA_ARGS__)
#define kmem_cache_alloc(c, f) NM_SYM(NMS_kmem_cache_alloc, void *(*)(struct kmem_cache *, gfp_t))((c), (f))
#define kmem_cache_free(c, p)  NM_SYM(NMS_kmem_cache_free, void (*)(struct kmem_cache *, void *))((c), (p))
#define kmem_cache_destroy(c)  NM_SYM(NMS_kmem_cache_destroy, void (*)(struct kmem_cache *))((c))

#define idr_alloc(r, p, s, e, g) \
                               NM_SYM(NMS_idr_alloc, int (*)(struct idr *, void *, int, int, gfp_t))((r), (p), (s), (e), (g))
#define idr_find(r, id)        NM_SYM(NMS_idr_find, void *(*)(const struct idr *, unsigned long))((r), (id))
#define idr_remove(r, id)      NM_SYM(NMS_idr_remove, void *(*)(struct idr *, unsigned long))((r), (id))
#define idr_get_next(r, id)    NM_SYM(NMS_idr_get_next, void *(*)(struct idr *, int *))((r), (id))
#define idr_destroy(r)         NM_SYM(NMS_idr_destroy, void (*)(struct idr *))((r))
#define idr_init(r)             NM_SYM(NMS_idr_init, void (*)(struct idr *))((r))

#define call_rcu(h, f)         NM_SYM(NMS_call_rcu, void (*)(struct rcu_head *, rcu_callback_t))((h), (f))
#define synchronize_rcu()      NM_SYM(NMS_synchronize_rcu, void (*)(void))()
#define rcu_barrier()          NM_SYM(NMS_rcu_barrier, void (*)(void))()

#define register_key_type(t)   NM_SYM(NMS_register_key_type, int (*)(struct key_type *))((t))
#define unregister_key_type(t) NM_SYM(NMS_unregister_key_type, void (*)(struct key_type *))((t))

#define capable(c)             NM_SYM(NMS_capable, bool (*)(int))((c))

#define generic_fillattr(...)   nm_kpm_generic_fillattr(__VA_ARGS__)
#define notify_change(...)      nm_kpm_notify_change(__VA_ARGS__)

#define vfs_getattr_nosec(...)  ((int (*)())nm_kpm_sym[NMS_vfs_getattr_nosec])(__VA_ARGS__)
#define vfs_llseek(f, o, w)     NM_SYM(NMS_vfs_llseek, loff_t (*)(struct file *, loff_t, int))((f), (o), (w))
#define vfs_copy_file_range(a, b, c, d, e, g) \
                                NM_SYM(NMS_vfs_copy_file_range, ssize_t (*)(struct file *, loff_t, struct file *, loff_t, size_t, unsigned int))((a), (b), (c), (d), (e), (g))
#define shmem_file_setup(n, sz, f) \
                                NM_SYM(NMS_shmem_file_setup, struct file *(*)(const char *, loff_t, unsigned long))((n), (sz), (f))
#define generic_file_mmap(f, v) NM_SYM(NMS_generic_file_mmap, int (*)(struct file *, struct vm_area_struct *))((f), (v))
#define __vfs_getxattr(d, i, n, v, sz) \
                                NM_SYM(NMS___vfs_getxattr, int (*)(struct dentry *, struct inode *, const char *, void *, size_t))((d), (i), (n), (v), (sz))
#define get_user_pages_fast(a, n, g, p) \
                                NM_SYM(NMS_get_user_pages_fast, int (*)(unsigned long, int, unsigned int, struct page **))((a), (n), (g), (p))

#define _printk(...)            NM_SYM(NMS__printk, int (*)(const char *, ...))(__VA_ARGS__)
#define printk(...)             NM_SYM(NMS__printk, int (*)(const char *, ...))(__VA_ARGS__)
#define strlen(x)               NM_SYM(NMS_strlen, __kernel_size_t (*)(const char *))((x))
#define memcmp(a, b, n)         NM_SYM(NMS_memcmp, int (*)(const void *, const void *, __kernel_size_t))((a), (b), (n))
#define rb_insert_color(n, r)   NM_SYM(NMS_rb_insert_color, void (*)(struct rb_node *, struct rb_root *))((n), (r))
#define rb_erase(n, r)          NM_SYM(NMS_rb_erase, void (*)(struct rb_node *, struct rb_root *))((n), (r))
#define rb_next(n)              NM_SYM(NMS_rb_next, struct rb_node *(*)(const struct rb_node *))((n))
#define radix_tree_tagged(r, t) NM_SYM(NMS_radix_tree_tagged, int (*)(const struct radix_tree_root *, unsigned int))((r), (t))

#define nm_kpm_generic_fillattr(...) \
    ((void (*)())nm_kpm_sym[NMS_generic_fillattr])(__VA_ARGS__)
#define nm_kpm_notify_change(...) \
    ((int (*)())nm_kpm_sym[NMS_notify_change])(__VA_ARGS__)

static inline void nm_kpm_spin_lock(spinlock_t *l)
{ NM_SYM(NMS__raw_spin_lock, void (*)(raw_spinlock_t *))(&l->rlock); }
static inline void nm_kpm_spin_unlock(spinlock_t *l)
{ NM_SYM(NMS__raw_spin_unlock, void (*)(raw_spinlock_t *))(&l->rlock); }
#undef  spin_lock
#define spin_lock(l)            nm_kpm_spin_lock(l)
#undef  spin_unlock
#define spin_unlock(l)          nm_kpm_spin_unlock(l)

#define down_read(s)            NM_SYM(NMS_down_read, void (*)(struct rw_semaphore *))((s))
#define up_read(s)              NM_SYM(NMS_up_read, void (*)(struct rw_semaphore *))((s))
#define down_write(s)           NM_SYM(NMS_down_write, void (*)(struct rw_semaphore *))((s))
#define up_write(s)             NM_SYM(NMS_up_write, void (*)(struct rw_semaphore *))((s))

static inline void nm_kpm_rcu_read_lock(void)
{ if (nm_kpm_sym[NMS___rcu_read_lock]) NM_SYM(NMS___rcu_read_lock, void (*)(void))(); }
static inline void nm_kpm_rcu_read_unlock(void)
{ if (nm_kpm_sym[NMS___rcu_read_unlock]) NM_SYM(NMS___rcu_read_unlock, void (*)(void))(); }
#undef  rcu_read_lock
#define rcu_read_lock()         nm_kpm_rcu_read_lock()
#undef  rcu_read_unlock
#define rcu_read_unlock()       nm_kpm_rcu_read_unlock()

static inline int nm_kpm_srcu_read_lock(struct srcu_struct *ssp)
{ return NM_SYM(NMS___srcu_read_lock, int (*)(struct srcu_struct *))(ssp); }
static inline void nm_kpm_srcu_read_unlock(struct srcu_struct *ssp, int idx)
{ NM_SYM(NMS___srcu_read_unlock, void (*)(struct srcu_struct *, int))(ssp, idx); }
static inline void nm_kpm_synchronize_srcu(struct srcu_struct *ssp)
{ NM_SYM(NMS_synchronize_srcu, void (*)(struct srcu_struct *))(ssp); }
static inline int nm_kpm_init_srcu_struct(struct srcu_struct *ssp)
{ return NM_SYM(NMS_init_srcu_struct, int (*)(struct srcu_struct *))(ssp); }
static inline void nm_kpm_cleanup_srcu_struct(struct srcu_struct *ssp)
{ if (nm_kpm_sym[NMS_cleanup_srcu_struct]) NM_SYM(NMS_cleanup_srcu_struct, void (*)(struct srcu_struct *))(ssp); }
#undef  srcu_read_lock
#define srcu_read_lock(s)       nm_kpm_srcu_read_lock(s)
#undef  srcu_read_unlock
#define srcu_read_unlock(s, i)  nm_kpm_srcu_read_unlock((s), (i))
#undef  synchronize_srcu
#define synchronize_srcu(s)     nm_kpm_synchronize_srcu(s)
#undef  init_srcu_struct
#define init_srcu_struct(s)     nm_kpm_init_srcu_struct(s)
#undef  cleanup_srcu_struct
#define cleanup_srcu_struct(s)  nm_kpm_cleanup_srcu_struct(s)
/* Real DEFINE_STATIC_SRCU() bakes a compile-time struct initializer that,
 * on some kernel versions (e.g. 6.6's Tree SRCU), embeds a raw function
 * pointer to `delayed_work_timer_fn` in .data — an unrelocatable address
 * in a KPM .o. Declare it zeroed instead and bring it up at runtime via
 * init_srcu_struct(), which lets the running kernel fill in its own
 * addresses. */
#undef  DEFINE_STATIC_SRCU
#define DEFINE_STATIC_SRCU(name) static struct srcu_struct name

static inline void nm_kpm_rb_erase_cached(struct rb_node *node, struct rb_root_cached *root)
{
    if (root->rb_leftmost == node)
        root->rb_leftmost = NM_SYM(NMS_rb_next, struct rb_node *(*)(const struct rb_node *))(node);
    NM_SYM(NMS_rb_erase, void (*)(struct rb_node *, struct rb_root *))(node, &root->rb_root);
}
#undef  rb_erase_cached
#define rb_erase_cached(n, r)   nm_kpm_rb_erase_cached((n), (r))

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
static inline bool nm_kpm_idr_is_empty(const struct idr *idr)
{
    return radix_tree_empty(&idr->idr_rt) &&
           !NM_SYM(NMS_radix_tree_tagged, int (*)(const struct radix_tree_root *, unsigned int))
                (&idr->idr_rt, IDR_FREE);
}
#undef  idr_is_empty
#define idr_is_empty(i)         nm_kpm_idr_is_empty(i)
#else
#define idr_is_empty(i)         NM_SYM(NMS_radix_tree_tagged, int (*)(const struct idr *))((i))
#endif

#define __vfs_setxattr(...)     ((int (*)())nm_kpm_sym[NMS___vfs_setxattr])(__VA_ARGS__)

static ssize_t nm_kpm_generic_read_dir(struct file *f, char __user *b, size_t n, loff_t *o)
{ return NM_SYM(NMS_generic_read_dir, ssize_t (*)(struct file *, char __user *, size_t, loff_t *))(f, b, n, o); }
#undef  generic_read_dir
#define generic_read_dir        nm_kpm_generic_read_dir

#undef  DEFINE_STATIC_KEY_FALSE
#define DEFINE_STATIC_KEY_FALSE(name)  bool name
#undef  static_branch_unlikely
#define static_branch_unlikely(k)      (*(k))
#undef  static_branch_likely
#define static_branch_likely(k)        (*(k))
#undef  static_branch_enable
#define static_branch_enable(k)        (*(k) = true)
#undef  static_branch_disable
#define static_branch_disable(k)       (*(k) = false)

static inline void nm_kpm_inode_lock(struct inode *i)
{ NM_SYM(NMS_down_write, void (*)(struct rw_semaphore *))(&i->i_rwsem); }
static inline void nm_kpm_inode_unlock(struct inode *i)
{ NM_SYM(NMS_up_write, void (*)(struct rw_semaphore *))(&i->i_rwsem); }
#undef  inode_lock
#define inode_lock(i)           nm_kpm_inode_lock(i)
#undef  inode_unlock
#define inode_unlock(i)         nm_kpm_inode_unlock(i)

static inline void nm_kpm_rb_insert_color_cached(struct rb_node *node,
                                                 struct rb_root_cached *root,
                                                 bool leftmost)
{
    if (leftmost)
        root->rb_leftmost = node;
    NM_SYM(NMS_rb_insert_color, void (*)(struct rb_node *, struct rb_root *))(node, &root->rb_root);
}
#undef  rb_insert_color_cached
#define rb_insert_color_cached(n, r, l)  nm_kpm_rb_insert_color_cached((n), (r), (l))

#define kvfree_call_rcu(...)    ((void (*)())nm_kpm_sym[NMS_kvfree_call_rcu])(__VA_ARGS__)

#undef  kfree_rcu
#define kfree_rcu(ptr, rhf)     ((void (*)(struct rcu_head *, void *))                 \
                                 nm_kpm_sym[NMS_kvfree_call_rcu])(&((ptr)->rhf), (void *)(ptr))
#define d_parent_ino(d)         NM_SYM(NMS_d_parent_ino, ino_t (*)(struct dentry *))((d))

static inline bool nm_kpm_dir_emit_dotdot(struct file *file, struct dir_context *ctx)
{
    return dir_emit(ctx, "..", 2,
                    NM_SYM(NMS_d_parent_ino, ino_t (*)(struct dentry *))(file->f_path.dentry),
                    DT_DIR);
}
#undef  dir_emit_dotdot
#define dir_emit_dotdot(f, c)   nm_kpm_dir_emit_dotdot((f), (c))

static inline bool nm_kpm_dir_emit_dots(struct file *file, struct dir_context *ctx)
{
    if (ctx->pos == 0) {
        if (!dir_emit(ctx, ".", 1, file->f_path.dentry->d_inode->i_ino, DT_DIR))
            return false;
        ctx->pos = 1;
    }
    if (ctx->pos == 1) {
        if (!nm_kpm_dir_emit_dotdot(file, ctx))
            return false;
        ctx->pos = 2;
    }
    return true;
}
#undef  dir_emit_dots
#define dir_emit_dots(f, c)     nm_kpm_dir_emit_dots((f), (c))

#undef  THIS_MODULE
#define THIS_MODULE ((struct module *)0)

static inline void *nm_kpm_kmalloc(size_t sz, gfp_t flags)
{
    return NM_SYM(NMS___kmalloc, void *(*)(size_t, gfp_t))(sz, flags);
}
static inline void *nm_kpm_kzalloc(size_t sz, gfp_t flags)
{
    void *p = nm_kpm_kmalloc(sz, flags);
    if (p)
        __builtin_memset(p, 0, sz);
    return p;
}
#undef  kmalloc
#define kmalloc(sz, f)          nm_kpm_kmalloc((sz), (f))
#undef  kzalloc
#define kzalloc(sz, f)          nm_kpm_kzalloc((sz), (f))
#undef  kcalloc
#define kcalloc(n, sz, f)       nm_kpm_kzalloc((n) * (sz), (f))
#undef  kmem_cache_zalloc
#define kmem_cache_zalloc(c, f) nm_kpm_kmem_cache_zalloc((c), (f))
static inline void *nm_kpm_kmem_cache_zalloc(struct kmem_cache *c, gfp_t f)
{
    void *p = NM_SYM(NMS_kmem_cache_alloc, void *(*)(struct kmem_cache *, gfp_t))(c, f);
    return p;
}

#undef  fs_initcall
#define fs_initcall(fn)
#undef  module_init
#define module_init(fn)
#undef  module_exit
#define module_exit(fn)
#undef  MODULE_LICENSE
#define MODULE_LICENSE(s)
#undef  MODULE_AUTHOR
#define MODULE_AUTHOR(s)
#undef  MODULE_DESCRIPTION
#define MODULE_DESCRIPTION(s)
#undef  MODULE_VERSION
#define MODULE_VERSION(s)
#undef  MODULE_IMPORT_NS
#define MODULE_IMPORT_NS(ns)

#endif
