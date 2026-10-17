#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/cred.h>
#include <linux/xattr.h>
#include <linux/module.h>
#include "nomount.h"

/*** Helpers ***/

static bool nm_block_isolated_uids = false;
static __always_inline bool nm_is_uid_blocked(uid_t target_uid)
{
    struct nm_uid_array *arr;

    if (unlikely(READ_ONCE(nm_block_isolated_uids))) {
        uid_t app_id = target_uid % 100000U;
        if (app_id >= 90000U && app_id <= 99999U) return true;
    }

    if (likely(!rcu_access_pointer(nm_uids)))
        return false;

    rcu_guard();
    if ((arr = rcu_dereference(nm_uids)) && nm_uid_bloom_test(arr, target_uid)) {
        for (int i = 0; i < arr->count; i++) {
            if (arr->uids[i] == target_uid)
                return true;
        }
    }
    return false;
}

static __always_inline bool nm_child_matches(struct nm_leaf *leaf, const char *name, size_t len)
{
    if (unlikely(leaf->child_len != len)) return false;
    const char *child_name = nm_get_child_name(leaf);
    return child_name[0] == name[0] && child_name[len - 1] == name[len - 1] && !memcmp(child_name, name, len);
}

static inline bool nm_per_uid_view_active(void)
{
    return rcu_access_pointer(nm_uids) != NULL || READ_ONCE(nm_block_isolated_uids);
}

static __always_inline struct nm_leaf *nm_bsearch_child(struct nm_child_array *arr, const char *name, size_t len, u32 hash, int *index)
{
	struct nm_child *entries = nm_entries(arr);
	int l = 0, n = arr->count;

	while (n > 0) {
		int step = n >> 1, m = l + step, less = (entries[m].hash < hash), mask = -less;
		l += (step + 1) & mask;
		n = step + ((n - (step << 1) - 1) & mask);
    }
	if (index) *index = l;
	while (l < arr->count && entries[l].hash == hash) {
		struct nm_leaf *leaf = entries[l].leaf;
		if (nm_child_matches(leaf, name, len)) {
			if (index) *index = l;
			return leaf;
		}
		l++;
	}
	return NULL;
}

static struct nm_rule *nm_select_rule(struct nm_rule_array *arr, unsigned int uid)
{
    if (!arr) return NULL;
    if (!arr->uid_dependent) return &arr->rules[0];
    for (int i = 0; i < arr->count; i++) {
        struct nm_rule *r = &arr->rules[i];
        if (r->target_uid == uid) return r;
        if (r->target_uid > uid) break;
    }
    return !arr->rules[0].target_uid ? &arr->rules[0] : NULL;
}

static bool nm_resolve_rule_from_leaf(struct nm_leaf *leaf, struct nm_rule_array *arr, uid_t fsuid, struct nm_rule_info *out)
{
    struct nm_rule *rule = nm_select_rule(arr, fsuid);
    if (!rule) return false;
    out->flags = rule->flags;
    out->v_ino = leaf->v_ino;
    out->r_path = rule->r_path;
    out->this_dir = (rule->flags & NM_FLAG_IS_DIR) ? leaf->this_dir : NULL;
    if (out->r_path.dentry) path_get(&out->r_path);
    if (out->this_dir) atomic_inc(&out->this_dir->refs);
    return true;
}

static struct nm_leaf *nm_find_leaf_in_dir(struct nm_dir_node *dir_node, const char *name, size_t len)
{
    struct nm_child_array *children;
    struct nm_leaf *leaf = NULL;

    if (!dir_node || !(children = rcu_dereference(dir_node->children)))
        return NULL;
    u32 bloom_h = nm_qhash(name, len);
    if (!children->bloom_words) {
        struct nm_child *entries = nm_entries(children);
        for (int i = 0; i < children->count; i++) {
            if (entries[i].bloom_hash == bloom_h && nm_child_matches(entries[i].leaf, name, len)) {
                leaf = entries[i].leaf;
                break;
            }
        }
    } else if (unlikely(nm_bloom_test(children, bloom_h))) {
        u32 hash = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, name, len);
        leaf = nm_bsearch_child(children, name, len, hash, NULL);
    }
    return leaf;
}

static void nm_dir_rcu_free(struct rcu_head *head)
{
    struct nm_dir_node *dir = container_of(head, struct nm_dir_node, rcu);
    if (dir->pinned_dentry) dput(dir->pinned_dentry);
    kfree(rcu_dereference_raw(dir->children));
    kfree(dir);
}

static void nm_dir_put(struct nm_dir_node *dir_node)
{
    if (dir_node && atomic_dec_and_test(&dir_node->refs))
        call_rcu(&dir_node->rcu, nm_dir_rcu_free);
}

static inline void nm_destroy_virtual_inode(struct inode *inode)
{
    struct nm_inode_info *info = READ_ONCE(inode->i_private);
    if (!info) return;

    WRITE_ONCE(inode->i_private, NULL);
    smp_wmb();

    nm_dir_put(info->dir_node);
    if (info->r_path.dentry) path_put(&info->r_path);
    kfree_rcu(info, rcu);
}

static inline void nm_destroy_hijacked_inode(struct inode *inode)
{
    struct nm_dir_ops *ops = nm_get_nm_iop(smp_load_acquire(&inode->i_op));
    struct nm_dir_node *node;

    if (!ops && !(ops = nm_get_nm_fop(smp_load_acquire(&inode->i_fop)))) return;
    if (!(node = xchg(&ops->dir_node, NULL))) return;

    if (inode->i_op == &ops->fake_iop) smp_store_release(&inode->i_op, ops->orig_iop);
    if (inode->i_fop == &ops->fake_fop) smp_store_release(&inode->i_fop, ops->orig_fop);
    nm_dir_put(node);
    kfree_rcu(ops, rcu);
}

struct nm_proxy_ctx {
    struct dir_context ctx;
    struct dir_context *orig_ctx;
    struct nm_dir_node *dir_node;
    uid_t fsuid;
    bool emitted;
};

static NM_ACTOR_RET nm_actor_proxy(struct dir_context *ctx, const char *name, int namelen,
                                        loff_t offset, u64 ino, unsigned int d_type)
{
    struct nm_proxy_ctx *proxy = container_of(ctx, struct nm_proxy_ctx, ctx);
    struct dir_context *orig_ctx = proxy->orig_ctx;
    NM_ACTOR_RET ret;

    {
        rcu_guard();
        struct nm_leaf *leaf = nm_find_leaf_in_dir(proxy->dir_node, name, namelen);
        if (unlikely(leaf && nm_select_rule(rcu_dereference(leaf->rules), proxy->fsuid))) {
            proxy->ctx.pos = offset;
            return NM_ACTOR_CONTINUE;
        }
    }

    orig_ctx->pos = proxy->ctx.pos;
    ret = orig_ctx->actor(orig_ctx, name, namelen, offset, ino, d_type);
    proxy->ctx.pos = orig_ctx->pos;
    proxy->emitted = true;

    return ret;
}

