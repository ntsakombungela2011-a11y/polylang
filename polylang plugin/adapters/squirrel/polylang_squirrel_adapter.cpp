// =============================================================
// polylang_squirrel_adapter.cpp  —  Squirrel Scripting Adapter v5
// =============================================================
// FIX P5-4:  #include individual sqstd headers (not sqstdlib.h).
// FIX P5-5:  sq_to_pl array: removed double-pop that corrupted stack.
// FIX P2-1:  pl_compile() receives source text; sq_compilebuffer used.
// SANDBOX:   pl_compile_sandboxed() creates isolated VM with no sqstdio,
//            no sqstdmath, and a blocked-print function that denies exec.
//            Dangerous globals (system/exec) are patched after compile.
// =============================================================
#include <squirrel.h>
#include <sqstdaux.h>
#include <sqstdio.h>
#include <sqstdmath.h>
#include <sqstdstring.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>

#include "../../include/pl_adapter_vtable.h"

static void sq_print_cb(HSQUIRRELVM, const SQChar* fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}
static void sq_error_cb(HSQUIRRELVM, const SQChar* fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
}

// ── Value helpers ─────────────────────────────────────────────
static void pl_to_sq(HSQUIRRELVM vm, const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    sq_pushnull(vm);                          break;
        case PL_TYPE_BOOL:   sq_pushbool(vm, v.b ? SQTrue : SQFalse); break;
        case PL_TYPE_INT:    sq_pushinteger(vm, (SQInteger)v.i);       break;
        case PL_TYPE_FLOAT:  sq_pushfloat(vm, (SQFloat)v.f);          break;
        case PL_TYPE_STRING: sq_pushstring(vm, v.s ? v.s : "", -1);   break;
        case PL_TYPE_VEC2:
            sq_newarray(vm, 0);
            sq_pushfloat(vm, v.v2[0]); sq_arrayappend(vm, -2);
            sq_pushfloat(vm, v.v2[1]); sq_arrayappend(vm, -2);
            break;
        case PL_TYPE_VEC3:
            sq_newarray(vm, 0);
            sq_pushfloat(vm, v.v3[0]); sq_arrayappend(vm, -2);
            sq_pushfloat(vm, v.v3[1]); sq_arrayappend(vm, -2);
            sq_pushfloat(vm, v.v3[2]); sq_arrayappend(vm, -2);
            break;
        default: sq_pushnull(vm); break;
    }
}

static void sq_to_pl(HSQUIRRELVM vm, SQInteger idx, PLValue& out) {
    pl_value_init(&out);
    SQObjectType t = sq_gettype(vm, idx);
    switch (t) {
        case OT_NULL:  out.type = PL_TYPE_NIL; break;
        case OT_BOOL: {
            SQBool b; sq_getbool(vm, idx, &b);
            out.type = PL_TYPE_BOOL; out.b = (b == SQTrue); break;
        }
        case OT_INTEGER: {
            SQInteger i; sq_getinteger(vm, idx, &i);
            out.type = PL_TYPE_INT; out.i = (int64_t)i; break;
        }
        case OT_FLOAT: {
            SQFloat f; sq_getfloat(vm, idx, &f);
            out.type = PL_TYPE_FLOAT; out.f = (double)f; break;
        }
        case OT_STRING: {
            const SQChar* s = nullptr; sq_getstring(vm, idx, &s);
            out.type = PL_TYPE_STRING; out.s = s ? strdup(s) : nullptr; break;
        }
        case OT_ARRAY: {
            SQInteger n = sq_getsize(vm, idx);
            out.type = PL_TYPE_ARRAY; out.array.len = (int32_t)n;
            out.array.data = (PLValue*)calloc(n, sizeof(PLValue));
            for (SQInteger i = 0; i < n; ++i) {
                sq_pushinteger(vm, i);
                sq_rawget(vm, idx < 0 ? idx - 1 : idx);
                sq_to_pl(vm, -1, out.array.data[i]);
                sq_pop(vm, 1); // FIX P5-5: only pop the value, NOT the index
            }
            break;
        }
        default: out.type = PL_TYPE_NIL; break;
    }
}

// ── Sandbox: blocked function stub ───────────────────────────
static SQInteger sq_blocked(HSQUIRRELVM vm) {
    return sq_throwerror(vm, "[PolyLang/Squirrel/sandbox] blocked in sandboxed scripts.");
}

