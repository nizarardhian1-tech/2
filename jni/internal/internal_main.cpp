// ============================================================================
// internal/internal_main.cpp — FINAL (Hybrid Architecture)
//
// File ini adalah merger dari:
//   - Proyek 1: Main.cpp internal (IL2CPP init, SHM tulis)
//   - Proyek 2: Main.cpp asli (on_init, Il2cpp::Init, Tool::Init, config JSON)
//
// YANG DIHAPUS:
//   ✗ initModMenu() — tidak ada ImGui di sini
//   ✗ hack_thread() yang memanggil initModMenu(swapbuffers/vkQueuePresent)
//   ✗ Java_imgui_il2cpp_tool_NativeMethods_onDrawFrame / onSurfaceChanged
//   ✗ Java_imgui_il2cpp_tool_NativeMethods_onSurfaceCreate
//   ✗ Semua hal terkait EGL, GLES, render loop
//
// YANG DIPERTAHANKAN (dari Proyek 2 Main.cpp):
//   ✓ g_jvm, g_jvm utilities (Java_ShowToast, Java_CopyToClipboard, Java_HttpGet,
//     Java_SendNotification) — semua tetap utuh
//   ✓ ConfigInit() / ConfigSet() / ConfigGet() dengan nlohmann/json
//   ✓ on_init() — Init IL2CPP, cari Assembly-CSharp, Tool::Init, Keyboard::Init
//   ✓ JNI_OnLoad() untuk menyimpan JavaVM
//
// YANG DITAMBAHKAN:
//   ✓ RpcHandler::registerAll() + IPCServer::get().start()
//   ✓ SHM writer (opsional) — tulis data ESP ke SharedData untuk OverlayESP Java
//
// FLOW EKSEKUSI:
//   1. lib_entry() [constructor] → spawn hack_thread
//   2. hack_thread() → waitForLib("libil2cpp.so")
//   3. on_init():
//      a. Il2cpp::Init() + EnsureAttached()
//      b. detectTargetLib() (MLBB → libcsharp.so)
//      c. Keyboard::Init()
//      d. ConfigInit()
//      e. Tool::Init()
//      f. NetworkTab::Init()
//      g. installGameHooks() (Dobby, NOP patch)
//      h. RpcHandler::registerAll()
//      i. IPCServer::get().start()
//      j. Java_ShowToast("Tool Loaded!")
//   4. IPC Server berjalan di background — siap terima command dari libimgui_ext.so
// ============================================================================

#include <jni.h>
#include <array>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sstream>
#include <string>

// ── IL2CPP (path dari Proyek 2) ───────────────────────────────────────────────
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Includes/Logger.h"
#include "Includes/Utils.h"
#include "Includes/obfuscate.h"
#include "Tool/Keyboard.h"
#include "Tool/Tool.h"
#include "Tool/Util.h"
#include "Tool/Unity.h"
#include "Tool/Tab_Network.h"
#include "Tool/ProjectSessionManager.h"
#include "Il2cpp/xdl/include/xdl.h"

// ── JSON Config ───────────────────────────────────────────────────────────────
#include "json/single_include/nlohmann/json.hpp"

// ── IPC Server ────────────────────────────────────────────────────────────────
#include "ipc/ipc_server.h"
#include "ipc/rpc_handler.h"

// ── SHM — sama dengan arsitektur MLBB-Mod ─────────────────────────────────────
// libinternal.so (game process) tulis SHM → OverlayMain (app_process) baca SHM
// Path SHM diderivasi otomatis dari /proc/self/cmdline (pkg name game)
#include "shm.h"

#define targetLibName OBFUSCATE("libil2cpp.so")

// ── Global state ───────────────────────────────────────────────────────────────
Il2CppImage* g_Image   = nullptr;
std::vector<MethodInfo*> g_Methods;

// Diakses dari Tab_Inspector, Tab_Tracer, dll (persis seperti Proyek 2)
std::unordered_map<void*, HookerData> hookerMap;
int maxLine = 0;
ImVec2 initialScreenSize{0.f, 0.f}; // Tetap ada sebagai extern; di ext process yang diisi

