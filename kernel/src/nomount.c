#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/cred.h>
#include <linux/statfs.h>
#include <linux/fs_struct.h>
#include "nomount.h"

static struct kmem_cache *nm_rule_cachep, *nm_dir_cachep, *nm_uid_cachep;
atomic_t nm_active_rules = ATOMIC_INIT(0);
atomic_t nm_active_dirs = ATOMIC_INIT(0);
#define nomount_num_rules() atomic_read(&nm_active_rules)
#define nomount_num_dirs() atomic_read(&nm_active_dirs)

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
    if (unlikely(!in_task() || in_nmi() || oops_in_progress)) return true;
    if (unlikely(nm_is_recursive())) return true;
    if (unlikely(current->flags & (PF_KTHREAD | PF_EXITING))) return true;
    if (unlikely(!hash_empty(nomount_uid_ht))) {
        if (unlikely(nomount_is_uid_blocked(current_uid().val))) return true;
    }
    return false;
}

/* Exported */
bool nomount_should_skip(void) {
    if (unlikely(nomount_num_dirs() == 0)) return true;
    return __nomount_should_skip();
}
EXPORT_SYMBOL(nomount_should_skip);

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

/*** Helpers & Path Resolution ***/

/**
 * nomount_drop_vpath_cache - Force VFS to drop dcache for a specific path
 * @path_str: The native absolute path to flush
 * @is_dir: True if we should aggressively invalidate a directory
 */
static void nomount_drop_vpath_cache(const char *path_str, bool is_dir)
{
    struct path path;
    nm_enter();
    if (kern_path(path_str, 0, &path) == 0) {
        if (is_dir) {
            d_invalidate(path.dentry);
        } else {
            d_drop(path.dentry);
        }
        path_put(&path);
    }
    nm_exit();
}

/**
 * __nomount_collect_parents - Track parent directories of a real path
 * @real_path: The absolute path of the underlying target file
 *
 * Recursively resolves and registers parent directory inodes to ensure
 * traversal permissions are granted during lookup operations.
 *
 * This function assumes the caller holds the nomount_write_mutex 
 * and is not in a recursive context. 
 */
