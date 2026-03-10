// ============================================================================
// Menu/ImGui.cpp  — HYBRID ARCHITECTURE VERSION
//
// PORTING NOTES (dibandingkan dengan versi Proyek 2 asli):
//   ✗ DIHAPUS: initModMenu()     → render tidak lagi butuh hook EGL/Vulkan
//   ✗ DIHAPUS: swapbuffers_hook() / eglSwapBuffers Dobby hook
//   ✗ DIHAPUS: vkQueuePresentKHR_hook() / Vulkan hook  
//   ✗ DIHAPUS: EGLOverlay namespace (PBuffer context lama)
//   ✗ DIHAPUS: internalDrawMenu() lama (dipindah ke imgui_render.cpp)
//   ✗ DIHAPUS: ImGui_ImplAndroid_NewFrame() (tidak ada AInputQueue di ext process)
//
//   ✓ DIPERTAHANKAN: ApplyCyberpunkStyle()  — persis sama
//   ✓ DIPERTAHANKAN: setupMenuContext()     — setup ImGui context + font + style
//   ✓ DIPERTAHANKAN: MainWindow_InitScale() — disebut dari imgui_render.cpp
//
// FILE INI sekarang hanya berisi helper murni.
// Render loop yang sebenarnya ada di: imgui_ext/imgui_render.cpp
// ============================================================================

#include "ImGui.h"

// ImGui core — path sesuai struktur Proyek 2
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_opengl3.h"
// CATATAN: imgui_impl_android.h TIDAK diinclude di sini.
// Touch injection dilakukan langsung via JNI (lihat jni_bridge.cpp)

// Font embedding dari Proyek 2
#include "Includes/Roboto-Regular.h"

#include "Includes/Logger.h"
#include "Includes/Utils.h"

#include <GLES3/gl3.h>
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Global state — diakses dari MainWindow.cpp dan imgui_render.cpp
// ─────────────────────────────────────────────────────────────────────────────

// isInitialized: true setelah setupMenuContext() sukses
// glWidth/glHeight: dimensi surface EGL kita (dari imgui_render.cpp)
// initialScreenSize: dipakai MainWindow.cpp untuk layout calculation
bool   isInitialized   = false;
int    glWidth         = 0;
int    glHeight        = 0;
ImVec2 initialScreenSize = {0.f, 0.f};

// Tanda bahwa menu sedang collapse/fullscreen (dari MainWindow.cpp)
bool collapsed  = false;
bool fullScreen = false;

// needClear: di arsitektur baru selalu true (kita clear sendiri di renderLoop)
bool needClear  = true;

// ─────────────────────────────────────────────────────────────────────────────
// Scaling constants — persis dari Proyek 2
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float FONT_SCALE     = 0.033f;
static constexpr float TOUCH_PAD_FRAC = 0.30f;
static constexpr float ITEM_SPC_FRAC  = 0.25f;
static constexpr float SCROLLBAR_FRAC = 0.99f;
static constexpr float GRAB_MIN_FRAC  = 0.70f;

