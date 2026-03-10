package com.hybrid.imgui;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public class Injector {

    private static final String TAG = "HybridInjector";

    public interface LogCallback {
        void onLog(String msg);
    }

    private final Context     ctx;
    private final LogCallback logger;

    public Injector(Context ctx, LogCallback logger) {
        this.ctx    = ctx;
        this.logger = logger;
    }

    private void log(String msg) {
        Log.d(TAG, msg);
        if (logger != null) logger.onLog(msg);
    }

    // =========================================================================
    // SELinux permissive
    // =========================================================================
    public void setSelinuxPermissive() {
        log("Setting SELinux permissive...");
        runSu("setenforce 0");
        String r = runSu("getenforce");
        log("  SELinux: " + (r != null ? r.trim() : "unknown"));
    }

    // =========================================================================
    // Extract libinternal.so ke /data/local/tmp/ untuk wrap/inject
    // libimgui_ext.so TIDAK perlu di-extract — sudah ada di nativeLibraryDir APK
    // =========================================================================
    public boolean extractInternal(String abi) {
        log("Extracting libinternal.so for ABI: " + abi);

        String destDir = "/data/local/tmp/";
        runSu("mkdir -p " + destDir);
        runSu("chmod 777 " + destDir);

        AssetManager am = ctx.getAssets();
        String assetPath = "jniLibs/" + abi + "/libinternal.so";
        String destPath  = destDir + "libinternal.so";

        try {
            InputStream      is  = am.open(assetPath);
            File             tmp = new File(ctx.getFilesDir(), "libinternal.so");
            FileOutputStream os  = new FileOutputStream(tmp);
            byte[] buf = new byte[65536];
            int n;
            while ((n = is.read(buf)) != -1) os.write(buf, 0, n);
            is.close(); os.close();

            runSu("cp " + tmp.getAbsolutePath() + " " + destPath);
            runSu("chmod 755 " + destPath);
            log("  ✓ libinternal.so → " + destPath);
            return true;
        } catch (IOException e) {
            // Fallback: libinternal.so sudah ada di nativeLibraryDir
            String nativeDir = ctx.getApplicationInfo().nativeLibraryDir;
            String srcPath   = nativeDir + "/libinternal.so";
            String check     = runSu("ls " + srcPath + " 2>/dev/null");
            if (check != null && check.contains("libinternal")) {
                runSu("cp " + srcPath + " " + destPath);
                runSu("chmod 755 " + destPath);
                log("  ✓ libinternal.so from nativeDir → " + destPath);
                return true;
            }
            log("  ✗ libinternal.so tidak ditemukan: " + e.getMessage());
            return false;
        }
    }

    // launchOverlay() DIHAPUS — sekarang dihandle oleh injector binary.
    // Lihat overlay_launcher.h dan injector_main.cpp.

    // [PLACEHOLDER — method ini tidak dipakai lagi, dipertahankan sementara untuk kompatibilitas]
    @Deprecated
    public void launchOverlay() {
        log("Launching overlay...");

        // Kill instance lama
        runSu("pkill -f 'uk.lgl.OverlayMain' 2>/dev/null");
        try { Thread.sleep(500); } catch (Exception e) { /* */ }

        // APK path sebagai CLASSPATH — ini yang benar, sama dgn referensi
        String apkPath = ctx.getPackageCodePath();
        // .so sudah ada di nativeLibraryDir, tidak perlu extract
        String libPath = ctx.getApplicationInfo().nativeLibraryDir;

        log("  APK  : " + apkPath);
        log("  libs : " + libPath);

        // Cari app_process yang tersedia
        String appProc = null;
        for (String p : new String[]{
                "/system/bin/app_process64",
                "/system/bin/app_process32",
                "/system/bin/app_process"}) {
            String chk = runSu("ls " + p + " 2>/dev/null");
            if (chk != null && chk.contains("app_process")) {
                appProc = p;
                break;
            }
        }
        if (appProc == null) {
            log("  ✗ app_process tidak ditemukan!");
            return;
        }
        log("  proc : " + appProc);

        // Launch — persis seperti LaunchOverlay() di overlay_launcher.h
        final String cmd =
            "export CLASSPATH=" + apkPath + "; " +
            appProc + " / uk.lgl.OverlayMain" +
            " > /data/local/tmp/overlay.log 2>&1 &";

        log("  CMD  : " + cmd);
        runSu(cmd);

        try { Thread.sleep(1500); } catch (Exception e) { /* */ }

        String pid = runSu("pgrep -f 'uk.lgl.OverlayMain' 2>/dev/null");
        if (pid != null && !pid.trim().isEmpty()) {
            log("  ✓ Overlay running! PID: " + pid.trim());
        } else {
            log("  ✗ Overlay gagal start!");
            String olg = runSu("cat /data/local/tmp/overlay.log 2>/dev/null");
            if (olg != null && !olg.trim().isEmpty()) {
                log("--- log ---");
                log(olg.trim());
                log("-----------");
            }
        }
    }

    // =========================================================================
    // Full inject flow
    //
    // launchOverlay() sudah deprecated — overlay kini dihandle injector binary.
    // fullInject() hanya menyiapkan libinternal.so dan wrap-script.
    // ModManager.launch(targetPackage) yang menjalankan injector binary.
    // =========================================================================
    public void fullInject(GameDetector.GameInfo game) {
        log("=== Prepare: " + game.appName + " ===");

        String abi = "arm64-v8a";
        if (game.nativeDir != null &&
            !game.nativeDir.contains("arm64") &&
            !game.nativeDir.contains("aarch64")) {
            abi = "armeabi-v7a";
        }
        log("ABI: " + abi);

        setSelinuxPermissive();
        extractInternal(abi);

        int pid = GameDetector.getGamePid(game.packageName);
        if (pid > 0) {
            log("Game running (PID " + pid + ") — injector akan attach");
        } else {
            log("Game not running — setup wrap script");
            setupLaunchWrapper(game.packageName);
            log("→ Buka game, injector akan tunggu prosesnya");
        }

        log("=== Preparation done ===");
    }

    private void setupLaunchWrapper(String pkg) {
        String soPath   = "/data/local/tmp/libinternal.so";
        String wrapPath = "/data/local/tmp/wrap." + pkg;
        runSu("printf '#!/system/bin/sh\\nLD_PRELOAD=" + soPath + " exec \"$@\"\\n' > " + wrapPath);
        runSu("chmod 755 " + wrapPath);
        String check = runSu("cat " + wrapPath);
        log("  ✓ wrap: " + wrapPath);
        log("  " + (check != null ? check.trim() : "empty"));
    }

    public void stopAll() {
        log("Stopping...");
        runSu("pkill -f 'uk.lgl.OverlayMain' 2>/dev/null");
        log("Stopped.");
    }

    public static String runSu(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
            java.io.BufferedReader br = new java.io.BufferedReader(
                new java.io.InputStreamReader(p.getInputStream()));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = br.readLine()) != null) sb.append(line).append("\n");
            p.waitFor();
            return sb.toString();
        } catch (Exception e) {
            Log.e(TAG, "runSu: " + e.getMessage());
            return null;
        }
    }

    public static boolean hasRoot() {
        String r = runSu("id");
        return r != null && r.contains("uid=0");
    }
}
