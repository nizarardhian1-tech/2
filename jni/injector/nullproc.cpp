#include "trace/nullproc.h"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <android/log.h>

// Logging helper — tulis ke logcat DAN ke stderr (visible di injector_log jika ada)
#define ILOG(fmt, ...) \
    __android_log_print(ANDROID_LOG_DEBUG, "NullInject", fmt, ##__VA_ARGS__); \
    fprintf(stderr, "[NullInject] " fmt "\n", ##__VA_ARGS__)

NullProcess::Process::Process() {
    this->pid     = -1;
    this->pkgName = "UNDEFINED";
}

bool NullProcess::Process::setProcByPid(pid_t procID) {
    std::ifstream cmdlineFile("/proc/" + std::to_string(procID) + "/cmdline");
    if (!cmdlineFile) return false;
    this->pid  = procID;
    this->maps = getMaps(this->pid);
    return true;
}

bool NullProcess::Process::setProcByName(const std::string& pkg) {
    this->pkgName = pkg;
    this->pid     = -1;

    DIR* dir = opendir("/proc");
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;

        char* endptr;
        long potential_pid = strtol(entry->d_name, &endptr, 10);
        if (*endptr != '\0') continue;

        std::string cmdline_path = std::string("/proc/") + entry->d_name + "/cmdline";
        std::ifstream cmdlineFile(cmdline_path);
        std::string   pkgNameBuffer;

        if (getline(cmdlineFile, pkgNameBuffer)) {
            pkgNameBuffer = NullUtils::removeNullChars(pkgNameBuffer);
            if (pkgNameBuffer == pkg) {
                this->pid  = static_cast<pid_t>(potential_pid);
                this->maps = getMaps(this->pid);
                this->locateSymbols();
                break;
            }
        }
    }
    closedir(dir);
    return pid != -1;
}

std::vector<NullProcess::Map> NullProcess::Process::getMaps(const pid_t& pid) {
    std::vector<NullProcess::Map> maps;
    std::string path = "/proc/" + std::to_string(pid) + "/maps";

    std::ifstream file(path);
    if (!file.is_open()) return {};

    std::string line;
    while (std::getline(file, line))
        maps.push_back(NullProcess::Process::parseMap(line));

    return maps;
}

bool NullProcess::Process::writeProcessMemory(std::string hex, uintptr_t address) {
    std::vector<uint8_t> hexVec = NullUtils::interpretHex(std::move(hex));
    if (hexVec.empty()) return false;

    if (!NullTrace::ptraceAttachWithRetry(this->pid)) return false;

    bool ok = NullTrace::ptraceWrite(this->pid, address, hexVec.data(), hexVec.size());

    ptrace(PTRACE_DETACH, this->pid, nullptr, nullptr);
    return ok;
}

std::string NullProcess::Process::readProcessMemory(uintptr_t address, size_t len) {
    if (!NullTrace::ptraceAttachWithRetry(this->pid)) return "";

    uint8_t readData[len];
    bool ok = NullTrace::ptraceRead(this->pid, address, readData, len);

    ptrace(PTRACE_DETACH, this->pid, nullptr, nullptr);
    if (!ok) return "";

    return NullUtils::bytesToHex(readData, len);
}

// ============================================================================
// injectLibraryFromFile — baca .so dari disk lalu inject via memfd
// ============================================================================
bool NullProcess::Process::injectLibraryFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        ILOG("injectLibraryFromFile: GAGAL buka file '%s' (errno=%d: %s)",
             path.c_str(), errno, strerror(errno));
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        ILOG("injectLibraryFromFile: GAGAL baca file (size=%ld)", (long)size);
        return false;
    }
    file.close();
    ILOG("injectLibraryFromFile: baca %ld bytes dari '%s'", (long)size, path.c_str());

    return this->injectLibraryMemfd(buffer.data(), buffer.size());
}

