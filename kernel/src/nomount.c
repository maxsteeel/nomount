#include <linux/init.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/xattr.h>
#include <linux/version.h>
#include <linux/module.h>
#include "nomount.h"

static struct kmem_cache *nm_dir_cachep __read_mostly, *nm_uid_cachep __read_mostly;
atomic_t nm_active_rules = ATOMIC_INIT(0);
atomic_t nm_active_uids = ATOMIC_INIT(0);
DEFINE_STATIC_KEY_FALSE(nomount_active_rules);
DEFINE_STATIC_KEY_FALSE(nomount_active_uids);

/*** Verification & Compatibility Checks ***/

static __always_inline bool nomount_is_uid_blocked(uid_t uid) {
    struct nomount_uid_node *entry;
    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_uid_ht, entry, node, uid) {
        if (entry->uid == uid) {
            rcu_read_unlock();
            return true;
        }
    }
    rcu_read_unlock();
    return false;
}

static __always_inline bool __nomount_should_skip(void) {
    if (!static_branch_unlikely(&nomount_active_rules)) return true;
    if (unlikely(in_interrupt() || oops_in_progress)) return true;
    if (unlikely(current->flags & (PF_KTHREAD | PF_EXITING))) return true;
    if (unlikely(static_branch_unlikely(&nomount_active_uids))) {
        if (unlikely(nomount_is_uid_blocked(current_uid().val))) return true;
    }
    return false;
}

static __always_inline struct nomount_rule *nomount_find_child_rule(struct nomount_dir_node *dir_node, 
                                                    const char *name, size_t len)
{
    struct nm_child_array *array;
    struct nomount_rule *found = NULL;
    u32 i;

    rcu_read_lock();
    array = rcu_dereference(dir_node->child_array);
    if (likely(array)) {
        for (i = 0; i < array->num_children; i++) {
            struct nomount_child_name *child = &array->entries[i];
            char *child_name_str = nm_get_child_name(array, child);
            if (child_name_str[len] == '\0' && memcmp(child_name_str, name, len) == 0) {
                found = child->rule;
                break;
            }
        }
    }
    rcu_read_unlock();
    return found;
}

/*** i_op / s_op / f_op Hijacking Hooks ***/

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#define copy_from_kernel_nofault probe_kernel_read
#endif

#define __get_nm(ptr, type, member) ({ \
    type *__target = NULL; \
    u64 __sig = 0; \
    if (likely(ptr)) { \
        type *__outer = container_of(ptr, type, member); \
        if (copy_from_kernel_nofault(&__sig, &__outer->signature, sizeof(__sig)) == 0) { \
            if (__sig == NOMOUNT_MAGIC_SIG) \
                __target = __outer; \
        } \
    } \
    __target; \
})

static struct nomount_dir_node *nomount_get_dir_node(struct inode *inode) {
    struct nm_iop *nm_iop;
    struct nm_fop *nm_fop;

    nm_iop = __get_nm(smp_load_acquire(&inode->i_op), struct nm_iop, fake_iop);
    if (nm_iop && nm_iop->dir_node) return nm_iop->dir_node;

    nm_fop = __get_nm(smp_load_acquire(&inode->i_fop), struct nm_fop, fake_fop);
    if (nm_fop && nm_fop->dir_node) return nm_fop->dir_node;
    
    return NULL;
}

struct nomount_proxy_ctx {
    struct dir_context ctx;
    struct dir_context *orig_ctx;
    struct nm_child_array *array;
};

static NM_ACTOR_RET nomount_actor_proxy(struct dir_context *ctx, const char *name, int namelen,
                                        loff_t offset, u64 ino, unsigned int d_type)
{
    struct nomount_proxy_ctx *proxy = container_of(ctx, struct nomount_proxy_ctx, ctx);
    struct nm_child_array *array = proxy->array;
    NM_ACTOR_RET ret;
    u32 i;

    if (likely(array)) {
        for (i = 0; i < array->num_children; i++) {
            struct nomount_child_name *child = &array->entries[i];
            char *child_name_str = nm_get_child_name(array, child);
            if (child_name_str[namelen] == '\0' && 
                memcmp(child_name_str, name, namelen) == 0) {
                return NM_ACTOR_CONTINUE;
            }
        }
    }

    proxy->orig_ctx->pos = proxy->ctx.pos;
    ret = proxy->orig_ctx->actor(proxy->orig_ctx, name, namelen, offset, ino, d_type);
    proxy->ctx.pos = proxy->orig_ctx->pos;

    return ret;
}

static int nomount_hijacked_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct nm_fop *nm_fop = __get_nm(smp_load_acquire(&file->f_op), struct nm_fop, fake_fop);
    struct nm_child_array *array = NULL;
    loff_t nomount_magic_pos = 0x7000000000000000ULL;
    unsigned long v_index;
    int res = 0;
    u32 i;

    if (__nomount_should_skip() || !nm_fop || !nm_fop->orig_fop)
        goto do_real_iterate;

#ifdef CONFIG_COMPAT
    if (in_compat_syscall()) nomount_magic_pos = 0x7E000000;
#endif

    rcu_read_lock();
    array = rcu_dereference(nm_fop->dir_node->child_array);
    if (array && atomic_inc_not_zero(&array->refcnt)) {
        rcu_read_unlock();
    } else {
        rcu_read_unlock();
        goto do_real_iterate;
    }

    if (ctx->pos < nomount_magic_pos) {
        struct nomount_proxy_ctx proxy_ctx = {
            .ctx.actor = nomount_actor_proxy,
            .ctx.pos = ctx->pos,
            .orig_ctx = ctx,
            .array = array
        };

        if (nm_fop->orig_fop->iterate_shared)
            res = nm_fop->orig_fop->iterate_shared(file, &proxy_ctx.ctx);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
        else if (nm_fop->orig_fop->iterate)
            res = nm_fop->orig_fop->iterate(file, &proxy_ctx.ctx);
#endif
        else
            res = -ENOTDIR;

        ctx->pos = proxy_ctx.ctx.pos;
    }

    if (res >= 0) {
        if (ctx->pos >= nomount_magic_pos && ctx->pos < nomount_magic_pos + 100000) {
            v_index = (unsigned long)(ctx->pos - nomount_magic_pos);
        } else {
            v_index = 0;
            ctx->pos = nomount_magic_pos;
        }

        for (i = v_index; i < array->num_children; i++) {
            struct nomount_child_name *child = &array->entries[i];
            char *child_name_str = nm_get_child_name(array, child);
            if (child->flags & NM_FLAG_WHITEOUT) {
                ctx->pos = nomount_magic_pos + i + 1;
                continue;
            }
            if (!dir_emit(ctx, child_name_str, strlen(child_name_str), child->fake_ino, child->d_type))
                break;
            ctx->pos = nomount_magic_pos + i + 1;
        }
    }

    if (atomic_dec_and_test(&array->refcnt)) 
        kfree_rcu(array, rcu);

    return res;

do_real_iterate:
    if (nm_fop && nm_fop->orig_fop) {
        if (nm_fop->orig_fop->iterate_shared)
            return nm_fop->orig_fop->iterate_shared(file, ctx);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
        else if (nm_fop->orig_fop->iterate)
            return nm_fop->orig_fop->iterate(file, ctx);
#endif
    }
    return -ENOTDIR;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static int nomount_hijacked_iterate(struct file *file, struct dir_context *ctx)
{
    return nomount_hijacked_iterate_shared(file, ctx);
}
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