// ── JavaVM ────────────────────────────────────────────────────────────────────
static JavaVM* g_jvm = nullptr;

// ── Config (persis dari Proyek 2) ─────────────────────────────────────────────
static nlohmann::ordered_json gConf;
static int s_selectedScale = 3;

template <typename T>
void ConfigSet(const char* key, T value) {
    gConf[key] = value;
    LOGD("ConfigWrite %s = %s", key, gConf[key].dump().c_str());
    Util::FileWriter fw("tool_conf.json");
    fw.write(gConf.dump(2).c_str());
}

template <typename T>
T ConfigGet(const char* key, T defaultValue) {
    if (gConf.contains(key)) return gConf[key].get<T>();
    ConfigSet(key, defaultValue);
    return defaultValue;
}

// Compat untuk MainWindow.cpp yang memanggilnya via extern
void ConfigSet_int(const char* key, int value)  { ConfigSet(key, value); }
int  ConfigGet_int(const char* key, int def)     { return ConfigGet(key, def); }

static void ConfigInit() {
    Util::FileReader fr("tool_conf.json");
    if (fr.exists()) {
        auto data = fr.read();
        try {
            gConf = nlohmann::json::parse(data);
        } catch (nlohmann::json::exception& e) {
            LOGE("ConfigInit parse error: %s", e.what());
            Util::FileWriter("tool_conf.json").write("{}");
        }
    } else {
        Util::FileWriter("tool_conf.json").write("{}");
    }
}

// ── Java Utilities (persis dari Proyek 2) ────────────────────────────────────
void Java_ShowToast(const char* msg) {
    if (!g_jvm) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    jclass atClass  = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication",
                                              "()Landroid/app/Application;");
    jobject ctx = env->CallStaticObjectMethod(atClass, curApp);
    if (env->ExceptionCheck()) { env->ExceptionClear(); goto done; }
    if (!ctx) goto done;
    {
        jclass toastClass  = env->FindClass("android/widget/Toast");
        jmethodID makeText = env->GetStaticMethodID(toastClass, "makeText",
            "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
        jmethodID show     = env->GetMethodID(toastClass, "show", "()V");
        jstring jmsg       = env->NewStringUTF(msg);
        jobject toast      = env->CallStaticObjectMethod(toastClass, makeText, ctx, jmsg, (jint)0);
        if (env->ExceptionCheck()) { env->ExceptionClear(); goto done; }
        env->CallVoidMethod(toast, show);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    done:
    if (attached) g_jvm->DetachCurrentThread();
}

void Java_CopyToClipboard(const char* label, const char* text) {
    if (!g_jvm) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    jclass atClass   = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication",
                                              "()Landroid/app/Application;");
    jobject ctx = env->CallStaticObjectMethod(atClass, curApp);
    if (env->ExceptionCheck()) { env->ExceptionClear(); goto done2; }
    if (!ctx) goto done2;
    {
        jclass ctxClass   = env->FindClass("android/content/Context");
        jstring clipSvc   = env->NewStringUTF("clipboard");
        jmethodID getSvc  = env->GetMethodID(ctxClass, "getSystemService",
                                             "(Ljava/lang/String;)Ljava/lang/Object;");
        jobject cm        = env->CallObjectMethod(ctx, getSvc, clipSvc);
        if (env->ExceptionCheck()) { env->ExceptionClear(); goto done2; }
        jclass cmClass    = env->FindClass("android/content/ClipboardManager");
        jclass cdClass    = env->FindClass("android/content/ClipData");
        jmethodID newPlain = env->GetStaticMethodID(cdClass, "newPlainText",
            "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");
        jmethodID setPrimary = env->GetMethodID(cmClass, "setPrimaryClip",
                                                "(Landroid/content/ClipData;)V");
        jstring jlabel = env->NewStringUTF(label);
        jstring jtext  = env->NewStringUTF(text);
        jobject clip   = env->CallStaticObjectMethod(cdClass, newPlain, jlabel, jtext);
        if (env->ExceptionCheck()) { env->ExceptionClear(); goto done2; }
        env->CallVoidMethod(cm, setPrimary, clip);
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGI("[Java] Copied to clipboard: %s", label);
    }
    done2:
    if (attached) g_jvm->DetachCurrentThread();
}

