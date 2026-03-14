// =============================================================
// pl_profiler.hpp  —  PolyLang v6.5
// =============================================================
// PLProfiler bridges the vtable pl_profiler_begin / pl_profiler_end
// slots to Godot's built-in ScriptLanguage profiling infrastructure.
//
// DESIGN:
//   Each pl_profiler_begin(label) opens a named timing scope.
//   Each pl_profiler_end(label) closes it and records elapsed time.
//   Results are aggregated per-label and flushed to Godot's profiler
//   each frame via PolyLangLanguage::_frame().
//
//   The same PLProfiler also intercepts calls coming from adapter
//   inject-time (e.g. Odin's OdinRuntimeServices.profiler_begin).
//
// THREAD SAFETY:
//   begin/end may be called from any thread (worker compiles, async).
//   Thread-local scope stacks are used; aggregation is mutex-protected.
//
// SANDBOX:
//   Profiler records all tiers. No sandbox restriction on profiling.
//
// GODOT INTEGRATION:
//   Calls LanguageServer profiling hooks when available.
//   Accumulated data is exposed via:
//     PLProfiler.get_scope_stats() → Dictionary { label: { calls, total_usec } }
//     PLProfiler.reset()
// =============================================================
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stack>
#include <string>
#include <unordered_map>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace polylang {

// ── Per-scope stats ───────────────────────────────────────────
struct ScopeStats {
    uint64_t calls{0};
    uint64_t total_usec{0};
    uint64_t min_usec{UINT64_MAX};
    uint64_t max_usec{0};
};

class PLProfiler : public godot::Object {
    GDCLASS(PLProfiler, godot::Object)

public:
    static PLProfiler* get_singleton();

    // ── Adapter vtable functions ──────────────────────────────
    // These are the function pointers injected into adapter runtime services.
    static void pl_profiler_begin_impl(const char* label);
    static void pl_profiler_end_impl(const char* label);

    // ── Frame flush ───────────────────────────────────────────
    // Called from PolyLangLanguage::_frame() — sends accumulated data
    // to Godot's profiler API if the editor is running.
    void flush_to_godot();

    // ── GDScript API ──────────────────────────────────────────
    godot::Dictionary get_scope_stats() const;
    void              reset();
    bool              is_enabled() const;
    void              set_enabled(bool v);

protected:
    static void _bind_methods();

private:
    PLProfiler() = default;

    void record(const std::string& label, uint64_t elapsed_usec);

    // Per-thread scope stack: label + start timestamp.
    struct ScopeEntry {
        std::string label;
        std::chrono::steady_clock::time_point start;
    };
    static thread_local std::stack<ScopeEntry> scope_stack_;

    mutable std::mutex                            stats_mutex_;
    std::unordered_map<std::string, ScopeStats>   stats_;
    std::atomic<bool>                             enabled_{true};

    static PLProfiler* singleton_;
};

} // namespace polylang

// v6.6 addition — expose dropped label counter
// (append to class public section)
