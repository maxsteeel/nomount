#!/system/bin/sh
# Shared functions for NoMount's scripts

NOMOUNT_DATA="/data/adb/nomount"
LOADER="$MODDIR/bin/nm"
BOOT_SEMAPHORE="$NOMOUNT_DATA/.booting"
DUMP_RULES_FLAG="$NOMOUNT_DATA/dump_rules"
PROP_FILE="$MODDIR/module.prop"
BASE_DESC="A metamodule that replaces OverlayFS/MagicMount with VFS path redirection."

klog()      { echo "<6>nomount: $*" > /dev/kmsg; }
klogwarn()  { echo "<4>nomount: $*" > /dev/kmsg; }
klogerr()   { echo "<3>nomount: $*" > /dev/kmsg; }

try() {
    local out
    out=$("$@" 2>&1) && return 0
    klogerr "$1 failed: $(echo "$out" | head -n 3 | tr '\n' ' ')"
    return 1
}

disable_module() {
    touch "$NOMOUNT_DATA/disable"
    sed -i "s|^description=.*|description=[$1] \\\\n$BASE_DESC|" "$PROP_FILE"
    rm -f "$BOOT_SEMAPHORE"
}

load_ko() {
    local root_cmd=""
    if command -v ksud >/dev/null 2>&1 && ksud -h 2>&1 | grep -qE '(^|[[:space:]])insmod([[:space:]]|$)'; then root_cmd="ksud"
    elif command -v apd >/dev/null 2>&1 && apd -h 2>&1 | grep -qE '(^|[[:space:]])insmod([[:space:]]|$)'; then root_cmd="apd"; fi

    if [ -n "$root_cmd" ]; then
        if try "$root_cmd" insmod "$1" && "$LOADER" version >/dev/null 2>&1; then
            return 0
        fi
        klogwarn "$root_cmd path failed, falling back to lkmloader"
        rmmod nomount 2>/dev/null
    fi

    if ! { try "$MODDIR/lkm/lkmloader" "$1" && "$LOADER" version >/dev/null 2>&1; }; then
        klogerr "lkmloader failed; LKM not loaded"
        return 1
    fi
    return 0
}

_lock_fd() {
    local fd="$1" timeout="$2" n=0
    while ! flock -n "$fd"; do
        n=$((n + 1))
        [ "$n" -ge "$timeout" ] && return 1
        sleep 1
    done
    return 0
}

bootloop_check() {
    exec 8>"$NOMOUNT_DATA/.boot.lock"
    if ! _lock_fd 8 15; then
        klogerr "boot lock timeout after 15s"
        exec 8>&-
        return 1
    fi

    local current prev
    current=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
    [ -z "$current" ] && current="fallback-$(date +%s)"

    if [ -f "$BOOT_SEMAPHORE" ] && [ -s "$BOOT_SEMAPHORE" ]; then
        prev=$(cat "$BOOT_SEMAPHORE" 2>/dev/null || echo "MISSING")
        if [ "$prev" != "$current" ]; then
            klogerr "Bootloop detected (prev=$prev current=$current)"
            exec 8>&-
            disable_module "🚨 Bootloop Prevented"
            return 1
        fi
    else
        klog "NoMount module boot log started"
        echo "$current" > "$BOOT_SEMAPHORE"
    fi

    exec 8>&-
    return 0
}

ensure_lkm_loaded() {
    exec 7>"$NOMOUNT_DATA/.lkm.lock"
    if ! _lock_fd 7 60; then
        klogerr "LKM lock timed out after 60s"
        exec 7>&-
        return 1
    fi

    if "$LOADER" version >/dev/null 2>&1; then
        exec 7>&-
        return 0
    fi

    klog "Loading LKM..."
    if ! load_ko "$MODDIR/lkm/nomount.ko"; then
        klogerr "LKM load failed."
        exec 7>&-
        return 1
    fi
    klog "LKM loaded and initialized correctly."
    exec 7>&-
    return 0
}

dump_all_rules_logcat() {
    log -t nomount -p i "rules dump started (triggered $*)"

    "$LOADER" rule list 2>&1 | {
        buf=""
        n=0
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            if [ -n "$buf" ] && [ $(( ${#buf} + ${#line} + 3 )) -gt 900 ]; then
                log -t nomount -p i "$buf"
                buf="$line"
            else
                buf="${buf:+$buf ; }$line"
            fi
            n=$((n + 1))
        done
        [ -n "$buf" ] && log -t nomount -p i "$buf"
        log -t nomount -p i "rules dump end (total $n rules)"
    }
}
