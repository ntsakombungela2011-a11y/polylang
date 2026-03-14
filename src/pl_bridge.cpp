// =============================================================
// pl_bridge.cpp  —  PolyLang v6.6
//
// ZERO-TRUST AUDIT ROUND 2 FIXES:
//
// FIX VLN-03 [CRITICAL]: Recursive cross-language call → stack overflow.
//   BEFORE: call_script() had no recursion guard. Script A calls Script B
//           which calls back into Script A → infinite recursion until
//           the process stack is exhausted (crash/DoS).
//   AFTER:  Thread-local PL_BRIDGE_CALL_DEPTH counter. If it exceeds
//           PL_BRIDGE_MAX_CALL_DEPTH (64) the call is rejected with
//           PL_ERR_GENERIC and an ERR_PRINT. Depth is decremented via
//           RAII guard to be exception-safe.
// =============================================================
#include "pl_bridge.hpp"
#include "polylang_script_instance.hpp"
#include "variant_bridge.hpp"

#include <godot_cpp/core/error_macros.hpp>

namespace polylang {

// Hard limit on cross-language call stack depth (VLN-03).
static constexpr int PL_BRIDGE_MAX_CALL_DEPTH = 64;
static thread_local int tl_call_depth = 0;

struct CallDepthGuard {
    explicit CallDepthGuard() { ++tl_call_depth; }
    ~CallDepthGuard()         { --tl_call_depth; }
};

// ── PLScriptRegistry ──────────────────────────────────────────

PLScriptRegistry* PLScriptRegistry::singleton_ = nullptr;

PLScriptRegistry* PLScriptRegistry::get_singleton() { return singleton_; }
void PLScriptRegistry::create()  { singleton_ = new PLScriptRegistry(); }
void PLScriptRegistry::destroy() { delete singleton_; singleton_ = nullptr; }

void PLScriptRegistry::register_instance(const std::string& path,
                                          PolyLangScriptInstance* inst) {
    if (!inst) return;
    std::unique_lock lk(mutex_);
    map_[path].push_back(inst);
}

void PLScriptRegistry::unregister_instance(const std::string& path,
                                            PolyLangScriptInstance* inst) {
    std::unique_lock lk(mutex_);
    auto it = map_.find(path);
    if (it == map_.end()) return;
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), inst), vec.end());
    if (vec.empty()) map_.erase(it);
}

PolyLangScriptInstance* PLScriptRegistry::find_instance(const std::string& path) const {
    std::shared_lock lk(mutex_);
    auto it = map_.find(path);
    if (it == map_.end() || it->second.empty()) return nullptr;
    return it->second.front();
}

std::vector<PolyLangScriptInstance*>
PLScriptRegistry::find_all(const std::string& path) const {
    std::shared_lock lk(mutex_);
    auto it = map_.find(path);
    if (it == map_.end()) return {};
    return it->second;
}

// ── PolyLangBridge ────────────────────────────────────────────

PolyLangBridge* PolyLangBridge::singleton_ = nullptr;
PolyLangBridge* PolyLangBridge::get_singleton() { return singleton_; }

godot::Variant PolyLangBridge::call_script(const godot::String& path,
                                            const godot::String& method,
                                            const godot::Array&  args) {
    // FIX VLN-03: Reject if call depth exceeds maximum.
    if (tl_call_depth >= PL_BRIDGE_MAX_CALL_DEPTH) {
        ERR_PRINT(("[PolyLang/Bridge] Max cross-language call depth ("
                   + std::to_string(PL_BRIDGE_MAX_CALL_DEPTH)
                   + ") exceeded — recursive call rejected").c_str());
        return godot::Variant();
    }
    CallDepthGuard depth_guard;

    std::string spath  = path.utf8().get_data();
    std::string smethod = method.utf8().get_data();

    PolyLangScriptInstance* inst =
        PLScriptRegistry::get_singleton()->find_instance(spath);
    if (!inst) {
        ERR_PRINT(("[PolyLang/Bridge] No live instance for: " + spath).c_str());
        return godot::Variant();
    }

    int32_t argc = static_cast<int32_t>(args.size());
    std::vector<PLValue> pl_args(argc);
    for (int32_t i = 0; i < argc; ++i)
        VariantBridge::to_pl_value(args[i], pl_args[i]);

    PLValue ret{}; pl_value_init(&ret);
    int r = inst->call_method_direct(smethod.c_str(),
                                     pl_args.data(), argc, &ret);

    godot::Variant result;
    if (r == PL_OK) result = VariantBridge::from_pl_value(ret);

    const PLAdapterVTable* vt = inst->get_vtable();
    for (auto& v : pl_args)
        if (vt && vt->pl_free_value_contents) vt->pl_free_value_contents(&v);
    if (vt && vt->pl_free_value_contents) vt->pl_free_value_contents(&ret);

    return result;
}

int PolyLangBridge::call_native(const std::string& path,
                                 const std::string& method,
                                 PLValue* args, int32_t argc,
                                 PLValue* ret_out) {
    // FIX VLN-03: depth guard on native path too.
    if (tl_call_depth >= PL_BRIDGE_MAX_CALL_DEPTH) {
        pl_value_init(ret_out);
        ERR_PRINT("[PolyLang/Bridge] Max call depth exceeded in call_native");
        return PL_ERR_GENERIC;
    }
    CallDepthGuard depth_guard;

    PolyLangScriptInstance* inst =
        PLScriptRegistry::get_singleton()->find_instance(path);
    if (!inst) {
        pl_value_init(ret_out);
        return PL_ERR_GENERIC;
    }
    return inst->call_method_direct(method.c_str(), args, argc, ret_out);
}

void PolyLangBridge::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("call_script", "path", "method", "args"),
        &PolyLangBridge::call_script);
}

} // namespace polylang