static int nm_open(struct inode *inode, struct file *file)
{
    struct inode *r_inode = inode->i_private;
    int ret = 0;

    if (r_inode && r_inode->i_fop && r_inode->i_fop->open) {
        file->f_inode = r_inode;
        file->f_mapping = r_inode->i_mapping;
        ret = r_inode->i_fop->open(r_inode, file);
        file->f_inode = inode;
    }
    return ret;
}

static int nm_release(struct inode *inode, struct file *file)
{
    struct inode *r_inode = inode->i_private;
    int ret = 0;

    if (r_inode && r_inode->i_fop && r_inode->i_fop->release) {
        file->f_inode = r_inode;
        ret = r_inode->i_fop->release(r_inode, file);
        file->f_inode = inode;
    }
    return ret;
}

static loff_t nm_llseek(struct file *file, loff_t offset, int whence)
{
    struct inode *v_inode = file_inode(file);
    struct inode *r_inode = v_inode->i_private;

    if (likely(r_inode)) {
        v_inode->i_size = i_size_read(r_inode);
    }
    return generic_file_llseek(file, offset, whence);
}

static ssize_t nm_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct inode *v_inode = file_inode(file);
    struct inode *r_inode = v_inode->i_private;
    ssize_t ret;

    if (unlikely(!r_inode || !r_inode->i_fop || !r_inode->i_fop->read_iter))
        return generic_file_read_iter(iocb, to);

    file->f_mapping = r_inode->i_mapping;
    file->f_inode = r_inode;
    ret = r_inode->i_fop->read_iter(iocb, to);
    file->f_inode = v_inode;
    
    return ret;
}

static ssize_t nm_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct inode *v_inode = file_inode(file);
    struct inode *r_inode = v_inode->i_private;
    ssize_t ret;

    if (unlikely(!r_inode || !r_inode->i_fop || !r_inode->i_fop->write_iter))
        return generic_file_write_iter(iocb, from);

    file->f_mapping = r_inode->i_mapping;
    file->f_inode = r_inode;
    ret = r_inode->i_fop->write_iter(iocb, from);
    file->f_inode = v_inode;

    return ret;
}

static int nm_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct inode *v_inode = file_inode(file);
    struct inode *r_inode = v_inode->i_private;
    const struct file_operations *old_fop;
    int ret;

    if (unlikely(!r_inode))
        return -ENODEV;

    file->f_mapping = r_inode->i_mapping;
    file->f_inode = r_inode;

    if (unlikely(!r_inode->i_fop || !r_inode->i_fop->mmap)) {
        ret = generic_file_mmap(file, vma);
        file->f_inode = v_inode;
        return ret;
    }

    old_fop = file->f_op;
    file->f_op = r_inode->i_fop;
    
    ret = r_inode->i_fop->mmap(file, vma);
    
    file->f_op = old_fop;
    file->f_inode = v_inode;

    if (ret == -ENODEV || ret == -ENOEXEC) {
        file->f_inode = r_inode;
        ret = generic_file_mmap(file, vma);
        file->f_inode = v_inode;
    }

    return ret;
}

static const struct file_operations nm_fops = {
    .llseek = nm_llseek,
    .open = nm_open,
    .release = nm_release,
    .read_iter = nm_read_iter,
    .write_iter = nm_write_iter,
    .mmap = nm_mmap,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static int nm_mmap_prepare(struct vm_area_desc *desc)
{
    struct file *file = desc->file;
    struct inode *v_inode = file_inode(file);
    struct inode *r_inode = v_inode->i_private;
    const struct file_operations *old_fop;
    int ret;

    if (unlikely(!r_inode || !r_inode->i_fop || !r_inode->i_fop->mmap_prepare))
        return -ENODEV;

    file->f_mapping = r_inode->i_mapping;
    file->f_inode = r_inode;
    old_fop = file->f_op;
    file->f_op = r_inode->i_fop;
    
    ret = r_inode->i_fop->mmap_prepare(desc);
    
    file->f_op = old_fop;
    file->f_inode = v_inode;
    file->f_mapping = v_inode->i_mapping;

    return ret;
}

static const struct file_operations nm_fops_mmap_prepare = {
    .llseek = nm_llseek,
    .open = nm_open,
    .release = nm_release,
    .read_iter = nm_read_iter,
    .write_iter = nm_write_iter,
    .mmap_prepare = nm_mmap_prepare,
};
#endif

static ssize_t nm_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
    struct inode *v_inode = d_inode(dentry);
    struct inode *r_inode = v_inode->i_private;
    struct dentry *r_dentry;
    ssize_t ret;

    if (unlikely(!r_inode || !r_inode->i_op || !r_inode->i_op->listxattr))
        return -EOPNOTSUPP;

    r_dentry = d_find_alias(r_inode);
    if (!r_dentry) {
        struct inode *grabbed = igrab(r_inode);
        if (!grabbed) return -ENODATA;
        r_dentry = d_obtain_alias(grabbed);
        if (IS_ERR(r_dentry)) return PTR_ERR(r_dentry);
    }

    ret = r_inode->i_op->listxattr(r_dentry, buffer, size);
    dput(r_dentry);
    return ret;
}

static int nm_file_getattr(IDMAP_ARG const struct path *path, struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
    struct inode *v_inode = d_backing_inode(path->dentry);
    struct inode *r_inode = v_inode->i_private;

    if (unlikely(!r_inode))
        return -EIO;

    v_inode->i_size = i_size_read(r_inode);
    v_inode->i_blocks = r_inode->i_blocks;
    nm_sync_inode_times(v_inode, r_inode);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
    generic_fillattr(IDMAP_CALL request_mask, v_inode, stat);
#else
    generic_fillattr(IDMAP_CALL v_inode, stat);
#endif
    return 0;
}

static int nm_setattr(IDMAP_ARG struct dentry *dentry, struct iattr *attr)
{
    struct inode *v_inode = d_inode(dentry);
    struct inode *r_inode = v_inode->i_private;
    struct dentry *r_dentry;
    int err;

    if (unlikely(!r_inode)) 
        return -EIO;

    r_dentry = d_find_alias(r_inode);
    if (!r_dentry) {
        r_dentry = d_obtain_alias(igrab(r_inode));
        if (IS_ERR(r_dentry))
            return PTR_ERR(r_dentry);
    }

    inode_lock(r_inode);
    err = notify_change(IDMAP_CALL r_dentry, attr, NULL);
    inode_unlock(r_inode);

    if (likely(!err)) {
        if (attr->ia_valid & ATTR_MODE)
            v_inode->i_mode = r_inode->i_mode;
        if (attr->ia_valid & ATTR_UID)
            v_inode->i_uid = r_inode->i_uid;
        if (attr->ia_valid & ATTR_GID)
            v_inode->i_gid = r_inode->i_gid;

        nm_sync_inode_times(v_inode, r_inode);
    }

    dput(r_dentry);
    return err;
}

static const char *nm_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done)
{
    struct inode *real_inode = inode->i_private;

    if (!dentry)
        return ERR_PTR(-ECHILD);

    if (real_inode && real_inode->i_op && real_inode->i_op->get_link)
        return real_inode->i_op->get_link(dentry, real_inode, done);

    return ERR_PTR(-EINVAL);
}

static const struct inode_operations nm_file_iops = {
    .getattr = nm_file_getattr,
    .setattr = nm_setattr,
    .listxattr = nm_listxattr,
};

static const struct inode_operations nm_symlink_iops = {
    .getattr = simple_getattr,
    .setattr = nm_setattr,
    .get_link = nm_get_link,
    .listxattr = nm_listxattr,
};

