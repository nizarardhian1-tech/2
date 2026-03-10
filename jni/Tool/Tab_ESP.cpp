// ============================================================================
// Tab_ESP.cpp  v8 — FieldDirect / MethodInvoke / TransformChain
// ============================================================================

#include "Tool/Tab_ESP.h"
#include "Tool/ESPManager.h"
#include "Tool/Keyboard.h"
#include "Tool/Util.h"
#include "Il2cpp/Il2cpp.h"
#include "Il2cpp/il2cpp-class.h"
#include "Includes/Logger.h"
#include "json/single_include/nlohmann/json.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstring>

using ESPTab::FieldKind;

extern std::vector<Il2CppImage *> g_Images;
static ESPManager g_espManager;

static std::vector<ESPTab::Condition> s_conditions;
static Il2CppClass *s_condTargetClass = nullptr;
static bool s_showCondPopup = false;

// ─── Scrollbar style ─────────────────────────────────────────────────────────
static void PushThickScrollbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 26.f);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,          IM_COL32(10, 10, 20, 200));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,        IM_COL32(40, 80, 120, 220));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(60, 110, 160, 255));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  IM_COL32(80, 140, 200, 255));
}
static void PopThickScrollbar() { ImGui::PopStyleColor(4); ImGui::PopStyleVar(1); }

// ─── Colored button ───────────────────────────────────────────────────────────
static bool ColorBtn(const char *lbl, ImU32 bg, ImU32 bgHov, ImVec2 sz = ImVec2(0,0)) {
    ImGui::PushStyleColor(ImGuiCol_Button,        bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bgHov);
    bool r = ImGui::Button(lbl, sz);
    ImGui::PopStyleColor(2);
    return r;
}

// ─── Finger scroll ────────────────────────────────────────────────────────────
static void FingerScroll(float &prev, float &startY) {
    auto &io = ImGui::GetIO();
    auto *win = ImGui::GetCurrentWindow();
    if (GImGui->MovingWindow == win) { prev=-1.f; startY=-1.f; return; }
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) { prev=-1.f; startY=-1.f; return; }
    if (io.MouseDown[0]) {
        if (startY < 0.f) startY = io.MousePos.y;
        float moved = fabsf(io.MousePos.y - startY);
        if (moved > 8.f) {
            if (prev >= 0.f) ImGui::SetScrollY(ImGui::GetScrollY() + (prev - io.MousePos.y));
            prev = io.MousePos.y;
        } else if (prev < 0.f) { prev = io.MousePos.y; }
    } else { prev=-1.f; startY=-1.f; }
}

// ============================================================================
// Condition evaluate
// ============================================================================
const char* ESPTab::Condition::opLabel() const {
    switch (op) {
        case CondOp::EQ:  return "=="; case CondOp::NEQ: return "!=";
        case CondOp::GT:  return ">";  case CondOp::LT:  return "<";
        case CondOp::GTE: return ">="; case CondOp::LTE: return "<=";
    }
    return "==";
}

bool ESPTab::Condition::evaluate(Il2CppObject *obj) const {
    if (!active || !obj || !cachedField) return true;
    try {
        auto *ft = cachedField->getType(); if (!ft) return true;
        const char *tn = ft->getName();    if (!tn) return true;
        uintptr_t base2 = (uintptr_t)obj, off = cachedField->getOffset();
        if (strcmp(tn, "System.Boolean") == 0) {
            bool v = *(bool*)(base2+off);
            return op==CondOp::EQ ? v==boolVal : v!=boolVal;
        } else if (strstr(tn,"Int")||strstr(tn,"UInt")) {
            int32_t v = *(int32_t*)(base2+off);
            switch(op){ case CondOp::EQ:return v==intVal; case CondOp::NEQ:return v!=intVal;
                        case CondOp::GT:return v>intVal;  case CondOp::LT:return v<intVal;
                        case CondOp::GTE:return v>=intVal; case CondOp::LTE:return v<=intVal; }
        } else {
            float v = *(float*)(base2+off);
            switch(op){ case CondOp::EQ:return fabsf(v-floatVal)<0.001f; case CondOp::NEQ:return fabsf(v-floatVal)>=0.001f;
                        case CondOp::GT:return v>floatVal; case CondOp::LT:return v<floatVal;
                        case CondOp::GTE:return v>=floatVal; case CondOp::LTE:return v<=floatVal; }
        }
    } catch (...) {}
    return true;
}

// ============================================================================
// Config Save / Load
// ============================================================================
namespace {

struct SavedConfig {
    std::string className, fieldName, assemblyName, savedAt, note;
    int         posMode     = 0;  // 0=Field,1=Method,2=Transform
    std::string methodName;
    int         methodParamCount = -1;
    struct SavedCond {
        std::string fieldName, valueStr;
        int op; bool boolVal, active; int intVal; float floatVal;
    };
    std::vector<SavedCond> conditions;
};

static nlohmann::ordered_json configToJson(const SavedConfig &cfg) {
    nlohmann::ordered_json j;
    j["className"]       = cfg.className;
    j["fieldName"]       = cfg.fieldName;
    j["assemblyName"]    = cfg.assemblyName;
    j["savedAt"]         = cfg.savedAt;
    j["note"]            = cfg.note;
    j["posMode"]         = cfg.posMode;
    j["methodName"]      = cfg.methodName;
    j["methodParamCount"]= cfg.methodParamCount;
    nlohmann::ordered_json conds = nlohmann::ordered_json::array();
    for (auto &c : cfg.conditions) {
        nlohmann::ordered_json jc;
        jc["fieldName"]=c.fieldName; jc["op"]=c.op; jc["valueStr"]=c.valueStr;
        jc["boolVal"]=c.boolVal; jc["intVal"]=c.intVal; jc["floatVal"]=c.floatVal; jc["active"]=c.active;
        conds.push_back(jc);
    }
    j["conditions"] = conds;
    return j;
}

static SavedConfig configFromJson(const nlohmann::ordered_json &j) {
    SavedConfig cfg;
    cfg.className       = j.value("className",       "");
    cfg.fieldName       = j.value("fieldName",       "");
    cfg.assemblyName    = j.value("assemblyName",    "");
    cfg.savedAt         = j.value("savedAt",         "");
    cfg.note            = j.value("note",            "");
    cfg.posMode         = j.value("posMode",         0);
    cfg.methodName      = j.value("methodName",      "");
    cfg.methodParamCount= j.value("methodParamCount",-1);
    if (j.contains("conditions") && j["conditions"].is_array())
        for (auto &jc : j["conditions"]) {
            SavedConfig::SavedCond c;
            c.fieldName=jc.value("fieldName",""); c.op=jc.value("op",0);
            c.valueStr=jc.value("valueStr",""); c.boolVal=jc.value("boolVal",false);
            c.intVal=jc.value("intVal",0); c.floatVal=jc.value("floatVal",0.f);
            c.active=jc.value("active",true);
            cfg.conditions.push_back(c);
        }
    return cfg;
}

static bool saveConfig(const std::string &slot, Il2CppClass *klass, const std::string &fieldOrMethod,
                        int posMode, int methodParamCount,
                        const std::vector<ESPTab::Condition> &conds, const std::string &note = "") {
    if (!klass) return false;
    nlohmann::ordered_json root;
    { Util::FileReader fr(Util::DirESP(), "esp_configs.json");
      if (fr.exists()) try { root = nlohmann::json::parse(fr.read()); } catch (...) {} }
    char ts[32]; snprintf(ts, sizeof(ts), "frame_%u", (unsigned)ImGui::GetFrameCount());
    SavedConfig cfg;
    cfg.className    = klass->getName()  ? klass->getName()  : "";
    cfg.assemblyName = klass->getImage() ? klass->getImage()->getName() : "";
    cfg.savedAt      = ts;
    cfg.note         = note;
    cfg.posMode      = posMode;
    cfg.methodParamCount = methodParamCount;
    if (posMode == 1) { cfg.methodName = fieldOrMethod; cfg.fieldName = ""; }
    else              { cfg.fieldName  = fieldOrMethod; cfg.methodName = ""; }
    for (auto &c : conds) {
        SavedConfig::SavedCond sc;
        sc.fieldName=c.fieldName; sc.op=(int)c.op; sc.valueStr=c.valueStr;
        sc.boolVal=c.boolVal; sc.intVal=c.intVal; sc.floatVal=c.floatVal; sc.active=c.active;
        cfg.conditions.push_back(sc);
    }
    root[slot] = configToJson(cfg);
    Util::FileWriter(Util::DirESP(), "esp_configs.json").write(root.dump(2).c_str());
    return true;
}

static std::unordered_map<std::string, SavedConfig> loadAllConfigs() {
    std::unordered_map<std::string, SavedConfig> result;
    Util::FileReader fr(Util::DirESP(), "esp_configs.json");
    if (!fr.exists()) return result;
    try {
        auto root = nlohmann::json::parse(fr.read());
        for (auto it = root.begin(); it != root.end(); ++it)
            result[it.key()] = configFromJson(it.value());
    } catch (...) {}
    return result;
}

static bool applyConfig(const SavedConfig &cfg,
                         std::vector<ESPTab::Condition> &outConds,
                         Il2CppClass *&outClass) {
    outClass = nullptr;
    for (auto *img : g_Images) {
        if (!img) continue;
        bool asmMatch = cfg.assemblyName.empty() || (img->getName() && cfg.assemblyName == img->getName());
        for (auto *k : img->getClasses()) {
            if (!k || !k->getName() || cfg.className != k->getName()) continue;
            if (asmMatch || !outClass) { outClass = k; if (asmMatch) goto found; }
        }
    }
    found:
    if (!outClass) return false;
    outConds.clear();
    for (auto &sc : cfg.conditions) {
        ESPTab::Condition nc{};
        strncpy(nc.fieldName, sc.fieldName.c_str(), sizeof(nc.fieldName)-1);
        strncpy(nc.valueStr,  sc.valueStr.c_str(),  sizeof(nc.valueStr)-1);
        nc.op=static_cast<ESPTab::CondOp>(sc.op); nc.boolVal=sc.boolVal;
        nc.intVal=sc.intVal; nc.floatVal=sc.floatVal; nc.active=sc.active;
        for (auto *f : outClass->getFields())
            if (f && f->getName() && sc.fieldName == f->getName()) { nc.cachedField=f; break; }
        outConds.push_back(nc);
    }
    return true;
}

} // anonymous namespace

