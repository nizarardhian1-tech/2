// ============================================================================
// ESPManager.h  v8  —  PositionMode: FieldDirect / MethodInvoke / TransformChain
// ============================================================================
#pragma once

#include "../Il2cpp/Il2cpp.h"
#include "../Il2cpp/il2cpp-class.h"
#include "../Il2cpp/il2cpp-tabledefs.h"
#include "../Includes/Logger.h"
#include "../Includes/Utils.h"
#include "Util.h"
#include "CodeExporter.h"
#include "../imgui/imgui.h"
#include <functional>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdint>

extern void*        (*il2cpp_class_get_static_field_data)(Il2CppClass* klass);
extern void         (*il2cpp_field_static_get_value)(FieldInfo* field, void* value);
extern void         (*il2cpp_runtime_class_init)(Il2CppClass* klass);
extern Il2CppClass* (*il2cpp_class_get_element_class)(Il2CppClass* klass);

// ─── Types ────────────────────────────────────────────────────────────────────
struct Vec3      { float x = 0.f, y = 0.f, z = 0.f; };
struct Matrix4x4 { float m[16] = {}; };

enum class PositionMode : int { FieldDirect = 0, MethodInvoke = 1, TransformChain = 2 };

struct ESPPointerChain {
    std::string holderClass, holderNamespace, holderAssembly;
    std::string fieldName, fieldTypeName;
    uintptr_t   fieldOffset   = 0;
    uintptr_t   staticDataRVA = 0;
    bool        isList        = false;
    bool        hasLiveValue  = false;
};

struct ESPExportData {
    std::string  className, classNamespace, assemblyName;
    std::string  fieldName,  fieldType;
    std::string  methodName;
    uintptr_t    fieldOffset  = 0;
    uintptr_t    methodRVA    = 0;
    PositionMode posMode      = PositionMode::FieldDirect;
    uintptr_t    cameraViewMatrixOffset = 0;
    bool         viewMatrixFound        = false;
    std::vector<ESPPointerChain> pointerChains;
    bool valid = false;
};

// =============================================================================
class ESPManager
{
private:
    // ── Tracking state ────────────────────────────────────────────────────────
    Il2CppClass  *trackedClass_     = nullptr;
    FieldInfo    *trackedField_     = nullptr;   // FieldDirect only
    MethodInfo   *trackedMethod_    = nullptr;   // MethodInvoke only
    FieldInfo    *transformField_   = nullptr;   // TransformChain: field override
    MethodInfo   *getTransformMth_  = nullptr;   // TransformChain: cached
    MethodInfo   *getPositionMth_   = nullptr;   // TransformChain: cached
    PositionMode  posMode_          = PositionMode::FieldDirect;
    std::string   dispName_;
    Il2CppObject *localPlayer_      = nullptr;
    FieldInfo    *labelField_       = nullptr;   // custom label field for overlay

    // ── Scan state ────────────────────────────────────────────────────────────
    std::vector<Il2CppObject*> objects_, pendingObjects_;
    mutable std::mutex         bufMtx_;
    std::atomic<bool>          scanRunning_{false};
    std::atomic<bool>          hasPending_{false};
    std::atomic<bool>          stopFlag_{false};
    float lastScan_     = 0.f;
    float scanInterval_ = 3.f;
    bool  autoScan_     = true;

    // ── Camera state ──────────────────────────────────────────────────────────
    Il2CppObject *camera_     = nullptr;
    MethodInfo   *w2s_        = nullptr;
    MethodInfo   *camGetMain_ = nullptr;
    bool          camReady_   = false;
    int           camW_ = 0, camH_ = 0;

    // ── Render config ─────────────────────────────────────────────────────────
    size_t maxObj_     = 30;
    float  maxDist_    = 2000.f;
    bool   drawLines_  = true, drawBoxes_ = false, drawDist_ = true, drawNames_ = false;
    float  lineW_ = 1.5f, circleR_ = 5.f;
    int    colorMode_  = 0;
    ImU32  solidColor_ = IM_COL32(255, 50, 50, 255);
    int rendered_ = 0, culled_ = 0, total_ = 0;

    ESPExportData exportData_;

    // =========================================================================
    // Private helpers
    // =========================================================================

    static std::string MakeFullName(const std::string &ns, const std::string &name) {
        if (ns.empty()) return name;
        if (name.substr(0, ns.size() + 1) == ns + ".") return name;
        return ns + "." + name;
    }

