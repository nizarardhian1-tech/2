// ============================================================================
// Tool.cpp
// Koordinator utama: Init, Config, Draw (tab Tools / ClassesTab),
// ToggleHooker, CalculateSomething, GameObjects.
//
// Fitur yang dipecah ke file terpisah:
//   ESPTab / ESPRender   -> Tool/Tab_ESP.cpp
//   DrawInspectorWindows -> Tool/Tab_Inspector.cpp
//   Dumper               -> Tool/Tab_Dumper.cpp
//   Tracer tab           -> Tool/Tab_Tracer.cpp
//   draw_thread / window -> Menu/MainWindow.cpp
// ============================================================================

#include "Tool.h"
#include "Il2cpp/Il2cpp.h"
#include "Tool/ProjectSessionManager.h"
#include "Tool/Keyboard.h"
#include "Tool/Util.h"
#include "Tool/Tab_ESP.h"
#include "Tool/Tab_Inspector.h"
#include "Tool/Tab_Dumper.h"
#include "Includes/Utils.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <future>
#include <set>
#include <unordered_set>
#include <mutex>
#include <cstring>
#include <sstream>
#include <algorithm>
#include "json/single_include/nlohmann/json.hpp"

#ifdef USE_FRIDA
#include "Tool/Frida.h"
#else
#include "Dobby/include/dobby.h"
#endif

extern ImVec2 initialScreenSize;

std::unordered_map<void *, HookerData> hookerMap;
std::mutex hookerMtx;

std::vector<Il2CppImage *> g_Images;
CircularBuffer<HookerTrace> HookerData::visited{50};
std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> HookerData::collectSet{};

// ─── Utility: SmallButton berwarna ───────────────────────────────────────────
static bool ColorSmallBtn(const char *lbl, ImU32 bg, ImU32 bgHov = 0)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgHov ? bgHov : bg);
    bool r = ImGui::Button(lbl, ImVec2(ImGui::GetIO().DisplaySize.x * 0.16f, 0));
    ImGui::PopStyleColor(2);
    return r;
}

static void FingerScroll(float &prev)
{
    auto &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        if (io.MouseDown[0]) {
            if (prev >= 0.f) ImGui::SetScrollY(ImGui::GetScrollY() + (prev - io.MousePos.y));
            prev = io.MousePos.y;
        } else prev = -1.f;
    } else prev = -1.f;
}

#ifndef USE_FRIDA
extern void hookerHandler(void *address, DobbyRegisterContext *ctx);
#endif

namespace Tool
{
    std::vector<ClassesTab> classesTabs;

    // ── Config ────────────────────────────────────────────────────────────────
    void ConfigLoad()
    {
        try {
            Util::FileReader f("class_tabs.json");
            classesTabs = nlohmann::ordered_json::parse(f.read())
                              .template get<std::vector<ClassesTab>>();
        } catch (nlohmann::json::exception &e) {
            LOGE("ConfigLoad: %s", e.what());
            ConfigSave();
        }
    }

    void ConfigSave()
    {
        Util::FileWriter f("class_tabs.json");
        nlohmann::ordered_json j = classesTabs;
        f.write(j.dump(2, ' ').c_str());
    }

    static void ConfigInit()
    {
        Util::FileReader f("class_tabs.json");
        if (f.exists()) ConfigLoad(); else ConfigSave();
    }

    void CalculateSomething()
    {
        constexpr auto *P = "BRUH";
        int max = 10;
        for (int i = 0; i < 100; i++) {
            float y = 150.f + ImGui::CalcTextSize(P).y * i;
            if (y >= ImGui::GetIO().DisplaySize.y) { max = i - 5; break; }
        }
        if (max < 5) max = 5;
        HookerData::visited = CircularBuffer<HookerTrace>(max);
    }

