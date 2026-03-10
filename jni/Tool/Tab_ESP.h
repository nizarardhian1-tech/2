#pragma once
#include "imgui/imgui.h"
#include "Il2cpp/il2cpp-class.h"
#include <string>
#include <vector>

namespace ESPTab
{
    enum class CondOp : int { EQ=0, NEQ, GT, LT, GTE, LTE };

    struct Condition {
        char       fieldName[64] = {};
        char       valueStr[64]  = {};
        CondOp     op            = CondOp::EQ;
        FieldInfo *cachedField   = nullptr;
        bool       active        = true;
        bool       boolVal       = false;
        int        intVal        = 0;
        float      floatVal      = 0.f;

        const char* opLabel() const;
        bool evaluate(Il2CppObject *obj) const;
    };

    enum class FieldKind { Bool, Int, Float, Vector, Other };

    struct FieldEntry {
        FieldInfo  *field = nullptr;
        std::string name;
        std::string label;
        ImVec4      color = {};
        FieldKind   kind  = FieldKind::Other;
    };

    struct MethodEntry {
        MethodInfo  *method       = nullptr;
        std::string  name;
        std::string  returnType;
        int          paramCount   = 0;
        bool         returnsVec3  = false;
        bool         returnsTrans = false;
        uintptr_t    rva          = 0;
    };

    void Draw();
    void Render();
    bool EvaluateConditions(Il2CppObject *obj);

} // namespace ESPTab
