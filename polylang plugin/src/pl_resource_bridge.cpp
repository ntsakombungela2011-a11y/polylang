// =============================================================
// pl_resource_bridge.cpp  —  PolyLang v6.6
//
// RETAINED FIXES from v6.5: C-5, C-6, M-1.
//
// ZERO-TRUST AUDIT ROUND 2 FIX:
//
// FIX VLN-08 [HIGH]: Worker threads block on condition_variable with
//   NO TIMEOUT — deadlock if main thread never calls flush().
//   BEFORE: done_cv.wait(lk, [&done_flag]{ return done_flag; });
//     If the engine shuts down, flush() is never called again, and
//     any worker thread blocked in pl_resource_fetch_impl() hangs
//     forever, preventing clean shutdown.
//   AFTER:  wait_for() with PL_RESOURCE_DEFERRED_TIMEOUT_MS (2000 ms).
//     If the timeout expires, the worker returns PL_ERR_GENERIC and
//     logs a warning. Also: PLResourceBridge::shutdown() sets
//     shutdown_flag_ and notifies all waiters so they can exit cleanly.
// =============================================================
#include "pl_resource_bridge.hpp"

#include <cstring>
#include <thread>
#include <chrono>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace polylang {

static constexpr int PL_RESOURCE_DEFERRED_TIMEOUT_MS = 2000;

PLResourceBridge* PLResourceBridge::singleton_ = nullptr;
PLResourceBridge* PLResourceBridge::get_singleton() { return singleton_; }

PLResourceBridge::PLResourceBridge()
    : main_thread_id_(std::this_thread::get_id())
{}

bool PLResourceBridge::is_main_thread() const {
    return std::this_thread::get_id() == main_thread_id_;
}

void PLResourceBridge::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("load_resource", "path"),
        &PLResourceBridge::load_resource);
    godot::ClassDB::bind_method(
        godot::D_METHOD("is_path_allowed", "path", "sandbox_tier"),
        &PLResourceBridge::is_path_allowed);
}

bool PLResourceBridge::check_access(const std::string& path, SandboxTier tier) const {
    switch (tier) {
        case SandboxTier::Trusted:     return true;
        case SandboxTier::Isolated:    return path.rfind("res://mods/", 0) == 0;
        case SandboxTier::Quarantined: return false;
    }
    return false;
}

/*static*/ int PLResourceBridge::pl_resource_fetch_impl(
        const char* res_path, PLValue* out, SandboxTier tier) {
    pl_value_init(out);

    auto* self = PLResourceBridge::get_singleton();
    if (!self || !res_path) return PL_ERR_GENERIC;

    // FIX VLN-08: If shutdown has been requested, refuse new deferred loads.
    if (self->shutdown_flag_.load(std::memory_order_acquire)) return PL_ERR_GENERIC;

    std::string path(res_path);
    if (!self->check_access(path, tier)) {
        ERR_PRINT(("[PolyLang/Resource] Access denied for tier to: " + path).c_str());
        return PL_ERR_SANDBOX;
    }

    // Cache lookup with stale-entry eviction.
    {
        std::lock_guard<std::mutex> lk(self->cache_mutex_);
        auto it = self->cache_.find(path);
        if (it != self->cache_.end()) {
            if (it->second.is_valid()) {
                out->type = PL_TYPE_INT;
                out->i    = (int64_t)it->second->get_instance_id();
                return PL_OK;
            } else {
                self->cache_.erase(it);
            }
        }
    }

    auto* rl = godot::ResourceLoader::get_singleton();
    if (!rl) return PL_ERR_GENERIC;

    if (self->is_main_thread()) {
        godot::Ref<godot::Resource> res = rl->load(godot::String(res_path));
        if (!res.is_valid()) {
            ERR_PRINT(("[PolyLang/Resource] Failed to load: " + path).c_str());
            return PL_ERR_GENERIC;
        }
        self->cache_resource(path, res);
        out->type = PL_TYPE_INT;
        out->i    = (int64_t)res->get_instance_id();
        return PL_OK;
    }

    // Worker thread: enqueue deferred request.
    // FIX VLN-08: use timed wait instead of indefinite wait.
    std::mutex          done_mtx;
    std::condition_variable done_cv;
    bool                done_flag = false;

    {
        std::lock_guard<std::mutex> lk(self->deferred_mutex_);
        DeferredRequest req;
        req.path       = path;
        req.tier       = tier;
        req.out        = out;
        req.done_mutex = &done_mtx;
        req.done_cv    = &done_cv;
        req.done_flag  = &done_flag;
        self->deferred_queue_.push(std::move(req));
    }

    // FIX VLN-08: timed wait — unblocks on timeout or shutdown.
    std::unique_lock<std::mutex> lk(done_mtx);
    bool completed = done_cv.wait_for(lk,
        std::chrono::milliseconds(PL_RESOURCE_DEFERRED_TIMEOUT_MS),
        [&done_flag, &self = *self]{
            return done_flag || self.shutdown_flag_.load(std::memory_order_acquire);
        });

    if (!completed || self->shutdown_flag_.load(std::memory_order_acquire)) {
        ERR_PRINT(("[PolyLang/Resource] Deferred load timed out or shutdown for: " + path).c_str());
        pl_value_init(out);
        return PL_ERR_GENERIC;
    }

    return (out->type != PL_TYPE_NIL) ? PL_OK : PL_ERR_GENERIC;
}

