// =============================================================
// polylang_wren_adapter.cpp  —  Wren Scripting Adapter v5
// =============================================================
// FIX P2-1:  pl_compile stores source text; wrenInterpret used directly.
// FIX NEW-6: WrenHandle* cached per (instance, signature), freed in free_instance.
// FIX NEW-17: Module name strips all extensions.
// SANDBOX:   pl_compile_sandboxed() creates an isolated WrenVM that has
//            no foreign class or method bindings for file/process/net.
//            The sandboxed write/error callbacks suppress output.
// =============================================================
#include <wren.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>

#include "../../include/pl_adapter_vtable.h"

// ── Value conversion ──────────────────────────────────────────
static void pl_to_wren_slot(WrenVM* vm, int slot, const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    wrenSetSlotNull(vm, slot);                     break;
        case PL_TYPE_BOOL:   wrenSetSlotBool(vm, slot, v.b);               break;
        case PL_TYPE_INT:    wrenSetSlotDouble(vm, slot, (double)v.i);     break;
        case PL_TYPE_FLOAT:  wrenSetSlotDouble(vm, slot, v.f);             break;
        case PL_TYPE_STRING: wrenSetSlotString(vm, slot, v.s ? v.s : ""); break;
        default:             wrenSetSlotNull(vm, slot);                     break;
    }
}

static void wren_slot_to_pl(WrenVM* vm, int slot, PLValue& out) {
    pl_value_init(&out);
    WrenType t = wrenGetSlotType(vm, slot);
    switch (t) {
        case WREN_TYPE_BOOL:   out.type = PL_TYPE_BOOL;  out.b = wrenGetSlotBool(vm, slot); break;
        case WREN_TYPE_NUM:    out.type = PL_TYPE_FLOAT; out.f = wrenGetSlotDouble(vm, slot); break;
        case WREN_TYPE_STRING: {
            const char* s = wrenGetSlotString(vm, slot);
            out.type = PL_TYPE_STRING; out.s = s ? strdup(s) : nullptr; break;
        }
        case WREN_TYPE_NULL:   out.type = PL_TYPE_NIL; break;
        default:               out.type = PL_TYPE_NIL; break;
    }
}

// ── Callbacks ─────────────────────────────────────────────────
static void wren_write_cb(WrenVM*, const char* text) { fputs(text, stderr); }
static void wren_error_cb(WrenVM*, WrenErrorType type,
                          const char* module, int line, const char* msg) {
    if (type == WREN_ERROR_COMPILE)
        fprintf(stderr, "[PolyLang/Wren] [%s:%d] Compile: %s\n", module, line, msg);
    else if (type == WREN_ERROR_RUNTIME)
        fprintf(stderr, "[PolyLang/Wren] Runtime: %s\n", msg);
    else
        fprintf(stderr, "[PolyLang/Wren] [%s:%d] %s\n", module, line, msg);
}
// Sandbox suppresses output entirely
static void wren_sandbox_write(WrenVM*, const char*) {}
static void wren_sandbox_error(WrenVM*, WrenErrorType, const char*, int, const char* msg) {
    fprintf(stderr, "[PolyLang/Wren/sandbox] Error: %s\n", msg ? msg : "?");
}

// Sandboxed foreign method resolver — blocks everything
static WrenForeignMethodFn wren_sandbox_foreign_method(
    WrenVM*, const char*, const char*, bool, const char*) {
    return nullptr;  // deny all foreign methods in sandbox
}

// ── Handles ───────────────────────────────────────────────────
struct WrenCompiled {
    WrenVM*     vm{nullptr};
    std::mutex  vm_mutex;
    std::string class_name;
    std::string module_name;
    bool        sandboxed{false};
};
struct WrenInstance {
    WrenCompiled* compiled{nullptr};
    WrenHandle*   recv{nullptr};
    std::unordered_map<std::string, WrenHandle*> call_cache;
};

// ── Core compile ──────────────────────────────────────────────
static void* wren_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source) return nullptr;

    // Derive module name: strip all extensions from basename
    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    // Strip ALL extensions (FIX NEW-17: "Enemy.pl.wren" → "Enemy")
    while (true) {
        auto dot = p.rfind('.');
        if (dot == std::string::npos) break;
        p = p.substr(0, dot);
    }

    WrenConfiguration cfg;
    wrenInitConfiguration(&cfg);
    cfg.writeFn = sandboxed ? wren_sandbox_write : wren_write_cb;
    cfg.errorFn = sandboxed ? wren_sandbox_error : wren_error_cb;
    if (sandboxed) {
        cfg.bindForeignMethodFn = wren_sandbox_foreign_method;
        cfg.bindForeignClassFn  = nullptr;
    }

    WrenVM* vm = wrenNewVM(&cfg);
    if (!vm) return nullptr;

    WrenInterpretResult result = wrenInterpret(vm, p.c_str(), source);
    if (result != WREN_RESULT_SUCCESS) {
        fprintf(stderr, "[PolyLang/Wren%s] Interpret failed: %s\n",
                sandboxed ? "/sandbox" : "", path ? path : "");
        wrenFreeVM(vm); return nullptr;
    }

    auto* c = new WrenCompiled();
    c->vm = vm; c->class_name = p; c->module_name = p; c->sandboxed = sandboxed;
    return c;
}

// ── ABI ───────────────────────────────────────────────────────
static int wren_init_runtime()    { return PL_OK; }
static void wren_shutdown_runtime() {}

