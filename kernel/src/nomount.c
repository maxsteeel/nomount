#include <linux/init.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/statfs.h>
#include <linux/fs_struct.h>
#include "nomount.h"

static struct kmem_cache *nm_rule_cachep, *nm_dir_cachep, *nm_child_cachep, *nm_uid_cachep;
const loff_t nomount_magic_pos = 0x7E000000;
atomic_t nm_active_rules = ATOMIC_INIT(0);
atomic_t nm_active_dirs = ATOMIC_INIT(0);
DEFINE_STATIC_KEY_FALSE(nomount_active_rules);
DEFINE_STATIC_KEY_FALSE(nomount_active_dirs);

/* logs */
#define nm_debug(fmt, ...) printk(KERN_DEBUG "NoMount: [DEBUG] " fmt, ##__VA_ARGS__)
#define nm_info(fmt, ...) printk(KERN_INFO "NoMount: " fmt, ##__VA_ARGS__)
#define nm_warn(fmt, ...) printk(KERN_WARNING "NoMount: [WARN] " fmt, ##__VA_ARGS__)
#define nm_err(fmt, ...)  printk(KERN_ERR "NoMount: [ERROR] " fmt, ##__VA_ARGS__)

/*** Verification & Compatibility Checks ***/

/**
 * nomount_is_uid_blocked - Check if a specific UID is excluded from redirection
 * @uid: The User ID to check
 *
 * Returns true if the UID exists in the exclusion hash table.
 */
bool nomount_is_uid_blocked(uid_t uid) {
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

/**
 * __nomount_should_skip - Determine if the current context should bypass hooks
 *
 * Returns true if NoMount is disabled, if running in interrupt context,
 * if recursion is detected, or if the current UID is in the blocklist.
 */
static __always_inline bool __nomount_should_skip(void) {
    if (!static_branch_unlikely(&nomount_active_rules)) return true;
    if (unlikely(!in_task() || in_nmi() || oops_in_progress)) return true;
    if (unlikely(current->flags & (PF_KTHREAD | PF_EXITING))) return true;
    if (unlikely(!hash_empty(nomount_uid_ht))) {
        if (unlikely(nomount_is_uid_blocked(current_uid().val))) return true;
    }
    return false;
}

/* Exported */
bool nomount_should_skip(void) {
    return __nomount_should_skip();
}
EXPORT_SYMBOL(nomount_should_skip);

/*** Helpers & Path Resolution ***/

/**
 * __nomount_is_injected_file_rcu - Check if an inode number belongs to an injected file.
 * @ino: The inode number to check
 *
 * This function performs a lockless check against the registered rules to determine
 * if the given inode number corresponds to an injected file.
 * It checks both real and virtual inode hash tables.
 *
 * NOTE: The caller MUST hold rcu_read_lock() before calling this function
 * and keep it held as long as the result is being used.
 */
static inline bool __nomount_is_injected_file_rcu(unsigned long ino) {
    struct nomount_rule *rule;
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, ino) {
        if (rule->real_ino == ino) return true;
    }
    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, ino) {
        if (rule->v_ino == ino) return true;
    }
    return false;
}

/**
 * __nomount_is_traversal_allowed_rcu - Check if an inode number corresponds to a 
 * directory with traversal permissions
 * @ino: The inode number to check
 *
 * This function checks if the given inode number is registered as a directory that allows traversal.
 *
 * NOTE: The caller MUST hold rcu_read_lock() before calling this function
 * and keep it held as long as the result is being used.
 */
static inline bool __nomount_is_traversal_allowed_rcu(unsigned long ino) {
    struct nomount_dir_node *dir;
    hash_for_each_possible_rcu(nomount_dirs_ht, dir, node, ino) {
        if (dir->dir_ino == ino) return true;
    }
    return false;
}

/**
 * nomount_build_path_from_pwd - Construct an absolute path using the current working directory
 * @rel_name: The relative filename to append to the current working directory
 *
 * This helper is used to reconstruct an absolute path for operations that provide
 * a relative filename without a DFD, ensuring that NoMount can still resolve the intended target.
 *
 * Returns an __getname() buffer containing the absolute path, or NULL on failure.
 * Caller must free the returned buffer using __putname().
 */
static char *nomount_build_path_from_pwd(const char *rel_name, size_t name_len, size_t *out_len, const char **out_path) 
{
    struct path pwd;
    char *cwd_str, *page_buf = __getname();
    size_t dir_len;

    if (!page_buf) return NULL;

    get_fs_pwd(current->fs, &pwd);
    cwd_str = d_path(&pwd, page_buf, PATH_MAX);
    path_put(&pwd);

    if (IS_ERR_OR_NULL(cwd_str)) {
        __putname(page_buf);
        return NULL;
    }

    dir_len = strlen(cwd_str);
    if (likely(dir_len + name_len + 2 <= PATH_MAX)) {
        char *end_ptr = page_buf + dir_len; 
        if (dir_len > 0 && cwd_str[dir_len - 1] != '/') {
            *end_ptr = '/'; end_ptr++; dir_len++;
        }
        memcpy(end_ptr, rel_name, name_len + 1);
        if (out_len) *out_len = dir_len + name_len;
        *out_path = cwd_str;
        return page_buf;
    }

    __putname(page_buf);
    return NULL;
}

/**
 * nomount_drop_vpath_cache - Force VFS to drop dcache for a specific path
 * @path_str: The native absolute path to flush
 * @is_dir: True if we should aggressively invalidate a directory
 */
static void nomount_drop_vpath_cache(const char *path_str, bool is_dir)
{
    struct path path;
    if (kern_path(path_str, 0, &path) == 0) {
        if (is_dir) {
            d_invalidate(path.dentry);
        } else {
            d_drop(path.dentry);
        }
        path_put(&path);
    }
}

/**
 * nomount_get_rule_by_ino - Look up the registered rule for an inode
 * @inode: The inode to query
 *
 * NOTE: The caller MUST hold rcu_read_lock() before calling this function
 * and keep it held as long as the returned rule is being used.
 */
struct nomount_rule *nomount_get_rule_by_ino(struct inode *inode) {
    struct nomount_rule *rule;
    unsigned long ino = inode->i_ino;
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, ino) {
        if (rule->real_ino == ino) return rule;
    }
    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, ino) {
        if (rule->v_ino == ino) return rule;
    }
    return NULL;
}

