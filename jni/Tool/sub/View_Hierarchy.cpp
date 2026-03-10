// ============================================================================
// View_Hierarchy.cpp
// Pilar 3: Dipecah dari ClassesTab.cpp (4479 baris)
// ImGuiJson (JSON object viewer), ensureIfValueType, HierarchyViewerPopup
// ============================================================================
#include "ClassesTab_Shared.h"

void ensureIfValueType(Il2CppObject *currentObj, std::vector<std::string> &paths, Il2CppObject *rootObj)
{
    if (currentObj == rootObj)
        return;
    bool isValueType = Il2cpp::GetClassType(currentObj->klass)->isValueType();
    if (isValueType)
    {
        if (paths.size() > 1)
        {
            auto pathsButLast = std::vector<std::string>(paths.begin(), paths.end() - 1);
            auto [beforeObject, j] = rootObj->dump(pathsButLast, true);

            auto path = paths.rbegin();
            std::istringstream iss(path->c_str());
            std::string _, val;
            iss >> _ >> val;
            void *unboxed = Il2cpp::GetUnboxedValue(currentObj);
            // object->setField(val.c_str(), unboxed);
            auto f = beforeObject->klass->getField(val.c_str());
            if (beforeObject && IsPtrValid(beforeObject)) {
                Il2CppClass *_bk = nullptr;
                if (SafeRead<Il2CppClass*>((const void*)beforeObject, &_bk) && _bk && IsPtrValid(_bk))
                    Il2cpp::SetFieldValue(beforeObject, f, unboxed);
            }
            ensureIfValueType(beforeObject, pathsButLast, rootObj);
        }
        else
        {
            auto path = paths.begin();

            std::istringstream iss(path->c_str());
            std::string _, val;
            iss >> _ >> val;
            void *unboxed = Il2cpp::GetUnboxedValue(currentObj);
            // object->setField(val.c_str(), unboxed);
            auto f = rootObj->klass->getField(val.c_str());
            if (rootObj && IsPtrValid(rootObj)) {
                Il2CppClass *_rk = nullptr;
                if (SafeRead<Il2CppClass*>((const void*)rootObj, &_rk) && _rk && IsPtrValid(_rk))
                    Il2cpp::SetFieldValue(rootObj, f, unboxed);
            }
        }
    }
}


