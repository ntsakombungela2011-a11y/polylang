// =============================================================
// pl_export_parser.hpp / .cpp  —  Typed Export Variable Parser v5
// =============================================================
// Architecture Feature #1: Typed export variables.
//
// Parses "@export var name: type = default" annotations from script
// source text (language-agnostic comment/annotation scanning).
// Exposes parsed properties to Godot inspector via
// PolyLangScript::_get_script_property_list().
//
// Supported syntax variants (found in any line/comment style):
//   @export var speed: float = 5.0
//   @export var label: String = "hello"
//   @export var active: bool = true
//   @export var count: int = 0
//   # @export var hidden: int = 3         (comment-prefixed, still parsed)
//   -- @export var lua_var: float = 1.0   (Lua comment style)
//
// Type mapping → Godot Variant::Type:
//   float / double → FLOAT    int / integer → INT
//   bool           → BOOL     String/string → STRING
//   Vector2        → VECTOR2  Vector3       → VECTOR3
// =============================================================
#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace polylang {

struct ExportedProperty {
    std::string       name;
    godot::Variant::Type  type{godot::Variant::NIL};
    godot::Variant    default_value;
    std::string       hint_string;  // e.g. enum hints, range hints (future)
};

class PLExportParser {
public:
    // Parse all @export var annotations from source.
    // Returns list in declaration order.
    static std::vector<ExportedProperty> parse(const std::string& source);

    // Convert a single ExportedProperty to the Dictionary format
    // Godot expects from _get_script_property_list().
    static godot::Dictionary to_property_dict(const ExportedProperty& p);

    // Convert a parsed list to a TypedArray<Dictionary> ready for Godot.
    static godot::TypedArray<godot::Dictionary>
    to_property_array(const std::vector<ExportedProperty>& props);

private:
    static godot::Variant::Type type_name_to_variant(const std::string& type_name);
    static godot::Variant       parse_default(const std::string& type_name,
                                               const std::string& raw_value);
};

} // namespace polylang
