#ifndef UTILS
#define UTILS

#include <jni.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

typedef unsigned long DWORD;

// ─── Library Base ─────────────────────────────────────────────────────────────
/**
 * GetLibBase — dapatkan base address library dari /proc/self/maps dengan cache.
 *
 * Mencari region pertama yang berisi nama library (terurut ascending di maps).
 * Hasilnya di-cache per-library sehingga hanya baca /proc/self/maps sekali.
 *
 * Cara hitung RVA yang BENAR dan STABIL:
 *   uintptr_t rva = (uintptr_t)method->methodPointer - GetLibBase("libil2cpp.so");
 *
 * RVA ini sama persis dengan offset yang ditunjukkan IDA/Ghidra dan TIDAK
 * berubah antar restart game (walaupun base address berubah karena ASLR).
 */
uintptr_t GetLibBase(const char *libraryName);

// Kompatibilitas lama — memanggil GetLibBase secara internal
DWORD findLibrary(const char *library);

DWORD getAbsoluteAddress(const char *libraryName, DWORD relativeAddr);

jboolean isGameLibLoaded(JNIEnv *env, jobject thiz);

bool isLibraryLoaded(const char *libraryName);

uintptr_t string2Offset(const char *c);

// ─── CATATAN BREAKING CHANGE ──────────────────────────────────────────────────
// patchOffset() dan patchOffsetSym() telah DIHAPUS karena duplikat KittyMemory.
// Gunakan KittyMemory::MemoryPatch secara langsung:
//
//   #include "KittyMemory/MemoryPatch.h"
//
//   // Patch via offset relatif dari library:
//   auto p = MemoryPatch::createWithHex("libil2cpp.so", rva, "C0 03 5F D6");
//   p.Modify();    // aktifkan
//   p.Restore();   // kembalikan ke original
//
//   // Patch via alamat absolut:
//   auto p = MemoryPatch::createWithHex(absAddress, "C0 03 5F D6");
// ─────────────────────────────────────────────────────────────────────────────

namespace ToastLength {
    inline const int LENGTH_LONG  = 1;
    inline const int LENGTH_SHORT = 0;
}

// ─── Memory Safety ────────────────────────────────────────────────────────────

/**
 * IsPtrValid — cek apakah alamat memori valid dan readable menggunakan mincore().
 * nullptr dan alamat < 4096 selalu return false.
 */
bool IsPtrValid(const void *addr);

/**
 * SafeRead<T> — baca nilai bertipe T dari addr secara aman.
 * Return false jika addr tidak valid; memcpy ke *out jika valid.
 */
template<typename T>
inline bool SafeRead(const void *addr, T *out)
{
    if (!addr || !out) return false;
    if (!IsPtrValid(addr)) return false;
    const char *end = static_cast<const char *>(addr) + sizeof(T) - 1;
    if (!IsPtrValid(end)) return false;
    memcpy(out, addr, sizeof(T));
    return true;
}

/**
 * GetTargetLib — nama library target untuk kalkulasi RVA.
 * Default: "libil2cpp.so"
 * Untuk MLBB dan game yang pakai libcsharp.so: "libcsharp.so"
 * Bisa diubah via SetTargetLib() atau UI Settings.
 */
const char* GetTargetLib();
void        SetTargetLib(const char* libName);

#endif // UTILS