std::string Java_HttpGet(const char* url) {
    std::string result;
    if (!g_jvm) return result;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    auto cleanup = [&]() {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
    };
    jclass urlClass   = env->FindClass("java/net/URL");
    jmethodID urlInit = env->GetMethodID(urlClass, "<init>", "(Ljava/lang/String;)V");
    jstring jurl      = env->NewStringUTF(url);
    jobject urlObj    = env->NewObject(urlClass, urlInit, jurl);
    if (env->ExceptionCheck()) { cleanup(); return result; }
    jmethodID openConn = env->GetMethodID(urlClass, "openConnection",
                                          "()Ljava/net/URLConnection;");
    jobject conn = env->CallObjectMethod(urlObj, openConn);
    if (env->ExceptionCheck()) { cleanup(); return result; }
    jclass httpClass = env->FindClass("java/net/HttpURLConnection");
    jmethodID setMethod = env->GetMethodID(httpClass, "setRequestMethod",
                                           "(Ljava/lang/String;)V");
    env->CallVoidMethod(conn, setMethod, env->NewStringUTF("GET"));
    jmethodID setTimeout = env->GetMethodID(httpClass, "setConnectTimeout", "(I)V");
    env->CallVoidMethod(conn, setTimeout, (jint)5000);
    jmethodID setReadTimeout = env->GetMethodID(httpClass, "setReadTimeout", "(I)V");
    env->CallVoidMethod(conn, setReadTimeout, (jint)5000);
    jmethodID getRespCode = env->GetMethodID(httpClass, "getResponseCode", "()I");
    jint code = env->CallIntMethod(conn, getRespCode);
    if (env->ExceptionCheck() || code != 200) { cleanup(); return result; }
    jmethodID getInput = env->GetMethodID(httpClass, "getInputStream",
                                          "()Ljava/io/InputStream;");
    jobject is = env->CallObjectMethod(conn, getInput);
    if (env->ExceptionCheck()) { cleanup(); return result; }
    jclass isrClass   = env->FindClass("java/io/InputStreamReader");
    jmethodID isrInit = env->GetMethodID(isrClass, "<init>", "(Ljava/io/InputStream;)V");
    jobject isr = env->NewObject(isrClass, isrInit, is);
    jclass brClass    = env->FindClass("java/io/BufferedReader");
    jmethodID brInit  = env->GetMethodID(brClass, "<init>", "(Ljava/io/Reader;)V");
    jobject br = env->NewObject(brClass, brInit, isr);
    jmethodID readLine = env->GetMethodID(brClass, "readLine", "()Ljava/lang/String;");
    while (true) {
        jstring line = (jstring)env->CallObjectMethod(br, readLine);
        if (env->ExceptionCheck() || !line) break;
        const char* lineC = env->GetStringUTFChars(line, nullptr);
        result += lineC;
        result += "\n";
        env->ReleaseStringUTFChars(line, lineC);
        env->DeleteLocalRef(line);
    }
    jmethodID disconnect = env->GetMethodID(httpClass, "disconnect", "()V");
    env->CallVoidMethod(conn, disconnect);
    cleanup();
    return result;
}

