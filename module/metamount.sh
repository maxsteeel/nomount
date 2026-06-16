#!/system/bin/sh

MODDIR=${0%/*}
LOADER="$MODDIR/bin/nm"
MODULES_DIR="/data/adb/modules"
NOMOUNT_DATA="/data/adb/nomount"
LOG_FILE="$NOMOUNT_DATA/nomount.log"
VERBOSE_FLAG="$NOMOUNT_DATA/.verbose"
BOOT_SEMAPHORE="$NOMOUNT_DATA/.booting"
TARGET_PARTITIONS="system vendor product system_ext odm oem"
PROP_FILE="$MODDIR/module.prop"
BASE_DESC="A metamodule that replaces OverlayFS/MagicMount with VFS path redirection."

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

if ! "$LOADER" v > /dev/null 2>&1; then
    echo "[FATAL] NoMount Netlink interface missing/unresponsive." >> "$LOG_FILE"
    touch "$MODDIR/disable"
    sed -i "s|^description=.*|description=[❌ ERROR: Kernel not patched] \\\\n$BASE_DESC|" "$PROP_FILE"
    rm -f "$BOOT_SEMAPHORE"
    exit 1
fi
echo "[OK] Netlink socket responding properly." >> "$LOG_FILE"

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
                    find -L "$partition" \( -type f -o -type l \) 2>/dev/null | while read -r relative_path; do
                        real_path="$mod_path/$relative_path"
                        virtual_path="/$relative_path"
                        echo "  -> Inject: $virtual_path" >> "$LOG_FILE"
                        "$LOADER" add "$virtual_path" "$real_path" 2>> "$LOG_FILE"

                        case "$relative_path" in
                            vendor/* | product/* | system_ext/* | odm/* | oem/*)
                                if [ ! -e "$mod_path/system/$relative_path" ] && [ ! -L "$mod_path/system/$relative_path" ]; then
                                    echo "  -> Inject (Alias): /system/$relative_path" >> "$LOG_FILE"
                                    "$LOADER" add "/system/$relative_path" "$real_path" 2>> "$LOG_FILE"
                                fi
                                ;;
                            system/vendor/* | system/product/* | system/system_ext/* | system/odm/* | system/oem/*)
                                alias_path="/${relative_path#system/}"
                                if [ ! -e "$mod_path$alias_path" ] && [ ! -L "$mod_path$alias_path" ]; then
                                    echo "  -> Inject (Alias): $alias_path" >> "$LOG_FILE"
                                    "$LOADER" add "$alias_path" "$real_path" 2>> "$LOG_FILE"
                                fi
                                ;;
                        esac
                    done
                else
                    find -L "$partition" \( -type f -o -type l \) -exec sh -c '
                        mod="$1"; shift
                        for f do
                            printf "/%s\0%s/%s\0" "$f" "$mod" "$f"
                            case "$f" in
                                vendor/*|product/*|system_ext/*|odm/*|oem/*)
                                    if [ ! -e "$mod/system/$f" ] && [ ! -L "$mod/system/$f" ]; then
                                        printf "/system/%s\0%s/%s\0" "$f" "$mod" "$f"
                                    fi
                                    ;;
                                system/vendor/*|system/product/*|system/system_ext/*|system/odm/*|system/oem/*)
                                    if [ ! -e "$mod/${f#system/}" ] && [ ! -L "$mod/${f#system/}" ]; then
                                        printf "/%s\0%s/%s\0" "${f#system/}" "$mod" "$f"
                                    fi
                                    ;;
                            esac
                        done
                    ' _ "$mod_path" {} + 2>/dev/null | xargs -0 -r -n 500 "$LOADER" add >> "$LOG_FILE" 2>&1
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
    "$LOADER" list >> "$LOG_FILE"
fi

exit 0
