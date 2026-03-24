/*
 * polylang_go_shim.c — CGO vtable shim for PolyLang Go adapter v5
 *
 * PURPOSE:
 *   CGO does not allow assigning //export'd Go function addresses to C
 *   function pointer fields from within Go code. The canonical solution
 *   is a plain C file in the same package that:
 *     1. Declares extern prototypes for each //export'd Go function.
 *     2. Defines pl_get_vtable() and fills every vtable field here.
 *
 *   The Go source file (polylang_go_adapter.go) must NOT define
 *   pl_get_vtable(); only this shim does.
 *
 * BUILD:
 *   go build -buildmode=c-shared -o libpolylang_go.so .
 *   Both .go and .c files in the package directory are compiled together.
 */

#include "../../include/pl_adapter_vtable.h"
#include <string.h>

/* Prototypes for //export'd Go functions */
extern int          go_init_runtime(void);
extern void         go_shutdown_runtime(void);
extern void*        go_compile(const char* source, const char* path);
extern void*        go_compile_sandboxed(const char* source, const char* path, uint32_t caps);
extern void         go_free_compiled(void* h);
extern void*        go_instantiate_class(void* ch, const char* path);
extern void         go_free_instance(void* raw);
extern int          go_call_method(void* raw, const char* name,
                                   PLValue* args, int32_t argc, PLValue* ret);
extern int          go_call_builtin(void* raw, int32_t id,
                                    PLValue* args, int32_t argc, PLValue* ret);
extern int          go_set_property(void* raw, const char* name, const PLValue* v);
extern int          go_get_property(void* raw, const char* name, PLValue* out);
extern uint8_t      go_has_method(void* ch, const char* name);
extern void         go_free_value_contents(PLValue* v);

PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    memset(out, 0, sizeof(*out));

    out->abi_version          = PL_ABI_VERSION;
    out->capabilities         = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                               | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;

    out->pl_init_runtime       = go_init_runtime;
    out->pl_shutdown_runtime   = go_shutdown_runtime;
    out->pl_compile            = go_compile;
    out->pl_compile_sandboxed  = go_compile_sandboxed;
    out->pl_free_compiled      = go_free_compiled;
    out->pl_instantiate_class  = go_instantiate_class;
    out->pl_free_instance      = go_free_instance;
    out->pl_call_method        = go_call_method;
    out->pl_call_builtin       = go_call_builtin;
    out->pl_set_property       = go_set_property;
    out->pl_get_property       = go_get_property;
    out->pl_has_method         = go_has_method;
    out->pl_free_value_contents= go_free_value_contents;
}
