#pragma once
#include <jni.h>
#include <android/log.h>
#include <cstdarg>

// ============================================================
// Logger — selalu aktif, output ke logcat (matlog/alogcat dll)
// Tag: MXP
// Filter di matlog: tag = MXP
// ============================================================

#define LOG_TAG "MXP"

// ── Macro utama ─────────────────────────────────────────────
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── Helper pointer/value ─────────────────────────────────────
#define LOGPTR(ptr)    LOGD(#ptr " => %p",     (void*)(ptr))
#define LOGHEX(ptr)    LOGD(#ptr " => 0x%llX", (unsigned long long)(uintptr_t)(ptr))
#define LOGINT(var)    LOGD(#var " => %d",     (int)(var))
#define LOGSINGLE(var) LOGD(#var " => %f",     (float)(var))
#define LOGSTR(il2str) LOGD(#il2str " => %s",  (il2str) ? (il2str)->to_string().c_str() : "(null)")

// ── Cek null + log otomatis ──────────────────────────────────
#define LOGNULL(ptr, tag) \
    do { if (!(ptr)) { LOGE("[NULL] %s: " #ptr " is null", tag); return; } } while(0)

#define LOGNULL_V(ptr, tag, retval) \
    do { if (!(ptr)) { LOGE("[NULL] %s: " #ptr " is null", tag); return retval; } } while(0)

// ── Stub agar panggilan logger::Clear() / logger::Draw() tidak error ─
namespace logger {
    inline void Clear() {}
    inline void AddLog(const char* /*prefix*/, const char* /*fmt*/, ...) {}
    void Draw(const char *title, bool *p_open = nullptr);
}
