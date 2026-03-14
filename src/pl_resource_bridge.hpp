// =============================================================
// pl_resource_bridge.hpp  —  PolyLang v6.5
// =============================================================
// PLResourceBridge provides adapters access to Godot's resource system.
//
// Adapters receive a function pointer table (OdinRuntimeServices-style)
// injected at instantiation time.  Two of those slots are:
//
//   resource_fetch(path, PLValue* out) → int
//   resource_release(PLValue* v)       → void
//
// This file implements the C-callable functions that back those slots.
// They are also exposed as a Godot singleton for GDScript.
//
// Supported resource types via PLValue:
//   PL_TYPE_STRING  → the resource path (for use with load())
//   PL_TYPE_INT     → resource object ID (Object::get_instance_id())
//   PL_TYPE_NIL     → resource not found / null
//
// SANDBOX:
//   Resources requested from Quarantined scripts are denied entirely.
//   Isolated scripts may only read resources whose path starts with
//   "res://mods/" (configurable via RuntimeManager).
//   Trusted scripts have unrestricted access.
//
// THREAD SAFETY:
//   fetch() and release() are safe from any thread. Actual ResourceLoader
//   calls are marshalled to the main thread via a request queue flushed
//   each frame, with callers blocked on a condition variable.
//   In the common case (main thread caller) the resource is loaded directly.
// =============================================================
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "../include/pl_adapter_vtable.h"
#include "pl_sandbox_tiers.hpp"

namespace polylang {

class PLResourceBridge : public godot::Object {
    GDCLASS(PLResourceBridge, godot::Object)

public:
    static PLResourceBridge* get_singleton();

    // ── Adapter C-callable API ────────────────────────────────
    // These are the function pointers injected into adapter runtime services.

    // Load a resource by path. Fills out with PL_TYPE_INT (object id) or NIL.
    // sandbox_tier controls access checks.
    static int  pl_resource_fetch_impl(const char* res_path, PLValue* out,
                                        SandboxTier tier);
    static void pl_resource_release_impl(PLValue* v);

    // ── Flush (main thread) ───────────────────────────────────
    // Process deferred resource loads queued from worker threads.
    void flush();

    // ── GDScript API ──────────────────────────────────────────
    godot::Variant load_resource(const godot::String& path);
    bool           is_path_allowed(const godot::String& path,
                                    int sandbox_tier) const;

    // ── Internal: cache a loaded resource ─────────────────────
    void cache_resource(const std::string& path,
                        godot::Ref<godot::Resource> res);

protected:
    static void _bind_methods();

private:

    PLResourceBridge();
    bool is_main_thread() const;
    bool check_access(const std::string& path, SandboxTier tier) const;

    // Cache: path → weakly-held resource (Ref keeps it alive as long as
    // any foreign script holds a PLValue referencing its id).
    mutable std::mutex                                          cache_mutex_;
    std::unordered_map<std::string, godot::Ref<godot::Resource>> cache_;

    // Deferred-load queue for worker-thread callers.
    struct DeferredRequest {
        std::string   path;
        SandboxTier   tier;
        PLValue*      out;
        std::mutex*   done_mutex;
        std::condition_variable* done_cv;
        bool*         done_flag;
    };
    std::mutex                         deferred_mutex_;
    std::thread::id                    main_thread_id_;
    std::queue<DeferredRequest>        deferred_queue_;

    static PLResourceBridge* singleton_;
};

// ── Convenience wrappers for adapter injection ─────────────────
// These match the pl_resource_fetch / pl_resource_release vtable slots.
// They always use Trusted tier (adapters injected into trusted contexts).

inline int pl_resource_fetch_trusted(const char* path, PLValue* out) {
    return PLResourceBridge::pl_resource_fetch_impl(
        path, out, SandboxTier::Trusted);
}

inline void pl_resource_release(PLValue* v) {
    PLResourceBridge::pl_resource_release_impl(v);
}

} // namespace polylang
