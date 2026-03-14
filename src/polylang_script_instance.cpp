// =============================================================
// polylang_script_instance.cpp  —  PolyLang v6.7
//
// FIX VLN-13 [CRITICAL]: hot_swap() deadlock via re-entrant lock.
//   BEFORE: hot_swap() held unique_lock(instance_lock_) while calling
//     StateTransfer::save(old, vtable_, script_). That function calls
//     vtable->pl_get_property() — foreign code. If that foreign code
//     calls back into PolyLang (e.g. a Lua getter that reads a property
//     from another PolyLang script via the bridge), call_method_direct()
//     → call_method() tries to acquire shared_lock(instance_lock_).
//     A unique_lock blocks all shared_lock acquirers → DEADLOCK.
//   AFTER:
//     1. Snapshot `old` and `vtable_` under a brief shared_lock.
//     2. Release the lock entirely.
//     3. Call StateTransfer::save() OUTSIDE any lock.
//     4. Re-acquire unique_lock only for the atomic swap itself.
//     5. Call StateTransfer::restore() OUTSIDE the lock (same reason).
//   The window between step 2 and 4 is safe: `old` is a raw pointer
//   owned by this instance; no other thread can free it without first
//   acquiring unique_lock (which only hot_swap holds, exclusively).
// =============================================================
#include "polylang_script_instance.hpp"
#include "polylang_script.hpp"
#include "polylang_language.hpp"
#include "runtime_manager.hpp"
#include "variant_bridge.hpp"
#include "pl_state_transfer.hpp"
#include "pl_bridge.hpp"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <cstring>
#include <vector>

