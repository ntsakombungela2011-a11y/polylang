// =============================================================
// pl_adapter_vtable.h  —  PolyLang Adapter ABI v6
// PolyLang v6.3  |  Godot 4.6 GDExtension
// =============================================================
// ABI v6 appends at end (never reorders):
//   Coroutine:    pl_coroutine_create / _resume / _free
//   Async/Await:  pl_async_begin / _poll / _free
//   Resource:     pl_resource_fetch / _release
//   Profiler:     pl_profiler_begin / _end
//   Export vars:  pl_get_export_vars / _free_export_vars
// =============================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_ABI_VERSION  6

// ── Forward declarations ─────────────────────────────────────
struct PLRuntimeServices;

// ── Return codes ─────────────────────────────────────────────
#define PL_OK                    0
#define PL_ERR_GENERIC          -1
#define PL_ERR_METHOD_NOT_FOUND -2
#define PL_ERR_PROP_NOT_FOUND   -3
#define PL_ERR_BAD_ARG_TYPE     -4
#define PL_ERR_EXCEPTION        -5
#define PL_ERR_NOT_IMPLEMENTED  -6
#define PL_ERR_MEMORY           -7
#define PL_ERR_SANDBOX          -8
#define PL_ERR_CORO_DEAD        -9
#define PL_ERR_ASYNC_PENDING     1   // non-error: future still running

// ── Coroutine status ─────────────────────────────────────────
#define PL_CORO_SUSPENDED   0
#define PL_CORO_DONE        1
#define PL_CORO_FAILED     -1

// ── Capability flags ─────────────────────────────────────────
#define PL_CAP_ANDROID      (1u << 0)
#define PL_CAP_IOS          (1u << 1)
#define PL_CAP_DESKTOP      (1u << 2)
#define PL_CAP_JIT_NEEDED   (1u << 3)
#define PL_CAP_THREAD_SAFE  (1u << 4)
#define PL_CAP_BATCH        (1u << 5)
#define PL_CAP_BUILTIN_CALL (1u << 6)
#define PL_CAP_SANDBOX      (1u << 7)
#define PL_CAP_COROUTINE    (1u << 8)
#define PL_CAP_ASYNC        (1u << 9)
#define PL_CAP_RESOURCE     (1u << 10)
#define PL_CAP_PROFILER     (1u << 11)
#define PL_CAP_EXPORT_VARS  (1u << 12)

// ── Built-in method IDs ──────────────────────────────────────
#define PL_METHOD_READY             1
#define PL_METHOD_PROCESS           2
#define PL_METHOD_PHYSICS_PROCESS   3
#define PL_METHOD_ENTER_TREE        4
#define PL_METHOD_EXIT_TREE         5
#define PL_METHOD_INPUT             6
#define PL_METHOD_UNHANDLED_INPUT   7
#define PL_METHOD_NOTIFICATION      8
#define PL_METHOD_USER_DEFINED     64

// ── Sandbox caps ──────────────────────────────────────────────
#define PL_SANDBOX_NONE         0u
#define PL_SANDBOX_FILE_READ   (1u << 0)
#define PL_SANDBOX_FILE_WRITE  (1u << 1)
#define PL_SANDBOX_NETWORK     (1u << 2)
#define PL_SANDBOX_PROCESS     (1u << 3)
#define PL_SANDBOX_FULL        0xFFFFFFFFu

// ── Value types ───────────────────────────────────────────────
typedef enum PLValueType {
    PL_TYPE_NIL      = 0,
    PL_TYPE_BOOL     = 1,
    PL_TYPE_INT      = 2,
    PL_TYPE_FLOAT    = 3,
    PL_TYPE_STRING   = 4,
    PL_TYPE_VEC2     = 5,
    PL_TYPE_VEC3     = 6,
    PL_TYPE_QUAT     = 7,
    PL_TYPE_OBJECT   = 8,
    PL_TYPE_ARRAY    = 9,
    PL_TYPE_DICT     = 10,
    PL_TYPE_RESOURCE = 11,   // v6: opaque Godot Resource handle
} PLValueType;

// ── PLValue — 32 bytes ────────────────────────────────────────
typedef struct PLValue {
    int32_t type;
    int32_t _pad;
    union {
        bool    b;
        int64_t i;
        double  f;
        char*   s;
        float   v2[2];
        float   v3[3];
        float   q4[4];
        void*   obj_ptr;
        struct { struct PLValue* data;  int32_t len; int32_t _cap; }          array;
        struct { struct PLValue* keys; struct PLValue* values; int32_t len; int32_t _cap; } dict;
        uint8_t _raw[24];
    };
} PLValue;

#ifdef __cplusplus
static_assert(sizeof(PLValue) == 32, "PLValue must be 32 bytes");
#endif

// ── Introspection ─────────────────────────────────────────────
typedef struct PLMethodInfo   { const char* name; int32_t arg_count; int32_t _pad; }      PLMethodInfo;
typedef struct PLPropertyInfo { const char* name; int32_t type_hint; int32_t _pad; }      PLPropertyInfo;

typedef struct PLExportVarInfo {   // v6
    const char* name;
    int32_t     type_hint;
    int32_t     _pad;
    PLValue     default_val;
} PLExportVarInfo;

typedef struct PLBatchEntry { void* instance; double delta; PLValue error; } PLBatchEntry;

