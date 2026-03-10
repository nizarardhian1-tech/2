// ============================================================================
// View_FieldPatcher.cpp
// Pilar 3: Dipecah dari ClassesTab.cpp (4479 baris)
// Field Patcher — FieldPatcherView, FieldViewer, CSharpToCppType helpers
// ============================================================================
#include "ClassesTab_Shared.h"

// ── Helper: type mapping ─────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Helper: convert C# type name → C++ type string
// ─────────────────────────────────────────────────────────────────────────────
static const char *CSharpToCppType(const char *tn)
{
    if (!tn) return "uint8_t";
    if (strcmp(tn, "System.Boolean") == 0) return "bool";
    if (strcmp(tn, "System.Int16")   == 0) return "int16_t";
    if (strcmp(tn, "System.UInt16")  == 0) return "uint16_t";
    if (strcmp(tn, "System.Int32")   == 0) return "int32_t";
    if (strcmp(tn, "System.UInt32")  == 0) return "uint32_t";
    if (strcmp(tn, "System.Int64")   == 0) return "int64_t";
    if (strcmp(tn, "System.UInt64")  == 0) return "uint64_t";
    if (strcmp(tn, "System.Single")  == 0) return "float";
    if (strcmp(tn, "System.Double")  == 0) return "double";
    return "int32_t";
}

// ── Skor prioritas method untuk hook ─────────────────────────────────────────
// Makin tinggi skor = makin bagus jadi hook entry point
// Kriteria: dipanggil tiap frame (Update), punya 0 param, bukan ctor/special
static int HookMethodScore(MethodInfo *m)
{
    if (!m || !m->methodPointer) return -1;
    const char *n = m->getName();
    if (!n) return -1;
    // Skip ctor, cctor, special
    if (n[0] == '.') return -1;
    if (strncmp(n, "get_", 4) == 0 || strncmp(n, "set_", 4) == 0) return 0;

    int score = 1;
    // Bonus: dipanggil tiap frame
    if (strcmp(n, "Update")       == 0) score = 100;
    else if (strcmp(n, "FixedUpdate")  == 0) score = 90;
    else if (strcmp(n, "LateUpdate")   == 0) score = 85;
    else if (strcmp(n, "ManagedUpdate")== 0) score = 80;
    else if (strcmp(n, "Tick")         == 0) score = 75;
    else if (strcmp(n, "OnUpdate")     == 0) score = 75;
    else if (strstr(n, "Update")       != nullptr) score = 60;
    // Bonus: 0 parameter (thiz-only) = lebih aman di-hook
    auto params = m->getParamsInfo();
    if (params.empty()) score += 5;
    return score;
}

// Kembalikan daftar method yang layak jadi hook, diurutkan score tertinggi dulu
static std::vector<MethodInfo*> GetHookCandidates(Il2CppClass *klass)
{
    if (!klass) return {};
    auto allMethods = klass->getMethods();
    std::vector<std::pair<int,MethodInfo*>> scored;
    for (auto *m : allMethods) {
        int s = HookMethodScore(m);
        if (s >= 0) scored.push_back({s, m});
    }
    std::sort(scored.begin(), scored.end(), [](auto &a, auto &b){ return a.first > b.first; });
    std::vector<MethodInfo*> result;
    result.reserve(scored.size());
    for (auto &[s,m] : scored) result.push_back(m);
    return result;
}


