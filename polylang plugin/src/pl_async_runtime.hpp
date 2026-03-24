// =============================================================
// pl_async_runtime.hpp  —  PolyLang v6.5
// FIX C-1: Replaced heap-pointer TOCTOU lid_ptr pattern with a
//           lock-protected main_thread_callbacks_ queue.
//           Poll thread only pushes to queue — never calls Godot API.
//           Main thread drains in tick_main_thread() from _frame().
// =============================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <condition_variable>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>

#include "../include/pl_adapter_vtable.h"

namespace polylang {

struct PendingFuture {
    uint64_t         id{0};
    PLAdapterVTable* vtable{nullptr};    // non-owning
    void*            handle{nullptr};    // owned; freed via pl_async_free
    godot::Callable  on_done;
};

// Delivered result waiting for main-thread dispatch.
struct DoneCallback {
    godot::Callable  on_done;
    godot::Variant   result;
    uint64_t         future_id{0};
};

class PLAsyncRuntime : public godot::Object {
    GDCLASS(PLAsyncRuntime, godot::Object)
public:
    static PLAsyncRuntime* get_singleton();

    void start();
    void stop();

    // Low-level: submit an already-created future handle.
    uint64_t submit(PLAdapterVTable* vtable, void* handle,
                    godot::Callable on_done);

    void cancel(uint64_t id);

    // Called from PolyLangLanguage::_frame() on the main thread.
    // Drains main_thread_callbacks_ and fires on_done callables safely.
    void tick_main_thread();

    // GDScript API
    int64_t submit_method(godot::Object* owner,
                          const godot::String& method,
                          const godot::Array& args,
                          const godot::Callable& on_done);
    void gd_cancel(int64_t id);

    static PLAsyncRuntime* singleton_;

protected:
    static void _bind_methods();

private:
    void poll_loop();

    // FIX C-1: poll thread pushes here; main thread drains.
    void push_done_callback(const godot::Callable& on_done,
                            godot::Variant result,
                            uint64_t future_id);

    std::atomic<uint64_t>                           id_counter_{1};
    mutable std::mutex                              futures_mutex_;
    std::unordered_map<uint64_t, PendingFuture>     futures_;

    std::thread                                     poll_thread_;
    std::atomic<bool>                               running_{false};
    std::condition_variable                         poll_cv_;
    std::mutex                                      poll_cv_mutex_;

    // FIX C-1: thread-safe callback queue.
    mutable std::mutex               cb_mutex_;
    std::vector<DoneCallback>        main_thread_callbacks_;
};

} // namespace polylang
