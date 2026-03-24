// =============================================================
// runtime_manager.hpp  —  PolyLang v6.3
// =============================================================
// v6.3 changes:
//   • LanguageID::Odin added (index 16, extension "pl.odin")
//   • LANGUAGE_COUNT bumped to 17
//   • OdinBuildMode enum: controls AOT vs pre-built .so loading
//   • register_odin_script_so() caches pre-built Odin script binaries
// =============================================================
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <cstring>

#include <godot_cpp/core/error_macros.hpp>

#include "../include/pl_adapter_vtable.h"

#if defined(_WIN32)
#  include <windows.h>
#  define PL_DLOPEN(p)  (void*)LoadLibraryA(p)
#  define PL_DLSYM(h,s) (void*)GetProcAddress((HMODULE)(h),(s))
#  define PL_DLCLOSE(h) FreeLibrary((HMODULE)(h))
#elif defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
#  include <dlfcn.h>
#  define PL_DLOPEN(p)  dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define PL_DLSYM(h,s) dlsym((h),(s))
#  define PL_DLCLOSE(h) dlclose(h)
#endif

namespace polylang {

enum class LanguageID : int {
    Lua = 0, Python, JavaScript, TypeScript, Squirrel, Wren,
    AngelScript, Julia, Kotlin, Go, Swift, Haxe, CSharp, Nim,
    Rust, Zig,
    Odin,     // v6.3: index 16
    COUNT
};
constexpr int LANGUAGE_COUNT = static_cast<int>(LanguageID::COUNT);

inline const char* language_name(LanguageID id) {
    static const char* n[] = {
        "lua","python","javascript","typescript","squirrel","wren",
        "angelscript","julia","kotlin","go","swift","haxe","csharp","nim",
        "rust","zig",
        "odin"  // v6.3
    };
    int i = static_cast<int>(id);
    return (i >= 0 && i < LANGUAGE_COUNT) ? n[i] : "unknown";
}

inline const char* language_extension(LanguageID id) {
    static const char* e[] = {
        "pl.lua","pl.py","pl.js","pl.ts","pl.nut","pl.wren",
        "pl.as","pl.jl","pl.kt","pl.go","pl.swift","pl.hx","pl.cs","pl.nim",
        "pl.rs","pl.zig",
        "pl.odin"  // v6.3
    };
    int i = static_cast<int>(id);
    return (i >= 0 && i < LANGUAGE_COUNT) ? e[i] : "";
}

inline LanguageID language_from_path(const char* path) {
    if (!path) return LanguageID::COUNT;
    std::string p(path);
    for (int i = 0; i < LANGUAGE_COUNT; ++i) {
        std::string fe = ".";
        fe += language_extension(static_cast<LanguageID>(i));
        if (p.size() >= fe.size() &&
            p.compare(p.size() - fe.size(), fe.size(), fe) == 0)
            return static_cast<LanguageID>(i);
    }
    return LanguageID::COUNT;
}

// ── Sandbox registration ──────────────────────────────────────

struct SandboxEntry {
    uint32_t    allowed_caps;
    std::string tier_name;   // "isolated" | "quarantined" | "trusted"
};

// ── Odin build mode ───────────────────────────────────────────
// Controls how .pl.odin scripts are handled at compile time.
enum class OdinBuildMode : uint8_t {
    // Pre-built: user provides <script>.pl.odin.so alongside the source.
    // pl_compile() just loads the .so — no Odin compiler invoked.
    PreBuilt = 0,
    // AOT: invoke `odin build` at runtime (requires odin in PATH).
    // Outputs <cache_dir>/<hash>.so; cached between reloads.
    AOT      = 1,
};

// ── RuntimeManager ────────────────────────────────────────────

class RuntimeManager {
public:
    static RuntimeManager* get_singleton();
    static void create();
    static void destroy();

    // ── Adapter .so lifecycle ─────────────────────────────────

    // Load adapter .so and call pl_init_runtime().
    // Returns false if load or init fails.
    bool load_adapter(LanguageID id, const char* so_path);

    // Retrieve the vtable for a loaded adapter (null if not loaded).
    PLAdapterVTable* get_vtable(LanguageID id) const;

    // Unload all adapters (calls pl_shutdown_runtime + dlclose).
    void unload_all();

    // ── Sandbox path registration ─────────────────────────────

    // Mark a res:// script path as requiring sandboxed compilation.
    // Idempotent — re-registering the same path is a no-op.
    void register_sandboxed_path(const std::string& res_path,
                                  uint32_t           allowed_caps,
                                  const std::string& tier = "isolated");

    bool     is_sandboxed(const std::string& res_path) const;
    uint32_t sandboxed_caps(const std::string& res_path) const;

    // Parse a .polylang_config sidecar file and register if found.
    // Looks for <res_path>.polylang_config then <dir>/.polylang_config.
    void maybe_register_sidecar(const std::string& res_path);

    // ── Odin-specific ─────────────────────────────────────────

    OdinBuildMode odin_build_mode() const { return odin_build_mode_; }
    void set_odin_build_mode(OdinBuildMode m) { odin_build_mode_ = m; }

    // Cache directory for Odin AOT-compiled .so files.
    const std::string& odin_cache_dir() const { return odin_cache_dir_; }
    void set_odin_cache_dir(const std::string& d) { odin_cache_dir_ = d; }

    // Register a pre-built Odin script .so path explicitly.
    // The Odin adapter queries this before invoking the build pipeline.
    void register_odin_script_so(const std::string& res_path,
                                  const std::string& so_path);
    // Returns empty string if not registered.
    std::string odin_script_so_path(const std::string& res_path) const;

    // ── Health monitor ────────────────────────────────────────
    void start_health_monitor();
    void stop_health_monitor();

private:
    RuntimeManager() = default;

    struct AdapterEntry {
        void*            dl_handle{nullptr};
        PLAdapterVTable  vtable{};
        bool             loaded{false};
    };

    AdapterEntry adapters_[LANGUAGE_COUNT]{};

    mutable std::shared_mutex          sandbox_mutex_;
    std::unordered_map<std::string, SandboxEntry> sandbox_map_;

    mutable std::mutex                 odin_so_mutex_;
    std::unordered_map<std::string, std::string> odin_so_map_;

    OdinBuildMode                      odin_build_mode_{OdinBuildMode::PreBuilt};
    std::string                        odin_cache_dir_;

    std::thread                        health_thread_;
    std::atomic<bool>                  health_running_{false};
    std::condition_variable            health_cv_;
    std::mutex                         health_mutex_;

    static RuntimeManager*             singleton_;

    void health_monitor_loop();
    bool parse_sidecar_file(const std::string& config_path,
                             std::string& out_tier,
                             uint32_t&    out_caps);
};

} // namespace polylang
