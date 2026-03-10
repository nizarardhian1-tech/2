#include "Frida.h"
#if defined(__aarch64__)
#include "Frida/arm64-v8a/frida-gum.h"
#elif defined(__arm__)
#include "Frida/armeabi-v7a/frida-gum.h"
#else
#include "Frida/arm64-v8a/frida-gum.h"
#endif
#include "Frida/gumpp/gumpp.hpp"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Tool/Tool.h"
#include <mutex>
#include <cstring>
#include "Includes/Utils.h"

extern std::vector<MethodInfo *> g_Methods;
extern std::mutex hookerMtx;

// ─── CPU Context structs (minimal, sesuai layout frida-gum internal) ─────────
// Tidak pakai Gum::Backtracer → crash di Unity release (DWARF stripped)
struct Arm64Ctx {
    uint64_t pc, sp, fp, lr;       // lr = x30 = return address
    uint64_t x[29];                 // x0..x28
    struct { uint64_t lo, hi; } v[32]; // v[0].lo = float/double return
};
struct Arm32Ctx {
    uint32_t pc, sp, cpsr;
    uint32_t r[16]; // r[14] = LR, r[0] = int/float return
};

// ─── Binary search: cari MethodInfo paling dekat ke addr ─────────────────────
static MethodInfo *binarySearchClosest(uintptr_t addr)
{
    if (g_Methods.empty()) return nullptr;
    int left = 0, right = (int)g_Methods.size() - 1;
    while (left <= right)
    {
        int pivot = (left + right) / 2;
        intptr_t cmp = (intptr_t)g_Methods[pivot]->methodPointer - (intptr_t)addr;
        if      (cmp == 0) return g_Methods[pivot];
        else if (cmp  > 0) right = pivot - 1;
        else               left  = pivot + 1;
    }
    int sz = (int)g_Methods.size();
    if (right < 0)  return g_Methods[0];
    if (left >= sz) return g_Methods[sz - 1];
    intptr_t dR = (intptr_t)addr - (intptr_t)g_Methods[right]->methodPointer;
    intptr_t dL = (intptr_t)g_Methods[left]->methodPointer - (intptr_t)addr;
    return (dR <= dL) ? g_Methods[right] : g_Methods[left];
}

// ─── Format addr → "Namespace.Class::Method+0xOffset" ────────────────────────
static std::string resolveAddr(uintptr_t addr)
{
    if (!addr) return "(null)";
    MethodInfo *m = binarySearchClosest(addr);
    if (!m || !m->methodPointer) return "(unknown)";
    intptr_t off = (intptr_t)addr - (intptr_t)m->methodPointer;
    if (off < 0 || off > 0x8000)
    {
        char buf[48]; snprintf(buf, sizeof(buf), "(native 0x%" PRIxPTR ")", addr);
        return buf;
    }
    auto *klass = m->getClass();
    char buf[256];
    snprintf(buf, sizeof(buf), "%s::%s+0x%" PRIxPTR,
             klass ? klass->getFullName().c_str() : "?",
             m->getName() ? m->getName() : "?",
             (uintptr_t)off);
    return buf;
}

// ─── Type helpers ─────────────────────────────────────────────────────────────
static bool isPrimitive(const char *tn)
{
    if (!tn) return false;
    return !strcmp(tn,"System.Boolean")||!strcmp(tn,"System.Byte")||
           !strcmp(tn,"System.Int16")  ||!strcmp(tn,"System.UInt16")||
           !strcmp(tn,"System.Int32")  ||!strcmp(tn,"System.UInt32")||
           !strcmp(tn,"System.Int64")  ||!strcmp(tn,"System.UInt64")||
           !strcmp(tn,"System.Single") ||!strcmp(tn,"System.Double");
}
static bool isVec3(const char *tn){ return tn && strstr(tn,"Vector3"); }
static bool isVec2(const char *tn){ return tn && strstr(tn,"Vector2") && !isVec3(tn); }

