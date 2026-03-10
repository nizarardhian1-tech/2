#include "Logger.h"
#include "imgui/imgui.h"

// Stub Draw — tetap ada biar kode UI tidak error
// Tidak ada log ImGui buffer lagi, semua sudah ke logcat
namespace logger {
    void Draw(const char *title, bool *p_open)
    {
        (void)title; (void)p_open;
        ImGui::TextDisabled("Log -> logcat (tag: MXP)");
    }
}