static inline void nm_emit_virtual_children(struct dir_context *ctx, struct nm_dir_node *dir_node, uid_t fsuid)
{
	if (!dir_node) return;
	if (!nm_is_virtual_pos(ctx->pos)) ctx->pos = nm_pack_pos(0);

	for (int id = nm_unpack_pos(ctx->pos); ; id++) {
		char name_buf[NAME_MAX + 1];
		int name_len = 0;
		unsigned long v_ino = 0;
		unsigned char d_type = 0;
		bool do_emit = false, has_more = false;

		rcu_read_lock();
		struct nm_child_array *array = rcu_dereference(dir_node->children);
		if (array && id < array->count) {
			struct nm_leaf *leaf = nm_entries(array)[id].leaf;
			struct nm_rule *rule = nm_select_rule(rcu_dereference(leaf->rules), fsuid);
			has_more = true;
			if (rule && !(rule->flags & NM_FLAG_WHITEOUT)) {
				name_len = leaf->child_len;
				if (likely(name_len <= NAME_MAX)) {
					memcpy(name_buf, nm_get_child_name(leaf), name_len);
					v_ino = leaf->v_ino;
					d_type = (rule->flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG;
					do_emit = true;
				}
			}
		}
		rcu_read_unlock();
		if (!has_more) break;

		ctx->pos = nm_pack_pos(id);
		if (do_emit && !dir_emit(ctx, name_buf, name_len, v_ino, d_type)) break;
		ctx->pos = nm_pack_pos(id + 1);
	}
}

static void nm_init_prealloc_inode(struct inode *inode, struct nm_inode_info *info, unsigned long v_ino)
{
    struct inode *r_inode = info->r_path.dentry ? d_backing_inode(info->r_path.dentry) : NULL;

    inode->i_ino = v_ino;
    inode->i_private = info;
    inode->i_mode   = r_inode ? r_inode->i_mode    : (S_IFDIR | 0755);
    inode->i_size   = r_inode ? i_size_read(r_inode) : 4096;
    inode->i_blocks = r_inode ? r_inode->i_blocks  : 8;
    inode->i_uid    = r_inode ? r_inode->i_uid     : GLOBAL_ROOT_UID;
    inode->i_gid    = r_inode ? r_inode->i_gid     : GLOBAL_ROOT_GID;
    inode->i_op     = (r_inode && !S_ISDIR(r_inode->i_mode)) ? &nm_file_iops : &nm_dir_iops;

    if (r_inode && !S_ISDIR(r_inode->i_mode))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
        inode->i_fop = (r_inode->i_fop && r_inode->i_fop->mmap_prepare) ? &nm_file_fops_mmap_prepare : &nm_file_fops;
#else
        inode->i_fop = &nm_file_fops;
#endif
    else
        inode->i_fop = &nm_dir_fops;

    if (r_inode) nm_sync_inode_times(inode, r_inode), inode->i_mapping = r_inode->i_mapping;
    inode->i_flags |= S_PRIVATE | S_NOATIME | S_NOCMTIME | S_NOSEC;
    if (!S_ISLNK(inode->i_mode)) inode->i_opflags |= IOP_NOFOLLOW;
}

static int nm_cached_inode_set(struct inode *inode, void *data) { return 0; }
static int nm_cached_inode_test(struct inode *inode, void *data)
{
    struct nm_inode_info *info = READ_ONCE(inode->i_private);
    struct nm_inode_info *binding = data;
    struct inode *r_inode;

    if (inode->i_op != &nm_dir_iops || !info || info->dir_node != binding->dir_node || !path_equal(&info->r_path, &binding->r_path))
        return 0;
    r_inode = binding->r_path.dentry ? d_backing_inode(binding->r_path.dentry) : NULL;
    return !r_inode || nm_inode_is_new(inode) || (inode->i_mode == r_inode->i_mode && uid_eq(inode->i_uid, r_inode->i_uid) &&
            gid_eq(inode->i_gid, r_inode->i_gid));
}

static struct dentry *nm_materialize_rule(struct inode *dir, struct dentry *dentry, struct nm_rule_info *rule_info)
{
    struct nm_inode_info probe, *info = NULL;
    struct inode *inode;
    struct dentry *res = ERR_PTR(-ENODATA);

    if (rule_info->flags & NM_FLAG_WHITEOUT) {
        d_add(dentry, NULL);
        res = NULL;
        goto out;
    }

    if (rule_info->this_dir) {
        probe.r_path = rule_info->r_path;
        probe.dir_node = rule_info->this_dir;
        inode = iget5_locked(dir->i_sb, (unsigned long)rule_info->this_dir, nm_cached_inode_test, nm_cached_inode_set, &probe);
        if (!inode) goto out;
        if (!nm_inode_is_new(inode)) {
            res = d_splice_alias(inode, dentry);
            goto out;
        }
        if (!(info = kmalloc(sizeof(*info), GFP_KERNEL))) { iget_failed(inode); goto out; }
        *info = probe;
        nm_init_prealloc_inode(inode, info, rule_info->v_ino);
        unlock_new_inode(inode);
        res = d_splice_alias(inode, dentry);
        return res;
    }

    if (!(info = kmalloc(sizeof(*info), GFP_KERNEL))) goto out;
    info->r_path = rule_info->r_path;
    info->dir_node = NULL;
    if (!(inode = new_inode(dir->i_sb))) goto out;
    nm_init_prealloc_inode(inode, info, rule_info->v_ino);
    d_add(dentry, inode);
    return NULL;

out:
    kfree(info);
    nm_dir_put(rule_info->this_dir);
    if (rule_info->r_path.dentry) path_put(&rule_info->r_path);
    return res;
}

/*** i_op / s_op / f_op Hijacking Hooks ***/
static struct dentry *nm_hijacked_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    const struct inode_operations *orig_iop = NULL;
    struct nm_rule_info rule_info = {0};
    struct nm_dir_node *dir_node;
    struct nm_dir_ops *nm_iop = NULL;
    struct nm_leaf *leaf = NULL;
    struct dentry *res;
    bool special = false, found = false, uid_dep = false;

    rcu_read_lock();
    if (likely((nm_iop = nm_get_nm_iop(rcu_dereference(dir->i_op))))) {
        orig_iop = nm_iop->orig_iop;
        if ((dir_node = rcu_dereference(nm_iop->dir_node))) {
            if ((leaf = nm_find_leaf_in_dir(dir_node, dentry->d_name.name, dentry->d_name.len))) {
                struct nm_rule_array *arr = rcu_dereference(leaf->rules);
                special = nm_per_uid_view_active();
                uid_dep = arr && arr->uid_dependent;
                uid_t fsuid = (special || uid_dep) ? current_fsuid().val : 0;
                if (!special || likely(!nm_is_uid_blocked(fsuid))) {
                    special = special || uid_dep;
                    found = nm_resolve_rule_from_leaf(leaf, arr, fsuid, &rule_info);
                }
            }
        }
    }
    rcu_read_unlock();

    if (found)
        res = nm_materialize_rule(dir, dentry, &rule_info);
    else if (likely(orig_iop && orig_iop->lookup))
        res = orig_iop->lookup(dir, dentry, flags);
    else
        return ERR_PTR(-EOPNOTSUPP);

    if (special && !IS_ERR(res))
        nm_install_dops(res ? res : dentry, uid_dep);
    return res;
}

static int nm_hijacked_iterate_dir(struct file *file, struct dir_context *ctx)
{
    const struct file_operations *orig_fop = NULL;
    struct nm_dir_node *d, *dir_node = NULL;
    struct nm_dir_ops *nm_fop = NULL;
    struct nm_proxy_ctx proxy_ctx = { .ctx.actor = nm_actor_proxy };
    uid_t fsuid = current_fsuid().val;
    bool uid_blocked = false;
    int res = 0;

    rcu_read_lock();
    if ((nm_fop = nm_get_nm_fop(rcu_dereference(file->f_op)))) {
        orig_fop = nm_fop->orig_fop;
        if ((d = rcu_dereference(nm_fop->dir_node)) && rcu_access_pointer(d->children) && atomic_inc_not_zero(&d->refs))
            dir_node = d;
    }
    rcu_read_unlock();

    if (unlikely(!orig_fop || !dir_node))
        goto do_real_iterate;

    uid_blocked = nm_is_uid_blocked(fsuid);
    if (unlikely(nm_is_virtual_pos(ctx->pos))) {
        if (!uid_blocked) nm_emit_virtual_children(ctx, dir_node, fsuid);
        nm_dir_put(dir_node);
        return 0;
    }

    if (likely(!rcu_access_pointer(dir_node->children)) || unlikely(uid_blocked))
        goto put_dir_node;

    proxy_ctx.ctx.pos = ctx->pos;
    proxy_ctx.orig_ctx = ctx;
    proxy_ctx.fsuid = fsuid;
    proxy_ctx.dir_node = dir_node;
    res = nm_call_iterate(file, &proxy_ctx.ctx, orig_fop);
    ctx->pos = proxy_ctx.ctx.pos;

    if (res < 0 || proxy_ctx.emitted) {
        nm_dir_put(dir_node);
        return res;
    }

    ctx->pos = nm_pack_pos(0);
    nm_emit_virtual_children(ctx, dir_node, fsuid);
    nm_dir_put(dir_node);
    return res;

put_dir_node:
    nm_dir_put(dir_node);
do_real_iterate:
    if (likely(orig_fop))
        return nm_call_iterate(file, ctx, orig_fop);
    return -ENOTDIR;
}

static int nm_hijacked_drop_inode(struct inode *inode)
{
    struct nm_sop *nm_sop;
    if (nm_inode_is_virtual(inode)) return 1;

    nm_sop = nm_get_nm_sop(smp_load_acquire(&inode->i_sb->s_op));
    if (nm_sop && nm_sop->orig_sop && nm_sop->orig_sop->drop_inode)
        return nm_sop->orig_sop->drop_inode(inode);

    return !inode->i_nlink || inode_unhashed(inode);
}

