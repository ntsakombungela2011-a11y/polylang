// =============================================================
// runtime_manager.cpp  —  PolyLang v6.5
//
// FIX H-6: validate_required_vtable_slots() called after adapter load.
//   BEFORE: Only abi_version was checked after fn(&vtable).
//           Required slots (pl_compile, pl_instantiate_class,
//           pl_free_compiled, pl_free_instance, pl_free_value_contents,
//           pl_call_method) could be null, causing crashes on first use.
//   AFTER:  All required function pointers verified; adapter rejected
//           with a clear error message if any are null.
// =============================================================
#include "runtime_manager.hpp"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>

#include <godot_cpp/core/error_macros.hpp>

namespace polylang {

RuntimeManager* RuntimeManager::singleton_ = nullptr;

RuntimeManager* RuntimeManager::get_singleton() { return singleton_; }

void RuntimeManager::create() {
    ERR_FAIL_COND_MSG(singleton_, "RuntimeManager already created");
    singleton_ = new RuntimeManager();
}

void RuntimeManager::destroy() {
    if (!singleton_) return;
    singleton_->stop_health_monitor();
    singleton_->unload_all();
    delete singleton_;
    singleton_ = nullptr;
}

// ── FIX H-6: Required vtable slot validation ─────────────────
// Called immediately after fn(&vtable) fills in the adapter's function pointers.
// Returns true if all required slots are non-null, false otherwise.

static bool validate_required_vtable_slots(const PLAdapterVTable* vt,
                                            const char* so_path) {
    struct { const void* ptr; const char* name; } required[] = {
        { (const void*)vt->pl_compile,             "pl_compile"             },
        { (const void*)vt->pl_free_compiled,        "pl_free_compiled"       },
        { (const void*)vt->pl_instantiate_class,    "pl_instantiate_class"   },
        { (const void*)vt->pl_free_instance,        "pl_free_instance"       },
        { (const void*)vt->pl_free_value_contents,  "pl_free_value_contents" },
        { (const void*)vt->pl_call_method,          "pl_call_method"         },
    };
    bool ok = true;
    for (const auto& r : required) {
        if (!r.ptr) {
            ERR_PRINT((std::string("PolyLang: adapter missing required vtable slot '")
                       + r.name + "' in " + so_path).c_str());
            ok = false;
        }
    }
    return ok;
}

// ── Adapter lifecycle ─────────────────────────────────────────

bool RuntimeManager::load_adapter(LanguageID id, const char* so_path) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= LANGUAGE_COUNT) return false;
    if (adapters_[idx].loaded) return true;

    void* dl = PL_DLOPEN(so_path);
    if (!dl) {
        ERR_PRINT((std::string("PolyLang: dlopen failed for ") + so_path).c_str());
        return false;
    }

    PLGetVTableFn fn = reinterpret_cast<PLGetVTableFn>(PL_DLSYM(dl, "pl_get_vtable"));
    if (!fn) {
        ERR_PRINT((std::string("PolyLang: pl_get_vtable not found in ") + so_path).c_str());
        PL_DLCLOSE(dl);
        return false;
    }

    memset(&adapters_[idx].vtable, 0, sizeof(PLAdapterVTable));
    fn(&adapters_[idx].vtable);

    // ABI version check.
    if (adapters_[idx].vtable.abi_version != PL_ABI_VERSION) {
        ERR_PRINT((std::string("PolyLang: ABI mismatch in ") + so_path
            + " (expected " + std::to_string(PL_ABI_VERSION)
            + ", got " + std::to_string(adapters_[idx].vtable.abi_version) + ")").c_str());
        PL_DLCLOSE(dl);
        return false;
    }

    // FIX H-6: Validate all required function pointer slots.
    if (!validate_required_vtable_slots(&adapters_[idx].vtable, so_path)) {
        ERR_PRINT((std::string("PolyLang: adapter rejected (missing required slots): ") + so_path).c_str());
        PL_DLCLOSE(dl);
        return false;
    }

    if (adapters_[idx].vtable.pl_init_runtime) {
        int rc = adapters_[idx].vtable.pl_init_runtime();
        if (rc != 0) {
            ERR_PRINT((std::string("PolyLang: pl_init_runtime failed (") + std::to_string(rc)
                       + ") for " + so_path).c_str());
            PL_DLCLOSE(dl);
            return false;
        }
    }

    adapters_[idx].dl_handle = dl;
    adapters_[idx].loaded    = true;
    return true;
}

PLAdapterVTable* RuntimeManager::get_vtable(LanguageID id) const {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= LANGUAGE_COUNT) return nullptr;
    if (!adapters_[idx].loaded) return nullptr;
    return const_cast<PLAdapterVTable*>(&adapters_[idx].vtable);
}

