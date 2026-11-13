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

static __always_inline u32 nm_child_frag(u32 hash)
{
    return (hash >> 16) & 0xFFFF;
}

static bool nm_rules_overlap(const struct nm_rule *a, const struct nm_rule *b)
{
    unsigned int lo = max(a->target_uid, b->target_uid), hi = min(a->uid_end, b->uid_end);
    if (lo > hi) return false;
    if (a->deny_start > b->deny_start) swap(a, b);
    if (a->deny_start && a->deny_start <= lo && lo <= a->deny_end) {
        if (a->deny_end >= hi) return false;
        lo = a->deny_end + 1;
    }
    return !(b->deny_start && b->deny_start <= lo && b->deny_end >= hi);
}

static struct nm_rule *nm_select_rule(struct nm_leaf *leaf, unsigned int uid)
{
    struct nm_rule_array *arr = rcu_dereference_check(leaf->rules, lockdep_is_held(&nm_mutex));
    struct nm_rule *fallback = NULL;
    if (!arr) return NULL;
    if (!arr->uid_count) return arr->rules;

    struct nm_rule *r = arr->rules, *end = r + arr->count;
    if (arr->uid_count != arr->count) fallback = r++;
    while (r < end) {
        if (r->target_uid > uid) break;
        if (uid <= r->uid_end) {
            if (r->deny_start && uid >= r->deny_start && uid <= r->deny_end) { r++; continue; }
            return r;
        }
        r++;
    }
    return fallback;
}

static struct nm_leaf *nm_find_leaf_in_dir(struct nm_dir_node *dir_node, const char *name, size_t len)
{
    struct nm_child_array *children;
    struct nm_child *entries;
    u32 hash, bucket, i;

    if (!dir_node || !(children = rcu_dereference_check(dir_node->children, lockdep_is_held(&nm_mutex))))
        return NULL;
    entries = nm_entries(children);
    hash = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, name, len);
    if (!children->bucket_slots) {
        for (i = 0; i < children->count; i++)
            if (entries[i].hash == hash && nm_child_matches(entries[i].leaf, name, len))
                return entries[i].leaf;
        return NULL;
    }
    bucket = nm_child_bucket(children, hash);
    for (i = children->starts[bucket]; i < children->starts[bucket + 1]; i++)
        if (entries[i].hash == hash && nm_child_matches(entries[i].leaf, name, len))
            return entries[i].leaf;
    return NULL;
}

static bool nm_backing_gone(struct dentry *d)
{
    struct inode *inode = READ_ONCE(d->d_inode);
    return !inode || inode->i_nlink == 0;
}

static bool nm_resolve_rule_from_leaf(struct nm_leaf *leaf, uid_t fsuid, struct nm_rule_info *out)
{
    struct nm_rule *rule = nm_select_rule(leaf, fsuid);
    if (!rule) return false;

    out->flags = rule->flags;
    out->v_ino = leaf->v_ino;
    out->r_path = rule->r_path;
    out->this_dir = (rule->flags & NM_FLAG_IS_DIR) ? leaf->this_dir : NULL;
    out->key = (unsigned long)(out->this_dir ? (void *)out->this_dir : (void *)leaf);
    out->leaf = (rule->flags & (NM_FLAG_IS_DIR | NM_FLAG_WHITEOUT)) ? NULL : leaf;
    if (out->leaf) atomic_inc(&out->leaf->refs);
    if (out->r_path.dentry) path_get(&out->r_path);
    if (out->this_dir) atomic_inc(&out->this_dir->refs);
    return true;
}

static int nm_rebind_path(struct nm_rule_info *rule_info)
{
    struct dentry *dentry = rule_info->r_path.dentry;
    struct inode *old_inode = d_backing_inode(dentry), *new_inode;
    struct dentry *parent, *new;
    const char *name;
    unsigned seq;
    u16 len;

    if (!old_inode) return -ENOENT;
    if (!nm_backing_gone(dentry)) return 0;

    for (;;) {
        seq = raw_seqcount_begin(&dentry->d_seq);
        parent = dget_parent(dentry);
        name = dentry->d_name.name;
        len = dentry->d_name.len;
        if (!read_seqcount_retry(&dentry->d_seq, seq)) break;
        dput(parent);
    }

    new = nm_lookup_one_len_unlocked(name, parent, len);
    dput(parent);
    if (IS_ERR(new)) return PTR_ERR(new);

    new_inode = d_backing_inode(new);
    if (!d_is_positive(new) || new->d_sb != dentry->d_sb || !new_inode || nm_inode_is_virtual(new_inode) || d_mountpoint(new) || ((new_inode->i_mode ^ old_inode->i_mode) & S_IFMT)) {
        dput(new);
        return -ENOENT;
    }

    dput(rule_info->r_path.dentry);
    rule_info->r_path.dentry = new;
    return 0;
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
    if (info->r_file) fput(info->r_file);
    if (inode->i_op == &nm_file_iops) nm_leaf_put(info->leaf);
    else nm_dir_put(info->dir_node);
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
    bool native_view;
};

static bool nm_rule_needs_readdir(struct nm_leaf *leaf, struct nm_rule *rule)
{
    return !leaf->native_file || rule->flags || !S_ISREG(d_backing_inode(rule->r_path.dentry)->i_mode);
}