// ============================================================================
// injectLibrary — inject via dlopen path (fallback)
// ============================================================================
bool NullProcess::Process::injectLibrary(std::string path) {
    struct stat buffer;
    if (stat(path.c_str(), &buffer) != 0) return false;

    bool shouldEmuInject = false;

#if defined(__x86_64__) || defined(__i386__)
    int libArch = NullElf::getLibraryArch(path.c_str());
    if (libArch == NullElf::EARM || libArch == NullElf::EAARCH64)
        shouldEmuInject = true;
#else
    (void)NullElf::getLibraryArch(path.c_str());
#endif

#if defined(__arm__) || defined(__aarch64__)
    if (!this->libc.remote_malloc || !this->libc.remote_free ||
        !this->libdl.remote_dlopen || !this->libdl.remote_dlsym) {
        return false;
    }
#elif defined(__i386__) || defined(__x86_64__)
    if (!this->libc.remote_malloc || !this->libc.remote_free) return false;
    if (!this->libdl.remote_dlopen || !this->libdl.remote_dlsym)
        shouldEmuInject = true;
#endif

    void* pathAddr = this->remoteString(path);
    if (pathAddr == nullptr) return false;

    uintptr_t libLR_base = 0x0;
    for (const NullProcess::Map& map : this->maps) {
#if defined(__arm__) || defined(__aarch64__)
        if (map.pathName.find("libart.so") != std::string::npos) {
            libLR_base = map.start; break;
        }
        if (map.pathName.find("libRS.so") != std::string::npos) {
            libLR_base = map.start; break;
        }
#endif
    }
    if (!libLR_base) {
        for (const NullProcess::Map& map : this->maps) {
#if defined(__arm__) || defined(__aarch64__)
            if (map.pathName.find("libc.so") != std::string::npos) {
                libLR_base = map.start; break;
            }
#endif
        }
    }

    void* handle = this->callR<void*>(libLR_base, this->libdl.remote_dlopen,
                                      pathAddr, RTLD_NOW | RTLD_GLOBAL);
    this->call<void>(this->libc.remote_free, pathAddr);
    return handle != nullptr;
}

bool NullProcess::Process::injectLibNB(void* pathAddr, uintptr_t libLR_base) {
    if (!this->nbInfo.nativeBridgeActive) return false;

    void* handle = nullptr;
    if (this->nbInfo.usesCallbacksPtr) {
        if (NullUtils::getApiLevel() >= 26)
            handle = this->callR<void*>(libLR_base,
                reinterpret_cast<uintptr_t>(nbCallbacks.loadLibraryExt),
                pathAddr, RTLD_NOW | RTLD_GLOBAL, 3);
        else
            handle = this->callR<void*>(libLR_base,
                reinterpret_cast<uintptr_t>(nbCallbacks.loadLibrary),
                RTLD_GLOBAL | RTLD_NOW);
    } else {
        if (NullUtils::getApiLevel() >= 26)
            handle = this->callR<void*>(libLR_base,
                libnativebridge.remote_loadLibraryExt,
                pathAddr, RTLD_NOW | RTLD_GLOBAL, 0);
        else
            handle = this->callR<void*>(libLR_base,
                libnativebridge.remote_loadLibrary,
                pathAddr, RTLD_GLOBAL | RTLD_NOW);
    }
    return handle != nullptr;
}

