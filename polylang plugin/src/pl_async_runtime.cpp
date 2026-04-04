// =============================================================
// pl_async_runtime.cpp  —  PolyLang v6.5
//
// FIX C-1: Replaced heap-pointer TOCTOU lid_ptr pattern.
//   BEFORE: deliver_result allocated uint64_t* lid_ptr on the heap
//           and registered a self-disconnecting native listener.
//           Race: lambda could fire (reading *lid_ptr==0) before
//           connect_native returned; lid_ptr leaked on cancel.
//   AFTER:  Poll thread calls push_done_callback() which appends
//           to main_thread_callbacks_ (mutex-protected vector).
//           tick_main_thread() drains it on the main thread each frame.
//           No heap allocation, no race, no listener registration.
// =============================================================
#include "pl_async_runtime.hpp"
#include "pl_bridge.hpp"
#include "pl_polyglot_script.hpp"
#include "polylang_script_instance.hpp"
#include "pl_signal_bus.hpp"
#include "variant_bridge.hpp"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <sstream>

namespace polylang {

namespace {

static bool resolve_async_target(godot::Object* owner,
                                 const char* method_name,
                                 PLAdapterVTable** out_vtable,
                                 void** out_foreign) {
    if (out_vtable)  *out_vtable = nullptr;
    if (out_foreign) *out_foreign = nullptr;
    auto* reg = PLScriptRegistry::get_singleton();
    if (!owner || !reg) return false;

    PLScriptHandle handle = reg->find_owner(owner);
    if (!handle.valid()) return false;

    switch (handle.kind) {
        case PLScriptKind::PolyLang:
            return static_cast<PolyLangScriptInstance*>(handle.ptr)
                ->resolve_method_target(method_name, PL_CAP_ASYNC, out_vtable, out_foreign);
        case PLScriptKind::Polyglot:
            return static_cast<PolyglotInstance*>(handle.ptr)
                ->resolve_method_target(method_name, PL_CAP_ASYNC, out_vtable, out_foreign);
    }
    return false;
}

} // namespace

PLAsyncRuntime* PLAsyncRuntime::singleton_ = nullptr;
PLAsyncRuntime* PLAsyncRuntime::get_singleton() { return singleton_; }

void PLAsyncRuntime::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("submit_method", "script_owner", "method", "args", "on_done"),
        &PLAsyncRuntime::submit_method);
    godot::ClassDB::bind_method(
        godot::D_METHOD("cancel", "future_id"),
        &PLAsyncRuntime::gd_cancel);
}

// ── start / stop ──────────────────────────────────────────────

void PLAsyncRuntime::start() {
    if (running_.load()) return;
    running_.store(true);
    poll_thread_ = std::thread([this]{ poll_loop(); });
}

void PLAsyncRuntime::stop() {
    if (!running_.load()) return;
    running_.store(false);
    poll_cv_.notify_all();
    if (poll_thread_.joinable()) poll_thread_.join();

    // Free all remaining futures.
    std::lock_guard<std::mutex> lk(futures_mutex_);
    for (auto& [id, f] : futures_) {
        if (f.vtable && f.vtable->pl_async_free && f.handle)
            f.vtable->pl_async_free(f.handle);
    }
    futures_.clear();

    // FIX C-1: Drain pending callbacks so on_done callables are not orphaned.
    // Futures were cancelled above; drop callbacks without calling on_done
    // (incomplete async tasks should not fire partial results on shutdown).
    std::lock_guard<std::mutex> cb_lk(cb_mutex_);
    main_thread_callbacks_.clear();
}

// ── submit ────────────────────────────────────────────────────

uint64_t PLAsyncRuntime::submit(PLAdapterVTable* vtable, void* handle,
                                 godot::Callable on_done) {
    if (!vtable || !handle) return 0;
    if (!vtable->pl_async_poll || !vtable->pl_async_free) return 0;

    uint64_t id = id_counter_.fetch_add(1, std::memory_order_relaxed);
    PendingFuture f;
    f.id      = id;
    f.vtable  = vtable;
    f.handle  = handle;
    f.on_done = on_done;

    {
        std::lock_guard<std::mutex> lk(futures_mutex_);
        futures_.emplace(id, std::move(f));
    }
    poll_cv_.notify_one();
    return id;
}

void PLAsyncRuntime::cancel(uint64_t id) {
    std::lock_guard<std::mutex> lk(futures_mutex_);
    auto it = futures_.find(id);
    if (it == futures_.end()) return;
    auto& f = it->second;
    if (f.vtable && f.vtable->pl_async_free && f.handle)
        f.vtable->pl_async_free(f.handle);
    futures_.erase(it);
}

// ── push_done_callback ────────────────────────────────────────
// FIX C-1: Poll thread calls this instead of registering a native listener.
// This is the ONLY cross-thread communication point — trivially safe.

