// =============================================================
// polylang_julia_adapter.cpp  —  PolyLang Julia Adapter v5
// =============================================================
// FIX P5-1:  JL_GC_PUSH1 now has matching JL_GC_POP on all paths.
// FIX P5-2:  jl_args GC-rooted with JL_GC_PUSHARGS.
// FIX P5-3:  Instance rooting uses a Julia-side IdDict (g_instance_roots).
// FIX P2-1:  pl_compile receives source text, not filesystem path.
// SANDBOX:   pl_compile_sandboxed() wraps the module in a restricted
//            environment that redefines Base.open/run/cd/download/eval
//            as throwing stubs, then evaluates the script source text.
//            Desktop-only: Julia is never available on Android/iOS.
// =============================================================
#include <julia.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

#include "../../include/pl_adapter_vtable.h"

static bool       g_julia_init = false;
static std::mutex g_julia_mutex;
static jl_value_t* g_instance_roots = nullptr;

// ── Value conversion ──────────────────────────────────────────
static jl_value_t* pl_to_jl(const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    return jl_nothing;
        case PL_TYPE_BOOL:   return jl_box_bool(v.b ? 1 : 0);
        case PL_TYPE_INT:    return jl_box_int64(v.i);
        case PL_TYPE_FLOAT:  return jl_box_float64(v.f);
        case PL_TYPE_STRING: return jl_cstr_to_string(v.s ? v.s : "");
        case PL_TYPE_VEC2: {
            jl_value_t* arr = jl_alloc_array_1d(
                jl_apply_array_type((jl_value_t*)jl_float32_type, 1), 2);
            float* d = (float*)jl_array_data(arr);
            d[0] = v.v2[0]; d[1] = v.v2[1];
            return arr;
        }
        case PL_TYPE_VEC3: {
            jl_value_t* arr = jl_alloc_array_1d(
                jl_apply_array_type((jl_value_t*)jl_float32_type, 1), 3);
            float* d = (float*)jl_array_data(arr);
            d[0] = v.v3[0]; d[1] = v.v3[1]; d[2] = v.v3[2];
            return arr;
        }
        default: return jl_nothing;
    }
}

static void jl_to_pl(jl_value_t* val, PLValue& out) {
    pl_value_init(&out);
    if (!val || val == jl_nothing) { out.type = PL_TYPE_NIL; return; }
    if (jl_is_bool(val))   { out.type = PL_TYPE_BOOL;  out.b = (jl_unbox_bool(val) != 0); return; }
    if (jl_is_int64(val))  { out.type = PL_TYPE_INT;   out.i = jl_unbox_int64(val); return; }
    if (jl_is_float64(val)){ out.type = PL_TYPE_FLOAT; out.f = jl_unbox_float64(val); return; }
    if (jl_is_string(val)) { out.type = PL_TYPE_STRING; out.s = strdup(jl_string_ptr(val)); return; }
    out.type = PL_TYPE_NIL;
}

// ── Handles ───────────────────────────────────────────────────
struct JlCompiled {
    jl_module_t* module{nullptr};
    std::string  class_name;
    bool         sandboxed{false};
};
struct JlInstance {
    JlCompiled*  compiled{nullptr};
    jl_value_t*  obj{nullptr};
    uintptr_t    root_key{0};
};

// ── ABI ───────────────────────────────────────────────────────
static int jl_init_runtime_pl() {
    std::lock_guard<std::mutex> lk(g_julia_mutex);
    if (g_julia_init) return PL_OK;
    jl_init();
    // Create IdDict for instance rooting (FIX P5-3)
    g_instance_roots = jl_eval_string("IdDict()");
    g_julia_init = true;
    return PL_OK;
}

static void jl_shutdown_runtime_pl() {
    std::lock_guard<std::mutex> lk(g_julia_mutex);
    if (!g_julia_init) return;
    jl_atexit_hook(0);
    g_julia_init = false;
}