// ── Generate .h ──────────────────────────────────────────────────────────────
static std::string GenerateFieldHeader(Il2CppClass *klass,
    const std::vector<std::pair<FieldInfo*, ClassesTab::FieldPatchData*>> &selected,
    MethodInfo *hookMethod)
{
    if (!klass || selected.empty()) return "";

    uintptr_t il2cppBase = (uintptr_t)GetLibBase(GetTargetLib());
    std::string className = klass->getName() ? klass->getName() : "Unknown";
    std::string fullName  = klass->getFullName();
    std::string safe = fullName;
    for (char &c : safe) if (c=='.'||c=='/'||c=='<'||c=='>') c='_';
    std::string MACRO = safe;
    for (char &c : MACRO) c = toupper(c);

    uintptr_t hookRVA = 0;
    std::string hookName = "Update";
    if (hookMethod && hookMethod->methodPointer && il2cppBase &&
        (uintptr_t)hookMethod->methodPointer > il2cppBase)
    {
        hookRVA  = (uintptr_t)hookMethod->methodPointer - il2cppBase;
        hookName = hookMethod->getName() ? hookMethod->getName() : "Update";
    }

    std::ostringstream h;
    h << "// ================================================================\n";
    h << "// Field Patch — " << fullName << "\n";
    h << "// Assembly : " << (klass->getImage() ? klass->getImage()->getName() : "?") << "\n";
    h << "// Hook     : " << className << "::" << hookName;
    if (hookRVA) h << "  (RVA 0x" << std::hex << hookRVA << std::dec << ")";
    h << "\n";
    h << "// ================================================================\n";
    h << "#pragma once\n#include <cstdint>\n";
    h << "#include \"Dobby/include/dobby.h\"\n";
    h << "#include \"Il2cpp/Il2cpp.h\"\n\n";

    // Offsets
    h << "// ── Field Offsets ────────────────────────────────────────────────\n";
    for (auto &[f,pd] : selected) {
        if (!f) continue;
        std::string fn = f->getName() ? f->getName() : "f";
        std::string FN = fn; for (char &c:FN) c=toupper(c);
        auto *ft = f->getType();
        bool isSt = (Il2cpp::GetFieldFlags(f) & FIELD_ATTRIBUTE_STATIC) != 0;
        h << "#define OFFSET_" << MACRO << "_" << FN
          << "  0x" << std::hex << f->getOffset() << std::dec
          << "  // " << (isSt?"static ":"") << (ft?ft->getName():"?") << " " << fn
          << "  =  " << pd->text << "\n";
    }

    // Patch values
    h << "\n// ── Patch Values ─────────────────────────────────────────────────\n";
    for (auto &[f,pd] : selected) {
        if (!f) continue;
        std::string fn = f->getName() ? f->getName() : "f";
        std::string FN = fn; for (char &c:FN) c=toupper(c);
        auto *ft = f->getType();
        const char *cppType = CSharpToCppType(ft ? ft->getName() : nullptr);
        h << "#define PATCH_" << MACRO << "_" << FN
          << "  ((" << cppType << ")" << pd->text << ")\n";
    }

    // Hook RVA
    if (hookRVA) {
        h << "\n// ── Hook RVA ─────────────────────────────────────────────────────\n";
        h << "#define RVA_" << MACRO << "_HOOK  0x" << std::hex << hookRVA << std::dec << "\n";
    }

    // Namespace
    h << "\n// ── Implementation ───────────────────────────────────────────────\n";
    h << "namespace FieldPatch_" << className << "\n{\n";

    if (hookRVA) {
        h << "    static void (*orig_" << hookName << ")(void *thiz);\n\n";
        h << "    static void hook_" << hookName << "(void *thiz)\n    {\n";
        h << "        orig_" << hookName << "(thiz);\n";
        h << "        if (!thiz) return;\n";
        h << "        auto inst = (uintptr_t)thiz;\n\n";
        for (auto &[f,pd] : selected) {
            if (!f || (Il2cpp::GetFieldFlags(f) & FIELD_ATTRIBUTE_STATIC)) continue;
            std::string fn = f->getName() ? f->getName() : "f";
            std::string FN = fn; for (char &c:FN) c=toupper(c);
            auto *ft = f->getType();
            const char *cppType = CSharpToCppType(ft?ft->getName():nullptr);
            h << "        // " << fn << " = " << pd->text << "\n";
            h << "        *(" << cppType << "*)(inst + OFFSET_" << MACRO << "_" << FN
              << ") = PATCH_" << MACRO << "_" << FN << ";\n";
        }
        h << "    }\n\n";
    }

    // Static fields
    bool hasStatic = false;
    for (auto &[f,pd]:selected) if(f&&(Il2cpp::GetFieldFlags(f)&FIELD_ATTRIBUTE_STATIC)){hasStatic=true;break;}
    if (hasStatic) {
        h << "    static void PatchStatic()\n    {\n";
        for (auto &[f,pd]:selected) {
            if (!f||!(Il2cpp::GetFieldFlags(f)&FIELD_ATTRIBUTE_STATIC)) continue;
            std::string fn=f->getName()?f->getName():"f";
            std::string FN=fn; for(char&c:FN)c=toupper(c);
            auto *ft=f->getType();
            const char *cpp=CSharpToCppType(ft?ft->getName():nullptr);
            h << "        auto v_"<<fn<<" = PATCH_"<<MACRO<<"_"<<FN<<";\n";
            h << "        Il2cpp::FindClass(\""<<fullName<<"\")\n";
            h << "            ->getField(\""<<fn<<"\")->setStaticValue(&v_"<<fn<<");\n";
        }
        h << "    }\n\n";
    }

    h << "    static void Setup()\n    {\n";
    if (hasStatic) h << "        PatchStatic();\n";
    if (hookRVA) {
        h << "        auto base = (uintptr_t)findLibrary(\"libil2cpp.so\");\n";
        h << "        DobbyHook((void*)(base + RVA_" << MACRO << "_HOOK),\n";
        h << "                  (void*)hook_" << hookName << ",\n";
        h << "                  (void**)&orig_" << hookName << ");\n";
    }
    h << "    }\n";
    h << "} // namespace FieldPatch_" << className << "\n\n";
    h << "// ── Usage ────────────────────────────────────────────────────────\n";
    h << "// #include \"" << safe << "_fields.h\"\n";
    h << "// FieldPatch_" << className << "::Setup();\n";
    return h.str();
}


