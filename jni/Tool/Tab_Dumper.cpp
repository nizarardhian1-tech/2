// ============================================================================
// Tab_Dumper.cpp
// Implementasi tab Dumper.
// Edit file ini untuk update fitur dump IL2CPP.
// ============================================================================

#include "Tool/Tab_Dumper.h"
#include "Tool/Keyboard.h"
#include "Tool/Util.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/Logger.h"
#include "imgui/imgui.h"

#include <future>
#include <string>

void DumperTab::Draw()
{
    static std::string currentDump;
    static bool dumping  = false;
    static bool dumped   = false;
    static std::shared_future<void> dumpFuture;
    static char outFile[256] = {};

    if (!currentDump.empty())
        ImGui::Text("Status: %s", currentDump.c_str());

    float bw     = ImGui::GetContentRegionAvail().x;
    bool canDump = !dumping || dumped;
    if (!canDump) ImGui::BeginDisabled();

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30,100,30,220));
    if (ImGui::Button(dumped ? "Dump Again" : "DUMP", ImVec2(bw * 0.5f, 0))) {
        dumping = true; dumped = false; currentDump = "Starting...";
        snprintf(outFile, sizeof(outFile), "%s/%s_%s.cs",
            Util::DirDump().c_str(),
            Il2cpp::getPackageName().c_str(),
            Il2cpp::getGameVersion().c_str());
        dumpFuture = std::async(std::launch::async, [] {
            il2cpp_dump(outFile, [](const char *name, int, int) { currentDump = name; });
        }).share();
    }
    ImGui::PopStyleColor();
    if (!canDump) ImGui::EndDisabled();

    if (dumping && !dumped && dumpFuture.valid()) {
        if (dumpFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            currentDump = "Done! File: " + std::string(outFile);
            dumped = true;
        }
    }

    if (dumped) {
        ImGui::SameLine();
        if (ImGui::Button("Copy Path", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            Keyboard::Open(outFile, nullptr);
    }
}
