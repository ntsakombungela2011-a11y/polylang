// =============================================================
// polylang_ts_adapter.cpp  —  TypeScript (QuickJS) Adapter v5
// =============================================================
// Sandbox: same isolated-JSContext strategy as JS adapter.
// Each compiled script owns its own JSContext. Sandboxed contexts
// have eval/Function/require/process/os/std deleted/blocked before
// script execution. JS_EVAL_TYPE_GLOBAL prevents import() syntax.
// =============================================================
#include <quickjs/quickjs.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <regex>
#include <vector>

#include "../../include/pl_adapter_vtable.h"

static JSRuntime* g_rt = nullptr;
static std::mutex g_ts_mutex;

// ── Type stripping (FIX P5-14) ────────────────────────────────
static std::string strip_typescript(const std::string& ts) {
    std::string s = ts;
    { std::regex r(R"(interface\s+\w+\s*\{[^}]*\})", std::regex::ECMAScript);
      s = std::regex_replace(s, r, ""); }
    { std::regex r(R"(type\s+\w+\s*=[^;]+;)", std::regex::ECMAScript);
      s = std::regex_replace(s, r, ""); }
    { std::regex r(R"(\)\s*:\s*[\w\[\]|<>, ]+\s*\{)", std::regex::ECMAScript);
      s = std::regex_replace(s, r, ") {"); }
    { std::regex r(R"(:\s*[\w\[\]|<>.? ]+(?=[,)]|$))", std::regex::ECMAScript);
      s = std::regex_replace(s, r, ""); }
    { std::regex r(R"(<[^>{}()]+>)", std::regex::ECMAScript);
      s = std::regex_replace(s, r, ""); }
    { std::regex r(R"(\b(public|private|protected|readonly)\s+)", std::regex::ECMAScript);
      s = std::regex_replace(s, r, ""); }
    return s;
}

