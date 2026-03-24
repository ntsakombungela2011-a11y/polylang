// =============================================================
// pl_signal_bus.cpp  —  PolyLang v6.6
//
// ZERO-TRUST AUDIT ROUND 2 FIXES:
//
// FIX VLN-01 [CRITICAL]: Unbounded emission_queue_ growth (DoS/OOM).
//   BEFORE: No cap. Adversarial adapter floods queue → OOM.
//   AFTER:  PL_SIGNAL_QUEUE_HARD_CAP (4096) enforced; excess dropped
//           with ERR_PRINT + atomic dropped_emissions_ counter.
//
// FIX VLN-02 [HIGH]: connect_signal() allows duplicate Callable registration.
//   BEFORE: push_back() unconditionally → one emission fires N callbacks
//           if same callable was connected N times (amplification attack).
//   AFTER:  Linear scan rejects duplicates before inserting.
// =============================================================
#include "pl_signal_bus.hpp"
#include <algorithm>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/error_macros.hpp>

namespace polylang {

static constexpr size_t PL_SIGNAL_QUEUE_HARD_CAP = 4096;

PLSignalBus* PLSignalBus::singleton_ = nullptr;
PLSignalBus* PLSignalBus::get_singleton() { return singleton_; }

void PLSignalBus::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("emit_signal_pl","signal_name","args"), &PLSignalBus::emit_signal_pl);
    godot::ClassDB::bind_method(
        godot::D_METHOD("connect_signal","signal_name","callable"), &PLSignalBus::connect_signal);
    godot::ClassDB::bind_method(
        godot::D_METHOD("disconnect_signal","signal_name","callable"), &PLSignalBus::disconnect_signal);
    godot::ClassDB::bind_method(
        godot::D_METHOD("has_listeners","signal_name"), &PLSignalBus::has_listeners);
    godot::ClassDB::bind_method(
        godot::D_METHOD("get_dropped_emissions"), &PLSignalBus::get_dropped_emissions);
}

void PLSignalBus::emit_signal_pl(const godot::String& signal_name,
                                  const godot::Array& args) {
    std::string name = signal_name.utf8().get_data();
    std::lock_guard<std::mutex> lk(queue_mutex_);
    // FIX VLN-01: hard cap — drop emissions when queue is full.
    if (emission_queue_.size() >= PL_SIGNAL_QUEUE_HARD_CAP) {
        dropped_emissions_.fetch_add(1, std::memory_order_relaxed);
        ERR_PRINT("[PolyLang/SignalBus] emission_queue_ full — dropping emission (DoS guard)");
        return;
    }
    emission_queue_.push_back({ name, args });
}

void PLSignalBus::connect_signal(const godot::String& signal_name,
                                  const godot::Callable& callable) {
    std::string name = signal_name.utf8().get_data();
    std::unique_lock lk(callable_mutex_);
    auto& vec = callable_map_[name];
    // FIX VLN-02: reject duplicate callable registration.
    for (const auto& existing : vec) {
        if (existing == callable) return;
    }
    vec.push_back(callable);
}

void PLSignalBus::disconnect_signal(const godot::String& signal_name,
                                     const godot::Callable& callable) {
    std::string name = signal_name.utf8().get_data();
    std::unique_lock lk(callable_mutex_);
    auto it = callable_map_.find(name);
    if (it == callable_map_.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
        [&](const godot::Callable& c){ return c == callable; }), vec.end());
    if (vec.empty()) callable_map_.erase(it);
}

bool PLSignalBus::has_listeners(const godot::String& signal_name) const {
    std::string name = signal_name.utf8().get_data();
    { std::shared_lock lk(callable_mutex_);
      auto it = callable_map_.find(name);
      if (it != callable_map_.end() && !it->second.empty()) return true; }
    { std::shared_lock lk(native_mutex_);
      for (const auto& nl : native_listeners_)
          if (nl.signal_name == name) return true; }
    return false;
}

int64_t PLSignalBus::get_dropped_emissions() const {
    return (int64_t)dropped_emissions_.load(std::memory_order_relaxed);
}

uint64_t PLSignalBus::connect_native(const std::string& signal_name,
    std::function<void(const godot::Array&)> callback) {
    std::unique_lock lk(native_mutex_);
    uint64_t id = next_native_id_++;
    native_listeners_.push_back({ signal_name, std::move(callback), id });
    return id;
}

void PLSignalBus::disconnect_native(uint64_t listener_id) {
    std::unique_lock lk(native_mutex_);
    native_listeners_.erase(std::remove_if(native_listeners_.begin(), native_listeners_.end(),
        [listener_id](const NativeListener& nl){ return nl.id == listener_id; }),
        native_listeners_.end());
}

void PLSignalBus::emit_native(const std::string& signal_name, const godot::Array& args) {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    // FIX VLN-01: cap also applied on the native emit path.
    if (emission_queue_.size() >= PL_SIGNAL_QUEUE_HARD_CAP) {
        dropped_emissions_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    emission_queue_.push_back({ signal_name, args });
}

void PLSignalBus::flush_queued() {
    std::deque<QueuedEmission> local;
    { std::lock_guard<std::mutex> lk(queue_mutex_); local.swap(emission_queue_); }
    for (const auto& e : local) dispatch_emission(e.signal_name, e.args);
}

void PLSignalBus::dispatch_emission(const std::string& signal_name,
                                     const godot::Array& args) {
    std::vector<godot::Callable> callables;
    { std::shared_lock lk(callable_mutex_);
      auto it = callable_map_.find(signal_name);
      if (it != callable_map_.end()) callables = it->second; }
    for (const auto& c : callables) { if (c.is_valid()) c.callv(args); }

    std::vector<std::function<void(const godot::Array&)>> natives;
    { std::shared_lock lk(native_mutex_);
      for (const auto& nl : native_listeners_)
          if (nl.signal_name == signal_name) natives.push_back(nl.callback); }
    for (const auto& cb : natives) cb(args);
}

} // namespace polylang
