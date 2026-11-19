#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/common.sh"

MODULES_DIR="/data/adb/modules"

[ -d "$NOMOUNT_DATA" ] || mkdir -p "$NOMOUNT_DATA"

if [ -f "$NOMOUNT_DATA/disable" ]; then
    exit 0
fi

if ! bootloop_check; then
    exit 1
fi

if ! ensure_lkm_loaded; then
    disable_module "❌ Kernel not patched or module failed to load"
    exit 1
fi

if ! _lock_acquire "$NOMOUNT_DATA/.inject.flock" 120; then
    klogerr "failed to acquire injection lock"
    exit 1
fi
trap 'exit 1' INT TERM

klog "Injection started"
set -o pipefail
status=0

for mod_path in "$MODULES_DIR"/*; do
    [ -d "$mod_path" ] || continue
    mod_name="${mod_path##*/}"
    [ "$mod_name" = "nomount" ] && continue

    if [ -f "$mod_path/disable" ] || [ -f "$mod_path/remove" ] || [ -f "$mod_path/skip_mount" ]; then
        klog "SKIPPED module $mod_name, it is disabled/removed/skipped"
        continue
    fi

    klog "loading module $mod_name"
    "$MODDIR/bin/nm" module load "$mod_path" || status=1
done

if [ "$status" -eq 0 ]; then
    klog "Injection completed"
    sed -i "s|^description=.*|description=$BASE_DESC|" "$PROP_FILE"
else
    klogerr "Injection completed with errors."
fi

exit "$status"
