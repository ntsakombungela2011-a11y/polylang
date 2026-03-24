// =============================================================
// polylang_angelscript_adapter.cpp  —  AngelScript Adapter v5
// =============================================================
// FIX P5-7:  new std::string in pl_to_as_arg → freed by AS destructor reg.
// FIX P2-1:  pl_compile stores source text; Build() uses AddScriptSection.
// FIX NEW-1: ctx->Prepare return code checked; null-check before Execute;
//            null-check before GetAddressOfReturnValue.
// SANDBOX:   pl_compile_sandboxed() does NOT register file/process/network
//            functions. All bridge-side dangerous functions are blocked.
//            AS itself has no dynamic OS access; sandbox = deny registrations.
// =============================================================
#include <angelscript.h>
#include <scriptstdstring/scriptstdstring.h>
#include <scriptarray/scriptarray.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>

#include "../../include/pl_adapter_vtable.h"

static asIScriptEngine* g_engine = nullptr;
static std::mutex       g_as_mutex;

static void as_message_cb(const asSMessageInfo* msg, void*) {
    const char* t = "ERR ";
    if (msg->type == asMSGTYPE_INFORMATION) t = "INFO";
    else if (msg->type == asMSGTYPE_WARNING) t = "WARN";
    fprintf(stderr, "[PolyLang/AS] %s (%d,%d): %s: %s\n",
        msg->section, msg->row, msg->col, t, msg->message);
}

// ── Value helpers ─────────────────────────────────────────────
static void pl_to_as_arg(asIScriptContext* ctx, int arg_idx, const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_BOOL:   ctx->SetArgByte(arg_idx, v.b ? 1 : 0); break;
        case PL_TYPE_INT:    ctx->SetArgQWord(arg_idx, (asQWORD)v.i); break;
        case PL_TYPE_FLOAT:  ctx->SetArgDouble(arg_idx, v.f); break;
        case PL_TYPE_STRING: {
            // FIX P5-7: AS owns via registered destructor
            std::string* s = new std::string(v.s ? v.s : "");
            ctx->SetArgObject(arg_idx, s);
            break;
        }
        default: break;
    }
}

static void as_ret_to_pl(asIScriptContext* ctx, const asIScriptFunction* fn, PLValue& out) {
    pl_value_init(&out);
    int ret_type_id = fn->GetReturnTypeId();
    if (ret_type_id == asTYPEID_VOID)   { out.type = PL_TYPE_NIL;   return; }
    if (ret_type_id == asTYPEID_BOOL)   { out.type = PL_TYPE_BOOL;  out.b = ctx->GetReturnByte() != 0; return; }
    if (ret_type_id == asTYPEID_INT64)  { out.type = PL_TYPE_INT;   out.i = (int64_t)ctx->GetReturnQWord(); return; }
    if (ret_type_id == asTYPEID_FLOAT || ret_type_id == asTYPEID_DOUBLE) {
        out.type = PL_TYPE_FLOAT; out.f = ctx->GetReturnDouble(); return;
    }
    // String return: get pointer via GetAddressOfReturnValue (FIX NEW-1: null check)
    void* ptr = ctx->GetAddressOfReturnValue();
    if (ptr) {
        int type_id = fn->GetReturnTypeId();
        asITypeInfo* ti = g_engine->GetTypeInfoById(type_id);
        if (ti && strcmp(ti->GetName(), "string") == 0) {
            out.type = PL_TYPE_STRING;
            out.s = strdup(static_cast<std::string*>(ptr)->c_str());
            return;
        }
    }
    out.type = PL_TYPE_NIL;
}

// ── Handles ───────────────────────────────────────────────────
struct ASCompiled {
    asIScriptModule* module{nullptr};
    std::string      module_name;
    std::string      class_name;
    asITypeInfo*     type_info{nullptr};
    bool             sandboxed{false};
};
struct ASInstance {
    ASCompiled*      compiled{nullptr};
    asIScriptObject* obj{nullptr};
};

// ── ABI ───────────────────────────────────────────────────────
static int as_init_runtime() {
    std::lock_guard<std::mutex> lk(g_as_mutex);
    if (g_engine) return PL_OK;
    g_engine = asCreateScriptEngine();
    if (!g_engine) return PL_ERR_GENERIC;
    g_engine->SetMessageCallback(asFUNCTION(as_message_cb), nullptr, asCALL_CDECL);
    RegisterStdString(g_engine);
    RegisterScriptArray(g_engine, true);
    return PL_OK;
}