// ─── Baca satu field dari memory secara type-safe ─────────────────────────────
std::string ReadFieldValue(void *obj, uintptr_t offset, const char *tn)
{
    if (!obj || !tn) return "?";
    uint8_t *b = (uint8_t*)obj + offset;
    char buf[80];
    if (!strcmp(tn,"System.Boolean")) return *b ? "true":"false";
    if (!strcmp(tn,"System.Byte"))  { snprintf(buf,sizeof(buf),"%u", *(uint8_t*)b);  return buf;}
    if (!strcmp(tn,"System.Int16")) { snprintf(buf,sizeof(buf),"%d", *(int16_t*)b);  return buf;}
    if (!strcmp(tn,"System.UInt16")){ snprintf(buf,sizeof(buf),"%u", *(uint16_t*)b); return buf;}
    if (!strcmp(tn,"System.Int32")) { snprintf(buf,sizeof(buf),"%d", *(int32_t*)b);  return buf;}
    if (!strcmp(tn,"System.UInt32")){ snprintf(buf,sizeof(buf),"%u", *(uint32_t*)b); return buf;}
    if (!strcmp(tn,"System.Int64")) { snprintf(buf,sizeof(buf),"%lld",(long long)*(int64_t*)b);  return buf;}
    if (!strcmp(tn,"System.UInt64")){ snprintf(buf,sizeof(buf),"%llu",(unsigned long long)*(uint64_t*)b); return buf;}
    if (!strcmp(tn,"System.Single")){ snprintf(buf,sizeof(buf),"%.5g",*(float*)b);   return buf;}
    if (!strcmp(tn,"System.Double")){ snprintf(buf,sizeof(buf),"%.5g",*(double*)b);  return buf;}
   if (!strcmp(tn,"System.String")){
        auto *s = *(Il2CppString**)b;
        if (!s || !IsPtrValid(s)) return "(null)";
        
        // Pengecekan ekstra untuk klass pointer agar tidak crash
        Il2CppClass *sKlass = nullptr;
        if (!SafeRead<Il2CppClass*>((const void*)s, &sKlass) || !sKlass || !IsPtrValid(sKlass)) {
            return "(invalid_string_ptr)";
        }
        
        try {
            return "\"" + s->to_string() + "\"";
        } catch (...) {
            return "(string_read_error)";
        }
    }
    if (isVec3(tn)){
        float *f=(float*)b;
        snprintf(buf,sizeof(buf),"(%.3f,%.3f,%.3f)",f[0],f[1],f[2]);
        return buf;
    }
    if (isVec2(tn)){
        float *f=(float*)b;
        snprintf(buf,sizeof(buf),"(%.3f,%.3f)",f[0],f[1]);
        return buf;
    }
    void *ref = *(void**)b;
    if (!ref) return "(null)";
    snprintf(buf,sizeof(buf),"@0x%llX",(unsigned long long)ref);
    return buf;
}

// ─── Baca argument dari register ─────────────────────────────────────────────
static std::string readArgValue(Gum::InvocationContext *ctx, unsigned int idx, const char *tn)
{
    if (!tn) return "?";
    void *raw = ctx->get_nth_argument_ptr(idx);
    char buf[64];
    if (!strcmp(tn,"System.Boolean")) return ((uintptr_t)raw&0xFF)?"true":"false";
    if (!strcmp(tn,"System.Byte"))  { snprintf(buf,sizeof(buf),"%u", (uint32_t)(uintptr_t)raw&0xFF); return buf;}
    if (!strcmp(tn,"System.Int16")) { snprintf(buf,sizeof(buf),"%d", (int16_t)(uintptr_t)raw);  return buf;}
    if (!strcmp(tn,"System.UInt16")){ snprintf(buf,sizeof(buf),"%u", (uint16_t)(uintptr_t)raw); return buf;}
    if (!strcmp(tn,"System.Int32")) { snprintf(buf,sizeof(buf),"%d", (int32_t)(uintptr_t)raw);  return buf;}
    if (!strcmp(tn,"System.UInt32")){ snprintf(buf,sizeof(buf),"%u", (uint32_t)(uintptr_t)raw); return buf;}
    if (!strcmp(tn,"System.Int64")) { snprintf(buf,sizeof(buf),"%lld",(long long)(intptr_t)raw); return buf;}
    if (!strcmp(tn,"System.UInt64")){ snprintf(buf,sizeof(buf),"%llu",(unsigned long long)(uintptr_t)raw); return buf;}
    if (!strcmp(tn,"System.Single")||!strcmp(tn,"System.Double")){
#if defined(__aarch64__)
        auto *c=(const Arm64Ctx*)ctx->get_cpu_context();
        if (c){
            if (!strcmp(tn,"System.Single")){float f;memcpy(&f,&c->v[idx].lo,4);snprintf(buf,sizeof(buf),"%.5g",f);return buf;}
            else{double d;memcpy(&d,&c->v[idx].lo,8);snprintf(buf,sizeof(buf),"%.5g",d);return buf;}
        }
#endif
        return "float(?)";
    }
    if (!strcmp(tn,"System.String")){
        auto *s=(Il2CppString*)raw;
        if (!s||!s->klass) return "(null)";
        return "\""+s->to_string()+"\"";
    }
    if (!raw) return "(null)";
    snprintf(buf,sizeof(buf),"@0x%llX",(unsigned long long)raw);
    return buf;
}

