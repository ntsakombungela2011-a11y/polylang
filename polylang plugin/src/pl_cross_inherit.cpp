// =============================================================
// pl_cross_inherit.cpp  —  PolyLang v6.5
//
// FIX H-3: resolve_chain() holds map_mutex_ for the entire walk.
//   BEFORE: base_path_for() acquired/released map_mutex_ per step.
//           Between steps, another thread could modify entries_,
//           making the in-progress chain stale (TOCTOU).
//   AFTER:  resolve_chain_locked() holds a shared_lock for the
//           entire traversal. No intermediate lock drops.
// =============================================================
#include "pl_cross_inherit.hpp"

#include "pl_bridge.hpp"
#include "pl_polyglot_script.hpp"
#include "runtime_manager.hpp"
#include "variant_bridge.hpp"
#include "polylang_script_instance.hpp"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/resource_loader.hpp>

#include <algorithm>

namespace polylang {

PLCrossInherit* PLCrossInherit::singleton_ = nullptr;
PLCrossInherit* PLCrossInherit::get_singleton() {
    if (!singleton_) singleton_ = new PLCrossInherit();
    return singleton_;
}

void PLCrossInherit::destroy() {
    delete singleton_;
    singleton_ = nullptr;
}

// ── Registration ──────────────────────────────────────────────

void PLCrossInherit::register_base(const std::string& child_path,
                                    const std::string& base_path,
                                    SandboxTier child_tier) {
    if (child_path.empty() || base_path.empty()) return;
    std::lock_guard<std::mutex> lk(map_mutex_);
    InheritanceEntry e;
    e.child_path  = child_path;
    e.base_path   = base_path;
    e.child_tier  = child_tier;
    entries_[child_path] = std::move(e);
}

void PLCrossInherit::unregister(const std::string& child_path) {
    std::lock_guard<std::mutex> lk(map_mutex_);
    entries_.erase(child_path);
}

std::string PLCrossInherit::base_path_for(const std::string& child) const {
    std::lock_guard<std::mutex> lk(map_mutex_);
    auto it = entries_.find(child);
    return (it != entries_.end()) ? it->second.base_path : "";
}

// ── FIX H-3: Whole-walk locked chain resolution ──────────────
// Internal helper: CALLER must already hold map_mutex_.

std::vector<std::string> PLCrossInherit::resolve_chain_locked(
        const std::string& child) const {
    // map_mutex_ held by caller.
    std::vector<std::string> chain;
    chain.push_back(child);
    std::string cur = child;
    for (int depth = 0; depth < 16; ++depth) {
        auto it = entries_.find(cur);
        if (it == entries_.end()) break;
        const std::string& base = it->second.base_path;
        if (base.empty()) break;
        // Cycle detection.
        if (std::find(chain.begin(), chain.end(), base) != chain.end()) {
            ERR_PRINT(("[PolyLang/Inherit] Cycle detected at: " + base).c_str());
            break;
        }
        chain.push_back(base);
        cur = base;
    }
    return chain;
}

std::vector<std::string> PLCrossInherit::resolve_chain(
        const std::string& child) const {
    std::lock_guard<std::mutex> lk(map_mutex_);
    return resolve_chain_locked(child);
}

// ── Sandbox check ─────────────────────────────────────────────

static bool inherit_allowed(SandboxTier child, SandboxTier /*base*/) {
    return child != SandboxTier::Quarantined;
}

// ── call_on_base ──────────────────────────────────────────────

int PLCrossInherit::call_on_base(const std::string& base_path,
                                  godot::Object*     owner,
                                  const char*        method,
                                  PLValue*           args, int32_t argc,
                                  PLValue*           ret) {
    pl_value_init(ret);
    if (!owner) return PL_ERR_GENERIC;

    bool is_polylang = (base_path.find(".pl.") != std::string::npos)
                    || (base_path.size() >= 5
                        && base_path.substr(base_path.size()-5) == ".poly");
    bool is_gdscript = (base_path.size() >= 3
                        && base_path.substr(base_path.size()-3) == ".gd");

    if (is_polylang) {
        auto* bridge = PolyLangBridge::get_singleton();
        if (!bridge) return PL_ERR_GENERIC;
        godot::Array gd_args;
        for (int32_t i = 0; i < argc; ++i)
            gd_args.push_back(VariantBridge::from_pl_value(args[i]));
        godot::Variant result = bridge->call_script(
            godot::String(base_path.c_str()),
            godot::String(method), gd_args);
        VariantBridge::to_pl_value(result, *ret);
        return PL_OK;
    }

    if (is_gdscript) {
        if (owner->has_method(godot::StringName(method))) {
            godot::Array gd_args;
            for (int32_t i = 0; i < argc; ++i)
                gd_args.push_back(VariantBridge::from_pl_value(args[i]));
            godot::Variant result = owner->callv(godot::StringName(method), gd_args);
            VariantBridge::to_pl_value(result, *ret);
            return PL_OK;
        }
        return PL_ERR_METHOD_NOT_FOUND;
    }

    ERR_PRINT(("[PolyLang/Inherit] Unknown base script type: " + base_path).c_str());
    return PL_ERR_GENERIC;
}

int PLCrossInherit::get_on_base(const std::string& base_path,
                                 godot::Object*     owner,
                                 const char*        prop,
                                 PLValue*           ret) {
    pl_value_init(ret);
    if (!owner || !prop) return PL_ERR_GENERIC;

    bool is_polylang = (base_path.find(".pl.") != std::string::npos)
                    || (base_path.size() >= 5
                        && base_path.substr(base_path.size()-5) == ".poly");

    if (is_polylang) {
        auto* reg  = PLScriptRegistry::get_singleton();
        PLScriptHandle handle = reg ? reg->find_handle(base_path) : PLScriptHandle{};
        if (!handle.valid()) return PL_ERR_GENERIC;
        switch (handle.kind) {
            case PLScriptKind::PolyLang:
                return static_cast<PolyLangScriptInstance*>(handle.ptr)
                    ->get_property_direct(prop, ret);
            case PLScriptKind::Polyglot:
                return static_cast<PolyglotInstance*>(handle.ptr)
                    ->get_property_direct(prop, ret);
        }
        return PL_ERR_GENERIC;
    }

    godot::Variant val = owner->get(godot::StringName(prop));
    if (val.get_type() == godot::Variant::NIL) return PL_ERR_GENERIC;
    VariantBridge::to_pl_value(val, *ret);
    return PL_OK;
}

int PLCrossInherit::set_on_base(const std::string& base_path,
                                 godot::Object*     owner,
                                 const char*        prop,
                                 const PLValue*     val) {
    if (!owner || !prop || !val) return PL_ERR_GENERIC;

    bool is_polylang = (base_path.find(".pl.") != std::string::npos)
                    || (base_path.size() >= 5
                        && base_path.substr(base_path.size()-5) == ".poly");

    if (is_polylang) {
        auto* reg  = PLScriptRegistry::get_singleton();
        PLScriptHandle handle = reg ? reg->find_handle(base_path) : PLScriptHandle{};
        if (!handle.valid()) return PL_ERR_GENERIC;
        switch (handle.kind) {
            case PLScriptKind::PolyLang:
                return static_cast<PolyLangScriptInstance*>(handle.ptr)
                    ->set_property_direct(prop, val);
            case PLScriptKind::Polyglot:
                return static_cast<PolyglotInstance*>(handle.ptr)
                    ->set_property_direct(prop, val);
        }
        return PL_ERR_GENERIC;
    }

    godot::Variant gv = VariantBridge::from_pl_value(*val);
    owner->set(godot::StringName(prop), gv);
    return PL_OK;
}

// ── Public chain walkers ──────────────────────────────────────

int PLCrossInherit::try_base_call(const std::string& child_path,
                                   godot::Object*     owner,
                                   const char*        method,
                                   PLValue*           args, int32_t argc,
                                   PLValue*           ret_out) {
    pl_value_init(ret_out);

    // FIX H-3: resolve_chain() now holds the lock for the entire walk.
    std::vector<std::string> chain;
    SandboxTier child_tier = SandboxTier::Trusted;
    {
        std::lock_guard<std::mutex> lk(map_mutex_);
        auto it = entries_.find(child_path);
        if (it == entries_.end()) return PL_ERR_METHOD_NOT_FOUND;
        child_tier = it->second.child_tier;
        chain = resolve_chain_locked(child_path);
    }

    if (!inherit_allowed(child_tier, SandboxTier::Trusted)) {
        ERR_PRINT(("[PolyLang/Inherit] Quarantined script cannot call base: "
                   + child_path).c_str());
        return PL_ERR_SANDBOX;
    }

    for (size_t i = 1; i < chain.size(); ++i) {
        int rc = call_on_base(chain[i], owner, method, args, argc, ret_out);
        if (rc == PL_OK) return PL_OK;
        if (rc != PL_ERR_METHOD_NOT_FOUND) return rc;
    }
    return PL_ERR_METHOD_NOT_FOUND;
}

int PLCrossInherit::try_base_get(const std::string& child_path,
                                  godot::Object*     owner,
                                  const char*        prop,
                                  PLValue*           ret_out) {
    auto chain = resolve_chain(child_path);
    for (size_t i = 1; i < chain.size(); ++i) {
        int rc = get_on_base(chain[i], owner, prop, ret_out);
        if (rc == PL_OK) return PL_OK;
    }
    return PL_ERR_GENERIC;
}

int PLCrossInherit::try_base_set(const std::string& child_path,
                                  godot::Object*     owner,
                                  const char*        prop,
                                  const PLValue*     val) {
    auto chain = resolve_chain(child_path);
    for (size_t i = 1; i < chain.size(); ++i) {
        int rc = set_on_base(chain[i], owner, prop, val);
        if (rc == PL_OK) return PL_OK;
    }
    return PL_ERR_GENERIC;
}

// ── pl_call_super_impl ────────────────────────────────────────

/*static*/ int PLCrossInherit::pl_call_super_impl(void* child_instance_ptr,
                                                    const char* method,
                                                    PLValue* args, int32_t argc,
                                                    PLValue* ret) {
    auto* inst = static_cast<PolyLangScriptInstance*>(child_instance_ptr);
    if (!inst) { pl_value_init(ret); return PL_ERR_GENERIC; }
    std::string child_path = inst->get_script_path();
    godot::Object* owner   = inst->get_owner_object();
    return PLCrossInherit::get_singleton()->try_base_call(
        child_path, owner, method, args, argc, ret);
}

} // namespace polylang
