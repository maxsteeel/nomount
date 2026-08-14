#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/xattr.h>
#include <linux/module.h>
#include "nomount.h"

static struct kmem_cache *nm_dir_cachep __read_mostly, *nm_inode_cachep __read_mostly;
static struct kmem_cache *nm_iop_cachep __read_mostly, *nm_fop_cachep __read_mostly;
static DEFINE_STATIC_KEY_FALSE(nomount_active_uids);

/*** Helpers ***/

static __always_inline bool nomount_is_uid_blocked(uid_t uid)
{
    bool is_blocked;
    if (!static_branch_unlikely(&nomount_active_uids)) return false;
    rcu_read_lock();
    is_blocked = (idr_find(&nomount_uid_idr, uid) != NULL);
    rcu_read_unlock();
    return is_blocked;
}

#define __get_nm(ptr, type, member, field, hook_func) ({ \
    typeof(ptr) __p = (ptr); \
    (likely(__p) && __p->field == (hook_func)) ? container_of(__p, type, member) : NULL; \
})

static __always_inline struct nomount_dir_node *nomount_get_dir_node(struct inode *inode) 
{
    struct nm_iop *nm_iop;
    struct nm_fop *nm_fop;

    nm_iop = __get_nm(smp_load_acquire(&inode->i_op), struct nm_iop, fake_iop, lookup, nomount_hijacked_lookup);
    if (nm_iop && nm_iop->dir_node) return nm_iop->dir_node;

    nm_fop = __get_nm(smp_load_acquire(&inode->i_fop), struct nm_fop, fake_fop, iterate_shared, nomount_hijacked_iterate_dir);
    if (nm_fop && nm_fop->dir_node) return nm_fop->dir_node;
    
    return NULL;
}

static __always_inline struct nomount_child_node *nomount_bsearch_child(struct nomount_child_array *arr, const char *name, size_t len, u32 hash)
{
    int l = 0, r = arr->count;
    u32 *hashes = arr->hashes;

    if (unlikely(r <= 0)) return NULL;

    __builtin_prefetch(&hashes[r >> 1], 0, 3);
    while (l < r) {
        int m = l + ((r - l) >> 1);
        if (m + 1 < r) __builtin_prefetch(&hashes[(m + 1 + r) >> 1], 0, 1);
        if (l < m) __builtin_prefetch(&hashes[(l + m) >> 1], 0, 1);
        if (hashes[m] < hash) l = m + 1;
        else r = m;
    }

    while (l < arr->count) {
        struct nomount_child_node *c = arr->nodes[l];
        if (unlikely(hashes[l] != hash)) break;
        if (c->name_len == len && !memcmp(c->name, name, len)) return c;
        l++;
    }

    return NULL;
}

static __always_inline bool nomount_get_rule_info(struct nomount_dir_node *dir_node, const char *name, size_t len, u32 hash, struct nm_rule_info *rule_info, bool get_path)
{
    struct nomount_child_array *arr;
    struct nomount_child_node *c;
    unsigned int seq;
    bool found = false;
    if (unlikely(!dir_node)) return false;

    rcu_read_lock();
    do {
        seq = read_seqcount_begin(&dir_node->seq);
        arr = rcu_dereference(dir_node->children);
        if (likely(arr)) {
            c = nomount_bsearch_child(arr, name, len, hash);
            if (c && c->rule && (c->rule->target_uid == 0 || c->rule->target_uid == current_uid().val)) {
                rule_info->flags = c->rule->flags;
                rule_info->v_ino = c->rule->v_ino;
                rule_info->this_dir = c->rule->this_dir;
                if (get_path && c->rule->r_path.dentry) {
                    rule_info->r_path = c->rule->r_path;
                } else {
                    rule_info->r_path.dentry = NULL;
                }
                found = true;
            } else {
                found = false;
            }
        }
    } while (read_seqcount_retry(&dir_node->seq, seq));

    if (found && get_path && rule_info->r_path.dentry) path_get(&rule_info->r_path);
    rcu_read_unlock();
    return found;
}

#define NM_DEFINE_RCU_FREE(_name, _type, _cache, ...) \
static void _name(struct rcu_head *head) { \
    _type *obj = container_of(head, _type, rcu); \
    __VA_ARGS__ \
    kmem_cache_free(_cache, obj); \
}
NM_DEFINE_RCU_FREE(nm_iop_rcu_free, struct nm_iop, nm_iop_cachep)
NM_DEFINE_RCU_FREE(nm_fop_rcu_free, struct nm_fop, nm_fop_cachep)

static void nm_dir_rcu_free(struct rcu_head *head)
{
    struct nomount_dir_node *dir = container_of(head, struct nomount_dir_node, rcu);
    struct nomount_child_array *arr = dir->children;
    if (arr) {
        int i; for (i = 0; i < arr->count; i++) kfree(arr->nodes[i]);
        kfree(arr);
    }
    kmem_cache_free(nm_dir_cachep, dir);
}

static void nm_child_array_rcu_free(struct rcu_head *head)
{
    struct nomount_child_array *arr = container_of(head, struct nomount_child_array, rcu);
    kfree(arr);
}

struct nomount_proxy_ctx {
    struct dir_context ctx;
    struct dir_context *orig_ctx;
    struct nomount_dir_node *dir_node;
    int emitted;
};

static NM_ACTOR_RET nomount_actor_proxy(struct dir_context *ctx, const char *name, int namelen,
                                        loff_t offset, u64 ino, unsigned int d_type)
{
    struct nomount_proxy_ctx *proxy = container_of(ctx, struct nomount_proxy_ctx, ctx);
    struct nomount_child_array *arr;
    struct nomount_child_node *c;
    NM_ACTOR_RET ret;
    unsigned int seq;
    bool is_injected = false;
    u32 hash;

    if (!proxy->dir_node) goto do_real_actor;
    hash = full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, name, namelen);
    if (!(proxy->dir_node->bloom_mask & (1ULL << (hash & 63)))) goto do_real_actor;

    rcu_read_lock();
    do {
        seq = read_seqcount_begin(&proxy->dir_node->seq);
        arr = rcu_dereference(proxy->dir_node->children);
        if (likely(arr)) {
            c = nomount_bsearch_child(arr, name, namelen, hash);
            if (c && c->rule && (c->rule->target_uid == 0 || c->rule->target_uid == current_uid().val)) {
                is_injected = true;
            } else {
                is_injected = false;
            }
        }
    } while (read_seqcount_retry(&proxy->dir_node->seq, seq));
    rcu_read_unlock();

    if (is_injected) {
        proxy->ctx.pos = offset;
        return NM_ACTOR_CONTINUE;
    }

do_real_actor:
    proxy->orig_ctx->pos = proxy->ctx.pos;
    ret = proxy->orig_ctx->actor(proxy->orig_ctx, name, namelen, offset, ino, d_type);
    proxy->ctx.pos = proxy->orig_ctx->pos;
    proxy->emitted++;

    return ret;
}

static inline void nomount_emit_virtual_children(struct dir_context *ctx, struct nomount_dir_node *dir_node)
{
    struct nomount_child_array *array;
    int id;

    if (!dir_node) return;
    if (!nm_is_virtual_pos(ctx->pos)) ctx->pos = nm_pack_pos(0);
    id = nm_unpack_pos(ctx->pos);

    down_read(&nomount_rwsem);
    array = dir_node->children; 
    if (array) {
        for (; id < array->count; id++) {
            struct nomount_child_node *child = array->nodes[id];
            ctx->pos = nm_pack_pos(id);
            if (child->rule->target_uid == 0 || child->rule->target_uid == current_uid().val) {
                if (!(child->flags & NM_FLAG_WHITEOUT) &&
                    !dir_emit(ctx, child->name, child->name_len, child->fake_ino, child->d_type)) break;
            }
            ctx->pos = nm_pack_pos(id + 1);
        }
    }
    up_read(&nomount_rwsem);
}

static struct inode *nomount_create_new_inode(struct super_block *virtual_sb, struct nm_rule_info *rule_info)
{
    struct inode *inode;
    struct nm_inode_info *info;

    inode = new_inode(virtual_sb);
    if (unlikely(!inode)) return NULL;

    info = kmem_cache_alloc(nm_inode_cachep, GFP_KERNEL);
    if (unlikely(!info)) {
        iput(inode);
        return NULL;
    }

    info->flags = rule_info->flags;
    info->dir_node = rule_info->this_dir;
    if (info->dir_node) WRITE_ONCE(info->dir_node->v_inode, inode);

