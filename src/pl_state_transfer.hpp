// =============================================================
// pl_state_transfer.hpp / .cpp  —  Hot Reload State Preservation v5
// =============================================================
// Architecture Feature #5: Hot reload state preservation.
//
// When a script is hot-reloaded, the instance's exported property values
// should be preserved across the reload (state transfer).
//
// Mechanism:
//   1. Before hot_swap(), serialize the old instance's exported properties
//      into a StateSnapshot (Dictionary keyed by property name).
//   2. After hot_swap() creates the new foreign instance, restore the
//      snapshot into the new instance by calling pl_set_property() for
//      each saved key that exists in the new script's property list.
//
// The vtable pl_serialize_state / pl_deserialize_state slots are used
// when adapters implement them (e.g. to preserve non-exported state).
// For adapters that don't implement those slots, the generic property
// snapshot path covers @export vars automatically.
//
// Integration:
//   Called from PolyLangScriptInstance::hot_swap() (already in v5).
//   StateTransfer::save() is called before the swap.
//   StateTransfer::restore() is called after the new foreign handle is live.
// =============================================================
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "../include/pl_adapter_vtable.h"

namespace polylang {

class PolyLangScript;

// A snapshot of exported property values at a point in time.
struct StateSnapshot {
    // property_name → PLValue serialised as a Godot Variant.
    std::unordered_map<std::string, godot::Variant> values;
    bool empty() const { return values.empty(); }
};

class StateTransfer {
public:
    // Capture exported property values from a live foreign instance.
    // Uses pl_get_property_list() + pl_get_property() to enumerate.
    // Falls back to vtable->pl_serialize_state() if available.
    static StateSnapshot save(void* foreign_handle,
                               const PLAdapterVTable* vtable,
                               const PolyLangScript*  script);

    // Restore saved property values into a new foreign instance.
    // For each key in snapshot, calls pl_set_property() if the key
    // still exists in the new script's property list.
    // Falls back to vtable->pl_deserialize_state() if available.
    static void restore(const StateSnapshot&   snapshot,
                        void*                  new_foreign,
                        const PLAdapterVTable* vtable,
                        const PolyLangScript*  script);

private:
    static godot::Variant pl_to_variant(const PLValue& v);
    static PLValue        variant_to_pl(const godot::Variant& v);
};

} // namespace polylang