    std::string ObjLabel(Il2CppObject *o) const {
        if (!o) return "";
        Il2CppClass *klass = nullptr;
        if (!SafeRead<Il2CppClass*>((const void*)o, &klass) || !klass || !IsPtrValid(klass)) return "";
        // Custom label field
        if (labelField_ && IsPtrValid(labelField_)) {
            try {
                uintptr_t off = labelField_->getOffset();
                auto *ft = labelField_->getType();
                if (ft) {
                    const char *tn = ft->getName();
                    if (tn && strstr(tn, "String")) {
                        Il2CppString *s = nullptr;
                        if (SafeRead<Il2CppString*>((const void*)((uintptr_t)o + off), &s) && s && IsPtrValid(s)) {
                            try { auto v = s->to_string(); if (!v.empty()) return v; } catch (...) {}
                        }
                    } else if (tn && (strstr(tn, "Int") || strstr(tn, "Single"))) {
                        char buf[32];
                        if (strstr(tn, "Single")) { float v = *(float*)((uintptr_t)o + off); snprintf(buf, 32, "%.1f", v); }
                        else { int32_t v = *(int32_t*)((uintptr_t)o + off); snprintf(buf, 32, "%d", v); }
                        return buf;
                    }
                }
            } catch (...) {}
        }
        // Auto name detection
        try {
            for (auto *f : klass->getFields()) {
                if (!f || !IsPtrValid(f)) continue;
                const char *fn = f->getName(); if (!fn) continue;
                std::string nl = fn;
                std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                if (nl=="name"||nl=="_name"||nl=="playername"||nl=="username"||nl=="heroname") {
                    Il2CppString *s = nullptr;
                    if (!SafeRead<Il2CppString*>((const void*)((uintptr_t)o + f->getOffset()), &s) || !s || !IsPtrValid(s)) continue;
                    try { auto v = s->to_string(); if (!v.empty()) return v; } catch (...) {}
                }
            }
            const char *cn = klass->getName();
            return cn ? std::string(cn) : "";
        } catch (...) { return ""; }
    }

    bool InitCam() {
        if (camReady_) return true;
        try {
            auto *cc = Il2cpp::FindClass("UnityEngine.Camera");
            if (!cc) return false;
            camGetMain_ = cc->getMethod("get_main", 0);
            if (!camGetMain_) return false;
            camera_ = cc->invoke_static_method<Il2CppObject*>("get_main");
            if (!camera_ || !IsPtrValid(camera_)) { camera_ = nullptr; return false; }
            for (auto *m : cc->getMethods("WorldToScreenPoint"))
                if (m && m->methodPointer && m->getParamsInfo().size()==1) { w2s_=m; break; }
            if (!w2s_) return false;
            try { camW_ = camera_->invoke_method<int>("get_pixelWidth");  } catch (...) { camW_=0; }
            try { camH_ = camera_->invoke_method<int>("get_pixelHeight"); } catch (...) { camH_=0; }
            if (camW_<=0) camW_ = (int)ImGui::GetIO().DisplaySize.x;
            if (camH_<=0) camH_ = (int)ImGui::GetIO().DisplaySize.y;
            camReady_ = true;
        } catch (...) { return false; }
        return true;
    }

    bool InitTransformMethods() {
        if (getPositionMth_) return true;
        try {
            auto *tc = Il2cpp::FindClass("UnityEngine.Transform");
            if (!tc) return false;
            getPositionMth_ = tc->getMethod("get_position", 0);
            if (!getPositionMth_) return false;
            if (!transformField_) {
                auto *cc = Il2cpp::FindClass("UnityEngine.Component");
                if (cc) getTransformMth_ = cc->getMethod("get_transform", 0);
            }
            return true;
        } catch (...) { return false; }
    }

    Vec3 CamPos() {
        if (!camera_ || !IsPtrValid(camera_)) return {};
        try {
            auto *tr = camera_->invoke_method<Il2CppObject*>("get_transform");
            if (!tr || !IsPtrValid(tr)) return {};
            return tr->invoke_method<Vec3>("get_position");
        } catch (...) { return {}; }
    }

    Il2CppObject* DetectLocal(const std::vector<Il2CppObject*> &insts) {
        if (!trackedClass_ || insts.empty()) return nullptr;
        static const char *KW[] = {
            "islocal","ismine","islocalplayer","bislocalplayer",
            "bismine","isself","bisself","isowner","bisowner", nullptr
        };
        try {
            for (auto *f : trackedClass_->getFields(true)) {
                if (!f || !IsPtrValid(f)) continue;
                auto *ft = f->getType();
                if (!ft || ft->type != IL2CPP_TYPE_BOOLEAN) continue;
                const char *fn = f->getName(); if (!fn) continue;
                std::string nl = fn;
                std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                bool match = false;
                for (int i=0; KW[i]; i++)
                    if (nl.find(KW[i])!=std::string::npos) { match=true; break; }
                if (!match) continue;
                uintptr_t off = f->getOffset();
                for (auto *inst : insts) {
                    if (!inst || !IsPtrValid(inst)) continue;
                    bool bv = false;
                    if (SafeRead<bool>((const void*)((uintptr_t)inst+off), &bv) && bv) return inst;
                }
            }
        } catch (...) {}
        return nullptr;
    }

    void DoScanOnMainThread() {
        if (stopFlag_.load()) { scanRunning_.store(false); return; }
        if (!trackedClass_)   { scanRunning_.store(false); return; }
        try {
            auto insts = Il2cpp::GC::FindObjects(trackedClass_);
            if (stopFlag_.load()) { scanRunning_.store(false); return; }
            if (!localPlayer_ && !insts.empty()) localPlayer_ = DetectLocal(insts);
            std::vector<Il2CppObject*> fresh;
            fresh.reserve(std::min(insts.size(), maxObj_));
            for (auto *inst : insts) {
                if (stopFlag_.load(std::memory_order_relaxed)) break;
                if (!inst || !IsPtrValid(inst)) continue;
                if (inst == localPlayer_) continue;
                if (fresh.size() >= maxObj_) break;
                fresh.push_back(inst);
            }
            { std::lock_guard<std::mutex> lk(bufMtx_); objects_ = std::move(fresh); }
        } catch (...) {}
        scanRunning_.store(false);
    }

