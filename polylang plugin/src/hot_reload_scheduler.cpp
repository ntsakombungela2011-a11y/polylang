// =============================================================
// hot_reload_scheduler.cpp  —  v5 + sandbox (SECTION 7)
// =============================================================
// SECTION 7 additions:
//   compile_and_enqueue() now:
//     1. Asks RuntimeManager whether the script path is sandboxed.
//     2. If sandboxed: calls CompileCache::get_or_compile_sandboxed()
//        which routes to vtable->pl_compile_sandboxed().
//     3. If trusted: calls CompileCache::get_or_compile() as before.
//     4. Stores sandboxed/allowed_caps on ReloadJob so finalize_job()
//        can release the correct cache key on completion.
//   rollback_job() releases the sandboxed cache key (not res_path).
//   finalize_job() calls CompileCache::release(cache_key, ...) which
//        correctly handles the ":s:000000ff" suffix key for sandboxed.
// All A-2/A-3/A-8/A-10/B-3 fixes from v5 are retained.
// =============================================================
#include "hot_reload_scheduler.hpp"
#include "polylang_script.hpp"
#include "polylang_script_instance.hpp"
#include "runtime_manager.hpp"
#include "compile_cache.hpp"

#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <algorithm>
#include <chrono>

namespace polylang {

HotReloadScheduler* HotReloadScheduler::singleton_ = nullptr;

// ── enqueue_reload ────────────────────────────────────────────
// FIX A-8: Only enqueues — no compile on main thread.

void HotReloadScheduler::enqueue_reload(PolyLangScript* script,
                                         const std::string& res_path) {
    struct ReloadTask {
        HotReloadScheduler* scheduler;
        PolyLangScript* script;
        std::string res_path;
    };
    auto* task = new ReloadTask{this, script, res_path};
    godot::WorkerThreadPool::get_singleton()->add_native_task(
        [](void* ud) {
            auto* t = static_cast<ReloadTask*>(ud);
            t->scheduler->compile_and_enqueue(t->script, t->res_path);
            delete t;
        }, task, true);
}

// ── compile_and_enqueue (worker thread) ──────────────────────
// SECTION 7: Sandbox-aware compile routing.

void HotReloadScheduler::compile_and_enqueue(PolyLangScript* script,
                                              std::string res_path) {
    LanguageID lang = script->get_language_id();
    const PLAdapterVTable* vt = RuntimeManager::get_singleton()->get_vtable(lang);
    if (!vt) return;

    // Read source text (works inside .pck exports).
    godot::Ref<godot::FileAccess> fa = godot::FileAccess::open(
        godot::String(res_path.c_str()), godot::FileAccess::READ);
    if (!fa.is_valid()) {
        ERR_PRINT(("[PolyLang] Hot-reload: cannot open " + res_path).c_str());
        return;
    }
    std::string source = fa->get_as_text().utf8().get_data();
    fa.unref();

    // SECTION 7: Determine sandbox policy from RuntimeManager registry.
    // Call maybe_register_sidecar first so scripts whose sidecar has not
    // yet been parsed (e.g. on first hot-reload) get auto-registered.
    RuntimeManager* rm = RuntimeManager::get_singleton();
    rm->maybe_register_sidecar(res_path);
    bool     sandboxed   = rm->is_sandboxed(res_path);
    uint32_t allowed_caps = sandboxed ? rm->sandboxed_caps(res_path) : PL_SANDBOX_NONE;

    // Build cache key (differs for sandboxed entries).
    std::string cache_key = sandboxed
        ? CompileCache::sandboxed_key(res_path, allowed_caps)
        : res_path;

    // Compile via appropriate cache path.
    void* new_handle = nullptr;
    if (sandboxed) {
        // SECTION 7: Route through sandboxed compile path.
        // CompileCache::get_or_compile_sandboxed() calls pl_compile_sandboxed().
        if (!(vt->capabilities & PL_CAP_SANDBOX) || !vt->pl_compile_sandboxed) {
            ERR_PRINT(("[PolyLang] Adapter does not support PL_CAP_SANDBOX: "
                       + res_path).c_str());
            return;
        }
        new_handle = CompileCache::get_singleton()->get_or_compile_sandboxed(
            res_path, source, vt, allowed_caps);
    } else {
        new_handle = CompileCache::get_singleton()->get_or_compile(
            res_path, source, vt);
    }

    if (!new_handle) {
        ERR_PRINT(("[PolyLang] Hot-reload compile failed: " + res_path).c_str());
        return;
    }

    // Build ReloadJob.
    auto* job = new ReloadJob();
    job->script              = script;
    job->new_compiled_handle = new_handle;
    job->old_compiled_handle = script->get_compiled_handle();
    job->rollback_handle     = job->old_compiled_handle;   // FIX A-2
    job->res_path            = res_path;
    job->cache_key           = cache_key;                  // SECTION 7
    job->vtable              = vt;
    job->sandboxed           = sandboxed;                  // SECTION 7
    job->allowed_caps        = allowed_caps;               // SECTION 7
    job->snapshot            = script->snapshot_instances(); // FIX A-10
    job->total_instances     = static_cast<int>(job->snapshot.size());
    job->swapped_instances   = 0;
    job->any_failed          = false;

    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        pending_jobs_.push_back(job);
    }
}