static void __nomount_collect_parents(const char *real_path, size_t len)
{
    char *path_tmp, *p, *slash;
    struct path kp;
    struct nomount_dir_node *dir_node;
    struct inode *p_inode;
    unsigned long p_ino;
    umode_t mode;
    bool priv;

    if (!real_path) return;

    path_tmp = __getname();
    if (!path_tmp) return;
    memcpy(path_tmp, real_path, len + 1);

    p = path_tmp;
    while (1) {
        slash = strrchr(p, '/');
        if (!slash || slash == p)
            break;

        *slash = '\0';

        nm_enter();
        if (kern_path(p, LOOKUP_FOLLOW, &kp) == 0) {
            p_inode = d_backing_inode(kp.dentry);
            p_ino = p_inode->i_ino;
            mode = p_inode->i_mode;
            priv = ((mode & S_IXOTH) == 0);
            path_put(&kp);
            nm_exit();

            {
                struct nomount_dir_node *curr;
                bool exists = false;
                
                hash_for_each_possible(nomount_dirs_ht, curr, node, p_ino) {
                    if (curr->dir_ino == p_ino) {
                        exists = true;
                        break;
                    }
                }

                if (!exists) {
                    dir_node = kmem_cache_zalloc(nm_dir_cachep, GFP_KERNEL);
                    if (dir_node) {
                        dir_node->dir_ino = p_ino;
                        dir_node->is_private = priv;
                        INIT_LIST_HEAD(&dir_node->children_names);
                        hash_add_rcu(nomount_dirs_ht, &dir_node->node, p_ino);
                        if (priv) {
                            dir_node->dir_path_len = slash - p;
                            dir_node->dir_path = kmalloc(dir_node->dir_path_len + 1, GFP_KERNEL);
                            if (dir_node->dir_path) {
                                memcpy(dir_node->dir_path, p, dir_node->dir_path_len + 1);
                                list_add_tail_rcu(&dir_node->private_list, &nomount_private_dirs_list);
                            }
                        } else {
                            dir_node->dir_path = NULL;
                            dir_node->dir_path_len = 0;
                        }
                        atomic_inc(&nm_active_dirs);
                    }
                }
            }
        } else {
            nm_exit();
        }
    }
    __putname(path_tmp);
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
static char *nomount_build_path_from_pwd(const char *rel_name, size_t name_len, size_t *out_len) 
{
    struct path pwd;
    char *cwd_str;
    char *page_buf = __getname();
    size_t dir_len;

    if (!page_buf) return NULL;

    get_fs_pwd(current->fs, &pwd);
    cwd_str = d_path(&pwd, page_buf, PATH_MAX);
    path_put(&pwd);

    if (IS_ERR_OR_NULL(cwd_str)) {
        __putname(page_buf);
        return NULL;
    }

    dir_len = (page_buf + PATH_MAX - 1) - cwd_str;

    if (likely(dir_len + name_len + 2 <= PATH_MAX)) {
        memmove(page_buf, cwd_str, dir_len);

        if (dir_len > 0 && page_buf[dir_len - 1] != '/') {
            page_buf[dir_len] = '/';
            memcpy(page_buf + dir_len + 1, rel_name, name_len + 1);
            if (out_len) *out_len = dir_len + 1 + name_len;
        } else {
            memcpy(page_buf + dir_len, rel_name, name_len + 1);
            if (out_len) *out_len = dir_len + name_len;
        }
        return page_buf;
    }

    __putname(page_buf);
    return NULL;
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
    unsigned long ino;

    ino = inode->i_ino;

    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, ino) {
        if (rule->real_ino == ino) {
            return rule;
        }
    }

    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, ino) {
        if (rule->v_ino == ino) {
            return rule;
        }
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
    u32 hash;
    hash = full_name_hash(NULL, pathname, len);
    hash_for_each_possible_rcu(nomount_rules_by_vpath, rule, vpath_node, hash) {
        if (rule->v_hash == hash && rule->vp_len == len) {
            if (memcmp(pathname, rule->virtual_path, len) == 0) {
                return rule;
            }
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
    char *res;
    int len;

    if (unlikely(IS_ERR_OR_NULL(path) || !path->dentry || !path->dentry->d_inode)) return NULL;
    if (unlikely(nomount_num_rules() == 0)) return NULL;

    nm_enter();
    rcu_read_lock();
    rule = nomount_get_rule_by_ino(path->dentry->d_inode);

    if (rule) {
        len = rule->vp_len;
        if (buflen >= len + 1) {
            res = buf + buflen - 1;
            *res = '\0';
            res -= len;
            memcpy(res, rule->virtual_path, len);
            rcu_read_unlock();
            nm_exit();
            return res;
        }
    }

    rcu_read_unlock();
    nm_exit();
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
    if (unlikely(nomount_num_rules() == 0)) return 0;
    
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
    char *abs_path = NULL, *rp_copy = NULL;
    const char *check_name;
    const char *s, *last_slash = NULL;
    size_t name_len = 0, b_len, r_len;
    struct filename *new_name;
    bool basename_match;
    u32 b_hash;

    if (IS_ERR_OR_NULL(name) || !name->name) return name;
    if (__nomount_should_skip()) return name;

    s = name->name;
    while (*s) {
        if (*s == '/') last_slash = s;
        s++; name_len++;
    }

    if (unlikely(name_len == 1 && name->name[0] == '/'))
        return name;

    if (current_uid().val != 0 && nomount_num_dirs() != 0 && !list_empty(&nomount_private_dirs_list)) {
        struct nomount_dir_node *priv_dir;
        bool is_shielded = false;

        rcu_read_lock();
        list_for_each_entry_rcu(priv_dir, &nomount_private_dirs_list, private_list) {
            size_t len = priv_dir->dir_path_len;
            if (name->name[1] != priv_dir->dir_path[1]) continue;
            if (memcmp(name->name, priv_dir->dir_path, len) == 0) {
                char next = name->name[len];
                if (next == '\0' || next == '/') {
                    is_shielded = true;
                    break;
                }
            }
        }
        rcu_read_unlock();

        if (is_shielded) {
            putname(name);
            return ERR_PTR(-ENOENT);
        }
    }

    if (unlikely(nomount_num_rules() == 0))
        return name;

    if (last_slash && *(last_slash + 1) != '\0') {
        check_name = last_slash + 1;
        b_len = s - check_name;
    } else {
        check_name = name->name;
        b_len = name_len;
    }

    b_hash = full_name_hash(NULL, check_name, b_len);
    basename_match = false;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_basenames_ht, rule, basename_node, b_hash) {
        if (rule->b_len == b_len && memcmp(rule->basename, check_name, b_len) == 0) { 
            basename_match = true;
            break;
        }
    }
    rcu_read_unlock();

    if (!basename_match) return name; 

    if (name->name[0] != '/') {
        abs_path = nomount_build_path_from_pwd(name->name, name_len, &r_len);
        if (!abs_path) {
            nm_debug("Failed to construct absolute path for relative target: %s\n", name->name);
            return name;
        }
        check_name = abs_path;
    } else {
        check_name = name->name;
        r_len = name_len;
    }

    rp_copy = __getname();
    if (likely(rp_copy)) {
        rcu_read_lock();
        rule = nomount_get_rule_by_path(check_name, r_len);
        if (rule) {
            memcpy(rp_copy, rule->real_path, rule->rp_len + 1);
        } else {
            __putname(rp_copy);
            rp_copy = NULL;
        }
        rcu_read_unlock();
    }

    if (abs_path) 
        __putname(abs_path);

    if (rp_copy) {
        new_name = getname_kernel(rp_copy);
        __putname(rp_copy);

        if (!IS_ERR(new_name)) {
            new_name->uptr = name->uptr;
            new_name->aname = name->aname;
            putname(name);
            nm_debug("Redirected: %s -> %s\n", check_name, rule->real_path);
            return new_name;
        }
    }

    return name;
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
    struct nomount_dir_node *curr_dir = NULL;
    struct nomount_child_name *child;
    struct inode *dir_inode = file_inode(file);
    unsigned long v_index;

    if (!dir_inode || __nomount_should_skip()) return;
    if (unlikely(nomount_num_dirs() == 0)) return;

    nm_enter();
restart:
    rcu_read_lock();
    
    hash_for_each_possible_rcu(nomount_dirs_ht, curr_dir, node, dir_inode->i_ino) {
        if (curr_dir->dir_ino == dir_inode->i_ino) {
            goto found;
        }
    }
    rcu_read_unlock();
    nm_exit();
    return;

found:
    if (ctx->pos >= NOMOUNT_MAGIC_POS) {
        v_index = (unsigned long)(ctx->pos - NOMOUNT_MAGIC_POS);
    } else {
        v_index = 0;
        ctx->pos = NOMOUNT_MAGIC_POS;
    }

    list_for_each_entry_rcu(child, &curr_dir->children_names, list) {
        char local_name[NAME_MAX];
        size_t local_len;
        unsigned long local_ino;
        unsigned char local_type;

        if (child->v_index < v_index) continue;
        local_len = child->name_len;
        if (local_len >= NAME_MAX) continue;
        memcpy(local_name, child->name, local_len + 1);
        local_ino = child->fake_ino;
        local_type = child->d_type;
        rcu_read_unlock();

        nm_debug("Injected virtual entry '%s' (ino: %lu) into dir ino %lu\n", 
                 local_name, local_ino, dir_inode->i_ino);

        if (!dir_emit(ctx, local_name, local_len, local_ino, local_type)) {
            nm_exit();
            return;
        }

        ctx->pos = NOMOUNT_MAGIC_POS + child->v_index + 1;
        goto restart;
    }

    rcu_read_unlock();
    nm_exit();
}

/**
 * __nomount_auto_inject_parent - Create a fake directory entry node
 * @parent_ino: Inode of the native parent directory
 * @name: Name of the child entry to inject
 * @type: Directory entry type (e.g., DT_REG, DT_DIR)
 * @child_fake_ino: Child inode precalculated
 *
 * Automatically tracks new entries to be injected during getdents calls.
 * This function assumes the caller holds the nomount_write_mutex
 * and is not in a recursive context.
 */
static void __nomount_auto_inject_parent(unsigned long parent_ino, const char *name, unsigned char type, unsigned long child_fake_ino)
{
    struct nomount_dir_node *dir_node = NULL, *curr;
    struct nomount_child_name *child;
    size_t name_len;

    name_len = strlen(name);
    hash_for_each_possible(nomount_dirs_ht, curr, node, parent_ino) {
        if (curr->dir_ino == parent_ino) {
            dir_node = curr;
            break;
        }
    }

    if (!dir_node) {
        dir_node = kmem_cache_zalloc(nm_dir_cachep, GFP_KERNEL);
        if (dir_node) {
            INIT_LIST_HEAD(&dir_node->cleanup_list);
            dir_node->dir_ino = parent_ino;
            INIT_LIST_HEAD(&dir_node->children_names);
            dir_node->next_child_index = 0;
            hash_add_rcu(nomount_dirs_ht, &dir_node->node, parent_ino);
            atomic_inc(&nm_active_dirs);
        }
    }

    if (dir_node) {
        bool exists = false;
        list_for_each_entry(child, &dir_node->children_names, list) {
            if (child->name_len == name_len && memcmp(child->name, name, name_len) == 0) {
                exists = true; 
                break;
            }
        }

        if (!exists) {
            child = kmalloc(sizeof(*child) + name_len + 1, GFP_KERNEL);
            if (child) {
                memcpy(child->name, name, name_len + 1);
                child->name_len = name_len;
                child->d_type = type;
                child->fake_ino = child_fake_ino;
                child->v_index = dir_node->next_child_index++;
                list_add_tail_rcu(&child->list, &dir_node->children_names);
            }
        }
    }
}

/**
 * nomount_generate_virtual_topology - Autogenerates intermediate directory rules
 * @rule: The main rule being added
 * @v_path: Virtual path string
 * @r_path: Real path string
 * @v_len: Length of virtual path
 * @flags: Original IOCTL flags
 *
 * Walks the path backwards using in-place mutation to find the closest
 * native parent, inherits its metadata (s_dev, s_magic), and auto-injects
 * intermediate virtual directory rules to satisfy VFS lookups.
 *
 * Returns 0 on success, or negative error code (e.g., -ENOMEM) on failure.
 */
static int nomount_generate_virtual_topology(struct nomount_rule *rule, 
                                             const char *v_path, const char *r_path, 
                                             size_t v_len, int flags)
{
    char *tmp_v, *tmp_r;
    int err = 0;
    int child_len, cur_v_len, cur_r_len, current_flags;
    struct nomount_rule *pending_rules[32];
    char *slashes_v[32];
    char *slashes_r[32];
    int p_count = 0;
    unsigned long inherited_dev = 0;
    unsigned long inherited_fs_type = 0;

    nm_debug("Virtual path %s not present natively, generating virtual topology\n", v_path);

    tmp_v = __getname();
    tmp_r = __getname();
    if (!tmp_v || !tmp_r) {
        if (tmp_v) __putname(tmp_v);
        if (tmp_r) __putname(tmp_r);
        return -ENOMEM;
    }

    memcpy(tmp_v, v_path, v_len + 1);
    memcpy(tmp_r, r_path, rule->rp_len + 1);
    
    child_len = v_len;
    cur_v_len = v_len;
    cur_r_len = rule->rp_len;
    current_flags = flags;

    while (p_count < 32) {
        char *slash_v, *slash_r;
        struct path p_path;
        unsigned long current_parent_ino;

        slash_v = strrchr(tmp_v, '/');
        slash_r = strrchr(tmp_r, '/');

        if (!slash_v || slash_v == tmp_v) {
            if (kern_path("/", LOOKUP_FOLLOW, &p_path) == 0) {
                current_parent_ino = d_backing_inode(p_path.dentry)->i_ino;
                __nomount_auto_inject_parent(current_parent_ino, tmp_v + 1, 
                    (current_flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG,
                    full_name_hash(NULL, rule->virtual_path, child_len));
                path_put(&p_path);
            }
            break;
        }

        *slash_v = '\0';
        slashes_v[p_count] = slash_v;
        cur_v_len = slash_v - tmp_v;

        if (slash_r && slash_r != tmp_r) {
            *slash_r = '\0';
            slashes_r[p_count] = slash_r;
            cur_r_len = slash_r - tmp_r;
        } else {
            slashes_r[p_count] = NULL;
            tmp_r = NULL;
        }

        pending_rules[p_count] = NULL; 
        p_count++;

        if (kern_path(tmp_v, LOOKUP_FOLLOW, &p_path) == 0) {
            inherited_dev = p_path.dentry->d_sb->s_dev;
            if (p_path.dentry->d_sb->s_op->statfs) {
                struct kstatfs st;
                p_path.dentry->d_sb->s_op->statfs(p_path.dentry, &st);
                inherited_fs_type = st.f_type;
            } else {
                inherited_fs_type = p_path.dentry->d_sb->s_magic;
            }
            current_parent_ino = d_backing_inode(p_path.dentry)->i_ino;
            __nomount_auto_inject_parent(current_parent_ino, slash_v + 1, 
                (current_flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG,
                full_name_hash(NULL, rule->virtual_path, child_len));
            path_put(&p_path);
            break; 
        } else {
            u32 h_inter;
            bool inter_exists;
            struct nomount_rule *ex;

            current_parent_ino = (unsigned long)full_name_hash(NULL, tmp_v, cur_v_len);
            __nomount_auto_inject_parent(current_parent_ino, slash_v + 1, 
                (current_flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG, full_name_hash(NULL, rule->virtual_path, child_len));

            h_inter = full_name_hash(NULL, tmp_v, cur_v_len);
            inter_exists = false;
            
            hash_for_each_possible(nomount_rules_by_vpath, ex, vpath_node, h_inter) {
                if (ex->vp_len == cur_v_len && memcmp(ex->virtual_path, tmp_v, cur_v_len) == 0) {
                    inherited_dev = ex->v_dev;
                    inherited_fs_type = ex->v_fs_type;
                    inter_exists = true;
                    break;
                }
            }

            if (!inter_exists) {
                pending_rules[p_count - 1] = kmem_cache_zalloc(nm_rule_cachep, GFP_KERNEL);
                if (!pending_rules[p_count - 1]) {
                    err = -ENOMEM;
                    break;
                }

                {
                    struct nomount_rule *irule = pending_rules[p_count - 1];
                    char *b_slash;
                    const char *b_name_inter;

                    irule->virtual_path = kstrdup(tmp_v, GFP_KERNEL);
                    irule->real_path = tmp_r ? kstrdup(tmp_r, GFP_KERNEL) : kstrdup("/", GFP_KERNEL);

                    if (!irule->virtual_path || !irule->real_path) {
                        if (irule->virtual_path) kfree(irule->virtual_path);
                        if (irule->real_path) kfree(irule->real_path);
                        kmem_cache_free(nm_rule_cachep, irule);
                        pending_rules[p_count - 1] = NULL;
                        err = -ENOMEM;
                        break;
                    }

                    irule->vp_len = cur_v_len;
                    irule->rp_len = tmp_r ? cur_r_len : 1;

                    b_slash = strrchr(irule->virtual_path, '/');
                    b_name_inter = b_slash ? b_slash + 1 : irule->virtual_path;
                    irule->basename = b_name_inter;
                    irule->b_len = strlen(b_name_inter);

                    irule->v_hash = h_inter;
                    irule->v_ino = (unsigned long)h_inter;
                    irule->flags = NM_FLAG_IS_DIR;
                    
                    irule->real_ino = 0;
                    irule->real_dev = 0;

                    if (tmp_r) {
                        struct path r_path_struct;
                        if (kern_path(irule->real_path, LOOKUP_FOLLOW, &r_path_struct) == 0) {
                            unsigned long r_ino = d_backing_inode(r_path_struct.dentry)->i_ino;
                            if (r_ino > 10) {
                                irule->real_ino = r_ino;
                                irule->real_dev = r_path_struct.dentry->d_sb->s_dev;
                            }
                            path_put(&r_path_struct);
                        }
                    }
                }
            }
            current_flags = NM_FLAG_IS_DIR;
            child_len = cur_v_len;
        }
    }

    while (p_count > 0) {
        p_count--;
        if (slashes_v[p_count]) *slashes_v[p_count] = '/';
        if (slashes_r[p_count]) *slashes_r[p_count] = '/';

        if (pending_rules[p_count]) {
            struct nomount_rule *irule = pending_rules[p_count];
            
            if (err == 0) {
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
            } else {
                kfree(irule->virtual_path);
                kfree(irule->real_path);
                kmem_cache_free(nm_rule_cachep, irule);
            }
        }
    }
    
    if (tmp_v) __putname(tmp_v);
    if (tmp_r) __putname(tmp_r);

    if (err == 0) {
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
    dev_t current_dev;

    if (IS_ERR_OR_NULL(path) || IS_ERR_OR_NULL(stat)) return;
    if (unlikely(nomount_num_rules() == 0)) return;
    inode = d_backing_inode(path->dentry);
    if (!inode) return;

    current_dev = inode->i_sb->s_dev;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino && rule->real_dev == current_dev) {
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
    dev_t current_dev;

    if (IS_ERR_OR_NULL(path) || IS_ERR_OR_NULL(buf) || __nomount_should_skip()) return;
    if (unlikely(nomount_num_rules() == 0)) return;
    inode = d_backing_inode(path->dentry);
    if (!inode) return;

    current_dev = inode->i_sb->s_dev;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino && rule->real_dev == current_dev) {
            if (rule->v_fs_type != 0)
                buf->f_type = READ_ONCE(rule->v_fs_type);
            goto unlock;
        }
    }

    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, inode->i_ino) {
        if (rule->v_ino == inode->i_ino && rule->v_dev == current_dev) {
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
    unsigned long target_ino = inode->i_ino;

    if (unlikely(IS_ERR_OR_NULL(inode) || IS_ERR_OR_NULL(dev) ||
                  IS_ERR_OR_NULL(ino) || __nomount_should_skip()))
        return false;

    if (unlikely(nomount_num_rules() == 0)) return false;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, target_ino) {
        if (rule->real_ino == target_ino) {
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
        nm_enter();
        nomount_spoof_stat(path, stat);
        nm_exit();
    }
    return ret;
}

/*** IOCTL API & Module Management ***/

static int nomount_ioctl_add_rule(unsigned long arg)
{
    struct nomount_ioctl_data data;
    struct nomount_rule *rule, *existing;
    struct path path_main, r_path_struct_main;
    char *v_path, *r_path, *slash, *search_path;
    const char *b_name;
    size_t v_len;
    u32 hash, b_hash;
    unsigned long inherited_dev = 0;
    unsigned long inherited_fs_type = 0;
    int err = 0;

    if (copy_from_user(&data, (void __user *)arg, sizeof(data))) {
        nm_warn("Failed to copy rule data from userspace\n");
        return -EFAULT;
    }

    if (!capable(CAP_SYS_ADMIN)) {
        nm_warn("Permission denied: CAP_SYS_ADMIN required\n");
        return -EPERM;
    }

    v_path = strndup_user((const char __user *)(unsigned long)data.virtual_path, PATH_MAX);
    r_path = strndup_user((const char __user *)(unsigned long)data.real_path, PATH_MAX);
    if (IS_ERR_OR_NULL(v_path) || IS_ERR_OR_NULL(r_path)) {
        nm_err("Failed to allocate memory for path strings\n");
        if (!IS_ERR_OR_NULL(v_path)) kfree(v_path);
        if (!IS_ERR_OR_NULL(r_path)) kfree(r_path);
        return -ENOMEM;
    }
    
    v_len = strlen(v_path);
    hash = full_name_hash(NULL, v_path, v_len);

    mutex_lock(&nomount_write_mutex);
    hash_for_each_possible(nomount_rules_by_vpath, existing, vpath_node, hash) {
        if (existing->v_hash == hash && existing->vp_len == v_len &&
             memcmp(existing->virtual_path, v_path, v_len) == 0) {
            mutex_unlock(&nomount_write_mutex);
            nm_warn("Rule already exists for virtual path: %s. Aborting.\n", v_path);
            kfree(v_path); 
            kfree(r_path);
            return -EEXIST;
        }
    }

    rule = kmem_cache_zalloc(nm_rule_cachep, GFP_KERNEL);
    if (!rule) {
        mutex_unlock(&nomount_write_mutex);
        nm_err("Failed to allocate slab cache memory for new rule\n");
        kfree(v_path); kfree(r_path);
        return -ENOMEM;
    }

    rule->virtual_path = v_path;
    rule->real_path = r_path;
    rule->vp_len = v_len;
    rule->rp_len = strlen(r_path);

    slash = strrchr(v_path, '/');
    b_name = slash ? slash + 1 : v_path;
    rule->basename = b_name;
    rule->b_len = strlen(b_name);

    rule->v_hash = hash;
    rule->flags = data.flags;

    rule->real_ino = 0;
    rule->real_dev = 0;

    nm_enter();

    if (kern_path(r_path, LOOKUP_FOLLOW, &r_path_struct_main) == 0) {
        struct inode *r_inode = d_backing_inode(r_path_struct_main.dentry);
        rule->real_ino = r_inode->i_ino;
        rule->real_dev = r_path_struct_main.dentry->d_sb->s_dev;
        if (S_ISDIR(r_inode->i_mode)) {
            rule->flags |= NM_FLAG_IS_DIR;
        }
        path_put(&r_path_struct_main);
    }

    search_path = kstrdup(v_path, GFP_KERNEL);
    if (search_path) {
        while (strlen(search_path) > 0) {
            struct path sp;
            char *s;

            if (kern_path(search_path, LOOKUP_FOLLOW, &sp) == 0) {
                inherited_dev = sp.dentry->d_sb->s_dev;
                if (sp.dentry->d_sb->s_op->statfs) {
                    struct kstatfs st;
                    sp.dentry->d_sb->s_op->statfs(sp.dentry, &st);
                    inherited_fs_type = st.f_type;
                } else {
                    inherited_fs_type = sp.dentry->d_sb->s_magic;
                }
                path_put(&sp);
                break;
            }
            
            s = strrchr(search_path, '/');
            if (!s || s == search_path) {
                if (kern_path("/", LOOKUP_FOLLOW, &sp) == 0) {
                    inherited_dev = sp.dentry->d_sb->s_dev;
                    inherited_fs_type = sp.dentry->d_sb->s_magic;
                    path_put(&sp);
                }
                break;
            }
            *s = '\0';
        }
        kfree(search_path);
    }

    rule->v_dev = inherited_dev;
    rule->v_fs_type = inherited_fs_type;

    if (kern_path(v_path, LOOKUP_FOLLOW, &path_main) == 0) {
        rule->v_ino = d_backing_inode(path_main.dentry)->i_ino;
        path_put(&path_main);
        nm_debug("Resolved physical backing for %s (ino: %lu)\n", v_path, rule->v_ino);
    } else {
        rule->v_ino = (unsigned long)hash;
        err = nomount_generate_virtual_topology(rule, v_path, r_path, v_len, data.flags);
        if (err != 0) {
            nm_exit();
            mutex_unlock(&nomount_write_mutex);
            kfree(v_path);
            kfree(r_path);
            kmem_cache_free(nm_rule_cachep, rule);
            return err;
        }
    }

    __nomount_collect_parents(rule->real_path, rule->rp_len);
    nm_exit();

    b_hash = full_name_hash(NULL, rule->basename, rule->b_len);
    hash_add_rcu(nomount_basenames_ht, &rule->basename_node, b_hash);

    hash_add_rcu(nomount_rules_by_vpath, &rule->vpath_node, hash);
    if (rule->real_ino)
        hash_add_rcu(nomount_rules_by_real_ino, &rule->real_ino_node, rule->real_ino);
    if (rule->v_ino)
        hash_add_rcu(nomount_rules_by_v_ino, &rule->v_ino_node, rule->v_ino);

    list_add_tail_rcu(&rule->list, &nomount_rules_list);
    atomic_inc(&nm_active_rules);
    nomount_drop_vpath_cache(v_path, (rule->flags & NM_FLAG_IS_DIR));
    mutex_unlock(&nomount_write_mutex);

    nm_info("Successfully added rule: %s -> %s\n", rule->virtual_path, rule->real_path);

    return 0;
}

static int nomount_ioctl_del_rule(unsigned long arg)
{
    struct nomount_ioctl_data data;
    struct nomount_rule *rule, *victim = NULL;
    struct nomount_dir_node *dir, *victim_dir = NULL;
    struct nomount_child_name *child, *tmp_child, *victim_child = NULL;
    struct hlist_node *tmp;
    char *v_path;
    size_t v_len;
    u32 hash;
    int bkt;

    if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
        return -EFAULT;

    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;

    v_path = strndup_user((const char __user *)(unsigned long)data.virtual_path, PATH_MAX);
    if (IS_ERR_OR_NULL(v_path)) return -ENOMEM;
    v_len = strlen(v_path);
    hash = full_name_hash(NULL, v_path, v_len);

    mutex_lock(&nomount_write_mutex);
    hash_for_each_possible_safe(nomount_rules_by_vpath, rule, tmp, vpath_node, hash) {
        if (rule->v_hash == hash && rule->vp_len == v_len && 
             memcmp(rule->virtual_path, v_path, v_len) == 0) {
            hash_del_rcu(&rule->vpath_node);
            hash_del_rcu(&rule->basename_node);
            if (rule->real_ino)
                hash_del_rcu(&rule->real_ino_node);
            if (rule->v_ino) 
                hash_del_rcu(&rule->v_ino_node);
            list_del_rcu(&rule->list);
            victim = rule;
            atomic_dec(&nm_active_rules);
            hash_for_each(nomount_dirs_ht, bkt, dir, node) {
                list_for_each_entry_safe(child, tmp_child, &dir->children_names, list) {
                    if (child->fake_ino == hash) {
                        list_del_rcu(&child->list);
                        victim_child = child;
                        if (list_empty(&dir->children_names)) {
                            hash_del_rcu(&dir->node);
                            if (dir->is_private) list_del_rcu(&dir->private_list);
                            atomic_dec(&nm_active_dirs);
                            victim_dir = dir;
                        }
                        goto ghost_found;
                    }
                }
            }
ghost_found:
            break;
        }
    }
    mutex_unlock(&nomount_write_mutex);

    if (victim) {
        nomount_drop_vpath_cache(v_path, (victim->flags & NM_FLAG_IS_DIR));
        nm_info("Deleted rule for: %s\n", victim->virtual_path);
        synchronize_rcu();

        if (victim_child) {
            kfree(victim_child);
        }

        if (victim_dir) {
            kfree(victim_dir->dir_path);
            kmem_cache_free(nm_dir_cachep, victim_dir);
        }

        kfree(victim->virtual_path);
        kfree(victim->real_path);
        kmem_cache_free(nm_rule_cachep, victim);
        
        kfree(v_path);
        return 0;
    }

    kfree(v_path);
    return -ENOENT;
}

static int nomount_ioctl_clear_rules(void)
{
    struct nomount_rule *rule, *tmp_rule;
    struct nomount_uid_node *uid_node, *tmp_uid;
    struct nomount_dir_node *dir_node, *tmp_dir;
    struct nomount_child_name *child, *tmp_child;
    struct hlist_node *hlist_tmp;
    LIST_HEAD(rule_victims);
    LIST_HEAD(uid_victims);
    LIST_HEAD(dir_victims);
    LIST_HEAD(dir_victims_children);
    int bkt;
    
    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;

    mutex_lock(&nomount_write_mutex);
    list_for_each_entry_safe(rule, tmp_rule, &nomount_rules_list, list) {
        hash_del_rcu(&rule->vpath_node);
        hash_del_rcu(&rule->basename_node);
        if (rule->real_ino)
            hash_del_rcu(&rule->real_ino_node);
        if (rule->v_ino)
            hash_del_rcu(&rule->v_ino_node);

        nomount_drop_vpath_cache(rule->virtual_path, (rule->flags & NM_FLAG_IS_DIR));
        list_del_rcu(&rule->list);
        list_add_tail(&rule->cleanup_list, &rule_victims);
    }

    hash_for_each_safe(nomount_uid_ht, bkt, hlist_tmp, uid_node, node) {
        hash_del_rcu(&uid_node->node);
        list_add_tail(&uid_node->cleanup_list, &uid_victims);
    }

    hash_for_each_safe(nomount_dirs_ht, bkt, hlist_tmp, dir_node, node) {
        hash_del_rcu(&dir_node->node);
        list_for_each_entry_safe(child, tmp_child, &dir_node->children_names, list) {
            list_del_rcu(&child->list); 
            list_add_tail(&child->cleanup_list, &dir_victims_children);
        }
        list_add_tail(&dir_node->cleanup_list, &dir_victims);
    }

    atomic_set(&nm_active_rules, 0);
    atomic_set(&nm_active_dirs, 0);

    INIT_LIST_HEAD(&nomount_private_dirs_list);

    mutex_unlock(&nomount_write_mutex);

    synchronize_rcu();

    list_for_each_entry_safe(dir_node, tmp_dir, &dir_victims, cleanup_list) {
        list_del(&dir_node->cleanup_list);
        kfree(dir_node->dir_path);
        kmem_cache_free(nm_dir_cachep, dir_node);
    }

    list_for_each_entry_safe(child, tmp_child, &dir_victims_children, cleanup_list) {
        list_del(&child->cleanup_list);
        kfree(child);
    }

    list_for_each_entry_safe(rule, tmp_rule, &rule_victims, cleanup_list) {
        list_del(&rule->cleanup_list);
        kfree(rule->virtual_path);
        kfree(rule->real_path);
        kmem_cache_free(nm_rule_cachep, rule);
    }

    list_for_each_entry_safe(uid_node, tmp_uid, &uid_victims, cleanup_list) {
        list_del(&uid_node->cleanup_list);
        kmem_cache_free(nm_uid_cachep, uid_node);
    }

    nm_info("Cleared all active rules and UIDs\n");
    return 0;
}

static int nomount_ioctl_list_rules(unsigned long arg)
{
    struct nomount_rule *rule;
    char *kbuf;
    unsigned short *p_len;
    size_t total_required = 0;
    size_t pos = 0;
    int ret = 0;

    mutex_lock(&nomount_write_mutex);
    list_for_each_entry(rule, &nomount_rules_list, list) {
        total_required += sizeof(unsigned short) * 2 + (rule->vp_len + 1) + (rule->rp_len + 1);
    }

    if (total_required == 0) {
        mutex_unlock(&nomount_write_mutex);
        return 0;
    }

    kbuf = kmalloc(total_required, GFP_KERNEL);
    if (!kbuf) {
        mutex_unlock(&nomount_write_mutex);
        nm_err("Failed to allocate %zu bytes for listing rules\n", total_required);
        return -ENOMEM;
    }

    list_for_each_entry(rule, &nomount_rules_list, list) {
        size_t v_len = rule->vp_len + 1; 
        size_t r_len = rule->rp_len + 1;
        size_t step_len = sizeof(unsigned short) * 2 + v_len + r_len;

        if (pos + step_len > total_required) break;

        p_len = (unsigned short *)(kbuf + pos);
        *p_len = step_len;
        pos += sizeof(unsigned short);

        p_len = (unsigned short *)(kbuf + pos);
        *p_len = v_len;
        pos += sizeof(unsigned short);

        memcpy(kbuf + pos, rule->virtual_path, v_len);
        pos += v_len;

        memcpy(kbuf + pos, rule->real_path, r_len);
        pos += r_len;
    }

    mutex_unlock(&nomount_write_mutex);

    if (copy_to_user((void __user *)arg, kbuf, pos)) {
        nm_warn("-EFAULT while copying rule list to userspace\n");
        ret = -EFAULT;
    } else {
        ret = pos; 
    }

    kfree(kbuf);
    return ret;
}

static int nomount_ioctl_add_uid(unsigned long arg)
{
    unsigned int uid;
    struct nomount_uid_node *entry;

    if (copy_from_user(&uid, (void __user *)arg, sizeof(uid)))
        return -EFAULT;
    
    if (nomount_is_uid_blocked(uid)) return -EEXIST;

    entry = kmem_cache_zalloc(nm_uid_cachep, GFP_KERNEL);
    if (!entry) return -ENOMEM;

    entry->uid = uid;
    
    mutex_lock(&nomount_write_mutex);
    hash_add_rcu(nomount_uid_ht, &entry->node, uid);
    mutex_unlock(&nomount_write_mutex);
    
    return 0;
}

static int nomount_ioctl_del_uid(unsigned long arg)
{
    unsigned int uid;
    struct nomount_uid_node *entry;
    struct hlist_node *tmp;
    int bkt;
    bool found = false;

    if (copy_from_user(&uid, (void __user *)arg, sizeof(uid)))
        return -EFAULT;

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

static long nomount_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    if (_IOC_TYPE(cmd) != NOMOUNT_MAGIC_CODE)
        return -ENOTTY;

    switch (cmd) {
    case NOMOUNT_IOC_GET_VERSION: return NOMOUNT_VERSION;
    case NOMOUNT_IOC_ADD_RULE:    return nomount_ioctl_add_rule(arg);
    case NOMOUNT_IOC_DEL_RULE:    return nomount_ioctl_del_rule(arg);
    case NOMOUNT_IOC_CLEAR_ALL:   return nomount_ioctl_clear_rules();
    case NOMOUNT_IOC_ADD_UID:     return nomount_ioctl_add_uid(arg);
    case NOMOUNT_IOC_DEL_UID:     return nomount_ioctl_del_uid(arg);
    case NOMOUNT_IOC_GET_LIST:    return nomount_ioctl_list_rules(arg);
    default: return -ENOTTY;
    }
}

static const struct file_operations nomount_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = nomount_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = nomount_ioctl,
#endif
};

static struct miscdevice nomount_device = {
    .minor = MISC_DYNAMIC_MINOR, 
    .name = "nomount", 
    .fops = &nomount_fops, 
    .mode = 0600,
};

static int __init nomount_init(void) {
    int ret;

    /* Initialize hash tables */
    hash_init(nomount_rules_by_vpath);
    hash_init(nomount_rules_by_real_ino);
    hash_init(nomount_rules_by_v_ino);
    hash_init(nomount_dirs_ht);
    hash_init(nomount_basenames_ht);
    hash_init(nomount_uid_ht);

    nm_rule_cachep = kmem_cache_create("nomount_rules", 
                                       sizeof(struct nomount_rule), 
                                       0, SLAB_HWCACHE_ALIGN, NULL);
    nm_dir_cachep = kmem_cache_create("nomount_dirs", 
                                      sizeof(struct nomount_dir_node), 
                                      0, SLAB_HWCACHE_ALIGN, NULL);
    nm_uid_cachep = kmem_cache_create("nomount_uids", 
                                      sizeof(struct nomount_uid_node), 
                                      0, SLAB_HWCACHE_ALIGN, NULL);

    if (!nm_rule_cachep || !nm_dir_cachep || !nm_uid_cachep) {
        nm_err("Failed to allocate memory slab caches\n");
        if (nm_rule_cachep) kmem_cache_destroy(nm_rule_cachep);
        if (nm_dir_cachep) kmem_cache_destroy(nm_dir_cachep);
        if (nm_uid_cachep) kmem_cache_destroy(nm_uid_cachep);
        return -ENOMEM;
    }

    ret = misc_register(&nomount_device);
    if (ret) {
        nm_err("Failed to register misc device '/dev/nomount' (err: %d)\n", ret);
        kmem_cache_destroy(nm_rule_cachep);
        kmem_cache_destroy(nm_dir_cachep);
        kmem_cache_destroy(nm_uid_cachep);
        return ret;
    }

    nm_info("Loaded successfully\n");
    return 0;
}

fs_initcall(nomount_init);
