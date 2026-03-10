// ============================================================================
// Menu/MainWindow.cpp — Cyberpunk UI Edition
// Hexagon glowing toggle button · Neon accents · Animated scanlines
// ============================================================================

#include "Menu/MainWindow.h"
#include "Tool/Tool.h"
#include "Tool/Tab_Tracer.h"
#include "Tool/Tab_Dumper.h"
#include "Tool/Tab_ESP.h"
#include "Tool/Tab_Inspector.h"
#include "Tool/Tab_Network.h"
#include "Tool/ProjectSessionManager.h"
#include "Tool/ClassesTab.h"
#include "Tool/Keyboard.h"
#include "Il2cpp/Il2cpp.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <array>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cmath>

extern std::unordered_map<void *, HookerData> hookerMap;
extern ImVec2 initialScreenSize;

// ── State ────────────────────────────────────────────────────────────────────
bool menu_minimized = false;
bool show_menu      = true;

static ImVec2 toggle_pos  = ImVec2(50, 50);
static float  ui_scale    = 1.0f;
static float  s_fineScale = 1.0f;   // slider halus 0.5–2.0

bool collapsed   = false;
bool fullScreen  = false;

static bool        s_resetWindow   = false;
static int         s_selectedScale = 3;
static bool        s_doChangeScale = false;
static bool        s_doFineScale   = false;
static ImGuiStyle  s_initialStyle;
static ImVec2      s_lastSize, s_lastPos;

#ifdef __DEBUG__
static bool s_showDemoWindow = false;
#endif

constexpr std::array<const char *, 7> kScaleLabels  = {"Smallest","Smaller","Small","Default","Large","Larger","Largest"};
constexpr std::array<float,       7> kScaleFactors  = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};

extern void ConfigSet_int(const char *key, int value);
extern int  ConfigGet_int(const char *key, int defaultValue);

// ── Neon color helpers ────────────────────────────────────────────────────────
static constexpr ImU32 COL_CYAN       = IM_COL32(0,   217, 255, 255);
static constexpr ImU32 COL_CYAN_DIM   = IM_COL32(0,   140, 180, 180);
static constexpr ImU32 COL_CYAN_GLOW  = IM_COL32(0,   217, 255, 60);
static constexpr ImU32 COL_PURPLE     = IM_COL32(183, 25,  255, 255);
static constexpr ImU32 COL_PURPLE_DIM = IM_COL32(110, 15,  150, 180);
static constexpr ImU32 COL_DARK_BG    = IM_COL32(5,   5,   13,  240);
static constexpr ImU32 COL_WHITE      = IM_COL32(230, 245, 255, 255);

// ── Draw 6-point hexagon ──────────────────────────────────────────────────────
static void AddHexagonFilled(ImDrawList *dl, ImVec2 center, float r, ImU32 col, float rotation = 0.0f)
{
    const int n = 6;
    ImVec2 pts[n];
    for (int i = 0; i < n; i++) {
        float a = rotation + (float)i / n * 2.0f * IM_PI;
        pts[i] = ImVec2(center.x + r * cosf(a), center.y + r * sinf(a));
    }
    dl->AddConvexPolyFilled(pts, n, col);
}

static void AddHexagonOutline(ImDrawList *dl, ImVec2 center, float r, ImU32 col, float thickness = 2.0f, float rotation = 0.0f)
{
    const int n = 6;
    ImVec2 pts[n];
    for (int i = 0; i < n; i++) {
        float a = rotation + (float)i / n * 2.0f * IM_PI;
        pts[i] = ImVec2(center.x + r * cosf(a), center.y + r * sinf(a));
    }
    dl->AddPolyline(pts, n, col, ImDrawFlags_Closed, thickness);
}

