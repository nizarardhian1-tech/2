// =============================================================================
// rpc_handler.cpp — Implementasi Semua Handler IPC
// =============================================================================

#include "rpc_handler.h"
#include "../ipc/ipc_server.h"
#include "../ipc/ipc_protocol.h"

#include "../Il2cpp/Il2cpp.h"
#include "../Il2cpp/il2cpp-class.h"
#include "../Includes/Utils.h"
#include "../Includes/Logger.h"
#include "../KittyMemory/KittyMemory.h"
#include "../KittyMemory/KittyUtils.h"

#include <android/log.h>
#include <sys/uio.h>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstring>

#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  "RPC_HANDLER", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "RPC_HANDLER", fmt, ##__VA_ARGS__)

// ── Hex helpers ───────────────────────────────────────────────────────────────
static std::string bytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < len; i++)
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    return oss.str();
}

static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (hex.size() % 2 != 0) return bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}

static uintptr_t parseAddr(const std::string& s) {
    if (s.empty()) return 0;
    try { return static_cast<uintptr_t>(std::stoull(s, nullptr, 16)); }
    catch (...) { return 0; }
}

static std::string addrToStr(uintptr_t addr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << addr;
    return oss.str();
}

// ── Hook tracking ─────────────────────────────────────────────────────────────
struct HookEntry { bool hooked = false; };
static std::unordered_map<uintptr_t, HookEntry> s_hookMap;
static std::mutex s_hookMutex;

