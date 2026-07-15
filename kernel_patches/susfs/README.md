# NoMount — SUSFS-compatible variant (mainline susfs4ksu)

The same NoMount VFS driver as the parent directory, but re-anchored to a
**mainline susfs4ksu** base so `apply SUSFS → apply NoMount` is conflict-free on
a SUSFS kernel. Use these when you want **NoMount *and* SUSFS in the same
kernel**.

| You want | Use |
|----------|-----|
| NoMount only | `../nomount-android*.patch` (parent — raw GKI, standalone) |
| NoMount **+ SUSFS** | these (`susfs/nomount-android*.patch`) |

The two sets are **not interchangeable** — the parent (raw-GKI) patches conflict
if SUSFS is also applied, and these conflict on a tree without SUSFS.

## Base

Regenerated against **raw GKI + mainline susfs4ksu**
(`gitlab.com/simonpunk/susfs4ksu`, `gki-android<ver>` branches). Each patch's
context reflects mainline susfs (e.g. `susfs_sus_kstat_spoof_show_map_vma`,
`CONFIG_KSU_SUSFS_SUS_MAP`).

## Apply order (SUSFS first)

```sh
# 1. apply mainline susfs4ksu to your kernel source first
patch -p1 < 50_add_susfs_in_gki-android16-6.12.patch
# 2. then apply the matching NoMount patch
patch -p1 < nomount-android16-6.12.patch
```

Enable both (neither is bundled):

```
CONFIG_NOMOUNT=y
CONFIG_KSU_SUSFS=y
```

## Runtime

By design NoMount cooperates with SUSFS — its hooks are guarded by
`#ifdef CONFIG_KSU_SUSFS` and defer to `susfs_is_current_proc_umounted()`, so
mountless VFS injection and SUSFS hiding run together without fighting. The
recursion guard uses `current->journal_info` (never `android_oem_data1`; see the
parent [`README.md`](../README.md#recursion-guard)).

## Caveats

- **Aligned to susfs4ksu HEAD + google-common GKI HEAD.** If your kernel source
  or susfs4ksu revision differs meaningfully, expect a little fuzz or a stray
  `.rej` to reconcile by hand — inherent to pre-baking against a moving base.
  Verified: applies with **zero fuzz/offset** to that reference base.
- The ioctl magic (`0x5A`) and wire ABI match the `nomount` userspace binary in
  this repo.
- **android16-6.12** is boot-verified on a OnePlus 15 (SukiSU-Ultra + SUSFS);
  the other four versions apply cleanly but are **not device-tested** — build on
  a throwaway boot image first.
