#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/dirent.h>
#include <linux/miscdevice.h>
#include <linux/cred.h>
#include <linux/vmalloc.h>
#include <linux/sched/mm.h>
#include <linux/statfs.h>
#include <linux/workqueue.h>
#include <linux/fs_struct.h>
#include <linux/jhash.h>
#include "nomount.h"

atomic_t nomount_enabled = ATOMIC_INIT(0);
atomic_t nm_active_rules = ATOMIC_INIT(0);
atomic_t nm_active_dirs = ATOMIC_INIT(0);
#define nomount_disabled() (atomic_read(&nomount_enabled) == 0)
#define nomount_num_rules() atomic_read(&nm_active_rules)
#define nomount_num_dirs() atomic_read(&nm_active_dirs)

struct linux_dirent {
    unsigned long   d_ino;
    unsigned long   d_off;
    unsigned short  d_reclen;
    char        d_name[];
};

/*** Counting Bloom Filter Implementation ***/

/**
 * nomount_bloom_add_path - Add a path to the counting bloom filter
 * @name: The path to add
 */
static inline void nomount_bloom_add_path(const char *name)
{
    size_t len = strlen(name);
    u32 h1 = jhash(name, len, 0) & (NOMOUNT_BLOOM_SIZE - 1);
    u32 h2 = jhash(name, len, 1) & (NOMOUNT_BLOOM_SIZE - 1);

    if (nomount_bloom_paths[h1] < 65535) nomount_bloom_paths[h1]++;
    if (nomount_bloom_paths[h2] < 65535) nomount_bloom_paths[h2]++;
}

/**
 * nomount_bloom_del_path - Remove a path from the counting bloom filter
 * @name: The path to remove
 */
static inline void nomount_bloom_del_path(const char *name)
{
    size_t len = strlen(name);
    u32 h1 = jhash(name, len, 0) & (NOMOUNT_BLOOM_SIZE - 1);
    u32 h2 = jhash(name, len, 1) & (NOMOUNT_BLOOM_SIZE - 1);

    if (nomount_bloom_paths[h1] > 0) nomount_bloom_paths[h1]--;
    if (nomount_bloom_paths[h2] > 0) nomount_bloom_paths[h2]--;
}

/**
 * nomount_bloom_test_path - Check if a path is likely in the bloom filter
 * @name: The path to check
 *
 * Returns true if the path is likely present, false if definitely not present.
 */
static inline bool nomount_bloom_test_path(const char *name)
{
    size_t len = strlen(name);
    if (unlikely(!nomount_bloom_paths)) return false;
    u32 h1 = jhash(name, len, 0) & (NOMOUNT_BLOOM_SIZE - 1);
    u32 h2 = jhash(name, len, 1) & (NOMOUNT_BLOOM_SIZE - 1);

    return (nomount_bloom_paths[h1] > 0) && (nomount_bloom_paths[h2] > 0);
}

/**
 * nomount_bloom_add_ino - Add an inode number to the counting bloom filter
 * @ino: The inode number to add
 */

static inline void nomount_bloom_add_ino(unsigned long ino)
{
    u32 h = ino & (NOMOUNT_BLOOM_SIZE - 1);
    if (nomount_bloom_inos[h] < 65535) nomount_bloom_inos[h]++;
}

/**
 * nomount_bloom_del_ino - Remove an inode number from the counting bloom filter
 * @ino: The inode number to remove
 */
static inline void nomount_bloom_del_ino(unsigned long ino)
{
    u32 h = ino & (NOMOUNT_BLOOM_SIZE - 1);
    if (nomount_bloom_inos[h] > 0) nomount_bloom_inos[h]--;
}

/**
 * nomount_bloom_test_ino - Check if an inode number is likely in the bloom filter
 * @ino: The inode number to check
 *
 * Returns true if the inode is likely present, false if definitely not present.
 */
static inline bool nomount_bloom_test_ino(unsigned long ino)
{
    u32 h = ino & (NOMOUNT_BLOOM_SIZE - 1);
    if (unlikely(!nomount_bloom_inos)) return false;
    return nomount_bloom_inos[h] > 0;
}

/*** Verification & Compatibility Checks ***/

/* Forward declaration */
bool nomount_is_uid_blocked(uid_t uid);

/**
 * nomount_should_skip - Determine if the current context should bypass hooks
 *
 * Returns true if NoMount is disabled, if running in interrupt context,
 * if recursion is detected, or if the current UID is in the blocklist.
 */