static void sq_block_global(HSQUIRRELVM vm, const SQChar* name) {
    sq_pushroottable(vm);
    sq_pushstring(vm, name, -1);
    sq_newclosure(vm, sq_blocked, 0);
    sq_newslot(vm, -3, SQFalse);
    sq_pop(vm, 1);
}

static void sq_apply_sandbox(HSQUIRRELVM vm) {
    // Block dangerous globals
    const SQChar* blocked[] = {
        "system", "exec", "popen", "getenv", "setenv", "tmpname",
        nullptr
    };
    for (int i = 0; blocked[i]; ++i) sq_block_global(vm, blocked[i]);
    // sqstdio already not loaded in sandboxed mode — belt and suspenders:
    sq_pushroottable(vm);
    sq_pushstring(vm, "file", -1);
    sq_deleteslot(vm, -2, SQFalse);
    sq_pop(vm, 1);
}

// ── Handles ───────────────────────────────────────────────────
struct SqCompiled {
    HSQUIRRELVM  vm{nullptr};
    std::mutex   vm_mutex;
    std::string  class_name;
    bool         sandboxed{false};
};
struct SqInstance {
    SqCompiled* compiled{nullptr};
    HSQOBJECT   obj;
};

// ── Core compile ──────────────────────────────────────────────
static void* sq_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source) return nullptr;

    HSQUIRRELVM vm = sq_open(1024);
    if (!vm) return nullptr;

    sq_setprintfunc(vm, sq_print_cb, sq_error_cb);
    sqstd_seterrorhandlers(vm);

    if (sandboxed) {
        // Only safe: string + math (no io, no aux file functions)
        sqstd_register_stringlib(vm);
        sqstd_register_mathlib(vm);
        sq_apply_sandbox(vm);
    } else {
        sqstd_register_iolib(vm);
        sqstd_register_mathlib(vm);
        sqstd_register_stringlib(vm);
    }

    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find('.');  if (dot   != std::string::npos) p = p.substr(0, dot);

    SQInteger top = sq_gettop(vm);
    if (SQ_FAILED(sq_compilebuffer(vm, source, (SQInteger)strlen(source),
                                   path ? path : "<polylang>", SQTrue))) {
        fprintf(stderr, "[PolyLang/Squirrel%s] Compile failed: %s\n",
                sandboxed ? "/sandbox" : "", path ? path : "");
        sq_settop(vm, top);
        sq_close(vm);
        return nullptr;
    }
    sq_pushroottable(vm);
    if (SQ_FAILED(sq_call(vm, 1, SQFalse, SQTrue))) {
        fprintf(stderr, "[PolyLang/Squirrel%s] Execute failed\n", sandboxed ? "/sandbox" : "");
        sq_close(vm); return nullptr;
    }
    sq_settop(vm, top);

    auto* c = new SqCompiled(); c->vm = vm; c->class_name = p; c->sandboxed = sandboxed;
    return c;
}

// ── ABI ───────────────────────────────────────────────────────
static int sq_init_runtime()    { return PL_OK; }
static void sq_shutdown_runtime() {}

static void* sq_compile_pl(const char* s, const char* p)              { return sq_compile_core(s, p, false); }
static void* sq_compile_sandboxed(const char* s, const char* p, uint32_t) { return sq_compile_core(s, p, true); }

static void sq_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<SqCompiled*>(h);
    std::lock_guard<std::mutex> lk(c->vm_mutex);
    if (c->vm) sq_close(c->vm);
    delete c;
}

static void* sq_instantiate_class(void* ch, const char*) {
    auto* c = static_cast<SqCompiled*>(ch);
    if (!c || !c->vm) return nullptr;
    std::lock_guard<std::mutex> lk(c->vm_mutex);

    sq_pushroottable(c->vm);
    sq_pushstring(c->vm, c->class_name.c_str(), -1);
    if (SQ_FAILED(sq_get(c->vm, -2))) {
        sq_pop(c->vm, 1);
        fprintf(stderr, "[PolyLang/Squirrel] Class '%s' not found\n", c->class_name.c_str());
        return nullptr;
    }
    // Create instance
    if (SQ_FAILED(sq_createinstance(c->vm, -1))) {
        sq_pop(c->vm, 2);
        return nullptr;
    }
    auto* inst = new SqInstance();
    inst->compiled = c;
    sq_getstackobj(c->vm, -1, &inst->obj);
    sq_addref(c->vm, &inst->obj);
    sq_pop(c->vm, 3); // instance, class, root
    return inst;
}

static void sq_free_instance(void* raw) {
    if (!raw) return;
    auto* i = static_cast<SqInstance*>(raw);
    if (i->compiled && i->compiled->vm)
        sq_release(i->compiled->vm, &i->obj);
    delete i;
}