    static void InitScreenSize()
    {
        try {
            auto *D = Il2cpp::FindClass("UnityEngine.Display");
            if (D) {
                auto *mMain = D->getMethod("get_main", 0);
                if (mMain) {
                    auto *m = mMain->invoke_static<Il2CppObject *>();
                    if (m) {
                        int w = m->invoke_method<int32_t>("get_systemWidth");
                        int h = m->invoke_method<int32_t>("get_systemHeight");
                        if (w > 0 && h > 0) { initialScreenSize.x = (float)w; initialScreenSize.y = (float)h; return; }
                    }
                }
            }
        } catch (...) {}

        try {
            auto *S = Il2cpp::FindClass("UnityEngine.Screen");
            if (S) {
                auto *mW = S->getMethod("get_width",  0);
                auto *mH = S->getMethod("get_height", 0);
                int w = mW ? mW->invoke_static<int32_t>() : 0;
                int h = mH ? mH->invoke_static<int32_t>() : 0;
                if (w > 0 && h > 0) { initialScreenSize.x = (float)w; initialScreenSize.y = (float)h; return; }
            }
        } catch (...) {}

        LOGW("[InitScreenSize] Fallback to ImGui DisplaySize");
    }

    ClassesTab &GetFirstTab() { return classesTabs[0]; }

    ClassesTab &OpenNewTab()
    {
        ClassesTab c;
        for (auto &t : classesTabs) if (t.currentlyOpened) { c.selectedImage = t.selectedImage; break; }
        return classesTabs.emplace_back(c);
    }

    ClassesTab &OpenNewTabFromClass(Il2CppClass *klass)
    {
        auto &t = OpenNewTab();
        t.filter = klass->getFullName();
        t.selectedImage = klass->getImage();
        t.FilterClasses(t.filter);
        return t;
    }

    void Init(Il2CppImage *image, std::vector<Il2CppImage *> images)
    {
        ConfigInit();
        InitScreenSize();
        g_Images = images;
        std::sort(g_Images.begin(), g_Images.end(),
            [](Il2CppImage *a, Il2CppImage *b) { return strcmp(a->getName(), b->getName()) < 0; });
        classesTabs.reserve(32);
        if (classesTabs.empty()) OpenNewTab();
        for (auto &t : classesTabs) t.FilterClasses(t.filter);
#ifdef USE_FRIDA
        Frida::Init();
#endif
    }

    void Draw()
    {
        [[maybe_unused]] static auto _ = [] { CalculateSomething(); return true; }();
        if (ImGui::BeginTabBar("tabber",
            ImGuiTabBarFlags_AutoSelectNewTabs | ImGuiTabBarFlags_FittingPolicyScroll |
            ImGuiTabBarFlags_TabListPopupButton))
        {
            if (ImGui::TabItemButton("+", ImGuiTabItemFlags_NoTooltip | ImGuiTabItemFlags_Leading)) {
                auto &t = OpenNewTab(); t.FilterClasses(t.filter);
            }

            int i = 0;
            auto it = classesTabs.begin();
            while (it != classesTabs.end()) {
                if (!it->opened) {
                    it = classesTabs.erase(it);
                    if (classesTabs.empty()) { auto &t = OpenNewTab(); t.FilterClasses(t.filter); break; }
                    ConfigSave();
                } else {
                    ImGui::PushID(i);
                    it->Draw(i, true);
                    it->DrawTabMap();
                    ImGui::PopID();
                    ++it; i++;
                }
            }
            ImGui::EndTabBar();
        }
    }

