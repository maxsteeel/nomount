#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/common.sh"
[ -d "$NOMOUNT_DATA" ] || mkdir -p "$NOMOUNT_DATA"

if [ -f "$NOMOUNT_DATA/disable" ]; then
    echo "[INFO] Safe Mode active. Skipping NoMount initialization." >> "$LOG_FILE"
    exit 0
fi

if ! bootloop_check; then
    exit 1
fi

if ! ensure_lkm_loaded; then
    disable_module "❌ ERROR: Kernel not patched or module failed to load"
    exit 1
fi

echo "[OK] Kernel API ready (post-fs-data)." >> "$LOG_FILE"

"$LOADER" reboot-watch </dev/null >/dev/null 2>&1 &

exit 0