static int nm_dir_iterate_shared(struct file *file, struct dir_context *ctx)
{
    struct inode *v_inode = file_inode(file);
    struct inode *r_inode = v_inode->i_private;
    int ret = -ENOTDIR;

    if (r_inode && r_inode->i_fop && r_inode->i_fop->iterate_shared) {
        file->f_inode = r_inode;
        ret = r_inode->i_fop->iterate_shared(file, ctx);
        file->f_inode = v_inode;
    }
    return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
static int nm_dir_iterate(struct file *file, struct dir_context *ctx)
{
    struct inode *v_inode = file_inode(file);
    struct inode *r_inode = v_inode->i_private;
    int ret = -ENOTDIR;

    if (r_inode && r_inode->i_fop && r_inode->i_fop->iterate) {
        file->f_inode = r_inode;
        ret = r_inode->i_fop->iterate(file, ctx);
        file->f_inode = v_inode;
    }
    return ret;
}
#endif

static struct dentry *nm_dir_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct inode *r_dir = dir->i_private;
    if (r_dir && r_dir->i_op && r_dir->i_op->lookup)
        return r_dir->i_op->lookup(r_dir, dentry, flags);

    return ERR_PTR(-EOPNOTSUPP);
}

static const struct file_operations nm_dir_fops = {
    .open = nm_open,
    .release = nm_release,
    .llseek = nm_llseek,
    .read = generic_read_dir,
    .iterate_shared = nm_dir_iterate_shared,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    .iterate = nm_dir_iterate,
#endif
};

static const struct inode_operations nm_dir_iops = {
    .lookup = nm_dir_lookup,
    .getattr = nm_file_getattr,
    .setattr = nm_setattr,
    .listxattr = nm_listxattr,
};

struct nm_xattr_proxy {
    struct xattr_handler fake;
    const struct xattr_handler *orig;
};

static int nm_xattr_get(const struct xattr_handler *handler, struct dentry *dentry, struct inode *inode, const char *name, void *buffer, size_t size FLAGS_ARG)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    
    if (inode->i_op == &nm_file_iops || inode->i_op == &nm_symlink_iops || inode->i_op == &nm_dir_iops) {
        struct inode *r_inode = inode->i_private;
        if (!r_inode) return -ENODATA;
        if (r_inode->i_sb != inode->i_sb) return -ENODATA;
        return proxy->orig->get(proxy->orig, dentry, r_inode, name, buffer, size FLAGS_VAL);
    }
    return proxy->orig->get(proxy->orig, dentry, inode, name, buffer, size FLAGS_VAL);
}

static int nm_xattr_set(const struct xattr_handler *handler, IDMAP_ARG struct dentry *dentry, struct inode *inode, const char *name, const void *buffer, size_t size, int flags)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    
    if (inode->i_op == &nm_file_iops || inode->i_op == &nm_symlink_iops || inode->i_op == &nm_dir_iops) {
        struct inode *r_inode = inode->i_private;
        if (!r_inode) return -ENODATA;
        if (r_inode->i_sb != inode->i_sb) return -EOPNOTSUPP;
        return proxy->orig->set(proxy->orig, IDMAP_CALL dentry, r_inode, name, buffer, size, flags);
    }
    return proxy->orig->set(proxy->orig, IDMAP_CALL dentry, inode, name, buffer, size, flags);
}

static struct inode *nomount_create_new_inode(struct super_block *virtual_sb, 
                                              struct inode *real_inode, 
                                              struct nomount_rule *rule)
{
    struct inode *inode = new_inode(virtual_sb);
    if (unlikely(!inode)) return NULL;

    inode->i_ino = rule->v_ino;
    inode->i_mode = real_inode->i_mode;
    inode->i_size = i_size_read(real_inode);
    inode->i_blocks = real_inode->i_blocks;
    inode->i_uid = real_inode->i_uid;
    inode->i_gid = real_inode->i_gid;
    nm_sync_inode_times(inode, real_inode);

    if (S_ISDIR(real_inode->i_mode)) {
        inode->i_op = &nm_dir_iops;
        inode->i_fop = &nm_dir_fops;
    } else if (S_ISLNK(real_inode->i_mode) || (real_inode->i_op && real_inode->i_op->get_link)) {
        inode->i_op = &nm_symlink_iops;
        inode->i_fop = &nm_fops;
    } else { 
        inode->i_op = &nm_file_iops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
        if (real_inode->i_fop && real_inode->i_fop->mmap_prepare)
            inode->i_fop = &nm_fops_mmap_prepare;
        else
#endif
            inode->i_fop = &nm_fops;
    }
    inode->i_mapping = real_inode->i_mapping;
    inode->i_private = igrab(real_inode);
    inode->i_flags |= S_PRIVATE | S_NOATIME | S_NOCMTIME | S_NOSEC;
    inode->i_opflags |= IOP_XATTR;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
    INIT_LIST_HEAD(&inode->i_data.i_private_list);
#else
    INIT_LIST_HEAD(&inode->i_data.private_list);
#endif
    insert_inode_hash(inode);

    return inode;
}

static struct dentry *nomount_hijacked_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct nm_iop *nm_iop = __get_nm(smp_load_acquire(&dir->i_op), struct nm_iop, fake_iop);
    struct nomount_rule *rule;
    const char *name = dentry->d_name.name;
    size_t len = dentry->d_name.len;

    if (unlikely(__nomount_should_skip() || !nm_iop || !nm_iop->dir_node))
        goto fallback;

    rule = nomount_find_child_rule(nm_iop->dir_node, name, len);
    if (likely(rule)) {
        if (rule->flags & NM_FLAG_WHITEOUT) {
            d_add(dentry, NULL); 
            return NULL;
        }

        if (likely(rule->cached_r_inode)) {
            struct inode *new_inode = nomount_create_new_inode(dir->i_sb, rule->cached_r_inode, rule);
            if (likely(new_inode)) {
                nm_debug("Lookup hijacked! Splicing inode %lu into dentry '%s'\n", new_inode->i_ino, name);
                return d_splice_alias(new_inode, dentry);
            }
        }
    }

fallback:
    if (nm_iop && nm_iop->orig_iop && nm_iop->orig_iop->lookup) {
        return nm_iop->orig_iop->lookup(dir, dentry, flags);
    }
    
    return ERR_PTR(-EOPNOTSUPP);
}

static void nomount_hijacked_destroy_inode(struct inode *inode)
{
    struct nm_sop *nm_sop;

    if (inode->i_private) {
        iput((struct inode *)inode->i_private);
        inode->i_private = NULL;
    }

    nm_sop = __get_nm(smp_load_acquire(&inode->i_sb->s_op), struct nm_sop, fake_sop);
    if (nm_sop && nm_sop->orig_sop && nm_sop->orig_sop->destroy_inode) {
        nm_sop->orig_sop->destroy_inode(inode);
    }
}

/* --- Hijacking Management --- */

