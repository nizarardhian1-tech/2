// ============================================================================
// ProjectSessionManager.cpp — Implementasi
// ============================================================================
#include "ProjectSessionManager.h"

#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <ctime>

#include "json/single_include/nlohmann/json.hpp"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Tool/ClassesTab.h"
#include "Tool/Keyboard.h"
#include "Tool/Util.h"
#include "Tool/Patcher.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"   // GetLibBase()
#include "KittyMemory/KittyMemory.h"
#include "imgui/imgui.h"
#include "Tool/Tool.h"  // ⭐ TAMBAH INI

#ifndef USE_FRIDA
#include "Dobby/include/dobby.h"
#endif

namespace ProjectSessionManager
{

// ─── JSON serialization ───────────────────────────────────────────────────────
void to_json(nlohmann::ordered_json &j, const FeatureEntry &e)
{
    j["namespaceName"]    = e.namespaceName;
    if (!e.hexSignature.empty()) {
        j["hexSignature"]    = e.hexSignature;
        j["signatureOffset"] = e.signatureOffset;
    }
    j["className"]        = e.className;
    j["assemblyName"]     = e.assemblyName;
    j["methodName"]       = e.methodName;
    j["methodParamCount"] = e.methodParamCount;
    j["fieldName"]        = e.fieldName;
    j["kind"]             = (int)e.kind;
    j["patchValue"]       = e.patchValue;
    j["returnTypeName"]   = e.returnTypeName;
    j["fieldTypeName"]    = e.fieldTypeName;
    j["displayLabel"]     = e.displayLabel;
    j["notes"]            = e.notes;  // ⭐ BARU
}

void from_json(const nlohmann::ordered_json &j, FeatureEntry &e)
{
    auto get = [&](const char *k, auto &v) {
        if (j.contains(k)) j.at(k).get_to(v);
    };
    get("namespaceName",    e.namespaceName);
    get("className",        e.className);
    get("assemblyName",     e.assemblyName);
    get("methodName",       e.methodName);
    get("methodParamCount", e.methodParamCount);
    get("fieldName",        e.fieldName);
    get("patchValue",       e.patchValue);
    get("returnTypeName",   e.returnTypeName);
    get("fieldTypeName",    e.fieldTypeName);
    get("displayLabel",     e.displayLabel);
    get("notes",            e.notes);  // ⭐ BARU
    get("hexSignature",     e.hexSignature);
    get("signatureOffset",  e.signatureOffset);
    if (j.contains("kind")) e.kind = (PatchKind)j.at("kind").get<int>();
}

void to_json(nlohmann::ordered_json &j, const Session &s)
{
    j["projectName"] = s.projectName;
    j["gameVersion"] = s.gameVersion;
    j["savedAt"]     = s.savedAt;
    j["description"] = s.description;  // ⭐ BARU
    j["features"]    = s.features;
}

void from_json(const nlohmann::ordered_json &j, Session &s)
{
    auto get = [&](const char *k, auto &v) {
        if (j.contains(k)) j.at(k).get_to(v);
    };
    get("projectName", s.projectName);
    get("gameVersion", s.gameVersion);
    get("savedAt",     s.savedAt);
    get("description", s.description);  // ⭐ BARU
    get("features",    s.features);
}

// ─── State internal ───────────────────────────────────────────────────────────
static Session            g_session;
static bool               g_dirty       = false;
static std::atomic<bool>  g_resolving   {false};
static std::atomic<int>   g_resolveOk   {0};
static std::atomic<int>   g_resolveFail {0};
// Path session — disimpan di IL2CPPTOOLS/session/
// Gunakan Util::DirSession() + filename agar konsisten dengan sistem folder baru.
static const std::string  SESSION_FILENAME = "project_session.json";

static std::string Now()
{
    time_t t = time(nullptr);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return buf;
}

// ─── Patch bytes derivation (ARM64) ──────────────────────────────────────────
static std::vector<uint8_t> DerivePatchBytes(const std::string &tn, const std::string &tv)
{
    if (tn == "System.Void" || tv == "NOP")
        return {0xC0, 0x03, 0x5F, 0xD6};

    if (tn == "System.Boolean") {
        bool bv = (tv == "True" || tv == "true" || tv == "1");
        if (bv) return {0x20, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};
        else    return {0x00, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};
    }
    try {
        if (tn == "System.Int32" || tn == "System.UInt32") {
            uint32_t v = (uint32_t)std::stol(tv);
            uint32_t ldr = 0x18000060, ret = 0xD65F03C0;
            std::vector<uint8_t> b(12);
            memcpy(b.data()+0, &ldr, 4);
            memcpy(b.data()+4, &ret, 4);
            memcpy(b.data()+8, &v, 4);
            return b;
        }
        if (tn == "System.Single") {
            float v = std::stof(tv);
            uint32_t ldr = 0x1C000060, ret = 0xD65F03C0;
            std::vector<uint8_t> b(12);
            memcpy(b.data()+0, &ldr, 4);
            memcpy(b.data()+4, &ret, 4);
            memcpy(b.data()+8, &v, 4);
            return b;
        }
    } catch(...) {}
    return {0xC0, 0x03, 0x5F, 0xD6};
}

// ─── Build feature list dari oMap ────────────────────────────────────────────
static std::vector<FeatureEntry> BuildFromOMap(
    const std::unordered_map<MethodInfo*, ClassesTab::OriginalMethodBytes> &oMap)
{
    std::vector<FeatureEntry> result;
    for (const auto &[method, data] : oMap) {
        if (data.bytes.empty() || !method) continue;
        FeatureEntry e;
        e.kind = PatchKind::MethodPatch;
        auto *klass = method->getClass();
        if (klass) {
            e.className     = klass->getName()     ? klass->getName()     : "Unknown";
            e.namespaceName = klass->getNamespace() ? klass->getNamespace() : "";
            e.assemblyName  = (klass->getImage() && klass->getImage()->getName())
                              ? klass->getImage()->getName() : "";
        }
        e.methodName       = method->getName() ? method->getName() : "method";
        e.methodParamCount = (int)method->getParamsInfo().size();
        auto *rt           = method->getReturnType();
        e.returnTypeName   = rt ? rt->getName() : "System.Void";
        e.patchValue       = data.text.empty() ? "NOP" : data.text;
        e.displayLabel     = e.className + "::" + e.methodName + " -> " + e.patchValue;
        e.notes            = "";
        // [Pilar 3] Auto-generate hex signature untuk fallback resolve
        if (method->methodPointer) {
            e.hexSignature = PatternScanner::GenerateSignature(method->methodPointer, 16);
        }
        result.push_back(std::move(e));
    }
    return result;
}

static std::vector<FeatureEntry> BuildFromFieldPatchMap(
    const std::unordered_map<FieldInfo*, ClassesTab::FieldPatchData> &fieldPatchMap)
{
    std::vector<FeatureEntry> result;
    for (const auto &[field, data] : fieldPatchMap) {
        if (!data.active || !field) continue;
        FeatureEntry e;
        e.kind          = PatchKind::FieldPatch;
        e.fieldName     = field->getName() ? field->getName() : "field";
        auto *ft        = field->getType();
        e.fieldTypeName = ft ? ft->getName() : "System.Int32";
        e.className     = "Unknown";
        e.patchValue    = data.text;
        e.displayLabel  = e.fieldName + " = " + data.text;
        e.notes         = "";  // ⭐ BARU
        result.push_back(std::move(e));
    }
    return result;
}


// ─────────────────────────────────────────────────────────────────────────────
// PatternScanner — Implementasi
// [Pilar 3] Hex signature scan sebagai fallback resolve saat nama berubah
// ─────────────────────────────────────────────────────────────────────────────
namespace PatternScanner {

// Parse "AA BB ?? CC" → vector<pair<uint8_t, bool>> (byte, isWildcard)
static std::vector<std::pair<uint8_t,bool>> ParsePattern(const std::string& pat) {
    std::vector<std::pair<uint8_t,bool>> result;
    std::istringstream ss(pat);
    std::string token;
    while (ss >> token) {
        if (token == "??" || token == "?") {
            result.push_back({0x00, true});
        } else {
            try {
                uint8_t b = (uint8_t)std::stoul(token, nullptr, 16);
                result.push_back({b, false});
            } catch (...) {
                result.push_back({0x00, true}); // parse error = wildcard
            }
        }
    }
    return result;
}

// Cari region libil2cpp.so yang executable dari /proc/self/maps
struct Region { uintptr_t start, end; };
static std::vector<Region> GetExecRegions(const char* libName) {
    std::vector<Region> regions;
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return regions;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, libName)) continue;
        // Perlu: r-x (executable)
        char perms[8] = {};
        uintptr_t s = 0, e = 0;
        sscanf(line, "%lx-%lx %7s", &s, &e, perms);
        if (perms[2] == 'x') regions.push_back({s, e});
    }
    fclose(fp);
    return regions;
}

