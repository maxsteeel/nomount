# NoMount - Detail Hook Architecture & Integration

This document details the hook architecture used by NoMount within the Linux Virtual File System (VFS).


## 1. Initialization and Configuration (`Kconfig` & `Makefile`)
*   **Files:** `fs/Kconfig`, `fs/Makefile`
*   **Purpose:** Register NoMount in kernel.
*   **Mechanism:** Adds the `CONFIG_NOMOUNT` flag to allow enabling/disabling the module during kernel configuration (`menuconfig`) and tells the build system to include `nomount.o` in the final link.
*   **Integration:**:
```
diff --git a/fs/Kconfig b/fs/Kconfig
index c34b9d4e0..8ec1f2b07 100644
--- a/fs/Kconfig
+++ b/fs/Kconfig
@@ -xxx,xx +xxx,xx @@

+config NOMOUNT
+	bool "NoMount Path Redirection Subsystem"
+	default y
+	help
+	  NoMount allows path redirection and virtual file injection
+	  without mounting filesystems. Useful for systemless modifications.
+
 endmenu
diff --git a/fs/Makefile b/fs/Makefile
--- a/fs/Makefile
+++ b/fs/Makefile
@@ -xx,x +xx,x @@
 
+obj-$(CONFIG_NOMOUNT) += nomount.o
+
 obj-$(CONFIG_PROC_FS) += proc_namespace.o

```

---

## 2. Path Lookup, Access Control, and Permissions.
*   **Files:** `fs/namei.c` and `fs/open.c`
*   **Hooks:** `getname_flags`, `generic_permission`, `inode_permission` (en `namei.c`) y `do_faccessat` (en `open.c`).
*   **Purpose:** Intercept text strings (paths) that come from the Userspace before the kernel converts them into physical structures (`dentry`/`inode`). And also ensure that the injected files can be traversed and read, while correctly simulating the typical attributes of system partitions.
*   **Mechanism:**
    *   In `namei.c`, the `nomount_getname_hook` hook is executed immediately after copying the path from the user. If the path matches an active rule, the original string is replaced with the actual path of the redirected file. The rest of the kernel processes the call without knowing that it was tricked.
    *   In `namei.c`, the `nomount_allow_access` hook forces a return of `0` (Success) to prevent native DAC/MAC checks (like SELinux) from blocking access to our injected folders.
    *   In `open.c`, the `nomount_handle_faccessat` hook intercepts early calls that use Directory File Descriptors (`dfd`) to resolve relative paths (e.g., `openat(dir_fd, "su")`). It reconstructs the absolute path and checks it against active rules. If it matches, it forces an Early Exit returning the correct access rights (spoofing Read-Only for `MAY_WRITE`), thus completely neutralizing evasion techniques that bypass absolute path filters.
    *   In `open.c`, if a process tries to check whether an injected file is writable (`MAY_WRITE`), the hook returns `-EACCES`, perfectly mimicking the behavior of a read-only mounted partition.
*   **Integration:**

