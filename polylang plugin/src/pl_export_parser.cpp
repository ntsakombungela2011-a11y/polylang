// =============================================================
// pl_export_parser.cpp  —  PolyLang v6.6
//
// ZERO-TRUST AUDIT ROUND 2 FIX:
//
// FIX VLN-11 [MEDIUM]: No cap on @export var count.
//   BEFORE: parse() returns all matches from source regardless of count.
//     A 1 MB source (allowed by polyglot parser) containing 10,000
//     @export var lines produces 10,000 ExportedProperty objects,
//     causing unnecessary memory use and slow property_list iteration.
//   AFTER:  PL_EXPORT_MAX_VARS (512) cap. Iteration stops after 512
//     matches and an ERR_PRINT is emitted.
// =============================================================
#include "pl_export_parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace polylang {

static constexpr size_t PL_EXPORT_MAX_VARS = 512;

/*static*/ godot::Variant::Type
PLExportParser::type_name_to_variant(const std::string& t) {
    if (t == "float"   || t == "double") return godot::Variant::FLOAT;
    if (t == "int"     || t == "integer" || t == "Int" || t == "Integer")
                                          return godot::Variant::INT;
    if (t == "bool"    || t == "Bool")   return godot::Variant::BOOL;
    if (t == "String"  || t == "string") return godot::Variant::STRING;
    if (t == "Vector2" || t == "vec2")   return godot::Variant::VECTOR2;
    if (t == "Vector3" || t == "vec3")   return godot::Variant::VECTOR3;
    return godot::Variant::NIL;
}

/*static*/ godot::Variant
PLExportParser::parse_default(const std::string& type_name,
                               const std::string& raw) {
    if (raw.empty()) {
        godot::Variant::Type vt = type_name_to_variant(type_name);
        switch (vt) {
            case godot::Variant::FLOAT:  return 0.0f;
            case godot::Variant::INT:    return (int64_t)0;
            case godot::Variant::BOOL:   return false;
            case godot::Variant::STRING: return godot::String("");
            default:                     return godot::Variant();
        }
    }

    godot::Variant::Type vt = type_name_to_variant(type_name);
    std::string v = raw;
    while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
    while (!v.empty() && std::isspace((unsigned char)v.back()))  v.pop_back();

    switch (vt) {
        case godot::Variant::FLOAT:  return (float)std::atof(v.c_str());
        case godot::Variant::INT:    return (int64_t)std::atoll(v.c_str());
        case godot::Variant::BOOL:   return (v == "true" || v == "True" || v == "1");
        case godot::Variant::STRING: {
            if (v.size() >= 2 &&
                ((v.front()=='"' && v.back()=='"') ||
                 (v.front()=='\'' && v.back()=='\'')))
                v = v.substr(1, v.size() - 2);
            return godot::String(v.c_str());
        }
        default: return godot::Variant();
    }
}

/*static*/ std::vector<ExportedProperty>
PLExportParser::parse(const std::string& source) {
    std::vector<ExportedProperty> results;

    static const std::regex RE(
        R"((?:^|[\r\n])[ \t]*(?:--[ \t]*|//[ \t]*|#[ \t]*)?)"
        R"(@export[ \t]+var[ \t]+([A-Za-z_][A-Za-z0-9_]*))"
        R"([ \t]*:[ \t]*([A-Za-z][A-Za-z0-9_]*))"
        R"((?:[ \t]*=[ \t]*([^\r\n,};]*))?)",
        std::regex::ECMAScript);

    auto begin = std::sregex_iterator(source.cbegin(), source.cend(), RE);
    auto end   = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        // FIX VLN-11: stop processing after cap.
        if (results.size() >= PL_EXPORT_MAX_VARS) {
            ERR_PRINT(("[PolyLang/ExportParser] @export var count exceeds limit ("
                       + std::to_string(PL_EXPORT_MAX_VARS) + ") — truncating").c_str());
            break;
        }

        const std::smatch& m = *it;
        std::string name     = m[1].str();
        std::string type_str = m[2].str();
        std::string def_raw  = m[3].matched ? m[3].str() : "";

        while (!def_raw.empty() && (std::isspace((unsigned char)def_raw.back())
                                    || def_raw.back() == ';'))
            def_raw.pop_back();

        godot::Variant::Type vtype = type_name_to_variant(type_str);
        if (vtype == godot::Variant::NIL) vtype = godot::Variant::STRING;

        ExportedProperty prop;
        prop.name          = name;
        prop.type          = vtype;
        prop.default_value = parse_default(type_str, def_raw);
        prop.hint_string   = "";
        results.push_back(std::move(prop));
    }
    return results;
}

/*static*/ godot::Dictionary
PLExportParser::to_property_dict(const ExportedProperty& p) {
    godot::Dictionary d;
    d["name"]        = godot::String(p.name.c_str());
    d["type"]        = static_cast<int>(p.type);
    d["hint"]        = 0;
    d["hint_string"] = godot::String(p.hint_string.c_str());
    d["usage"]       = 6;
    return d;
}

/*static*/ godot::TypedArray<godot::Dictionary>
PLExportParser::to_property_array(const std::vector<ExportedProperty>& props) {
    godot::TypedArray<godot::Dictionary> arr;
    arr.resize(static_cast<int>(props.size()));
    for (int i = 0; i < (int)props.size(); ++i)
        arr[i] = to_property_dict(props[i]);
    return arr;
}

} // namespace polylang
