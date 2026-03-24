// =============================================================
// pl_state_transfer.cpp  —  Hot Reload State Preservation v5
// =============================================================
#include "pl_state_transfer.hpp"
#include "polylang_script.hpp"
#include "pl_export_parser.hpp"

#include <godot_cpp/core/error_macros.hpp>

namespace polylang {

// ── PLValue ↔ Variant helpers ─────────────────────────────────

/*static*/ godot::Variant StateTransfer::pl_to_variant(const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_BOOL:   return v.b;
        case PL_TYPE_INT:    return v.i;
        case PL_TYPE_FLOAT:  return v.f;
        case PL_TYPE_STRING:
            return v.s ? godot::Variant(godot::String(v.s)) : godot::Variant();
        default:             return godot::Variant();
    }
}

/*static*/ PLValue StateTransfer::variant_to_pl(const godot::Variant& var) {
    PLValue v{}; pl_value_init(&v);
    switch (var.get_type()) {
        case godot::Variant::BOOL:
            v.type = PL_TYPE_BOOL; v.b = (bool)var; break;
        case godot::Variant::INT:
            v.type = PL_TYPE_INT;  v.i = (int64_t)var; break;
        case godot::Variant::FLOAT:
            v.type = PL_TYPE_FLOAT; v.f = (double)var; break;
        case godot::Variant::STRING: {
            v.type = PL_TYPE_STRING;
            godot::String gs = var;
            v.s = strdup(gs.utf8().get_data()); // caller must free
            break;
        }
        default: break;
    }
    return v;
}

// ── StateTransfer::save ───────────────────────────────────────

/*static*/ StateSnapshot
StateTransfer::save(void* foreign,
                     const PLAdapterVTable* vt,
                     const PolyLangScript*  script) {
    StateSnapshot snap;
    if (!foreign || !vt) return snap;

    // Priority 1: adapter-native serialize
    if (vt->pl_serialize_state) {
        PLValue out{}; pl_value_init(&out);
        if (vt->pl_serialize_state(foreign, &out) == PL_OK
            && out.type == PL_TYPE_STRING && out.s) {
            snap.values["__native_state__"] = godot::String(out.s);
            if (vt->pl_free_value_contents) vt->pl_free_value_contents(&out);
            return snap;
        }
        if (vt->pl_free_value_contents) vt->pl_free_value_contents(&out);
    }

    // Priority 2: exported property enumeration
    // Build list of property names from the vtable (if available) or parser.
    std::vector<std::string> prop_names;

    void* compiled = nullptr; // can't access easily; use source-based parser
    // Use export parser on source text to get property names
    std::string source = script->get_source_code().utf8().get_data();
    if (!source.empty()) {
        auto props = PLExportParser::parse(source);
        for (const auto& p : props) prop_names.push_back(p.name);
    }

    // Also try vtable property list for adapter-declared properties
    if (vt->pl_get_property_list) {
        PLPropertyInfo* infos = nullptr;
        int32_t count = 0;
        // We need the compiled handle — get via PolyLangScript cast
        // (the compiled handle is what was passed; foreign is the instance)
        // We use the foreign handle directly for getting properties.
        // Note: pl_get_property_list takes a *compiled* handle, not instance.
        // Since we don't have it here, rely on the source-based path above.
    }

    // Snapshot each named property
    for (const auto& name : prop_names) {
        PLValue pv{}; pl_value_init(&pv);
        if (!vt->pl_get_property) break;
        int r = vt->pl_get_property(foreign, name.c_str(), &pv);
        if (r == PL_OK) {
            snap.values[name] = pl_to_variant(pv);
        }
        if (vt->pl_free_value_contents) vt->pl_free_value_contents(&pv);
    }

    return snap;
}

// ── StateTransfer::restore ────────────────────────────────────

/*static*/ void
StateTransfer::restore(const StateSnapshot&   snap,
                        void*                  new_foreign,
                        const PLAdapterVTable* vt,
                        const PolyLangScript*  /*script*/) {
    if (snap.empty() || !new_foreign || !vt) return;

    // Priority 1: adapter-native deserialize
    auto ns_it = snap.values.find("__native_state__");
    if (ns_it != snap.values.end() && vt->pl_deserialize_state) {
        PLValue in = variant_to_pl(ns_it->second);
        vt->pl_deserialize_state(new_foreign, &in);
        if (vt->pl_free_value_contents) vt->pl_free_value_contents(&in);
        return;
    }

    // Priority 2: restore property-by-property
    if (!vt->pl_set_property) return;
    for (const auto& [name, val] : snap.values) {
        if (name == "__native_state__") continue;
        PLValue pv = variant_to_pl(val);
        vt->pl_set_property(new_foreign, name.c_str(), &pv);
        if (vt->pl_free_value_contents) vt->pl_free_value_contents(&pv);
    }
}

} // namespace polylang