// ─── Baca return value ────────────────────────────────────────────────────────
static std::string readReturnValue(Gum::InvocationContext *ctx, MethodInfo *method)
{
    auto *rt = method->getReturnType();
    if (!rt) return "?";
    const char *tn = rt->getName();
    if (!tn||!strcmp(tn,"System.Void")) return "void";
    char buf[64];
    void *raw = ctx->get_return_value<void*>();
    if (!strcmp(tn,"System.Boolean")) return ((uintptr_t)raw&0xFF)?"true":"false";
    if (!strcmp(tn,"System.Int32"))  { snprintf(buf,sizeof(buf),"%d", (int32_t)(uintptr_t)raw); return buf;}
    if (!strcmp(tn,"System.UInt32")) { snprintf(buf,sizeof(buf),"%u", (uint32_t)(uintptr_t)raw); return buf;}
    if (!strcmp(tn,"System.Int64"))  { snprintf(buf,sizeof(buf),"%lld",(long long)(intptr_t)raw); return buf;}
    if (!strcmp(tn,"System.UInt64")) { snprintf(buf,sizeof(buf),"%llu",(unsigned long long)(uintptr_t)raw); return buf;}
    if (!strcmp(tn,"System.Byte"))   { snprintf(buf,sizeof(buf),"%u", (uint32_t)(uintptr_t)raw&0xFF); return buf;}
    if (!strcmp(tn,"System.Int16"))  { snprintf(buf,sizeof(buf),"%d", (int16_t)(uintptr_t)raw); return buf;}
    if (!strcmp(tn,"System.Single")||!strcmp(tn,"System.Double")){
#if defined(__aarch64__)
        auto *c=(const Arm64Ctx*)ctx->get_cpu_context();
        if (c){
            if (!strcmp(tn,"System.Single")){float f;memcpy(&f,&c->v[0].lo,4);snprintf(buf,sizeof(buf),"%.5g",f);return buf;}
            else{double d;memcpy(&d,&c->v[0].lo,8);snprintf(buf,sizeof(buf),"%.5g",d);return buf;}
        }
#elif defined(__arm__)
        auto *c=(const Arm32Ctx*)ctx->get_cpu_context();
        if (c&&!strcmp(tn,"System.Single")){float f;memcpy(&f,&c->r[0],4);snprintf(buf,sizeof(buf),"%.5g",f);return buf;}
#endif
        return "float(?)";
    }
    if (!strcmp(tn,"System.String")){
        auto *s=(Il2CppString*)raw;
        if (!s||!s->klass) return "(null)";
        return "\""+s->to_string()+"\"";
    }
    if (!raw) return "(null)";
    snprintf(buf,sizeof(buf),"@0x%llX",(unsigned long long)raw);
    return buf;
}

