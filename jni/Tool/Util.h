#pragma once
#include "imgui/imgui.h"
#include <string>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

// ============================================================================
// Util.h
// Perubahan: Tambah EnsureDir() + struktur folder IL2CPPTOOLS/
//
// Struktur output:
//   getDataPath()/IL2CPPTOOLS/
//     session/    -> project_session.json
//     dump/       -> *.cs, *_dump.cs, class_tabs.json, all_patches_export.lua,
//                    *_patch.lua, dump_*.json
//     fields/     -> *_fields.h, *_fields.txt, *_fields.h (ClassesTab)
//     inspector/  -> *_inspector.h, *_inspector.txt
//     esp/        -> *_esp.h
//     codegen/    -> *_gg.lua, *.h (CodeGeneratorUI), MyProject_*.lua, MyProject_*.h
// ============================================================================

namespace Util
{
    void prependStringToBuffer(char *buffer, const char *string);
    std::string extractClassNameFromTypename(const char *typeName);

    // ── Folder root dan subfolder ─────────────────────────────────────────────
    // Semua path subfolder dipusatkan di sini.
    // Gunakan fungsi ini di mana saja agar konsisten dan mudah diganti.

    /** Buat folder beserta parent-nya jika belum ada. Return true jika berhasil. */
    bool EnsureDir(const std::string &path);

    /** Root IL2CPPTOOLS: getDataPath() + "/IL2CPPTOOLS" */
    std::string RootDir();

    /** getDataPath()/IL2CPPTOOLS/session/ */
    std::string DirSession();

    /** getDataPath()/IL2CPPTOOLS/dump/ */
    std::string DirDump();

    /** getDataPath()/IL2CPPTOOLS/fields/ */
    std::string DirFields();

    /** getDataPath()/IL2CPPTOOLS/inspector/ */
    std::string DirInspector();

    /** getDataPath()/IL2CPPTOOLS/esp/ */
    std::string DirESP();

    /** getDataPath()/IL2CPPTOOLS/codegen/ */
    std::string DirCodegen();

    // ─────────────────────────────────────────────────────────────────────────

    class FileWriter
    {
      public:
        FileWriter() = default;

        /**
         * Constructor lama (untuk kompatibilitas).
         * fileName = nama file saja (tanpa path), disimpan ke RootDir().
         * Lebih baik gunakan constructor dua-argumen di bawah.
         */
        FileWriter(const std::string &fileName);

        /**
         * Constructor baru: tentukan subfolder eksplisit.
         * subDir  = path folder lengkap (pakai DirSession(), DirESP(), dst.)
         * fileName = nama file saja.
         * Contoh: FileWriter(Util::DirESP(), "Player_esp.h")
         */
        FileWriter(const std::string &subDir, const std::string &fileName);

        void open();
        void init(const std::string &fileName);
        void init(const std::string &subDir, const std::string &fileName);
        void write(const char *data);
        bool exists();
        ~FileWriter();

        /** Path lengkap file yang sedang dibuka (berguna untuk Keyboard::Open). */
        const std::string &fullPath() const { return fileName; }

      private:
        std::ofstream fileStream;
        std::string   fileName;
    };

    class FileReader
    {
      public:
        /**
         * Constructor lama: cari fileName di RootDir().
         */
        FileReader(const std::string &fileName);

        /**
         * Constructor baru: cari fileName di subDir.
         */
        FileReader(const std::string &subDir, const std::string &fileName);

        std::string read();
        bool exists();
        ~FileReader();

      private:
        std::ifstream fileStream;
        std::string   fileName;
    };
} // namespace Util

namespace ImGui
{
    void ScrollWhenDraggingOnVoid_Internal(const ImVec2 &delta, ImGuiMouseButton mouse_button);
    void ScrollWhenDraggingOnVoid();
    bool IsItemHeld(float holdTime = 0.5f);

    void FpsGraph_Internal(const char *label, const std::vector<float> &fpsBuffer,
                           const ImVec2 &graphSize = ImVec2(-1, 200));
    void FpsGraph();

} // namespace ImGui
