#pragma once
// =============================================================================
// overlay_launcher.h — Launch overlay via app_process + APK sebagai CLASSPATH
// uk.lgl.OverlayMain sudah terkompile di dalam APK kita (src/uk/lgl/)
// Tidak perlu overlay.dex terpisah
// =============================================================================
#include <unistd.h>
#include <sys/wait.h>

#define OVERLAY_CLASS "uk.lgl.OverlayMain"

static const char* _FindAppProcess() {
    static const char* k[] = {
        "/system/bin/app_process64",
        "/system/bin/app_process32",
        "/system/bin/app_process",
        nullptr
    };
    for (int i = 0; k[i]; i++)
        if (access(k[i], X_OK) == 0) return k[i];
    return nullptr;
}

static bool LaunchOverlay(const char* apkPath) {
    if (!apkPath || !apkPath[0]) return false;
    const char* appProc = _FindAppProcess();
    if (!appProc) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setenv("CLASSPATH", apkPath, 1);
        execl(appProc, appProc, "/", OVERLAY_CLASS, nullptr);
        _exit(1);
    }
    sleep(1);
    return true;
}
