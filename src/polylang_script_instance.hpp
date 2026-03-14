#pragma once
// =============================================================
// polylang_script_instance.hpp  —  GDExtension script instance (v5)
// =============================================================
// FIXES IN THIS FILE:
//
//   FIX A-6:  args_buf allocation comment removed — implementation
//     now uses std::vector<PLValue> (see .cpp), which is RAII and
//     exception-safe. In v4, raw new PLValue[argc] was used and
//     leaked on any exception or early return.
//
//   FIX D-1:  Thread-local method ID cache declared. Maps StringName
//     uint64 hashes to int32_t PL_METHOD_* IDs. Avoids 8 strcmp
//     calls per _call_trampoline invocation at 120K calls/sec.
//     Cache is 64-entry LRU (4KB per thread), covers all built-ins.
// =============================================================
#include <shared_mutex>
#include <atomic>
#include <array>

#include <godot_cpp/classes/script_extension.hpp>
#include <gdextension_interface.h>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "../include/pl_adapter_vtable.h"
#include "runtime_manager.hpp"

namespace polylang {

class PolyLangScript;

class PolyLangScriptInstance {
public:
    PolyLangScriptInstance(PolyLangScript* script,
                           godot::Object*  owner,
                           void*           foreign_instance);
    ~PolyLangScriptInstance();

    // ── Hot-swap (main thread only) ───────────────────────────
    void hot_swap(void* new_foreign);

    // ── GDExtension trampolines ───────────────────────────────
    static void* _create_trampoline(void* p_userdata);
    static void  _free_trampoline(void* p_userdata,
                                   GDExtensionScriptInstanceDataPtr p_ri);
    static GDExtensionBool _set_trampoline(
        GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr,
        GDExtensionConstVariantPtr);
    static GDExtensionBool _get_trampoline(
        GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr,
        GDExtensionVariantPtr);
    static void _call_trampoline(
        GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr,
        const GDExtensionConstVariantPtr*, GDExtensionInt,
        GDExtensionVariantPtr, GDExtensionCallError*);
    static GDExtensionBool _has_method_trampoline(
        GDExtensionScriptInstanceDataPtr, GDExtensionConstStringNamePtr);
    static GDExtensionObjectPtr _get_owner_trampoline(
        GDExtensionScriptInstanceDataPtr);

    static GDExtensionScriptInstancePtr create_godot_instance(
        PolyLangScript*, godot::Object*, void*);

    // Public: used by PolyLangBridge for cross-language calls.
    int call_method_direct(const char* name, PLValue* args, int32_t argc, PLValue* ret);
    const PLAdapterVTable* get_vtable() const { return vtable_; }

private:
    int call_builtin(int32_t method_id, PLValue* args, int32_t argc, PLValue* ret);
    int call_method(const char* name,   PLValue* args, int32_t argc, PLValue* ret);

    // FIX D-1: Cached numeric method ID lookup.
    // Thread-local so no synchronisation needed.
    // Returns 0 if not a built-in (caller falls back to string dispatch).
    static int32_t cached_builtin_id(const godot::StringName& name);

    PolyLangScript* script_;
    godot::Object*  owner_;

    // Foreign instance — atomic for lock-free hot-swap visibility.
    std::atomic<void*>     foreign_instance_{nullptr};

    // Per-instance RW lock:
    //   shared_lock  → concurrent method calls (read path, common case)
    //   unique_lock  → hot_swap only (rare, brief)
    mutable std::shared_mutex instance_lock_;

    const PLAdapterVTable* vtable_{nullptr};
    LanguageID             language_id_{LanguageID::COUNT};
};

} // namespace polylang