/**
 * nomount_get_rule_by_path - Look up the rule for a virtual path
 * @pathname: The requested virtual path
 *
 * Performs a fast hash lookup to find redirection rules.
 * Returns a pointer to the rule, or NULL if no rule matches.
 *
 * NOTE: The caller MUST hold rcu_read_lock() before calling this function
 * and keep it held as long as the returned rule is being used.
 */
struct nomount_rule *nomount_get_rule_by_path(const char *pathname, size_t len) {
    struct nomount_rule *rule;
    u32 hash = full_name_hash(NULL, pathname, len);
    hash_for_each_possible_rcu(nomount_rules_by_vpath, rule, vpath_node, hash) {
        if (rule->v_hash == hash && rule->vp_len == len &&
             memcmp(pathname, rule->virtual_path, len) == 0) {
            return rule;
        }
    }
    return NULL;
}

/*** VFS Hooks & Injection Logic ***/

/**
 * nomount_handle_dpath - Intercept d_path calls to hide real locations
 * @path: The path struct being resolved
 * @buf: The buffer to write the result into
 * @buflen: Length of the buffer
 *
 * Replaces the real physical path of an injected file with its intended 
 * virtual path to prevent information leaks in Userspace.
 * 
 * Returns a pointer within the buffer where the virtual path begins.
 */
char *nomount_handle_dpath(const struct path *path, char *buf, int buflen) 
{
    struct nomount_rule *rule;
    char *res; int len;

    if (unlikely(IS_ERR_OR_NULL(path) || !path->dentry || !path->dentry->d_inode)) return NULL;
    if (__nomount_should_skip()) return NULL;

    rcu_read_lock();
    rule = nomount_get_rule_by_ino(path->dentry->d_inode);

    if (likely(rule)) {
        len = rule->vp_len;
        if (likely(buflen >= len + 1)) {
            res = buf + buflen - len - 1;
            *res = '\0';
            memcpy(res, rule->virtual_path, len);
            rcu_read_unlock();
            return res;
        }
    }

    rcu_read_unlock();
    return NULL;
}

/**
 * nomount_allow_access - Enforce permissions for injected structure
 * @inode: The inode being accessed
 * @mask: The requested permission mask
 *
 * Return: > 0 to bypass native checks (allow read/exec), 
 *         < 0 to explicitly deny (block writes), 
 *           0 to fallback to standard VFS permissions.
 */
int nomount_allow_access(struct inode *inode, int mask)
{
    bool is_injected = false, is_dir = false;
    unsigned long ino;

    if (!inode || IS_ERR_OR_NULL(inode)) return 0;
    if (__nomount_should_skip()) return 0;

    ino = inode->i_ino;

    if (unlikely(!__nomount_should_skip())) {
        rcu_read_lock();
        is_injected = __nomount_is_injected_file_rcu(ino);
        if (!is_injected) {
            is_dir = __nomount_is_traversal_allowed_rcu(ino);
        }
        rcu_read_unlock();

        if (is_dir && !is_injected) {
            if (mask & (MAY_READ | MAY_WRITE | MAY_APPEND))
                return 0;

            if (mask & MAY_EXEC)
                return 1;
        }

        if (is_injected) {
            if (mask & (MAY_WRITE | MAY_APPEND))
                return 0;

            return 1; 
        }
    }

    return 0;
}

/**
 * nomount_getname_hook - Redirect paths during filename struct creation
 * @name: The original filename struct requested by userspace
 *
 * This is the primary entry point for path redirection. If the requested 
 * path matches a rule, it alters the filename struct to point to the real 
 * physical location on disk.
 * 
 * Returns the modified filename struct, or the original if no match.
 */
struct filename *nomount_getname_hook(struct filename *name)
{
    struct nomount_rule *rule;
    char *abs_path = NULL;
    const char *check_name, *s, *last_slash;
    size_t name_len, b_len, r_len;
    bool basename_match = false;
    u32 b_hash;

    if (unlikely(__nomount_should_skip()))
        return name;

    if (unlikely(IS_ERR_OR_NULL(name) || !name->name))
        return name;

    s = name->name;
    name_len = strlen(s);
    if (unlikely(name_len == 1 && s[0] == '/'))
        return name;

    last_slash = strrchr(s, '/');
    check_name = (last_slash && *(last_slash + 1) != '\0') ? last_slash + 1 : s;
    b_len = name_len - (check_name - s);
    b_hash = full_name_hash(NULL, check_name, b_len);

    rcu_read_lock();
    if (unlikely(s[0] == '/' && current_uid().val >= AID_APP_START)) {
        const char *p = s + name_len;
        while (p > s) {
            if (*p == '/') {
                size_t sub_len = (p == s) ? 1 : (p - s);
                u32 h = full_name_hash(NULL, s, sub_len);
                struct nomount_dir_node *priv_dir;
                hash_for_each_possible_rcu(nomount_private_dirs_ht, priv_dir, private_hash_node, h) {
                    if (priv_dir->dir_hash == h &&
                        priv_dir->dir_path_len == sub_len &&
                        memcmp(priv_dir->dir_path, s, sub_len) == 0) {
                        nm_debug("Access to private directory blocked: %.*s\n", (int)sub_len, s);
                        goto out_unlock2;
                    }
                }
            }
            p--;
        }
    }

    hash_for_each_possible_rcu(nomount_basenames_ht, rule, basename_node, b_hash) {
        if (rule->b_len == b_len && memcmp(rule->basename, check_name, b_len) == 0) {
            basename_match = true;
            break;
        }
    }

    if (unlikely(!basename_match)) goto out_unlock;

    check_name = s;
    r_len = name_len;
    if (unlikely(s[0] != '/')) {
        abs_path = nomount_build_path_from_pwd(s, name_len, &r_len, &check_name);
        if (!abs_path) goto out_unlock;
    }

    rule = nomount_get_rule_by_path(check_name, r_len);
    if (likely(rule)) {
        memcpy((char *)name->name, rule->real_path, rule->rp_len);
        ((char *)name->name)[rule->rp_len] = '\0';
        nm_debug("Redirected: %s -> %s\n", check_name, rule->real_path);
    }

    if (abs_path) __putname(abs_path);

out_unlock:
    rcu_read_unlock();
    return name;

out_unlock2:
    rcu_read_unlock();
    putname(name);
    return ERR_PTR(-ENOENT);
}

/*** Directory Injection ***/

