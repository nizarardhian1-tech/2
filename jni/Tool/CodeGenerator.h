// ============================================================================
// CodeGenerator.h  [REFACTORED — Pilar 1: Feature Consolidation]
// ============================================================================
//
// File ini sekarang adalah thin wrapper / alias layer.
// Semua implementasi nyata telah dipindahkan ke:
//
//   Tool/CodeExporter.h   <- SINGLE SOURCE OF TRUTH
//
// Alasan refactor:
//   1. DRY Principle: DerivePatchBytes sebelumnya terduplikasi di ClassesTab.cpp
//      dengan LDR offset yang BERBEDA (0x18000040 vs 0x18000060) — menyebabkan
//      bug patch byte yang tidak konsisten.
//   2. File ini sebelumnya header-only 890 baris tanpa .cpp pasangannya.
//   3. SaveFile dan CollectPatches kini terpusat di CodeExporter.
//
// Backward compatibility:
//   - Semua simbol lama (CGPatchEntry, CodeGenerator::ExportAll, CodeGeneratorUI)
//     tetap tersedia via include + alias/using di bawah.
//   - Tidak ada perubahan di call site yang sudah ada.
//
// ============================================================================
#pragma once

#include "Tool/CodeExporter.h"
#include "imgui/imgui.h"
#include "Tool/Keyboard.h"

// ============================================================================
// namespace CodeGenerator — alias ke CodeExporter agar kode lama tetap kompil
// ============================================================================
namespace CodeGenerator {

    using CodeExporter::Sanitize;
    using CodeExporter::TypeToC;
    using CodeExporter::TypeToGG;
    using CodeExporter::DerivePatchBytes;
    using CodeExporter::SaveFile;
    using CodeExporter::CollectPatches;
    using CodeExporter::ExportAll;
    using CodeExporter::ExportGlobalLuaMenu;
    using CodeExporter::ExportAIDEHeaders;
    using CodeExporter::BuildClassLua;
    using CodeExporter::BuildClassCpp;

} // namespace CodeGenerator

// ============================================================================
// CodeGeneratorUI — ImGui tab untuk per-class Lua/C++ header generation
// ============================================================================
class CodeGeneratorUI {
public:
    enum class OutputFormat { LuaGG = 0, CppHeader = 1, Both = 2 };

private:
    Il2CppClass*  selectedClass_  = nullptr;
    OutputFormat  outputFormat_   = OutputFormat::Both;
    std::string   generatedLua_;
    std::string   generatedCpp_;
    bool          generated_      = false;
    char          classFilterBuf_[128] = {};
    std::vector<Il2CppClass*> filteredClasses_;
    bool          needsRefresh_   = true;

    static std::string GetOutputDir() { return Util::DirCodegen(); }

public:
    CodeGeneratorUI() = default;