static NM_ACTOR_RET nm_actor_proxy(struct dir_context *ctx, const char *name, int namelen, loff_t offset, u64 ino, unsigned int d_type)
{
    struct nm_proxy_ctx *proxy = container_of(ctx, struct nm_proxy_ctx, ctx);
    struct dir_context *orig_ctx = proxy->orig_ctx;
    NM_ACTOR_RET ret;

    {
        rcu_guard();
        struct nm_leaf *leaf = nm_find_leaf_in_dir(proxy->dir_node, name, namelen);
        struct nm_rule *rule = leaf ? nm_select_rule(leaf, proxy->fsuid) : NULL;
        if (unlikely(rule && (!proxy->native_view || nm_rule_needs_readdir(leaf, rule)))) {
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

static inline void nm_emit_virtual_children(struct dir_context *ctx, struct nm_dir_node *dir_node, uid_t fsuid, bool native_view)
{
	if (!dir_node) return;

	u32 index = 0;
	if (nm_is_virtual_pos(ctx->pos)) {
		index = nm_unpack_pos(ctx->pos);
		u32 frag = nm_unpack_frag(ctx->pos);
		if (frag) {
			rcu_guard();
			struct nm_child_array *array = rcu_dereference(dir_node->children);
			if (array) {
				if (index >= (u32)array->count || nm_child_frag(nm_entries(array)[index].hash) != frag) {
					for (u32 i = 0; i < (u32)array->count; i++)
						if (nm_child_frag(nm_entries(array)[i].hash) == frag) { index = i; break; }
				}
			}
		}
	}

	for (u32 id = index; ; id++) {
		char name_buf[NAME_MAX + 1];
		int name_len = 0;
		unsigned long v_ino = 0;
		unsigned char d_type = 0;
		bool do_emit = false, has_more = false;
		u32 cur_frag = 0, next_frag = 0;

		rcu_read_lock();
		struct nm_child_array *array = rcu_dereference(dir_node->children);
		if (array && id < (u32)array->count) {
			struct nm_leaf *leaf = nm_entries(array)[id].leaf;
			struct nm_rule *rule = nm_select_rule(leaf, fsuid);
			cur_frag = nm_child_frag(nm_entries(array)[id].hash);
			has_more = true;
			if (rule && !(rule->flags & NM_FLAG_WHITEOUT) && (!native_view || nm_rule_needs_readdir(leaf, rule))) {
				name_len = leaf->child_len;
				if (likely(name_len <= NAME_MAX)) {
					memcpy(name_buf, nm_get_child_name(leaf), name_len);
					v_ino = leaf->v_ino;
					d_type = (rule->flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG;
					do_emit = true;
				}
			}
			if (id + 1 < (u32)array->count) next_frag = nm_child_frag(nm_entries(array)[id + 1].hash);
		}
		rcu_read_unlock();
		if (!has_more) break;

		ctx->pos = nm_pack_pos(id, cur_frag);
		if (do_emit && !dir_emit(ctx, name_buf, name_len, v_ino, d_type)) break;
		ctx->pos = nm_pack_pos(id + 1, next_frag);
	}
}

static inline umode_t nm_visible_mode(umode_t mode) { return mode & ~(S_ISUID | S_ISGID); }

static void nm_init_prealloc_inode(struct inode *inode, struct nm_inode_info *info, unsigned long v_ino)
{
    struct inode *r_inode = info->r_path.dentry ? d_backing_inode(info->r_path.dentry) : NULL;

    inode->i_ino = v_ino;
    inode->i_private = info;
    inode->i_mode   = r_inode ? nm_visible_mode(r_inode->i_mode) : (S_IFDIR | 0755);
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

    if (r_inode) {
        nm_sync_inode_times(inode, r_inode);
        inode->i_mapping = r_inode->i_mapping;
    }
    inode->i_flags |= S_PRIVATE | S_NOATIME | S_NOCMTIME | S_NOSEC;
    if (!S_ISLNK(inode->i_mode)) inode->i_opflags |= IOP_NOFOLLOW;
}

static bool nm_inode_matches_attrs(struct inode *inode, struct inode *r_inode)
{
    return !r_inode || nm_inode_is_new(inode) || (inode->i_mode == nm_visible_mode(r_inode->i_mode) && uid_eq(inode->i_uid, r_inode->i_uid) && gid_eq(inode->i_gid, r_inode->i_gid));
}

static bool nm_inode_matches_binding(struct inode *inode, const struct path *path, const void *node, bool opaque)
{
    struct nm_inode_info *info = READ_ONCE(inode->i_private);
    struct inode *r_inode;

    if (!info || info->node != node || !path_equal(&info->r_path, path) || (node && info->opaque != opaque))
        return false;
    r_inode = path->dentry ? d_backing_inode(path->dentry) : NULL;
    return nm_inode_matches_attrs(inode, r_inode);
}

static bool nm_binding_matches_rule(struct inode *inode, const struct nm_rule *rule)
{
    struct nm_inode_info *info = READ_ONCE(inode->i_private);
    struct dentry *d;
    struct inode *r_inode;

    if (!info || info->backing_child || inode->i_op != &nm_file_iops || info->opaque)
        return false;
    d = READ_ONCE(info->r_path.dentry);
    if (!d || nm_backing_gone(d))
        return false;
    if (d != rule->r_path.dentry) {
        struct dentry *rd = rule->r_path.dentry;
        if (d->d_parent != rd->d_parent || d->d_name.len != rd->d_name.len ||
            memcmp(d->d_name.name, rd->d_name.name, rd->d_name.len))
            return false;
    }
    r_inode = d_backing_inode(d);
    return nm_inode_matches_attrs(inode, r_inode);
}

static int nm_cached_inode_set(struct inode *inode, void *data) { return 0; }
static int nm_cached_inode_test(struct inode *inode, void *data)
{
    struct nm_inode_info *info = data;
    return nm_inode_is_virtual(inode) && nm_inode_matches_binding(inode, &info->r_path, info->node, info->opaque);
}

static struct dentry *nm_materialize_rule(struct inode *dir, struct dentry *dentry, struct nm_rule_info *rule_info)
{
    struct nm_inode_info probe, *info = NULL;
    struct inode *inode;
    struct dentry *res = ERR_PTR(rule_info->backing_child ? -ENOMEM : -ENODATA);

    if (rule_info->r_path.dentry) {
        int err = nm_rebind_path(rule_info);
        if (err) goto negative_dentry;
    }

    if (rule_info->flags & NM_FLAG_WHITEOUT) goto negative_dentry;

    probe = (struct nm_inode_info){
        .r_path = rule_info->r_path, .dir_node = rule_info->this_dir,
        .opaque = rule_info->this_dir && (rule_info->flags & NM_FLAG_OPAQUE),
        .backing_child = rule_info->backing_child,
    };
    if (rule_info->leaf) probe.leaf = rule_info->leaf;
    inode = iget5_locked(dir->i_sb, rule_info->key, nm_cached_inode_test, nm_cached_inode_set, &probe);
    if (!inode) goto out;

    if (nm_inode_is_new(inode)) {
        if (!(info = kmalloc(sizeof(*info), GFP_KERNEL))) { iget_failed(inode); goto out; }
        *info = probe;
        nm_init_prealloc_inode(inode, info, rule_info->v_ino);
        unlock_new_inode(inode);
    }
    res = d_splice_alias(inode, dentry);
    if (!IS_ERR(res))
        nm_hijack_dentry_ops(dir, res ? res : dentry, true);
    if (info) return res;
    goto out;

negative_dentry:
    nm_hijack_dentry_ops(dir, dentry, true);
    d_add(dentry, NULL);
    res = NULL;
out:
    nm_dir_put(rule_info->this_dir);
    nm_leaf_put(rule_info->leaf);
    if (rule_info->r_path.dentry) path_put(&rule_info->r_path);
    return res;
}

/*** i_op / s_op / f_op Hijacking Hooks ***/
static struct dentry *nm_hijacked_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    const struct inode_operations *orig_iop = NULL;
    struct nm_rule_info rule_info = {0};
    struct nm_dir_ops *nm_iop = NULL;
    struct dentry *res, *target;
    bool found = false, has_leaf = false;

    rcu_read_lock();
    if (likely((nm_iop = nm_get_nm_iop(rcu_dereference(dir->i_op))))) {
        orig_iop = nm_iop->orig_iop;
        struct nm_dir_node *dir_node = rcu_dereference(nm_iop->dir_node);
        struct nm_leaf *leaf = nm_find_leaf_in_dir(dir_node, dentry->d_name.name, dentry->d_name.len);
        has_leaf = leaf != NULL;
        if (has_leaf) {
            uid_t fsuid = current_fsuid().val;
            if (likely(!nm_is_uid_blocked(fsuid))) found = nm_resolve_rule_from_leaf(leaf, fsuid, &rule_info);
        }
    }
    rcu_read_unlock();

    if (found)
        return nm_materialize_rule(dir, dentry, &rule_info);

    if (likely(orig_iop->lookup)) {
        target = (res = orig_iop->lookup(dir, dentry, flags)) ? res : dentry;
        if (likely(!IS_ERR(target)) && has_leaf && !nm_is_pseudo_fs(dir->i_sb))
            nm_hijack_dentry_ops(dir, target, false);
        return res;
    }
    return ERR_PTR(-EOPNOTSUPP);
}

static int nm_iterate_view(struct file *file, struct dir_context *ctx, const struct file_operations *fop,
                           struct nm_dir_node *dir_node, bool native_view, bool opaque)
{
    struct file *real_file = native_view ? file : file->private_data;
    uid_t fsuid = current_fsuid().val;
    bool uid_blocked = nm_is_uid_blocked(fsuid);
    int res = 0;

    if (unlikely(nm_is_virtual_pos(ctx->pos)))
        goto emit_virtual;
    if ((opaque && !uid_blocked) || !real_file) {
        if (ctx->pos < 2 && !dir_emit_dots(file, ctx)) return 0;
        ctx->pos = nm_pack_pos(0, 0);
        goto emit_virtual;
    }
    if (unlikely(uid_blocked || !dir_node))
        return nm_call_iterate(real_file, ctx, fop);

    struct nm_proxy_ctx proxy_ctx = {
        .ctx.actor = nm_actor_proxy, .ctx.pos = ctx->pos, .orig_ctx = ctx,
        .dir_node = dir_node, .fsuid = fsuid, .native_view = native_view,
    };
    res = nm_call_iterate(real_file, &proxy_ctx.ctx, fop);
    ctx->pos = proxy_ctx.ctx.pos;
    if (res < 0 || proxy_ctx.emitted) return res;
    ctx->pos = nm_pack_pos(0, 0);

emit_virtual:
    if (likely(!uid_blocked))
        nm_emit_virtual_children(ctx, dir_node, fsuid, native_view);
    return res;
}

static int nm_hijacked_iterate_dir(struct file *file, struct dir_context *ctx)
{
    const struct file_operations *orig_fop = NULL;
    struct nm_dir_node *d, *dir_node = NULL;
    struct nm_dir_ops *nm_fop;
    int res;

    rcu_read_lock();
    if ((nm_fop = nm_get_nm_fop(rcu_dereference(file->f_op)))) {
        orig_fop = nm_fop->orig_fop;
        if ((d = rcu_dereference(nm_fop->dir_node)) && rcu_access_pointer(d->children) && (READ_ONCE(d->readdir_count) || nm_is_virtual_pos(ctx->pos)) && atomic_inc_not_zero(&d->refs))
            dir_node = d;
    }
    rcu_read_unlock();

    if (unlikely(!orig_fop || !dir_node))
        return orig_fop ? nm_call_iterate(file, ctx, orig_fop) : -ENOTDIR;
    res = nm_iterate_view(file, ctx, orig_fop, dir_node, true, false);
    nm_dir_put(dir_node);
    return res;
}

static int nm_hijacked_unlink(struct inode *dir, struct dentry *dentry)
{
    struct nm_dir_ops *nm_iop;
    struct nm_dir_node *dir_node;
    struct nm_leaf *leaf;
    struct nm_rule *rule;
    struct inode *inode = d_inode(dentry);
    uid_t fsuid = current_fsuid().val;
    int ret;

    if (unlikely(!inode))
        return -ENOENT;

    if (unlikely(!(nm_iop = nm_get_nm_iop(smp_load_acquire(&dir->i_op))) || !nm_inode_is_virtual(inode) || nm_is_uid_blocked(fsuid)))
        goto do_real_unlink;

    if (!mutex_trylock(&nm_mutex)) return -EBUSY;
    dir_node = rcu_dereference_protected(nm_iop->dir_node, lockdep_is_held(&nm_mutex));
    leaf = nm_find_leaf_in_dir(dir_node, dentry->d_name.name, dentry->d_name.len);
    if (unlikely(!leaf || !(rule = nm_select_rule(leaf, fsuid)) || (!rule->target_uid && !rule->uid_end))) {
        mutex_unlock(&nm_mutex);
        goto do_real_unlink;
    }

    ret = nm_del_rule_locked(leaf->paths, leaf->v_len, rule->target_uid, rule->uid_end, rule->deny_start, rule->deny_end);
    mutex_unlock(&nm_mutex);
    if (ret)
        return ret;

    d_drop(dentry);
    return 0;

do_real_unlink:
    if (nm_inode_is_virtual(inode)) return -ENOENT;
    if (nm_iop && nm_iop->orig_iop && nm_iop->orig_iop->unlink)
        return nm_iop->orig_iop->unlink(dir, dentry);
    return -EPERM;
}

static int nm_hijacked_rename(IDMAP_ARG struct inode *old_dir, struct dentry *old_dentry,
                               struct inode *new_dir, struct dentry *new_dentry, unsigned int flags)
{
    struct inode *old_inode = d_inode(old_dentry);
    struct inode *new_inode = d_inode(new_dentry);
    struct nm_dir_ops *iop;

    if ((old_inode && nm_inode_is_virtual(old_inode)) || (new_inode && nm_inode_is_virtual(new_inode)))
        return -EPERM;

    iop = nm_get_nm_iop(smp_load_acquire(&old_dir->i_op));
    if (iop && iop->orig_iop && iop->orig_iop->rename)
        return iop->orig_iop->rename(IDMAP_CALL old_dir, old_dentry, new_dir, new_dentry, flags);
    return -EPERM;
}

static int nm_hijacked_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct nm_dir_ops *iop;
    if (inode && nm_inode_is_virtual(inode))
        return -EPERM;
    iop = nm_get_nm_iop(smp_load_acquire(&dir->i_op));
    if (iop && iop->orig_iop && iop->orig_iop->rmdir)
        return iop->orig_iop->rmdir(dir, dentry);
    return -EPERM;
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
    struct nm_sop *nm_sop;

    if (nm_inode_is_virtual(inode)) {
        nm_destroy_virtual_inode(inode);
        truncate_inode_pages_final(&inode->i_data);
        clear_inode(inode);
        return;
    }
    nm_destroy_hijacked_inode(inode);
    nm_sop = nm_get_nm_sop(smp_load_acquire(&inode->i_sb->s_op));
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
    if (unlikely(!info->r_path.dentry)) {
        file->private_data = NULL;
        return 0;
    }

    bool readonly = (file->f_flags & ~(O_LARGEFILE | O_CLOEXEC)) == O_RDONLY;
    if (readonly && (real_file = smp_load_acquire(&info->r_file)) && real_file->f_cred == file->f_cred) {
        file->private_data = real_file;
        return 0;
    }

    real_file = dentry_open(&info->r_path, (file->f_flags & ~(O_CREAT | O_EXCL | O_NOCTTY)), file->f_cred);
    if (IS_ERR(real_file)) return PTR_ERR(real_file);

    file->private_data = real_file;
    if (readonly && !READ_ONCE(info->r_file) && S_ISREG(inode->i_mode)) {
        unsigned long magic = info->r_path.dentry->d_sb->s_magic;
        if (magic == EXT4_SUPER_MAGIC || magic == F2FS_SUPER_MAGIC) cmpxchg(&info->r_file, NULL, real_file);
    }
    return 0;
}

static int nm_release(struct inode *inode, struct file *file)
{
    struct nm_inode_info *info = READ_ONCE(inode->i_private);
    struct file *real_file = file->private_data;
    if (real_file && info && real_file != READ_ONCE(info->r_file)) fput(real_file);
    file->private_data = NULL;
    return 0;
}

static loff_t nm_llseek(struct file *file, loff_t offset, int whence)
{
    struct file *real_file = file->private_data;
    loff_t pos;

    if (!real_file) return -EINVAL;

    switch (whence) {
    case SEEK_SET: pos = offset; break;
    case SEEK_CUR: pos = file->f_pos + offset; break;
    case SEEK_END: pos = i_size_read(file_inode(real_file)) + offset; break;
    case SEEK_DATA:
    case SEEK_HOLE:
        if ((pos = vfs_llseek(real_file, offset, whence)) < 0) return pos;
        break;
    default: return -EINVAL;
    }
    if (pos < 0) return -EINVAL;
    file->f_pos = pos;
    return pos;
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
    if (!r_inode->i_op->listxattr) return 0;
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
        stat->mode = nm_visible_mode(stat->mode);
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
        if (attr->ia_valid & ATTR_MODE) v_inode->i_mode = nm_visible_mode(r_inode->i_mode);
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
    if (real_inode && real_inode->i_op->get_link) {
        return real_inode->i_op->get_link(target_dentry, real_inode, done);
    }

    return ERR_PTR(-EINVAL);
}

static int nm_dir_iterate_dir(struct file *file, struct dir_context *ctx)
{
    struct nm_inode_info *info = READ_ONCE(file_inode(file)->i_private);
    struct file *real_file = file->private_data;

    if (unlikely(!info)) return -ENODEV;
    return nm_iterate_view(file, ctx, real_file ? real_file->f_op : NULL, info->dir_node, false, info->opaque);
}

static struct dentry *nm_dir_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct nm_inode_info *info = dir->i_private;
    struct nm_rule_info rule_info = {0};
    struct inode *r_dir;
    struct dentry *res;
    uid_t fsuid;
    bool uid_blocked, found = false, has_leaf = false;

    if (unlikely(!info))
        goto negative_dentry;

    uid_blocked = nm_is_uid_blocked((fsuid = current_fsuid().val));
    rcu_read_lock();
    struct nm_leaf *leaf = nm_find_leaf_in_dir(READ_ONCE(info->dir_node), dentry->d_name.name, dentry->d_name.len);
    has_leaf = leaf != NULL;
    if (leaf && !uid_blocked) found = nm_resolve_rule_from_leaf(leaf, fsuid, &rule_info);
    rcu_read_unlock();
    if (found) return nm_materialize_rule(dir, dentry, &rule_info);

    if (info->opaque && !uid_blocked)
        goto negative_dentry;

    if (!info->r_path.dentry)
        goto negative_dentry;
    r_dir = d_backing_inode(info->r_path.dentry);

    if (r_dir->i_sb != dir->i_sb && !nm_dir_lookup_tolerates_cross_sb(r_dir->i_sb)) {
        struct dentry *backing;

        if (IS_ERR((backing = nm_lookup_one_len_unlocked(dentry->d_name.name, info->r_path.dentry, dentry->d_name.len))))
            return backing;
        if (d_is_negative(backing)) {
            dput(backing);
            goto negative_dentry;
        }
        if (nm_inode_is_virtual(d_inode(backing))) {
            dput(backing);
            return ERR_PTR(-ELOOP);
        }
        rule_info = (struct nm_rule_info){
            .r_path = { .mnt = info->r_path.mnt },
            .v_ino = d_inode(backing)->i_ino,
            .key = (unsigned long)backing,
            .backing_child = true,
        };
        rule_info.r_path.dentry = backing;
        path_get(&rule_info.r_path);
        dput(backing);
        return nm_materialize_rule(dir, dentry, &rule_info);
    }

    if (r_dir->i_op->lookup) {
        res = r_dir->i_op->lookup(r_dir, dentry, flags);
        if (!IS_ERR(res ? res : dentry) && has_leaf && !nm_is_pseudo_fs(r_dir->i_sb))
            nm_hijack_dentry_ops(dir, res ? res : dentry, false);
        return res;
    }
    return ERR_PTR(-EOPNOTSUPP);

negative_dentry:
    nm_hijack_dentry_ops(dir, dentry, true);
    d_add(dentry, NULL);
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

#ifdef NM_HAS_XATTR__GET
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

static int nm_xattr_set(const struct xattr_handler *handler, IDMAP_ARG struct dentry *dentry,
                        struct inode *inode, const char *name, const void *buffer, size_t size, int flags)
{
    struct nm_xattr_proxy *proxy = container_of(handler, struct nm_xattr_proxy, fake);
    if (nm_inode_is_virtual(inode)) {
        struct nm_inode_info *info = inode->i_private;
        const char *full_name;

        if (unlikely(!info || !info->r_path.dentry)) return -ENODATA;
        if (sb_rdonly(d_backing_inode(info->r_path.dentry)->i_sb)) return -EROFS;
        full_name = xattr_full_name(handler, name);
        if (buffer)
            return vfs_setxattr(IDMAP_PATH(info->r_path) info->r_path.dentry, full_name, buffer, size, flags);
        return vfs_removexattr(IDMAP_PATH(info->r_path) info->r_path.dentry, full_name);
    }
    return proxy->orig->set(proxy->orig, IDMAP_CALL dentry, inode, name, buffer, size, flags);
}

static int nm_shutdown_sync_fs(struct super_block *sb, int wait)
{
    struct nm_sop *nm_sop = nm_get_nm_sop(smp_load_acquire(&sb->s_op));
    if (current->pid == 1) nm_shutdown_release();
    if (nm_sop && nm_sop->orig_sop && nm_sop->orig_sop->sync_fs)
        return nm_sop->orig_sop->sync_fs(sb, wait);
    return 0;
}

static bool nm_is_our_dops(struct nm_dir_ops *iop, const struct dentry_operations *dop)
{
    return dop == &nm_dops || (iop && dop == &iop->fake_dops);
}

static int nm_d_revalidate_common(struct inode *parent_inode, const struct qstr *name,
                                  struct dentry *dentry, unsigned int flags, bool weak)
{
    struct nm_dir_node *parent_dir = NULL;
    const struct dentry_operations *orig_dops = NULL;
    struct inode *inode = READ_ONCE(dentry->d_inode);
    struct nm_dir_ops *iop = NULL;
    struct nm_rule *rule;
    bool has_rule = false, whiteout = false, match_rule = false, uid_blocked = false;
    uid_t fsuid;

    if (unlikely(!parent_inode)) return 1;
    if ((flags & LOOKUP_RENAME_TARGET) && inode && nm_inode_is_virtual(inode))
        return (flags & LOOKUP_RCU) ? -ECHILD : -EPERM;

    rcu_read_lock();
    const struct inode_operations *parent_iop = rcu_dereference(parent_inode->i_op);
    if (parent_iop == &nm_dir_iops) {
        struct nm_inode_info *pinfo = rcu_dereference(parent_inode->i_private);
        parent_dir = pinfo ? pinfo->dir_node : NULL;
    } else if ((iop = nm_get_nm_iop(parent_iop))) {
        parent_dir = rcu_dereference(iop->dir_node);
    }

    struct nm_inode_info *bound = inode && inode->i_op == &nm_file_iops ? rcu_dereference(inode->i_private) : NULL;
    struct nm_leaf *leaf = bound ? bound->leaf : NULL;
    if (!leaf || leaf->parent_dir != parent_dir || !nm_child_matches(leaf, name->name, name->len) || !rcu_access_pointer(leaf->rules))
        leaf = nm_find_leaf_in_dir(parent_dir, name->name, name->len);
    if (leaf && !(uid_blocked = nm_is_uid_blocked((fsuid = current_fsuid().val))) && (rule = nm_select_rule(leaf, fsuid))) {
        has_rule = true;
        if (rule->flags & NM_FLAG_WHITEOUT)
            whiteout = true;
        else if (inode && nm_inode_is_virtual(inode)) {
            if (rule->flags & NM_FLAG_IS_DIR)
                match_rule = nm_inode_matches_binding(inode, &rule->r_path, leaf->this_dir, rule->flags & NM_FLAG_OPAQUE);
            else
                match_rule = nm_binding_matches_rule(inode, rule);
        }
    }

    if (!has_rule) orig_dops = nm_get_orig_dops(iop);
    rcu_read_unlock();

    if (whiteout && !inode) return weak ? 0 : 1;
    if (match_rule) return 1;
    if (!has_rule && inode && nm_inode_is_virtual(inode)) {
        struct nm_inode_info *info = READ_ONCE(inode->i_private);
        if (info && info->backing_child) {
            struct nm_inode_info *pinfo;
            struct dentry *backing, *backing_parent;
            bool valid;

            if (flags & LOOKUP_RCU) return -ECHILD;
            pinfo = READ_ONCE(parent_inode->i_private);
            backing_parent = pinfo ? pinfo->r_path.dentry : NULL;
            if (!backing_parent)
                return nm_drop_dentry(dentry, flags, weak);

            backing = nm_lookup_one_len_unlocked(name->name, backing_parent, name->len);
            if (IS_ERR(backing)) return PTR_ERR(backing);
            valid = (backing == info->r_path.dentry && d_is_positive(backing) && nm_inode_matches_attrs(inode, d_inode(backing)));
            dput(backing);
            return valid ? 1 : 0;
        }
    }

    if (has_rule || (!uid_blocked && nm_is_our_dops(iop, READ_ONCE(dentry->d_op))) || (inode && nm_inode_is_virtual(inode)))
        return nm_drop_dentry(dentry, flags, weak);
    return nm_call_orig_revalidate(orig_dops, parent_inode, name, dentry, flags);
}

static int nm_d_weak_revalidate(struct dentry *dentry, unsigned int flags)
{
    return nm_d_revalidate_common(d_inode(READ_ONCE(dentry->d_parent)), &dentry->d_name, dentry, flags, true);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
static int nm_d_revalidate(struct inode *parent_inode, const struct qstr *name, struct dentry *dentry, unsigned int flags) {
    return nm_d_revalidate_common(parent_inode, name, dentry, flags, false);
}
#else
static int nm_d_revalidate(struct dentry *dentry, unsigned int flags) {
    return nm_d_revalidate_common(d_inode(READ_ONCE(dentry->d_parent)), &dentry->d_name, dentry, flags, false);
}
#endif

static const struct dentry_operations nm_dops = {
    .d_revalidate = nm_d_revalidate,
    .d_weak_revalidate = nm_d_weak_revalidate,
};

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

static int nm_hijack_superblock(struct super_block *sb, bool with_xattr)
{
    struct nm_sop *nm_sop = nm_get_nm_sop(smp_load_acquire(&sb->s_op));
    lockdep_assert_held(&nm_mutex);

    if (!nm_sop) {
        if (!with_xattr && (sb_rdonly(sb) || nm_is_pseudo_fs(sb))) return 0;
        bool kernfs = with_xattr && nm_is_kernfs(sb);
        if (kernfs) {
            list_for_each_entry(nm_sop, &nm_sb_list, list)
                if (nm_sop->sb == sb) return 0;
        }
        if (!(nm_sop = kzalloc(sizeof(*nm_sop), GFP_KERNEL))) return -ENOMEM;
        nm_sop->orig_sop = sb->s_op;
        nm_sop->sb = sb;
        if (!kernfs) {
            nm_sop->fake_sop = *sb->s_op;
            if (with_xattr) {
                nm_sop->fake_sop.drop_inode = nm_hijacked_drop_inode;
                nm_sop->fake_sop.evict_inode = nm_hijacked_evict_inode;
            }
            if (!sb_rdonly(sb) && !nm_is_pseudo_fs(sb))
                nm_sop->fake_sop.sync_fs = nm_shutdown_sync_fs;
            smp_store_release(&sb->s_op, &nm_sop->fake_sop);
        }
        list_add_tail_rcu(&nm_sop->list, &nm_sb_list);
        nm_debug("Superblock successfully hijacked for dev: 0x%x\n", sb->s_dev);
    } else if (!with_xattr || nm_sop->fake_xattr) {
        return 0;
    } else {
        WRITE_ONCE(nm_sop->fake_sop.drop_inode, nm_hijacked_drop_inode);
        WRITE_ONCE(nm_sop->fake_sop.evict_inode, nm_hijacked_evict_inode);
    }

    if (with_xattr && sb->s_xattr) {
        const struct xattr_handler **new_array;
        struct nm_xattr_proxy *proxies;
        int count = 0;

        while (sb->s_xattr[count]) count++;
        if (!(new_array = kmalloc((count + 1) * sizeof(void *) + (count * sizeof(*proxies)), GFP_KERNEL))) return -ENOMEM;
        proxies = (void *)(new_array + count + 1);
        new_array[count] = NULL;
        for (int i = 0; i < count; i++) {
            proxies[i].orig = sb->s_xattr[i];
            proxies[i].fake = *sb->s_xattr[i];
            if (proxies[i].fake.get) proxies[i].fake.get = nm_xattr_get;
#ifdef NM_HAS_XATTR__GET
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

    return 0;
}

static inline int nm_hijack_dir_ops(struct nm_dir_node *dir_node, struct inode *inode)
{
    const struct inode_operations *iop = smp_load_acquire(&inode->i_op);
    struct nm_dir_ops *ops = nm_get_nm_iop(iop);
    int err;

    if (ops) return ops->dir_node == dir_node ? 0 : -EBUSY;
    const struct file_operations *fop = smp_load_acquire(&inode->i_fop);
    if ((ops = nm_get_nm_fop(fop))) return ops->dir_node == dir_node ? 0 : -EBUSY;
    bool iterate = fop && (fop->iterate_shared
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0)
        || fop->iterate
#endif
    );

    if (!iop && !iterate) return -EOPNOTSUPP;
    if (!(ops = kmalloc(sizeof(*ops), GFP_KERNEL))) return -ENOMEM;
    if ((err = nm_hijack_superblock(inode->i_sb, true))) {
        kfree(ops);
        return err;
    }

    ops->orig_iop = iop;
    ops->orig_fop = fop;
    ops->orig_dops = NULL;
    ops->dir_node = dir_node;
    if (iop) {
        ops->fake_iop = *iop;
        ops->fake_iop.lookup = nm_hijacked_lookup;
        ops->fake_iop.unlink = nm_hijacked_unlink;
        ops->fake_iop.rename = nm_hijacked_rename;
        ops->fake_iop.rmdir  = nm_hijacked_rmdir;
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
    return 0;
}

static void nm_hijack_dentry_ops(struct inode *dir, struct dentry *dentry, bool injected)
{
#define DCACHE_OPS (DCACHE_OP_HASH | DCACHE_OP_COMPARE | DCACHE_OP_DELETE | DCACHE_OP_PRUNE | DCACHE_OP_REAL)
    const struct dentry_operations *orig, *current_orig;
    struct nm_dir_ops *iop;
    typeof(dentry->d_flags) new_flags;

    if (unlikely(!dentry || !dir)) return;
    iop = nm_get_nm_iop(smp_load_acquire(&dir->i_op));
    orig = READ_ONCE(dentry->d_op);
    if (unlikely(!injected && !iop && orig) || nm_is_our_dops(iop, orig)) return;

    spin_lock(&dentry->d_lock);
    orig = dentry->d_op;
    if (nm_is_our_dops(iop, orig)) {
        spin_unlock(&dentry->d_lock);
        return;
    }

    if (injected || !orig) {
        new_flags = dentry->d_flags & ~DCACHE_OPS;
        new_flags |= (DCACHE_OP_REVALIDATE | DCACHE_OP_WEAK_REVALIDATE);
        dentry->d_op = &nm_dops;
        smp_store_release(&dentry->d_flags, new_flags);
    } else if (iop) {
        if (unlikely((current_orig = smp_load_acquire(&iop->orig_dops)) != orig)) {
            if (current_orig == NULL) {
                if (cmpxchg(&iop->orig_dops, NULL, NM_DOP_INITIALIZING) == NULL) {
                    iop->fake_dops = *orig;
                    iop->fake_dops.d_revalidate = nm_d_revalidate;
                    smp_store_release(&iop->orig_dops, orig);
                } else {
                    while (smp_load_acquire(&iop->orig_dops) == NM_DOP_INITIALIZING) cpu_relax();
                }
            } else if (current_orig == NM_DOP_INITIALIZING) {
                while (smp_load_acquire(&iop->orig_dops) == NM_DOP_INITIALIZING) cpu_relax();
            }
        }
        new_flags = dentry->d_flags | DCACHE_OP_REVALIDATE;
        dentry->d_op = &iop->fake_dops;
        smp_store_release(&dentry->d_flags, new_flags);
    }

    spin_unlock(&dentry->d_lock);
}

static void nm_clear_dops_sb(struct super_block *sb)
{
    struct inode *inode;
    struct dentry *dentry;
    spin_lock(&sb->s_inode_list_lock);
    list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
        bool is_virtual = nm_inode_is_virtual(inode);
        spin_lock(&inode->i_lock);
        for_each_alias(dentry, inode) {
            const struct dentry_operations *dop, *orig = NULL;
            spin_lock(&dentry->d_lock);
            dop = dentry->d_op;
            if (dop && dop->d_revalidate == nm_d_revalidate) {
                if (dop != &nm_dops)
                    orig = nm_get_orig_dops(container_of(dop, struct nm_dir_ops, fake_dops));
                if (orig) {
                    dentry->d_op = orig;
                    if (!orig->d_revalidate)
                        dentry->d_flags &= ~DCACHE_OP_REVALIDATE;
                } else {
                    dentry->d_op = NULL;
                    dentry->d_flags &= ~(DCACHE_OP_REVALIDATE | DCACHE_OP_WEAK_REVALIDATE |
                                         DCACHE_OP_DELETE | DCACHE_OP_PRUNE |
                                         DCACHE_OP_HASH | DCACHE_OP_COMPARE | DCACHE_OP_REAL | DCACHE_DONTCACHE);
                }
            }
            spin_unlock(&dentry->d_lock);

            if (is_virtual)
                d_drop(dentry);
        }
        spin_unlock(&inode->i_lock);
    }
    spin_unlock(&sb->s_inode_list_lock);
}

static void nm_restore_superblocks(void)
{
    struct nm_sop *nm_sop, *tmp;
    struct inode *inode;
    list_for_each_entry_safe(nm_sop, tmp, &nm_sb_list, list) {
        if (nm_sop->sb) {
            nm_clear_dops_sb(nm_sop->sb);
            shrink_dcache_sb(nm_sop->sb);
            spin_lock(&nm_sop->sb->s_inode_list_lock);
            list_for_each_entry(inode, &nm_sop->sb->s_inodes, i_sb_list)
                nm_destroy_hijacked_inode(inode);
            spin_unlock(&nm_sop->sb->s_inode_list_lock);
            if (READ_ONCE(nm_sop->sb->s_op) == &nm_sop->fake_sop)
                smp_store_release(&nm_sop->sb->s_op, nm_sop->orig_sop);
            if (nm_sop->fake_xattr) {
                if (READ_ONCE(nm_sop->sb->s_xattr) == nm_sop->fake_xattr)
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

static struct nm_child_array *nm_alloc_child_array(int count)
{
    u16 slots = nm_child_slots_for(count);
    size_t sz = ALIGN(sizeof(struct nm_child_array) + slots * sizeof(u32), 8) + count * sizeof(struct nm_child);
    struct nm_child_array *array = kmalloc(sz, GFP_KERNEL);
    if (array) {
        array->count = count;
        array->bucket_slots = slots;
    }
    return array;
}

static void nm_rebuild_children(struct nm_child_array *array, struct nm_child_array *old, struct nm_child extra, int skip)
{
    int count = old ? old->count : 0;
    struct nm_child *entries = nm_entries(array);
    u16 b;

    if (!array->bucket_slots) {
        int at = 0;
        for (int i = 0; i < count; i++)
            if (i != skip) entries[at++] = nm_entries(old)[i];
        if (extra.leaf) entries[at] = extra;
        return;
    }

    memset(array->starts, 0, array->bucket_slots * sizeof(u32));
    for (int i = 0; i < count; i++)
        if (i != skip) array->starts[nm_child_bucket(array, nm_entries(old)[i].hash)]++;
    if (extra.leaf) array->starts[nm_child_bucket(array, extra.hash)]++;

    for (b = 1; b < array->bucket_slots; b++)
        array->starts[b] += array->starts[b - 1];

    for (int i = count - 1; i >= 0; i--) {
        if (i == skip) continue;
        struct nm_child *child = &nm_entries(old)[i];
        entries[--array->starts[nm_child_bucket(array, child->hash)]] = *child;
    }
    if (extra.leaf)
        entries[--array->starts[nm_child_bucket(array, extra.hash)]] = extra;
}

static bool nm_rules_need_readdir(struct nm_leaf *leaf, struct nm_rule_array *rules)
{
    for (int i = 0; i < rules->count; i++)
        if (nm_rule_needs_readdir(leaf, &rules->rules[i])) return true;
    return false;
}

static void nm_publish_child_view_locked(struct nm_dir_node *dir_node, struct nm_child_array *children)
{
    struct nm_child_array *old = rcu_dereference_protected(dir_node->children, lockdep_is_held(&nm_mutex));
    rcu_assign_pointer(dir_node->children, children);
    if (!children) {
        WRITE_ONCE(dir_node->readdir_count, 0);
        if (dir_node->pinned_dentry) {
            struct dentry *pinned = dir_node->pinned_dentry;
            dir_node->pinned_dentry = NULL;
            dput(pinned);
        }
    }
    if (old) kfree_rcu(old, rcu);
}

static int nm_inject_child_locked(struct nm_dir_node *dir_node, struct nm_leaf *leaf, const char *name, size_t name_len)
{
    struct nm_child_array *array, *old = rcu_dereference_protected(dir_node->children, lockdep_is_held(&nm_mutex));
    int count = old ? old->count : 0;

    if (nm_find_leaf_in_dir(dir_node, name, name_len)) return -EEXIST;
    leaf->child_len = name_len;
    struct nm_child child = { .leaf = leaf, .hash = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, name, name_len) };
    leaf->child_hash = (u16)child.hash;

    if (!(array = nm_alloc_child_array(count + 1))) return -ENOMEM;
    if (old && old->bucket_slots && array->bucket_slots == old->bucket_slots) {
        u32 bucket = nm_child_bucket(array, child.hash), pos = old->starts[bucket + 1];
        u16 i;
        for (i = 0; i <= bucket; i++) array->starts[i] = old->starts[i];
        for (; i < array->bucket_slots; i++) array->starts[i] = old->starts[i] + 1;
        memcpy(nm_entries(array), nm_entries(old), pos * sizeof(struct nm_child));
        nm_entries(array)[pos] = child;
        memcpy(nm_entries(array) + pos + 1, nm_entries(old) + pos, (count - pos) * sizeof(struct nm_child));
    } else nm_rebuild_children(array, old, child, -1);

    leaf->parent_dir = dir_node;
    atomic_inc(&dir_node->refs);
    if (nm_rules_need_readdir(leaf, rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex))))
        WRITE_ONCE(dir_node->readdir_count, dir_node->readdir_count + 1);
    nm_publish_child_view_locked(dir_node, array);
    return 0;
}

static struct nm_dir_node *nm_delete_child_locked(struct nm_leaf *leaf, bool clear)
{
    struct nm_dir_node *dir_node = leaf->parent_dir;
    struct nm_child_array *old, *array = NULL;

    if (!dir_node) return NULL;
    if (!(old = rcu_dereference_protected(dir_node->children, lockdep_is_held(&nm_mutex)))) return dir_node;
    if (!clear) {
        u32 bucket = 0;
        int pos = 0, end = old->count;
        if (old->bucket_slots) {
            bucket = nm_child_bucket(old, leaf->child_hash);
            pos = old->starts[bucket];
            end = old->starts[bucket + 1];
        }
        for (; pos < end && nm_entries(old)[pos].leaf != leaf; pos++) {}
        if (pos == end) return dir_node;
        if (old->count > 1) {
            int new_count = old->count - 1;
            if (!(array = nm_alloc_child_array(new_count))) return ERR_PTR(-ENOMEM);
            if (array->bucket_slots && array->bucket_slots == old->bucket_slots) {
                u16 i;
                for (i = 0; i <= bucket; i++) array->starts[i] = old->starts[i];
                for (; i < array->bucket_slots; i++) array->starts[i] = old->starts[i] - 1;
                memcpy(nm_entries(array), nm_entries(old), pos * sizeof(struct nm_child));
                memcpy(nm_entries(array) + pos, nm_entries(old) + pos + 1, (new_count - pos) * sizeof(struct nm_child));
            } else nm_rebuild_children(array, old, (struct nm_child){0}, pos);
        }
    }
    nm_publish_child_view_locked(dir_node, array);
    if (array && nm_rules_need_readdir(leaf, rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex))))
        WRITE_ONCE(dir_node->readdir_count, dir_node->readdir_count - 1);
    return dir_node;
}

static struct nm_leaf *nm_find_leaf(const char *path, u16 len)
{
    struct nm_leaf *leaf;
    u32 hash = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, path, len);
    hash_for_each_possible(nm_leaves_ht, leaf, ht_node, hash) {
        if (leaf->v_hash == hash && leaf->v_len == len && !memcmp(leaf->paths, path, len)) return leaf;
    }
    return NULL;
}

static struct nm_rule_array *nm_alloc_rule_array(int count)
{
    struct nm_rule_array *arr = kmalloc(sizeof(struct nm_rule_array) + count * sizeof(struct nm_rule), GFP_KERNEL);
    if (arr) arr->count = count;
    return arr;
}

static int nm_prepare_rule(const char *r_path, u32 flags, unsigned int uid, unsigned int uid_end,
                           unsigned int deny_start, unsigned int deny_end, struct nm_rule *out)
{
    bool backed = !(flags & (NM_FLAG_WHITEOUT | NM_FLAG_VIRTUAL_DIR));
    if (backed && !r_path) return -EINVAL;

    memset(out, 0, sizeof(*out));
    out->flags = flags;
    out->target_uid = uid;
    out->uid_end = uid_end;
    out->deny_start = deny_start;
    out->deny_end = deny_end;
    if (backed) {
        int err = kern_path(r_path, LOOKUP_FOLLOW, &out->r_path);
        if (err) return err;
        if ((err = nm_hijack_superblock(out->r_path.dentry->d_sb, false))) {
            path_put(&out->r_path);
            return err;
        }
        struct inode *inode = d_backing_inode(out->r_path.dentry);
        if (S_ISDIR(inode->i_mode)) out->flags |= NM_FLAG_IS_DIR;
    }
    return 0;
}

static inline void nm_copy_rule(struct nm_rule *dst, const struct nm_rule *src)
{
    *dst = *src;
    if (dst->r_path.dentry) path_get(&dst->r_path);
}

static struct nm_leaf *nm_alloc_leaf(const char *path, u16 len)
{
    struct nm_leaf *leaf = kzalloc(sizeof(*leaf) + len + 1, GFP_KERNEL);
    if (!leaf) return ERR_PTR(-ENOMEM);

    atomic_set(&leaf->refs, 1);
    memcpy(leaf->paths, path, len);
    leaf->v_len = len;
    leaf->v_hash = full_name_hash((const void *)(unsigned long)NM_MAGIC_SIG, path, len);
    leaf->v_ino = (unsigned long)leaf->v_hash;
    return leaf;
}

static void nm_resolve_anchor_locked(struct nm_leaf *leaf)
{
    struct path anchor;
    struct inode *inode;
    struct nm_dir_ops *ops;
    struct nm_dir_node *dir;

    if (kern_path(leaf->paths, LOOKUP_FOLLOW, &anchor)) return;
    inode = d_backing_inode(anchor.dentry);
    if (!S_ISDIR(inode->i_mode)) {
        path_put(&anchor);
        return;
    }

    leaf->v_ino = inode->i_ino;
    d_drop(anchor.dentry);
    leaf->anchor = anchor;

    if (inode->i_op != &nm_dir_iops) {
        ops = nm_get_nm_iop(smp_load_acquire(&inode->i_op));
        if (!ops) ops = nm_get_nm_fop(smp_load_acquire(&inode->i_fop));
        dir = ops ? ops->dir_node : NULL;
        if (dir && !dir->owner) {
            leaf->this_dir = dir;
            atomic_inc(&dir->refs);
            dir->owner = leaf;
        }
    }
}

static void nm_free_leaf(struct nm_leaf *leaf, bool deferred)
{
    if (leaf->anchor.dentry) path_put(&leaf->anchor);
    if (leaf->this_dir) {
        if (leaf->this_dir->owner == leaf) leaf->this_dir->owner = NULL;
        nm_dir_put(leaf->this_dir);
    }
    nm_dir_put(leaf->parent_dir);
    if (deferred) kfree_rcu(leaf, rcu);
    else kfree(leaf);
}

static void nm_leaf_put(struct nm_leaf *leaf)
{
    if (leaf && atomic_dec_and_test(&leaf->refs)) nm_free_leaf(leaf, true);
}

static void nm_rule_array_free(struct nm_rule_array *arr)
{
    for (int i = 0; i < arr->count; i++) {
        if (arr->rules[i].r_path.dentry) path_put(&arr->rules[i].r_path);
    }
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
        nm_leaf_put(leaf);
    }
}

static DECLARE_WORK(nm_reclaim_work, nm_reclaim);

static bool nm_shutdown_released;
static void nm_shutdown_release(void)
{
    if (READ_ONCE(nm_shutdown_released)) return;
    WRITE_ONCE(nm_shutdown_released, true);
    nm_clear_state(NM_CLEAR_UIDS | NM_CLEAR_RULES);
    flush_work(&nm_reclaim_work);
}

static void nm_retire_array_locked(struct nm_rule_array *arr)
{
    arr->next = nm_retired_arrays;
    nm_retired_arrays = arr;
    schedule_work(&nm_reclaim_work);
}

static void nm_publish_rules_locked(struct nm_leaf *leaf, struct nm_rule_array *array, struct nm_rule_array *old)
{
    struct nm_dir_node *parent = leaf->parent_dir;
    bool before = old && nm_rules_need_readdir(leaf, old);
    bool after = nm_rules_need_readdir(leaf, array);

    if (parent && !before && after) WRITE_ONCE(parent->readdir_count, parent->readdir_count + 1);
    rcu_assign_pointer(leaf->rules, array);
    if (parent && before && !after) WRITE_ONCE(parent->readdir_count, parent->readdir_count - 1);
    if (old) nm_retire_array_locked(old);
}

static void nm_retire_leaf_locked(struct nm_leaf *leaf)
{
    struct nm_rule_array *array = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex));
    if (leaf->this_dir) leaf->this_dir->owner = NULL;
    RCU_INIT_POINTER(leaf->rules, NULL);
    leaf->next = nm_retired_leaves;
    nm_retired_leaves = leaf;
    if (array) nm_retire_array_locked(array);
    else schedule_work(&nm_reclaim_work);
}

