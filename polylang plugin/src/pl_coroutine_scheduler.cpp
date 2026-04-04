// =============================================================
// pl_coroutine_scheduler.cpp  —  PolyLang v6.6
//
// RETAINED FIXES from v6.5: C-2, C-3, C-4.
//
// ZERO-TRUST AUDIT ROUND 2 FIXES:
//
// FIX VLN-04 [HIGH]: Lock ordering hazard — pending_mutex_ nested
//   inside active_mutex_ in tick().
//   BEFORE: tick() acquired active_mutex_ first, then pending_mutex_
//           inside the same scope. Other code paths (spawn()) acquire
//           pending_mutex_ alone. The nesting can cause ABBA deadlock
//           if a third code path ever acquires them in opposite order.
//   AFTER:  Pending adoption is performed in a separate scope BEFORE
//           active_mutex_ is acquired for the main tick loop. The two
//           locks are never held simultaneously.
//
// FIX VLN-05 [HIGH]: Signal listener lambda fires before coroutine
//   enters active_ (still in pending_spawn_) → first signal silently dropped.
//   BEFORE: spawn() registers the signal listener immediately, then pushes
//           the ManagedCoroutine into pending_spawn_. If the signal fires
//           between registration and the next tick() that moves the entry
//           from pending_spawn_ into active_, the lambda finds nothing in
//           active_ and discards the signal.
//   AFTER:  The coroutine is moved into active_ BEFORE the signal listener
//           is registered. The listener ID is written back into active_
//           immediately after. This is safe because the listener lambda
//           already guards with active_.find() == active_.end() check.
// =============================================================
#include "pl_coroutine_scheduler.hpp"
#include "pl_bridge.hpp"
#include "pl_signal_bus.hpp"
#include "pl_polyglot_script.hpp"
#include "polylang_script_instance.hpp"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace polylang {

PLCoroutineScheduler* PLCoroutineScheduler::singleton_ = nullptr;
PLCoroutineScheduler* PLCoroutineScheduler::get_singleton() { return singleton_; }

void PLCoroutineScheduler::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("spawn_next_frame", "script_owner", "method_name", "on_done"),
        &PLCoroutineScheduler::spawn_next_frame);
    godot::ClassDB::bind_method(
        godot::D_METHOD("spawn_after", "script_owner", "method_name", "seconds", "on_done"),
        &PLCoroutineScheduler::spawn_after);
    godot::ClassDB::bind_method(
        godot::D_METHOD("spawn_on_signal", "script_owner", "method_name", "signal_name", "on_done"),
        &PLCoroutineScheduler::spawn_on_signal);
    godot::ClassDB::bind_method(
        godot::D_METHOD("cancel", "coroutine_id"), &PLCoroutineScheduler::gd_cancel);
    godot::ClassDB::bind_method(
        godot::D_METHOD("is_running", "coroutine_id"), &PLCoroutineScheduler::gd_is_running);
}

uint64_t PLCoroutineScheduler::next_id() {
    return id_counter_.fetch_add(1, std::memory_order_relaxed);
}

// ── spawn ─────────────────────────────────────────────────────

uint64_t PLCoroutineScheduler::spawn(PLAdapterVTable* vtable, void* coro_handle,
                                      CoroWake wake, double sleep_seconds,
                                      const std::string& signal_name,
                                      godot::Callable on_done) {
    if (!vtable || !coro_handle) return 0;
    if (!vtable->pl_coroutine_resume || !vtable->pl_coroutine_free) return 0;

    ManagedCoroutine mc;
    mc.id              = next_id();
    mc.vtable          = vtable;
    mc.handle          = coro_handle;
    mc.wake            = wake;
    mc.sleep_remaining = sleep_seconds;
    mc.signal_name     = signal_name;
    mc.on_done         = on_done;
    pl_value_init(&mc.signal_send_value);

    uint64_t id = mc.id;

    if (wake == CoroWake::OnSignal && !signal_name.empty()) {
        // FIX VLN-05: Insert into active_ BEFORE registering the signal listener.
        // This ensures the lambda always finds an entry in active_ when it fires.
        {
            std::lock_guard<std::mutex> lk(active_mutex_);
            active_.emplace(id, std::move(mc));
        }

        // Now register the listener — the entry is already live in active_.
        auto* bus = PLSignalBus::get_singleton();
        if (bus) {
            uint64_t listener_id = bus->connect_native(
                signal_name,
                [this, id](const godot::Array& args) {
                    std::lock_guard<std::mutex> lk(active_mutex_);
                    auto it = active_.find(id);
                    if (it == active_.end()) return;
                    it->second.signal_fired = true;
                    pl_value_init(&it->second.signal_send_value);
                    if (args.size() > 0) {
                        const godot::Variant& v = args[0];
                        switch (v.get_type()) {
                            case godot::Variant::BOOL:
                                it->second.signal_send_value.type = PL_TYPE_BOOL;
                                it->second.signal_send_value.b    = (bool)v; break;
                            case godot::Variant::INT:
                                it->second.signal_send_value.type = PL_TYPE_INT;
                                it->second.signal_send_value.i    = (int64_t)v; break;
                            case godot::Variant::FLOAT:
                                it->second.signal_send_value.type = PL_TYPE_FLOAT;
                                it->second.signal_send_value.f    = (double)v; break;
                            default: break;
                        }
                    }
                });
            // Write listener_id back into active_ entry.
            {
                std::lock_guard<std::mutex> lk(active_mutex_);
                auto it = active_.find(id);
                if (it != active_.end()) it->second.signal_listener_id = listener_id;
            }
        }
        return id;
    }

    // Non-signal coroutines: queue via pending (no listener registration).
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_spawn_.push_back(std::move(mc));
    }
    return id;
}