// ── VTable ────────────────────────────────────────────────────
// FIELD ORDER IS ABI. Never reorder. Append only.
typedef struct PLAdapterVTable {

    uint32_t abi_version;   // must equal PL_ABI_VERSION (6)
    uint32_t _reserved;
    uint32_t capabilities;
    uint32_t _pad2;

    // Runtime lifecycle
    int  (*pl_init_runtime)(void);
    void (*pl_shutdown_runtime)(void);

    // Compilation
    void* (*pl_compile)(const char* src, const char* path);
    void  (*pl_free_compiled)(void* h);

    // Instantiation
    void* (*pl_instantiate_class)(void* compiled, const char* path);
    void  (*pl_free_instance)(void* inst);

    // Dispatch
    int  (*pl_call_method)(void* inst, const char* name, PLValue* args, int32_t argc, PLValue* ret);
    int  (*pl_call_builtin)(void* inst, int32_t id,   PLValue* args, int32_t argc, PLValue* ret);
    void (*pl_batch_process)(PLBatchEntry* entries, int32_t count);

    // Properties
    int (*pl_set_property)(void* inst, const char* name, const PLValue* v);
    int (*pl_get_property)(void* inst, const char* name, PLValue* out);

    // Introspection
    uint8_t (*pl_has_method)(void* compiled, const char* name);
    void    (*pl_get_method_list)(void* compiled, PLMethodInfo** out, int32_t* count);
    void    (*pl_free_method_list)(PLMethodInfo* m);
    void    (*pl_get_property_list)(void* compiled, PLPropertyInfo** out, int32_t* count);
    void    (*pl_free_property_list)(PLPropertyInfo* p);

    // Hot-reload state
    int (*pl_serialize_state)(void* inst, PLValue* out);
    int (*pl_deserialize_state)(void* inst, const PLValue* state);

    // Sandbox
    void* (*pl_compile_sandboxed)(const char* src, const char* path, uint32_t caps);

    // Memory
    void (*pl_free_value_contents)(PLValue* v);

    // ══ v6 EXTENSIONS ════════════════════════════════════════

    // Coroutine (PL_CAP_COROUTINE)
    void* (*pl_coroutine_create)(void* inst, const char* method_name);
    int   (*pl_coroutine_resume)(void* coro, const PLValue* send, PLValue* yield_out);
    void  (*pl_coroutine_free)(void* coro);

    // Async/Await (PL_CAP_ASYNC)
    void* (*pl_async_begin)(void* inst, const char* method, PLValue* args, int32_t argc);
    int   (*pl_async_poll)(void* future, PLValue* result_out);
    void  (*pl_async_free)(void* future);

    // Resource bridges (PL_CAP_RESOURCE)
    int  (*pl_resource_fetch)(const char* res_path, PLValue* out);
    void (*pl_resource_release)(PLValue* resource_val);

    // Profiler hooks (PL_CAP_PROFILER)
    void (*pl_profiler_begin)(const char* label);
    void (*pl_profiler_end)(const char* label);

    // Typed export variables (PL_CAP_EXPORT_VARS)
    void (*pl_get_export_vars)(void* compiled, PLExportVarInfo** out, int32_t* count);
    void (*pl_free_export_vars)(PLExportVarInfo* vars, int32_t count);

    // v6.4: Generic runtime service injection.
    // Called once after all singletons are live (from register_types.cpp).
    // Adapters store the pointers and use them for signal/resource/profiler/engine calls.
    // NULL-safe: adapters that don't need injection leave this slot NULL.
    void (*pl_inject_services)(const struct PLRuntimeServices* svc);

    // v6.4: super() call slot — injected into adapters that support inheritance.
    // child_instance_ptr is a PolyLangScriptInstance*.
    int  (*pl_call_super)(void* child_instance_ptr, const char* method,
                          PLValue* args, int32_t argc, PLValue* ret);

} PLAdapterVTable;

// ── v6.4: Runtime services injected into adapters ─────────────────────────
// All function pointers are nullable; adapters must null-check before calling.
typedef struct PLRuntimeServices {
    // Signal bus
    void (*signal_emit)(const char* signal_name, PLValue* args, int32_t argc);
    void (*signal_connect)(const char* signal_name,
                           void(*callback)(PLValue* args, int32_t argc, void* userdata),
                           void* userdata);
    void (*signal_disconnect)(const char* signal_name, void* userdata);

    // Resource bridge
    int  (*resource_fetch)(const char* res_path, PLValue* out);   // returns PL_OK or error
    void (*resource_release)(PLValue* resource_val);

    // Profiler
    void (*profiler_begin)(const char* label);
    void (*profiler_end)(const char* label);

    // Engine API bridge
    int  (*engine_call)(const char* api_group, const char* method,
                        PLValue* args, int32_t argc, PLValue* ret_out);

    // Super call (cross-language inheritance)
    int  (*call_super)(void* child_instance_ptr, const char* method,
                       PLValue* args, int32_t argc, PLValue* ret);
} PLRuntimeServices;

typedef void (*PLGetVTableFn)(PLAdapterVTable* out);

#if defined(_WIN32) || defined(__CYGWIN__)
#  define PL_EXPORT __declspec(dllexport)
#  define PL_WEAK
#else
#  define PL_EXPORT __attribute__((visibility("default")))
#  define PL_WEAK   __attribute__((weak))
#endif

#ifdef __cplusplus
inline void pl_value_init(PLValue* v) { v->type = PL_TYPE_NIL; v->_pad = 0; memset(v->_raw, 0, 24); }
#else
#define pl_value_init(v) do { (v)->type=PL_TYPE_NIL; (v)->_pad=0; memset((v)->_raw,0,24); } while(0)
#endif

#ifdef __cplusplus
} // extern "C"
#endif