/**
 * nomount_vfs_inject_dir - Inject fake directory entries at the VFS level
 * @file: The directory file being iterated
 * @ctx: The VFS directory context
 *
 * This function is called during the filldir phase of a readdir operation. 
 * It checks if the current directory has any associated injected entries and,
 * if so, appends them to the directory listing being constructed for userspace.
 * This ensures that tools like 'ls' will see the injected files as part of the directory contents.
 */
void nomount_vfs_inject_dir(struct file *file, struct dir_context *ctx)
{
    struct nomount_dir_node *curr_dir;
    struct nomount_child_name *child;
    struct inode *dir_inode = file_inode(file);
    unsigned long v_index;

    if (!dir_inode || __nomount_should_skip()) return;
    if (!static_branch_unlikely(&nomount_active_dirs)) return;

    if (ctx->pos >= nomount_magic_pos && ctx->pos < nomount_magic_pos + 100000) {
        v_index = (unsigned long)(ctx->pos - nomount_magic_pos);
    } else {
        v_index = 0;
        ctx->pos = nomount_magic_pos;
    }

    down_read(&nomount_dirs_rwsem); 
    hash_for_each_possible(nomount_dirs_ht, curr_dir, node, dir_inode->i_ino) {
        if (curr_dir->dir_ino == dir_inode->i_ino) {
            list_for_each_entry(child, &curr_dir->children_names, list) {
                if (child->v_index < v_index) continue;
                if (!dir_emit(ctx, child->name, child->name_len, child->fake_ino, child->d_type))
                    break;
                ctx->pos = nomount_magic_pos + child->v_index + 1;
            }
            break; 
        }
    }
    up_read(&nomount_dirs_rwsem);
}

/**
 * __nomount_get_or_create_dir - Factory function to retrieve or create a directory node
 * @ino: Inode number of the directory
 *
 * Checks if a directory node already exists for the given inode. If not, allocates
 * a new node from nm_dir_cachep, initializes its lists, and adds it to the global
 * hash table.
 *
 * Return a pointer to the nomount_dir_node on success, NULL on failure (ENOMEM).
 */
static struct nomount_dir_node* __nomount_get_or_create_dir(unsigned long ino)
{
    struct nomount_dir_node *dir_node, *curr;

    hash_for_each_possible(nomount_dirs_ht, curr, node, ino) {
        if (curr->dir_ino == ino) return curr;
    }

    dir_node = kmem_cache_alloc(nm_dir_cachep, GFP_KERNEL);
    if (unlikely(!dir_node)) return NULL;

    dir_node->dir_ino = ino;
    dir_node->is_private = false;
    dir_node->dir_path = NULL;
    dir_node->dir_path_len = 0;
    dir_node->next_child_index = 0;
    INIT_LIST_HEAD(&dir_node->children_names);
    hash_init(dir_node->children_ht);

    hash_add_rcu(nomount_dirs_ht, &dir_node->node, ino);
    atomic_inc(&nm_active_dirs);
    if (atomic_read(&nm_active_dirs) == 1)
        static_branch_enable(&nomount_active_dirs);

    return dir_node;
}


/* __nomount_collect_parents - Walks the dentry tree to register directory hierarchy
 * @rule: The rule containing the absolute real_path string
 * @d: A valid referenced dentry resolved from kern_path
 *
 * This function recursively climbs the dentry tree starting from the provided 
 * dentry. It registers every parent inode encountered and handles the extraction 
 * of private directory paths automatically when traversal permissions are restricted.
 *
 * This function relies on the caller to provide a valid reference (dget).
 */
static void __nomount_collect_parents(struct nomount_rule *rule, struct dentry *d)
{
    struct dentry *parent;
    char *r_tmp = rule->real_path, *slash, *slashes[32];
    int p_count = 0;

    while (d && !IS_ROOT(d) && p_count < 32) {
        struct inode *inode = d_backing_inode(d);
        if (likely(inode && S_ISDIR(inode->i_mode))) {
            struct nomount_dir_node *dir_node = __nomount_get_or_create_dir(inode->i_ino);
            if (likely(dir_node) && unlikely(!(inode->i_mode & S_IXOTH) && !dir_node->dir_path)) {
                dir_node->is_private = true;
                nm_debug("Registered private dir: %s (ino: %lu)\n", r_tmp, inode->i_ino);
                dir_node->dir_path_len = strlen(r_tmp);
                dir_node->dir_path = kmemdup_nul(r_tmp, dir_node->dir_path_len, GFP_KERNEL);
                if (likely(dir_node->dir_path)) {
                    u32 hash = full_name_hash(NULL, r_tmp, dir_node->dir_path_len);
                    dir_node->dir_hash = hash;
                    hash_add_rcu(nomount_private_dirs_ht, &dir_node->private_hash_node, hash);
                }
            }
        }

        slash = strrchr(r_tmp, '/');
        if (!slash || slash == r_tmp) break;
        *slash = '\0';
        slashes[p_count++] = slash;

        parent = dget_parent(d);
        dput(d);
        d = parent;
    }

    if (d) dput(d);
    while (p_count > 0) *slashes[--p_count] = '/';
}

/**
 * __nomount_inject_child_locked - Atomically inserts a virtual child into a parent
 * @dir_node: The parent directory node to inject into
 * @name: Filename of the child
 * @name_len: Length of the name string
 * @name_hash: Precalculated hash of the name string
 * @type: File type (DT_DIR, DT_REG, etc.)
 * @child_fake_ino: The synthetic inode number for the virtual file
 *
 * This function performs an hash check to see if the child already exists 
 * to prevent duplicates, then appends it to the directory's child list.
 *
 * This function assumes the caller is already holding the appropriate rwsem or mutex to 
 * ensure the integrity of the children_ht and children_names list.
 */
static void __nomount_inject_child_locked(struct nomount_dir_node *dir_node,
                                          const char *name, size_t name_len, u32 name_hash,
                                          unsigned char type, unsigned long child_fake_ino)
{
    struct nomount_child_name *child;

    hash_for_each_possible(dir_node->children_ht, child, hash_node, name_hash) {
        if (child->name_len == name_len && !memcmp(child->name, name, name_len))
            return;
    }

    child = kmem_cache_alloc(nm_child_cachep, GFP_KERNEL);
    if (unlikely(!child)) return;

    memcpy(child->name, name, name_len + 1);
    child->name_len = (u16)name_len;
    child->d_type = type;
    child->fake_ino = child_fake_ino;
    child->v_index = dir_node->next_child_index++;

    list_add_tail_rcu(&child->list, &dir_node->children_names);
    hash_add_rcu(dir_node->children_ht, &child->hash_node, name_hash);
}

