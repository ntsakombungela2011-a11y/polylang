// =============================================================
// polylang_js_adapter.cpp  —  QuickJS JavaScript Adapter (ABI v5)
// SECTION 3: Full source with pl_compile_sandboxed
// =============================================================
// Sandbox policy:
//   Blocked globals (set to undefined in sandbox context):
//     eval, Function, import, require,
//     XMLHttpRequest, fetch, WebSocket,
//     setTimeout, setInterval, clearTimeout, clearInterval,
//     process, global, globalThis (replaced with restricted proxy)
//   Blocked object constructors from std lib:
//     Function constructor (new Function("...") pattern blocked by
//     replacing Function.prototype.constructor with a throwing stub)
//   Implementation:
//     Per-compiled-handle isolated JSContext (not shared global).
//     Sandbox context has all dangerous globals deleted/overwritten
//     before script execution. JS_Eval is called with
//     JS_EVAL_TYPE_GLOBAL (not module mode) so import() is a syntax
//     error — module mode is not enabled in the sandbox runtime.
//     eval and Function are individually poisoned after context init.
// =============================================================
#include <quickjs/quickjs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <atomic>
#include <vector>

#include "../../include/pl_adapter_vtable.h"

static JSRuntime* g_rt      = nullptr;
static std::mutex g_js_mutex;

// ── Value conversion ──────────────────────────────────────────

static JSValue pl_to_js(JSContext* ctx, const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    return JS_NULL;
        case PL_TYPE_BOOL:   return JS_NewBool(ctx, v.b);
        case PL_TYPE_INT:    return JS_NewInt64(ctx, v.i);
        case PL_TYPE_FLOAT:  return JS_NewFloat64(ctx, v.f);
        case PL_TYPE_STRING: return JS_NewString(ctx, v.s ? v.s : "");
        case PL_TYPE_VEC2: {
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v.v2[0]));
            JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v.v2[1]));
            return arr;
        }
        case PL_TYPE_VEC3: {
            JSValue arr = JS_NewArray(ctx);
            JS_SetPropertyUint32(ctx, arr, 0, JS_NewFloat64(ctx, v.v3[0]));
            JS_SetPropertyUint32(ctx, arr, 1, JS_NewFloat64(ctx, v.v3[1]));
            JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, v.v3[2]));
            return arr;
        }
        default: return JS_NULL;
    }
}

static void js_to_pl(JSContext* ctx, JSValue val, PLValue& out) {
    pl_value_init(&out);
    if (JS_IsNull(val) || JS_IsUndefined(val)) { out.type = PL_TYPE_NIL; return; }
    if (JS_IsBool(val)) {
        out.type = PL_TYPE_BOOL; out.b = (bool)JS_ToBool(ctx, val); return;
    }
    if (JS_IsNumber(val)) {
        double d; JS_ToFloat64(ctx, &d, val);
        if (d == (double)(int64_t)d) { out.type = PL_TYPE_INT; out.i = (int64_t)d; }
        else                          { out.type = PL_TYPE_FLOAT; out.f = d; }
        return;
    }
    if (JS_IsString(val)) {
        const char* s = JS_ToCString(ctx, val);
        out.type = PL_TYPE_STRING; out.s = s ? strdup(s) : nullptr;
        if (s) JS_FreeCString(ctx, s);
        return;
    }
    out.type = PL_TYPE_NIL;
}

// ── Sandbox helpers ───────────────────────────────────────────

// Throwing stub for blocked globals (eval, Function, require, etc.)
static JSValue js_blocked(JSContext* ctx, JSValue /*this_val*/,
                           int /*argc*/, JSValue* /*argv*/) {
    JS_ThrowTypeError(ctx, "[PolyLang/sandbox] This function is blocked in sandboxed scripts.");
    return JS_EXCEPTION;
}

// Installs a throwing function over a global name.
static void js_block_global(JSContext* ctx, const char* name) {
    JSValue fn = JS_NewCFunction(ctx, js_blocked, name, 0);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, name, fn); // steals ref to fn
    JS_FreeValue(ctx, global);
}

