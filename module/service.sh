#!/system/bin/sh

MODDIR=${0%/*}
NM_BIN="$MODDIR/bin/nm"
EXCLUSION_FILE="/data/adb/nomount/.exclusion_list"
TEMP_FILE="$EXCLUSION_FILE.tmp"

[ -x "$NM_BIN" ] || exit 0
[ -f "$EXCLUSION_FILE" ] || exit 0

# b16cafe wrote multiple UIDs as one comma-separated value. Normalize old files
# before replaying them so upgrades recover existing exclusions automatically.
set -f
seen=""
{
    for uid in $(tr ',\r' '  ' < "$EXCLUSION_FILE"); do
        case "$uid" in
            ''|*[!0-9]*) continue ;;
        esac
        case " $seen " in
            *" $uid "*) continue ;;
        esac
        seen="$seen $uid"
        printf '%s\n' "$uid"
    done
} > "$TEMP_FILE" || exit 0

mv -f "$TEMP_FILE" "$EXCLUSION_FILE" || exit 0

while IFS= read -r uid; do
    [ -z "$uid" ] && continue
    "$NM_BIN" block "$uid"
done < "$EXCLUSION_FILE"
