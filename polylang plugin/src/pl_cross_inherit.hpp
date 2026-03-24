// =============================================================
// pl_cross_inherit.hpp  —  PolyLang v6.5
// =============================================================
// Cross-language inheritance allows a PolyLang script to designate
// another PolyLang script as its base, and to call super() on it.
//
// SUPPORTED PATTERNS:
//
//   1. Foreign → GDScript base
//      A Lua script declares `# base_script: res://base.gd`
//      in its sidecar or header. When a method is not found in the
//      Lua instance, it is looked up in the loaded GDScript and called.
//
//   2. Foreign → Foreign base (same or different language)
//      A Python script declares `# base_script: res://base.pl.lua`
//      Method resolution: Python first, then Lua base.
//      Property access follows the same chain.
//
//   3. Polyglot chain
//      A .poly file declares `# base_class: CharacterBody3D` in its
//      header; the PolyglotScript calls the Godot base class methods
//      via the ScriptExtension override chain automatically.
//
//   4. super() call
//      Adapters that implement pl_call_super receive a vtable slot
//      pointing to PLCrossInherit::call_super_impl() which walks the
//      inheritance chain upward.
//
// IMPLEMENTATION:
//   PLCrossInherit maintains a map of res_path → base_script_path.
//   This map is populated by:
//     a) RuntimeManager::maybe_register_sidecar() parsing the sidecar JSON.
//     b) PolyglotParser reading the header directive `base_script:`.
//
//   When PolyLangScriptInstance::call_method_direct() returns
//   PL_ERR_METHOD_NOT_FOUND, PLCrossInherit::try_base_call() is invoked
//   to walk the chain.
//
// SANDBOX:
//   Calling a base script from a Quarantined script is denied.
//   Isolated → Trusted base calls are allowed (base runs at Trusted tier).
// =============================================================
#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <godot_cpp/variant/variant.hpp>

#include "../include/pl_adapter_vtable.h"
#include "pl_sandbox_tiers.hpp"

namespace polylang {

class PolyLangScriptInstance;
class PolyglotInstance;

struct InheritanceEntry {
    std::string child_path;     // res path of the child script
    std::string base_path;      // res path of the base (may be .gd or .pl.*)
    SandboxTier child_tier{SandboxTier::Trusted};
};

class PLCrossInherit {
public:
    static PLCrossInherit* get_singleton();

    // ── Registration ──────────────────────────────────────────
    // Register a base script for a child script path.
    void register_base(const std::string& child_path,
                       const std::string& base_path,
                       SandboxTier child_tier = SandboxTier::Trusted);

    // Unregister (called when script is unloaded).
    void unregister(const std::string& child_path);
    static void destroy();

    // ── Method resolution chain ───────────────────────────────
    // Called when a PolyLang instance fails to find a method.
    // Walks the inheritance chain upward; returns PL_OK on success.
    int try_base_call(const std::string& child_path,
                      godot::Object*     owner,
                      const char*        method_name,
                      PLValue*           args, int32_t argc,
                      PLValue*           ret_out);

    // Property get chain.
    int try_base_get(const std::string& child_path,
                     godot::Object*     owner,
                     const char*        prop_name,
                     PLValue*           ret_out);

    // Property set chain.
    int try_base_set(const std::string& child_path,
                     godot::Object*     owner,
                     const char*        prop_name,
                     const PLValue*     value);

    // ── Adapter super() slot ──────────────────────────────────
    // Function pointer compatible with a vtable extension slot.
    // Signature: pl_call_super(child_instance*, method, args, argc, ret)
    // child_instance carries the res_path so we can look up the chain.
    static int pl_call_super_impl(void* child_instance_ptr,
                                   const char* method,
                                   PLValue* args, int32_t argc,
                                   PLValue* ret);

    // ── Utility ───────────────────────────────────────────────
    // Returns the base path for a child, or "" if none.
    std::string base_path_for(const std::string& child_path) const;

    // Build the full chain: child → base → base-of-base → ...
    std::vector<std::string> resolve_chain_locked(const std::string& child) const;
    std::vector<std::string> resolve_chain(const std::string& child_path) const;

private:
    PLCrossInherit() = default;

    // Invoke a method on a base script identified by res_path.
    // Handles both GDScript (.gd) and PolyLang (.pl.*) bases.
    int call_on_base(const std::string& base_path,
                     godot::Object*     owner,
                     const char*        method,
                     PLValue*           args, int32_t argc,
                     PLValue*           ret);

    int get_on_base(const std::string& base_path,
                    godot::Object*     owner,
                    const char*        prop,
                    PLValue*           ret);

    int set_on_base(const std::string& base_path,
                    godot::Object*     owner,
                    const char*        prop,
                    const PLValue*     val);

    mutable std::mutex                                   map_mutex_;
    std::unordered_map<std::string, InheritanceEntry>    entries_;

    static PLCrossInherit*                               singleton_;
};

} // namespace polylang
