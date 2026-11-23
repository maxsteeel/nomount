#!/system/bin/sh

MODDIR=${0%/*}
. "$MODDIR/common.sh"

EXCLUSION_JSON="/data/adb/nomount/.exclusion_list.json"
ISOLATED_FLAG="/data/adb/nomount/.block_isolated_uids"
DISABLE_FLAG="/data/adb/nomount/disable"

[ -x "$LOADER" ] || exit 0
[ -f "$DISABLE_FLAG" ] && exit 0

if [ -f "$EXCLUSION_JSON" ]; then
    uids=$(grep -o '"uid":"[0-9]*"' "$EXCLUSION_JSON" | cut -d'"' -f4)
    if [ -n "$uids" ]; then
        set -f
        try "$LOADER" uid add $uids
    fi
fi

if [ -f "$ISOLATED_FLAG" ]; then
    try "$LOADER" uid block_isolated on
fi