namespace Frida
{
    // ─── TraceListener ───────────────────────────────────────────────────────
    class TraceListener : public Gum::InvocationListener
    {
    public:
        virtual void on_enter(Gum::InvocationContext *context)
        {
            auto *hd = context->get_listener_function_data<HookerData>();
            if (!hd || !hd->method) return;

            // ── 1. Overlay trace (selalu) ─────────────────────────────────────
            hd->hitCount++;
            hd->time = 1.f;
            char ovlBuf[256]{0};
            snprintf(ovlBuf, sizeof(ovlBuf), "%p | %s::%s",
                (void*)hd->method->getAbsAddress(),
                hd->method->getClass() ? hd->method->getClass()->getName() : "?",
                hd->method->getName() ? hd->method->getName() : "?");
            bool found = false;
            for (auto it = HookerData::visited.rbegin(); it != HookerData::visited.rend(); ++it)
            {
                if (it->name == ovlBuf)
                { it->goneTime=10.f; it->time=2.f; it->hitCount++; found=true; break; }
            }
            if (!found) HookerData::visited.push_back({ovlBuf, 2.f, 10.f, 0});

            // ── 2. Collect 'this' ─────────────────────────────────────────────
            Il2CppObject *thiz = nullptr;
            bool isStatic = Il2cpp::GetIsMethodStatic(hd->method);
            if (!isStatic)
            {
                thiz = context->get_nth_argument<Il2CppObject*>(0);
                if (thiz) HookerData::collectSet[hd->method->getClass()].emplace(thiz);
            }

            // ── 3. Flags ──────────────────────────────────────────────────────
            bool doCaller = __atomic_load_n(&hd->trackCaller, __ATOMIC_ACQUIRE);
            bool doParams = __atomic_load_n(&hd->trackParams, __ATOMIC_ACQUIRE);
            bool doFields = __atomic_load_n(&hd->trackFields, __ATOMIC_ACQUIRE);
            bool doReturn = __atomic_load_n(&hd->trackReturn, __ATOMIC_ACQUIRE);

            // ── 4. Inisialisasi per-invocation slot ───────────────────────────
            // PENTING: CallRecord mengandung std::string/vector → harus placement new
            // Frida alokasikan raw memory berukuran sizeof(CallRecord) per invocation
            CallRecord *rec = context->get_listener_invocation_data<CallRecord>();
            new (rec) CallRecord();
            rec->index   = ++hd->callCounter;
            rec->thisPtr = thiz;

            // ── 5. Field Watch: update instance jika belum ada ────────────────
            if (__atomic_load_n(&hd->watchActive, __ATOMIC_ACQUIRE) && thiz && !hd->watchInstance)
            {
                std::lock_guard<std::mutex> lk(hookerMtx);
                hd->watchInstance = thiz;
            }

            if (!doCaller && !doParams && !doFields && !doReturn) return;

            // ── 6. Caller (LR register) ───────────────────────────────────────
            if (doCaller)
            {
                uintptr_t lr = 0;
#if defined(__aarch64__)
                auto *c = (const Arm64Ctx*)context->get_cpu_context();
                if (c) lr = c->lr;
#elif defined(__arm__)
                auto *c = (const Arm32Ctx*)context->get_cpu_context();
                if (c) lr = c->r[14];
#endif
                if (lr) rec->caller = resolveAddr(lr);
            }

            // ── 7. Params ─────────────────────────────────────────────────────
            if (doParams)
            {
                auto pi = hd->method->getParamsInfo();
                unsigned int off = isStatic ? 0 : 1;
                for (size_t i = 0; i < pi.size(); i++)
                {
                    auto &[pname, ptype] = pi[i];
                    if (!ptype) continue;
                    const char *tn = ptype->getName();
                    CapturedParam cp;
                    cp.name  = pname ? pname : "arg";
                    cp.type  = tn ? tn : "?";
                    cp.value = readArgValue(context, (unsigned int)(i+off), tn);
                    rec->params.push_back(std::move(cp));
                }
            }

            // ── 8. Fields (snapshot dari 'this') ──────────────────────────────
            if (doFields && thiz)
            {
                auto fields = hd->method->getClass()->getFields(false);
                int cnt = 0;
                for (auto *f : fields)
                {
                    if (!f || cnt >= 32) break;
                    auto *ft = f->getType();
                    if (!ft) continue;
                    const char *tn = ft->getName();
                    if (!tn) continue;
                    CapturedField cf;
                    cf.name    = f->getName() ? f->getName() : "?";
                    cf.type    = tn;
                    cf.offset  = f->getOffset();
                    cf.canEdit = isPrimitive(tn);
                    cf.value   = ReadFieldValue(thiz, cf.offset, tn);
                    rec->fields.push_back(std::move(cf));
                    cnt++;
                }
            }
        }

        virtual void on_leave(Gum::InvocationContext *context)
        {
            auto *hd = context->get_listener_function_data<HookerData>();
            if (!hd || !hd->method) return;

            CallRecord *rec = context->get_listener_invocation_data<CallRecord>();
            if (!rec) return;

            // ── Return value ──────────────────────────────────────────────────
            if (__atomic_load_n(&hd->trackReturn, __ATOMIC_ACQUIRE))
            {
                rec->retValue  = readReturnValue(context, hd->method);
                rec->hasReturn = true;
            }

            // ── Simpan ke history (brief lock) ────────────────────────────────
            {
                std::lock_guard<std::mutex> lk(hookerMtx);
                hd->lastCall  = *rec;
                hd->callReady = true;
                hd->callHistory.push_back(*rec);
            }

            // PENTING: explicit destructor karena string/vector tidak di-manage
            rec->~CallRecord();
        }
    };

    Gum::RefPtr<Gum::Interceptor> interceptor;
    std::unordered_map<void*, std::unique_ptr<TraceListener>> traceListeners;

    void Init() { interceptor = Gum::Interceptor_obtain(); }

    bool Trace(MethodInfo *method, HookerData *data)
    {
        if (!method || !method->methodPointer || !data) return false;
        if (traceListeners.count(method->methodPointer))
        { LOGE("Already hooked %s", method->getName()); return false; }
        traceListeners[method->methodPointer] = std::make_unique<TraceListener>();
        bool ok = interceptor->attach(method->methodPointer,
                                      traceListeners[method->methodPointer].get(), data);
        if (!ok)
        { traceListeners.erase(method->methodPointer); LOGE("Failed to attach %s", method->getName()); }
        return ok;
    }

    bool Untrace(MethodInfo *method)
    {
        if (!method || !method->methodPointer) return false;
        auto it = traceListeners.find(method->methodPointer);
        if (it == traceListeners.end()) return false;
        interceptor->detach(it->second.get());
        traceListeners.erase(it);
        return true;
    }

    bool isTraced(MethodInfo *method)
    {
        if (!method || !method->methodPointer) return false;
        return traceListeners.count(method->methodPointer) > 0;
    }
} // namespace Frida
