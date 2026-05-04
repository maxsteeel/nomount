ui_print " "
ui_print "======================================="
ui_print "               NoMount                 "
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

ui_print "- Checking Kernel support..."
if [ -e "/dev/nomount" ]; then
  ui_print "  [OK] Driver /dev/nomount detected."
  ui_print "  [OK] System is ready for injection."
else
  ui_print " "
  ui_print "***************************************************"
  ui_print "* [!] WARNING: KERNEL DRIVER NOT DETECTED         *"
  ui_print "***************************************************"
  ui_print "* The device node /dev/nomount is missing.        *"
  ui_print "* *"
  ui_print "* This module will NOT FUNCTION until you flash   *"
  ui_print "* a Kernel compiled with CONFIG_NOMOUNT=y         *"
  ui_print "***************************************************"
  ui_print " "
  
  touch "$MODPATH/disable"
fi

if [ -f "/data/adb/nomount" ]; then
    rm -rf "/data/adb/nomount"
fi

ui_print "- Installation complete."