static void nm_hijacked_evict_inode(struct inode *inode)
{
    struct nm_sop *nm_sop = nm_get_nm_sop(smp_load_acquire(&inode->i_sb->s_op));
    bool virtual = nm_inode_is_virtual(inode);

    virtual ? nm_destroy_virtual_inode(inode) : nm_destroy_hijacked_inode(inode);

    if (!virtual && nm_sop && nm_sop->orig_sop && nm_sop->orig_sop->evict_inode) {
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
    if (unlikely(!info->r_path.dentry)) {
        file->private_data = NULL;
        return 0;
    }

    real_file = dentry_open(&info->r_path, (file->f_flags & ~(O_CREAT | O_EXCL | O_NOCTTY)), file->f_cred);
    if (IS_ERR(real_file)) return PTR_ERR(real_file);

    file->private_data = real_file;
    return 0;
}

static int nm_release(struct inode *inode, struct file *file)
{
    struct file *real_file = file->private_data;
    if (real_file) fput(real_file), file->private_data = NULL;
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
    int ret = generic_file_mmap(file, vma);
    return ret ? ret : (file_inode(file)->i_flags &= ~S_PRIVATE, 0);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
static int nm_mmap_prepare(struct vm_area_desc *desc)
{
    int ret = generic_file_mmap_prepare(desc);
    return ret ? ret : (file_inode(desc->file)->i_flags &= ~S_PRIVATE, 0);
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
    struct inode *r_inode;

    if (unlikely(!info)) return -EIO;
    if (!info->r_path.dentry) return 0;

    r_inode = d_backing_inode(info->r_path.dentry);
    if (!r_inode || !r_inode->i_op || !r_inode->i_op->listxattr) return 0;
    return r_inode->i_op->listxattr(info->r_path.dentry, buffer, size);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
static int nm_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
#else
static int nm_getattr(IDMAP_ARG const struct path *path, struct kstat *stat, u32 request_mask, unsigned int query_flags)
#endif
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
    struct dentry *dentry = path->dentry;
#endif
    struct inode *v_inode = d_backing_inode(dentry);
    struct nm_inode_info *info = v_inode->i_private;
    int res = 0;

    if (unlikely(!info)) return -EIO;
    if (unlikely(!info->r_path.dentry))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
        generic_fillattr(IDMAP_CALL request_mask, v_inode, stat);
#else
        generic_fillattr(IDMAP_CALL v_inode, stat);
#endif
    else
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
        res = vfs_getattr_nosec(&info->r_path, stat);
#else
        res = vfs_getattr_nosec(&info->r_path, stat, request_mask, query_flags);
#endif

    if (likely(res == 0)) {
        stat->ino = v_inode->i_ino;
        stat->dev = v_inode->i_sb->s_dev;
    }
    
    return res;
}

static int nm_setattr(IDMAP_ARG struct dentry *dentry, struct iattr *attr)
{
    struct inode *v_inode = d_inode(dentry);
    struct nm_inode_info *info = v_inode->i_private;
    struct inode *r_inode;
    int err;

    if (unlikely(!info)) return -EIO;
    if (!info->r_path.dentry) return 0;

    r_inode = d_backing_inode(info->r_path.dentry);
    inode_lock(r_inode);
    err = notify_change(IDMAP_CALL info->r_path.dentry, attr, NULL);
    inode_unlock(r_inode);

    if (likely(!err)) {
        if (attr->ia_valid & ATTR_SIZE) i_size_write(v_inode, i_size_read(r_inode));
        if (attr->ia_valid & ATTR_MODE) v_inode->i_mode = r_inode->i_mode;
        if (attr->ia_valid & ATTR_UID)  v_inode->i_uid = r_inode->i_uid;
        if (attr->ia_valid & ATTR_GID)  v_inode->i_gid = r_inode->i_gid;
        nm_sync_inode_times(v_inode, r_inode);
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
    struct nm_dir_node *dir_node = info ? info->dir_node : NULL;
    struct file *real_file = file->private_data;
    uid_t fsuid = current_fsuid().val;
    bool uid_blocked = nm_is_uid_blocked(fsuid);
    int res = 0;

    if (unlikely(nm_is_virtual_pos(ctx->pos)))
        goto emit_virtual;

    if (real_file) {
        if (unlikely(uid_blocked || !dir_node))
            return nm_call_iterate(real_file, ctx, real_file->f_op);

        struct nm_proxy_ctx proxy_ctx = {
            .ctx.actor = nm_actor_proxy, .ctx.pos = ctx->pos, .orig_ctx = ctx,
            .dir_node = dir_node, .fsuid = fsuid, .emitted = false,
        };
        res = nm_call_iterate(real_file, &proxy_ctx.ctx, real_file->f_op);
        ctx->pos = proxy_ctx.ctx.pos;
        if (res < 0 || proxy_ctx.emitted) return res;
        ctx->pos = nm_pack_pos(0);
    } else if (info && !info->r_path.dentry) {
        if (ctx->pos < 2 && !dir_emit_dots(file, ctx)) return 0;
        ctx->pos = nm_pack_pos(0);
    } else {
        return -ENOTDIR;
    }

emit_virtual:
    if (likely(!uid_blocked))
        nm_emit_virtual_children(ctx, dir_node, fsuid);
    return res;
}

static struct dentry *nm_dir_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct nm_inode_info *info = dir->i_private;
    struct nm_rule_info rule_info = {0};
    struct nm_dir_node *dir_node;
    struct dentry *res;
    bool special = false, found = false, uid_blocked = false, uid_dep = false;

    if (unlikely(!info))
        goto negative_dentry;

    rcu_read_lock();
    if ((dir_node = READ_ONCE(info->dir_node))) {
        struct nm_leaf *leaf = nm_find_leaf_in_dir(dir_node, dentry->d_name.name, dentry->d_name.len);
        if (leaf) {
            struct nm_rule_array *arr = rcu_dereference(leaf->rules);
            special = nm_per_uid_view_active();
            uid_dep = arr && arr->uid_dependent;
            uid_t fsuid = (special || uid_dep) ? current_fsuid().val : 0;
            if (!special || likely(!(uid_blocked = nm_is_uid_blocked(fsuid)))) {
                special = special || uid_dep;
                found = nm_resolve_rule_from_leaf(leaf, arr, fsuid, &rule_info);
            }
        }
    }
    rcu_read_unlock();

    if (unlikely(uid_blocked))
        goto negative_dentry;

    if (found) {
        res = nm_materialize_rule(dir, dentry, &rule_info);
        if (special && !IS_ERR(res))
            nm_install_dops(res ? res : dentry, uid_dep);
        return res;
    }

    if (!info->r_path.dentry)
        goto negative_dentry;

    struct inode *r_dir = d_backing_inode(info->r_path.dentry);
    if (!r_dir)
        goto negative_dentry;

    if (r_dir->i_op->lookup) {
        res = r_dir->i_op->lookup(r_dir, dentry, flags);
        if (special && !IS_ERR(res))
            nm_install_dops(res ? res : dentry, uid_dep);
        return res;
    }
    return ERR_PTR(-EOPNOTSUPP);

negative_dentry:
    d_add(dentry, NULL);
    if (special)
        nm_install_dops(dentry, uid_dep);
    return NULL;
}

struct nm_xattr_proxy {
    struct xattr_handler fake;
    const struct xattr_handler *orig;
};

static int nm_xattr_get(const struct xattr_handler *handler, struct dentry *dentry, struct inode *inode, const char *name, void *buffer, size_t size FLAGS_ARG)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    if (nm_inode_is_virtual(inode)) {
        struct nm_inode_info *info = inode->i_private;
        if (unlikely(!info || !info->r_path.dentry)) return -ENODATA;
        return __vfs_getxattr(info->r_path.dentry, d_inode(info->r_path.dentry), xattr_full_name(handler, name), buffer, size FLAGS_VAL);
    }

    return proxy->orig->get(proxy->orig, dentry, inode, name, buffer, size FLAGS_VAL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
static int nm_xattr__get(const struct xattr_handler *handler, struct dentry *dentry,
                         struct inode *inode, const char *name, void *buffer, size_t size)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    if (nm_inode_is_virtual(inode)) {
        struct nm_inode_info *info = inode->i_private;
        if (unlikely(!info || !info->r_path.dentry)) return -ENODATA;
        return __vfs_getxattr(info->r_path.dentry, d_inode(info->r_path.dentry), xattr_full_name(handler, name), buffer, size);
    }

    if (proxy->orig->__get)
        return proxy->orig->__get(proxy->orig, dentry, inode, name, buffer, size);
    return proxy->orig->get(proxy->orig, dentry, inode, name, buffer, size);
}
#endif

static int nm_xattr_set(const struct xattr_handler *handler, IDMAP_ARG struct dentry *dentry, struct inode *inode, const char *name, const void *buffer, size_t size, int flags)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    if (nm_inode_is_virtual(inode)) {
        struct nm_inode_info *info = inode->i_private;
        if (unlikely(!info || !info->r_path.dentry)) return -ENODATA;
        return __vfs_setxattr(IDMAP_PATH(info->r_path) info->r_path.dentry, d_inode(info->r_path.dentry), xattr_full_name(handler, name), buffer, size, flags);
    }
    return proxy->orig->set(proxy->orig, IDMAP_CALL dentry, inode, name, buffer, size, flags);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
static int nm_d_revalidate(struct inode *dir, const struct qstr *name, struct dentry *dentry, unsigned int flags)
#else
static int nm_d_revalidate(struct dentry *dentry, unsigned int flags)
#endif
{
    struct nm_dops *ops;

    if (unlikely(!dentry->d_op || dentry->d_op->d_revalidate != nm_d_revalidate))
        return 1;

    ops = container_of(dentry->d_op, struct nm_dops, fake);
    if (ops->orig && ops->orig->d_revalidate) {
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
        int ret = ops->orig->d_revalidate(dir, name, dentry, flags);
    #else
        int ret = ops->orig->d_revalidate(dentry, flags);
    #endif
        if (ret <= 0) return ret;
    }
    if (likely(ops->view_uid == current_fsuid().val))
        return 1;

    if (flags & LOOKUP_RCU) return -ECHILD;
    d_drop(dentry);
    return 0;
}

#ifndef DCACHE_DONTCACHE
static int nm_d_delete(const struct dentry *dentry) { return 1; }
#endif

static void nm_d_release(struct dentry *dentry)
{
    struct nm_dops *ops = container_of(dentry->d_op, struct nm_dops, fake);
    if (ops->orig && ops->orig->d_release)
        ops->orig->d_release(dentry);
    kfree_rcu(ops, rcu);
}

static inline void nm_install_dops(struct dentry *dentry, bool uid_dependent)
{
    struct nm_dops *ops;

    if (!dentry || (dentry->d_op && dentry->d_op->d_revalidate == nm_d_revalidate))
        return;

    if (unlikely(!(ops = kzalloc(sizeof(*ops), GFP_KERNEL)))) { d_drop(dentry); return; }

    spin_lock(&dentry->d_lock);
    if (dentry->d_op && dentry->d_op->d_revalidate == nm_d_revalidate) {
        spin_unlock(&dentry->d_lock);
        kfree(ops);
        return;
    }

    ops->orig = dentry->d_op;
    if (ops->orig) ops->fake = *ops->orig;
    smp_wmb();
    ops->fake.d_revalidate = nm_d_revalidate;
    ops->fake.d_release    = nm_d_release;
#ifndef DCACHE_DONTCACHE
    if (uid_dependent) {
        ops->fake.d_delete = nm_d_delete;
        dentry->d_flags |= DCACHE_OP_DELETE;
    }
#endif
    ops->view_uid = current_fsuid().val;
    dentry->d_op = &ops->fake;
    dentry->d_flags |= DCACHE_OP_REVALIDATE;
#ifdef DCACHE_DONTCACHE
    if (uid_dependent) dentry->d_flags |= DCACHE_DONTCACHE;
#endif
    spin_unlock(&dentry->d_lock);
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
    .getattr = nm_getattr,
    .setattr = nm_setattr,
    .listxattr = nm_listxattr,
    .get_link = nm_get_link,
};

static const struct file_operations nm_dir_fops = {
    .owner = THIS_MODULE,
    .open = nm_open,
    .release = nm_release,
    .llseek = default_llseek,
    .read = generic_read_dir,
    .iterate_shared = nm_dir_iterate_dir,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
    .iterate = nm_dir_iterate_dir,
#endif
};

static const struct inode_operations nm_dir_iops = {
    .lookup = nm_dir_lookup,
    .getattr = nm_getattr,
    .setattr = nm_setattr,
    .listxattr = nm_listxattr,
};

/* --- Hijacking Management --- */

static inline int nm_hijack_superblock(struct super_block *sb)
{
    struct nm_sop *nm_sop;
    int count = 0;

    if (nm_get_nm_sop(smp_load_acquire(&sb->s_op))) return 0;
    if (!(nm_sop = kmalloc(sizeof(*nm_sop), GFP_KERNEL))) return -ENOMEM;

    nm_sop->fake_sop = *(sb->s_op);
    nm_sop->orig_sop = sb->s_op;
    nm_sop->orig_xattr = nm_sop->fake_xattr = NULL;
    nm_sop->sb = sb;
    nm_sop->fake_sop.drop_inode = nm_hijacked_drop_inode;
    nm_sop->fake_sop.evict_inode = nm_hijacked_evict_inode;

    if (sb->s_xattr) {
        const struct xattr_handler **new_array;
        struct nm_xattr_proxy *proxies;

        while (sb->s_xattr[count]) count++;
        if (!(new_array = kmalloc((count + 1) * sizeof(void *) + (count * sizeof(*proxies)), GFP_KERNEL))) {
            kfree(nm_sop);
            return -ENOMEM;
        }
        proxies = (void *)(new_array + count + 1);
        new_array[count] = NULL;
        for (int i = 0; i < count; i++) {
            proxies[i].orig = sb->s_xattr[i];
            proxies[i].fake = *sb->s_xattr[i];
            if (proxies[i].fake.get) proxies[i].fake.get = nm_xattr_get;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
            if (proxies[i].fake.__get) proxies[i].fake.__get = nm_xattr__get;
#endif
            if (proxies[i].fake.set) proxies[i].fake.set = nm_xattr_set;
            new_array[i] = &proxies[i].fake;
        }
        nm_sop->orig_xattr = (const struct xattr_handler **)sb->s_xattr;
        nm_sop->fake_xattr = new_array;
        smp_store_release((const struct xattr_handler ***)&sb->s_xattr, new_array);
        nm_debug("xattr handlers successfully hijacked for dev: 0x%x\n", sb->s_dev);
    }

    list_add_tail_rcu(&nm_sop->list, &nm_sb_list);
    smp_store_release(&sb->s_op, &nm_sop->fake_sop);
    nm_debug("Superblock successfully hijacked for dev: 0x%x\n", sb->s_dev);
    return 0;
}

static inline int nm_hijack_dir_ops(struct nm_dir_node *dir_node, struct inode *inode)
{
    const struct inode_operations *iop = smp_load_acquire(&inode->i_op);
    const struct file_operations *fop = smp_load_acquire(&inode->i_fop);
    struct nm_dir_ops *ops;
    int err;
    bool iterate = fop && (fop->iterate_shared
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
        || fop->iterate
#endif
    );

    if ((ops = nm_get_nm_iop(iop)) || (ops = nm_get_nm_fop(fop))) {
        if (ops->dir_node != dir_node) return -EBUSY;
        return 0;
    }
    if (!iop && !iterate) return -EOPNOTSUPP;
    if (!(ops = kmalloc(sizeof(*ops), GFP_KERNEL))) return -ENOMEM;
    if ((err = nm_hijack_superblock(inode->i_sb))) {
        kfree(ops);
        return err;
    }

    ops->orig_iop = iop;
    ops->orig_fop = fop;
    ops->dir_node = dir_node;
    if (iop) {
        ops->fake_iop = *iop;
        ops->fake_iop.lookup = nm_hijacked_lookup;
    }
    if (fop) {
        ops->fake_fop = *fop;
        ops->fake_fop.owner = THIS_MODULE;
        if (fop->iterate_shared) ops->fake_fop.iterate_shared = nm_hijacked_iterate_dir;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
        if (fop->iterate) ops->fake_fop.iterate = nm_hijacked_iterate_dir;
#endif
    }
    atomic_inc(&dir_node->refs);
    if (iop) smp_store_release(&inode->i_op, &ops->fake_iop);
    if (fop) smp_store_release(&inode->i_fop, &ops->fake_fop);

    nm_debug("Successfully hijacked VFS ops for parent dir (ino: %lu)\n", (unsigned long)inode->i_ino);
    return 1;
}

static void nm_restore_superblocks(void)
{
    struct nm_sop *nm_sop, *tmp;
    struct inode *inode;
    list_for_each_entry_safe(nm_sop, tmp, &nm_sb_list, list) {
        if (nm_sop->sb) {
            shrink_dcache_sb(nm_sop->sb);
            spin_lock(&nm_sop->sb->s_inode_list_lock);
            list_for_each_entry(inode, &nm_sop->sb->s_inodes, i_sb_list) {
                if (!inode->i_op && !inode->i_fop) continue;
                nm_destroy_hijacked_inode(inode);
            }
            spin_unlock(&nm_sop->sb->s_inode_list_lock);
            smp_store_release(&nm_sop->sb->s_op, nm_sop->orig_sop);
            if (nm_sop->fake_xattr) {
                smp_store_release((const struct xattr_handler ***)&nm_sop->sb->s_xattr, nm_sop->orig_xattr);
                kfree(nm_sop->fake_xattr); 
            }
            nm_debug("Successfully cured superblock for dev: 0x%x\n", nm_sop->sb->s_dev);
        }
        list_del_rcu(&nm_sop->list);
        kfree_rcu(nm_sop, rcu);
    }
}

/*** Module Management ***/

static struct nm_dir_node *nm_alloc_dir_node(void)
{
    struct nm_dir_node *dir_node = kzalloc(sizeof(*dir_node), GFP_KERNEL);
    if (dir_node) atomic_set(&dir_node->refs, 1);
    return dir_node;
}

static struct nm_child_array *nm_alloc_child_array(int count, u16 bloom_words)
{
    size_t sz = sizeof(struct nm_child_array) + bloom_words * sizeof(u64) + count * sizeof(struct nm_child);
    struct nm_child_array *array = kmalloc(sz, GFP_KERNEL);
    if (array) {
        array->count = count;
        array->bloom_words = bloom_words;
    }
    return array;
}

static void nm_publish_child_view_locked(struct nm_dir_node *dir_node, struct nm_child_array *children)
{
    struct nm_child_array *old = rcu_dereference_protected(dir_node->children, lockdep_is_held(&nm_mutex));
    rcu_assign_pointer(dir_node->children, children);
    if (!children && dir_node->pinned_dentry) {
        struct dentry *pinned = dir_node->pinned_dentry;
        dir_node->pinned_dentry = NULL;
        dput(pinned);
    }
    if (old) kfree_rcu(old, rcu);
}

static int nm_inject_child_locked(struct nm_dir_node *dir_node, struct nm_leaf *leaf, const char *name, size_t name_len)
{
    struct nm_child_array *array, *old = rcu_dereference_protected(dir_node->children, lockdep_is_held(&nm_mutex));
    int count = old ? old->count : 0, pos = 0;
    u32 full_h = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, name, name_len);
    u32 bloom_h = nm_qhash(name, name_len);

    if (old && nm_bsearch_child(old, name, name_len, full_h, &pos)) return -EEXIST;
    if (!(array = nm_alloc_child_array(count + 1, nm_bloom_words_for(count + 1)))) return -ENOMEM;

    if (old) {
        memcpy(nm_entries(array), nm_entries(old), pos * sizeof(struct nm_child));
        memcpy(nm_entries(array) + pos + 1, nm_entries(old) + pos, (count - pos) * sizeof(struct nm_child));
    }
    if (array->bloom_words) {
        if (old && array->bloom_words == old->bloom_words) {
            memcpy(array->bloom, old->bloom, old->bloom_words * sizeof(u64));
        } else {
            memset(array->bloom, 0, array->bloom_words * sizeof(u64));
            for (int i = 0; i < count; i++)
                nm_bloom_set(array, nm_entries(old)[i].bloom_hash);
        }
        nm_bloom_set(array, bloom_h);
    }
    nm_entries(array)[pos] = (struct nm_child){ .hash = full_h, .bloom_hash = bloom_h, .leaf = leaf };
    leaf->child_len = name_len;
    leaf->parent_dir = dir_node;
    atomic_inc(&dir_node->refs);
    nm_publish_child_view_locked(dir_node, array);
    return 0;
}

static struct nm_dir_node *nm_delete_child_locked(struct nm_leaf *leaf, bool clear)
{
    struct nm_dir_node *dir_node = leaf->parent_dir;
    struct nm_child_array *old, *array = NULL;
    int pos;

    if (!dir_node) return NULL;
    if (!(old = rcu_dereference_protected(dir_node->children, lockdep_is_held(&nm_mutex)))) return dir_node;
    if (!clear) {
        for (pos = 0; pos < old->count && nm_entries(old)[pos].leaf != leaf; pos++) {}
        if (pos == old->count) return dir_node;
        if (old->count > 1) {
            int new_count = old->count - 1;
            if (!(array = nm_alloc_child_array(new_count, nm_bloom_words_for(new_count)))) return ERR_PTR(-ENOMEM);
            memcpy(nm_entries(array), nm_entries(old), pos * sizeof(struct nm_child));
            memcpy(nm_entries(array) + pos, nm_entries(old) + pos + 1, (old->count - pos - 1) * sizeof(struct nm_child));
            if (array->bloom_words) {
                memset(array->bloom, 0, array->bloom_words * sizeof(u64));
                for (int i = 0; i < array->count; i++)
                    nm_bloom_set(array, nm_entries(array)[i].bloom_hash);
            }
        }
    }
    nm_publish_child_view_locked(dir_node, array);
    return dir_node;
}

static struct nm_leaf *nm_find_leaf(const char *path, u16 len)
{
    struct nm_leaf *leaf;
    u32 hash = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, path, len);
    hash_for_each_possible(nm_leaves_ht, leaf, ht_node, hash) {
        if (leaf->v_len == len && !memcmp(leaf->paths, path, len)) return leaf;
    }
    return NULL;
}