// ─────────────────────────────────────────────────────────────────────────────
// Cyberpunk Color Palette — persis dari Proyek 2
// ─────────────────────────────────────────────────────────────────────────────
namespace CyberpunkColors {
    constexpr ImVec4 BG_VOID       = {0.05f, 0.06f, 0.10f, 0.97f};
    constexpr ImVec4 BG_PANEL      = {0.07f, 0.08f, 0.13f, 0.95f};
    constexpr ImVec4 BG_CHILD      = {0.04f, 0.05f, 0.09f, 0.90f};
    constexpr ImVec4 BG_POPUP      = {0.05f, 0.05f, 0.12f, 0.98f};
    constexpr ImVec4 NEON_CYAN     = {0.00f, 0.85f, 1.00f, 1.00f};
    constexpr ImVec4 NEON_CYAN_DIM = {0.00f, 0.50f, 0.60f, 0.80f};
    constexpr ImVec4 NEON_CYAN_BG  = {0.00f, 0.85f, 1.00f, 0.12f};
    constexpr ImVec4 NEON_PURPLE   = {0.72f, 0.10f, 1.00f, 1.00f};
    constexpr ImVec4 TEXT_BRIGHT   = {0.90f, 0.95f, 1.00f, 1.00f};
    constexpr ImVec4 TEXT_DIM      = {0.40f, 0.50f, 0.65f, 1.00f};
    constexpr ImVec4 BORDER_GLOW   = {0.00f, 0.85f, 1.00f, 0.40f};
    constexpr ImVec4 BORDER_DIM    = {0.12f, 0.15f, 0.25f, 1.00f};
    constexpr ImVec4 TAB_ACTIVE    = {0.00f, 0.85f, 1.00f, 0.22f};
    constexpr ImVec4 TAB_INACTIVE  = {0.05f, 0.07f, 0.12f, 1.00f};
    constexpr ImVec4 TAB_HOVER     = {0.00f, 0.85f, 1.00f, 0.12f};
    constexpr ImVec4 BTN_BG        = {0.00f, 0.55f, 0.70f, 0.20f};
    constexpr ImVec4 BTN_HOVER     = {0.00f, 0.85f, 1.00f, 0.30f};
    constexpr ImVec4 BTN_ACTIVE    = {0.00f, 0.85f, 1.00f, 0.50f};
    constexpr ImVec4 HEADER_BG     = {0.00f, 0.60f, 0.80f, 0.14f};
    constexpr ImVec4 HEADER_HOVER  = {0.00f, 0.85f, 1.00f, 0.20f};
    constexpr ImVec4 HEADER_ACTV   = {0.00f, 0.85f, 1.00f, 0.30f};
    constexpr ImVec4 FRAME_BG      = {0.07f, 0.10f, 0.18f, 1.00f};
    constexpr ImVec4 FRAME_HOVER   = {0.10f, 0.15f, 0.25f, 1.00f};
    constexpr ImVec4 FRAME_ACTIVE  = {0.00f, 0.55f, 0.70f, 0.50f};
    constexpr ImVec4 SCROLL_BG     = {0.02f, 0.02f, 0.05f, 1.00f};
    constexpr ImVec4 SCROLL_GRAB   = {0.00f, 0.60f, 0.75f, 0.60f};
    constexpr ImVec4 SCROLL_HOV    = {0.00f, 0.85f, 1.00f, 0.80f};
    constexpr ImVec4 SCROLL_ACT    = {0.72f, 0.10f, 1.00f, 0.90f};
    constexpr ImVec4 CHECK_MARK    = {0.00f, 0.85f, 1.00f, 1.00f};
    constexpr ImVec4 SLIDER_GRAB   = {0.00f, 0.85f, 1.00f, 0.80f};
    constexpr ImVec4 SLIDER_ACT    = {0.72f, 0.10f, 1.00f, 1.00f};
    constexpr ImVec4 SEPARATOR     = {0.00f, 0.85f, 1.00f, 0.20f};
    constexpr ImVec4 TITLEBAR      = {0.02f, 0.04f, 0.10f, 1.00f};
    constexpr ImVec4 TITLEBAR_ACT  = {0.00f, 0.50f, 0.65f, 0.50f};
}

