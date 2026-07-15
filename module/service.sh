#!/system/bin/sh
# Bootloop-guard reset: once the system finishes booting, the last boot was
# healthy, so clear the boot counter (re-arms the guard for next time).
NMDIR=/data/adb/nomount
i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 120 ]; do
    sleep 2
    i=$((i + 1))
done

# Re-assert the umount master switch now that the system has booted. SukiSU
# re-applies kernel_umount from the manager's config during early boot, which
# reverts metamount.sh's post-fs-data set back to OFF. Running here (after
# boot-completed) wins that race so NoMount's overlay mounts stay hidden.
# Re-register the mount paths too (idempotent; ksud dedups) in case they were
# cleared. Apps the user opens (e.g. detectors) spawn later, so they get the
# umount applied in their namespace.
if command -v ksud >/dev/null 2>&1; then
    ksud feature set kernel_umount 1 >/dev/null 2>&1
    awk '$1=="nomount_ov" || $1=="nomount_work" || $2 ~ /^\/mnt\/nomount/ {print $2}' \
        /proc/self/mounts 2>/dev/null | while read -r mp; do
        [ -n "$mp" ] && ksud umount-config add "$mp" --flags 2 >/dev/null 2>&1
    done
fi

sleep 10
rm -f "$NMDIR/bootcount"
echo "nomount: boot completed, guard counter reset" > /dev/kmsg 2>/dev/null
exit 0
