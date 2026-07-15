//! Hybrid overlay support for RRO (Runtime Resource Overlay) directories.
//!
//! Android's OverlayManager + idmap2 pipeline needs overlay APKs on a *real*
//! filesystem mount — VFS path redirection via /dev/nomount leaves them in
//! `STATE_NO_IDMAP` (never enabled). So for module `**/overlay/*.apk` dirs we
//! mount a real overlayfs instead of injecting VFS rules.
//!
//! Complication: module files live on /data (f2fs with the `casefold` feature),
//! which overlayfs refuses as a lowerdir ("case-insensitive capable filesystem
//! ... not supported"). We therefore stage the overlay APKs onto a tmpfs
//! (non-casefold), then mount `overlayfs[tmpfs-staged : <the partition's own
//! overlay lowerdirs>]` over the target dir, reproducing exactly how the stock
//! (working) overlays are presented.

use std::collections::BTreeMap;
use std::ffi::CString;
use std::fs;
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};

/// Partition overlay directories Android scans for RRO overlays. A module
/// subtree resolving to one of these is mounted here, not VFS-redirected.
const OVERLAY_TARGETS: &[&str] = &[
    "/system/overlay",
    "/product/overlay",
    "/vendor/overlay",
    "/system_ext/overlay",
    "/odm/overlay",
];

/// Private tmpfs work area: staged overlay APKs and partition-base binds. tmpfs
/// is non-casefold, so overlayfs accepts it as a lowerdir.
const WORK: &str = "/mnt/nomount";
const SECONTEXT: &str = "u:object_r:system_file:s0";
const OVERLAY_OPTS: &str = "redirect_dir=nofollow,userxattr";

/// True if `target` is a partition overlay dir that must be real-mounted.
pub fn is_overlay_target(target: &Path) -> bool {
    target.to_str().is_some_and(|s| OVERLAY_TARGETS.contains(&s))
}

fn cstr(s: &str) -> io::Result<CString> {
    CString::new(s).map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "nul in string"))
}

fn mount(
    src: &str,
    tgt: &str,
    fstype: &str,
    flags: libc::c_ulong,
    data: Option<&str>,
) -> io::Result<()> {
    let src = cstr(src)?;
    let tgt = cstr(tgt)?;
    let fst = cstr(fstype)?;
    let data = match data {
        Some(d) => Some(cstr(d)?),
        None => None,
    };
    let dptr = data
        .as_ref()
        .map_or(std::ptr::null(), |d| d.as_ptr() as *const libc::c_void);
    let r = unsafe { libc::mount(src.as_ptr(), tgt.as_ptr(), fst.as_ptr(), flags, dptr) };
    if r == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

fn umount_detach(tgt: &str) {
    if let Ok(t) = cstr(tgt) {
        unsafe {
            libc::umount2(t.as_ptr(), libc::MNT_DETACH);
        }
    }
}

/// Relabel a path's SELinux context (idmap2 must be able to read it as a
/// system_file, like the stock overlays it sits beside).
fn set_secontext(path: &Path, ctx: &str) -> io::Result<()> {
    let p = CString::new(path.as_os_str().as_bytes())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "nul in path"))?;
    let name = cstr("security.selinux")?;
    let mut val = ctx.as_bytes().to_vec();
    val.push(0);
    let r = unsafe {
        libc::setxattr(
            p.as_ptr(),
            name.as_ptr(),
            val.as_ptr() as *const libc::c_void,
            val.len(),
            0,
        )
    };
    if r == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

/// "/product/overlay" -> "product"; "" if malformed.
fn partition_name(target: &str) -> &str {
    target
        .trim_start_matches('/')
        .strip_suffix("/overlay")
        .unwrap_or("")
}

/// lowerdir= list of an overlayfs already mounted at `target` (OnePlus stacks
/// its regional my_* overlay dirs there). None if `target` isn't an overlay.
fn stock_lowerdirs(target: &str) -> Option<String> {
    let mounts = fs::read_to_string("/proc/self/mounts").ok()?;
    let mut result = None;
    for line in mounts.lines() {
        let mut f = line.split(' ');
        let _src = f.next();
        let mp = f.next();
        let fstype = f.next();
        let opts = f.next();
        if mp == Some(target) && fstype == Some("overlay") {
            if let Some(opts) = opts {
                for opt in opts.split(',') {
                    if let Some(l) = opt.strip_prefix("lowerdir=") {
                        result = Some(l.to_string());
                    }
                }
            }
        }
    }
    result
}

