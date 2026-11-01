ui_print " "
ui_print "======================================="
ui_print "               NoMount                 "
ui_print "  Native Kernel Injection Metamodule   "
ui_print "======================================="
ui_print " "

ui_print "- Device Architecture: $ARCH"

# KernelSU reports x86_64 devices as "x64", Magisk as "x86_64"
ARCH_BIN="$ARCH"
[ "$ARCH_BIN" = "x64" ] && ARCH_BIN="x86_64"

# Check root implementation
if [ "$KSU" = "true" ] || command -v ksud >/dev/null 2>&1 || [ -x /data/adb/ksu/bin/ksud ]; then
  ROOT_IMP=ksu
  ui_print "- Root implementation: KernelSU"
elif [ "$APATCH" = "true" ] || command -v apd >/dev/null 2>&1 || [ -x /data/adb/ap/bin/apd ]; then
  ROOT_IMP=ap
  ui_print "- Root implementation: APatch"
else
  abort "! Unsupported root env"
fi

if [ ! -f "$MODPATH/bin/nm-$ARCH_BIN" ]; then
  abort "! Unsupported architecture: $ARCH"
fi
mv "$MODPATH/bin/nm-$ARCH_BIN" "$MODPATH/bin/nm"
set_perm "$MODPATH/bin/nm" 0 0 0755

mv "$MODPATH/bin/lkmloader-$ARCH_BIN" "$MODPATH/lkm/lkmloader"
set_perm "$MODPATH/lkm/lkmloader" 0 0 0755
rm -rf "$MODPATH"/bin/nm-* "$MODPATH"/bin/lkmloader-*

try_load_ko() {
  local output

  output=$("$MODPATH/lkm/lkmloader" "$1" 2>&1)
  if "$MODPATH/bin/nm" version >/dev/null 2>&1; then
    return 0
  fi

  if echo "$output" | grep -qi "File exists"; then
    return 0
  fi

  ui_print "  $output"
  return 1
}

load_ko_for_kernel() {
  local kver="$1" akver="$2" arch_sfx="$3"
  local exact="$MODPATH/lkm/nomount-${akver}-${kver}${arch_sfx}.ko"
  local mod
  local found=0

  if [ -n "$akver" ] && [ -f "$exact" ]; then
    found=1
    ui_print "  [*] Trying: ${exact##*/}"
    if try_load_ko "$exact"; then
      mv "$exact" "$MODPATH/lkm/nomount.ko"
      return 0
    fi
  fi

  for mod in "$MODPATH"/lkm/nomount*-${kver}${arch_sfx}.ko; do
    [ -f "$mod" ] || continue
    [ "$mod" = "$exact" ] && continue
    found=1
    ui_print "  [*] Trying fallback: ${mod##*/}"
    if try_load_ko "$mod"; then
      mv "$mod" "$MODPATH/lkm/nomount.ko"
      return 0
    fi
  done

  [ "$found" = 0 ] && return 2
  return 1
}

OLD_MODPATH="/data/adb/modules/nomount"
KVER=$(uname -r | cut -d'.' -f1,2)
AKVER=$(uname -r | grep -oE 'android[0-9]+')
ARCH_SFX=""
[ "$ARCH_BIN" = "x86_64" ] && ARCH_SFX="-x86_64"

if [ -n "$AKVER" ]; then
  ui_print "- Detected Kernel: $KVER ($AKVER branch)"
else
  ui_print "- Detected Kernel: $KVER (Custom/Unknown branch)"
fi

API_RESPONDS=false
LKM_RUNNING=false
BUILTIN=false
STAGED_FOR_REBOOT=false
LOADED_FRESH=false

ui_print "- Checking kernel support via internal API..."
if "$MODPATH/bin/nm" version >/dev/null 2>&1; then
  API_RESPONDS=true
elif [ -x "$OLD_MODPATH/bin/nm" ] && "$OLD_MODPATH/bin/nm" version >/dev/null 2>&1; then
  API_RESPONDS=true
fi

if [ "$API_RESPONDS" = true ]; then
  if grep -q '^nomount ' /proc/modules; then
    LKM_RUNNING=true
  else
    BUILTIN=true
  fi
fi

if [ "$BUILTIN" = true ]; then
  ui_print "  [OK] NoMount is built into the kernel."
  rm -rf "$MODPATH/lkm"

elif [ "$LKM_RUNNING" = true ]; then
  ui_print "  [*] Active LKM detected."
  ui_print "  [*] Verifying new driver (no unload)..."

  load_ko_for_kernel "$KVER" "$AKVER" "$ARCH_SFX"
  KO_RC=$?

  if [ "$KO_RC" = 0 ]; then
    STAGED_FOR_REBOOT=true
    ui_print "  [+] New driver verified and staged for the next reboot."
  elif [ "$KO_RC" = 2 ]; then
    ui_print "  [*] No new KO shipped; keeping the running driver as is."
  else
    ui_print "  [!] New driver failed verification; keeping the running one."
    if [ -f "$OLD_MODPATH/lkm/nomount.ko" ]; then
      mkdir -p "$MODPATH/lkm"
      cp "$OLD_MODPATH/lkm/nomount.ko" "$MODPATH/lkm/nomount.ko"
      cp "$OLD_MODPATH/bin/nm" "$MODPATH/bin/nm" 2>/dev/null && set_perm "$MODPATH/bin/nm" 0 0 0755
    fi
  fi

  rm -f "$MODPATH"/lkm/nomount-*.ko