```
diff --git a/fs/namei.c b/fs/namei.c
--- a/fs/namei.c
+++ b/fs/namei.c
@@ -xxx,xx +xxx,xx @@
 
 #define EMBEDDED_NAME_MAX	(PATH_MAX - offsetof(struct filename, iname))
 
+#ifdef CONFIG_NOMOUNT
+extern bool nomount_should_skip(void);
+extern struct filename *nomount_getname_hook(struct filename *name);
+extern int nomount_allow_access(struct inode *inode, int mask);
+#endif
+
 struct filename *
 getname_flags(const char __user *filename, int flags, int *empty)
 {
@@ -xxx,xx +xxx,xx @@ getname_flags(const char __user *filename, int flags, int *empty)
 
 	result->uptr = filename;
 	result->aname = NULL;
+#ifdef CONFIG_NOMOUNT
+	if (!IS_ERR(result) && !nomount_should_skip()) {
+		result = nomount_getname_hook(result);
+	}
+#endif
 	audit_getname(result);
 	return result;
 }
@@ -xxx,xx +xxx,xx @@ int generic_permission(struct inode *inode, int mask)
 {
 	int ret;
 
+#ifdef CONFIG_NOMOUNT
+        if (unlikely(nomount_allow_access(inode, mask)))
+		return 0;
+#endif
+
 	/*
 	 * Do the basic permission checks.
 	 */
@@ -xxx,xx +xxx,xx @@ int inode_permission(struct inode *inode, int mask)
 {
 	int retval;
 
+#ifdef CONFIG_NOMOUNT
+        if (unlikely(nomount_allow_access(inode, mask)))
+		return 0;
+#endif
+
 	retval = sb_permission(inode->i_sb, inode, mask);
 	if (retval)
 		return retval;
diff --git a/fs/open.c b/fs/open.c
--- a/fs/open.c
+++ b/fs/open.c
@@ -xxx,xx +xxx,xx @@ static const struct cred *access_override_creds(void)
 	return old_cred;
 }
 
+#ifdef CONFIG_NOMOUNT
+extern bool nomount_should_skip(void);
+extern bool nomount_is_injected_file(struct inode *inode);
+extern bool nomount_handle_faccessat(int dfd, const char __user *filename,
+									 int mode, unsigned int lookup_flags, long *out_res);
+#endif
+
 static long do_faccessat(int dfd, const char __user *filename, int mode, int flags)
 {
 	struct path path;
@@ -xxx,6 +xx,xx @@ static long do_faccessat(int dfd, const char __user *filename, int mode, int fla
 	unsigned int lookup_flags = LOOKUP_FOLLOW;
 	const struct cred *old_cred = NULL;
 
+#ifdef CONFIG_NOMOUNT
+	long nm_res;
+	if (nomount_handle_faccessat(dfd, filename, mode, lookup_flags, &nm_res))
+		return nm_res;
+#endif
+
 	if (mode & ~S_IRWXO)	/* where's F_OK, X_OK, W_OK, R_OK? */
 		return -EINVAL;
 
@@ -xxx,xx +xxx,xx @@ static long do_faccessat(int dfd, const char __user *filename, int mode, int fla
 
 	inode = d_backing_inode(path.dentry);
 
+#ifdef CONFIG_NOMOUNT
+    /* spoof writable attribute */
+    if (!nomount_should_skip() && nomount_is_injected_file(inode)) {
+        if (mode & MAY_WRITE) {
+            res = -EACCES; // non-writable
+            goto out_path_release;
+        }
+    }
+#endif
+
 	if ((mode & MAY_EXEC) && S_ISREG(inode->i_mode)) {
 		/*
 		 * MAY_EXEC on regular files is denied if the fs is mounted
```

---

## 3. Inverse Resolution and Visibility
*   **File:** `fs/d_path.c`
*   **Hook:** `d_path`
*   **Purpose:** Avoid information leaks and falsify real paths through virtual paths.
*   **Mechanism:** If the kernel tries to resolve the path of a file that was injected by us, `nomount_handle_dpath` intercepts the output and returns the "Virtual Path" instead of the actual physical location (e.g. returning `/system/bin/su` instead of `/data/adb/modules/test/su`).
*   **Integration:**
```
diff --git a/fs/d_path.c b/fs/d_path.c
--- a/fs/d_path.c
+++ b/fs/d_path.c
@@ -xxx,xx +xxx,xx @@ static void get_fs_root_rcu(struct fs_struct *fs, struct path *root)
 	} while (read_seqcount_retry(&fs->seq, seq));
 }
 
+#ifdef CONFIG_NOMOUNT
+extern char *nomount_handle_dpath(const struct path *path, char *buf, int buflen);
+#endif
+
 /**
  * d_path - return the path of a dentry
  * @path: path to report
@@ -xxx,x +xxx,xx @@ char *d_path(const struct path *path, char *buf, int buflen)

+#ifdef CONFIG_NOMOUNT
+	char *nm_path = nomount_handle_dpath(path, buf, buflen);
+	if (unlikely(nm_path)) {
+		return nm_path;
+	}
+#endif
+
 	/*
 	 * We have various synthetic filesystems that never get mounted.  On
 	 * these filesystems dentries are never used for lookup purposes, and
```