static inline void nomount_hijack_superblock(struct super_block *sb)
{
    struct nm_sop *nm_sop;
    int i, count = 0;
    if (unlikely(!sb || !sb->s_op || __get_nm(smp_load_acquire(&sb->s_op), struct nm_sop, fake_sop))) return;

    nm_sop = kzalloc(sizeof(*nm_sop), GFP_KERNEL);
    if (unlikely(!nm_sop)) return;

    memcpy(&nm_sop->fake_sop, sb->s_op, sizeof(struct super_operations));
    nm_sop->orig_sop = sb->s_op;
    nm_sop->signature = NOMOUNT_MAGIC_SIG;
    nm_sop->sb = sb;
    nm_sop->fake_sop.destroy_inode = nomount_hijacked_destroy_inode;

    if (sb->s_xattr && !nm_sop->orig_xattr) {
        const struct xattr_handler **new_array;
        while (sb->s_xattr[count]) count++;
        new_array = kzalloc((count + 1) * sizeof(void *), GFP_KERNEL);
        if (new_array) {
            for (i = 0; i < count; i++) {
                struct nm_xattr_proxy *proxy = kzalloc(sizeof(*proxy), GFP_KERNEL);
                if (!proxy) continue;
                proxy->orig = sb->s_xattr[i];
                proxy->fake.name = proxy->orig->name;
                proxy->fake.prefix = proxy->orig->prefix;
                proxy->fake.flags = proxy->orig->flags;
                proxy->fake.list = proxy->orig->list;
                if (proxy->orig->get) proxy->fake.get = nm_xattr_get;
                if (proxy->orig->set) proxy->fake.set = nm_xattr_set;
                new_array[i] = &proxy->fake;
            }
            nm_sop->orig_xattr = (const struct xattr_handler **)sb->s_xattr;
            nm_sop->fake_xattr = new_array;
            smp_store_release((const struct xattr_handler ***)&sb->s_xattr, new_array);
            nm_debug("xattr handlers successfully hijacked for dev: 0x%x\n", sb->s_dev);
        }
    }

    hash_add_rcu(nomount_sb_ht, &nm_sop->node, (unsigned long)sb);
    smp_store_release(&sb->s_op, &nm_sop->fake_sop);
    nm_debug("Superblock successfully hijacked for dev: 0x%x\n", sb->s_dev);
}

static inline void nomount_hijack_virtual_parent(struct nomount_dir_node *dir_node, struct inode *inode)
{
    struct nm_fop *nm_fop;
    if (unlikely(!inode->i_fop || __get_nm(smp_load_acquire(&inode->i_fop), struct nm_fop, fake_fop))) return;

    nm_fop = kzalloc(sizeof(*nm_fop), GFP_KERNEL);
    if (likely(nm_fop)) {
        memcpy(&nm_fop->fake_fop, inode->i_fop, sizeof(struct file_operations));
        nm_fop->orig_fop = inode->i_fop;
        nm_fop->signature = NOMOUNT_MAGIC_SIG;
        nm_fop->dir_node = dir_node;

        if (nm_fop->fake_fop.iterate_shared)
            nm_fop->fake_fop.iterate_shared = nomount_hijacked_iterate_shared;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
        else if (nm_fop->fake_fop.iterate)
            nm_fop->fake_fop.iterate = nomount_hijacked_iterate;
#endif

        smp_store_release(&inode->i_fop, &nm_fop->fake_fop);
        nm_debug("i_fop successfully hijacked for virtual parent dir (ino: %lu)\n", inode->i_ino);
    }
}

static inline void nomount_hijack_dir_inode(struct nomount_dir_node *dir_node, struct inode *inode)
{
    struct nm_iop *nm_iop;
    if (unlikely(!inode->i_op || __get_nm(smp_load_acquire(&inode->i_op), struct nm_iop, fake_iop))) return;

    nm_iop = kzalloc(sizeof(*nm_iop), GFP_KERNEL);
    if (likely(nm_iop)) {
        memcpy(&nm_iop->fake_iop, inode->i_op, sizeof(struct inode_operations));
        nm_iop->orig_iop = inode->i_op;
        nm_iop->signature = NOMOUNT_MAGIC_SIG;
        nm_iop->dir_node = dir_node;
        nm_iop->had_private_flag = (inode->i_flags & S_PRIVATE) != 0;

        if (nm_iop->orig_iop->lookup) nm_iop->fake_iop.lookup = nomount_hijacked_lookup;
        smp_store_release(&inode->i_op, &nm_iop->fake_iop);
        inode->i_flags |= S_PRIVATE;
        nm_debug("i_op successfully hijacked for parent dir (ino: %lu)\n", inode->i_ino);
    }
}

static void nomount_invalidate_dcache(const char *v_path)
{
    struct path target_path;
    if (kern_path(v_path, 0, &target_path) == 0) {
        shrink_dcache_parent(target_path.dentry);
        d_drop(target_path.dentry);
        path_put(&target_path);
    }
}

static void nomount_restore_dir_node(struct nomount_dir_node *dir_node)
{
    struct inode *t_inode = dir_node->dir_inode;
    struct nm_iop *nm_iop;
    struct nm_fop *nm_fop;
    struct dentry *d;
 
    if (unlikely(!t_inode)) return;

    spin_lock(&t_inode->i_lock);
    nm_iop = __get_nm(smp_load_acquire(&t_inode->i_op), struct nm_iop, fake_iop);
    if (nm_iop && nm_iop->dir_node == dir_node) {
        smp_store_release(&t_inode->i_op, nm_iop->orig_iop);
        if (!nm_iop->had_private_flag) t_inode->i_flags &= ~S_PRIVATE;
        nm_debug("Successfully cured i_op for dir %lu\n", t_inode->i_ino);
        kfree_rcu(nm_iop, rcu);
    }

    nm_fop = __get_nm(smp_load_acquire(&t_inode->i_fop), struct nm_fop, fake_fop);
    if (nm_fop && nm_fop->dir_node == dir_node) {
        smp_store_release(&t_inode->i_fop, nm_fop->orig_fop);
        nm_debug("Successfully cured i_fop for dir %lu\n", t_inode->i_ino);
        kfree_rcu(nm_fop, rcu);
    }
    spin_unlock(&t_inode->i_lock);

    d = d_find_alias(t_inode);
    if (d) {
        shrink_dcache_parent(d);
        d_drop(d);
        dput(d);
    }

    iput(t_inode);
    dir_node->dir_inode = NULL;
}

static void nomount_restore_superblocks(void)
{
    struct nm_sop *nm_sop;
    struct hlist_node *tmp;
    int bkt;

    hash_for_each_safe(nomount_sb_ht, bkt, tmp, nm_sop, node) {
        int i = 0;
        if (nm_sop->sb) {
            smp_store_release(&nm_sop->sb->s_op, nm_sop->orig_sop);
            if (nm_sop->fake_xattr) {
                smp_store_release((const struct xattr_handler ***)&nm_sop->sb->s_xattr, nm_sop->orig_xattr);
                while (nm_sop->fake_xattr[i]) {
                    kfree(container_of(nm_sop->fake_xattr[i], struct nm_xattr_proxy, fake));
                    i++;
                }
                kfree(nm_sop->fake_xattr);
            }
            nm_debug("Successfully cured superblock for dev: 0x%x\n", nm_sop->sb->s_dev);
        }
        hash_del_rcu(&nm_sop->node);
        kfree_rcu(nm_sop, rcu);
    }
}

/*** Module Management ***/

static struct nomount_dir_node *__nomount_alloc_dir_node(struct inode *inode) 
{
    struct nomount_dir_node *dir_node = kmem_cache_alloc(nm_dir_cachep, GFP_KERNEL);
    if (unlikely(!dir_node)) return NULL;

    dir_node->dir_inode = inode ? igrab(inode) : NULL;
    RCU_INIT_POINTER(dir_node->child_array, NULL);
    list_add_tail(&dir_node->list, &nomount_all_dirs_list);

    return dir_node;
}

