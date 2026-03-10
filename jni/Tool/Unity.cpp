// ============================================================================
// Tool/Unity.cpp — HYBRID ARCHITECTURE VERSION
//
// PERUBAHAN DARI PROYEK 2 ASLI:
//   ✓ Ditambahkan guard #ifndef EXT_PROCESS di sekeliling Unity::HookInput()
//     dan seluruh namespace NativeInput / LegacyInput.
//
// PENJELASAN GUARD:
//   - libimgui_ext.so dikompile dengan flag -DEXT_PROCESS
//   - libinternal.so dikompile TANPA flag tersebut
//
//   Ketika dikompile untuk libimgui_ext.so (external process):
//     → NativeInput, LegacyInput, Unity::HookInput() TIDAK ADA di binary
//     → Touch injection dilakukan via Java (OverlayMain.dispatchTouchEvent
//       → nativeInjectTouch JNI → ImGuiRenderer::queueTouch)
//     → Unity.cpp di ext hanya menyediakan stub kosong agar link tidak error
//
//   Ketika dikompile untuk libinternal.so (internal/game process):
//     → Semua kode berjalan normal (NativeInput, LegacyInput, dll)
//     → Tapi perlu diingat: di arsitektur baru, HookInput() untuk ImGui
//       TIDAK LAGI dipanggil dari on_init() karena ImGui ada di ext process.
//     → HookInput() bisa tetap dipanggil jika Anda ingin intercept Unity input
//       untuk keperluan CHEAT (bukan untuk ImGui).
//
// CARA BUILD:
//   libimgui_ext.so:
//     target_compile_definitions(imgui_ext PRIVATE EXT_PROCESS)
//   libinternal.so:
//     (tidak ada definisi EXT_PROCESS)
// ============================================================================

#include "Unity.h"
#include "Includes/Logger.h"

// ─────────────────────────────────────────────────────────────────────────────
// GUARD UTAMA: Blok ini HANYA ada di libinternal.so
// Di libimgui_ext.so, semua kode di bawah ini TIDAK dikompile sama sekali.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef EXT_PROCESS

#include "Il2cpp/Il2cpp.h"
#include "Includes/Macros.h"
#include "Includes/obfuscate.h"
#include "imgui/imgui.h"
#include <dlfcn.h>
#include <android/input.h>
#include <jni.h>
#include <mutex>
#include <atomic>
#include <cmath>

// Globals dari ImGui.h (hanya valid di internal process jika ImGui ada)
// Di arsitektur baru, ImGui tidak ada di internal.
// Guard tambahan agar tidak error link jika internal tidak punya ImGui.
#ifndef NO_IMGUI
extern bool isInitialized;
extern bool collapsed;
extern bool fullScreen;
#else
// Stub untuk internal process tanpa ImGui
static bool isInitialized = false;
static bool collapsed     = false;
static bool fullScreen    = false;
#endif

// ─────────────────────────────────────────────────────────────────────────────
// LAYER 1: Native AInputEvent Hook — persis dari Proyek 2
// ─────────────────────────────────────────────────────────────────────────────
namespace NativeInput {

    static std::mutex  g_mtx;
    static std::atomic<bool> g_active{false};

    using fn_getType    = int32_t (*)(const AInputEvent*);
    using fn_getSource  = int32_t (*)(const AInputEvent*);
    using fn_getAction  = int32_t (*)(const AInputEvent*);
    using fn_getX       = float   (*)(const AInputEvent*, size_t);
    using fn_getY       = float   (*)(const AInputEvent*, size_t);
    using fn_getCount   = size_t  (*)(const AInputEvent*);

    static fn_getType   p_getType   = nullptr;
    static fn_getSource p_getSource = nullptr;
    static fn_getAction p_getAction = nullptr;
    static fn_getX      p_getX      = nullptr;
    static fn_getY      p_getY      = nullptr;
    static fn_getCount  p_getCount  = nullptr;

    static bool Init() {
        void* h = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libandroid.so", RTLD_NOW);
        if (!h) {
            LOGE("[NativeInput] libandroid.so not available: %s", dlerror());
            return false;
        }
        p_getType   = (fn_getType)  dlsym(h, "AInputEvent_getType");
        p_getSource = (fn_getSource)dlsym(h, "AInputEvent_getSource");
        p_getAction = (fn_getAction)dlsym(h, "AMotionEvent_getActionMasked");
        if (!p_getAction)
            p_getAction = (fn_getAction)dlsym(h, "AMotionEvent_getAction");
        p_getX      = (fn_getX)     dlsym(h, "AMotionEvent_getX");
        p_getY      = (fn_getY)     dlsym(h, "AMotionEvent_getY");
        p_getCount  = (fn_getCount) dlsym(h, "AMotionEvent_getPointerCount");

        bool ok = p_getType && p_getAction && p_getX && p_getY;
        LOGI("[NativeInput] symbols loaded → %s", ok ? "OK" : "PARTIAL");
        return ok;
    }

