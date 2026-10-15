ui_print " "
ui_print "======================================="
ui_print "               NoMount                 "
ui_print "  Native Kernel Injection Metamodule   "
ui_print "======================================="
ui_print " "

ui_print "- Device Architecture: $ARCH"

# Check root implementation
if [ "$KSU" = "true" ] || command -v ksud >/dev/null 2>&1 || [ -x /data/adb/ksu/bin/ksud ];  then
  ROOT_IMP=ksu
  ui_print "- Root implementation: KernelSU"
elif [ "$APATCH" = "true" ] || command -v apd >/dev/null 2>&1 || [ -x /data/adb/ap/bin/apd ]; then
  ROOT_IMP=ap
  ui_print "- Root implementation: APatch"
else
  abort "! Unsupported root env"
fi

if [ ! -f "$MODPATH/bin/nm-$ARCH" ]; then
  abort "! Unsupported architecture: $ARCH"
fi
mv "$MODPATH/bin/nm-$ARCH" "$MODPATH/bin/nm"
set_perm "$MODPATH/bin/nm" 0 0 0755

mkdir -p "/data/adb/$ROOT_IMP/bin"
if ln -sf "/data/adb/modules/nomount/bin/nm" "/data/adb/$ROOT_IMP/bin/nm"; then
    ui_print "- Symlink created."
else
    ui_print "! Failed to create 'nm' symlink, skipping.."
fi

USE_ROOT_INSMOD=false

mv "$MODPATH/bin/lkmloader-$ARCH" "$MODPATH/lkm/lkmloader"
set_perm "$MODPATH/lkm/lkmloader" 0 0 0755
rm -rf "$MODPATH"/bin/nm-* "$MODPATH"/bin/lkmloader-*

load_ko() {
  local ko_path="$1"
  local output
  local ret

  if [ "$USE_ROOT_INSMOD" = true ] && [ "$MODULE_WAS_BUSY" = false ]; then
    if "${ROOT_IMP}d" insmod "$ko_path" >/dev/null 2>&1 && "$MODPATH/bin/nm" version >/dev/null 2>&1; then
      return 0
    fi
    ui_print "  [!] ${ROOT_IMP}d insmod failed; falling back to lkmloader."
    rmmod nomount 2>/dev/null
    USE_ROOT_INSMOD=false
  fi

  output=$("$MODPATH/lkm/lkmloader" "$ko_path" 2>&1)
  ret=$?

  if [ $ret -eq 0 ] && "$MODPATH/bin/nm" version >/dev/null 2>&1; then
    return 0
  fi

  if [ "$MODULE_WAS_BUSY" = true ] && echo "$output" | grep -iq "File exists"; then
    ui_print "  [~] lkmloader verified compatibility (Dry-run pass)."
    return 0
  fi

  ui_print "  [!] lkmloader output: $output"
  return 1
}

OLD_MODPATH="/data/adb/modules/nomount"
KVER=$(uname -r | cut -d'.' -f1,2)
AKVER=$(uname -r | grep -oE 'android[0-9]+')
ARCH_SFX=""
[ "$ARCH" = "x86_64" ] && ARCH_SFX="-x86_64"

if [ -n "$AKVER" ]; then
  ui_print "- Detected Kernel: $KVER ($AKVER branch)"
else
  ui_print "- Detected Kernel: $KVER (Custom/Unknown branch)"
fi

NOMOUNT_LOADED=false
OLD_LKM_UNLOADED=false
RESTORED_OLD_KO=false
IS_BUILTIN=false
MODULE_WAS_BUSY=false

ui_print "- Checking Kernel support via Internal API..."
if "$MODPATH/bin/nm" version > /dev/null 2>&1 || "$OLD_MODPATH/bin/nm" version > /dev/null 2>&1; then
  if grep -q '^nomount ' /proc/modules; then
    ui_print "  [*] Active LKM detected during update."
    ui_print "  [*] Attempting safe unload of the old driver..."
    rmmod_output=$(rmmod nomount 2>&1)
    if [ $? -eq 0 ]; then
      ui_print "  [+] Old driver unloaded successfully."
      OLD_LKM_UNLOADED=true
    else
      ui_print "  [!] rmmod failed: $rmmod_output"
      ui_print "  [!] Old driver is busy. Using 'File exists' as compatibility dry-run."
      MODULE_WAS_BUSY=true
    fi
  else
    IS_BUILTIN=true
  fi
fi

if [ "$IS_BUILTIN" = true ]; then
  ui_print "  [OK] NoMount Internal API detected (Built-in)."
  NOMOUNT_LOADED=true
  rm -rf "$MODPATH/lkm"