    void TriggerScan() {
        if (!autoScan_ || scanRunning_.load()) return;
        float now = ImGui::GetTime();
        if (now - lastScan_ < scanInterval_) return;
        lastScan_ = now;
        scanRunning_.store(true);
        DoScanOnMainThread();
    }

    void SwapBuf() {
        if (!hasPending_.load()) return;
        std::lock_guard<std::mutex> lk(bufMtx_);
        if (!hasPending_.load()) return;
        objects_ = std::move(pendingObjects_);
        hasPending_.store(false);
    }

    void FindGlobalStaticInstances(uintptr_t il2cppBase) {
        if (!trackedClass_ || !il2cppBase) return;
        exportData_.pointerChains.clear();
        const char *tName_c = trackedClass_->getName();
        std::string tName = tName_c ? tName_c : "";
        if (tName.empty()) return;
        const size_t MAX_CHAINS = 15;
        const auto &images = Il2cpp::GetImages();
        for (auto *img : images) {
            if (!img || !IsPtrValid(img)) continue;
            if (exportData_.pointerChains.size() >= MAX_CHAINS) break;
            auto classes = Il2cpp::GetClasses(img);
            for (auto *klass : classes) {
                if (stopFlag_.load()) return;
                if (!klass || !IsPtrValid(klass) || klass == trackedClass_) continue;
                if (exportData_.pointerChains.size() >= MAX_CHAINS) break;
                std::vector<FieldInfo*> fields;
                try { fields = klass->getFields(false); } catch (...) { continue; }
                for (auto *sf : fields) {
                    if (!sf || !IsPtrValid(sf)) continue;
                    if (!(Il2cpp::GetFieldFlags(sf) & FIELD_ATTRIBUTE_STATIC)) continue;
                    auto *sft = sf->getType(); if (!sft) continue;
                    bool isSingleton = false, isList = false;
                    Il2CppClass *fieldKlass = nullptr;
                    try { fieldKlass = Il2cpp::GetClassFromType(sft); } catch (...) {}
                    if (fieldKlass) {
                        if (fieldKlass == trackedClass_) { isSingleton = true; }
                        else if (il2cpp_class_get_element_class) {
                            Il2CppClass *elemKlass = nullptr;
                            try { elemKlass = il2cpp_class_get_element_class(fieldKlass); } catch (...) {}
                            if (elemKlass == trackedClass_) { isSingleton = true; isList = true; }
                        }
                    }
                    if (!isSingleton) {
                        const char *sftn_c = sft->getName();
                        if (sftn_c) {
                            std::string sftn = sftn_c;
                            if (sftn.find(tName) != std::string::npos &&
                               (sftn.find("List") != std::string::npos ||
                                sftn.find("[]")   != std::string::npos ||
                                sftn.find("Array")!= std::string::npos ||
                                sftn.find("IList")!= std::string::npos))
                            { isSingleton = true; isList = true; }
                        }
                    }
                    if (!isSingleton) continue;
                    bool hasLiveValue = false;
                    if (il2cpp_field_static_get_value) {
                        try {
                            if (il2cpp_runtime_class_init) il2cpp_runtime_class_init(klass);
                            uintptr_t ptrVal = 0;
                            il2cpp_field_static_get_value(sf, &ptrVal);
                            if (ptrVal && IsPtrValid((const void*)ptrVal)) {
                                if (isList) { hasLiveValue = true; }
                                else {
                                    Il2CppClass *vtKlass = nullptr;
                                    if (SafeRead<Il2CppClass*>((const void*)ptrVal, &vtKlass))
                                        hasLiveValue = (vtKlass == trackedClass_);
                                }
                            }
                        } catch (...) {}
                    }
                    uintptr_t sdRVA = 0;
                    if (il2cpp_class_get_static_field_data) {
                        try {
                            void *sd = il2cpp_class_get_static_field_data(klass);
                            if (sd && IsPtrValid(sd)) {
                                uintptr_t abs = (uintptr_t)sd;
                                if (abs > il2cppBase) sdRVA = abs - il2cppBase;
                            }
                        } catch (...) {}
                    }
                    const char *sftn_c = sft->getName();
                    ESPPointerChain chain;
                    chain.holderClass     = klass->getName()      ? klass->getName()      : "?";
                    chain.holderNamespace = klass->getNamespace() ? klass->getNamespace() : "";
                    chain.holderAssembly  = klass->getImage()     ? klass->getImage()->getName() : "?";
                    chain.fieldName       = sf->getName()         ? sf->getName()         : "?";
                    chain.fieldTypeName   = sftn_c ? sftn_c : "?";
                    chain.fieldOffset     = sf->getOffset();
                    chain.staticDataRVA   = sdRVA;
                    chain.isList          = isList;
                    chain.hasLiveValue    = hasLiveValue;
                    exportData_.pointerChains.push_back(chain);
                }
            }
        }
    }