uintptr_t Scan(const std::string& pattern, int offset) {
    auto pat     = ParsePattern(pattern);
    if (pat.empty()) return 0;
    auto regions = GetExecRegions(GetTargetLib());
    if (regions.empty()) {
        LOGE("[PatternScanner] Tidak ada region executable libil2cpp.so");
        return 0;
    }
    size_t patLen = pat.size();
    for (auto& r : regions) {
        if (r.end <= r.start) continue;
        const uint8_t* mem = (const uint8_t*)r.start;
        size_t memLen = r.end - r.start;
        if (memLen < patLen) continue;
        for (size_t i = 0; i <= memLen - patLen; i++) {
            bool match = true;
            for (size_t j = 0; j < patLen; j++) {
                if (!pat[j].second && mem[i+j] != pat[j].first) {
                    match = false; break;
                }
            }
            if (match) {
                uintptr_t result = r.start + i + (uintptr_t)offset;
                LOGI("[PatternScanner] Match at 0x%lx (region 0x%lx-0x%lx)",
                     result, r.start, r.end);
                return result;
            }
        }
    }
    LOGW("[PatternScanner] Pattern tidak ditemukan: %s", pattern.c_str());
    return 0;
}

uintptr_t ScanRVA(const std::string& pattern, int offset) {
    uintptr_t abs = Scan(pattern, offset);
    if (!abs) return 0;
    uintptr_t base = GetLibBase(GetTargetLib());
    return (base && abs > base) ? (abs - base) : abs;
}