bool nomount_should_skip(void) {
    if (unlikely(nomount_disabled())) return true;
    if (unlikely(in_interrupt() || in_nmi() || oops_in_progress)) return true;
    if (unlikely(nm_is_recursive())) return true;
    if (unlikely(current->flags & (PF_KTHREAD | PF_EXITING))) return true;
    if (unlikely(!hash_empty(nomount_uid_ht))) {
        if (unlikely(nomount_is_uid_blocked(current_uid().val))) return true;
    }
    return false;
}
EXPORT_SYMBOL(nomount_should_skip);

/**
 * nomount_is_uid_blocked - Check if a specific UID is excluded from redirection
 * @uid: The User ID to check
 *
 * Returns true if the UID exists in the exclusion hash table.
 */
bool nomount_is_uid_blocked(uid_t uid) {
    struct nomount_uid_node *entry;
    if (nomount_should_skip()) return false;
    
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
 * nomount_is_injected_file - Check if an inode belongs to an active rule
 * @inode: The inode to evaluate
 *
 * Returns true if the inode matches either the virtual or real inode
 * of a registered NoMount rule.
 */
bool nomount_is_injected_file(struct inode *inode) {
    struct nomount_rule *rule;
    bool found = false;

    if (unlikely(!inode || IS_ERR_OR_NULL(inode))) return false;
    if (unlikely(nomount_num_rules() == 0)) return false;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino) {
            found = true;
            goto out;
        }
    }

    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, inode->i_ino) {
        if (rule->v_ino == inode->i_ino) {
            found = true;
            goto out;
        }
    }

out:
    rcu_read_unlock();
    return found;
}

/**
 * nomount_is_traversal_allowed - Check if an inode allows directory traversal
 * @inode: The directory inode to check
 * @mask: The requested access mask
 *
 * Prevents DAC/MAC modules from blocking access to injected directory structures.
 * Returns true if the inode is tracked as a parent directory.
 */
bool nomount_is_traversal_allowed(struct inode *inode, int mask)
{
    struct nomount_dir_node *dir;
    unsigned long ino;

    if (!inode || IS_ERR_OR_NULL(inode)) return false;
    if (unlikely(nomount_num_dirs() == 0)) return false;

    ino = inode->i_ino;
    if (nomount_is_injected_file(inode)) return true;

    rcu_read_lock();

    hash_for_each_possible_rcu(nomount_dirs_ht, dir, node, ino) {
        if (dir->dir_ino == ino) {
            rcu_read_unlock();
            return true;
        }
    }

    rcu_read_unlock();
    return false;
}
EXPORT_SYMBOL(nomount_is_traversal_allowed);

/*** Helpers & Path Resolution ***/

/**
 * nomount_get_static_vpath - Get the registered virtual path for an inode
 * @inode: The inode to query
 *
 * Returns a pointer to the virtual path string, or NULL if not found.
 */
const char *nomount_get_static_vpath(struct inode *inode) {
    struct nomount_rule *rule;
    unsigned long ino;

    if (unlikely(!inode || IS_ERR_OR_NULL(inode))) return NULL;
    if (unlikely(nomount_num_rules() == 0)) return NULL;

    ino = inode->i_ino;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, ino) {
        if (rule->real_ino == ino) {
            rcu_read_unlock();
            return rule->virtual_path;
        }
    }

    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, ino) {
        if (rule->v_ino == ino) {
            rcu_read_unlock();
            return rule->virtual_path;
        }
    }

    rcu_read_unlock();
    return NULL;
}
EXPORT_SYMBOL(nomount_get_static_vpath);

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
static void __nomount_collect_parents(const char *real_path)
{
    char *path_tmp, *p;
    struct path kp;
    struct nomount_dir_node *dir_node;
    unsigned long p_ino;

    if (!real_path) return;

    path_tmp = kstrdup(real_path, GFP_KERNEL);
    if (!path_tmp) return;

    p = path_tmp;
    while (1) {
        char *slash = strrchr(p, '/');
        if (!slash || slash == p)
            break;

        *slash = '\0';

        nm_enter();
        if (kern_path(p, LOOKUP_FOLLOW, &kp) == 0) {
            p_ino = d_backing_inode(kp.dentry)->i_ino;
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
                    dir_node = kzalloc(sizeof(*dir_node), GFP_KERNEL);
                    if (dir_node) {
                        dir_node->dir_ino = p_ino;
                        INIT_LIST_HEAD(&dir_node->children_names);
                        hash_add_rcu(nomount_dirs_ht, &dir_node->node, p_ino);
                        nomount_bloom_add_ino(p_ino);
                        atomic_inc(&nm_active_dirs);
                    }
                }
            }
        } else {
            nm_exit();
        }
    }
    kfree(path_tmp);
}

/**
 * nomount_build_absolute_path - Reconstruct absolute path from DFD
 * @dfd: Directory file descriptor (e.g., from openat)
 * @name: The relative filename
 *
 * Prevents evasion by constructing the full absolute path when a 
 * relative path is requested via a specific directory descriptor.
 * 
 * Returns an allocated string containing the absolute path, or NULL.
 * Caller must free the returned string using kfree().
 */