static void nm_prepare_native_file(struct nm_leaf *leaf, struct nm_dir_node *dir_node, const struct path *parent, const char *name)
{
    switch (parent->dentry->d_sb->s_magic) {
#if defined(EROFS_SUPER_MAGIC_V1) && BITS_PER_LONG == 64
    case EROFS_SUPER_MAGIC_V1:
#endif
    case SQUASHFS_MAGIC:
        break;
    default:
        return;
    }
    struct nm_rule_array *rules = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex));
    struct nm_rule *rule = &rules->rules[0];
    struct path path;

    if (dir_node->owner || rule->flags || !S_ISREG(d_backing_inode(rule->r_path.dentry)->i_mode)) return;
    if (vfs_path_lookup(parent->dentry, parent->mnt, name, 0, &path)) return;
    struct inode *inode = d_backing_inode(path.dentry);
    if (path.dentry->d_parent == parent->dentry && S_ISREG(inode->i_mode) && inode->i_nlink == 1) {
        leaf->v_ino = inode->i_ino;
        leaf->native_file = true;
    }
    path_put(&path);
}

static int nm_generate_virtual_topology(struct nm_leaf *target, unsigned int uid)
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
            struct nm_rule *rule = nm_select_rule(parent, uid);
            if ((rule && !(rule->flags & NM_FLAG_IS_DIR)) || !parent->this_dir) { err = -ENOTDIR; break; }
            if (parent->anchor.dentry && (err = nm_hijack_dir_ops(parent->this_dir, d_backing_inode(parent->anchor.dentry)))) break;
            err = nm_inject_child_locked(parent->this_dir, child, name, name_len);
            if (!err && parent->anchor.dentry) {
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
            bool virtual = inode->i_op == &nm_dir_iops;
            if (!S_ISDIR(inode->i_mode)) {
                err = -ENOTDIR;
            } else if (virtual) {
                dir = ((struct nm_inode_info *)inode->i_private)->dir_node;
                if (dir) atomic_inc(&dir->refs);
                else err = -EOPNOTSUPP;
            } else {
                struct nm_dir_ops *ops = nm_get_nm_iop(smp_load_acquire(&inode->i_op));
                if (!ops) ops = nm_get_nm_fop(smp_load_acquire(&inode->i_fop));
                dir = ops ? ops->dir_node : NULL;
                if (dir) atomic_inc(&dir->refs);
                else dir = nm_alloc_dir_node();
                if (!dir) err = -ENOMEM;
            }
            if (!err && !virtual) {
                nm_prepare_native_file(child, dir, &path, name);
                err = nm_hijack_dir_ops(dir, inode);
            }
            if (!err && !(err = nm_inject_child_locked(dir, child, name, name_len))) {
                if (!virtual && !dir->owner && !dir->pinned_dentry) dir->pinned_dentry = dget(path.dentry);
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
            nm_free_leaf(parent, false);
            err = -ENOMEM;
            break;
        }
        parent->this_dir->owner = parent;
        struct nm_rule_array *rules = nm_alloc_rule_array(1);
        if (!rules) {
            err = -ENOMEM;
            nm_free_leaf(parent, false);
            break;
        }
        memset(&rules->rules[0], 0, sizeof(struct nm_rule));
        rules->rules[0].flags = NM_FLAG_IS_DIR | NM_FLAG_VIRTUAL_DIR;
        rules->uid_count = 0;
        RCU_INIT_POINTER(parent->rules, rules);

        hlist_add_head(&parent->ht_node, &pending);
        err = nm_inject_child_locked(parent->this_dir, child, name, name_len);
        if (err) break;
        child = parent;
        len = parent_len;
    }

    hlist_for_each_entry_safe(parent, h_tmp, &pending, ht_node) {
        hlist_del(&parent->ht_node);
        if (!err) hash_add(nm_leaves_ht, &parent->ht_node, parent->v_hash);
        else {
            nm_delete_child_locked(parent, false);
            nm_retire_leaf_locked(parent);
        }
    }
    return err;
}