static struct nm_rule_array *nm_alloc_rule_array(int count, size_t heap_size)
{
    struct nm_rule_array *arr = kmalloc(sizeof(struct nm_rule_array) + count * sizeof(struct nm_rule) + heap_size, GFP_KERNEL);
    if (arr) arr->count = count;
    return arr;
}

static int nm_prepare_rule(const char *r_path, u16 r_len, u32 flags, unsigned int uid, struct nm_rule *out)
{
    bool backed = !(flags & (NM_FLAG_WHITEOUT | NM_FLAG_VIRTUAL_DIR));
    if (backed && !r_path) return -EINVAL;
    if (backed) while (r_len > 1 && r_path[r_len - 1] == '/') r_len--;
    else r_len = 0;

    memset(out, 0, sizeof(*out));
    out->r_len = r_len;
    out->flags = flags;
    out->target_uid = uid;
    if (backed) {
        int err = kern_path(r_path, LOOKUP_FOLLOW, &out->r_path);
        if (err) { memset(out, 0, sizeof(*out)); return err; }
        struct inode *inode = d_backing_inode(out->r_path.dentry);
        if (S_ISDIR(inode->i_mode)) out->flags |= NM_FLAG_IS_DIR;
    }
    return 0;
}

static inline char *nm_copy_rule(struct nm_rule *dst, const struct nm_rule *src, char *dst_h, const char *src_h)
{
    *dst = *src;
    if (dst->r_path.dentry) path_get(&dst->r_path);
    memcpy(dst_h, src_h, src->r_len);
    return dst_h + src->r_len;
}

