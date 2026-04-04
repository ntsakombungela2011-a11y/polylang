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
#include "pl_polyglot_script.hpp"
#include "polylang_script_instance.hpp"
#include "variant_bridge.hpp"

#include <algorithm>

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
                                         godot::Object* owner,
                                         PolyLangScriptInstance* inst) {
    if (!inst) return;
    std::unique_lock lk(mutex_);
    map_[path].push_back({PLScriptKind::PolyLang, inst});
    if (owner) owner_map_[owner] = {PLScriptKind::PolyLang, inst};
}

void PLScriptRegistry::unregister_instance(const std::string& path,
                                           godot::Object* owner,
                                           PolyLangScriptInstance* inst) {
    std::unique_lock lk(mutex_);
    auto it = map_.find(path);
    if (it != map_.end()) {
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [inst](const PLScriptHandle& handle) {
                return handle.kind == PLScriptKind::PolyLang && handle.ptr == inst;
            }), vec.end());
        if (vec.empty()) map_.erase(it);
    }
    if (owner) {
        auto oit = owner_map_.find(owner);
        if (oit != owner_map_.end() && oit->second.ptr == inst)
            owner_map_.erase(oit);
    }
}

void PLScriptRegistry::register_polyglot(const std::string& path,
                                         godot::Object* owner,
                                         PolyglotInstance* inst) {
    if (!inst) return;
    std::unique_lock lk(mutex_);
    map_[path].push_back({PLScriptKind::Polyglot, inst});
    if (owner) owner_map_[owner] = {PLScriptKind::Polyglot, inst};
}

void PLScriptRegistry::unregister_polyglot(const std::string& path,
                                           godot::Object* owner,
                                           PolyglotInstance* inst) {
    std::unique_lock lk(mutex_);
    auto it = map_.find(path);
    if (it != map_.end()) {
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
            [inst](const PLScriptHandle& handle) {
                return handle.kind == PLScriptKind::Polyglot && handle.ptr == inst;
            }), vec.end());
        if (vec.empty()) map_.erase(it);
    }
    if (owner) {
        auto oit = owner_map_.find(owner);
        if (oit != owner_map_.end() && oit->second.ptr == inst)
            owner_map_.erase(oit);
    }
}

PolyLangScriptInstance* PLScriptRegistry::find_instance(const std::string& path) const {
    std::shared_lock lk(mutex_);
    auto it = map_.find(path);
    if (it == map_.end()) return nullptr;
    for (const auto& handle : it->second) {
        if (handle.kind == PLScriptKind::PolyLang)
            return static_cast<PolyLangScriptInstance*>(handle.ptr);
    }
    return nullptr;
}

std::vector<PolyLangScriptInstance*>
PLScriptRegistry::find_all(const std::string& path) const {
    std::shared_lock lk(mutex_);
    std::vector<PolyLangScriptInstance*> out;
    auto it = map_.find(path);
    if (it == map_.end()) return out;
    for (const auto& handle : it->second) {
        if (handle.kind == PLScriptKind::PolyLang)
            out.push_back(static_cast<PolyLangScriptInstance*>(handle.ptr));
    }
    return out;
}

PLScriptHandle PLScriptRegistry::find_handle(const std::string& path) const {
    std::shared_lock lk(mutex_);
    auto it = map_.find(path);
    if (it == map_.end() || it->second.empty()) return {};
    return it->second.front();
}

PLScriptHandle PLScriptRegistry::find_owner(godot::Object* owner) const {
    if (!owner) return {};
    std::shared_lock lk(mutex_);
    auto it = owner_map_.find(owner);
    return (it != owner_map_.end()) ? it->second : PLScriptHandle{};
}

namespace {

static int call_handle_direct(const PLScriptHandle& handle,
                              const char* method,
                              PLValue* args, int32_t argc,
                              PLValue* ret) {
    if (!handle.valid()) {
        pl_value_init(ret);
        return PL_ERR_GENERIC;
    }
    switch (handle.kind) {
        case PLScriptKind::PolyLang:
            return static_cast<PolyLangScriptInstance*>(handle.ptr)
                ->call_method_direct(method, args, argc, ret);
        case PLScriptKind::Polyglot:
            return static_cast<PolyglotInstance*>(handle.ptr)
                ->call_method_direct(method, args, argc, ret);
    }
    pl_value_init(ret);
    return PL_ERR_GENERIC;
}

static void free_handle_result(const PLScriptHandle& handle, PLValue* value) {
    if (!value) return;
    if (!handle.valid()) {
        VariantBridge::free_pl_value(*value);
        return;
    }
    switch (handle.kind) {
        case PLScriptKind::PolyLang: {
            const PLAdapterVTable* vt =
                static_cast<PolyLangScriptInstance*>(handle.ptr)->get_vtable();
            if (vt && vt->pl_free_value_contents) {
                vt->pl_free_value_contents(value);
                return;
            }
            break;
        }
        case PLScriptKind::Polyglot:
            static_cast<PolyglotInstance*>(handle.ptr)->free_value_contents(value);
            return;
    }
    VariantBridge::free_pl_value(*value);
}

} // namespace

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

    std::string spath   = path.utf8().get_data();
    std::string smethod = method.utf8().get_data();

    PLScriptHandle handle =
        PLScriptRegistry::get_singleton()->find_handle(spath);
    if (!handle.valid()) {
        ERR_PRINT(("[PolyLang/Bridge] No live instance for: " + spath).c_str());
        return godot::Variant();
    }

    int32_t argc = static_cast<int32_t>(args.size());
    std::vector<PLValue> pl_args(argc);
    for (int32_t i = 0; i < argc; ++i)
        VariantBridge::to_pl_value(args[i], pl_args[i]);

    PLValue ret{}; pl_value_init(&ret);
    int r = call_handle_direct(handle, smethod.c_str(),
                               pl_args.data(), argc, &ret);

    godot::Variant result;
    if (r == PL_OK) result = VariantBridge::from_pl_value(ret);

    for (auto& v : pl_args)
        VariantBridge::free_pl_value(v);
    free_handle_result(handle, &ret);

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

    PLScriptHandle handle =
        PLScriptRegistry::get_singleton()->find_handle(path);
    if (!handle.valid()) {
        pl_value_init(ret_out);
        return PL_ERR_GENERIC;
    }
    return call_handle_direct(handle, method.c_str(), args, argc, ret_out);
}

void PolyLangBridge::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("call_script", "path", "method", "args"),
        &PolyLangBridge::call_script);
}

} // namespace polylang