// ============================================================================
// HEXAGON TOGGLE BUTTON — Cyberpunk Floating FAB
// ============================================================================
static void DrawToggleButton()
{
    const float HEX_R   = 55.0f * ui_scale;   // Hexagon circumradius
    const float WIN_SZ  = HEX_R * 2.4f;       // Window size (hex + glow margin)
    const float PADDING = (WIN_SZ - HEX_R * 2.0f) * 0.5f;

    ImGui::SetNextWindowPos(toggle_pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(WIN_SZ, WIN_SZ));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,     ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::Begin("##toggle_hex", nullptr,
        ImGuiWindowFlags_NoTitleBar     |
        ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoCollapse     |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    toggle_pos = ImGui::GetWindowPos();

    ImVec2 winPos = ImGui::GetWindowPos();
    ImVec2 center = ImVec2(winPos.x + WIN_SZ * 0.5f, winPos.y + WIN_SZ * 0.5f);

    // Check hover
    ImVec2 mouse = ImGui::GetIO().MousePos;
    float dist = sqrtf((mouse.x - center.x) * (mouse.x - center.x) +
                       (mouse.y - center.y) * (mouse.y - center.y));
    bool isHovered = dist < HEX_R;

    float time = ImGui::GetTime();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Pulse animation speed
    float pulse = (sinf(time * 3.0f) + 1.0f) * 0.5f;

    if (menu_minimized) {
        // ── State CLOSED: Red/Purple theme
        ImU32 bgColor    = IM_COL32(40, 3, 60, 200);
        ImU32 rimColor   = isHovered ? COL_PURPLE : IM_COL32(130, 20, 190, 200);
        ImU32 glowColor  = IM_COL32(183, 25, 255, (int)(30 + 30 * pulse));

        // Outer glow rings
        for (int i = 3; i >= 1; i--)
            AddHexagonOutline(dl, center, HEX_R + i * 6.0f, IM_COL32(183, 25, 255, (int)(15 * pulse / i)), 1.5f, IM_PI / 6.0f);

        // Background fill
        AddHexagonFilled(dl, center, HEX_R, bgColor, IM_PI / 6.0f);

        // Rim
        AddHexagonOutline(dl, center, HEX_R, rimColor, isHovered ? 3.5f : 2.5f, IM_PI / 6.0f);

        // Inner cross / "X" icon (show menu)
        float hs = HEX_R * 0.32f;
        ImU32 iconCol = IM_COL32(220, 80, 255, 230);
        dl->AddLine({center.x - hs, center.y - hs}, {center.x + hs, center.y + hs}, iconCol, 3.5f * ui_scale);
        dl->AddLine({center.x + hs, center.y - hs}, {center.x - hs, center.y + hs}, iconCol, 3.5f * ui_scale);

        // Hover glow ring
        if (isHovered)
            AddHexagonOutline(dl, center, HEX_R + 4.0f, IM_COL32(255, 255, 255, 80), 2.0f, IM_PI / 6.0f);

    } else {
        // ── State OPEN: Cyan theme
        ImU32 bgColor   = IM_COL32(3, 25, 40, 210);
        ImU32 rimColor  = isHovered ? COL_CYAN : COL_CYAN_DIM;

        // Outer glow rings (animated pulse)
        for (int i = 3; i >= 1; i--)
            AddHexagonOutline(dl, center, HEX_R + i * 6.0f, IM_COL32(0, 217, 255, (int)(20 * pulse / i)), 1.5f, IM_PI / 6.0f);

        // Fill
        AddHexagonFilled(dl, center, HEX_R, bgColor, IM_PI / 6.0f);

        // Inner subtle fill gradient illusion: smaller darker hex
        AddHexagonFilled(dl, center, HEX_R * 0.55f, IM_COL32(0, 60, 80, 120), IM_PI / 6.0f);

        // Rim
        AddHexagonOutline(dl, center, HEX_R, rimColor, isHovered ? 3.5f : 2.0f, IM_PI / 6.0f);

        // Inner checkmark / "✓" icon (menu open)
        float hs = HEX_R * 0.32f;
        ImU32 iconCol = isHovered ? COL_CYAN : COL_CYAN_DIM;
        // Draw simple ">" arrow as shorthand icon: three lines forming chevron
        ImVec2 p1 = {center.x - hs * 0.5f, center.y - hs * 0.7f};
        ImVec2 p2 = {center.x + hs * 0.5f, center.y};
        ImVec2 p3 = {center.x - hs * 0.5f, center.y + hs * 0.7f};
        dl->AddLine(p1, p2, iconCol, 4.0f * ui_scale);
        dl->AddLine(p2, p3, iconCol, 4.0f * ui_scale);

        // Hover white ring
        if (isHovered)
            AddHexagonOutline(dl, center, HEX_R + 4.0f, IM_COL32(255, 255, 255, 80), 2.0f, IM_PI / 6.0f);

        // Rotating inner decorative ring (slow spin when open)
        float rot = fmodf(time * 0.4f, 2.0f * IM_PI) + IM_PI / 6.0f;
        AddHexagonOutline(dl, center, HEX_R * 0.72f, IM_COL32(0, 217, 255, 40), 1.5f, rot);
    }

    // Tooltip text
    if (isHovered && !ImGui::IsMouseDragging(0)) {
        ImGui::SetTooltip("%s", menu_minimized ? "Open Menu" : "Close Menu");
    }

    // Invisible button for interaction
    ImGui::SetCursorScreenPos(ImVec2(winPos.x + PADDING, winPos.y + PADDING));
    ImGui::InvisibleButton("##hex_btn", ImVec2(HEX_R * 2.0f, HEX_R * 2.0f));

    if (ImGui::IsItemClicked() && !ImGui::IsMouseDragging(0)) {
        menu_minimized = !menu_minimized;
        show_menu      = !menu_minimized;
    }

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        toggle_pos.x += ImGui::GetIO().MouseDelta.x;
        toggle_pos.y += ImGui::GetIO().MouseDelta.y;

        ImVec2 disp = ImGui::GetIO().DisplaySize;
        toggle_pos.x = std::max(0.0f, std::min(toggle_pos.x, disp.x - WIN_SZ));
        toggle_pos.y = std::max(0.0f, std::min(toggle_pos.y, disp.y - WIN_SZ));
        ImGui::SetWindowPos(toggle_pos);
    }

    ImGui::End();
}

