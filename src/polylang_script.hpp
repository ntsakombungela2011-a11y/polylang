#pragma once
// =============================================================
// polylang_script.hpp  —  Script resource (v5)
// =============================================================
// FIXES IN THIS FILE:
//
//   FIX A-4:  Removed notify_instance_destroyed from ~PolyLangScript.
//     A script resource is not an instance. Calling notify_instance_destroyed
//     in the destructor decremented active_instances for the SCRIPT
//     rather than an actual node instance, wrapping the counter to 2^32-1.
//     This prevented idle eviction and caused memory pressure.
//
//   FIX A-8:  trigger_compile() / _reload() no longer call compile_internal()
//     directly. All compilation goes through HotReloadScheduler::enqueue_reload()
//     which runs on a worker thread. The main thread is never stalled.
//
//   FIX B-7:  detect_language_from_path() has an early-return guard.
//     In v4 it ran on every trigger_compile() AND _reload() call even when
//     language_id_ was already known. require_vtable() behind it could
//     attempt dlopen on every reload cycle for no reason.
//
//   FIX B-8:  pending_handle_ is std::atomic<void*> so it is safe to
//     read from workers and write from main thread without a lock.
//     Previously it was a raw pointer with no protection.
// =============================================================
#include <shared_mutex>
#include <unordered_set>
#include <vector>
#include <atomic>
#include <string>

#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include "../include/pl_adapter_vtable.h"
#include "runtime_manager.hpp"

namespace polylang {

class PolyLangScriptInstance;

class PolyLangScript final : public godot::ScriptExtension {
    GDCLASS(PolyLangScript, godot::ScriptExtension)

public:
    PolyLangScript();
    ~PolyLangScript() override;

    // ── Language identification ───────────────────────────────
    LanguageID             get_language_id() const { return language_id_; }
    const PLAdapterVTable* get_vtable()      const { return vtable_; }
    void* get_compiled_handle() const {
        return compiled_handle_.load(std::memory_order_acquire);
    }

    // ── Source management ─────────────────────────────────────
    void          set_source_code(const godot::String& p_code);
    godot::String get_source_code() const;

    // ── Instance registry ─────────────────────────────────────
    void register_instance(PolyLangScriptInstance* inst);
    void unregister_instance(PolyLangScriptInstance* inst);
    std::vector<PolyLangScriptInstance*> snapshot_instances() const;

    // ── Hot-reload support ────────────────────────────────────
    // Called by HotReloadScheduler after ALL instances have successfully swapped.
    void apply_new_compiled_handle(void* new_handle);

    // ── ScriptExtension overrides ─────────────────────────────
    bool _can_instantiate()  const override;
    bool _is_valid()         const override;
    bool _is_tool()          const override { return false; }
    bool _is_abstract()      const override { return false; }

    godot::Ref<godot::Script>  _get_base_script()      const override { return {}; }
    godot::StringName          _get_global_name()       const override { return {}; }
    bool _inherits_script(const godot::Ref<godot::Script>&) const override { return false; }
    godot::StringName          _get_instance_base_type() const override { return "Node"; }

    void* _instance_create(godot::Object* p_object) const override;
    void* _placeholder_instance_create(godot::Object*) const override { return nullptr; }
    bool  _is_placeholder_fallback_enabled()            const override { return false; }

    bool _has_source_code()  const override;
    bool _has_method(const godot::StringName& p_method) const override;
    bool _has_static_method(const godot::StringName&)   const override { return false; }

    godot::Variant     _get_method_info(const godot::StringName&) const override;
    godot::ScriptLanguage* _get_language() const override;

    bool         _has_property_default_value(const godot::StringName&) const override { return false; }
    godot::Variant _get_property_default_value(const godot::StringName&) const override { return {}; }

    void _update_exports() override {}
    void _reload(bool keep_state) override;  // FIX A-8: async-only now

    godot::TypedArray<godot::Dictionary> _get_script_method_list()   const override;
    godot::TypedArray<godot::Dictionary> _get_script_property_list() const override;
    int32_t _get_member_line(const godot::StringName&) const override { return -1; }

    godot::Dictionary _get_constants()  const override { return {}; }
    godot::TypedArray<godot::StringName> _get_members() const override { return {}; }
    bool _is_placeholder_instance(const godot::Object*) const override { return false; }

    static void _bind_methods();

private:
    // FIX B-7: Early-return guard — detect_language_from_path is O(N·strlen).
    void detect_language_from_path();
    bool language_detected_{false};  // FIX B-7: prevent re-detection

    // FIX A-8: Schedules async compilation via HotReloadScheduler.
    // Does NOT call pl_compile on the main thread.
    void schedule_async_compile();

    godot::String             source_code_;
    mutable std::shared_mutex source_mutex_;

    LanguageID             language_id_{LanguageID::COUNT};
    const PLAdapterVTable* vtable_{nullptr};

    // Atomic handles — hot_swap and apply_new_compiled_handle write with release,
    // all reads use acquire. Avoids locking in the common read path.
    std::atomic<void*>     compiled_handle_{nullptr};

    // FIX B-8: pending_handle_ was a raw pointer, unsafe across threads.
    // Now atomic<void*> so worker thread can read it without a lock.
    std::atomic<void*>     pending_handle_{nullptr};

    mutable std::shared_mutex              instances_mutex_;
    std::unordered_set<PolyLangScriptInstance*> instances_;
};

} // namespace polylang