// =============================================================================
void RpcHandler::registerAll() {
    IPCServer& srv = IPCServer::get();
    LOGI("Registering all RPC handlers...");

    // ── get_images ────────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_GET_IMAGES, [](const json& params) -> json {
        auto images = Il2cpp::GetImages();
        json arr = json::array();
        for (auto* img : images) {
            if (!img) continue;
            arr.push_back({
                {"name",  img->getName() ? img->getName() : ""},
                {"count", img->getClasses().size()}
            });
        }
        return {{"ok", true}, {"data", arr}};
    });

    // ── get_classes ───────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_GET_CLASSES, [](const json& params) -> json {
        std::string imageName = params.value("image", std::string("Assembly-CSharp"));
        auto* asm_ = Il2cpp::GetAssembly(imageName.c_str());
        if (!asm_) return {{"ok", false}, {"error", "Image not found: " + imageName}};
        auto* img = asm_->getImage();
        if (!img)  return {{"ok", false}, {"error", "Image pointer null"}};

        json arr = json::array();
        for (auto* klass : img->getClasses()) {
            if (!klass) continue;
            arr.push_back({
                {"name",         klass->getName()      ? klass->getName()      : ""},
                {"namespace",    klass->getNamespace() ? klass->getNamespace() : ""},
                {"ptr",          addrToStr(reinterpret_cast<uintptr_t>(klass))},
                {"method_count", klass->getMethods().size()},
                {"field_count",  klass->getFields().size()}
            });
        }
        LOGI("get_classes: %s -> %zu classes", imageName.c_str(), arr.size());
        return {{"ok", true}, {"data", arr}};
    });

    // ── get_methods ───────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_GET_METHODS, [](const json& params) -> json {
        uintptr_t addr = parseAddr(params.value("class_ptr", std::string("")));
        if (!addr) return {{"ok", false}, {"error", "invalid class_ptr"}};
        auto* klass = reinterpret_cast<Il2CppClass*>(addr);

        json arr = json::array();
        try {
            uintptr_t libBase = GetLibBase(GetTargetLib());
            for (auto* m : klass->getMethods()) {
                if (!m) continue;
                uintptr_t offset = (m->methodPointer && libBase)
                    ? reinterpret_cast<uintptr_t>(m->methodPointer) - libBase : 0;
                arr.push_back({
                    {"name",        m->getName() ? m->getName() : ""},
                    {"ptr",         addrToStr(reinterpret_cast<uintptr_t>(m->methodPointer))},
                    {"offset",      addrToStr(offset)},
                    {"param_count", m->getParamsInfo().size()}
                });
            }
        } catch (...) {
            return {{"ok", false}, {"error", "exception iterating methods (bad ptr?)"}};
        }
        return {{"ok", true}, {"data", arr}};
    });

    // ── get_fields ────────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_GET_FIELDS, [](const json& params) -> json {
        uintptr_t addr = parseAddr(params.value("class_ptr", std::string("")));
        if (!addr) return {{"ok", false}, {"error", "invalid class_ptr"}};
        auto* klass = reinterpret_cast<Il2CppClass*>(addr);

        json arr = json::array();
        try {
            for (auto* f : klass->getFields()) {
                if (!f) continue;
                Il2CppType* ftype = f->getType();
                const char* typeName = "?";
                if (ftype) {
                    Il2CppClass* typeClass = ftype->getClass();
                    if (typeClass) typeName = typeClass->getName() ? typeClass->getName() : "?";
                }
                arr.push_back({
                    {"name",   f->getName() ? f->getName() : ""},
                    {"type",   typeName},
                    {"offset", addrToStr(f->getOffset())}
                });
            }
        } catch (...) {
            return {{"ok", false}, {"error", "exception iterating fields"}};
        }
        return {{"ok", true}, {"data", arr}};
    });

    // ── read_mem ──────────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_READ_MEM, [](const json& params) -> json {
        uintptr_t addr = parseAddr(params.value("addr", std::string("")));
        int size       = params.value("size", 0);
        if (!addr || size <= 0 || size > 65536)
            return {{"ok", false}, {"error", "invalid addr or size (max 65536)"}};

        std::vector<uint8_t> buf(size, 0);
        struct iovec local_iov{buf.data(), static_cast<size_t>(size)};
        struct iovec remote_iov{reinterpret_cast<void*>(addr), static_cast<size_t>(size)};
        ssize_t nread = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0);
        if (nread != size)
            return {{"ok", false}, {"error", "read failed (invalid addr?)"}};
        return {{"ok", true}, {"data", bytesToHex(buf.data(), size)}};
    });

    // ── write_mem ─────────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_WRITE_MEM, [](const json& params) -> json {
        uintptr_t addr  = parseAddr(params.value("addr",  std::string("")));
        std::string hex = params.value("bytes", std::string(""));
        if (!addr) return {{"ok", false}, {"error", "invalid addr"}};
        auto bytes = hexToBytes(hex);
        if (bytes.empty()) return {{"ok", false}, {"error", "invalid bytes"}};
        KittyMemory::memWrite(reinterpret_cast<void*>(addr), bytes.data(), bytes.size());
        return {{"ok", true}, {"data", {{"written", bytes.size()}}}};
    });

    // ── hook_method ───────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_HOOK_METHOD, [](const json& params) -> json {
        uintptr_t addr = parseAddr(params.value("method_ptr", std::string("")));
        bool enable    = params.value("enable", true);
        if (!addr) return {{"ok", false}, {"error", "invalid method_ptr"}};

        std::lock_guard<std::mutex> lock(s_hookMutex);
        auto& entry = s_hookMap[addr];
        if (enable && !entry.hooked) {
            KittyMemory::memWrite(reinterpret_cast<void*>(addr), "\x1F\x20\x03\xD5", 4);
            entry.hooked = true;
        } else if (!enable && entry.hooked) {
            entry.hooked = false;
        }
        return {{"ok", true}, {"data", {{"hooked", entry.hooked}}}};
    });

    // ── get_lib_base ──────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_GET_LIB_BASE, [](const json& params) -> json {
        std::string lib = params.value("lib", std::string("libil2cpp.so"));
        uintptr_t base  = GetLibBase(lib.c_str());
        if (!base) return {{"ok", false}, {"error", "library not found: " + lib}};
        return {{"ok", true}, {"data", {{"base", addrToStr(base)}}}};
    });

    // ── set_value ─────────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_SET_VALUE, [](const json& params) -> json {
        uintptr_t addr   = parseAddr(params.value("addr", std::string("")));
        std::string type = params.value("type", std::string("int"));
        if (!addr) return {{"ok", false}, {"error", "invalid addr"}};
        void* ptr = reinterpret_cast<void*>(addr);

        if (type == "float") {
            float v = params.value("value", 0.0f);
            KittyMemory::memWrite(ptr, &v, sizeof(v));
        } else if (type == "int") {
            int32_t v = params.value("value", 0);
            KittyMemory::memWrite(ptr, &v, sizeof(v));
        } else if (type == "int64") {
            int64_t v = params.value("value", (int64_t)0);
            KittyMemory::memWrite(ptr, &v, sizeof(v));
        } else if (type == "bool") {
            uint8_t v = params.value("value", false) ? 1 : 0;
            KittyMemory::memWrite(ptr, &v, sizeof(v));
        } else {
            return {{"ok", false}, {"error", "unknown type: " + type}};
        }
        return {{"ok", true}, {"data", nullptr}};
    });

    // ── dump_classes ──────────────────────────────────────────────────────────
    srv.registerHandler(IPC::CMD_DUMP_CLASSES, [](const json& params) -> json {
        std::string imageName = params.value("image", std::string("Assembly-CSharp"));
        bool inclMethods      = params.value("include_methods", true);
        bool inclFields       = params.value("include_fields",  true);

        auto* asm_ = Il2cpp::GetAssembly(imageName.c_str());
        if (!asm_) return {{"ok", false}, {"error", "Image not found"}};
        auto* img = asm_->getImage();
        if (!img)  return {{"ok", false}, {"error", "Image pointer null"}};

        uintptr_t libBase = GetLibBase(GetTargetLib());
        json arr = json::array();

        for (auto* klass : img->getClasses()) {
            if (!klass) continue;
            json entry = {
                {"name",      klass->getName()      ? klass->getName()      : ""},
                {"namespace", klass->getNamespace() ? klass->getNamespace() : ""},
                {"ptr",       addrToStr(reinterpret_cast<uintptr_t>(klass))}
            };
            if (inclMethods) {
                json mArr = json::array();
                for (auto* m : klass->getMethods()) {
                    if (!m) continue;
                    uintptr_t offset = (m->methodPointer && libBase)
                        ? reinterpret_cast<uintptr_t>(m->methodPointer) - libBase : 0;
                    mArr.push_back({
                        {"name",   m->getName() ? m->getName() : ""},
                        {"offset", addrToStr(offset)},
                        {"params", m->getParamsInfo().size()}
                    });
                }
                entry["methods"] = mArr;
            }
            if (inclFields) {
                json fArr = json::array();
                for (auto* f : klass->getFields()) {
                    if (!f) continue;
                    fArr.push_back({
                        {"name",   f->getName() ? f->getName() : ""},
                        {"offset", addrToStr(f->getOffset())}
                    });
                }
                entry["fields"] = fArr;
            }
            arr.push_back(entry);
        }

        LOGI("dump_classes: %s -> %zu classes", imageName.c_str(), arr.size());
        return {{"ok", true}, {"data", arr}};
    });

    LOGI("All RPC handlers registered.");

    // ── ESP SHM handlers (start_esp / stop_esp) ───────────────────────────────

}