    uintptr_t FindCameraMatrixOffset() {
        try {
            auto *cc = Il2cpp::FindClass("UnityEngine.Camera");
            if (!cc) return 0;
            Il2CppObject *cam = cc->invoke_static_method<Il2CppObject*>("get_main");
            if (!cam || !IsPtrValid(cam)) return 0;
            uintptr_t nativeCam = 0;
            if (!SafeRead<uintptr_t>((const void*)((uintptr_t)cam + 0x10), &nativeCam)) return 0;
            if (!nativeCam || !IsPtrValid((const void*)nativeCam)) return 0;
            Matrix4x4 refMat{}; bool gotRef = false;
            auto *mProj = cc->getMethod("get_projectionMatrix", 0);
            if (mProj && mProj->methodPointer) {
                try {
                    refMat = mProj->invoke_static<Matrix4x4>(cam);
                    float m0=refMat.m[0], m5=refMat.m[5], m10=refMat.m[10];
                    if (m0==m0 && m5==m5 && m10==m10 &&
                        fabsf(m0)>0.1f && fabsf(m0)<1000.f &&
                        fabsf(m5)>0.1f && fabsf(m5)<1000.f && m10!=0.f) gotRef = true;
                } catch (...) {}
            }
            if (!gotRef) {
                auto *mC2W = cc->getMethod("get_cameraToWorldMatrix", 0);
                if (mC2W && mC2W->methodPointer) {
                    try {
                        refMat = mC2W->invoke_static<Matrix4x4>(cam);
                        int nz=0;
                        for (int i=0;i<16;i++) if(refMat.m[i]==refMat.m[i]&&fabsf(refMat.m[i])>0.001f) nz++;
                        if (nz>=6) gotRef=true;
                    } catch (...) {}
                }
            }
            if (!gotRef) return 0;
            for (uintptr_t off=0; off+sizeof(Matrix4x4)<=0x600; off+=4) {
                uintptr_t addr = nativeCam+off;
                if (!IsPtrValid((const void*)addr)) { off+=4091; continue; }
                if (!IsPtrValid((const void*)(addr+sizeof(Matrix4x4)-1))) continue;
                Matrix4x4 cand{};
                if (!SafeRead<Matrix4x4>((const void*)addr, &cand)) continue;
                int matched=0, nonZero=0; bool hasNaN=false;
                for (int i=0;i<16;i++) {
                    if(cand.m[i]!=cand.m[i]){hasNaN=true;break;}
                    if(fabsf(cand.m[i])>0.0001f) nonZero++;
                    if(fabsf(cand.m[i]-refMat.m[i])<0.005f) matched++;
                }
                if (!hasNaN && matched>=12 && nonZero>=4) return off;
            }
        } catch (...) {}
        return 0;
    }

    ESPPointerChain FindCameraChain(uintptr_t il2cppBase) {
        ESPPointerChain result;
        auto *camClass = Il2cpp::FindClass("UnityEngine.Camera");
        if (!camClass || !il2cppBase) return result;
        const auto &images = Il2cpp::GetImages();
        for (auto *img : images) {
            if (!img || !IsPtrValid(img)) continue;
            auto classes = Il2cpp::GetClasses(img);
            for (auto *klass : classes) {
                if (!klass || !IsPtrValid(klass) || klass == camClass) continue;
                std::vector<FieldInfo*> fields;
                try { fields = klass->getFields(false); } catch (...) { continue; }
                for (auto *sf : fields) {
                    if (!sf || !IsPtrValid(sf)) continue;
                    if (!(Il2cpp::GetFieldFlags(sf) & FIELD_ATTRIBUTE_STATIC)) continue;
                    auto *sft = sf->getType(); if (!sft) continue;
                    Il2CppClass *fieldKlass = nullptr;
                    try { fieldKlass = Il2cpp::GetClassFromType(sft); } catch (...) {}
                    if (fieldKlass != camClass) continue;
                    bool hasLive = false;
                    if (il2cpp_field_static_get_value) {
                        try {
                            if (il2cpp_runtime_class_init) il2cpp_runtime_class_init(klass);
                            uintptr_t ptrVal = 0;
                            il2cpp_field_static_get_value(sf, &ptrVal);
                            if (ptrVal && IsPtrValid((const void*)ptrVal)) {
                                Il2CppClass *vt = nullptr;
                                if (SafeRead<Il2CppClass*>((const void*)ptrVal, &vt))
                                    hasLive = (vt == camClass);
                            }
                        } catch (...) {}
                    }
                    uintptr_t sdRVA = 0;
                    if (il2cpp_class_get_static_field_data) {
                        try {
                            void *sd = il2cpp_class_get_static_field_data(klass);
                            if (sd && IsPtrValid(sd)) {
                                uintptr_t abs = (uintptr_t)sd;
                                if (abs > il2cppBase) sdRVA = abs - il2cppBase;
                            }
                        } catch (...) {}
                    }
                    result.holderClass     = klass->getName()      ? klass->getName()      : "?";
                    result.holderNamespace = klass->getNamespace() ? klass->getNamespace() : "";
                    result.holderAssembly  = klass->getImage()     ? klass->getImage()->getName() : "?";
                    result.fieldName       = sf->getName()         ? sf->getName()         : "?";
                    result.fieldTypeName   = "UnityEngine.Camera";
                    result.fieldOffset     = sf->getOffset();
                    result.staticDataRVA   = sdRVA;
                    result.isList          = false;
                    result.hasLiveValue    = hasLive;
                    return result;
                }
            }
        }
        return result;
    }