static void __nomount_inject_child_locked(struct nomount_dir_node *dir_node, struct nomount_rule *rule,
                                           const char *name, size_t name_len)
{
    struct nm_child_array *old_array, *new_array;
    u32 i, old_num = 0, old_heap_size = 0, new_heap_size;
    size_t new_total_size;
    char *old_heap = NULL, *new_heap;

    if (unlikely(!dir_node)) return;
    rule->parent_dir = dir_node;

    old_array = rcu_dereference_protected(dir_node->child_array, lockdep_is_held(&nomount_write_mutex));
    if (old_array) {
        old_num = old_array->num_children;
        old_heap_size = old_array->heap_size;

        for (i = 0; i < old_num; i++) {
            char *child_name_str = nm_get_child_name(old_array, &old_array->entries[i]);
            if (strlen(child_name_str) == name_len && !memcmp(child_name_str, name, name_len)) {
                old_array->entries[i].flags = rule->flags;
                return;
            }
        }
        old_heap = (char *)&old_array->entries[old_num];
    }

    new_heap_size = old_heap_size + name_len + 1;
    new_total_size = sizeof(struct nm_child_array) + 
                     ((old_num + 1) * sizeof(struct nomount_child_name)) + new_heap_size;

    new_array = kmalloc(new_total_size, GFP_KERNEL);
    if (unlikely(!new_array)) return;

    atomic_set(&new_array->refcnt, 1);
    new_array->num_children = old_num + 1;
    new_array->heap_size = new_heap_size;
    new_heap = (char *)&new_array->entries[old_num + 1];

    if (old_array) {
        memcpy(new_array->entries, old_array->entries, old_num * sizeof(struct nomount_child_name));
        memcpy(new_heap, old_heap, old_heap_size);
    }

    new_array->entries[old_num].name_offset = old_heap_size;
    new_array->entries[old_num].flags = rule->flags;
    new_array->entries[old_num].d_type = (rule->flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG;
    new_array->entries[old_num].fake_ino = rule->v_hash;
    new_array->entries[old_num].rule = rule;
    memcpy(new_heap + old_heap_size, name, name_len + 1);
    rcu_assign_pointer(dir_node->child_array, new_array);

    if (old_array && atomic_dec_and_test(&old_array->refcnt)) {
        kfree_rcu(old_array, rcu);
    }
}

static void __nomount_delete_child_locked(struct nomount_dir_node *dir_node, unsigned long fake_ino, struct list_head *d_victims)
{
    struct nm_child_array *old_array, *new_array;
    int found_idx = -1;
    u32 i, num, dst = 0;
    u32 new_heap_size, current_offset = 0;
    char *old_heap, *new_heap;

    old_array = rcu_dereference_protected(dir_node->child_array, lockdep_is_held(&nomount_write_mutex));
    if (!old_array) return;

    num = old_array->num_children;
    for (i = 0; i < num; i++) {
        if (old_array->entries[i].fake_ino == fake_ino) {
            found_idx = i;
            break;
        }
    }
    if (found_idx == -1) return;

    if (num == 1) {
        rcu_assign_pointer(dir_node->child_array, NULL);
        if (atomic_dec_and_test(&old_array->refcnt)) kfree_rcu(old_array, rcu);
        list_del(&dir_node->list);
        list_add(&dir_node->list, d_victims);
    } else {
        new_heap_size = old_array->heap_size - (strlen(nm_get_child_name(old_array, &old_array->entries[found_idx])) + 1);
        new_array = kmalloc(sizeof(struct nm_child_array) + ((num - 1) * sizeof(struct nomount_child_name)) + new_heap_size, GFP_KERNEL);
        if (unlikely(!new_array)) return;

        atomic_set(&new_array->refcnt, 1);
        new_array->num_children = num - 1;
        new_array->heap_size = new_heap_size;
        old_heap = (char *)&old_array->entries[num];
        new_heap = (char *)&new_array->entries[num - 1];

        for (i = 0; i < num; i++) {
            u16 len;
            if (i == found_idx) continue;
            memcpy(&new_array->entries[dst], &old_array->entries[i], sizeof(struct nomount_child_name));
            len = strlen(nm_get_child_name(old_array, &old_array->entries[i]));
            memcpy(new_heap + current_offset, old_heap + old_array->entries[i].name_offset, len + 1);
            new_array->entries[dst].name_offset = current_offset;
            current_offset += len + 1;
            dst++;
        }

        rcu_assign_pointer(dir_node->child_array, new_array);
        if (atomic_dec_and_test(&old_array->refcnt))
            kfree_rcu(old_array, rcu);
    }
}

static int nomount_generate_virtual_topology(struct nomount_rule *rule)
{
    struct nomount_rule *ex, *irule = NULL, *t_rule, *pending_rules[32];
    struct nomount_dir_node *dir_node; 
    struct path p_path, inter_r_path;
    char *v_tmp = nm_get_vpath(rule);
    char *r_tmp = nm_get_rpath(rule);
    char *slash_v, *slash_r, *slashes_v[32], *slashes_r[32];
    int cur_r_len = rule->flags & NM_FLAG_WHITEOUT ? 0 : strlen(r_tmp);
    int cur_v_len = rule->v_len;
    struct dentry *found_ghost = NULL; 
    struct qstr qname;
    const char *child_name;
    int p_count = 0, err = 0;
    size_t child_name_len;
    bool inter_exists;
    u32 h_inter;

    while (p_count < 32) {
        slash_v = strrchr(v_tmp, '/');
        slash_r = r_tmp ? strrchr(r_tmp, '/') : NULL; 
        if (slash_r == r_tmp) slash_r = NULL;

        if (!slash_v || slash_v == v_tmp) {
            if (likely(kern_path("/", LOOKUP_FOLLOW, &p_path) == 0)) {
                struct inode *v_inode = d_backing_inode(p_path.dentry);
                dir_node = nomount_get_dir_node(v_inode);
                if (!dir_node) dir_node = __nomount_alloc_dir_node(v_inode);
                if (likely(dir_node)) {
                    nomount_hijack_virtual_parent(dir_node, v_inode);
                    nomount_hijack_dir_inode(dir_node, v_inode);
                    nomount_hijack_superblock(p_path.dentry->d_sb);
                    
                    child_name = v_tmp + 1;
                    child_name_len = strlen(child_name);
                    qname.name = child_name;
                    qname.len = child_name_len;
                    qname.hash = full_name_hash(p_path.dentry, child_name, child_name_len);
                    if (unlikely(p_path.dentry->d_flags & DCACHE_OP_HASH))
                        p_path.dentry->d_op->d_hash(p_path.dentry, &qname);

                    t_rule = (p_count == 0) ? rule : pending_rules[p_count - 1];
                    __nomount_inject_child_locked(dir_node, t_rule, child_name, child_name_len);
                }
                path_put(&p_path);
            }
            break;
        }

        *slash_v = '\0';
        slashes_v[p_count] = slash_v;
        cur_v_len = slash_v - v_tmp;

        if (slash_r) {
            *slash_r = '\0';
            slashes_r[p_count] = slash_r;
            cur_r_len = slash_r - r_tmp;
        } else {
            slashes_r[p_count] = NULL;
        }

        pending_rules[p_count] = NULL; 
        p_count++;
        
        h_inter = full_name_hash(NULL, v_tmp, cur_v_len);
        inter_exists = false;

        hash_for_each_possible(nomount_rules_ht, ex, vpath_node, h_inter) {
            if (ex->v_len == cur_v_len && memcmp(nm_get_vpath(ex), v_tmp, cur_v_len) == 0) {
                inter_exists = true;
                break;
            }
        }

        if (inter_exists) {
            dir_node = ex->this_dir;
            if (!dir_node) { dir_node = __nomount_alloc_dir_node(NULL); ex->this_dir = dir_node; }
            child_name = slash_v + 1;
            child_name_len = strlen(child_name);
            t_rule = (p_count == 1) ? rule : pending_rules[p_count - 2];
            __nomount_inject_child_locked(dir_node, t_rule, child_name, child_name_len);
            break;
        }

        if (likely(kern_path(v_tmp, LOOKUP_FOLLOW, &p_path) == 0)) {
            struct inode *v_inode = d_backing_inode(p_path.dentry);
            dir_node = nomount_get_dir_node(v_inode);
            if (!dir_node) dir_node = __nomount_alloc_dir_node(v_inode);
            if (likely(dir_node)) {
                nomount_hijack_virtual_parent(dir_node, v_inode);
                nomount_hijack_dir_inode(dir_node, v_inode);
                nomount_hijack_superblock(p_path.dentry->d_sb);
            }
            child_name = slash_v + 1;
            child_name_len = strlen(child_name);

            qname.name = child_name;
            qname.len = child_name_len;
            qname.hash = full_name_hash(p_path.dentry, qname.name, qname.len);
            if (p_path.dentry->d_flags & DCACHE_OP_HASH)
                p_path.dentry->d_op->d_hash(p_path.dentry, &qname);

            found_ghost = d_lookup(p_path.dentry, &qname);
            if (found_ghost) {
                d_drop(found_ghost);
                dput(found_ghost);
                nm_debug("Cleaned cached dentry for: %s\n", child_name);
            }

            t_rule = (p_count == 1) ? rule : pending_rules[p_count - 2];
            __nomount_inject_child_locked(dir_node, t_rule, child_name, child_name_len);
            path_put(&p_path);
            break; 
        } else {
            size_t req_r_len = slash_r ? cur_r_len : 1;
            size_t i_size = sizeof(struct nomount_rule) + cur_v_len + 1 + req_r_len + 1;

            pending_rules[p_count - 1] = kzalloc(i_size, GFP_KERNEL);
            if (unlikely(!pending_rules[p_count - 1])) {
                err = -ENOMEM;
                break;
            }
            irule = pending_rules[p_count - 1];
            INIT_HLIST_NODE(&irule->vpath_node);
            irule->v_len = (u16)cur_v_len;
            irule->v_hash = h_inter;
            irule->flags = NM_FLAG_IS_DIR;
            irule->v_ino = (unsigned long)h_inter;

            memcpy(nm_get_vpath(irule), v_tmp, cur_v_len);
            nm_get_vpath(irule)[cur_v_len] = '\0';
            
            if (slash_r) {
                memcpy(nm_get_rpath(irule), r_tmp, cur_r_len);
                nm_get_rpath(irule)[cur_r_len] = '\0';
            } else {
                nm_get_rpath(irule)[0] = '/';
                nm_get_rpath(irule)[1] = '\0';
            }

            if (kern_path(nm_get_rpath(irule), LOOKUP_FOLLOW, &inter_r_path) == 0) {
                irule->cached_r_inode = igrab(d_backing_inode(inter_r_path.dentry));
                nomount_hijack_superblock(inter_r_path.dentry->d_sb);
                path_put(&inter_r_path);
            } else if (kern_path("/", LOOKUP_FOLLOW, &inter_r_path) == 0) {
                irule->cached_r_inode = igrab(d_backing_inode(inter_r_path.dentry));
                nomount_hijack_superblock(inter_r_path.dentry->d_sb);
                path_put(&inter_r_path);
            }
        }
    }

    while (p_count > 0) {
        p_count--;
        if (slashes_v[p_count]) *slashes_v[p_count] = '/';
        if (slashes_r[p_count]) *slashes_r[p_count] = '/';

        if (pending_rules[p_count]) {
            irule = pending_rules[p_count];
            if (likely(err == 0)) {
                hash_add_rcu(nomount_rules_ht, &irule->vpath_node, irule->v_hash);
                atomic_inc(&nm_active_rules);
                if (atomic_read(&nm_active_rules) == 1) static_branch_enable(&nomount_active_rules);
            } else {
                if (irule->cached_r_inode) iput(irule->cached_r_inode);
                kfree(irule);
            }
        }
    }

    return err;
}

/*** Rule Operations ***/

static int __nomount_add_rule(const char *v_path, const char *r_path, u16 v_len, u16 r_len, u32 flags)
{
    struct nomount_rule *rule = NULL, *existing, *victim = NULL;
    struct path r_path_struct_main;
    u32 hash;
    int err = 0;
    bool is_whiteout = (flags & NM_FLAG_WHITEOUT);
    size_t total_size;

    if (!v_path || (!r_path && !is_whiteout)) return -EINVAL;

    hash = full_name_hash(NULL, v_path, v_len);

    if (is_whiteout) r_len = 0;
    total_size = sizeof(struct nomount_rule) + v_len + 1 + r_len + 1;

    rule = kmalloc(total_size, GFP_KERNEL);
    if (!rule) return -ENOMEM;

    rule->v_len = v_len;
    memcpy(nm_get_vpath(rule), v_path, v_len);
    nm_get_vpath(rule)[v_len] = '\0';

    if (is_whiteout) {
        nm_get_rpath(rule)[0] = '\0';
    } else {
        memcpy(nm_get_rpath(rule), r_path, r_len);
        nm_get_rpath(rule)[r_len] = '\0';
    }

    INIT_HLIST_NODE(&rule->vpath_node);
    rule->v_hash = hash;
    rule->v_ino = (unsigned long)hash;
    rule->flags = flags;

    if (!is_whiteout && kern_path(nm_get_rpath(rule), LOOKUP_FOLLOW, &r_path_struct_main) == 0) {
        struct inode *r_inode = d_backing_inode(r_path_struct_main.dentry);
        rule->cached_r_inode = igrab(r_inode);
        if (S_ISDIR(r_inode->i_mode)) rule->flags |= NM_FLAG_IS_DIR;
        path_put(&r_path_struct_main);
    }

    mutex_lock(&nomount_write_mutex);
    if (rule->cached_r_inode)
        nomount_hijack_superblock(rule->cached_r_inode->i_sb);

    hash_for_each_possible(nomount_rules_ht, existing, vpath_node, hash) {
        if (existing->v_hash == hash && existing->v_len == v_len &&
             memcmp(nm_get_vpath(existing), nm_get_vpath(rule), v_len) == 0) {
            hash_del_rcu(&existing->vpath_node);
            atomic_dec(&nm_active_rules);
            victim = existing;
            nm_info("Shadowing existing rule for: %s\n", nm_get_vpath(rule));
            break;
        }
    }

    err = nomount_generate_virtual_topology(rule);
    if (err != 0) {
        mutex_unlock(&nomount_write_mutex);
        kfree(rule); 
        return err;
    }

    hash_add_rcu(nomount_rules_ht, &rule->vpath_node, hash);
    atomic_inc(&nm_active_rules);
    if (atomic_read(&nm_active_rules) == 1) static_branch_enable(&nomount_active_rules);
    mutex_unlock(&nomount_write_mutex);

    if (unlikely(victim)) {
        synchronize_rcu();
        kfree(victim); 
    }

    if (is_whiteout)
        nm_info("Successfully added whiteout rule: %s\n", nm_get_vpath(rule));
    else
        nm_info("Successfully added injection rule: %s -> %s\n", nm_get_vpath(rule), nm_get_rpath(rule));
        
    return 0;
}

static void __nomount_del_rule(const char *v_path, size_t v_len, struct hlist_head *r_victims, struct list_head *d_victims)
{
    struct nomount_rule *rule;
    u32 hash = full_name_hash(NULL, v_path, v_len);

    hash_for_each_possible(nomount_rules_ht, rule, vpath_node, hash) {
        if (rule->v_hash == hash && rule->v_len == v_len &&
             memcmp(nm_get_vpath(rule), v_path, v_len) == 0) {
            hash_del_rcu(&rule->vpath_node);
            atomic_dec(&nm_active_rules);
            if (atomic_read(&nm_active_rules) == 0) static_branch_disable(&nomount_active_rules);
            hlist_add_head(&rule->vpath_node, r_victims);
            if (rule->parent_dir)
                __nomount_delete_child_locked(rule->parent_dir, hash, d_victims);
            break;
        }
    }
}

static void __nomount_clear_all(void)
{
    struct nomount_rule *rule;
    struct nomount_dir_node *dir_node, *tmp_dir;
    struct nomount_uid_node *uid_node;
    struct hlist_node *hlist_tmp;
    struct nm_child_array *array;
    HLIST_HEAD(rule_victims);
    LIST_HEAD(dir_victims);
    HLIST_HEAD(uid_victims);
    int bkt;

    hash_for_each_safe(nomount_rules_ht, bkt, hlist_tmp, rule, vpath_node) {
        hash_del_rcu(&rule->vpath_node);
        hlist_add_head(&rule->vpath_node, &rule_victims);
    }

    hash_for_each_safe(nomount_uid_ht, bkt, hlist_tmp, uid_node, node) {
        hash_del_rcu(&uid_node->node);
        hlist_add_head(&uid_node->node, &uid_victims);
    }

    list_for_each_entry_safe(dir_node, tmp_dir, &nomount_all_dirs_list, list) {
        list_del(&dir_node->list);
        array = rcu_dereference_protected(dir_node->child_array, 1);
        if (array && atomic_dec_and_test(&array->refcnt)) kfree_rcu(array, rcu);
        list_add_tail(&dir_node->list, &dir_victims);
    }

    atomic_set(&nm_active_rules, 0);
    atomic_set(&nm_active_uids, 0);
    static_branch_disable(&nomount_active_rules);
    static_branch_disable(&nomount_active_uids);
    INIT_LIST_HEAD(&nomount_all_dirs_list);
    synchronize_rcu();

    list_for_each_entry_safe(dir_node, tmp_dir, &dir_victims, list) {
        nomount_restore_dir_node(dir_node);
        kmem_cache_free(nm_dir_cachep, dir_node);
    }
    hlist_for_each_entry_safe(rule, hlist_tmp, &rule_victims, vpath_node) {
        nomount_invalidate_dcache(nm_get_vpath(rule));
        if (rule->cached_r_inode) iput(rule->cached_r_inode);
        kfree(rule);
    }
    hlist_for_each_entry_safe(uid_node, hlist_tmp, &uid_victims, node) {
        kmem_cache_free(nm_uid_cachep, uid_node);
    }

    nomount_restore_superblocks();
}


/*** Generic Netlink API ***/

static struct genl_family nomount_genl_family;

static int nomount_genl_add_rule(struct sk_buff *skb, struct genl_info *info)
{
    if (info->attrs[NOMOUNT_ATTR_PAYLOAD]) {
        struct nlattr *attr = info->attrs[NOMOUNT_ATTR_PAYLOAD];
        const char *data = nla_data(attr), *raw_v_ptr, *raw_r_ptr;
        int len = nla_len(attr);
        int pos = 0, err = 0;

        while (pos + 8 <= len) {
            u32 flags = get_unaligned((const u32 *)(data + pos));
            u16 vp_len = get_unaligned((const u16 *)(data + pos + 4));
            u16 rp_len = get_unaligned((const u16 *)(data + pos + 6));
            pos += 8;

            if (pos + vp_len + rp_len > len) break;
            if (unlikely(vp_len >= PATH_MAX || rp_len >= PATH_MAX)) break;

            raw_v_ptr = data + pos; pos += vp_len;
            raw_r_ptr = data + pos;  pos += rp_len;
            err = __nomount_add_rule(raw_v_ptr, raw_r_ptr, vp_len, rp_len, flags);
            if (err) {
                nm_err("Failed to inject rule batch entry (err: %d). Skipping.\n", err);
            }
        }
        return 0;

    } else if (info->attrs[NOMOUNT_ATTR_VIRTUAL_PATH] && info->attrs[NOMOUNT_ATTR_REAL_PATH]) {
        char *v_str = nla_data(info->attrs[NOMOUNT_ATTR_VIRTUAL_PATH]);
        char *r_str = nla_data(info->attrs[NOMOUNT_ATTR_REAL_PATH]);
        u32 flags = info->attrs[NOMOUNT_ATTR_FLAGS] ? nla_get_u32(info->attrs[NOMOUNT_ATTR_FLAGS]) : 0;
        return __nomount_add_rule(v_str, r_str, strlen(v_str), strlen(r_str), flags);
    }

    return -EINVAL;
}

static int nomount_genl_del_rule(struct sk_buff *skb, struct genl_info *info)
{
    struct nomount_rule *rule;
    struct nomount_dir_node *dir_node, *tmp_dir;
    struct hlist_node *tmp_r;
    HLIST_HEAD(r_victims);
    LIST_HEAD(d_victims);

    if (info->attrs[NOMOUNT_ATTR_PAYLOAD]) {
        struct nlattr *attr = info->attrs[NOMOUNT_ATTR_PAYLOAD];
        const char *data = nla_data(attr);
        int len = nla_len(attr);
        int pos = 0;

        mutex_lock(&nomount_write_mutex);
        while (pos + 2 <= len) {
            u16 vp_len = get_unaligned((const u16 *)(data + pos));
            pos += 2; if (pos + vp_len > len) break;
            __nomount_del_rule(data + pos, vp_len, &r_victims, &d_victims);
            pos += vp_len;
        }
        mutex_unlock(&nomount_write_mutex);
    } else if (info->attrs[NOMOUNT_ATTR_VIRTUAL_PATH]) {
        char *v_path = nla_data(info->attrs[NOMOUNT_ATTR_VIRTUAL_PATH]);
        mutex_lock(&nomount_write_mutex);
        __nomount_del_rule(v_path, strlen(v_path), &r_victims, &d_victims);
        mutex_unlock(&nomount_write_mutex);
    } else {
        return -EINVAL;
    }

    if (hlist_empty(&r_victims)) return -ENOENT;
    synchronize_rcu();

    list_for_each_entry_safe(dir_node, tmp_dir, &d_victims, list) {
        nomount_restore_dir_node(dir_node);
        kmem_cache_free(nm_dir_cachep, dir_node);
    }
    hlist_for_each_entry_safe(rule, tmp_r, &r_victims, vpath_node) {
        nm_info("Deleted rule for: %s\n", nm_get_vpath(rule));
        nomount_invalidate_dcache(nm_get_vpath(rule));
        if (rule->cached_r_inode) iput(rule->cached_r_inode);
        kfree(rule);
    }

    return 0;
}

static int nomount_genl_clear_rules(struct sk_buff *skb, struct genl_info *info)
{
    mutex_lock(&nomount_write_mutex);
    __nomount_clear_all();
    mutex_unlock(&nomount_write_mutex);
    nm_info("Cleared all active rules and UIDs\n");
    return 0;
}

static int nomount_genl_dump_rules(struct sk_buff *skb, struct netlink_callback *cb)
{
    struct nomount_rule *rule;
    int start_idx = cb->args[0], bkt, idx = 0;
    void *hdr;

    rcu_read_lock();
    hash_for_each_rcu(nomount_rules_ht, bkt, rule, vpath_node) {
        if (idx < start_idx) { idx++; continue; }

        hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
                          &nomount_genl_family, NLM_F_MULTI, NM_CMD_GET_LIST);
        if (!hdr) break; 

        if (nla_put_string(skb, NOMOUNT_ATTR_VIRTUAL_PATH, nm_get_vpath(rule)) ||
               nla_put_string(skb, NOMOUNT_ATTR_REAL_PATH, nm_get_rpath(rule)) ||
               nla_put_u32(skb, NOMOUNT_ATTR_FLAGS, rule->flags)) {
            genlmsg_cancel(skb, hdr);
            break;
        }
        genlmsg_end(skb, hdr);
        idx++;
    }
    rcu_read_unlock();

    cb->args[0] = idx; 
    return skb->len;
}

