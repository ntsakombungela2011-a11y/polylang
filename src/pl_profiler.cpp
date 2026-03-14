// =============================================================
// pl_profiler.cpp  —  PolyLang v6.6
//
// ZERO-TRUST AUDIT ROUND 2 FIX:
//
// FIX VLN-10 [MEDIUM]: stats_ map grows unbounded from unlimited
//   unique labels — slow memory DoS over time.
//   BEFORE: record() inserts any label string without limit.
//     An adversarial adapter calling pl_profiler_begin() with a
//     unique label per call (e.g. UUID-like strings) fills stats_
//     indefinitely.
//   AFTER:  PL_PROFILER_MAX_LABELS (1024) cap. record() silently
//     drops new labels once the cap is reached and increments a
//     dropped_labels_ counter visible via get_dropped_labels().
//     Labels that already exist in stats_ are always updated normally.
// =============================================================
#include "pl_profiler.hpp"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace polylang {

static constexpr size_t PL_PROFILER_MAX_LABELS = 1024;

PLProfiler* PLProfiler::singleton_ = nullptr;
thread_local std::stack<PLProfiler::ScopeEntry> PLProfiler::scope_stack_;

PLProfiler* PLProfiler::get_singleton() { return singleton_; }

void PLProfiler::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("get_scope_stats"), &PLProfiler::get_scope_stats);
    godot::ClassDB::bind_method(
        godot::D_METHOD("reset"), &PLProfiler::reset);
    godot::ClassDB::bind_method(
        godot::D_METHOD("is_enabled"), &PLProfiler::is_enabled);
    godot::ClassDB::bind_method(
        godot::D_METHOD("set_enabled", "enabled"), &PLProfiler::set_enabled);
    godot::ClassDB::bind_method(
        godot::D_METHOD("get_dropped_labels"), &PLProfiler::get_dropped_labels);
}

/*static*/ void PLProfiler::pl_profiler_begin_impl(const char* label) {
    auto* self = PLProfiler::get_singleton();
    if (!self || !self->enabled_.load(std::memory_order_relaxed)) return;
    if (!label) label = "(unnamed)";
    ScopeEntry e;
    e.label = label;
    e.start = std::chrono::steady_clock::now();
    scope_stack_.push(std::move(e));
}

/*static*/ void PLProfiler::pl_profiler_end_impl(const char* label) {
    auto* self = PLProfiler::get_singleton();
    if (!self || !self->enabled_.load(std::memory_order_relaxed)) return;
    if (scope_stack_.empty()) {
        ERR_PRINT("[PolyLang/Profiler] pl_profiler_end called with empty stack");
        return;
    }
    auto now = std::chrono::steady_clock::now();
    const ScopeEntry& top = scope_stack_.top();
    if (label && top.label != label) {
        ERR_PRINT(("[PolyLang/Profiler] end label mismatch: expected '"
                   + top.label + "' got '" + std::string(label) + "'").c_str());
    }
    uint64_t elapsed = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        now - top.start).count();
    std::string lbl = top.label;
    scope_stack_.pop();
    self->record(lbl, elapsed);
}

void PLProfiler::record(const std::string& label, uint64_t usec) {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    auto it = stats_.find(label);
    if (it == stats_.end()) {
        // FIX VLN-10: enforce label count cap.
        if (stats_.size() >= PL_PROFILER_MAX_LABELS) {
            dropped_labels_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        it = stats_.emplace(label, ScopeStats{}).first;
    }
    auto& s    = it->second;
    s.calls   += 1;
    s.total_usec += usec;
    if (usec < s.min_usec) s.min_usec = usec;
    if (usec > s.max_usec) s.max_usec = usec;
}

void PLProfiler::flush_to_godot() {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    // Godot 4 GDExtension does not yet expose a direct profiler push API.
    // Data is polled by the editor plugin via get_scope_stats().
}

godot::Dictionary PLProfiler::get_scope_stats() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    godot::Dictionary out;
    for (const auto& [label, s] : stats_) {
        godot::Dictionary entry;
        entry["calls"]       = (int64_t)s.calls;
        entry["total_usec"]  = (int64_t)s.total_usec;
        entry["avg_usec"]    = s.calls > 0 ? (int64_t)(s.total_usec / s.calls) : 0;
        entry["min_usec"]    = s.min_usec == UINT64_MAX ? 0 : (int64_t)s.min_usec;
        entry["max_usec"]    = (int64_t)s.max_usec;
        out[godot::String(label.c_str())] = entry;
    }
    return out;
}

void PLProfiler::reset() {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    stats_.clear();
    dropped_labels_.store(0, std::memory_order_relaxed);
}

bool PLProfiler::is_enabled() const {
    return enabled_.load(std::memory_order_relaxed);
}
void PLProfiler::set_enabled(bool v) {
    enabled_.store(v, std::memory_order_relaxed);
}
int64_t PLProfiler::get_dropped_labels() const {
    return (int64_t)dropped_labels_.load(std::memory_order_relaxed);
}

} // namespace polylang