// ============================================================================
// Tracer Overlay — styled with neon text
// ============================================================================
static void DrawTracerOverlay()
{
    auto *drawList = ImGui::GetBackgroundDrawList();
    int i = 0;
    for (auto &v : HookerData::visited) {
        if (v.name.empty()) continue;
        char label[256]{0};
        snprintf(label, sizeof(label), "%s", v.name.c_str());
        if (v.hitCount > 0) snprintf(label, sizeof(label), "%s (%dx)", label, v.hitCount);

        auto labelSize = ImGui::CalcTextSize(label);
        ImVec2 pos{20, 150 + labelSize.y * i};

        float dt = ImGui::GetIO().DeltaTime;
        constexpr ImVec4 CYAN_V = {0.f, 0.85f, 1.f, 1.f};
        ImColor color = ImColor(1.f, 1.f, 1.f, 1.f);

        if (v.time > 0.f) {
            v.time -= dt;
            float t = v.time;
            color = ImColor(ImLerp(color.Value.x, CYAN_V.x, t),
                            ImLerp(color.Value.y, CYAN_V.y, t),
                            ImLerp(color.Value.z, CYAN_V.z, t), 1.f);
        }

        v.goneTime -= dt;
        if (v.goneTime > 0.f && v.goneTime <= 1.f) color.Value.w = ImLerp(0.f, color.Value.w, v.goneTime);
        if (v.goneTime <= 0.f) { v.name = ""; continue; }

        // Background strip with cyan tint
        drawList->AddRectFilled(
            pos, {pos.x + labelSize.x + 8, pos.y + labelSize.y + 2},
            IM_COL32(0, 10, 20, 160), 2.0f);
        // Cyan left border accent
        drawList->AddRectFilled(
            pos, {pos.x + 2, pos.y + labelSize.y + 2},
            IM_COL32(0, 217, 255, 200));
        // Label text offset
        drawList->AddText({pos.x + 6, pos.y + 1}, color, label);
        i++;
    }
}

