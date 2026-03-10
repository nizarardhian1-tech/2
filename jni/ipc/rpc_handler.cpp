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
}
