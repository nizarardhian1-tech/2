// ============================================================================
// View_MethodAnalyzer.cpp
// Pilar 3: Dipecah dari ClassesTab.cpp (4479 baris)
// CallerView, PatcherView, HookerView, HookerData, MethodViewer
// ============================================================================
#include "ClassesTab_Shared.h"

// hookerMap & hookerMtx didefinisikan di Tool.cpp — extern via ClassesTab_Shared.h
// maxLine hanya didefinisikan di sini
int maxLine{5};

#ifndef USE_FRIDA
// ─────────────────────────────────────────────────────────────────────────────
// hookerHandler — dipanggil Dobby dari game thread (UnityMain) setiap kali
// method yang di-trace dipanggil.
//
// CRITICAL RULES untuk Dobby instrument callback di ARM64 Unity:
//
//  1. JANGAN blocking mutex (std::lock_guard). Kalau UI thread sedang pegang
//     hookerMtx (misal saat ToggleHooker), game thread akan STALL.
//     Di ARM64 Unity, stall di dalam Dobby pre-call handler korupsi register
//     context (termasuk x0 = this → null) → SIGSEGV saat method resume.
//     FIX: pakai try_to_lock — kalau mutex sedang dipakai, skip saja.
//
//  2. JANGAN panggil fungsi berat / alokasi heap di sini. Seminimal mungkin.
//
//  3. JANGAN akses hookerMap via operator[] (bisa rehash & crash).
//     Gunakan find() saja.
// ─────────────────────────────────────────────────────────────────────────────
void hookerHandler(void *address, DobbyRegisterContext *ctx)
{
    // try_to_lock: jika mutex sedang dipegang UI thread, SKIP (jangan block!)
    std::unique_lock<std::mutex> guard(hookerMtx, std::try_to_lock);
    if (!guard.owns_lock())
        return; // skip invocation ini daripada stall game thread

    auto it = hookerMap.find(address);
    if (it == hookerMap.end())
        return;

    auto &hookerData = it->second;
    hookerData.hitCount++;
    hookerData.time = 1.f;

    // Bangun label — minimal, tanpa alokasi heap
    const char *name = hookerData.method ? hookerData.method->getName() : "?";
    char buffer[128]{0};
    snprintf(buffer, sizeof(buffer), "%p | %s",
             hookerData.method ? hookerData.method->getAbsAddress() : nullptr, name);

    // Update visited (circular buffer) — cari duplikat dulu
    int i = 0;
    for (auto vi = HookerData::visited.rbegin(); vi != HookerData::visited.rend(); ++vi)
    {
        if (i >= maxLine) break;
        if (vi->name == buffer)
        {
            vi->goneTime = 10.f;
            vi->time     = 2.f;
            vi->hitCount++;
            return;
        }
        i++;
    }
    HookerData::visited.push_back({buffer, 2.f, 10.f, 0});
}
#endif

ClassesTab::MethodList &ClassesTab::buildMethodMap(Il2CppClass *klass)
{
    static Il2CppClass *lastClass = nullptr;
    static MethodList methodList;

    if (lastClass != klass)
    {
        methodList.clear();
        auto methods = klass->getMethods();
        LOGD("Rebuilding %s | %lu methods", klass->getName(), methods.size());
        for (auto method : methods)
        {
            auto paramsInfo = method->getParamsInfo();
            methodList.push_back({method, paramsInfo});
        }
        LOGD("Rebuilt %lu methods", methodList.size());
        lastClass = klass;
    }
    return methodList;
}

ClassesTab::ClassesTab()
{
    selectedImage = g_Image;

    for (int i = 0; i < g_Images.size(); i++)
    {
        if (g_Images[i] == selectedImage)
        {
            selectedImageIndex = i;
            break;
        }
    }
    classes = selectedImage->getClasses();
    filteredClasses = classes;
}

ClassesTab::Paths &ClassesTab::getJsonPaths(Il2CppObject *object)
{
    return dataMap[object].second;
}

void ClassesTab::setJsonObject(Il2CppObject *object)
{
    // std::vector<uintptr_t> visited{};
    // dataMap[object].first = object->dump(visited, 9999);
    dataMap[object].first = object->dump({});

    tabMap.emplace(object, true);
}

ClassesTab::Json &ClassesTab::getJsonObject(Il2CppObject *object)
{
    return dataMap[object].first.second;
}