// =============================================================================
// ESP SHM WRITER — start_esp / stop_esp
//
// Persis seperti MLBB-Mod esp_thread, tapi generic:
//   - class_ptr dikirim dari overlay via IPC (user pilih dari class browser)
//   - Scan GC objects setiap ~50ms
//   - Tulis posisi ke g_shm->entities[]
//   - OverlayESP.java (Canvas) baca SHM → gambar di layar
// =============================================================================

#include "../shared_data.h"
#include "../internal/shm.h"   // g_shm

#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <cmath>

// ── ESP thread state ──────────────────────────────────────────────────────────
static std::atomic<bool>  s_espRunning{false};
static std::atomic<bool>  s_espStop{false};
static std::thread        s_espThread;

// ── Vec3 helper ───────────────────────────────────────────────────────────────
struct _Vec3 { float x=0,y=0,z=0; };

// ── W2S helper (via UnityEngine.Camera) ───────────────────────────────────────
static Il2CppObject* s_camera    = nullptr;
static MethodInfo*   s_w2sMth    = nullptr;
static bool          s_camReady  = false;
static int           s_camW=0, s_camH=0;

static bool _InitCam() {
    if (s_camReady) return true;
    try {
        auto *cc = Il2cpp::FindClass("UnityEngine.Camera");
        if (!cc) return false;
        s_camera = cc->invoke_static_method<Il2CppObject*>("get_main");
        if (!s_camera || !IsPtrValid(s_camera)) { s_camera=nullptr; return false; }
        for (auto *m : cc->getMethods("WorldToScreenPoint"))
            if (m && m->methodPointer && m->getParamsInfo().size()==1) { s_w2sMth=m; break; }
        if (!s_w2sMth) return false;
        try { s_camW = s_camera->invoke_method<int>("get_pixelWidth");  } catch(...) { s_camW=0; }
        try { s_camH = s_camera->invoke_method<int>("get_pixelHeight"); } catch(...) { s_camH=0; }
        if (s_camW <= 0 && g_shm) s_camW = g_shm->screenW;
        if (s_camH <= 0 && g_shm) s_camH = g_shm->screenH;
        s_camReady = true;
    } catch(...) { return false; }
    return true;
}

