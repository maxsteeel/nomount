ui_print "- Using NoMount metainstall"
install_module
ui_print "- Installation complete"

metamodule_hot_install() {

	# ksu only for now, verify on apatch later
	if [ ! "$KSU" = true ]; then
		return
	fi

	if [ -z "$MODID" ]; then
		return
	fi

	MODDIR_INTERNAL="/data/adb/modules/$MODID"
	MODPATH_INTERNAL="/data/adb/modules_update/$MODID"

	if [ ! -d "$MODDIR_INTERNAL" ] || [ ! -d "$MODPATH_INTERNAL" ]; then
		return
	fi

	# hot install
	busybox rm -rf "$MODDIR_INTERNAL"
	busybox mv "$MODPATH_INTERNAL" "$MODDIR_INTERNAL"

	# run script requested, blocking, just fork it yourselves if you want it on background
	if [ ! -z "$MODULE_HOT_RUN_SCRIPT" ]; then
		[ -f "$MODDIR_INTERNAL/$MODULE_HOT_RUN_SCRIPT" ] && sh "$MODDIR_INTERNAL/$MODULE_HOT_RUN_SCRIPT"
	fi

	# we do this dance to satisfy kernelsu's ensure_file_exists
	mkdir -p "$MODPATH_INTERNAL"
	cat "$MODDIR_INTERNAL/module.prop" > "$MODPATH_INTERNAL/module.prop"

	( sleep 3 ; 
		rm -rf "$MODDIR_INTERNAL/update" ; 
		rm -rf "$MODPATH_INTERNAL"
	) & # fork in background

	ui_print "- Module hot install requested!"
	ui_print "- Refresh module page after installation!"
	ui_print "- No need to reboot!"
}

if [ "$MODULE_HOT_INSTALL_REQUEST" = true ]; then
	metamodule_hot_install
fi

