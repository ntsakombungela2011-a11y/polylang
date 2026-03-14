// =============================================================
// polylang_odin_script_api.odin  —  PolyLang v6.3 Script Protocol
// Package: main   (included in every user script package)
// =============================================================
// This file defines:
//   1. The OdinScript interface type — users embed this and override methods.
//   2. The C-exported wrapper functions that the C++ adapter calls.
//      These are the mandatory and optional exports listed in OdinScriptAPI
//      in polylang_odin_adapter.cpp.
//   3. Default implementations that call the user's OdinScript vtable.
//
// USER SCRIPT EXAMPLE:
//
//   package main
//
//   import pl "polylang"
//
//   // The script struct
//   Enemy :: struct {
//       using script: pl.OdinScript,   // embeds the interface
//       health: i64,
//       speed:  f64,
//   }
//
//   // Create a new instance — must be implemented by user.
//   // The polylang_script var below routes here.
//   enemy_create :: proc() -> ^Enemy {
//       e := new(Enemy)
//       e.health = 100
//       e.speed  = 5.0
//       e._vtable = {
//           call_method   = enemy_call_method,
//           set_property  = enemy_set_property,
//           get_property  = enemy_get_property,
//           has_method    = enemy_has_method,
//       }
//       return e
//   }
//
//   // Bind the factory. The rest of the API is wired up automatically.
//   polylang_script_factory :: #procedure_of_type(proc() -> rawptr) {
//       return enemy_create()
//   }
//
// BUILD:
//   odin build . -build-mode:shared -out:Enemy.pl.odin.so
//   (polylang_odin_shim.odin and this file are compiled in the same package)
// =============================================================
package main

import pl "polylang"
import "core:c"
import "core:mem"

// ── OdinScript interface vtable ───────────────────────────────
// User scripts set these fields when constructing their script struct.

OdinScriptVTable :: struct {
    call_method:   proc(inst: rawptr, name: cstring, args: [^]pl.PLValue, argc: c.int32_t, ret: ^pl.PLValue) -> c.int,
    set_property:  proc(inst: rawptr, name: cstring, v: ^pl.PLValue) -> c.int,
    get_property:  proc(inst: rawptr, name: cstring, out: ^pl.PLValue) -> c.int,
    has_method:    proc(name: cstring) -> c.uint8_t,
    // Optional
    get_method_list:    proc(out: ^[^]pl.PLMethodInfo, count: ^c.int32_t),
    get_property_list:  proc(out: ^[^]pl.PLPropertyInfo, count: ^c.int32_t),
    serialize_state:    proc(inst: rawptr, out: ^pl.PLValue) -> c.int,
    deserialize_state:  proc(inst: rawptr, state: ^pl.PLValue) -> c.int,
    get_export_vars:    proc(out: ^[^]pl.PLExportVarInfo, count: ^c.int32_t),
    // Coroutine
    coroutine_create:   proc(inst: rawptr, method: cstring) -> rawptr,
    coroutine_resume:   proc(coro: rawptr, send: ^pl.PLValue, yield_out: ^pl.PLValue) -> c.int,
    coroutine_free:     proc(coro: rawptr),
    // Async
    async_begin:        proc(inst: rawptr, method: cstring, args: [^]pl.PLValue, argc: c.int32_t) -> rawptr,
    async_poll:         proc(future: rawptr, result_out: ^pl.PLValue) -> c.int,
    async_free:         proc(future: rawptr),
}

OdinScript :: struct {
    _vtable: OdinScriptVTable,
}

// ── Script factory: user script must set this ─────────────────
// polylang_script_factory is a package-level variable pointing to the
// user's factory proc.  User scripts assign to it at package init:
//
//   @(init)
//   register_factory :: proc() { polylang_script_factory = my_create }
//
// This is the idiomatic Odin alternative to a global __attribute__((constructor)).

@(export, link_name="polylang_script_factory")
polylang_script_factory: proc "c" () -> rawptr

// ── Mandatory C exports ───────────────────────────────────────

@(export, link_name="odin_script_create_instance")
odin_script_create_instance :: proc "c" () -> rawptr {
    context = runtime.default_context()
    if polylang_script_factory == nil { return nil }
    return polylang_script_factory()
}

