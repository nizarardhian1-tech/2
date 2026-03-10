#pragma once
// =============================================================================
// overlay_launcher.h — Launch overlay via app_process + APK sebagai CLASSPATH
//
// LaunchOverlay(apkPath, gamePkg):
//   - gamePkg dikirim sebagai argv[1] ke OverlayMain.main(String[] args)
//   - OverlayMain pakai gamePkg untuk konstruksi SHM path:
//       /data/data/<gamePkg>/files/tool_esp.shm
//   - Tidak ada hardcoded package name di sisi overlay
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

// apkPath  = CLASSPATH (path APK mod kita)
// gamePkg  = package name game target, dikirim ke OverlayMain sebagai args[0]
//            sehingga OverlayMain tahu path SHM: /data/data/<gamePkg>/files/tool_esp.shm
static bool LaunchOverlay(const char* apkPath, const char* gamePkg = nullptr) {
    if (!apkPath || !apkPath[0]) return false;
    const char* appProc = _FindAppProcess();
    if (!appProc) return false;

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        setenv("CLASSPATH", apkPath, 1);
        if (gamePkg && gamePkg[0]) {
            // argv: appProc "/" OVERLAY_CLASS gamePkg null
            execl(appProc, appProc, "/", OVERLAY_CLASS, gamePkg, nullptr);
        } else {
            execl(appProc, appProc, "/", OVERLAY_CLASS, nullptr);
        }
        _exit(1);
    }
    sleep(1);
    return true;
}
