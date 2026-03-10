// =============================================================================
// ext_stubs.cpp — Stub definitions untuk libimgui_ext.so (EXT_PROCESS)
//
// Simbol-simbol ini didefinisikan di internal_main.cpp (libinternal.so),
// tapi di-refer oleh Tool files yang juga masuk ke libimgui_ext.so.
// Stub ini hanya placeholder agar linker tidak error saat build libimgui_ext.
// =============================================================================

#ifdef EXT_PROCESS

#include <vector>
#include "Il2cpp/il2cpp-class.h"
#include "Il2cpp/Il2cpp.h"

// Didefinisikan di internal_main.cpp — stub untuk EXT_PROCESS
Il2CppImage* g_Image = nullptr;
std::vector<MethodInfo*> g_Methods;

void ConfigSet_int(const char* key, int value) {
    (void)key; (void)value; // no-op di ext process
}

int ConfigGet_int(const char* key, int defaultValue) {
    (void)key;
    return defaultValue; // kembalikan default di ext process
}

#endif // EXT_PROCESS