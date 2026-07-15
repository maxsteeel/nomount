//! Metamodule mount pass: scan /data/adb/modules and, for every enabled module
//! file, inject a VFS redirection rule via /dev/nomount — except RRO overlay
//! dirs (`**/overlay/*.apk`), which Android's idmap2/OMS pipeline needs on a
//! real mount; those are handled by [`crate::overlay`] instead.

use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};

use crate::overlay;
use crate::vfs::VfsDriver;

const MODULES_DIR: &str = "/data/adb/modules";

// Module IDs known to perform their own mounting/path redirection in
// post-fs-data or service scripts. Injecting their files through NoMount as
// well would double-handle the same targets and can fight their own mounts,
// so we skip them entirely. Users can extend this list at runtime via
// /data/adb/nomount/blocklist (one module id per line; `#` starts a comment).
const BUILTIN_BLOCKLIST: &[&str] = &["scene_swap_controller", "AAaTempSpoof"];

const BLOCKLIST_FILE: &str = "/data/adb/nomount/blocklist";

// On System-as-Root devices these live under /vendor, /product, etc. even
// though modules stage them under system/. Canonicalize so rules target the
// real inode instead of the /system/<x> symlink alias.
const SAR_ALIAS_PARTITIONS: &[&str] = &[
    "system/vendor",
    "system/product",
    "system/system_ext",
    "system/odm",
];

/// Resolve a module-relative path (e.g. "system/app/Foo.apk") to its absolute
/// on-device target ("/system/app/Foo.apk", or "/vendor/..." for SAR aliases).
fn resolve_target_path(relative: &Path) -> Option<PathBuf> {
    let s = relative.to_str()?;
    if s.is_empty() {
        return None;
    }
    for alias in SAR_ALIAS_PARTITIONS {
        let canonical = &alias["system/".len()..];
        if s == *alias {
            return Some(PathBuf::from(format!("/{canonical}")));
        }
        if let Some(rest) = s.strip_prefix(alias).and_then(|r| r.strip_prefix('/')) {
            return Some(PathBuf::from(format!("/{canonical}/{rest}")));
        }
    }
    Some(PathBuf::from(format!("/{s}")))
}

fn module_enabled(dir: &Path) -> bool {
    !dir.join("disable").exists()
        && !dir.join("remove").exists()
        && !dir.join("skip_mount").exists()
}

/// Built-in self-mounting IDs plus any listed in /data/adb/nomount/blocklist.
fn load_blocklist() -> HashSet<String> {
    let mut set: HashSet<String> =
        BUILTIN_BLOCKLIST.iter().map(|s| (*s).to_string()).collect();
    if let Ok(contents) = fs::read_to_string(BLOCKLIST_FILE) {
        for line in contents.lines() {
            let id = line.trim();
            if !id.is_empty() && !id.starts_with('#') {
                set.insert(id.to_string());
            }
        }
    }
    set
}

/// Recursively inject rules for a module subtree rooted at `dir`.
/// Files always get a redirect rule; directories only get one when they don't
/// already exist on the live filesystem (adding one for a stock dir would hide
/// its real contents). Symlinks are treated as files (file_type does not follow).
///
/// RRO overlay dirs are *not* injected: they're recorded in `overlays` (target
/// dir -> module source dir) for [`crate::overlay`] to real-mount, because a VFS
/// redirect would keep them in `STATE_NO_IDMAP` and also shadow the real mount.
fn inject_tree(
    driver: &VfsDriver,
    module_root: &Path,
    dir: &Path,
    applied: &mut u32,
    failed: &mut u32,
    overlays: &mut Vec<(PathBuf, PathBuf)>,
) {
    let entries = match fs::read_dir(dir) {
        Ok(e) => e,
        Err(_) => return,
    };
    for entry in entries.flatten() {
        let ft = match entry.file_type() {
            Ok(t) => t,
            Err(_) => continue,
        };
        let source = entry.path();
        let rel = match source.strip_prefix(module_root) {
            Ok(r) => r,
            Err(_) => continue,
        };
        let target = match resolve_target_path(rel) {
            Some(t) => t,
            None => continue,
        };

        if ft.is_dir() {
            if overlay::is_overlay_target(&target) {
                // Hand RRO overlays to the real-mount path; don't VFS-inject.
                overlays.push((target, source));
                continue;
            }
            if !target.exists() {
                match driver.add_rule(&target, &source, true) {
                    Ok(()) => *applied += 1,
                    Err(_) => *failed += 1,
                }
            }
            inject_tree(driver, module_root, &source, applied, failed, overlays);
        } else {
            match driver.add_rule(&target, &source, false) {
                Ok(()) => *applied += 1,
                Err(_) => *failed += 1,
            }
        }
    }
}

/// Metamodule entry point (`nomount mount`): rebuild rules from the current set
/// of enabled modules and enable the engine.
pub fn run_mount() -> Result<()> {
    let driver = VfsDriver::open()
        .context("cannot open /dev/nomount -- is the kernel module loaded?")?;

    // Start clean so uninstalled/updated modules don't leave stale rules.
    let _ = driver.clear_all();

    let blocklist = load_blocklist();
    let mut modules = 0u32;
    let mut skipped = 0u32;
    let mut applied = 0u32;
    let mut failed = 0u32;
    let mut overlays: Vec<(PathBuf, PathBuf)> = Vec::new();

    if let Ok(dirs) = fs::read_dir(MODULES_DIR) {
        for entry in dirs.flatten() {
            let mdir = entry.path();
            if !mdir.is_dir() || !module_enabled(&mdir) {
                continue;
            }
            // Leave self-mounting modules to manage their own redirection.
            if mdir
                .file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|id| blocklist.contains(id))
            {
                skipped += 1;
                continue;
            }
            let sysroot = mdir.join("system");
            if !sysroot.is_dir() {
                continue;
            }
            modules += 1;
            inject_tree(
                &driver,
                &mdir,
                &sysroot,
                &mut applied,
                &mut failed,
                &mut overlays,
            );
        }
    }

    driver.enable().context("failed to enable VFS engine")?;
    driver.refresh().context("failed to refresh dcache")?;

    // Real-mount RRO overlay dirs (grouped by target, since several modules may
    // contribute to the same partition overlay dir).
    let mut by_target: BTreeMap<PathBuf, Vec<PathBuf>> = BTreeMap::new();
    for (target, source) in overlays {
        by_target.entry(target).or_default().push(source);
    }
    let (ov_ok, ov_fail) = overlay::setup(&by_target);

    println!(
        "nomount: {modules} modules, {applied} rules applied, {failed} failed, \
         {skipped} skipped, {ov_ok} overlay mounts ({ov_fail} failed)"
    );
    Ok(())
}
