#!/system/bin/sh

MODDIR=${0%/*}
. "$MODDIR/common.sh"

dump_all_rules_logcat "$*"
