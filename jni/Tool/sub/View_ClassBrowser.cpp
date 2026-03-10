// ============================================================================
// View_ClassBrowser.cpp
// Pilar 3: Dipecah dari ClassesTab.cpp (4479 baris)
// Logika UI filtering, listing class, Draw() main loop, StaticReferencesTab
// ============================================================================
#include "ClassesTab_Shared.h"

// ── Forward declaration ──────────────────────────────────────────────────────
static void DrawStaticRefWindows();
// il2cpp function pointer externs sudah via ClassesTab_Shared.h

std::vector<Il2CppImage *> g_Images;
// g_Image extern sudah via ClassesTab_Shared.h

constexpr int MAX_CLASSES = 500;

// ── Forward declarations untuk helper functions ───────────────────────────
// IsTransformType forward decl ada di ClassesTab_Shared.h

// MemViewState struct & g_memViewStates → didefinisikan di View_MemoryHex.cpp
// extern sudah via ClassesTab_Shared.h
// DrawMemoryViewer forward decl ada di ClassesTab_Shared.h

// ── StaticRefResult: hasil scan static instance finder ───────────────────
struct StaticRefResult {
    Il2CppClass  *ownerClass  = nullptr;
    FieldInfo    *field       = nullptr;
    std::string   fieldType;
    uintptr_t     offset      = 0;
    bool          isList      = false;
    bool          isArray     = false;
    std::string   assemblyName;
    std::string   chainHint;
    uint64_t      enumValue   = 0;  // <-- TAMBAH INI
};

// ── Floating window untuk Static References ──
struct StaticRefWindow {
    Il2CppClass* targetClass = nullptr;
    std::vector<StaticRefResult> results;
    bool scanning = false;
    bool open = false;
    char filter[64] = "";
};
static std::unordered_map<Il2CppClass*, StaticRefWindow> g_staticRefWindows;

// Shared cache untuk StaticReferencesTab
static std::unordered_map<Il2CppClass*, std::vector<StaticRefResult>> g_staticRefCache;

// Local copy of FingerScroll (defined in Tool.cpp)
static void FingerScroll(float &prev)
{
    auto &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        if (io.MouseDown[0]) {
            if (prev >= 0.f)
                ImGui::SetScrollY(ImGui::GetScrollY() + (prev - io.MousePos.y));
            prev = io.MousePos.y;
        } else prev = -1.f;
    } else prev = -1.f;
}

// hookerMap, hookerMtx, maxLine didefinisikan di Tool.cpp & View_MethodAnalyzer.cpp
// (tidak perlu didefinisikan ulang di sini — extern sudah via ClassesTab_Shared.h)

