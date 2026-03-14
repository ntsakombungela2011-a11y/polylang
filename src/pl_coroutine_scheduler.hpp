// =============================================================
// pl_coroutine_scheduler.hpp  —  PolyLang v6.5
// FIX C-2: tick() no longer calls on_done while holding active_mutex_.
// FIX C-3: spawn() saves mc.id before std::move.
// FIX C-4: shutdown() disconnects all signal listeners before destruction.
// =============================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "../include/pl_adapter_vtable.h"

namespace polylang {

enum class CoroWake { NextFrame, AfterSeconds, OnSignal };

struct ManagedCoroutine {
    uint64_t          id{0};
    PLAdapterVTable*  vtable{nullptr};
    void*             handle{nullptr};
    CoroWake          wake{CoroWake::NextFrame};
    double            sleep_remaining{0.0};
    std::string       signal_name;
    godot::Callable   on_done;
    uint64_t          signal_listener_id{0};
    PLValue           signal_send_value{};
    bool              signal_fired{false};
};

// Collected during tick() for post-lock dispatch.
struct FinishedCoro {
    uint64_t          id;
    godot::Callable   on_done;
    godot::Variant    result;
    uint64_t          signal_listener_id;
    bool              vtable_ok;
};

class PLCoroutineScheduler : public godot::Object {
    GDCLASS(PLCoroutineScheduler, godot::Object)
public:
    static PLCoroutineScheduler* get_singleton();

    // Spawn and register a coroutine. Returns the coroutine ID (or 0 on failure).
    uint64_t spawn(PLAdapterVTable* vtable, void* coro_handle,
                   CoroWake wake,
                   double sleep_seconds = 0.0,
                   const std::string& signal_name = "",
                   godot::Callable on_done = godot::Callable());

    void cancel(uint64_t id);
    bool is_running(uint64_t id) const;

    // Called once per frame from PolyLangLanguage::_frame().
    void tick(double delta_seconds);

    // FIX C-4: Disconnect all signal listeners before shutdown.
    void shutdown();

    // GDScript bindings
    int64_t spawn_next_frame(godot::Object* owner, const godot::String& method,
                             const godot::Callable& on_done);
    int64_t spawn_after(godot::Object* owner, const godot::String& method,
                        double seconds, const godot::Callable& on_done);
    int64_t spawn_on_signal(godot::Object* owner, const godot::String& method,
                            const godot::String& signal, const godot::Callable& on_done);
    void    gd_cancel(int64_t id);
    bool    gd_is_running(int64_t id) const;

    static PLCoroutineScheduler* singleton_;

protected:
    static void _bind_methods();

private:
    uint64_t next_id();

    // Returns finished-coro data for post-lock dispatch (may erase from active_).
    // Caller MUST hold active_mutex_.
    bool step_one(ManagedCoroutine& mc, double delta, FinishedCoro& out_done);

    std::atomic<uint64_t>                        id_counter_{1};

    mutable std::mutex                           pending_mutex_;
    std::vector<ManagedCoroutine>                pending_spawn_;

    mutable std::mutex                           active_mutex_;
    std::unordered_map<uint64_t, ManagedCoroutine> active_;
};

} // namespace polylang
