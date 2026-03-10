// ============================================================================
// imgui_ext/imgui_render.cpp — THE BRIDGE (Render Loop Final)
//
// FIX dari versi sebelumnya:
//   - Tambah static EGLStandalone s_egl sebagai instance
//   - Semua EGLStandalone::xxx() diganti s_egl.xxx() karena EGLStandalone
//     adalah class (member functions, bukan namespace)
//   - imgui_render.h dikembalikan ke namespace (matching jni_bridge.cpp)
// ============================================================================

#include "imgui_render.h"
#include "egl_standalone.h"

// ImGui core
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_opengl3.h"

// UI dari Proyek 2
#include "Menu/ImGui.h"
#include "Menu/MainWindow.h"
#include "Tool/Tab_Inspector.h"
#include "Includes/Logger.h"

// IPC Client
#include "ipc/ipc_client.h"

#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Touch Queue (ring buffer, max 64 event)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

    constexpr int TOUCH_QUEUE_SIZE = 64;

    struct TouchEvent {
        float   x;
        float   y;
        int32_t action;
    };

    static std::mutex  s_touchMtx;
    static TouchEvent  s_touchQueue[TOUCH_QUEUE_SIZE];
    static int         s_touchHead = 0;
    static int         s_touchTail = 0;

    static inline bool touchQueueEmpty() { return s_touchHead == s_touchTail; }
    static inline bool touchQueueFull()  {
        return (s_touchTail + 1) % TOUCH_QUEUE_SIZE == s_touchHead;
    }

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// ImGuiRenderer namespace — semua implementasi di sini
// ─────────────────────────────────────────────────────────────────────────────
namespace ImGuiRenderer {

    // ── EGL instance ──────────────────────────────────────────────────────────
    // EGLStandalone adalah CLASS — harus pakai instance, bukan namespace static
    static EGLStandalone s_egl;

    // ── State ─────────────────────────────────────────────────────────────────
    static std::atomic<bool> s_running{false};
    static std::atomic<bool> s_wantsCapture{false};
    static std::thread       s_renderThread;

    static ANativeWindow*    s_window     = nullptr;
    static int               s_surfWidth  = 0;
    static int               s_surfHeight = 0;
    static std::mutex        s_surfMtx;

    static std::atomic<bool> s_pendingResize{false};
    static int               s_newWidth  = 0;
    static int               s_newHeight = 0;