static bool s_notifChannelCreated = false;
void Java_SendNotification(const char* title, const char* msg) {
    if (!g_jvm) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_jvm->AttachCurrentThread(&env, nullptr);
        attached = true;
    }
    auto cleanup = [&]() {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
    };
    jclass atClass   = env->FindClass("android/app/ActivityThread");
    jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication",
                                              "()Landroid/app/Application;");
    jobject ctx = env->CallStaticObjectMethod(atClass, curApp);
    if (env->ExceptionCheck() || !ctx) { cleanup(); return; }
    jclass ctxClass  = env->FindClass("android/content/Context");
    jstring notifSvc = env->NewStringUTF("notification");
    jmethodID getSvc = env->GetMethodID(ctxClass, "getSystemService",
                                        "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject nm = env->CallObjectMethod(ctx, getSvc, notifSvc);
    if (env->ExceptionCheck() || !nm) { cleanup(); return; }
    jclass nmClass = env->FindClass("android/app/NotificationManager");
    jclass buildVersion = env->FindClass("android/os/Build$VERSION");
    jfieldID sdkField   = env->GetStaticFieldID(buildVersion, "SDK_INT", "I");
    jint sdk = env->GetStaticIntField(buildVersion, sdkField);
    jstring channelId = env->NewStringUTF("tool_channel");
    if (sdk >= 26 && !s_notifChannelCreated) {
        jclass chanClass    = env->FindClass("android/app/NotificationChannel");
        jmethodID chanInit  = env->GetMethodID(chanClass, "<init>",
                                               "(Ljava/lang/String;Ljava/lang/CharSequence;I)V");
        jobject chan = env->NewObject(chanClass, chanInit, channelId,
                                     env->NewStringUTF("Tool"), (jint)2);
        if (!env->ExceptionCheck()) {
            jmethodID createChan = env->GetMethodID(nmClass, "createNotificationChannel",
                                                    "(Landroid/app/NotificationChannel;)V");
            env->CallVoidMethod(nm, createChan, chan);
            if (!env->ExceptionCheck()) s_notifChannelCreated = true;
            else env->ExceptionClear();
        } else { env->ExceptionClear(); }
    }
    jclass builderClass;
    jobject builder;
    builderClass = env->FindClass("android/app/Notification$Builder");
    if (sdk >= 26) {
        jmethodID bi = env->GetMethodID(builderClass, "<init>",
                                        "(Landroid/content/Context;Ljava/lang/String;)V");
        builder = env->NewObject(builderClass, bi, ctx, channelId);
    } else {
        jmethodID bi = env->GetMethodID(builderClass, "<init>",
                                        "(Landroid/content/Context;)V");
        builder = env->NewObject(builderClass, bi, ctx);
    }
    if (env->ExceptionCheck()) { cleanup(); return; }
    auto callBuilder = [&](const char* method, const char* sig, jobject arg) -> jobject {
        jmethodID m = env->GetMethodID(builderClass, method, sig);
        return env->CallObjectMethod(builder, m, arg);
    };
    builder = callBuilder("setContentTitle",
        "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;",
        env->NewStringUTF(title));
    builder = callBuilder("setContentText",
        "(Ljava/lang/CharSequence;)Landroid/app/Notification$Builder;",
        env->NewStringUTF(msg));
    {
        jmethodID setIcon = env->GetMethodID(builderClass, "setSmallIcon",
                                             "(I)Landroid/app/Notification$Builder;");
        builder = env->CallObjectMethod(builder, setIcon, (jint)0x01080020);
    }
    if (env->ExceptionCheck()) { cleanup(); return; }
    jmethodID build = env->GetMethodID(builderClass, "build", "()Landroid/app/Notification;");
    jobject notif   = env->CallObjectMethod(builder, build);
    if (env->ExceptionCheck()) { cleanup(); return; }
    jmethodID notify = env->GetMethodID(nmClass, "notify", "(ILandroid/app/Notification;)V");
    env->CallVoidMethod(nm, notify, (jint)1, notif);
    if (env->ExceptionCheck()) env->ExceptionClear();
    else LOGI("[Notif] Sent: %s — %s", title, msg);
    cleanup();
}

// ── Helper: tunggu library dimuat ────────────────────────────────────────────
static void waitForLib(const char* libName, int maxWaitSec = 60) {
    LOGI("[on_init] Waiting for %s ...", libName);
    for (int i = 0; i < maxWaitSec; i++) {
        if (isLibraryLoaded(libName)) {
            LOGI("[on_init] %s loaded after %ds", libName, i);
            return;
        }
        if (i == 5) {
            // Dump /proc/self/maps sekali untuk debug
            FILE* fp = fopen("/proc/self/maps", "rt");
            if (fp) {
                char line[512];
                while (fgets(line, sizeof(line), fp)) {
                    if (strstr(line, ".so")) {
                        line[strcspn(line, "\n")] = 0;
                        LOGW("[maps] %s", line);
                    }
                }
                fclose(fp);
            }
        }
        sleep(1);
    }
    LOGW("[on_init] Timeout waiting for %s, continuing...", libName);
}