    bool ToggleHooker(MethodInfo *method, int state)
    {
        if (!ClassesTab::oMap[method].bytes.empty()) { LOGE("Can't hook while patched!"); return false; }

        auto it    = hookerMap.find(method->methodPointer);
        bool hooked = (it != hookerMap.end());

#ifdef USE_FRIDA
        auto Enable = [&]() -> bool {
            LOGD("%s", method->getName());
            std::lock_guard<std::mutex> g(hookerMtx);
            hookerMap[method->methodPointer].hitCount = 0;
            hookerMap[method->methodPointer].method   = method;
            if (!Frida::Trace(method, &hookerMap[method->methodPointer])) {
                hookerMap.erase(method->methodPointer);
                LOGE("Failed %s", method->getName()); return false;
            }
            return true;
        };
        auto Disable = [&]() -> bool {
            if (Frida::Untrace(method)) {
                std::lock_guard<std::mutex> g(hookerMtx);
                hookerMap.erase(method->methodPointer); return true;
            }
            LOGE("Failed restore %s", method->getName()); return false;
        };
#else
        auto Enable = [&]() -> bool {
            LOGD("%s", method->getName());
            // FIX CRASH: Insert ke hookerMap SEBELUM DobbyInstrument.
            // Kalau dibalik, ada race condition: Dobby langsung memanggil
            // hookerHandler di game thread, tapi entry hookerMap belum ada
            // → hookerMap.find() miss → data tidak ter-record. Lebih buruk,
            // jika hookerHandler dipanggil saat hookerMap sedang rehash
            // (karena insert concurrent) → undefined behavior / crash.
            {
                std::lock_guard<std::mutex> g(hookerMtx);
                HookerData hd;
                hd.hitCount = 0;
                hd.time     = 0.f;
                hd.method   = method;
                hookerMap.emplace(method->methodPointer, std::move(hd));
            }
            bool ok = (DobbyInstrument((void *)method->methodPointer, hookerHandler) == 0);
            if (!ok) {
                // Rollback jika Dobby gagal
                std::lock_guard<std::mutex> g(hookerMtx);
                hookerMap.erase(method->methodPointer);
                LOGE("Failed %s", method->getName());
            }
            return ok;
        };
        auto Disable = [&]() -> bool {
            if (DobbyDestroy(method->methodPointer) == 0) {
                std::lock_guard<std::mutex> g(hookerMtx);
                hookerMap.erase(method->methodPointer); return true;
            }
            LOGE("Failed restore %s", method->getName()); return false;
        };
#endif
        if (state == -1) return hooked ? Disable() : Enable();
        if (state == 0 && hooked)  return Disable();
        if (state == 1 && !hooked) return Enable();
        return false;
    }

    void GameObjects()
    {
        static Il2CppClass *goKlass = Il2cpp::FindClass("UnityEngine.GameObject");
        static std::vector<Il2CppObject *> GOs;
        static float goLastScan = -999.f;
        float goNow = ImGui::GetTime();
        if (goKlass && GOs.empty() && (goNow - goLastScan) > 5.f) {
            goLastScan = goNow;
            try { GOs = Il2cpp::GC::FindObjects(goKlass); } catch (...) { GOs.clear(); }
        }
        static Il2CppObject *cam = []() -> Il2CppObject * {
            try {
                auto *C = Il2cpp::FindClass("UnityEngine.Camera");
                if (!C) return nullptr;
                auto *mMain = C->getMethod("get_main", 0);
                return mMain ? mMain->invoke_static<Il2CppObject *>() : nullptr;
            } catch (...) { return nullptr; }
        }();
        static MethodInfo *WTS = [&]() -> MethodInfo * {
            if (!cam) return nullptr;
            auto ms = cam->klass->getMethods("WorldToScreenPoint");
            for (auto *m : ms) if (m && m->getParamsInfo().size() == 1) return m;
            return nullptr;
        }();

        struct V3 { float x, y, z; };
        ImGui::Text("GameObjects: %zu", GOs.size());
        if (!cam || !WTS) { ImGui::TextDisabled("Camera not available"); return; }
        auto *dl = ImGui::GetBackgroundDrawList();
        ImVec2 ctr(ImGui::GetIO().DisplaySize.x / 2, ImGui::GetIO().DisplaySize.y);
        for (auto *go : GOs) {
            if (!go) continue;
            try {
                auto *tr = go->invoke_method<Il2CppObject *>("get_transform");
                V3 pos = tr->invoke_method<V3>("get_position");
                V3 s = WTS->invoke_static<V3>(cam, pos);
                if (s.z <= 0) continue;
                dl->AddLine(ctr, ImVec2(s.x, ImGui::GetIO().DisplaySize.y - s.y), IM_COL32(255,50,50,180));
            } catch (...) {}
        }
    }

    // Delegate ke Tab_ESP.cpp
    void ESPTab()    { ESPTab::Draw(); }
    void ESPRender() { ESPTab::Render(); }

    // Delegate ke Tab_Inspector.cpp
    void DrawInspectorWindows() { InspectorTab::DrawWindows(); }

    void FilterClasses(const std::string &f)
    {
        for (auto &t : classesTabs) t.FilterClasses(f);
    }

    void Dumper()  { DumperTab::Draw(); }
    // LANGKAH 2 - Dead Code Removal:
    // void Tracer() dan void Hooker() dihapus — fungsi kosong tanpa implementasi.
    // Jika dibutuhkan di masa depan, implementasi ada di Tab_Tracer.cpp.

} // namespace Tool