/*static*/ void PLResourceBridge::pl_resource_release_impl(PLValue* v) {
    if (!v) return;
    pl_value_init(v);
}

godot::Variant PLResourceBridge::load_resource(const godot::String& path) {
    PLValue out;
    int rc = pl_resource_fetch_impl(path.utf8().get_data(), &out, SandboxTier::Trusted);
    if (rc != PL_OK) return godot::Variant();
    uint64_t iid = (uint64_t)out.i;
    return godot::Variant(godot::ObjectDB::get_instance(iid));
}

bool PLResourceBridge::is_path_allowed(const godot::String& path, int sandbox_tier) const {
    return check_access(path.utf8().get_data(), (SandboxTier)sandbox_tier);
}

void PLResourceBridge::cache_resource(const std::string& path,
                                       godot::Ref<godot::Resource> res) {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    cache_[path] = res;
}

// FIX VLN-08: Signal shutdown to unblock all waiting workers.
void PLResourceBridge::shutdown() {
    shutdown_flag_.store(true, std::memory_order_release);
    // Drain the deferred queue signalling all waiters as failed.
    std::queue<DeferredRequest> local;
    {
        std::lock_guard<std::mutex> lk(deferred_mutex_);
        std::swap(local, deferred_queue_);
    }
    while (!local.empty()) {
        auto& req = local.front();
        if (req.done_mutex && req.done_cv && req.done_flag) {
            std::lock_guard<std::mutex> dlk(*req.done_mutex);
            *req.done_flag = true;
            req.done_cv->notify_all();
        }
        local.pop();
    }
}

void PLResourceBridge::flush() {
    std::queue<DeferredRequest> local;
    {
        std::lock_guard<std::mutex> lk(deferred_mutex_);
        std::swap(local, deferred_queue_);
    }
    auto* rl = godot::ResourceLoader::get_singleton();
    while (!local.empty()) {
        auto& req = local.front();
        if (rl && req.out) {
            if (!check_access(req.path, req.tier)) {
                pl_value_init(req.out);
                ERR_PRINT(("[PolyLang/Resource] Deferred access denied for: " + req.path).c_str());
            } else {
                godot::Ref<godot::Resource> res =
                    rl->load(godot::String(req.path.c_str()));
                pl_value_init(req.out);
                if (res.is_valid()) {
                    cache_resource(req.path, res);
                    req.out->type = PL_TYPE_INT;
                    req.out->i    = (int64_t)res->get_instance_id();
                }
            }
        }
        if (req.done_mutex && req.done_cv && req.done_flag) {
            std::lock_guard<std::mutex> dlk(*req.done_mutex);
            *req.done_flag = true;
            req.done_cv->notify_all();
        }
        local.pop();
    }
}

} // namespace polylang