    if (rule_info->flags & NM_FLAG_VIRTUAL_DIR) {
        info->r_path.dentry = NULL;
        info->r_path.mnt = NULL;
    } else if (rule_info->r_path.dentry) {
        info->r_path = rule_info->r_path;
    } else {
        info->r_path.dentry = NULL;
        info->r_path.mnt = NULL;
    }

    info->v_ino = rule_info->v_ino;
    inode->i_private = info;
    inode->i_ino = rule_info->v_ino;
    if (rule_info->flags & NM_FLAG_VIRTUAL_DIR) {
        inode->i_mode = S_IFDIR | 0755;
        inode->i_size = 4096;
        inode->i_blocks = 8;
        inode->i_uid = GLOBAL_ROOT_UID;
        inode->i_gid = GLOBAL_ROOT_GID;
        inode->i_op = &nm_dir_iops;
        inode->i_fop = &nm_dir_fops;
    } else if (rule_info->r_path.dentry) {
        struct inode *real_inode = d_backing_inode(rule_info->r_path.dentry);
        inode->i_mode = real_inode->i_mode;
        inode->i_size = i_size_read(real_inode);
        inode->i_blocks = real_inode->i_blocks;
        inode->i_uid = real_inode->i_uid;
        inode->i_gid = real_inode->i_gid;
        nm_sync_inode_times(inode, real_inode);
        if (S_ISDIR(real_inode->i_mode)) {
            inode->i_op = &nm_dir_iops;
            inode->i_fop = &nm_dir_fops;
        } else {
            inode->i_op = &nm_file_iops;
        #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
            if (!S_ISLNK(real_inode->i_mode) && real_inode->i_fop && real_inode->i_fop->mmap_prepare)
                inode->i_fop = &nm_file_fops_mmap_prepare;
            else
        #endif
                inode->i_fop = &nm_file_fops;
        }
        inode->i_mapping = real_inode->i_mapping;
    }

    inode->i_flags |= S_PRIVATE | S_NOATIME | S_NOCMTIME | S_NOSEC;
    inode->i_opflags |= IOP_XATTR;
    if (!S_ISLNK(inode->i_mode)) inode->i_opflags |= IOP_NOFOLLOW;

    return inode;
}

/*** i_op / s_op / f_op Hijacking Hooks ***/

static struct dentry *nomount_hijacked_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct nm_iop *nm_iop = __get_nm(smp_load_acquire(&dir->i_op), struct nm_iop, fake_iop, lookup, nomount_hijacked_lookup);
    struct nomount_dir_node *dir_node = nm_iop ? READ_ONCE(nm_iop->dir_node) : NULL;
    struct nm_rule_info rule_info;
    const char *name = dentry->d_name.name;
    size_t len = dentry->d_name.len;
    struct dentry *res;

    if (unlikely(!nm_iop || !dir_node))
        goto do_real_lookup;

    if (nomount_get_rule_info(dir_node, name, len, full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, name, len), &rule_info, true)) {
        if (nomount_is_uid_blocked(current_uid().val)) {
            if (rule_info.r_path.dentry) path_put(&rule_info.r_path);
            if (d_is_negative(dentry)) d_drop(dentry);
            if (nm_iop->orig_iop->lookup) {
                res = nm_iop->orig_iop->lookup(dir, dentry, flags);
                if (!IS_ERR(res)) nomount_hijack_dentry_ops(res ? res : dentry);
                return res;
            }
            return ERR_PTR(-EOPNOTSUPP);
        }

        if (rule_info.flags & NM_FLAG_WHITEOUT) {
            nomount_hijack_dentry_ops(dentry);
            d_add(dentry, NULL); 
            if (rule_info.r_path.dentry) path_put(&rule_info.r_path);
            return NULL;
        }

        if ((rule_info.flags & NM_FLAG_VIRTUAL_DIR) || rule_info.r_path.dentry) {
            struct inode *new_inode = nomount_create_new_inode(dir->i_sb, &rule_info);
            if (likely(new_inode)) {
                res = d_splice_alias(new_inode, dentry);
                if (!IS_ERR(res)) nomount_hijack_dentry_ops(res ? res : dentry);
                return res;
            }
        }
        if (rule_info.r_path.dentry) path_put(&rule_info.r_path);
    }

do_real_lookup:
    if (nm_iop && nm_iop->orig_iop && nm_iop->orig_iop->lookup) {
        return nm_iop->orig_iop->lookup(dir, dentry, flags);
    }
    return ERR_PTR(-EOPNOTSUPP);
}

static int nomount_hijacked_iterate_dir(struct file *file, struct dir_context *ctx)
{
    struct nm_fop *nm_fop = __get_nm(smp_load_acquire(&file->f_op), struct nm_fop, fake_fop, iterate_shared, nomount_hijacked_iterate_dir);
    struct nomount_dir_node *dir_node = nm_fop ? READ_ONCE(nm_fop->dir_node) : NULL;
    const struct file_operations *orig_fop = nm_fop ? nm_fop->orig_fop : NULL;
    struct nomount_proxy_ctx proxy_ctx = {
        .ctx.actor = nomount_actor_proxy,
    };
    int res = 0;

    if (unlikely(nomount_is_uid_blocked(current_uid().val) || !orig_fop || !dir_node))
        goto do_real_iterate;

    if (unlikely(nm_is_virtual_pos(ctx->pos))) {
        nomount_emit_virtual_children(ctx, dir_node);
        return 0;
    }

    proxy_ctx.ctx.pos = ctx->pos;
    proxy_ctx.orig_ctx = ctx;
    proxy_ctx.dir_node = dir_node;
    proxy_ctx.emitted = 0;

    res = nm_call_iterate(file, &proxy_ctx.ctx, orig_fop);
    ctx->pos = proxy_ctx.ctx.pos;
    if (res < 0 || proxy_ctx.emitted > 0) return res;

    ctx->pos = nm_pack_pos(0);
    nomount_emit_virtual_children(ctx, dir_node);
    return res;

do_real_iterate:
    if (orig_fop) return nm_call_iterate(file, ctx, orig_fop);
    return -ENOTDIR;
}

static void nomount_hijacked_destroy_inode(struct inode *inode)
{
    struct nm_sop *nm_sop;
    if (inode->i_op == &nm_file_iops || inode->i_op == &nm_dir_iops) {
        if (inode->i_private) {
            struct nm_inode_info *info = inode->i_private;
            if (info->r_path.dentry) path_put(&info->r_path);
            if (info->dir_node) {
                WRITE_ONCE(info->dir_node->v_inode, NULL);
                if (READ_ONCE(info->dir_node->_tag_ptr) == 1UL)
                    call_rcu(&info->dir_node->rcu, nm_dir_rcu_free);
            }
            kmem_cache_free(nm_inode_cachep, info);
            inode->i_private = NULL;
        }
    } else {
        struct nm_iop *nm_iop = __get_nm(inode->i_op, struct nm_iop, fake_iop, lookup, nomount_hijacked_lookup);
        struct nm_fop *nm_fop = __get_nm(inode->i_fop, struct nm_fop, fake_fop, iterate_shared, nomount_hijacked_iterate_dir);
        struct nomount_dir_node *dir_node = NULL;
        if (nm_iop) {
            dir_node = nm_iop->dir_node;
            call_rcu(&nm_iop->rcu, nm_iop_rcu_free);
        }
        if (nm_fop) {
            if (!dir_node) dir_node = nm_fop->dir_node;
            call_rcu(&nm_fop->rcu, nm_fop_rcu_free);
        }
        if (dir_node && !(dir_node->_tag_ptr & 1UL))
            call_rcu(&dir_node->rcu, nm_dir_rcu_free);
    }

    nm_sop = __get_nm(smp_load_acquire(&inode->i_sb->s_op), struct nm_sop, fake_sop, destroy_inode, nomount_hijacked_destroy_inode);
    if (nm_sop && nm_sop->orig_sop && nm_sop->orig_sop->destroy_inode) {
        nm_sop->orig_sop->destroy_inode(inode);
    }
}

static int nomount_hijacked_drop_inode(struct inode *inode)
{
    struct nm_sop *nm_sop;
    if (inode->i_op == &nm_file_iops || inode->i_op == &nm_dir_iops) {
        return !inode->i_nlink || inode_unhashed(inode);
    }

    nm_sop = __get_nm(smp_load_acquire(&inode->i_sb->s_op), struct nm_sop, fake_sop, drop_inode, nomount_hijacked_drop_inode);
    if (nm_sop && nm_sop->orig_sop && nm_sop->orig_sop->drop_inode) {
        return nm_sop->orig_sop->drop_inode(inode);
    }
    
    return !inode->i_nlink || inode_unhashed(inode);
}