@(export, link_name="odin_script_free_instance")
odin_script_free_instance :: proc "c" (inst: rawptr) {
    context = runtime.default_context()
    if inst == nil { return }
    // User is responsible for correct cast. We call free on the raw pointer.
    // Script structs must be heap-allocated via new().
    s := cast(^OdinScript)inst
    _ = s // suppresses unused
    mem.heap_free(inst)
}

@(export, link_name="odin_script_call_method")
odin_script_call_method :: proc "c" (inst: rawptr, name: cstring,
    args: [^]pl.PLValue, argc: c.int32_t, ret: ^pl.PLValue) -> c.int {
    context = runtime.default_context()
    if inst == nil || ret == nil { return pl.PL_ERR_GENERIC }
    ret^ = pl.make_nil()
    s := cast(^OdinScript)inst
    if s._vtable.call_method == nil { return pl.PL_ERR_NOT_IMPLEMENTED }
    return s._vtable.call_method(inst, name, args, argc, ret)
}

@(export, link_name="odin_script_set_property")
odin_script_set_property :: proc "c" (inst: rawptr, name: cstring, v: ^pl.PLValue) -> c.int {
    context = runtime.default_context()
    if inst == nil { return pl.PL_ERR_GENERIC }
    s := cast(^OdinScript)inst
    if s._vtable.set_property == nil { return pl.PL_ERR_NOT_IMPLEMENTED }
    return s._vtable.set_property(inst, name, v)
}

@(export, link_name="odin_script_get_property")
odin_script_get_property :: proc "c" (inst: rawptr, name: cstring, out: ^pl.PLValue) -> c.int {
    context = runtime.default_context()
    if inst == nil || out == nil { return pl.PL_ERR_GENERIC }
    out^ = pl.make_nil()
    s := cast(^OdinScript)inst
    if s._vtable.get_property == nil { return pl.PL_ERR_PROP_NOT_FOUND }
    return s._vtable.get_property(inst, name, out)
}

@(export, link_name="odin_script_has_method")
odin_script_has_method :: proc "c" (name: cstring) -> c.uint8_t {
    context = runtime.default_context()
    // Default: create a temporary instance and query.
    // Scripts can override by setting _vtable.has_method at the package level.
    if polylang_script_factory == nil { return 0 }
    inst := polylang_script_factory()
    if inst == nil { return 0 }
    defer mem.heap_free(inst)
    s := cast(^OdinScript)inst
    if s._vtable.has_method == nil { return 0 }
    return s._vtable.has_method(name)
}

// ── Optional C exports ────────────────────────────────────────

@(export, link_name="odin_script_get_method_list")
odin_script_get_method_list :: proc "c" (out: ^[^]pl.PLMethodInfo, count: ^c.int32_t) {
    context = runtime.default_context()
    if out == nil || count == nil { return }
    out^   = nil
    count^ = 0
    if polylang_script_factory == nil { return }
    inst := polylang_script_factory()
    if inst == nil { return }
    defer mem.heap_free(inst)
    s := cast(^OdinScript)inst
    if s._vtable.get_method_list == nil { return }
    s._vtable.get_method_list(out, count)
}

@(export, link_name="odin_script_free_method_list")
odin_script_free_method_list :: proc "c" (list: [^]pl.PLMethodInfo) {
    context = runtime.default_context()
    mem.heap_free(list)
}

@(export, link_name="odin_script_get_property_list")
odin_script_get_property_list :: proc "c" (out: ^[^]pl.PLPropertyInfo, count: ^c.int32_t) {
    context = runtime.default_context()
    if out == nil || count == nil { return }
    out^   = nil
    count^ = 0
    if polylang_script_factory == nil { return }
    inst := polylang_script_factory()
    if inst == nil { return }
    defer mem.heap_free(inst)
    s := cast(^OdinScript)inst
    if s._vtable.get_property_list == nil { return }
    s._vtable.get_property_list(out, count)
}

@(export, link_name="odin_script_free_property_list")
odin_script_free_property_list :: proc "c" (list: [^]pl.PLPropertyInfo) {
    context = runtime.default_context()
    mem.heap_free(list)
}