// ============================================================================
// EvaluateConditions / Render
// ============================================================================
bool ESPTab::EvaluateConditions(Il2CppObject *obj) {
    for (auto &c : s_conditions) { if (!c.active) continue; if (!c.evaluate(obj)) return false; }
    return true;
}

void ESPTab::Render() {
    g_espManager.Tick();
    g_espManager.Render(EvaluateConditions);
}

// ============================================================================
// DrawConditionsPopup
// ============================================================================
static void DrawConditionsPopup(float bw, float sp,
                                 std::vector<ESPTab::FieldEntry> &s_fieldEntries,
                                 bool tracking) {
    if (!s_showCondPopup) return;
    auto &io = ImGui::GetIO();
    float scrW=io.DisplaySize.x, scrH=io.DisplaySize.y, sheetH=scrH*0.65f;
    ImGui::SetNextWindowPos(ImVec2(0, scrH-sheetH), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(scrW, sheetH),  ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.97f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(8,10,18,255));
    ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(30,80,50,255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(10,8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   10.f);

    if (ImGui::Begin("Conditions##condWin", &s_showCondPopup,
        ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoCollapse))
    {
        float pw = ImGui::GetContentRegionAvail().x;
        { int ac=0; for (auto &c:s_conditions) if(c.active&&c.cachedField) ac++;
          if (s_condTargetClass) {
              ImGui::TextDisabled("%s", s_condTargetClass->getName());
              if (ac>0) { ImGui::SameLine(); ImGui::TextColored(ImVec4(.2f,1.f,.4f,1.f), "— %d active", ac); }
          } }
        ImGui::Separator();

        static int s_subTab = 0;
        { const char *tabs[]={"Conditions","Render Cfg","Save/Load"};
          ImU32 colors[]={IM_COL32(18,88,38,240),IM_COL32(18,58,108,240),IM_COL32(88,58,12,240)};
          float tw=(pw-sp*2)/3.f;
          for (int i=0;i<3;i++) {
              if(i>0) ImGui::SameLine();
              bool sel=(s_subTab==i);
              ImGui::PushStyleColor(ImGuiCol_Button,        sel?colors[i]:IM_COL32(38,38,52,200));
              ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel?colors[i]:IM_COL32(58,58,72,230));
              if(ImGui::Button(tabs[i],ImVec2(tw,0))) s_subTab=i;
              ImGui::PopStyleColor(2);
          } }
        ImGui::Spacing();

        auto readFieldVal = [&](FieldInfo *f) -> std::string {
            if (!f) return "";
            try {
                auto *ft=f->getType(); if(!ft) return "";
                const char *tn=ft->getName(); if(!tn) return "";
                bool isSt=(Il2cpp::GetFieldFlags(f)&FIELD_ATTRIBUTE_STATIC)!=0;
                Il2CppObject *obj0=g_espManager.GetFirstObject();
                if (!isSt && obj0) {
                    uintptr_t b2=(uintptr_t)obj0, off=f->getOffset();
                    if(strcmp(tn,"System.Boolean")==0) return *(bool*)(b2+off)?"true":"false";
                    if(strstr(tn,"Int32")||strstr(tn,"UInt32")) return std::to_string(*(int32_t*)(b2+off));
                    if(strcmp(tn,"System.Single")==0){char b[24];snprintf(b,24,"%.2f",*(float*)(b2+off));return b;}
                    if(strstr(tn,"Vector3")){float *v=(float*)(b2+off);char b[48];snprintf(b,48,"%.1f,%.1f,%.1f",v[0],v[1],v[2]);return b;}
                }
                if (isSt) {
                    if(strcmp(tn,"System.Boolean")==0) return f->getStaticValue<bool>()?"true":"false";
                    if(strstr(tn,"Int32")) return std::to_string(f->getStaticValue<int32_t>());
                    if(strcmp(tn,"System.Single")==0){char b[24];snprintf(b,24,"%.2f",f->getStaticValue<float>());return b;}
                }
            } catch (...) {}
            return "";
        };

        // ── SUB-TAB 0: CONDITIONS ──────────────────────────────────────────
        if (s_subTab == 0) {
            if (!s_condTargetClass) {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(35,25,10,200));
                ImGui::BeginChild("##condNA", ImVec2(0,40), true);
                ImGui::TextColored(ImVec4(1.f,.7f,.2f,1.f), "  Activate ESP first — pick class & field, click Test");
                ImGui::EndChild(); ImGui::PopStyleColor();
            } else {
                // Presets
                ImGui::TextDisabled("Quick:"); ImGui::SameLine();
                auto addPreset = [&](const char *kwA, const char *kwB, FieldKind ft2,
                                      ESPTab::CondOp op2, float fv, int iv, bool bv, const char *vs) {
                    FieldInfo *found=nullptr;
                    for (auto &fe:s_fieldEntries) {
                        std::string nl=fe.name; std::transform(nl.begin(),nl.end(),nl.begin(),::tolower);
                        if ((nl.find(kwA)!=std::string::npos||(kwB&&nl.find(kwB)!=std::string::npos))&&fe.kind==ft2)
                        { found=fe.field; break; }
                    }
                    ESPTab::Condition nc{};
                    if (found){strncpy(nc.fieldName,found->getName(),sizeof(nc.fieldName)-1);nc.cachedField=found;}
                    else strncpy(nc.fieldName,kwA,sizeof(nc.fieldName)-1);
                    strncpy(nc.valueStr,vs,sizeof(nc.valueStr)-1);
                    nc.op=op2; nc.boolVal=bv; nc.intVal=iv; nc.floatVal=fv;
                    s_conditions.push_back(nc);
                };
                float preW=(pw-sp*2)/3.f;
                if(ColorBtn("isVisible",  IM_COL32(15,55,25,220),IM_COL32(20,75,35,255),ImVec2(preW,0)))
                    addPreset("visible",nullptr,FieldKind::Bool,ESPTab::CondOp::EQ,0,0,true,"1");
                ImGui::SameLine();
                if(ColorBtn("enemyOnly",  IM_COL32(15,55,25,220),IM_COL32(20,75,35,255),ImVec2(preW,0)))
                    addPreset("camp","samecamp",FieldKind::Bool,ESPTab::CondOp::EQ,0,0,false,"0");
                ImGui::SameLine();
                if(ColorBtn("health<50",  IM_COL32(15,55,25,220),IM_COL32(20,75,35,255),ImVec2(-1,0)))
                    addPreset("health","hp",FieldKind::Float,ESPTab::CondOp::LT,50.f,50,false,"50");
                ImGui::Spacing();

                float listH=ImGui::GetContentRegionAvail().y-46.f;
                PushThickScrollbar();
                ImGui::BeginChild("##condList", ImVec2(0,listH), false);
                ESPTab::Condition *toRemove=nullptr;
                for (int ci=0; ci<(int)s_conditions.size(); ci++) {
                    auto &cond=s_conditions[ci];
                    ImGui::PushID(ci);
                    ImU32 rbg=!cond.active?IM_COL32(20,20,28,180):
                              cond.cachedField?IM_COL32(8,32,8,210):
                              strlen(cond.fieldName)>0?IM_COL32(38,8,8,210):IM_COL32(20,20,32,180);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, rbg);
                    ImGui::BeginChild("##crow", ImVec2(0,ImGui::GetTextLineHeightWithSpacing()*2.5f), true);
                    float rowW=ImGui::GetContentRegionAvail().x;
                    ImGui::Checkbox("##act",&cond.active); ImGui::SameLine();
                    bool isBool=cond.cachedField&&cond.cachedField->getType()&&strcmp(cond.cachedField->getType()->getName(),"System.Boolean")==0;
                    bool isFloat=cond.cachedField&&cond.cachedField->getType()&&strcmp(cond.cachedField->getType()->getName(),"System.Single")==0;
                    float valW=isBool?56.f:72.f, opW=isBool?0.f:38.f, xW=26.f;
                    float dropW=rowW-ImGui::GetFrameHeight()-sp-opW-(opW>0?sp:0)-valW-sp-xW-sp-8.f;
                    char prev[128];
                    if(!strlen(cond.fieldName)) snprintf(prev,128,"-- pick field --");
                    else {
                        std::string rv=cond.cachedField?readFieldVal(cond.cachedField):"";
                        if(!rv.empty()) snprintf(prev,128,"%s [%s]",cond.fieldName,rv.c_str());
                        else snprintf(prev,128,"%s",cond.fieldName);
                    }
                    ImGui::PushStyleColor(ImGuiCol_FrameBg,
                        cond.cachedField?IM_COL32(12,52,12,220):
                        strlen(cond.fieldName)>0?IM_COL32(65,12,12,220):IM_COL32(28,28,48,220));
                    ImGui::SetNextItemWidth(dropW);
                    if (ImGui::BeginCombo("##cf", prev, ImGuiComboFlags_HeightLarge)) {
                        struct Grp{const char *hdr;FieldKind fk;ImVec4 hc;};
                        Grp grps[]={{"Bool",FieldKind::Bool,{.3f,1.f,.4f,1.f}},{"Int",FieldKind::Int,{.4f,.8f,1.f,1.f}},{"Float",FieldKind::Float,{1.f,.75f,.3f,1.f}},{"Vector",FieldKind::Vector,{.8f,.5f,1.f,1.f}}};
                        for (auto &g:grps) {
                            bool hasAny=false; for (auto &fe:s_fieldEntries) if(fe.kind==g.fk){hasAny=true;break;}
                            if (!hasAny) continue;
                            ImGui::PushStyleColor(ImGuiCol_Text,g.hc); ImGui::TextUnformatted(g.hdr); ImGui::PopStyleColor();
                            ImGui::Separator();
                            for (auto &fe:s_fieldEntries) {
                                if (fe.kind!=g.fk) continue;
                                bool sel2=(cond.cachedField==fe.field);
                                std::string rv2=readFieldVal(fe.field);
                                char lbl[160];
                                if(!rv2.empty()) snprintf(lbl,160,"  %s  [%s]  0x%zX",fe.name.c_str(),rv2.c_str(),fe.field->getOffset());
                                else snprintf(lbl,160,"  %s  0x%zX",fe.name.c_str(),fe.field->getOffset());
                                ImGui::PushStyleColor(ImGuiCol_Text,fe.color);
                                if (ImGui::Selectable(lbl,sel2)) {
                                    strncpy(cond.fieldName,fe.field->getName(),sizeof(cond.fieldName)-1);
                                    cond.cachedField=fe.field; cond.op=ESPTab::CondOp::EQ;
                                    if (fe.kind==FieldKind::Bool){cond.boolVal=true;strncpy(cond.valueStr,"1",sizeof(cond.valueStr)-1);}
                                    else if(fe.kind==FieldKind::Float){cond.floatVal=0.f;strncpy(cond.valueStr,"0.0",sizeof(cond.valueStr)-1);}
                                    else{cond.intVal=0;strncpy(cond.valueStr,"0",sizeof(cond.valueStr)-1);}
                                }
                                if (sel2) ImGui::SetItemDefaultFocus();
                                ImGui::PopStyleColor();
                            }
                            ImGui::Spacing();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (!isBool) {
                        const char *ops[]={"==","!=",">","<",">=","<="};
                        int oi=(int)cond.op;
                        ImGui::SetNextItemWidth(opW);
                        if(ImGui::BeginCombo("##op",ops[oi],ImGuiComboFlags_NoArrowButton)){
                            for(int oo=0;oo<6;oo++) if(ImGui::Selectable(ops[oo],oi==oo)) cond.op=(ESPTab::CondOp)oo;
                            ImGui::EndCombo();
                        }
                        ImGui::SameLine();
                    }
                    if (isBool) {
                        ImGui::PushStyleColor(ImGuiCol_Button,cond.boolVal?IM_COL32(20,140,20,230):IM_COL32(140,20,20,230));
                        if(ImGui::Button(cond.boolVal?" ON ":" OFF",ImVec2(valW,0))){cond.boolVal=!cond.boolVal;strncpy(cond.valueStr,cond.boolVal?"1":"0",sizeof(cond.valueStr)-1);}
                        ImGui::PopStyleColor();
                    } else if (isFloat) {
                        ImGui::SetNextItemWidth(valW);
                        if(ImGui::DragFloat("##fv",&cond.floatVal,.5f,-99999.f,99999.f,"%.2f")) snprintf(cond.valueStr,sizeof(cond.valueStr),"%.4f",cond.floatVal);
                    } else {
                        float bsz=ImGui::GetFrameHeight();
                        ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32(30,60,100,220));
                        if(ImGui::Button("-##m",ImVec2(bsz,0))){cond.intVal--;snprintf(cond.valueStr,sizeof(cond.valueStr),"%d",cond.intVal);}
                        ImGui::PopStyleColor(); ImGui::SameLine(0,2);
                        ImGui::SetNextItemWidth(valW-bsz*2-4);
                        if(ImGui::DragInt("##iv",&cond.intVal)) snprintf(cond.valueStr,sizeof(cond.valueStr),"%d",cond.intVal);
                        ImGui::SameLine(0,2);
                        ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32(30,60,100,220));
                        if(ImGui::Button("+##p",ImVec2(bsz,0))){cond.intVal++;snprintf(cond.valueStr,sizeof(cond.valueStr),"%d",cond.intVal);}
                        ImGui::PopStyleColor();
                    }
                    ImGui::SameLine();
                    if(ColorBtn("X##dx",IM_COL32(120,25,25,200),IM_COL32(160,35,35,255),ImVec2(xW,0))) toRemove=&cond;
                    if(cond.active&&cond.cachedField){
                        auto *ft2=cond.cachedField->getType();
                        ImGui::TextDisabled("  0x%zX | %s",cond.cachedField->getOffset(),ft2?ft2->getName():"?");
                    } else if(cond.active&&strlen(cond.fieldName)>0) {
                        ImGui::TextColored(ImVec4(1.f,.35f,.35f,1.f),"  '%s' not found",cond.fieldName);
                    }
                    ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopID();
                }
                if (toRemove)
                    s_conditions.erase(std::remove_if(s_conditions.begin(),s_conditions.end(),
                        [toRemove](const ESPTab::Condition &c){return &c==toRemove;}),s_conditions.end());
                {static float fsCL=-1.f,fsCLs=-1.f;FingerScroll(fsCL,fsCLs);}
                ImGui::EndChild(); PopThickScrollbar();
                ImGui::Spacing();
                if(ColorBtn("+ Add Condition",IM_COL32(28,78,148,220),IM_COL32(38,98,178,255),ImVec2((pw-sp)*0.65f,0))){
                    ESPTab::Condition nc{};
                    if(!s_fieldEntries.empty()){
                        auto &fe0=s_fieldEntries[0];
                        strncpy(nc.fieldName,fe0.field->getName(),sizeof(nc.fieldName)-1);
                        nc.cachedField=fe0.field;
                        if(fe0.kind==FieldKind::Bool) strncpy(nc.valueStr,"1",sizeof(nc.valueStr)-1);
                        else if(fe0.kind==FieldKind::Float) strncpy(nc.valueStr,"0.0",sizeof(nc.valueStr)-1);
                        else strncpy(nc.valueStr,"0",sizeof(nc.valueStr)-1);
                    }
                    s_conditions.push_back(nc);
                }
                ImGui::SameLine();
                if(ColorBtn("Clear All",IM_COL32(80,22,22,200),IM_COL32(110,30,30,255),ImVec2(-1,0))) s_conditions.clear();
            }
        }

        // ── SUB-TAB 1: RENDER CONFIG ───────────────────────────────────────
        else if (s_subTab == 1) {
            float hw=(pw-sp)/2.f;
            static int   maxObj=20;
            static float maxDist=300.f, intv=3.f, lw=1.5f, cr=5.f;
            static bool  ln=true, bx=false, dist=true, names=false;
            static int   cmode=0;
            static ImVec4 ccol={1.f,.2f,.2f,1.f};
            static bool  autoScan=true;

            // Mode display
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10,28,18,220));
            ImGui::BeginChild("##modeDisp", ImVec2(0,36), true);
            const char *modeLabels[]={"[F] FieldDirect","[M] MethodInvoke","[T] TransformChain"};
            ImVec4 modeColors[]={{.3f,.9f,.3f,1.f},{.3f,.7f,1.f,1.f},{1.f,.7f,.2f,1.f}};
            ImGui::TextUnformatted("Mode:"); ImGui::SameLine();
            if (g_espManager.IsTracking()) {
                int m=(int)g_espManager.GetPosMode();
                ImGui::TextColored(modeColors[m], "%s", modeLabels[m]);
                ImGui::SameLine(); ImGui::TextDisabled("| %s", g_espManager.GetTrackedDispName().c_str());
            } else {
                ImGui::TextDisabled("ESP inactive");
            }
            ImGui::EndChild(); ImGui::PopStyleColor();
            ImGui::Spacing();

            // Scan control
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10,22,38,220));
            ImGui::BeginChild("##scanCfg", ImVec2(0,54), true);
            if(ImGui::Checkbox("Auto Scan",&autoScan)) g_espManager.SetAutoScan(autoScan);
            ImGui::SameLine(0,16);
            if (g_espManager.IsScanRunning()) ImGui::TextColored(ImVec4(1.f,.85f,.1f,1.f),"[Scanning...]");
            else if (autoScan)                ImGui::TextColored(ImVec4(.2f,1.f,.4f,1.f), "[Auto]");
            else                              ImGui::TextColored(ImVec4(1.f,.5f,.2f,1.f), "[Manual]");
            bool busy=g_espManager.IsScanRunning();
            if (busy) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,autoScan?IM_COL32(38,58,118,200):IM_COL32(18,118,40,230));
            if(ImGui::Button(busy?"  Scanning...  ":"  Refresh Instances  ",ImVec2(-1,0)))
                if(!busy) g_espManager.TriggerManualScan();
            ImGui::PopStyleColor();
            if (busy) ImGui::EndDisabled();
            ImGui::EndChild(); ImGui::PopStyleColor();
            ImGui::Spacing();

            ImGui::SetNextItemWidth(hw);
            if(ImGui::SliderInt("Max Obj",&maxObj,5,100))       g_espManager.SetMaxObjects(maxObj);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(hw);
            if(ImGui::SliderFloat("Max Dist",&maxDist,50,2000)) g_espManager.SetMaxDistance(maxDist);
            ImGui::SetNextItemWidth(hw);
            if(ImGui::SliderFloat("Scan Intv",&intv,2.f,10.f)) g_espManager.SetUpdateInterval(intv);
            ImGui::Spacing();
            ImGui::TextDisabled("Draw:"); ImGui::SameLine();
            if(ImGui::Checkbox("Lines",&ln))   g_espManager.SetDrawLines(ln);   ImGui::SameLine(0,14);
            if(ImGui::Checkbox("Boxes",&bx))   g_espManager.SetDrawBoxes(bx);   ImGui::SameLine(0,14);
            if(ImGui::Checkbox("Dist",&dist))  g_espManager.SetDrawDistance(dist); ImGui::SameLine(0,14);
            if(ImGui::Checkbox("Names",&names)) g_espManager.SetDrawNames(names);
            ImGui::SetNextItemWidth(hw*0.45f);
            if(ImGui::SliderFloat("Line W",&lw,.5f,5.f)) g_espManager.SetLineWidth(lw);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(hw*0.45f);
            if(ImGui::SliderFloat("Circle",&cr,1.f,12.f)) g_espManager.SetCircleRadius(cr);
            ImGui::Spacing();
            ImGui::TextDisabled("Color:"); ImGui::SameLine();
            if(ImGui::RadioButton("Gradient",&cmode,0)) g_espManager.SetColorMode(0); ImGui::SameLine();
            if(ImGui::RadioButton("Custom",&cmode,1))   g_espManager.SetColorMode(1);
            if(cmode==1){
                if(ImGui::ColorEdit4("##cc",&ccol.x,ImGuiColorEditFlags_NoInputs|ImGuiColorEditFlags_NoLabel))
                    g_espManager.SetCustomColor(ImGui::ColorConvertFloat4ToU32(ccol));
            }

            // Label field selector
            if (g_espManager.IsTracking() && g_espManager.GetTrackedClass()) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextDisabled("Label Field (overlay):");
                auto *lf = g_espManager.GetLabelField();
                const char *curLbl = lf ? (lf->getName() ? lf->getName() : "?") : "[Auto detect]";
                ImGui::SetNextItemWidth(pw);
                if (ImGui::BeginCombo("##labelField", curLbl, ImGuiComboFlags_HeightLarge)) {
                    bool selNone = (lf == nullptr);
                    if (ImGui::Selectable("[Auto detect]", selNone)) g_espManager.SetLabelField(nullptr);
                    if (selNone) ImGui::SetItemDefaultFocus();
                    ImGui::Separator();
                    for (auto *f : g_espManager.GetTrackedClass()->getFields()) {
                        if (!f || !f->getName()) continue;
                        auto *ft2 = f->getType(); const char *tn2 = ft2 ? ft2->getName() : "";
                        bool isUseful = tn2 && (strstr(tn2,"String")||strstr(tn2,"Int")||strstr(tn2,"Single"));
                        if (!isUseful) continue;
                        bool selThis = (lf == f);
                        char lbl2[128]; snprintf(lbl2,128,"  %s  [%s]",f->getName(),tn2);
                        if (ImGui::Selectable(lbl2, selThis)) g_espManager.SetLabelField(f);
                        if (selThis) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
        }

        // ── SUB-TAB 2: SAVE / LOAD ─────────────────────────────────────────
        else if (s_subTab == 2) {
            static char slotName[64] = "default";
            static char saveNote[128] = "";
            static std::unordered_map<std::string,SavedConfig> slots;
            static bool slotsInited = false;
            static std::string msg; static float msgTimer=0.f;
            if (!slotsInited) { slotsInited=true; slots=loadAllConfigs(); }

            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10,25,42,230));
            ImGui::BeginChild("##slotInput", ImVec2(0,78), true);
            float kbW=38.f;
            ImGui::TextColored(ImVec4(.4f,.9f,1.f,1.f),"Slot "); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-kbW-sp);
            ImGui::InputText("##sn",slotName,sizeof(slotName)); ImGui::SameLine();
            if(ImGui::Button("[KB]##sk",ImVec2(kbW,0)))
                Keyboard::Open(slotName,[](const std::string &t){strncpy(slotName,t.c_str(),sizeof(slotName)-1);});
            ImGui::TextColored(ImVec4(.6f,.6f,.6f,1.f),"Note "); ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-kbW-sp);
            ImGui::InputText("##nt",saveNote,sizeof(saveNote)); ImGui::SameLine();
            if(ImGui::Button("[KB]##nk",ImVec2(kbW,0)))
                Keyboard::Open(saveNote,[](const std::string &t){strncpy(saveNote,t.c_str(),sizeof(saveNote)-1);});
            ImGui::EndChild(); ImGui::PopStyleColor();
            ImGui::Spacing();

            float btnHalf=(pw-sp)/2.f;
            bool canSave=tracking&&strlen(slotName)>0;
            if(!canSave) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,canSave?IM_COL32(18,108,48,230):IM_COL32(40,40,40,180));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,IM_COL32(22,138,58,255));
            if(ImGui::Button("  Save Config  ",ImVec2(btnHalf,0))) {
                auto *tc = g_espManager.GetTrackedClass();
                auto pm = (int)g_espManager.GetPosMode();
                std::string dispN = g_espManager.GetTrackedDispName();
                auto *tm = g_espManager.GetTrackedMethod();
                int mpc = tm ? (int)tm->getParamsInfo().size() : -1;
                if(saveConfig(slotName,tc,dispN,pm,mpc,s_conditions,saveNote)){
                    slots=loadAllConfigs(); msg=std::string("Saved: ")+slotName; msgTimer=3.f;
                }
            }
            if(!canSave) ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32(48,48,88,200));
            if(ImGui::Button("  Refresh  ",ImVec2(-1,0))){slots=loadAllConfigs();msg="Refreshed";msgTimer=2.f;}
            ImGui::PopStyleColor();

            msgTimer-=ImGui::GetIO().DeltaTime;
            if(msgTimer>0.f) ImGui::TextColored(ImVec4(.3f,1.f,.4f,std::min(msgTimer,1.f)),"%s",msg.c_str());
            ImGui::Spacing();

            if(slots.empty()){ImGui::TextDisabled("No saved configs.");}
            else {
                float listH2=ImGui::GetContentRegionAvail().y-4.f;
                PushThickScrollbar();
                ImGui::PushStyleColor(ImGuiCol_ChildBg,IM_COL32(12,12,20,200));
                ImGui::BeginChild("##slotList",ImVec2(0,listH2),false);
                std::string toDel;
                for (auto &[name,cfg]:slots) {
                    ImGui::PushID(name.c_str());
                    ImGui::PushStyleColor(ImGuiCol_ChildBg,IM_COL32(15,30,22,220));
                    ImGui::BeginChild("##srow",ImVec2(0,62),true);
                    float rw=ImGui::GetContentRegionAvail().x;
                    const char *mStr[]={"[F]","[M]","[T]"};
                    ImGui::TextColored(ImVec4(.3f,1.f,.5f,1.f),"[%s]%s",name.c_str(),mStr[std::max(0,std::min(cfg.posMode,2))]);
                    ImGui::SameLine(); ImGui::TextDisabled("%s.%s",cfg.className.c_str(),
                        cfg.posMode==1?cfg.methodName.c_str():cfg.fieldName.c_str());
                    if(!cfg.note.empty()){ImGui::SameLine();ImGui::TextColored(ImVec4(.7f,.7f,.5f,1.f)," \"%s\"",cfg.note.c_str());}
                    ImGui::TextDisabled("  %zu filters | %s",cfg.conditions.size(),cfg.assemblyName.c_str());
                    float bw3=(rw-sp*2)/3.f;
                    if(ColorBtn("Load",IM_COL32(22,88,158,230),IM_COL32(32,108,188,255),ImVec2(bw3,0))){
                        Il2CppClass *lc=nullptr;
                        std::vector<ESPTab::Condition> lcs;
                        if(applyConfig(cfg,lcs,lc)){
                            g_espManager.StopTracking();
                            bool started=false;
                            if (cfg.posMode==1) {
                                // MethodInvoke — find method
                                if(lc) for(auto *m:lc->getMethods()){
                                    if(!m||!m->getName()) continue;
                                    if(cfg.methodName==m->getName()){
                                        int pc=(int)m->getParamsInfo().size();
                                        if(cfg.methodParamCount<0||pc==cfg.methodParamCount){
                                            try{g_espManager.StartTrackingMethod(lc,m);started=true;}catch(...){}
                                            break;
                                        }
                                    }
                                }
                                if(!started) msg="FAIL: method "+cfg.methodName+" not found";
                            } else if(cfg.posMode==2) {
                                // TransformChain
                                if(lc){try{g_espManager.StartTrackingTransform(lc,nullptr);started=true;}catch(...){}};
                            } else {
                                // FieldDirect
                                if(lc) for(auto *f:lc->getFields()){
                                    if(f&&f->getName()&&cfg.fieldName==f->getName()){
                                        try{g_espManager.StartTracking(lc,f);started=true;}catch(...){}
                                        break;
                                    }
                                }
                            }
                            if(started){
                                s_conditions=std::move(lcs); s_condTargetClass=lc;
                                strncpy(slotName,name.c_str(),sizeof(slotName)-1);
                                msg=std::string("Loaded: ")+name; msgTimer=3.f;
                            } else if(msg.empty()){msg="FAIL: class/field not found"; msgTimer=4.f;}
                            msgTimer=4.f;
                        } else {msg="FAIL: class not found";msgTimer=4.f;}
                    }
                    ImGui::SameLine();
                    if(ColorBtn("Overwrite",IM_COL32(88,68,12,220),IM_COL32(108,88,20,255),ImVec2(bw3,0))&&tracking){
                        auto *tc=g_espManager.GetTrackedClass();
                        auto pm=(int)g_espManager.GetPosMode();
                        std::string dn=g_espManager.GetTrackedDispName();
                        auto *tm2=g_espManager.GetTrackedMethod();
                        int mpc2=tm2?(int)tm2->getParamsInfo().size():-1;
                        if(saveConfig(name,tc,dn,pm,mpc2,s_conditions)){slots=loadAllConfigs();msg=std::string("Overwritten: ")+name;msgTimer=3.f;}
                    }
                    ImGui::SameLine();
                    if(ColorBtn("Del",IM_COL32(108,18,18,220),IM_COL32(140,25,25,255),ImVec2(-1,0))) toDel=name;
                    ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopID();
                }
                if(!toDel.empty()){
                    slots.erase(toDel);
                    nlohmann::ordered_json root2;
                    for(auto &[n,c]:slots) root2[n]=configToJson(c);
                    Util::FileWriter(Util::DirESP(),"esp_configs.json").write(root2.dump(2).c_str());
                    msg=std::string("Deleted: ")+toDel; msgTimer=2.f;
                }
                {static float fsSL=-1.f,fsSLs=-1.f;FingerScroll(fsSL,fsSLs);}
                ImGui::EndChild(); ImGui::PopStyleColor();
                PopThickScrollbar();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3); ImGui::PopStyleColor(2);
}

