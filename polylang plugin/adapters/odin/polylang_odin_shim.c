/*
 * polylang_odin_shim.c — PolyLang v6.3 Odin Adapter C Shim
 *
 * PURPOSE:
 *   Odin's foreign export mechanism (@(export) proc) cannot assign
 *   exported procedure addresses to C function-pointer struct fields
 *   from within Odin code itself (analogous to the Go CGo and Swift
 *   @_silgen_name patterns). The canonical solution is this plain C
 *   file that:
 *     1. Declares extern prototypes for odin_fill_vtable (defined in
 *        polylang_odin_adapter.cpp).
 *     2. Defines pl_get_vtable() here in C, delegating to odin_fill_vtable.
 *
 *   polylang_odin_adapter.cpp must NOT define pl_get_vtable(); only
 *   this shim does, so the linker resolves exactly one definition.
 *
 * BUILD:
 *   This file is compiled together with polylang_odin_adapter.cpp into
 *   libpolylang_odin.so by CMake:
 *
 *   add_library(polylang_odin SHARED
 *       adapters/odin/polylang_odin_adapter.cpp
 *       adapters/odin/polylang_odin_shim.c)
 *   target_include_directories(polylang_odin PRIVATE include)
 *
 * ANDROID:
 *   Same file is used for the Android arm64-v8a build. The NDK toolchain
 *   compiles it as part of the CMake cross-compile step.
 */

#include "../../include/pl_adapter_vtable.h"
#include <string.h>

/* Defined in polylang_odin_adapter.cpp */
extern void odin_fill_vtable(PLAdapterVTable* out);

PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    odin_fill_vtable(out);
}
