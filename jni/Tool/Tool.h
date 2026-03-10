#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "Il2cpp/il2cpp-class.h"
#include "Includes/circular_buffer.h"
#include "Tool/ClassesTab.h"
#include <set>

// ─── Overlay Tracer ───────────────────────────────────────────────────────────
struct HookerTrace
{
    std::string name;
    float time;
    float goneTime;
    int hitCount;
};

// ─── Inspector: satu parameter ter-capture ────────────────────────────────────
struct CapturedParam
{
    std::string name;
    std::string type;
    std::string value;
};

// ─── Inspector: satu field dari instance ──────────────────────────────────────
struct CapturedField
{
    std::string name;
    std::string type;
    uintptr_t   offset  = 0;
    std::string value;
    bool        canEdit = false;
};

// ─── Inspector: satu snapshot pemanggilan method ─────────────────────────────
// PENTING: struct ini dipakai sebagai invocation_data di Frida.
// Frida alokasikan raw bytes berdasarkan sizeof() → semua member harus trivially
// constructible ATAU kita harus placement-new + explicit dtor di on_enter/on_leave.
// Kita pakai pendekatan placement-new (sudah benar di Frida.cpp).
struct CallRecord
{
    uint32_t                   index     = 0;
    std::string                caller;
    std::vector<CapturedParam> params;
    std::vector<CapturedField> fields;
    std::string                retValue;
    void*                      thisPtr   = nullptr;
    bool                       hasReturn = false;
};

// ─── Field Watch: monitor field yang berubah secara realtime ─────────────────
struct WatchedField
{
    std::string  name;
    std::string  type;
    uintptr_t    offset    = 0;
    std::string  lastValue;
    std::string  label;     // anotasi user ("HP", "Cooldown Skill 1", dst)
    bool         changed   = false;
    bool         pinned    = false; // tampilkan di overlay
};

// ─── HookerData: state per-method yang di-hook ───────────────────────────────
struct HookerData
{
    int         hitCount  = 0;
    float       time      = 0.f;
    MethodInfo *method    = nullptr;

    // Tracking flags — ditulis UI thread, dibaca game thread via atomic
    bool trackCaller = false;
    bool trackParams = false;
    bool trackFields = false;
    bool trackReturn = false;

    // Inspector floating window
    bool inspectorOpen = false;

    // Call history — akses dilindungi hookerMtx
    uint32_t                   callCounter = 0;
    bool                       callReady   = false;
    CallRecord                 lastCall;
    CircularBuffer<CallRecord> callHistory{5};

    // Field Watch state
    bool                      watchActive      = false;
    std::vector<WatchedField> watchedFields;
    float                     watchLastRefresh = 0.f;
    void*                     watchInstance    = nullptr;

    // Legacy overlay
    static CircularBuffer<HookerTrace> visited;
    static std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> collectSet;
};

namespace Tool
{
    void ConfigSave();
    void ConfigLoad();
    void Init(Il2CppImage *image, std::vector<Il2CppImage *> images);
    void FilterClasses(const std::string &filter);
    void Draw();
    // Tracer() dan Hooker() dihapus — dead code.
    void GameObjects();
    void Dumper();
    void ESPTab();
    void ESPRender();   // Tick + Render overlay, dipanggil tiap frame dari main loop
    bool ToggleHooker(MethodInfo *method, int state = -1);
    void CalculateSomething();
    void DrawInspectorWindows();
    ClassesTab &GetFirstTab();
    ClassesTab &OpenNewTab();
    ClassesTab &OpenNewTabFromClass(Il2CppClass *klass);
} // namespace Tool