static struct nm_leaf *nm_alloc_leaf(const char *path, u16 len)
{
    struct nm_leaf *leaf = kzalloc(sizeof(*leaf) + len + 1, GFP_KERNEL);
    if (!leaf) return ERR_PTR(-ENOMEM);

    memcpy(leaf->paths, path, len);
    leaf->v_len = len;
    leaf->v_hash = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, path, len);
    leaf->v_ino = (unsigned long)leaf->v_hash;
    return leaf;
}

static int nm_resolve_anchor_locked(struct nm_leaf *leaf)
{
    struct path anchor;
    struct inode *inode;
    struct nm_dir_ops *iop, *fop;
    struct nm_dir_node *dir;

    if (kern_path(leaf->paths, LOOKUP_FOLLOW, &anchor)) {
        leaf->this_dir = nm_alloc_dir_node();
        if (!leaf->this_dir) return -ENOMEM;
        leaf->this_dir->owner = leaf;
        return 0;
    }

    inode = d_backing_inode(anchor.dentry);
    if (!S_ISDIR(inode->i_mode)) {
        path_put(&anchor);
        return 0;
    }

    leaf->v_ino = inode->i_ino;
    d_drop(anchor.dentry);
    leaf->anchor = anchor;

    if (inode->i_op != &nm_dir_iops) {
        iop = nm_get_nm_iop(smp_load_acquire(&inode->i_op));
        fop = nm_get_nm_fop(smp_load_acquire(&inode->i_fop));
        dir = iop ? iop->dir_node : (fop ? fop->dir_node : NULL);
        if (dir && !dir->owner) {
            leaf->this_dir = dir;
            atomic_inc(&dir->refs);
            dir->owner = leaf;
            return 0;
        }
    }

    leaf->this_dir = nm_alloc_dir_node();
    if (!leaf->this_dir) return -ENOMEM;
    leaf->this_dir->owner = leaf;
    return 0;
}

static void nm_invalidate_vpath(const char *v_path, size_t v_len)
{
    struct path parent_path;
    struct dentry *child;
    char *parent_buf;
    int slash;

    while (v_len > 1 && v_path[v_len - 1] == '/') v_len--;
    for (slash = v_len - 1; slash > 0; slash--)
        if (v_path[slash] == '/') break;
    if (slash == 0) return;

    parent_buf = kmalloc(slash + 1, GFP_KERNEL);
    if (!parent_buf) return;
    memcpy(parent_buf, v_path, slash);
    parent_buf[slash] = '\0';

    if (kern_path(parent_buf, LOOKUP_FOLLOW, &parent_path) != 0) {
        kfree(parent_buf);
        return;
    }
    kfree(parent_buf);

    child = nm_hash_and_lookup(parent_path.dentry, &(struct qstr)QSTR_INIT(v_path + slash + 1, v_len - slash - 1));
    if (child) {
        d_drop(child);
        dput(child);
    }

    path_put(&parent_path);
}

