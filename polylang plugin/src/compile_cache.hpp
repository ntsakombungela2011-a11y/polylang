#pragma once
// =============================================================
// compile_cache.hpp  —  Script compile cache (v5 + sandbox)
// SECTION 5: CompileCache integration for pl_compile_sandboxed
// =============================================================
// Changes for sandbox support:
//   • Entry stores sandboxed flag and allowed_caps alongside vtable.
//     Sandboxed and trusted compilations of the same source path are
//     stored under different keys (path + ":sandboxed") so a trusted
//     compile never caches over a sandboxed one or vice versa.
//   • get_or_compile_sandboxed() calls vtable->pl_compile_sandboxed
//     if PL_CAP_SANDBOX is set, otherwise returns PL_ERR_SANDBOX.
//   • clear_all() properly frees both sandboxed and trusted handles
//     via their stored vtable pointers.
// =============================================================
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <atomic>

#include <godot_cpp/core/error_macros.hpp>
#include "../include/pl_adapter_vtable.h"

namespace polylang {

// ── SHA-256 ───────────────────────────────────────────────────
struct SHA256Result { uint8_t bytes[32]; };
SHA256Result sha256(const char* data, size_t len);

inline std::string sha256_hex(const char* data, size_t len) {
    auto r = sha256(data, len);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[i*2]   = hex[r.bytes[i] >> 4];
        out[i*2+1] = hex[r.bytes[i] & 0xF];
    }
    return out;
}

// ── Dependency / circular import guard ───────────────────────
class DependencyGuard {
public:
    static bool push(const std::string& path) {
        auto& ctx = get_ctx();
        if (ctx.in_stack.count(path)) {
            std::string msg = "[PolyLang] Circular import: ";
            for (auto& p : ctx.stack) msg += p + " -> ";
            msg += path;
            ERR_PRINT(msg.c_str());
            return false;
        }
        ctx.stack.push_back(path);
        ctx.in_stack.insert(path);
        return true;
    }
    static void pop(const std::string& path) {
        auto& ctx = get_ctx();
        ctx.in_stack.erase(path);
        if (!ctx.stack.empty() && ctx.stack.back() == path)
            ctx.stack.pop_back();
    }
private:
    struct Ctx {
        std::vector<std::string>        stack;
        std::unordered_set<std::string> in_stack;
    };
    static Ctx& get_ctx() { thread_local Ctx c; return c; }
};

struct LoadingGuard {
    std::string path; bool ok;
    explicit LoadingGuard(const std::string& p)
        : path(p), ok(DependencyGuard::push(p)) {}
    ~LoadingGuard() { if (ok) DependencyGuard::pop(path); }
    explicit operator bool() const { return ok; }
};

// ── CompileCache ──────────────────────────────────────────────
class CompileCache {
public:
    static CompileCache* get_singleton() { return singleton_; }
    static void create()  { singleton_ = new CompileCache(); }
    static void destroy() { delete singleton_; singleton_ = nullptr; }

    // Trusted compilation (calls pl_compile)
    void* get_or_compile(const std::string& res_path,
                         const std::string& source,
                         const PLAdapterVTable* vtable);

    // SECTION 5: Sandboxed compilation (calls pl_compile_sandboxed)
    // Returns nullptr and logs error if adapter lacks PL_CAP_SANDBOX.
    // Cache key is res_path + ":s:" + hex(allowed_caps) so sandboxed
    // and trusted compilations of the same script never collide.
    void* get_or_compile_sandboxed(const std::string& res_path,
                                    const std::string& source,
                                    const PLAdapterVTable* vtable,
                                    uint32_t allowed_caps);

    void release(const std::string& cache_key, const PLAdapterVTable* vtable);
    void invalidate(const std::string& res_path); // invalidates both trusted + sandboxed

    void clear_all();

    // Build the cache key used for a sandboxed entry.
    static std::string sandboxed_key(const std::string& res_path, uint32_t allowed_caps);

private:
    // FIX A-1: ref_count is atomic<int32_t> — safe under shared_lock.
    // FIX A-5: vtable stored for proper handle freeing in destructor.
    struct Entry {
        void*                  handle{nullptr};
        std::string            hash;
        std::atomic<int32_t>   ref_count{0};
        const PLAdapterVTable* vtable{nullptr};
        bool                   sandboxed{false};

        Entry() = default;
        Entry(Entry&& o) noexcept
            : handle(o.handle), hash(std::move(o.hash))
            , ref_count(o.ref_count.load()), vtable(o.vtable)
            , sandboxed(o.sandboxed) {
            o.handle = nullptr; o.vtable = nullptr;
        }
        Entry& operator=(Entry&&) = delete;
    };

    ~CompileCache() { clear_all(); }

    void* compile_into_cache(const std::string& cache_key,
                              const std::string& source,
                              const PLAdapterVTable* vtable,
                              bool sandboxed,
                              uint32_t allowed_caps);

    mutable std::shared_mutex                    mutex_;
    std::unordered_map<std::string, Entry>       entries_;

    static CompileCache* singleton_;
};

} // namespace polylang
