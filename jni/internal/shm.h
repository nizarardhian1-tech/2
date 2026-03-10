#pragma once
// =============================================================================
// shm.h — SHM init/cleanup untuk libinternal.so
//
// Cara kerja:
//   1. _GetPackageName() baca /proc/self/cmdline → dapat pkg name game
//   2. _GetSHMPath() buat path /data/data/<pkg>/files/tool_esp.shm
//   3. SHM_Init() buat/buka file, ftruncate, mmap
//   4. SHM_Cleanup() munmap + unlink
//
// g_shm adalah pointer ke SharedData yang sudah di-mmap.
// Tulis ke g_shm dari thread manapun (pakai seqlock untuk entity data).
// =============================================================================

#include <string>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <android/log.h>

#include "../shared_data.h"

#define SHM_LOG(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, "SHM", fmt, ##__VA_ARGS__)

static char        g_shm_real_path[256] = {0};
static SharedData* g_shm                = nullptr;
static int         g_shm_fd             = -1;

// Baca package name dari /proc/self/cmdline (sama persis dengan MLBB-Mod)
static std::string _GetPackageName() {
    char buf[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return "";
    read(fd, buf, sizeof(buf) - 1);
    close(fd);
    std::string pkg(buf);
    // cmdline format: "com.pkg.name:processName" atau "com.pkg.name"
    size_t colon = pkg.find(':');
    if (colon != std::string::npos) pkg = pkg.substr(0, colon);
    // Hapus null chars
    size_t nul = pkg.find('\0');
    if (nul != std::string::npos) pkg = pkg.substr(0, nul);
    return pkg;
}

static void _MakeDir(const std::string& path) {
    mkdir(path.c_str(), 0777);
    chmod(path.c_str(), 0777);
}

// Return path lengkap SHM file, misal:
//   /data/data/com.ohzegame.ramboshooter.brothersquad/files/tool_esp.shm
static std::string _GetSHMPath() {
    std::string pkg = _GetPackageName();
    if (pkg.empty()) {
        SHM_LOG("WARNING: pkg kosong, fallback ke /data/local/tmp");
        return "/data/local/tmp/" SHM_FILENAME;
    }
    SHM_LOG("Package: %s", pkg.c_str());
    std::string dir = "/data/data/" + pkg + "/files";
    _MakeDir(dir);
    return dir + "/" SHM_FILENAME;
}

static bool SHM_Init() {
    std::string path = _GetSHMPath();
    snprintf(g_shm_real_path, sizeof(g_shm_real_path), "%s", path.c_str());
    SHM_LOG("SHM path: %s", g_shm_real_path);

    // Hapus file lama kalau ada (ukuran mungkin beda)
    unlink(g_shm_real_path);

    g_shm_fd = open(g_shm_real_path, O_CREAT | O_RDWR, 0777);
    if (g_shm_fd < 0) {
        SHM_LOG("GAGAL open SHM file (errno=%d)", errno);
        return false;
    }

    fchmod(g_shm_fd, 0777);
    chmod(g_shm_real_path, 0777);
    ftruncate(g_shm_fd, sizeof(SharedData));

    g_shm = (SharedData*)mmap(nullptr, sizeof(SharedData),
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        SHM_LOG("GAGAL mmap (errno=%d)", errno);
        g_shm = nullptr;
        return false;
    }

    memset(g_shm, 0, sizeof(SharedData));
    g_shm->version = SHM_VERSION;
    g_shm->ready   = false;
    SHM_LOG("SHM init OK, sizeof(SharedData)=%zu", sizeof(SharedData));
    return true;
}

static void SHM_Cleanup() {
    if (g_shm)      { munmap(g_shm, sizeof(SharedData)); g_shm = nullptr; }
    if (g_shm_fd >= 0) { close(g_shm_fd); g_shm_fd = -1; }
    if (g_shm_real_path[0]) unlink(g_shm_real_path);
}