static void nm_invalidate_all_leaves(void)
{
    struct nm_leaf *leaf;
    int bkt;

    hash_for_each(nm_leaves_ht, bkt, leaf, ht_node)
        nm_invalidate_vpath(leaf->paths, leaf->v_len);
}

static void nm_free_leaf(struct nm_leaf *leaf)
{
    if (leaf->anchor.dentry) path_put(&leaf->anchor);
    if (leaf->this_dir) {
        if (leaf->this_dir->owner == leaf) leaf->this_dir->owner = NULL;
        nm_dir_put(leaf->this_dir);
    }
    nm_dir_put(leaf->parent_dir);
    kfree(leaf);
}

static void nm_rule_array_free(struct nm_rule_array *arr)
{
    for (int i = 0; i < arr->count; i++)
        if (arr->rules[i].r_path.dentry) path_put(&arr->rules[i].r_path);
    kfree(arr);
}

static void nm_reclaim(struct work_struct *work)
{
    struct nm_rule_array *arrays;
    struct nm_leaf *leaves;
    mutex_lock(&nm_mutex);
    arrays = nm_retired_arrays;
    leaves = nm_retired_leaves;
    nm_retired_arrays = NULL;
    nm_retired_leaves = NULL;
    mutex_unlock(&nm_mutex);
    if (!arrays && !leaves) return;

    synchronize_rcu();
    while (arrays) {
        struct nm_rule_array *arr = arrays;
        arrays = arr->next;
        nm_rule_array_free(arr);
    }
    while (leaves) {
        struct nm_leaf *leaf = leaves;
        leaves = leaf->next;
        nm_free_leaf(leaf);
    }
}

static DECLARE_WORK(nm_reclaim_work, nm_reclaim);

static void nm_retire_array_locked(struct nm_rule_array *arr)
{
    arr->next = nm_retired_arrays;
    nm_retired_arrays = arr;
    schedule_work(&nm_reclaim_work);
}

static void nm_retire_leaf_locked(struct nm_leaf *leaf)
{
    struct nm_rule_array *array = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex));
    RCU_INIT_POINTER(leaf->rules, NULL);
    leaf->next = nm_retired_leaves;
    nm_retired_leaves = leaf;
    if (array) nm_retire_array_locked(array);
    else schedule_work(&nm_reclaim_work);
}

static int __nm_generate_virtual_topology(struct nm_leaf *target, unsigned int uid)
{
    struct nm_leaf *child = target, *parent;
    struct hlist_node *h_tmp;
    int len = target->v_len, err = 0;
    HLIST_HEAD(pending);

    while (len > 1) {
        int slash = len - 1;
        while (slash > 0 && target->paths[slash] != '/') slash--;
        int parent_len = slash ? slash : 1;
        const char *name = target->paths + slash + 1;
        int name_len = len - slash - 1;

        if ((parent = nm_find_leaf(target->paths, parent_len))) {
            struct nm_rule *rule = nm_select_rule(rcu_dereference_protected(parent->rules, lockdep_is_held(&nm_mutex)), uid);
            int hijacked = 0;
            if ((rule && !(rule->flags & NM_FLAG_IS_DIR)) || !parent->this_dir) { err = -ENOTDIR; break; }
            if (parent->anchor.dentry && (hijacked = nm_hijack_dir_ops(parent->this_dir, d_backing_inode(parent->anchor.dentry))) < 0) {
                err = hijacked;
                break;
            }
            err = nm_inject_child_locked(parent->this_dir, child, name, name_len);
            if (!err && parent->anchor.dentry) {
                if (hijacked > 0) shrink_dcache_parent(parent->anchor.dentry);
                struct dentry *dentry = nm_hash_and_lookup(parent->anchor.dentry, &(struct qstr)QSTR_INIT(name, name_len));
                if (dentry) { d_drop(dentry); dput(dentry); }
            }
            break;
        }

        char saved = target->paths[parent_len];
        target->paths[parent_len] = '\0';
        struct path path;
        int found = kern_path(target->paths, LOOKUP_FOLLOW, &path);
        target->paths[parent_len] = saved;
        if (!found) {
            struct inode *inode = d_backing_inode(path.dentry);
            struct nm_dir_node *dir = NULL;
            int hijacked = 0;
            bool virtual = inode->i_op == &nm_dir_iops;
            if (!S_ISDIR(inode->i_mode)) {
                err = -ENOTDIR;
            } else if (virtual) {
                dir = ((struct nm_inode_info *)inode->i_private)->dir_node;
                atomic_inc(&dir->refs);
            } else {
                struct nm_dir_ops *iop = nm_get_nm_iop(smp_load_acquire(&inode->i_op));
                struct nm_dir_ops *fop = nm_get_nm_fop(smp_load_acquire(&inode->i_fop));
                dir = iop ? iop->dir_node : (fop ? fop->dir_node : NULL);
                if (dir) atomic_inc(&dir->refs);
                else dir = nm_alloc_dir_node();
                if (!dir) err = -ENOMEM;
            }
            if (!err && !virtual && (hijacked = nm_hijack_dir_ops(dir, inode)) < 0)
                err = hijacked;
            if (!err && !(err = nm_inject_child_locked(dir, child, name, name_len))) {
                if (!virtual && !dir->owner && !dir->pinned_dentry) dir->pinned_dentry = dget(path.dentry);
                if (hijacked > 0) shrink_dcache_parent(path.dentry);
                struct dentry *child = nm_hash_and_lookup(path.dentry, &(struct qstr)QSTR_INIT(name, name_len));
                if (child) { d_drop(child); dput(child); }
            }
            nm_dir_put(dir);
            path_put(&path);
            break;
        }

        parent = nm_alloc_leaf(target->paths, parent_len);
        if (IS_ERR(parent)) { err = PTR_ERR(parent); break; }

        if (!(parent->this_dir = nm_alloc_dir_node())) {
            nm_free_leaf(parent);
            err = -ENOMEM;
            break;
        }
        parent->this_dir->owner = parent;
        struct nm_rule_array *rules = nm_alloc_rule_array(1, 0);
        if (!rules) {
            err = -ENOMEM;
            nm_free_leaf(parent);
            break;
        }
        memset(&rules->rules[0], 0, sizeof(struct nm_rule));
        rules->rules[0].flags = NM_FLAG_IS_DIR | NM_FLAG_VIRTUAL_DIR;
        rules->uid_dependent = false;
        RCU_INIT_POINTER(parent->rules, rules);

        hlist_add_head(&parent->ht_node, &pending);
        err = nm_inject_child_locked(parent->this_dir, child, name, name_len);
        if (err) break;
        child = parent;
        len = parent_len;
    }

    hlist_for_each_entry_safe(parent, h_tmp, &pending, ht_node) {
        hlist_del(&parent->ht_node);
        if (!err) hash_add_rcu(nm_leaves_ht, &parent->ht_node, parent->v_hash);
        else {
            kfree(rcu_access_pointer(parent->rules));
            nm_free_leaf(parent);
        }
    }
    return err;
}

static struct nm_dir_node *nm_detach_leaf_locked(struct nm_leaf *leaf)
{
    struct nm_dir_node *parent = nm_delete_child_locked(leaf, false);
    if (IS_ERR(parent)) return parent;
    if (leaf->this_dir) leaf->this_dir->owner = NULL;
    hash_del_rcu(&leaf->ht_node);
    nm_retire_leaf_locked(leaf);
    return parent;
}

static int __nm_prune_empty_virtual_dirs(struct nm_dir_node *dir)
{
    struct nm_leaf *leaf;
    while (dir && !rcu_access_pointer(dir->children) && (leaf = dir->owner)) {
        struct nm_rule_array *rules = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex));
        if (rules && (rules->count != 1 || !(rules->rules[0].flags & NM_FLAG_VIRTUAL_DIR))) break;
        dir = nm_detach_leaf_locked(leaf);
        if (IS_ERR(dir)) return PTR_ERR(dir);
    }
    return 0;
}

/*** Rule Operations ***/