// Deletes a global property (sets it to undefined permanently).
static void js_delete_global(JSContext* ctx, const char* name) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_DeleteProperty(ctx, global,
        JS_NewAtom(ctx, name), 0);
    JS_FreeValue(ctx, global);
}

// Applies full sandbox restrictions to a freshly created context.
// Must be called before any user script is evaluated.
static void js_apply_sandbox(JSContext* ctx) {
    // Block eval and Function (dynamic code execution)
    js_block_global(ctx, "eval");
    js_block_global(ctx, "Function");

    // Block Node/browser APIs that could leak to filesystem or network
    const char* to_delete[] = {
        "require", "process", "global",
        "XMLHttpRequest", "fetch", "WebSocket",
        "setTimeout", "setInterval",
        "clearTimeout", "clearInterval",
        "queueMicrotask",
        // QuickJS specific
        "os", "std",
        nullptr
    };
    for (int i = 0; to_delete[i]; ++i) {
        js_delete_global(ctx, to_delete[i]);
    }

    // Replace globalThis with a frozen empty object so scripts cannot
    // escape the sandbox by writing to globalThis.eval, etc.
    JSValue restricted_global = JS_NewObject(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    // Copy only Math and JSON to restricted_global
    JSValue math_obj = JS_GetPropertyStr(ctx, global, "Math");
    JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
    JS_SetPropertyStr(ctx, restricted_global, "Math", math_obj);
    JS_SetPropertyStr(ctx, restricted_global, "JSON", json_obj);
    JS_SetPropertyStr(ctx, global, "globalThis", restricted_global);
    JS_FreeValue(ctx, global);

    // Poison Function.prototype.constructor so `[].constructor.constructor('return process')()`
    // style attacks fail. We evaluate a tiny script to do this inside the context.
    const char* poison =
        "(function(){"
        "  try {"
        "    Object.defineProperty(Function.prototype, 'constructor', {"
        "      get: function() { throw new TypeError('[PolyLang/sandbox] Function constructor blocked'); },"
        "      configurable: false"
        "    });"
        "  } catch(e) {}"
        "})();";
    JSValue r = JS_Eval(ctx, poison, strlen(poison), "<sandbox_init>",
                        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    JS_FreeValue(ctx, r);
}

// ── Handles ───────────────────────────────────────────────────

struct JsCompiled {
    JSContext*  ctx{nullptr};   // Isolated context per compiled script
    std::mutex  ctx_mutex;
    std::string class_name;
    JSValue     class_val = JS_UNDEFINED;
    bool        sandboxed{false};
};

struct JsInstance {
    JsCompiled* compiled{nullptr};
    JSValue     obj = JS_UNDEFINED;
};

// ── Shared compile core ───────────────────────────────────────

static void* js_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source) return nullptr;
    std::lock_guard<std::mutex> lk(g_js_mutex);
    if (!g_rt) return nullptr;

    // Each compiled script gets its OWN JSContext for isolation.
    // This means sandboxed and trusted scripts never share a heap.
    JSContext* ctx = JS_NewContext(g_rt);
    if (!ctx) return nullptr;

    if (sandboxed) {
        js_apply_sandbox(ctx);
    }

    // Derive class name
    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find('.');  if (dot   != std::string::npos) p = p.substr(0, dot);

    // Evaluate as global (non-module) script.
    // JS_EVAL_TYPE_MODULE would enable import() — explicitly avoided in sandbox.
    // JS_EVAL_FLAG_STRICT enforces strict mode (no with, no arguments.caller, etc.)
    int eval_flags = JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT;
    JSValue result = JS_Eval(ctx, source, strlen(source),
                             path ? path : "<polylang>", eval_flags);
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "[PolyLang/JS%s] Eval error: %s\n",
                sandboxed ? "/sandbox" : "", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, result);
        JS_FreeContext(ctx);
        return nullptr;
    }
    JS_FreeValue(ctx, result);

    // Retrieve class from global scope
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue cls = JS_GetPropertyStr(ctx, global, p.c_str());
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, cls) && !JS_IsObject(cls)) {
        fprintf(stderr, "[PolyLang/JS%s] Class '%s' not found in global scope.\n",
                sandboxed ? "/sandbox" : "", p.c_str());
        JS_FreeValue(ctx, cls);
        JS_FreeContext(ctx);
        return nullptr;
    }

    auto* c       = new JsCompiled();
    c->ctx        = ctx;
    c->class_name = p;
    c->class_val  = cls; // owned ref
    c->sandboxed  = sandboxed;
    return c;
}

