// =============================================================
// pl_signal_bus.hpp  —  PolyLang v6.6
// v6.6: dropped_emissions_ counter exposed via get_dropped_emissions().
// =============================================================
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace polylang {

struct NativeListener {
    std::string                              signal_name;
    std::function<void(const godot::Array&)> callback;
    uint64_t                                 id;
};

struct QueuedEmission {
    std::string  signal_name;
    godot::Array args;
};

class PLSignalBus : public godot::Object {
    GDCLASS(PLSignalBus, godot::Object)

public:
    static PLSignalBus* get_singleton();

    void    emit_signal_pl(const godot::String& signal_name, const godot::Array& args);
    void    connect_signal(const godot::String& signal_name, const godot::Callable& callable);
    void    disconnect_signal(const godot::String& signal_name, const godot::Callable& callable);
    bool    has_listeners(const godot::String& signal_name) const;
    int64_t get_dropped_emissions() const;   // v6.6: observable DoS counter
    void    flush_queued();

    uint64_t connect_native(const std::string& signal_name,
                             std::function<void(const godot::Array&)> callback);
    void     disconnect_native(uint64_t listener_id);
    void     emit_native(const std::string& signal_name, const godot::Array& args);

protected:
    static void _bind_methods();

private:
    PLSignalBus() = default;
    void dispatch_emission(const std::string& signal_name, const godot::Array& args);

    mutable std::shared_mutex                                    callable_mutex_;
    std::unordered_map<std::string, std::vector<godot::Callable>> callable_map_;

    mutable std::shared_mutex               native_mutex_;
    std::vector<NativeListener>             native_listeners_;
    uint64_t                                next_native_id_{1};

    std::mutex                              queue_mutex_;
    std::deque<QueuedEmission>              emission_queue_;

    // v6.6: Counts emissions dropped due to queue-full guard (VLN-01).
    std::atomic<uint64_t>                   dropped_emissions_{0};

    static PLSignalBus* singleton_;
};

} // namespace polylang