void PLAsyncRuntime::push_done_callback(const godot::Callable& on_done,
                                         godot::Variant result,
                                         uint64_t future_id) {
    std::lock_guard<std::mutex> lk(cb_mutex_);
    main_thread_callbacks_.push_back({ on_done, std::move(result), future_id });
}

// ── tick_main_thread ──────────────────────────────────────────
// Called from PolyLangLanguage::_frame() on the main thread.
// Drains the callback queue — Callable::call() is safe here.

void PLAsyncRuntime::tick_main_thread() {
    std::vector<DoneCallback> local;
    {
        std::lock_guard<std::mutex> lk(cb_mutex_);
        local.swap(main_thread_callbacks_);
    }
    for (auto& dc : local) {
        // Fire the "async_done" signal on the bus with [future_id, result].
        if (auto* bus = PLSignalBus::get_singleton()) {
            godot::Array sig_args;
            sig_args.push_back((int64_t)dc.future_id);
            sig_args.push_back(dc.result);
            bus->emit_native("async_done", sig_args);
        }
        // Direct callable — safe on main thread.
        if (dc.on_done.is_valid())
            dc.on_done.call(dc.result);
    }
}

// ── poll_loop ─────────────────────────────────────────────────

void PLAsyncRuntime::poll_loop() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lk(poll_cv_mutex_);
            poll_cv_.wait_for(lk, std::chrono::milliseconds(10),
                [this]{ return !running_.load(); });
        }
        if (!running_.load()) break;

        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lk(futures_mutex_);
            ids.reserve(futures_.size());
            for (auto& [id, _] : futures_) ids.push_back(id);
        }

        std::vector<uint64_t> done_ids;

        for (uint64_t id : ids) {
            PLAdapterVTable* vt  = nullptr;
            void*            hdl = nullptr;
            godot::Callable  on_done;
            {
                std::lock_guard<std::mutex> lk(futures_mutex_);
                auto it = futures_.find(id);
                if (it == futures_.end()) continue;
                vt      = it->second.vtable;
                hdl     = it->second.handle;
                on_done = it->second.on_done;
            }

            PLValue result; pl_value_init(&result);
            int rc = vt->pl_async_poll(hdl, &result);

            if (rc == PL_OK) {
                // FIX C-1: Convert result here on the poll thread (safe —
                // VariantBridge only reads the PLValue, no Godot API calls).
                godot::Variant gv = VariantBridge::from_pl_value(result);
                if (vt->pl_free_value_contents) vt->pl_free_value_contents(&result);
                // Push to main-thread queue — no Godot API called here.
                push_done_callback(on_done, std::move(gv), id);
                done_ids.push_back(id);
            } else if (rc != PL_ERR_ASYNC_PENDING) {
                ERR_PRINT(("[PolyLang/Async] Future " + std::to_string(id)
                           + " failed with rc=" + std::to_string(rc)).c_str());
                if (vt->pl_free_value_contents) vt->pl_free_value_contents(&result);
                // Push empty result so on_done is still notified.
                push_done_callback(on_done, godot::Variant(), id);
                done_ids.push_back(id);
            }
        }

        // Cleanup resolved futures.
        std::lock_guard<std::mutex> lk(futures_mutex_);
        for (uint64_t id : done_ids) {
            auto it = futures_.find(id);
            if (it == futures_.end()) continue;
            if (it->second.vtable && it->second.vtable->pl_async_free && it->second.handle)
                it->second.vtable->pl_async_free(it->second.handle);
            futures_.erase(it);
        }
    }
}

// ── GDScript API ──────────────────────────────────────────────

int64_t PLAsyncRuntime::submit_method(godot::Object* owner,
                                       const godot::String& method,
                                       const godot::Array&  args,
                                       const godot::Callable& on_done) {
    std::string m = method.utf8().get_data();
    if (m.empty()) return 0;

    PLAdapterVTable* vt = nullptr;
    void* foreign = nullptr;
    if (!resolve_async_target(owner, m.c_str(), &vt, &foreign) ||
        !vt || !foreign || !vt->pl_async_begin) {
        ERR_PRINT("[PolyLang/Async] submit_method: no async-capable PolyLang instance for owner.");
        return 0;
    }

    int32_t argc = static_cast<int32_t>(args.size());
    std::vector<PLValue> pl_args(argc);
    for (int32_t i = 0; i < argc; ++i)
        VariantBridge::to_pl_value(args[i], pl_args[i]);

    void* future = vt->pl_async_begin(foreign, m.c_str(), pl_args.data(), argc);

    for (auto& v : pl_args) {
        if (vt->pl_free_value_contents) vt->pl_free_value_contents(&v);
        else VariantBridge::free_pl_value(v);
    }

    if (!future) return 0;
    return (int64_t)submit(vt, future, on_done);
}

void PLAsyncRuntime::gd_cancel(int64_t id) { cancel((uint64_t)id); }

} // namespace polylang
