#pragma once

// ============================================================================
// Tab_Inspector.h
// Floating inspector windows (Field Watch, Call Inspector, Export).
// Edit Tab_Inspector.cpp untuk update fitur inspector.
// ============================================================================

namespace InspectorTab
{
    // Dipanggil tiap frame dari draw_thread, di luar window menu utama
    void DrawWindows();

} // namespace InspectorTab