    void ResetCamState() {
        camReady_=false; camera_=nullptr; w2s_=nullptr; camGetMain_=nullptr; camW_=camH_=0;
    }

    void InitExportData() {
        const char *cn  = trackedClass_->getName()      ? trackedClass_->getName()      : "?";
        const char *ns  = trackedClass_->getNamespace() ? trackedClass_->getNamespace() : "";
        const char *asm_= trackedClass_->getImage()     ? trackedClass_->getImage()->getName() : "";
        exportData_ = {};
        exportData_.className      = cn;
        exportData_.classNamespace = ns;
        exportData_.assemblyName   = asm_;
        exportData_.posMode        = posMode_;
        exportData_.valid          = true;
    }

public:
    ESPManager()  = default;
    ~ESPManager() { StopTracking(); }

    // ── Start Tracking: FieldDirect ───────────────────────────────────────────
    void StartTracking(Il2CppClass *klass, FieldInfo *field) {
        if (!klass || !field) return;
        StopTracking();
        stopFlag_.store(false);
        trackedClass_  = klass;
        trackedField_  = field;
        posMode_       = PositionMode::FieldDirect;
        dispName_      = field->getName() ? field->getName() : "?";
        localPlayer_   = nullptr;
        InitExportData();
        exportData_.fieldName   = dispName_;
        auto *ft = field->getType();
        exportData_.fieldType   = (ft && ft->getName()) ? ft->getName() : "?";
        exportData_.fieldOffset = field->getOffset();
        { std::lock_guard<std::mutex> lk(bufMtx_); objects_.clear(); pendingObjects_.clear(); }
        hasPending_.store(false);
        lastScan_ = -scanInterval_;
    }

    // ── Start Tracking: MethodInvoke ─────────────────────────────────────────
    void StartTrackingMethod(Il2CppClass *klass, MethodInfo *method) {
        if (!klass || !method) return;
        StopTracking();
        stopFlag_.store(false);
        trackedClass_  = klass;
        trackedMethod_ = method;
        posMode_       = PositionMode::MethodInvoke;
        dispName_      = (method->getName() ? std::string(method->getName()) : "?") + "()";
        localPlayer_   = nullptr;
        InitExportData();
        exportData_.methodName = method->getName() ? method->getName() : "?";
        uintptr_t base = GetLibBase(GetTargetLib());
        if (base && method->methodPointer)
            exportData_.methodRVA = (uintptr_t)method->methodPointer - base;
        { std::lock_guard<std::mutex> lk(bufMtx_); objects_.clear(); pendingObjects_.clear(); }
        hasPending_.store(false);
        lastScan_ = -scanInterval_;
    }

    // ── Start Tracking: TransformChain ───────────────────────────────────────
    // trField: optional — if null, uses inherited get_transform() via Component
    void StartTrackingTransform(Il2CppClass *klass, FieldInfo *trField = nullptr) {
        if (!klass) return;
        StopTracking();
        stopFlag_.store(false);
        trackedClass_   = klass;
        transformField_ = trField;
        posMode_        = PositionMode::TransformChain;
        dispName_       = trField ? (std::string(trField->getName() ? trField->getName() : "?") + "→pos")
                                  : "Transform→pos";
        localPlayer_    = nullptr;
        getTransformMth_ = nullptr;
        getPositionMth_  = nullptr;
        InitExportData();
        if (trField) {
            exportData_.fieldName   = trField->getName() ? trField->getName() : "?";
            exportData_.fieldOffset = trField->getOffset();
            auto *ft = trField->getType();
            exportData_.fieldType   = (ft && ft->getName()) ? ft->getName() : "?";
        }
        { std::lock_guard<std::mutex> lk(bufMtx_); objects_.clear(); pendingObjects_.clear(); }
        hasPending_.store(false);
        lastScan_ = -scanInterval_;
    }

    void StopTracking() {
        stopFlag_.store(true);
        for (int w=0; scanRunning_.load() && w<60; w++)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        trackedClass_=nullptr; trackedField_=nullptr; trackedMethod_=nullptr;
        transformField_=nullptr; getTransformMth_=nullptr; getPositionMth_=nullptr;
        localPlayer_=nullptr;
        { std::lock_guard<std::mutex> lk(bufMtx_); objects_.clear(); pendingObjects_.clear(); }
        hasPending_.store(false);
        ResetCamState();
        rendered_=culled_=total_=0;
        exportData_.valid=false;
        dispName_.clear();
        labelField_=nullptr;
    }

    void TriggerManualScan() {
        if (scanRunning_.load()) return;
        lastScan_ = ImGui::GetTime();
        scanRunning_.store(true);
        DoScanOnMainThread();
    }

    void Tick() {
        if (!trackedClass_) return;
        TriggerScan();
    }