static int nomount_genl_add_uid(struct sk_buff *skb, struct genl_info *info)
{
    unsigned int uid;
    struct nomount_uid_node *entry;

    if (!info->attrs[NOMOUNT_ATTR_UID])
        return -EINVAL;

    uid = nla_get_u32(info->attrs[NOMOUNT_ATTR_UID]);

    if (nomount_is_uid_blocked(uid)) 
        return -EEXIST;

    entry = kmem_cache_alloc(nm_uid_cachep, GFP_KERNEL);
    if (!entry) return -ENOMEM;
    entry->uid = uid;
    
    mutex_lock(&nomount_write_mutex);
    hash_add_rcu(nomount_uid_ht, &entry->node, uid);
    atomic_inc(&nm_active_uids);
    if (atomic_read(&nm_active_uids) == 1) static_branch_enable(&nomount_active_uids);
    mutex_unlock(&nomount_write_mutex);
    
    nm_info("Successfully added blocked UID: %u\n", uid);
    return 0;
}

static int nomount_genl_del_uid(struct sk_buff *skb, struct genl_info *info)
{
    unsigned int uid;
    struct nomount_uid_node *entry;
    struct hlist_node *tmp;
    int bkt;
    bool found = false;

    if (!info->attrs[NOMOUNT_ATTR_UID])
        return -EINVAL;

    uid = nla_get_u32(info->attrs[NOMOUNT_ATTR_UID]);

    mutex_lock(&nomount_write_mutex);
    hash_for_each_safe(nomount_uid_ht, bkt, tmp, entry, node) {
        if (entry->uid == uid) {
            hash_del_rcu(&entry->node);
            found = true;
            break; 
        }
    }
    atomic_dec(&nm_active_uids);
    if (atomic_read(&nm_active_uids) == 0) static_branch_disable(&nomount_active_uids);
    mutex_unlock(&nomount_write_mutex);

    if (found && entry) {
        synchronize_rcu();
        kmem_cache_free(nm_uid_cachep, entry);
    }

    nm_info("Successfully remove blocked UID: %u\n", uid);
    return found ? 0 : -ENOENT;
}

