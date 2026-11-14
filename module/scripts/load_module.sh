#!/system/bin/sh
#
# load_module.sh - inject a NoMount module's rules
# Usage: load_module.sh <mod_path> <nm_bin> <target_partitions> <script_dir>
#
# Emits "nm_phase_*" timing lines and a final "nm_load_status=N" on stderr.
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
[ -z "$valid_dirs" ] && { echo "nm_load_status=0" >&2; exit 0; }

status=0
t0=$(_nm_now)

# Phase 1: whiteout rules from .remove markers
find -L $valid_dirs -name '.remove' -print 2>/dev/null \
    | awk -v mod="$MOD_PATH" -f "$SCRIPT_DIR/remove_whiteout.awk" \
    | tr '\n' '\0' \
    | busybox xargs -0 -r -n 200 "$NM_BIN" rule add --whiteout
st=$?; [ "$st" -ne 0 ] && status=$st
t1=$(_nm_now)

# Phase 2+3: whiteout / opaque rules from .replace markers
#   .replace in empty parent     -> whiteout parent
#   .replace in non-empty parent -> opaque parent -> module's dir

# NOTE: trusted.overlay.opaque=y auto-detection is dropped in favor of
# .replace markers. If a module really needs it, we can add a batched getfattr
# pass here (~125ms/module even when unused, so only add if needed).
find -L $valid_dirs -name '.replace' -print0 2>/dev/null \
    | busybox xargs -0 -r -n 100 sh -c '
        mod="$1"; shift
        for f do
            p="${f%/.replace}"
            if [ -z "$(find "$p" -mindepth 1 ! -name ".replace" ! -name ".remove" -print -quit 2>/dev/null)" ]; then
                v="${p#$mod/}"
                case "$v" in
                    system/*)
                        rest="${v#system/}"; pp="${rest%%/*}"
                        case "$pp" in
                            vendor|system_ext|product|odm|apex|oem|optics|prism|mi_ext|my_*)
                                v="$pp${v#system/$pp}" ;;
                        esac ;;
                esac
                printf "/%s\0" "$v"
            fi
        done
    ' _ "$MOD_PATH" \
    | busybox xargs -0 -r -n 200 "$NM_BIN" rule add --whiteout
st=$?; [ "$st" -ne 0 ] && status=$st

find -L $valid_dirs -name '.replace' -print0 2>/dev/null \
    | busybox xargs -0 -r -n 100 sh -c '
        mod="$1"; shift
        for f do
            p="${f%/.replace}"
            if [ -n "$(find "$p" -mindepth 1 ! -name ".replace" ! -name ".remove" -print -quit 2>/dev/null)" ]; then
                v="${p#$mod/}"
                case "$v" in
                    system/*)
                        rest="${v#system/}"; pp="${rest%%/*}"
                        case "$pp" in
                            vendor|system_ext|product|odm|apex|oem|optics|prism|mi_ext|my_*)
                                v="$pp${v#system/$pp}" ;;
                        esac ;;
                esac
                printf "/%s\0%s\0" "$v" "$p"
            fi
        done
    ' _ "$MOD_PATH" \
    | busybox xargs -0 -r -n 200 "$NM_BIN" rule add --opaque
st=$?; [ "$st" -ne 0 ] && status=$st
t2=$(_nm_now)

# Phase 4: pair rules (regular files and symlinks)
find -L $valid_dirs '(' -type f -o -type l ')' ! -name '.replace' ! -name '.remove' -print 2>/dev/null \
    | awk -v mod="$MOD_PATH" -f "$SCRIPT_DIR/pair.awk" \
    | tr '\n' '\0' \
    | busybox xargs -0 -r -n 200 "$NM_BIN" rule add
st=$?; [ "$st" -ne 0 ] && status=$st
t3=$(_nm_now)

echo "nm_phase_whiteout=$(( t1 - t0 ))ms" >&2
echo "nm_phase_replace=$(( t2 - t1 ))ms" >&2
echo "nm_phase_pair=$(( t3 - t2 ))ms" >&2
echo "nm_load_status=$status" >&2
exit "$status"
