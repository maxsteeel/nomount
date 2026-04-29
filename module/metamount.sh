MODDIR=${0%/*}
LOADER="$MODDIR/bin/nm"
MODULES_DIR="/data/adb/modules"
NOMOUNT_DATA="/data/adb/nomount"
LOG_FILE="$NOMOUNT_DATA/nomount.log"
VERBOSE_FLAG="$NOMOUNT_DATA/.verbose"
TARGET_PARTITIONS="system vendor product system_ext odm oem"
ACTIVE_MODULES_COUNT=0

if [ ! -d "$NOMOUNT_DATA" ]; then
    mkdir -p "$NOMOUNT_DATA"
fi

echo "=== NoMount Boot Log | Started: $(date) ===" > "$LOG_FILE"
echo "Kernel Version: $(uname -r)" >> "$LOG_FILE"

VERBOSE=false
if [ -f "$VERBOSE_FLAG" ]; then
    VERBOSE=true
    echo "[CONFIG] Verbose Mode: ON (Detailed logging enabled)" >> "$LOG_FILE"
else
    echo "[CONFIG] Verbose Mode: OFF (Summary only)" >> "$LOG_FILE"
fi

if [ ! -e "/dev/nomount" ]; then
    echo "[FATAL] /dev/nomount missing. Is the kernel patched?" >> "$LOG_FILE"
    touch "$MODDIR/disable"
    exit 1
fi

for mod_path in "$MODULES_DIR"/*; do
    [ -d "$mod_path" ] || continue
    mod_name="${mod_path##*/}"
    [ "$mod_name" = "nomount" ] && continue
    if [ -f "$mod_path/disable" ] || [ -f "$mod_path/remove" ] || [ -f "$mod_path/skip_mount" ]; then
        if $VERBOSE; then echo "[SKIP] Module $mod_name is disabled/removed" >> "$LOG_FILE"; fi
        continue
    fi
    MODULE_INJECTED="false"
    for partition in $TARGET_PARTITIONS; do
        if [ -d "$mod_path/$partition" ]; then
            MODULE_INJECTED="true"
            echo "[INFO] Mounting module: $mod_name (/$partition)" >> "$LOG_FILE"
            (
                cd "$mod_path" || exit
                find "$partition" -type f | while read -r relative_path; do
                    real_path="$mod_path/$relative_path"
                    virtual_path="/$relative_path"
                    if $VERBOSE; then
                        echo "  -> Inject: $virtual_path" >> "$LOG_FILE"
                    fi
                    OUTPUT=$("$LOADER" add "$virtual_path" "$real_path" 2>&1)
                    RET_CODE=$?
                    if [ $RET_CODE -ne 0 ]; then
                        echo "  [ERROR] Failed to inject $virtual_path" >> "$LOG_FILE"
                        echo "          Reason: $OUTPUT" >> "$LOG_FILE"
                    fi
                done
            )
        fi
    done
    if [ "$MODULE_INJECTED" = "true" ]; then
        ACTIVE_MODULES_COUNT=$((ACTIVE_MODULES_COUNT + 1))
    fi
done

echo "=== Injection Complete: $(date) ===" >> "$LOG_FILE"
"$LOADER" refresh >/dev/null 2>&1 &
echo "Refresh command sent to /dev/nomount!" >> "$LOG_FILE"

sh "$MODDIR/monitor.sh" "$ACTIVE_MODULES_COUNT" &