char *nomount_build_absolute_path(int dfd, const char *name)
{
    char *page_buf, *dir_path, *abs_path = NULL;
    size_t dir_len, name_len;
    struct fd f;

    if (unlikely(!name || name[0] == '/' || *name == '\0')) return NULL;
    if (dfd == AT_FDCWD || dfd < 0) return NULL;

    f = fdget_raw(dfd);
    if (!f.file) return NULL;

    page_buf = (char *)__get_free_page(GFP_KERNEL);
    if (!page_buf) {
        fdput(f);
        return NULL;
    }

    dir_path = d_path(&f.file->f_path, page_buf, PAGE_SIZE);
    if (IS_ERR_OR_NULL(dir_path)) {
        free_page((unsigned long)page_buf);
        fdput(f);
        return NULL;
    }

    dir_len = strlen(dir_path);
    name_len = strlen(name);

    if (likely(dir_len + name_len + 2 <= PATH_MAX)) {
        abs_path = kmalloc(dir_len + name_len + 2, GFP_KERNEL);
        if (abs_path) {
            memcpy(abs_path, dir_path, dir_len);
            if (dir_len > 0 && dir_path[dir_len - 1] != '/') {
                abs_path[dir_len] = '/';
                memcpy(abs_path + dir_len + 1, name, name_len + 1);
            } else {
                memcpy(abs_path + dir_len, name, name_len + 1);
            }
        }
    }

    free_page((unsigned long)page_buf);
    fdput(f);
    return abs_path; 
}
EXPORT_SYMBOL(nomount_build_absolute_path);

/**
 * nomount_build_path_from_pwd - Construct an absolute path using the current working directory
 * @rel_name: The relative filename to append to the current working directory
 *
 * This helper is used to reconstruct an absolute path for operations that provide
 * a relative filename without a DFD, ensuring that NoMount can still resolve the intended target.
 *
 * Returns an allocated string containing the absolute path, or NULL on failure.
 * Caller must free the returned string using kfree().
 */
static char *nomount_build_path_from_pwd(const char *rel_name) 
{
    struct path pwd;
    char *cwd_str;
    char *page_buf = __getname();
    char *abs_path = NULL;
    size_t dir_len, name_len;

    if (!page_buf) return NULL;

    get_fs_pwd(current->fs, &pwd);
    cwd_str = d_path(&pwd, page_buf, PATH_MAX);
    path_put(&pwd);

    if (IS_ERR_OR_NULL(cwd_str)) {
        __putname(page_buf);
        return NULL;
    }

    dir_len = strlen(cwd_str);
    name_len = strlen(rel_name);

    if (likely(dir_len + name_len + 2 <= PATH_MAX)) {
        abs_path = kmalloc(dir_len + name_len + 2, GFP_KERNEL);
        if (abs_path) {
            memcpy(abs_path, cwd_str, dir_len);
            if (dir_len > 0 && cwd_str[dir_len - 1] != '/') {
                abs_path[dir_len] = '/';
                memcpy(abs_path + dir_len + 1, rel_name, name_len + 1);
            } else {
                memcpy(abs_path + dir_len, rel_name, name_len + 1);
            }
        }
    }
    
    __putname(page_buf);
    return abs_path;
}

/**
 * nomount_resolve_path - Look up the real physical path for a virtual path
 * @pathname: The requested virtual path
 *
 * Performs a fast RCU-protected hash lookup to find redirection rules.
 * Returns a pointer to the real path string, or NULL if no rule matches.
 * The returned string is a kernel-allocated copy and must be freed by the caller.
 */
