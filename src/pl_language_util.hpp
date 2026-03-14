// =============================================================
// pl_language_util.hpp  —  PolyLang v6.5
// =============================================================
// Shared utility: canonical language name string → LanguageID.
// Avoids duplicate definitions scattered across .cpp files.
// =============================================================
#pragma once
#include "runtime_manager.hpp"
#include <string>

namespace polylang {

inline LanguageID language_from_string(const char* name) {
    if (!name) return LanguageID::COUNT;
    const std::string s(name);
    if (s == "lua")          return LanguageID::Lua;
    if (s == "python")       return LanguageID::Python;
    if (s == "javascript")   return LanguageID::JavaScript;
    if (s == "typescript")   return LanguageID::TypeScript;
    if (s == "squirrel")     return LanguageID::Squirrel;
    if (s == "wren")         return LanguageID::Wren;
    if (s == "angelscript")  return LanguageID::AngelScript;
    if (s == "julia")        return LanguageID::Julia;
    if (s == "kotlin")       return LanguageID::Kotlin;
    if (s == "go")           return LanguageID::Go;
    if (s == "swift")        return LanguageID::Swift;
    if (s == "haxe")         return LanguageID::Haxe;
    if (s == "csharp")       return LanguageID::CSharp;
    if (s == "nim")          return LanguageID::Nim;
    if (s == "rust")         return LanguageID::Rust;
    if (s == "zig")          return LanguageID::Zig;
    if (s == "odin")         return LanguageID::Odin;
    return LanguageID::COUNT;
}

} // namespace polylang
