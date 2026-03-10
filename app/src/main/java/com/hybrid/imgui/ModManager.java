package com.hybrid.imgui;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * ModManager — Kelola lifecycle injector binary.
 *
 * Alur inject (mirip MLBB-Mod ModManager):
 *   1. writeConfig()    → tulis apkPath + nativeDir + targetPackage ke .tool_cfg
 *   2. extractBinary()  → ekstrak binary "injector" dari assets ke filesDir
 *   3. chmodBinary()    → chmod 755 via su
 *   4. launch()         → jalankan binary via su (background thread)
 *                         Binary menangani: ptrace inject + overlay launch + keepalive
 *
 * Binary di-bundle di assets/injector.
 * Mendukung dua mode build:
 *   - GitHub CI   : binary XOR-encrypted (key 0x4B) → auto-decrypt saat extract
 *   - AIDE local  : binary raw ELF                  → copy langsung
 * Deteksi otomatis via ELF magic bytes (0x7F 'E' 'L' 'F').
 */
public class ModManager {

    private static final String TAG         = "ModManager";
    static final  String        BINARY_NAME = "injector";

    // XOR key — harus sama dengan build.yml (BINARY_XOR_KEY: 75 = 0x4B)
    private static final byte XOR_KEY = 0x4B;

    private final Context mCtx;
    private Process       mProcess;
    private volatile boolean mRunning   = false;
    private volatile Boolean mRootCache = null;

    public interface StatusListener {
        void onLog(String msg);
        void onStateChanged(boolean running);
    }

    private StatusListener mListener;

    public ModManager(Context ctx)           { mCtx = ctx; }
    public void setListener(StatusListener l) { mListener = l; }

