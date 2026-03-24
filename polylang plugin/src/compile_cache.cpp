// =============================================================
// compile_cache.cpp  —  v5 + sandbox (SECTION 5)
// =============================================================
#include "compile_cache.hpp"
#include <cstdlib>
#include <cstdint>
#include <new>

namespace polylang {

CompileCache* CompileCache::singleton_ = nullptr;

// ── SHA-256 (portable, stack-optimized for small scripts) ─────
static constexpr size_t SHA256_STACK_LIMIT = 8192;

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

#define ROR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define S0(a)      (ROR32(a,2)^ROR32(a,13)^ROR32(a,22))
#define S1(e)      (ROR32(e,6)^ROR32(e,11)^ROR32(e,25))
#define G0(x)      (ROR32(x,7)^ROR32(x,18)^((x)>>3))
#define G1(x)      (ROR32(x,17)^ROR32(x,19)^((x)>>10))

SHA256Result sha256(const char* data, size_t len) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };

    uint64_t bit_len = (uint64_t)len * 8;
    size_t   padded  = ((len + 9 + 63) / 64) * 64;

    uint8_t stack_buf[SHA256_STACK_LIMIT];
    uint8_t* msg   = nullptr;
    bool heap_used = (padded > SHA256_STACK_LIMIT);
    if (heap_used) {
        msg = new (std::nothrow) uint8_t[padded];
        if (!msg) { SHA256Result r{}; return r; }
    } else {
        msg = stack_buf;
    }

    memset(msg, 0, padded);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    for (int i = 7; i >= 0; --i)
        msg[padded - 8 + (7 - i)] = (uint8_t)(bit_len >> (i * 8));

    for (size_t chunk = 0; chunk < padded; chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* b = msg + chunk + i * 4;
            w[i] = ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|
                   ((uint32_t)b[2]<<8)|(uint32_t)b[3];
        }
        for (int i = 16; i < 64; ++i)
            w[i] = G1(w[i-2]) + w[i-7] + G0(w[i-15]) + w[i-16];

        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],
                 e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t T1 = hh + S1(e) + CH(e,f,g) + K[i] + w[i];
            uint32_t T2 = S0(a) + MAJ(a,b,c);
            hh=g; g=f; f=e; e=d+T1;
            d=c; c=b; b=a; a=T1+T2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    if (heap_used) delete[] msg;

    SHA256Result r;
    for (int i = 0; i < 8; ++i) {
        r.bytes[i*4+0] = (uint8_t)(h[i]>>24);
        r.bytes[i*4+1] = (uint8_t)(h[i]>>16);
        r.bytes[i*4+2] = (uint8_t)(h[i]>>8);
        r.bytes[i*4+3] = (uint8_t)(h[i]);
    }
    return r;
}

// ── Cache key helpers ─────────────────────────────────────────

// SECTION 5: Sandboxed entries use a distinct cache key so they never
// collide with trusted compilations of the same source file.
// Format: "res://path/script.pl.lua:s:000000ff"
/*static*/ std::string CompileCache::sandboxed_key(const std::string& res_path,
                                                    uint32_t allowed_caps) {
    char buf[16];
    snprintf(buf, sizeof(buf), ":s:%08x", allowed_caps);
    return res_path + buf;
}

// ── Internal compile_into_cache ───────────────────────────────