void ClassesTab::ImGuiJson(Il2CppObject *rootObj)
{
    // auto &paths = tool.dataMap[object].second;
    auto &paths = getJsonPaths(rootObj);

    int indentCounter = 0;
    auto currentObj = dataMap[rootObj].first.first;
    for (auto it = paths.begin() + (paths.size() > 3 ? paths.size() - 4 : 0); it != paths.end(); ++it)
    {
        const bool isLast = std::next(it) == paths.end();
        auto key = it->c_str();
        ImGui::PushID(indentCounter);

        bool buttonPressed = false;
        bool isValueType = Il2cpp::GetClassType(currentObj->klass)->isValueType();
        if (isLast && isValueType)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(222, 222, 222, 255));
        }
        if (isLast && currentObj && savedSet[currentObj->klass].count(currentObj) == 0)
        {
            auto width = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 5.f;
            buttonPressed = ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - width, 0));
            if (ImGui::IsItemHeld())
            {
                Tool::OpenNewTabFromClass(currentObj->klass);
                LOGD("OpenNewTabFromObject %p", currentObj);
            }
            ImGui::SameLine();
            char buttonLabel[32]{0};
            sprintf(buttonLabel, "Save");
            if (ImGui::Button(buttonLabel,
                              ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
            {
                savedSet[currentObj->klass].insert(currentObj);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%p", currentObj);
            }
        }
        else
        {
            buttonPressed =
                ImGui::Button(key, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0));
            if (ImGui::IsItemHeld())
            {
                Tool::OpenNewTabFromClass(currentObj->klass);
                LOGD("OpenNewTabFromObject %p", currentObj);
            }
        }
        if (isLast && isValueType)
        {
            ImGui::PopStyleColor();
        }
        ImGui::Indent(10.f);
        indentCounter++;
        ImGui::PopID();

        if (buttonPressed)
        {
            paths.erase(it, paths.end());
            dataMap[rootObj].first = rootObj->dump(paths);
            break;
        }
    }
    if (indentCounter)
        ImGui::Unindent(10.f * indentCounter);

    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 200, 20, 128));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(100, 200, 20, 255));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(100, 200, 20, 255));
    static bool doRefresh = false;
    if (ImGui::Button("Refresh", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)))
    {
        doRefresh = true;
    }
    if (doRefresh)
    {
        doRefresh = false;
        dataMap[rootObj].first = rootObj->dump(paths);
    }
    ImGui::PopStyleColor(3);

    ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(0, 255, 100, 255));
    ImGui::Separator();
    ImGui::PopStyleColor();
    static PopUpSelector poper;
    if (ImGui::BeginTable("sometable", 1))
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("ChildJson",
                          ImVec2(0, ImGui::GetContentRegionAvail().y - (ImGui::GetFontSize() * 1.8f) * 2.f), 0,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const nlohmann::ordered_json &current = getJsonObject(rootObj);
        if (current.empty())
        {
            ImGui::Text("Empty");
        }

        for (auto &[key, value] : current.items())
        {
            // int count = 0;
            if (value.is_object() || value.is_array())
            {
                if (value.is_array() && value.size() == 0)
                {
                    ImGui::Text("%s = [Empty]", key.c_str());
                }
                else if (ImGui::Button(key.c_str(), ImVec2(key.length() <= 3 ? ImGui::GetContentRegionAvail().x -
                                                                                   ImGui::GetStyle().FramePadding.x
                                                                             : 0,
                                                           0)))
                {
                    try
                    {
                        paths.push_back(key);
                        // LOGD("%s", object->dump(paths).dump().c_str());
                        dataMap[rootObj].first = rootObj->dump(paths);
                        break;
                    }
                    catch (nlohmann::json::exception &e)
                    {
                        LOGE("Json exception %s", e.what());
                    }
                    catch (std::exception &e)
                    {
                        LOGE("Exception %s", e.what());
                    }
                }
            }
            else if (value.is_string())
            {
                auto text = value.get<std::string>();
                ImGui::Text("%s = %s", key.c_str(), text.c_str());
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    if (strcmp(type.c_str(), "String") == 0)
                    {
                        Keyboard::Open(
                            text.c_str(),
                            [type = std::move(type), val = std::move(val), currentObj](const std::string &value)
                            {
                                LOGD("%s", value.c_str());
                                auto f = currentObj->klass->getField(val.c_str());
                                auto newStr = Il2cpp::NewString(value.c_str());
                                // Il2cpp::SetFieldValueObject(currentObj, f, newStr);
                                // SafeRead guard sebelum SetFieldValue
                                if (currentObj && IsPtrValid(currentObj)) {
                                    Il2CppClass *_ck = nullptr;
                                    if (SafeRead<Il2CppClass*>((const void*)currentObj, &_ck) && _ck && IsPtrValid(_ck))
                                        Il2cpp::SetFieldValue(currentObj, f, newStr);
                                }
                                doRefresh = true;
                            });
                    }
                    else
                    {
                        auto field = currentObj->klass->getField(val.c_str());
                        auto fieldType = field->getType();
                        if (fieldType->isEnum())
                        {
                            poper.Open(
                                "EnumSelector",
                                [fieldType, currentObj, field](const std::string &result)
                                {
                                    int value = fieldType->getClass()->getField(result.c_str())->getStaticValue<int>();
                                    if (currentObj && IsPtrValid(currentObj)) {
                                        Il2CppClass *_ck = nullptr;
                                        if (SafeRead<Il2CppClass*>((const void*)currentObj, &_ck) && _ck && IsPtrValid(_ck))
                                            Il2cpp::SetFieldValue(currentObj, field, &value);
                                    }
                                    doRefresh = true;
                                },
                                fieldType);
                        }
                    }
                }
            }
            else if (value.is_boolean())
            {
                ImGui::Text("%s = %s", key.c_str(), value.get<bool>() ? "True" : "False");
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string _, val;
                    iss >> _ >> val;
                    poper.Open("BooleanSelector",
                               [currentObj, val, &paths, rootObj](const std::string &value)
                               {
                                   bool b = value == "True";
                                   // split key by space
                                   currentObj->setField(val.c_str(), (int)b);
                                   ensureIfValueType(currentObj, paths, rootObj);
                                   doRefresh = true;
                               });
                }
            }
            else if (value.is_number_float())
            {
                ImGui::Text("%s = %f", key.c_str(), value.get<float>());

                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    Keyboard::Open(std::to_string(value.get<float>()).c_str(),
                                   [type, currentObj, val, &paths, rootObj](const std::string &text)
                                   {
                                       if (strcmp(type.c_str(), "Single") == 0)
                                       {
                                           float value = std::stof(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Double") == 0)
                                       {
                                           double value = std::stod(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       ensureIfValueType(currentObj, paths, rootObj);
                                       doRefresh = true;
                                   });
                }
            }
            else if (value.is_number())
            {
                ImGui::Text("%s = %d", key.c_str(), value.get<int>());
                if (ImGui::IsItemClicked())
                {
                    std::istringstream iss(key);
                    std::string type, val;
                    iss >> type >> val;
                    Keyboard::Open(std::to_string(value.get<int>()).c_str(),
                                   [type, currentObj, val, &paths, rootObj](const std::string &text)
                                   {
                                       if (strcmp(type.c_str(), "Int16") == 0)
                                       {
                                           int16_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt16") == 0)
                                       {
                                           uint16_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Int32") == 0)
                                       {
                                           int32_t value = std::stoi(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt32") == 0)
                                       {
                                           uint32_t value = std::stoul(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "Int64") == 0)
                                       {
                                           int64_t value = std::stoll(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       else if (strcmp(type.c_str(), "UInt64") == 0)
                                       {
                                           uint64_t value = std::stoull(text);
                                           currentObj->setField(val.c_str(), value);
                                       }
                                       ensureIfValueType(currentObj, paths, rootObj);
                                       doRefresh = true;
                                   });
                }
            }
            else
            {
                ImGui::Text("Unk %s %s", key.c_str(), value.type_name());
            }
            ImGui::Separator();
        }

        ImGui::ScrollWhenDraggingOnVoid();
        ImGui::EndChild();

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("bottom");
        if (ImGui::Button("Methods", ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
        {
            ImGui::OpenPopup("MethodPopup");
        }

        static ImGuiIO &io = ImGui::GetIO();
        ImGui::SetNextWindowSizeConstraints(ImVec2(io.DisplaySize.x / 1.2f, 0),
                                            ImVec2(io.DisplaySize.x / 1.2f, io.DisplaySize.y / 2));
        if (ImGui::BeginPopup("MethodPopup", ImGuiWindowFlags_HorizontalScrollbar))
        {
            auto text = currentObj->klass->getName();
            auto windowWidth = ImGui::GetWindowSize().x;
            auto textWidth = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
            ImGui::Text("%s", text);

            ImGui::Separator();

            auto &methods = buildMethodMap(currentObj->klass);
            if (methods.empty())
            {
                ImGui::Text("No methods for class %s", currentObj->klass->getName());
            }
            else
            {
                int j = 0;
                for (auto &[method, paramsInfo] : methods)
                {
                    ImGui::PushID(method + j++);
                    MethodViewer(currentObj->klass, method, paramsInfo, currentObj, true);
                    ImGui::Separator();
                    ImGui::PopID();
                }
            }
            ImGui::EndPopup();
        }

        if (ImGui::Button("Dump to file",
                          ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
        {
            ImGui::OpenPopup("ProceedPopUp");
        }
        if (ImGui::BeginPopup("ProceedPopUp"))
        {
            char fileName[256]{0};
            snprintf(fileName, sizeof(fileName), "dump_%s (%p).json", currentObj->klass->getName(), currentObj);
            ImGui::Text("File will be saved as %s", fileName);
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 50, 255));
            ImGui::Text("Note: This may take a while depending on the size of the object");
            ImGui::Text("Do not touch the screen if it's freezing!");
            ImGui::PopStyleColor();
            if (ImGui::Button("Proceed", ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            {
                ChangeMaxListArraySize(9999,
                                       [&object = currentObj]()
                                       {
                                           char fileName[256]{0};
                                           snprintf(fileName, sizeof(fileName), "dump_%s (%p).json",
                                                    object->klass->getName(), object);
                                           Util::FileWriter file(Util::DirDump(), fileName);
                                           std::vector<uintptr_t> visited{};
                                           nlohmann::ordered_json j = object->dump(visited, 9999);
                                           file.write(j.dump(2, ' ').c_str());
                                           LOGD("Done save");
                                           ImGui::CloseCurrentPopup();
                                       });
            }
            ImGui::EndPopup();
        }
        // ── View Memory button ────────────────────────────────────────────────
        // Tombol ini ada di bottom bar Inspector tab setelah "Dump to file"
        {
            auto &mv = g_memViewStates[rootObj];
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(55,25,110,220));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80,40,160,255));
            if (ImGui::Button("View Memory",
                ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x, 0)))
            {
                mv.target   = rootObj;
                mv.baseAddr = (uintptr_t)rootObj;
                mv.open     = true;
                char mvId[64]; snprintf(mvId, sizeof(mvId), "MemViewerJson_%p", rootObj);
                ImGui::OpenPopup(mvId);
            }
            ImGui::PopStyleColor(2);
            ImGui::SetItemTooltip("Lihat raw memory / hex dump object ini\nAddr: %p", rootObj);

            if (mv.open)
            {
                char mvId[64]; snprintf(mvId, sizeof(mvId), "MemViewerJson_%p", rootObj);
                if (!ImGui::IsPopupOpen(mvId)) ImGui::OpenPopup(mvId);
                ImGui::SetNextWindowSize(
                    ImVec2(ImGui::GetIO().DisplaySize.x * 0.95f,
                           ImGui::GetIO().DisplaySize.y * 0.80f), ImGuiCond_Always);
                ImGui::SetNextWindowPos(
                    ImVec2(ImGui::GetIO().DisplaySize.x * 0.025f,
                           ImGui::GetIO().DisplaySize.y * 0.10f), ImGuiCond_Always);
                if (ImGui::BeginPopupModal(mvId, &mv.open,
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoTitleBar))
                {
                    DrawMemoryViewer(mv);
                    ImGui::EndPopup();
                }
            }
        }

        poper.Update();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

// ═════════════════════════════════════════════════════════════════════════════