static int nomount_genl_get_version(struct sk_buff *skb, struct genl_info *info)
{
    struct sk_buff *msg;
    void *hdr;
    int ret;

    msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
    if (!msg) return -ENOMEM;

    hdr = genlmsg_put_reply(msg, info, &nomount_genl_family, 0, info->genlhdr->cmd);
    if (!hdr) {
        nlmsg_free(msg);
        return -EMSGSIZE;
    }

    ret = nla_put_string(msg, NOMOUNT_ATTR_VERSION, NOMOUNT_VERSION);
    if (ret) {
        genlmsg_cancel(msg, hdr);
        nlmsg_free(msg);
        return ret;
    }

    genlmsg_end(msg, hdr);
    return genlmsg_reply(msg, info);
}

static const struct nla_policy nomount_genl_policy[NOMOUNT_ATTR_MAX + 1] = {
    [NOMOUNT_ATTR_VIRTUAL_PATH] = { .type = NLA_NUL_STRING, .len = PATH_MAX },
    [NOMOUNT_ATTR_REAL_PATH]    = { .type = NLA_NUL_STRING, .len = PATH_MAX },
    [NOMOUNT_ATTR_FLAGS]        = { .type = NLA_U32 },
    [NOMOUNT_ATTR_UID]          = { .type = NLA_U32 },
    [NOMOUNT_ATTR_VERSION]      = { .type = NLA_NUL_STRING },
    [NOMOUNT_ATTR_PAYLOAD]      = { .type = NLA_BINARY },
};