// ============================================================================
// injectLibraryMemfd — inject dari buffer memory menggunakan memfd_create
// Strategy: buat memfd di proses game via remote syscall, tulis bytes,
//           game dlopen("/proc/self/fd/<n>") — bypass SELinux file context
// ============================================================================
bool NullProcess::Process::injectLibraryMemfd(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        ILOG("injectLibraryMemfd: data kosong");
        return false;
    }

    // Cek symbols — jika locateSymbols() gagal, ini akan 0
    if (!this->libc.remote_malloc || !this->libc.remote_free ||
        !this->libdl.remote_dlopen) {
        ILOG("injectLibraryMemfd: symbols belum siap! malloc=%p free=%p dlopen=%p",
             (void*)this->libc.remote_malloc,
             (void*)this->libc.remote_free,
             (void*)this->libdl.remote_dlopen);
        return false;
    }
    ILOG("injectLibraryMemfd: symbols OK. malloc=%p dlopen=%p",
         (void*)this->libc.remote_malloc, (void*)this->libdl.remote_dlopen);

    // Cari syscall() di libc remote
    uintptr_t remote_syscall = 0;
    for (const NullProcess::Map& map : this->maps) {
        if (NullUtils::endsWith(map.pathName, "libc.so")) {
            uintptr_t sym = NullElf::getAddrSym(map.pathName.c_str(), "syscall");
            if (sym) {
                remote_syscall = map.start + sym;
                ILOG("remote syscall @ 0x%lx (libc: %s)", (unsigned long)remote_syscall, map.pathName.c_str());
                break;
            }
        }
    }
    if (!remote_syscall) {
        ILOG("injectLibraryMemfd: GAGAL temukan syscall di libc remote!");
        ILOG("Daftar maps libc:");
        for (const NullProcess::Map& map : this->maps) {
            if (map.pathName.find("libc") != std::string::npos)
                ILOG("  %s", map.pathName.c_str());
        }
        return false;
    }

    // Remote call memfd_create di proses game
    void* nameAddr = this->remoteString("dalvik-jit-code-cache");
    if (!nameAddr) {
        ILOG("injectLibraryMemfd: GAGAL remoteString (ptrace attach gagal?)");
        // Test ptrace manual
        if (!NullTrace::ptraceAttachWithRetry(this->pid)) {
            ILOG("ptrace ATTACH GAGAL untuk PID=%d (errno=%d: %s)",
                 this->pid, errno, strerror(errno));
        } else {
            ILOG("ptrace attach OK, tapi remoteString gagal");
            ptrace(PTRACE_DETACH, this->pid, nullptr, nullptr);
        }
        return false;
    }

    int remote_fd = static_cast<int>(
        this->call<uintptr_t>(remote_syscall,
            static_cast<uintptr_t>(__NR_memfd_create),
            reinterpret_cast<uintptr_t>(nameAddr),
            static_cast<uintptr_t>(0))
    );
    this->call<void>(this->libc.remote_free, nameAddr);

    if (remote_fd < 0) {
        ILOG("injectLibraryMemfd: memfd_create di game gagal! fd=%d", remote_fd);
        return false;
    }
    ILOG("remote memfd fd=%d", remote_fd);

    // Tulis bytes .so ke /proc/<game_pid>/fd/<remote_fd>
    std::string gameFdPath = "/proc/" + std::to_string(this->pid) +
                             "/fd/" + std::to_string(remote_fd);
    int localFd = open(gameFdPath.c_str(), O_WRONLY);
    if (localFd < 0) {
        ILOG("injectLibraryMemfd: GAGAL buka game fd path '%s' (errno=%d: %s)",
             gameFdPath.c_str(), errno, strerror(errno));
        return false;
    }
    if (ftruncate(localFd, static_cast<off_t>(size)) < 0) {
        ILOG("injectLibraryMemfd: ftruncate gagal");
        close(localFd);
        return false;
    }
    size_t written = 0;
    while (written < size) {
        ssize_t w = write(localFd, data + written, size - written);
        if (w <= 0) {
            ILOG("injectLibraryMemfd: write gagal (errno=%d: %s)", errno, strerror(errno));
            close(localFd);
            return false;
        }
        written += w;
    }
    close(localFd);
    ILOG("tulis %zu bytes ke %s OK", size, gameFdPath.c_str());

    // Game dlopen("/proc/self/fd/<n>")
    std::string selfFdPath = "/proc/self/fd/" + std::to_string(remote_fd);
    void* pathAddr = this->remoteString(selfFdPath);
    if (!pathAddr) {
        ILOG("injectLibraryMemfd: GAGAL remoteString selfFdPath");
        return false;
    }

    uintptr_t libLR_base = 0;
    for (const NullProcess::Map& map : this->maps) {
        if (map.pathName.find("libart.so") != std::string::npos) {
            libLR_base = map.start; break;
        }
    }
    if (!libLR_base) {
        for (const NullProcess::Map& map : this->maps) {
            if (map.pathName.find("libc.so") != std::string::npos) {
                libLR_base = map.start; break;
            }
        }
    }
    ILOG("libLR_base = 0x%lx", (unsigned long)libLR_base);

    void* handle = this->callR<void*>(libLR_base, this->libdl.remote_dlopen,
                                      pathAddr, RTLD_NOW | RTLD_GLOBAL);
    this->call<void>(this->libc.remote_free, pathAddr);

    if (!handle) {
        ILOG("injectLibraryMemfd: dlopen GAGAL! handle=null");
        // Coba baca dlerror
        uintptr_t errMsg = this->call<uintptr_t>(this->libdl.remote_dlerror);
        if (errMsg) {
            std::vector<uint8_t> errChars =
                NullUtils::interpretHex(this->readProcessMemory(errMsg, 256));
            std::string errStr;
            for (unsigned char c : errChars) {
                if (c == 0) break;
                errStr += (char)c;
            }
            ILOG("dlerror: %s", errStr.c_str());
        }
        return false;
    }

    ILOG("injectLibraryMemfd: BERHASIL! handle=%p", handle);
    return true;
}

