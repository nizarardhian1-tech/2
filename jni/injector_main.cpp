// =============================================================================
// injector_main.cpp — Executable injector untuk IL2Cpp Hybrid Tool
//
// CARA PAKAI (dijalankan oleh ModManager.java):
//   su -c "<binaryPath> <filesDir>"
//   argv[1] = filesDir (getFilesDir().getAbsolutePath())
//
// CONFIG FORMAT — filesDir/.tool_cfg (4 baris, ditulis ModManager.java):
//   Line 1 : apkPath   (getPackageCodePath())
//   Line 2 : nativeDir (getApplicationInfo().nativeLibraryDir — informational)
//   Line 3 : targetPkg (packageName game)
//   Line 4 : soPath    (path absolut ke libinternal.so yang sudah di-extract)
//             ↑ PENTING: ini path aktual, bukan nativeDir + "/libinternal.so"
//             Contoh: /data/local/tmp/libinternal.so
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

static std::string gLogPath1;
static std::string gLogPath2;

static void L(const char* msg) {
    if (!gLogPath1.empty()) {
        FILE* f = fopen(gLogPath1.c_str(), "a");
        if (f) { fprintf(f, "%s\n", msg); fclose(f); }
    }
    FILE* g = fopen(gLogPath2.c_str(), "a");
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

// =============================================================================
// ReadConfig — baca 4 baris dari .tool_cfg
// =============================================================================
static void ReadConfig(const std::string& cfgPath,
                       std::string& apkPath,
                       std::string& nativeDir,
                       std::string& targetPkg,
                       std::string& soPath) {
    FILE* f = fopen(cfgPath.c_str(), "r");
    if (!f) {
        Lf("CFG not found: %s", cfgPath.c_str());
        return;
    }
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
    readLine(soPath);
    fclose(f);
}

// =============================================================================
// verifyFile — cek file exist dan bisa dibaca, log ukurannya
// =============================================================================
static bool verifyFile(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        Lf("FILE_CHECK FAIL: %s (errno=%d)", path.c_str(), errno);
        return false;
    }
    Lf("FILE_CHECK OK: %s (size=%ld bytes)", path.c_str(), (long)st.st_size);
    return true;
}

// =============================================================================
// main()
// =============================================================================
__attribute__((visibility("default")))
int main(int argc, char* argv[]) {

    std::string filesDir;
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        filesDir = argv[1];
    } else {
        filesDir = "/data/local/tmp";
    }
    while (filesDir.size() > 1 && filesDir.back() == '/')
        filesDir.pop_back();

    const std::string cfgPath = filesDir + "/.tool_cfg";
    gLogPath1                 = filesDir + "/injector_log.txt";
    gLogPath2                 = "/data/local/tmp/injector_log.txt";

    remove(gLogPath1.c_str());
    remove(gLogPath2.c_str());
    chmod(filesDir.c_str(), 0777);

    L("started");
    Lf("filesDir : %s", filesDir.c_str());

    if (!CheckExpiry()) {
        L("EXPIRED");
        return 0;
    }

    // ─ Baca config (4 baris) ─────────────────────────────────────────────────
    std::string apkPath, nativeDir, targetPkg, soPath;
    ReadConfig(cfgPath, apkPath, nativeDir, targetPkg, soPath);

    if (apkPath.empty() || targetPkg.empty() || soPath.empty()) {
        Lf("CFG_INVALID apkPath='%s' targetPkg='%s' soPath='%s'",
           apkPath.c_str(), targetPkg.c_str(), soPath.c_str());
        return 1;
    }
    Lf("apkPath  : %s", apkPath.c_str());
    Lf("nativeDir: %s", nativeDir.c_str());
    Lf("targetPkg: %s", targetPkg.c_str());
    Lf("soPath   : %s", soPath.c_str());

    // ─ Verifikasi libinternal.so ada sebelum inject ───────────────────────────
    if (!verifyFile(soPath)) {
        L("ABORT: libinternal.so tidak ditemukan di soPath!");
        L("Pastikan Injector.java sudah ekstrak libinternal.so ke path tersebut.");
        return 1;
    }

    // ─ SELinux permissive ─────────────────────────────────────────────────────
    int seMode = NullUtils::SELINUX_GetEnforce();
    Lf("SELinux enforce: %d", seMode);
    if (seMode == 1) {
        system("setenforce 0");
        sleep(1);
        L("SELinux set permissive");
    }

    system("/system/bin/device_config put activity_manager max_phantom_processes 2147483647");
    system("/system/bin/settings put global settings_enable_monitor_phantom_procs false");

    // ─ Inject loop ───────────────────────────────────────────────────────────
    NullProcess::Process proc;
    bool injected = false;
    int  attempts = 0;

    for (int i = 0; i < 60 && !injected; i++) {
        if (proc.setProcByName(targetPkg)) {
            if (attempts == 0) Lf("Process found (try #%d), injecting...", i + 1);
            injected = proc.injectLibraryFromFile(soPath);
            attempts++;
            if (injected) L("Game found! Inject result=1");
            else {
                // Log setiap 5 detik agar log tidak spam
                if (attempts % 5 == 1) Lf("Inject result=0 (attempt #%d)", attempts);
            }
        } else {
            if (i == 0) L("Waiting for game process...");
        }
        if (!injected) sleep(1);
    }

    if (!injected) {
        Lf("GAGAL inject setelah %d percobaan", attempts);
    }

    // ─ Launch overlay ─────────────────────────────────────────────────────────
    // targetPkg dikirim sebagai argv[1] ke OverlayMain agar bisa konstruksi SHM path
    bool overlayOk = LaunchOverlay(apkPath.c_str(), targetPkg.c_str());
    L(overlayOk ? "Overlay launch=1" : "Overlay launch=0");

    L("Keepalive");
    while (true) { sleep(60); }
    return 0;
}