static struct nm_dir_node *nm_detach_leaf_locked(struct nm_leaf *leaf)
{
    struct nm_dir_node *parent = nm_delete_child_locked(leaf, false);
    if (IS_ERR(parent)) return parent;
    hash_del(&leaf->ht_node);
    nm_retire_leaf_locked(leaf);
    return parent;
}

static int nm_prune_empty_virtual_dirs_locked(struct nm_dir_node *dir)
{
    struct nm_leaf *leaf;
    while (dir && !rcu_access_pointer(dir->children) && (leaf = dir->owner)) {
        struct nm_rule_array *rules = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex));
        int i;

        if (!rules) break;
        for (i = 0; i < rules->count; i++)
            if (!(rules->rules[i].flags & NM_FLAG_VIRTUAL_DIR)) break;
        if (i < rules->count) break;
        dir = nm_detach_leaf_locked(leaf);
        if (IS_ERR(dir)) return PTR_ERR(dir);
    }
    return 0;
}

/*** Rule Operations ***/

static int nm_add_rule_locked(const char *v_path, const char *r_path, u16 v_len, u16 r_len, u32 flags,
                              unsigned int uid, unsigned int uid_end, unsigned int deny_start, unsigned int deny_end)
{
    struct nm_rule new_rule;
    struct nm_leaf *leaf;
    struct nm_rule_array *old, *array;
    struct nm_dir_node *new_dir = NULL;
    int pos = 0, count, new_count, err = 0, j = 0;
    bool fresh, replace = false;

    if (READ_ONCE(nm_shutdown_released))
        return -ESHUTDOWN;

    if (deny_start || deny_end) {
        if (!deny_start || !deny_end) return -EINVAL;
        if (deny_start > deny_end) { unsigned int t = deny_start; deny_start = deny_end; deny_end = t; }
        if (deny_start < uid || deny_end > uid_end) return -EINVAL;
    }

    char *r_buf = NULL;
    if (r_path && r_len && !(r_buf = kmemdup_nul(r_path, r_len, GFP_KERNEL))) return -ENOMEM;

    err = nm_prepare_rule(r_buf, flags, uid, uid_end, deny_start, deny_end, &new_rule);
    kfree(r_buf);
    if (err) return err;

    while (v_len > 1 && v_path[v_len - 1] == '/') v_len--;

    fresh = !(leaf = nm_find_leaf(v_path, v_len));
    if (fresh && IS_ERR((leaf = nm_alloc_leaf(v_path, v_len)))) {
        err = PTR_ERR(leaf);
        goto out_rule;
    }

    if (fresh && (new_rule.flags & NM_FLAG_IS_DIR))
        nm_resolve_anchor_locked(leaf);

    count = (old = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex))) ? old->count : 0;

    if (uid == 0 && uid_end == 0) {
        replace = count && old->uid_count != count;
    } else {
        while (pos < count && (old->rules[pos].target_uid < uid || (old->rules[pos].target_uid == uid && old->rules[pos].uid_end < uid_end))) pos++;
        replace = pos < count && old->rules[pos].target_uid == uid && old->rules[pos].uid_end == uid_end;
        if (!replace && old) {
            for (int i = count - old->uid_count; i < count; i++) {
                if (nm_rules_overlap(&old->rules[i], &new_rule)) { err = -EEXIST; goto out_leaf; }
            }
        }
    }

    new_count = count + !replace;

    if ((new_rule.flags & NM_FLAG_IS_DIR) && !leaf->this_dir && !(new_dir = nm_alloc_dir_node())) {
        err = -ENOMEM;
        goto out_leaf;
    }

    if (!(array = nm_alloc_rule_array(new_count))) {
        err = -ENOMEM;
        goto out_dir;
    }

    for (int i = 0; i < pos; i++, j++) nm_copy_rule(&array->rules[j], &old->rules[i]);
    array->rules[j++] = new_rule;
    for (int i = pos + replace; i < count; i++, j++) nm_copy_rule(&array->rules[j], &old->rules[i]);
    array->uid_count = new_count - (!array->rules[0].target_uid && !array->rules[0].uid_end);

    if (new_dir) {
        leaf->this_dir = new_dir;
        new_dir->owner = leaf;
    }
    if (fresh) {
        RCU_INIT_POINTER(leaf->rules, array);
        if ((err = nm_generate_virtual_topology(leaf, uid))) {
            nm_delete_child_locked(leaf, false);
            nm_retire_leaf_locked(leaf);
            return err;
        }
        hash_add(nm_leaves_ht, &leaf->ht_node, leaf->v_hash);
    } else {
        nm_publish_rules_locked(leaf, array, old);
    }

    return 0;