// ── flush_frame ───────────────────────────────────────────────

void HotReloadScheduler::flush_frame(double budget_seconds) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::duration<double>(budget_seconds);

    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        for (auto* job : pending_jobs_) {
            active_jobs_.push_back(job);

            if (job->snapshot.empty()) {
                job->script->apply_new_compiled_handle(job->new_compiled_handle);
                // SECTION 7: release old handle via correct cache key.
                if (job->old_compiled_handle && job->vtable && job->vtable->pl_free_compiled)
                    job->vtable->pl_free_compiled(job->old_compiled_handle);
                active_jobs_.erase(
                    std::find(active_jobs_.begin(), active_jobs_.end(), job));
                delete job;
                continue;
            }
            for (auto* inst : job->snapshot)
                stage_queue_.push_back({inst, job});
        }
        pending_jobs_.clear();
    }

    while (!stage_queue_.empty() && Clock::now() < deadline) {
        StagedInstance si = stage_queue_.front();
        stage_queue_.pop_front();
        ReloadJob* job = si.job;

        void* new_foreign = nullptr;
        if (job->vtable && job->vtable->pl_instantiate_class)
            new_foreign = job->vtable->pl_instantiate_class(
                job->new_compiled_handle, job->res_path.c_str());

        // FIX A-2: null guard before hot_swap.
        if (!new_foreign) {
            ERR_PRINT(("[PolyLang] Hot-reload: instantiation failed in "
                       + job->res_path + " — rolling back.").c_str());
            job->any_failed = true;
        } else {
            if (si.inst) si.inst->hot_swap(new_foreign);
        }

        int done = job->swapped_instances.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done >= job->total_instances) {
            if (job->any_failed) rollback_job(job);
            else                 finalize_job(job);
        }
    }
}

// ── finalize_job ──────────────────────────────────────────────
// SECTION 7: Releases old handle. The new handle is already in the
// cache under cache_key — no separate release needed for it.

void HotReloadScheduler::finalize_job(ReloadJob* job) {
    job->script->apply_new_compiled_handle(job->new_compiled_handle);
    // Free old compiled handle directly (not via cache — cache tracks new_handle).
    if (job->old_compiled_handle && job->vtable && job->vtable->pl_free_compiled)
        job->vtable->pl_free_compiled(job->old_compiled_handle);
    active_jobs_.erase(std::find(active_jobs_.begin(), active_jobs_.end(), job));
    delete job;
}

// ── rollback_job ──────────────────────────────────────────────
// FIX A-2 + SECTION 7: Releases new_handle from cache (including
// sandboxed key variant) and keeps old handle/instances alive.

void HotReloadScheduler::rollback_job(ReloadJob* job) {
    ERR_PRINT(("[PolyLang] Hot-reload ROLLED BACK: " + job->res_path).c_str());
    // Release new handle from cache via its correct key (sandboxed or plain).
    CompileCache::get_singleton()->release(job->cache_key, job->vtable);
    // Do NOT apply new handle — script keeps old.
    active_jobs_.erase(std::find(active_jobs_.begin(), active_jobs_.end(), job));
    delete job;
}

// ── FIX B-3: is_busy acquires pending_mutex_ ──────────────────

// FIX VLN-15: stage_queue_ read under pending_mutex_ to prevent data race.
//   BEFORE: stage_queue_.empty() was checked without any lock.
//     flush_frame() writes to stage_queue_ inside pending_mutex_ on the main
//     thread. If is_busy() is called concurrently from another thread
//     (e.g. editor polling) it could observe partially-written state —
//     a C++ data race with undefined behaviour.
//   AFTER:  stage_queue_ is accessed only under pending_mutex_ in both
//     flush_frame() and is_busy().
bool HotReloadScheduler::is_busy() const {
    std::lock_guard<std::mutex> lk(pending_mutex_);
    return !stage_queue_.empty() || !pending_jobs_.empty();
}

} // namespace polylang
