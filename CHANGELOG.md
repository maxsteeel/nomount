## NoMount v2.3.1 is here! 🎉

This release is all about making the metamodule as fast and as self-contained as possible. The heavy lifting that used to happen in shell (`find`, one `sh` per batch, `xargs`, `rule-paths.sh`) now lives inside `nm`. Module load, unload and listing are a single native pass, whiteouts / opaque dirs / `.replace` / `.remove` are resolved by the binary, and the WebUI reads everything through that same listing. The module list is now up to 100x faster and boot injection no longer spawns a shell per file.

The kernel also got another hot-path and lifetime pass. The wire protocol is still v23, so there are no ABI changes: the new module works with the 2.3.0 driver and vice versa. Still, there are enough kernel fixes here that you should update both together.

### Small Changelog:
> There are too many changes in this version, so this is sort of a summary of most important changes of this release :)

- kernel/:
  - Rule path hashes are computed once and reused, and the synthetic virtual-parent rule is now shared instead of allocated and copied per operation. This adds up during boot with thousands of rules.
  - Revalidation reuses the directory leaf already bound to the inode instead of probing the hash table again, and `nm_child_matches` short-circuits empty names.
  - Rule validation on add: empty names, `NAME_MAX` overflows, unknown flags, conflicting whiteout/opaque/virtual-dir flags and malformed UID ranges are rejected; trailing slashes are normalized.
  - `.` and `..` path components are rejected and dot entries are skipped in readdir, so a rule can no longer be created for a name that the actor can never match.
  - A rule whose backing path resolves to a virtual inode is rejected (#120), preventing loops. Rename targets and hardlinks on virtual inodes are rejected with `-EPERM`.
  - Invalid backing child dentries are dropped during revalidation, so a replaced or removed backing file no longer keeps serving a stale dentry.
  - Virtual inode size is synced from the backing inode on open and write, so `stat`/`fstat` reports the real size after writes.
  - Async direct I/O on virtual files is rejected with `-EOPNOTSUPP`, since the proxy don't handle it.
  - Repeated mmaps no longer dirty the virtual inode `S_PRIVATE` flag (atomic clear), removing a cacheline write from the mmap hot path.
  - Virtual directory entries now emit their real `d_type` (char, block, fifo, socket, symlink, ...), not just dir/reg, so `ls`, `find` and other readdir consumers see the right type.
  - File handles now preserve the native `encode_fh` for real inodes, and refuse to encode virtual inodes (`-ESTALE`) instead of handing them to NFS/FUSE/overlay export paths (thanks @1385721 to report this bug).
  - Superblock hooks are removed on `put_super` before the superblock can be freed, hook/unhook is unified, and fake xattr tables are freed after an RCU grace period. Fixes a class of UAFs on unmount.
  - Hijacked callbacks fall back to the live `i_op`/`f_op` when the original pointer is being restored, closing a race during rule teardown.
  - Leaf hash table grew from 64 to 1024 buckets, and the UID exclusion bloom filter from 256 to 1024 bits. Rule control lookups and `nomount_is_uid_blocked` collide far less on devices with many rules
  - `setup.sh` without arguments now actually follows the newest `v*` tag (fetching tags first) instead of blindly pulling `dev`; an explicit tag or commit still works.
  - `Kconfig` now `select`s `KEYS`. NoMount genuinely needs the keyring, and kernels built with `CONFIG_KEYS=n` no longer fail to compile.

- userspace/:
  - **New `nm module load <mod_path> [--targets=...]` and `nm module unload <mod_path>`.** The whole module tree is walked in C, in one pass, replacing the old `find | sh rule-paths.sh | xargs` pipeline.
    - All partitions are auto-detected (`system`, `system_ext`, `vendor`, `odm`, `product`, `apex`, `my_*`, ...), with `--targets` to override the list.
    - `.replace` with content becomes an opaque rule, `.replace` alone or empty becomes a whiteout, `.remove` becomes a whiteout, character devices become whiteouts, and symlinks are resolved with loop detection.
    - `trusted.overlay.opaque=y` is honored, and a failed xattr probe simply degrades to "not opaque" instead of aborting the module.
    - Whiteouted directories skip their whole subtree on load, while unload still walks them to remove every rule underneath.
    - Reports `nm_load_status` / `nm_unload_status` on stderr so the boot scripts and the WebUI can react.
  - **New `nm module list --json [--only=a,b]`.** Lists `/data/adb/modules` natively, reads the real name from `module.prop`, the `disable` and `skip_mount` markers, and counts the live rules per module (whiteouts are attributed by virtual path). This is what makes the WebUI listing fast and correct.
  - Rule batching was reworked into a single bounded pass, flushing pages as they fill. Per-rule errors no longer abort the rest of the batch, and invalid paths/UIDs are reported with an explicit message.
  - UID and range parsing is overflow-safe and validated; paths are normalized (`//`, `.`/`..`, trailing slashes) before they reach the driver.
  - `nm --try-lock` internal mode lets the module scripts take a `flock` through the binary, with no dependency on the `flock` utility.

- module/:
  - **Boot injection is now one `nm module load "$mod_path"` per module.** `rule-paths.sh` and `dump_rules.sh` are gone, along with the `find`/`xargs`/per-file shell forks. Whiteouts, opaque dirs, `.replace`/`.remove` and char devices are all handled by the binary.
  - **Locks were reworked:** each stage takes a dedicated `.boot.flock` / `.lkm.flock` / `.inject.flock` through `nm --try-lock` (no `flock` binary), and the injection lock is held until the script exits. A killed stage can't leave a stale lock behind because the kernel releases the fd, and `metamount.sh` now aborts cleanly on `SIGINT`/`SIGTERM`.
  - The installer prefers `ksud insmod` / `apd insmod` when the root manager supports it, falling back to `lkmloader`, with clearer error messages.
  - `service.sh` logs failures when applying excluded UIDs and the isolated-UID flag instead of swallowing them.
  - `boot-completed.sh` only clears the boot semaphore; the lock files are managed by the flock implementation.

- webui/:
  - **Module listing is a single `nm module list --json` call.** No shell loop, no `find`, no per-file `sh`, no JSON stitching in JS: module cards, file counts, whiteout/opaque attribution and statuses all come from one native listing. It is up to 100x faster and stays correct with `.replace`/`.remove`, whiteouts and opaque dirs.
  - Loading and unloading a module from the WebUI is a single `nm module load` / `nm module unload` call, using the exact same walk as boot.
  - **UI state is read with a single shell command:** UID list, exclusions, safe mode, dump-rules flag and isolated-UID state arrive in one `exec` separated by a marker, instead of one `exec` per value. This makes the webui open more fast.
  - The app list is cached in `localStorage` with a 5-minute TTL and refreshed in the background. The first paint uses the cache, `pm list packages` runs in parallel for multiple users, and the selector re-renders in place when the refresh lands.
  - Home stats and module cards are updated from the cached listing; toggling a module no longer re-lists every module.
  - **Rule-to-module attribution moved into the binary.** The old JS walk over rules, modules and path parents is gone, so listing cost no longer explodes with the number of rules.
  - Material You seed detection was fixed: it reads the live `--primary`, falls back to `cmd overlay lookup` accent tones, and caches the seed, so theme colors are stable and correct.
  - Module ids and paths passed to the shell are quoted, closing an injection/whitespace bug.
  - Removed redundant work from the render paths (cached `scrollTop`, no duplicate lookups), and a missing home cache no longer logs an error.

and a lot of other misc changes :)

#### Thanks to everyone who kept testing, contributing and reporting through this one: @ZQZCC, @sohan-f, and the testers on the Telegram group 🫂

**Full Changelog**: https://github.com/maxsteeel/nomount/compare/v2.3.0-720...v2.3.1