static void nomount_hijacked_evict_inode(struct inode *inode)
{
    struct nm_sop *nm_sop;
    if (inode->i_op == &nm_file_iops || inode->i_op == &nm_dir_iops) {
        truncate_inode_pages_final(&inode->i_data);
        clear_inode(inode);
        return;
    }
    nm_sop = __get_nm(smp_load_acquire(&inode->i_sb->s_op), struct nm_sop, fake_sop, evict_inode, nomount_hijacked_evict_inode);
    if (nm_sop && nm_sop->orig_sop && nm_sop->orig_sop->evict_inode) {
        nm_sop->orig_sop->evict_inode(inode);
    } else {
        truncate_inode_pages_final(&inode->i_data);
        clear_inode(inode);
    }
}

/*** file / inode / superblock operations ***/

static int nm_open(struct inode *inode, struct file *file)
{
    struct nm_inode_info *info = inode->i_private;
    struct file *real_file;

    if (unlikely(!info)) return -ENODEV;
    if (unlikely(info->flags & NM_FLAG_VIRTUAL_DIR)) {
        file->private_data = NULL;
        return 0;
    }
    if (unlikely(!info->r_path.dentry)) return -ENODEV;

    real_file = dentry_open(&info->r_path, file->f_flags, file->f_cred);
    if (IS_ERR(real_file)) return PTR_ERR(real_file);

    file->private_data = real_file;
    return 0;
}

static int nm_release(struct inode *inode, struct file *file)
{
    struct file *real_file = file->private_data;
    if (real_file) {
        fput(real_file);
        file->private_data = NULL;
    }
    return 0;
}

static loff_t nm_llseek(struct file *file, loff_t offset, int whence)
{
    struct file *real_file = file->private_data;
    loff_t res;
    if (!real_file) return -EINVAL;

    real_file->f_pos = file->f_pos;
    res = vfs_llseek(real_file, offset, whence);
    file->f_pos = real_file->f_pos;

    return res;
}

static ssize_t nm_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct file *real_file = file->private_data;
    ssize_t ret;
    if (!real_file || !real_file->f_op->read_iter) return -EINVAL;

    iocb->ki_filp = real_file;
    ret = real_file->f_op->read_iter(iocb, to);
    iocb->ki_filp = file;

    return ret;
}

static ssize_t nm_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct file *real_file = file->private_data;
    ssize_t ret;
    if (!real_file || !real_file->f_op->write_iter) return -EINVAL;

    iocb->ki_filp = real_file;
    ret = real_file->f_op->write_iter(iocb, from);
    iocb->ki_filp = file;

    return ret;
}

static int nm_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct file *real_file = file->private_data;
    int ret;
    if (!real_file || !real_file->f_op->mmap) return -ENODEV;

    vma->vm_file = real_file;
    ret = real_file->f_op->mmap(real_file, vma);
    if (ret == 0 && vma->vm_file == real_file) vma->vm_file = file;
    if (ret == 0) file_inode(file)->i_flags &= ~S_PRIVATE;
    return ret;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static int nm_mmap_prepare(struct vm_area_desc *desc)
{
    struct file *file = desc->file;
    struct file *real_file = file->private_data;
    int ret;
    if (!real_file || !real_file->f_op->mmap_prepare) return -ENODEV;

    *(struct file **)&desc->file = real_file;
    ret = real_file->f_op->mmap_prepare(desc);
    if (ret == 0 && desc->file == real_file) *(struct file **)&desc->file = file;
    if (ret == 0) file_inode(file)->i_flags &= ~S_PRIVATE;
    return ret;
}
#endif

static long nm_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct file *real_file = file->private_data;
    if (!real_file || !real_file->f_op->unlocked_ioctl) return -ENOTTY;
    return real_file->f_op->unlocked_ioctl(real_file, cmd, arg);
}

#ifdef CONFIG_COMPAT
static long nm_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct file *real_file = file->private_data;
    if (!real_file || !real_file->f_op->compat_ioctl) return -ENOTTY;
    return real_file->f_op->compat_ioctl(real_file, cmd, arg);
}
#endif

static ssize_t nm_splice_read(struct file *in, loff_t *ppos, struct pipe_inode_info *pipe,
                              size_t len, unsigned int flags)
{
    struct file *real_file = in->private_data;
    if (!real_file || !real_file->f_op->splice_read) return -EINVAL;
    return real_file->f_op->splice_read(real_file, ppos, pipe, len, flags);
}

static ssize_t nm_splice_write(struct pipe_inode_info *pipe, struct file *out,
                               loff_t *ppos, size_t len, unsigned int flags)
{
    struct file *real_file = out->private_data;
    if (!real_file || !real_file->f_op->splice_write) return -EINVAL;
    return real_file->f_op->splice_write(pipe, real_file, ppos, len, flags);
}

static int nm_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
    struct file *real_file = file->private_data;
    if (!real_file || !real_file->f_op->fsync) return -EINVAL;
    return real_file->f_op->fsync(real_file, start, end, datasync);
}

static ssize_t nm_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
    struct nm_inode_info *info = d_backing_inode(dentry)->i_private;
    if (unlikely(!info || (info->flags & NM_FLAG_VIRTUAL_DIR) || !d_backing_inode(info->r_path.dentry)->i_op->listxattr))
        return -EOPNOTSUPP;

    return d_backing_inode(info->r_path.dentry)->i_op->listxattr(info->r_path.dentry, buffer, size);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
static int nm_file_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
#else
static int nm_file_getattr(IDMAP_ARG const struct path *path, struct kstat *stat, u32 request_mask, unsigned int query_flags)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
    struct dentry *dentry = path->dentry;
#endif
    struct inode *v_inode = d_backing_inode(dentry);
    struct nm_inode_info *info = v_inode->i_private;
    int res;
    if (unlikely(!info)) return -EIO;

    if (unlikely(info->flags & NM_FLAG_VIRTUAL_DIR)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
        generic_fillattr(IDMAP_CALL request_mask, v_inode, stat);
#else
        generic_fillattr(IDMAP_CALL v_inode, stat);
#endif
        stat->ino = info->v_ino;
        stat->dev = v_inode->i_sb->s_dev;
        return 0;
    }

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
    res = vfs_getattr_nosec(&info->r_path, stat);
#else
    res = vfs_getattr_nosec(&info->r_path, stat, request_mask, query_flags);
#endif
    if (likely(res == 0)) {
        stat->ino = info->v_ino;
        stat->dev = v_inode->i_sb->s_dev;
    }
    return res;
}

static int nm_setattr(IDMAP_ARG struct dentry *dentry, struct iattr *attr)
{
    struct inode *v_inode = d_inode(dentry);
    struct nm_inode_info *info = v_inode->i_private;
    int err;

    if (unlikely(!info)) return -EIO;
    if (info->flags & NM_FLAG_VIRTUAL_DIR) return 0;

    inode_lock(d_backing_inode(info->r_path.dentry));
    err = notify_change(IDMAP_CALL info->r_path.dentry, attr, NULL);
    inode_unlock(d_backing_inode(info->r_path.dentry));

    if (likely(!err)) {
        if (attr->ia_valid & ATTR_MODE) v_inode->i_mode = d_backing_inode(info->r_path.dentry)->i_mode;
        if (attr->ia_valid & ATTR_UID)  v_inode->i_uid = d_backing_inode(info->r_path.dentry)->i_uid;
        if (attr->ia_valid & ATTR_GID)  v_inode->i_gid = d_backing_inode(info->r_path.dentry)->i_gid;
        nm_sync_inode_times(v_inode, d_backing_inode(info->r_path.dentry));
    }
    return err;
}

static const char *nm_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done)
{
    struct nm_inode_info *info = inode->i_private;
    struct inode *real_inode;
    struct dentry *target_dentry;
    if (unlikely(!info || !info->r_path.dentry)) return ERR_PTR(-ECHILD);

    real_inode = d_backing_inode(info->r_path.dentry);
    target_dentry = dentry ? info->r_path.dentry : NULL;
    if (real_inode && real_inode->i_op && real_inode->i_op->get_link) {
        return real_inode->i_op->get_link(target_dentry, real_inode, done);
    }

    return ERR_PTR(-EINVAL);
}

