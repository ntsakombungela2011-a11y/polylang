#pragma once
// =============================================================
// hot_reload_scheduler.hpp  —  v5 + sandbox (SECTION 7)
// =============================================================
#include <deque>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>

#include "../include/pl_adapter_vtable.h"

namespace polylang {

class PolyLangScript;
class PolyLangScriptInstance;

struct ReloadJob {
    PolyLangScript*         script{nullptr};
    void*                   new_compiled_handle{nullptr};
    void*                   old_compiled_handle{nullptr};
    void*                   rollback_handle{nullptr};
    std::string             res_path;
    std::string             cache_key;        // SECTION 7: may differ from res_path for sandboxed
    const PLAdapterVTable*  vtable{nullptr};
    int                     total_instances{0};
    std::atomic<int>        swapped_instances{0};
    bool                    any_failed{false};
    bool                    sandboxed{false};  // SECTION 7: was compiled via pl_compile_sandboxed
    uint32_t                allowed_caps{0};   // SECTION 7: sandbox caps used

    std::vector<PolyLangScriptInstance*> snapshot;
};

struct StagedInstance {
    PolyLangScriptInstance* inst{nullptr};
    ReloadJob*              job{nullptr};
};

class HotReloadScheduler {
public:
    static HotReloadScheduler* get_singleton() { return singleton_; }
    static void create()  { singleton_ = new HotReloadScheduler(); }
    static void destroy() { delete singleton_; singleton_ = nullptr; }

    // enqueue_reload() dispatches async compilation on a worker thread.
    // Sandbox policy is read from RuntimeManager inside compile_and_enqueue()
    // (after calling maybe_register_sidecar), so callers need not pass it.
    void enqueue_reload(PolyLangScript* script, const std::string& res_path);

    void flush_frame(double budget_seconds = 0.002);
    void set_budget(double s) { budget_seconds_ = s; }

    bool is_busy() const;

private:
    void compile_and_enqueue(PolyLangScript* script, std::string res_path);
    void finalize_job(ReloadJob* job);
    void rollback_job(ReloadJob* job);

    mutable std::mutex      pending_mutex_;
    std::vector<ReloadJob*> pending_jobs_;

    std::deque<StagedInstance> stage_queue_;
    std::vector<ReloadJob*>    active_jobs_;

    double budget_seconds_{0.002};

    static HotReloadScheduler* singleton_;
};

} // namespace polylang