/**
 * nomount_generate_virtual_topology - Autogenerates intermediate directory rules
 * @rule: The main rule being added
 *
 * Walks the path backwards using in-place mutation to find the closest
 * native parent, inherits its metadata (s_dev, s_magic), and auto-injects
 * intermediate virtual directory rules to satisfy VFS lookups.
 *
 * Returns 0 on success, or negative error code (e.g., -ENOMEM) on failure.
 */
static int nomount_generate_virtual_topology(struct nomount_rule *rule)
{
    struct nomount_rule *ex, *irule = NULL, *pending_rules[32];
    char *v_tmp = rule->virtual_path, *r_tmp = rule->real_path;
    char *b_slash, *slashes_v[32], *slashes_r[32];
    int cur_v_len = rule->vp_len, cur_r_len = rule->rp_len;
    int child_len = rule->vp_len, current_flags = rule->flags;
    unsigned long inherited_dev = 0, inherited_fs_type = 0;
    int p_count = 0, err = 0;
    const char *b_name_inter, *child_name;
    bool inter_exists;
    size_t child_name_len;
    u32 child_name_hash, h_inter;

    while (p_count < 32) {
        char *slash_v, *slash_r;
        struct path p_path;
        unsigned long current_parent_ino;

        slash_v = strrchr(v_tmp, '/');
        slash_r = r_tmp ? strrchr(r_tmp, '/') : NULL; 
        if (slash_r == r_tmp) slash_r = NULL;
        if (!slash_v || slash_v == v_tmp) {
            if (likely(kern_path("/", LOOKUP_FOLLOW, &p_path) == 0)) {
                current_parent_ino = d_backing_inode(p_path.dentry)->i_ino;
                child_name = v_tmp + 1;
                child_name_len = strlen(child_name);
                child_name_hash = full_name_hash(NULL, child_name, child_name_len);
                __nomount_inject_child_locked(__nomount_get_or_create_dir(current_parent_ino),
                                              child_name, child_name_len, child_name_hash,
                                              (current_flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG,
                                              full_name_hash(NULL, rule->virtual_path, child_len)); 
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

        hash_for_each_possible(nomount_rules_by_vpath, ex, vpath_node, h_inter) {
            if (ex->vp_len == cur_v_len && memcmp(ex->virtual_path, v_tmp, cur_v_len) == 0) {
                inherited_dev = ex->v_dev;
                inherited_fs_type = ex->v_fs_type;
                current_parent_ino = ex->v_ino;
                inter_exists = true;
                break;
            }
        }

        if (inter_exists) {
        child_name = slash_v + 1;
        child_name_len = strlen(child_name);
        child_name_hash = full_name_hash(NULL, child_name, child_name_len);
        __nomount_inject_child_locked(__nomount_get_or_create_dir(current_parent_ino),
                                      child_name, child_name_len, child_name_hash,
                                      (current_flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG,
                                      full_name_hash(NULL, rule->virtual_path, child_len));
            break;
        }

        if (likely(kern_path(v_tmp, LOOKUP_FOLLOW, &p_path) == 0)) {
            inherited_dev = p_path.dentry->d_sb->s_dev;
            if (p_path.dentry->d_sb->s_op->statfs) {
                struct kstatfs st;
                p_path.dentry->d_sb->s_op->statfs(p_path.dentry, &st);
                inherited_fs_type = st.f_type;
            } else {
                inherited_fs_type = p_path.dentry->d_sb->s_magic;
            }
            current_parent_ino = d_backing_inode(p_path.dentry)->i_ino;
            child_name = slash_v + 1;
            child_name_len = strlen(child_name);
            child_name_hash = full_name_hash(NULL, child_name, child_name_len);
            __nomount_inject_child_locked(__nomount_get_or_create_dir(current_parent_ino),
                                          child_name, child_name_len, child_name_hash,
                                          (current_flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG,
                                          full_name_hash(NULL, rule->virtual_path, child_len));
            path_put(&p_path);
            break; 
        } else {
            pending_rules[p_count - 1] = kmem_cache_alloc(nm_rule_cachep, GFP_KERNEL);
            if (unlikely(!pending_rules[p_count - 1])) {
                err = -ENOMEM;
                break;
            }

            irule = pending_rules[p_count - 1];

            INIT_LIST_HEAD(&irule->list);
            INIT_HLIST_NODE(&irule->v_ino_node);
            INIT_HLIST_NODE(&irule->real_ino_node);
            INIT_HLIST_NODE(&irule->vpath_node);
            INIT_HLIST_NODE(&irule->basename_node);

            irule->virtual_path = kmemdup_nul(v_tmp, cur_v_len, GFP_KERNEL);
            irule->real_path = slash_r ? kmemdup_nul(r_tmp, cur_r_len, GFP_KERNEL) : kstrdup("/", GFP_KERNEL);

            if (unlikely(!irule->virtual_path || !irule->real_path)) {
                if (irule->virtual_path) kfree(irule->virtual_path);
                if (irule->real_path) kfree(irule->real_path);
                kmem_cache_free(nm_rule_cachep, irule);
                pending_rules[p_count - 1] = NULL;
                err = -ENOMEM;
                break;
            }

            irule->vp_len = (u16)cur_v_len;
            irule->rp_len = (u16)(slash_r ? cur_r_len : 1);

            b_slash = strrchr(irule->virtual_path, '/');
            b_name_inter = b_slash ? b_slash + 1 : irule->virtual_path;
            irule->basename = b_name_inter;
            irule->b_len = (u16)strlen(b_name_inter);

            irule->v_hash = h_inter;
            irule->v_ino = (unsigned long)h_inter;
            irule->flags = NM_FLAG_IS_DIR;
            irule->real_ino = 0;
            irule->real_dev = 0;

            if (slash_r) {
                struct path r_path_struct;
                if (likely(kern_path(irule->real_path, LOOKUP_FOLLOW, &r_path_struct) == 0)) {
                    irule->real_ino = d_backing_inode(r_path_struct.dentry)->i_ino;
                    irule->real_dev = r_path_struct.dentry->d_sb->s_dev;
                    path_put(&r_path_struct);
                }
            }
        }
        current_flags = NM_FLAG_IS_DIR;
        child_len = cur_v_len;
    }

    while (p_count > 0) {
        p_count--;
        if (slashes_v[p_count]) *slashes_v[p_count] = '/';
        if (slashes_r[p_count]) *slashes_r[p_count] = '/';

        if (pending_rules[p_count]) {
            irule = pending_rules[p_count];

            if (likely(err == 0)) {
                u32 bh = full_name_hash(NULL, irule->basename, irule->b_len);
                irule->v_dev = inherited_dev;
                irule->v_fs_type = inherited_fs_type;

                hash_add_rcu(nomount_basenames_ht, &irule->basename_node, bh);
                hash_add_rcu(nomount_rules_by_vpath, &irule->vpath_node, irule->v_hash);
                if (irule->real_ino)
                    hash_add_rcu(nomount_rules_by_real_ino, &irule->real_ino_node, irule->real_ino);

                hash_add_rcu(nomount_rules_by_v_ino, &irule->v_ino_node, irule->v_ino);
                list_add_tail_rcu(&irule->list, &nomount_rules_list);
                atomic_inc(&nm_active_rules);
                if (atomic_read(&nm_active_rules) == 1) static_branch_enable(&nomount_active_rules);
            } else {
                kfree(irule->virtual_path);
                kfree(irule->real_path);
                kmem_cache_free(nm_rule_cachep, irule);
            }
        }
    }

    if (likely(err == 0)) {
        rule->v_dev = inherited_dev;
        rule->v_fs_type = inherited_fs_type;
    }

    return err;
}

/*** Metadata Spoofing ***/

/**
 * nomount_spoof_stat - Forge stat data for injected files
 * @path: The path being evaluated
 * @stat: The stat struct to modify
 *
 * Alters the returned inode and device ID to match the virtual path's 
 * expected location, rather than exposing the physical /data identifiers.
 */
void nomount_spoof_stat(const struct path *path, struct kstat *stat)
{
    struct nomount_rule *rule;
    struct inode *inode;

    if (IS_ERR_OR_NULL(path) || IS_ERR_OR_NULL(stat) || __nomount_should_skip()) return;
    inode = d_backing_inode(path->dentry);
    if (!inode) return;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino && rule->real_dev == inode->i_sb->s_dev) {
            stat->ino = READ_ONCE(rule->v_ino);
            if (rule->v_dev != 0)
                stat->dev = READ_ONCE(rule->v_dev);
            break;
        }
    }
    rcu_read_unlock();
}

/**
 * nomount_spoof_statfs - Forge filesystem type data
 * @path: The path being evaluated
 * @buf: The statfs struct to modify
 *
 * Injects the correct Magic Number (e.g., ext4, erofs) to match the 
 * virtual partition, preventing detection via filesystem type checks.
 */
void nomount_spoof_statfs(const struct path *path, struct kstatfs *buf)
{
    struct nomount_rule *rule;
    struct inode *inode;

    if (IS_ERR_OR_NULL(path) || IS_ERR_OR_NULL(buf) || __nomount_should_skip()) return;
    inode = d_backing_inode(path->dentry);
    if (!inode) return;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino && rule->real_dev == inode->i_sb->s_dev) {
            if (rule->v_fs_type != 0)
                buf->f_type = READ_ONCE(rule->v_fs_type);
            goto unlock;
        }
    }

    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, inode->i_ino) {
        if (rule->v_ino == inode->i_ino && rule->v_dev == inode->i_sb->s_dev) {
            if (rule->v_fs_type != 0)
                buf->f_type = READ_ONCE(rule->v_fs_type);
            break;
        }
    }

unlock:
    rcu_read_unlock();
}