static int nm_dir_iterate_dir(struct file *file, struct dir_context *ctx)
{
    struct nm_inode_info *info = file_inode(file)->i_private;
    struct nomount_dir_node *dir_node = info ? info->dir_node : NULL;
    struct file *real_file = file->private_data;
    int res = 0;

    if (unlikely(nm_is_virtual_pos(ctx->pos))) {
        nomount_emit_virtual_children(ctx, dir_node);
        return 0;
    }

    if (real_file) {
        struct nomount_proxy_ctx proxy_ctx = {
            .ctx.actor = nomount_actor_proxy, .ctx.pos = ctx->pos,
            .orig_ctx = ctx, .dir_node = dir_node, .emitted = 0
        };
        res = nm_call_iterate(real_file, &proxy_ctx.ctx, real_file->f_op);
        ctx->pos = proxy_ctx.ctx.pos;
        if (res < 0 || proxy_ctx.emitted > 0) return res;
        ctx->pos = nm_pack_pos(0);
    } else if (info && (info->flags & NM_FLAG_VIRTUAL_DIR)) {
        if (ctx->pos < 2 && !dir_emit_dots(file, ctx)) return 0;
        ctx->pos = nm_pack_pos(0);
    } else {
        return -ENOTDIR;
    }

    nomount_emit_virtual_children(ctx, dir_node);
    return res;
}

static struct dentry *nm_dir_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct inode *r_dir = nm_get_real_inode(dir);
    struct nm_inode_info *info = dir->i_private;
    const char *name = dentry->d_name.name;
    size_t len = dentry->d_name.len;
    struct nm_rule_info rule_info;
    struct dentry *res;

    if (info && info->dir_node) {
        u32 v_hash = full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, name, len);
        if (nomount_get_rule_info(info->dir_node, name, len, v_hash, &rule_info, true)) {
            if (rule_info.flags & NM_FLAG_WHITEOUT) {
                nomount_hijack_dentry_ops(dentry);
                d_add(dentry, NULL);
                if (rule_info.r_path.dentry) path_put(&rule_info.r_path);
                return NULL;
            }
            if ((rule_info.flags & NM_FLAG_VIRTUAL_DIR) || rule_info.r_path.dentry) {
                struct inode *new_inode = nomount_create_new_inode(dir->i_sb, &rule_info);
                if (new_inode) {
                    nomount_hijack_dentry_ops(dentry);
                    res = d_splice_alias(new_inode, dentry);
                    if (!IS_ERR(res) && res) nomount_hijack_dentry_ops(res ? res : dentry);
                    return res;
                }
            }
            if (rule_info.r_path.dentry) path_put(&rule_info.r_path);
        }
    }

    if (r_dir && r_dir->i_op && r_dir->i_op->lookup)
        return r_dir->i_op->lookup(r_dir, dentry, flags);

    if (info && (info->flags & NM_FLAG_VIRTUAL_DIR)) {
        nomount_hijack_dentry_ops(dentry);
        d_add(dentry, NULL);
        return NULL;
    }
    return ERR_PTR(-EOPNOTSUPP);
}

struct nm_xattr_proxy {
    struct xattr_handler fake;
    const struct xattr_handler *orig;
};

static int nm_xattr_get(const struct xattr_handler *handler, struct dentry *dentry, struct inode *inode, const char *name, void *buffer, size_t size FLAGS_ARG)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    if (inode->i_op == &nm_file_iops || inode->i_op == &nm_dir_iops) {
        struct nm_inode_info *info = inode->i_private;
        const char *full_name = xattr_full_name(handler, name);
        if (unlikely(!info || !info->r_path.dentry)) return -ENODATA;
        return __vfs_getxattr(info->r_path.dentry, d_inode(info->r_path.dentry), full_name, buffer, size FLAGS_VAL);
    }

    return proxy->orig->get(proxy->orig, dentry, inode, name, buffer, size FLAGS_VAL);
}

static int nm_xattr_set(const struct xattr_handler *handler, IDMAP_ARG struct dentry *dentry, struct inode *inode, const char *name, const void *buffer, size_t size, int flags)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    if (inode->i_op == &nm_file_iops || inode->i_op == &nm_dir_iops) {
        struct nm_inode_info *info = inode->i_private;
        const char *full_name = xattr_full_name(handler, name);
        if (unlikely(!info || !info->r_path.dentry)) return -ENODATA;
        return __vfs_setxattr(IDMAP_PATH(info->r_path) info->r_path.dentry, d_inode(info->r_path.dentry), full_name, buffer, size, flags);
    }
    return proxy->orig->set(proxy->orig, IDMAP_CALL dentry, inode, name, buffer, size, flags);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
static int nm_d_revalidate(struct inode *inode, const struct qstr *name, struct dentry *dentry, unsigned int flags)
#else
static int nm_d_revalidate(struct dentry *dentry, unsigned int flags)
#endif
{
    struct inode *parent_inode;
    struct nm_iop *nm_iop;
    struct nomount_dir_node *parent_dir = NULL;
    struct nm_rule_info rule_info;
    bool injected;
    u32 hash;

    injected = dentry->d_inode &&
               (dentry->d_inode->i_op == &nm_file_iops || dentry->d_inode->i_op == &nm_dir_iops);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
    parent_inode = inode;
#else
    parent_inode = d_inode(READ_ONCE(dentry->d_parent));
#endif
    if (!parent_inode) return 1;

    nm_iop = __get_nm(smp_load_acquire(&parent_inode->i_op), struct nm_iop, fake_iop, lookup, nomount_hijacked_lookup);
    if (nm_iop) {
        parent_dir = READ_ONCE(nm_iop->dir_node);
    } else if (parent_inode->i_op == &nm_dir_iops) { 
        struct nm_inode_info *parent_info = parent_inode->i_private;
        if (parent_info) parent_dir = READ_ONCE(parent_info->dir_node);
    }
    if (!parent_dir) goto negative;

    hash = full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, dentry->d_name.name, dentry->d_name.len);
    if (nomount_get_rule_info(parent_dir, dentry->d_name.name, dentry->d_name.len, hash, &rule_info, false)) {
        if (nomount_is_uid_blocked(current_uid().val)) goto negative;
        if (rule_info.flags & NM_FLAG_WHITEOUT) return d_is_negative(dentry) ? 1 : 0;
        return injected ? 1 : 0;
    }

negative:
    if (d_is_negative(dentry)) {
        if (flags & LOOKUP_RCU) return -ECHILD;
        d_drop(dentry);
        return 0;
    }

    return injected ? 0 : 1;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static const struct file_operations nm_file_fops_mmap_prepare = {
    .owner = THIS_MODULE,
    .llseek = nm_llseek,
    .open = nm_open,
    .release = nm_release,
    .read_iter = nm_read_iter,
    .write_iter = nm_write_iter,
    .mmap_prepare = nm_mmap_prepare,
    .unlocked_ioctl = nm_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = nm_compat_ioctl,
#endif
    .splice_read = nm_splice_read,
    .splice_write = nm_splice_write,
    .fsync = nm_fsync,
};
#endif

static const struct file_operations nm_file_fops = {
    .owner = THIS_MODULE,
    .llseek = nm_llseek,
    .open = nm_open,
    .release = nm_release,
    .read_iter = nm_read_iter,
    .write_iter = nm_write_iter,
    .mmap = nm_mmap,
    .unlocked_ioctl = nm_unlocked_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = nm_compat_ioctl,
#endif
    .splice_read = nm_splice_read,
    .splice_write = nm_splice_write,
    .fsync = nm_fsync,
};

static const struct inode_operations nm_file_iops = {
    .getattr = nm_file_getattr,
    .setattr = nm_setattr,
    .listxattr = nm_listxattr,
    .get_link = nm_get_link,
};

static const struct file_operations nm_dir_fops = {
    .owner = THIS_MODULE,
    .open = nm_open,
    .release = nm_release,
    .llseek = nm_llseek,
    .read = generic_read_dir,
    .iterate_shared = nm_dir_iterate_dir,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    .iterate = nm_dir_iterate_dir,
#endif
};

static const struct inode_operations nm_dir_iops = {
    .lookup = nm_dir_lookup,
    .getattr = nm_file_getattr,
    .setattr = nm_setattr,
    .listxattr = nm_listxattr,
};

/* --- Hijacking Management --- */

static inline void nomount_hijack_superblock(struct super_block *sb)
{
    struct nm_sop *nm_sop;
    int i, count = 0;
    if (unlikely(!sb || !sb->s_op || 
        __get_nm(smp_load_acquire(&sb->s_op), struct nm_sop, fake_sop, destroy_inode, nomount_hijacked_destroy_inode))) return;

    nm_sop = kzalloc(sizeof(*nm_sop), GFP_KERNEL);
    if (unlikely(!nm_sop)) return;

    nm_sop->fake_sop = *(sb->s_op);
    nm_sop->orig_sop = sb->s_op;
    nm_sop->sb = sb;
    nm_sop->fake_sop.destroy_inode = nomount_hijacked_destroy_inode;
    nm_sop->fake_sop.drop_inode = nomount_hijacked_drop_inode;
    nm_sop->fake_sop.evict_inode = nomount_hijacked_evict_inode;

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

    list_add_tail_rcu(&nm_sop->list, &nomount_sb_list);
    smp_store_release(&sb->s_op, &nm_sop->fake_sop);
    nm_debug("Superblock successfully hijacked for dev: 0x%x\n", sb->s_dev);
}

