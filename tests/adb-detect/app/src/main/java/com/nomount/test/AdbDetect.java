package com.nomount.test;

import android.app.Activity;
import android.content.ContentResolver;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;
import android.graphics.Typeface;

import org.lsposed.hiddenapibypass.HiddenApiBypass;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.lang.reflect.Method;
import java.net.Socket;

public class AdbDetect extends Activity {

    private static final String TAG = "AdbDetect";
    private final StringBuilder out = new StringBuilder();
    private int passCount, failCount;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            HiddenApiBypass.addHiddenApiExemptions("");
        }

        runAllChecks();

        ScrollView sv = new ScrollView(this);
        TextView tv = new TextView(this);
        tv.setText(out.toString());
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setTextSize(11f);
        tv.setPadding(24, 24, 24, 24);
        sv.addView(tv);
        setContentView(sv);

        Log.i(TAG, out.toString());
    }

    private void runAllChecks() {
        out.append("=== ADB/USB Detection Adversarial Test ===\n");
        out.append("UID: ").append(android.os.Process.myUid()).append("\n");
        out.append("SDK: ").append(Build.VERSION.SDK_INT).append("\n\n");

        checkSettingsGlobal();
        checkSettingsSecure();
        checkSettingsSystem();
        checkSystemProperties();
        checkBuildFields();
        checkFilePaths();
        checkNetworkProbes();
        checkProcessState();
        checkInstallSource();

        out.append("\n=== RESULTS: ")
           .append(passCount).append(" PASS / ")
           .append(failCount).append(" FAIL ===\n");

        if (failCount > 0) {
            out.append("DETECTION VECTORS FOUND\n");
        } else {
            out.append("ALL VECTORS HIDDEN\n");
        }
    }

    private void checkSettingsGlobal() {
        out.append("--- Settings.Global ---\n");
        ContentResolver cr = getContentResolver();

        // Hooked by Zygisk ContentProvider hook
        expectZero(cr, "global", "adb_enabled");
        expectZero(cr, "global", "development_settings_enabled");
        expectZero(cr, "global", "adb_wifi_enabled");
        expectNull(cr, "global", "hidden_api_policy");
        expectZero(cr, "global", "allow_mock_location");

        // Not currently hooked — detection surface assessment
        readOnly(cr, "global", "debug_app");
        readOnly(cr, "global", "wait_for_debugger");
        readOnly(cr, "global", "bugreport_in_power_menu");
        readOnly(cr, "global", "debug_view_attributes");
        readOnly(cr, "global", "stay_on_while_plugged_in");
        readOnly(cr, "global", "oem_unlock_allowed");
        readOnly(cr, "global", "package_verifier_include_adb");
        readOnly(cr, "global", "hidden_api_policy_pre_p_apps");
        readOnly(cr, "global", "hidden_api_policy_p_apps");
        readOnly(cr, "global", "force_allow_on_external");
        readOnly(cr, "global", "always_finish_activities");
        out.append("\n");
    }

    private void checkSettingsSecure() {
        out.append("--- Settings.Secure ---\n");
        ContentResolver cr = getContentResolver();

        // Legacy path — some OEMs write here
        readOnly(cr, "secure", "adb_enabled");
        readOnly(cr, "secure", "development_settings_enabled");
        readOnly(cr, "secure", "bluetooth_hci_log");
        readOnly(cr, "secure", "anr_show_background");
        out.append("\n");
    }

    private void checkSettingsSystem() {
        out.append("--- Settings.System ---\n");
        ContentResolver cr = getContentResolver();

        readOnly(cr, "system", "pointer_location");
        readOnly(cr, "system", "show_touches");
        out.append("\n");
    }

    // SystemProperties via hidden API — no fork overhead
    private void checkSystemProperties() {
        out.append("--- System Properties ---\n");

        // Prop-watch targets (spoofed when hide_usb_debugging=true)
        expectPropNoAdb("persist.sys.usb.config");
        expectPropNoAdb("sys.usb.config");
        expectPropNoAdb("sys.usb.state");
        expectPropNot("init.svc.adbd", "running");

        // Build-level props (read-only, can't be spoofed at runtime)
        readProp("ro.debuggable");
        readProp("ro.secure");
        readProp("ro.adb.secure");
        readProp("ro.build.type");
        readProp("ro.build.tags");
        readProp("ro.boot.flash.locked");
        readProp("ro.boot.verifiedboot.state");

        // USB gadget state
        readProp("sys.usb.ffs.ready");
        readProp("sys.usb.controller");
        readProp("sys.usb.configfs");
        readProp("service.bootanim.exit");
        out.append("\n");
    }

    private void checkBuildFields() {
        out.append("--- Build Fields ---\n");
        readValue("Build.TYPE", Build.TYPE);
        readValue("Build.TAGS", Build.TAGS);
        readValue("Build.FINGERPRINT", Build.FINGERPRINT);
        out.append("\n");
    }

    private void checkFilePaths() {
        out.append("--- File Paths ---\n");

        expectFileBlocked("/data/misc/adb/adb_keys");
        expectFileBlocked("/data/misc/adb/adb_temp_keys.xml");
        expectFileBlocked("/sys/class/android_usb/android0/functions");
        expectFileBlocked("/sys/class/android_usb/android0/state");

        checkTracerPid();
        checkJdwpThread();
        out.append("\n");
    }

    private void checkNetworkProbes() {
        out.append("--- Network Probes ---\n");
        expectPortClosed(5037, "adb-server");
        expectPortClosed(5555, "adb-tcp");
        out.append("\n");
    }

    private void checkProcessState() {
        out.append("--- Process State ---\n");

        report("Debug.isDebuggerConnected",
            !android.os.Debug.isDebuggerConnected(),
            String.valueOf(android.os.Debug.isDebuggerConnected()), "false");

        report("Debug.waitingForDebugger",
            !android.os.Debug.waitingForDebugger(),
            String.valueOf(android.os.Debug.waitingForDebugger()), "false");

        boolean debuggable = (getApplicationInfo().flags & 0x2) != 0;
        report("FLAG_DEBUGGABLE", !debuggable,
            String.valueOf(debuggable), "false");
        out.append("\n");
    }

    private void checkInstallSource() {
        out.append("--- Install Source ---\n");
        try {
            String installer = getPackageManager()
                .getInstallSourceInfo(getPackageName())
                .getInstallingPackageName();
            boolean adb = "com.android.shell".equals(installer);
            report("installSource", !adb, String.valueOf(installer), "not shell");
        } catch (Exception e) {
            report("installSource", true, "unavailable", "any");
        }
        out.append("\n");
    }

    private void expectZero(ContentResolver cr, String ns, String key) {
        String val = getSetting(cr, ns, key);
        boolean pass = val == null || "0".equals(val) || val.isEmpty();
        report(ns + "/" + key, pass, val == null ? "null" : val, "0/null");
    }

    private void expectNull(ContentResolver cr, String ns, String key) {
        String val = getSetting(cr, ns, key);
        report(ns + "/" + key, val == null, val == null ? "null" : val, "null");
    }

    private void readOnly(ContentResolver cr, String ns, String key) {
        String val = getSetting(cr, ns, key);
        out.append(String.format("  [INFO] %-40s = %s\n", ns + "/" + key,
            val == null ? "null" : val));
    }

    private String getSetting(ContentResolver cr, String ns, String key) {
        try {
            return switch (ns) {
                case "global" -> Settings.Global.getString(cr, key);
                case "secure" -> Settings.Secure.getString(cr, key);
                case "system" -> Settings.System.getString(cr, key);
                default -> null;
            };
        } catch (Exception e) { return null; }
    }

    private void expectPropNoAdb(String prop) {
        String val = getSystemProp(prop);
        boolean pass = !val.contains("adb");
        report("prop:" + prop, pass, val.isEmpty() ? "(empty)" : val, "no 'adb'");
    }

    private void expectPropNot(String prop, String bad) {
        String val = getSystemProp(prop);
        report("prop:" + prop, !bad.equals(val), val.isEmpty() ? "(empty)" : val, "not " + bad);
    }

    private void readProp(String prop) {
        String val = getSystemProp(prop);
        out.append(String.format("  [INFO] %-40s = %s\n", "prop:" + prop,
            val.isEmpty() ? "(empty)" : val));
    }

    // HiddenApiBypass exempts all hidden APIs at runtime — reflection resolves at compile time
    private String getSystemProp(String name) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            Method get = sp.getMethod("get", String.class, String.class);
            String val = (String) get.invoke(null, name, "");
            return val != null ? val : "";
        } catch (Exception e) {
            return "";
        }
    }

    private void readValue(String label, String val) {
        out.append(String.format("  [INFO] %-40s = %s\n", label, val));
    }

    private void expectFileBlocked(String path) {
        boolean readable = new File(path).canRead();
        report("file:" + path, !readable, readable ? "READABLE" : "blocked", "blocked");
    }

    private void expectPortClosed(int port, String label) {
        boolean open = false;
        try (Socket s = new Socket("127.0.0.1", port)) {
            open = true;
        } catch (Exception ignored) {}
        report("port:" + port + "(" + label + ")", !open, open ? "OPEN" : "closed", "closed");
    }

    private void checkTracerPid() {
        try (BufferedReader br = new BufferedReader(new FileReader("/proc/self/status"))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (line.startsWith("TracerPid:")) {
                    String pid = line.substring(line.indexOf(':') + 1).trim();
                    report("TracerPid", "0".equals(pid), pid, "0");
                    return;
                }
            }
        } catch (Exception ignored) {}
        report("TracerPid", true, "unreadable", "0");
    }

    private void checkJdwpThread() {
        File taskDir = new File("/proc/self/task");
        File[] tasks = taskDir.listFiles();
        if (tasks == null) return;
        for (File t : tasks) {
            try (BufferedReader br = new BufferedReader(new FileReader(new File(t, "comm")))) {
                String comm = br.readLine();
                if (comm != null && comm.contains("JDWP")) {
                    report("JDWP-thread", false, comm, "absent");
                    return;
                }
            } catch (Exception ignored) {}
        }
        report("JDWP-thread", true, "absent", "absent");
    }

    private void report(String label, boolean pass, String actual, String expected) {
        String tag = pass ? "PASS" : "FAIL";
        if (pass) passCount++; else failCount++;
        out.append(String.format("  [%s] %-40s got=%-16s want=%s\n",
            tag, label, actual, expected));
    }
}