/**
 * nomount_spoof_mmap_metadata - Forge VMA metadata for /proc/self/maps
 * @inode: The underlying inode of the mapped memory
 * @dev: Pointer to the device ID variable to overwrite
 * @ino: Pointer to the inode number variable to overwrite
 *
 * Ensures that shared libraries or binaries executed via NoMount show 
 * the correct virtual device and inode in process memory maps.
 * 
 * Returns true if the metadata was spoofed.
 */
bool nomount_spoof_mmap_metadata(struct inode *inode, dev_t *dev, unsigned long *ino)
{
    struct nomount_rule *rule;
    bool found = false;

    if (unlikely(IS_ERR_OR_NULL(inode) || IS_ERR_OR_NULL(dev) ||
                  IS_ERR_OR_NULL(ino) || __nomount_should_skip()))
        return false;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino) {
            *dev = READ_ONCE(rule->v_dev);
            *ino = READ_ONCE(rule->v_ino);
            found = true;
            break;
        }
    }
    rcu_read_unlock();

    return found;
}

/**
 * nomount_handle_getattr - Wrapper for vfs_getattr intercept
 * @ret: The return code from the native vfs_getattr execution
 * @path: The path being evaluated
 * @stat: The stat struct populated by the kernel
 *
 * Applies the stat spoofing logic only if the original lookup succeeded.
 * Returns the original return code.
 */
int nomount_handle_getattr(int ret, const struct path *path, struct kstat *stat)
{
    if (likely(ret == 0) && !__nomount_should_skip()) {
        nomount_spoof_stat(path, stat);
    }
    return ret;
}

/*** Generic Netlink API & Module Management ***/