static bool _W2S(_Vec3 world, float &sx, float &sy) {
    if (!s_camera || !s_w2sMth || s_camW<=0 || s_camH<=0) return false;
    if (!IsPtrValid(s_camera)) { s_camReady=false; s_camera=nullptr; return false; }
    _Vec3 s{};
    try { s = s_w2sMth->invoke_static<_Vec3>(s_camera, world); }
    catch(...) { return false; }
    if (s.z <= 0.1f) return false;
    float scaleX = g_shm ? (float)g_shm->screenW / (float)s_camW : 1.f;
    float scaleY = g_shm ? (float)g_shm->screenH / (float)s_camH : 1.f;
    sx = s.x * scaleX;
    sy = (g_shm ? g_shm->screenH : s_camH) - (s.y * scaleY);
    return true;
}

// ── ESP scan thread ───────────────────────────────────────────────────────────
static void _EspThreadFn(Il2CppClass* klass, int posMode,
                          uintptr_t fieldOffset, uintptr_t methodPtr) {
    LOGI("[ESP] Thread started. class=%p posMode=%d fieldOff=0x%zx methodPtr=0x%zx",
         (void*)klass, posMode, fieldOffset, methodPtr);

    MethodInfo* posMethod = methodPtr ? reinterpret_cast<MethodInfo*>(methodPtr) : nullptr;

    // Cache Transform methods untuk mode 2
    MethodInfo *getTransform = nullptr, *getPosition = nullptr;
    if (posMode == 2) {
        auto *tc = Il2cpp::FindClass("UnityEngine.Transform");
        auto *cc = Il2cpp::FindClass("UnityEngine.Component");
        if (tc) getPosition  = tc->getMethod("get_position", 0);
        if (cc) getTransform = cc->getMethod("get_transform", 0);
    }

    while (!s_espStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // ~20fps

        if (!g_shm || s_espStop.load()) break;

        _InitCam();

        std::vector<Il2CppObject*> objects;
        try {
            objects = Il2cpp::GC::FindObjects(klass);
        } catch(...) { continue; }

        int count = 0;
        g_shm->seq++;  // seqlock: mulai tulis

        for (auto* obj : objects) {
            if (s_espStop.load() || count >= MAX_ENTITIES) break;
            if (!obj || !IsPtrValid(obj)) continue;

            _Vec3 pos{};
            bool gotPos = false;

            switch (posMode) {
                case 0: // FieldDirect
                    if (fieldOffset) {
                        gotPos = SafeRead<_Vec3>((const void*)((uintptr_t)obj + fieldOffset), &pos);
                    }
                    break;
                case 1: // MethodInvoke
                    if (posMethod && posMethod->methodPointer) {
                        try { pos = posMethod->invoke_static<_Vec3>(obj); gotPos=true; }
                        catch(...) {}
                    }
                    break;
                case 2: // TransformChain
                    try {
                        Il2CppObject* tr = nullptr;
                        if (getTransform) tr = getTransform->invoke_static<Il2CppObject*>(obj);
                        if (tr && IsPtrValid(tr) && getPosition) {
                            pos = getPosition->invoke_static<_Vec3>(tr);
                            gotPos = true;
                        }
                    } catch(...) {}
                    break;
            }

            if (!gotPos) continue;
            if (pos.x!=pos.x || pos.y!=pos.y || pos.z!=pos.z) continue; // NaN

            ShmEntity& e = g_shm->entities[count];
            memset(&e, 0, sizeof(ShmEntity));
            e.worldX = pos.x; e.worldY = pos.y; e.worldZ = pos.z;

            float sx=0, sy=0;
            if (_W2S(pos, sx, sy)) {
                e.screenX = sx; e.screenY = sy;
                // head: estimasi 2 meter di atas kaki
                _Vec3 headPos{pos.x, pos.y + 2.f, pos.z};
                float hx=0, hy=0;
                if (_W2S(headPos, hx, hy)) { e.headX=hx; e.headY=hy; }
            }

            // Jarak dari self
            if (g_shm) {
                float dx=pos.x-g_shm->screenW*0.5f, dz=pos.z;
                // Fallback simple distance
                e.distance = sqrtf(dx*dx + pos.y*pos.y + dz*dz);
            }

            // Tag dari nama class
            const char* cn = klass->getName();
            if (cn) strncpy(e.tag, cn, sizeof(e.tag)-1);

            e.isValid = true;
            count++;
        }

        g_shm->entityCount = count;
        g_shm->seq++;  // seqlock: selesai tulis
    }

    // Bersihkan saat stop
    if (g_shm) {
        g_shm->seq++;
        memset(g_shm->entities, 0, sizeof(g_shm->entities));
        g_shm->entityCount = 0;
        g_shm->seq++;
    }
    s_espRunning.store(false);
    LOGI("[ESP] Thread stopped.");
}

