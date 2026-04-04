// =============================================================
// pl_bridge.hpp / pl_bridge.cpp  —  PolyLang Cross-Language Bridge v5
// =============================================================
// Architecture Feature #3: Cross-language calls.
//
// PolyLangBridge::call(script_path, method_name, args) lets any adapter
// call a method on any other loaded PolyLang script, regardless of language.
//
// Flow:
//   1. Caller adapter calls polylang_bridge_call() injected into its VM.
//   2. Bridge looks up the target PolyLangScript by res:// path in the
//      ScriptRegistry (populated when scripts are instantiated).
//   3. Bridge calls vtable->pl_call_method() on the target's foreign handle.
//   4. Result is converted and returned to the caller.
//
// THREAD SAFETY:
//   Registry access is protected by a shared_mutex. Script instances may
//   only be safely called from the main thread (Godot requirement).
// =============================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "../include/pl_adapter_vtable.h"

namespace polylang {

class PolyLangScript;
class PolyLangScriptInstance;
class PolyglotInstance;

enum class PLScriptKind : uint8_t {
    PolyLang,
    Polyglot,
};

struct PLScriptHandle {
    PLScriptKind kind{PLScriptKind::PolyLang};
    void*        ptr{nullptr};

    bool valid() const { return ptr != nullptr; }
};

// ── Script registry ───────────────────────────────────────────
// Maintains a map of res:// path → live script instances.
// Both PolyLangScriptInstance and PolyglotInstance register here.

class PLScriptRegistry {
public:
    static PLScriptRegistry* get_singleton();
    static void create();
    static void destroy();

    void register_instance(const std::string& path,
                           godot::Object* owner,
                           PolyLangScriptInstance* inst);
    void unregister_instance(const std::string& path,
                             godot::Object* owner,
                             PolyLangScriptInstance* inst);
    void register_polyglot(const std::string& path,
                           godot::Object* owner,
                           PolyglotInstance* inst);
    void unregister_polyglot(const std::string& path,
                             godot::Object* owner,
                             PolyglotInstance* inst);

    // Returns the first live PolyLang instance for the given path, or nullptr.
    PolyLangScriptInstance* find_instance(const std::string& path) const;

    // Returns all live PolyLang instances for the path (there may be multiple).
    std::vector<PolyLangScriptInstance*> find_all(const std::string& path) const;
    PLScriptHandle find_handle(const std::string& path) const;
    PLScriptHandle find_owner(godot::Object* owner) const;

private:
    mutable std::shared_mutex                            mutex_;
    std::unordered_map<std::string, std::vector<PLScriptHandle>> map_;
    std::unordered_map<godot::Object*, PLScriptHandle>   owner_map_;

public:
    static PLScriptRegistry* singleton_;
};

// ── PolyLangBridge ────────────────────────────────────────────

class PolyLangBridge : public godot::Object {
    GDCLASS(PolyLangBridge, godot::Object)

public:
    static PolyLangBridge* get_singleton();

    // Call a method on a loaded PolyLang script instance.
    // path:   res:// path of the target script (e.g. "res://mods/Enemy.pl.lua")
    // method: name of the method to call
    // args:   array of arguments (PLValue-compatible types)
    // Returns Variant with the result, or null on failure.
    godot::Variant call_script(const godot::String& path,
                                const godot::String& method,
                                const godot::Array&  args);

    // Native (adapter) version — called directly from bridge functions injected
    // into adapter VMs.  Returns PLValue (caller must free contents).
    int call_native(const std::string& path,
                    const std::string& method,
                    PLValue* args, int32_t argc,
                    PLValue* ret_out);

protected:
    static void _bind_methods();

public:
    PolyLangBridge() = default;
    static PolyLangBridge* singleton_;
};

} // namespace polylang