static int __nomount_add_rule(const char *v_path, const char *r_path, u16 v_len, u16 r_len, u32 flags)
{
    struct nomount_rule *rule, *existing, *victim = NULL;
    struct path path_main, r_path_struct_main;
    struct dentry *r_path_dentry = NULL;
    char *slash;
    const char *b_name;
    u32 hash, b_hash;
    int err = 0;
    bool v_path_exists = false;

    if (!v_path || !r_path) return -EINVAL;

    hash = full_name_hash(NULL, v_path, v_len);
    rule = kmem_cache_alloc(nm_rule_cachep, GFP_KERNEL);
    if (!rule)
        return -ENOMEM;

    rule->virtual_path = kmemdup_nul(v_path, v_len, GFP_KERNEL);
    rule->real_path = kmemdup_nul(r_path, r_len, GFP_KERNEL);

    if (!rule->virtual_path || !rule->real_path) {
        if (rule->virtual_path) kfree(rule->virtual_path);
        if (rule->real_path) kfree(rule->real_path);
        kmem_cache_free(nm_rule_cachep, rule);
        return -ENOMEM;
    }

    INIT_LIST_HEAD(&rule->list);
    INIT_HLIST_NODE(&rule->v_ino_node);
    INIT_HLIST_NODE(&rule->real_ino_node);
    INIT_HLIST_NODE(&rule->vpath_node);
    INIT_HLIST_NODE(&rule->basename_node);

    slash = strrchr(rule->virtual_path, '/');
    b_name = slash ? slash + 1 : rule->virtual_path;
    rule->basename = b_name;
    rule->b_len = strlen(b_name);
    rule->vp_len = v_len;
    rule->rp_len = r_len;
    rule->v_hash = hash;
    rule->flags = flags;
    rule->real_ino = 0;
    rule->real_dev = 0;

    if (kern_path(rule->real_path, LOOKUP_FOLLOW, &r_path_struct_main) == 0) {
        struct inode *r_inode = d_backing_inode(r_path_struct_main.dentry);
        rule->real_ino = r_inode->i_ino;
        rule->real_dev = r_path_struct_main.dentry->d_sb->s_dev;
        if (S_ISDIR(r_inode->i_mode)) {
            rule->flags |= NM_FLAG_IS_DIR;
        }
        r_path_dentry = dget(r_path_struct_main.dentry);
        path_put(&r_path_struct_main);
    }

    if (kern_path(rule->virtual_path, LOOKUP_FOLLOW, &path_main) == 0) {
        rule->v_ino = d_backing_inode(path_main.dentry)->i_ino;
        rule->v_dev = path_main.dentry->d_sb->s_dev;
        if (path_main.dentry->d_sb->s_op->statfs) {
            struct kstatfs st;
            path_main.dentry->d_sb->s_op->statfs(path_main.dentry, &st);
            rule->v_fs_type = st.f_type;
        } else {
            rule->v_fs_type = path_main.dentry->d_sb->s_magic;
        }
        path_put(&path_main);
        v_path_exists = true;
        nm_debug("Resolved physical backing for %s (ino: %lu)\n", rule->virtual_path, rule->v_ino);
    } else {
        rule->v_ino = (unsigned long)hash;
    }

    mutex_lock(&nomount_write_mutex);
    down_write(&nomount_dirs_rwsem);

    hash_for_each_possible(nomount_rules_by_vpath, existing, vpath_node, hash) {
        if (existing->v_hash == hash && existing->vp_len == v_len &&
             memcmp(existing->virtual_path, rule->virtual_path, v_len) == 0) {
            hash_del_rcu(&existing->vpath_node);
            hash_del_rcu(&existing->basename_node);
            if (existing->real_ino) hash_del_rcu(&existing->real_ino_node);
            if (existing->v_ino) hash_del_rcu(&existing->v_ino_node);
            list_del_rcu(&existing->list);
            atomic_dec(&nm_active_rules);
            victim = existing;
            nm_info("Shadowing existing rule for: %s\n", rule->virtual_path);
            break;
        }
    }

    if (!v_path_exists) {
        err = nomount_generate_virtual_topology(rule);
        if (err != 0) {
            up_write(&nomount_dirs_rwsem);
            mutex_unlock(&nomount_write_mutex);
            if (r_path_dentry) dput(r_path_dentry);
            kfree(rule->virtual_path);
            kfree(rule->real_path);
            kmem_cache_free(nm_rule_cachep, rule);
            return err;
        }
    }
    
    if (r_path_dentry)
        __nomount_collect_parents(rule, r_path_dentry);

    b_hash = full_name_hash(NULL, rule->basename, rule->b_len);
    hash_add_rcu(nomount_basenames_ht, &rule->basename_node, b_hash);
    hash_add_rcu(nomount_rules_by_vpath, &rule->vpath_node, hash);
    if (rule->real_ino)
        hash_add_rcu(nomount_rules_by_real_ino, &rule->real_ino_node, rule->real_ino);
    if (rule->v_ino)
        hash_add_rcu(nomount_rules_by_v_ino, &rule->v_ino_node, rule->v_ino);

    list_add_tail_rcu(&rule->list, &nomount_rules_list);
    atomic_inc(&nm_active_rules);
    if (atomic_read(&nm_active_rules) == 1) static_branch_enable(&nomount_active_rules);
    nomount_drop_vpath_cache(rule->virtual_path, (rule->flags & NM_FLAG_IS_DIR));

    up_write(&nomount_dirs_rwsem);
    mutex_unlock(&nomount_write_mutex);

    if (unlikely(victim)) {
        synchronize_rcu();
        kfree(victim->virtual_path);
        kfree(victim->real_path);
        kmem_cache_free(nm_rule_cachep, victim);
    }

    nm_info("Successfully added rule: %s -> %s\n", rule->virtual_path, rule->real_path);
    return 0;
}

static void __nomount_del_rule(const char *v_path, size_t v_len, 
                                        struct list_head *r_victims, 
                                        struct list_head *c_victims, 
                                        struct hlist_head *d_victims)
{
    struct nomount_rule *rule;
    struct nomount_dir_node *dir;
    struct nomount_child_name *child;
    u32 hash = full_name_hash(NULL, v_path, v_len);
    int bkt;

    hash_for_each_possible(nomount_rules_by_vpath, rule, vpath_node, hash) {
        if (rule->v_hash == hash && rule->vp_len == v_len && 
             memcmp(rule->virtual_path, v_path, v_len) == 0) {
            hash_del_rcu(&rule->vpath_node);
            hash_del_rcu(&rule->basename_node);
            if (rule->real_ino) hash_del_rcu(&rule->real_ino_node);
            if (rule->v_ino) hash_del_rcu(&rule->v_ino_node);
            list_del_rcu(&rule->list);
            atomic_dec(&nm_active_rules);
            if (atomic_read(&nm_active_rules) == 0) static_branch_disable(&nomount_active_rules);
            list_add_tail(&rule->list, r_victims);

            hash_for_each(nomount_dirs_ht, bkt, dir, node) {
                list_for_each_entry(child, &dir->children_names, list) {
                    if (child->fake_ino == hash) {
                        list_del_rcu(&child->list);
                        list_add_tail(&child->list, c_victims);
                        
                        if (list_empty(&dir->children_names)) {
                            hash_del_rcu(&dir->node);
                            atomic_dec(&nm_active_dirs);
                            if (atomic_read(&nm_active_dirs) == 0)
                                static_branch_disable(&nomount_active_dirs);
                            hlist_add_head(&dir->node, d_victims);
                        }
                        goto ghost_found;
                    }
                }
            }
ghost_found:
            break;
        }
    }
}

