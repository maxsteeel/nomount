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

/*** Bloom Filter Implementation ***/

/**
 * nomount_bloom_add_path - Add a path to the bloom filter
 * @name: The string path to hash and add
 */
static void nomount_bloom_add_path(const char *name)
{
    size_t len = strlen(name);
    u32 h1 = jhash(name, len, 0) & (NOMOUNT_BLOOM_SIZE - 1);
    u32 h2 = jhash(name, len, 1) & (NOMOUNT_BLOOM_SIZE - 1);
    
    set_bit(h1, nomount_bloom_paths);
    set_bit(h2, nomount_bloom_paths);
}

/**
 * nomount_bloom_test_path - Check if a path might exist in the rules
 * @name: The string path to test
 *
 * Returns true if the path might be tracked, false if it is definitely not.
 */
bool nomount_bloom_test_path(const char *name)
{
    size_t len = strlen(name);
    u32 h1 = jhash(name, len, 0) & (NOMOUNT_BLOOM_SIZE - 1);
    u32 h2 = jhash(name, len, 1) & (NOMOUNT_BLOOM_SIZE - 1);
    
    return test_bit(h1, nomount_bloom_paths) && test_bit(h2, nomount_bloom_paths);
}

/**
 * nomount_bloom_add_ino - Add an inode number to the bloom filter
 * @ino: The inode number to add
 */
static inline void nomount_bloom_add_ino(unsigned long ino)
{
    set_bit(ino & (NOMOUNT_BLOOM_SIZE - 1), nomount_bloom_inos);
}

/**
 * nomount_bloom_rebuild - Reconstruct bloom filters from active rules
 *
 * Called after a rule is deleted to clear false positives.
 */