static inline void nomount_hijack_dir_ops(struct nomount_dir_node *dir_node, struct inode *inode)
{
    struct nm_iop *nm_iop = NULL;
    struct nm_fop *nm_fop = NULL;

    if (inode->i_op && !__get_nm(smp_load_acquire(&inode->i_op), struct nm_iop, fake_iop, lookup, nomount_hijacked_lookup)) {
        nm_iop = kmem_cache_zalloc(nm_iop_cachep, GFP_KERNEL);
        if (likely(nm_iop)) {
            nm_iop->fake_iop = *(inode->i_op);
            nm_iop->orig_iop = inode->i_op;
            nm_iop->dir_node = dir_node;

            if (nm_iop->orig_iop->lookup) nm_iop->fake_iop.lookup = nomount_hijacked_lookup;
            smp_store_release(&inode->i_op, &nm_iop->fake_iop);
        }
    }

    if (inode->i_fop && !__get_nm(smp_load_acquire(&inode->i_fop), struct nm_fop, fake_fop, iterate_shared, nomount_hijacked_iterate_dir)) {
        nm_fop = kmem_cache_zalloc(nm_fop_cachep, GFP_KERNEL);
        if (likely(nm_fop)) {
            nm_fop->fake_fop = *(inode->i_fop);
            nm_fop->orig_fop = inode->i_fop;
            nm_fop->dir_node = dir_node;

            nm_fop->fake_fop.iterate_shared = nomount_hijacked_iterate_dir;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
            if (nm_fop->fake_fop.iterate)
                nm_fop->fake_fop.iterate = nomount_hijacked_iterate_dir;
#endif
            smp_store_release(&inode->i_fop, &nm_fop->fake_fop);
        }
    }

    if (nm_iop || nm_fop) nm_debug("Successfully hijacked VFS ops for parent dir (ino: %lu)\n", inode->i_ino);
}

static void nomount_hijack_dentry_ops(struct dentry *dentry)
{
    static const struct dentry_operations nm_dops = { .d_revalidate = nm_d_revalidate };
    if (!dentry) return;
    spin_lock(&dentry->d_lock);
    if (dentry->d_op != &nm_dops) {
        dentry->d_op = &nm_dops;
        dentry->d_flags &= ~(DCACHE_OP_WEAK_REVALIDATE | DCACHE_OP_DELETE | DCACHE_OP_PRUNE
                             | DCACHE_OP_COMPARE | DCACHE_OP_HASH | DCACHE_OP_REAL);
        dentry->d_flags |= DCACHE_OP_REVALIDATE;
    }
    spin_unlock(&dentry->d_lock);
}

static void nomount_cure_sb_inodes(struct super_block *sb)
{
    struct inode *inode;
    struct nm_iop *nm_iop;
    struct nm_fop *nm_fop;
    struct nomount_dir_node *dir_node;

    spin_lock(&sb->s_inode_list_lock);
    list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
        if (!inode->i_op && !inode->i_fop) continue;

        nm_iop = __get_nm(inode->i_op, struct nm_iop, fake_iop, lookup, nomount_hijacked_lookup);
        nm_fop = __get_nm(inode->i_fop, struct nm_fop, fake_fop, iterate_shared, nomount_hijacked_iterate_dir);
        if (!nm_iop && !nm_fop) continue;

        dir_node = NULL;
        if (nm_iop) {
            dir_node = nm_iop->dir_node;
            smp_store_release(&inode->i_op, nm_iop->orig_iop);
            call_rcu(&nm_iop->rcu, nm_iop_rcu_free);
        }
        if (nm_fop) {
            if (!dir_node) dir_node = nm_fop->dir_node;
            smp_store_release(&inode->i_fop, nm_fop->orig_fop);
            call_rcu(&nm_fop->rcu, nm_fop_rcu_free);
        }
        if (dir_node && !(dir_node->_tag_ptr & 1UL))
            call_rcu(&dir_node->rcu, nm_dir_rcu_free);
    }
    spin_unlock(&sb->s_inode_list_lock);
}

static void nomount_restore_superblocks(void)
{
    struct nm_sop *nm_sop, *tmp;

    list_for_each_entry_safe(nm_sop, tmp, &nomount_sb_list, list) {
        int i = 0;
        if (nm_sop->sb) {
            shrink_dcache_sb(nm_sop->sb);
            nomount_cure_sb_inodes(nm_sop->sb);
            smp_store_release(&nm_sop->sb->s_op, nm_sop->orig_sop);
            if (nm_sop->fake_xattr) {
                smp_store_release((const struct xattr_handler ***)&nm_sop->sb->s_xattr, nm_sop->orig_xattr);
                while (nm_sop->orig_xattr[i]) {
                    if (nm_sop->fake_xattr[i]) {
                        kfree(container_of(nm_sop->fake_xattr[i], struct nm_xattr_proxy, fake));
                    }
                    i++;
                }
                kfree(nm_sop->fake_xattr);
            }
            nm_debug("Successfully cured superblock for dev: 0x%x\n", nm_sop->sb->s_dev);
        }
        list_del_rcu(&nm_sop->list);
        kfree_rcu(nm_sop, rcu);
    }
}

/*** Module Management ***/

static struct nomount_dir_node *__nomount_alloc_dir_node(struct inode *inode) 
{
    struct nomount_dir_node *dir_node = kmem_cache_zalloc(nm_dir_cachep, GFP_KERNEL);
    if (unlikely(!dir_node)) return NULL;
    dir_node->dir_inode = inode ? igrab(inode) : NULL;
    seqcount_init(&dir_node->seq); 
    return dir_node;
}

static void __nomount_inject_child_locked(struct nomount_dir_node *dir_node, struct nomount_rule *rule, const char *name, size_t name_len)
{
    struct nomount_child_node *new_child;
    struct nomount_child_array *old_arr, *new_arr;
    int old_count = 0, capacity = 0, new_cap, i, pos = 0;
    u32 target_hash = full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, name, name_len);

    if (unlikely(!dir_node)) return;
    rule->parent_dir = dir_node;

    new_child = kmalloc(sizeof(*new_child) + name_len + 1, GFP_KERNEL);
    if (unlikely(!new_child)) return;

    new_child->fake_ino = rule->v_hash;
    new_child->name_hash = target_hash;
    new_child->d_type = (rule->flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG;
    new_child->flags = rule->flags;
    new_child->name_len = name_len;
    new_child->rule = rule;
    memcpy(new_child->name, name, name_len);
    new_child->name[name_len] = '\0';

    old_arr = dir_node->children;
    if (old_arr) {
        old_count = old_arr->count;
        capacity = old_arr->capacity;
        while (pos < old_count && old_arr->hashes[pos] < target_hash) pos++;
    }

    if (old_count < capacity) {
        write_seqcount_begin(&dir_node->seq);
        if (pos < old_count) {
            memmove(&old_arr->hashes[pos + 1], &old_arr->hashes[pos], (old_count - pos) * sizeof(u32));
            memmove(&old_arr->nodes[pos + 1], &old_arr->nodes[pos], (old_count - pos) * sizeof(void *));
        }
        old_arr->hashes[pos] = target_hash;
        old_arr->nodes[pos] = new_child;
        old_arr->count++;
        dir_node->bloom_mask |= (1ULL << (target_hash & 63));
        write_seqcount_end(&dir_node->seq);
        return;
    }

    new_cap = capacity == 0 ? 4 : capacity * 2;
    new_arr = kmalloc(sizeof(struct nomount_child_array) + (new_cap * sizeof(u32)) + (new_cap * sizeof(void *)), GFP_KERNEL);
    if (!new_arr) { kfree(new_child); return; }

    new_arr->hashes = (u32 *)(new_arr + 1);
    new_arr->nodes = (struct nomount_child_node **)(new_arr->hashes + new_cap);
    new_arr->capacity = new_cap;
    new_arr->count = old_count + 1;
    for (i = 0; i < pos; i++) {
        new_arr->hashes[i] = old_arr->hashes[i];
        new_arr->nodes[i] = old_arr->nodes[i];
    }

    new_arr->hashes[pos] = target_hash;
    new_arr->nodes[pos] = new_child;
    for (i = pos; i < old_count; i++) {
        new_arr->hashes[i + 1] = old_arr->hashes[i];
        new_arr->nodes[i + 1] = old_arr->nodes[i];
    }

    write_seqcount_begin(&dir_node->seq);
    rcu_assign_pointer(dir_node->children, new_arr);
    dir_node->bloom_mask |= (1ULL << (target_hash & 63));
    write_seqcount_end(&dir_node->seq);
    if (old_arr) call_rcu(&old_arr->rcu, nm_child_array_rcu_free);
}