void* NullProcess::Process::remoteString(std::string str) {
    size_t strSize = str.size();

    void* strAddr = this->call<void*>(this->libc.remote_malloc, strSize + 1);
    if (strAddr == nullptr) return nullptr;

    if (!NullTrace::ptraceAttachWithRetry(this->pid)) return nullptr;

    if (!NullTrace::ptraceWrite(this->pid, reinterpret_cast<uintptr_t>(strAddr),
                                (uint8_t*)str.c_str(), strSize + 1)) {
        ptrace(PTRACE_DETACH, this->pid, nullptr, nullptr);
        this->call<void>(this->libc.remote_free, strAddr);
        return nullptr;
    }

    if (ptrace(PTRACE_DETACH, this->pid, nullptr, nullptr) == -1) return nullptr;

    return strAddr;
}

bool NullProcess::Process::processExists(pid_t pid) {
    DIR* dir = opendir("/proc");
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        char* endptr;
        long dir_pid = strtol(entry->d_name, &endptr, 10);
        if (*endptr == '\0' && dir_pid == pid) {
            closedir(dir);
            return true;
        }
    }
    closedir(dir);
    return false;
}

// ============================================================================
// locateSymbols — temukan malloc/free/dlopen di peta memori proses game
// ============================================================================
void NullProcess::Process::locateSymbols() {
    this->libdl.remote_dlopen                   = 0x0;
    this->libdl.remote_dlerror                  = 0x0;
    this->libdl.remote_dlsym                    = 0x0;
    this->libc.remote_malloc                    = 0x0;
    this->libc.remote_free                      = 0x0;
    this->libc.remote_mmap                      = 0x0;
    this->libnativebridge.remote_loadLibraryExt = 0x0;
    this->libnativebridge.remote_loadLibrary    = 0x0;
    this->libnativebridge.remote_getTrampoline  = 0x0;
    this->libnativebridge.remote_getError       = 0x0;

    bool libdlinit = false;
    bool libcinit  = false;

    int apiLevel = NullUtils::getApiLevel();
    ILOG("locateSymbols: PID=%d apiLevel=%d", this->pid, apiLevel);

#if defined(__arm__) || defined(__aarch64__)
    for (const NullProcess::Map& map : this->maps) {
        // --- libdl ---
        if (!libdlinit && NullUtils::endsWith(map.pathName, "libdl.so")) {
            uintptr_t sym = NullElf::getAddrSym(map.pathName.c_str(), "dlopen");
            if (sym != 0) {
                this->libdl.remote_dlopen  = map.start + sym;
                this->libdl.remote_dlerror = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlerror");
                this->libdl.remote_dlsym   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlsym");
                libdlinit = true;
                ILOG("libdl.so dlopen @ 0x%lx", (unsigned long)this->libdl.remote_dlopen);
            } else {
                ILOG("libdl.so stub (dlopen=0), akan cari di linker");
            }
        }
        // --- libc ---
        if (!libcinit && NullUtils::endsWith(map.pathName, "libc.so")) {
            this->libc.remote_malloc = map.start + NullElf::getAddrSym(map.pathName.c_str(), "malloc");
            this->libc.remote_free   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "free");
            this->libc.remote_mmap   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "mmap");
            if (this->libc.remote_malloc) {
                libcinit = true;
                ILOG("libc.so malloc @ 0x%lx", (unsigned long)this->libc.remote_malloc);
            } else {
                ILOG("libc.so malloc=0 di path '%s'", map.pathName.c_str());
            }
        }
    }

    // Fallback linker64 untuk Android 10+ (dlopen ada di linker, bukan libdl stub)
    if (!libdlinit && apiLevel >= 29) {
        ILOG("Mencari dlopen di linker64 (Android 10+)...");
        for (const NullProcess::Map& map : this->maps) {
            bool isLinker =
#if defined(__aarch64__)
                NullUtils::endsWith(map.pathName, "/linker64") ||
                map.pathName.find("linker64") != std::string::npos;
#else
                NullUtils::endsWith(map.pathName, "/linker") ||
                map.pathName.find("/linker") != std::string::npos;
#endif
            if (isLinker && !libdlinit) {
                uintptr_t sym = NullElf::getAddrSym(map.pathName.c_str(), "dlopen");
                if (sym != 0) {
                    this->libdl.remote_dlopen  = map.start + sym;
                    this->libdl.remote_dlerror = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlerror");
                    this->libdl.remote_dlsym   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlsym");
                    libdlinit = true;
                    ILOG("linker64 dlopen @ 0x%lx (%s)", (unsigned long)this->libdl.remote_dlopen, map.pathName.c_str());
                    break;
                }
            }
        }
    }

    // Fallback libdl_android.so
    if (!libdlinit && apiLevel >= 29) {
        for (const NullProcess::Map& map : this->maps) {
            if (NullUtils::endsWith(map.pathName, "libdl_android.so") && !libdlinit) {
                uintptr_t sym = NullElf::getAddrSym(map.pathName.c_str(), "dlopen");
                if (sym != 0) {
                    this->libdl.remote_dlopen  = map.start + sym;
                    this->libdl.remote_dlerror = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlerror");
                    this->libdl.remote_dlsym   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlsym");
                    libdlinit = true;
                    ILOG("libdl_android.so dlopen @ 0x%lx", (unsigned long)this->libdl.remote_dlopen);
                    break;
                }
            }
        }
    }

    if (!libdlinit) ILOG("locateSymbols: GAGAL temukan dlopen!");
    if (!libcinit)  ILOG("locateSymbols: GAGAL temukan malloc!");