    // =========================================================================
    // Render — 3 position modes
    // =========================================================================
    void Render(std::function<bool(Il2CppObject*)> filterFn = nullptr) {
        if (!trackedClass_) return;
        if (posMode_ == PositionMode::TransformChain && !InitTransformMethods()) return;
        SwapBuf();
        std::vector<Il2CppObject*> snap;
        { std::lock_guard<std::mutex> lk(bufMtx_); snap = objects_; }
        if (snap.empty()) return;
        if (!camReady_ && !InitCam()) return;
        auto *dl = ImGui::GetBackgroundDrawList(); if (!dl) return;

        float sw = ImGui::GetIO().DisplaySize.x, sh = ImGui::GetIO().DisplaySize.y;
        float sx = camW_>0 ? sw/camW_ : 1.f, sy = camH_>0 ? sh/camH_ : 1.f;
        ImVec2 center(sw*0.5f, sh);
        Vec3 cp = CamPos();
        rendered_=culled_=total_=0;

        for (auto *obj : snap) {
            if (!obj || stopFlag_.load(std::memory_order_relaxed)) break;
            if (rendered_ >= (int)maxObj_) break;
            total_++;
            if (!IsPtrValid(obj)) { culled_++; continue; }
            Il2CppClass *ok = nullptr;
            if (!SafeRead<Il2CppClass*>((const void*)obj, &ok) || !ok || !IsPtrValid(ok)) { culled_++; continue; }
            if (filterFn) { try { if (!filterFn(obj)) { culled_++; continue; } } catch (...) { culled_++; continue; } }

            // ── Get position based on mode ────────────────────────────────────
            Vec3 pos{};
            bool gotPos = false;

            switch (posMode_) {
                case PositionMode::FieldDirect:
                    if (!trackedField_) { culled_++; continue; }
                    gotPos = SafeRead<Vec3>((const void*)((uintptr_t)obj + trackedField_->getOffset()), &pos);
                    break;

                case PositionMode::MethodInvoke:
                    if (!trackedMethod_) { culled_++; continue; }
                    try { pos = trackedMethod_->invoke_static<Vec3>(obj); gotPos = true; }
                    catch (...) {}
                    break;

                case PositionMode::TransformChain: {
                    try {
                        Il2CppObject *tr = nullptr;
                        if (transformField_) {
                            SafeRead<Il2CppObject*>((const void*)((uintptr_t)obj + transformField_->getOffset()), &tr);
                        } else if (getTransformMth_) {
                            tr = getTransformMth_->invoke_static<Il2CppObject*>(obj);
                        }
                        if (tr && IsPtrValid(tr) && getPositionMth_) {
                            pos = getPositionMth_->invoke_static<Vec3>(tr);
                            gotPos = true;
                        }
                    } catch (...) {}
                    break;
                }
            }

            if (!gotPos) { culled_++; continue; }
            if (pos.x!=pos.x || pos.y!=pos.y || pos.z!=pos.z) { culled_++; continue; }

            float dx=pos.x-cp.x, dy=pos.y-cp.y, dz=pos.z-cp.z;
            float dist = sqrtf(dx*dx + dy*dy + dz*dz);
            if (dist > maxDist_) { culled_++; continue; }
            if (!IsPtrValid(camera_)) { camReady_=false; camera_=nullptr; break; }

            Vec3 s{};
            try { s = w2s_->invoke_static<Vec3>(camera_, pos); } catch (...) { culled_++; continue; }
            if (s.z <= 0.1f) { culled_++; continue; }

            float fx = s.x*sx, fy = sh - (s.y*sy);
            bool offscreen = (fx<0||fx>sw||fy<0||fy>sh);
            if (offscreen) { fx=std::max(8.f,std::min(fx,sw-8)); fy=std::max(8.f,std::min(fy,sh-8)); }
            ImVec2 p(fx, fy);

            ImU32 col;
            if (colorMode_==0) { float t=std::min(dist/maxDist_,1.f); col=IM_COL32((int)(t*255),(int)((1-t)*255),80,255); }
            else col = solidColor_;

            if (drawLines_)  dl->AddLine(center, p, col, lineW_);
            dl->AddCircleFilled(p, circleR_, col);
            if (offscreen)   dl->AddRect(ImVec2(p.x-circleR_-2,p.y-circleR_-2), ImVec2(p.x+circleR_+2,p.y+circleR_+2), IM_COL32(255,200,0,180), 2.f);
            if (drawBoxes_)  dl->AddRect(ImVec2(p.x-15,p.y-25), ImVec2(p.x+15,p.y+5), col, 0, 0, 1.5f);
            if (drawDist_) {
                char b[16]; snprintf(b, sizeof(b), "%.0fm", dist);
                dl->AddText(ImVec2(p.x+circleR_+2, p.y-8), col, b);
            }
            if (drawNames_) {
                try {
                    std::string lb = ObjLabel(obj);
                    if (!lb.empty())
                        dl->AddText(ImVec2(p.x+circleR_+2, p.y-8-ImGui::GetTextLineHeight()), IM_COL32(255,255,255,220), lb.c_str());
                } catch (...) {}
            }
            rendered_++;
        }
    }