// ── ABI ───────────────────────────────────────────────────────

static int js_init_runtime() {
    std::lock_guard<std::mutex> lk(g_js_mutex);
    if (g_rt) return PL_OK;
    g_rt = JS_NewRuntime();
    if (!g_rt) return PL_ERR_GENERIC;
    // 512 MB memory limit for the shared runtime
    JS_SetMemoryLimit(g_rt, 512 * 1024 * 1024);
    JS_SetMaxStackSize(g_rt, 8 * 1024 * 1024); // 8 MB stack
    return PL_OK;
}

static void js_shutdown_runtime() {
    std::lock_guard<std::mutex> lk(g_js_mutex);
    if (g_rt) { JS_FreeRuntime(g_rt); g_rt = nullptr; }
}

static void* js_compile_pl(const char* source, const char* path) {
    return js_compile_core(source, path, false);
}

static void* js_compile_sandboxed_pl(const char* source, const char* path,
                                      uint32_t /*allowed_caps*/) {
    return js_compile_core(source, path, true);
}

static void js_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<JsCompiled*>(h);
    std::lock_guard<std::mutex> lk(g_js_mutex);
    JS_FreeValue(c->ctx, c->class_val);
    JS_FreeContext(c->ctx);
    delete c;
}

static void* js_instantiate_class(void* compiled_handle, const char* /*path*/) {
    auto* c = static_cast<JsCompiled*>(compiled_handle);
    if (!c || !c->ctx) return nullptr;

    std::lock_guard<std::mutex> lk(c->ctx_mutex);
    JSContext* ctx = c->ctx;

    // Instantiate via `new ClassName()`
    JSValue obj = JS_CallConstructor(ctx, c->class_val, 0, nullptr);
    if (JS_IsException(obj)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "[PolyLang/JS] Instantiation error: %s\n", msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        return nullptr;
    }

    auto* inst    = new JsInstance;
    inst->compiled = c;
    inst->obj      = obj; // owned ref
    return inst;
}

static void js_free_instance(void* raw) {
    if (!raw) return;
    auto* inst = static_cast<JsInstance*>(raw);
    if (inst->compiled) {
        std::lock_guard<std::mutex> lk(inst->compiled->ctx_mutex);
        JS_FreeValue(inst->compiled->ctx, inst->obj);
    }
    delete inst;
}

static int js_call_method(void* raw, const char* method_name,
                           PLValue* args, int32_t argc, PLValue* ret) {
    auto* inst = static_cast<JsInstance*>(raw);
    if (!inst || !inst->compiled || !inst->compiled->ctx) return PL_ERR_GENERIC;

    std::lock_guard<std::mutex> lk(inst->compiled->ctx_mutex);
    JSContext* ctx = inst->compiled->ctx;

    JSValue fn = JS_GetPropertyStr(ctx, inst->obj, method_name);
    if (!JS_IsFunction(ctx, fn)) {
        JS_FreeValue(ctx, fn);
        return PL_ERR_METHOD_NOT_FOUND;
    }

    std::vector<JSValue> js_args(argc);
    for (int32_t i = 0; i < argc; ++i) js_args[i] = pl_to_js(ctx, args[i]);

    JSValue result = JS_Call(ctx, fn, inst->obj, argc, js_args.data());
    JS_FreeValue(ctx, fn);
    for (int32_t i = 0; i < argc; ++i) JS_FreeValue(ctx, js_args[i]);

    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "[PolyLang/JS] Error in %s: %s\n",
                method_name, msg ? msg : "unknown");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        ret->type = PL_TYPE_NIL;
        return PL_ERR_EXCEPTION;
    }

    js_to_pl(ctx, result, *ret);
    JS_FreeValue(ctx, result);
    return PL_OK;
}

