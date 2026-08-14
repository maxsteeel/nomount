#!/system/bin/sh

MODDIR=${0%/*}
LOADER="$MODDIR/bin/nm"
KO_LOADER="$MODDIR/loader"
USE_KSUD=false
MODULES_DIR="/data/adb/modules"
NOMOUNT_DATA="/data/adb/nomount"
LOG_FILE="$NOMOUNT_DATA/nomount.log"
VERBOSE_FLAG="$NOMOUNT_DATA/.verbose"
BOOT_SEMAPHORE="$NOMOUNT_DATA/.booting"
TARGET_PARTITIONS="system vendor product system_ext odm oem"
PROP_FILE="$MODDIR/module.prop"
BASE_DESC="A metamodule that replaces OverlayFS/MagicMount with VFS path redirection."

if command -v ksud >/dev/null 2>&1 && \
   ksud -h 2>&1 | grep -qE '(^|[[:space:]])insmod([[:space:]]|$)'; then
    USE_KSUD=true
fi

load_ko() {
    ko_name=${1##*/}

    if [ "$USE_KSUD" = true ]; then
        if ksud insmod "$1" && "$LOADER" version > /dev/null 2>&1; then
            return 0
        fi
        echo "[WARN] ksud insmod failed; falling back to KoLoader." >> "$LOG_FILE"
        rmmod nomount 2>/dev/null
        USE_KSUD=false
    fi

    (
        cd "$MODDIR/lkm" || exit 1
        "$KO_LOADER" "$ko_name"
    )
}

if [ ! -d "$NOMOUNT_DATA" ]; then
    mkdir -p "$NOMOUNT_DATA"
fi

echo "=== NoMount Boot Log | Started: $(date) ===" > "$LOG_FILE"
echo "Kernel Version: $(uname -r)" >> "$LOG_FILE"

if [ -f "$BOOT_SEMAPHORE" ]; then
    echo "[FATAL] Bootloop detected! NoMount caused a crash on the last boot." >> "$LOG_FILE"
    echo "[INFO] Disabling NoMount for safety..." >> "$LOG_FILE"
    touch "$MODDIR/disable"
    sed -i "s|^description=.*|description=[🚨 DISABLED: Bootloop Prevented] \\\\n$BASE_DESC|" "$PROP_FILE"
    rm -f "$BOOT_SEMAPHORE"
    exit 1
fi

touch "$BOOT_SEMAPHORE"

echo "[INFO] Checking NoMount kernel support..." >> "$LOG_FILE"
if "$LOADER" version > /dev/null 2>&1; then
    echo "[INFO] Built-in Kernel support detected." >> "$LOG_FILE"
else
    echo "[INFO] Built-in not found. Attempting to load LKM..." >> "$LOG_FILE"
    if [ -f "$MODDIR/lkm/nomount.ko" ]; then
        load_ko "$MODDIR/lkm/nomount.ko" >> "$LOG_FILE" 2>&1
    fi

    if ! "$LOADER" version > /dev/null 2>&1; then
        echo "[FATAL] NoMount Internal API is missing/unresponsive." >> "$LOG_FILE"
        touch "$MODDIR/disable"
        sed -i "s|^description=.*|description=[❌ ERROR: Kernel not patched or module failed to load] \\\\n$BASE_DESC|" "$PROP_FILE"
        rm -f "$BOOT_SEMAPHORE"
        exit 1
    fi
    echo "[INFO] LKM loaded and initialized correctly." >> "$LOG_FILE"
fi
echo "[OK] Internal API responding properly." >> "$LOG_FILE"

VERBOSE=false
if [ -f "$VERBOSE_FLAG" ]; then
    VERBOSE=true
    echo "[CONFIG] Verbose Mode: ON" >> "$LOG_FILE"
else
    echo "[CONFIG] Verbose Mode: OFF" >> "$LOG_FILE"
fi

for mod_path in "$MODULES_DIR"/*; do
    [ -d "$mod_path" ] || continue
    mod_name="${mod_path##*/}"
    [ "$mod_name" = "nomount" ] && continue

    if [ -f "$mod_path/disable" ] || [ -f "$mod_path/remove" ] || [ -f "$mod_path/skip_mount" ]; then
        if $VERBOSE; then echo "[SKIP] Module $mod_name is disabled/removed/skipped" >> "$LOG_FILE"; fi
        continue
    fi

    for partition in $TARGET_PARTITIONS; do
        if [ -d "$mod_path/$partition" ]; then
            echo "[INFO] Mounting module: $mod_name (/$partition)" >> "$LOG_FILE"
            (
                cd "$mod_path" || exit
                if $VERBOSE; then
                    find -L "$partition" \( -type f -o -type l -o -type c \) 2>/dev/null | while read -r relative_path; do
                        real_path="$mod_path/$relative_path"
                        v_path="$relative_path"

                        if [ "${v_path#system/odm/}" != "$v_path" ]; then
                            v_path="odm/${v_path#system/odm/}"
                        fi
                        virtual_path="/$v_path"

                        if [ "${relative_path##*/}" = ".replace" ]; then
                            target_dir="/${v_path%/.replace}"
                            echo "  -> Whiteout: $target_dir" >> "$LOG_FILE"
                            "$LOADER" rule add --whiteout "$target_dir" 2>> "$LOG_FILE"
                            continue
                        fi

                        if [ -c "$real_path" ]; then
                            echo "  -> Whiteout: $virtual_path" >> "$LOG_FILE"
                            "$LOADER" rule add --whiteout "$virtual_path" 2>> "$LOG_FILE"
                            continue
                        fi

                        echo "  -> Inject: $virtual_path" >> "$LOG_FILE"
                        "$LOADER" rule add "$virtual_path" "$real_path" 2>> "$LOG_FILE"
                    done
                else
                    find -L "$partition" \( -type c -o -name ".replace" \) -exec sh -c '
                        for f do
                            v="$f"; [ "${v#system/odm/}" != "$v" ] && v="odm/${v#system/odm/}"
                            if [ "${f##*/}" = ".replace" ]; then printf "/%s\0" "${v%/.replace}"
                            else printf "/%s\0" "$v"; fi
                        done
                    ' _ {} + 2>/dev/null | xargs -0 -r "$LOADER" rule add --whiteout >> "$LOG_FILE" 2>&1

                    find -L "$partition" \( -type f -o -type l \) ! -name ".replace" -exec sh -c '
                        mod="$1"; shift
                        for f do
                            v="$f"; [ "${v#system/odm/}" != "$v" ] && v="odm/${v#system/odm/}"
                            printf "/%s\0%s/%s\0" "$v" "$mod" "$f"
                        done
                    ' _ "$mod_path" {} + 2>/dev/null | xargs -0 -r "$LOADER" rule add >> "$LOG_FILE" 2>&1
                fi
            )
        fi
    done
done

echo "=== Injection Complete: $(date) ===" >> "$LOG_FILE"

rm -f "$BOOT_SEMAPHORE"
echo "[OK] Boot phase completed safely." >> "$LOG_FILE"
sed -i "s|^description=.*|description=$BASE_DESC|" "$PROP_FILE"

if $VERBOSE; then
    echo "Current files injected:" >> "$LOG_FILE"
    "$LOADER" rule list >> "$LOG_FILE"
fi

exit 0
