## NoMount v2.3.0 is here! 🎉

The kernel driver and the metamodule **must be updated together**. There are no API-breaking changes (you can use the new module with previous kernel driver, and vice versa), but the amount of bug fixes below is large enough that running them out of sync is asking for trouble.

### Small Changelog:
> There are too many changes in this version, so this is sort of a summary of most important changes of this release :)

- kernel/:
  - **Rules are now inode-based.** A rule tracks the inode the path resolved to at add-time, not the path string. Rename or move the target and the rule keeps serving the same data as long as the inode is alive, and when the backing inode is gone (after unlink), the path is rebinded.
  - **Pre-opened backing file cache.** Read-only opens of a redirected file reuse a cached `struct file` pointing at the backing, so a redirected `open`+`read`+`close` cycle measures within ~80 ns of the native one. Eligibility is checked on first open (ext4/f2fs only, `f_cred` pointer equality, `S_ISREG`).
  - **Virtual inodes survive lookups.** File rules now go through `iget5_locked` like dir rules already did. The virtual inode stays in the dcache between accesses, which is what lets the pre-opened cache actually hit on repeated operations instead of the file being recreated per access.
  - **Rule list reconstructs paths with `d_path`.** Paths are no longer stored in the kernel heap; `NM_CMD_GET_LIST` rebuilds them on demand from `r_path`. Drops rule path strings from the heap and reports the path the rule actually serves. `GET_LIST` now takes `nm_mutex` instead of `rcu_read_lock` so `d_path` can sleep safely.
  - **Revalidate is O(1).** The virtual inode caches a reference to its leaf. `nm_d_revalidate_common` validates the parent dir and the child hash and returns, with no hash table walk and no `memcmp` in the fast path (rare cases where this fails will fall into the slow path).
  - **Child buckets store `{ leaf *, hash }` inline**, so bucket lookup compares the hash before dereferencing the leaf.
  - **`nm_rule` dropped `r_parent`, `r_name`, `r_name_len`, and the path strings.** `nm rule add` no longer copies names into a per-rule heap buffer.
  - **Readdir resumes at the same child after rule changes.** The cursor encodes a 16-bit hash fragment so `ls` during concurrent `nm rule add/del` no longer skips or duplicates entries.
  - **OOB read fix in `nm_select_rule`** when a single-rule path uses `--uid-range` scoping.
  - **UAF fixes:** leaves are retired via RCU instead of freed while still reachable from the hash; virtual dentries are dropped before restoring `s_op`; `nm clear rules` no longer blocks in the syscall path (it defers cleanup to a workqueue).
  - **Security:** `nm_xattr_set` runs LSM checks via `vfs_setxattr`/`vfs_removexattr`; SUID/SGID bits are cleared on virtual inodes; `unlink`/`rename`/`rmdir` are now managed by NoMount on virtual dentries (previously `f2fs_unlink` could walk into a virtual inode and corrupt slab).
  - **Cross-sb lookups use a proxy inode.** When the backing dentry lives in a different `super_block` (FUSE mounts, user-namespace bind mounts, etc), the lookup materializes a virtual inode that proxies to the backing dentry instead of touching `r_dir->i_op->lookup`. Fixes the `get_fuse_mount_super` crash, and the null dentry deref on the same path.
  - **kernfs superblocks are tracked but never hijacked.** `s_op` is left untouched (no more `cgroup2` panic on `unshare -C -m`), xattr proxies still install. `nm_hijack_superblock` no longer skips them entirely.
  - **Native view honored for blocked UIDs on opaque dirs.** A blocked UID listing an opaque virtual directory now sees the real directory contents, not the empty opaque overlay.
  - **Default rule is protected from `unlink`.** `nm_hijacked_unlink` refuses to drop the fallback rule when the virtual path only has the default UID scope.
  - **Boot-time speedup:** `nm_prepare_native_file` resolves against the parent dentry via `vfs_path_lookup` instead of calling `kern_path` from `/` for every rule.
  - UID overlap/deny checks done directly; childless stub leaves pruned; child array slots reused for small counts; reclaim moved to process context.

- userspace/:
  - `nm rule add` batches in a single bounded pass (no more double walk for size).
  - `nm rule list` flags completed requests so the driver returns clean empty pages.
  - UID range parsing shared between `add` and `del`.

- module/:
  - **`MODDIR` is now set in `boot-completed.sh`.** Without it, the script silently failed to find its temp files, so `nm clear` never ran and the bootloop safety net never triggered. Root cause of several "NoMount disabled itself after a reboot" reports.
  - `customize.sh` restores multi-candidate LKM fallback (`ko` → `lkmloader` → error).
  - `.remove` marker recognized alongside `.replace`; `rule-paths.sh` gains `path`, `pair`, `opaque` and `del` kinds; `metamount.sh` and WebUI hot load/unload now share the same flow.
  - Opaque directories with nonempty contents no longer abort the boot pipeline.
  - **Module logs now go to dmesg/logcat.** The separate `module.log` file is not needed (and not used); everything routes through the dmesg, same log used by the driver, so both logs land in the same place.
  - Injection failures propagate to the WebUI as explicit `nm_load_status=N` / `nm_unload_status=N` markers instead of being swallowed by the script.
  - **`android17-6.18` LKM** added to the pre-compilated LKM list of this module.

- webui/:
  - Module cards, app selector and exclusions are built with `document.createElement` + `textContent` instead of `innerHTML` templates. `realName` (from `module.prop`), `appLabel`/`pkg` (from `ksu.listPackages`) and exclusion `label`/`pkg` all go through `textContent`.
  - App selector reuses the in-memory cache on reopen (no more 250+ ms spinner at reopen).
  - Nav, switch and overlay render paths cut layout work: no transition on `grid-template-rows`, `transform`-only switch thumb, and `--nm-nav-background`/`--nm-scrim-mix`/`--nm-elevation-2` precomputed in `applyAppearance()` instead of `color-mix()` per frame.
  - Module detection fixed: `readModules` runs once at boot and is shared between Home and Modules; `updateModuleCardDOM` accepts a `[modId]` filter so a toggle only walks one module.
  - Locale fetch parallelized. Language selection moved to the Appearance group. Untranslated strings fall back to English.
  - A new "Dump rules" toggle in Advanced writes rules to the log on every boot (debug only), and a "Dump rules now" button triggers it on demand.

and a lot of other misc changes :)

#### Thanks to everyone who kept testing, contributing and reporting through the rough bits: @ZQZCC, @nleloc, @sohan-f, and the testers on the Telegram group 🫂

**Full Changelog**: https://github.com/maxsteeel/nomount/compare/v2.2.0...v2.3.0