char *nomount_resolve_path(const char *pathname) {
    struct nomount_rule *rule;
    u32 hash;
    size_t len;
    char *result = NULL;

    if (unlikely(!pathname || IS_ERR_OR_NULL(pathname))) return NULL;
    if (unlikely(nomount_num_rules() == 0)) return NULL;

    len = strlen(pathname);
    hash = full_name_hash(NULL, pathname, len);

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_vpath, rule, vpath_node, hash) {
        if (rule->v_hash == hash && rule->vp_len == len) {
            if (strcmp(pathname, rule->virtual_path) == 0) {
                result = kstrdup(rule->real_path, GFP_ATOMIC);
                break;
            }
        }
    }
    rcu_read_unlock();

    return result;
}
EXPORT_SYMBOL(nomount_resolve_path);

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
    const char *v_path;
    char *res;
    int len;

    if (unlikely(!path || !path->dentry || !path->dentry->d_inode)) return NULL;
    if (unlikely(nomount_num_rules() == 0)) return NULL;
    if (unlikely(!nomount_bloom_test_ino(path->dentry->d_inode->i_ino)))
        return NULL;

    nm_enter();
    v_path = nomount_get_static_vpath(path->dentry->d_inode);

    if (v_path) {
        len = strlen(v_path);
        if (buflen >= len + 1) {
            res = buf + buflen - 1;
            *res = '\0';
            res -= len;
            memcpy(res, v_path, len);
            nm_exit();
            return res;
        }
    }

    nm_exit();
    return NULL;
}
EXPORT_SYMBOL(nomount_handle_dpath);

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
    bool is_injected, is_dir;

    if (!inode || IS_ERR_OR_NULL(inode)) return 0;
    if (unlikely(nomount_num_rules() == 0)) return 0;
    if (unlikely(!nomount_bloom_test_ino(inode->i_ino)))
        return 0;

    if (unlikely(!nomount_should_skip())) {
        nm_enter();
        is_injected = nomount_is_injected_file(inode);
        is_dir = nomount_is_traversal_allowed(inode, mask);
        nm_exit();

        if (is_injected || is_dir) {
            if (mask & (MAY_WRITE | MAY_APPEND))
                return 0;

            return 1; 
        }
    }

    return 0;
}
EXPORT_SYMBOL(nomount_allow_access);

/**
 * nomount_handle_faccessat - Intercept early access checks for DFDs
 * @dfd: Directory file descriptor
 * @filename: User-provided path string
 * @mode: Access mode requested (e.g., F_OK, W_OK)
 * @lookup_flags: VFS path lookup flags
 * @out_res: Pointer to store the actual access result
 *
 * Mitigates relative path evasion by reconstructing the absolute path 
 * and performing access checks before the native VFS logic takes over.
 * 
 * Returns true if NoMount handled the request, false otherwise.
 */
bool nomount_handle_faccessat(int dfd, const char __user *filename, int mode, unsigned int lookup_flags, long *out_res)
{
    struct filename *tmp_name;
    char *nm_abs, *nm_res, *slash;
    const char *check_name;
    struct path path;
    int res;

    if (nomount_should_skip() || !filename)
        return false;

    if (unlikely(nomount_num_rules() == 0)) return false;

    tmp_name = getname_flags(filename, 0, NULL);
    if (IS_ERR(tmp_name))
        return false;

    if (tmp_name->name[0] == '/') {
        putname(tmp_name);
        return false; 
    }

    check_name = tmp_name->name;
    slash = strrchr(check_name, '/');
    if (slash && *(slash + 1) != '\0') {
        check_name = slash + 1;
    }

    if (unlikely(!nomount_bloom_test_path(check_name))) {
        putname(tmp_name);
        return false;
    }

    nm_abs = nomount_build_absolute_path(dfd, tmp_name->name);
    putname(tmp_name);

    if (nm_abs) {
        nm_res = nomount_resolve_path(nm_abs);
        kfree(nm_abs);

        if (nm_res) {
            if (mode & MAY_WRITE) {
                kfree(nm_res);
                *out_res = -EACCES;
                return true;
            }

            nm_enter();
            res = kern_path(nm_res, lookup_flags, &path);
            nm_exit();
            kfree(nm_res);

            if (res == 0) {
                path_put(&path);
                *out_res = 0;
            } else {
                *out_res = res;
            }
            return true;
        }
    }

    return false; 
}
EXPORT_SYMBOL(nomount_handle_faccessat);

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
    char *target = NULL, *abs_path = NULL, *slash;
    const char *check_name;
    struct filename *new_name;

    if (IS_ERR_OR_NULL(name) || !name->name) return name;
    if (nomount_should_skip()) return name;
    if (unlikely(nomount_num_rules() == 0)) return name;

    check_name = name->name;
    slash = strrchr(check_name, '/');
    if (slash && *(slash + 1) != '\0') {
        check_name = slash + 1;
    }

    if (unlikely(!nomount_bloom_test_path(check_name))) return name;

    if (name->name[0] != '/') {
        abs_path = nomount_build_path_from_pwd(name->name);
        if (!abs_path) return name;
        check_name = abs_path;
    } else {
        check_name = name->name;
    }

    target = nomount_resolve_path(check_name);
    
    if (abs_path) kfree(abs_path);

    if (target) {
        new_name = getname_kernel(target);
        kfree(target);

        if (!IS_ERR(new_name)) {
            new_name->uptr = name->uptr;
            new_name->aname = name->aname;
            putname(name);
            return new_name;
        }
    }

    return name;
}