static int nm_add_rule_locked(const char *v_path, const char *r_path, u16 v_len, u16 r_len, u32 flags, unsigned int uid)
{
    struct nm_rule new_rule;
    struct nm_leaf *leaf;
    struct nm_rule_array *old, *array;
    struct nm_dir_node *new_dir = NULL;
    int pos = 0, count, new_count, err = 0, j = 0;
    size_t heap_size = 0;
    bool fresh, replace;

    char *r_buf = NULL;
    if (r_path && r_len) {
        if (!(r_buf = kmalloc(r_len + 1, GFP_KERNEL))) return -ENOMEM;
        memcpy(r_buf, r_path, r_len);
        r_buf[r_len] = '\0';
    }

    err = nm_prepare_rule(r_buf, r_len, flags, uid, &new_rule);
    if (err) { kfree(r_buf); return err; }

    while (v_len > 1 && v_path[v_len - 1] == '/') v_len--;

    fresh = !(leaf = nm_find_leaf(v_path, v_len));
    if (fresh && IS_ERR((leaf = nm_alloc_leaf(v_path, v_len)))) {
        err = PTR_ERR(leaf);
        goto out_rule;
    }

    if (fresh && (new_rule.flags & NM_FLAG_IS_DIR)) {
        if ((err = nm_resolve_anchor_locked(leaf))) {
            nm_free_leaf(leaf);
            goto out_rule;
        }
    }

    count = (old = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex))) ? old->count : 0;
    while (pos < count && old->rules[pos].target_uid < uid) pos++;
    replace = pos < count && old->rules[pos].target_uid == uid;
    new_count = count + !replace;

    if ((new_rule.flags & NM_FLAG_IS_DIR) && !leaf->this_dir && !(new_dir = nm_alloc_dir_node())) {
        err = -ENOMEM;
        goto out_leaf;
    }

    if (old) {
        for (int i = 0; i < count; i++) {
            if (replace && i == pos) continue;
            heap_size += old->rules[i].r_len;
        }
    }
    heap_size += new_rule.r_len;

    if (!(array = nm_alloc_rule_array(new_count, heap_size))) {
        err = -ENOMEM;
        goto out_dir;
    }

    char *new_h = nm_rule_heap(array);
    if (old) {
        const char *old_h = nm_rule_heap(old);
        for (int i = 0; i < pos; i++, j++) {
            new_h = nm_copy_rule(&array->rules[j], &old->rules[i], new_h, old_h);
            old_h += old->rules[i].r_len;
        }
        if (replace) old_h += old->rules[pos].r_len;

        array->rules[j] = new_rule;
        memcpy(new_h, r_buf, new_rule.r_len);
        new_h += new_rule.r_len; j++;

        for (int i = pos + replace; i < count; i++, j++) {
            new_h = nm_copy_rule(&array->rules[j], &old->rules[i], new_h, old_h);
            old_h += old->rules[i].r_len;
        }
    } else {
        array->rules[0] = new_rule;
        memcpy(new_h, r_buf, new_rule.r_len);
    }

    kfree(r_buf);
    r_buf = NULL;

    array->uid_dependent = array->count && array->rules[array->count - 1].target_uid != 0;
    if (fresh) {
        RCU_INIT_POINTER(leaf->rules, array);
        if ((err = __nm_generate_virtual_topology(leaf, uid))) {
            nm_rule_array_free(array);
            nm_free_leaf(leaf);
            return err;
        }
        hash_add_rcu(nm_leaves_ht, &leaf->ht_node, leaf->v_hash);
    } else {
        if (new_dir) { leaf->this_dir = new_dir; new_dir->owner = leaf; }
        rcu_assign_pointer(leaf->rules, array);
        if (old) nm_retire_array_locked(old);
    }

    return 0;

out_dir:  nm_dir_put(new_dir);
out_leaf: if (fresh) nm_free_leaf(leaf);
out_rule: if (new_rule.r_path.dentry) path_put(&new_rule.r_path);
          kfree(r_buf);
          return err;
}

static int nm_delete_rule_locked(const char *path, u16 len, unsigned int uid)
{
    struct nm_leaf *leaf;
    struct nm_rule_array *old, *array = NULL;
    int pos = 0, new_count;
    size_t heap_size = 0;
    bool stub_virtual = false;

    while (len > 1 && path[len - 1] == '/') len--;
    if (!(leaf = nm_find_leaf(path, len)) || !(old = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex)))) return 0;
    while (pos < old->count && old->rules[pos].target_uid != uid) pos++;
    if (pos == old->count) return 0;

    if (leaf->this_dir && rcu_access_pointer(leaf->this_dir->children) &&
        ((old->rules[pos].flags & NM_FLAG_IS_DIR) || !uid || old->rules[0].target_uid)) {
        if (old->rules[pos].flags & NM_FLAG_VIRTUAL_DIR) return 0;
        stub_virtual = true;
    }

    if (!stub_virtual && old->count == 1 && (!leaf->this_dir || !rcu_access_pointer(leaf->this_dir->children))) {
        struct nm_dir_node *parent = nm_detach_leaf_locked(leaf);
        if (IS_ERR(parent)) return PTR_ERR(parent);
        return parent ? __nm_prune_empty_virtual_dirs(parent) : 0;
    }

    new_count = stub_virtual ? old->count : old->count - 1;
    for (int i = 0; i < old->count; i++) {
        if (i == pos) continue;
        heap_size += old->rules[i].r_len;
    }

    if (!(array = nm_alloc_rule_array(new_count, heap_size))) return -ENOMEM;

    char *new_h = nm_rule_heap(array);
    const char *old_h = nm_rule_heap(old);
    for (int i = 0, j = 0; i < old->count; i++) {
        if (!stub_virtual && i == pos) { old_h += old->rules[i].r_len; continue; }
        if (stub_virtual && i == pos) {
            array->rules[j].r_path = (struct path){ .mnt = NULL, .dentry = NULL };
            array->rules[j].target_uid = uid;
            array->rules[j].r_len = 0;
            array->rules[j].flags = NM_FLAG_IS_DIR | NM_FLAG_VIRTUAL_DIR;
            j++; old_h += old->rules[i].r_len;
            continue;
        }
        new_h = nm_copy_rule(&array->rules[j], &old->rules[i], new_h, old_h);
        old_h += old->rules[i].r_len;
        j++;
    }

    array->uid_dependent = array->count && array->rules[array->count - 1].target_uid != 0;
    rcu_assign_pointer(leaf->rules, array);
    nm_retire_array_locked(old);
    return 0;
}

static void nm_clear_state(int clear_flags)
{
    mutex_lock(&nm_mutex);
    if (clear_flags & NM_CLEAR_UIDS) {
        struct nm_uid_array *old = rcu_dereference_protected(nm_uids, lockdep_is_held(&nm_mutex));
        if (old) {
            RCU_INIT_POINTER(nm_uids, NULL);
            kfree_rcu(old, rcu);
            nm_invalidate_all_leaves();
        }
    }
    if (clear_flags & NM_CLEAR_RULES) {
        struct nm_leaf *leaf;
        struct hlist_node *tmp;
        int bkt;
        hash_for_each_safe(nm_leaves_ht, bkt, tmp, leaf, ht_node) {
            nm_delete_child_locked(leaf, true);
            if (leaf->this_dir) leaf->this_dir->owner = NULL;
            nm_invalidate_vpath(leaf->paths, leaf->v_len);
            hash_del_rcu(&leaf->ht_node);
            nm_retire_leaf_locked(leaf);
        }
    }
    if (clear_flags & NM_CLEAR_EXIT) nm_restore_superblocks();
    mutex_unlock(&nm_mutex);
}

/*** Payload Communication API ***/