else
  ui_print "  [*] Built-in support not found. Attempting LKM injection..."

  if load_ko_for_kernel "$KVER" "$AKVER" "$ARCH_SFX"; then
    LOADED_FRESH=true
  fi

  if [ "$LOADED_FRESH" = false ] && [ -f "$OLD_MODPATH/lkm/nomount.ko" ]; then
    ui_print "  [!] New driver failed; restoring previous working LKM..."
    mkdir -p "$MODPATH/lkm"
    cp "$OLD_MODPATH/lkm/nomount.ko" "$MODPATH/lkm/nomount.ko"
    cp "$OLD_MODPATH/bin/nm" "$MODPATH/bin/nm" 2>/dev/null && set_perm "$MODPATH/bin/nm" 0 0 0755
    if try_load_ko "$MODPATH/lkm/nomount.ko"; then
      LOADED_FRESH=true
    fi
  fi

  rm -f "$MODPATH"/lkm/nomount-*.ko

  if [ "$LOADED_FRESH" = false ]; then
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
fi

NM_REQUIRED_VER=22
NM_CURRENT_VER=$("$MODPATH/bin/nm" version 2>/dev/null | head -1 | grep -oE '^[0-9]+' | head -1)
NM_CURRENT_VER=${NM_CURRENT_VER:-0}

if [ "$NM_CURRENT_VER" -lt "$NM_REQUIRED_VER" ]; then
  if [ "$BUILTIN" = true ]; then
    ui_print " "
    ui_print "***************************************************"
    ui_print "* [!] BUILT-IN DRIVER TOO OLD                     *"
    ui_print "***************************************************"
    ui_print "* Built-in driver: v${NM_CURRENT_VER}             *"
    ui_print "* Required:        v${NM_REQUIRED_VER}            *"
    ui_print "*                                                 *"
    ui_print "* This kernel has an older NoMount built into it. *"
    ui_print "* Flash a kernel with v${NM_REQUIRED_VER}+, or    *"
    ui_print "* downgrade to NoMount v2.1.x.                    *"
    ui_print "***************************************************"
    ui_print " "
    abort "! Built-in driver v${NM_CURRENT_VER} incompatible (v${NM_REQUIRED_VER} required)"
  elif [ "$STAGED_FOR_REBOOT" = true ]; then
    ui_print "  [!] Current driver: v${NM_CURRENT_VER}; this module requires v${NM_REQUIRED_VER}+"
    ui_print "  [!] A v${NM_REQUIRED_VER} driver is staged for the next boot."
    ui_print "  [!] Do NOT use 'nm rule' or 'nm uid' until you reboot:"
    ui_print "      the v${NM_REQUIRED_VER} CLI cannot talk to the v${NM_CURRENT_VER} driver."
  else
    ui_print " "
    ui_print "***************************************************"
    ui_print "* [!] KERNEL DRIVER TOO OLD                       *"
    ui_print "***************************************************"
    ui_print "* Current driver: v${NM_CURRENT_VER}              *"
    ui_print "* Required:       v${NM_REQUIRED_VER}             *"
    ui_print "*                                                 *"
    ui_print "* No compatible driver update is available for    *"
    ui_print "* this kernel. Update your kernel first, then     *"
    ui_print "* reinstall this module. If you cannot update    *"
    ui_print "* the kernel, downgrade to NoMount v2.1.x.        *"
    ui_print "***************************************************"
    ui_print " "
    abort "! Kernel driver v${NM_CURRENT_VER} incompatible (v${NM_REQUIRED_VER} required)"
  fi
fi

if [ "$STAGED_FOR_REBOOT" = true ]; then
  ui_print "  [OK] Update staged. Reboot to activate the new driver."
  if [ "$NM_CURRENT_VER" -ge "$NM_REQUIRED_VER" ]; then
    ui_print "       The current driver keeps working until then."
  fi
else
  ui_print "  [OK] System is ready for injection."
fi

NOMOUNT_DATA="/data/adb/nomount"
mkdir -p "$NOMOUNT_DATA"
rm -f "$NOMOUNT_DATA/.booting"

mkdir -p "/data/adb/$ROOT_IMP/bin"
if ln -sf "/data/adb/modules/nomount/bin/nm" "/data/adb/$ROOT_IMP/bin/nm"; then
  ui_print "- Symlink created."
else
  ui_print "! Failed to create 'nm' symlink, skipping.."
fi

ui_print "- Installation complete."