// ============================================================================
// Helper: Styled section header with neon underline
// ============================================================================
static void CyberpunkSectionHeader(const char *label)
{
    ImGui::Dummy(ImVec2(0, 4));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w  = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddRectFilled(
        p, ImVec2(p.x + w, p.y + 1.5f), IM_COL32(0, 217, 255, 60));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 0.85f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImGui::GetCursorScreenPos(),
        ImVec2(ImGui::GetCursorScreenPos().x + w, ImGui::GetCursorScreenPos().y + 1.0f),
        IM_COL32(0, 217, 255, 40));
    ImGui::Dummy(ImVec2(0, 2));
}

// ============================================================================
// Settings Tab
// ============================================================================
static void DrawSettings()
{
    CyberpunkSectionHeader("DISPLAY");

    // ── Slider skala halus (0.5x – 2.0x) ─────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.85f, 1.0f, 0.9f));
    ImGui::TextUnformatted("UI Scale");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.7f, 0.9f, 1.0f));
    ImGui::Text("%.2fx", s_fineScale);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       ImVec4(0.0f, 0.85f, 1.0f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.72f, 0.1f, 1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          ImVec4(0.04f, 0.10f, 0.18f, 1.0f));
    if (ImGui::SliderFloat("##uiscale", &s_fineScale, 0.50f, 2.00f, "")) {
        // Clamp & snap ke 0.05 terdekat agar tidak terlalu smooth (lebih mudah set)
        s_fineScale = roundf(s_fineScale / 0.05f) * 0.05f;
        s_fineScale = std::max(0.50f, std::min(2.00f, s_fineScale));
        ConfigSet_int("fineScale100", (int)(s_fineScale * 100.f));
        s_doFineScale = true;
    }
    ImGui::PopStyleColor(3);

    // Tombol preset cepat dalam satu baris
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.25f, 0.35f, 0.50f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.85f, 1.00f, 0.25f));
    float btnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
    const float presets[] = {0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 1.75f};
    const char* pLabels[] = {"0.5x","0.75x","1x","1.25x","1.5x","1.75x"};
    int nPresets = 6;
    float avail = ImGui::GetContentRegionAvail().x;
    float pBtnW = (avail - ImGui::GetStyle().ItemSpacing.x * (nPresets-1)) / nPresets;
    for (int i = 0; i < nPresets; i++) {
        if (i > 0) ImGui::SameLine();
        bool isCur = fabsf(s_fineScale - presets[i]) < 0.01f;
        if (isCur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f,0.55f,0.7f,0.45f));
        if (ImGui::Button(pLabels[i], ImVec2(pBtnW, 0))) {
            s_fineScale = presets[i];
            ConfigSet_int("fineScale100", (int)(s_fineScale * 100.f));
            s_doFineScale = true;
        }
        if (isCur) ImGui::PopStyleColor();
    }
    ImGui::PopStyleColor(2);

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Checkbox("Keyboard fallback", &Keyboard::check);

#ifdef __DEBUG__
    ImGui::Checkbox("Show Demo Window", &s_showDemoWindow);