    // ForwardEvent — dari AInputQueue hook langsung
    void ForwardEvent(const AInputEvent* event) {
#ifndef NO_IMGUI
        if (!g_active.load() || !isInitialized) return;
        if (!p_getType || !p_getAction || !p_getX || !p_getY) return;

        int32_t type = p_getType(event);
        if (type != AINPUT_EVENT_TYPE_MOTION) return;

        ImGuiIO& io = ImGui::GetIO();
        std::lock_guard<std::mutex> lk(g_mtx);

        int32_t rawAction = p_getAction(event);
        int32_t action    = rawAction & AMOTION_EVENT_ACTION_MASK;
        float   x         = p_getX(event, 0);
        float   y         = io.DisplaySize.y - p_getY(event, 0);

        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        switch (action) {
            case AMOTION_EVENT_ACTION_DOWN:
            case AMOTION_EVENT_ACTION_POINTER_DOWN:
                io.AddMousePosEvent(x, y);
                io.AddMouseButtonEvent(0, true);
                break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_POINTER_UP:
            case AMOTION_EVENT_ACTION_CANCEL:
                io.AddMousePosEvent(x, y);
                io.AddMouseButtonEvent(0, false);
                io.AddMousePosEvent(-1.f, -1.f);
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                io.AddMousePosEvent(x, y);
                break;
            default: break;
        }
#endif // NO_IMGUI
    }

    // ForwardTouch — dari JNI bridge
    void ForwardTouch(float x, float y, int32_t action) {
#ifndef NO_IMGUI
        if (!g_active.load() || !isInitialized) return;
        ImGuiIO& io = ImGui::GetIO();
        std::lock_guard<std::mutex> lk(g_mtx);
        float iy = io.DisplaySize.y - y;
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        action &= AMOTION_EVENT_ACTION_MASK;
        switch (action) {
            case AMOTION_EVENT_ACTION_DOWN:
                io.AddMousePosEvent(x, iy);
                io.AddMouseButtonEvent(0, true);
                break;
            case AMOTION_EVENT_ACTION_UP:
            case AMOTION_EVENT_ACTION_CANCEL:
                io.AddMousePosEvent(x, iy);
                io.AddMouseButtonEvent(0, false);
                io.AddMousePosEvent(-1.f, -1.f);
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                io.AddMousePosEvent(x, iy);
                break;
            default: break;
        }
#endif // NO_IMGUI
    }

    bool IsActive() { return g_active.load(); }
    void SetActive(bool v) { g_active.store(v); }

} // namespace NativeInput

// JNI bridge — persis dari Proyek 2
extern "C" {
JNIEXPORT void JNICALL
Java_com_il2cpptool_input_NativeBridge_nativeOnTouch(
    JNIEnv* /*env*/, jclass /*clazz*/,
    jfloat x, jfloat y, jint action)
{
    NativeInput::ForwardTouch((float)x, (float)y, (int32_t)action);
}
} // extern "C"

// ─────────────────────────────────────────────────────────────────────────────
// LAYER 2: UnityEngine.Input IL2CPP Hook — persis dari Proyek 2
// ─────────────────────────────────────────────────────────────────────────────
namespace LegacyInput {

    int  (*o_get_touchCount)()       = nullptr;
    bool (*o_GetMouseButton)(int n)  = nullptr;
    static Il2CppClass* Input        = nullptr;
    static bool         active       = false;

    bool Input_GetMouseButton(int n) {
#ifndef NO_IMGUI
        if (!isInitialized) return o_GetMouseButton(n);
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
        if (io.WantCaptureMouse &&
            !(collapsed && fullScreen &&
              (io.MousePos.x > size.x && io.MousePos.y > size.y)))
            return false;
#endif
        return o_GetMouseButton(n);
    }

    int get_touchCount() {
        int count = o_get_touchCount();

#ifndef NO_IMGUI
        if (count > 0 && isInitialized) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            try {
                auto touch = Input->invoke_static_method<UnityEngine_Touch>("GetTouch", 0);
                float x  = touch.m_Position.x;
                float iy = io.DisplaySize.y - touch.m_Position.y;
                switch (touch.m_Phase) {
                    case UnityEngine_TouchPhase::Began:
                        io.AddMousePosEvent(x, iy);
                        io.AddMouseButtonEvent(0, true);
                        break;
                    case UnityEngine_TouchPhase::Ended:
                    case UnityEngine_TouchPhase::Canceled:
                        io.AddMousePosEvent(x, iy);
                        io.AddMouseButtonEvent(0, false);
                        io.AddMousePosEvent(-1.f, -1.f);
                        break;
                    case UnityEngine_TouchPhase::Moved:
                    case UnityEngine_TouchPhase::Stationary:
                        io.AddMousePosEvent(x, iy);
                        break;
                    default: break;
                }
            } catch (...) {}
        }

