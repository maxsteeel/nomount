#!/system/bin/sh
# Magisk fallback (no metamodule hook). KSU/APatch use metamount.sh instead.
[ -n "$KSU" ] && exit 0
[ -n "$APATCH" ] && exit 0
MODDIR="${0%/*}"
NMDIR=/data/adb/nomount
mkdir -p "$NMDIR"
ABI=$(getprop ro.product.cpu.abi)
BIN="$MODDIR/bin/$ABI/nomount"

GUARD_MAX=3
COUNT=$(cat "$NMDIR/bootcount" 2>/dev/null || echo 0)
COUNT=$((COUNT + 1))
echo "$COUNT" > "$NMDIR/bootcount"

if [ -f "$NMDIR/disabled" ]; then
    :
elif [ "$COUNT" -ge "$GUARD_MAX" ]; then
    echo "nomount: bootloop guard tripped -> self-disabling" > /dev/kmsg 2>/dev/null
    : > "$NMDIR/disabled"
elif [ -x "$BIN" ]; then
    "$BIN" mount 2>/dev/null
fi
exit 0