static void __nomount_clear_all(void)
{
    struct nomount_rule *rule, *tmp_rule;
    struct nomount_uid_node *uid_node;
    struct nomount_dir_node *dir_node;
    struct nomount_child_name *child, *tmp_child;
    struct hlist_node *hlist_tmp;
    LIST_HEAD(rule_victims);
    LIST_HEAD(dir_victims_children);
    HLIST_HEAD(uid_victims);
    HLIST_HEAD(dir_victims);
    int bkt;

    list_for_each_entry_safe(rule, tmp_rule, &nomount_rules_list, list) {
        hash_del_rcu(&rule->vpath_node);
        hash_del_rcu(&rule->basename_node);
        if (rule->real_ino) hash_del_rcu(&rule->real_ino_node);
        if (rule->v_ino) hash_del_rcu(&rule->v_ino_node);
        nomount_drop_vpath_cache(rule->virtual_path, (rule->flags & NM_FLAG_IS_DIR));
        list_move_tail(&rule->list, &rule_victims);
    }

    hash_for_each_safe(nomount_uid_ht, bkt, hlist_tmp, uid_node, node) {
        hash_del_rcu(&uid_node->node);
        hlist_add_head(&uid_node->node, &uid_victims);
    }

    hash_for_each_safe(nomount_private_dirs_ht, bkt, hlist_tmp, dir_node, private_hash_node) {
        hash_del_rcu(&dir_node->private_hash_node);
    }

    hash_for_each_safe(nomount_dirs_ht, bkt, hlist_tmp, dir_node, node) {
        hash_del_rcu(&dir_node->node);
        list_for_each_entry_safe(child, tmp_child, &dir_node->children_names, list) {
            list_move_tail(&child->list, &dir_victims_children);
        }
        hlist_add_head(&dir_node->node, &dir_victims);
    }

    atomic_set(&nm_active_rules, 0);
    atomic_set(&nm_active_dirs, 0);
    static_branch_disable(&nomount_active_rules);
    static_branch_disable(&nomount_active_dirs);

    synchronize_rcu();

    hlist_for_each_entry_safe(dir_node, hlist_tmp, &dir_victims, node) {
        kfree(dir_node->dir_path);
        kmem_cache_free(nm_dir_cachep, dir_node);
    }
    list_for_each_entry_safe(child, tmp_child, &dir_victims_children, list) {
        kfree(child);
    }
    list_for_each_entry_safe(rule, tmp_rule, &rule_victims, list) {
        kfree(rule->virtual_path);
        kfree(rule->real_path);
        kmem_cache_free(nm_rule_cachep, rule);
    }
    hlist_for_each_entry_safe(uid_node, hlist_tmp, &uid_victims, node) {
        kmem_cache_free(nm_uid_cachep, uid_node);
    }
}

static struct genl_family nomount_genl_family;

