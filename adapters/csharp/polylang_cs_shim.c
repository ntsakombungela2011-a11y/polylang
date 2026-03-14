/*
 * polylang_cs_shim.c — C# NativeAOT adapter vtable shim for PolyLang v5
 *
 * WHY THIS SHIM EXISTS:
 *   C# NativeAOT can export individual functions via [UnmanagedCallersOnly]
 *   with EntryPoint names, but it cannot implement pl_get_vtable() correctly
 *   because:
 *     1. C# structs cannot be used to fill C function-pointer structs portably.
 *     2. NativeAOT's managed-to-unmanaged thunks for function pointers differ
 *        from plain C function addresses that the vtable requires.
 *   This shim declares extern prototypes for every [UnmanagedCallersOnly] export
 *   and fills the PLAdapterVTable here in plain C.
 *
 * BUILD (.NET 8 NativeAOT + this shim):
 *   dotnet publish -r linux-x64 -p:NativeLib=Shared
 *   gcc -shared -fPIC polylang_cs_shim.c \
 *       -L . -l PolyLangCSharpAdapter \
 *       -I ../../include \
 *       -o libpolylang_csharp.so
 *
 *   Or via CMake: the CMakeLists target links both the NativeAOT output and
 *   this shim into the final .so.
 */

#include "../../include/pl_adapter_vtable.h"
#include <string.h>
#include <stdint.h>

/* ── Prototypes for [UnmanagedCallersOnly] C# exports ── */
extern int    cs_init_runtime(void);
extern void   cs_shutdown_runtime(void);
extern void*  cs_compile(void* source_ptr, void* path_ptr);
extern void*  cs_compile_sandboxed(void* source_ptr, void* path_ptr, uint32_t caps);
extern void   cs_free_compiled(void* h);
extern void*  cs_instantiate_class(void* ch, void* path_ptr);
extern void   cs_free_instance(void* raw);
extern int    cs_call_method(void* raw, void* name_ptr,
                              PLValue* args, int argc, PLValue* ret);
extern int    cs_call_builtin(void* raw, int id,
                               PLValue* args, int argc, PLValue* ret);
extern int    cs_set_property(void* raw, void* name_ptr, PLValue* v);
extern int    cs_get_property(void* raw, void* name_ptr, PLValue* out);
extern uint8_t cs_has_method(void* ch, void* name_ptr);
extern void   cs_free_value_contents(PLValue* v);

/*
 * Wrapper trampolines: the C# [UnmanagedCallersOnly] exports use IntPtr (void*)
 * for string arguments. These wrappers cast const char* to void* so the
 * vtable's typed function pointer signatures are satisfied.
 */
static int tramp_init_runtime(void)
    { return cs_init_runtime(); }
static void tramp_shutdown_runtime(void)
    { cs_shutdown_runtime(); }
static void* tramp_compile(const char* src, const char* path)
    { return cs_compile((void*)src, (void*)path); }
static void* tramp_compile_sandboxed(const char* src, const char* path, uint32_t caps)
    { return cs_compile_sandboxed((void*)src, (void*)path, caps); }
static void tramp_free_compiled(void* h)
    { cs_free_compiled(h); }
static void* tramp_instantiate_class(void* ch, const char* path)
    { return cs_instantiate_class(ch, (void*)path); }
static void tramp_free_instance(void* raw)
    { cs_free_instance(raw); }
static int tramp_call_method(void* raw, const char* name,
                              PLValue* args, int32_t argc, PLValue* ret)
    { return cs_call_method(raw, (void*)name, args, (int)argc, ret); }
static int tramp_call_builtin(void* raw, int32_t id,
                               PLValue* args, int32_t argc, PLValue* ret)
    { return cs_call_builtin(raw, (int)id, args, (int)argc, ret); }
static int tramp_set_property(void* raw, const char* name, const PLValue* v)
    { return cs_set_property(raw, (void*)name, (PLValue*)v); }
static int tramp_get_property(void* raw, const char* name, PLValue* out)
    { return cs_get_property(raw, (void*)name, out); }
static uint8_t tramp_has_method(void* ch, const char* name)
    { return cs_has_method(ch, (void*)name); }
static void tramp_free_value_contents(PLValue* v)
    { cs_free_value_contents(v); }

PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    memset(out, 0, sizeof(*out));

    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                               | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;

    out->pl_init_runtime       = tramp_init_runtime;
    out->pl_shutdown_runtime   = tramp_shutdown_runtime;
    out->pl_compile            = tramp_compile;
    out->pl_compile_sandboxed  = tramp_compile_sandboxed;
    out->pl_free_compiled      = tramp_free_compiled;
    out->pl_instantiate_class  = tramp_instantiate_class;
    out->pl_free_instance      = tramp_free_instance;
    out->pl_call_method        = tramp_call_method;
    out->pl_call_builtin       = tramp_call_builtin;
    out->pl_set_property       = tramp_set_property;
    out->pl_get_property       = tramp_get_property;
    out->pl_has_method         = tramp_has_method;
    out->pl_free_value_contents= tramp_free_value_contents;
}