static void* wren_compile_pl(const char* s, const char* p)             { return wren_compile_core(s, p, false); }
static void* wren_compile_sandboxed(const char* s, const char* p, uint32_t) { return wren_compile_core(s, p, true); }

static void wren_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<WrenCompiled*>(h);
    std::lock_guard<std::mutex> lk(c->vm_mutex);
    if (c->vm) wrenFreeVM(c->vm);
    delete c;
}

static void* wren_instantiate_class(void* ch, const char*) {
    auto* c = static_cast<WrenCompiled*>(ch); if (!c || !c->vm) return nullptr;
    std::lock_guard<std::mutex> lk(c->vm_mutex);

    wrenEnsureSlots(c->vm, 1);
    wrenGetVariable(c->vm, c->module_name.c_str(), c->class_name.c_str(), 0);
    WrenHandle* cls = wrenGetSlotHandle(c->vm, 0);
    if (!cls) return nullptr;

    WrenHandle* ctor_handle = wrenMakeCallHandle(c->vm, "new()");
    wrenEnsureSlots(c->vm, 1);
    wrenSetSlotHandle(c->vm, 0, cls);
    WrenInterpretResult r = wrenCall(c->vm, ctor_handle);
    wrenReleaseHandle(c->vm, ctor_handle);
    wrenReleaseHandle(c->vm, cls);

    if (r != WREN_RESULT_SUCCESS) return nullptr;

    WrenHandle* recv = wrenGetSlotHandle(c->vm, 0);
    auto* inst = new WrenInstance(); inst->compiled = c; inst->recv = recv;
    return inst;
}

static void wren_free_instance(void* raw) {
    if (!raw) return;
    auto* i = static_cast<WrenInstance*>(raw);
    if (i->compiled && i->compiled->vm) {
        std::lock_guard<std::mutex> lk(i->compiled->vm_mutex);
        for (auto& [k, h] : i->call_cache) wrenReleaseHandle(i->compiled->vm, h);
        if (i->recv) wrenReleaseHandle(i->compiled->vm, i->recv);
    }
    delete i;
}

static int wren_call_method(void* raw, const char* name, PLValue* args, int32_t argc, PLValue* ret) {
    auto* i = static_cast<WrenInstance*>(raw);
    if (!i || !i->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(i->compiled->vm_mutex);

    // Build Wren method signature: "methodName(_,_,...)"
    std::string sig = name;
    if (argc > 0) { sig += "(_"; for (int k = 1; k < argc; ++k) sig += ",_"; sig += ")"; }
    else          { sig += "()"; }

    WrenHandle* call_h = nullptr;
    auto it = i->call_cache.find(sig);
    if (it != i->call_cache.end()) {
        call_h = it->second;
    } else {
        call_h = wrenMakeCallHandle(i->compiled->vm, sig.c_str());
        i->call_cache[sig] = call_h;
    }

    wrenEnsureSlots(i->compiled->vm, argc + 1);
    wrenSetSlotHandle(i->compiled->vm, 0, i->recv);
    for (int32_t k = 0; k < argc; ++k) pl_to_wren_slot(i->compiled->vm, k + 1, args[k]);

    WrenInterpretResult r = wrenCall(i->compiled->vm, call_h);
    if (r != WREN_RESULT_SUCCESS) { ret->type = PL_TYPE_NIL; return PL_ERR_EXCEPTION; }

    wren_slot_to_pl(i->compiled->vm, 0, *ret);
    return PL_OK;
}

static int wren_call_builtin(void* raw, int32_t id, PLValue* args, int32_t argc, PLValue* ret) {
    const char* n = nullptr;
    switch (id) {
        case PL_METHOD_READY:           n="_ready"; break;
        case PL_METHOD_PROCESS:         n="_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: n="_physicsProcess"; break;
        case PL_METHOD_ENTER_TREE:      n="_enterTree"; break;
        case PL_METHOD_EXIT_TREE:       n="_exitTree"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return wren_call_method(raw, n, args, argc, ret);
}

static int wren_set_prop(void* raw, const char* name, const PLValue* v) {
    std::string setter = std::string(name) + "=(_)";
    PLValue dummy; pl_value_init(&dummy);
    PLValue arg = *v;
    return wren_call_method(raw, setter.c_str(), &arg, 1, &dummy);
}

static int wren_get_prop(void* raw, const char* name, PLValue* out) {
    PLValue dummy_arg; pl_value_init(&dummy_arg);
    return wren_call_method(raw, name, nullptr, 0, out);
}

static uint8_t wren_has_method(void*, const char*) { return 1; }

static void wren_free_val(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                               | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime      = wren_init_runtime;
    out->pl_shutdown_runtime  = wren_shutdown_runtime;
    out->pl_compile           = wren_compile_pl;
    out->pl_compile_sandboxed = wren_compile_sandboxed;
    out->pl_free_compiled     = wren_free_compiled;
    out->pl_instantiate_class = wren_instantiate_class;
    out->pl_free_instance     = wren_free_instance;
    out->pl_call_method       = wren_call_method;
    out->pl_call_builtin      = wren_call_builtin;
    out->pl_set_property      = wren_set_prop;
    out->pl_get_property      = wren_get_prop;
    out->pl_has_method        = wren_has_method;
    out->pl_free_value_contents = wren_free_val;
}