// ─────────────────────────────────────────────────────────────────────────────
// ApplyCyberpunkStyle — persis dari Proyek 2, tidak ada perubahan
// ─────────────────────────────────────────────────────────────────────────────
void ApplyCyberpunkStyle(float scaledFontSz)
{
    using namespace CyberpunkColors;
    ImGuiStyle& s = ImGui::GetStyle();

    const float r = scaledFontSz * 0.12f;
    s.WindowRounding    = r * 1.2f;
    s.ChildRounding     = r;
    s.FrameRounding     = r;
    s.PopupRounding     = r;
    s.ScrollbarRounding = r;
    s.GrabRounding      = r * 0.7f;
    s.TabRounding       = r;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;

    const float fp_x = scaledFontSz * 0.25f;
    const float fp_y = scaledFontSz * TOUCH_PAD_FRAC;
    s.WindowPadding    = ImVec2(scaledFontSz * 0.25f, scaledFontSz * 0.25f);
    s.FramePadding     = ImVec2(fp_x, fp_y);
    s.ItemSpacing      = ImVec2(scaledFontSz * 0.20f, scaledFontSz * ITEM_SPC_FRAC);
    s.ItemInnerSpacing = ImVec2(scaledFontSz * 0.15f, scaledFontSz * 0.10f);
    s.IndentSpacing    = scaledFontSz * 0.40f;
    s.ScrollbarSize    = scaledFontSz * SCROLLBAR_FRAC;
    s.GrabMinSize      = scaledFontSz * GRAB_MIN_FRAC;
    s.TabBarBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = TEXT_BRIGHT;
    c[ImGuiCol_TextDisabled]          = TEXT_DIM;
    c[ImGuiCol_WindowBg]              = BG_VOID;
    c[ImGuiCol_ChildBg]               = BG_CHILD;
    c[ImGuiCol_PopupBg]               = BG_POPUP;
    c[ImGuiCol_Border]                = BORDER_GLOW;
    c[ImGuiCol_BorderShadow]          = {0,0,0,0};
    c[ImGuiCol_FrameBg]               = FRAME_BG;
    c[ImGuiCol_FrameBgHovered]        = FRAME_HOVER;
    c[ImGuiCol_FrameBgActive]         = FRAME_ACTIVE;
    c[ImGuiCol_TitleBg]               = TITLEBAR;
    c[ImGuiCol_TitleBgActive]         = TITLEBAR_ACT;
    c[ImGuiCol_TitleBgCollapsed]      = {0.01f, 0.01f, 0.03f, 0.90f};
    c[ImGuiCol_MenuBarBg]             = BG_PANEL;
    c[ImGuiCol_ScrollbarBg]           = SCROLL_BG;
    c[ImGuiCol_ScrollbarGrab]         = SCROLL_GRAB;
    c[ImGuiCol_ScrollbarGrabHovered]  = SCROLL_HOV;
    c[ImGuiCol_ScrollbarGrabActive]   = SCROLL_ACT;
    c[ImGuiCol_CheckMark]             = CHECK_MARK;
    c[ImGuiCol_SliderGrab]            = SLIDER_GRAB;
    c[ImGuiCol_SliderGrabActive]      = SLIDER_ACT;
    c[ImGuiCol_Button]                = BTN_BG;
    c[ImGuiCol_ButtonHovered]         = BTN_HOVER;
    c[ImGuiCol_ButtonActive]          = BTN_ACTIVE;
    c[ImGuiCol_Header]                = HEADER_BG;
    c[ImGuiCol_HeaderHovered]         = HEADER_HOVER;
    c[ImGuiCol_HeaderActive]          = HEADER_ACTV;
    c[ImGuiCol_Separator]             = SEPARATOR;
    c[ImGuiCol_SeparatorHovered]      = NEON_CYAN_DIM;
    c[ImGuiCol_SeparatorActive]       = NEON_CYAN;
    c[ImGuiCol_ResizeGrip]            = NEON_CYAN_BG;
    c[ImGuiCol_ResizeGripHovered]     = NEON_CYAN_DIM;
    c[ImGuiCol_ResizeGripActive]      = NEON_CYAN;
    c[ImGuiCol_Tab]                   = TAB_INACTIVE;
    c[ImGuiCol_TabHovered]            = TAB_HOVER;
    c[ImGuiCol_TabActive]             = TAB_ACTIVE;
    c[ImGuiCol_TabUnfocused]          = {0.03f, 0.03f, 0.07f, 1.00f};
    c[ImGuiCol_TabUnfocusedActive]    = {0.05f, 0.07f, 0.14f, 1.00f};
    c[ImGuiCol_DockingPreview]        = NEON_CYAN_BG;
    c[ImGuiCol_DockingEmptyBg]        = BG_VOID;
    c[ImGuiCol_PlotLines]             = NEON_CYAN;
    c[ImGuiCol_PlotLinesHovered]      = NEON_PURPLE;
    c[ImGuiCol_PlotHistogram]         = NEON_CYAN;
    c[ImGuiCol_PlotHistogramHovered]  = NEON_PURPLE;
    c[ImGuiCol_TableHeaderBg]         = {0.03f, 0.06f, 0.12f, 1.00f};
    c[ImGuiCol_TableBorderStrong]     = BORDER_GLOW;
    c[ImGuiCol_TableBorderLight]      = BORDER_DIM;
    c[ImGuiCol_TableRowBg]            = {0.04f, 0.05f, 0.09f, 0.50f};
    c[ImGuiCol_TableRowBgAlt]         = {0.07f, 0.09f, 0.14f, 0.50f};
    c[ImGuiCol_TextSelectedBg]        = NEON_CYAN_BG;
    c[ImGuiCol_DragDropTarget]        = NEON_PURPLE;
    c[ImGuiCol_NavHighlight]          = NEON_CYAN;
    c[ImGuiCol_NavWindowingHighlight] = {1,1,1,0.7f};
    c[ImGuiCol_NavWindowingDimBg]     = {0.8f,0.8f,0.8f,0.2f};
    c[ImGuiCol_ModalWindowDimBg]      = {0.00f,0.00f,0.02f,0.70f};
}

