// =============================================================
// mod_loader.hpp  —  PolyLang Mod Loader / .polylang_config parser v5
// =============================================================
// Responsibilities:
//   1. Scan a directory for .polylang_config JSON sidecar files.
//   2. Parse each config: sandbox flag, tier, allowed_caps, dependencies, version, entry.
//   3. Register sandboxed paths via RuntimeManager::register_sandboxed_path().
//   4. Resolve dependency graph (topological sort, cycle detection).
//   5. Expose ModLoader::load_mod_directory(path) for editor plugin / autoload.
//
// .polylang_config format (JSON):
//   {
//     "sandbox": true,
//     "tier": "isolated",          // "trusted" | "isolated" | "quarantined"
//     "allowed_caps": ["PL_SANDBOX_MATH", "PL_SANDBOX_STRING"],
//     "dependencies": ["base_mod@1.0", "utils@>=2.0"],
//     "version": "1.0.0",
//     "entry": "MyMod.pl.lua"
//   }
// =============================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include "../../include/pl_adapter_vtable.h"

namespace polylang {

// ── Sandbox tier ──────────────────────────────────────────────
enum class SandboxTier {
    Trusted,       // No restriction (same as non-sandboxed compile)
    Isolated,      // No file I/O, no network, no subprocess
    Quarantined,   // Math + string only
};

struct ModConfig {
    std::string             entry;          // Script filename (e.g. "MyMod.pl.lua")
    std::string             version;        // Semver string
    SandboxTier             tier{SandboxTier::Isolated};
    bool                    sandbox{false};
    uint32_t                allowed_caps{0}; // Parsed from allowed_caps array
    std::vector<std::string> dependencies;   // e.g. "base_mod@1.0"
};

// ── ModLoader ─────────────────────────────────────────────────
class ModLoader {
public:
    static ModLoader* get_singleton();

    // Load all mods from a directory.
    // Scans for *.polylang_config files, parses them, resolves deps,
    // registers sandboxed paths with RuntimeManager, and calls on_loaded
    // for each entry script path in dependency order.
    bool load_mod_directory(
        const std::string& dir_path,
        std::function<void(const std::string& script_path,
                           const ModConfig&   config)> on_loaded = nullptr);

    // Parse a single .polylang_config file.
    // Returns false on parse error.
    static bool parse_config(const std::string& json_path, ModConfig& out);

    // Convert tier string → SandboxTier.
    static SandboxTier tier_from_string(const std::string& s);

    // Compute allowed_caps bitmask from array of cap name strings.
    static uint32_t caps_from_names(const std::vector<std::string>& names);

    // Topological sort of mod names by dependency order.
    // Returns sorted list, or empty on cycle detection.
    static std::vector<std::string> topo_sort(
        const std::unordered_map<std::string, std::vector<std::string>>& deps);

private:
    static ModLoader* s_singleton;
};

} // namespace polylang
