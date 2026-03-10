// ============================================================================
// ProjectSessionManager.h — Declarations only
// Implementasi ada di ProjectSessionManager.cpp
//
// FITUR:
//   - Save: simpan Namespace, ClassName, MethodName, FieldName ke JSON
//           TANPA hardcode RVA/Offset
//   - Load: resolve ulang RVA/Offset dari nama saat game dijalankan
//   - Ketika game update → RVA berubah → Load JSON lama → resolve otomatis
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>

#include "json/single_include/nlohmann/json.hpp"
#include "Il2cpp/il2cpp-class.h"
#include "Tool/ClassesTab.h"
#include "imgui/imgui.h"

namespace ProjectSessionManager
{

// ─── Tipe patch ───────────────────────────────────────────────────────────────
enum class PatchKind : int {
    MethodPatch = 0,
    FieldPatch  = 1,
};

// ─── Entry satu fitur dalam session ──────────────────────────────────────────
struct FeatureEntry
{
    std::string namespaceName;
    std::string className;
    std::string assemblyName;
    std::string methodName;
    int         methodParamCount = -1;
    std::string fieldName;
    PatchKind   kind = PatchKind::MethodPatch;

    std::string patchValue;
    std::string returnTypeName;
    std::string fieldTypeName;
    std::string displayLabel;
    std::string notes;

    // [Pilar 3] Hex signature fallback — dipakai jika resolve by-name gagal
    // Format: "AA BB CC ?? DD ?? FF" ('??' = wildcard byte)
    std::string hexSignature;
    int         signatureOffset = 0;

    // Derived saat load (tidak disimpan ke JSON)
    uintptr_t    resolvedRVA         = 0;
    uintptr_t    resolvedFieldOffset = 0;
    MethodInfo  *resolvedMethod      = nullptr;
    FieldInfo   *resolvedField       = nullptr;
    Il2CppClass *resolvedClass       = nullptr;
    bool         resolved            = false;
    std::string  resolveError;
    bool         resolvedViaPattern  = false;

    bool         hoveredForNavigation = false;
    float        hoverStartTime = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// PatternScanner — Hex signature scan untuk fallback resolve
// ─────────────────────────────────────────────────────────────────────────────
namespace PatternScanner {
    uintptr_t   Scan(const std::string& pattern, int offset = 0);
    uintptr_t   ScanRVA(const std::string& pattern, int offset = 0);
    std::string GenerateSignature(void* methodPtr, size_t sigLen = 16);
} // namespace PatternScanner

void to_json(nlohmann::ordered_json &j, const FeatureEntry &e);
void from_json(const nlohmann::ordered_json &j, FeatureEntry &e);

// ─── Session ──────────────────────────────────────────────────────────────────
struct Session
{
    std::string projectName = "MyProject";
    std::string gameVersion;
    std::string savedAt;
    std::string description;  // ⭐ Deskripsi session
    std::vector<FeatureEntry> features;
};

void to_json(nlohmann::ordered_json &j, const Session &s);
void from_json(const nlohmann::ordered_json &j, Session &s);

// ─── API publik ───────────────────────────────────────────────────────────────

/**
 * SaveSession — Kumpulkan data dari oMap + fieldPatchMap, simpan ke JSON.
 * Tidak menyimpan hardcode RVA, hanya nama string.
 */
bool SaveSession(
    const std::unordered_map<MethodInfo*, ClassesTab::OriginalMethodBytes> &oMap,
    const std::unordered_map<FieldInfo*, ClassesTab::FieldPatchData>       &fieldPatchMap);

/**
 * LoadSession — Baca JSON, lalu resolve RVA/offset di thread background.
 * Panggil IsResolving() untuk cek status, lalu ApplyResolved() jika selesai.
 */
bool LoadSession();

/**
 * ApplyResolved — Terapkan patch yang sudah di-resolve ke oMap/fieldPatchMap.
 * Kembalikan jumlah patch yang berhasil diapply.
 */
int ApplyResolved(
    std::unordered_map<MethodInfo*, ClassesTab::OriginalMethodBytes> &oMap,
    std::unordered_map<FieldInfo*, ClassesTab::FieldPatchData>       &fieldPatchMap);

/** DrawUI — ImGui panel untuk session manager. Panggil dari tab Tool. */
void DrawUI(
    std::unordered_map<MethodInfo*, ClassesTab::OriginalMethodBytes> &oMap,
    std::unordered_map<FieldInfo*, ClassesTab::FieldPatchData>       &fieldPatchMap);

// ⭐ Fungsi baru
void NavigateToFeature(const FeatureEntry& feature);
bool ExportUpdatedSession(const std::string& format); // "lua" atau "cpp"

// ─── Status query ─────────────────────────────────────────────────────────────
bool IsResolving();
int  GetResolveOk();
int  GetResolveFail();

} // namespace ProjectSessionManager