// ── Installasi game hooks (Dobby) ─────────────────────────────────────────────
static void installGameHooks() {
    // ════════════════════════════════════════════════════════════════════════
    // TEMPLATE: Isi di sini dengan hook game spesifik Anda
    //
    // Contoh:
    //   uintptr_t base = GetLibBase(GetTargetLib());
    //
    //   static void (*orig_TakeDamage)(void*, float, void*) = nullptr;
    //   auto hook_TakeDamage = [](void* self, float dmg, void* info) {
    //       orig_TakeDamage(self, 0.f, info); // godmode
    //   };
    //   DobbyHook((void*)(base + 0x1234ABCD),
    //             reinterpret_cast<void*>(+hook_TakeDamage),
    //             reinterpret_cast<void**>(&orig_TakeDamage));
    //
    // PENTING: Jangan hook eglSwapBuffers atau vkQueuePresentKHR di sini!
    //          Hook tersebut menyebabkan blackscreen dan sudah DIHAPUS.
    // ════════════════════════════════════════════════════════════════════════
    LOGI("[on_init] installGameHooks() — add Dobby hooks here");
}

// ── on_init() — persis dari Proyek 2, minus bagian ImGui ─────────────────────
void on_init() {
    LOGI("[on_init] ===== START =====");

    // 1. Tunggu libil2cpp.so
    waitForLib((const char*)targetLibName);

    // FIX TIMING: Sama dengan MLBB-Mod esp_thread — sleep 3 detik SETELAH
    // library ditemukan di /proc/self/maps. Library ada di memory tapi
    // constructors-nya belum tentu selesai jalan → crash kalau langsung Init().
    LOGI("[on_init] libil2cpp found, sleeping 3s before Init...");
    sleep(3);

    // 2. Detect & set target lib
    LOGI("[on_init] Il2cpp::Init ...");
    Il2cpp::Init();
    LOGI("[on_init] Il2cpp::EnsureAttached ...");
    Il2cpp::EnsureAttached();

    {
        std::string pkg = Il2cpp::getPackageName();
        LOGI("[on_init] Package: %s", pkg.c_str());
        bool needCsharp = (pkg.find("com.moonton")       != std::string::npos ||
                           pkg.find("com.mobile.legends") != std::string::npos ||
                           pkg.find("com.mobiin.gp")      != std::string::npos);
        if (needCsharp) {
            SetTargetLib("libcsharp.so");
            LOGI("[on_init] MLBB → target: libcsharp.so");
        } else {
            void* h = xdl_open("libcsharp.so", XDL_TRY_FORCE_LOAD);
            if (h) {
                xdl_close(h);
                if (GetLibBase("libil2cpp.so") == 0) {
                    SetTargetLib("libcsharp.so");
                    LOGI("[on_init] auto-switch → libcsharp.so");
                }
            }
        }
    }

    // 3. Keyboard
    Keyboard::Init();
    LOGI("[on_init] Keyboard init done");

    // 4. Config
    ConfigInit();
    s_selectedScale = ConfigGet<int>("selectedScale", s_selectedScale);
    if (s_selectedScale < 0 || s_selectedScale > 6) {
        s_selectedScale = 3;
        ConfigSet("selectedScale", s_selectedScale);
    }
    LOGI("[on_init] Config loaded, scale=%d", s_selectedScale);

    // 5. Cari g_Image (Assembly-CSharp)
    auto images = Il2cpp::GetImages();
    LOGI("[on_init] Total images: %zu", images.size());

    auto* asm_ = Il2cpp::GetAssembly("Assembly-CSharp");
    if (asm_) {
        g_Image = asm_->getImage();
        LOGI("[on_init] Assembly-CSharp found (%zu classes)",
             g_Image ? g_Image->getClasses().size() : 0);
    }
    if (!g_Image && !images.empty()) {
        // Fallback: image terbesar
        size_t maxCount = 0;
        for (auto* img : images) {
            if (!img) continue;
            size_t cnt = img->getClasses().size();
            if (cnt > maxCount) { maxCount = cnt; g_Image = img; }
        }
        LOGW("[on_init] Fallback image: %s (%zu classes)",
             g_Image ? g_Image->getName() : "null", maxCount);
    }
    if (!g_Image) {
        LOGE("[on_init] FATAL: No valid IL2CPP image!");
        return;
    }

    // 6. Collect methods (untuk Tab_Tracer)
    for (auto* image : images)
        for (auto* klass : image->getClasses())
            for (auto* m : klass->getMethods())
                if (m && m->methodPointer) g_Methods.emplace_back(m);
    LOGI("[on_init] Total methods: %zu", g_Methods.size());
    std::sort(g_Methods.begin(), g_Methods.end(),
              [](const auto& a, const auto& b) {
                  return a->methodPointer < b->methodPointer;
              });

    // 7. Tool::Init (Class browser, patcher, dll)
    LOGI("[on_init] Tool::Init ...");
    Tool::Init(g_Image, images);
    LOGI("[on_init] Tool::Init done");

    // 8. NetworkTab::Init
    NetworkTab::Init();

    // 9. Game hooks (Dobby, NOP patch)
    LOGI("[on_init] Installing game hooks ...");
    installGameHooks();
    LOGI("[on_init] Game hooks done");

    // ═══════════════════════════════════════════════════════════════════════
    // 10. START IPC SERVER — KUNCI ARSITEKTUR HYBRID
    //
    // RpcHandler mendaftarkan semua command handler ke IPCServer.
    // IPCServer mulai listen di abstract UNIX socket @hybrid_imgui_ipc.
    // libimgui_ext.so (yang ada di app_process) akan connect ke sini
    // setiap kali ImGui menekan tombol yang membutuhkan data IL2CPP.
    // ═══════════════════════════════════════════════════════════════════════
    LOGI("[on_init] Registering IPC handlers...");
    RpcHandler::registerAll();

    LOGI("[on_init] Starting IPC Server...");
    bool serverOk = IPCServer::get().start();
    if (serverOk) {
        LOGI("[on_init] IPC Server listening on @hybrid_imgui_ipc");
    } else {
        LOGE("[on_init] IPC Server failed to start!");
    }

    // 11. Notifikasi
    Java_ShowToast("Tool Loaded! IPC Ready.");
    Java_SendNotification("IL2CPP Tool", "Hybrid mode active — ImGui via overlay");

    // 12. Set SHM ready — overlay tahu bahwa internal sudah fully initialized
    if (g_shm) {
        g_shm->ready = true;
        LOGI("[on_init] SHM ready flag set");
    }

    LOGI("[on_init] ===== DONE =====");
}