static void nomount_bloom_rebuild(void)
{
    struct nomount_rule *rule;
    struct nomount_dir_node *dir;
    int bkt;
    
    bitmap_zero(nomount_bloom_paths, NOMOUNT_BLOOM_SIZE);
    bitmap_zero(nomount_bloom_inos, NOMOUNT_BLOOM_SIZE);

    list_for_each_entry(rule, &nomount_rules_list, list) {
        nomount_bloom_add_path(rule->virtual_path);
        if (rule->real_path)
            nomount_bloom_add_path(rule->real_path);
            
        if (rule->real_ino)
            nomount_bloom_add_ino(rule->real_ino);
        if (rule->v_ino)
            nomount_bloom_add_ino(rule->v_ino);
    }

    hash_for_each(nomount_dirs_ht, bkt, dir, node) {
        nomount_bloom_add_ino(dir->dir_ino);
    }
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
    if (NOMOUNT_DISABLED()) return true;
    if (unlikely(in_interrupt() || in_nmi() || oops_in_progress)) return true;
    if (nm_is_recursive()) return true;
    if (current->flags & (PF_KTHREAD | PF_EXITING)) return true;

    if (unlikely(!hash_empty(nomount_uid_ht))) {
        if (nomount_is_uid_blocked(current_uid().val)) return true;
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
    if (hash_empty(nomount_uid_ht) || nomount_should_skip()) return false;
    
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

    if (!inode || NOMOUNT_DISABLED()) return false;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, inode->i_ino) {
        if (rule->real_ino == inode->i_ino) {
            found = true;
            break;
        }
    }

    hash_for_each_possible_rcu(nomount_rules_by_v_ino, rule, v_ino_node, inode->i_ino) {
        if (rule->v_ino == inode->i_ino) {
            found = true;
            break;
        }
    }

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

    if (!inode || NOMOUNT_DISABLED())
        return false;

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

    if (unlikely(!inode || NOMOUNT_DISABLED()))
        return NULL;

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
 * nomount_collect_parents - Track parent directories of a real path
 * @real_path: The absolute path of the underlying target file
 *
 * Recursively resolves and registers parent directory inodes to ensure
 * traversal permissions are granted during lookup operations.
 */
static void nomount_collect_parents(const char *real_path)
{
    char *path_tmp, *p;
    struct path kp;
    struct nomount_dir_node *dir_node;

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
            struct nomount_dir_node *curr;
            bool exists;

            unsigned long p_ino = d_backing_inode(kp.dentry)->i_ino;
            path_put(&kp);
            nm_exit();

            mutex_lock(&nomount_write_mutex);
            exists = false;
            hash_for_each_possible(nomount_dirs_ht, curr, node, p_ino) {
                if (curr->dir_ino == p_ino) {
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                dir_node = kzalloc(sizeof(*dir_node), GFP_ATOMIC);
                if (dir_node) {
                    dir_node->dir_ino = p_ino;
                    INIT_LIST_HEAD(&dir_node->children_names);
                    hash_add_rcu(nomount_dirs_ht, &dir_node->node, p_ino);
                }
            }
            mutex_unlock(&nomount_write_mutex);
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
    char *page_buf, *dir_path, *abs_path;
    size_t dir_len, name_len;
    struct fd f;

    if (!name || name[0] == '/' || *name == '\0') return NULL;
    if (dfd == AT_FDCWD) return NULL;
    if (nomount_should_skip()) return NULL;

    page_buf = __getname();
    if (!page_buf)
        return NULL;

    f = fdget(dfd);
    if (!f.file) {
        __putname(page_buf);
        return NULL;
    }

    dir_path = d_path(&f.file->f_path, page_buf, PATH_MAX);
    fdput(f);

    if (IS_ERR(dir_path)) {
        __putname(page_buf);
        return NULL;
    }

    dir_len = strlen(dir_path);
    name_len = strlen(name);

    if (dir_len > PATH_MAX || name_len > NAME_MAX || dir_len + name_len + 2 > PATH_MAX) {
        __putname(page_buf);
        return NULL;
    }

    abs_path = kmalloc(dir_len + 1 + name_len + 1, GFP_KERNEL);
    if (abs_path) {
        memcpy(abs_path, dir_path, dir_len);

        if (dir_len > 1 || dir_path[0] != '/') {
            abs_path[dir_len] = '/';
            memcpy(abs_path + dir_len + 1, name, name_len + 1);
        } else {
            memcpy(abs_path + dir_len, name, name_len + 1);
        }
    }

    __putname(page_buf);
    return abs_path; 
}
EXPORT_SYMBOL(nomount_build_absolute_path);

/**
 * nomount_rule_free_rcu - Safely deallocate a rule after RCU grace period
 * @rp: Pointer to the rcu_head embedded within the nomount_rule
 *
 * This callback is invoked by the kernel once all concurrent RCU readers
 * (e.g., path lookups, permission checks) have finished traversing the
 * rule that was removed from the active lists. It safely frees the 
 * associated path strings and the rule structure itself.
 */
static void nomount_rule_free_rcu(struct rcu_head *rp) {
    struct nomount_rule *rule = container_of(rp, struct nomount_rule, rcu);
    kfree(rule->virtual_path);
    kfree(rule->real_path);
    kfree(rule);
} 

/**
 * nomount_resolve_path - Look up the real physical path for a virtual path
 * @pathname: The requested virtual path
 *
 * Performs a fast RCU-protected hash lookup to find redirection rules.
 * Returns a pointer to the real path string, or NULL if no rule matches.
 */
char *nomount_resolve_path(const char *pathname) {
    struct nomount_rule *rule;
    u32 hash;
    size_t len;

    if (unlikely(!pathname || NOMOUNT_DISABLED()))
        return NULL;

    len = strlen(pathname);
    hash = full_name_hash(NULL, pathname, len);

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_vpath, rule, vpath_node, hash) {
        if (rule->v_hash == hash && rule->vp_len == len) {
            if (strcmp(pathname, rule->virtual_path) == 0) {
                rcu_read_unlock();
                return rule->real_path;
            }
        }
    }
    rcu_read_unlock();

    return NULL;
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

    if (unlikely(!path || !path->dentry || !path->dentry->d_inode))
        return NULL;

    if (!test_bit(path->dentry->d_inode->i_ino & (NOMOUNT_BLOOM_SIZE - 1), nomount_bloom_inos))
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
 * Returns 1 (true) if NoMount overrides native permission checks to 
 * allow traversal, or 0 to fallback to standard VFS permissions.
 */
int nomount_allow_access(struct inode *inode, int mask)
{
    int allowed = 0;

    if (!test_bit(inode->i_ino & (NOMOUNT_BLOOM_SIZE - 1), nomount_bloom_inos))
        return 0;

    if (unlikely(!nomount_should_skip())) {
        nm_enter();
        allowed = nomount_is_injected_file(inode) || nomount_is_traversal_allowed(inode, mask);
        nm_exit();
    }
    
    return allowed;
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
    char *nm_abs, *nm_res;
    struct path path;
    int res;

    if (nomount_should_skip() || !filename)
        return false;

    tmp_name = getname_flags(filename, 0, NULL);
    if (IS_ERR(tmp_name))
        return false;

    nm_abs = nomount_build_absolute_path(dfd, tmp_name->name);
    if (nm_abs) {
        nm_res = nomount_resolve_path(nm_abs);
        if (nm_res) {
            kfree(nm_abs);
            putname(tmp_name);

            if (mode & MAY_WRITE) {
                *out_res = -EACCES;
                return true;
            }

            nm_enter();
            res = kern_path(nm_res, lookup_flags, &path);
            nm_exit();

            if (res == 0) {
                path_put(&path);
            }
            *out_res = res;
            return true;
        }
        kfree(nm_abs);
    }
    putname(tmp_name);

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
    char *target = NULL, *tmp_buf;
    struct filename *new_name;
    const char *full_path_ptr;

    if (!nomount_bloom_test_path(name->name)) return name;
    if (nomount_should_skip() || !name || !name->name)
        return name;

    tmp_buf = __getname();
    if (!tmp_buf) return name;

    if (name->name[0] == '/') {
        full_path_ptr = (char *)name->name;
    } else {
        struct fs_struct *fs = current->fs;
        char *cwd_str;
        char *t_buf = (char *)__get_free_page(GFP_ATOMIC); 

        if (!t_buf) {
            __putname(tmp_buf);
            return name;
        }

        spin_lock(&fs->lock);
        cwd_str = d_path(&fs->pwd, t_buf, PAGE_SIZE);
        spin_unlock(&fs->lock);

        if (IS_ERR(cwd_str)) {
            free_page((unsigned long)t_buf);
            __putname(tmp_buf);
            return name;
        }

        snprintf(tmp_buf, PATH_MAX, "%s/%s", cwd_str, name->name);
        full_path_ptr = tmp_buf;

        free_page((unsigned long)t_buf);
    }

    rcu_read_lock();
    target = nomount_resolve_path(full_path_ptr);
    if (target) {
        new_name = getname_kernel(target);
        rcu_read_unlock();

        if (!IS_ERR(new_name)) {
            new_name->uptr = name->uptr;
            new_name->aname = name->aname;
            putname(name);
            __putname(tmp_buf);
            return new_name;
        }
    } else {
        rcu_read_unlock();
    }

    __putname(tmp_buf);
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
    char *vpath_copy, *last_slash;

    if (nomount_should_skip() || !dentry || !dentry->d_inode)
        return ret;

    ino = dentry->d_inode->i_ino;

    rcu_read_lock();
    hash_for_each_possible_rcu(nomount_rules_by_real_ino, rule, real_ino_node, ino) {
        if (rule->real_ino == ino) {
            vpath_copy = kstrdup(rule->virtual_path, GFP_ATOMIC);
            rcu_read_unlock();

            if (!vpath_copy) return -ENOMEM;

            last_slash = strrchr(vpath_copy, '/');
            if (last_slash && last_slash != vpath_copy) {
                *last_slash = '\0';

                nm_enter();
                old_cred = override_creds(&init_cred);

                if (kern_path(vpath_copy, LOOKUP_FOLLOW, &parent_path) == 0) {
                    ret = nm_vfs_getxattr(parent_path.dentry, name, value, size);
                    path_put(&parent_path);
                } else {
                    ret = -ENOENT;
                }

                revert_creds(old_cred);
                nm_exit();
            }
            kfree(vpath_copy);
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
 * nomount_inject_dents - Insert fake dirents into userspace buffer
 * @file: The directory being read
 * @dirent: Pointer to the userspace dirent buffer
 * @count: Remaining size in the buffer
 * @pos: Current directory offset (f_pos)
 * @compat: Flag indicating 32-bit compat mode
 *
 * Safely appends fake entries to the end of a native directory listing 
 * (readdir/getdents) to ensure injected files are visible to tools like 'ls'.
 */
void nomount_inject_dents(struct file *file, void __user **dirent, int *count, loff_t *pos, int compat)
{
    struct nomount_dir_node *curr_dir;
    struct nomount_child_name *child;
    unsigned long v_index;
    int name_len, reclen;
    struct inode *dir_inode = d_backing_inode(file->f_path.dentry);

    if (!dir_inode || nomount_should_skip()) return;

    if (*pos >= NOMOUNT_MAGIC_POS) {
        unsigned long long diff = (unsigned long long)*pos - NOMOUNT_MAGIC_POS;
        if (diff > 0x7FFFFFFF) {
            v_index = 0;
            *pos = NOMOUNT_MAGIC_POS;
        } else {
            v_index = (unsigned long)diff;
        }
    } else {
        v_index = 0;
        *pos = NOMOUNT_MAGIC_POS;
    }

    nm_enter();
    rcu_read_lock();

    hash_for_each_possible_rcu(nomount_dirs_ht, curr_dir, node, dir_inode->i_ino) {
        if (curr_dir->dir_ino != dir_inode->i_ino) continue;

        list_for_each_entry_rcu(child, &curr_dir->children_names, list) {
            if (child->v_index < v_index) continue;

            name_len = strlen(child->name);
            if (compat) {
                reclen = ALIGN(offsetof(struct linux_dirent, d_name) + name_len + 2, 4);
            } else {
                reclen = ALIGN(offsetof(struct linux_dirent64, d_name) + name_len + 1, sizeof(u64));
            }
            if (*count < reclen) break;
            
            if (compat) {
                struct linux_dirent __user *d32 = (struct linux_dirent __user *)*dirent;
                if (unlikely(put_user(child->fake_ino, &d32->d_ino) ||
                    put_user(NOMOUNT_MAGIC_POS + child->v_index + 1, &d32->d_off) ||
                    put_user(reclen, &d32->d_reclen) ||
                    copy_to_user(d32->d_name, child->name, name_len) ||
                    put_user(0, d32->d_name + name_len) ||
                    put_user(child->d_type, (char __user *)d32 + reclen - 1))) {
                    break;
                }
            } else {
                struct linux_dirent64 __user *d64 = (struct linux_dirent64 __user *)*dirent;
                if (unlikely(put_user(child->fake_ino, &d64->d_ino) ||
                    put_user(NOMOUNT_MAGIC_POS + child->v_index + 1, &d64->d_off) ||
                    put_user(reclen, &d64->d_reclen) ||
                    put_user(child->d_type, &d64->d_type) ||
                    copy_to_user(d64->d_name, child->name, name_len) ||
                    put_user(0, d64->d_name + name_len))) {
                    break;
                }
            }

            *dirent = (void __user *)((char __user *)*dirent + reclen);
            *count -= reclen;
            *pos = NOMOUNT_MAGIC_POS + child->v_index + 1;
        }
        break;
    }

    rcu_read_unlock();
    nm_exit();
}

/**
 * nomount_auto_inject_parent - Create a fake directory entry node
 * @parent_ino: Inode of the native parent directory
 * @name: Name of the child entry to inject
 * @type: Directory entry type (e.g., DT_REG, DT_DIR)
 * @full_v_path: The complete virtual path for hashing
 *
 * Automatically tracks new entries to be injected during getdents calls.
 */
static void nomount_auto_inject_parent(unsigned long parent_ino, const char *name, unsigned char type, const char *full_v_path)
{
    struct nomount_dir_node *dir_node = NULL, *curr;
    struct nomount_child_name *child;

    mutex_lock(&nomount_write_mutex);
    hash_for_each_possible(nomount_dirs_ht, curr, node, parent_ino) {
        if (curr->dir_ino == parent_ino) {
            dir_node = curr;
            break;
        }
    }

    if (!dir_node) {
        dir_node = kzalloc(sizeof(*dir_node), GFP_ATOMIC);
        if (dir_node) {
            INIT_LIST_HEAD(&dir_node->cleanup_list);
            dir_node->dir_ino = parent_ino;
            INIT_LIST_HEAD(&dir_node->children_names);
            dir_node->next_child_index = 0;
            hash_add_rcu(nomount_dirs_ht, &dir_node->node, parent_ino);
            nomount_bloom_add_ino(parent_ino);
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
            child = kzalloc(sizeof(*child), GFP_ATOMIC);
            if (child) {
                child->name = kstrdup(name, GFP_ATOMIC);
                child->d_type = type;
                child->fake_ino = (unsigned long)full_name_hash(NULL, full_v_path, strlen(full_v_path));
                child->v_index = dir_node->next_child_index++;
                list_add_tail_rcu(&child->list, &dir_node->children_names);
            }
        }
    }
    mutex_unlock(&nomount_write_mutex);
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

    if (!path || !stat || nomount_should_skip()) return;

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

    if (!test_bit(inode->i_ino & (NOMOUNT_BLOOM_SIZE - 1), nomount_bloom_inos))
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
    struct nomount_rule *rule;
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

    rule = kzalloc(sizeof(*rule), GFP_KERNEL);
    if (!rule) {
        kfree(v_path); kfree(r_path);
        return -ENOMEM;
    }

    rule->virtual_path = v_path;
    rule->real_path = r_path;
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
                nomount_auto_inject_parent(p_ino, slash + 1, 
                    (data.flags & NM_FLAG_IS_DIR) ? DT_DIR : DT_REG, v_path);
                path_put(&p_path);
            }
        }
        kfree(parent_name);
    }

    nomount_collect_parents(r_path);
    nm_exit();
    
    mutex_lock(&nomount_write_mutex);

    {
        struct nomount_rule *existing;
        hash_for_each_possible(nomount_rules_by_vpath, existing, vpath_node, hash) {
            if (existing->v_hash == hash && strcmp(existing->virtual_path, v_path) == 0) {
                mutex_unlock(&nomount_write_mutex);
                kfree(rule->virtual_path); 
                kfree(rule->real_path);
                kfree(rule);
                return -EEXIST;
            }
        }
    }

    nomount_bloom_add_path(v_path);
    if (r_path) nomount_bloom_add_path(r_path);
    if (rule->real_ino) nomount_bloom_add_ino(rule->real_ino);
    if (rule->v_ino) nomount_bloom_add_ino(rule->v_ino);

    hash_add_rcu(nomount_rules_by_vpath, &rule->vpath_node, hash);
    if (rule->real_ino)
        hash_add_rcu(nomount_rules_by_real_ino, &rule->real_ino_node, rule->real_ino);

    if (rule->v_ino)
        hash_add_rcu(nomount_rules_by_v_ino, &rule->v_ino_node, rule->v_ino);

    list_add_tail_rcu(&rule->list, &nomount_rules_list);
    mutex_unlock(&nomount_write_mutex);

    return 0;
}

static int nomount_ioctl_del_rule(unsigned long arg)
{
    struct nomount_ioctl_data data;
    struct nomount_rule *rule, *victim = NULL;
    struct hlist_node *tmp;
    char *v_path;
    u32 hash;

    if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
        return -EFAULT;

    if (!capable(CAP_SYS_ADMIN))
        return -EPERM;

    v_path = strndup_user(data.virtual_path, PATH_MAX);
    if (IS_ERR(v_path))
        return PTR_ERR(v_path);

    hash = full_name_hash(NULL, v_path, strlen(v_path));

    mutex_lock(&nomount_write_mutex);
    hash_for_each_possible_safe(nomount_rules_by_vpath,
                                rule, tmp, vpath_node, hash) {
        if (strcmp(rule->virtual_path, v_path) == 0) {
            rule->flags &= ~NM_FLAG_ACTIVE;
            hash_del_rcu(&rule->vpath_node);
            if (rule->real_ino)
                hash_del_rcu(&rule->real_ino_node);
            if (rule->v_ino)
                hash_del_rcu(&rule->v_ino_node);
            list_del_rcu(&rule->list);
            victim = rule;
            nomount_bloom_rebuild();
            break;
        }
    }
    mutex_unlock(&nomount_write_mutex);

    if (victim) {
        call_rcu(&victim->rcu, nomount_rule_free_rcu);
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

    bitmap_zero(nomount_bloom_paths, NOMOUNT_BLOOM_SIZE);
    bitmap_zero(nomount_bloom_inos, NOMOUNT_BLOOM_SIZE);
    
    mutex_unlock(&nomount_write_mutex);

    synchronize_rcu();

    list_for_each_entry_safe(dir_node, tmp_dir, &dir_victims, cleanup_list) {
        list_del(&dir_node->cleanup_list);

        list_for_each_entry_safe(child, tmp_child, &dir_node->children_names, list) {
            list_del(&child->list);
            kfree(child->name);
            kfree(child);
        }

        kfree(dir_node);
    }

    list_for_each_entry_safe(rule, tmp_rule, &rule_victims, cleanup_list) {
        list_del(&rule->cleanup_list);
        
        kfree(rule->virtual_path);
        kfree(rule->real_path);
        kfree(rule);
    }

    list_for_each_entry_safe(uid_node, tmp_uid, &uid_victims, cleanup_list) {
        list_del(&uid_node->cleanup_list);
        kfree(uid_node);
    }

    list_for_each_entry_safe(child, tmp_child, &dir_victims_children, cleanup_list) {
        kfree(child->name);
        kfree(child);
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

    ret = misc_register(&nomount_device);
    if (ret) return ret;
    atomic_set(&nomount_enabled, 1);
    pr_info("NoMount: Loaded\n");
    return 0;
}

fs_initcall(nomount_init);