else
  if [ "$MODULE_WAS_BUSY" = false ]; then
    ui_print "  [*] Built-in support not found. Attempting LKM injection..."
  fi

  if command -v ksud >/dev/null 2>&1 && ksud -h 2>&1 | grep -qE '(^|[[:space:]])insmod([[:space:]]|$)'; then
    USE_ROOT_INSMOD=true
    ui_print "- KernelSU ksud insmod detected; lkmloader will remain as fallback."
  elif command -v apd >/dev/null 2>&1 && apd -h 2>&1 | grep -qE '(^|[[:space:]])insmod([[:space:]]|$)'; then
    USE_ROOT_INSMOD=true
    ui_print "- APatch apd insmod detected; lkmloader will remain as fallback."
  fi

  EXACT_MATCH="$MODPATH/lkm/nomount-${AKVER}-${KVER}${ARCH_SFX}.ko"
  if [ -n "$AKVER" ] && [ -f "$EXACT_MATCH" ]; then
    ui_print "  [*] Trying exact match: ${EXACT_MATCH##*/}"
    if load_ko "$EXACT_MATCH"; then
      mv "$EXACT_MATCH" "$MODPATH/lkm/nomount.ko"
      NOMOUNT_LOADED=true
      if [ "$MODULE_WAS_BUSY" = true ]; then
        ui_print "  [+] Update staged. It will fully apply on the next reboot."
      fi
    else
      rmmod nomount 2>/dev/null
    fi
  fi

  if [ "$NOMOUNT_LOADED" = false ]; then
    for mod in "$MODPATH"/lkm/nomount*-${KVER}${ARCH_SFX}.ko; do
      if [ ! -f "$mod" ] || [ "$mod" = "$EXACT_MATCH" ]; then continue; fi
      ui_print "  [*] Trying fallback: ${mod##*/}"
      if load_ko "$mod"; then
        mv "$mod" "$MODPATH/lkm/nomount.ko"
        NOMOUNT_LOADED=true
        if [ "$MODULE_WAS_BUSY" = true ]; then
          ui_print "  [+] Fallback update staged. It will fully apply on next reboot."
        fi
        break
      else
        rmmod nomount 2>/dev/null
      fi
    done
  fi

  if [ "$NOMOUNT_LOADED" = false ] && [ -f "$OLD_MODPATH/lkm/nomount.ko" ]; then
    ui_print "  [!] New modules failed. Restoring previous working LKM..."
    mkdir -p "$MODPATH/lkm"
    cp "$OLD_MODPATH/lkm/nomount.ko" "$MODPATH/lkm/nomount.ko"
    cp "$OLD_MODPATH/bin/nm" "$MODPATH/bin/nm" && set_perm "$MODPATH/bin/nm" 0 0 0755
    if load_ko "$MODPATH/lkm/nomount.ko"; then
      NOMOUNT_LOADED=true
      RESTORED_OLD_KO=true
    else
      rmmod nomount 2>/dev/null
    fi
  fi

  rm -f "$MODPATH"/lkm/nomount-*.ko
fi

if [ "$NOMOUNT_LOADED" = true ]; then
  if [ "$USE_KSUD" = true ] || [ "$IS_BUILTIN" = true ]; then
    rm -f "$MODPATH/lkm/lkmloader" || true
  fi
  ui_print "  [OK] System is ready for injection."
  if [ "$OLD_LKM_UNLOADED" = true ] || [ "$RESTORED_OLD_KO" = true ]; then
    if [ -f "$MODPATH/metamount.sh" ]; then
      ui_print "  [*] Executing metamount.sh to refresh bindings..."
      sh "$MODPATH/metamount.sh"
    fi
  fi
else
  ui_print " "
  ui_print "***************************************************"
  ui_print "* [!] WARNING: KERNEL DRIVER NOT DETECTED         *"
  ui_print "***************************************************"
  ui_print "* NoMount Internal API missing/unresponsive and   *"
  ui_print "* no compatible loadable kernel module was found. *"
  ui_print "*                                                 *"
  ui_print "* This module will NOT FUNCTION until you flash   *"
  ui_print "* a Kernel compiled with CONFIG_NOMOUNT=y         *"
  ui_print "***************************************************"
  ui_print " "
  abort "! Kernel module not detected"
fi

NOMOUNT_DATA="/data/adb/nomount"
mkdir -p "$NOMOUNT_DATA"
rm -f "$NOMOUNT_DATA/.booting"

ui_print "- Installation complete."