out_dir:  nm_dir_put(new_dir);
out_leaf: if (fresh) nm_free_leaf(leaf, false);
out_rule: if (new_rule.r_path.dentry) path_put(&new_rule.r_path);
          return err;
}

static int nm_del_rule_locked(const char *path, u16 len, unsigned int uid,
                              unsigned int uid_end, unsigned int deny_start, unsigned int deny_end)
{
    struct nm_leaf *leaf;
    struct nm_rule_array *old, *array = NULL;
    int pos = 0, new_count;
    bool stub_virtual = false;

    while (len > 1 && path[len - 1] == '/') len--;
    if (!(leaf = nm_find_leaf(path, len)) || !(old = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex)))) return 0;
    while (pos < old->count &&
           (old->rules[pos].target_uid != uid || old->rules[pos].uid_end != uid_end ||
            old->rules[pos].deny_start != deny_start || old->rules[pos].deny_end != deny_end)) pos++;
    if (pos == old->count) return 0;

    if (leaf->this_dir && rcu_access_pointer(leaf->this_dir->children) && ((old->rules[pos].flags & NM_FLAG_IS_DIR) || !pos || old->uid_count == old->count)) {
        if (old->rules[pos].flags & NM_FLAG_VIRTUAL_DIR) return 0;
        stub_virtual = true;
    }

    if (!stub_virtual && old->count == 1) {
        struct nm_dir_node *parent = nm_detach_leaf_locked(leaf);
        if (IS_ERR(parent)) return PTR_ERR(parent);
        return nm_prune_empty_virtual_dirs_locked(parent);
    }

    new_count = stub_virtual ? old->count : old->count - 1;

    if (!(array = nm_alloc_rule_array(new_count))) return -ENOMEM;

    for (int i = 0, j = 0; i < old->count; i++) {
        if (!stub_virtual && i == pos) continue;
        if (stub_virtual && i == pos) {
            array->rules[j] = (struct nm_rule){
                .target_uid = uid, .uid_end = uid_end,
                .deny_start = deny_start, .deny_end = deny_end,
                .flags = NM_FLAG_IS_DIR | NM_FLAG_VIRTUAL_DIR,
            };
            j++;
            continue;
        }
        nm_copy_rule(&array->rules[j], &old->rules[i]);
        j++;
    }
    array->uid_count = new_count - (!array->rules[0].target_uid && !array->rules[0].uid_end);

    nm_publish_rules_locked(leaf, array, old);
    return 0;
}