    void Draw(const std::vector<Il2CppImage*>& images) {
        auto& io = ImGui::GetIO();

        ImGui::TextColored(ImVec4(1, 1, 0, 1), "Code Generator");
        ImGui::Text("Generate Lua (GG) dan C++ Header dari class IL2CPP");
        ImGui::Separator();

        // Output format
        ImGui::Text("Output:");
        ImGui::SameLine();
        int fmt = (int)outputFormat_;
        if (ImGui::RadioButton("Lua (GG)",   &fmt, 0)) outputFormat_ = OutputFormat::LuaGG;
        ImGui::SameLine();
        if (ImGui::RadioButton("C++ Header", &fmt, 1)) outputFormat_ = OutputFormat::CppHeader;
        ImGui::SameLine();
        if (ImGui::RadioButton("Keduanya",   &fmt, 2)) outputFormat_ = OutputFormat::Both;
        ImGui::Separator();

        // Assembly selector
        static int selImg = 0;
        ImGui::Text("Assembly:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(io.DisplaySize.x * 0.35f);
        if (ImGui::BeginCombo("##CGImg",
                selImg < (int)images.size() ? images[selImg]->getName() : "Select...")) {
            for (int i = 0; i < (int)images.size(); i++) {
                if (ImGui::Selectable(images[i]->getName(), selImg == i)) {
                    selImg = i; needsRefresh_ = true;
                }
            }
            ImGui::EndCombo();
        }

        // Class filter
        ImGui::Text("Filter:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(io.DisplaySize.x * 0.30f);
        if (ImGui::InputText("##CGFlt", classFilterBuf_, sizeof(classFilterBuf_)))
            needsRefresh_ = true;
        ImGui::SameLine();
        if (ImGui::Button("Refresh##CG")) needsRefresh_ = true;

        // Refresh filtered list
        if (needsRefresh_ && selImg < (int)images.size()) {
            filteredClasses_.clear();
            std::string flt = classFilterBuf_;
            std::transform(flt.begin(), flt.end(), flt.begin(), ::tolower);
            for (auto* k : images[selImg]->getClasses()) {
                if (!k) continue; const char* cn = k->getName(); if (!cn) continue;
                if (flt.empty()) {
                    filteredClasses_.push_back(k);
                } else {
                    std::string lower = cn;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find(flt) != std::string::npos)
                        filteredClasses_.push_back(k);
                }
            }
            needsRefresh_ = false;
        }

        // Split panel
        const float panelH = ImGui::GetContentRegionAvail().y - 70.f;
        const float listW  = io.DisplaySize.x * 0.30f;

        ImGui::BeginChild("CGClassList", ImVec2(listW, panelH), true,
                          ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::Text("Classes (%zu):", filteredClasses_.size());
        ImGui::Separator();
        for (auto* k : filteredClasses_) {
            if (!k) continue; const char* cn = k->getName(); if (!cn) continue;
            if (ImGui::Selectable(cn, selectedClass_ == k)) {
                selectedClass_ = k;
                generated_     = false;
                generatedLua_.clear(); generatedCpp_.clear();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginChild("CGPreview", ImVec2(0, panelH), true,
                          ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_HorizontalScrollbar);
        if (selectedClass_) {
            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Class: %s", selectedClass_->getName());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - io.DisplaySize.x * 0.15f);
            // [FIX] SmallButton -> Button (touch-friendly)
            if (ImGui::Button("Generate", ImVec2(io.DisplaySize.x * 0.14f, 0))) {
                if (outputFormat_ == OutputFormat::LuaGG || outputFormat_ == OutputFormat::Both)
                    generatedLua_ = CodeExporter::BuildClassLua(selectedClass_);
                if (outputFormat_ == OutputFormat::CppHeader || outputFormat_ == OutputFormat::Both)
                    generatedCpp_ = CodeExporter::BuildClassCpp(selectedClass_);
                generated_ = true;
            }
            ImGui::Separator();

            if (!generated_) {
                ImGui::TextColored(ImVec4(.6f, .6f, .6f, 1), "Tekan Generate untuk preview");
            } else if (ImGui::BeginTabBar("CGPreviewTabs")) {
                if (!generatedLua_.empty() && ImGui::BeginTabItem("Lua (GG)")) {
                    ImGui::InputTextMultiline("##LuaP",
                        const_cast<char*>(generatedLua_.c_str()),
                        generatedLua_.size() + 1,
                        ImVec2(-1, ImGui::GetContentRegionAvail().y - 30),
                        ImGuiInputTextFlags_ReadOnly);
                    ImGui::EndTabItem();
                }
                if (!generatedCpp_.empty() && ImGui::BeginTabItem("C++ Header")) {
                    ImGui::InputTextMultiline("##CppP",
                        const_cast<char*>(generatedCpp_.c_str()),
                        generatedCpp_.size() + 1,
                        ImVec2(-1, ImGui::GetContentRegionAvail().y - 30),
                        ImGuiInputTextFlags_ReadOnly);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        } else {
            ImGui::TextColored(ImVec4(.5f, .5f, .5f, 1), "Pilih class dari list");
        }
        ImGui::EndChild();
        ImGui::Separator();

        // Save buttons
        if (generated_ && selectedClass_) {
            const std::string dir = GetOutputDir();
            const std::string cn  = CodeExporter::Sanitize(
                selectedClass_->getName() ? selectedClass_->getName() : "unknown");
            const float btnW = io.DisplaySize.x * 0.40f;

            if (!generatedLua_.empty()) {
                std::string path = dir + "/" + cn + "_gg.lua";
                if (ImGui::Button(("Save -> " + cn + "_gg.lua").c_str(), ImVec2(btnW, 0)))
                    CodeExporter::SaveFile(generatedLua_, path);
                ImGui::SameLine();
                // [FIX] SmallButton -> Button
                if (ImGui::Button("Open##Lua"))
                    Keyboard::Open(path.c_str(), [](const std::string&){});
            }
            if (!generatedCpp_.empty()) {
                std::string path = dir + "/" + cn + ".h";
                if (ImGui::Button(("Save -> " + cn + ".h").c_str(), ImVec2(btnW, 0)))
                    CodeExporter::SaveFile(generatedCpp_, path);
                ImGui::SameLine();
                if (ImGui::Button("Open##Cpp"))
                    Keyboard::Open(path.c_str(), [](const std::string&){});
            }
            ImGui::TextColored(ImVec4(.5f, .5f, .5f, 1), "Dir: %s", dir.c_str());
        }
    }
};