void ClassesTab::FieldPatcherView(Il2CppClass *klass, FieldInfo *field, Il2CppObject *instance)
{
    if (!field) return;
    auto *ft = field->getType();
    if (!ft) return;

    const char *tn     = ft->getName();
    bool isStatic      = (Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC) != 0;
    uintptr_t offset   = field->getOffset();
    auto &pd           = fieldPatchMap[field];

    bool supportedType =
        strcmp(tn, "System.Boolean") == 0 ||
        strcmp(tn, "System.Int16")   == 0 || strcmp(tn, "System.UInt16") == 0 ||
        strcmp(tn, "System.Int32")   == 0 || strcmp(tn, "System.UInt32") == 0 ||
        strcmp(tn, "System.Int64")   == 0 || strcmp(tn, "System.UInt64") == 0 ||
        strcmp(tn, "System.Single")  == 0 || ft->isEnum();

    if (!supportedType) {
        ImGui::TextColored(ImVec4(.9f,.3f,.3f,1.f), "Type not supported: %s", tn);
        return;
    }

    // ── Info row ─────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20,20,30,180));
    ImGui::BeginChild("##finfo", ImVec2(0, ImGui::GetTextLineHeightWithSpacing()*3 + 8), true);
    ImGui::Columns(2, "##fcols", false);
    ImGui::SetColumnWidth(0, 80);
    ImGui::TextDisabled("Type");   ImGui::NextColumn(); ImGui::Text("%s", tn);     ImGui::NextColumn();
    ImGui::TextDisabled("Offset"); ImGui::NextColumn(); ImGui::Text("0x%zX", offset); ImGui::NextColumn();
    ImGui::TextDisabled("Static"); ImGui::NextColumn();
    if (isStatic) ImGui::TextColored(ImVec4(1.f,.8f,.3f,1.f), "Yes");
    else          ImGui::TextDisabled("No");
    ImGui::Columns(1);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // ── Instance scan (non-static only) ──────────────────────────────────────
    static std::unordered_map<FieldInfo*, std::vector<Il2CppObject*>> instCache;
    static std::unordered_map<FieldInfo*, bool> scanState;
    static std::unordered_map<FieldInfo*, int>  selIdx;

    if (!isStatic)
    {
        auto &insts   = instCache[field];
        auto &scanning= scanState[field];
        auto &idx     = selIdx[field];
        float bw      = ImGui::GetContentRegionAvail().x;

        // Baris scan
        ImGui::PushStyleColor(ImGuiCol_Button,
            scanning ? IM_COL32(70,70,20,220) : IM_COL32(25,80,180,220));
        if (ImGui::Button(scanning ? "  Scanning..." : "  Scan Instances",
                          ImVec2(bw * 0.55f, 0)) && !scanning)
        {
            scanning = true; insts.clear(); idx = -1;
            // BUG FIX (Langkah 1 - Thread Safety):
            // Gunakan shared_ptr untuk insts dan scanning agar tidak ada
            // dangling reference ke local static map jika view di-close.
            auto sharedInsts    = std::make_shared<std::vector<Il2CppObject*>>();
            auto sharedScanning = std::make_shared<std::atomic<bool>>(true);
            // Map entry tetap valid selama FieldPatcherView dipanggil,
            // tapi kita amankan dengan pointer ke entry yang stabil.
            std::thread([klass, sharedInsts, sharedScanning, &insts, &scanning]() mutable
            {
                *sharedInsts = Il2cpp::GC::FindObjects(klass);
                insts = *sharedInsts; // safe: UI thread cek scanning==false dulu
                sharedScanning->store(false, std::memory_order_release);
                scanning = false;
            }).detach();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (insts.empty())
            ImGui::TextDisabled("No instances yet");
        else
            ImGui::TextColored(ImVec4(.4f,1.f,.4f,1.f), "%zu found", insts.size());

        if (!insts.empty())
        {
            char cur[72] = "-- pilih instance --";
            if (idx >= 0 && idx < (int)insts.size())
                snprintf(cur, sizeof(cur), "[%p]", insts[idx]);
            ImGui::SetNextItemWidth(bw);
            if (ImGui::BeginCombo("##isel", cur))
            {
                for (int ii = 0; ii < (int)insts.size() && ii < 100; ii++) {
                    char lbl[72]; snprintf(lbl, sizeof(lbl), "[%p]", insts[ii]);
                    bool sel = (idx == ii);
                    if (ImGui::Selectable(lbl, sel)) idx = ii;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (idx >= 0 && idx < (int)insts.size()) instance = insts[idx];
        }
        ImGui::Spacing();
    }

    // ── Patch / Restore ───────────────────────────────────────────────────────
    bool needInst = (!isStatic && !instance);
    if (needInst) {
        ImGui::TextColored(ImVec4(1.f,.65f,.0f,1.f), "Scan & pilih instance terlebih dahulu.");
        ImGui::BeginDisabled();
    }

    float bw = ImGui::GetContentRegionAvail().x;
    if (!pd.active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(20,120,20,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(20,160,20,255));
        if (ImGui::Button("  Set Value & Patch  ", ImVec2(bw, 0)))
        {
            if (strcmp(tn, "System.Boolean") == 0)
            {
                poper.Open("BooleanSelector",
                    [field,isStatic,instance](const std::string &val){
                        auto &p = fieldPatchMap[field];
                        bool bv = (val=="True");
                        p = {val, true, isStatic, {(uint8_t)bv}, 1, nullptr};
                        if (isStatic) field->setStaticValue(&bv);
                        else if (instance){p.patchedInstance=instance;Il2cpp::SetFieldValue(instance,field,&bv);}
                    });
            }
            else if (ft->isEnum())
            {
                poper.Open("EnumSelector",
                    [field,ft,isStatic,instance](const std::string &res){
                        int v = ft->getClass()->getField(res.c_str())->getStaticValue<int>();
                        auto &p = fieldPatchMap[field];
                        std::vector<uint8_t> vb(4); memcpy(vb.data(),&v,4);
                        p = {res, true, isStatic, vb, 4, nullptr};
                        if (isStatic) field->setStaticValue(&v);
                        else if (instance){p.patchedInstance=instance;Il2cpp::SetFieldValue(instance,field,&v);}
                    }, ft);
            }
            else
            {
                Keyboard::Open([field,tn,isStatic,instance](const std::string &text){
                    if (text.empty()) return;
                    try {
                        auto &p = fieldPatchMap[field];
                        p.active=true; p.isStatic=isStatic; p.text=text;
                        #define _FP(CS,CPP,CONV) if(strcmp(tn,CS)==0){ \
                            CPP v=(CPP)(CONV); p.typeSize=sizeof(v); \
                            p.valueBytes.resize(sizeof(v)); memcpy(p.valueBytes.data(),&v,sizeof(v)); \
                            if(isStatic)field->setStaticValue(&v); \
                            else if(instance){p.patchedInstance=instance;Il2cpp::SetFieldValue(instance,field,&v);}}
                        _FP("System.Int16",  int16_t,  std::stoi(text))
                        else _FP("System.UInt16",uint16_t,std::stoi(text))
                        else _FP("System.Int32",  int32_t,  std::stol(text))
                        else _FP("System.UInt32", uint32_t, std::stoul(text))
                        else _FP("System.Int64",  int64_t,  std::stoll(text))
                        else _FP("System.UInt64", uint64_t, std::stoull(text))
                        else _FP("System.Single", float,    std::stof(text))
                        #undef _FP
                    } catch (...) { fieldPatchMap[field].active=false; }
                });
            }
        }
        ImGui::PopStyleColor(2);
    }
    else
    {
        // Re-apply tiap frame — dengan validasi pointer SafeRead
        if (!pd.isStatic && pd.patchedInstance && !pd.valueBytes.empty())
        {
            // Validasi: halaman masih di-map dan klass pointer masih valid
            if (IsPtrValid(pd.patchedInstance))
            {
                Il2CppClass *_chkKlass = nullptr;
                if (SafeRead<Il2CppClass*>((const void*)pd.patchedInstance, &_chkKlass)
                    && _chkKlass && IsPtrValid(_chkKlass))
                {
                    try { Il2cpp::SetFieldValue(pd.patchedInstance, field, pd.valueBytes.data()); }
                    catch (...) {}
                }
                else
                {
                    // Object sudah corrupt — reset patch agar tidak crash terus
                    LOGI("FieldPatcher: instance corrupt, reset '%s'", field->getName() ? field->getName() : "?");
                    pd.active = false; pd.patchedInstance = nullptr; pd.valueBytes.clear();
                }
            }
            else
            {
                // Pointer sudah di-unmap (GC collect) — reset state
                pd.active = false; pd.patchedInstance = nullptr; pd.valueBytes.clear();
            }
        }

        // Status bar
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10,40,10,200));
        ImGui::BeginChild("##fstatus", ImVec2(0,28), false);
        ImGui::TextColored(ImVec4(.2f,1.f,.4f,1.f), "  PATCHED");
        ImGui::SameLine(); ImGui::TextDisabled("|");
        ImGui::SameLine(); ImGui::Text("%s", pd.text.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(180,40,40,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(220,50,50,255));
        if (ImGui::Button("  Restore Original  ", ImVec2(bw, 0)))
        {
            pd.active=false; pd.text.clear();
            pd.valueBytes.clear(); pd.patchedInstance=nullptr;
        }
        ImGui::PopStyleColor(2);
    }

    if (needInst) ImGui::EndDisabled();
    poper.Update();
}


void ClassesTab::FieldViewer(Il2CppClass *klass)
{
    if (!klass) return;
    auto fields = klass->getFields();
    if (fields.empty()) { ImGui::TextDisabled("No fields."); return; }

    // State per klass
    static std::unordered_map<Il2CppClass*, std::unordered_map<FieldInfo*,bool>> chkMap;
    static std::unordered_map<Il2CppClass*, int> hookIdxMap;     // index ke candidates
    static std::unordered_map<Il2CppClass*, std::vector<MethodInfo*>> hookCandMap;

    auto &checklist = chkMap[klass];
    auto &hookIdx   = hookIdxMap[klass];
    auto &candidates= hookCandMap[klass];

    // Lazy-build candidate list
    if (candidates.empty())
        candidates = GetHookCandidates(klass);

    // ── Toolbar atas ─────────────────────────────────────────────────────────
    {
        // Kumpulkan patched + selected
        std::vector<std::pair<FieldInfo*,FieldPatchData*>> selected;
        int totalPatch = 0;
        for (auto *f : fields) {
            if (!f) continue;
            bool active = fieldPatchMap.count(f) && fieldPatchMap[f].active;
            if (active) {
                totalPatch++;
                if (checklist[f]) selected.push_back({f, &fieldPatchMap[f]});
            }
        }

        if (totalPatch > 0)
        {
            // ── Baris 1: Select All / None + counter ─────────────────────────
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40,40,60,220));
            if (ImGui::SmallButton(" All "))
                for (auto *f:fields) if(f&&fieldPatchMap.count(f)&&fieldPatchMap[f].active) checklist[f]=true;
            ImGui::SameLine();
            if (ImGui::SmallButton(" None "))
                for (auto *f:fields) checklist[f]=false;
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%d/%d patched selected", (int)selected.size(), totalPatch);

            // ── Baris 2: Hook method selector ────────────────────────────────
            ImGui::Spacing();
            ImGui::TextDisabled("Hook via:");
            ImGui::SameLine();
            if (!candidates.empty())
            {
                // Label untuk combo
                MethodInfo *curHook = (hookIdx >= 0 && hookIdx < (int)candidates.size())
                                      ? candidates[hookIdx] : candidates[0];
                char comboLbl[128];
                uintptr_t base = (uintptr_t)GetLibBase(GetTargetLib());
                uintptr_t rva  = (base && curHook->methodPointer &&
                                  (uintptr_t)curHook->methodPointer > base)
                                  ? (uintptr_t)curHook->methodPointer - base : 0;
                int score = HookMethodScore(curHook);
                snprintf(comboLbl, sizeof(comboLbl), "%s  [0x%zX]%s",
                    curHook->getName() ? curHook->getName() : "?",
                    rva,
                    score >= 60 ? "  ✓" : "");

                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100);
                if (ImGui::BeginCombo("##hooksel", comboLbl, ImGuiComboFlags_HeightLarge))
                {
                    for (int mi = 0; mi < (int)candidates.size() && mi < 60; mi++)
                    {
                        auto *m = candidates[mi];
                        if (!m) continue;
                        uintptr_t mrva = (base && m->methodPointer &&
                                         (uintptr_t)m->methodPointer > base)
                                         ? (uintptr_t)m->methodPointer - base : 0;
                        int sc = HookMethodScore(m);
                        char lbl[128];
                        snprintf(lbl, sizeof(lbl), "%s  [0x%zX]%s",
                            m->getName() ? m->getName() : "?", mrva,
                            sc >= 100 ? "  ★★★" :
                            sc >= 85  ? "  ★★" :
                            sc >= 60  ? "  ★" : "");

                        // Warna: makin tinggi score makin hijau
                        int g = std::min(255, 100 + sc*2);
                        ImGui::PushStyleColor(ImGuiCol_Text,
                            sc >= 60 ? IM_COL32(100,g,100,255) : IM_COL32(180,180,180,255));
                        bool sel = (hookIdx == mi);
                        if (ImGui::Selectable(lbl, sel)) hookIdx = mi;
                        if (sel) ImGui::SetItemDefaultFocus();
                        ImGui::PopStyleColor();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();

                // Skor badge
                char badge[24];
                int sc = HookMethodScore(curHook);
                ImU32 badgeCol = sc >= 100 ? IM_COL32(40,200,40,255) :
                                 sc >= 60  ? IM_COL32(200,180,40,255) :
                                             IM_COL32(180,80,80,255);
                ImGui::PushStyleColor(ImGuiCol_Text, badgeCol);
                snprintf(badge, sizeof(badge), "Score:%d", sc);
                ImGui::TextUnformatted(badge);
                ImGui::PopStyleColor();
            }

            // ── Baris 3: Export button ────────────────────────────────────────
            ImGui::Spacing();
            bool canExp = !selected.empty();
            if (!canExp) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(20,130,20,220));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(20,170,20,255));
            char expLbl[64];
            snprintf(expLbl, sizeof(expLbl), "  Export .h  (%d selected)  ", (int)selected.size());
            if (ImGui::Button(expLbl, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            {
                MethodInfo *hookM = (!candidates.empty() && hookIdx >= 0 && hookIdx < (int)candidates.size())
                                    ? candidates[hookIdx] : nullptr;
                std::string hContent = GenerateFieldHeader(klass, selected, hookM);
                if (!hContent.empty()) {
                    std::string safe = klass->getFullName();
                    for (char &c:safe) if(c=='.'||c=='/'||c=='<'||c=='>') c='_';
                    std::string fname = safe + "_fields.h";
                    Util::FileWriter fw(Util::DirFields(), fname); fw.write(hContent.c_str());
                    Keyboard::Open((Util::DirFields()+"/"+fname).c_str(), [](const std::string&){});
                }
            }
            ImGui::PopStyleColor(2);
            if (!canExp) ImGui::EndDisabled();

            ImGui::Separator();
        }
    }

    // ── Daftar field ──────────────────────────────────────────────────────────
    int fi = 0;
    for (auto *field : fields)
    {
        if (!field) continue;
        auto *ft    = field->getType();
        const char *fn = field->getName() ? field->getName() : "?";
        const char *tn = ft ? ft->getName() : "?";
        bool isSt   = (Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC) != 0;
        uintptr_t off = field->getOffset();
        bool patched  = fieldPatchMap.count(field) && fieldPatchMap[field].active;

        ImGui::PushID(fi++);

        // Checkbox (hanya jika patched)
        if (patched) {
            bool &chk = checklist[field];
            ImGui::Checkbox("##ck", &chk);
        } else {
            ImGui::Dummy(ImVec2(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x, 0));
        }
        ImGui::SameLine();

        // ── TreeNode dengan warna custom per segmen ───────────────────────────
        // Gunakan "###fN" ID trick agar ImGui tetap track node, tapi label kita
        // render manual pakai TextColored setelah invisible TreeNode hitbox.
        //
        // Warna:
        //   patched   → hijau cerah  (aktif dipatch)
        //   static    → amber kuning (field static)
        //   typename  → teal redup   (info tipe, bukan fokus utama)
        //   fieldname → putih terang (fokus utama)
        //   offset    → orange redup (info sekunder)

        char nodeId[32];
        snprintf(nodeId, sizeof(nodeId), "###f%d", fi);

        bool open = false;
        if (patched) {
            // Patched: satu warna hijau dengan nilai
            char lbl[256];
            snprintf(lbl, sizeof(lbl), " ✓ %s  =  %s  [0x%zX]%s",
                fn, fieldPatchMap[field].text.c_str(), off, nodeId);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(60,230,120,255));
            open = ImGui::TreeNode(lbl);
            ImGui::PopStyleColor();
        } else if (isSt) {
            // Static: amber + typename redup + fieldname kuning terang + offset redup
            char lbl[256];
            snprintf(lbl, sizeof(lbl), "[S] %s  %s  [0x%zX]%s", tn, fn, off, nodeId);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,210,80,255));
            open = ImGui::TreeNode(lbl);
            ImGui::PopStyleColor();
        } else {
            // ── Normal field: TANPA overlay DrawList ─────────────────────────
            // Warna berbeda per segmen menggunakan label sederhana.
            // Tidak ada double render / ghost text.
            //   typename  → teal redup   (info sekunder)
            //   fieldname → putih terang (fokus utama) — dibedakan via warna keseluruhan
            //   offset    → orange redup (info tambahan)
            char lbl[256];
            snprintf(lbl, sizeof(lbl), "%s  %s  [0x%zX]%s", tn, fn, off, nodeId);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 215, 235, 255)); // biru-teal terang
            open = ImGui::TreeNode(lbl);
            ImGui::PopStyleColor();
        }

        // ── Fitur 5: Tombol Hierarchy untuk field Transform/GameObject ────────
        if (!open) {
            bool isTransformField = tn && IsTransformType(tn);
            if (isTransformField) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50,30,100,200));
                char hierBtnId[64]; snprintf(hierBtnId, sizeof(hierBtnId), "Hier##h%d", fi-1);
                if (ImGui::SmallButton(hierBtnId)) {
                    // Coba ambil instance dari objectMap jika ada
                    Il2CppObject *inst = nullptr;
                    auto it = objectMap.find(klass);
                    if (it != objectMap.end() && !it->second.empty())
                        inst = it->second[0];
                    if (inst) {
                        // Baca field Transform dari instance — SafeRead untuk hindari crash
                        Il2CppObject *tf = nullptr;
                        if (SafeRead<Il2CppObject*>((const void*)((uintptr_t)inst + off), &tf)
                            && tf && IsPtrValid(tf))
                        {
                            // tf valid, cek klass pointer
                            Il2CppClass *_tfk = nullptr;
                            if (SafeRead<Il2CppClass*>((const void*)tf, &_tfk) && _tfk && IsPtrValid(_tfk)) {
                                char popupId[128];
                                snprintf(popupId, sizeof(popupId), "HierarchyViewer_%p", tf);
                                ImGui::OpenPopup(popupId);
                                HierarchyViewerPopup(tf, fn);
                            }
                        }
                    } else {
                        ImGui::OpenPopup("HierarchyNoInst");
                    }
                }
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Trace Hierarchy / Bones (butuh instance)");
                if (ImGui::BeginPopup("HierarchyNoInst")) {
                    ImGui::TextColored(ImVec4(1.f,0.4f,0.2f,1.f),"Belum ada instance!");
                    ImGui::TextDisabled("Scan objects dulu lewat 'Find Objects' di atas.");
                    if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
        }

        if (open) {
            ImGui::Indent(8.f);
            FieldPatcherView(klass, field);
            ImGui::Unindent(8.f);
            ImGui::TreePop();
        }
        ImGui::PopID();
        ImGui::Separator();
    }
}