std::string GenerateSignature(void* methodPtr, size_t sigLen) {
    if (!methodPtr) return "";
    const uint8_t* ptr = (const uint8_t*)methodPtr;
    std::ostringstream ss;
    // ARM64: instruksi 4 byte. Byte wildcard: byte 0,1 dari setiap instruksi
    // yang mengandung immediate (BL, LDR, ADR, ADRP biasanya di byte 0-2).
    // Strategi sederhana: keep byte pertama (opcode), wildcard 3 byte berikutnya
    // untuk instruksi yang berpotensi relocatable (BL=0x??000094, ADRP=0x??000090, dll).
    // Byte yang aman (fixed opcode): RET=0xC0035FD6, NOP=0x1F2003D5.
    // Untuk kesederhanaan: emit semua sebagai literal (user bisa edit manual jika perlu).
    for (size_t i = 0; i < sigLen && i < 64; i++) {
        if (i > 0) ss << " ";
        // Wildcard untuk byte 1,2,3 dari instruksi BL/B/ADRP/LDR-literal
        // (instruksi ARM64 4 byte, byte index dalam instruksi = i%4)
        // Heuristik: jika byte 3 (MSB) adalah 0x94/0x97/0x90/0x91/0x18/0x58/0x1C
        //            maka byte 0,1,2 mengandung immediate → wildcard
        if ((i % 4) == 3) {
            uint8_t b = ptr[i];
            // BL=0x94, BLR-like=0x97, ADRP=0x90/0x91, LDR literal=0x18,0x58,0x1C,0x5C
            if (b==0x94||b==0x97||b==0x90||b==0x91||b==0x18||b==0x58||b==0x1C||b==0x5C) {
                // wildcard bytes 0,1,2 dari instruksi ini
                for (int k = 1; k <= 3; k++) {
                    // replace 3 bytes sebelum byte ini dengan ??
                }
                // Emit ?? untuk 3 bytes sebelumnya (sudah emit terlanjur)
                // Lebih mudah: emit 4 bytes sekaligus
                ss.seekp(0); // reset — ganti pendekatan
                // Emit ulang seluruh instruksi sebagai wildcard
                std::string cur = ss.str();
                // Trim 3 " XX" terakhir, replace dengan " ?? ?? ??"
                // Terlalu kompleks — gunakan pendekatan sederhana: emit semua literal
                // User bisa ganti byte offset immediate dengan ?? manual.
                // Untuk sekarang: emit literal saja (sudah cukup sebagai starting point)
                break;
            }
        }
    }
    // Simple reliable approach: emit semua bytes sebagai hex literal
    // User yang paham ARM64 bisa replace immediate dengan ?? sendiri
    ss.str(""); ss.clear();
    for (size_t i = 0; i < sigLen && i < 64; i++) {
        if (i > 0) ss << " ";
        char h[4]; snprintf(h, sizeof(h), "%02X", ptr[i]);
        ss << h;
    }
    return ss.str();
}

} // namespace PatternScanner