#endif

    // ── RVA Target Library ────────────────────────────────────────────────────
    CyberpunkSectionHeader("RVA TARGET LIBRARY");

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.7f, 0.9f, 1.0f));
    ImGui::Text("Library saat ini: %s", GetTargetLib());
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Dipakai untuk hitung RVA / offset semua method");
    ImGui::Dummy(ImVec2(0, 2));

    struct LibOption { const char* label; const char* lib; };
    static const LibOption kLibOptions[] = {
        { "libil2cpp.so",  "libil2cpp.so"  },
        { "libcsharp.so",  "libcsharp.so"  },
        { "libunity.so",   "liblogic.so"   },
    };
    float libBtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.f;
    for (int i = 0; i < 3; i++) {
        if (i > 0) ImGui::SameLine();
        bool isCur = (strcmp(GetTargetLib(), kLibOptions[i].lib) == 0);
        if (isCur) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f,0.55f,0.7f,0.55f));
        if (ImGui::Button(kLibOptions[i].label, ImVec2(libBtnW, 0)))
            SetTargetLib(kLibOptions[i].lib);
        if (isCur) ImGui::PopStyleColor();
    }

    // Input manual nama lib
    static char s_customLib[64] = {};
    if (s_customLib[0] == '\0')
        snprintf(s_customLib, sizeof(s_customLib), "%s", GetTargetLib());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.7f);
    ImGui::InputText("##customlib", s_customLib, sizeof(s_customLib));
    ImGui::SameLine();
    if (ImGui::Button("Set##lib", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        SetTargetLib(s_customLib);
    }
    ImGui::Dummy(ImVec2(0, 4));

    CyberpunkSectionHeader("DEVICE INFO");

    static auto packageName  = Il2cpp::getPackageName();
    static auto unityVersion = Il2cpp::getUnityVersion();
    static auto gameVersion  = Il2cpp::getGameVersion();

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.7f, 0.9f, 1.0f));
    ImGui::Text("Package : %s", packageName.c_str());
    ImGui::Text("Version : %s", gameVersion.c_str());
    ImGui::Text("Unity   : %s", unityVersion.c_str());
#ifdef __aarch64__
    ImGui::Text("Arch    : arm64-v8a");
#else
    ImGui::Text("Arch    : armeabi-v7a");
#endif
    ImGui::PopStyleColor();

    CyberpunkSectionHeader("LINKS");

    auto openURL = [](const char *url) {
        auto *App  = Il2cpp::FindClass("UnityEngine.Application");
        auto *mURL = App ? App->getMethod("OpenURL", 1) : nullptr;
        if (mURL) mURL->invoke_static<void>(Il2cpp::NewString(url));
        else      Keyboard::Open(url, nullptr);
    };

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.55f, 0.70f, 0.20f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.85f, 1.00f, 0.30f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 0.85f, 1.00f, 0.50f));
    if (ImGui::Button("[TG]  t.me/En_Xperience",          ImVec2(-1,0))) openURL(OBFUSCATE("https://t.me/En_Xperience"));
    if (ImGui::Button("[YT]  youtube.com/@mIsmanXP",       ImVec2(-1,0))) openURL(OBFUSCATE("https://www.youtube.com/@mIsmanXP"));
    if (ImGui::Button("[PM]  Platinmods Thread",           ImVec2(-1,0))) openURL(OBFUSCATE("https://platinmods.com/threads/imgui-il2cpp-tool.211155/"));
    ImGui::PopStyleColor(3);

    ImGui::Dummy(ImVec2(0, 4));
}

// ============================================================================
// Decorative scanline strip in title area
// ============================================================================
static void DrawTitleAccent(const char *label)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w  = ImGui::GetContentRegionAvail().x;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Subtle gradient bar behind title
    dl->AddRectFilledMultiColor(
        {p.x, p.y - 2}, {p.x + w, p.y + ImGui::GetTextLineHeight() + 4},
        IM_COL32(0, 80, 110, 80), IM_COL32(50, 0, 90, 80),
        IM_COL32(50, 0, 90, 0),   IM_COL32(0, 80, 110, 0));

    // Cyan underline
    dl->AddLine({p.x, p.y + ImGui::GetTextLineHeight() + 2},
                {p.x + w * 0.6f, p.y + ImGui::GetTextLineHeight() + 2},
                IM_COL32(0, 217, 255, 120), 1.0f);
}

