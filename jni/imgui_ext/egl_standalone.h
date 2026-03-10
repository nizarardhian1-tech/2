#pragma once
// =============================================================================
// egl_standalone.h — Standalone EGL Context untuk ImGui
//
// Context EGL ini MILIK KITA SENDIRI, bukan milik game.
// Di-render ke ANativeWindow yang berasal dari Java SurfaceView.
//
// PENTING: eglMakeCurrent() harus dipanggil dari thread yang sama dengan
//          thread yang akan melakukan draw calls (render thread ImGui).
//          Jangan panggil init() dari main thread jika render di thread lain.
// =============================================================================

#ifndef EGL_STANDALONE_H
#define EGL_STANDALONE_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <atomic>

class EGLStandalone {
public:
    EGLStandalone() = default;
    ~EGLStandalone() { destroy(); }

    // ── Lifecycle ─────────────────────────────────────────────────────────
    /**
     * Inisialisasi EGL context menggunakan ANativeWindow.
     * Harus dipanggil dari render thread.
     *
     * @param window  ANativeWindow* dari Java Surface (via ANativeWindow_fromSurface)
     * @return true jika berhasil
     */
    bool init(ANativeWindow* window);

    /**
     * Recreate window surface setelah surfaceChanged (resize).
     * Context tetap, hanya surface yang diganti.
     *
     * @param window  ANativeWindow baru (atau sama, jika hanya resize)
     * @param w       Lebar baru
     * @param h       Tinggi baru
     * @return true jika berhasil
     */
    bool resize(ANativeWindow* window, int w, int h);

    /**
     * Hapus semua EGL resource. Aman dipanggil berkali-kali.
     */
    void destroy();

    // ── Per-frame operations ───────────────────────────────────────────────
    /**
     * Make context current di thread pemanggil.
     * Harus dipanggil sebelum GL calls pertama di thread tersebut.
     */
    bool makeCurrent();

    /**
     * Present frame ke layar (swap buffers).
     * @return EGL_TRUE jika berhasil
     */
    bool swapBuffers();

    // ── Status ────────────────────────────────────────────────────────────
    bool isReady() const { return m_ready; }
    int  getWidth()  const { return m_width; }
    int  getHeight() const { return m_height; }

    EGLDisplay getDisplay() const { return m_display; }
    EGLContext getContext() const { return m_context; }
    EGLSurface getSurface() const { return m_surface; }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLConfig  m_config  = nullptr;

    int  m_width  = 0;
    int  m_height = 0;
    bool m_ready  = false;
};

#endif // EGL_STANDALONE_H