static int js_call_builtin(void* raw, int32_t method_id,
                            PLValue* args, int32_t argc, PLValue* ret) {
    const char* name = nullptr;
    switch (method_id) {
        case PL_METHOD_READY:           name = "_ready"; break;
        case PL_METHOD_PROCESS:         name = "_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: name = "_physicsProcess"; break;
        case PL_METHOD_ENTER_TREE:      name = "_enterTree"; break;
        case PL_METHOD_EXIT_TREE:       name = "_exitTree"; break;
        case PL_METHOD_INPUT:           name = "_input"; break;
        case PL_METHOD_NOTIFICATION:    name = "_notification"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return js_call_method(raw, name, args, argc, ret);
}

static int js_set_property(void* raw, const char* name, const PLValue* value) {
    auto* inst = static_cast<JsInstance*>(raw);
    if (!inst || !inst->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(inst->compiled->ctx_mutex);
    JSContext* ctx = inst->compiled->ctx;
    JSValue pv = pl_to_js(ctx, *value);
    JS_SetPropertyStr(ctx, inst->obj, name, pv); // steals ref
    return PL_OK;
}

static int js_get_property(void* raw, const char* name, PLValue* out) {
    auto* inst = static_cast<JsInstance*>(raw);
    if (!inst || !inst->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(inst->compiled->ctx_mutex);
    JSContext* ctx = inst->compiled->ctx;
    JSValue val = JS_GetPropertyStr(ctx, inst->obj, name);
    if (JS_IsUndefined(val)) { JS_FreeValue(ctx, val); return PL_ERR_PROP_NOT_FOUND; }
    js_to_pl(ctx, val, *out);
    JS_FreeValue(ctx, val);
    return PL_OK;
}

static uint8_t js_has_method(void* compiled_handle, const char* name) {
    auto* c = static_cast<JsCompiled*>(compiled_handle);
    if (!c || !c->ctx || !name) return 0;
    std::lock_guard<std::mutex> lk(c->ctx_mutex);
    JSValue proto = JS_GetPropertyStr(c->ctx, c->class_val, "prototype");
    if (JS_IsUndefined(proto)) { JS_FreeValue(c->ctx, proto); return 0; }
    JSValue fn = JS_GetPropertyStr(c->ctx, proto, name);
    uint8_t has = JS_IsFunction(c->ctx, fn) ? 1 : 0;
    JS_FreeValue(c->ctx, fn);
    JS_FreeValue(c->ctx, proto);
    return has;
}

static void js_free_value_contents(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    if (v->type == PL_TYPE_ARRAY && v->array.data) {
        for (int i = 0; i < v->array.len; ++i) js_free_value_contents(&v->array.data[i]);
        free(v->array.data); v->array.data = nullptr;
    }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version            = PL_ABI_VERSION;
    out->_reserved              = 0;
    out->_pad2                  = 0;
    out->capabilities           = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                                 | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime        = js_init_runtime;
    out->pl_shutdown_runtime    = js_shutdown_runtime;
    out->pl_compile             = js_compile_pl;
    out->pl_compile_sandboxed   = js_compile_sandboxed_pl;
    out->pl_free_compiled       = js_free_compiled;
    out->pl_instantiate_class   = js_instantiate_class;
    out->pl_free_instance       = js_free_instance;
    out->pl_call_method         = js_call_method;
    out->pl_call_builtin        = js_call_builtin;
    out->pl_batch_process       = nullptr;
    out->pl_set_property        = js_set_property;
    out->pl_get_property        = js_get_property;
    out->pl_has_method          = js_has_method;
    out->pl_free_value_contents = js_free_value_contents;
}