// ============================================================================
// MAIN WINDOW DRAW
// ============================================================================
void MainWindow::Draw()
{
    DrawToggleButton();
    DrawTracerOverlay();

    if (menu_minimized || !show_menu) {
        ESPTab::Render();
        InspectorTab::DrawWindows();
        return;
    }

    // ── Window setup ──────────────────────────────────────────────────────────
    if (s_resetWindow) {
        s_resetWindow = false;
        if (fullScreen) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        } else {
            ImGui::SetNextWindowPos(s_lastPos);
            ImGui::SetNextWindowSize(s_lastSize);
        }
    }

    // Custom title bar color override
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       ImVec4(0.02f, 0.04f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,  ImVec4(0.00f, 0.35f, 0.50f, 0.80f));

    if (fullScreen)
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImGui::GetFrameHeight()));

    static const char *title = OBFUSCATE("[ IL2CPP Tool v2 ] — mIsmanXP");
    collapsed = !ImGui::Begin(title, nullptr,
        fullScreen ? ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove : 0);

    if (fullScreen) ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);

    Keyboard::Update();

    if (!collapsed) {
        // ── Tab bar with styled tabs ─────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_TabActive, ImVec4(0.00f, 0.85f, 1.00f, 0.25f));

        if (ImGui::BeginTabBar("mainTabber")) {

            if (ImGui::BeginTabItem("Tools")) {
                ImGui::Dummy(ImVec2(0, 2));
                if (ImGui::Checkbox("Fullscreen", &fullScreen)) {
                    if (fullScreen) {
                        s_lastSize = ImGui::GetWindowSize();
                        s_lastPos  = ImGui::GetWindowPos();
                    }
                    s_resetWindow = true;
                }
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0.85f, 1, 0.3f));
                ImGui::TextUnformatted("|");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.5f, 0.65f, 1.0f));
                ImGui::TextUnformatted("IL2CPP Tool v2");
                ImGui::PopStyleColor();

                ImGui::Separator();
                Tool::Draw();
                ImGui::EndTabItem();
            }

            if (!hookerMap.empty()) {
                if (ImGui::BeginTabItem("Tracer")) {
                    ImGui::Dummy(ImVec2(0, 2));
                    TracerTab::Draw();
                    ImGui::EndTabItem();
                }
            }

            if (ImGui::BeginTabItem("Dumper")) {
                ImGui::Dummy(ImVec2(0, 2));
                DumperTab::Draw();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("ESP")) {
                ImGui::Dummy(ImVec2(0, 2));
                ESPTab::Draw();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Session")) {
                ImGui::Dummy(ImVec2(0, 2));
                ProjectSessionManager::DrawUI(ClassesTab::oMap, ClassesTab::fieldPatchMap);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Network")) {
                ImGui::Dummy(ImVec2(0, 2));
                NetworkTab::Draw();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings")) {
                ImGui::Dummy(ImVec2(0, 2));
                DrawSettings();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::PopStyleColor(); // TabActive override
    }

    ImGui::End();

    ESPTab::Render();
    InspectorTab::DrawWindows();

#ifdef __DEBUG__
    if (s_showDemoWindow) ImGui::ShowDemoWindow();
#endif

    if (s_doChangeScale) {
        s_doChangeScale = false;
        static auto *font = ImGui::GetFont();
        font->Scale = kScaleFactors[s_selectedScale];
        auto style  = s_initialStyle;
        style.ScaleAllSizes(font->Scale);
        ImGui::GetStyle() = style;
        Tool::CalculateSomething();
        if (fullScreen) s_resetWindow = true;
    }

    // ── Fine-grained slider scale ─────────────────────────────────────────────
    if (s_doFineScale) {
        s_doFineScale = false;
        static auto *font2 = ImGui::GetFont();
        font2->Scale = s_fineScale;
        auto style2  = s_initialStyle;
        style2.ScaleAllSizes(s_fineScale);
        ImGui::GetStyle() = style2;
        ui_scale = s_fineScale;
        Tool::CalculateSomething();
        if (fullScreen) s_resetWindow = true;
    }
}

void MainWindow_InitScale(int selectedScale, ImGuiStyle initialStyle)
{
    s_selectedScale = selectedScale;
    s_initialStyle  = initialStyle;
    // Restore fine scale dari config (default 1.0x = 100)
    int fineScale100 = ConfigGet_int("fineScale100", 100);
    s_fineScale = std::max(0.50f, std::min(2.00f, fineScale100 / 100.0f));
    s_doFineScale   = true;
}
