#!/system/bin/sh
MODDIR=${0%/*}
. "$MODDIR/common.sh"

rm -rf "$NOMOUNT_DATA/.boot.lock" \
       "$NOMOUNT_DATA/.lkm.lock" \
       "$NOMOUNT_DATA/.inject.lock"

if [ -f "$BOOT_SEMAPHORE" ]; then
    rm -f "$BOOT_SEMAPHORE"
    klog "boot completed safely."
fi

if [ -f "$DUMP_RULES_FLAG" ]; then
    dump_all_rules_logcat "on boot" || true
fi

exit 0
