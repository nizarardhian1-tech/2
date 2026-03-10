#pragma once

// ============================================================================
// Menu/MainWindow.h
// Entry point render loop: draw_thread() dan overlay tracer.
//
// Edit MainWindow.cpp untuk:
//   - Ganti urutan / nama tab utama (Tools, Tracer, Dumper, ESP, Session, Settings)
//   - Edit tampilan overlay method tracer (HookerData::visited)
//   - Edit panel Settings (scale, info game, link)
// ============================================================================

namespace MainWindow
{
    // Dipanggil tiap frame dari ImGui render loop
    void Draw();

} // namespace MainWindow