/**
 * nomount_getxattr_hook - Spoof SELinux contexts for injected files
 * @dentry: The dentry being queried
 * @name: The name of the extended attribute (e.g., "security.selinux")
 * @value: Buffer to store the attribute value
 * @size: Size of the buffer
 *
 * Prevents SELinux context leaks by returning the context of the native 
 * parent directory rather than the context of the underlying file in /data.
 * 
 * Returns the size of the attribute value or a negative error code.
 */
ssize_t nomount_getxattr_hook(struct dentry *dentry, const char *name, void *value, size_t size)
{
    struct nomount_rule *rule;
    struct path parent_path;
    const struct cred *old_cred;
    ssize_t ret = -EOPNOTSUPP;
    unsigned long ino;

    if (nomount_should_skip() || !dentry || !dentry->d_inode) return ret;
    if (unlikely(nomount_num_rules() == 0)) return ret;

    ino = dentry->d_inode->i_ino;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, ino) {
        if (rule->real_ino == ino) {
            if (rule->parent_vpath) {
                nm_enter();
                old_cred = override_creds(&init_cred);
                if (kern_path(rule->parent_vpath, LOOKUP_FOLLOW, &parent_path) == 0) {
                    ret = nm_vfs_getxattr(parent_path.dentry, name, value, size);
                    path_put(&parent_path);
                } else {
                    ret = -ENOENT;
                }
                revert_creds(old_cred);
                nm_exit();
            }
            rcu_read_unlock();
            return ret;
        }
    }
    rcu_read_unlock();
    return ret;
}
EXPORT_SYMBOL(nomount_getxattr_hook);

/**
 * nomount_setxattr_hook - Allow elevated writing of extended attributes
 * @dentry: The dentry being modified
 * @name: Attribute name
 * @value: Attribute value
 * @size: Value size
 * @flags: Modification flags
 *
 * Uses elevated capabilities to write xattrs to the underlying file.
 */
int nomount_setxattr_hook(struct dentry *dentry, const char *name, const void *value, size_t size, int flags)
{
    struct nomount_rule *rule;
    struct path r_path;
    const struct cred *old_cred;
    int ret = -EOPNOTSUPP;
    unsigned long ino;

    if (nomount_should_skip() || !dentry || !dentry->d_inode)
        return ret;

    if (unlikely(nomount_num_rules() == 0))
        return ret;

    ino = dentry->d_inode->i_ino;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, ino) {
        if (rule->real_ino == ino) {
            nm_enter();
            old_cred = override_creds(&init_cred);
            
            if (kern_path(rule->real_path, LOOKUP_FOLLOW, &r_path) == 0) {
                ret = nm_vfs_setxattr(r_path.dentry, name, value, size, flags);
                path_put(&r_path);
            } else {
                ret = -ENOENT;
            }
            
            revert_creds(old_cred);
            nm_exit();
            break; 
        }
    }
    rcu_read_unlock();

    return ret;
}
EXPORT_SYMBOL(nomount_setxattr_hook);

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
    struct nomount_dir_node *curr_dir = NULL, *tmp;
    struct nomount_child_name *child;
    unsigned long v_index;
    struct inode *dir_inode = file_inode(file);
    bool dir_found = false;

    if (!dir_inode || nomount_should_skip()) return;
    if (unlikely(nomount_num_dirs() == 0)) return;

    nm_enter();
    rcu_read_lock();

    hash_for_each_possible_rcu(nomount_dirs_ht, tmp, node, dir_inode->i_ino) {
        if (tmp->dir_ino == dir_inode->i_ino) {
            curr_dir = tmp;
            dir_found = true;
            break;
        }
    }

    if (!dir_found) {
        rcu_read_unlock();
        nm_exit();
        return;
    }

    if (ctx->pos >= NOMOUNT_MAGIC_POS) {
        v_index = (unsigned long)(ctx->pos - NOMOUNT_MAGIC_POS);
    } else {
        v_index = 0;
        ctx->pos = NOMOUNT_MAGIC_POS;
    }

    list_for_each_entry_rcu(child, &curr_dir->children_names, list) {
        if (child->v_index < v_index) continue;

        if (!dir_emit(ctx, child->name, strlen(child->name), child->fake_ino, child->d_type))
            break;

        ctx->pos = NOMOUNT_MAGIC_POS + child->v_index + 1;
    }

    rcu_read_unlock();
    nm_exit();
}
EXPORT_SYMBOL(nomount_vfs_inject_dir);

/**
 * __nomount_auto_inject_parent - Create a fake directory entry node
 * @parent_ino: Inode of the native parent directory
 * @name: Name of the child entry to inject
 * @type: Directory entry type (e.g., DT_REG, DT_DIR)
 * @full_v_path: The complete virtual path for hashing
 *
 * Automatically tracks new entries to be injected during getdents calls.
 * This function assumes the caller holds the nomount_write_mutex
 * and is not in a recursive context.
 */
