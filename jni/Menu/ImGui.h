#pragma once
// ============================================================================
// Menu/ImGui.h — HYBRID ARCHITECTURE VERSION
//
// Header ini kompatibel dengan semua file Proyek 2 yang menginclude ImGui.h
// (MainWindow.cpp, Main.cpp lama, dll) tanpa perlu modifikasi file tersebut.
//
// PERUBAHAN:
//   ✗ Dihapus: deklarasi initModMenu(), swapbuffers_hook(), internalDrawMenu()
//   ✓ Ditambah: setupMenuContext(), MainWindow_InitScale()
//   ✓ Dipertahankan: semua global variable yang diakses file lain
// ============================================================================

#include <EGL/egl.h>
#include "imgui/imgui.h"

// ── Globals yang diakses dari MainWindow.cpp, Tab_*.cpp ──────────────────────
extern bool   isInitialized;
extern bool   collapsed;
extern bool   fullScreen;
extern bool   needClear;
extern int    glWidth;
extern int    glHeight;

// ── ImGui setup helpers ───────────────────────────────────────────────────────

/**
 * Setup ImGui context, font Roboto, Cyberpunk style, OpenGL3 backend.
 * Dipanggil dari imgui_render.cpp (render thread) setelah EGL ready.
 * glWidth dan glHeight HARUS sudah diisi sebelum memanggil ini.
 */
void setupMenuContext();

/**
 * Apply Cyberpunk color/style ke ImGui::GetStyle().
 * @param scaledFontSz  Ukuran font yang sudah di-scale
 */
void ApplyCyberpunkStyle(float scaledFontSz);

/**
 * Shorthand ApplyCyberpunkStyle dengan ukuran font dari glWidth/glHeight.
 * Dipanggil dari beberapa tempat di Proyek 2 sebagai menuStyle().
 */
void menuStyle();

/**
 * Scale semua ImGui style proportional.
 * Forward ke MainWindow_InitScale_impl() di MainWindow.cpp.
 */
void MainWindow_InitScale(int selectedScale, ImGuiStyle initialStyle);

// ── Compat getters ────────────────────────────────────────────────────────────
int getGlWidth();
int getGlHeight();