// ── Value conversion ──────────────────────────────────────────
static JSValue pl_to_js(JSContext* ctx, const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    return JS_NULL;
        case PL_TYPE_BOOL:   return JS_NewBool(ctx, v.b);
        case PL_TYPE_INT:    return JS_NewInt64(ctx, v.i);
        case PL_TYPE_FLOAT:  return JS_NewFloat64(ctx, v.f);
        case PL_TYPE_STRING: return JS_NewString(ctx, v.s ? v.s : "");
        default:             return JS_NULL;
    }
}
static void js_to_pl(JSContext* ctx, JSValue val, PLValue& out) {
    pl_value_init(&out);
    if (JS_IsNull(val) || JS_IsUndefined(val)) { out.type = PL_TYPE_NIL; return; }
    if (JS_IsBool(val))   { out.type = PL_TYPE_BOOL;  out.b = (bool)JS_ToBool(ctx, val); return; }
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

// ── Sandbox (shared with JS adapter strategy) ─────────────────
static JSValue ts_blocked(JSContext* ctx, JSValue, int, JSValue*) {
    JS_ThrowTypeError(ctx, "[PolyLang/TS/sandbox] blocked in sandboxed scripts.");
    return JS_EXCEPTION;
}
static void ts_block_global(JSContext* ctx, const char* name) {
    JSValue fn = JS_NewCFunction(ctx, ts_blocked, name, 0);
    JSValue g  = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, name, fn);
    JS_FreeValue(ctx, g);
}
static void ts_delete_global(JSContext* ctx, const char* name) {
    JSValue g = JS_GetGlobalObject(ctx);
    JS_DeleteProperty(ctx, g, JS_NewAtom(ctx, name), 0);
    JS_FreeValue(ctx, g);
}
static void ts_apply_sandbox(JSContext* ctx) {
    ts_block_global(ctx, "eval");
    ts_block_global(ctx, "Function");
    const char* del[] = { "require","process","global","os","std",
                          "XMLHttpRequest","fetch","WebSocket",
                          "setTimeout","setInterval","clearTimeout","clearInterval", nullptr };
    for (int i = 0; del[i]; ++i) ts_delete_global(ctx, del[i]);
    const char* poison =
        "(function(){"
        "  try { Object.defineProperty(Function.prototype,'constructor',{"
        "    get:function(){ throw new TypeError('[PolyLang/sandbox] blocked'); },"
        "    configurable:false"
        "  }); } catch(e){}"
        "})();";
    JSValue r = JS_Eval(ctx, poison, strlen(poison), "<sandbox_init>",
                        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    JS_FreeValue(ctx, r);
}

// ── Handles ───────────────────────────────────────────────────
struct TsCompiled {
    JSContext*  ctx{nullptr};
    std::mutex  ctx_mutex;
    std::string class_name;
    JSValue     class_val{JS_UNDEFINED};
    bool        sandboxed{false};
};
struct TsInstance {
    TsCompiled* compiled{nullptr};
    JSValue     obj{JS_UNDEFINED};
};

// ── Core compile ──────────────────────────────────────────────
static void* ts_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source) return nullptr;
    std::lock_guard<std::mutex> lk(g_ts_mutex);
    if (!g_rt) return nullptr;

    JSContext* ctx = JS_NewContext(g_rt);
    if (!ctx) return nullptr;
    if (sandboxed) ts_apply_sandbox(ctx);

    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find('.');  if (dot   != std::string::npos) p = p.substr(0, dot);

    std::string stripped = strip_typescript(source);
    JSValue result = JS_Eval(ctx, stripped.c_str(), stripped.size(),
                             path ? path : "<polylang/ts>",
                             JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx); const char* m = JS_ToCString(ctx, ex);
        fprintf(stderr, "[PolyLang/TS%s] Eval error: %s\n", sandboxed?"/sandbox":"", m?m:"?");
        if (m) JS_FreeCString(ctx, m); JS_FreeValue(ctx, ex); JS_FreeValue(ctx, result);
        JS_FreeContext(ctx); return nullptr;
    }
    JS_FreeValue(ctx, result);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue cls    = JS_GetPropertyStr(ctx, global, p.c_str());
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, cls) && !JS_IsObject(cls)) {
        fprintf(stderr, "[PolyLang/TS%s] Class '%s' not found\n", sandboxed?"/sandbox":"", p.c_str());
        JS_FreeValue(ctx, cls); JS_FreeContext(ctx); return nullptr;
    }

    auto* c = new TsCompiled(); c->ctx = ctx; c->class_name = p;
    c->class_val = cls; c->sandboxed = sandboxed;
    return c;
}

// ── ABI ───────────────────────────────────────────────────────
static int ts_init_runtime() {
    std::lock_guard<std::mutex> lk(g_ts_mutex);
    if (g_rt) return PL_OK;
    g_rt = JS_NewRuntime();
    if (!g_rt) return PL_ERR_GENERIC;
    JS_SetMemoryLimit(g_rt, 512 * 1024 * 1024);
    JS_SetMaxStackSize(g_rt, 8 * 1024 * 1024);
    return PL_OK;
}
static void ts_shutdown_runtime() {
    std::lock_guard<std::mutex> lk(g_ts_mutex);
    if (g_rt) { JS_FreeRuntime(g_rt); g_rt = nullptr; }
}
static void* ts_compile_pl(const char* s, const char* p)    { return ts_compile_core(s, p, false); }
static void* ts_compile_sandboxed(const char* s, const char* p, uint32_t) { return ts_compile_core(s, p, true); }

