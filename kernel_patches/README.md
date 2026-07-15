# NoMount — kernel VFS path-redirection patches

Mountless, systemless module loading via VFS-layer path redirection and virtual
dirent injection — no entries in `/proc/mounts`. These patches add the
`/dev/nomount` driver and are **rebased onto pristine GKI**, so each applies to a
clean kernel tree with **no SUSFS prerequisite**. (For a kernel that also has
SUSFS, use the [`susfs/`](susfs/) variant instead.)

## Files

| Patch | Android | Kernel | GKI base branch |
|-------|---------|--------|-----------------|
| `nomount-android12-5.10.patch` | 12 | 5.10 | `common/android12-5.10` |
| `nomount-android13-5.15.patch` | 13 | 5.15 | `common/android13-5.15` |
| `nomount-android14-6.1.patch`  | 14 | 6.1  | `common/android14-6.1`  |
| `nomount-android15-6.6.patch`  | 15 | 6.6  | `common/android15-6.6`  |
| `nomount-android16-6.12.patch` | 16 | 6.12 | `common/android16-6.12` |

Each patch touches 10 core VFS files (`fs/Kconfig`, `fs/Makefile`, `fs/d_path.c`,
`fs/namei.c`, `fs/readdir.c`, `fs/stat.c`, `fs/statfs.c`, `fs/xattr.c`,
`fs/proc/base.c`, `fs/proc/task_mmu.c`) and adds two new files
(`fs/nomount.c`, `include/linux/nomount.h`).

## Apply

From the kernel source root:

```sh
patch -p1 < nomount-android16-6.12.patch      # pick the matching kernel version
```

Then enable the config (NOT bundled in the patch):

```sh
echo 'CONFIG_NOMOUNT=y' >> arch/arm64/configs/gki_defconfig
# or add it to your build's defconfig.fragment
```

The driver registers `/dev/nomount` (root-only, 0600) and `/sys/kernel/nomount/`,
and defaults **disabled** until userspace enables it via ioctl.

## Recursion guard

The per-task recursion guard uses `current->journal_info` — **never**
`current->android_oem_data1`. On Oplus/OnePlus GKI kernels that field holds the
`oplus_task_struct` pointer, so a `set_bit()` on it corrupts the scheduler's
per-task pointer and soft-locks the device during boot. `journal_info` is safe
across vendors and is reliably NULL during path resolution (the only hook
context here).

## Relationship to SUSFS

The patches in **this directory are standalone** (raw GKI, no SUSFS). If your
kernel also has SUSFS, do **not** mix them — use the [`susfs/`](susfs/) variant,
which is re-anchored to mainline `susfs4ksu` so `apply SUSFS → apply NoMount`
is conflict-free. Both edit overlapping functions (`getdents*`,
`inode_permission`, `d_path`, `vfs_statx`, …), which is exactly why the two
variants exist.

## Verification status — read before flashing

- ✅ Each patch applies to a fresh GKI tree with **zero fuzz / zero offset**;
  `#ifdef/#endif` balanced.
- ✅ **android16-6.12 is boot-verified** end-to-end on a OnePlus 15
  (SukiSU-Ultra): builds, boots, `/dev/nomount` present, `CONFIG_NOMOUNT=y`,
  modules load and RRO overlays enable.
- 🧩 The other four versions apply cleanly but are **not device-tested** — build
  on a throwaway boot image first.

Sources: raw GKI from `android.googlesource.com/kernel/common`.
