package com.hybrid.imgui;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.drawable.Drawable;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public class GameDetector {

    public static class GameInfo {
        public String packageName;
        public String appName;
        public Drawable icon;
        public String libPath;      // path ke libil2cpp.so
        public String nativeDir;    // path ke native lib dir

        @Override
        public String toString() { return appName + " (" + packageName + ")"; }
    }

    /**
     * Scan semua app terinstall, kembalikan yang punya libil2cpp.so
     * (= Unity IL2Cpp game)
     */
    public static List<GameInfo> detectUnityGames(Context ctx) {
        List<GameInfo> result = new ArrayList<>();
        PackageManager pm = ctx.getPackageManager();

        List<ApplicationInfo> apps;
        try {
            apps = pm.getInstalledApplications(PackageManager.GET_META_DATA);
        } catch (Exception e) {
            return result;
        }

        for (ApplicationInfo ai : apps) {
            // Skip system apps
            if ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0) continue;

            // Cari libil2cpp.so di native lib dir
            String[] possibleDirs = {
                ai.nativeLibraryDir,
                "/data/app/" + ai.packageName + "/lib/arm64",
                "/data/app/" + ai.packageName + "/lib/arm",
            };

            String il2cppPath = null;
            String nativeDir  = null;
            for (String dir : possibleDirs) {
                if (dir == null) continue;
                File f = new File(dir, "libil2cpp.so");
                if (f.exists()) {
                    il2cppPath = f.getAbsolutePath();
                    nativeDir  = dir;
                    break;
                }
                // Coba libcsharp.so (MLBB / beberapa Unity versi baru)
                File f2 = new File(dir, "libcsharp.so");
                if (f2.exists()) {
                    il2cppPath = f2.getAbsolutePath();
                    nativeDir  = dir;
                    break;
                }
            }

            if (il2cppPath != null) {
                GameInfo info  = new GameInfo();
                info.packageName = ai.packageName;
                info.nativeDir   = nativeDir;
                info.libPath     = il2cppPath;
                try {
                    info.appName = pm.getApplicationLabel(ai).toString();
                    info.icon    = pm.getApplicationIcon(ai);
                } catch (Exception e) {
                    info.appName = ai.packageName;
                }
                result.add(info);
            }
        }

        // Sort by name
        Collections.sort(result, (a, b) -> a.appName.compareToIgnoreCase(b.appName));
        return result;
    }

    /**
     * Cek apakah game sedang berjalan, kembalikan PID atau -1
     */
    public static int getGamePid(String packageName) {
        try {
            // Baca /proc untuk cari PID
            File procDir = new File("/proc");
            File[] pids  = procDir.listFiles();
            if (pids == null) return -1;

            for (File pidFile : pids) {
                String name = pidFile.getName();
                if (!name.matches("\\d+")) continue;

                File cmdline = new File(pidFile, "cmdline");
                if (!cmdline.exists()) continue;

                try {
                    java.io.BufferedReader br = new java.io.BufferedReader(
                        new java.io.FileReader(cmdline));
                    String line = br.readLine();
                    br.close();
                    if (line != null && line.contains(packageName)) {
                        // Pastikan ini main process (bukan :service dll)
                        String trimmed = line.replace("\0", "").trim();
                        if (trimmed.equals(packageName)) {
                            return Integer.parseInt(name);
                        }
                    }
                } catch (Exception e) { /* skip */ }
            }
        } catch (Exception e) { /* skip */ }
        return -1;
    }
}