#elif defined(__i386__) || defined(__x86_64__)
    for (const NullProcess::Map& map : this->maps) {
        bool notArm = map.pathName.find("/arm/")   == std::string::npos &&
                      map.pathName.find("/arm64/")  == std::string::npos &&
                      map.pathName.find("/nb/")     == std::string::npos;

        if (notArm && NullUtils::endsWith(map.pathName, "libdl.so") && !libdlinit) {
            uintptr_t sym = NullElf::getAddrSym(map.pathName.c_str(), "dlopen");
            if (sym != 0) {
                this->libdl.remote_dlopen  = map.start + sym;
                this->libdl.remote_dlerror = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlerror");
                this->libdl.remote_dlsym   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "dlsym");
                libdlinit = true;
            }
        }
        if (notArm && NullUtils::endsWith(map.pathName, "libc.so") && !libcinit) {
            this->libc.remote_malloc = map.start + NullElf::getAddrSym(map.pathName.c_str(), "malloc");
            this->libc.remote_free   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "free");
            this->libc.remote_mmap   = map.start + NullElf::getAddrSym(map.pathName.c_str(), "mmap");
            if (this->libc.remote_malloc) libcinit = true;
        }
        if (notArm && NullUtils::endsWith(map.pathName, "libnativebridge.so")) {
            this->libnativebridge.remote_loadLibraryExt =
                map.start + NullElf::getAddrSym(map.pathName.c_str(), "LoadLibraryExt", NullElf::CONTAINS);
            this->libnativebridge.remote_loadLibrary =
                map.start + NullElf::getAddrSym(map.pathName.c_str(), "LoadLibrary", NullElf::CONTAINS);
            this->libnativebridge.remote_getError =
                map.start + NullElf::getAddrSym(map.pathName.c_str(), "GetError", NullElf::CONTAINS);
            this->libnativebridge.remote_getTrampoline =
                map.start + NullElf::getAddrSym(map.pathName.c_str(), "GetTrampoline", NullElf::CONTAINS);
            this->nbInfo.nativeBridgeActive = true;
            this->nbInfo.usesCallbacksPtr   = false;
        }
    }
    NullProcess::Map nbMap = this->findMap("libhoudini.so");
    if (nbMap.pathName.empty()) {
        auto   nbProp = std::array<char, PROP_VALUE_MAX>();
        __system_property_get("ro.dalvik.vm.native.bridge", nbProp.data());
        std::string nbStr = {nbProp.data()};
        nbMap = this->findMap(nbStr);
    }
    if (nbMap.pathName.empty()) {
        this->nbInfo.nativeBridgeActive = false;
        this->nbInfo.usesCallbacksPtr   = false;
    } else {
        uintptr_t nbCallbacksAddr =
            NullElf::getAddrSym(nbMap.pathName.c_str(), "NativeBridgeItf") + nbMap.start;
        this->nbCallbacks               = this->readProcessMemory<NullUtils::NativeBridgeCallbacks>(nbCallbacksAddr);
        this->nbInfo.nativeBridgeActive = true;
        this->nbInfo.usesCallbacksPtr   = true;
    }