static void __nomount_delete_child_locked(struct nomount_rule *rule)
{
    struct nomount_dir_node *dir_node = rule->parent_dir;
    struct nomount_child_array *old_arr;
    struct nomount_child_node *child_to_free = NULL;
    int old_count = 0, i, target_idx = -1;
    u64 mask = 0;
    if (unlikely(!dir_node)) return;

    old_arr = dir_node->children;
    if (!old_arr) return;
    old_count = old_arr->count;

    for (i = 0; i < old_count; i++) {
        if (old_arr->nodes[i]->rule == rule) {
            target_idx = i;
            child_to_free = old_arr->nodes[i];
            break;
        }
    }
    if (target_idx == -1) return;

    if (old_count == 1) {
        write_seqcount_begin(&dir_node->seq);
        rcu_assign_pointer(dir_node->children, NULL);
        dir_node->bloom_mask = 0;
        write_seqcount_end(&dir_node->seq);
        call_rcu(&old_arr->rcu, nm_child_array_rcu_free);
        kfree_rcu(child_to_free, rcu);
        return;
    }

    write_seqcount_begin(&dir_node->seq);
    if (target_idx < old_count - 1) {
        memmove(&old_arr->hashes[target_idx], &old_arr->hashes[target_idx + 1], (old_count - 1 - target_idx) * sizeof(u32));
        memmove(&old_arr->nodes[target_idx], &old_arr->nodes[target_idx + 1], (old_count - 1 - target_idx) * sizeof(void *));
    }
    old_arr->count--;

    for (i = 0; i < old_arr->count; i++) mask |= (1ULL << (old_arr->hashes[i] & 63));
    dir_node->bloom_mask = mask;

    write_seqcount_end(&dir_node->seq);
    kfree_rcu(child_to_free, rcu);
}

static int nomount_generate_virtual_topology(struct nomount_rule *target_rule)
{
    struct nomount_rule *irule, *ex, *current_rule = target_rule;
    char orig_v_path, *v_path = nm_get_vpath(target_rule);
    int parent_len, p_len = target_rule->v_len;
    const char *child_name, *lookup_path;
    struct nomount_dir_node *dir_node;
    struct hlist_node *tmp;
    struct inode *v_inode;
    struct path p_path;
    size_t child_len;
    int i, err = 0;
    u32 h_parent;
    HLIST_HEAD(pending_list);

    while (p_len > 1) {
        for (i = p_len - 1; i >= 0; i--) { if (v_path[i] == '/') break; }
        parent_len = (i == 0) ? 1 : i;
        child_name = v_path + i + 1;
        child_len = p_len - i - 1;
        h_parent = full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, v_path, parent_len);
        orig_v_path = v_path[i];
        if (i > 0) v_path[i] = '\0';

        ex = nm_tree_search_path(h_parent, parent_len, v_path);
        if (ex) {
            dir_node = ex->this_dir ? ex->this_dir : __nomount_alloc_dir_node(NULL);
            if (unlikely(!dir_node)) { err = -ENOMEM; goto loop_end; }
            dir_node->_tag_ptr = (unsigned long)ex | 1UL;
            if (!ex->this_dir) ex->this_dir = dir_node;
            __nomount_inject_child_locked(dir_node, current_rule, child_name, child_len);
            if (i > 0) v_path[i] = orig_v_path;
            goto success_break;
        }

        lookup_path = (parent_len == 1) ? "/" : v_path;
        if (kern_path(lookup_path, LOOKUP_FOLLOW, &p_path) == 0) {
            v_inode = d_backing_inode(p_path.dentry);
            dir_node = nomount_get_dir_node(v_inode);
            if (!dir_node) dir_node = __nomount_alloc_dir_node(v_inode);
            if (likely(dir_node)) {
                struct dentry *dentry;
                struct qstr qname = {
                    .name = child_name, .len = child_len,
                    .hash = full_name_hash(p_path.dentry, child_name, child_len),
                };                
                if (p_path.dentry->d_flags & DCACHE_OP_HASH)
                    p_path.dentry->d_op->d_hash(p_path.dentry, &qname);

                nomount_hijack_dir_ops(dir_node, v_inode);
                nomount_hijack_superblock(p_path.dentry->d_sb);
                dentry = d_lookup(p_path.dentry, &qname);
                if (dentry) { d_drop(dentry); dput(dentry); }
                __nomount_inject_child_locked(dir_node, current_rule, child_name, child_len);
            } else {
                err = -ENOMEM;
            }
            path_put(&p_path);

            if (i > 0) v_path[i] = orig_v_path;
            if (err) break;
            goto success_break;
        }

        irule = kzalloc(sizeof(struct nomount_rule) + parent_len + 1 + 2, GFP_KERNEL);
        if (!irule) { err = -ENOMEM; goto loop_end; }

        irule->v_len = parent_len;
        irule->v_hash = h_parent;
        irule->flags = NM_FLAG_IS_DIR | NM_FLAG_VIRTUAL_DIR;
        irule->v_ino = (unsigned long)h_parent;
        memcpy(nm_get_vpath(irule), v_path, parent_len);
        nm_get_vpath(irule)[parent_len] = '\0';
        nm_get_rpath(irule)[0] = '\0';

        dir_node = __nomount_alloc_dir_node(NULL);
        if (likely(dir_node)) {
            dir_node->_tag_ptr = (unsigned long)irule | 1UL;
            irule->this_dir = dir_node;
            __nomount_inject_child_locked(dir_node, current_rule, child_name, child_len);
        } else {
            kfree(irule);
            err = -ENOMEM;
            goto loop_end;
        }

        hlist_add_head(&irule->vpath_node, &pending_list);
        current_rule = irule;

loop_end:
        if (i > 0) v_path[i] = orig_v_path;
        if (err) break;
        p_len = i; 
        continue;

success_break:
        break;
    }

    if (likely(err == 0)) {
        hlist_for_each_entry_safe(irule, tmp, &pending_list, vpath_node) {
            hlist_del_init(&irule->vpath_node); 
            nm_tree_insert(irule);
        }
    } else {
        hlist_for_each_entry_safe(irule, tmp, &pending_list, vpath_node) {
            hlist_del_init(&irule->vpath_node);
            nm_free_rule(irule);
        }
    }

    return err;
}

static void nm_detach_dir_node(struct nomount_dir_node *dir_node)
{
    struct inode *inode;
    if (!dir_node || (dir_node->_tag_ptr & 1UL)) return; 
    inode = dir_node->dir_inode;
    if (inode) {
        struct nm_iop *nm_iop = __get_nm(smp_load_acquire(&inode->i_op), struct nm_iop, fake_iop, lookup, nomount_hijacked_lookup);
        struct nm_fop *nm_fop = __get_nm(smp_load_acquire(&inode->i_fop), struct nm_fop, fake_fop, iterate_shared, nomount_hijacked_iterate_dir);
        if (nm_iop) WRITE_ONCE(nm_iop->dir_node, NULL);
        if (nm_fop) WRITE_ONCE(nm_fop->dir_node, NULL);
    }
}

static void nomount_prune_empty_virtual_dirs(struct nomount_dir_node *dir_node, struct hlist_head *victims)
{
    struct nomount_rule *owner;
    while (dir_node && (!dir_node->children || dir_node->children->count == 0)) {
        owner = dir_node->_tag_ptr & 1UL ? (struct nomount_rule *)(dir_node->_tag_ptr & ~1UL) : NULL;
        if (!owner) break;
        if (!(owner->flags & NM_FLAG_VIRTUAL_DIR)) {
            owner->this_dir = NULL;
            if (READ_ONCE(dir_node->v_inode)) {
                WRITE_ONCE(dir_node->_tag_ptr, 1UL);
            } else {
                nm_detach_dir_node(dir_node);
                call_rcu(&dir_node->rcu, nm_dir_rcu_free);
            }
            break;
        }
        rb_erase_cached(&owner->rb_node, &nomount_rules_tree);
        if (owner->parent_dir) __nomount_delete_child_locked(owner);
        nm_debug("Pruned empty virtual directory: %s\n", nm_get_vpath(owner));
        dir_node = owner->parent_dir;
        hlist_add_head(&owner->vpath_node, victims);
    }
}