static void nm_clear_state(int clear_flags)
{
    mutex_lock(&nm_mutex);
    if (clear_flags & NM_CLEAR_UIDS) {
        struct nm_uid_array *old = rcu_dereference_protected(nm_uids, lockdep_is_held(&nm_mutex));
        RCU_INIT_POINTER(nm_uids, NULL);
        if (old) kfree_rcu(old, rcu);
    }
    if (clear_flags & NM_CLEAR_RULES) {
        struct nm_leaf *leaf;
        struct hlist_node *tmp;
        int bkt;
        hash_for_each_safe(nm_leaves_ht, bkt, tmp, leaf, ht_node) {
            nm_delete_child_locked(leaf, true);
            hash_del(&leaf->ht_node);
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
            if (data_size > sizeof(payload->buffer) || offset > data_size) { payload->status = -EINVAL; break; }
            buf_ptr = payload->buffer + offset;
            buf_end = payload->buffer + data_size;
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
                err = nm_add_rule_locked(buf_ptr, buf_ptr + h.v_len, h.v_len, h.r_len, h.flags, h.uid, h.uid_end, h.deny_start, h.deny_end);
                if (!payload->status) payload->status = err;
                buf_ptr += (size_t)(h.v_len + h.r_len);
            }
            mutex_unlock(&nm_mutex);
            payload->arg1 = buf_ptr - payload->buffer;
            break;
        }

        case NM_CMD_DEL_RULE: {
            unsigned int data_size = payload->data_size, offset = payload->arg1;
            if (data_size > sizeof(payload->buffer) || offset > data_size) { payload->status = -EINVAL; break; }
            buf_ptr = payload->buffer + offset;
            buf_end = payload->buffer + data_size;
            mutex_lock(&nm_mutex);
            while (buf_ptr < buf_end) {
                struct nm_del_hdr h;
                int err;
                if ((size_t)(buf_end - buf_ptr) < sizeof(h)) { payload->status = -EINVAL; break; }
                h = *(struct nm_del_hdr *)buf_ptr;
                if (!h.v_len || h.v_len > (size_t)(buf_end - buf_ptr) - sizeof(h)) { payload->status = -EINVAL; break; }
                buf_ptr += sizeof(h);
                err = nm_del_rule_locked(buf_ptr, h.v_len, h.uid, h.uid_end, h.deny_start, h.deny_end);
                if (!payload->status) payload->status = err;
                buf_ptr += h.v_len;
            }
            mutex_unlock(&nm_mutex);
            payload->arg1 = buf_ptr - payload->buffer;
            break;
        }

        case NM_CMD_ADD_UID:
            mutex_lock(&nm_mutex);
            payload->status = nm_uid_add(payload->target_uid);
            mutex_unlock(&nm_mutex);
            break;

        case NM_CMD_DEL_UID:
            mutex_lock(&nm_mutex);
            payload->status = nm_uid_del(payload->target_uid);
            mutex_unlock(&nm_mutex);
            break;

        case NM_CMD_BLOCK_ISOLATED_UIDS:
            WRITE_ONCE(nm_block_isolated_uids, payload->arg1 != 0);
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
            if (payload->arg1 == ~0U) { payload->data_size = 0; break; }
            int current_idx = 0;
            int bkt;
            buf_ptr = payload->buffer;
            buf_end = payload->buffer + sizeof(payload->buffer);

            mutex_lock(&nm_mutex);
            struct nm_leaf *leaf;
            hash_for_each(nm_leaves_ht, bkt, leaf, ht_node) {
                struct nm_rule_array *rules = rcu_dereference_protected(leaf->rules, lockdep_is_held(&nm_mutex));
                if (!rules) continue;
                for (int i = 0; i < rules->count; i++) {
                    struct nm_rule *r = &rules->rules[i];
                    char *p = NULL;
                    int r_len_str = 0;
                    if (current_idx++ < payload->arg1) continue;

                    size_t head_len = sizeof(struct nm_rule_hdr) + leaf->v_len;
                    if (head_len > (size_t)(buf_end - buf_ptr)) goto list_full;
                    if (r->r_path.dentry) {
                        p = d_path(&r->r_path, buf_ptr + head_len, buf_end - (buf_ptr + head_len));
                        if (IS_ERR(p)) {
                            if (PTR_ERR(p) == -ENAMETOOLONG) goto list_full;
                            payload->status = PTR_ERR(p);
                            goto list_done;
                        }
                        r_len_str = strnlen(p, buf_end - p);
                    }

                    if (head_len + r_len_str > (size_t)(buf_end - buf_ptr)) goto list_full;
                    if (p && p != buf_ptr + head_len) memmove(buf_ptr + head_len, p, r_len_str);
                    *(struct nm_rule_hdr *)buf_ptr = (struct nm_rule_hdr){
                        .flags = r->flags, .v_len = leaf->v_len, .r_len = r_len_str,
                        .uid = r->target_uid, .uid_end = r->uid_end,
                        .deny_start = r->deny_start, .deny_end = r->deny_end
                    };
                    buf_ptr += sizeof(struct nm_rule_hdr);
                    memcpy(buf_ptr, leaf->paths, leaf->v_len); buf_ptr += leaf->v_len + r_len_str;
                }
            }
            current_idx = -1;
            goto list_done;
        list_full:
            current_idx--;
            if (current_idx == (int)payload->arg1) payload->status = -ENAMETOOLONG;
        list_done:
            mutex_unlock(&nm_mutex);
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
            payload->arg1 = arr && start_idx < arr->count ? start_idx : ~0U;
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