    // =========================================================================
    // ExportAll — clean single-namespace output
    // =========================================================================
    bool ExportAll(const std::string &extraConditions = "") {
        if (!exportData_.valid) return false;
        const auto &d = exportData_;

        uintptr_t il2cppBase = GetLibBase(GetTargetLib());
        auto toRVA = [&](uintptr_t abs) -> uintptr_t {
            return (il2cppBase && abs > il2cppBase) ? (abs - il2cppBase) : 0;
        };
        auto getRVA = [&](const char *cls, const char *mth, int argc) -> uintptr_t {
            auto *c = Il2cpp::FindClass(cls); if (!c) return 0;
            auto *m = c->getMethod(mth, argc);
            return (m && m->methodPointer) ? toRVA((uintptr_t)m->methodPointer) : 0;
        };

        uintptr_t rvaCamMain  = getRVA("UnityEngine.Camera",     "get_main",       0);
        uintptr_t rvaGetTrans = getRVA("UnityEngine.Component",  "get_transform",  0);
        uintptr_t rvaGetPos   = getRVA("UnityEngine.Transform",  "get_position",   0);
        uintptr_t rvaObjName  = getRVA("UnityEngine.Object",     "get_name",       0);
        uintptr_t rvaActive   = getRVA("UnityEngine.GameObject", "get_activeSelf", 0);
        uintptr_t rvaW2S=0, rvaW2S2=0;
        {
            auto *cc = Il2cpp::FindClass("UnityEngine.Camera");
            if (cc) for (auto *m : cc->getMethods("WorldToScreenPoint")) {
                if (!m || !m->methodPointer) continue;
                size_t ac = m->getParamsInfo().size();
                if (ac==1 && !rvaW2S)  rvaW2S  = toRVA((uintptr_t)m->methodPointer);
                if (ac==2 && !rvaW2S2) rvaW2S2 = toRVA((uintptr_t)m->methodPointer);
            }
        }

        FindGlobalStaticInstances(il2cppBase);
        uintptr_t camMatOff = FindCameraMatrixOffset();
        exportData_.cameraViewMatrixOffset = camMatOff;
        exportData_.viewMatrixFound        = (camMatOff > 0);
        ESPPointerChain camChain = FindCameraChain(il2cppBase);

        std::string safeClass = d.className;
        for (char &c : safeClass) if (!isalnum(c)) c = '_';
        std::string fullCls = MakeFullName(d.classNamespace, d.className);

        const char *modeStr[] = { "FieldDirect", "MethodInvoke", "TransformChain" };

        std::ostringstream o;
        o << std::hex;

        // ── Header ────────────────────────────────────────────────────────────
        o << "// Auto-generated — IL2CPP ESP Tool\n";
        o << "// Target : " << fullCls << "  (" << d.assemblyName << ")\n";
        o << "// Mode   : " << modeStr[(int)d.posMode];
        switch (d.posMode) {
            case PositionMode::FieldDirect:
                o << "  →  " << d.fieldName << " [" << d.fieldType << "] @ 0x" << d.fieldOffset;
                break;
            case PositionMode::MethodInvoke:
                if (d.methodRVA) o << "  →  " << d.methodName << "()  RVA=0x" << d.methodRVA;
                else             o << "  →  " << d.methodName << "()";
                break;
            case PositionMode::TransformChain:
                o << "  →  get_transform() → get_position()";
                break;
        }
        o << "\n\n#pragma once\n#include <cstdint>\n\nnamespace ESP {\n\n";

        // ── Position ──────────────────────────────────────────────────────────
        o << "// ── Position\n";
        switch (d.posMode) {
            case PositionMode::FieldDirect:
                o << "constexpr uintptr_t PosOffset      = 0x" << d.fieldOffset
                  << ";  // " << d.fieldName << " [" << d.fieldType << "]\n";
                break;
            case PositionMode::MethodInvoke:
                if (d.methodRVA)
                    o << "constexpr uintptr_t RVA_PosMethod  = 0x" << d.methodRVA
                      << ";  // " << d.methodName << "() → Vector3\n";
                break;
            case PositionMode::TransformChain:
                if (rvaGetTrans) o << "constexpr uintptr_t RVA_GetTransform = 0x" << rvaGetTrans << ";\n";
                if (rvaGetPos)   o << "constexpr uintptr_t RVA_GetPosition  = 0x" << rvaGetPos   << ";\n";
                if (d.fieldOffset && !d.fieldName.empty())
                    o << "constexpr uintptr_t TransformFieldOffset = 0x" << d.fieldOffset
                      << ";  // " << d.fieldName << " [" << d.fieldType << "]\n";
                break;
        }
        o << "\n";

        // ── Camera ────────────────────────────────────────────────────────────
        o << "// ── Camera\n";
        if (rvaCamMain) o << "constexpr uintptr_t RVA_Camera_GetMain = 0x" << rvaCamMain << ";\n";
        if (rvaW2S)     o << "constexpr uintptr_t RVA_Camera_W2S     = 0x" << rvaW2S     << ";\n";
        if (rvaW2S2)    o << "constexpr uintptr_t RVA_Camera_W2S_v2  = 0x" << rvaW2S2    << ";  // Eye overload\n";
        if (camMatOff)  o << "constexpr uintptr_t CamNativeOffset     = 0x10;  // camObj → nativeCamPtr\n"
                          << "constexpr uintptr_t CamViewMatrixOffset = 0x" << camMatOff << ";  // nativeCam → float[16]\n";
        else            o << "// CamViewMatrixOffset: not detected — try 0xDC / 0x2E4 / 0x314\n";
        o << "\n";

        // ── Core RVAs ─────────────────────────────────────────────────────────
        o << "// ── Core RVAs\n";
        if (rvaGetTrans && d.posMode != PositionMode::TransformChain)
            o << "constexpr uintptr_t RVA_GetTransform = 0x" << rvaGetTrans << ";\n";
        if (rvaGetPos && d.posMode != PositionMode::TransformChain)
            o << "constexpr uintptr_t RVA_GetPosition  = 0x" << rvaGetPos   << ";\n";
        if (rvaObjName) o << "constexpr uintptr_t RVA_GetName      = 0x" << rvaObjName << ";\n";
        if (rvaActive)  o << "constexpr uintptr_t RVA_ActiveSelf   = 0x" << rvaActive  << ";\n";
        o << "\n";

        // ── Camera Chain ──────────────────────────────────────────────────────
        if (!camChain.holderClass.empty() && camChain.holderClass != "?") {
            std::string hFull = MakeFullName(camChain.holderNamespace, camChain.holderClass);
            o << "// ── Camera Chain : " << hFull << "::" << camChain.fieldName;
            if (camChain.hasLiveValue) o << "  (LIVE)";
            o << "\n";
            if (camChain.staticDataRVA)
                o << "constexpr uintptr_t CamChain_Static = 0x" << camChain.staticDataRVA << ";\n";
            o << "constexpr uintptr_t CamChain_Offset = 0x" << camChain.fieldOffset << ";\n\n";
        }

        // ── Conditions ────────────────────────────────────────────────────────
        if (!extraConditions.empty()) {
            o << "// ── Conditions\n" << extraConditions << "\n";
        }

        // ── Object Chains ─────────────────────────────────────────────────────
        if (exportData_.pointerChains.empty()) {
            o << "// ── No static chains found for " << d.className
              << " — use GC scan or find Manager/List<" << d.className << "> in dump.cs\n\n";
        } else {
            int idx = 0;
            for (const auto &ch : exportData_.pointerChains) {
                idx++;
                std::string hFull = MakeFullName(ch.holderNamespace, ch.holderClass);
                o << "// ── Chain #" << std::dec << idx << " : " << hFull << "::" << ch.fieldName;
                if (ch.hasLiveValue) o << "  (LIVE)";
                if (ch.isList)       o << "  [LIST]";
                o << "\n";
                if (ch.staticDataRVA)
                    o << "constexpr uintptr_t Chain" << idx << "_Static = 0x" << std::hex << ch.staticDataRVA << ";\n";
                o << "constexpr uintptr_t Chain" << idx << "_Offset = 0x" << std::hex << ch.fieldOffset << ";\n";
                if (ch.isList)
                    o << "// List → items=*(ptr+0x10), count=*(int*)(ptr+0x18), obj[i]@items+0x20+i*8\n";
                o << "\n";
            }
        }

        o << "} // namespace ESP\n";

        std::string fileName = safeClass + "_esp.h";
        const std::string outPath = Util::DirESP() + "/" + fileName;
        CodeExporter::SaveFile(o.str(), outPath);
        LOGI("ESP ExportAll: %s (mode=%s chains=%zu camMat=%s)",
             outPath.c_str(), modeStr[(int)d.posMode],
             exportData_.pointerChains.size(),
             exportData_.viewMatrixFound ? "YES" : "NO");
        return true;
    }

