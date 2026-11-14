#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/common.sh"

MODULES_DIR="/data/adb/modules"
TARGET_PARTITIONS="system system_ext vendor odm product apex oem optics prism
                    mi_ext my_bigball my_carrier my_company my_engineering my_heytap
                    my_manifest my_preload my_product my_region my_reserve my_stock"

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

exec 6>"$NOMOUNT_DATA/.inject.lock"
if ! _lock_fd 6 120; then
    klogerr "inject lock timed out after 120s"
    exec 6>&-
    exit 1
fi

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
    sh "$MODDIR/scripts/load_module.sh" "$mod_path" "$MODDIR/bin/nm" "$TARGET_PARTITIONS" "$MODDIR/scripts" || status=1
done

if [ "$status" -eq 0 ]; then
    klog "Injection completed"
    sed -i "s|^description=.*|description=$BASE_DESC|" "$PROP_FILE"
else
    klogerr "Injection completed with errors."
fi

exec 6>&-
exit "$status"