// ─── Resolve satu FeatureEntry ────────────────────────────────────────────────
// ─── Resolve satu FeatureEntry ────────────────────────────────────────────────
static bool ResolveEntry(FeatureEntry &e)
{
    e.resolved = false;
    e.resolveError.clear();
    e.resolvedMethod      = nullptr;
    e.resolvedField       = nullptr;
    e.resolvedClass       = nullptr;
    e.resolvedRVA         = 0;
    e.resolvedFieldOffset = 0;

    // Cari class
    Il2CppClass *klass = nullptr;
    if (!e.namespaceName.empty()) {
        std::string full = e.namespaceName + "." + e.className;
        klass = Il2cpp::FindClass(full.c_str());
    }
    if (!klass) klass = Il2cpp::FindClass(e.className.c_str());

    // Brute force fallback
    if (!klass) {
        for (auto *img : Il2cpp::GetImages()) {
            if (!img) continue;
            if (!e.assemblyName.empty() && img->getName()) {
                std::string imgN = img->getName();
                std::string asmN = e.assemblyName;
                auto strip = [](std::string &s){ if (s.size()>4&&s.substr(s.size()-4)==".dll") s=s.substr(0,s.size()-4); };
                strip(imgN); strip(asmN);
                if (imgN != asmN) continue;
            }
            for (auto *k : img->getClasses()) {
                if (!k) continue;
                const char *cn = k->getName();
                if (!cn || e.className != cn) continue;
                const char *ns = k->getNamespace();
                if (e.namespaceName.empty() || (ns && e.namespaceName == ns)) {
                    klass = k; break;
                }
            }
            if (klass) break;
        }
    }

    if (!klass) {
        e.resolveError = "Class not found: " + e.className;
        return false;
    }
    e.resolvedClass = klass;

    // GetLibBase() pakai cache stabil
    uintptr_t il2cppBase = GetLibBase(GetTargetLib());

    if (e.kind == PatchKind::MethodPatch) {
        MethodInfo *method = nullptr;
        if (e.methodParamCount >= 0)
            method = klass->getMethod(e.methodName.c_str(), e.methodParamCount);
        if (!method) {
            for (auto *m : klass->getMethods()) {
                if (!m) continue;
                const char *mn = m->getName();
                if (mn && e.methodName == mn) { method = m; break; }
            }
        }
        if (!method) {
            // [Pilar 3] Fallback: coba pattern scan jika hexSignature tersedia
            if (!e.hexSignature.empty()) {
                uintptr_t abs = PatternScanner::Scan(e.hexSignature, e.signatureOffset);
                if (abs) {
                    uintptr_t base = GetLibBase(GetTargetLib());
                    e.resolvedRVA        = (base && abs > base) ? (abs - base) : abs;
                    e.resolved           = true;
                    e.resolvedViaPattern = true;
                    LOGI("[PSM] Pattern scan fallback OK: %s::%s → RVA 0x%lx",
                         e.className.c_str(), e.methodName.c_str(), e.resolvedRVA);
                    return true;
                }
            }
            e.resolveError = "Method not found: " + e.className + "::" + e.methodName;
            return false;
        }
        if (!method->methodPointer) {
            e.resolveError = "Method pointer null: " + e.methodName;
            return false;
        }
        e.resolvedMethod = method;
        uintptr_t abs    = (uintptr_t)method->methodPointer;
        e.resolvedRVA    = (il2cppBase && abs > il2cppBase) ? (abs - il2cppBase) : abs;
        e.resolved       = true;
        return true;
    }

    if (e.kind == PatchKind::FieldPatch) {
        FieldInfo *field = nullptr;
        for (auto *f : klass->getFields(true)) {
            if (!f || !f->getName()) continue;
            if (strcmp(f->getName(), e.fieldName.c_str()) == 0) { field = f; break; }
        }
        if (!field) {
            e.resolveError = "Field not found: " + e.className + "::" + e.fieldName;
            return false;
        }
        e.resolvedField       = field;
        e.resolvedFieldOffset = field->getOffset();
        e.resolved            = true;
        return true;
    }

    e.resolveError = "Unknown patch kind";
    return false;
}

// ─── API Publik ───────────────────────────────────────────────────────────────

bool SaveSession(
    const std::unordered_map<MethodInfo*, ClassesTab::OriginalMethodBytes> &oMap,
    const std::unordered_map<FieldInfo*, ClassesTab::FieldPatchData>       &fieldPatchMap)
{
    g_session.savedAt = Now();
    g_session.features.clear();

    auto mf = BuildFromOMap(oMap);
    g_session.features.insert(g_session.features.end(), mf.begin(), mf.end());
    auto ff = BuildFromFieldPatchMap(fieldPatchMap);
    g_session.features.insert(g_session.features.end(), ff.begin(), ff.end());

    try {
        nlohmann::ordered_json j = g_session;
        Util::FileWriter fw(Util::DirSession(), SESSION_FILENAME);
        fw.write(j.dump(2, ' ').c_str());
        g_dirty = false;
        LOGI("SaveSession: %zu fitur disimpan", g_session.features.size());
        return true;
    } catch (const std::exception &ex) {
        LOGE("SaveSession error: %s", ex.what());
        return false;
    }
}