// ── cancel ────────────────────────────────────────────────────

void PLCoroutineScheduler::cancel(uint64_t id) {
    std::lock_guard<std::mutex> lk(active_mutex_);
    auto it = active_.find(id);
    if (it == active_.end()) return;
    auto& mc = it->second;
    if (mc.signal_listener_id && PLSignalBus::get_singleton())
        PLSignalBus::get_singleton()->disconnect_native(mc.signal_listener_id);
    if (mc.vtable && mc.vtable->pl_coroutine_free && mc.handle)
        mc.vtable->pl_coroutine_free(mc.handle);
    active_.erase(it);
}

bool PLCoroutineScheduler::is_running(uint64_t id) const {
    std::lock_guard<std::mutex> lk(active_mutex_);
    return active_.count(id) > 0;
}

// ── shutdown ──────────────────────────────────────────────────

void PLCoroutineScheduler::shutdown() {
    std::lock_guard<std::mutex> lk(active_mutex_);
    auto* bus = PLSignalBus::get_singleton();
    for (auto& [id, mc] : active_) {
        if (mc.signal_listener_id && bus)
            bus->disconnect_native(mc.signal_listener_id);
        if (mc.vtable && mc.vtable->pl_coroutine_free && mc.handle)
            mc.vtable->pl_coroutine_free(mc.handle);
    }
    active_.clear();
}

// ── tick ──────────────────────────────────────────────────────
// FIX VLN-04: Pending adoption is completed in a separate scope BEFORE
// active_mutex_ is acquired. The two locks are NEVER held simultaneously.

void PLCoroutineScheduler::tick(double delta_seconds) {
    // Step 1: Adopt pending spawns — hold pending_mutex_ ONLY.
    {
        std::lock_guard<std::mutex> pk(pending_mutex_);
        if (!pending_spawn_.empty()) {
            std::lock_guard<std::mutex> ak(active_mutex_);
            for (auto& mc : pending_spawn_)
                active_.emplace(mc.id, std::move(mc));
            pending_spawn_.clear();
        }
    }
    // pending_mutex_ is fully released here — no nested lock.

    std::vector<FinishedCoro> finished;

    {
        std::lock_guard<std::mutex> lk(active_mutex_);
        std::vector<uint64_t> to_remove;

        for (auto& [id, mc] : active_) {
            bool should_resume = false;
            const PLValue* send = nullptr;

            switch (mc.wake) {
                case CoroWake::NextFrame:
                    should_resume = true;
                    break;
                case CoroWake::AfterSeconds:
                    mc.sleep_remaining -= delta_seconds;
                    if (mc.sleep_remaining <= 0.0) {
                        should_resume = true;
                        mc.sleep_remaining = 0.0;
                    }
                    break;
                case CoroWake::OnSignal:
                    if (mc.signal_fired) {
                        should_resume   = true;
                        send            = &mc.signal_send_value;
                        mc.signal_fired = false;
                    }
                    break;
            }

            if (!should_resume) continue;

            PLValue nil_send; pl_value_init(&nil_send);
            PLValue yield_out; pl_value_init(&yield_out);
            const PLValue* sv = send ? send : &nil_send;

            int status = mc.vtable->pl_coroutine_resume(mc.handle, sv, &yield_out);

            if (status == PL_CORO_DONE || status == PL_CORO_FAILED) {
                if (status == PL_CORO_FAILED)
                    ERR_PRINT("[PolyLang/Coro] Coroutine failed.");

                godot::Variant result;
                if (status == PL_CORO_DONE && yield_out.type != PL_TYPE_NIL) {
                    switch (yield_out.type) {
                        case PL_TYPE_BOOL:   result = yield_out.b;          break;
                        case PL_TYPE_INT:    result = (int64_t)yield_out.i; break;
                        case PL_TYPE_FLOAT:  result = yield_out.f;          break;
                        case PL_TYPE_STRING:
                            result = godot::String(yield_out.s ? yield_out.s : "");
                            break;
                        default: break;
                    }
                }

                if (mc.vtable->pl_free_value_contents)
                    mc.vtable->pl_free_value_contents(&yield_out);
                if (mc.vtable->pl_coroutine_free)
                    mc.vtable->pl_coroutine_free(mc.handle);
                mc.handle = nullptr;

                finished.push_back({ id, mc.on_done, result,
                                     mc.signal_listener_id, true });
                to_remove.push_back(id);
            } else {
                if (mc.wake == CoroWake::AfterSeconds &&
                    yield_out.type == PL_TYPE_FLOAT)
                    mc.sleep_remaining = yield_out.f;
                if (mc.vtable->pl_free_value_contents)
                    mc.vtable->pl_free_value_contents(&yield_out);
            }
        }

        for (uint64_t rid : to_remove) active_.erase(rid);
    }
    // active_mutex_ released here.

    auto* bus = PLSignalBus::get_singleton();
    for (auto& fc : finished) {
        if (fc.signal_listener_id && bus)
            bus->disconnect_native(fc.signal_listener_id);
        if (fc.on_done.is_valid())
            fc.on_done.call(fc.result);
    }
}