---

## 4. Directory Listing
*   **File:** `fs/readdir.c`
*   **Hooks:** `sys_getdents`, `ksys_getdents64`, `compat_sys_getdents`
*   **Purpose:** Allow commands like `ls` or listing APIs to see virtual files injected into legitimate system directories.
*   **Mechanism:** To avoid *Bootloops* or *Deadlocks* when interacting with `iterate_dir` locks, NoMount injects the data in a "Post-Iteration" manner. The `nomount_inject_post_getdents` macro allows the kernel to read the real physical files first and, just before returning the buffer to the user, inject the fake `linux_dirent` structures at the end of the memory buffer.
*   **Integration:**
```
diff --git a/fs/readdir.c b/fs/readdir.c
--- a/fs/readdir.c
+++ b/fs/readdir.c
@@ -xx,xx +xx,xx @@
 	unsafe_copy_to_user(dst, src, len, label);		\
 } while (0)
 
+#ifdef CONFIG_NOMOUNT
+extern void nomount_inject_dents(struct file *file, void __user **dirent, int *count, loff_t *pos, int compat);
+
+#define nomount_inject_post_getdents(file, buf_ptr, init_count, is_compat, error) \
+do {                                                                              \
+    if ((error) >= 0 && !signal_pending(current)) {                               \
+        nomount_inject_dents((file), (void __user **)&(buf_ptr)->current_dir,     \
+                             &(buf_ptr)->count, &(file)->f_pos, (is_compat));     \
+        (error) = (init_count) - (buf_ptr)->count;                                \
+    }                                                                             \
+} while (0)
+#endif
+
 
 int iterate_dir(struct file *file, struct dir_context *ctx)
 {
@@ -xxx,xx +xxx,xx @@ SYSCALL_DEFINE3(getdents, unsigned int, fd,
 		else
 			error = count - buf.count;
 	}
+#ifdef CONFIG_NOMOUNT
+	nomount_inject_post_getdents(f.file, &buf, count, 0, error);
+#endif
 	fdput_pos(f);
 	return error;
 }
@@ -xxx,xx +xxx,xx @@ SYSCALL_DEFINE3(getdents64, unsigned int, fd,
 		else
 			error = count - buf.count;
 	}
+#ifdef CONFIG_NOMOUNT
+	nomount_inject_post_getdents(f.file, &buf, count, 0, error);
+#endif
 	fdput_pos(f);
 	return error;
 }
@@ -xxx,xx +xxx,xx @@ COMPAT_SYSCALL_DEFINE3(getdents, unsigned int, fd,
 		else
 			error = count - buf.count;
 	}
+#ifdef CONFIG_NOMOUNT
+	nomount_inject_post_getdents(f.file, &buf, count, 1, error);
+#endif
 	fdput_pos(f);
 	return error;
 }
```

---

## 5. Metadata Spoofing (Stat & Mmap)
To be undetectable, the metadata of the files and file systems must match their virtual location.

*   **File Attributes (`fs/stat.c`):**
    *   **Hook:** `vfs_getattr`
    *   **Mechanism:** A *Wrapper* pattern is used. The `nomount_handle_getattr` hook captures the native output of `vfs_getattr_nosec`. If the original read was successful, it overwrites the `inode` and `device id` (dev) in the `kstat` structure before returning it to the user.

*   **Memory Maps (`fs/proc/task_mmu.c`):**
    *   **Hook:** `show_map_vma`
    *   **Mechanism:** When a process reads `/proc/self/maps`, the kernel prints the loaded libraries into memory. The `nomount_spoof_mmap_metadata` hook replaces the raw device and inode just before they are printed to the `seq_file`.