@(export, link_name="odin_script_serialize_state")
odin_script_serialize_state :: proc "c" (inst: rawptr, out: ^pl.PLValue) -> c.int {
    context = runtime.default_context()
    if inst == nil || out == nil { return pl.PL_ERR_GENERIC }
    out^ = pl.make_nil()
    s := cast(^OdinScript)inst
    if s._vtable.serialize_state == nil { return pl.PL_ERR_NOT_IMPLEMENTED }
    return s._vtable.serialize_state(inst, out)
}

@(export, link_name="odin_script_deserialize_state")
odin_script_deserialize_state :: proc "c" (inst: rawptr, state: ^pl.PLValue) -> c.int {
    context = runtime.default_context()
    if inst == nil || state == nil { return pl.PL_ERR_GENERIC }
    s := cast(^OdinScript)inst
    if s._vtable.deserialize_state == nil { return pl.PL_ERR_NOT_IMPLEMENTED }
    return s._vtable.deserialize_state(inst, state)
}

@(export, link_name="odin_script_get_export_vars")
odin_script_get_export_vars :: proc "c" (out: ^[^]pl.PLExportVarInfo, count: ^c.int32_t) {
    context = runtime.default_context()
    if out == nil || count == nil { return }
    out^   = nil
    count^ = 0
    if polylang_script_factory == nil { return }
    inst := polylang_script_factory()
    if inst == nil { return }
    defer mem.heap_free(inst)
    s := cast(^OdinScript)inst
    if s._vtable.get_export_vars == nil { return }
    s._vtable.get_export_vars(out, count)
}

@(export, link_name="odin_script_free_export_vars")
odin_script_free_export_vars :: proc "c" (vars: [^]pl.PLExportVarInfo, count: c.int32_t) {
    context = runtime.default_context()
    if vars == nil { return }
    for i in 0..<int(count) { pl.free_value(&vars[i].default_val) }
    mem.heap_free(vars)
}

// ── Coroutine C exports ───────────────────────────────────────

@(export, link_name="odin_script_coroutine_create")
odin_script_coroutine_create :: proc "c" (inst: rawptr, method: cstring) -> rawptr {
    context = runtime.default_context()
    if inst == nil { return nil }
    s := cast(^OdinScript)inst
    if s._vtable.coroutine_create == nil { return nil }
    return s._vtable.coroutine_create(inst, method)
}

@(export, link_name="odin_script_coroutine_resume")
odin_script_coroutine_resume :: proc "c" (coro: rawptr, send: ^pl.PLValue, yield_out: ^pl.PLValue) -> c.int {
    context = runtime.default_context()
    if coro == nil { return pl.PL_ERR_CORO_DEAD }
    // coro is an ^pl.OdinCoroutine — but the C++ adapter tags it.
    // Here we receive the inner handle (the C++ adapter unwraps the tag before calling).
    odin_coro := cast(^pl.OdinCoroutine)coro
    send_val: pl.PLValue
    if send != nil { send_val = send^ } else { send_val = pl.make_nil() }
    return c.int(pl.coroutine_resume(odin_coro, send_val, yield_out))
}

@(export, link_name="odin_script_coroutine_free")
odin_script_coroutine_free :: proc "c" (coro: rawptr) {
    context = runtime.default_context()
    pl.coroutine_free(cast(^pl.OdinCoroutine)coro)
}

// ── Async C exports ───────────────────────────────────────────

@(export, link_name="odin_script_async_begin")
odin_script_async_begin :: proc "c" (inst: rawptr, method: cstring,
    args: [^]pl.PLValue, argc: c.int32_t) -> rawptr {
    context = runtime.default_context()
    if inst == nil { return nil }
    s := cast(^OdinScript)inst
    if s._vtable.async_begin == nil { return nil }
    return s._vtable.async_begin(inst, method, args, argc)
}

@(export, link_name="odin_script_async_poll")
odin_script_async_poll :: proc "c" (future: rawptr, result_out: ^pl.PLValue) -> c.int {
    context = runtime.default_context()
    if future == nil { return pl.PL_ERR_GENERIC }
    f := cast(^pl.OdinFuture)future
    return c.int(pl.async_poll(f, result_out))
}

@(export, link_name="odin_script_async_free")
odin_script_async_free :: proc "c" (future: rawptr) {
    context = runtime.default_context()
    pl.async_free(cast(^pl.OdinFuture)future)
}