static void __nomount_auto_inject_parent(unsigned long parent_ino, const char *name, unsigned char type, const char *full_v_path)
{
    struct nomount_dir_node *dir_node = NULL, *curr;
    struct nomount_child_name *child;

    if (unlikely(nomount_num_dirs() == 0)) return;

    hash_for_each_possible(nomount_dirs_ht, curr, node, parent_ino) {
        if (curr->dir_ino == parent_ino) {
            dir_node = curr;
            break;
        }
    }

    if (!dir_node) {
        dir_node = kzalloc(sizeof(*dir_node), GFP_KERNEL);
        if (dir_node) {
            INIT_LIST_HEAD(&dir_node->cleanup_list);
            dir_node->dir_ino = parent_ino;
            INIT_LIST_HEAD(&dir_node->children_names);
            dir_node->next_child_index = 0;
            hash_add_rcu(nomount_dirs_ht, &dir_node->node, parent_ino);
            nomount_bloom_add_ino(parent_ino);
            atomic_inc(&nm_active_dirs);
        }
    }

    if (dir_node) {
        bool exists = false;
        list_for_each_entry(child, &dir_node->children_names, list) {
            if (strcmp(child->name, name) == 0) {
                exists = true; 
                break;
            }
        }

        if (!exists) {
            child = kzalloc(sizeof(*child), GFP_KERNEL);
            if (child) {
                child->name = kstrdup(name, GFP_KERNEL);
                child->d_type = type;
                child->fake_ino = (unsigned long)full_name_hash(NULL, full_v_path, strlen(full_v_path));
                child->v_index = dir_node->next_child_index++;
                list_add_tail_rcu(&child->list, &dir_node->children_names);
            }
        }
    }
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

    if (!path || !stat || IS_ERR_OR_NULL(path) || IS_ERR_OR_NULL(stat)) return;
    if (unlikely(nomount_num_rules() == 0)) return;
    inode = d_backing_inode(path->dentry);
    if (!inode) return;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino) {
            stat->ino = rule->v_ino;
            if (rule->v_dev != 0)
                stat->dev = rule->v_dev;
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

    if (!path || !buf || nomount_should_skip()) return;
    if (unlikely(nomount_num_rules() == 0)) return;
    inode = d_backing_inode(path->dentry);
    if (!inode) return;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino) {
            if (rule->v_fs_type != 0)
                buf->f_type = rule->v_fs_type;
            break;
        }
    }

    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, inode->i_ino) {
        if (rule->v_ino == inode->i_ino) {
            if (rule->v_fs_type != 0)
                buf->f_type = rule->v_fs_type;
            break;
        }
    }

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

    if (unlikely(!inode || !dev || !ino || nomount_should_skip()))
        return false;

    if (unlikely(nomount_num_rules() == 0)) return false;

    if (unlikely(!nomount_bloom_test_ino(inode->i_ino)))
        return false;

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
EXPORT_SYMBOL(nomount_spoof_mmap_metadata);

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
    if (likely(ret == 0) && !nomount_should_skip()) {
        nm_enter();
        nomount_spoof_stat(path, stat);
        nm_exit();
    }
    return ret;
}
EXPORT_SYMBOL(nomount_handle_getattr);

/*** IOCTL API & Module Management ***/

