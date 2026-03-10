#include "Utils.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "../Il2cpp/xdl/include/xdl.h"
#include <cerrno>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unordered_map>
#include <string>
#include <mutex>
#include "obfuscate.h"
#include "Logger.h"

// ─── TAMBAHAN FIX RVA ────────────────────────────────────────────────────────
// Mengambil base address akurat dari Dumper (hasil dari dladdr di Il2cpp.cpp)
extern uint64_t il2cpp_base;
// ─────────────────────────────────────────────────────────────────────────────

// ─── GetLibBase — cached, stabil antar panggilan ─────────────────────────────
// Cache: nama library → base address (uintptr_t)
static std::unordered_map<std::string, uintptr_t> s_libBaseCache;
static std::mutex s_libBaseMutex;

uintptr_t GetLibBase(const char *libraryName)
{
    if (!libraryName || libraryName[0] == '\0') return 0;

    // ─── FIX: Jika yang dicari libil2cpp.so, selalu gunakan il2cpp_base ──────
    // Ini menjamin kalkulasi RVA (offset) persis sama dengan Dumper / DnSpy
    if (strcmp(libraryName, "libil2cpp.so") == 0 && il2cpp_base != 0) {
        return (uintptr_t)il2cpp_base;
    }
    // ─────────────────────────────────────────────────────────────────────────

    {
        std::lock_guard<std::mutex> lock(s_libBaseMutex);
        auto it = s_libBaseCache.find(libraryName);
        if (it != s_libBaseCache.end()) return it->second;
    }

    // Baca /proc/self/maps untuk library lain (selain libil2cpp.so)
    uintptr_t base = 0;
    FILE *fp = fopen(OBFUSCATE("/proc/self/maps"), OBFUSCATE("r"));
    if (!fp) return 0;

    char line[512];
    while (fgets(line, sizeof(line), fp))
    {
        if (!strstr(line, libraryName)) continue;

        // Format: start-end perms offset dev:inode pathname
        uintptr_t start = 0;
        sscanf(line, "%lx", &start);
        if (start != 0)
        {
            base = start;
            break; // ambil yang pertama saja (terendah)
        }
    }
    fclose(fp);

    if (base)
    {
        std::lock_guard<std::mutex> lock(s_libBaseMutex);
        s_libBaseCache[libraryName] = base;
        LOGI("[GetLibBase] %s -> 0x%lX", libraryName, (unsigned long)base);
    }
    else
    {
        LOGE("[GetLibBase] Library tidak ditemukan: %s", libraryName);
    }
    return base;
}

// Kompatibilitas dengan kode lama
DWORD findLibrary(const char *library)
{
    return (DWORD)GetLibBase(library);
}

// ─── Memory Safety ───────────────────────────────────────────────────────────
bool IsPtrValid(const void *addr)
{
    if (!addr) return false;
    if (reinterpret_cast<uintptr_t>(addr) < 4096UL) return false;

    static const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;

    uintptr_t page_start = reinterpret_cast<uintptr_t>(addr)
                           & ~static_cast<uintptr_t>(page_size - 1);

    unsigned char vec = 0;
    int ret = mincore(reinterpret_cast<void *>(page_start), 1, &vec);
    return (ret == 0);
}

// ─── Utility ──────────────────────────────────────────────────────────────────

static uintptr_t s_libBase = 0;
static bool s_libLoaded = false;

DWORD getAbsoluteAddress(const char *libraryName, DWORD relativeAddr)
{
    uintptr_t base = GetLibBase(libraryName);
    if (base == 0) return 0;
    return (DWORD)(base + relativeAddr);
}

jboolean isGameLibLoaded(JNIEnv */*env*/, jobject /*thiz*/)
{
    return s_libLoaded;
}

bool isLibraryLoaded(const char *libraryName)
{
    // ── Primary: xdl_open ─────────────────────────────────────────────────────
    // XDL_TRY_FORCE_LOAD: jika lib belum di-load, coba force load dulu.
    // Kalau berhasil = lib sudah siap di-resolve symbolnya (tidak hanya mapped).
    // Works untuk extracted maupun embedded di split APK.
    void *h = xdl_open(libraryName, XDL_TRY_FORCE_LOAD);
    if (h) {
        xdl_close(h);
        s_libLoaded = true;
        return true;
    }

    // ── Fallback: /proc/self/maps ─────────────────────────────────────────────
    // Untuk lib yang di-extract ke disk tapi xdl belum sync.
    char line[512] = {0};
    FILE *fp = fopen(OBFUSCATE("/proc/self/maps"), OBFUSCATE("rt"));
    if (!fp) return false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, libraryName)) {
            s_libLoaded = true;
            fclose(fp);
            return true;
        }
    }
    fclose(fp);
    return false;
}

uintptr_t string2Offset(const char *c)
{
    int base = 16;
    static_assert(sizeof(uintptr_t) == sizeof(unsigned long) ||
                  sizeof(uintptr_t) == sizeof(unsigned long long),
                  "Please handle conversion for this architecture.");

    if (sizeof(uintptr_t) == sizeof(unsigned long))
        return strtoul(c, nullptr, base);

    return strtoull(c, nullptr, base);
}
// ─── Target Library untuk kalkulasi RVA ──────────────────────────────────────
static std::string s_targetLib = "libil2cpp.so";

const char* GetTargetLib() {
    return s_targetLib.c_str();
}

void SetTargetLib(const char* libName) {
    if (!libName || libName[0] == '\0') return;
    if (s_targetLib == libName) return;
    s_targetLib = libName;
    // Reset cache GetLibBase agar lib baru di-resolve fresh
    {
        std::lock_guard<std::mutex> lock(s_libBaseMutex);
        s_libBaseCache.erase(s_targetLib);
    }
    LOGI("[SetTargetLib] Target library RVA: %s", s_targetLib.c_str());
}
