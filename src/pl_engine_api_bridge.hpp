// =============================================================
// pl_engine_api_bridge.hpp  —  PolyLang v6.5
// =============================================================
// The EngineAPIBridge provides foreign scripts structured access to
// Godot subsystems without requiring them to use GDNative/GDExtension
// directly.  Calls are routed through PLValue-typed arguments and
// dispatched by a string-keyed API table.
//
// SUPPORTED API GROUPS:
//
//   "physics"
//     physics.raycast(from: vec3, to: vec3) → { hit, position, normal, collider_id }
//     physics.move_and_collide(body_id: int, motion: vec3) → collision dict
//     physics.get_gravity() → vec3
//
//   "audio"
//     audio.play(stream_path: string, bus: string, volume_db: float)
//     audio.stop(player_id: int)
//     audio.set_volume(player_id: int, db: float)
//
//   "input"
//     input.is_action_pressed(action: string) → bool
//     input.get_axis(neg: string, pos: string) → float
//     input.get_vector(nx, px, ny, py: string) → vec2
//
//   "scene"
//     scene.instantiate(packed_scene_path: string) → node_id: int
//     scene.get_node(owner_id: int, path: string) → node_id: int
//     scene.queue_free(node_id: int)
//     scene.add_child(parent_id: int, child_id: int)
//
//   "time"
//     time.get_ticks_msec() → int
//     time.get_unix_time() → float
//
// USAGE FROM ADAPTERS:
//   Adapters receive an `engine_call` function in their runtime services:
//     int engine_call(const char* api_group, const char* method,
//                     PLValue* args, int32_t argc, PLValue* ret_out)
//
// SANDBOX:
//   Quarantined: no engine calls allowed.
//   Isolated: only "time" and "input" groups.
//   Trusted: all groups.
//
// THREAD SAFETY:
//   All calls that touch Godot nodes/physics must execute on the main
//   thread.  Calls from worker threads are deferred via PLSignalBus and
//   the caller blocks until the result is available (synchronous
//   request-response through a condition variable).
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
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "../include/pl_adapter_vtable.h"
#include "pl_sandbox_tiers.hpp"

namespace polylang {

// Signature for a single API handler.
using EngineAPIHandler = std::function<int(PLValue* args, int32_t argc, PLValue* ret)>;

class PLEngineAPIBridge : public godot::Object {
    GDCLASS(PLEngineAPIBridge, godot::Object)

public:
    static PLEngineAPIBridge* get_singleton();

    // ── Adapter C-callable entry point ────────────────────────
    // This is the function pointer injected into adapter runtime tables.
    static int pl_engine_call_trusted(const char* g, const char* m, PLValue* a, int32_t n, PLValue* r);
    static int pl_engine_call_impl(const char* api_group,
                                    const char* method,
                                    PLValue*    args,
                                    int32_t     argc,
                                    PLValue*    ret_out,
                                    SandboxTier tier);



    // ── Main-thread flush ─────────────────────────────────────
    // Process deferred engine calls queued from worker threads.
    void flush();

    // ── GDScript API (for testing / editor use) ───────────────
    godot::Dictionary call_api(const godot::String& group,
                                const godot::String& method,
                                const godot::Array&  args);

protected:
    static void _bind_methods();

private:
    PLEngineAPIBridge();
    friend void initialize_polylang(godot::ModuleInitializationLevel);
    friend void uninitialize_polylang(godot::ModuleInitializationLevel);

    void register_all_handlers();

    // Physics
    int h_physics_raycast(PLValue* args, int32_t argc, PLValue* ret);
    int h_physics_get_gravity(PLValue* args, int32_t argc, PLValue* ret);

    // Audio
    int h_audio_play(PLValue* args, int32_t argc, PLValue* ret);
    int h_audio_stop(PLValue* args, int32_t argc, PLValue* ret);

    // Input
    int h_input_is_action_pressed(PLValue* args, int32_t argc, PLValue* ret);
    int h_input_get_axis(PLValue* args, int32_t argc, PLValue* ret);
    int h_input_get_vector(PLValue* args, int32_t argc, PLValue* ret);

    // Scene
    int h_scene_instantiate(PLValue* args, int32_t argc, PLValue* ret);
    int h_scene_get_node(PLValue* args, int32_t argc, PLValue* ret);
    int h_scene_queue_free(PLValue* args, int32_t argc, PLValue* ret);
    int h_scene_add_child(PLValue* args, int32_t argc, PLValue* ret);

    // Time
    int h_time_get_ticks_msec(PLValue* args, int32_t argc, PLValue* ret);
    int h_time_get_unix(PLValue* args, int32_t argc, PLValue* ret);

    static bool group_allowed(const std::string& group, SandboxTier tier);

    // group → method → handler
    std::unordered_map<std::string,
        std::unordered_map<std::string, EngineAPIHandler>> handlers_;

    // Deferred-call queue (worker thread → main thread).
    struct DeferredCall {
        std::string   group;
        std::string   method;
        PLValue       args[8]; // max 8 args for deferred calls
        int32_t       argc{0};
        PLValue*      ret_out{nullptr};
        int*          rc_out{nullptr};
        SandboxTier   tier{SandboxTier::Trusted};
        std::mutex*           done_mutex{nullptr};
        std::condition_variable* done_cv{nullptr};
        bool*                 done_flag{nullptr};
    };
    std::mutex                   deferred_mutex_;
    std::queue<DeferredCall>     deferred_queue_;

    static PLEngineAPIBridge*    singleton_;
};

} // namespace polylang