static void as_shutdown_runtime() {
    std::lock_guard<std::mutex> lk(g_as_mutex);
    if (g_engine) { g_engine->ShutDownAndRelease(); g_engine = nullptr; }
}

static void* as_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source || !g_engine) return nullptr;

    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find('.');  if (dot   != std::string::npos) p = p.substr(0, dot);

    // Unique module name to allow parallel scripts
    static std::atomic<int> mod_id{0};
    std::string mod_name = "polylang_" + p + "_" + std::to_string(mod_id.fetch_add(1));

    std::lock_guard<std::mutex> lk(g_as_mutex);
    asIScriptModule* mod = g_engine->GetModule(mod_name.c_str(), asGM_ALWAYS_CREATE);
    if (!mod) return nullptr;

    mod->AddScriptSection(path ? path : "<polylang>", source, strlen(source));
    if (mod->Build() < 0) {
        fprintf(stderr, "[PolyLang/AS%s] Build failed: %s\n", sandboxed ? "/sandbox" : "", path ? path : "");
        g_engine->DiscardModule(mod_name.c_str());
        return nullptr;
    }

    asITypeInfo* ti = mod->GetTypeInfoByName(p.c_str());
    if (!ti) {
        fprintf(stderr, "[PolyLang/AS%s] Class '%s' not found\n", sandboxed ? "/sandbox" : "", p.c_str());
        g_engine->DiscardModule(mod_name.c_str());
        return nullptr;
    }

    auto* c = new ASCompiled();
    c->module = mod; c->module_name = mod_name; c->class_name = p;
    c->type_info = ti; c->sandboxed = sandboxed;
    return c;
}

static void* as_compile_pl(const char* s, const char* p)                   { return as_compile_core(s, p, false); }
static void* as_compile_sandboxed(const char* s, const char* p, uint32_t)  { return as_compile_core(s, p, true); }

static void as_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<ASCompiled*>(h);
    std::lock_guard<std::mutex> lk(g_as_mutex);
    if (g_engine) g_engine->DiscardModule(c->module_name.c_str());
    delete c;
}

static void* as_instantiate_class(void* ch, const char*) {
    auto* c = static_cast<ASCompiled*>(ch);
    if (!c || !c->type_info) return nullptr;
    std::lock_guard<std::mutex> lk(g_as_mutex);
    asIScriptObject* obj = reinterpret_cast<asIScriptObject*>(
        g_engine->CreateScriptObject(c->type_info));
    if (!obj) return nullptr;
    auto* inst = new ASInstance(); inst->compiled = c; inst->obj = obj;
    return inst;
}

static void as_free_instance(void* raw) {
    if (!raw) return;
    auto* i = static_cast<ASInstance*>(raw);
    if (i->obj) { std::lock_guard<std::mutex> lk(g_as_mutex); i->obj->Release(); }
    delete i;
}

static int as_call_method(void* raw, const char* name, PLValue* args, int32_t argc, PLValue* ret) {
    auto* i = static_cast<ASInstance*>(raw);
    if (!i || !i->compiled || !i->obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_as_mutex);

    asITypeInfo* ti = i->compiled->type_info;
    asIScriptFunction* fn = nullptr;
    for (asUINT m = 0; m < ti->GetMethodCount(); ++m) {
        asIScriptFunction* f = ti->GetMethodByIndex(m);
        if (strcmp(f->GetName(), name) == 0) { fn = f; break; }
    }
    if (!fn) return PL_ERR_METHOD_NOT_FOUND;

    asIScriptContext* ctx = g_engine->CreateContext();
    if (!ctx) return PL_ERR_GENERIC;

    int r = ctx->Prepare(fn);   // FIX NEW-1: check return
    if (r < 0) { ctx->Release(); return PL_ERR_GENERIC; }
    ctx->SetObject(i->obj);
    for (int32_t k = 0; k < argc; ++k) pl_to_as_arg(ctx, k, args[k]);

    r = ctx->Execute();
    if (r == asEXECUTION_EXCEPTION) {
        fprintf(stderr, "[PolyLang/AS] Exception: %s\n", ctx->GetExceptionString());
        ctx->Release(); ret->type = PL_TYPE_NIL; return PL_ERR_EXCEPTION;
    }
    if (r == asEXECUTION_FINISHED) as_ret_to_pl(ctx, fn, *ret);
    ctx->Release();
    return PL_OK;
}

