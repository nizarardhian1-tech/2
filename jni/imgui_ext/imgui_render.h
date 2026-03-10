#pragma once
// ============================================================================
// imgui_ext/imgui_render.h
// Public API untuk imgui_render.cpp — dipanggil dari jni_bridge.cpp
//
// CATATAN: Ini adalah NAMESPACE, bukan class.
// jni_bridge.cpp memanggil langsung:  ImGuiRenderer::init(...)
// imgui_render.cpp mengimplementasi:  namespace ImGuiRenderer { ... }
// Keduanya harus konsisten — NAMESPACE.
// ============================================================================

#include <android/native_window.h>
#include <cstdint>

namespace ImGuiRenderer {

    /**
     * Inisialisasi EGL + ImGui + IPC Client lalu mulai render thread.
     * Dipanggil dari nativeInitImGui() di jni_bridge.cpp.
     * @param window  ANativeWindow dari Surface Java
     * @param width   Lebar surface awal
     * @param height  Tinggi surface awal
     * @return true   jika berhasil memulai render thread
     */
    bool init(ANativeWindow* window, int width, int height);

    /**
     * Stop render thread dan cleanup semua resource (EGL, ImGui, IPC).
     * Dipanggil dari nativeDestroy() di jni_bridge.cpp.
     */
    void destroy();

    /**
     * Beritahu renderer bahwa surface telah resize (rotasi / split-screen).
     * Thread-safe: hanya set flag, resize actual terjadi di render thread.
     * Dipanggil dari nativeOnSurfaceChanged() di jni_bridge.cpp.
     */
    void onSurfaceChanged(int width, int height);

    /**
     * Tambahkan touch event ke ring buffer (thread-safe).
     * Dipanggil dari nativeInjectTouch() di jni_bridge.cpp.
     * @param x, y   Koordinat layar dari MotionEvent.getX/Y()
     * @param action MotionEvent.getAction() raw value
     */
    void queueTouch(float x, float y, int32_t action);

    /**
     * Apakah ImGui sedang meng-capture mouse (mau input touch)?
     * Java polling setiap 100ms → toggle FLAG_NOT_TOUCHABLE pada surface ImGui.
     */
    bool wantsCapture();

    /**
     * Apakah render thread sedang berjalan?
     */
    bool isRunning();

} // namespace ImGuiRenderer
