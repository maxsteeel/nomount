#!/system/bin/sh
#
# unload_module.sh - remove a NoMount module's rules
# Usage: unload_module.sh <mod_path> <nm_bin> <target_partitions> <script_dir>
#

set -u

MOD_PATH="$1"
NM_BIN="$2"
TARGET_PARTITIONS="$3"
SCRIPT_DIR="$4"

_nm_now() {
    awk '{ printf "%d\n", $1 * 1000 }' /proc/uptime
}

cd "$MOD_PATH" || { echo "nm_load_status=1" >&2; exit 1; }

valid_dirs=""
for p in $TARGET_PARTITIONS; do
    [ -d "$p" ] && { [ -d "/$p" ] || [ -d "/system/$p" ]; } && valid_dirs="$valid_dirs $p"
done
[ -z "$valid_dirs" ] && { echo "nm_unload_status=0" >&2; exit 0; }

status=0
t0=$(_nm_now)

find -L $valid_dirs \( -type f -o -type l -o -type c -o -type d \) -print 2>/dev/null \
    | awk -v mod="$MOD_PATH" -f "$SCRIPT_DIR/del.awk" \
    | tr '\n' '\0' \
    | busybox xargs -0 -r -n 200 "$NM_BIN" rule del
st=$?; [ "$st" -ne 0 ] && status=$st
t1=$(_nm_now)

echo "nm_phase_del=$(( t1 - t0 ))ms" >&2
echo "nm_unload_status=$status" >&2
exit "$status"
