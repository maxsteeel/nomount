#!/system/bin/sh

MODDIR=${0%/*}
. "$MODDIR/common.sh"

NM_BIN="$MODDIR/bin/nm"
EXCLUSION_JSON="/data/adb/nomount/.exclusion_list.json"
ISOLATED_FLAG="/data/adb/nomount/.block_isolated_uids"
DISABLE_FLAG="/data/adb/nomount/disable"

[ -x "$NM_BIN" ] || exit 0
[ -f "$DISABLE_FLAG" ] && exit 0

if [ -f "$EXCLUSION_JSON" ]; then
    uids=$(grep -o '"uid":"[0-9]*"' "$EXCLUSION_JSON" | cut -d'"' -f4)
    if [ -n "$uids" ]; then
        set -f
        "$NM_BIN" uid add $uids >/dev/null 2>&1 || klogerr "failed to apply excluded UIDs"
    fi
fi

if [ -f "$ISOLATED_FLAG" ]; then
    "$NM_BIN" uid block_isolated on >/dev/null 2>&1 || klogerr "failed to enable isolated UID blocking"
fi