/*** Rule Operations ***/

static struct nomount_rule *nm_alloc_rule(const char *v_path, const char *r_path, u16 v_len, u16 r_len, u32 flags, unsigned int target_uid)
{
    struct nomount_rule *rule;
    bool is_whiteout = (flags & NM_FLAG_WHITEOUT);
    struct path v_path_struct;

    if (!v_path || (!r_path && !is_whiteout)) return ERR_PTR(-EINVAL);
    while (v_len > 1 && v_path[v_len - 1] == '/') { v_len--; }
    if (!is_whiteout) { while (r_len > 1 && r_path[r_len - 1] == '/') { r_len--; } }

    if (is_whiteout) r_len = 0;
    rule = kzalloc((sizeof(struct nomount_rule) + v_len + 1 + r_len + 1), GFP_KERNEL);
    if (!rule) return ERR_PTR(-ENOMEM);

    rule->v_hash = full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, v_path, v_len);
    rule->flags = flags;
    rule->v_len = v_len;
    rule->target_uid = target_uid;
    memcpy(nm_get_vpath(rule), v_path, v_len);
    nm_get_vpath(rule)[v_len] = '\0';
    if (!is_whiteout) memcpy(nm_get_rpath(rule), r_path, r_len);
    nm_get_rpath(rule)[r_len] = '\0';

    if (!is_whiteout && kern_path(nm_get_rpath(rule), LOOKUP_FOLLOW, &rule->r_path) == 0) {
        struct inode *real_inode = d_backing_inode(rule->r_path.dentry);
        if (likely(real_inode)) {
            real_inode->i_flags |= S_PRIVATE;
            if (S_ISDIR(real_inode->i_mode)) rule->flags |= NM_FLAG_IS_DIR;
        }
    }

    if (kern_path(nm_get_vpath(rule), LOOKUP_FOLLOW, &v_path_struct) == 0) {
        rule->v_ino = d_backing_inode(v_path_struct.dentry)->i_ino;
        path_put(&v_path_struct);
    } else {
         rule->v_ino = (unsigned long)rule->v_hash;
    }

    return rule;
}

static void nm_free_rule(struct nomount_rule *rule)
{
    if (unlikely(!rule)) return;
    if (rule->r_path.dentry) path_put(&rule->r_path);
    if (rule->this_dir) {
        if (READ_ONCE(rule->this_dir->v_inode)) {
            WRITE_ONCE(rule->this_dir->_tag_ptr, 1UL);
        } else {
            nm_detach_dir_node(rule->this_dir);
            nm_dir_rcu_free(&rule->this_dir->rcu);
        }
    }
    kfree(rule);
}

static void nm_detach_rule_locked(struct nomount_rule *rule, struct hlist_head *victims, bool prune)
{
    rb_erase_cached(&rule->rb_node, &nomount_rules_tree);
    if (rule->parent_dir) {
        __nomount_delete_child_locked(rule);
        if (prune) nomount_prune_empty_virtual_dirs(rule->parent_dir, victims); 
    }
    hlist_add_head(&rule->vpath_node, victims);
}

static int __nomount_add_rule(const char *v_path, const char *r_path, u16 v_len, u16 r_len, u32 flags, unsigned int target_uid)
{
    struct nomount_rule *rule, *existing, *victim_rule;
    struct hlist_node *tmp;
    HLIST_HEAD(victims);
    int err = 0;

    rule = nm_alloc_rule(v_path, r_path, v_len, r_len, flags, target_uid);
    if (IS_ERR(rule)) return PTR_ERR(rule);

    down_write(&nomount_rwsem);
    existing = nm_tree_search_exact(rule->v_hash, v_len, nm_get_vpath(rule), target_uid);
    if (existing) {
        if (existing->this_dir) {
            if (rule->this_dir) call_rcu(&rule->this_dir->rcu, nm_dir_rcu_free);
            rule->this_dir = existing->this_dir;
            if (rule->this_dir->_tag_ptr & 1UL) rule->this_dir->_tag_ptr = (unsigned long)rule | 1UL;
            existing->this_dir = NULL;
        }
        nm_detach_rule_locked(existing, &victims, false);
        nm_info("Shadowing existing rule for: %s\n", nm_get_vpath(rule));
    }

    err = nomount_generate_virtual_topology(rule);
    if (err != 0) {
        up_write(&nomount_rwsem);
        nm_free_rule(rule); 
        synchronize_rcu();
        hlist_for_each_entry_safe(victim_rule, tmp, &victims, vpath_node) {
            nm_free_rule(victim_rule);
        }
        return err;
    }

    nm_tree_insert(rule);
    up_write(&nomount_rwsem);

    if (!hlist_empty(&victims)) {
        synchronize_rcu();
        hlist_for_each_entry_safe(victim_rule, tmp, &victims, vpath_node) {
            nm_free_rule(victim_rule);
        }
    }

    if (flags & NM_FLAG_WHITEOUT)
        nm_info("Successfully added whiteout rule: %s\n", nm_get_vpath(rule));
    else
        nm_info("Successfully added injection rule: %s -> %s\n", nm_get_vpath(rule), nm_get_rpath(rule));
        
    return 0;
}

static void __nomount_del_rule(const char *v_path, size_t v_len, unsigned int target_uid, struct hlist_head *r_victims)
{
    u32 hash = full_name_hash((const void *)(unsigned long)NOMOUNT_MAGIC_SIG, v_path, v_len);
    struct nomount_rule *rule = nm_tree_search_exact(hash, v_len, v_path, target_uid);
    if (rule) nm_detach_rule_locked(rule, r_victims, true);
}

static void __nomount_clear_all(int clear_flags)
{
    struct nomount_rule *rule;
    struct hlist_node *tmp;
    HLIST_HEAD(r_victims);

    if (clear_flags & NM_CLEAR_UIDS) {
        static_branch_disable(&nomount_active_uids);
        synchronize_rcu();
        idr_destroy(&nomount_uid_idr);
        if (!(clear_flags & NM_CLEAR_EXIT)) idr_init(&nomount_uid_idr);
    }
    if (clear_flags & NM_CLEAR_RULES) {
        struct rb_node *node;
        while ((node = rb_first_cached(&nomount_rules_tree)) != NULL) {
            rule = rb_entry(node, struct nomount_rule, rb_node);
            nm_detach_rule_locked(rule, &r_victims, false);
        }
        synchronize_rcu();
        hlist_for_each_entry_safe(rule, tmp, &r_victims, vpath_node) {
            nm_free_rule(rule);
        }
    }

    if (clear_flags & NM_CLEAR_EXIT) nomount_restore_superblocks();
}

/*** Keyring Communication API ***/