void ClassesTab::CallerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                            Il2CppObject *thiz)
{
    static ImGuiIO &io = ImGui::GetIO();
    bool methodIsStatic = Il2cpp::GetIsMethodStatic(method);
    auto &params = paramMap[method];
    if (!methodIsStatic && !thiz)
    {
        auto &thisParam = params["this"];
        char thisLabel[128]{0};
        sprintf(thisLabel, "%s this", Il2cpp::GetClassType(klass)->getName());
        if (!thisParam.value.empty())
        {
            sprintf(thisLabel, "%s = %s", thisLabel, thisParam.value.c_str());
        }
        if (ImGui::Button(thisLabel))
        {
            ImGui::OpenPopup("ThisObjectSelector");
        }
        if (ImGui::BeginPopup("ThisObjectSelector"))
        {
            ImGuiObjectSelector(
                ImGui::GetID("ThisObjectSelector"), klass, "this",
                [&thisParam](Il2CppObject *object)
                {
                    char objStr[16]{0};
                    sprintf(objStr, "%p", object);
                    thisParam.value = objStr;
                    thisParam.object = object;
                    ImGui::CloseCurrentPopup();
                },
                strcmp(method->getName(), ".ctor") == 0);
            ImGui::EndPopup();
        }
    }
    for (int k = 0; k < paramsInfo.size(); k++)
    {
        auto &[name, type] = paramsInfo[k];

        char paramKey[64]{0};
        sprintf(paramKey, "%p%s%d", method, name, k);
        auto &param = params[paramKey];

        char buttonLabel[128]{0};
        sprintf(buttonLabel, "%s %s", type->getName(), name);
        if (!param.value.empty())
        {
            sprintf(buttonLabel, "%s = %s", buttonLabel, param.value.c_str());
        }
        ImGui::PushID(k);
        if (ImGui::Button(buttonLabel))
        {
            bool isString = strcmp(type->getName(), "System.String") == 0;
            if (type->isPrimitive() || isString)
            {
                if (strcmp(type->getName(), "System.Boolean") == 0)
                {
                    // ImGui::OpenPopup("BooleanSelector");
                    poper.Open("BooleanSelector", [&param](const std::string &result) { param.value = result; });
                }
                else
                {
                    Keyboard::Open(
                        [&param, &isString](const std::string &text)
                        {
                            if (isString)
                            {
                                param.object = Il2cpp::NewString(text.c_str());
                            }
                            param.value = text;
                        });
                }
            }
            else if (type->isEnum())
            {
                poper.Open(
                    "EnumSelector", [&param](const std::string &result) { param.value = result; }, type);
            }
            // else if (!type->isValueType() && !(type->isArray() || type->isList()
            // ||
            //                                    strstr(type->getName(),
            //                                    "System.Object")))
            // else if (!strstr(type->getName(), "System.Object"))
            else
            {
                ImGui::OpenPopup("ParamObjectSelector");
            }
        }
        if (ImGui::BeginPopup("ParamObjectSelector"))
        {
            ImGuiObjectSelector(ImGui::GetID("ParamObjectSelector"), type->getClass(), name,
                                [&param](Il2CppObject *object)
                                {
                                    char objStr[16]{0};
                                    sprintf(objStr, "%p", object);
                                    param.value = objStr;
                                    param.object = object;
                                    ImGui::CloseCurrentPopup();
                                });
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30, 200, 25, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 200, 25, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(30, 200, 25, 255));
    if (ImGui::Button("Call", ImVec2(io.DisplaySize.x / 2, 0)))
    {
        auto paramsInfo = method->getParamsInfo();
        auto params = paramMap[method];
        // BUG FIX (Langkah 1 - RAII): Ganti raw pointer new[] dengan std::vector
        // untuk mencegah memory leak jika ada early return atau exception.
        std::vector<Il2CppObject*> arrayParamsVec(paramsInfo.size(), nullptr);
        Il2CppObject **arrayParams = paramsInfo.size() > 0 ? arrayParamsVec.data() : nullptr;

        bool hasParams = true;
        Il2CppObject *thisParam = nullptr;
        if (!methodIsStatic && !thiz)
        {
            if (params["this"].value.empty())
            {
                hasParams = false;
            }
            else
            {
                thisParam = params["this"].object;
                LOGD("this = %s", params["this"].value.c_str());
            }
        }
        else if (thiz)
        {
            thisParam = thiz;
        }

        for (int k = 0; k < paramsInfo.size(); k++)
        {
            auto &[name, type] = paramsInfo[k];

            char paramKey[64]{0};
            sprintf(paramKey, "%p%s%d", method, name, k);
            auto &param = params[paramKey];
            LOGD("%s %s = %s", type->getName(), name, param.value.c_str());
            if (!param.value.empty())
            {
                if (strcmp(type->getName(), "System.Int32") == 0)
                {
                    ValueType<int> value{std::stoi(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Int64") == 0)
                {
                    ValueType<long> value{std::stol(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.UInt32") == 0)
                {
                    ValueType<unsigned int> value{static_cast<unsigned int>(std::stoul(param.value))};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.UInt64") == 0)
                {
                    ValueType<unsigned long> value{std::stoul(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Single") == 0)
                {
                    ValueType<float> value{std::stof(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Double") == 0)
                {
                    ValueType<double> value{std::stod(param.value)};
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (strcmp(type->getName(), "System.Boolean") == 0)
                {
                    ValueType<int> value{param.value == "True" ? 1 : 0}; // using true/false sometimes causing
                                                                         // crash for me, don't know why
                    auto boxedValue = value.box(type->getClass());
                    arrayParams[k] = boxedValue;
                }
                else if (type->isEnum())
                {
                    arrayParams[k] = type->getClass()
                                         ->getField(param.value.c_str())
                                         ->getStaticValue<ValueType<int>>()
                                         .box(type->getClass());
                }
                else if (strcmp(type->getName(), "System.String") == 0)
                {
                    arrayParams[k] = Il2cpp::NewString(param.value.c_str());
                }
                else if (param.object)
                {
                    arrayParams[k] = param.object;
                }
                else
                {
                    LOGD("Unhandled type: %s %s", type->getName(), name);
                }
            }
            else
            {
                hasParams = false;
            }
        }
        if (hasParams)
        {
            Il2CppObject *result = nullptr;
            if (strcmp(method->getName(), ".ctor") != 0 && thisParam &&
                Il2cpp::GetClassType(thisParam->klass)->isValueType())
            {
                auto thizz = Il2cpp::GetUnboxedValue(thisParam);
                result = Il2cpp::RuntimeInvokeConvertArgs(method, thizz, arrayParams, paramsInfo.size());
            }
            else
            {
                result = Il2cpp::RuntimeInvokeConvertArgs(method, thisParam, arrayParams, paramsInfo.size());
            }
            LOGPTR(result);
            if (result && strcmp(method->getName(), ".ctor") != 0)
            {
                auto resultType = Il2cpp::GetClassType(result->klass);
                if (resultType->isPrimitive())
                {
                    std::vector<uintptr_t> visited;
                    auto j = result->dump(visited, 1);
                    callResults.at(method).push_back(std::pair{j.begin().value().dump(), nullptr});
                }
                else if (strcmp(resultType->getName(), "System.String") == 0)
                {
                    callResults.at(method).push_back({((Il2CppString *)result)->to_string(), nullptr});
                }
                else if (resultType->isEnum())
                {
                    callResults.at(method).push_back(
                        {result->invoke_method<Il2CppString *>("ToString")->to_string(), nullptr});
                }
                else
                {
                    if (resultType->isValueType())
                    {
                        Il2cpp::GC::KeepAlive(result); // does this actually work?
                    }
                    auto toString = result->klass->getMethod("ToString", 0);
                    if (toString)
                    {
                        Il2CppString *str = nullptr;
                        if (resultType->isValueType())
                        {
                            auto thizz = Il2cpp::GetUnboxedValue(result);
                            // str = toString->invoke_static<Il2CppString *>(thizz);
                            str = (Il2CppString *)Il2cpp::RuntimeInvokeConvertArgs(toString, thizz, nullptr, 0);
                        }
                        else
                        {
                            str = toString->invoke_static<Il2CppString *>(result);
                        }
                        if (str)
                        {
                            callResults.at(method).push_back({str->to_string(), result});
                        }
                        else
                        {
                            callResults.at(method).push_back({"the call returned null", result});
                        }
                    }
                    else
                    {
                        char resultStr[16]{0};
                        sprintf(resultStr, "%p", result);
                        callResults.at(method).push_back({resultStr, result});
                    }
                }
                savedSet[resultType->getClass()].insert(result);
                // setJsonObject(result);
            }
            else
            {
                callResults.at(method).push_back({"the call returned null", nullptr});
            }
        }
        else
        {
            LOGE("Not all params are set!");
        }
        // BUG FIX (Langkah 1): delete[] dihapus — arrayParamsVec (vector) 
        // di-destroy otomatis saat keluar scope (RAII).
        // if (arrayParams) delete[] arrayParams;  // <-- REMOVED
    }
    ImGui::PopStyleColor(3);
    if (!callResults.at(method).empty())
    {
        ImGui::Separator();
        ImGui::Text("Call Results:");
        for (auto [callResult, object] : callResults.at(method))
        {
            if (object)
            {
                if (ImGui::Button(callResult.c_str()))
                {
                    setJsonObject(object);
                }
            }
            else
            {
                ImGui::Text("%s", callResult.c_str());
            }
            ImGui::Separator();
        }

    }
}


bool ClassesTab::isMethodHooked(MethodInfo *method)
{
    return hookerMap.find(method->methodPointer) != hookerMap.end();
}


void ClassesTab::PatcherView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                             Il2CppObject *thiz)
{
    {
        if (isMethodHooked(method))
        {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't patch while hooked!");
            return;
        }
    }

    auto &o = oMap[method];
    auto type = method->getReturnType();
    if (strcmp(type->getName(), "System.Int16") == 0 || strcmp(type->getName(), "System.Int32") == 0 ||
        strcmp(type->getName(), "System.Int64") == 0 || strcmp(type->getName(), "System.UInt16") == 0 ||
        strcmp(type->getName(), "System.UInt32") == 0 || strcmp(type->getName(), "System.UInt64") == 0 ||
        strcmp(type->getName(), "System.Single") == 0 || strcmp(type->getName(), "System.Boolean") == 0 ||
        strcmp(type->getName(), "System.String") == 0 || type->isEnum())
    {
        if (ImGui::Button("Patch return value"))
        {
            ImGui::OpenPopup("HookReturnValuePopup");
        }
        if (!o.text.empty())
        {
            ImGui::SameLine();
            ImGui::Text("-> %s", o.text.c_str());
        }
    }
    else if (strcmp(type->getName(), "System.Void") == 0)
    {
        if (o.bytes.empty())
        {
            if (ImGui::Button("NOP"))
            {
                Patcher p{method};
                p.ret();
                o.bytes = p.patch();
            }
        }
        else
        {
            if (ImGui::Button("Restore"))
            {
                memcpy(method->methodPointer, o.bytes.data(), o.bytes.size());
                o.bytes.clear();
                o.text.clear();
            }
        }
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
        ImGui::Text("Not supported!");
        ImGui::PopStyleColor();
    }
    if (ImGui::BeginPopup("HookReturnValuePopup"))
    {
        ImGui::Text("Change return value");
        ImGui::PushID(type);
        char label[64]{0};
        if (!o.bytes.empty())
        {
            sprintf(label, "%s", "Restore");
        }
        else
        {
            sprintf(label, "%s", type->getName());
        }
        if (ImGui::Button(label))
        {
            if (!o.bytes.empty())
            {
                memcpy(method->methodPointer, o.bytes.data(), o.bytes.size());
                o.bytes.clear();
                o.text.clear();
            }
            else
            {
                if (strcmp(type->getName(), "System.Int16") == 0 || strcmp(type->getName(), "System.Int32") == 0 ||
                    strcmp(type->getName(), "System.Int64") == 0 || strcmp(type->getName(), "System.UInt16") == 0 ||
                    strcmp(type->getName(), "System.UInt32") == 0 || strcmp(type->getName(), "System.UInt64") == 0 ||
                    strcmp(type->getName(), "System.Single") == 0 || strcmp(type->getName(), "System.Boolean") == 0 ||
                    strcmp(type->getName(), "System.String") == 0)
                {
                    if (strcmp(type->getName(), "System.Boolean") == 0)
                    {
                        poper.Open("BooleanSelector",
                                   [method](const std::string &b)
                                   {
                                       using namespace asmjit;
                                       Patcher p{method};
                                       bool value = b == "True";
                                       p.movBool(value);
                                       p.ret();

                                       if (oMap[method].bytes.empty())
                                       {
                                           oMap[method].bytes = p.patch();
                                           oMap[method].text = b;
                                       }
                                       else
                                       {
                                           LOGE("oMap is not empty for %s", method->getName());
                                       }
                                   });
                    }
                    else
                    {
                        auto typ = type;
                        auto m = method;
                        Keyboard::Open(
                            [typ, method = m](const std::string &text)
                            {
                                auto isString = strcmp(typ->getName(), "System.String") == 0;
                                if (text.empty())
                                    return;

                                auto type = typ;
                                Patcher p{method};
                                // auto &assembler = p.assembler;
                                if (strcmp(type->getName(), "System.Int16") == 0)
                                {
                                    int16_t value = std::stoi(text);
                                    p.movInt16(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt16") == 0)
                                {
                                    unsigned short value = std::stoi(text);
                                    p.movUInt16(value);
                                }
                                else if (strcmp(type->getName(), "System.Int32") == 0)
                                {
                                    int value{std::stoi(text)};
                                    p.movInt32(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt32") == 0)
                                {
                                    unsigned int value{static_cast<unsigned int>(std::stoul(text))};
                                    p.movUInt32(value);
                                }
                                else if (strcmp(type->getName(), "System.Int64") == 0)
                                {
                                    long value{std::stol(text)};
                                    p.movInt64(value);
                                }
                                else if (strcmp(type->getName(), "System.UInt64") == 0)
                                {
                                    unsigned long value{std::stoul(text)};
                                    p.movUInt64(value);
                                }
                                else if (strcmp(type->getName(), "System.Single") == 0)
                                {
                                    float value = std::stof(text);
                                    p.movFloat(value);
                                }
                                else if (isString)
                                {
                                    p.movPtr(Il2cpp::NewString(text.c_str()));
                                }

                                p.ret();

                                if (oMap[method].bytes.empty())
                                {
                                    oMap[method].bytes = p.patch();
                                    oMap[method].text = text;
                                }
                                else
                                {
                                    LOGE("oMap is not empty for %s", method->getName());
                                }
                            });
                    }
                }
                else if (type->isEnum())
                {
                    poper.Open(
                        "EnumSelector",
                        [method, type](const std::string &result)
                        {
                            int value = type->getClass()->getField(result.c_str())->getStaticValue<int>();

                            using namespace asmjit;
                            Patcher p{method};
                            p.movInt16(value);
                            p.ret();

                            if (oMap[method].bytes.empty())
                            {
                                oMap[method].bytes = p.patch();
                                oMap[method].text = result;
                            }
                            else
                            {
                                LOGE("oMap is not empty for %s", method->getName());
                            }
                        },
                        type);
                }
            }
        }

        ImGui::PopID();
        ImGui::EndPopup();
    }

    // ── Export Lua per-method (hanya tampil jika sudah di-patch) ────────────
    auto &oPatch = oMap[method];
    if (!oPatch.bytes.empty() && method->methodPointer)
    {
        ImGui::Separator();

        // Status
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "Status: PATCHED");
        ImGui::SameLine();
        ImGui::Text("(Value: %s)", oPatch.text.c_str());

        // RESTORE ORIGINAL
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(200, 50, 50, 200));
        if (ImGui::Button("RESTORE ORIGINAL", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            KittyMemory::ProtectAddr(method->methodPointer, oPatch.bytes.size(), PROT_READ | PROT_WRITE | PROT_EXEC);
            std::memcpy(method->methodPointer, oPatch.bytes.data(), oPatch.bytes.size());
            __builtin___clear_cache((char *)method->methodPointer,
                                    (char *)method->methodPointer + oPatch.bytes.size());
            oPatch.bytes.clear();
            oPatch.text.clear();
            LOGI("Restored %s", method->getName());
        }
        ImGui::PopStyleColor();

        // EXPORT LUA
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30, 140, 30, 200));
        if (ImGui::Button("Export Lua", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
        {
            auto klass2    = method->getClass();
            std::string cn = klass2 && klass2->getName() ? klass2->getName() : "Unknown";
            std::string mn = method->getName() ? method->getName() : "unknown";
            std::string tv = oPatch.text;

            // [Pilar 1 — DRY] Delegasi ke CodeExporter::DerivePatchBytes()
            // Sebelumnya: copy-paste ~40 baris inline dengan LDR offset BERBEDA
            // dari CodeGenerator.h (bug: 0x18000040 vs 0x18000060).
            // Sekarang: satu sumber kebenaran, encoding LDR literal BENAR.
            auto rt = method->getReturnType();
            std::string tn = rt ? rt->getName() : "System.Void";
            std::vector<uint8_t> pb = CodeExporter::DerivePatchBytes(tn, tv);

            // Hitung RVA
            uintptr_t absAddr = (uintptr_t)method->methodPointer;
            uintptr_t base    = (uintptr_t)GetLibBase(GetTargetLib());
            uintptr_t rva     = (base > 0 && absAddr > base) ? (absAddr - base) : absAddr;

            // Build Lua script
            std::ostringstream lua;
            lua << "-- ============================================================\n";
            lua << "-- GG Lua Script | " << cn << "::" << mn << " -> " << tv << "\n";
            lua << "-- RVA: 0x" << std::hex << rva << std::dec << "\n";
            lua << "-- ============================================================\n\n";
            lua << "local function getLibBase(n)\n";
            lua << "    local r=gg.getRangesList(n); if r and #r>0 then return r[1].start end; return nil\n";
            lua << "end\n\n";
            lua << "local libBase = getLibBase('libil2cpp.so')\n";
            lua << "if not libBase then print('[GG] libil2cpp.so not found'); return end\n\n";
            lua << "local rva = 0x" << std::hex << rva << std::dec << "\n";
            lua << "local addr = libBase + rva\n";
            lua << "local bytes = {";
            for (size_t i = 0; i < pb.size(); i++) {
                char h[8]; snprintf(h, sizeof(h), "0x%02X", pb[i]);
                lua << h; if (i+1 < pb.size()) lua << ",";
            }
            lua << "}\n";
            lua << "local t={}\n";
            lua << "for i,b in ipairs(bytes) do\n";
            lua << "    table.insert(t,{address=addr+(i-1),flags=gg.TYPE_BYTE,value=b})\n";
            lua << "end\n";
            lua << "gg.setValues(t)\n";
            lua << "print('[GG] Patched: " << cn << "::" << mn << " -> " << tv << "')\n";

            // Simpan file
            std::string sc = cn;
            std::replace(sc.begin(), sc.end(), '.', '_');
            std::string fname = sc + "_" + mn + "_patch.lua";
            Util::FileWriter fw(Util::DirDump(), fname);
            fw.write(lua.str().c_str());
            Keyboard::Open((Util::DirDump() + "/" + fname).c_str(), [](const std::string&){});
        }
        ImGui::PopStyleColor();
    }
}


// ─── Helper: tombol toggle kecil dengan warna aktif/nonaktif ─────────────────
void ClassesTab::HookerView(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                            Il2CppObject *thiz)
{
    // Guard: tidak bisa hook kalau sedang di-patch
    if (oMap[method].bytes.empty() == false)
    {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Can't hook while patched!");
        return;
    }

    auto it    = hookerMap.find(method->methodPointer);
    bool hooked = (it != hookerMap.end());
    float bw   = ImGui::GetContentRegionAvail().x;

    if (!hooked)
    {
        // ── Belum di-trace: tampilkan tombol Trace ────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(30, 120, 30, 220));
        if (ImGui::Button("Trace", ImVec2(bw, 0)))
        {
            Tool::ToggleHooker(method);
            it    = hookerMap.find(method->methodPointer);
            hooked = (it != hookerMap.end());
        }
        ImGui::PopStyleColor();
        return;
    }

    // ── Sudah di-trace ────────────────────────────────────────────────────
    HookerData &hd = it->second;

    // Status bar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 40, 18, 220));
    ImGui::BeginChild("##HVSt", ImVec2(0, ImGui::GetFrameHeightWithSpacing()), false);
    ImGui::TextColored(ImVec4(0.4f, 1.f, 0.4f, 1.f), "TRACED");
    ImGui::SameLine(); ImGui::TextDisabled("|");
    ImGui::SameLine(); ImGui::Text("Hit: %d", hd.hitCount);
    ImGui::SameLine(bw - 65);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(170, 35, 35, 220));
    if (ImGui::Button("Restore", ImVec2(ImGui::GetIO().DisplaySize.x * 0.18f, 0)))
        Tool::ToggleHooker(method);
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ── Tombol Open / Close Inspector Window ──────────────────────────────
    bool inspOpen = hd.inspectorOpen;
    if (!inspOpen)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(25, 80, 180, 230));
        if (ImGui::Button("Open Inspector", ImVec2(bw, 0)))
            hd.inspectorOpen = true;
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(140, 80, 20, 220));
        if (ImGui::Button("Close Inspector", ImVec2(bw, 0)))
            hd.inspectorOpen = false;
        ImGui::PopStyleColor();
        ImGui::TextDisabled("  Inspector window terbuka. Drag & resize bebas.");
    }
}



bool ClassesTab::MethodViewer(Il2CppClass *klass, MethodInfo *method, const MethodParamList &paramsInfo,
                              Il2CppObject *thiz, bool includeInflated)
{
    bool zeroPointer = method->methodPointer == nullptr;
    
    // ⭐ BARU: Auto-expand kalau method ini yang diminta
    bool forceOpen = false;
    if (expandMethod == method) {
        forceOpen = true;
        expandMethod = nullptr;  // Reset setelah dipake
    }

    if (callResults.find(method) == callResults.end())
    {
        callResults.emplace(method, (size_t)5);
    }

    bool methodIsStatic = Il2cpp::GetIsMethodStatic(method);

    char treeLabel[512]{0};
    sprintf(treeLabel, "%s %s(%zu)###", method->getReturnType()->getName(), method->getName(), paramsInfo.size());
    int pushedColor = 0;
    if (methodIsStatic)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 100, 255));
        pushedColor++;
        Util::prependStringToBuffer(treeLabel, "static ");
    }
    bool patched = oMap[method].bytes.empty() == false;
    bool hooked = hookerMap.find(method->methodPointer) != hookerMap.end();
    
    if (zeroPointer)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        pushedColor++;
    }
    if (patched || hooked)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(25, 255, 125, 255));
        pushedColor++;
        if (hooked)
        {
            int hitCount = hookerMap[method->methodPointer].hitCount;
            char hitLabel[64]{0};
            sprintf(hitLabel, "Hit Count %d | ", hitCount);
            Util::prependStringToBuffer(treeLabel, hitLabel);
        }
        else if (patched)
        {
            auto text = oMap[method].text;
            if (!text.empty())
            {
                char buff[64]{0};
                sprintf(buff, "Returns %s | ", text.c_str());
                Util::prependStringToBuffer(treeLabel, buff);
            }
        }
    }
    
    // ⭐ SetNextItemOpen SEBELUM TreeNode
    if (forceOpen) {
        ImGui::SetNextItemOpen(true);
    }
    
    bool state = ImGui::TreeNode(treeLabel);
    
    // ⭐ Kalau forceOpen, state jadi true
    if (forceOpen) {
        state = true;
    }
    
    if (state)
    {
        if (pushedColor)
        {
            ImGui::PopStyleColor(pushedColor);
            pushedColor = 0;
        }

        if (ImGui::BeginTabBar("##methoder"))
        {
            if (ImGui::BeginTabItem("Caller"))
            {
                CallerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Patcher"))
            {
                PatcherView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Tracer"))
            {
                HookerView(klass, method, paramsInfo, thiz);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("RVA"))
            {
                uintptr_t base = (uintptr_t)GetLibBase(GetTargetLib());
                uintptr_t rva  = 0;
                if (method->methodPointer && base && (uintptr_t)method->methodPointer > base)
                    rva = (uintptr_t)method->methodPointer - base;

                ImGui::Spacing();
                if (rva) {
                    char rvaBuf[32];
                    snprintf(rvaBuf, sizeof(rvaBuf), "0x%zX", rva);
                    ImGui::TextColored(ImVec4(0.4f,1.f,0.8f,1.f), "RVA: %s", rvaBuf);
                    ImGui::TextDisabled("DobbyHook(base + %s, ...)", rvaBuf);
                    ImGui::Spacing();
                    if (ImGui::Button("Copy RVA")) {
                        Keyboard::Open(rvaBuf, [](const std::string&){});
                    }
                } else {
                    ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "RVA tidak tersedia (null pointer)");
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::TreePop();
    }
    poper.Update();
    ImGui::PopStyleColor(pushedColor);
    return state;
}

const ClassesTab::MethodParamList &ClassesTab::getCachedParams(MethodInfo *method)
{
    static std::unordered_map<MethodInfo *, MethodParamList> params;
    if (params.find(method) == params.end())
    {
        params[method] = method->getParamsInfo();
    }
    return params[method];
}

// static std::unordered_map<Il2CppClass *, bool> states;