bool LoadSession()
{
    try {
        Util::FileReader fr(Util::DirSession(), SESSION_FILENAME);
        if (!fr.exists()) { LOGE("Session file tidak ada: %s/%s", Util::DirSession().c_str(), SESSION_FILENAME.c_str()); return false; }
        g_session = nlohmann::ordered_json::parse(fr.read()).get<Session>();
        LOGI("LoadSession: %zu fitur, mulai resolve...", g_session.features.size());
    } catch (const std::exception &ex) {
        LOGE("LoadSession parse error: %s", ex.what());
        return false;
    }

    for (auto &fe : g_session.features) {
        fe.resolved = false; fe.resolveError.clear();
        fe.resolvedMethod = nullptr; fe.resolvedField = nullptr; fe.resolvedClass = nullptr;
    }
    g_resolving.store(true);
    g_resolveOk.store(0);
    g_resolveFail.store(0);

    std::thread([&]() {
        for (auto &fe : g_session.features) {
            bool ok = ResolveEntry(fe);
            if (ok) g_resolveOk.fetch_add(1);
            else    g_resolveFail.fetch_add(1);
            LOGI("Resolve [%s::%s]: %s | RVA=0x%zX | Err=%s",
                 fe.className.c_str(),
                 (fe.kind==PatchKind::MethodPatch ? fe.methodName : fe.fieldName).c_str(),
                 ok ? "OK" : "FAIL",
                 fe.resolvedRVA,
                 fe.resolveError.c_str());
        }
        g_resolving.store(false);
    }).detach();

    return true;
}

int ApplyResolved(
    std::unordered_map<MethodInfo*, ClassesTab::OriginalMethodBytes> &oMap,
    std::unordered_map<FieldInfo*, ClassesTab::FieldPatchData>       &fieldPatchMap)
{
    int applied = 0;
    for (auto &fe : g_session.features) {
        if (!fe.resolved) continue;

        if (fe.kind == PatchKind::MethodPatch && fe.resolvedMethod) {
            auto &entry = oMap[fe.resolvedMethod];
            if (!entry.bytes.empty()) continue;

            auto *mp = fe.resolvedMethod->methodPointer;
            if (!mp) continue;

            auto patchBytes = DerivePatchBytes(fe.returnTypeName, fe.patchValue);
            size_t sz = patchBytes.size();

            entry.bytes.resize(sz);
            KittyMemory::ProtectAddr(mp, sz, PROT_READ | PROT_WRITE | PROT_EXEC);
            memcpy(entry.bytes.data(), mp, sz);
            entry.text = fe.patchValue;

            memcpy(mp, patchBytes.data(), sz);
            __builtin___clear_cache((char*)mp, (char*)mp + sz);

            LOGI("ApplyResolved: %s::%s @ RVA 0x%zX",
                 fe.className.c_str(), fe.methodName.c_str(), fe.resolvedRVA);
            applied++;
        }

        if (fe.kind == PatchKind::FieldPatch && fe.resolvedField) {
            auto &pd = fieldPatchMap[fe.resolvedField];
            if (pd.active) continue;
            pd.text = fe.patchValue;
            pd.active = false; // user confirm dari UI
            LOGI("ApplyResolved: field %s::%s @ 0x%zX (butuh instance)",
                 fe.className.c_str(), fe.fieldName.c_str(), fe.resolvedFieldOffset);
        }
    }
    return applied;
}

// ============================================================================
// ⭐ FITUR NAVIGASI
// ============================================================================

void NavigateToFeature(const FeatureEntry& feature)
{
    if (!feature.resolved || !feature.resolvedClass) {
        LOGE("Cannot navigate: feature not resolved");
        return;
    }
    
    // 1. Buka tab baru di Class Browser
    auto& tab = Tool::OpenNewTabFromClass(feature.resolvedClass);
    
    // 2. Set filter ke class name
    tab.filter = feature.className;
    tab.FilterClasses(tab.filter);
    
    // 3. Kalau method, set flag untuk expand method tertentu
    if (feature.kind == PatchKind::MethodPatch && feature.resolvedMethod) {
        tab.expandMethod = feature.resolvedMethod;
        LOGI("Navigated to method: %s::%s", 
             feature.className.c_str(), feature.methodName.c_str());
    }
    
    // 4. Kalau field, switch ke mode field viewer
    if (feature.kind == PatchKind::FieldPatch && feature.resolvedField) {
        tab.filterByField = true;
        tab.FilterClasses(tab.filter);
        LOGI("Navigated to field: %s::%s", 
             feature.className.c_str(), feature.fieldName.c_str());
    }
}

// ============================================================================
// ⭐ FITUR EXPORT UPDATED SESSION
// ============================================================================