*   **Filesystem Attributes (`fs/statfs.c`):**
    *   **Hook:** `vfs_statfs`
    *   **Mechanism:** Alter the `kstatfs` structure. If an application requests partition metadata (e.g. to check if the file is on an `ext4` or `erofs` volume), the `nomount_spoof_statfs` hook injects the "Magic Number" (FS type) corresponding to the desired virtual partition.
*  **Integration:**
```
diff --git a/fs/proc/task_mmu.c b/fs/proc/task_mmu.c
index 839b6d686..bd581901c 100644
--- a/fs/proc/task_mmu.c
+++ b/fs/proc/task_mmu.c
@@ -xxx,xx +xxx,xx @@ static void show_vma_header_prefix(struct seq_file *m,
 	seq_putc(m, ' ');
 }
 
+#ifdef CONFIG_NOMOUNT
+extern bool nomount_should_skip(void);
+extern bool nomount_spoof_mmap_metadata(struct inode *inode, dev_t *dev, unsigned long *ino);
+#endif
+
 static void
 show_map_vma(struct seq_file *m, struct vm_area_struct *vma)
 {
@@ -xxx,xx +xxx,xx @@ show_map_vma(struct seq_file *m, struct vm_area_struct *vma)
 		struct inode *inode = file_inode(vma->vm_file);
 		dev = inode->i_sb->s_dev;
 		ino = inode->i_ino;
+#ifdef CONFIG_NOMOUNT
+		if (!nomount_should_skip()) {
+			nomount_spoof_mmap_metadata(inode, &dev, &ino);
+		}
+#endif
 		pgoff = ((loff_t)vma->vm_pgoff) << PAGE_SHIFT;
 	}
 
diff --git a/fs/stat.c b/fs/stat.c
--- a/fs/stat.c
+++ b/fs/stat.c
@@ -xxx,xx +xxx,xx @@ int vfs_getattr_nosec(const struct path *path, struct kstat *stat,
 }
 EXPORT_SYMBOL(vfs_getattr_nosec);
 
+#ifdef CONFIG_NOMOUNT
+extern int nomount_handle_getattr(int ret, const struct path *path, struct kstat *stat);
+#endif
+
 /*
  * vfs_getattr - Get the enhanced basic attributes of a file
  * @path: The file of interest
@@ -xxx,xx +xxx,xx @@ int vfs_getattr(const struct path *path, struct kstat *stat,
 	retval = security_inode_getattr(path);
 	if (retval)
 		return retval;
+#ifdef CONFIG_NOMOUNT
+        return nomount_handle_getattr(vfs_getattr_nosec(path, stat, request_mask, query_flags), path, stat);
+#else
 	return vfs_getattr_nosec(path, stat, request_mask, query_flags);
+#endif
 }
 EXPORT_SYMBOL(vfs_getattr);
 
diff --git a/fs/statfs.c b/fs/statfs.c
--- a/fs/statfs.c
+++ b/fs/statfs.c
@@ -xxx,xx +xxx,xx @@ int vfs_get_fsid(struct dentry *dentry, __kernel_fsid_t *fsid)
 }
 EXPORT_SYMBOL(vfs_get_fsid);
 
+#ifdef CONFIG_NOMOUNT
+extern bool nomount_should_skip(void);
+extern void nomount_spoof_statfs(const struct path *path, struct kstatfs *buf);
+#endif
+
 int vfs_statfs(const struct path *path, struct kstatfs *buf)
 {
 	int error;
@@ -xxx,xx +xxx,xx @@ int vfs_statfs(const struct path *path, struct kstatfs *buf)
 	error = statfs_by_dentry(path->dentry, buf);
 	if (!error)
 		buf->f_flags = calculate_f_flags(path->mnt);
+#ifdef CONFIG_NOMOUNT
+	if (!nomount_should_skip())
+		nomount_spoof_statfs(path, buf);
+#endif
 	return error;
 }
 EXPORT_SYMBOL(vfs_statfs);

