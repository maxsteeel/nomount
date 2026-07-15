#!/system/bin/sh
# NoMount metamodule mount hook (KSU/APatch, post-fs-data).
# Injects enabled modules' files via /dev/nomount before Zygote, guarded by a
# bootloop counter, then signals mounts are ready.
MODDIR="${0%/*}"
NMDIR=/data/adb/nomount
mkdir -p "$NMDIR"

LOCK="/dev/nomount_metamount.lock"
( set -o noclobber; : > "$LOCK" ) 2>/dev/null || { ksud kernel notify-module-mounted 2>/dev/null; exit 0; }

ABI=$(getprop ro.product.cpu.abi)
BIN="$MODDIR/bin/$ABI/nomount"

# --- bootloop guard ---
GUARD_MAX=3
COUNT=$(cat "$NMDIR/bootcount" 2>/dev/null || echo 0)
COUNT=$((COUNT + 1))
echo "$COUNT" > "$NMDIR/bootcount"

if [ -f "$NMDIR/disabled" ]; then
    echo "nomount: disabled, skipping mount" > /dev/kmsg 2>/dev/null
elif [ "$COUNT" -ge "$GUARD_MAX" ]; then
    echo "nomount: bootloop guard tripped (count=$COUNT) -> self-disabling" > /dev/kmsg 2>/dev/null
    : > "$NMDIR/disabled"
elif [ -x "$BIN" ]; then
    timeout 60 "$BIN" mount 2>/dev/null
fi

# --- hide overlay mounts from detection ---
# The RRO overlay pass creates real overlayfs/tmpfs mounts (nomount_ov,
# nomount_work). Hide them from mount detectors the same way magic_mount_rs does,
# via KSU's native per-app umount:
#   1. ensure the kernel_umount feature is ON (the master switch -- without it
#      KSU ignores the umount list entirely);
#   2. register our mount paths so KSU MNT_DETACHes them inside the mount
#      namespace of apps configured for umounting (DenyList).
# Idempotent (ksud dedups); a no-op when nothing is mounted or ksud lacks these.
if command -v ksud >/dev/null 2>&1; then
    # enable + PERSIST the master umount switch (set alone is runtime-only and
    # is lost on reboot; save writes it to .feature_config so it loads enabled).
    ksud feature set kernel_umount 1 >/dev/null 2>&1 && ksud feature save >/dev/null 2>&1
    # hide the /dev/nomount driver node from non-root scanners (root still opens it)
    [ -x /data/adb/ksu/bin/ksu_susfs ] && \
        /data/adb/ksu/bin/ksu_susfs add_sus_path /dev/nomount >/dev/null 2>&1
    awk '$1=="nomount_ov" || $1=="nomount_work" || $2 ~ /^\/mnt\/nomount/ {print $2}' \
        /proc/self/mounts 2>/dev/null | while read -r mp; do
        [ -n "$mp" ] && ksud umount-config add "$mp" --flags 2 >/dev/null 2>&1
    done

    # --- tag managed modules in the manager with how NoMount serves them ---
    # vfs = mountless /dev/nomount redirection; overlay = real overlayfs for RRO.
    # --temp overrides clear on reboot/uninstall, re-set each boot (non-destructive).
    _vf=""; _ov=""
    for d in /data/adb/modules/*/; do
        [ -d "$d" ] || continue
        mid=$(basename "$d")
        [ "$mid" = "meta-nomount" ] && continue
        { [ -f "$d/disable" ] || [ -f "$d/remove" ] || [ -f "$d/skip_mount" ] || [ ! -d "$d/system" ]; } && continue
        _o=0; _v=0
        [ -n "$(find "$d/system" -path '*/overlay/*.apk' -print -quit 2>/dev/null)" ] && _o=1
        [ -n "$(find "$d/system" -type f ! -path '*/overlay/*' -print -quit 2>/dev/null)" ] && _v=1
        [ "$_o" = 0 ] && [ "$_v" = 0 ] && continue
        if [ "$_o" = 1 ] && [ "$_v" = 1 ]; then _t="vfs + overlay"; _ov="$_ov $mid";
        elif [ "$_o" = 1 ]; then _t="overlay"; _ov="$_ov $mid";
        else _t="vfs"; _vf="$_vf $mid"; fi
        _orig=$(sed -n 's/^description=//p' "$d/module.prop" | head -1)
        KSU_MODULE="$mid" ksud module config set --temp override.description "[NoMount - $_t] $_orig" >/dev/null 2>&1
    done
    KSU_MODULE=meta-nomount ksud module config set --temp override.description \
        "NoMount metamodule - mountless VFS + RRO overlays. Serving -> vfs:$_vf | overlay:$_ov  GHOST" >/dev/null 2>&1
fi

ksud kernel notify-module-mounted 2>/dev/null
exit 0