    // =========================================================================
    // ROOT CHECK (dengan cache — su hanya dipanggil sekali per sesi)
    // =========================================================================
    public boolean hasRoot() {
        if (mRootCache != null) return mRootCache;
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", "id"});
            p.waitFor();
            byte[] buf = new byte[256];
            int n = p.getInputStream().read(buf);
            p.destroy();
            mRootCache = new String(buf, 0, Math.max(n, 0)).contains("uid=0");
        } catch (Exception e) {
            mRootCache = false;
        }
        return mRootCache;
    }

    public void resetRootCache() { mRootCache = null; }

    // =========================================================================
    // WRITE CONFIG
    // Format .tool_cfg (3 baris):
    //   Line 1 : apkPath   (getPackageCodePath)
    //   Line 2 : nativeDir (getApplicationInfo().nativeLibraryDir)
    //   Line 3 : targetPkg (package name / process name game)
    // =========================================================================
    public boolean writeConfig(String targetPackage) {
        String apkPath   = mCtx.getPackageCodePath();
        String nativeDir = mCtx.getApplicationInfo().nativeLibraryDir;
        try {
            FileOutputStream out = new FileOutputStream(
                new File(mCtx.getFilesDir(), ".tool_cfg"));
            out.write((apkPath + "\n" + nativeDir + "\n" + targetPackage + "\n")
                .getBytes("UTF-8"));
            out.close();
            return true;
        } catch (IOException e) {
            log("writeConfig failed: " + e.getMessage());
            return false;
        }
    }

    // =========================================================================
    // EXTRACT BINARY FROM ASSETS
    //
    // Auto-detect enkripsi XOR vs raw ELF:
    //   Raw ELF     : 0x7F 0x45 0x4C 0x46
    //   XOR'd       : 0x34 0x0E 0x07 0x0D  (magic ^ 0x4B)
    // =========================================================================
    public boolean extractBinary() {
        File out = getBinaryFile();
        InputStream in   = null;
        FileOutputStream fos = null;
        try {
            in = mCtx.getAssets().open(BINARY_NAME);

            byte[] magic    = new byte[4];
            int    magicRead = in.read(magic, 0, 4);
            if (magicRead < 4) {
                log("Preparation failed. Invalid binary.");
                return false;
            }

            boolean isEncrypted =
                (magic[0] & 0xFF) == (0x7F ^ (XOR_KEY & 0xFF)) &&
                (magic[1] & 0xFF) == (0x45 ^ (XOR_KEY & 0xFF)) &&
                (magic[2] & 0xFF) == (0x4C ^ (XOR_KEY & 0xFF)) &&
                (magic[3] & 0xFF) == (0x46 ^ (XOR_KEY & 0xFF));

            boolean isRawElf =
                (magic[0] & 0xFF) == 0x7F &&
                (magic[1] & 0xFF) == 0x45 &&
                (magic[2] & 0xFF) == 0x4C &&
                (magic[3] & 0xFF) == 0x46;

            if (!isEncrypted && !isRawElf) {
                log("Preparation failed. Try reinstalling.");
                return false;
            }

            fos = new FileOutputStream(out);

            if (isEncrypted) {
                magic[0] ^= XOR_KEY; magic[1] ^= XOR_KEY;
                magic[2] ^= XOR_KEY; magic[3] ^= XOR_KEY;
            }
            fos.write(magic, 0, 4);

            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                if (isEncrypted) {
                    for (int i = 0; i < n; i++) buf[i] ^= XOR_KEY;
                }
                fos.write(buf, 0, n);
            }
            log("Binary extracted" + (isEncrypted ? " (decrypted)" : " (raw)") + ".");
            return true;
        } catch (IOException e) {
            log("Preparation failed. Try reinstalling.");
            return false;
        } finally {
            try { if (in  != null) in.close();  } catch (IOException ignored) {}
            try { if (fos != null) fos.close(); } catch (IOException ignored) {}
        }
    }

    public boolean chmodBinary() {
        return runSu("chmod 755 " + getBinaryPath());
    }

    // =========================================================================
    // LAUNCH INJECTOR
    //
    // Runnable di background thread — sama persis pola dengan MLBB-Mod.
    // 1. Tulis config
    // 2. Extract + chmod binary (kalau belum ada)
    // 3. Jalankan binary via su
    // 4. Tail log file bawaan binary → forward ke UI
    // =========================================================================
    public void launch(String targetPackage) {
        if (mRunning) { log("Already running."); return; }

        if (!writeConfig(targetPackage)) {
            log("Failed to start. Try again.");
            return;
        }

        if (!getBinaryFile().exists()) {
            if (!extractBinary()) return;
        }

        if (!chmodBinary()) {
            log("Failed to start. Make sure root is active.");
            return;
        }

        new Thread(new Runnable() {
            @Override public void run() {
                final String logFile = mCtx.getFilesDir() + "/injector_log.txt";
                try {
                    String execPath = getBinaryPath();

                    ProcessBuilder pb = new ProcessBuilder("su", "-c", execPath);
                    pb.redirectErrorStream(true);
                    mProcess = pb.start();

                    // Deteksi noexec (Android 10+ kadang larang execute di filesDir)
                    Thread.sleep(800);
                    try {
                        mProcess.exitValue();
                        mProcess.destroy();
                        // Fallback ke /data/local/tmp/
                        final String alt = "/data/local/tmp/" + BINARY_NAME;
                        runSu("cp " + getBinaryPath() + " " + alt);
                        runSu("chmod 755 " + alt);
                        pb = new ProcessBuilder("su", "-c", alt);
                        pb.redirectErrorStream(true);
                        mProcess = pb.start();
                    } catch (IllegalThreadStateException e) {
                        // Masih jalan di filesDir — OK
                    }

                    mRunning = true;
                    notifyState(true);
                    log("Injector active.");

                    // Tail log dari binary (non-blocking RandomAccessFile)
                    java.io.RandomAccessFile raf = null;
                    try {
                        java.io.File lf = new java.io.File(logFile);
                        for (int w = 0; w < 10 && !lf.exists(); w++)
                            Thread.sleep(500);
                        if (lf.exists()) raf = new java.io.RandomAccessFile(lf, "r");
                    } catch (Exception ignored) {}

                    while (mRunning) {
                        try {
                            if (raf != null) {
                                String line;
                                while ((line = raf.readLine()) != null)
                                    if (!line.isEmpty()) handleBinaryLog(line);
                            }
                            try {
                                int code = mProcess.exitValue();
                                log(code != 0 ? "Injector stopped. Try restarting." : "Injector stopped.");
                                break;
                            } catch (IllegalThreadStateException ignored) {}
                            Thread.sleep(500);
                        } catch (Exception e) { break; }
                    }
                    if (raf != null) try { raf.close(); } catch (Exception ignored) {}
                } catch (Exception e) {
                    log("Failed to start. Try again.");
                } finally {
                    mRunning = false;
                    mProcess = null;
                    notifyState(false);
                }
            }
        }).start();
    }

    // =========================================================================
    // LOG HANDLER — terjemahkan output binary ke pesan user-friendly
    // =========================================================================
    private void handleBinaryLog(String line) {
        if      (line.contains("started"))                                        log("Preparing...");
        else if (line.contains("Game found") || line.contains("Inject result=1")) log("Injected ✓");
        else if (line.contains("Inject result=0") || line.contains("GAGAL"))     log("Inject failed. Make sure the game is open.");
        else if (line.contains("Overlay launch=1"))                               log("Overlay active ✓");
        else if (line.contains("Overlay launch=0"))                               log("Overlay failed. Try restarting.");
        else if (line.contains("Keepalive"))                                      log("Running.");
        else if (line.contains("EXPIRED"))                                        log("Outdated version. Rebuild required.");
        else if (line.contains("Waiting for game"))                               log("Waiting for game process...");
        else if (line.contains("SELinux"))                                        log("SELinux: permissive ✓");
    }

    // =========================================================================
    // STOP
    // =========================================================================
    public void stop() {
        runSu("pkill -f " + BINARY_NAME);
        runSu("pkill -f uk.lgl.OverlayMain");
        if (mProcess != null) { mProcess.destroy(); mProcess = null; }
        mRunning = false;
        notifyState(false);
        log("Injector stopped.");
    }

    public boolean isRunning() {
        if (!mRunning || mProcess == null) return false;
        try {
            mProcess.exitValue();
            mRunning = false;
            notifyState(false);
            return false;
        } catch (IllegalThreadStateException e) {
            return true;
        }
    }

    // =========================================================================
    // REINSTALL (update binary tanpa uninstall APK)
    // =========================================================================
    public void reinstall() {
        stop();
        File old = getBinaryFile();
        if (old.exists()) old.delete();
        if (extractBinary() && chmodBinary())
            log("Binary updated ✓");
        else
            log("Update failed. Try reinstalling.");
    }

    // =========================================================================
    // HELPERS
    // =========================================================================
    private File   getBinaryFile() { return new File(mCtx.getFilesDir(), BINARY_NAME); }
    private String getBinaryPath() { return getBinaryFile().getAbsolutePath(); }

    private boolean runSu(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
            p.waitFor();
            boolean ok = p.exitValue() == 0;
            p.destroy();
            return ok;
        } catch (Exception e) { return false; }
    }

    private void log(final String msg) {
        Log.d(TAG, msg);
        if (mListener != null) mListener.onLog(msg);
    }

    private void notifyState(final boolean r) {
        if (mListener != null) mListener.onStateChanged(r);
    }
}
