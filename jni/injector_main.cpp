// =============================================================================
// injector_main.cpp — Executable injector untuk IL2Cpp Hybrid Tool
// Dibuat dari cheat_main.cpp (MLBB-Mod), diadaptasi untuk proyek Tool.
//
// CARA KERJA:
//   1. Baca config dari filesDir/.tool_cfg  (3 baris: apkPath / nativeDir / targetPkg)
//   2. Tunggu proses target sampai muncul (max 60 detik)
//   3. Inject libinternal.so via ptrace
//   4. Launch overlay via app_process + CLASSPATH=APK
//   5. Keepalive loop
//
// DIJALANKAN via:
//   su -c /data/data/com.hybrid.imgui/files/injector
// =============================================================================

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <stdarg.h>
#include <string>
#include <sys/stat.h>

#include "BuildExpiry.h"
#include "injector/trace/nullproc.h"
#include "injector/trace/nullutils.h"
#include "overlay_launcher.h"

// ── Paths ─────────────────────────────────────────────────────────────────────
static const char* LOG_PATH_1 = "/data/data/com.hybrid.imgui/files/injector_log.txt";
static const char* LOG_PATH_2 = "/data/local/tmp/injector_log.txt";
static const char* CFG_PATH   = "/data/data/com.hybrid.imgui/files/.tool_cfg";

// ── Logging ───────────────────────────────────────────────────────────────────
static void L(const char* msg) {
    FILE* f = fopen(LOG_PATH_1, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    FILE* g = fopen(LOG_PATH_2, "a");
    if (g) { fprintf(g, "%s\n", msg); fclose(g); }
}

static void Lf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    L(buf);
}

// ── Config reader ─────────────────────────────────────────────────────────────
// Format .tool_cfg (3 baris):
//   Line 1 : apkPath   (hasil getPackageCodePath())
//   Line 2 : nativeDir (hasil getApplicationInfo().nativeLibraryDir)
//   Line 3 : targetPkg (package:process name, misal "com.tencent.ig:UnityMain")
static void ReadConfig(std::string& apkPath, std::string& nativeDir, std::string& targetPkg) {
    FILE* f = fopen(CFG_PATH, "r");
    if (!f) { L("CFG not found: " CFG_PATH); return; }

    auto readLine = [&](std::string& out) {
        char buf[512] = {};
        if (fgets(buf, sizeof(buf), f)) {
            out = buf;
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
                out.pop_back();
        }
    };

    readLine(apkPath);
    readLine(nativeDir);
    readLine(targetPkg);
    fclose(f);
}

// ── Entry point ───────────────────────────────────────────────────────────────
__attribute__((visibility("default")))
int main() {
    // Bersihkan log lama
    remove(LOG_PATH_1);
    remove(LOG_PATH_2);
    chmod("/data/data/com.hybrid.imgui/files", 0777);

    L("started");

    // Build expiry check
    if (!CheckExpiry()) {
        L("EXPIRED");
        return 0;
    }

    // Baca config
    std::string apkPath, nativeDir, targetPkg;
    ReadConfig(apkPath, nativeDir, targetPkg);

    if (apkPath.empty() || nativeDir.empty() || targetPkg.empty()) {
        L("CFG invalid — apkPath/nativeDir/targetPkg kosong");
        return 1;
    }

    Lf("apkPath  : %s", apkPath.c_str());
    Lf("nativeDir: %s", nativeDir.c_str());
    Lf("targetPkg: %s", targetPkg.c_str());

    std::string internalSoPath = nativeDir + "/libinternal.so";
    Lf("soPath   : %s", internalSoPath.c_str());

    // SELinux permissive
    int seMode = NullUtils::SELINUX_GetEnforce();
    if (seMode == 1) {
        NullUtils::SELINUX_SetEnforce(0);
        sleep(1);
        L("SELinux set permissive");
    }

    // Phantom process bypass (Android 12+)
    system("/system/bin/device_config put activity_manager max_phantom_processes 2147483647");
    system("/system/bin/settings put global settings_enable_monitor_phantom_procs false");

    // ── Inject libinternal.so ke proses target ────────────────────────────────
    // Coba sampai 60 detik — game mungkin belum fully loaded
    NullProcess::Process proc;
    bool injected = false;

    for (int i = 0; i < 60 && !injected; i++) {
        if (proc.setProcByName(targetPkg.c_str())) {
            injected = proc.injectLibraryFromFile(internalSoPath);
            if (injected) L("Game found! Inject result=1");
            else           L("Inject result=0");
        } else {
            if (i == 0) L("Waiting for game process...");
        }
        if (!injected) sleep(1);
    }

    if (!injected) {
        L("GAGAL inject — game tidak ditemukan atau inject gagal");
    }

    // ── Launch overlay via app_process + CLASSPATH=APK ────────────────────────
    // LaunchOverlay() ada di overlay_launcher.h — persis sama dengan MLBB
    bool overlayOk = LaunchOverlay(apkPath.c_str());
    L(overlayOk ? "Overlay launch=1" : "Overlay launch=0");

    L("Keepalive");
    while (true) { sleep(60); }
    return 0;
}