namespace polylang {

// ── Thread-local method ID cache ─────────────────────────────
namespace {
struct MethodCacheEntry {
    uint64_t hash{0};
    int32_t  method_id{0};
};
constexpr int CACHE_SIZE = 64;
} // namespace

static thread_local std::array<MethodCacheEntry, CACHE_SIZE> tl_method_cache{};

struct BuiltinEntry { const char* name; int32_t id; };
static const BuiltinEntry k_builtins[] = {
    { "_ready",            PL_METHOD_READY           },
    { "_process",          PL_METHOD_PROCESS         },
    { "_physics_process",  PL_METHOD_PHYSICS_PROCESS },
    { "_enter_tree",       PL_METHOD_ENTER_TREE      },
    { "_exit_tree",        PL_METHOD_EXIT_TREE       },
    { "_input",            PL_METHOD_INPUT           },
    { "_unhandled_input",  PL_METHOD_UNHANDLED_INPUT },
    { "_notification",     PL_METHOD_NOTIFICATION    },
};

/*static*/ int32_t PolyLangScriptInstance::cached_builtin_id(
        const godot::StringName& name) {
    uint64_t h = name.hash();
    if (h == 0) h = 1;
    int slot = (int)(h & (CACHE_SIZE - 1));
    MethodCacheEntry& entry = tl_method_cache[slot];
    if (entry.hash == h) return entry.method_id;
    int32_t id = 0;
    godot::String n = name;
    for (const auto& be : k_builtins) {
        if (n == be.name) { id = be.id; break; }
    }
    entry.hash      = h;
    entry.method_id = id;
    return id;
}

// ── Constructor / Destructor ──────────────────────────────────

PolyLangScriptInstance::PolyLangScriptInstance(
        PolyLangScript* script, godot::Object* owner, void* foreign_instance)
    : script_(script)
    , owner_(owner)
    , foreign_instance_(foreign_instance)
    , vtable_(script ? script->get_vtable() : nullptr)
    , language_id_(script ? script->get_language_id() : LanguageID::COUNT)
{
    if (script_ && PLScriptRegistry::get_singleton()) {
        std::string path = script_->get_path().utf8().get_data();
        PLScriptRegistry::get_singleton()->register_instance(path, this);
    }
}

PolyLangScriptInstance::~PolyLangScriptInstance() {
    if (script_ && PLScriptRegistry::get_singleton()) {
        std::string path = script_->get_path().utf8().get_data();
        PLScriptRegistry::get_singleton()->unregister_instance(path, this);
    }
    void* fi = foreign_instance_.exchange(nullptr, std::memory_order_acq_rel);
    if (fi && vtable_ && vtable_->pl_free_instance)
        vtable_->pl_free_instance(fi);
}

// ── Hot-swap ──────────────────────────────────────────────────
// FIX VLN-13: StateTransfer::save/restore called OUTSIDE instance_lock_.

void PolyLangScriptInstance::hot_swap(void* new_foreign) {
    if (!new_foreign) {
        ERR_PRINT("[PolyLang] hot_swap called with nullptr — BUG in scheduler.");
        return;
    }

    // Step 1: Snapshot current state pointers under shared_lock.
    void* old      = nullptr;
    const PLAdapterVTable* vt = nullptr;
    {
        std::shared_lock lk(instance_lock_);
        old = foreign_instance_.load(std::memory_order_acquire);
        vt  = vtable_;
    }
    // instance_lock_ is now RELEASED — safe for foreign code to re-enter.

    // Step 2: Save state outside any lock.
    StateSnapshot snap;
    if (old && vt && script_) {
        snap = StateTransfer::save(old, vt, script_);
    }

    // Step 3: Swap foreign instance under unique_lock.
    {
        std::unique_lock lk(instance_lock_);
        foreign_instance_.store(new_foreign, std::memory_order_release);
        if (old && vt && vt->pl_free_instance)
            vt->pl_free_instance(old);
        if (script_) vtable_ = script_->get_vtable();
    }
    // unique_lock released.

    // Step 4: Restore state outside any lock.
    if (!snap.empty() && script_) {
        const PLAdapterVTable* new_vt = nullptr;
        {
            std::shared_lock lk(instance_lock_);
            new_vt = vtable_;
        }
        StateTransfer::restore(snap, new_foreign, new_vt, script_);
    }
}

// ── Call dispatch ─────────────────────────────────────────────

int PolyLangScriptInstance::call_builtin(int32_t method_id,
                                          PLValue* args, int32_t argc,
                                          PLValue* ret) {
    if (!vtable_ || !vtable_->pl_call_builtin)
        return PL_ERR_NOT_IMPLEMENTED;
    std::shared_lock lk(instance_lock_);
    void* fi = foreign_instance_.load(std::memory_order_acquire);
    if (!fi) { ret->type = PL_TYPE_NIL; return PL_ERR_GENERIC; }
    return vtable_->pl_call_builtin(fi, method_id, args, argc, ret);
}

int PolyLangScriptInstance::call_method(const char* name,
                                         PLValue* args, int32_t argc,
                                         PLValue* ret) {
    if (!vtable_ || !vtable_->pl_call_method) {
        ret->type = PL_TYPE_NIL; return PL_ERR_GENERIC;
    }
    std::shared_lock lk(instance_lock_);
    void* fi = foreign_instance_.load(std::memory_order_acquire);
    if (!fi) { ret->type = PL_TYPE_NIL; return PL_ERR_GENERIC; }
    return vtable_->pl_call_method(fi, name, args, argc, ret);
}

int PolyLangScriptInstance::call_method_direct(const char* name,
                                                PLValue* args, int32_t argc,
                                                PLValue* ret) {
    return call_method(name, args, argc, ret);
}

// ── Trampolines ───────────────────────────────────────────────

void* PolyLangScriptInstance::_create_trampoline(void* p_userdata) {
    return p_userdata;
}

void PolyLangScriptInstance::_free_trampoline(
        void*, GDExtensionScriptInstanceDataPtr p_ri) {
    auto* inst = static_cast<PolyLangScriptInstance*>(p_ri);
    if (!inst) return;
    if (inst->script_) inst->script_->unregister_instance(inst);
    delete inst;
}

GDExtensionBool PolyLangScriptInstance::_set_trampoline(
        GDExtensionScriptInstanceDataPtr p_ri,
        GDExtensionConstStringNamePtr    p_name,
        GDExtensionConstVariantPtr       p_value) {
    auto* inst = static_cast<PolyLangScriptInstance*>(p_ri);
    if (!inst || !inst->vtable_ || !inst->vtable_->pl_set_property) return false;
    const godot::StringName& nm = *reinterpret_cast<const godot::StringName*>(p_name);
    const godot::Variant&    vr = *reinterpret_cast<const godot::Variant*>(p_value);
    PLValue pv; pl_value_init(&pv);
    VariantBridge::to_pl_value(vr, pv);
    std::shared_lock lk(inst->instance_lock_);
    void* fi = inst->foreign_instance_.load(std::memory_order_acquire);
    if (!fi) { VariantBridge::free_pl_value(pv); return false; }
    int r = inst->vtable_->pl_set_property(fi, nm.utf8().get_data(), &pv);
    VariantBridge::free_pl_value(pv);
    return (r == PL_OK) ? 1 : 0;
}

GDExtensionBool PolyLangScriptInstance::_get_trampoline(
        GDExtensionScriptInstanceDataPtr p_ri,
        GDExtensionConstStringNamePtr    p_name,
        GDExtensionVariantPtr            r_ret) {
    auto* inst = static_cast<PolyLangScriptInstance*>(p_ri);
    if (!inst || !inst->vtable_ || !inst->vtable_->pl_get_property) return false;
    const godot::StringName& nm = *reinterpret_cast<const godot::StringName*>(p_name);
    godot::Variant& out = *reinterpret_cast<godot::Variant*>(r_ret);
    PLValue pv; pl_value_init(&pv);
    std::shared_lock lk(inst->instance_lock_);
    void* fi = inst->foreign_instance_.load(std::memory_order_acquire);
    if (!fi) return false;
    int r = inst->vtable_->pl_get_property(fi, nm.utf8().get_data(), &pv);
    if (r != PL_OK) return false;
    out = VariantBridge::from_pl_value(pv);
    VariantBridge::free_pl_value(pv);
    return 1;
}

void PolyLangScriptInstance::_call_trampoline(
        GDExtensionScriptInstanceDataPtr    p_ri,
        GDExtensionConstStringNamePtr       p_method,
        const GDExtensionConstVariantPtr*   p_args,
        GDExtensionInt                      p_argcount,
        GDExtensionVariantPtr               r_return,
        GDExtensionCallError*               r_error) {
    auto* inst = static_cast<PolyLangScriptInstance*>(p_ri);
    godot::Variant& ret = *reinterpret_cast<godot::Variant*>(r_return);
    ret = godot::Variant();
    if (!inst || !inst->vtable_) {
        r_error->error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
        return;
    }
    const godot::StringName& method_sn =
        *reinterpret_cast<const godot::StringName*>(p_method);
    const int32_t argc = static_cast<int32_t>(p_argcount);
    std::vector<PLValue> args_buf(argc);
    for (int32_t i = 0; i < argc; ++i) {
        const godot::Variant& av = *reinterpret_cast<const godot::Variant*>(p_args[i]);
        VariantBridge::to_pl_value(av, args_buf[i]);
    }
    PLValue result; pl_value_init(&result);
    int rc = PL_ERR_METHOD_NOT_FOUND;
    int32_t bid = cached_builtin_id(method_sn);
    if (bid != 0 && (inst->vtable_->capabilities & PL_CAP_BUILTIN_CALL)) {
        rc = inst->call_builtin(bid, args_buf.data(), argc, &result);
        if (rc == PL_ERR_NOT_IMPLEMENTED) {
            std::string mname = method_sn.utf8().get_data();
            rc = inst->call_method(mname.c_str(), args_buf.data(), argc, &result);
        }
    } else {
        std::string mname = method_sn.utf8().get_data();
        rc = inst->call_method(mname.c_str(), args_buf.data(), argc, &result);
    }
    for (auto& pv : args_buf) VariantBridge::free_pl_value(pv);
    if (rc == PL_OK || rc >= 0) {
        ret = VariantBridge::from_pl_value(result);
        r_error->error = GDEXTENSION_CALL_OK;
    } else if (rc == PL_ERR_METHOD_NOT_FOUND) {
        r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    } else {
        r_error->error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
    }
    VariantBridge::free_pl_value(result);
}

GDExtensionBool PolyLangScriptInstance::_has_method_trampoline(
        GDExtensionScriptInstanceDataPtr p_ri,
        GDExtensionConstStringNamePtr    p_method) {
    auto* inst = static_cast<PolyLangScriptInstance*>(p_ri);
    if (!inst || !inst->script_) return false;
    const godot::StringName& nm = *reinterpret_cast<const godot::StringName*>(p_method);
    return inst->script_->_has_method(nm) ? 1 : 0;
}

GDExtensionObjectPtr PolyLangScriptInstance::_get_owner_trampoline(
        GDExtensionScriptInstanceDataPtr p_ri) {
    auto* inst = static_cast<PolyLangScriptInstance*>(p_ri);
    if (!inst || !inst->owner_) return nullptr;
    return inst->owner_->get_instance_id() != 0
        ? reinterpret_cast<GDExtensionObjectPtr>(inst->owner_)
        : nullptr;
}

GDExtensionScriptInstancePtr PolyLangScriptInstance::create_godot_instance(
        PolyLangScript* script, godot::Object* owner, void* foreign) {
    auto* inst = new PolyLangScriptInstance(script, owner, foreign);
    GDExtensionScriptInstanceInfo3 info{};
    info.set_func             = _set_trampoline;
    info.get_func             = _get_trampoline;
    info.has_method_func      = _has_method_trampoline;
    info.call_func            = _call_trampoline;
    info.free_func            = _free_trampoline;
    info.get_owner_func       = _get_owner_trampoline;
    info.get_property_list_func  = [](auto, uint32_t* r) -> const GDExtensionPropertyInfo* { *r=0; return nullptr; };
    info.free_property_list_func = [](auto, auto, uint32_t) {};
    info.get_method_list_func    = [](auto, uint32_t* r) -> const GDExtensionMethodInfo* { *r=0; return nullptr; };
    info.free_method_list_func   = [](auto, auto, uint32_t) {};
    return godot::internal::gdextension_interface_script_instance_create3(&info, inst);
}

} // namespace polylang