bool ExportUpdatedSession(const std::string& format)
{
    if (g_session.features.empty()) {
        LOGE("No session to export");
        return false;
    }
    
    // Hitung berapa yang sudah resolve
    int resolvedCount = 0;
    for (auto& fe : g_session.features) {
        if (fe.resolved) resolvedCount++;
    }
    
    if (resolvedCount == 0) {
        LOGE("No resolved features to export");
        return false;
    }
    
    std::string gameVer = Il2cpp::getGameVersion();
    std::string packageName = Il2cpp::getPackageName();
    std::string timestamp = Now();
    
    if (format == "lua") {
        // ===== EXPORT LUA (GameGuardian) =====
        std::ostringstream lua;
        lua << "-- ============================================================\n";
        lua << "-- Project Session: " << g_session.projectName << "\n";
        lua << "-- Game Package : " << packageName << "\n";
        lua << "-- Game Version : " << gameVer << "\n";
        lua << "-- Exported     : " << timestamp << "\n";
        lua << "-- ============================================================\n\n";
        
        lua << "local function getLibBase()\n";
        lua << "    local ranges = gg.getRangesList('libil2cpp.so')\n";
        lua << "    if ranges and #ranges > 0 then return ranges[1].start end\n";
        lua << "    return nil\n";
        lua << "end\n\n";
        
        lua << "local libBase = getLibBase()\n";
        lua << "if not libBase then\n";
        lua << "    print('libil2cpp.so not found!')\n";
        lua << "    return\n";
        lua << "end\n\n";
        
        lua << "local patches = {\n";
        
        for (auto& fe : g_session.features) {
            if (!fe.resolved) continue;
            
            if (fe.kind == PatchKind::MethodPatch && fe.resolvedRVA) {
                // Generate bytes berdasarkan return type
                std::vector<uint8_t> bytes;
                if (fe.returnTypeName == "System.Void" || fe.patchValue == "NOP") {
                    bytes = {0xC0,0x03,0x5F,0xD6};
                } else if (fe.returnTypeName == "System.Boolean") {
                    bool bv = (fe.patchValue == "True" || fe.patchValue == "true" || fe.patchValue == "1");
                    if (bv) bytes = {0x20,0x00,0x80,0x52, 0xC0,0x03,0x5F,0xD6};
                    else    bytes = {0x00,0x00,0x80,0x52, 0xC0,0x03,0x5F,0xD6};
                } else if (fe.returnTypeName == "System.Int32" || fe.returnTypeName == "System.UInt32") {
                    try {
                        uint32_t v = (uint32_t)std::stol(fe.patchValue);
                        uint32_t ldr = 0x18000060;
                        bytes.resize(12);
                        memcpy(bytes.data(), &ldr, 4);
                        bytes[4] = 0xC0; bytes[5] = 0x03; bytes[6] = 0x5F; bytes[7] = 0xD6;
                        memcpy(bytes.data()+8, &v, 4);
                    } catch(...) { bytes = {0xC0,0x03,0x5F,0xD6}; }
                } else {
                    bytes = {0xC0,0x03,0x5F,0xD6};
                }
                
                lua << "    {\n";
                lua << "        name = \"" << fe.className << "::" << fe.methodName << " = " << fe.patchValue << "\",\n";
                lua << "        rva = 0x" << std::hex << fe.resolvedRVA << std::dec << ",\n";
                lua << "        bytes = {";
                for (size_t i=0; i<bytes.size(); i++) {
                    lua << "0x" << std::hex << (int)bytes[i] << std::dec;
                    if (i+1 < bytes.size()) lua << ",";
                }
                lua << "}\n";
                lua << "    },\n";
            }
        }
        
        lua << "}\n\n";
        lua << "for _,p in ipairs(patches) do\n";
        lua << "    local addr = libBase + p.rva\n";
        lua << "    local t = {}\n";
        lua << "    for i,b in ipairs(p.bytes) do\n";
        lua << "        table.insert(t, {address=addr+(i-1), flags=gg.TYPE_BYTE, value=b})\n";
        lua << "    end\n";
        lua << "    gg.setValues(t)\n";
        lua << "    print('[GG] Patched: ' .. p.name)\n";
        lua << "end\n";
        
        std::string filename = g_session.projectName + "_" + gameVer + ".lua";
        Util::FileWriter fw(Util::DirCodegen(), filename);
        fw.write(lua.str().c_str());
        
        LOGI("Exported Lua session: %s", filename.c_str());
        Keyboard::Open((Util::DirCodegen() + "/" + filename).c_str(), [](const std::string&){});
        
    } else if (format == "cpp") {
        // ===== EXPORT C++ HEADER =====
        std::ostringstream cpp;
        cpp << "// ============================================================\n";
        cpp << "// Project Session: " << g_session.projectName << "\n";
        cpp << "// Game Package : " << packageName << "\n";
        cpp << "// Game Version : " << gameVer << "\n";
        cpp << "// Exported     : " << timestamp << "\n";
        cpp << "// RVA dihitung otomatis (stabil antar restart)\n";
        cpp << "// ============================================================\n";
        cpp << "#pragma once\n#include <cstdint>\n\n";
        
        for (auto& fe : g_session.features) {
            if (!fe.resolved) continue;
            
            if (fe.kind == PatchKind::MethodPatch && fe.resolvedRVA) {
                std::string safeName = fe.className + "_" + fe.methodName;
                for (char &c : safeName) if (!isalnum(c)) c = '_';
                std::transform(safeName.begin(), safeName.end(), safeName.begin(), ::toupper);
                
                cpp << "// " << fe.className << "::" << fe.methodName << " -> " << fe.patchValue << "\n";
                cpp << "#define RVA_" << safeName << " 0x" << std::hex << fe.resolvedRVA << std::dec << "ULL\n";
            }
        }
        
        std::string filename = g_session.projectName + "_" + gameVer + ".h";
        Util::FileWriter fw(Util::DirCodegen(), filename);
        fw.write(cpp.str().c_str());
        
        LOGI("Exported C++ header: %s", filename.c_str());
        Keyboard::Open((Util::DirCodegen() + "/" + filename).c_str(), [](const std::string&){});
    }
    
    return true;
}

