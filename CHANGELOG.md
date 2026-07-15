# Changelog

## Unreleased

### Features
- Hybrid RRO overlay support — module `**/overlay/*.apk` dirs are now mounted as
  a real `overlayfs` instead of VFS-redirected, so Android's idmap2/OverlayManager
  pipeline can enable them (VFS redirect left them in `STATE_NO_IDMAP`, breaking
  RRO-based theming like OxygenCustomizer). Overlay APKs are staged on tmpfs
  first because `/data` (f2fs casefold) is rejected by overlayfs as a lowerdir;
  the mount reproduces the partition's existing overlay stack with the module
  dir prepended. Everything else stays mountless VFS redirection.
- Self-mounting module blocklist — skip modules that manage their own path
  redirection (built-in: `scene_swap_controller`, `AAaTempSpoof`; extend at
  runtime via `/data/adb/nomount/blocklist`, one id per line).
- Install-time sha256 integrity check — `customize.sh` verifies every bundled
  file against a `nomount.sha256sums` manifest and aborts on mismatch, catching
  a corrupted download or tampered zip before the root binary runs.

## v2.0.216-dev

### Features

**Mount Engine**
- Replace BFS planner with SAR-aware partition-root resolver — one overlay per resolved partition, aligns mount IDs with partition boundaries to defeat stat-based detection
- Handle >50 overlay layers via staging pyramid — intermediate read-only overlays on `/dev` prevent kernel layer limit failures (64 on older kernels, 128 on 5.10+)
- Narrow-dir overlay deferral — dirs like `lib`, `lib64`, `fonts` force BFS descent; shallow files fall back to individual bind mounts, preventing overlay from masking GPU drivers and shared libs
- Add `mount.exclude_hosts_modules` to skip modules containing `/system/etc/hosts` (prevents global DNS breakage, default: true)
- Add `mount.module_blacklist` — CSV list of module IDs to exclude from scanning
- Add `mount.ext4_image_size_mb` — override auto-calculated sparse image size (0 = auto, min 64MB)
- Add `mount.restart_framework` — stop/start Android framework after mounting for modules that need it
- Pre-mount script execution — run other modules' `post-fs-data.sh` before mount pipeline in metamodule mode so dynamically created files are visible to the scanner
- Per-module overlay fallback to MagicMount — one bad overlay no longer takes all mounts down
- VFS executor pre-injects directory rules for missing ancestor paths before leaf file rules
- Reflink (FICLONE) copy for f2fs CoW acceleration during overlay staging

**Stealth & Anti-Detection**
- Migrate prop spoofing from persistent daemon to short-lived CLI calls via `resetprop-rs` — no persistent mmap handle visible to detection apps
- Stealth property writes — no serial counter bump, no futex wake, no global serial broadcast
- Count-preserving nuke operations for PIF-leaking and custom ROM properties with arena compaction
- Defer all BRENE path hiding to boot-completed, matching real BRENE timing — paths and mounts aren't stable at post-fs-data
- Bridge susfs4ksu multi-value fields: `hide_sus_mnts` 0/1/2 (off-after-boot), `emulate_vold_app_data` 0/1/2 (sus_path_loop), `skip_legit_mounts`, `hide_cusrom` levels 0-5, `sus_open_redirect.txt`, kstat JSON
- Remove verified boot hash (`ro.boot.vbmeta.digest`) spoofing — conflicts with other root modules handling the same property

**Module Management**
- Auto-skip modules that perform their own mounts — heuristic scan of `post-fs-data.sh` and `service.sh` for mount/bind commands prevents double-mount conflicts
- Hot install support for script-only modules via `MODULE_HOT_INSTALL_REQUEST="true"` — activates without reboot
- Module exclusions UI — checkbox list on Status tab with optimistic update and rollback
- Stop calling other modules' post-fs-data scripts from metamount — trust KSU to handle script orchestration, preventing double-execution of non-idempotent modules

**Safety & Recovery**
- Unify boot guard into single bootcount with self-disable — one failed boot disables nomount only, replacing the dual competing shell+Rust system
- Disable SystemUI monitor by default — prevents false-positive recovery lockouts
- Volume-key config preservation on reinstall — stash config on uninstall, 60s prompt to preserve or reset on upgrade
- Atomic `module.prop` writes via tmp+rename to prevent corruption on OOM kill or watchdog timeout
- Panic hook and mount-error handler surface crash context in KSU/APatch manager description field

**WebUI**
- Migrate shell execution layer from `kernelsu` to `kernelsu-alt` with proper stderr preservation
- Browser locale detection with device-hint fallback via `__ZM_LOCALE_CODE__` injected at boot
- Guard module whitelist — protect specific modules from bootloop recovery disabling
- Complete internationalization across 36 locales (391 keys each) — activity log, capabilities, engine options, scenario descriptions all translated
- Mode-positive status display — all 3 mount modes shown as equally functional; no false "Degraded" warnings for non-VFS users
- Warn when MagicMount selected without SUSFS — contextual hint nudging toward OverlayFS
- Module exclusions section — checkbox list to blacklist modules, `meta-nomount` locked to prevent self-exclusion
- Collapse Performance and Emoji toggles into collapsible "More" subgroup
- Bootstrap ADB toggles from live Android state on every init, not stale cache
- Drop Orange and Lime from accent color palette
- Polish UI component styles across all tabs