    // ─────────────────────────────────────────────────────────────────────────
    // processTouchQueue()
    // ─────────────────────────────────────────────────────────────────────────
    static void processTouchQueue()
    {
        std::lock_guard<std::mutex> lk(s_touchMtx);
        if (touchQueueEmpty()) return;

        ImGuiIO& io = ImGui::GetIO();

        while (!touchQueueEmpty()) {
            TouchEvent& ev = s_touchQueue[s_touchHead];
            s_touchHead = (s_touchHead + 1) % TOUCH_QUEUE_SIZE;

            float x = ev.x;
            float y = ev.y;

            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);

            int32_t action = ev.action & 0xFF;
            switch (action) {
                case 0: // ACTION_DOWN
                case 5: // ACTION_POINTER_DOWN
                    io.AddMousePosEvent(x, y);
                    io.AddMouseButtonEvent(0, true);
                    break;
                case 1: // ACTION_UP
                case 6: // ACTION_POINTER_UP
                case 3: // ACTION_CANCEL
                    io.AddMousePosEvent(x, y);
                    io.AddMouseButtonEvent(0, false);
                    io.AddMousePosEvent(-1.f, -1.f);
                    break;
                case 2: // ACTION_MOVE
                    io.AddMousePosEvent(x, y);
                    break;
                default:
                    break;
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // doResize() — s_egl.resize() butuh window + w + h
    // ─────────────────────────────────────────────────────────────────────────
    static void doResize(int w, int h)
    {
        std::lock_guard<std::mutex> lk(s_surfMtx);
        if (w == s_surfWidth && h == s_surfHeight) return;

        // EGLStandalone::resize adalah class member — panggil via instance
        // signature: bool resize(ANativeWindow* window, int w, int h)
        s_egl.resize(s_window, w, h);

        s_surfWidth  = w;
        s_surfHeight = h;
        glWidth      = w;
        glHeight     = h;

        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().DisplaySize = ImVec2((float)w, (float)h);
        }

        LOGI("[Renderer] Surface resized: %dx%d", w, h);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // renderLoop()
    // ─────────────────────────────────────────────────────────────────────────
    static void renderLoop()
    {
        LOGI("[Renderer] renderLoop() started");

        // 1. Init EGL — pakai s_egl.init() bukan EGLStandalone::init()
        {
            std::lock_guard<std::mutex> lk(s_surfMtx);
            if (!s_window) {
                LOGE("[Renderer] No ANativeWindow — cannot init EGL");
                s_running = false;
                return;
            }
            if (!s_egl.init(s_window)) {
                LOGE("[Renderer] s_egl.init() failed");
                s_running = false;
                return;
            }
            // getWidth/getHeight adalah const member functions
            s_surfWidth  = s_egl.getWidth();
            s_surfHeight = s_egl.getHeight();
            glWidth      = s_surfWidth;
            glHeight     = s_surfHeight;
        }

        LOGI("[Renderer] EGL ready: %dx%d", glWidth, glHeight);

        // 2. Init ImGui context + font + style
        setupMenuContext();

        if (!ImGui::GetCurrentContext()) {
            LOGE("[Renderer] ImGui context null after setupMenuContext!");
            s_running = false;
            return;
        }

        // 3. Connect IPC ke libinternal.so (non-blocking, retry 10x)
        bool ipcConnected = false;
        for (int attempt = 0; attempt < 10 && s_running; attempt++) {
            if (IPCClient::get().connect()) {
                ipcConnected = true;
                LOGI("[Renderer] IPC connected to @hybrid_imgui_ipc");
                break;
            }
            LOGW("[Renderer] IPC connect failed (attempt %d/10), retry 1s...", attempt + 1);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!ipcConnected) {
            LOGW("[Renderer] IPC not available — UI renders but no IL2CPP data");
        }

        // 4. Render loop utama (60 FPS)
        using Clock = std::chrono::steady_clock;
        const auto frameDuration = std::chrono::microseconds(1000000 / 60);

        while (s_running) {
            auto frameStart = Clock::now();

            // Handle resize dari Java onSurfaceChanged
            if (s_pendingResize.exchange(false)) {
                doResize(s_newWidth, s_newHeight);
            }

            // Inject touch events ke ImGuiIO
            processTouchQueue();

            // Mulai frame
            ImGui_ImplOpenGL3_NewFrame();
            {
                ImGuiIO& io    = ImGui::GetIO();
                io.DisplaySize = ImVec2((float)glWidth, (float)glHeight);
                io.DeltaTime   = 1.0f / 60.0f;
            }
            ImGui::NewFrame();

            // Draw UI Proyek 2
            MainWindow::Draw();
            InspectorTab::DrawWindows();

            // Render ke framebuffer
            ImGui::Render();
            glViewport(0, 0, glWidth, glHeight);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // transparan
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            // Present — s_egl.swapBuffers() bukan EGLStandalone::swapBuffers()
            s_egl.swapBuffers();

            s_wantsCapture.store(ImGui::GetIO().WantCaptureMouse);

            // 60 FPS limiter
            auto elapsed = Clock::now() - frameStart;
            if (elapsed < frameDuration)
                std::this_thread::sleep_for(frameDuration - elapsed);
        }

        // Cleanup
        LOGI("[Renderer] renderLoop() exiting — cleanup");
        IPCClient::get().disconnect();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        s_egl.destroy();  // class destructor juga akan cleanup, tapi explicit lebih aman
        LOGI("[Renderer] renderLoop() done");
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Public API (dipanggil dari jni_bridge.cpp)
    // ─────────────────────────────────────────────────────────────────────────

    bool init(ANativeWindow* window, int width, int height)
    {
        if (s_running.load()) {
            LOGW("[Renderer] init() called while already running — ignored");
            return false;
        }
        if (!window) {
            LOGE("[Renderer] init() called with null window!");
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(s_surfMtx);
            s_window     = window;
            s_surfWidth  = width;
            s_surfHeight = height;
        }
        s_running      = true;
        s_renderThread = std::thread(renderLoop);
        LOGI("[Renderer] render thread started (%dx%d)", width, height);
        return true;
    }

    void destroy()
    {
        if (!s_running.load()) return;
        LOGI("[Renderer] destroy() called");
        s_running = false;
        if (s_renderThread.joinable())
            s_renderThread.join();
        {
            std::lock_guard<std::mutex> lk(s_surfMtx);
            s_window = nullptr;
        }
    }

    void onSurfaceChanged(int width, int height)
    {
        s_newWidth  = width;
        s_newHeight = height;
        s_pendingResize.store(true);
    }

    void queueTouch(float x, float y, int32_t action)
    {
        std::lock_guard<std::mutex> lk(s_touchMtx);
        if (touchQueueFull()) {
            s_touchHead = (s_touchHead + 1) % TOUCH_QUEUE_SIZE;
        }
        s_touchQueue[s_touchTail] = {x, y, action};
        s_touchTail = (s_touchTail + 1) % TOUCH_QUEUE_SIZE;
    }

    bool wantsCapture() { return s_wantsCapture.load(); }
    bool isRunning()    { return s_running.load(); }

} // namespace ImGuiRenderer