// ============================================================================
// UI DRAW
// ============================================================================

void DrawUI(
    std::unordered_map<MethodInfo*, ClassesTab::OriginalMethodBytes> &oMap,
    std::unordered_map<FieldInfo*, ClassesTab::FieldPatchData>       &fieldPatchMap)
{
    static char projNameBuf[128] = "MyProject";
    static char descBuf[256] = "";

    ImGui::SeparatorText("Project Session Manager");
    ImGui::TextDisabled("Save/Load fitur tanpa hardcode RVA — auto-resolve saat game update");
    ImGui::Spacing();

    // Project Name
    // Project Name - OTOMATIS dari package name
std::string packageName = Il2cpp::getPackageName();
std::string gameVersion = Il2cpp::getGameVersion();
std::string autoProjectName = packageName + "_" + gameVersion;
g_session.projectName = autoProjectName;

ImGui::Text("Project Name:");
ImGui::SameLine();
ImGui::TextColored(ImVec4(0.3f,1.f,0.4f,1.f), "%s", autoProjectName.c_str());
ImGui::SetItemTooltip("Otomatis dari package name + version");

// Description - TETAP ADA (opsional)
ImGui::Text("Description:");
ImGui::SameLine();
ImGui::SetNextItemWidth(300);
strncpy(descBuf, g_session.description.c_str(), 255);
if (ImGui::InputText("##Desc", descBuf, sizeof(descBuf))) {
    g_session.description = descBuf;
    g_dirty = true;
}
// Trigger keyboard untuk description aja
if (ImGui::IsItemActivated()) {
    Keyboard::Open(descBuf, [](const std::string& result) {
        strncpy(descBuf, result.c_str(), sizeof(descBuf)-1);
        g_session.description = result;
        g_dirty = true;
    });
}

    ImGui::Spacing();

    int activePatch = 0; for (auto &[m,d]:oMap) if (!d.bytes.empty()) activePatch++;
    int activeField = 0; for (auto &[f,d]:fieldPatchMap) if (d.active) activeField++;
    ImGui::TextColored(ImVec4(0.3f,1.f,0.4f,1.f),
        "Aktif: %d method patch, %d field patch", activePatch, activeField);
    ImGui::Spacing();

    // SAVE
    bool canSave = (activePatch + activeField) > 0;
    if (!canSave) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(20,120,20,230));
    if (ImGui::Button("  SAVE Session  ", ImVec2(180, 0))) {
        g_session.projectName = projNameBuf;
        g_session.description = descBuf;
        bool ok = SaveSession(oMap, fieldPatchMap);
        std::string msg = ok
            ? ("Session disimpan:\n" + Util::DirSession() + "/" + SESSION_FILENAME)
            : "GAGAL menyimpan session!";
        Keyboard::Open(msg.c_str(), [](const std::string&){});
    }
    ImGui::PopStyleColor();
    if (!canSave) ImGui::EndDisabled();

    ImGui::SameLine();

    // LOAD
    bool loading = g_resolving.load();
    if (loading) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(20,60,180,230));
    if (ImGui::Button("  LOAD Session  ", ImVec2(180, 0))) {
        LoadSession();
    }
    ImGui::PopStyleColor();
    if (loading) ImGui::EndDisabled();

    ImGui::Spacing();

    // Status
    if (g_resolving.load()) {
        ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f),
            "Resolving... (%d OK, %d FAIL)", g_resolveOk.load(), g_resolveFail.load());
    } else if (!g_session.features.empty()) {
        ImGui::TextColored(ImVec4(0.3f,1.f,0.4f,1.f),
            "%d fitur | %d resolved | %d gagal",
            (int)g_session.features.size(), g_resolveOk.load(), g_resolveFail.load());

        bool anyResolved = (g_resolveOk.load() > 0);
        if (!anyResolved) ImGui::BeginDisabled();
        
        // APPLY button
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150,60,20,230));
        if (ImGui::Button("  APPLY Resolved Patches  ", ImVec2(160, 0))) {
            int cnt = ApplyResolved(oMap, fieldPatchMap);
            char msg[128]; snprintf(msg, sizeof(msg), "%d patch berhasil di-apply!", cnt);
            Keyboard::Open(msg, [](const std::string&){});
        }
        ImGui::PopStyleColor();
        
        // ⭐ BARU: Export buttons
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30,80,150,230));
        if (ImGui::Button("  Export Lua  ", ImVec2(110, 0))) {
            ExportUpdatedSession("lua");
        }
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50,50,150,230));
        if (ImGui::Button("  Export C++  ", ImVec2(110, 0))) {
            ExportUpdatedSession("cpp");
        }
        ImGui::PopStyleColor();

        if (!anyResolved) ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Daftar fitur
    if (!g_session.features.empty()) {
        ImGui::Text("Session '%s' (disimpan: %s):",
            g_session.projectName.c_str(), g_session.savedAt.c_str());
        ImGui::Spacing();

        float avH = std::min(ImGui::GetContentRegionAvail().y - 10,
                             ImGui::GetTextLineHeightWithSpacing() * 12.f);
        ImGui::BeginChild("##SessionList", ImVec2(0, avH), true);

        for (int i = 0; i < (int)g_session.features.size(); i++) {
            auto &fe = g_session.features[i];
            ImGui::PushID(i);

            ImU32 col = IM_COL32(180,180,180,255);
            if (fe.resolved) col = IM_COL32(80,230,100,255);
            else if (!fe.resolveError.empty()) col = IM_COL32(255,80,80,255);

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            char label[256];
            snprintf(label, sizeof(label), "%s %s::%s = %s",
                fe.kind == PatchKind::MethodPatch ? "[M]" : "[F]",
                fe.className.c_str(),
                (fe.kind==PatchKind::MethodPatch ? fe.methodName : fe.fieldName).c_str(),
                fe.patchValue.c_str());
            
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();

            // ⭐ FITUR NAVIGASI: Klik tahan 0.5 detik
            if (ImGui::IsItemHovered()) {
                if (ImGui::IsMouseDown(0)) {
                    if (!fe.hoveredForNavigation) {
                        const_cast<FeatureEntry&>(fe).hoveredForNavigation = true;
                        const_cast<FeatureEntry&>(fe).hoverStartTime = ImGui::GetTime();
                    }
                    
                    if (ImGui::GetTime() - fe.hoverStartTime > 0.5f) {
                        NavigateToFeature(fe);
                        const_cast<FeatureEntry&>(fe).hoveredForNavigation = false;
                    }
                } else {
                    const_cast<FeatureEntry&>(fe).hoveredForNavigation = false;
                }
                
                // Tooltip
                ImGui::BeginTooltip();
                ImGui::TextColored(ImVec4(1,1,0,1), "Klik tahan untuk buka di Class Browser");
                if (!fe.notes.empty()) {
                    ImGui::Separator();
                    ImGui::TextWrapped("📝 %s", fe.notes.c_str());
                }
                if (fe.resolved) {
                    ImGui::Separator();
                    ImGui::Text("Assembly : %s", fe.assemblyName.c_str());
                    ImGui::Text("Namespace: %s", fe.namespaceName.c_str());
                    if (fe.resolvedRVA) {
                        ImGui::Text("RVA: 0x%zX", fe.resolvedRVA);
                        if (fe.resolvedViaPattern) {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1,0.8f,0,1), "[Pattern Scan]");
                        }
                    }
                    if (fe.resolvedFieldOffset)
                        ImGui::Text("Offset: 0x%zX", fe.resolvedFieldOffset);
                } else if (!fe.resolveError.empty()) {
                    ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f),
                        "%s", fe.resolveError.c_str());
                }
                ImGui::EndTooltip();
            } else {
                const_cast<FeatureEntry&>(fe).hoveredForNavigation = false;
            }

            ImGui::PopID();
        }
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("Belum ada session. Tekan LOAD atau SAVE.");
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150,150,150,200));
    ImGui::TextWrapped(
        "Tips: Session disimpan tanpa RVA. Saat game update, Load lama -> APPLY "
        "-> RVA baru di-resolve otomatis dari nama method/field.");
    ImGui::PopStyleColor();
}

// ─── Status queries ───────────────────────────────────────────────────────────
bool IsResolving()   { return g_resolving.load(); }
int  GetResolveOk()  { return g_resolveOk.load();  }
int  GetResolveFail(){ return g_resolveFail.load(); }

} // namespace ProjectSessionManager