# ==============================================================================
# CMakeLists.txt — HYBRID STANDALONE IMGUI
# Drop-in replacement untuk Android.mk Proyek 2.
#
# STRUKTUR OUTPUT:
#   libimgui_ext.so  → External process (app_process): EGL standalone + ImGui UI
#   libinternal.so   → Internal process (game):         IL2CPP + Dobby + IPC server
#
# CARA BUILD (di root project):
#   mkdir build && cd build
#   cmake .. -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#            -DANDROID_ABI=arm64-v8a \
#            -DANDROID_PLATFORM=android-28 \
#            -DANDROID_STL=c++_static \
#            -DCMAKE_BUILD_TYPE=Release
#   cmake --build . -j$(nproc)
# ==============================================================================

cmake_minimum_required(VERSION 3.22)
project(HybridImGui LANGUAGES CXX C)

# Set standar bahasa
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ==============================================================================
# Direktori root source (= folder jni/ Proyek 2)
# ==============================================================================
set(JNI_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# ==============================================================================
# ANDROID_ABI validation
# ==============================================================================
if(NOT ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
    message(WARNING "ANDROID_ABI not set, defaulting to arm64-v8a")
endif()
message(STATUS "Building for ABI: ${ANDROID_ABI}")

# ==============================================================================
# PREBUILT STATIC LIBRARIES
# ==============================================================================

# ── Dobby ─────────────────────────────────────────────────────────────────────
add_library(dobby_static STATIC IMPORTED)
set_target_properties(dobby_static PROPERTIES
    IMPORTED_LOCATION
        "${JNI_ROOT}/Dobby/prebuilt/${ANDROID_ABI}/libdobby.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${JNI_ROOT}/Dobby/include"
)

# ── AsmJit ────────────────────────────────────────────────────────────────────
add_library(asmjit STATIC IMPORTED)
set_target_properties(asmjit PROPERTIES
    IMPORTED_LOCATION
        "${JNI_ROOT}/asmjit/prebuilt/${ANDROID_ABI}/libasmjit.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${JNI_ROOT}/asmjit/src"
)

# ── Frida-Gum ─────────────────────────────────────────────────────────────────
add_library(frida_gum STATIC IMPORTED)
set_target_properties(frida_gum PROPERTIES
    IMPORTED_LOCATION
        "${JNI_ROOT}/Frida/${ANDROID_ABI}/libfrida-gum.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${JNI_ROOT}/Frida/${ANDROID_ABI};${JNI_ROOT}/Frida/gumpp"
)

# ── Gumpp ─────────────────────────────────────────────────────────────────────
add_library(gumpp STATIC IMPORTED)
set_target_properties(gumpp PROPERTIES
    IMPORTED_LOCATION
        "${JNI_ROOT}/Frida/prebuilt/${ANDROID_ABI}/libgumpp.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${JNI_ROOT}/Frida/${ANDROID_ABI};${JNI_ROOT}/Frida/gumpp"
)

# ==============================================================================
# SOURCE LISTS
# ==============================================================================

# ── ImGui ─────────────────────────────────────────────────────────────────────
set(IMGUI_SOURCES
    ${JNI_ROOT}/imgui/imgui.cpp
    ${JNI_ROOT}/imgui/imgui_draw.cpp
    ${JNI_ROOT}/imgui/imgui_demo.cpp
    ${JNI_ROOT}/imgui/imgui_tables.cpp
    ${JNI_ROOT}/imgui/imgui_widgets.cpp
    ${JNI_ROOT}/imgui/backends/imgui_impl_opengl3.cpp
)

# ── IL2CPP ────────────────────────────────────────────────────────────────────
set(IL2CPP_SOURCES
    ${JNI_ROOT}/Il2cpp/Il2cpp.cpp
    ${JNI_ROOT}/Il2cpp/il2cpp-class.cpp
)

# ── XDL ───────────────────────────────────────────────────────────────────────
set(XDL_SOURCES
    ${JNI_ROOT}/Il2cpp/xdl/xdl.c
    ${JNI_ROOT}/Il2cpp/xdl/xdl_iterate.c
    ${JNI_ROOT}/Il2cpp/xdl/xdl_linker.c
    ${JNI_ROOT}/Il2cpp/xdl/xdl_lzma.c
    ${JNI_ROOT}/Il2cpp/xdl/xdl_util.c
)

# ── KittyMemory ───────────────────────────────────────────────────────────────
set(KITTY_SOURCES
    ${JNI_ROOT}/KittyMemory/KittyMemory.cpp
    ${JNI_ROOT}/KittyMemory/MemoryPatch.cpp
    ${JNI_ROOT}/KittyMemory/MemoryBackup.cpp
    ${JNI_ROOT}/KittyMemory/KittyUtils.cpp
)

# ── Includes ──────────────────────────────────────────────────────────────────
set(INCLUDES_SOURCES
    ${JNI_ROOT}/Includes/Utils.cpp
    ${JNI_ROOT}/Includes/Logger.cpp
    ${JNI_ROOT}/Includes/aarch64_atomics_stub.c
)

# ── Tool ──────────────────────────────────────────────────────────────────────
set(TOOL_SOURCES
    ${JNI_ROOT}/Tool/Keyboard.cpp
    ${JNI_ROOT}/Tool/Tool.cpp
    ${JNI_ROOT}/Tool/Util.cpp
    ${JNI_ROOT}/Tool/Patcher.cpp
    ${JNI_ROOT}/Tool/PopUpSelector.cpp
    ${JNI_ROOT}/Tool/ProjectSessionManager.cpp
    ${JNI_ROOT}/Tool/Unity.cpp
    ${JNI_ROOT}/Tool/Frida.cpp
    ${JNI_ROOT}/Tool/CodeGenerator.cpp
    ${JNI_ROOT}/Tool/Tab_ESP.cpp
    ${JNI_ROOT}/Tool/Tab_Dumper.cpp
    ${JNI_ROOT}/Tool/Tab_Inspector.cpp
    ${JNI_ROOT}/Tool/Tab_Tracer.cpp
    ${JNI_ROOT}/Tool/Tab_Network.cpp
    ${JNI_ROOT}/Tool/sub/View_ClassBrowser.cpp
    ${JNI_ROOT}/Tool/sub/View_MemoryHex.cpp
    ${JNI_ROOT}/Tool/sub/View_MethodAnalyzer.cpp
    ${JNI_ROOT}/Tool/sub/View_FieldPatcher.cpp
    ${JNI_ROOT}/Tool/sub/View_Hierarchy.cpp
)

# ── Menu ──────────────────────────────────────────────────────────────────────
set(MENU_SOURCES
    ${JNI_ROOT}/Menu/ImGui.cpp
    ${JNI_ROOT}/Menu/MainWindow.cpp
)

# ── IPC Layer ─────────────────────────────────────────────────────────────────
set(IPC_CLIENT_SOURCES
    ${JNI_ROOT}/ipc/ipc_client.cpp
)
set(IPC_SERVER_SOURCES
    ${JNI_ROOT}/ipc/ipc_server.cpp
    ${JNI_ROOT}/ipc/rpc_handler.cpp
)

# ── ImGui Ext ─────────────────────────────────────────────────────────────────
set(IMGUI_EXT_SOURCES
    ${JNI_ROOT}/imgui_ext/egl_standalone.cpp
    ${JNI_ROOT}/imgui_ext/imgui_render.cpp
    ${JNI_ROOT}/imgui_ext/jni_bridge.cpp
    ${JNI_ROOT}/imgui_ext/ext_stubs.cpp
)

# ── Internal Entry Point ──────────────────────────────────────────────────────
set(INTERNAL_MAIN_SOURCES
    ${JNI_ROOT}/internal/internal_main.cpp
)

# ==============================================================================
# COMMON INCLUDE DIRECTORIES
# ==============================================================================
set(COMMON_INCLUDES
    ${JNI_ROOT}
    ${JNI_ROOT}/imgui
    ${JNI_ROOT}/imgui/backends
    ${JNI_ROOT}/asmjit/src
    ${JNI_ROOT}/json/single_include
    ${JNI_ROOT}/Il2cpp
    ${JNI_ROOT}/Il2cpp/xdl/include
    ${JNI_ROOT}/Dobby/include
    ${JNI_ROOT}/Frida/${ANDROID_ABI}
    ${JNI_ROOT}/Frida/gumpp
    ${JNI_ROOT}/KittyMemory
    ${JNI_ROOT}/ipc
    ${JNI_ROOT}/imgui_ext
)

# ==============================================================================
# COMMON COMPILE OPTIONS
# ==============================================================================
set(COMMON_DEFINES
    ASMJIT_STATIC
    ASMJIT_NO_X86
    USE_FRIDA
    __DEBUG__
    __LOG_ALL__
)

set(COMMON_LDFLAGS
    -Wl,--hash-style=both
    -Wl,-exclude-libs,ALL
    -Wl,--gc-sections
    -Wl,--allow-multiple-definition
)

# ==============================================================================
# TARGET 1: libimgui_ext.so
# ==============================================================================
add_library(imgui_ext SHARED
    ${IMGUI_SOURCES}
    ${INCLUDES_SOURCES}
    ${MENU_SOURCES}
    ${TOOL_SOURCES}
    ${IPC_CLIENT_SOURCES}
    ${IMGUI_EXT_SOURCES}
    ${IL2CPP_SOURCES}
    ${XDL_SOURCES}
    ${KITTY_SOURCES}
)

target_include_directories(imgui_ext PRIVATE ${COMMON_INCLUDES})

# COMPILE OPTIONS - DIPISAH PER BAHASA
target_compile_options(imgui_ext PRIVATE
    -fvisibility=hidden
    -O2
    -Wno-error=format-security
    -Wno-unused-variable
    -Wno-unused-function
    -w
)

target_compile_options(imgui_ext PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=c++11-narrowing>
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
)

target_compile_definitions(imgui_ext PRIVATE ${COMMON_DEFINES} EXT_PROCESS)
target_link_options(imgui_ext PRIVATE ${COMMON_LDFLAGS})

if(EXISTS "${JNI_ROOT}/imgui_ext/exports.map")
    target_link_options(imgui_ext PRIVATE
        -Wl,--version-script,${JNI_ROOT}/imgui_ext/exports.map
    )
endif()

target_link_libraries(imgui_ext
    asmjit
    gumpp
    frida_gum
    log
    android
    EGL
    GLESv3
)

# ==============================================================================
# TARGET 2: libinternal.so
# ==============================================================================
add_library(internal SHARED
    ${INCLUDES_SOURCES}
    ${IL2CPP_SOURCES}
    ${XDL_SOURCES}
    ${KITTY_SOURCES}
    ${TOOL_SOURCES}
    ${IPC_SERVER_SOURCES}
    ${INTERNAL_MAIN_SOURCES}
)

target_include_directories(internal PRIVATE ${COMMON_INCLUDES} ${JNI_ROOT}/internal)

# COMPILE OPTIONS - DIPISAH PER BAHASA
target_compile_options(internal PRIVATE
    -fvisibility=hidden
    -O2
    -Wno-error=format-security
    -Wno-unused-variable
    -Wno-unused-function
    -w
)

target_compile_options(internal PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-frtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=c++11-narrowing>
    $<$<COMPILE_LANGUAGE:CXX>:-fvisibility-inlines-hidden>
)

target_compile_definitions(internal PRIVATE ${COMMON_DEFINES} NO_IMGUI NO_EGL_HOOK)
target_link_options(internal PRIVATE ${COMMON_LDFLAGS})

target_link_libraries(internal
    dobby_static
    asmjit
    gumpp
    frida_gum
    log
    android
    EGL
    GLESv3
)

# ==============================================================================
# TARGET 3: injector (executable)
#
# Dikompile sebagai PIE executable — bisa dijalankan langsung via:
#   su -c /data/data/com.hybrid.imgui/files/injector
#
# Alur:
#   1. Baca config (.tool_cfg) → apkPath / nativeDir / targetPkg
#   2. Tunggu proses game → inject libinternal.so via ptrace
#   3. Launch overlay via app_process + CLASSPATH=APK
#   4. Keepalive loop
#
# Di-bundle di APK sebagai assets/injector (opsional XOR encrypt di CI).
# ModManager.java ekstrak, chmod 755, lalu jalankan via su.
# ==============================================================================
add_executable(injector
    ${JNI_ROOT}/injector_main.cpp
    ${JNI_ROOT}/injector/nullelf.cpp
    ${JNI_ROOT}/injector/nullproc.cpp
    ${JNI_ROOT}/injector/nullutils.cpp
    ${JNI_ROOT}/injector/pnull.cpp
)

target_include_directories(injector PRIVATE
    ${JNI_ROOT}
    ${JNI_ROOT}/injector
    ${JNI_ROOT}/injector/trace
    ${JNI_ROOT}/injector/null
)

target_compile_options(injector PRIVATE
    -std=c++17
    -w
    -fexceptions
    -fpermissive
    -Wno-error=format-security
)

# PIE wajib agar executable bisa run di Android modern
target_link_options(injector PRIVATE
    -pie
    -Wl,--gc-sections
    -Wl,--strip-all
)

target_link_libraries(injector
    log
    android
)