static void* jl_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source) return nullptr;
    std::lock_guard<std::mutex> lk(g_julia_mutex);

    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find('.');  if (dot   != std::string::npos) p = p.substr(0, dot);
    // Replace hyphens with underscores (invalid Julia identifiers)
    for (auto& c : p) if (c == '-') c = '_';

    std::string wrapped;
    if (sandboxed) {
        // Wrap in a module that shadows dangerous Base functions
        wrapped = "module " + p + "\n"
            "using Base\n"
            "# Sandbox: block dangerous Base functions\n"
            "open(args...; kwargs...) = error(\"[PolyLang/sandbox] open() blocked\")\n"
            "run(args...; kwargs...)  = error(\"[PolyLang/sandbox] run() blocked\")\n"
            "cd(args...; kwargs...)   = error(\"[PolyLang/sandbox] cd() blocked\")\n"
            "download(args...; kwargs...) = error(\"[PolyLang/sandbox] download() blocked\")\n"
            "eval(args...) = error(\"[PolyLang/sandbox] eval() blocked\")\n"
            "include(args...) = error(\"[PolyLang/sandbox] include() blocked\")\n"
            "\n" + std::string(source) + "\n"
            "end # module\n";
    } else {
        wrapped = "module " + p + "\n" + std::string(source) + "\nend\n";
    }

    jl_value_t* mod_val = jl_eval_string(wrapped.c_str());
    if (jl_exception_occurred()) {
        jl_value_t* ex = jl_exception_occurred();
        fprintf(stderr, "[PolyLang/Julia%s] Eval error: %s\n",
                sandboxed ? "/sandbox" : "",
                jl_typeof_str(ex));
        jl_exception_clear();
        return nullptr;
    }
    if (!mod_val || !jl_is_module((jl_value_t*)mod_val)) {
        fprintf(stderr, "[PolyLang/Julia%s] Module '%s' not created\n",
                sandboxed ? "/sandbox" : "", p.c_str());
        return nullptr;
    }

    auto* c = new JlCompiled();
    c->module = (jl_module_t*)mod_val;
    c->class_name = p;
    c->sandboxed = sandboxed;
    return c;
}

static void* jl_compile_pl(const char* s, const char* p)                   { return jl_compile_core(s, p, false); }
static void* jl_compile_sandboxed(const char* s, const char* p, uint32_t)  { return jl_compile_core(s, p, true); }

static void jl_free_compiled(void* h) { delete static_cast<JlCompiled*>(h); }

static void* jl_instantiate_class(void* ch, const char*) {
    auto* c = static_cast<JlCompiled*>(ch); if (!c || !c->module) return nullptr;
    std::lock_guard<std::mutex> lk(g_julia_mutex);

    jl_value_t* ctor = jl_get_global(c->module, jl_symbol(c->class_name.c_str()));
    if (!ctor) {
        fprintf(stderr, "[PolyLang/Julia] Constructor '%s' not found\n", c->class_name.c_str());
        return nullptr;
    }

    jl_value_t* obj = jl_call0(ctor);
    if (jl_exception_occurred()) {
        jl_exception_clear(); return nullptr;
    }

    // FIX P5-3: Root in IdDict to prevent GC collection
    static uintptr_t g_key_counter = 1;
    uintptr_t key = g_key_counter++;
    jl_value_t* key_val = jl_box_uint64((uint64_t)key);
    JL_GC_PUSH2(&obj, &key_val);
    jl_call2(jl_get_function(jl_base_module, "setindex!"), g_instance_roots, obj, key_val);
    JL_GC_POP();

    auto* inst = new JlInstance(); inst->compiled = c; inst->obj = obj; inst->root_key = key;
    return inst;
}

