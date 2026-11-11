## NoMount v2.2.0 is here!🎉

This release brings UID-scoped rules, opaque directory redirection, `.remove` support, and the biggest hot-path optimization pass we've ever shipped. The kernel driver and the metamodule **must be updated together**.

> Commit count hit 605 on this release. Driver protocol is now v22. Thanks to everyone who kept reporting, bisecting and testing through the rough week 💙

### Small Changelog:

- kernel/:
  - Wire protocol v22. `nm_rule_hdr` grew from 16 to 24 bytes and `nm_del_hdr` from 10 to 18 to fit the new UID range fields. A v22 CLI cannot talk to a v21 driver (and vice versa); the installer aborts if the running driver is older than v22.
  - UID-scoped rules with ranges: `--uid-range 1000-5000` and `--uid-range all`. A single rule now covers a whole range without enumerating every UID.
  - UID deny sub-ranges: `--uid-excluded 3000-4000`. Combine it with `--uid-range` for `[1000,5000] except [3000,4000]`, or use it alone as "default except this range". Rules with the same target range but different denies are rejected with `-EEXIST`; overlap between ranges is rejected too.
  - Opaque directories: `--opaque`. A directory rule can hide the underlying filesystem entirely and only expose the module's leaves. This is what `.replace` with content actually meant: previously it whiteouted the whole directory and bootlooped when the hidden dir was a system app the framework needed.
  - Pseudo filesystems (`proc`, `sysfs`, `devpts`, `debugfs`, `tracefs`, `selinuxfs`, `cgroup`, `bpf`) are no longer hijacked at the dentry level.
  - Skip d_op installation on dentries that have no rule for their name. This is the biggest change in the release: it removes the d_revalidate tax on every non-rule lookup under a hijacked directory, which is most of them.
  - Skip d_op installation on dentries in pseudo filesystems, and skip the `shrink_dcache_parent` pass when invalidating a single injected name. The `nm_hijack_dentry_ops` publication now uses `smp_store_release` and a single `d_flags` write.
  - Hot-path micro-optimizations, reorder `nm_dir_ops` so the hot fields fit in one cache line, avoid `current_fsuid()` when the leaf is NULL, skip `nm_is_uid_blocked()` and `nm_find_leaf_in_dir()` when there is no leaf or no children, short-circuit `nm_dir_lookup` and `nm_d_revalidate_common` on directories without rules.
  - `nm_d_revalidate_common` no longer reads `dentry->d_op` or `inode->i_op` when it does not need them, and chains to the original `d_revalidate` when the underlying filesystem has one.
  - On unload, walk `i_dentry` to clear `d_op` before freeing module memory. Fixes the panic on `rmmod` with files open on virtual paths.
  - `iget5_locked` hits no longer allocate an `nm_inode_info`; the probe uses a stack struct and `kmalloc` only happens for `I_NEW`. `d_add` instead of `d_splice_alias` for fresh file inodes.
  - Probe `xattr_handler->__get` via header grep instead of `LINUX_VERSION_CODE`, so kernel trees that never backported the member compile again.
  - `inode_state_read_once` for I_NEW checks (mainline >=6.19 compatibility).
  - Deferred reclaim of rules and leaves moved fully to process context. `nm clear rules` no longer blocks on `synchronize_rcu`.
  - Internal naming unified to the `nm_` prefix.

- userspace/:
  - `--uid-range <a>-<b>`, `--uid-range all`, `--uid-excluded <c>-<d>`, and `--opaque` flags.
  - `nm rule list` prints `[UID: a-b excl c-d]` and emits `"opaque": true` in the JSON output.
  - Streamlined absolute path handling, chunked output for large rule lists.

- module/:
  - `common.sh` holds the boot pipeline: bootloop detection keyed on `/proc/sys/kernel/random/boot_id`, LKM loading, module disable. `post-fs-data.sh` and `metamount.sh` are thin wrappers over it.
  - Boot order is now correct on both KernelSU and APatch. The previous flow silently lost rules on APatch (and sometimes in KernelSU with specific setups) because metamount.sh ran before the LKM was loaded.
  - Bootloop detection no longer relies on `.booting` existence; a successful boot clears the file, a failed boot keeps the boot_id and the next boot disables the module.
   - `.remove` support: Magisk's .remove marker is recognized alongside `.replace` (`.replace` with content → opaque rule, `.replace` alone → whiteout, `.remove` → whiteout).  `rule-paths.sh` gains `path`, `pair`, `opaque` and `del` kinds, and both `metamount.sh` and the WebUI's hot load/unload use the same flow. Hot unload now deletes opaque rules too.
  - `x86_64` build support: new CI matrix entry, `lkmloader-x86_64`, `nm-x86_64`, and `ARCH_BIN` normalization for KernelSU's `x64` label.
  - Installer aborts if the running driver is below v22, and falls back to the previous LKM if a v22 KO is staged for the next boot. Reboot is required to activate the new driver in LKM mode.

- webui/:
  - Skip / Unskip button on each module card (creates or deletes `skip_mount`).
  - Persist the "Show system apps" toggle across reloads.
  - Back gesture closes the right dialog in every navigation state (fixed double back navigation).
  - AMOLED palette adjustments and clear button corner fix.

and a lot of other misc changes :)

#### Thanks to everyone who kept testing, contributing and reporting through the rough bits: @ZQZCC, @nleloc, @sohan-f, and the testers on the Telegram group 🫂

**Full Changelog**: https://github.com/maxsteeel/nomount/compare/v2.1.1...v2.2.0