static int nm_process_payload(unsigned long user_addr)
{
    struct nm_payload *payload;
    struct page *page;
    unsigned long pg_off = offset_in_page(user_addr);
    char *buf_ptr, *buf_end;

    if (pg_off + sizeof(*payload) > PAGE_SIZE || get_user_pages_fast(user_addr, 1, FOLL_WRITE, &page) != 1) 
        return -EFAULT;

    if ((payload = (void *)((char *)kmap(page) + pg_off))->magic != NM_MAGIC_SIG) {
        kunmap(page);
        put_page(page);
        return -EFAULT;
    }

    payload->status = 0;

    switch (payload->cmd) {
        case NM_CMD_GET_VERSION:
            memcpy(payload->buffer, NOMOUNT_VERSION, (payload->data_size = strlen(NOMOUNT_VERSION)));
            break;

        case NM_CMD_ADD_RULE: {
            unsigned int data_size = payload->data_size, offset = payload->arg1;
            char *batch_start, *batch_end;
            if (data_size > sizeof(payload->buffer) || offset > data_size) { payload->status = -EINVAL; break; }
            buf_ptr = payload->buffer + offset;
            buf_end = payload->buffer + data_size;
            batch_start = buf_ptr;
            mutex_lock(&nm_mutex);
            while (buf_ptr < buf_end) {
                struct nm_rule_hdr h;
                int err;
                if ((size_t)(buf_end - buf_ptr) < sizeof(h)) {
                    if (!payload->status) payload->status = -EINVAL;
                    break;
                }
                h = *(struct nm_rule_hdr *)buf_ptr;
                if (unlikely(!h.v_len || h.v_len >= PATH_MAX || h.r_len >= PATH_MAX) ||
                        h.v_len + h.r_len > (size_t)(buf_end - buf_ptr) - sizeof(h)) {
                    if (!payload->status) payload->status = -EINVAL;
                    break;
                }
                buf_ptr += sizeof(h);
                err = nm_add_rule_locked(buf_ptr, buf_ptr + h.v_len, h.v_len, h.r_len, h.flags, h.uid);
                if (!payload->status) payload->status = err;
                buf_ptr += (size_t)(h.v_len + h.r_len);
            }
            batch_end = buf_ptr;
            payload->arg1 = batch_end - payload->buffer;

            buf_ptr = batch_start;
            while (buf_ptr < batch_end) {
                struct nm_rule_hdr h;
                if ((size_t)(batch_end - buf_ptr) < sizeof(h)) break;
                h = *(struct nm_rule_hdr *)buf_ptr;
                if (unlikely(!h.v_len || h.v_len >= PATH_MAX)) break;
                buf_ptr += sizeof(h);
                nm_invalidate_vpath(buf_ptr, h.v_len);
                buf_ptr += (size_t)(h.v_len + h.r_len);
            }

            mutex_unlock(&nm_mutex);
            break;
        }

        case NM_CMD_DEL_RULE: {
            unsigned int data_size = payload->data_size, offset = payload->arg1;
            char *batch_start, *batch_end;
            if (data_size > sizeof(payload->buffer) || offset > data_size) { payload->status = -EINVAL; break; }
            buf_ptr = payload->buffer + offset;
            buf_end = payload->buffer + data_size;
            batch_start = buf_ptr;
            mutex_lock(&nm_mutex);
            while (buf_ptr < buf_end) {
                struct nm_del_hdr h;
                int err;
                if ((size_t)(buf_end - buf_ptr) < sizeof(h)) { payload->status = -EINVAL; break; }
                h = *(struct nm_del_hdr *)buf_ptr;
                if (!h.v_len || h.v_len > (size_t)(buf_end - buf_ptr) - sizeof(h)) { payload->status = -EINVAL; break; }
                buf_ptr += sizeof(h);
                err = nm_delete_rule_locked(buf_ptr, h.v_len, h.uid);
                if (!payload->status) payload->status = err;
                buf_ptr += h.v_len;
            }
            batch_end = buf_ptr;
            payload->arg1 = batch_end - payload->buffer;

            buf_ptr = batch_start;
            while (buf_ptr < batch_end) {
                struct nm_del_hdr h;
                if ((size_t)(batch_end - buf_ptr) < sizeof(h)) break;
                h = *(struct nm_del_hdr *)buf_ptr;
                if (!h.v_len) break;
                buf_ptr += sizeof(h);
                nm_invalidate_vpath(buf_ptr, h.v_len);
                buf_ptr += h.v_len;
            }

            mutex_unlock(&nm_mutex);
            break;
        }

        case NM_CMD_ADD_UID:
            mutex_lock(&nm_mutex);
            payload->status = nm_uid_add(payload->target_uid);
            if (payload->status == 0)
                nm_invalidate_all_leaves();
            mutex_unlock(&nm_mutex);
            break;

        case NM_CMD_DEL_UID:
            mutex_lock(&nm_mutex);
            payload->status = nm_uid_del(payload->target_uid);
            if (payload->status == 0)
                nm_invalidate_all_leaves();
            mutex_unlock(&nm_mutex);
            break;

        case NM_CMD_BLOCK_ISOLATED_UIDS:
            if (READ_ONCE(nm_block_isolated_uids) != (payload->arg1 != 0)) {
                mutex_lock(&nm_mutex);
                WRITE_ONCE(nm_block_isolated_uids, payload->arg1 != 0);
                nm_invalidate_all_leaves();
                mutex_unlock(&nm_mutex);
            }
            break;

        case NM_CMD_GET_ISOLATED_STATE:
            payload->buffer[0] = READ_ONCE(nm_block_isolated_uids) ? '1' : '0';
            payload->buffer[1] = '\n';
            payload->data_size = 2;
            break;

        case NM_CMD_CLEAR_ALL:
        case NM_CMD_CLEAR_UIDS:
        case NM_CMD_CLEAR_RULES:
            nm_clear_state((payload->cmd == NM_CMD_CLEAR_ALL) ? (NM_CLEAR_UIDS | NM_CLEAR_RULES) :
                           (payload->cmd == NM_CMD_CLEAR_UIDS) ? NM_CLEAR_UIDS : NM_CLEAR_RULES);
            break;

        case NM_CMD_GET_LIST: {
            int current_idx = 0;
            int bkt;
            buf_ptr = payload->buffer;
            buf_end = payload->buffer + sizeof(payload->buffer);

            rcu_guard();
            struct nm_leaf *leaf;
            hash_for_each_rcu(nm_leaves_ht, bkt, leaf, ht_node) {
                struct nm_rule_array *rules = rcu_dereference(leaf->rules);
                if (!rules) continue;
                const char *heap = nm_rule_heap(rules);
                for (int i = 0; i < rules->count; i++) {
                    struct nm_rule *r = &rules->rules[i];
                    if (current_idx++ < payload->arg1) { heap += r->r_len; continue; }
                    if ((sizeof(struct nm_rule_hdr) + leaf->v_len + r->r_len) > (size_t)(buf_end - buf_ptr)) { current_idx--; goto list_done; }
                    *(struct nm_rule_hdr *)buf_ptr = (struct nm_rule_hdr){ .flags = r->flags, .uid = r->target_uid, .v_len = leaf->v_len, .r_len = r->r_len };
                    buf_ptr += sizeof(struct nm_rule_hdr);
                    memcpy(buf_ptr, leaf->paths, leaf->v_len); buf_ptr += leaf->v_len;
                    if (r->r_len > 0) { memcpy(buf_ptr, heap, r->r_len); buf_ptr += r->r_len; }
                    heap += r->r_len;
                }
            }
        list_done:
            payload->data_size = buf_ptr - payload->buffer;
            payload->arg1 = current_idx;
            break;
        }

        case NM_CMD_GET_UIDS: {
            u32 *out = (u32 *)payload->buffer;
            u32 start_idx = payload->arg1;
            struct nm_uid_array *arr;
            int count = 0;

            rcu_guard();
            if ((arr = rcu_dereference(nm_uids))) {
                while (start_idx < arr->count && count < (sizeof(payload->buffer) / sizeof(*out))) {
                    out[count++] = arr->uids[start_idx++];
                }
            }
            payload->data_size = count * sizeof(*out);
            payload->arg1 = start_idx;
            break;
        }
    }

    kunmap(page);
    set_page_dirty_lock(page);
    put_page(page);
    return 0;
}

static int nm_key_preparse(struct key_preparsed_payload *prep)
{
    unsigned long user_addr = 0;
    if (!capable(CAP_SYS_ADMIN)) return -EPERM;
    if (prep->datalen == 8) user_addr = *(u64 *)prep->data;
    else if (prep->datalen == 4) user_addr = *(u32 *)prep->data;
    else return -EINVAL;
    if (user_addr) nm_process_payload(user_addr);
    return -ECANCELED;
}

static int dummy_key_instantiate(struct key *key, struct key_preparsed_payload *prep) { return -EINVAL; }
static void dummy_key_free_preparse(struct key_preparsed_payload *prep) { }

static struct key_type nm_key_type = {
    .name = "nomount",
    .preparse = nm_key_preparse,
    .free_preparse = dummy_key_free_preparse,
    .instantiate = dummy_key_instantiate,
};

static int __init nomount_init(void)
{
    int ret = register_key_type(&nm_key_type);
    if (ret)
        nm_err("Failed to register key type (err: %d)\n", ret);
    else
        nm_info("Loaded successfully\n");
    return ret;
}

static void __exit nomount_exit(void)
{
    unregister_key_type(&nm_key_type);
    nm_clear_state(NM_CLEAR_UIDS | NM_CLEAR_RULES | NM_CLEAR_EXIT);
    flush_work(&nm_reclaim_work);
    rcu_barrier();
    nm_info("Unloaded successfully\n");
}

MODULE_LICENSE("GPL");
MODULE_VERSION(NOMOUNT_VERSION);
MODULE_AUTHOR("maxsteeel");
MODULE_DESCRIPTION("NoMount Path Redirection VFS Subsystem");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver");
MODULE_IMPORT_NS("ANDROID_GKI_VFS_EXPORT_ONLY");
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_IMPORT_NS(ANDROID_GKI_VFS_EXPORT_ONLY);
#endif

fs_initcall(nomount_init);
module_exit(nomount_exit);
