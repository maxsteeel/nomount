ui_print " "
ui_print "======================================="
ui_print "                NoMount                "
ui_print "  Native Kernel Injection Metamodule   "
ui_print "======================================="
ui_print " "

ui_print "- Device Architecture: $ARCH"

if [ ! -f "$MODPATH/bin/nm-$ARCH" ]; then
  abort "! Unsupported architecture: $ARCH"
fi
mv "$MODPATH/bin/nm-$ARCH" "$MODPATH/bin/nm"
set_perm "$MODPATH/bin/nm" 0 0 0755
rm -rf $MODPATH/bin/nm-*

ui_print "- Checking Kernel support via Netlink..."

if "$MODPATH/bin/nm" v > /dev/null 2>&1; then
  ui_print "  [OK] NoMount Netlink interface detected."
  ui_print "  [OK] System is ready for injection."
else
  ui_print " "
  ui_print "***************************************************"
  ui_print "* [!] WARNING: KERNEL DRIVER NOT DETECTED         *"
  ui_print "***************************************************"
  ui_print "* The NoMount Netlink interface is unresponsive.  *"
  ui_print "* *"
  ui_print "* This module will NOT FUNCTION until you flash   *"
  ui_print "* a Kernel compiled with CONFIG_NOMOUNT=y         *"
  ui_print "***************************************************"
  ui_print " "
  
  touch "$MODPATH/disable"
fi

NOMOUNT_DATA="/data/adb/nomount"
mkdir -p "$NOMOUNT_DATA"
# Preserve user options and exclusions across module updates. The boot marker is
# transient and was previously removed as part of deleting the entire directory.
rm -f "$NOMOUNT_DATA/.booting"

ui_print "- Installation complete."