void RuntimeManager::unload_all() {
    for (int i = 0; i < LANGUAGE_COUNT; ++i) {
        if (!adapters_[i].loaded) continue;
        if (adapters_[i].vtable.pl_shutdown_runtime)
            adapters_[i].vtable.pl_shutdown_runtime();
        if (adapters_[i].dl_handle)
            PL_DLCLOSE(adapters_[i].dl_handle);
        adapters_[i].loaded    = false;
        adapters_[i].dl_handle = nullptr;
    }
}

// ── Sandbox ───────────────────────────────────────────────────

void RuntimeManager::register_sandboxed_path(const std::string& res_path,
                                              uint32_t           allowed_caps,
                                              const std::string& tier) {
    std::unique_lock lock(sandbox_mutex_);
    if (sandbox_map_.count(res_path)) return;
    sandbox_map_[res_path] = { allowed_caps, tier };
}

bool RuntimeManager::is_sandboxed(const std::string& res_path) const {
    std::shared_lock lock(sandbox_mutex_);
    return sandbox_map_.count(res_path) > 0;
}

uint32_t RuntimeManager::sandboxed_caps(const std::string& res_path) const {
    std::shared_lock lock(sandbox_mutex_);
    auto it = sandbox_map_.find(res_path);
    return it != sandbox_map_.end() ? it->second.allowed_caps : PL_SANDBOX_NONE;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool RuntimeManager::parse_sidecar_file(const std::string& path,
                                         std::string& out_tier,
                                         uint32_t&    out_caps) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    out_tier = "isolated";
    out_caps = PL_SANDBOX_NONE;
    std::string line;

    while (std::getline(f, line)) {
        auto ci = line.find('#');
        if (ci != std::string::npos) line = line.substr(0, ci);
        line = trim(line);
        if (line.empty()) continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "sandbox") {
            out_tier = val;
            if (val == "trusted")          out_caps = PL_SANDBOX_FULL;
            else if (val == "quarantined") out_caps = PL_SANDBOX_NONE;
            else                           out_caps = PL_SANDBOX_NONE;
        } else if (key == "allowed_caps") {
            std::istringstream ss(val);
            std::string cap;
            while (std::getline(ss, cap, ',')) {
                cap = trim(cap);
                if      (cap == "file_read")  out_caps |= PL_SANDBOX_FILE_READ;
                else if (cap == "file_write") out_caps |= PL_SANDBOX_FILE_WRITE;
                else if (cap == "network")    out_caps |= PL_SANDBOX_NETWORK;
                else if (cap == "process")    out_caps |= PL_SANDBOX_PROCESS;
                else if (cap == "full")       out_caps  = PL_SANDBOX_FULL;
            }
        }
    }
    return true;
}

void RuntimeManager::maybe_register_sidecar(const std::string& res_path) {
    if (is_sandboxed(res_path)) return;

    auto try_config = [&](const std::string& config_path) -> bool {
        std::string tier;
        uint32_t    caps;
        if (!parse_sidecar_file(config_path, tier, caps)) return false;
        if (tier == "trusted") return true;
        register_sandboxed_path(res_path, caps, tier);
        return true;
    };

    if (try_config(res_path + ".polylang_config")) return;

    std::string dir = res_path;
    auto slash = dir.rfind('/');
    if (slash == std::string::npos) slash = dir.rfind('\\');
    if (slash != std::string::npos) {
        dir = dir.substr(0, slash);
        try_config(dir + "/.polylang_config");
    }
}

// ── Odin-specific ─────────────────────────────────────────────

void RuntimeManager::register_odin_script_so(const std::string& res_path,
                                               const std::string& so_path) {
    std::lock_guard lock(odin_so_mutex_);
    odin_so_map_[res_path] = so_path;
}

std::string RuntimeManager::odin_script_so_path(const std::string& res_path) const {
    std::lock_guard lock(odin_so_mutex_);
    auto it = odin_so_map_.find(res_path);
    return it != odin_so_map_.end() ? it->second : "";
}

// ── Health monitor ────────────────────────────────────────────

void RuntimeManager::start_health_monitor() {
    if (health_running_.load()) return;
    health_running_.store(true);
    health_thread_ = std::thread([this]{ health_monitor_loop(); });
}

void RuntimeManager::stop_health_monitor() {
    if (!health_running_.load()) return;
    health_running_.store(false);
    health_cv_.notify_all();
    if (health_thread_.joinable()) health_thread_.join();
}

void RuntimeManager::health_monitor_loop() {
    while (health_running_.load()) {
        std::unique_lock lock(health_mutex_);
        health_cv_.wait_for(lock, std::chrono::seconds(5),
            [this]{ return !health_running_.load(); });
    }
}

} // namespace polylang
