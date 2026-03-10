#pragma once
// ============================================================================
// TypeUtils.h
// Centralized C# / IL2CPP type conversion helpers.
// Eliminates duplication that existed across ScriptInjector, ModMenuExporter,
// ESPCodeExporter, and ClassesTab.
// Namespace: Tool::TypeUtils
// ============================================================================
#include <string>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace Tool
{
namespace TypeUtils
{

// ─── Sanitize an arbitrary string to a valid C++/Lua identifier ──────────────
inline std::string Sanitize(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') out += c;
        else out += '_';
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;
    return out;
}

// ─── Upper-case conversion ────────────────────────────────────────────────────
inline std::string Upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

// ─── Map a C# type name to its C++ equivalent (for header generation) ────────
inline std::string CSharpToCppType(const std::string &typeName)
{
    if (typeName == "System.Int32"   || typeName == "Int32")   return "int32_t";
    if (typeName == "System.Int64"   || typeName == "Int64")   return "int64_t";
    if (typeName == "System.UInt32"  || typeName == "UInt32")  return "uint32_t";
    if (typeName == "System.UInt64"  || typeName == "UInt64")  return "uint64_t";
    if (typeName == "System.Int16"   || typeName == "Int16")   return "int16_t";
    if (typeName == "System.UInt16"  || typeName == "UInt16")  return "uint16_t";
    if (typeName == "System.Boolean" || typeName == "Boolean") return "bool";
    if (typeName == "System.Single"  || typeName == "Single")  return "float";
    if (typeName == "System.Double"  || typeName == "Double")  return "double";
    if (typeName == "System.Byte"    || typeName == "Byte")    return "uint8_t";
    if (typeName == "System.SByte"   || typeName == "SByte")   return "int8_t";
    if (typeName == "System.Char"    || typeName == "Char")    return "uint16_t";
    if (typeName == "System.String"  || typeName == "String")  return "Il2CppString*";
    if (typeName.find("Vector3")    != std::string::npos)      return "Vector3";
    if (typeName.find("Vector2")    != std::string::npos)      return "Vector2";
    if (typeName.find("Quaternion") != std::string::npos)      return "Quaternion";
    return "Il2CppObject*";
}

// ─── Map a C# type name to a GameGuardian Lua TYPE constant ──────────────────
inline std::string TypeToGGType(const std::string &typeName)
{
    if (typeName == "System.Int32"   || typeName == "Int32")   return "DWORD";
    if (typeName == "System.Int64"   || typeName == "Int64")   return "QWORD";
    if (typeName == "System.UInt32"  || typeName == "UInt32")  return "DWORD";
    if (typeName == "System.UInt64"  || typeName == "UInt64")  return "QWORD";
    if (typeName == "System.Int16"   || typeName == "Int16")   return "WORD";
    if (typeName == "System.UInt16"  || typeName == "UInt16")  return "WORD";
    if (typeName == "System.Single"  || typeName == "Single")  return "FLOAT";
    if (typeName == "System.Double"  || typeName == "Double")  return "DOUBLE";
    if (typeName == "System.Boolean" || typeName == "Boolean") return "BYTE";
    if (typeName == "System.Byte"    || typeName == "Byte")    return "BYTE";
    if (typeName.find("Vector3")    != std::string::npos)      return "FLOAT";
    if (typeName.find("Vector2")    != std::string::npos)      return "FLOAT";
    return "DWORD";
}

// ─── Byte-size of a C# primitive type ────────────────────────────────────────
inline size_t CSharpTypeSize(const std::string &cppType)
{
    if (cppType == "int64_t"  || cppType == "uint64_t" || cppType == "double"
     || cppType == "Il2CppObject*" || cppType == "Il2CppString*") return 8;
    if (cppType == "int16_t"  || cppType == "uint16_t") return 2;
    if (cppType == "bool"     || cppType == "uint8_t"  || cppType == "int8_t") return 1;
    if (cppType == "Vector3")   return 12;
    if (cppType == "Vector2")   return 8;
    if (cppType == "Quaternion") return 16;
    return 4; // int32_t, uint32_t, float, enum
}

// ─── Quick checks ─────────────────────────────────────────────────────────────
inline bool IsPrimitiveCSharp(const char *tn)
{
    if (!tn) return false;
    return !strcmp(tn,"System.Boolean")||!strcmp(tn,"System.Byte")   ||
           !strcmp(tn,"System.Int16")  ||!strcmp(tn,"System.UInt16") ||
           !strcmp(tn,"System.Int32")  ||!strcmp(tn,"System.UInt32") ||
           !strcmp(tn,"System.Int64")  ||!strcmp(tn,"System.UInt64") ||
           !strcmp(tn,"System.Single") ||!strcmp(tn,"System.Double");
}

inline bool IsVector3(const char *tn) { return tn && strstr(tn,"Vector3"); }
inline bool IsVector2(const char *tn) { return tn && strstr(tn,"Vector2") && !IsVector3(tn); }

// ─── Extract last segment of a file path ─────────────────────────────────────
inline std::string ExtractFilename(const std::string &fullPath)
{
    if (fullPath.empty()) return "";
    size_t pos = fullPath.find_last_of('/');
    return (pos != std::string::npos) ? fullPath.substr(pos + 1) : fullPath;
}

} // namespace TypeUtils
} // namespace Tool