void ClassesTab::ImGuiObjectSelector(int id, Il2CppClass *klass, const char *prefix,
                                     std::function<void(Il2CppObject *)> onSelect, bool canNew)
{
    ImGui::PushID(id);
    // ── SAFETY FIX: Use shared_ptr<atomic<bool>> so the thread can safely
    // write the flag even if the UI has moved on or the map was rehashed.
    // The old raw bool& reference was a data race if the map reallocated.
    static std::unordered_map<void *, std::shared_ptr<std::atomic<bool>>> scanState;
    auto &scanPtr = scanState[klass];
    if (!scanPtr) scanPtr = std::make_shared<std::atomic<bool>>(false);
    bool scanning = scanPtr->load(std::memory_order_acquire);

    if (ImGui::Button("Find Objects") && !scanning)
    {
        scanPtr->store(true, std::memory_order_release);
        // Capture shared_ptr by value so the thread always has a valid flag
        auto flagRef = scanPtr;
        // THREAD SAFETY NOTE (Langkah 1):
        // flagRef adalah shared_ptr<atomic<bool>> by value — thread aman meskipun
        // UI ditutup sebelum thread selesai (tidak ada dangling reference).
        // objectMap[klass] ditulis sekaligus (move assignment) — cukup aman karena
        // UI thread hanya membaca saat flagRef->load() == false.
        std::thread(
            [flagRef, klass]()
            {
                auto foundObjects = Il2cpp::GC::FindObjects(klass);
                objectMap[klass] = std::move(foundObjects);
                flagRef->store(false, std::memory_order_release);
            })
            .detach();
    }
    ImGui::PopID();
    ImGuiIO &io = ImGui::GetIO();
    float width = io.DisplaySize.x;
    float height = io.DisplaySize.y;
    // FIXME: DRY!!
    {
        auto &objects = objectMap[klass];
        ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
        ImGui::BeginChild("##ScrollingObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
        {
            ImGui::SeparatorText("Result Object");
            if (objects.empty())
            {
                if (scanning)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(50, 255, 50, 255));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                }
                if (scanning)
                {
                    ImGui::Text("Scanning...");
                }
                else
                {
                    ImGui::Text("Nothing...");
                }
                ImGui::PopStyleColor();
            }
            else
            {
                if (objects.size() > 100)
                {
                    ImGui::Text("Showing 100 of %zu objects", objects.size());
                }
                for (auto it = objects.begin(); it != (objects.size() > 100 ? objects.begin() + 100 : objects.end());)
                {
                    auto object = *it;
                    char buff[64];
                    sprintf(buff, "%s [%p]", prefix, object);
                    auto size = ImGui::GetWindowSize();
                    if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                    {
                        onSelect(object);
                    }
                    ImGui::SetItemTooltip("%s", object->klass->getName());
                    ImGui::SameLine();
                    ImGui::PushID(buff);
                    if (ImGui::Button("Remove"))
                    {
                        it = objects.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60,30,120,220));
                    if (ImGui::Button("Mem", ImVec2(ImGui::GetIO().DisplaySize.x * 0.08f, 0)))
                    {
                        // Simpan ke global state — popup akan dibuka di ClassViewer
                        // level atas (bukan dari dalam nested DumpPopup)
                        auto &mv = g_memViewStates[object];
                        mv.target   = object;
                        mv.baseAddr = (uintptr_t)object;
                        mv.open     = true;
                        // Tutup DumpPopup dulu agar modal bisa terbuka
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    ImGui::SetItemTooltip("View raw memory / hex dump\nKlik: tutup popup ini lalu buka Hex Viewer");
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::Separator();



    {
        char buffer[128];
        sprintf(buffer, "Inherited from %s", klass->getName());
        if (ImGui::CollapsingHeader(buffer))
        {
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingInheritedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // {
                //     char buffer[128];
                //     sprintf(buffer, "Inherited from %s", klass->getName());
                //     ImGui::SeparatorText(buffer);
                // }
                bool empty = true;
                for (auto &[setKlass, _] : savedSet)
                {
                    if (klass != setKlass && Il2cpp::IsClassParentOf(setKlass, klass))
                    {
                        auto &objects = savedSet[setKlass];
                        for (auto it = objects.begin(); it != objects.end();)
                        {
                            empty = false;
                            auto object = *it;
                            char buff[64];
                            sprintf(buff, "%s [%p]", setKlass->getName(), object);
                            auto size = ImGui::GetWindowSize();
                            if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                            {
                                onSelect(object);
                            }
                            ImGui::SetItemTooltip("%s", object->klass->getName());
                            ImGui::SameLine();
                            ImGui::PushID(buff);
                            if (ImGui::Button("Remove"))
                            {
                                it = objects.erase(it);
                            }
                            else
                            {
                                ++it;
                            }
                            ImGui::PopID();
                        }
                    }
                }

                if (empty)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
            }
            ImGui::EndChild();
        }
    }


    {
        if (ImGui::CollapsingHeader("Saved Objects"))
        {
            auto &objects = savedSet[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingSavedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Saved Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
        }
    }


    {
        if (ImGui::CollapsingHeader("Collected Objects"))
        {
            auto &objects = HookerData::collectSet[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingCollectedObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Collected Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", klass->getName(), object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
        }
    }


    {
        if (ImGui::CollapsingHeader("Created Objects"))
        {
            auto &objects = newObjectMap[klass];
            ImGui::SetNextWindowSizeConstraints(ImVec2(-1, 0), ImVec2(-1, height / 3));
            ImGui::BeginChild("##ScrollingNewObjects", ImVec2(width / 1.4f, 0), ImGuiChildFlags_AutoResizeY);
            {
                // ImGui::SeparatorText("Created Objects");
                if (objects.empty())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 50, 50, 255));
                    ImGui::Text("Nothing...");
                    ImGui::PopStyleColor();
                }
                else
                {
                    for (auto it = objects.begin(); it != objects.end();)
                    {
                        auto object = *it;
                        char buff[64];
                        sprintf(buff, "%s [%p]", prefix, object);
                        auto size = ImGui::GetWindowSize();
                        if (ImGui::Button(buff, ImVec2(size.x / 1.5, 0)))
                        {
                            onSelect(object);
                        }
                        ImGui::SetItemTooltip("%s", object->klass->getName());
                        ImGui::SameLine();
                        ImGui::PushID(buff);
                        if (ImGui::Button("Remove"))
                        {
                            it = objects.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                        ImGui::PopID();
                    }
                }
                if (canNew)
                {
                    if (ImGui::Button("New"))
                    {
                        auto newObject = klass->New();
                        newObjectMap[klass].push_back(newObject);
                        if (Il2cpp::GetClassType(klass)->isValueType())
                        {
                            // Il2cpp::GC::KeepAlive(newObject);
                            newObject = (Il2CppObject *)Il2cpp::GetUnboxedValue(newObject);
                        }
                        onSelect(newObject);
                    }
                }
            }
            ImGui::EndChild();
        }
    }

}


void ClassesTab::ClassViewer(Il2CppClass *klass)
{
    if (ImGui::Button("Inspect Objects"))
    {
        ImGui::OpenPopup("DumpPopup");
    }
    ImGui::SameLine();
    // ── Quick Memory View: buka hex viewer langsung dari object pertama ───────
    {
        auto &objList = objectMap[klass];
        // Cari object pertama yang valid dari GC scan (jika sudah pernah scan)
        Il2CppObject *firstObj = nullptr;
        for (auto *o : objList) {
            if (o && IsPtrValid(o)) { firstObj = o; break; }
        }

        bool hasObj = (firstObj != nullptr);
        if (!hasObj) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(55,25,110,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80,40,160,255));
        if (ImGui::Button("Mem View"))
        {
            if (firstObj) {
                auto &mv = g_memViewStates[firstObj];
                mv.target   = firstObj;
                mv.baseAddr = (uintptr_t)firstObj;
                mv.open     = true;
            }
        }
        ImGui::PopStyleColor(2);
        if (!hasObj) {
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Scan objects dulu lewat 'Inspect Objects' -> 'Find Objects'");
        } else {
            ImGui::SetItemTooltip("Buka hex viewer untuk: %p\nScan lebih banyak lewat Inspect Objects", firstObj);
        }
    }
    ImGui::SameLine();
    // ── Dump Class: export satu class ke file .cs ────────────────────────────
    if (ImGui::Button("Dump Class"))
    {
        // Generate CS-style dump untuk class yang dipilih saja
        std::ostringstream cs;
        const char *ns    = klass->getNamespace();
        const char *cn    = klass->getName();
        std::string fullN = klass->getFullName();

        cs << "// ================================================================\n";
        cs << "// Class Dump: " << fullN << "\n";
        cs << "// Generated by IL2CPP Tool\n";
        cs << "// ================================================================\n\n";

        if (ns && ns[0] != '\0')
            cs << "namespace " << ns << "\n{\n";

        cs << "public class " << (cn ? cn : "Unknown") << "\n{\n";

        // ── Fields ────────────────────────────────────────────────────────────
        cs << "    // === Fields ===\n";
        for (auto *field : klass->getFields())
        {
            if (!field) continue;
            auto *ft = field->getType();
            cs << "    public "
               << (ft ? ft->getName() : "object") << " "
               << (field->getName() ? field->getName() : "field")
               << "; // offset: 0x" << std::hex << field->getOffset() << std::dec << "\n";
        }

        // ── Methods ───────────────────────────────────────────────────────────
        cs << "\n    // === Methods ===\n";
        for (auto *method : klass->getMethods())
        {
            if (!method) continue;
            auto *rt = method->getReturnType();
            cs << "    public ";
            if (Il2cpp::GetIsMethodStatic(method)) cs << "static ";
            cs << (rt ? rt->getName() : "void") << " "
               << (method->getName() ? method->getName() : "Method") << "(";

            auto params = method->getParamsInfo();
            for (size_t i = 0; i < params.size(); i++)
            {
                auto &[pname, ptype] = params[i];
                cs << (ptype ? ptype->getName() : "object") << " "
                   << (pname ? pname : "arg");
                if (i + 1 < params.size()) cs << ", ";
            }
            cs << ")";

            if (method->methodPointer)
            {
                uintptr_t il2cppBase = (uintptr_t)GetLibBase(GetTargetLib());
                uintptr_t abs = (uintptr_t)method->methodPointer;
                uintptr_t rva = (il2cppBase > 0 && abs > il2cppBase) ? (abs - il2cppBase) : abs;
                cs << " // RVA: 0x" << std::hex << rva << std::dec;
            }
            cs << "\n";
        }

        cs << "}\n";
        if (ns && ns[0] != '\0') cs << "}\n";

        // Simpan ke file
        std::string safeName = cn ? cn : "class";
        // Ganti karakter tidak valid
        std::replace(safeName.begin(), safeName.end(), '/', '_');
        std::replace(safeName.begin(), safeName.end(), '<', '_');
        std::replace(safeName.begin(), safeName.end(), '>', '_');

        std::string fname = safeName + "_dump.cs";
        Util::FileWriter fw(Util::DirDump(), fname);
        fw.write(cs.str().c_str());

        // Buka keyboard untuk tampilkan path
        Keyboard::Open((Util::DirDump() + "/" + fname).c_str(), [](const std::string&){});
    }
    {
        ImGui::SameLine();
        bool &state = states[klass];
        char label[12]{0};
        if (!state)
            sprintf(label, "Trace all");
        else
            sprintf(label, "Restore");
        if (ImGui::Button(label))
        {
            if (!state)
            {
                ImGui::OpenPopup("ConfirmPopup");
            }
            else
            {
                state = false;
                for (auto &[method, paramsInfo] : methodMap[klass])
                {
                    if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                        continue;

                    Tool::ToggleHooker(method, 0);
                }
            }
        }
        if (ImGui::BeginPopup("ConfirmPopup"))
        {
            ImGui::TextColored(ImVec4(0.8, 0.8, 0, 1), "WARNING: There's a high-risk of crash");
            if (ImGui::Button("Continue?"))
            {
                state = true;
                // BUG FIX (Langkah 1 - Thread Safety):
                // Capture semua data yang dibutuhkan thread by value agar tidak
                // ada dangling reference ke local/member data jika UI berubah.
                // Gunakan shared_ptr<atomic<bool>> untuk track status selesai.
                auto hookDone = std::make_shared<std::atomic<bool>>(false);
                auto methodMapCopy = methodMap[klass]; // copy by value — safe
                std::thread([hookDone, methodMapCopy]()
                {
                    for (auto &[method, paramsInfo] : methodMapCopy)
                    {
                        if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                            continue;
                        Tool::ToggleHooker(method, 1);
                    }
                    hookDone->store(true, std::memory_order_release);
                }).detach();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    // ImGui::SameLine();
    // if (ImGui::Button("Add to Tracer"))
    // {
    //     tracer.push_back(klass);
    // }
    if (ImGui::BeginPopup("DumpPopup"))
    {
        ImGuiObjectSelector(ImGui::GetID("ObjectSelector"), klass, "Inspect",
                            [this](Il2CppObject *object) { setJsonObject(object); });
        ImGui::EndPopup();
    }

    // ── Memory Viewer Modal — dibuka dari tombol Mem di ImGuiObjectSelector ───
    // Cek apakah ada object dari kelas ini yang punya MemViewState terbuka
    {
        auto &objList = objectMap[klass];
        for (auto *obj : objList) {
            if (!obj) continue;
            auto it2 = g_memViewStates.find(obj);
            if (it2 != g_memViewStates.end() && it2->second.open) {
                char mvPopupId[64];
                snprintf(mvPopupId, sizeof(mvPopupId), "MemViewer_%p", obj);
                // Buka popup sekali
                if (!ImGui::IsPopupOpen(mvPopupId))
                    ImGui::OpenPopup(mvPopupId);
                ImGui::SetNextWindowSize(
                    ImVec2(ImGui::GetIO().DisplaySize.x * 0.95f,
                           ImGui::GetIO().DisplaySize.y * 0.80f), ImGuiCond_Always);
                ImGui::SetNextWindowPos(
                    ImVec2(ImGui::GetIO().DisplaySize.x * 0.025f,
                           ImGui::GetIO().DisplaySize.y * 0.10f), ImGuiCond_Always);
                if (ImGui::BeginPopupModal(mvPopupId, &it2->second.open,
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoTitleBar))
                {
                    DrawMemoryViewer(it2->second);
                    ImGui::EndPopup();
                }
                if (!it2->second.open)
                    g_memViewStates.erase(it2);
                break; // hanya satu modal sekaligus
            }
        }
    }

    ImGui::Separator();

    // ── BUG FIX: Tampilkan field atau method sesuai mode filter aktif ─────────
    if (filterByField)
    {
        // Mode Field: tampilkan FieldViewer bukan method list
        FieldViewer(klass);
    }
    else
    {
        // Mode normal: tampilkan method list + Developer Kit tabs
        if (ImGui::BeginTabBar("##classViewerTabs"))
        {
            // ── Tab Methods (default) ──────────────────────────────────────
            if (ImGui::BeginTabItem("Methods"))
            {
                int j = 0;
                for (auto &[method, paramsInfo] : methodMap[klass])
                {
                    ImGui::PushID(method + j++);
                    MethodViewer(klass, method, paramsInfo);
                    ImGui::Separator();
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }

            // ── Tab References (Fitur 1: Static Instance Finder) ───────────
            if (ImGui::BeginTabItem("References"))
            {
                StaticReferencesTab(klass);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
}


void ClassesTab::Draw(int index, bool closeable)
{
    static ImGuiIO &io = ImGui::GetIO();
    char tabLabel[256];
    if (filter.empty())
    {
        sprintf(tabLabel, "Classes");
        if (index >= 0)
            sprintf(tabLabel, "Classes [%d]", index + 1);
    }
    else
    {
        sprintf(tabLabel, "%s", filter.c_str());
    }

    if ((currentlyOpened = ImGui::BeginTabItem(tabLabel, closeable ? &opened : nullptr,
                                               setOpenedTab ? ImGuiTabItemFlags_SetSelected : 0)))
    {
        setOpenedTab = false;
        ImGui::BeginDisabled(includeAllImages);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(-1, io.DisplaySize.y / 1.5f));
        if (ImGui::BeginCombo("Image##ImageSelector", selectedImage->getName()))
        {

            for (int i = 0; i < g_Images.size(); i++)
            {
                bool selected = selectedImageIndex == i;
                if (ImGui::Selectable(g_Images[i]->getName(), selected))
                {
                    selectedImage = g_Images[i];
                    selectedImageIndex = i;
                    FilterClasses(filter);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Checkbox("All", &includeAllImages))
        {
            FilterClasses(filter);
        }

        char filterBuffer[256];
        sprintf(filterBuffer, "Filter : %s | %zu of %zu", filter.empty() ? "(none)" : filter.c_str(),
                filteredClasses.size(), classes.size());
        if (ImGui::Button(filterBuffer, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f)) && !Keyboard::IsOpen())
        {
            Keyboard::Open(
                [this](const std::string &text)
                {
                    filter = text;
                    FilterClasses(filter);
                });
        }
        if (!Keyboard::IsOpen() && ImGui::IsItemHeld())
        {
            Keyboard::Open(filter.c_str(),
                           [this](const std::string &text)
                           {
                               filter = text;
                               FilterClasses(filter);
                           });
        }
        if (ImGui::Button("Filter Options"))
        {
            ImGui::OpenPopup("FilterOptions");
        }

        // ── Export ALL patch aktif ke satu file Lua ───────────────────────────
        {
            int patchCount = 0;
            int hookCount  = 0;
            for (auto& [m, data] : oMap)      if (!data.bytes.empty()) patchCount++;
            for (auto& [p, data] : hookerMap) hookCount++;

            if (patchCount > 0) {
                // ─────────────────────────────────────────────────────────────
                // TOMBOL 1: "Patched (NP)" → popup patch manager
                // Ganti dari "Export Lua" karena Lua Menu sudah handle export.
                // Fungsi baru: lihat semua yang sedang di-patch, restore individual / restore all.
                // ─────────────────────────────────────────────────────────────
                ImGui::SameLine();
                char patchedLabel[48];
                snprintf(patchedLabel, sizeof(patchedLabel), "Patched (%dM)", patchCount);
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 80, 20, 220));
                if (ImGui::Button(patchedLabel))
                    ImGui::OpenPopup("##PatchManager");
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Lihat & kelola semua method yang sedang di-patch\nRestore individual atau sekaligus");

                // ── Popup Patch Manager ───────────────────────────────────────
                ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x * 0.90f, 0), ImGuiCond_Always);
                if (ImGui::BeginPopup("##PatchManager"))
                {
                    uintptr_t il2cppBase2 = (uintptr_t)GetLibBase(GetTargetLib());

                    // ── Header + Restore ALL ──────────────────────────────────
                    ImGui::TextColored(ImVec4(1.f,.6f,.1f,1.f), "Patch Manager");
                    ImGui::SameLine();
                    ImGui::TextDisabled("| %d method aktif", patchCount);
                    ImGui::Separator();

                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(160,30,30,220));
                    if (ImGui::Button("Restore ALL", ImVec2(-1, 0)))
                    {
                        for (auto &[m, data] : oMap)
                        {
                            if (data.bytes.empty() || !m || !m->methodPointer) continue;
                            KittyMemory::ProtectAddr(m->methodPointer, data.bytes.size(),
                                                     PROT_READ | PROT_WRITE | PROT_EXEC);
                            std::memcpy(m->methodPointer, data.bytes.data(), data.bytes.size());
                            __builtin___clear_cache((char*)m->methodPointer,
                                                    (char*)m->methodPointer + data.bytes.size());
                            data.bytes.clear(); data.text.clear();
                        }
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();

                    ImGui::Spacing();

                    // ── List method patches ───────────────────────────────────
                    int idx = 0;
                    for (auto &[m, data] : oMap)
                    {
                        if (data.bytes.empty() || !m || !m->methodPointer) continue;
                        ImGui::PushID(idx++);

                        auto *klass2 = m->getClass();
                        const char *cn2 = klass2 && klass2->getName() ? klass2->getName() : "?";
                        const char *mn2 = m->getName() ? m->getName() : "?";
                        uintptr_t rvaDisp = 0;
                        if (il2cppBase2 && (uintptr_t)m->methodPointer > il2cppBase2)
                            rvaDisp = (uintptr_t)m->methodPointer - il2cppBase2;

                        // Satu baris per patch: [Restore] ClassName::Method  ->  Value  |  RVA
                        float rowH = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18,22,32,220));
                        ImGui::BeginChild("##pm_row", ImVec2(0, rowH), true);

                        // Baris 1: nama + nilai
                        ImGui::TextColored(ImVec4(.3f,1.f,.4f,1.f), "%s", cn2);
                        ImGui::SameLine(0,2); ImGui::TextDisabled("::"); ImGui::SameLine(0,2);
                        ImGui::TextColored(ImVec4(.6f,.9f,1.f,1.f), "%s", mn2);
                        ImGui::SameLine(); ImGui::TextDisabled("->"); ImGui::SameLine();
                        ImGui::TextColored(ImVec4(1.f,.85f,.2f,1.f), "%s", data.text.c_str());

                        // Baris 2: RVA + tombol restore sejajar kanan
                        ImGui::TextDisabled("RVA 0x%zX", rvaDisp);
                        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 58);
                        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150,30,30,200));
                        if (ImGui::Button("Restore", ImVec2(ImGui::GetIO().DisplaySize.x * 0.18f, 0)))
                        {
                            KittyMemory::ProtectAddr(m->methodPointer, data.bytes.size(),
                                                     PROT_READ | PROT_WRITE | PROT_EXEC);
                            std::memcpy(m->methodPointer, data.bytes.data(), data.bytes.size());
                            __builtin___clear_cache((char*)m->methodPointer,
                                                    (char*)m->methodPointer + data.bytes.size());
                            data.bytes.clear(); data.text.clear();
                        }
                        ImGui::PopStyleColor();

                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }

                    // ── Field patches ─────────────────────────────────────────
                    int fPatchCount = 0;
                    for (auto &[f,pd] : fieldPatchMap) if (pd.active) fPatchCount++;
                    if (fPatchCount > 0)
                    {
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(.7f,.5f,1.f,1.f), "Field Patches");
                        ImGui::SameLine(); ImGui::TextDisabled("| %d aktif", fPatchCount);
                        ImGui::Spacing();

                        int fi = 0;
                        for (auto &[f, pd] : fieldPatchMap)
                        {
                            if (!pd.active || !f) continue;
                            ImGui::PushID(1000 + fi++);
                            const char *fn  = f->getName() ? f->getName() : "?";
                            auto *fft       = f->getType();
                            const char *ftn = fft ? fft->getName() : "?";
                            uintptr_t foff  = f->getOffset();

                            float rowHF = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
                            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(22,18,32,220));
                            ImGui::BeginChild("##pm_frow", ImVec2(0, rowHF), true);

                            // Baris 1: nama field = nilai
                            ImGui::TextColored(ImVec4(.7f,.5f,1.f,1.f), "%s", fn);
                            ImGui::SameLine(); ImGui::TextDisabled("="); ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.f,.85f,.2f,1.f), "%s", pd.text.c_str());
                            ImGui::SameLine(); ImGui::TextDisabled("(%s)", ftn);

                            // Baris 2: offset + static/inst + tombol stop kanan
                            ImGui::TextDisabled("0x%zX | %s", foff, pd.isStatic ? "Static" : "Instance");
                            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 40);
                            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150,30,30,200));
                            if (ImGui::Button("Stop", ImVec2(ImGui::GetIO().DisplaySize.x * 0.15f, 0)))
                            {
                                pd.active = false; pd.text.clear();
                                pd.valueBytes.clear(); pd.patchedInstance = nullptr;
                            }
                            ImGui::PopStyleColor();

                            ImGui::EndChild();
                            ImGui::PopStyleColor();
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndPopup();
                }

                // ─────────────────────────────────────────────────────────────
                // TOMBOL 2: "Lua Menu" → Enhanced dengan:
                //   - getBase() cari ELF header (r--p offset 0) bukan sekedar getRangesList()[1]
                //   - Support multiple libil2cpp (game yang pakai beberapa so)
                //   - while true do dengan gg.isVisible() check
                //   - Field patch via gg.setValues tiap iterasi menu
                // ─────────────────────────────────────────────────────────────
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(20, 120, 60, 220));
                if (ImGui::Button("Lua Menu"))
                {
                    // Kumpulkan info size dari proses saat ini untuk filter ELF
                    uintptr_t il2cppBase3 = (uintptr_t)GetLibBase(GetTargetLib());

                    // Helper: reconstruct bytes — sama seperti sebelumnya
                    auto makeBytes3 = [](MethodInfo* m, const std::string& text) -> std::vector<uint8_t> {
                        auto rt = m->getReturnType();
                        std::string tn = rt ? rt->getName() : "";
                        if (text == "NOP" || tn == "System.Void") return {0xC0,0x03,0x5F,0xD6};
                        if (tn == "System.Boolean") {
                            bool bv = (text=="True"||text=="true"||text=="1");
                            if (bv) return {0x20,0x00,0x80,0x52,0xC0,0x03,0x5F,0xD6};
                            else    return {0x00,0x00,0x80,0x52,0xC0,0x03,0x5F,0xD6};
                        }
                        if (tn=="System.Int32"||tn=="System.UInt32") {
                            try { uint32_t v=(uint32_t)std::stol(text); uint32_t ldr=0x18000040;
                                  std::vector<uint8_t> b(12);
                                  memcpy(b.data(),&ldr,4); memcpy(b.data()+4,"\xC0\x03\x5F\xD6",4);
                                  memcpy(b.data()+8,&v,4); return b; } catch(...) {}
                        }
                        if (tn=="System.Int64"||tn=="System.UInt64") {
                            try { uint64_t v=(uint64_t)std::stoll(text); uint32_t ldr=0x58000040;
                                  std::vector<uint8_t> b(16);
                                  memcpy(b.data(),&ldr,4); memcpy(b.data()+4,"\xC0\x03\x5F\xD6",4);
                                  memcpy(b.data()+8,&v,8); return b; } catch(...) {}
                        }
                        if (tn=="System.Single") {
                            try { float v=std::stof(text); uint32_t ldr=0x1C000040;
                                  std::vector<uint8_t> b(12);
                                  memcpy(b.data(),&ldr,4); memcpy(b.data()+4,"\xC0\x03\x5F\xD6",4);
                                  memcpy(b.data()+8,&v,4); return b; } catch(...) {}
                        }
                        return {0xC0,0x03,0x5F,0xD6};
                    };

                    // Helper: bytes to spaced hex string
                    auto bytesToHex = [](const std::vector<uint8_t>& b) -> std::string {
                        std::ostringstream s;
                        for (size_t i=0;i<b.size();i++){
                            char h[4]; snprintf(h,sizeof(h),"%02X",b[i]);
                            if (i) s<<" "; s<<h;
                        }
                        return s.str();
                    };

                    // Kumpulkan patches
                    struct LuaMenuEntry {
                        std::string featureName;
                        std::string label;
                        uintptr_t rva;
                        std::string hexON;
                        std::string hexOFF;
                        // Field patch info
                        bool isField = false;
                        uintptr_t fieldOffset = 0;
                        std::string fieldValue;
                        std::string fieldType;
                        bool fieldIsStatic = false;
                    };
                    std::vector<LuaMenuEntry> entries;

                    // Method patches
                    for (auto &[m, data] : oMap) {
                        if (data.bytes.empty() || !m || !m->methodPointer) continue;
                        auto *klass2 = m->getClass();
                        std::string cn = klass2 && klass2->getName() ? klass2->getName() : "Unknown";
                        std::string mn2 = m->getName() ? m->getName() : "method";
                        uintptr_t abs = (uintptr_t)m->methodPointer;
                        uintptr_t rva = (il2cppBase3 && abs > il2cppBase3) ? (abs - il2cppBase3) : abs;
                        auto pb  = makeBytes3(m, data.text);

                        LuaMenuEntry e;
                        // Safe feature name
                        e.featureName = cn + "_" + mn2;
                        for (char &c : e.featureName) if (!isalnum(c)) c='_';
                        e.label  = cn + "::" + mn2 + " = " + data.text;
                        e.rva    = rva;
                        e.hexON  = bytesToHex(pb);
                        e.hexOFF = bytesToHex(data.bytes);
                        entries.push_back(e);
                    }

                    // Field patches
                    for (auto &[f, pd] : fieldPatchMap) {
                        if (!pd.active || !f) continue;
                        const char *fn = f->getName() ? f->getName() : "field";
                        auto *fft = f->getType();
                        const char *ftn = fft ? fft->getName() : "?";
                        LuaMenuEntry e;
                        e.isField = true;
                        e.featureName = std::string("Field_") + fn;
                        for (char &c : e.featureName) if (!isalnum(c)) c='_';
                        e.label = std::string("[Field] ") + fn + " = " + pd.text;
                        e.fieldOffset = f->getOffset();
                        e.fieldValue  = pd.text;
                        e.fieldType   = ftn;
                        e.fieldIsStatic = pd.isStatic;
                        entries.push_back(e);
                    }

                    if (!entries.empty())
                    {
                        // ── Dapatkan libSize dari /proc/self/maps untuk ELF filter ──
                        size_t elfLibSize = 0;
                        {
                            FILE *fp = fopen("/proc/self/maps","r");
                            if (fp) {
                                char line[512]; uintptr_t fStart=0, lEnd=0;
                                while (fgets(line,sizeof(line),fp)) {
                                    if (!strstr(line,GetTargetLib())) continue;
                                    uintptr_t s=0,e=0;
                                    sscanf(line,"%lx-%lx",&s,&e);
                                    if (!fStart) fStart=s;
                                    if (e>lEnd) lEnd=e;
                                }
                                fclose(fp);
                                if (fStart && lEnd>fStart) elfLibSize = lEnd-fStart;
                            }
                        }

                        std::ostringstream lua;
                        lua << "-- =================================================================\n";
                        lua << "-- IL2CPP Mod Menu — Auto Generated by IL2CPP Tool\n";
                        lua << "-- ELF Base Detection: filter region r--p offset 0 + ELF magic\n";
                        lua << "-- Field Patch: tulis value ke instance+offset tiap iterasi\n";
                        lua << "-- =================================================================\n\n";

                        // ── getBase: ELF-aware, cari region yang header-nya \x7fELF ──
                        // Cara paling akurat di GG: baca 4 byte pertama region,
                        // cocokkan dengan ELF magic, DAN pastikan region punya r--p / r-xp.
                        // Ini menghindari salah ambil region di game dengan multiple so.
                        lua << "-- getBase: cari ELF base yang benar (tahan multiple lib)\n";
                        lua << "local function getBase(libName, libSize)\n";
                        lua << "    local ranges = gg.getRangesList(libName)\n";
                        lua << "    if not ranges or #ranges == 0 then\n";
                        lua << "        gg.alert('Library tidak ditemukan: ' .. libName)\n";
                        lua << "        return nil\n";
                        lua << "    end\n";
                        lua << "    -- Cari region dengan ELF header (\\x7fELF) = base yang benar\n";
                        lua << "    for _, r in ipairs(ranges) do\n";
                        lua << "        local ok, bytes = pcall(gg.readBytes, r.start, 4)\n";
                        lua << "        if ok and bytes and #bytes == 4 then\n";
                        lua << "            -- ELF magic: 0x7F 'E' 'L' 'F'\n";
                        lua << "            if bytes[1]==0x7F and bytes[2]==0x45 and bytes[3]==0x4C and bytes[4]==0x46 then\n";
                        lua << "                return r.start\n";
                        lua << "            end\n";
                        lua << "        end\n";
                        lua << "    end\n";
                        lua << "    -- Fallback: cocokkan ukuran total region\n";
                        if (elfLibSize > 0) {
                            lua << "    local targetSize = " << elfLibSize << "\n";
                            lua << "    for _, r in ipairs(ranges) do\n";
                            lua << "        if math.abs((r['end'] - r.start) - targetSize) < 0x10000 then\n";
                            lua << "            return r.start\n";
                            lua << "        end\n";
                            lua << "    end\n";
                        }
                        lua << "    -- Last resort: region pertama\n";
                        lua << "    return ranges[1].start\n";
                        lua << "end\n\n";

                        // ── writePatch: tulis spaced hex ke addr ──
                        lua << "local function writePatch(addr, hexStr)\n";
                        lua << "    local t, i = {}, 0\n";
                        lua << "    for h in hexStr:gmatch('%S%S') do\n";
                        lua << "        t[#t+1] = {address=addr+i, flags=gg.TYPE_BYTE, value=h..'r'}\n";
                        lua << "        i = i + 1\n";
                        lua << "    end\n";
                        lua << "    if #t > 0 then gg.setValues(t) end\n";
                        lua << "end\n\n";

                        // ── FEATURES table ──
                        lua << "local BASE = getBase('libil2cpp.so'," << elfLibSize << ")\n";
                        lua << "if not BASE then return end\n\n";

                        lua << "local FEATURES = {\n";
                        for (size_t i=0;i<entries.size();i++) {
                            const auto &e = entries[i];
                            lua << "    [" << (i+1) << "] = {\n";
                            lua << "        name    = \"" << e.label << "\",\n";
                            lua << "        enabled = false,\n";
                            if (!e.isField) {
                                lua << "        isField = false,\n";
                                lua << "        addr    = BASE + 0x" << std::hex << e.rva << std::dec << ",\n";
                                lua << "        hexON   = \"" << e.hexON  << "\",\n";
                                lua << "        hexOFF  = \"" << e.hexOFF << "\",\n";
                            } else {
                                lua << "        isField  = true,\n";
                                lua << "        -- Field patch: tidak ada ASM, tulis value ke instance+offset tiap frame\n";
                                lua << "        -- Perlu cari instance terlebih dahulu (scan GC atau pointer)\n";
                                lua << "        fieldOff = 0x" << std::hex << e.fieldOffset << std::dec << ",\n";
                                lua << "        fieldVal = " << e.fieldValue << ",  -- " << e.fieldType << "\n";
                                lua << "        isStatic = " << (e.fieldIsStatic ? "true" : "false") << ",\n";
                                lua << "        inst     = nil,  -- isi manual dengan pointer instance\n";
                            }
                            lua << "    },\n";
                        }
                        lua << "}\n\n";

                        // ── Helper ON/OFF ──
                        lua << "local function applyFeature(f)\n";
                        lua << "    if f.isField then\n";
                        lua << "        -- Field patch: butuh instance pointer dari user / scan\n";
                        lua << "        if not f.inst then\n";
                        lua << "            gg.toast('Set f.inst dulu untuk field: ' .. f.name)\n";
                        lua << "            return\n";
                        lua << "        end\n";
                        lua << "        local addr = f.inst + f.fieldOff\n";
                        lua << "        gg.setValues({{address=addr, flags=gg.TYPE_FLOAT, value=f.fieldVal}})\n";
                        lua << "    else\n";
                        lua << "        if f.enabled then\n";
                        lua << "            writePatch(f.addr, f.hexON)\n";
                        lua << "            gg.toast('\\xE2\\x9C\\x85 ON: ' .. f.name)\n";
                        lua << "        else\n";
                        lua << "            writePatch(f.addr, f.hexOFF)\n";
                        lua << "            gg.toast('\\xE2\\x9D\\x8C OFF: ' .. f.name)\n";
                        lua << "        end\n";
                        lua << "    end\n";
                        lua << "end\n\n";

                        // ── buildMenu ──
                        lua << "local function buildMenu()\n";
                        lua << "    local items = {}\n";
                        lua << "    for _, f in ipairs(FEATURES) do\n";
                        lua << "        local icon\n";
                        lua << "        if f.isField then\n";
                        lua << "            icon = f.enabled and '\\xF0\\x9F\\x94\\x81 ' or '\\xF0\\x9F\\x94\\xB4 '\n";
                        lua << "        else\n";
                        lua << "            icon = f.enabled and '\\xE2\\x9C\\x85 ON  ' or '\\xE2\\x9D\\x8C OFF '\n";
                        lua << "        end\n";
                        lua << "        items[#items+1] = icon .. f.name\n";
                        lua << "    end\n";
                        lua << "    items[#items+1] = '\\xE2\\x9E\\x95 Semua ON'\n";
                        lua << "    items[#items+1] = '\\xE2\\x9E\\x96 Semua OFF'\n";
                        lua << "    items[#items+1] = '\\xF0\\x9F\\x9A\\xAA Keluar'\n";
                        lua << "    return items\n";
                        lua << "end\n\n";

                        // ── Main loop: while true do + gg.isVisible() ──
                        lua << "-- Main loop — berjalan terus hingga user pilih Keluar\n";
                        lua << "-- gg.isVisible(): true = GG overlay tampil, bisa tampilkan menu\n";
                        lua << "local running = true\n";
                        lua << "while running do\n";
                        lua << "    gg.sleep(100)\n";
                        lua << "    if not gg.isVisible() then\n";
                        lua << "        -- GG tidak tampil, re-apply field patch tiap iterasi jika enabled\n";
                        lua << "        for _, f in ipairs(FEATURES) do\n";
                        lua << "            if f.isField and f.enabled and f.inst then\n";
                        lua << "                local ok = pcall(gg.setValues, {{address=f.inst+f.fieldOff, flags=gg.TYPE_FLOAT, value=f.fieldVal}})\n";
                        lua << "            end\n";
                        lua << "        end\n";
                        lua << "    else\n";
                        lua << "        local n      = #FEATURES\n";
                        lua << "        local choice = gg.choice(buildMenu(), nil, 'IL2CPP Mod Menu | ' .. n .. ' Fitur')\n";
                        lua << "        gg.setVisible(false)  -- tutup overlay setelah pilihan\n";
                        lua << "        if not choice then\n";
                        lua << "            -- user tekan back: tidak keluar, lanjut loop\n";
                        lua << "        elseif choice <= n then\n";
                        lua << "            local f = FEATURES[choice]\n";
                        lua << "            f.enabled = not f.enabled\n";
                        lua << "            applyFeature(f)\n";
                        lua << "        elseif choice == n+1 then\n";
                        lua << "            for _, f in ipairs(FEATURES) do f.enabled=true; applyFeature(f) end\n";
                        lua << "        elseif choice == n+2 then\n";
                        lua << "            for _, f in ipairs(FEATURES) do f.enabled=false; applyFeature(f) end\n";
                        lua << "        elseif choice == n+3 then\n";
                        lua << "            running = false\n";
                        lua << "        end\n";
                        lua << "    end\n";
                        lua << "end\n";
                        lua << "print('[Menu] Selesai.')\n";

                        std::string gameVer  = Il2cpp::getGameVersion();
                        std::string projName = Il2cpp::getPackageName();
                        std::string fname3   = projName + "_" + gameVer + "_menu.lua";
                        Util::FileWriter fw3(Util::DirCodegen(), fname3);
                        fw3.write(lua.str().c_str());
                        Keyboard::Open((Util::DirCodegen() + "/" + fname3).c_str(), [](const std::string&){});
                    }
                }
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Generate GG Lua script:\n- ELF-aware base detection\n- while true loop + gg.isVisible()\n- Method patch ON/OFF toggle\n- Field patch tiap frame");

                // ─────────────────────────────────────────────────────────────
                // TOMBOL 3: "AIDE .h" → Info lengkap simpel, tanpa implementasi
                // Output: header berisi semua info (class, method, RVA, offset,
                // value, instance) + komentar cara pakai. Bukan boilerplate hook.
                // ─────────────────────────────────────────────────────────────
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(20, 60, 160, 220));
                if (ImGui::Button("AIDE .h"))
                {
                    uintptr_t il2cppBase4 = (uintptr_t)GetLibBase(GetTargetLib());
                    std::string gameVer4  = Il2cpp::getGameVersion();
                    std::string pkgName4  = Il2cpp::getPackageName();

                    // Helper: bytes -> "0xAB, 0xCD, ..."
                    auto bytesToHexStr = [](const std::vector<uint8_t>& b) -> std::string {
                        std::ostringstream s;
                        for (size_t i=0;i<b.size();i++){
                            char h[8]; snprintf(h,sizeof(h),"0x%02X",b[i]);
                            if(i) s<<", "; s<<h;
                        }
                        return s.str();
                    };

                    // Helper: reconstruct patch bytes
                    auto makeBytes4 = [](MethodInfo* m, const std::string& text) -> std::vector<uint8_t> {
                        auto rt = m->getReturnType();
                        std::string tn = rt ? rt->getName() : "";
                        if (text=="NOP"||tn=="System.Void") return {0xC0,0x03,0x5F,0xD6};
                        if (tn=="System.Boolean") {
                            bool bv=(text=="True"||text=="true"||text=="1");
                            return bv ? std::vector<uint8_t>{0x20,0x00,0x80,0x52,0xC0,0x03,0x5F,0xD6}
                                      : std::vector<uint8_t>{0x00,0x00,0x80,0x52,0xC0,0x03,0x5F,0xD6};
                        }
                        if (tn=="System.Int32"||tn=="System.UInt32") {
                            try { uint32_t v=(uint32_t)std::stol(text); uint32_t ldr=0x18000040;
                                  std::vector<uint8_t> b(12); memcpy(b.data(),&ldr,4);
                                  memcpy(b.data()+4,"\xC0\x03\x5F\xD6",4); memcpy(b.data()+8,&v,4); return b;
                            } catch(...) {}
                        }
                        if (tn=="System.Int64"||tn=="System.UInt64") {
                            try { uint64_t v=(uint64_t)std::stoll(text); uint32_t ldr=0x58000040;
                                  std::vector<uint8_t> b(16); memcpy(b.data(),&ldr,4);
                                  memcpy(b.data()+4,"\xC0\x03\x5F\xD6",4); memcpy(b.data()+8,&v,8); return b;
                            } catch(...) {}
                        }
                        if (tn=="System.Single") {
                            try { float v=std::stof(text); uint32_t ldr=0x1C000040;
                                  std::vector<uint8_t> b(12); memcpy(b.data(),&ldr,4);
                                  memcpy(b.data()+4,"\xC0\x03\x5F\xD6",4); memcpy(b.data()+8,&v,4); return b;
                            } catch(...) {}
                        }
                        return {0xC0,0x03,0x5F,0xD6};
                    };

                    std::ostringstream h;
                    h << "// =============================================================\n";
                    h << "// IL2CPP Patch Info — " << pkgName4 << " v" << gameVer4 << "\n";
                    h << "// Generated by IL2CPP Tool\n";
                    h << "// =============================================================\n";
                    h << "// Cara pakai method patch:\n";
                    h << "//   uintptr_t base = GetLibBase(\"libil2cpp.so\");\n";
                    h << "//   PatchMemory(base + RVA_xxx, bytes_xxx_on, sizeof(bytes_xxx_on));\n";
                    h << "// Cara pakai field patch:\n";
                    h << "//   *(type*)(instance_ptr + OFFSET_xxx) = value; // tiap frame\n";
                    h << "// =============================================================\n";
                    h << "#pragma once\n\n";

                    // ── Method Patches ────────────────────────────────────────
                    bool hasMethod = false;
                    for (auto &[m, data] : oMap)
                        if (!data.bytes.empty() && m && m->methodPointer) { hasMethod=true; break; }

                    if (hasMethod)
                    {
                        h << "// =============================================================\n";
                        h << "// METHOD PATCHES\n";
                        h << "// =============================================================\n\n";

                        for (auto &[m, data] : oMap)
                        {
                            if (data.bytes.empty() || !m || !m->methodPointer) continue;

                            auto *klass2 = m->getClass();
                            std::string ns_  = (klass2&&klass2->getNamespace()&&klass2->getNamespace()[0])
                                               ? klass2->getNamespace() : "";
                            std::string cn   = klass2&&klass2->getName() ? klass2->getName() : "Unknown";
                            std::string mn2  = m->getName() ? m->getName() : "method";
                            std::string asm_ = (klass2&&klass2->getImage()&&klass2->getImage()->getName())
                                               ? klass2->getImage()->getName() : "";
                            auto *rt         = m->getReturnType();
                            std::string rtn  = rt ? rt->getName() : "void";
                            int paramCnt     = (int)m->getParamsInfo().size();
                            uintptr_t abs    = (uintptr_t)m->methodPointer;
                            uintptr_t rva    = (il2cppBase4&&abs>il2cppBase4) ? (abs-il2cppBase4) : abs;
                            auto bytesOn     = makeBytes4(m, data.text);

                            std::string safeName = cn+"_"+mn2;
                            for (char &c : safeName) if (!isalnum(c)) c='_';
                            std::string UP = safeName;
                            std::transform(UP.begin(),UP.end(),UP.begin(),::toupper);

                            h << "// ── " << cn << "::" << mn2 << " ──\n";
                            if (!ns_.empty())  h << "// Namespace   : " << ns_  << "\n";
                            if (!asm_.empty()) h << "// Assembly    : " << asm_ << "\n";
                            h << "// Return type : " << rtn << "\n";
                            h << "// Param count : " << paramCnt << "\n";
                            h << "// Patch value : " << data.text << "\n";
                            h << "#define RVA_" << UP << "  0x" << std::hex << rva << std::dec << "ULL\n";
                            h << "// Patch ON  (bytes baru) : " << bytesToHexStr(bytesOn)    << "\n";
                            h << "// Patch OFF (bytes asli) : " << bytesToHexStr(data.bytes) << "\n";
                            h << "\n";
                        }
                    }

                    // ── Field Patches ─────────────────────────────────────────
                    bool hasField = false;
                    for (auto &[f,pd] : fieldPatchMap)
                        if (pd.active && f) { hasField=true; break; }

                    if (hasField)
                    {
                        h << "// =============================================================\n";
                        h << "// FIELD PATCHES\n";
                        h << "// Cara pakai: *(type*)(instance_ptr + OFFSET_xxx) = value; tiap frame\n";
                        h << "// Field tidak di-patch via ASM — value ditulis langsung ke memori.\n";
                        h << "// =============================================================\n\n";

                        for (auto &[f, pd] : fieldPatchMap)
                        {
                            if (!pd.active || !f) continue;

                            const char *fn  = f->getName() ? f->getName() : "field";
                            auto *fft       = f->getType();
                            const char *ftn = fft ? fft->getName() : "?";
                            uintptr_t foff  = f->getOffset();
                            bool isStatic4  = pd.isStatic;

                            Il2CppClass *fKlass = nullptr;
                            // FieldInfo tidak punya getClass() — cari via loop oMap atau biarkan kosong
                            // Nama class dari field name saja sudah cukup untuk info header
                            std::string fCn  = (fKlass&&fKlass->getName())  ? fKlass->getName()  : "?";
                            std::string fNs  = (fKlass&&fKlass->getNamespace()&&fKlass->getNamespace()[0])
                                               ? fKlass->getNamespace() : "";
                            std::string fAsm = (fKlass&&fKlass->getImage()&&fKlass->getImage()->getName())
                                               ? fKlass->getImage()->getName() : "";

                            std::string safeFn = std::string("Field_")+fn;
                            for (char &c : safeFn) if (!isalnum(c)) c='_';
                            std::string UFN = safeFn;
                            std::transform(UFN.begin(),UFN.end(),UFN.begin(),::toupper);

                            h << "// ── [Field] " << fCn << "." << fn << " ──\n";
                            if (!fNs.empty())  h << "// Namespace  : " << fNs  << "\n";
                            if (!fAsm.empty()) h << "// Assembly   : " << fAsm << "\n";
                            h << "// Field type : " << ftn << "\n";
                            h << "// Patch value: " << pd.text << "\n";
                            h << "// Storage    : " << (isStatic4 ? "Static" : "Instance") << "\n";
                            h << "#define OFFSET_" << UFN << "  0x" << std::hex << foff << std::dec << "ULL\n";

                            if (!isStatic4)
                            {
                                uintptr_t instPtr = pd.patchedInstance ? (uintptr_t)pd.patchedInstance : 0;
                                if (instPtr)
                                    h << "// Instance ptr (saat export): 0x" << std::hex << instPtr << std::dec
                                      << "  — bisa berubah antar sesi, scan ulang via GC\n";
                                else
                                    h << "// Instance ptr : scan via GC FindObjects(" << fCn << ")\n";
                                h << "// Cara pakai   :\n";
                                h << "//   uintptr_t inst = /* GC scan */;\n";
                                h << "//   *(" << ftn << "*)(inst + OFFSET_" << UFN << ") = " << pd.text << ";\n";
                            }
                            else
                            {
                                h << "// Cara pakai (static) :\n";
                                h << "//   void* sp = il2cpp_class_get_static_field_data(klass);\n";
                                h << "//   *(" << ftn << "*)((uintptr_t)sp + OFFSET_" << UFN << ") = " << pd.text << ";\n";
                            }
                            h << "\n";
                        }
                    }

                    std::string hfname = pkgName4 + "_" + gameVer4 + "_offsets.h";
                    Util::FileWriter fw4(Util::DirCodegen(), hfname);
                    fw4.write(h.str().c_str());
                    Keyboard::Open((Util::DirCodegen() + "/" + hfname).c_str(), [](const std::string&){});
                }
                ImGui::PopStyleColor();
                ImGui::SetItemTooltip("Generate C++ header berisi info lengkap:\n- RVA + bytes ON/OFF tiap method\n- Offset + instance info tiap field\n- Cara pakai singkat, tanpa boilerplate");
            }
        }
        {
            static bool processing = false;
            ImGui::SameLine();
            char label[12]{0};
            if (!traceState)
                sprintf(label, "Trace all");
            else
                sprintf(label, "Restore");

            bool disabled = false;
            if (processing)
            {
                disabled = true; // creating variable because `processing` could be set to false and we don't call
                                 // `EndDisabled`
                ImGui::BeginDisabled();
            }
            if (ImGui::Button(label))
            {
                {
                    if (!traceState)
                    {
                        ImGui::OpenPopup("ConfirmPopup");
                    }
                    else
                    {
                        traceState = false;
                        // BUG FIX (Langkah 1 - Thread Safety):
                        // Capture tracedMethods by value (copy) agar tidak ada
                        // dangling reference ke this->tracedMethods jika tab dihapus.
                        auto methodsCopy = tracedMethods;
                        auto *procFlag   = &processing;
                        auto *maxProg    = &maxProgress;
                        auto *prog       = &progress;
                        std::thread([methodsCopy, procFlag, maxProg, prog]() mutable
                        {
                            LOGD("Restoring all methods...");
                            *procFlag = true;
                            *maxProg = (int)methodsCopy.size();
                            for (auto method : methodsCopy)
                            {
                                Tool::ToggleHooker(method);
                            }
                            LOGD("Restored %zu methods", methodsCopy.size());
                            *procFlag = false;
                            *maxProg  = 0;
                            *prog     = 0;
                            LOGD("Done");
                        }).detach();
                        tracedMethods.clear();
                    }
                }
            }

            if (disabled)
                ImGui::EndDisabled();

            if (ImGui::BeginPopup("ConfirmPopup"))
            {
                ImGui::TextColored(ImVec4(0.8, 0.8, 0, 1), "WARNING: There's a high-risk of crash");
                if (ImGui::Button("Continue?"))
                {
                    traceState = true;
                    // BUG FIX (Langkah 1 - Thread Safety):
                    // Capture filteredClasses + methodMap by value.
                    // Pointer ke processing/progress/states tetap valid selama
                    // ClassesTab hidup — gunakan atomic via pointer.
                    auto filtCopy    = filteredClasses;
                    auto methCopy    = methodMap;
                    auto *procFlag   = &processing;
                    auto *maxProg    = &maxProgress;
                    auto *prog       = &progress;
                    auto *traced     = &tracedMethods;
                    std::thread([filtCopy, methCopy, procFlag, maxProg, prog, traced]() mutable
                    {
                        LOGD("Tracing all methods...");
                        *maxProg = 0;
                        for (auto &klass : filtCopy)
                        {
                            auto it = methCopy.find(klass);
                            if (it == methCopy.end()) continue;
                            for (auto &[method, paramsInfo] : it->second)
                            {
                                if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                    continue;
                                (*maxProg)++;
                            }
                        }
                        *procFlag = true;
                        for (auto &klass : filtCopy)
                        {
                            auto it = methCopy.find(klass);
                            if (it == methCopy.end()) continue;
                            for (auto &[method, paramsInfo] : it->second)
                            {
                                if (!method->methodPointer && !Il2cpp::GetIsMethodInflated(method))
                                    continue;
                                if (Tool::ToggleHooker(method, 1))
                                    traced->push_back(method);
                                (*prog)++;
                            }
                        }
                        LOGD("Traced %zu methods", traced->size());
                        *procFlag = false;
                        *maxProg  = 0;
                        *prog     = 0;
                        LOGD("Done");
                    }).detach();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (processing)
            {
                ImGui::SameLine();
                ImGui::Text("Processing %d of %d ...", progress, maxProgress);
            }
        }
        if (ImGui::BeginPopup("FilterOptions"))
        {
            if (ImGui::Checkbox("Case-Sensitive", &caseSensitive))
            {
                FilterClasses(filter);
            }
            ImGui::Text("Filter by ");
            ImGui::SameLine();
            if (ImGui::RadioButton("Class", filterByClass == true))
            {
                filterByClass = true;
                filterByMethod = false;
                filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Method", filterByMethod == true))
            {
                filterByClass = false;
                filterByMethod = true;
                filterByField = false;
                FilterClasses(filter);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Field", filterByField == true))
            {
                filterByClass = false;
                filterByMethod = false;
                filterByField = true;
                FilterClasses(filter);
            }
            if (ImGui::Checkbox("Show All Classes", &showAllClasses))
            {
                FilterClasses(filter);
            }
            ImGui::EndPopup();
        }
        ImGui::Separator();
        if (!filteredClasses.empty())
        {
            ImGui::BeginChild("Child", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            for (int i = 0; i < filteredClasses.size(); i++)
            {
                auto klass = filteredClasses[i];

                bool isValueType = Il2cpp::GetClassType(klass)->isValueType();

                // ── CollapsingHeader dengan warna per segmen ──────────────────
                // FIX GHOST TEXT: Label CollapsingHeader = kosong ("###chN")
                // Semua teks dirender HANYA via DrawList — tidak ada double render.
                //
                // Warna:
                //   namespace  → teal redup  RGB(80,160,185)  — info sekunder
                //   classname  → putih terang RGB(230,240,255) — fokus utama
                //   valuetype  → amber kuning RGB(255,210,80)  — struct/valuetype
                const char *ns  = klass->getNamespace();
                const char *cn  = klass->getName();
                bool hasNs      = (ns && ns[0] != '\0');

                char hdrId[64];
                snprintf(hdrId, sizeof(hdrId), "###ch%d", i);

                // Render header, teks transparan — kita gambar sendiri
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
                bool collapsingHeader = ImGui::CollapsingHeader(hdrId);
                ImGui::PopStyleColor();

                // Gambar teks SEKALI via DrawList (tidak ada overlap)
                if (ImGui::IsItemVisible()) {
                    float itemX   = ImGui::GetItemRectMin().x;
                    float itemY   = ImGui::GetItemRectMin().y;
                    float arrowSz = ImGui::GetFrameHeight();
                    float padX    = arrowSz + ImGui::GetStyle().FramePadding.x;
                    float padY    = ImGui::GetStyle().FramePadding.y;
                    ImDrawList *dl = ImGui::GetWindowDrawList();

                    if (hasNs) {
                        char nsDot[256];
                        snprintf(nsDot, sizeof(nsDot), "%s.", ns);
                        // namespace: teal redup
                        dl->AddText(ImVec2(itemX + padX, itemY + padY),
                            IM_COL32(80, 160, 185, 210), nsDot);
                        // classname: putih terang / amber
                        float nsW   = ImGui::CalcTextSize(nsDot).x;
                        ImU32 cnCol = isValueType
                            ? IM_COL32(255, 210, 80, 255)
                            : IM_COL32(230, 240, 255, 255);
                        dl->AddText(ImVec2(itemX + padX + nsW, itemY + padY),
                            cnCol, cn ? cn : "?");
                    } else {
                        ImU32 cnCol = isValueType
                            ? IM_COL32(255, 210, 80, 255)
                            : IM_COL32(230, 240, 255, 255);
                        dl->AddText(ImVec2(itemX + padX, itemY + padY),
                            cnCol, cn ? cn : "?");
                    }
                }

                                if (filterByMethod && ImGui::IsItemHeld(0.7f))
                {
                    auto name = Util::extractClassNameFromTypename(klass->getFullName().c_str());
                    auto &tab = Tool::OpenNewTab();
                    tab.filter = name;
                    tab.selectedImage = klass->getImage();
                    tab.FilterClasses(tab.filter);
                }
                if (collapsingHeader)
                {
                    ImGui::PushID(i);
                    ClassViewer(klass);
                    ImGui::PopID();
                }
            }
            ImGui::ScrollWhenDraggingOnVoid();
            ImGui::EndChild();
        }
        ImGui::EndTabItem();
    }
    DrawStaticRefWindows();
}

void ClassesTab::DrawTabMap()
{
    for (auto it = tabMap.begin(); it != tabMap.end();)
    {
        auto &[object, visible] = *it;
        char buff[32]{0};
        sprintf(buff, "[%p]", object);

        if (!visible)
        {
            it = tabMap.erase(it);
            dataMap.erase(object);
        }
        else
        {
            if (ImGui::BeginTabItem(buff, &visible))
            {
                ImGuiJson(object);
                ImGui::EndTabItem();
            }
            ++it;
        }
    }
}


void ClassesTab::FilterClasses(const std::string &filter)
{
    filteredClasses.clear();
    classes.clear();
    methodMap.clear();
    if (includeAllImages)
    {
        for (auto image : g_Images)
        {
            auto imageClasses = image->getClasses();
            classes.insert(classes.end(), imageClasses.begin(), imageClasses.end());
        }
    }
    else
    {
        classes = selectedImage->getClasses();
    }

    auto finderCaseSensitive = [](const std::string &a, const std::string &b)
    { return a.find(b) != std::string::npos; };

    auto finderCaseInsensitive = [](const std::string &a, const std::string &b)
    {
        auto newA = a;
        auto newB = b;
        std::transform(newA.begin(), newA.end(), newA.begin(), ::tolower);
        std::transform(newB.begin(), newB.end(), newB.begin(), ::tolower);
        return newA.find(newB) != std::string::npos;
    };

    auto finder = caseSensitive ? finderCaseSensitive : finderCaseInsensitive;

    for (int i = 0; i < (int)classes.size() && filteredClasses.size() < (showAllClasses ? classes.size() : MAX_CLASSES); i++)
    {
        auto klass = classes[i];

        if (Il2cpp::GetClassIsEnum(klass))
            continue;

        if (filterByClass)
        {
            if (finder(klass->getFullName(), filter))
            {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods())
                {
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
        }
        else if (filterByMethod)
        {
            // Cari method yang match ATAU class name match — supaya tidak hilang saat switch mode
            bool classMatch = finder(klass->getFullName(), filter);
            bool found = false;
            for (auto m : klass->getMethods())
            {
                if (finder(m->getName(), filter) || classMatch)
                {
                    found = true;
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
            if (found)
                filteredClasses.push_back(klass);
        }
        else if (filterByField)
        {
            // Cari field yang match ATAU class name match — supaya tidak hilang saat switch mode
            bool classMatch = finder(klass->getFullName(), filter);
            bool fieldMatch = false;
            for (auto f : klass->getFields())
            {
                if (finder(f->getName(), filter))
                {
                    fieldMatch = true;
                    break;
                }
            }
            if (fieldMatch || classMatch)
            {
                filteredClasses.push_back(klass);
                for (auto m : klass->getMethods())
                {
                    auto paramsInfo = m->getParamsInfo();
                    methodMap[klass].push_back({m, paramsInfo});
                }
            }
        }
    }
    Tool::ConfigSave();
}

// ═══════════════════════════════════════════════════════════════════════════

// ── FITUR 1: Static Instance Finder ─────────────────────────────────────────
// Scan semua class di semua images, cari static field yang tipenya sama
// dengan klass target (atau List/Array yang mengandung tipe tersebut).
// Hasil: "EntryPoint" pointer chain untuk ESP C++ tanpa reflection.
// ═══════════════════════════════════════════════════════════════════════════
// Cek apakah type name mengandung nama class target (untuk List<Enemy> dsb)
static bool TypeMatchesClass(const char *typeName, const char *targetName)
{
    if (!typeName || !targetName) return false;
    if (strcmp(typeName, targetName) == 0) return true;
    return strstr(typeName, targetName) != nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
// ── FITUR 1: Static Instance Finder ─────────────────────────────────────────
// Ubah: sekarang tampilkan daftar Owner Class saja, setiap klik buka popup
// ═══════════════════════════════════════════════════════════════════════════
void ClassesTab::StaticReferencesTab(Il2CppClass *klass)
{
    if (!klass) return;
    
    auto &win = g_staticRefWindows[klass];
    win.targetClass = klass;
    
    if (!win.open) {
        if (ImGui::Button("Open Static References Window", ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            win.open = true;
        }
        return;
    }
    
    // Kalau window udah open, kita render di Draw() nanti
    // Fungsi ini kosong karena render-nya di DrawStaticRefWindows()
}


// ═══════════════════════════════════════════════════════════════════════════
// ── Render semua floating window Static References ─────────────────────────
// ═══════════════════════════════════════════════════════════════════════════
static void DrawStaticRefWindows()
{
    for (auto &[klass, win] : g_staticRefWindows) {
        if (!win.open || !klass) continue;
        
        char windowId[128];
        snprintf(windowId, sizeof(windowId), "Static References: %s###staticref_%p", 
                 klass->getName() ? klass->getName() : "?", klass);
        
        ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
        
        if (!ImGui::Begin(windowId, &win.open, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            continue;
        }
        
        const char *targetName = klass->getName() ? klass->getName() : "?";
        
        // ── Header + Tombol Scan ───────────────────────────────────────
        ImGui::TextColored(ImVec4(0.4f,0.9f,1.f,1.f), "Target: %s", targetName);
        ImGui::SameLine();
        
        if (win.scanning) {
            ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f), "  Scanning...");
        } else {
           if (ImGui::Button("Rescan")) {
    win.scanning = true;
    win.results.clear();
    
    std::string tn = targetName;
    std::vector<Il2CppImage*> imgs = g_Images;
    Il2CppClass *targetKlass = klass;
    
    // Simpen pointer ke window biar bisa diakses di thread
    StaticRefWindow *winPtr = &win;
    
    // BUG FIX (Langkah 1 - Thread Safety):
    // Ganti raw pointer winPtr dengan shared data yang aman:
    // - Hasil scan disimpan ke shared_ptr<vector> (bukan langsung ke winPtr)
    // - winPtr->scanning di-update via atomic setelah hasil tersedia
    // - Ini mencegah crash jika window ditutup sebelum thread selesai
    auto sharedResults  = std::make_shared<std::vector<StaticRefResult>>();
    auto scanDone       = std::make_shared<std::atomic<bool>>(false);
    // Capture winPtr sebagai raw pointer — valid selama g_staticRefWindows tidak di-rehash.
    // g_staticRefWindows menggunakan unordered_map dengan pointer stability untuk value.
    StaticRefWindow *safeWinPtr = winPtr; // explicit untuk clarity
    
    std::thread([safeWinPtr, sharedResults, scanDone, tn, imgs, targetKlass]() {
        std::vector<StaticRefResult> tmp;
        for (auto *img : imgs) {
            if (!img) continue;
            for (auto *ownerKlass : img->getClasses()) {
                if (!ownerKlass || ownerKlass == targetKlass) continue;
                
                for (auto *field : ownerKlass->getFields()) {
                    if (!field) continue;
                    if (!(Il2cpp::GetFieldFlags(field) & FIELD_ATTRIBUTE_STATIC)) continue;
                    
                    auto *ft = field->getType();
                    if (!ft) continue;
                    const char *ftn = ft->getName();
                    if (!ftn) continue;
                    
                    if (strstr(ftn, tn.c_str()) == nullptr) continue;
                    
                    StaticRefResult r;
                    r.ownerClass   = ownerKlass;
                    r.field        = field;
                    r.fieldType    = ftn;
                    r.offset       = field->getOffset();
                    r.isList       = strstr(ftn, "List`1") != nullptr || strstr(ftn, "List<") != nullptr;
                    r.isArray      = strstr(ftn, "[]") != nullptr;
                    r.assemblyName = img->getName() ? img->getName() : "?";
                    
                    if (ft->isEnum() && il2cpp_field_static_get_value) {
                        try {
                            uint64_t ev = 0;
                            il2cpp_field_static_get_value(field, &ev);
                            r.enumValue = ev;
                        } catch(...) {}
                    }
                    
                    tmp.push_back(r);
                }
            }
        }
        // Tulis hasil ke shared_ptr dulu, lalu update winPtr sekaligus
        *sharedResults = std::move(tmp);
        safeWinPtr->results = *sharedResults;
        scanDone->store(true, std::memory_order_release);
        safeWinPtr->scanning = false;
    }).detach();
}
            ImGui::SameLine();
            ImGui::TextDisabled("%zu result(s)", win.results.size());
        }
        
        ImGui::Separator();
        
        // ── Filter ──────────────────────────────────────────────────────
        ImGui::InputText("Filter", win.filter, sizeof(win.filter));
        ImGui::Separator();
        
        // ── Tabel Hasil ─────────────────────────────────────────────────
        if (ImGui::BeginTable("##StaticRefTable", 6, 
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | 
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Owner Class",  ImGuiTableColumnFlags_WidthStretch, 25.f);
            ImGui::TableSetupColumn("Field",        ImGuiTableColumnFlags_WidthStretch, 20.f);
            ImGui::TableSetupColumn("Type",         ImGuiTableColumnFlags_WidthStretch, 20.f);
            ImGui::TableSetupColumn("Offset",       ImGuiTableColumnFlags_WidthFixed,   70.f);
            ImGui::TableSetupColumn("Value",        ImGuiTableColumnFlags_WidthFixed,   60.f);
            ImGui::TableSetupColumn("Copy",         ImGuiTableColumnFlags_WidthFixed,   60.f);
            ImGui::TableHeadersRow();
            
            std::string filterLower = win.filter;
            std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);
            
            for (auto &r : win.results) {
                if (!r.ownerClass || !r.field) continue;
                
                // Filter
                if (win.filter[0]) {
                    std::string ownerName = r.ownerClass->getName() ? r.ownerClass->getName() : "";
                    std::string fieldName = r.field->getName() ? r.field->getName() : "";
                    std::string typeLower = r.fieldType;
                    std::transform(ownerName.begin(), ownerName.end(), ownerName.begin(), ::tolower);
                    std::transform(fieldName.begin(), fieldName.end(), fieldName.begin(), ::tolower);
                    std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);
                    
                    if (ownerName.find(filterLower) == std::string::npos &&
                        fieldName.find(filterLower) == std::string::npos &&
                        typeLower.find(filterLower) == std::string::npos) {
                        continue;
                    }
                }
                
                ImGui::TableNextRow();
                ImGui::PushID(r.field);
                
                // Owner Class
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(r.ownerClass->getName() ? r.ownerClass->getName() : "?");
                ImGui::SameLine();
                ImGui::TextDisabled("<%s>", r.assemblyName.c_str());
                
                // Field Name
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(r.field->getName() ? r.field->getName() : "?");
                
                // Type
                ImGui::TableSetColumnIndex(2);
                bool isEnum = r.field->getType() && r.field->getType()->isEnum();
                if (isEnum) {
                    ImGui::TextColored(ImVec4(0.2f,1.f,0.4f,1.f), "%s", r.fieldType.c_str());
                } else {
                    ImGui::TextDisabled("%s", r.fieldType.c_str());
                }
                
                // Offset
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("0x%zX", r.offset);
                
                // Value preview (enum)
                ImGui::TableSetColumnIndex(4);
                if (isEnum && r.enumValue != 0) {
                    ImGui::Text("%llu", (unsigned long long)r.enumValue);
                } else {
                    ImGui::TextDisabled("-");
                }
                
                // Copy button
                ImGui::TableSetColumnIndex(5);
                if (ImGui::SmallButton("Chain")) {
                    std::ostringstream chain;
                    uintptr_t base = GetLibBase(GetTargetLib());
                    uintptr_t sdRVA = 0;
                    
                    if (il2cpp_class_get_static_field_data && base) {
                        try {
                            void *sd = il2cpp_class_get_static_field_data(r.ownerClass);
                            if (sd && IsPtrValid(sd)) {
                                uintptr_t abs = (uintptr_t)sd;
                                if (abs > base) sdRVA = abs - base;
                            }
                        } catch(...) {}
                    }
                    
                    chain << "// " << r.ownerClass->getName() << "::" << r.field->getName() << "\n";
                    chain << "uintptr_t base = GetLibBase(\"libil2cpp.so\");\n";
                    if (sdRVA) {
                        chain << "uintptr_t sd = base + 0x" << std::hex << sdRVA << ";\n";
                    } else {
                        chain << "uintptr_t sd = (uintptr_t)il2cpp_class_get_static_field_data(" 
                              << r.ownerClass->getName() << "_klass);\n";
                    }
                    chain << "uintptr_t inst = *(uintptr_t*)(sd + 0x" << std::hex << r.offset << ");\n";
                    
                    Keyboard::Open(chain.str().c_str(), [](const std::string&){});
                }
                
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        
        ImGui::End();
    }
}

// ═══════════════════════════════════════════════════════════════════════════

// ── FITUR 5: Hierarchy / Transform Path Viewer ──────────────────────────────
// Saat klik field bertipe Transform/GameObject, tampilkan popup hierarchy:
// parent chain + children + bone index hints.
// ═══════════════════════════════════════════════════════════════════════════
bool IsTransformType(const char *tn)
{
    if (!tn) return false;
    return strstr(tn,"Transform")!=nullptr || strstr(tn,"GameObject")!=nullptr;
}

void ClassesTab::HierarchyViewerPopup(Il2CppObject *obj, const char *fieldName)
{
    if (!obj || !obj->klass) return;

    char popupId[128];
    snprintf(popupId, sizeof(popupId), "HierarchyViewer_%p", obj);

    if (ImGui::BeginPopup(popupId))
    {
        ImGui::TextColored(ImVec4(0.4f,1.f,1.f,1.f), "Hierarchy: %s", fieldName ? fieldName : "?");
        ImGui::Separator();

        // Coba akses transform via Unity API
        try {
            // get name
            auto *mn = obj->klass->getMethod("get_name", 0);
            if (mn) {
                auto *nameStr = obj->invoke_method<Il2CppString*>("get_name");
                if (nameStr) {
                    const char *name = Il2cpp::GetChars(nameStr);
                    if (name) ImGui::Text("Name: %s", name);
                }
            }

            // get position (Transform)
            auto *posMethod = obj->klass->getMethod("get_position", 0);
            if (posMethod) {
                // Vector3 is a value type, returned via registers on ARM64
                // Kita hanya tampilkan RVA saja untuk safety
                uintptr_t base = (uintptr_t)GetLibBase(GetTargetLib());
                uintptr_t rva = (base && posMethod->methodPointer &&
                                 (uintptr_t)posMethod->methodPointer > base)
                                 ? (uintptr_t)posMethod->methodPointer - base : 0;
                if (rva) ImGui::Text("get_position RVA: 0x%zX", rva);
            }

            // get childCount
            auto *ccm = obj->klass->getMethod("get_childCount", 0);
            if (ccm) {
                int childCount = 0;
                try { childCount = obj->invoke_method<int>("get_childCount"); } catch(...) {}
                ImGui::Text("Children: %d", childCount);
                if (childCount > 0 && childCount < 64) {
                    ImGui::Indent();
                    auto *getChildM = obj->klass->getMethod("GetChild", 1);
                    if (getChildM) {
                        for (int ci = 0; ci < childCount && ci < 32; ci++) {
                            auto *child = obj->invoke_method<Il2CppObject*>("GetChild", ci);
                            if (!child || !child->klass) continue;
                            auto *childNameM = child->klass->getMethod("get_name", 0);
                            std::string cname = "child_" + std::to_string(ci);
                            if (childNameM) {
                                try {
                                    auto *cs = child->invoke_method<Il2CppString*>("get_name");
                                    if (cs) { const char *_s = Il2cpp::GetChars(cs); if (_s) cname = _s; }
                                } catch(...) {}
                            }
                            char clbl[128];
                            snprintf(clbl,sizeof(clbl),"[%d] %s", ci, cname.c_str());
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180,220,255,255));
                            ImGui::Selectable(clbl);
                            ImGui::SetItemTooltip("Index: %d | Ptr: %p", ci, child);
                            ImGui::PopStyleColor();
                        }
                    }
                    ImGui::Unindent();
                }
            }

            // get parent
            auto *pm = obj->klass->getMethod("get_parent", 0);
            if (pm) {
                ImGui::Separator();
                ImGui::TextDisabled("get_parent tersedia — chain bisa dilanjut ke atas");
                uintptr_t base = (uintptr_t)GetLibBase(GetTargetLib());
                uintptr_t rva = (base && pm->methodPointer &&
                                 (uintptr_t)pm->methodPointer > base)
                                 ? (uintptr_t)pm->methodPointer - base : 0;
                if (rva) ImGui::Text("get_parent RVA: 0x%zX", rva);
            }

        } catch(...) {
            ImGui::TextColored(ImVec4(1.f,0.3f,0.3f,1.f), "Error accessing hierarchy");
        }

        ImGui::Separator();
        ImGui::TextDisabled("Gunakan index child untuk Aimbot bone targeting.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void to_json(nlohmann::ordered_json &j, const ClassesTab &p)
{
    j["filter"] = p.filter;
    j["filterByClass"] = p.filterByClass;
    j["filterByField"] = p.filterByField;
    j["filterByMethod"] = p.filterByMethod;
    j["showAllClasses"] = p.showAllClasses;
    j["includeAllImages"] = p.includeAllImages;
    j["caseSensitive"] = p.caseSensitive;
    j["selectedImage"] = p.selectedImage->getName();
}

void from_json(const nlohmann::ordered_json &j, ClassesTab &p)
{
    j.at("filter").get_to(p.filter);
    j.at("filterByClass").get_to(p.filterByClass);
    j.at("filterByField").get_to(p.filterByField);
    j.at("filterByMethod").get_to(p.filterByMethod);
    j.at("showAllClasses").get_to(p.showAllClasses);
    j.at("includeAllImages").get_to(p.includeAllImages);
    j.at("caseSensitive").get_to(p.caseSensitive);
    std::string selectedImage = j.at("selectedImage").get<std::string>();
    if (selectedImage.size() >= 4 && selectedImage.substr(selectedImage.size() - 4) == ".dll")
    {
        selectedImage.erase(selectedImage.size() - 4);
    }
    auto assembly = Il2cpp::GetAssembly(selectedImage.c_str());
    if (assembly)
    {
        auto image = assembly->getImage();
        if (image)
        {
            p.selectedImage = image;
        }
    }
}

std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> ClassesTab::objectMap;
std::unordered_map<Il2CppClass *, std::vector<Il2CppObject *>> ClassesTab::newObjectMap;
std::unordered_map<Il2CppClass *, std::set<Il2CppObject *>> ClassesTab::savedSet;
std::unordered_map<MethodInfo *, ClassesTab::OriginalMethodBytes> ClassesTab::oMap;
std::unordered_map<FieldInfo *, ClassesTab::FieldPatchData> ClassesTab::fieldPatchMap;
std::unordered_map<Il2CppClass *, bool> ClassesTab::states;
PopUpSelector ClassesTab::poper;
// DrawStaticRefWindows() sudah dideklarasikan di atas file (forward decl baris 9)
// dan didefinisikan di bawah (StaticRefWindows section)