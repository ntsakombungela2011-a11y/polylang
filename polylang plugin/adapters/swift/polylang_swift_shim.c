/*
 * polylang_swift_shim.c — Swift adapter vtable shim for PolyLang v5
 *
 * WHY THIS SHIM EXISTS:
 *   Swift exports symbols via @_silgen_name but cannot reliably fill a C
 *   function-pointer struct from Swift code because the Swift compiler does
 *   not guarantee the ability to take the address of @_silgen_name symbols
 *   for assignment into typed C function pointer fields at Swift compile time.
 *   A thin C translation unit bridges this gap: it declares extern prototypes
 *   for every @_silgen_name Swift export and fills the vtable here.
 *
 * BUILD (macOS/Linux, Swift 5.9+):
 *   swiftc -emit-library -module-name PolyLangSwift \
 *          -Xlinker polylang_swift_shim.c \
 *          -I ../../include \
 *          polylang_swift_adapter.swift \
 *          -o libpolylang_swift.dylib
 *
 *   Or via CMake: add_library(polylang_swift SHARED
 *       polylang_swift_adapter.swift polylang_swift_shim.c)
 */

#include "../../include/pl_adapter_vtable.h"
#include <string.h>

/* ── Prototypes for @_silgen_name Swift exports ── */
extern int    swift_init_runtime(void);
extern void   swift_shutdown_runtime(void);
extern void*  swift_compile(const char* source, const char* path);
extern void*  swift_compile_sandboxed(const char* source, const char* path,
                                       uint32_t caps);
extern void   swift_free_compiled(void* h);
extern void*  swift_instantiate_class(void* ch, const char* path);
extern void   swift_free_instance(void* raw);
extern int    swift_call_method(void* raw, const char* name,
                                PLValue* args, int32_t argc, PLValue* ret);
extern int    swift_call_builtin(void* raw, int32_t id,
                                 PLValue* args, int32_t argc, PLValue* ret);
extern int    swift_set_property(void* raw, const char* name,
                                  const PLValue* v);
extern int    swift_get_property(void* raw, const char* name, PLValue* out);
extern uint8_t swift_has_method(void* ch, const char* name);
extern void   swift_free_value_contents(PLValue* v);

PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    memset(out, 0, sizeof(*out));

    out->abi_version          = PL_ABI_VERSION;
    /* Desktop-only: Swift runtime is not available on Android or standard Linux */
    out->capabilities         = PL_CAP_DESKTOP | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;

    out->pl_init_runtime       = swift_init_runtime;
    out->pl_shutdown_runtime   = swift_shutdown_runtime;
    out->pl_compile            = swift_compile;
    out->pl_compile_sandboxed  = swift_compile_sandboxed;
    out->pl_free_compiled      = swift_free_compiled;
    out->pl_instantiate_class  = swift_instantiate_class;
    out->pl_free_instance      = swift_free_instance;
    out->pl_call_method        = swift_call_method;
    out->pl_call_builtin       = swift_call_builtin;
    out->pl_set_property       = swift_set_property;
    out->pl_get_property       = swift_get_property;
    out->pl_has_method         = swift_has_method;
    out->pl_free_value_contents= swift_free_value_contents;
}