static const struct genl_ops nomount_genl_ops[] = {
    { .cmd = NM_CMD_ADD_RULE, .flags = GENL_ADMIN_PERM, .doit = nomount_genl_add_rule, NM_OPS_POLICY(nomount_genl_policy) },
    { .cmd = NM_CMD_DEL_RULE, .flags = GENL_ADMIN_PERM, .doit = nomount_genl_del_rule, NM_OPS_POLICY(nomount_genl_policy) },
    { .cmd = NM_CMD_CLEAR_ALL, .flags = GENL_ADMIN_PERM, .doit = nomount_genl_clear_rules, NM_OPS_POLICY(nomount_genl_policy) },
    { .cmd = NM_CMD_ADD_UID, .flags = GENL_ADMIN_PERM, .doit = nomount_genl_add_uid, NM_OPS_POLICY(nomount_genl_policy) },
    { .cmd = NM_CMD_DEL_UID, .flags = GENL_ADMIN_PERM, .doit = nomount_genl_del_uid, NM_OPS_POLICY(nomount_genl_policy) },
    { .cmd = NM_CMD_GET_LIST, .flags = GENL_ADMIN_PERM, .dumpit = nomount_genl_dump_rules, NM_OPS_POLICY(nomount_genl_policy) },
    { .cmd = NM_CMD_GET_VERSION, .flags = GENL_ADMIN_PERM, .doit = nomount_genl_get_version, NM_OPS_POLICY(nomount_genl_policy) },
};

static struct genl_family nomount_genl_family = {
    .name = NOMOUNT_GENL_NAME,
    .version = NOMOUNT_GENL_VERSION,
    .maxattr = NOMOUNT_ATTR_MAX,
    NM_FAMILY_POLICY(nomount_genl_policy)
    .netnsok = true,
    .module = THIS_MODULE,
    .ops = nomount_genl_ops,
    .n_ops = ARRAY_SIZE(nomount_genl_ops),
};

static int __init nomount_init(void)
{
    int ret;

    hash_init(nomount_sb_ht);
    hash_init(nomount_rules_ht);
    hash_init(nomount_uid_ht);

    nm_dir_cachep = kmem_cache_create("nm_dirs", sizeof(struct nomount_dir_node), 0, SLAB_HWCACHE_ALIGN, NULL);
    nm_uid_cachep = kmem_cache_create("nm_uids", sizeof(struct nomount_uid_node), 0, SLAB_HWCACHE_ALIGN, NULL);

    if (!nm_dir_cachep || !nm_uid_cachep) {
        nm_err("Failed to allocate memory slab caches\n");
        if (nm_dir_cachep) kmem_cache_destroy(nm_dir_cachep);
        if (nm_uid_cachep) kmem_cache_destroy(nm_uid_cachep);
        return -ENOMEM;
    }

    ret = genl_register_family(&nomount_genl_family);
    if (ret) {
        nm_err("Failed to register Generic Netlink family (err: %d)\n", ret);
        kmem_cache_destroy(nm_dir_cachep);
        kmem_cache_destroy(nm_uid_cachep);
        return ret;
    }

    nm_info("Loaded successfully\n");
    return 0;
}

static void __exit nomount_exit(void)
{
    genl_unregister_family(&nomount_genl_family);

    mutex_lock(&nomount_write_mutex);
    __nomount_clear_all();
    mutex_unlock(&nomount_write_mutex);

    kmem_cache_destroy(nm_dir_cachep);
    kmem_cache_destroy(nm_uid_cachep);

    nm_info("Unloaded successfully\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION(NOMOUNT_VERSION);
MODULE_AUTHOR("maxsteeel");
MODULE_DESCRIPTION("NoMount Path Redirection VFS Subsystem");

fs_initcall(nomount_init);
module_exit(nomount_exit);