static int as_call_builtin(void* raw, int32_t id, PLValue* args, int32_t argc, PLValue* ret) {
    const char* n = nullptr;
    switch (id) {
        case PL_METHOD_READY:           n="_ready"; break;
        case PL_METHOD_PROCESS:         n="_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: n="_physicsProcess"; break;
        case PL_METHOD_ENTER_TREE:      n="_enterTree"; break;
        case PL_METHOD_EXIT_TREE:       n="_exitTree"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return as_call_method(raw, n, args, argc, ret);
}

static int as_set_prop(void* raw, const char* name, const PLValue* v) {
    auto* i = static_cast<ASInstance*>(raw); if (!i || !i->obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_as_mutex);
    asITypeInfo* ti = i->compiled->type_info;
    for (asUINT p = 0; p < ti->GetPropertyCount(); ++p) {
        const char* pname = nullptr; int type_id = 0;
        ti->GetProperty(p, &pname, &type_id);
        if (pname && strcmp(pname, name) == 0) {
            void* addr = i->obj->GetAddressOfProperty(p);
            if (!addr) return PL_ERR_PROP_NOT_FOUND;
            switch (v->type) {
                case PL_TYPE_BOOL:  *(bool*)addr   = v->b; break;
                case PL_TYPE_INT:   *(int64_t*)addr = v->i; break;
                case PL_TYPE_FLOAT: *(double*)addr  = v->f; break;
                case PL_TYPE_STRING:
                    if (type_id == g_engine->GetTypeIdByDecl("string"))
                        *(std::string*)addr = v->s ? v->s : "";
                    break;
                default: break;
            }
            return PL_OK;
        }
    }
    return PL_ERR_PROP_NOT_FOUND;
}

static int as_get_prop(void* raw, const char* name, PLValue* out) {
    auto* i = static_cast<ASInstance*>(raw); if (!i || !i->obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_as_mutex);
    asITypeInfo* ti = i->compiled->type_info;
    for (asUINT p = 0; p < ti->GetPropertyCount(); ++p) {
        const char* pname = nullptr; int type_id = 0;
        ti->GetProperty(p, &pname, &type_id);
        if (pname && strcmp(pname, name) == 0) {
            void* addr = i->obj->GetAddressOfProperty(p);
            if (!addr) return PL_ERR_PROP_NOT_FOUND;
            pl_value_init(out);
            if (type_id == asTYPEID_BOOL)   { out->type = PL_TYPE_BOOL;  out->b = *(bool*)addr;   return PL_OK; }
            if (type_id == asTYPEID_INT64)  { out->type = PL_TYPE_INT;   out->i = *(int64_t*)addr; return PL_OK; }
            if (type_id == asTYPEID_DOUBLE) { out->type = PL_TYPE_FLOAT; out->f = *(double*)addr; return PL_OK; }
            if (type_id == g_engine->GetTypeIdByDecl("string")) {
                out->type = PL_TYPE_STRING;
                out->s = strdup(((std::string*)addr)->c_str());
                return PL_OK;
            }
            return PL_ERR_PROP_NOT_FOUND;
        }
    }
    return PL_ERR_PROP_NOT_FOUND;
}

static uint8_t as_has_method(void* ch, const char* name) {
    auto* c = static_cast<ASCompiled*>(ch); if (!c || !c->type_info) return 0;
    for (asUINT m = 0; m < c->type_info->GetMethodCount(); ++m)
        if (strcmp(c->type_info->GetMethodByIndex(m)->GetName(), name) == 0) return 1;
    return 0;
}

static void as_free_val(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                               | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime      = as_init_runtime;
    out->pl_shutdown_runtime  = as_shutdown_runtime;
    out->pl_compile           = as_compile_pl;
    out->pl_compile_sandboxed = as_compile_sandboxed;
    out->pl_free_compiled     = as_free_compiled;
    out->pl_instantiate_class = as_instantiate_class;
    out->pl_free_instance     = as_free_instance;
    out->pl_call_method       = as_call_method;
    out->pl_call_builtin      = as_call_builtin;
    out->pl_set_property      = as_set_prop;
    out->pl_get_property      = as_get_prop;
    out->pl_has_method        = as_has_method;
    out->pl_free_value_contents = as_free_val;
}