// ── hack_thread ────────────────────────────────────────────────────────────────
static void* hack_thread(void*) {
    // FIX: Jangan langsung on_init() — init SHM dulu agar overlay bisa connect
    // segera setelah injector selesai, bahkan sebelum IL2CPP ready.
    if (!SHM_Init()) {
        LOGE("[libinternal] SHM_Init failed, continuing without SHM");
    } else {
        LOGI("[libinternal] SHM ready at: %s", g_shm_real_path);
    }

    // Tunggu proses stabil setelah inject (sama dengan MLBB-Mod: sleep sebelum init)
    // 500ms terlalu singkat — game masih loading, crash!
    sleep(2);

    on_init();
    return nullptr;
}

// ── JNI_OnLoad ────────────────────────────────────────────────────────────────
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_jvm = vm;
    JNIEnv* env = nullptr;
    vm->GetEnv((void**)&env, JNI_VERSION_1_6);
    LOGI("[libinternal] JNI_OnLoad");
    return JNI_VERSION_1_6;
}

// ── Constructor: dipanggil saat dlopen oleh injector ─────────────────────────
__attribute__((constructor))
void lib_entry() {
    LOGI("[libinternal] lib_entry() constructor");
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, hack_thread, nullptr);
    pthread_attr_destroy(&attr);
}

// ── Destructor: dipanggil saat library di-unload ──────────────────────────────
__attribute__((destructor))
void lib_exit() {
    SHM_Cleanup();
}