#endif
}

// ============================================================================
// MAP PARSING
// ============================================================================
NullProcess::Map NullProcess::Process::parseMap(const std::string& line) {
    NullProcess::Map map;

    size_t spaces[5];
    spaces[0] = line.find(' ');
    for (int i = 1; i < 5; i++)
        spaces[i] = line.find(' ', spaces[i - 1] + 1);

    map.arch = -1;

    size_t dashIndex = line.find('-');
    size_t pathIndex;

    if ((pathIndex = line.find('/')) != std::string::npos) {
        map.pathName = line.substr(pathIndex);
        if (NullUtils::endsWith(map.pathName, ".so"))
            map.arch = NullElf::getLibraryArch(map.pathName.c_str());
    } else if ((pathIndex = line.find('[')) != std::string::npos) {
        map.pathName = line.substr(pathIndex);
    } else {
        map.pathName = "";
    }

    map.start  = std::stoull(line.substr(0, dashIndex), nullptr, 16);
    map.end    = std::stoull(line.substr(dashIndex + 1, spaces[0] - (dashIndex + 1)), nullptr, 16);
    map.length = map.end - map.start;
    map.perms  = 0;

    if (spaces[0] != std::string::npos && spaces[0] + 3 < line.size()) {
        if (line[spaces[0] + 1] == 'r') map.perms |= NullProcess::READ;
        if (line[spaces[0] + 2] == 'w') map.perms |= NullProcess::WRITE;
        if (line[spaces[0] + 3] == 'x') map.perms |= NullProcess::EXECUTE;
    }

    if (spaces[1] != std::string::npos)
        map.offset = std::stoull(line.substr(spaces[1] + 1, spaces[2] - spaces[1] + 1), nullptr, 16);
    if (spaces[2] != std::string::npos && spaces[3] != std::string::npos)
        map.device = line.substr(spaces[2] + 1, spaces[3] - spaces[2] + 1);
    if (spaces[3] != std::string::npos && spaces[4] != std::string::npos)
        map.inode = std::stoi(line.substr(spaces[3] + 1, spaces[4] - spaces[3] + 1));

    return map;
}

NullProcess::Map NullProcess::Process::findMap(std::string mapname) {
    for (NullProcess::Map map : this->maps) {
        if (map.pathName.find(mapname) != std::string::npos)
            return map;
    }
    NullProcess::Map invalidMap;
    invalidMap.pathName = "";
    return invalidMap;
}