        if (!isInitialized) return count;
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 size{ImGui::GetFrameHeight() * 2.f, ImGui::GetFrameHeight() * 2.f};
        if (io.WantCaptureMouse &&
            !(collapsed && fullScreen &&
              (io.MousePos.x > size.x && io.MousePos.y > size.y)))
            return 0;
#endif // NO_IMGUI
        return count;
    }

    bool TryInstall() {
        Il2CppClass* foundKlass = nullptr;
        for (auto* img : Il2cpp::GetImages()) {
            if (!img) continue;
            auto* klass = img->getClass("UnityEngine.Input");
            if (!klass) continue;
            auto* mTouch = klass->getMethod("get_touchCount", 0);
            auto* mMouse = klass->getMethod("GetMouseButton",  1);
            if (!mTouch || !mMouse) continue;
            foundKlass = klass;
            LOGI("[LegacyInput] UnityEngine.Input found in: %s", img->getName());
            break;
        }
        if (!foundKlass) return false;

        REPLACE_NAME_KLASS_ORIG(foundKlass, "get_touchCount",
                                get_touchCount, o_get_touchCount);
        REPLACE_NAME_KLASS_ORIG(foundKlass, "GetMouseButton",
                                Input_GetMouseButton, o_GetMouseButton);
        Input  = foundKlass;
        active = true;
        LOGI("[LegacyInput] IL2CPP input hooks installed.");
        return true;
    }

    bool IsActive() { return active; }

} // namespace LegacyInput

// ─────────────────────────────────────────────────────────────────────────────
// Unity::HookInput — Orchestrator 3-layer
//
// CATATAN ARSITEKTUR BARU:
//   Di arsitektur Hybrid, HookInput() DI libinternal.so TIDAK perlu dipanggil
//   untuk kebutuhan ImGui (karena ImGui ada di external process dan menerima
//   touch dari Java, bukan dari Unity input hook).
//
//   Anda BOLEH tetap memanggil HookInput() dari internal_main.cpp jika ingin
//   mencegah game menerima touch saat ImGui aktif (untuk anti-through-click).
//   Tapi ini opsional dan butuh koordinasi via SHM/IPC.
// ─────────────────────────────────────────────────────────────────────────────
namespace Unity {

    void HookInput() {
        // Layer 1: Load libandroid symbols
        if (NativeInput::Init()) {
            NativeInput::SetActive(true);
            LOGI("[Unity::HookInput] Layer 1: NativeInput ready.");
        } else {
            LOGW("[Unity::HookInput] Layer 1: libandroid symbols missing.");
        }

        // Layer 2: IL2CPP hook
        if (LegacyInput::TryInstall()) {
            LOGI("[Unity::HookInput] Layer 2: IL2CPP input hooks active.");
            return;
        }

        LOGE("[Unity::HookInput] All layers failed.");
    }

    bool IsNativeInputActive() { return NativeInput::IsActive(); }
    bool IsLegacyInputActive() { return LegacyInput::IsActive(); }

} // namespace Unity

#else // EXT_PROCESS — Stub untuk libimgui_ext.so

// ─────────────────────────────────────────────────────────────────────────────
// STUB: Kode ini dikompile ke libimgui_ext.so sebagai pengganti implementasi
// penuh di atas. Stub diperlukan agar file yang menginclude Unity.h bisa
// compile tanpa error. Implementasinya kosong karena ext process tidak
// melakukan Unity IL2CPP hooking.
//
// Touch di ext process datang dari:
//   Java OverlayMain.dispatchTouchEvent()
//   → nativeInjectTouch() JNI
//   → ImGuiRenderer::queueTouch()
//   → processTouchQueue() di render loop
// ─────────────────────────────────────────────────────────────────────────────

namespace NativeInput {
    void ForwardEvent(const AInputEvent* /*event*/) {}
    void ForwardTouch(float /*x*/, float /*y*/, int32_t /*action*/) {}
    bool IsActive() { return false; }
    void SetActive(bool /*v*/) {}
} // namespace NativeInput

namespace Unity {
    // HookInput() TIDAK DIPANGGIL di ext process.
    // Dipertahankan sebagai stub agar file yang forward-declare tidak error.
    void HookInput() {
        LOGI("[Unity::HookInput] EXT_PROCESS: skipped (touch via Java JNI).");
    }
    bool IsNativeInputActive() { return false; }
    bool IsLegacyInputActive() { return false; }
} // namespace Unity

#endif // EXT_PROCESS
