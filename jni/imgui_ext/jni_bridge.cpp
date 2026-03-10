// =============================================================================
// jni_bridge.cpp — JNI Bridge: Java ↔ C++ (libimgui_ext.so)
// =============================================================================

#include <jni.h>
#include <android/native_window_jni.h>  // ANativeWindow_fromSurface
#include <android/log.h>

#include "imgui_render.h"

#define LOGI(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  "JNI_BRIDGE", fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "JNI_BRIDGE", fmt, ##__VA_ARGS__)

static JavaVM* g_jvm = nullptr;
static ANativeWindow* g_currentWindow = nullptr;

extern "C"
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    LOGI("libimgui_ext.so loaded (JNI_OnLoad)");
    return JNI_VERSION_1_6;
}

extern "C"
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
    LOGI("libimgui_ext.so unloaded (JNI_OnUnload)");
    g_jvm = nullptr;
}

extern "C"
JNIEXPORT void JNICALL
Java_uk_lgl_OverlayMain_nativeInitImGui(JNIEnv* env, jclass clazz,
                                         jobject surface, jint w, jint h) {
    LOGI("nativeInitImGui() w=%d h=%d", w, h);

    if (g_currentWindow) {
        ANativeWindow_release(g_currentWindow);
        g_currentWindow = nullptr;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        LOGE("ANativeWindow_fromSurface() returned null! Surface mungkin belum siap.");
        return;
    }

    g_currentWindow = window;
    ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);

    // PERBAIKAN: Langsung panggil fungsi di namespace, tanpa .get()
    ImGuiRenderer::init(window, static_cast<int>(w), static_cast<int>(h));

    LOGI("nativeInitImGui() complete, render thread started");
}

extern "C"
JNIEXPORT void JNICALL
Java_uk_lgl_OverlayMain_nativeInjectTouch(JNIEnv* env, jclass clazz,
                                           jfloat x, jfloat y, jint action) {
    // PERBAIKAN: Langsung panggil queueTouch
    ImGuiRenderer::queueTouch(
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<int>(action)
    );
}

extern "C"
JNIEXPORT void JNICALL
Java_uk_lgl_OverlayMain_nativeOnSurfaceChanged(JNIEnv* env, jclass clazz,
                                                jobject surface, jint w, jint h) {
    LOGI("nativeOnSurfaceChanged() w=%d h=%d", w, h);

    ANativeWindow* newWindow = ANativeWindow_fromSurface(env, surface);
    if (!newWindow) {
        LOGE("nativeOnSurfaceChanged: ANativeWindow_fromSurface() returned null");
        return;
    }

    if (g_currentWindow && g_currentWindow != newWindow) {
        ANativeWindow_release(g_currentWindow);
    }
    g_currentWindow = newWindow;

    // PERBAIKAN: Langsung panggil onSurfaceChanged
    ImGuiRenderer::onSurfaceChanged(static_cast<int>(w), static_cast<int>(h));
}

extern "C"
JNIEXPORT void JNICALL
Java_uk_lgl_OverlayMain_nativeDestroy(JNIEnv* env, jclass clazz) {
    LOGI("nativeDestroy() called");

    // PERBAIKAN: Langsung panggil destroy (sesuai nama di header)
    ImGuiRenderer::destroy();

    if (g_currentWindow) {
        ANativeWindow_release(g_currentWindow);
        g_currentWindow = nullptr;
    }

    LOGI("nativeDestroy() complete");
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_uk_lgl_OverlayMain_nativeWantsCapture(JNIEnv* env, jclass clazz) {
    // PERBAIKAN: Langsung panggil wantsCapture
    return static_cast<jboolean>(ImGuiRenderer::wantsCapture() ? JNI_TRUE : JNI_FALSE);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_uk_lgl_OverlayMain_nativeIsRunning(JNIEnv* env, jclass clazz) {
    // PERBAIKAN: Langsung panggil isRunning
    return static_cast<jboolean>(ImGuiRenderer::isRunning() ? JNI_TRUE : JNI_FALSE);
}