static int nomount_ioctl_add_rule(unsigned long arg)
{
    struct nomount_ioctl_data data;
    struct nomount_rule *rule, *existing;
    char *v_path, *r_path, *parent_name, *slash;
    struct path path, p_path;
    struct kstatfs tmp_stfs;
    unsigned long p_ino;
    u32 hash;

    if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
        return -EFAULT;
    if (!capable(CAP_SYS_ADMIN)) return -EPERM;

    v_path = strndup_user(data.virtual_path, PATH_MAX);
    r_path = strndup_user(data.real_path, PATH_MAX);
    if (IS_ERR(v_path) || IS_ERR(r_path)) return -ENOMEM;
    hash = full_name_hash(NULL, v_path, strlen(v_path));

    mutex_lock(&nomount_write_mutex);
    hash_for_each_possible(nomount_rules_by_vpath, existing, vpath_node, hash) {
        if (existing->v_hash == hash && strcmp(existing->virtual_path, v_path) == 0) {
            mutex_unlock(&nomount_write_mutex);
            kfree(v_path); 
            kfree(r_path);
            return -EEXIST;
        }
    }

    rule = kzalloc(sizeof(*rule), GFP_KERNEL);
    if (!rule) {
        mutex_unlock(&nomount_write_mutex);
        kfree(v_path); kfree(r_path);
        return -ENOMEM;
    }

    rule->virtual_path = v_path;
    rule->real_path = r_path;
    rule->parent_vpath = kstrdup(v_path, GFP_KERNEL);
    if (rule->parent_vpath) {
        char *p_slash = strrchr(rule->parent_vpath, '/');
        if (p_slash && p_slash != rule->parent_vpath) 
            *p_slash = '\0';
        else if (p_slash == rule->parent_vpath) 
            rule->parent_vpath[1] = '\0';
    }

    rule->vp_len = strlen(v_path);
    rule->v_hash = hash;
    rule->real_ino = data.real_ino;
    rule->real_dev = data.real_dev;
    rule->flags = data.flags | NM_FLAG_ACTIVE;

    nm_enter();

    if (kern_path(v_path, LOOKUP_FOLLOW, &path) == 0) {
        rule->v_ino = d_backing_inode(path.dentry)->i_ino;
        rule->v_dev = path.dentry->d_sb->s_dev;
        if (path.dentry->d_sb->s_op->statfs) {
            path.dentry->d_sb->s_op->statfs(path.dentry, &tmp_stfs);
            rule->v_fs_type = tmp_stfs.f_type;
        } else {
            rule->v_fs_type = path.dentry->d_sb->s_magic;
        }
        path_put(&path);
    } else {
        rule->v_ino = (unsigned long)hash;

        parent_name = kstrdup(v_path, GFP_KERNEL);
        slash = parent_name ? strrchr(parent_name, '/') : NULL;
        if (slash) {
            *slash = '\0';
            if (kern_path(parent_name, LOOKUP_FOLLOW, &p_path) == 0) {
                rule->v_dev = p_path.dentry->d_sb->s_dev;

                if (p_path.dentry->d_sb->s_op->statfs) {
                    p_path.dentry->d_sb->s_op->statfs(p_path.dentry, &tmp_stfs);
                    rule->v_fs_type = tmp_stfs.f_type;
                } else {
                    rule->v_fs_type = p_path.dentry->d_sb->s_magic;
                }

                p_ino = d_backing_inode(p_path.dentry)->i_ino;
                __nomount_auto_inject_parent(p_ino, slash + 1, 
                    (data.flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG, v_path);
                path_put(&p_path);
            }
        }
        kfree(parent_name);
    }

    __nomount_collect_parents(r_path);
    nm_exit();

    nomount_bloom_add_path(v_path);
    if (r_path) nomount_bloom_add_path(r_path);
    if (rule->real_ino) nomount_bloom_add_ino(rule->real_ino);
    if (rule->v_ino) nomount_bloom_add_ino(rule->v_ino);

    slash = strrchr(v_path, '/');
    if (slash && *(slash + 1) != '\0') {
        nomount_bloom_add_path(slash + 1);
    }

    hash_add_rcu(nomount_rules_by_vpath, &rule->vpath_node, hash);
    if (rule->real_ino)
        hash_add_rcu(nomount_rules_by_real_ino, &rule->real_ino_node, rule->real_ino);
    if (rule->v_ino)
        hash_add_rcu(nomount_rules_by_v_ino, &rule->v_ino_node, rule->v_ino);

    list_add_tail_rcu(&rule->list, &nomount_rules_list);
    atomic_inc(&nm_active_rules);
    mutex_unlock(&nomount_write_mutex);

    return 0;
}

static int nomount_ioctl_del_rule(unsigned long arg)
{
    struct nomount_ioctl_data data;
    struct nomount_rule *rule, *victim = NULL;
    struct nomount_dir_node *dir;
    struct nomount_child_name *child, *tmp_child, *victim_child = NULL;
    struct hlist_node *tmp;
    char *v_path;
    u32 hash;
    int bkt;

    if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
        return -EFAULT;

    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;

    v_path = strndup_user(data.virtual_path, PATH_MAX);
    if (IS_ERR(v_path))
        return PTR_ERR(v_path);

    hash = full_name_hash(NULL, v_path, strlen(v_path));

    mutex_lock(&nomount_write_mutex);
    hash_for_each_possible_safe(nomount_rules_by_vpath, rule, tmp, vpath_node, hash) {
        if (strcmp(rule->virtual_path, v_path) == 0) {
            rule->flags &= ~NM_FLAG_ACTIVE;
            hash_del_rcu(&rule->vpath_node);
            
            nomount_bloom_del_path(rule->virtual_path);
            if (rule->real_path) nomount_bloom_del_path(rule->real_path);
            
            char *slash = strrchr(rule->virtual_path, '/');
            if (slash && *(slash + 1) != '\0') {
                nomount_bloom_del_path(slash + 1);
            }

            if (rule->real_ino) {
                hash_del_rcu(&rule->real_ino_node);
                nomount_bloom_del_ino(rule->real_ino);
            }
            if (rule->v_ino) {
                hash_del_rcu(&rule->v_ino_node);
                nomount_bloom_del_ino(rule->v_ino);
            }

            list_del_rcu(&rule->list);
            victim = rule;
            atomic_dec(&nm_active_rules);

            hash_for_each(nomount_dirs_ht, bkt, dir, node) {
                list_for_each_entry_safe(child, tmp_child, &dir->children_names, list) {
                    if (child->fake_ino == hash) {
                        list_del_rcu(&child->list);
                        victim_child = child;
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
        synchronize_rcu();

        if (victim_child) {
            kfree(victim_child->name);
            kfree(victim_child);
        }

        kfree(victim->parent_vpath);
        kfree(victim->virtual_path);
        kfree(victim->real_path);
        kfree(victim);
        
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
        if (rule->real_ino)
            hash_del_rcu(&rule->real_ino_node);
        if (rule->v_ino)
            hash_del_rcu(&rule->v_ino_node);

        list_del_rcu(&rule->list);
        list_add_tail(&rule->cleanup_list, &rule_victims);
        rule->flags &= ~NM_FLAG_ACTIVE;
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

    memset(nomount_bloom_paths, 0, NOMOUNT_BLOOM_SIZE * sizeof(unsigned short));
    memset(nomount_bloom_inos, 0, NOMOUNT_BLOOM_SIZE * sizeof(unsigned short));

    atomic_set(&nm_active_rules, 0);
    atomic_set(&nm_active_dirs, 0);
    
    mutex_unlock(&nomount_write_mutex);

    synchronize_rcu();

    list_for_each_entry_safe(dir_node, tmp_dir, &dir_victims, cleanup_list) {
        list_del(&dir_node->cleanup_list);
        kfree(dir_node);
    }

    list_for_each_entry_safe(child, tmp_child, &dir_victims_children, cleanup_list) {
        list_del(&child->cleanup_list);
        kfree(child->name);
        kfree(child);
    }

    list_for_each_entry_safe(rule, tmp_rule, &rule_victims, cleanup_list) {
        list_del(&rule->cleanup_list);
        kfree(rule->parent_vpath);
        kfree(rule->virtual_path);
        kfree(rule->real_path);
        kfree(rule);
    }

    list_for_each_entry_safe(uid_node, tmp_uid, &uid_victims, cleanup_list) {
        list_del(&uid_node->cleanup_list);
        kfree(uid_node);
    }

    return 0;
}

static int nomount_ioctl_list_rules(unsigned long arg)
{
    struct nomount_rule *rule;
    char *kbuf;
    size_t len = 0;
    const size_t max_size = MAX_LIST_BUFFER_SIZE;
    int ret = 0;

    kbuf = vmalloc(max_size);
    if (!kbuf) return -ENOMEM;

    memset(kbuf, 0, max_size);

    rcu_read_lock();
    list_for_each_entry_rcu(rule, &nomount_rules_list, list) {
        size_t entry_len = strlen(rule->virtual_path) + strlen(rule->real_path) + 4; 

        if (len + entry_len >= max_size - 1)
            break;

        len += scnprintf(kbuf + len, max_size - len, "%s->%s\n", 
                         rule->virtual_path, rule->real_path);
    }
    rcu_read_unlock();

    if (len > 0) {
        if (copy_to_user((void __user *)arg, kbuf, len))
            ret = -EFAULT;
        else
            ret = len; 
    } else {
        ret = 0; 
    }

    vfree(kbuf);
    return ret;
}

static int nomount_ioctl_add_uid(unsigned long arg)
{
    unsigned int uid;
    struct nomount_uid_node *entry;

    if (copy_from_user(&uid, (void __user *)arg, sizeof(uid)))
        return -EFAULT;
    
    if (nomount_is_uid_blocked(uid)) return -EEXIST;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
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
        kfree(entry); 
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
    hash_init(nomount_uid_ht);

    /* Initialize bloom filters */
    nomount_bloom_paths = vzalloc(NOMOUNT_BLOOM_SIZE * sizeof(unsigned short));
    nomount_bloom_inos = vzalloc(NOMOUNT_BLOOM_SIZE * sizeof(unsigned short));

    if (!nomount_bloom_paths || !nomount_bloom_inos) {
        pr_err("NoMount: Error allocating memory for bloom filters\n");
        if (nomount_bloom_paths) vfree(nomount_bloom_paths);
        if (nomount_bloom_inos) vfree(nomount_bloom_inos);
        return -ENOMEM;
    }

    ret = misc_register(&nomount_device);
    if (ret) return ret;
    atomic_set(&nomount_enabled, 1);
    pr_info("NoMount: Loaded\n");
    return 0;
}

fs_initcall(nomount_init);
