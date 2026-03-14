// =============================================================
// variant_bridge.hpp  —  PolyLang v6.5
// FIX H-5: depth-limited recursion prevents stack overflow
// =============================================================
#pragma once
#include "../include/pl_adapter_vtable.h"
#include <godot_cpp/variant/variant.hpp>

namespace polylang {

constexpr int PL_MAX_CONVERSION_DEPTH = 64;

class VariantBridge {
public:
    static void to_pl_value(const godot::Variant& in, PLValue& out,
                            int depth = 0);
    static godot::Variant from_pl_value(const PLValue& in, int depth = 0);
    static void free_pl_value(PLValue& v, int depth = 0);
};

} // namespace polylang