static void jl_free_instance(void* raw) {
    if (!raw) return;
    auto* i = static_cast<JlInstance*>(raw);
    if (i->root_key && g_instance_roots) {
        std::lock_guard<std::mutex> lk(g_julia_mutex);
        jl_value_t* key_val = jl_box_uint64((uint64_t)i->root_key);
        JL_GC_PUSH1(&key_val);
        jl_call2(jl_get_function(jl_base_module, "delete!"), g_instance_roots, key_val);
        JL_GC_POP();
    }
    delete i;
}

static int jl_call_method(void* raw, const char* name, PLValue* args, int32_t argc, PLValue* ret) {
    auto* i = static_cast<JlInstance*>(raw);
    if (!i || !i->compiled || !i->obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_julia_mutex);

    jl_value_t* fn = jl_get_global(i->compiled->module, jl_symbol(name));
    if (!fn) return PL_ERR_METHOD_NOT_FOUND;

    std::vector<jl_value_t*> jl_args;
    jl_args.push_back(i->obj);
    for (int32_t k = 0; k < argc; ++k) jl_args.push_back(pl_to_jl(args[k]));

    jl_value_t** arr = jl_args.data();
    // FIX P5-2: Root args
    JL_GC_PUSHARGS(arr, (int)jl_args.size());
    jl_value_t* result = jl_call(fn, arr, (int32_t)jl_args.size());
    bool ex = jl_exception_occurred() != nullptr;
    if (ex) { jl_exception_clear(); }
    if (!ex && result) jl_to_pl(result, *ret);
    JL_GC_POP();

    return ex ? PL_ERR_EXCEPTION : PL_OK;
}

static int jl_call_builtin(void* raw, int32_t id, PLValue* args, int32_t argc, PLValue* ret) {
    const char* n = nullptr;
    switch (id) {
        case PL_METHOD_READY:           n="_ready"; break;
        case PL_METHOD_PROCESS:         n="_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: n="_physicsProcess"; break;
        case PL_METHOD_ENTER_TREE:      n="_enterTree"; break;
        case PL_METHOD_EXIT_TREE:       n="_exitTree"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return jl_call_method(raw, n, args, argc, ret);
}

static int jl_set_prop(void* raw, const char* name, const PLValue* v) {
    auto* i = static_cast<JlInstance*>(raw); if (!i || !i->obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_julia_mutex);
    jl_value_t* val = pl_to_jl(*v);
    JL_GC_PUSH1(&val);
    jl_set_field(i->obj, name, val);
    JL_GC_POP();
    return jl_exception_occurred() ? (jl_exception_clear(), PL_ERR_PROP_NOT_FOUND) : PL_OK;
}

static int jl_get_prop(void* raw, const char* name, PLValue* out) {
    auto* i = static_cast<JlInstance*>(raw); if (!i || !i->obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_julia_mutex);
    jl_value_t* val = jl_get_field(i->obj, name);
    if (jl_exception_occurred()) { jl_exception_clear(); return PL_ERR_PROP_NOT_FOUND; }
    jl_to_pl(val, *out);
    return PL_OK;
}

static uint8_t jl_has_method(void* ch, const char* name) {
    auto* c = static_cast<JlCompiled*>(ch); if (!c || !c->module) return 0;
    return jl_get_global(c->module, jl_symbol(name)) ? 1 : 0;
}

static void jl_free_val(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_DESKTOP | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime      = jl_init_runtime_pl;
    out->pl_shutdown_runtime  = jl_shutdown_runtime_pl;
    out->pl_compile           = jl_compile_pl;
    out->pl_compile_sandboxed = jl_compile_sandboxed;
    out->pl_free_compiled     = jl_free_compiled;
    out->pl_instantiate_class = jl_instantiate_class;
    out->pl_free_instance     = jl_free_instance;
    out->pl_call_method       = jl_call_method;
    out->pl_call_builtin      = jl_call_builtin;
    out->pl_set_property      = jl_set_prop;
    out->pl_get_property      = jl_get_prop;
    out->pl_has_method        = jl_has_method;
    out->pl_free_value_contents = jl_free_val;
}