/// True if our own nomount overlay is currently the mount at `target`.
fn nomount_mounted_at(target: &str) -> bool {
    let Ok(mounts) = fs::read_to_string("/proc/self/mounts") else {
        return false;
    };
    mounts.lines().any(|line| {
        let mut f = line.split(' ');
        f.next() == Some("nomount_ov") && f.next() == Some(target)
    })
}

/// Copy a module overlay dir's APKs onto the tmpfs stage, labelled system_file.
fn copy_apks(src: &Path, stage: &Path) -> io::Result<()> {
    fs::create_dir_all(stage)?;
    let _ = set_secontext(stage, SECONTEXT);
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        // Overlay dirs are flat piles of .apk; skip anything that isn't a file.
        if !entry.file_type()?.is_file() {
            continue;
        }
        let dst = stage.join(entry.file_name());
        fs::copy(entry.path(), &dst)?;
        let _ = set_secontext(&dst, SECONTEXT);
    }
    Ok(())
}

/// Mount a real overlayfs for every overlay target (target dir -> the module
/// overlay source dirs contributing APKs to it). Returns (mounted, failed).
///
/// Best-effort: individual failures are counted, never fatal, so the rest of
/// the NoMount mount pass still completes.
pub fn setup(targets: &BTreeMap<PathBuf, Vec<PathBuf>>) -> (u32, u32) {
    if targets.is_empty() {
        return (0, 0);
    }

    // Idempotent: drop any previous nomount overlays and the old work tmpfs.
    for target in targets.keys() {
        if let Some(t) = target.to_str() {
            while nomount_mounted_at(t) {
                umount_detach(t);
            }
        }
    }
    umount_detach(WORK);

    if fs::create_dir_all(WORK).is_err()
        || mount("nomount_work", WORK, "tmpfs", 0, Some("mode=0755")).is_err()
    {
        return (0, targets.len() as u32);
    }

    let mut ok = 0u32;
    let mut fail = 0u32;

    for (target, sources) in targets {
        let Some(target_s) = target.to_str() else {
            fail += 1;
            continue;
        };
        let part = partition_name(target_s);
        if part.is_empty() {
            fail += 1;
            continue;
        }

        // 1. Stage all contributing overlay APKs onto tmpfs.
        let stage = PathBuf::from(format!("{WORK}/stage/{part}"));
        let mut staged = false;
        for src in sources {
            if copy_apks(src, &stage).is_ok() {
                staged = true;
            }
        }
        if !staged {
            fail += 1;
            continue;
        }
        let stage_s = stage.to_string_lossy();

        // 2. Build the lowerdir list: staged APKs on top, then either the
        //    partition's existing overlay stack (with its self-referential base
        //    entry re-pointed at a bind of the real partition, since the base is
        //    shadowed by the stock overlay), or just the target dir itself.
        let lower = match stock_lowerdirs(target_s) {
            Some(stock) => {
                let base_bind = format!("{WORK}/base/{part}");
                let partition = format!("/{part}");
                let _ = fs::create_dir_all(&base_bind);
                let base_ok =
                    mount(&partition, &base_bind, "", libc::MS_BIND, None).is_ok();
                let mut parts = vec![stage_s.into_owned()];
                for elem in stock.split(':') {
                    if elem == target_s && base_ok {
                        parts.push(format!("{base_bind}/overlay"));
                    } else {
                        parts.push(elem.to_string());
                    }
                }
                parts.join(":")
            }
            None => format!("{stage_s}:{target_s}"),
        };

        // 3. Mount the real overlayfs over the target.
        let data = format!("lowerdir={lower},{OVERLAY_OPTS}");
        match mount("nomount_ov", target_s, "overlay", 0, Some(&data)) {
            Ok(()) => ok += 1,
            Err(_) => fail += 1,
        }
    }

    (ok, fail)
}