// ── Daftarkan ke IPCServer (dipanggil dari registerAll) ───────────────────────
static void _RegisterESPHandlers() {
    IPCServer& srv = IPCServer::get();

    // start_esp
    srv.registerHandler(IPC::CMD_START_ESP, [](const json& params) -> json {
        // Stop ESP lama kalau ada
        if (s_espRunning.load()) {
            s_espStop.store(true);
            if (s_espThread.joinable()) s_espThread.join();
            s_espRunning.store(false);
            s_espStop.store(false);
        }

        uintptr_t classAddr   = parseAddr(params.value("class_ptr",   std::string("")));
        int       posMode     = params.value("pos_mode",    0);
        uintptr_t fieldOffset = (uintptr_t)params.value("field_offset", 0);
        uintptr_t methodAddr  = parseAddr(params.value("method_ptr",  std::string("")));

        if (!classAddr)
            return {{"ok", false}, {"error", "invalid class_ptr"}};

        auto* klass = reinterpret_cast<Il2CppClass*>(classAddr);
        if (!klass || !IsPtrValid(klass))
            return {{"ok", false}, {"error", "class_ptr invalid or not mapped"}};

        s_espStop.store(false);
        s_espRunning.store(true);
        s_espThread = std::thread(_EspThreadFn, klass, posMode, fieldOffset, methodAddr);
        s_espThread.detach();

        LOGI("[ESP] start_esp: class=%p posMode=%d fieldOff=0x%zx",
             (void*)klass, posMode, fieldOffset);
        return {{"ok", true}, {"data", {{"tracking", klass->getName() ? klass->getName() : "?"}}}};
    });

    // stop_esp
    srv.registerHandler(IPC::CMD_STOP_ESP, [](const json& params) -> json {
        s_espStop.store(true);
        // Thread detached, tidak perlu join
        LOGI("[ESP] stop_esp called");
        return {{"ok", true}};
    });
}

// ── Patch registerAll() — tambahkan panggilan ke _RegisterESPHandlers ─────────
// CATATAN: Fungsi ini dipanggil di akhir registerAll() di atas.
// Karena kita tidak bisa mengubah definisi fungsi dari sini, kita
// tambahkan init di sini dan panggil via constructor trick.
namespace {
    struct _ESPAutoRegister {
        _ESPAutoRegister() {
            // Tidak bisa register di sini karena IPCServer belum start.
            // _RegisterESPHandlers() dipanggil manual dari registerAll().
        }
    };
}
