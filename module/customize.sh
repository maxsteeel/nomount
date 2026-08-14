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

USE_KSUD=false
if command -v ksud >/dev/null 2>&1 && \
   ksud -h 2>&1 | grep -qE '(^|[[:space:]])insmod([[:space:]]|$)'; then
  USE_KSUD=true
  ui_print "- KernelSU ksud insmod detected; KoLoader will remain as fallback."
fi

if [ ! -f "$MODPATH/bin/ko-loader-$ARCH" ]; then
  abort "! KoLoader binary not found for architecture: $ARCH"
fi
mv "$MODPATH/bin/ko-loader-$ARCH" "$MODPATH/loader"
set_perm "$MODPATH/loader" 0 0 0755
rm -rf "$MODPATH"/bin/nm-* "$MODPATH"/bin/ko-loader-*

KO_LOADER="$MODPATH/loader"

load_ko() {
  if [ "$USE_KSUD" = true ]; then
    if ksud insmod "$1" && "$MODPATH/bin/nm" version > /dev/null 2>&1; then
      return 0
    fi
    ui_print "  [!] ksud insmod failed; falling back to KoLoader."
    rmmod nomount 2>/dev/null
    USE_KSUD=false
  fi

  ko_name=${1##*/}
  (
    cd "$MODPATH/lkm" || exit 1
    "$KO_LOADER" "$ko_name"
  )
}

KVER=$(uname -r | cut -d'.' -f1,2)
AKVER=$(uname -r | grep -oE 'android[0-9]+')

if [ -n "$AKVER" ]; then
  ui_print "- Detected Kernel: $KVER ($AKVER branch)"
else
  ui_print "- Detected Kernel: $KVER (Custom/Unknown branch)"
fi

install_lkm() {
  local module_path="$1"
  if command -v ksud >/dev/null 2>&1; then
    ksud insmod "$module_path"
    return $?
  fi
  insmod "$module_path"
  return $?
}

NOMOUNT_LOADED=false
OLD_LKM_UNLOADED=false
RESTORED_OLD_KO=false
IS_BUILTIN=false

ui_print "- Checking Kernel support via Internal API..."
if "$MODPATH/bin/nm" version > /dev/null 2>&1; then
  if lsmod | grep -q "^nomount"; then
    ui_print "  [*] Active LKM detected during update. Unloading old driver..."
    rmmod nomount 2>/dev/null
    OLD_LKM_UNLOADED=true
  else
    IS_BUILTIN=true
  fi
fi

if [ "$IS_BUILTIN" = true ]; then
  ui_print "  [OK] NoMount Internal API detected (Built-in)."
  NOMOUNT_LOADED=true
  rm -rf "$MODPATH/lkm"
else
  ui_print "  [*] Built-in support not found. Attempting LKM injection..."

  EXACT_MATCH="$MODPATH/lkm/nomount-${AKVER}-${KVER}.ko"
  if [ -n "$AKVER" ] && [ -f "$EXACT_MATCH" ]; then
    ui_print "  [*] Trying exact match: $(basename "$EXACT_MATCH")"
    if load_ko "$EXACT_MATCH" && "$MODPATH/bin/nm" version > /dev/null 2>&1; then
      mv "$EXACT_MATCH" "$MODPATH/lkm/nomount.ko"
      NOMOUNT_LOADED=true
    else
      rmmod nomount 2>/dev/null
    fi
  fi

  if [ "$NOMOUNT_LOADED" = false ]; then
    for mod in "$MODPATH"/lkm/nomount*-${KVER}.ko; do
      if [ ! -f "$mod" ] || [ "$mod" = "$EXACT_MATCH" ]; then continue; fi
      ui_print "  [*] Trying fallback: $(basename "$mod")"
      if load_ko "$mod" && "$MODPATH/bin/nm" version > /dev/null 2>&1; then
        mv "$mod" "$MODPATH/lkm/nomount.ko"
        NOMOUNT_LOADED=true
        break
      else
        rmmod nomount 2>/dev/null
      fi
    done
  fi

  OLD_MODULE_KO="/data/adb/modules/nomount/lkm/nomount.ko"
  if [ "$NOMOUNT_LOADED" = false ] && [ -f "$OLD_MODULE_KO" ]; then
    ui_print "  [!] New modules failed. Restoring previous working LKM..."
    mkdir -p "$MODPATH/lkm"
    cp "$OLD_MODULE_KO" "$MODPATH/lkm/nomount.ko"
    if load_ko "$MODPATH/lkm/nomount.ko" && "$MODPATH/bin/nm" version > /dev/null 2>&1; then
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
    rm -f "$MODPATH/loader"
  fi
  ui_print "  [OK] System is ready for injection."
  if { [ "$OLD_LKM_UNLOADED" = true ] && [ "$IS_BUILTIN" = false ]; } || [ "$RESTORED_OLD_KO" = true ]; then
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
