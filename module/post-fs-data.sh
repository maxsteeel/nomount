#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/common.sh"
[ -d "$NOMOUNT_DATA" ] || mkdir -p "$NOMOUNT_DATA"

if [ -f "$NOMOUNT_DATA/disable" ]; then
    klog "SKIPPED initialization, Safe mode actived"
    exit 0
fi

if ! bootloop_check; then
    exit 1
fi

if ! ensure_lkm_loaded; then
    disable_module "❌ Kernel not patched or module failed to load"
    exit 1
fi

klog "Kernel API ready (post-fs-data)"
exit 0