// ─────────────────────────────────────────────────────────────────────────────
// setupMenuContext()
//
// Dipanggil SEKALI dari imgui_render.cpp (render thread) setelah EGL siap.
// Setup: ImGui context, font Roboto, Cyberpunk style, OpenGL3 backend.
//
// PERUBAHAN dari setupMenu() asli:
//   - Tidak memanggil onInitAddr() — on_init dijalankan dari internal_main.cpp
//   - Menggunakan ImGui_ImplOpenGL3 tanpa ImGui_ImplAndroid
//   - glWidth/glHeight diisi oleh imgui_render.cpp sebelum memanggil ini
// ─────────────────────────────────────────────────────────────────────────────
void setupMenuContext()
{
    if (isInitialized) return;

    IMGUI_CHECKVERSION();
    auto ctx = ImGui::CreateContext();
    if (!ctx) {
        LOGE("[ImGui] CreateContext() failed!");
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);

    // Disable fitur yang tidak dipakai di overlay
    io.IniFilename  = nullptr;  // Jangan simpan layout ke file
    io.LogFilename  = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // Backend OpenGL3 untuk EGL context kita
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Font — Roboto dengan ukuran proporsional layar
    const float shortSide = std::min((float)glWidth, (float)glHeight);
    const float fontSz    = std::max(12.0f, shortSide * FONT_SCALE);

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    io.Fonts->AddFontFromMemoryTTF(
        Roboto_Regular,
        sizeof(Roboto_Regular),
        fontSz,
        &cfg
    );

    // Apply Cyberpunk style
    ApplyCyberpunkStyle(fontSz);

    // Simpan initialScreenSize (dipakai MainWindow.cpp)
    initialScreenSize = ImVec2((float)glWidth, (float)glHeight);

    isInitialized = true;
    LOGI("[ImGui] setupMenuContext done. font=%.1fpx display=%dx%d", fontSz, glWidth, glHeight);
}

// ─────────────────────────────────────────────────────────────────────────────
// MainWindow_InitScale()
//
// Dipanggil dari imgui_render.cpp setelah setupMenuContext().
// Persis seperti yang dipanggil dari Main.cpp Proyek 2 asli.
// ─────────────────────────────────────────────────────────────────────────────
void MainWindow_InitScale(int selectedScale, ImGuiStyle initialStyle)
{
    // Forward ke fungsi di MainWindow.cpp yang sudah ada
    extern void MainWindow_InitScale_impl(int selectedScale, ImGuiStyle initialStyle);
    MainWindow_InitScale_impl(selectedScale, initialStyle);
}

// ─────────────────────────────────────────────────────────────────────────────
// menuStyle() — alias untuk ApplyCyberpunkStyle dengan ukuran default
// Dipakai dari beberapa tempat di Proyek 2
// ─────────────────────────────────────────────────────────────────────────────
void menuStyle()
{
    const float shortSide = std::min((float)glWidth, (float)glHeight);
    const float fontSz    = std::max(12.0f, shortSide * FONT_SCALE);
    ApplyCyberpunkStyle(fontSz);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stub/compat functions — dipertahankan agar file lain yang menginclude
// ImGui.h tidak perlu diubah.
// ─────────────────────────────────────────────────────────────────────────────

int getGlWidth()  { return glWidth; }
int getGlHeight() { return glHeight; }
