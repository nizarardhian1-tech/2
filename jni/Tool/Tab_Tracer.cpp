// ============================================================================
// Tab_Tracer.cpp
// Implementasi tab Tracer — dipanggil dari MainWindow.
// Edit file ini untuk update tampilan/fitur tracer.
// ============================================================================

#include "Tool/Tab_Tracer.h"
#include "Tool.h"
#include "Tool/ClassesTab.h"
#include "imgui/imgui.h"
#include "Includes/Logger.h"
#include "Tool/Util.h"
#include <mutex>
#include <vector>
#include <algorithm>

extern std::unordered_map<void *, HookerData> hookerMap;
extern std::mutex hookerMtx;

void TracerTab::Draw()
{
    ImGui::Text("Traced method count : %zu", hookerMap.size());

    std::vector<HookerData *> sortedHooker;
    for (auto &[name, data] : hookerMap) {
        if (data.hitCount > 0) sortedHooker.push_back(&data);
    }

    if (sortedHooker.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,0,0,255));
        ImGui::Text("No traced method has been called yet");
        ImGui::PopStyleColor();
        return;
    }

    if (ImGui::Button("Quick Restore"))
        ImGui::OpenPopup("QuickRestorePopup");

    std::sort(sortedHooker.begin(), sortedHooker.end(),
        [](const HookerData *a, const HookerData *b) { return a->hitCount > b->hitCount; });

    ImGui::BeginChild("TracerList", ImVec2(0, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_HorizontalScrollbar);

    auto &tab = Tool::GetFirstTab();
    static bool changeToToolsTab = false;
    changeToToolsTab = false;

    for (auto v : sortedHooker) {
        char label[256]{0};
        snprintf(label, sizeof(label), "%s::%s (%dx)###%p",
            v->method->getClass()->getName(), v->method->getName(),
            v->hitCount, v->method);

        ImGui::PushID(v->method);
        bool opened = tab.MethodViewer(v->method->getClass(), v->method, tab.getCachedParams(v->method));
        if (!opened && !changeToToolsTab && ImGui::IsItemHeld()) {
            LOGD("IsItemHeld %s", v->method->getName());
            changeToToolsTab = true;
            Tool::OpenNewTabFromClass(v->method->getClass()).setOpenedTab = true;
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    static auto &io = ImGui::GetIO();
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(io.DisplaySize.x / 1.2f, 0),
        ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));

    if (ImGui::BeginPopup("QuickRestorePopup",
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_MenuBar))
    {
        HookerData *toBeErased = nullptr;
        for (auto v : sortedHooker) {
            char label[256]{0};
            snprintf(label, sizeof(label), "%s::%s (%dx)###%p",
                v->method->getClass()->getName(), v->method->getName(),
                v->hitCount, v->method);
            if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
                toBeErased = v;
        }
        if (toBeErased) Tool::ToggleHooker(toBeErased->method, 0);
        ImGui::EndPopup();
    }
}