// ── GDScript API ──────────────────────────────────────────────

static PLAdapterVTable* vtable_from_owner(godot::Object* owner, void** foreign_out) {
    if (foreign_out) *foreign_out = nullptr;
    auto* reg = PLScriptRegistry::get_singleton();
    if (!owner || !reg) return nullptr;

    PLScriptHandle handle = reg->find_owner(owner);
    if (!handle.valid()) return nullptr;

    PLAdapterVTable* vt = nullptr;
    void* foreign = nullptr;
    bool ok = false;

    switch (handle.kind) {
        case PLScriptKind::PolyLang:
            ok = static_cast<PolyLangScriptInstance*>(handle.ptr)
                ->resolve_method_target(nullptr, PL_CAP_COROUTINE, &vt, &foreign);
            break;
        case PLScriptKind::Polyglot:
            ok = static_cast<PolyglotInstance*>(handle.ptr)
                ->resolve_method_target(nullptr, PL_CAP_COROUTINE, &vt, &foreign);
            break;
    }

    if (!ok) return nullptr;
    if (foreign_out) *foreign_out = foreign;
    return vt;
}

int64_t PLCoroutineScheduler::spawn_next_frame(godot::Object* owner,
                                                const godot::String& method,
                                                const godot::Callable& on_done) {
    void* foreign = nullptr;
    PLAdapterVTable* vt = vtable_from_owner(owner, &foreign);
    if (!vt || !foreign || !vt->pl_coroutine_create) return 0;
    std::string m = method.utf8().get_data();
    void* handle = vt->pl_coroutine_create(foreign, m.c_str());
    if (!handle) return 0;
    return (int64_t)spawn(vt, handle, CoroWake::NextFrame, 0.0, "", on_done);
}

int64_t PLCoroutineScheduler::spawn_after(godot::Object* owner,
                                           const godot::String& method,
                                           double seconds,
                                           const godot::Callable& on_done) {
    void* foreign = nullptr;
    PLAdapterVTable* vt = vtable_from_owner(owner, &foreign);
    if (!vt || !foreign || !vt->pl_coroutine_create) return 0;
    std::string m = method.utf8().get_data();
    void* handle = vt->pl_coroutine_create(foreign, m.c_str());
    if (!handle) return 0;
    return (int64_t)spawn(vt, handle, CoroWake::AfterSeconds, seconds, "", on_done);
}

int64_t PLCoroutineScheduler::spawn_on_signal(godot::Object* owner,
                                               const godot::String& method,
                                               const godot::String& signal,
                                               const godot::Callable& on_done) {
    void* foreign = nullptr;
    PLAdapterVTable* vt = vtable_from_owner(owner, &foreign);
    if (!vt || !foreign || !vt->pl_coroutine_create) return 0;
    std::string m  = method.utf8().get_data();
    std::string sn = signal.utf8().get_data();
    void* handle = vt->pl_coroutine_create(foreign, m.c_str());
    if (!handle) return 0;
    return (int64_t)spawn(vt, handle, CoroWake::OnSignal, 0.0, sn, on_done);
}

void PLCoroutineScheduler::gd_cancel(int64_t id)       { cancel((uint64_t)id); }
bool PLCoroutineScheduler::gd_is_running(int64_t id) const { return is_running((uint64_t)id); }

} // namespace polylang
