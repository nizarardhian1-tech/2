// =============================================================================
// egl_standalone.cpp — Standalone EGL Context Implementation
//
// Cara kerja EGL dalam konteks ini:
//   1. eglGetDisplay()      : Dapatkan display handle (biasanya EGL_DEFAULT_DISPLAY)
//   2. eglInitialize()      : Init EGL, dapatkan versi
//   3. eglChooseConfig()    : Pilih konfigurasi yang mendukung GLES3 + alpha channel
//   4. eglCreateContext()   : Buat context GLES3 milik kita
//   5. eglCreateWindowSurface() : Buat surface dari ANativeWindow
//   6. eglMakeCurrent()     : Aktifkan context di thread render
//
// NOTE PENTING tentang transparansi:
//   Agar overlay transparan (hanya ImGui widgets yang terlihat, background bening),
//   kita butuh:
//   - EGL config dengan EGL_ALPHA_SIZE = 8
//   - Di render loop: glClearColor(0, 0, 0, 0) + glClear(GL_COLOR_BUFFER_BIT)
//   - ANativeWindow sudah di-set transparent dari Java via SurfaceHolder.setFormat(TRANSLUCENT)
// =============================================================================

#include "egl_standalone.h"
#include <android/log.h>
#include <cstring>

#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  "EGL_STANDALONE", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "EGL_STANDALONE", fmt, ##__VA_ARGS__)

// =============================================================================
// init()
// =============================================================================
bool EGLStandalone::init(ANativeWindow* window) {
    if (!window) {
        LOGE("init() called with null ANativeWindow!");
        return false;
    }
    if (m_ready) {
        LOGE("Already initialized. Call destroy() first.");
        return false;
    }

    // ── 1. Get EGL display ────────────────────────────────────────────────
    m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    // ── 2. Initialize EGL ─────────────────────────────────────────────────
    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_display, &major, &minor)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    LOGI("EGL initialized: v%d.%d", major, minor);

    // ── 3. Choose config ──────────────────────────────────────────────────
    // Kita minta:
    //   - GLES3 renderable (EGL_OPENGL_ES3_BIT)
    //   - Window surface support
    //   - RGBA 8888 + alpha (untuk transparansi overlay)
    //   - Tidak perlu depth/stencil (ImGui tidak butuh)
    const EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,    // ← Krusial untuk transparansi
        EGL_DEPTH_SIZE,      0,    // Tidak butuh depth buffer
        EGL_STENCIL_SIZE,    0,
        EGL_NONE
    };

    EGLint numConfigs = 0;
    if (!eglChooseConfig(m_display, attribs, &m_config, 1, &numConfigs) || numConfigs < 1) {
        LOGE("eglChooseConfig failed (numConfigs=%d, error=0x%x)", numConfigs, eglGetError());
        // Fallback: coba tanpa depth constraint yang ketat
        const EGLint fallbackAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_ALPHA_SIZE,      8,
            EGL_NONE
        };
        if (!eglChooseConfig(m_display, fallbackAttribs, &m_config, 1, &numConfigs) || numConfigs < 1) {
            LOGE("eglChooseConfig fallback also failed");
            eglTerminate(m_display);
            m_display = EGL_NO_DISPLAY;
            return false;
        }
        LOGI("Using fallback EGL config");
    }

    // ── 4. Create GLES3 context ───────────────────────────────────────────
    const EGLint ctxAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, ctxAttribs);
    if (m_context == EGL_NO_CONTEXT) {
        // Fallback ke GLES2
        const EGLint ctxAttribs2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, ctxAttribs2);
        if (m_context == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext failed: 0x%x", eglGetError());
            eglTerminate(m_display);
            m_display = EGL_NO_DISPLAY;
            return false;
        }
        LOGI("Using GLES2 context (GLES3 not available)");
    } else {
        LOGI("GLES3 context created");
    }

    // ── 5. Create window surface ──────────────────────────────────────────
    // Tidak ada special attributes untuk window surface
    m_surface = eglCreateWindowSurface(m_display, m_config, window, nullptr);
    if (m_surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
        m_context = EGL_NO_CONTEXT;
        m_display = EGL_NO_DISPLAY;
        return false;
    }

    // ── 6. Make current di thread ini (render thread) ─────────────────────
    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        eglDestroySurface(m_display, m_surface);
        eglDestroyContext(m_display, m_context);
        eglTerminate(m_display);
        m_surface = EGL_NO_SURFACE;
        m_context = EGL_NO_CONTEXT;
        m_display = EGL_NO_DISPLAY;
        return false;
    }

    // ── Dapatkan dimensi surface ──────────────────────────────────────────
    eglQuerySurface(m_display, m_surface, EGL_WIDTH,  &m_width);
    eglQuerySurface(m_display, m_surface, EGL_HEIGHT, &m_height);

    LOGI("EGL init complete: surface %dx%d", m_width, m_height);
    m_ready = true;
    return true;
}

// =============================================================================
// resize()
// =============================================================================
bool EGLStandalone::resize(ANativeWindow* window, int w, int h) {
    if (!m_ready) return false;

    // Hapus surface lama
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_display, m_surface);
        m_surface = EGL_NO_SURFACE;
    }

    // Buat surface baru dari window yang (mungkin baru)
    m_surface = eglCreateWindowSurface(m_display, m_config, window, nullptr);
    if (m_surface == EGL_NO_SURFACE) {
        LOGE("resize: eglCreateWindowSurface failed: 0x%x", eglGetError());
        m_ready = false;
        return false;
    }

    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context)) {
        LOGE("resize: eglMakeCurrent failed: 0x%x", eglGetError());
        m_ready = false;
        return false;
    }

    m_width  = w;
    m_height = h;
    LOGI("EGL resized to %dx%d", w, h);
    return true;
}

// =============================================================================
// destroy()
// =============================================================================
void EGLStandalone::destroy() {
    if (m_display == EGL_NO_DISPLAY) return;

    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(m_display, m_surface);
        m_surface = EGL_NO_SURFACE;
    }
    if (m_context != EGL_NO_CONTEXT) {
        eglDestroyContext(m_display, m_context);
        m_context = EGL_NO_CONTEXT;
    }
    eglTerminate(m_display);
    m_display = EGL_NO_DISPLAY;
    m_ready   = false;
    LOGI("EGL destroyed");
}

// =============================================================================
// makeCurrent()
// =============================================================================
bool EGLStandalone::makeCurrent() {
    if (!m_ready) return false;
    return eglMakeCurrent(m_display, m_surface, m_surface, m_context) == EGL_TRUE;
}

// =============================================================================
// swapBuffers()
// =============================================================================
bool EGLStandalone::swapBuffers() {
    if (!m_ready) return false;
    EGLBoolean result = eglSwapBuffers(m_display, m_surface);
    if (result != EGL_TRUE) {
        EGLint err = eglGetError();
        // EGL_BAD_SURFACE: surface tidak valid lagi (misal setelah surfaceDestroyed)
        if (err == EGL_BAD_SURFACE || err == EGL_BAD_ALLOC) {
            LOGE("eglSwapBuffers failed (surface lost): 0x%x", err);
            m_ready = false;
        }
        return false;
    }
    return true;
}