static int nomount_genl_add_rule(struct sk_buff *skb, struct genl_info *info)
{
    if (info->attrs[NOMOUNT_ATTR_PAYLOAD]) {
        struct nlattr *attr = info->attrs[NOMOUNT_ATTR_PAYLOAD];
        const char *data = nla_data(attr);
        char *v_buf = __getname();
        char *r_buf = __getname();
        int len = nla_len(attr);
        int pos = 0, err = 0;

        if (!v_buf || !r_buf) {
            if (v_buf) __putname(v_buf);
            if (r_buf) __putname(r_buf);
            return -ENOMEM;
        }

        while (pos + 8 <= len) {
            u32 flags = *(u32 *)(data + pos);
            u16 vp_len = *(u16 *)(data + pos + 4);
            u16 rp_len = *(u16 *)(data + pos + 6);
            pos += 8;

            if (pos + vp_len + rp_len > len) break;

            memcpy(v_buf, data + pos, vp_len);
            v_buf[vp_len] = '\0';
            pos += vp_len;
            memcpy(r_buf, data + pos, rp_len);
            r_buf[rp_len] = '\0';
            pos += rp_len;

            err = __nomount_add_rule(v_buf, r_buf, vp_len, rp_len, flags);
            if (err) {
                nm_err("Failed to inject %s -> %s (err: %d). Skipping.\n", v_buf, r_buf, err);
            }
        }

        __putname(v_buf);
        __putname(r_buf);
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
    LIST_HEAD(r_victims);
    LIST_HEAD(c_victims);
    HLIST_HEAD(d_victims);
    struct nomount_child_name *child, *tmp_c;
    struct nomount_rule *rule, *tmp_r;
    struct nomount_dir_node *dir;
    struct hlist_node *tmp_d;

    if (info->attrs[NOMOUNT_ATTR_PAYLOAD]) {
        struct nlattr *attr = info->attrs[NOMOUNT_ATTR_PAYLOAD];
        const char *data = nla_data(attr);
        int len = nla_len(attr);
        int pos = 0;

        mutex_lock(&nomount_write_mutex);
        down_write(&nomount_dirs_rwsem);

        while (pos + 2 <= len) {
            u16 vp_len = *(u16 *)(data + pos);
            pos += 2;
            if (pos + vp_len > len) break;

            __nomount_del_rule(data + pos, vp_len, &r_victims, &c_victims, &d_victims);
            pos += vp_len;
        }

        up_write(&nomount_dirs_rwsem);
        mutex_unlock(&nomount_write_mutex);
    } else if (info->attrs[NOMOUNT_ATTR_VIRTUAL_PATH]) {
        char *v_path = nla_data(info->attrs[NOMOUNT_ATTR_VIRTUAL_PATH]);
        mutex_lock(&nomount_write_mutex);
        down_write(&nomount_dirs_rwsem);
        __nomount_del_rule(v_path, strlen(v_path), &r_victims, &c_victims, &d_victims);
        up_write(&nomount_dirs_rwsem);
        mutex_unlock(&nomount_write_mutex);
    } else {
        return -EINVAL;
    }

    if (list_empty(&r_victims)) return -ENOENT;

    list_for_each_entry_safe(rule, tmp_r, &r_victims, list) {
        nomount_drop_vpath_cache(rule->virtual_path, (rule->flags & NM_FLAG_IS_DIR));
    }

    synchronize_rcu();

    list_for_each_entry_safe(child, tmp_c, &c_victims, list) {
        kfree(child);
    }

    hlist_for_each_entry_safe(dir, tmp_d, &d_victims, node) {
        kfree(dir->dir_path);
        kmem_cache_free(nm_dir_cachep, dir);
    }

    list_for_each_entry_safe(rule, tmp_r, &r_victims, list) {
        nm_info("Deleted rule for: %s\n", rule->virtual_path);
        kfree(rule->virtual_path);
        kfree(rule->real_path);
        kmem_cache_free(nm_rule_cachep, rule);
    }

    return 0;
}

static int nomount_genl_clear_rules(struct sk_buff *skb, struct genl_info *info)
{
    mutex_lock(&nomount_write_mutex);
    down_write(&nomount_dirs_rwsem);

    __nomount_clear_all();

    up_write(&nomount_dirs_rwsem);
    mutex_unlock(&nomount_write_mutex);

    nm_info("Cleared all active rules and UIDs\n");
    return 0;
}

static int nomount_genl_dump_rules(struct sk_buff *skb, struct netlink_callback *cb)
{
    struct nomount_rule *rule;
    int idx = 0; void *hdr;
    int start_idx = cb->args[0];

    rcu_read_lock();
    list_for_each_entry_rcu(rule, &nomount_rules_list, list) {
        if (idx < start_idx) {
            idx++;
            continue;
        }

        hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
                          &nomount_genl_family, NLM_F_MULTI, NOMOUNT_CMD_GET_LIST);
        if (!hdr)
            break;

        if (nla_put_string(skb, NOMOUNT_ATTR_VIRTUAL_PATH, rule->virtual_path) ||
            nla_put_string(skb, NOMOUNT_ATTR_REAL_PATH, rule->real_path) ||
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
    if (!entry) 
        return -ENOMEM;

    entry->uid = uid;
    
    mutex_lock(&nomount_write_mutex);
    hash_add_rcu(nomount_uid_ht, &entry->node, uid);
    mutex_unlock(&nomount_write_mutex);
    
    nm_info("Successfully added blocked UID: %u via Netlink\n", uid);
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
    mutex_unlock(&nomount_write_mutex);

    if (found && entry) {
        synchronize_rcu();
        kmem_cache_free(nm_uid_cachep, entry);
    }

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

    ret = nla_put_u32(msg, NOMOUNT_ATTR_VERSION, NOMOUNT_VERSION);
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
    [NOMOUNT_ATTR_VERSION]      = { .type = NLA_U32 },
    [NOMOUNT_ATTR_PAYLOAD]      = { .type = NLA_BINARY },
};

static const struct genl_ops nomount_genl_ops[] = {
    {
        .cmd = NOMOUNT_CMD_ADD_RULE,
        .flags = GENL_ADMIN_PERM,
        .doit = nomount_genl_add_rule,
        .dumpit = NULL,
        NM_OPS_POLICY(nomount_genl_policy)
    },
    {
        .cmd = NOMOUNT_CMD_DEL_RULE,
        .flags = GENL_ADMIN_PERM,
        .doit = nomount_genl_del_rule,
        .dumpit = NULL,
        NM_OPS_POLICY(nomount_genl_policy)
    },
    {
        .cmd = NOMOUNT_CMD_CLEAR_ALL,
        .flags = GENL_ADMIN_PERM,
        .doit = nomount_genl_clear_rules,
        .dumpit = NULL,
        NM_OPS_POLICY(nomount_genl_policy)
    },
    {
        .cmd = NOMOUNT_CMD_ADD_UID,
        .flags = GENL_ADMIN_PERM,
        .doit = nomount_genl_add_uid,
        .dumpit = NULL,
        NM_OPS_POLICY(nomount_genl_policy)
    },
    {
        .cmd = NOMOUNT_CMD_DEL_UID,
        .flags = GENL_ADMIN_PERM,
        .doit = nomount_genl_del_uid,
        .dumpit = NULL,
        NM_OPS_POLICY(nomount_genl_policy)
    },
    {
        .cmd = NOMOUNT_CMD_GET_LIST,
        .flags = GENL_ADMIN_PERM,
        .doit = NULL,
        .dumpit = nomount_genl_dump_rules,
        NM_OPS_POLICY(nomount_genl_policy)
    },
    {
        .cmd = NOMOUNT_CMD_GET_VERSION,
        .flags = GENL_ADMIN_PERM,
        .doit = nomount_genl_get_version,
        .dumpit = NULL,
        NM_OPS_POLICY(nomount_genl_policy)
    },
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

static int __init nomount_init(void) {
    int ret;

    /* Initialize hash tables */
    hash_init(nomount_rules_by_vpath);
    hash_init(nomount_rules_by_real_ino);
    hash_init(nomount_rules_by_v_ino);
    hash_init(nomount_dirs_ht);
    hash_init(nomount_basenames_ht);
    hash_init(nomount_private_dirs_ht);
    hash_init(nomount_uid_ht);

    nm_rule_cachep = kmem_cache_create("nomount_rules", 
                                       sizeof(struct nomount_rule), 
                                       0, SLAB_HWCACHE_ALIGN, NULL);
    nm_dir_cachep = kmem_cache_create("nomount_dirs", 
                                      sizeof(struct nomount_dir_node), 
                                      0, SLAB_HWCACHE_ALIGN, NULL);
    nm_child_cachep = kmem_cache_create("nomount_child", 512,
                                      0, SLAB_HWCACHE_ALIGN, NULL);
    nm_uid_cachep = kmem_cache_create("nomount_uids", 
                                      sizeof(struct nomount_uid_node), 
                                      0, SLAB_HWCACHE_ALIGN, NULL);

    if (!nm_rule_cachep || !nm_dir_cachep || !nm_uid_cachep) {
        nm_err("Failed to allocate memory slab caches\n");
        if (nm_rule_cachep) kmem_cache_destroy(nm_rule_cachep);
        if (nm_dir_cachep) kmem_cache_destroy(nm_dir_cachep);
        if (nm_child_cachep) kmem_cache_destroy(nm_child_cachep);
        if (nm_uid_cachep) kmem_cache_destroy(nm_uid_cachep);
        return -ENOMEM;
    }

    ret = genl_register_family(&nomount_genl_family);
    if (ret) {
        nm_err("Failed to register Generic Netlink family (err: %d)\n", ret);
        kmem_cache_destroy(nm_rule_cachep);
        kmem_cache_destroy(nm_dir_cachep);
        kmem_cache_destroy(nm_uid_cachep);
        return ret;
    }

    nm_info("Loaded successfully\n");
    return 0;
}

fs_initcall(nomount_init);