**Build & Deploy**
- `--deploy` and `--reboot` flags for `package.sh` — push ZIP to device, install via ksud/apd, and reboot in one command
- `--release`/`--debug` deploy profile selection

### Bug Fixes

- Fix vol-key timeout during install — `choose_config 0` blocked indefinitely over ADB since `getevent` has no stdin; now 60s timeout
- Fix metamount.sh using hardcoded filename instead of iterated `$_pfd` variable for post-fs-data script paths
- Harden input validation, error propagation, and boot script safety — 19 findings fixed across Rust core, WebUI, and shell scripts (path traversal rejection, shell interpolation sanitization, timeout restoration, stale artifact cleanup)
- Wire overlay executor to partition-root resolver — SAR alias paths (`system/vendor/*`) now correctly map to the vendor partition overlay
- Fix deploy script calling `su -c` which doesn't exist on KSU — call ksud/apd binaries directly
- Stage `resetprop-rs` binary and `prop_table.sh` in ZIP packaging pipeline
- Add missing `install_i18n.sh` and `install_func.sh` to SCRIPTS array — install was failing on device
- Fix SELinux context in overlay mode — copies context from real system path instead of module source on `/data`
- Clean up `skip_mount` flags from previous boots — prevents permanent module orphaning on Magisk
- Hijack sweep no longer treats KSU bind mounts as rogue when they fall under NoMount-managed paths

### Performance

- Eliminate all root shell I/O from WebUI init — `service.sh` pre-generates `zm-init.js` at boot containing the `webui-init` JSON blob, system accent color, and all 36 locale bundles; WebUI loads data synchronously at parse time, cutting 5+ shell round-trips (200-500ms each) on every open

### Refactors

- Remove unused `notify_module_mounted` trait method and all impls — notification handled entirely from shell
- Switch WebUI packaging from npm to pnpm to match lockfile

### CI/CD

- Full release pipeline — build all 4 ABIs (debug+release), build WebUI and axon from source, package ZIPs, extract changelog, publish GitHub release with both ZIPs attached
- Switch build triggers to manual dispatch only — no unnecessary CI on every push

### Documentation

- Rewrite README body — concise grouped features, orchestration pipeline overview, mount strategy comparison table, dual Telegram links (group + channel)

### Dependencies

- Bump resetprop-rs to v0.2.1 — corrupt data hardening, futex wake, per-process prop isolation, persistent property support, protobuf bounds checks
- Upgrade resetprop-rs to v0.4.0 — stealth writes, nuke operations, arena compaction
- Switch resetprop to remote git dependency (no local submodule checkout required)

## v2.0.161-dev

- Adopt resetprop-rs library for prop spoofing — eliminates subprocess forks per enforcement cycle, reads/writes property areas via direct mmap
- Add resetprop-rs as git submodule under `external/resetprop-rs`
- Add `resetprop-rs` CLI binary to module for shell-level hexpatch operations
- Remove `command -v resetprop` PATH guard from service.sh — prop-watch no longer depends on Magisk's resetprop binary
- Add explicit `[profile.dev]` to preserve full debug info and symbols in debug builds
- Add GitHub Actions build workflow with debug + release variants across all 4 Android ABIs
- Add Makefile for local build convenience
- Add `.gitignore`

## v2.0.160-dev

- Fix SAR partition promotion for bind-mounted partitions (product, system_ext, vendor, odm now visible in overlay and magic mount modes on devices where /system/product is a bind mount instead of a symlink)
- Fix kernel `auto_inject_parent` duplicate directory entries — skip registration when path already exists on real filesystem (patched across all KernelSU-Next, ReSukiSU, SukiSU-Ultra, WildKSU variants)
- Add VFS rule injection for novel directories and opaque-dir replacements (previously skipped, relying only on kernel-side readdir injection)
- Add hybrid overlay+VFS fallback — when overlay mounts fail on novel targets, VFS rules fill the gap
- Detect `.replace` sentinel files for opaque directory classification (previously only xattr-based detection)
- Add `module unload <id>` CLI command for hot-unloading modules without reboot
- Add `sync-description` CLI command for live KSU/APatch Manager description updates
- WebUI module unload now routes through CLI backend instead of manual VFS rule deletion
- Auto-resume normal guard operation after clean boot post-recovery

## v2.0.146-dev

- Detect and uninstall conflicting metamodules during installation

## v2.0.144-dev

- Change safe mode trigger from volume-down to volume-up + volume-down combo (fixes fastboot key conflict)
- Add persistent recovery lockout so guard recovery survives boot-completed cleanup
- Add `guard clear-lockout` CLI subcommand and WebUI lockout banner with clear button
- Add diagnostic kmsg logging when guard marker recording fails

## v2.0.143-dev

- Add i18n support for 36 languages in WebUI and daemon description strings

## v2.0.142-dev

- Initial release on nomount repository
