// AUTO-GENERATED SHARED HEADER — included by all ClassesTab sub-files
// Pilar 3: ClassesTab.cpp dipecah menjadi beberapa file terpisah
#pragma once
#include "Tool/ClassesTab.h"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-tabledefs.h"
#include "Tool/Keyboard.h"
#include "Tool/Patcher.h"
#include "Tool/Tool.h"
#include "Tool/Util.h"
#include "Tool/CodeGenerator.h"
#include "imgui/imgui.h"
#include "Includes/Utils.h"
#include "KittyMemory/KittyMemory.h"
#include <mutex>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#ifndef USE_FRIDA
#include "Dobby/include/dobby.h"
#endif

// ── Globals yang didefinisikan di Tool.cpp ───────────────────────────────────
extern std::unordered_map<void *, HookerData> hookerMap;
extern std::mutex hookerMtx;
// ── maxLine didefinisikan di View_MethodAnalyzer.cpp ─────────────────────────
extern int maxLine;
// ── g_Images definisi di View_ClassBrowser.cpp, g_Image dari Main/Il2cpp ─────
extern std::vector<Il2CppImage *> g_Images;
extern Il2CppImage *g_Image;
// ── IL2CPP function pointers (didefinisikan di Il2cpp.cpp) ───────────────────
extern void* (*il2cpp_class_get_static_field_data)(Il2CppClass* klass);
extern void  (*il2cpp_field_static_get_value)(FieldInfo* field, void* value);
extern void  (*il2cpp_runtime_class_init)(Il2CppClass* klass);

// ── MemViewState: struct dan global g_memViewStates (def di View_MemoryHex.cpp)
struct MemViewState {
    Il2CppObject *target      = nullptr;
    uintptr_t     baseAddr    = 0;
    int           viewSize    = 512;
    int           bytesPerRow = 16;
    bool          followObj   = true;
    char          gotoHex[20] = {0};
    int           highlightOffset = -1;
    int           highlightSize   = 1;
    bool          open        = false;
};
extern std::unordered_map<void*, MemViewState> g_memViewStates;

// ── Helper functions lintas file ─────────────────────────────────────────────
bool IsTransformType(const char *tn);
void DrawMemoryViewer(MemViewState &mv);