// ============================================================================
// ESPTab::Draw
// ============================================================================
void ESPTab::Draw() {
    auto resolveCachedFields = [](Il2CppClass *klass) {
        if (!klass) return;
        for (auto &c : s_conditions) {
            if (c.cachedField) continue;
            for (auto *f : klass->getFields())
                if (f && f->getName() && strcmp(f->getName(), c.fieldName)==0) { c.cachedField=f; break; }
        }
    };

    g_espManager.Tick();

    float bw = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    bool tracking = g_espManager.IsTracking();

    Il2CppClass *curTracked = tracking ? g_espManager.GetTrackedClass() : nullptr;
    if (curTracked && curTracked != s_condTargetClass) {
        s_condTargetClass = curTracked;
        for (auto &c : s_conditions) c.cachedField = nullptr;
        resolveCachedFields(s_condTargetClass);
    }

    // Field entries cache (for Conditions popup)
    static std::vector<FieldEntry>  s_fieldEntries;
    static Il2CppClass             *s_fieldEntriesFor = nullptr;
    if (s_condTargetClass && s_condTargetClass != s_fieldEntriesFor) {
        s_fieldEntriesFor = s_condTargetClass;
        s_fieldEntries.clear();
        for (auto *f : s_condTargetClass->getFields(true)) {
            if (!f || !f->getName()) continue;
            auto *ft=f->getType(); if(!ft||!ft->getName()) continue;
            const char *tn=ft->getName();
            FieldKind kind=FieldKind::Other; ImVec4 col={.7f,.7f,.7f,1.f};
            if(strcmp(tn,"System.Boolean")==0)                                          {kind=FieldKind::Bool;  col={.3f,1.f,.4f,1.f};}
            else if(strstr(tn,"Int")||strstr(tn,"UInt"))                                {kind=FieldKind::Int;   col={.4f,.8f,1.f,1.f};}
            else if(strcmp(tn,"System.Single")==0||strcmp(tn,"System.Double")==0)       {kind=FieldKind::Float; col={1.f,.75f,.3f,1.f};}
            else if(strstr(tn,"Vector")||strstr(tn,"Vec"))                              {kind=FieldKind::Vector;col={.8f,.5f,1.f,1.f};}
            else continue;
            FieldEntry e; e.field=f; e.name=f->getName(); e.kind=kind; e.color=col;
            char lbl[128]; snprintf(lbl,128,"%s  [0x%zX]",f->getName(),f->getOffset()); e.label=lbl;
            s_fieldEntries.push_back(e);
        }
    }

    // ── STATUS BAR ────────────────────────────────────────────────────────────
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, tracking ? IM_COL32(10,45,10,230) : IM_COL32(25,25,40,220));
        ImGui::BeginChild("##espStatus", ImVec2(0, tracking ? 52.f : 32.f), true);
        if (tracking) {
            const char *modeBadges[]  = {"[F]","[M]","[T]"};
            ImVec4      modeColors[]  = {{.3f,.9f,.3f,1.f},{.3f,.7f,1.f,1.f},{1.f,.7f,.2f,1.f}};
            int pm = (int)g_espManager.GetPosMode();
            ImGui::TextColored(modeColors[pm], "%s", modeBadges[pm]); ImGui::SameLine();
            ImGui::TextColored(ImVec4(.2f,1.f,.3f,1.f), "ESP ON"); ImGui::SameLine();
            ImGui::TextDisabled("|"); ImGui::SameLine();
            ImGui::TextUnformatted(g_espManager.GetTrackedClass()->getName()); ImGui::SameLine();
            ImGui::TextDisabled("."); ImGui::SameLine();
            ImGui::TextColored(ImVec4(.4f,.9f,1.f,1.f), "%s", g_espManager.GetTrackedDispName().c_str());
            ImGui::TextDisabled("Obj: %zu  Drawn: %d  Culled: %d  %s",
                g_espManager.GetObjectCount(), g_espManager.GetRenderedCount(),
                g_espManager.GetCulledCount(),  g_espManager.IsScanRunning() ? "[Scanning...]" : "");
        } else {
            ImGui::TextColored(ImVec4(.5f,.5f,.5f,1.f), "ESP OFF");
            ImGui::SameLine(); ImGui::TextDisabled("— browse below, click Test/Method/Transform");
        }
        ImGui::EndChild(); ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    // ── ACTION ROW ────────────────────────────────────────────────────────────
    {
        float thirdW = (bw - sp*2) / 3.f;
        ImGui::PushStyleColor(ImGuiCol_Button,        tracking ? IM_COL32(160,30,30,220) : IM_COL32(55,55,55,180));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200,40,40,255));
        if (ImGui::Button(tracking ? "  Stop ESP  " : "  ESP Off  ", ImVec2(thirdW,0)) && tracking)
            g_espManager.StopTracking();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        {
            int actCnt=0; for(auto &c:s_conditions) if(c.active&&c.cachedField) actCnt++;
            bool hasActive=actCnt>0;
            ImGui::PushStyleColor(ImGuiCol_Button,        hasActive?IM_COL32(18,88,38,230):IM_COL32(38,48,38,200));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(25,118,50,255));
            char condLbl[32];
            snprintf(condLbl,32,actCnt>0?" Filter [%d] ":" Conditions ",actCnt);
            if(ImGui::Button(condLbl,ImVec2(thirdW,0))) s_showCondPopup=true;
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(25,80,160,220));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(35,110,210,255));
        if (ImGui::Button("  Export  ", ImVec2(-1,0)) && tracking) {
            // Build conditions string for export
            std::ostringstream condStr;
            for (auto &cond : s_conditions) {
                if (!cond.active || !cond.cachedField) continue;
                auto *ft2=cond.cachedField->getType(); std::string tn2=ft2&&ft2->getName()?ft2->getName():"?";
                condStr << "constexpr uintptr_t FilterOff_" << cond.fieldName
                        << " = 0x" << std::hex << cond.cachedField->getOffset() << ";\n";
                if (tn2=="System.Boolean")
                    condStr << "constexpr bool     FilterVal_" << cond.fieldName << " = " << (cond.boolVal?"true":"false") << ";\n";
                else if (tn2=="System.Single")
                    condStr << "constexpr float    FilterVal_" << cond.fieldName << " = " << std::dec << cond.floatVal << "f;\n";
                else
                    condStr << "constexpr int32_t  FilterVal_" << cond.fieldName << " = " << std::dec << cond.intVal << ";\n";
            }
            g_espManager.ExportAll(condStr.str());
        }
        ImGui::PopStyleColor(2);
    }
    ImGui::Spacing();

    // ── BROWSE / SEARCH TOGGLE ────────────────────────────────────────────────
    static int  searchMode = 0;
    static bool vec3Only   = false;
    static bool needRef    = true;
    {
        float mw=(bw-sp)/2.f;
        ImGui::PushStyleColor(ImGuiCol_Button, searchMode==0?IM_COL32(38,98,38,220):IM_COL32(48,48,48,180));
        if(ImGui::Button("Browse",ImVec2(mw,0))){searchMode=0;needRef=true;}
        ImGui::PopStyleColor(); ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, searchMode==1?IM_COL32(38,78,158,220):IM_COL32(48,48,48,180));
        if(ImGui::Button("Search",ImVec2(-1,0))){searchMode=1;needRef=true;}
        ImGui::PopStyleColor();
    }

    // Filter row
    static char cf[128]="";
    {
        if(ImGui::Button("[KB]##ek",ImVec2(36,0)))
            Keyboard::Open(cf,[](const std::string &t){strncpy(cf,t.c_str(),127);needRef=true;});
        ImGui::SameLine();
        ImGui::SetNextItemWidth(bw-36-70-55-sp*3);
        if(ImGui::InputText("##Flt",cf,sizeof(cf))) needRef=true;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button,vec3Only?IM_COL32(18,158,48,230):IM_COL32(48,48,48,180));
        if(ImGui::Button("Vec3",ImVec2(55,0))){vec3Only=!vec3Only;needRef=true;}
        ImGui::PopStyleColor(); ImGui::SameLine();
        if(ImGui::Button("##refresh",ImVec2(-1,0))) needRef=true;
        // Draw refresh icon manually
        ImGui::SameLine(bw-34); ImGui::TextDisabled("[R]");
    }

    // ═════════════════════════════════════════════════════════════════════════
    // BROWSE MODE
    // ═════════════════════════════════════════════════════════════════════════
    if (searchMode == 0) {
        struct ClassEntry { Il2CppClass *klass; bool hasVec3; };
        static int selImg = -1;
        static std::vector<ClassEntry>  filtered, pendingFiltered;
        static std::mutex               browseMtx;
        static std::atomic<bool>        browseBuilding{false}, browseReady{false};
        static Il2CppClass             *selClass = nullptr;

        // Assembly selector
        ImGui::SetNextItemWidth(bw);
        const char *curImg = (selImg==-1) ? "[All Assemblies]" : (selImg<(int)g_Images.size()?g_Images[selImg]->getName():"...");
        if (ImGui::BeginCombo("##Img", curImg)) {
            bool a=(selImg==-1);
            if(ImGui::Selectable("[All Assemblies]",a)){selImg=-1;needRef=true;} if(a) ImGui::SetItemDefaultFocus();
            ImGui::Separator();
            for (int i=0;i<(int)g_Images.size();i++){
                bool s=(selImg==i);
                if(ImGui::Selectable(g_Images[i]->getName(),s)){selImg=i;needRef=true;}
                if(s) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        // Async class filter
        if (needRef && !browseBuilding.load()) {
            needRef=false; browseReady.store(false); browseBuilding.store(true); selClass=nullptr;
            std::string fs=cf; bool v3=vec3Only; int img=selImg;
            std::vector<Il2CppImage*> imgs=g_Images;
            std::thread([fs,v3,img,imgs]() mutable {
                std::string fl=fs; std::transform(fl.begin(),fl.end(),fl.begin(),::tolower);
                std::vector<ClassEntry> tmp; tmp.reserve(1024);
                auto proc=[&](Il2CppImage *im){
                    for(auto *k:im->getClasses()){
                        if(!k) continue; const char *cn=k->getName(); if(!cn) continue;
                        bool hv3=false;
                        for(auto *f:k->getFields()){if(!f)continue;auto *ft=f->getType();if(ft&&ft->getName()&&strstr(ft->getName(),"Vector3")){hv3=true;break;}}
                        if(v3&&!hv3) continue;
                        if(!fl.empty()){std::string nm=cn;std::transform(nm.begin(),nm.end(),nm.begin(),::tolower);if(nm.find(fl)==std::string::npos) continue;}
                        tmp.push_back({k,hv3});
                    }
                };
                if(img==-1) for(auto *im:imgs){if(im) proc(im);}
                else if(img<(int)imgs.size()&&imgs[img]) proc(imgs[img]);
                {std::lock_guard<std::mutex> lk(browseMtx); pendingFiltered=std::move(tmp);}
                browseReady.store(true); browseBuilding.store(false);
            }).detach();
        }
        if(browseReady.load()){browseReady.store(false);std::lock_guard<std::mutex> lk(browseMtx);filtered=std::move(pendingFiltered);}

        float avH = ImGui::GetContentRegionAvail().y - 4.f;
        PushThickScrollbar();

        // Left panel — class list
        ImGui::BeginChild("##Cls", ImVec2(210, avH), true);
        if(browseBuilding.load()) ImGui::TextColored(ImVec4(1.f,.8f,.2f,1.f),"Loading...");
        else ImGui::TextDisabled("Classes (%zu)", filtered.size());
        ImGui::Separator();
        ImGuiListClipper cc; cc.Begin((int)filtered.size());
        while(cc.Step()) for(int i=cc.DisplayStart;i<cc.DisplayEnd;i++){
            auto &e=filtered[i]; if(!e.klass) continue;
            const char *cn=e.klass->getName(); if(!cn) continue;
            bool sel=(selClass==e.klass);
            if(e.hasVec3) ImGui::PushStyleColor(ImGuiCol_Text,IM_COL32(80,220,100,255));
            if(ImGui::Selectable(cn,sel)) selClass=e.klass;
            if(e.hasVec3) ImGui::PopStyleColor();
        }
        cc.End();
        {static float fsC=-1.f,fsCs=-1.f;FingerScroll(fsC,fsCs);}
        ImGui::EndChild(); ImGui::SameLine();

        // Right panel — Fields / Methods tabs
        ImGui::BeginChild("##Detail", ImVec2(0, avH), true);
        if (!selClass) {
            ImGui::SetCursorPosY(avH*0.45f);
            ImGui::TextDisabled("  Select a class from the list");
        } else {
            ImGui::TextColored(ImVec4(.2f,1.f,1.f,1.f), "%s", selClass->getName());
            if(selClass->getImage()){ImGui::SameLine();ImGui::TextDisabled("<%s>",selClass->getImage()->getName());}
            if(g_espManager.IsTracking()&&g_espManager.GetTrackedClass()==selClass){
                ImGui::SameLine(); ImGui::TextColored(ImVec4(0,1,0,1)," [ACTIVE]");
            }
            ImGui::Separator();

            // Method entries cache for selected class
            static std::vector<ESPTab::MethodEntry> s_browseMethodEntries;
            static Il2CppClass *s_browseMethodsFor = nullptr;
            if (selClass != s_browseMethodsFor) {
                s_browseMethodsFor = selClass;
                s_browseMethodEntries.clear();
                uintptr_t libBase = GetLibBase(GetTargetLib());
                for (auto *m : selClass->getMethods()) {
                    if (!m || !m->getName()) continue;
                    auto *rt = m->getReturnType(); if (!rt) continue;
                    const char *rtn = rt->getName(); if (!rtn) continue;
                    bool isVec3  = strstr(rtn,"Vector3")   != nullptr;
                    bool isTrans = strstr(rtn,"Transform")  != nullptr;
                    if (!isVec3 && !isTrans) continue;
                    ESPTab::MethodEntry e;
                    e.method       = m;
                    e.name         = m->getName();
                    e.returnType   = rtn;
                    e.paramCount   = (int)m->getParamsInfo().size();
                    e.returnsVec3  = isVec3;
                    e.returnsTrans = isTrans;
                    uintptr_t abs  = m->methodPointer ? (uintptr_t)m->methodPointer : 0;
                    e.rva = (libBase && abs > libBase) ? (abs - libBase) : 0;
                    s_browseMethodEntries.push_back(e);
                }
            }

            // ── Inner tab: Fields | Methods ───────────────────────────────────
            static int innerTab = 0;
            {
                float tbw = (ImGui::GetContentRegionAvail().x - sp) / 2.f;
                ImGui::PushStyleColor(ImGuiCol_Button, innerTab==0?IM_COL32(30,80,130,220):IM_COL32(40,40,55,180));
                if(ImGui::Button("Fields",  ImVec2(tbw,0))) innerTab=0;
                ImGui::PopStyleColor(); ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, innerTab==1?IM_COL32(30,80,130,220):IM_COL32(40,40,55,180));
                char mthLbl[32]; snprintf(mthLbl,32,"Methods (%zu)",s_browseMethodEntries.size());
                if(ImGui::Button(mthLbl, ImVec2(-1,0))) innerTab=1;
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();

            float panelH = ImGui::GetContentRegionAvail().y - 2.f;

            // ── FIELDS TAB ────────────────────────────────────────────────────
            if (innerTab == 0) {
                auto fields = selClass->getFields();
                if (ImGui::BeginTable("##EFT", 6, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                    ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingStretchProp, ImVec2(0,panelH))) {
                    ImGui::TableSetupScrollFreeze(0,1);
                    ImGui::TableSetupColumn("Name",   ImGuiTableColumnFlags_WidthStretch, 30.f);
                    ImGui::TableSetupColumn("Type",   ImGuiTableColumnFlags_WidthStretch, 25.f);
                    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed,   58.f);
                    ImGui::TableSetupColumn("Test",   ImGuiTableColumnFlags_WidthFixed,   50.f);
                    ImGui::TableSetupColumn("Copy",   ImGuiTableColumnFlags_WidthFixed,   40.f);
                    ImGui::TableSetupColumn("Watch",  ImGuiTableColumnFlags_WidthFixed,   44.f);
                    ImGui::TableHeadersRow();
                    for (auto *f : fields) {
                        if (!f) continue; const char *fn=f->getName(); if(!fn) continue;
                        auto *ft=f->getType(); const char *tn=ft?ft->getName():"?";
                        bool isVec3  = tn && strstr(tn,"Vector3");
                        bool isTrans = tn && strstr(tn,"Transform");
                        ImGui::TableNextRow(); ImGui::PushID(f);
                        ImGui::TableSetColumnIndex(0);
                        if(isVec3)       ImGui::TextColored(ImVec4(.2f,1.f,.4f,1.f),"%s",fn);
                        else if(isTrans) ImGui::TextColored(ImVec4(1.f,.7f,.2f,1.f),"%s",fn);
                        else             ImGui::TextUnformatted(fn);
                        ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s",tn?tn:"?");
                        ImGui::TableSetColumnIndex(2); ImGui::Text("0x%lX",(unsigned long)f->getOffset());
                        ImGui::TableSetColumnIndex(3);
                        if (isVec3) {
                            bool act=g_espManager.IsTracking()&&g_espManager.GetTrackedField()==f&&g_espManager.GetPosMode()==PositionMode::FieldDirect;
                            if(act){if(ColorBtn("ON",IM_COL32(30,140,30,200),IM_COL32(40,170,40,255),ImVec2(-1,0))) g_espManager.StopTracking();}
                            else{if(ImGui::Button("Test",ImVec2(-1,0))) try{g_espManager.StartTracking(selClass,f);}catch(...){}
                            }
                        } else if (isTrans) {
                            bool act=g_espManager.IsTracking()&&g_espManager.GetTrackedClass()==selClass&&g_espManager.GetPosMode()==PositionMode::TransformChain&&g_espManager.GetTrackedField()==f;
                            if(act){if(ColorBtn("ON",IM_COL32(30,140,30,200),IM_COL32(40,170,40,255),ImVec2(-1,0))) g_espManager.StopTracking();}
                            else{if(ColorBtn("Trans",IM_COL32(100,60,10,220),IM_COL32(130,80,15,255),ImVec2(-1,0))) try{g_espManager.StartTrackingTransform(selClass,f);}catch(...){}
                            }
                        } else { ImGui::TextDisabled("—"); }
                        ImGui::TableSetColumnIndex(4);
                        if(ImGui::Button("Copy",ImVec2(-1,0))){
                            char buf[32]; snprintf(buf,32,"0x%lX",(unsigned long)f->getOffset());
                            Keyboard::Open(buf,[](const std::string &){});
                        }
                        ImGui::TableSetColumnIndex(5);
                        ImGui::Button("Watch",ImVec2(-1,0));
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }

            // ── METHODS TAB ───────────────────────────────────────────────────
            else if (innerTab == 1) {
                // TransformChain shortcut at top
                ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30,20,5,200));
                ImGui::BeginChild("##transHint", ImVec2(0,40), true);
                float hw2=(ImGui::GetContentRegionAvail().x-sp)/2.f;
                bool actTrans=g_espManager.IsTracking()&&g_espManager.GetTrackedClass()==selClass&&g_espManager.GetPosMode()==PositionMode::TransformChain;
                if(actTrans){
                    if(ColorBtn("  Transform → ON  ",IM_COL32(30,140,30,200),IM_COL32(40,170,40,255),ImVec2(hw2,0))) g_espManager.StopTracking();
                } else {
                    if(ColorBtn("[T] Track Transform",IM_COL32(100,60,10,220),IM_COL32(130,80,15,255),ImVec2(hw2,0)))
                        try{g_espManager.StartTrackingTransform(selClass,nullptr);}catch(...){}
                }
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(.6f,.6f,.6f,1.f),"inherit get_transform→pos");
                ImGui::EndChild(); ImGui::PopStyleColor();
                ImGui::Spacing();

                if (s_browseMethodEntries.empty()) {
                    ImGui::TextColored(ImVec4(.6f,.6f,.6f,1.f), "No Vector3/Transform methods found in this class.");
                    ImGui::TextDisabled("(Includes methods in base classes)");
                } else {
                    float methH = ImGui::GetContentRegionAvail().y - 2.f;
                    if (ImGui::BeginTable("##EMT", 5, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                        ImGuiTableFlags_ScrollY|ImGuiTableFlags_SizingStretchProp, ImVec2(0,methH))) {
                        ImGui::TableSetupScrollFreeze(0,1);
                        ImGui::TableSetupColumn("Method",  ImGuiTableColumnFlags_WidthStretch, 35.f);
                        ImGui::TableSetupColumn("Return",  ImGuiTableColumnFlags_WidthStretch, 25.f);
                        ImGui::TableSetupColumn("Args",    ImGuiTableColumnFlags_WidthFixed,   34.f);
                        ImGui::TableSetupColumn("RVA",     ImGuiTableColumnFlags_WidthFixed,   70.f);
                        ImGui::TableSetupColumn("Test",    ImGuiTableColumnFlags_WidthFixed,   50.f);
                        ImGui::TableHeadersRow();
                        for (auto &e : s_browseMethodEntries) {
                            if (!e.method) continue;
                            ImGui::TableNextRow(); ImGui::PushID(e.method);
                            ImGui::TableSetColumnIndex(0);
                            if(e.returnsVec3)  ImGui::TextColored(ImVec4(.2f,1.f,.4f,1.f),"%s",e.name.c_str());
                            else               ImGui::TextColored(ImVec4(1.f,.7f,.2f,1.f),"%s",e.name.c_str());
                            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s",e.returnType.c_str());
                            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%d",e.paramCount);
                            ImGui::TableSetColumnIndex(3);
                            if(e.rva) ImGui::Text("0x%llX",(unsigned long long)e.rva);
                            else ImGui::TextDisabled("—");
                            ImGui::TableSetColumnIndex(4);
                            if (e.returnsVec3) {
                                bool actM=g_espManager.IsTracking()&&g_espManager.GetTrackedMethod()==e.method&&g_espManager.GetPosMode()==PositionMode::MethodInvoke;
                                if(actM){if(ColorBtn("ON",IM_COL32(30,140,30,200),IM_COL32(40,170,40,255),ImVec2(-1,0))) g_espManager.StopTracking();}
                                else{if(ImGui::Button("Test",ImVec2(-1,0))) try{g_espManager.StartTrackingMethod(selClass,e.method);}catch(...){}
                                }
                            } else {
                                ImGui::TextDisabled("—");
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
            }
        }
        {static float fsF=-1.f,fsFs=-1.f;FingerScroll(fsF,fsFs);}
        ImGui::EndChild();
        PopThickScrollbar();

    // ═════════════════════════════════════════════════════════════════════════
    // SEARCH MODE
    // ═════════════════════════════════════════════════════════════════════════
    } else {
        struct SearchResult { Il2CppClass *klass; FieldInfo *field; bool isVec3; };
        static std::vector<SearchResult>  results, pendingResults;
        static std::mutex                 searchMtx;
        static std::atomic<bool>          searching{false}, searchReady{false};

        if (needRef && !searching.load()) {
            needRef=false; searching.store(true); searchReady.store(false);
            std::string fs=cf; bool v3=vec3Only;
            std::vector<Il2CppImage*> imgs=g_Images;
            std::thread([fs,v3,imgs]() mutable {
                std::string fl=fs; std::transform(fl.begin(),fl.end(),fl.begin(),::tolower);
                std::vector<SearchResult> tmp; tmp.reserve(512);
                for (auto *img:imgs){
                    if(!img) continue;
                    for (auto *k:img->getClasses()){
                        if(!k) continue;
                        for (auto *f:k->getFields()){
                            if(!f||!f->getName()) continue;
                            auto *ft=f->getType(); const char *tn=ft?ft->getName():"";
                            bool isVec3=tn&&strstr(tn,"Vector3");
                            if(v3&&!isVec3) continue;
                            std::string fn=f->getName(),fnl=fn;
                            std::transform(fnl.begin(),fnl.end(),fnl.begin(),::tolower);
                            std::string cnl=k->getName()?k->getName():"";
                            std::transform(cnl.begin(),cnl.end(),cnl.begin(),::tolower);
                            if(!fl.empty()&&fnl.find(fl)==std::string::npos&&cnl.find(fl)==std::string::npos) continue;
                            tmp.push_back({k,f,isVec3});
                        }
                    }
                }
                {std::lock_guard<std::mutex> lk(searchMtx);pendingResults=std::move(tmp);}
                searchReady.store(true); searching.store(false);
            }).detach();
        }
        if(searchReady.load()){searchReady.store(false);std::lock_guard<std::mutex> lk(searchMtx);results=std::move(pendingResults);}

        float avH2=ImGui::GetContentRegionAvail().y-4.f;
        PushThickScrollbar();
        ImGui::BeginChild("##SRes",ImVec2(0,avH2),true);
        if(searching.load()) ImGui::TextColored(ImVec4(1.f,.8f,.2f,1.f),"Searching...");
        else ImGui::TextDisabled("Results (%zu)",results.size());
        ImGui::Separator();
        ImGuiListClipper cl; cl.Begin((int)results.size());
        while(cl.Step()) for(int i=cl.DisplayStart;i<cl.DisplayEnd;i++){
            auto &r=results[i];
            if(!r.klass||!r.field) continue;
            const char *fn=r.field->getName(); if(!fn) continue;
            auto *ft=r.field->getType(); const char *tn=ft?ft->getName():"?";
            ImGui::PushID(r.field);
            if(r.isVec3) ImGui::PushStyleColor(ImGuiCol_Text,IM_COL32(80,220,100,255));
            char rowlbl[192];
            snprintf(rowlbl,192,"%s → %s  [%s]  0x%zX",r.klass->getName(),fn,tn,r.field->getOffset());
            bool sel=g_espManager.IsTracking()&&g_espManager.GetTrackedField()==r.field&&g_espManager.GetPosMode()==PositionMode::FieldDirect;
            if(ImGui::Selectable(rowlbl,sel)){
                if(!sel) try{g_espManager.StartTracking(r.klass,r.field);}catch(...){}
                else g_espManager.StopTracking();
            }
            if(r.isVec3) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        cl.End();
        {static float ssP=-1.f,ssPs=-1.f;FingerScroll(ssP,ssPs);}
        ImGui::EndChild();
        PopThickScrollbar();
    }

    // Conditions popup — rendered last (on top)
    DrawConditionsPopup(bw, sp, s_fieldEntries, tracking);
}