static void ts_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<TsCompiled*>(h);
    std::lock_guard<std::mutex> lk(g_ts_mutex);
    JS_FreeValue(c->ctx, c->class_val); JS_FreeContext(c->ctx); delete c;
}
static void* ts_instantiate_class(void* ch, const char*) {
    auto* c = static_cast<TsCompiled*>(ch);
    if (!c) return nullptr;
    std::lock_guard<std::mutex> lk(c->ctx_mutex);
    JSValue obj = JS_CallConstructor(c->ctx, c->class_val, 0, nullptr);
    if (JS_IsException(obj)) {
        JSValue ex = JS_GetException(c->ctx); JS_FreeValue(c->ctx, ex); return nullptr;
    }
    auto* i = new TsInstance(); i->compiled = c; i->obj = obj; return i;
}
static void ts_free_instance(void* raw) {
    if (!raw) return;
    auto* i = static_cast<TsInstance*>(raw);
    if (i->compiled) { std::lock_guard<std::mutex> lk(i->compiled->ctx_mutex);
        JS_FreeValue(i->compiled->ctx, i->obj); }
    delete i;
}
static int ts_call_method(void* raw, const char* name, PLValue* args, int32_t argc, PLValue* ret) {
    auto* i = static_cast<TsInstance*>(raw);
    if (!i || !i->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(i->compiled->ctx_mutex);
    JSContext* ctx = i->compiled->ctx;
    JSValue fn = JS_GetPropertyStr(ctx, i->obj, name);
    if (!JS_IsFunction(ctx, fn)) { JS_FreeValue(ctx, fn); return PL_ERR_METHOD_NOT_FOUND; }
    std::vector<JSValue> jargs(argc);
    for (int32_t k = 0; k < argc; ++k) jargs[k] = pl_to_js(ctx, args[k]);
    JSValue result = JS_Call(ctx, fn, i->obj, argc, jargs.data());
    JS_FreeValue(ctx, fn);
    for (auto& v : jargs) JS_FreeValue(ctx, v);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx); JS_FreeValue(ctx, ex);
        JS_FreeValue(ctx, result); ret->type = PL_TYPE_NIL; return PL_ERR_EXCEPTION;
    }
    js_to_pl(ctx, result, *ret); JS_FreeValue(ctx, result); return PL_OK;
}
static int ts_call_builtin(void* raw, int32_t id, PLValue* args, int32_t argc, PLValue* ret) {
    const char* n = nullptr;
    switch(id) {
        case PL_METHOD_READY:           n="_ready"; break;
        case PL_METHOD_PROCESS:         n="_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: n="_physicsProcess"; break;
        case PL_METHOD_ENTER_TREE:      n="_enterTree"; break;
        case PL_METHOD_EXIT_TREE:       n="_exitTree"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return ts_call_method(raw, n, args, argc, ret);
}
static int ts_set_prop(void* raw, const char* name, const PLValue* v) {
    auto* i = static_cast<TsInstance*>(raw);
    if (!i || !i->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(i->compiled->ctx_mutex);
    JS_SetPropertyStr(i->compiled->ctx, i->obj, name, pl_to_js(i->compiled->ctx, *v));
    return PL_OK;
}
static int ts_get_prop(void* raw, const char* name, PLValue* out) {
    auto* i = static_cast<TsInstance*>(raw);
    if (!i || !i->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(i->compiled->ctx_mutex);
    JSValue v = JS_GetPropertyStr(i->compiled->ctx, i->obj, name);
    if (JS_IsUndefined(v)) { JS_FreeValue(i->compiled->ctx, v); return PL_ERR_PROP_NOT_FOUND; }
    js_to_pl(i->compiled->ctx, v, *out); JS_FreeValue(i->compiled->ctx, v); return PL_OK;
}
static uint8_t ts_has_method(void*, const char*) { return 1; }
static void ts_free_val(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                               | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime      = ts_init_runtime;
    out->pl_shutdown_runtime  = ts_shutdown_runtime;
    out->pl_compile           = ts_compile_pl;
    out->pl_compile_sandboxed = ts_compile_sandboxed;
    out->pl_free_compiled     = ts_free_compiled;
    out->pl_instantiate_class = ts_instantiate_class;
    out->pl_free_instance     = ts_free_instance;
    out->pl_call_method       = ts_call_method;
    out->pl_call_builtin      = ts_call_builtin;
    out->pl_set_property      = ts_set_prop;
    out->pl_get_property      = ts_get_prop;
    out->pl_has_method        = ts_has_method;
    out->pl_free_value_contents = ts_free_val;
}