void* CompileCache::compile_into_cache(const std::string& cache_key,
                                        const std::string& source,
                                        const PLAdapterVTable* vtable,
                                        bool sandboxed,
                                        uint32_t allowed_caps) {
    std::string hash = sha256_hex(source.c_str(), source.size());

    // Fast path: cache hit
    {
        std::shared_lock lk(mutex_);
        auto it = entries_.find(cache_key);
        if (it != entries_.end() && it->second.hash == hash) {
            // FIX A-1: atomic fetch_add safe under shared_lock
            it->second.ref_count.fetch_add(1, std::memory_order_acq_rel);
            return it->second.handle;
        }
    }

    // Circular import guard
    LoadingGuard guard(cache_key);
    if (!guard) return nullptr;

    // Compile off-lock (can be slow)
    void* handle = nullptr;
    if (sandboxed) {
        if (!(vtable->capabilities & PL_CAP_SANDBOX) || !vtable->pl_compile_sandboxed) {
            ERR_PRINT(("[PolyLang] Adapter does not support PL_CAP_SANDBOX for: " + cache_key).c_str());
            return nullptr;
        }
        handle = vtable->pl_compile_sandboxed(source.c_str(), cache_key.c_str(), allowed_caps);
    } else {
        if (!vtable->pl_compile) return nullptr;
        handle = vtable->pl_compile(source.c_str(), cache_key.c_str());
    }
    if (!handle) return nullptr;

    // Store under exclusive lock
    {
        std::unique_lock lk(mutex_);
        auto it = entries_.find(cache_key);
        if (it != entries_.end()) {
            if (it->second.hash == hash) {
                it->second.ref_count.fetch_add(1, std::memory_order_acq_rel);
                vtable->pl_free_compiled(handle);
                return it->second.handle;
            }
            if (it->second.ref_count.load(std::memory_order_acquire) <= 0
                && it->second.handle && it->second.vtable) {
                it->second.vtable->pl_free_compiled(it->second.handle);
            }
            entries_.erase(it);
        }
        Entry& e   = entries_[cache_key];
        e.handle   = handle;
        e.hash     = hash;
        e.ref_count.store(1, std::memory_order_release);
        e.vtable   = vtable;
        e.sandboxed = sandboxed;
    }
    return handle;
}

// ── Public API ────────────────────────────────────────────────

void* CompileCache::get_or_compile(const std::string& res_path,
                                    const std::string& source,
                                    const PLAdapterVTable* vtable) {
    if (!vtable) return nullptr;
    return compile_into_cache(res_path, source, vtable, false, PL_SANDBOX_NONE);
}

// SECTION 5: Routes sandboxed compile through pl_compile_sandboxed.
void* CompileCache::get_or_compile_sandboxed(const std::string& res_path,
                                              const std::string& source,
                                              const PLAdapterVTable* vtable,
                                              uint32_t allowed_caps) {
    if (!vtable) return nullptr;
    std::string key = sandboxed_key(res_path, allowed_caps);
    return compile_into_cache(key, source, vtable, true, allowed_caps);
}

void CompileCache::release(const std::string& cache_key,
                            const PLAdapterVTable* vtable) {
    std::unique_lock lk(mutex_);
    auto it = entries_.find(cache_key);
    if (it == entries_.end()) return;
    int32_t remaining = it->second.ref_count.fetch_sub(
        1, std::memory_order_acq_rel) - 1;
    if (remaining <= 0) {
        const PLAdapterVTable* vt = it->second.vtable ? it->second.vtable : vtable;
        if (vt && vt->pl_free_compiled && it->second.handle)
            vt->pl_free_compiled(it->second.handle);
        entries_.erase(it);
    }
}

void CompileCache::invalidate(const std::string& res_path) {
    std::unique_lock lk(mutex_);
    // Invalidate trusted entry
    auto it = entries_.find(res_path);
    if (it != entries_.end()) it->second.hash.clear();
    // Invalidate all sandboxed variants (any allowed_caps)
    for (auto& [key, entry] : entries_) {
        if (key.size() > res_path.size() &&
            key.compare(0, res_path.size(), res_path) == 0 &&
            key[res_path.size()] == ':') {
            entry.hash.clear();
        }
    }
}

void CompileCache::clear_all() {
    std::unique_lock lk(mutex_);
    for (auto& [path, entry] : entries_) {
        if (entry.handle && entry.vtable && entry.vtable->pl_free_compiled) {
            entry.vtable->pl_free_compiled(entry.handle);
            entry.handle = nullptr;
        }
    }
    entries_.clear();
}

} // namespace polylang