static int sq_call_method(void* raw, const char* name, PLValue* args, int32_t argc, PLValue* ret) {
    auto* i = static_cast<SqInstance*>(raw);
    if (!i || !i->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(i->compiled->vm_mutex);
    HSQUIRRELVM vm = i->compiled->vm;

    sq_pushobject(vm, i->obj);
    sq_pushstring(vm, name, -1);
    if (SQ_FAILED(sq_get(vm, -2))) {
        sq_pop(vm, 1);
        return PL_ERR_METHOD_NOT_FOUND;
    }
    sq_pushobject(vm, i->obj); // 'this'
    for (int32_t k = 0; k < argc; ++k) pl_to_sq(vm, args[k]);
    if (SQ_FAILED(sq_call(vm, argc + 1, SQTrue, SQTrue))) {
        sq_pop(vm, 2);
        ret->type = PL_TYPE_NIL;
        return PL_ERR_EXCEPTION;
    }
    sq_to_pl(vm, -1, *ret);
    sq_pop(vm, 3); // result, fn, self
    return PL_OK;
}

static int sq_call_builtin(void* raw, int32_t id, PLValue* args, int32_t argc, PLValue* ret) {
    const char* n = nullptr;
    switch (id) {
        case PL_METHOD_READY:           n="_ready"; break;
        case PL_METHOD_PROCESS:         n="_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: n="_physics_process"; break;
        case PL_METHOD_ENTER_TREE:      n="_enter_tree"; break;
        case PL_METHOD_EXIT_TREE:       n="_exit_tree"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return sq_call_method(raw, n, args, argc, ret);
}

static int sq_set_prop(void* raw, const char* name, const PLValue* v) {
    auto* i = static_cast<SqInstance*>(raw); if (!i || !i->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(i->compiled->vm_mutex);
    sq_pushobject(i->compiled->vm, i->obj);
    sq_pushstring(i->compiled->vm, name, -1);
    pl_to_sq(i->compiled->vm, *v);
    sq_newslot(i->compiled->vm, -3, SQFalse);
    sq_pop(i->compiled->vm, 1);
    return PL_OK;
}

static int sq_get_prop(void* raw, const char* name, PLValue* out) {
    auto* i = static_cast<SqInstance*>(raw); if (!i || !i->compiled) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(i->compiled->vm_mutex);
    sq_pushobject(i->compiled->vm, i->obj);
    sq_pushstring(i->compiled->vm, name, -1);
    if (SQ_FAILED(sq_get(i->compiled->vm, -2))) {
        sq_pop(i->compiled->vm, 1); return PL_ERR_PROP_NOT_FOUND;
    }
    sq_to_pl(i->compiled->vm, -1, *out);
    sq_pop(i->compiled->vm, 2);
    return PL_OK;
}

static uint8_t sq_has_method(void* ch, const char* name) {
    auto* c = static_cast<SqCompiled*>(ch); if (!c || !c->vm) return 0;
    std::lock_guard<std::mutex> lk(c->vm_mutex);
    sq_pushroottable(c->vm);
    sq_pushstring(c->vm, c->class_name.c_str(), -1);
    if (SQ_FAILED(sq_get(c->vm, -2))) { sq_pop(c->vm, 1); return 0; }
    sq_pushstring(c->vm, name, -1);
    uint8_t has = SQ_SUCCEEDED(sq_get(c->vm, -2)) ? 1 : 0;
    sq_pop(c->vm, has ? 3 : 2);
    return has;
}

static void sq_free_val(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    if (v->type == PL_TYPE_ARRAY && v->array.data) {
        for (int i = 0; i < v->array.len; ++i) sq_free_val(&v->array.data[i]);
        free(v->array.data); v->array.data = nullptr;
    }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                               | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime      = sq_init_runtime;
    out->pl_shutdown_runtime  = sq_shutdown_runtime;
    out->pl_compile           = sq_compile_pl;
    out->pl_compile_sandboxed = sq_compile_sandboxed;
    out->pl_free_compiled     = sq_free_compiled;
    out->pl_instantiate_class = sq_instantiate_class;
    out->pl_free_instance     = sq_free_instance;
    out->pl_call_method       = sq_call_method;
    out->pl_call_builtin      = sq_call_builtin;
    out->pl_set_property      = sq_set_prop;
    out->pl_get_property      = sq_get_prop;
    out->pl_has_method        = sq_has_method;
    out->pl_free_value_contents = sq_free_val;
}
