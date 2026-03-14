// =============================================================
// pl_sandbox_tiers.hpp  —  PolyLang Sandbox Tier Definitions v5
// =============================================================
// Architecture Feature #4: Sandbox tiers.
//
// Three tiers with fixed capability masks:
//
//   Trusted      — unconstrained, same as pl_compile().
//                  allowed_caps = PL_SANDBOX_FULL
//
//   Isolated     — no file I/O, no network, no subprocess.
//                  allowed_caps = PL_SANDBOX_NONE
//                  Individual caps can be added via .polylang_config.
//
//   Quarantined  — math + string only.  Strictest.
//                  allowed_caps = PL_SANDBOX_NONE  (no additions allowed)
//
// Tier resolution order:
//   1. Explicit .polylang_config "sandbox = <tier>" overrides all.
//   2. RuntimeManager::register_sandboxed_path() for programmatic control.
//   3. No config → Trusted (unconstrained).
//
// Adapters receive the resolved allowed_caps bitmask via
// pl_compile_sandboxed(source, path, allowed_caps).
// Each adapter's sandbox enforcement is documented in its header.
// =============================================================
#pragma once
#include "../include/pl_adapter_vtable.h"
#include <string>

namespace polylang {

enum class SandboxTier : uint8_t {
    Trusted     = 0,  // unrestricted
    Isolated    = 1,  // no OS access (default for sandboxed mods)
    Quarantined = 2,  // math + string only (most restrictive)
};

inline uint32_t caps_for_tier(SandboxTier tier) {
    switch (tier) {
        case SandboxTier::Trusted:     return PL_SANDBOX_FULL;
        case SandboxTier::Isolated:    return PL_SANDBOX_NONE;
        case SandboxTier::Quarantined: return PL_SANDBOX_NONE;
    }
    return PL_SANDBOX_NONE;
}

// Whether a tier is considered sandboxed at all (should use pl_compile_sandboxed)
inline bool tier_is_sandboxed(SandboxTier tier) {
    return tier != SandboxTier::Trusted;
}

inline SandboxTier tier_from_string(const std::string& s) {
    if (s == "trusted")     return SandboxTier::Trusted;
    if (s == "quarantined") return SandboxTier::Quarantined;
    return SandboxTier::Isolated;  // default
}

inline const char* tier_to_string(SandboxTier t) {
    switch (t) {
        case SandboxTier::Trusted:     return "trusted";
        case SandboxTier::Isolated:    return "isolated";
        case SandboxTier::Quarantined: return "quarantined";
    }
    return "isolated";
}

} // namespace polylang
