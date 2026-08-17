#!/system/bin/sh

# Remove old log / exclusion_list
rm -rf /data/adb/nomount/ || true

# Remove symlink
rm -f /data/adb/ksu/bin/nm || true
rm -f /data/adb/ap/bin/nm || true