    // =========================================================================
    // Getters / Setters
    // =========================================================================
    void SetMaxObjects(size_t v)      { maxObj_=v; }
    void SetMaxDistance(float v)      { maxDist_=v; }
    void SetUpdateInterval(float v)   { scanInterval_=v<2.f?2.f:v; }
    void SetAutoScan(bool v)          { autoScan_=v; if(v) lastScan_=-scanInterval_; }
    bool GetAutoScan()          const { return autoScan_; }
    void SetDrawLines(bool v)         { drawLines_=v; }
    void SetDrawBoxes(bool v)         { drawBoxes_=v; }
    void SetDrawDistance(bool v)      { drawDist_=v; }
    void SetDrawNames(bool v)         { drawNames_=v; }
    void SetLineWidth(float v)        { lineW_=v; }
    void SetCircleRadius(float v)     { circleR_=v; }
    void SetColorMode(int v)          { colorMode_=v; }
    void SetCustomColor(ImU32 v)      { solidColor_=v; }
    void SetLabelField(FieldInfo *f)  { labelField_=f; }

    PositionMode  GetPosMode()          const { return posMode_; }
    std::string   GetTrackedDispName()  const { return dispName_; }
    bool          IsTracking()          const { return trackedClass_ != nullptr; }
    Il2CppClass*  GetTrackedClass()     const { return trackedClass_; }
    FieldInfo*    GetTrackedField()     const { return trackedField_; }
    MethodInfo*   GetTrackedMethod()    const { return trackedMethod_; }
    bool          IsScanRunning()       const { return scanRunning_.load(); }
    int           GetRenderedCount()    const { return rendered_; }
    int           GetCulledCount()      const { return culled_; }
    int           GetTotalCount()       const { return total_; }
    size_t        GetObjectCount()      const { return objects_.size(); }
    FieldInfo*    GetLabelField()       const { return labelField_; }
    const ESPExportData& GetExportData()const { return exportData_; }

    Il2CppObject* GetFirstObject() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(bufMtx_));
        return objects_.empty() ? nullptr : objects_[0];
    }
};
