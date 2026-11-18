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

_nm_boot_id() { cat /proc/sys/kernel/random/boot_id 2>/dev/null; }

_lock_acquire() {
    local lockdir="$1" timeout="$2" n=0
    local pid lock_boot cur_boot claimed
    cur_boot=$(_nm_boot_id)
    while ! mkdir "$lockdir" 2>/dev/null; do
        pid=$(cat "$lockdir/pid" 2>/dev/null)
        lock_boot=$(cat "$lockdir/boot_id" 2>/dev/null)
        claimed="$lockdir.claimed.$$"
        if { [ -n "$cur_boot" ] && [ -n "$lock_boot" ] && [ "$lock_boot" != "$cur_boot" ]; } ||
           { [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; }; then
            if mv "$lockdir" "$claimed" 2>/dev/null; then
                klogwarn "cleaning stale lock $lockdir"
                rm -rf "$claimed"
            fi
        fi
        n=$((n + 1))
        if [ "$n" -ge "$timeout" ]; then
            return 1
        fi
        sleep 1
    done
    echo $$ > "$lockdir/pid"
    [ -n "$cur_boot" ] && printf '%s\n' "$cur_boot" > "$lockdir/boot_id"
    return 0
}

_lock_release() {
    rm -rf "$1" 2>/dev/null
    return 0
}

bootloop_check() {
    local lockdir="$NOMOUNT_DATA/.boot.lock"
    if ! _lock_acquire "$lockdir" 15; then
        klogerr "boot lock timeout after 15s"
        return 1
    fi

    local current prev
    current=$(_nm_boot_id)
    [ -z "$current" ] && current="fallback-$(date +%s)"

    if [ -f "$BOOT_SEMAPHORE" ] && [ -s "$BOOT_SEMAPHORE" ]; then
        prev=$(cat "$BOOT_SEMAPHORE" 2>/dev/null || echo "MISSING")
        if [ "$prev" != "$current" ]; then
            klogerr "Bootloop detected (prev=$prev current=$current)"
            _lock_release "$lockdir"
            disable_module "🚨 Bootloop Prevented"
            return 1
        fi
    else
        klog "NoMount module boot log started"
        echo "$current" > "$BOOT_SEMAPHORE"
    fi

    _lock_release "$lockdir"
    return 0
}

ensure_lkm_loaded() {
    local lockdir="$NOMOUNT_DATA/.lkm.lock"
    if ! _lock_acquire "$lockdir" 60; then
        klogerr "LKM lock timed out after 60s"
        return 1
    fi

    if "$LOADER" version >/dev/null 2>&1; then
        _lock_release "$lockdir"
        return 0
    fi

    klog "Loading LKM..."
    if ! load_ko "$MODDIR/lkm/nomount.ko"; then
        klogerr "LKM load failed."
        _lock_release "$lockdir"
        return 1
    fi
    klog "LKM loaded and initialized correctly."
    _lock_release "$lockdir"
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