static int nm_process_payload(unsigned long user_addr)
{
    struct nm_payload *payload;
    struct page *page;
    int nr_pages;
    char *v_ptr, *r_ptr;

    nr_pages = get_user_pages_fast(user_addr, 1, FOLL_WRITE, &page);
    if (nr_pages != 1) return -EFAULT;

    payload = kmap(page);
    if (payload->magic != NOMOUNT_MAGIC_SIG) {
        kunmap(page);
        put_page(page);
        return -EFAULT;
    }

    payload->status = 0;
    switch (payload->cmd) {
        case NM_CMD_GET_VERSION:
            payload->data_size = strlen(NOMOUNT_VERSION);
            memcpy(payload->buffer, NOMOUNT_VERSION, payload->data_size);
            break;

        case NM_CMD_ADD_RULE:
        case NM_CMD_ADD_RULE_BATCH:
            if (payload->data_size > 0) {
                while (payload->arg1 + 12 <= payload->data_size) {
                    payload->flags      = get_unaligned((const u32 *)(payload->buffer + payload->arg1));
                    payload->target_uid = get_unaligned((const u32 *)(payload->buffer + payload->arg1 + 4));
                    payload->v_len      = get_unaligned((const u16 *)(payload->buffer + payload->arg1 + 8));
                    payload->r_len      = get_unaligned((const u16 *)(payload->buffer + payload->arg1 + 10));
                    payload->arg1 += 12;
                    if (payload->arg1 + payload->v_len + payload->r_len > payload->data_size) break;
                    if (unlikely(payload->v_len >= PATH_MAX || payload->r_len >= PATH_MAX)) break;

                    v_ptr = (char *)payload->buffer + payload->arg1; payload->arg1 += payload->v_len;
                    r_ptr = (char *)payload->buffer + payload->arg1; payload->arg1 += payload->r_len;
                    payload->status = __nomount_add_rule(v_ptr, r_ptr, payload->v_len, payload->r_len, payload->flags, payload->target_uid);
                }
            } else {
                v_ptr = payload->buffer; r_ptr = payload->buffer + payload->v_len + 1;
                payload->status = __nomount_add_rule(v_ptr, r_ptr, payload->v_len, payload->r_len, payload->flags, payload->target_uid);
            }
            break;

        case NM_CMD_DEL_RULE: {
            HLIST_HEAD(r_victims);    
            down_write(&nomount_rwsem);
            if (payload->data_size > 0) {
                while (payload->arg1 + 6 <= payload->data_size) {
                    payload->target_uid = get_unaligned((const u32 *)(payload->buffer + payload->arg1));
                    payload->v_len = get_unaligned((const u16 *)(payload->buffer + payload->arg1 + 4));
                    payload->arg1 += 6;
                    if (payload->arg1 + payload->v_len > payload->data_size) break;
                    __nomount_del_rule(payload->buffer + payload->arg1, payload->v_len, payload->target_uid, &r_victims);
                    payload->arg1 += payload->v_len;
                }
            } else {
                __nomount_del_rule(payload->buffer, payload->v_len, payload->target_uid, &r_victims);
            }
            up_write(&nomount_rwsem);
            if (!hlist_empty(&r_victims)) {
                struct nomount_rule *rule; struct hlist_node *tmp;
                synchronize_rcu();
                hlist_for_each_entry_safe(rule, tmp, &r_victims, vpath_node) {
                    nm_free_rule(rule);
                }
            } else {
                payload->status = -ENOENT;
            }
            break;
        }

        case NM_CMD_ADD_UID:
            if (!nomount_is_uid_blocked(payload->target_uid)) {
                down_write(&nomount_rwsem);
                if (idr_alloc(&nomount_uid_idr, (void *)8, payload->target_uid, payload->target_uid + 1, GFP_KERNEL) >= 0) {
                    static_branch_enable(&nomount_active_uids);
                    nm_info("Successfully added blocked UID: %u\n", payload->target_uid);
                } else {
                    payload->status = -ENOMEM;
                }
                up_write(&nomount_rwsem);
            } else {
                payload->status = -EEXIST;
            }
            break;

        case NM_CMD_DEL_UID:
            down_write(&nomount_rwsem);
            if (idr_find(&nomount_uid_idr, payload->target_uid)) {
                idr_remove(&nomount_uid_idr, payload->target_uid);
                if (idr_is_empty(&nomount_uid_idr)) static_branch_disable(&nomount_active_uids);
                nm_info("Successfully removed blocked UID: %u\n", payload->target_uid);
            } else {
                payload->status = -ENOENT;
            }
            up_write(&nomount_rwsem);
            break;

        case NM_CMD_CLEAR_ALL:
        case NM_CMD_CLEAR_UIDS:
        case NM_CMD_CLEAR_RULES: {
            int clear_flags = 0;
            if (payload->cmd == NM_CMD_CLEAR_ALL) clear_flags = (NM_CLEAR_UIDS | NM_CLEAR_RULES);
            if (payload->cmd == NM_CMD_CLEAR_UIDS) clear_flags = NM_CLEAR_UIDS;
            if (payload->cmd == NM_CMD_CLEAR_RULES) clear_flags = NM_CLEAR_RULES;

            down_write(&nomount_rwsem);
            __nomount_clear_all(clear_flags);
            up_write(&nomount_rwsem);
            nm_info("Executed selective clear (cmd: %u)\n", payload->cmd);
            break;
        }

        case NM_CMD_GET_LIST: {
            struct nomount_rule *rule;
            struct rb_node *node;
            int current_idx = 0;
            int target_idx = payload->arg1;
            payload->data_size = 0;

            down_read(&nomount_rwsem);
            for (node = rb_first_cached(&nomount_rules_tree); node; node = rb_next(node)) {
                if (current_idx < target_idx) { current_idx++; continue; }
                rule = rb_entry(node, struct nomount_rule, rb_node);
                payload->r_len = rule->flags & NM_FLAG_WHITEOUT ? 0 : strlen(nm_get_rpath(rule));
                if (payload->data_size + 12 + rule->v_len + payload->r_len > sizeof(payload->buffer)) break;

                put_unaligned(rule->flags, (u32 *)(payload->buffer + payload->data_size));
                put_unaligned(rule->target_uid, (u32 *)(payload->buffer + payload->data_size + 4));
                put_unaligned(rule->v_len, (u16 *)(payload->buffer + payload->data_size + 8));
                put_unaligned(payload->r_len, (u16 *)(payload->buffer + payload->data_size + 10));
                payload->data_size += 12;

                memcpy(payload->buffer + payload->data_size, nm_get_vpath(rule), rule->v_len);
                payload->data_size += rule->v_len;
                if (payload->r_len > 0) memcpy(payload->buffer + payload->data_size, nm_get_rpath(rule), payload->r_len);
                payload->data_size += payload->r_len;
                current_idx++;
            }
            up_read(&nomount_rwsem);
            payload->arg1 = current_idx;
            break;
        }

        case NM_CMD_GET_UIDS:
            payload->data_size = 0;
            if (!static_branch_unlikely(&nomount_active_uids)) break;

            rcu_read_lock();
            while (idr_get_next(&nomount_uid_idr, &payload->arg1) != NULL) {
                if (payload->data_size + 4 > sizeof(payload->buffer)) break;
                put_unaligned(payload->arg1, (u32 *)(payload->buffer + payload->data_size));
                payload->data_size += 4;
                payload->arg1++;
            }
            rcu_read_unlock();
            break;
    }

    kunmap(page);
    put_page(page);
    return 0;
}

static int nm_key_instantiate(struct key *key, struct key_preparsed_payload *prep)
{
    unsigned long user_addr = 0;
    if (!capable(CAP_SYS_ADMIN)) return -EPERM;
    if (prep->datalen == 8) user_addr = *(u64 *)prep->data;
    else if (prep->datalen == 4) user_addr = *(u32 *)prep->data;
    if (user_addr) nm_process_payload(user_addr);
    return -ECANCELED; 
}

static struct key_type nm_key_type = {
    .name = "nomount",
    .instantiate = nm_key_instantiate,
};

static int __init nomount_init(void)
{
    int ret;

    nm_dir_cachep   = KMEM_CACHE(nomount_dir_node, SLAB_HWCACHE_ALIGN);
    nm_inode_cachep = KMEM_CACHE(nm_inode_info, SLAB_HWCACHE_ALIGN);
    nm_iop_cachep   = KMEM_CACHE(nm_iop, SLAB_HWCACHE_ALIGN);
    nm_fop_cachep   = KMEM_CACHE(nm_fop, SLAB_HWCACHE_ALIGN);

    if (!nm_dir_cachep || !nm_inode_cachep || !nm_iop_cachep || !nm_fop_cachep) {
        nm_err("Failed to allocate memory slab caches\n");
        if (nm_dir_cachep) kmem_cache_destroy(nm_dir_cachep);
        if (nm_inode_cachep) kmem_cache_destroy(nm_inode_cachep);
        if (nm_iop_cachep) kmem_cache_destroy(nm_iop_cachep);
        if (nm_fop_cachep) kmem_cache_destroy(nm_fop_cachep);
        return -ENOMEM;
    }

    ret = register_key_type(&nm_key_type);
    if (ret) {
        nm_err("Failed to register key type (err: %d)\n", ret);
        kmem_cache_destroy(nm_dir_cachep);
        kmem_cache_destroy(nm_inode_cachep);
        kmem_cache_destroy(nm_iop_cachep);
        kmem_cache_destroy(nm_fop_cachep);
        return ret;
    }

    nm_info("Loaded successfully\n");
    return 0;
}

static void __exit nomount_exit(void)
{
    unregister_key_type(&nm_key_type);

    down_write(&nomount_rwsem);
    __nomount_clear_all(NM_CLEAR_UIDS | NM_CLEAR_RULES | NM_CLEAR_EXIT);
    up_write(&nomount_rwsem);
    rcu_barrier();
    kmem_cache_destroy(nm_dir_cachep);
    kmem_cache_destroy(nm_inode_cachep);
    kmem_cache_destroy(nm_iop_cachep);
    kmem_cache_destroy(nm_fop_cachep);

    nm_info("Unloaded successfully\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION(NOMOUNT_VERSION);
MODULE_AUTHOR("maxsteeel");
MODULE_DESCRIPTION("NoMount Path Redirection VFS Subsystem");

fs_initcall(nomount_init);
module_exit(nomount_exit);
