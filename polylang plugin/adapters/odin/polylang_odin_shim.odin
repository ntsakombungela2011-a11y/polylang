// =============================================================
// polylang_odin_shim.odin  —  PolyLang v6.3 Odin Runtime Shim
// Package: polylang
// =============================================================
// This package is compiled as a static library (odin build -build-mode:obj)
// and linked into every user .pl.odin script shared library.
//
// User scripts import it:
//   import pl "polylang"   // path resolved relative to script at build time
//
// The shim stores runtime service function pointers injected by the
// C++ adapter immediately after each script .so is loaded. This is
// done through odin_script_init_runtime_services() which every
// compiled script .so must export (the script_api.odin file below
// provides the default export that routes here).
//
// COROUTINE MODEL:
//   Odin does not have native coroutines. The shim implements them via
//   POSIX ucontext_t (Linux/Android) or platform-equivalent. Each
//   coroutine gets a dedicated stack (default 256 KB). The adapter's
//   pl_coroutine_create() calls odin_script_coroutine_create() in the
//   compiled script .so which in turn calls polylang.coroutine_create()
//   from this shim.
//
// ASYNC MODEL:
//   Async procs run on a background thread (via core:thread). The future
//   handle holds the thread handle + result storage. pl_async_poll()
//   checks if the thread is done and extracts the result.
//
// =============================================================
package polylang

import "core:c"
import "core:fmt"
import "core:mem"
import "core:strings"
import "core:sync"
import "core:thread"
import "core:sys/unix"
import "core:runtime"

// ── PLValue mirror — must match pl_adapter_vtable.h exactly ──

PLValueType :: enum c.int {
    Nil      = 0,
    Bool     = 1,
    Int      = 2,
    Float    = 3,
    String   = 4,
    Vec2     = 5,
    Vec3     = 6,
    Quat     = 7,
    Object   = 8,
    Array    = 9,
    Dict     = 10,
    Resource = 11,
}

PLArrayData :: struct {
    data: [^]PLValue,
    len:  c.int32_t,
    _cap: c.int32_t,
}

PLDictData :: struct {
    keys:   [^]PLValue,
    values: [^]PLValue,
    len:    c.int32_t,
    _cap:   c.int32_t,
}

PLValueUnion :: struct #raw_union {
    b:       bool,
    i:       c.int64_t,
    f:       c.double,
    s:       cstring,
    v2:      [2]c.float,
    v3:      [3]c.float,
    q4:      [4]c.float,
    obj_ptr: rawptr,
    array:   PLArrayData,
    dict:    PLDictData,
    _raw:    [24]u8,
}

PLValue :: struct #align(8) {
    type: c.int32_t,
    _pad: c.int32_t,
    using _union: PLValueUnion,
}
#assert(size_of(PLValue) == 32)

PLMethodInfo :: struct {
    name:      cstring,
    arg_count: c.int32_t,
    _pad:      c.int32_t,
}

PLPropertyInfo :: struct {
    name:      cstring,
    type_hint: c.int32_t,
    _pad:      c.int32_t,
}

PLExportVarInfo :: struct {
    name:        cstring,
    type_hint:   c.int32_t,
    _pad:        c.int32_t,
    default_val: PLValue,
}

// ── Return codes ──────────────────────────────────────────────
PL_OK                 :: 0
PL_ERR_GENERIC        :: -1
PL_ERR_METHOD_NOT_FOUND :: -2
PL_ERR_PROP_NOT_FOUND :: -3
PL_ERR_EXCEPTION      :: -5
PL_ERR_NOT_IMPLEMENTED:: -6
PL_ERR_SANDBOX        :: -8
PL_ERR_CORO_DEAD      :: -9
PL_ERR_ASYNC_PENDING  :: 1

PL_CORO_SUSPENDED :: 0
PL_CORO_DONE      :: 1
PL_CORO_FAILED    :: -1

// Sandbox capability flags
PL_SANDBOX_NONE       :: 0
PL_SANDBOX_FILE_READ  :: (1 << 0)
PL_SANDBOX_FILE_WRITE :: (1 << 1)
PL_SANDBOX_NETWORK    :: (1 << 2)
PL_SANDBOX_PROCESS    :: (1 << 3)
PL_SANDBOX_FULL       :: 0xFFFFFFFF

// ── Runtime services table — filled by C++ adapter ───────────

OdinRuntimeServices :: struct {
    signal_emit:        proc "c" (name: cstring, args: [^]PLValue, argc: c.int32_t),
    signal_connect:     proc "c" (name: cstring,
                                  cb:   proc "c" (args: [^]PLValue, argc: c.int32_t, userdata: rawptr),
                                  ud:   rawptr) -> c.uint64_t,
    signal_disconnect:  proc "c" (id: c.uint64_t),
    bridge_call:        proc "c" (path: cstring, method: cstring,
                                  args: [^]PLValue, argc: c.int32_t, ret: ^PLValue) -> c.int,
    resource_fetch:     proc "c" (path: cstring, out: ^PLValue) -> c.int,
    resource_release:   proc "c" (v: ^PLValue),
    profiler_begin:     proc "c" (label: cstring),
    profiler_end:       proc "c" (label: cstring),
}

// Package-level pointer set once by odin_script_init_runtime_services.
@(private)
g_services: ^OdinRuntimeServices

@(export, link_name="odin_script_init_runtime_services")
init_runtime_services :: proc "c" (svc: ^OdinRuntimeServices) {
    g_services = svc
}

// ── PLValue constructors ──────────────────────────────────────

make_nil :: proc() -> PLValue {
    v: PLValue
    v.type = c.int32_t(PLValueType.Nil)
    return v
}

make_bool :: proc(b: bool) -> PLValue {
    v: PLValue
    v.type = c.int32_t(PLValueType.Bool)
    v.b    = b
    return v
}

make_int :: proc(i: i64) -> PLValue {
    v: PLValue
    v.type = c.int32_t(PLValueType.Int)
    v.i    = c.int64_t(i)
    return v
}

make_float :: proc(f: f64) -> PLValue {
    v: PLValue
    v.type = c.int32_t(PLValueType.Float)
    v.f    = c.double(f)
    return v
}

// Allocates a malloc'd copy of the string. Caller owns the PLValue.
make_string :: proc(s: string) -> PLValue {
    v: PLValue
    v.type = c.int32_t(PLValueType.String)
    cs := strings.clone_to_cstring(s, mem.heap_allocator())
    v.s = cs
    return v
}

make_vec2 :: proc(x, y: f32) -> PLValue {
    v: PLValue
    v.type  = c.int32_t(PLValueType.Vec2)
    v.v2[0] = c.float(x)
    v.v2[1] = c.float(y)
    return v
}

make_vec3 :: proc(x, y, z: f32) -> PLValue {
    v: PLValue
    v.type  = c.int32_t(PLValueType.Vec3)
    v.v3[0] = c.float(x)
    v.v3[1] = c.float(y)
    v.v3[2] = c.float(z)
    return v
}

// ── PLValue extractors ────────────────────────────────────────

as_bool :: proc(v: ^PLValue) -> (bool, bool) {
    if PLValueType(v.type) != .Bool { return false, false }
    return v.b, true
}

as_int :: proc(v: ^PLValue) -> (i64, bool) {
    if PLValueType(v.type) != .Int { return 0, false }
    return i64(v.i), true
}

as_float :: proc(v: ^PLValue) -> (f64, bool) {
    switch PLValueType(v.type) {
    case .Float: return f64(v.f), true
    case .Int:   return f64(v.i), true
    case:        return 0, false
    }
}

as_string :: proc(v: ^PLValue) -> (string, bool) {
    if PLValueType(v.type) != .String || v.s == nil { return "", false }
    return string(v.s), true
}

as_vec2 :: proc(v: ^PLValue) -> ([2]f32, bool) {
    if PLValueType(v.type) != .Vec2 { return {}, false }
    return [2]f32{f32(v.v2[0]), f32(v.v2[1])}, true
}

as_vec3 :: proc(v: ^PLValue) -> ([3]f32, bool) {
    if PLValueType(v.type) != .Vec3 { return {}, false }
    return [3]f32{f32(v.v3[0]), f32(v.v3[1]), f32(v.v3[2])}, true
}

// Free string or array contents.
free_value :: proc(v: ^PLValue) {
    switch PLValueType(v.type) {
    case .String:
        if v.s != nil { mem.heap_free(rawptr(v.s)); v.s = nil }
    case .Array:
        if v.array.data != nil {
            for i in 0..<v.array.len { free_value(&v.array.data[i]) }
            mem.heap_free(v.array.data); v.array.data = nil
        }
    case .Dict:
        if v.dict.keys != nil {
            for i in 0..<v.dict.len {
                free_value(&v.dict.keys[i]); free_value(&v.dict.values[i])
            }
            mem.heap_free(v.dict.keys); v.dict.keys = nil
            mem.heap_free(v.dict.values); v.dict.values = nil
        }
    case: // nothing to free for scalars
    }
    v.type = c.int32_t(PLValueType.Nil)
}

// ── SignalBus ─────────────────────────────────────────────────

// Emit a named signal. Thread-safe: PLSignalBus queues off-main emissions.
emit_signal :: proc(name: string, args: []PLValue) {
    if g_services == nil || g_services.signal_emit == nil { return }
    cs := strings.clone_to_cstring(name, context.temp_allocator)
    raw_args := raw_data(args)
    g_services.signal_emit(cs, raw_args, c.int32_t(len(args)))
}

// Connect a proc to a named signal. Returns a listener ID for disconnect.
// The callback receives a slice of PLValues. Lifetime: until disconnect.
connect_signal :: proc(name: string, cb: proc(args: []PLValue)) -> u64 {
    if g_services == nil || g_services.signal_connect == nil { return 0 }

    // Allocate a heap closure that wraps the Odin proc.
    Closure :: struct { cb: proc(args: []PLValue) }
    cl := new(Closure, mem.heap_allocator())
    cl.cb = cb

    c_cb :: proc "c" (args: [^]PLValue, argc: c.int32_t, userdata: rawptr) {
        context = runtime.default_context()
        cl := cast(^Closure)userdata
        cl.cb(args[:argc])
    }

    cs := strings.clone_to_cstring(name, context.temp_allocator)
    return u64(g_services.signal_connect(cs, c_cb, cl))
}

disconnect_signal :: proc(listener_id: u64) {
    if g_services != nil && g_services.signal_disconnect != nil {
        g_services.signal_disconnect(c.uint64_t(listener_id))
    }
}

// ── Cross-language bridge ─────────────────────────────────────

// Call a method on another PolyLang script by res:// path.
// Returns PL_OK on success; ret_out receives the result (caller must free_value).
call_script :: proc(target_path: string, method: string,
                    args: []PLValue, ret_out: ^PLValue) -> int {
    if g_services == nil || g_services.bridge_call == nil {
        if ret_out != nil { ret_out^ = make_nil() }
        return PL_ERR_NOT_IMPLEMENTED
    }
    cs_path   := strings.clone_to_cstring(target_path, context.temp_allocator)
    cs_method := strings.clone_to_cstring(method, context.temp_allocator)
    raw_args  := raw_data(args)
    v: PLValue
    rc := g_services.bridge_call(cs_path, cs_method, raw_args, c.int32_t(len(args)), &v)
    if ret_out != nil { ret_out^ = v }
    return int(rc)
}

// ── Resource bridge ───────────────────────────────────────────

// Load a Godot resource into a PLValue (type = Resource).
// The caller must call release_resource() when done.
load_resource :: proc(res_path: string) -> (PLValue, bool) {
    if g_services == nil || g_services.resource_fetch == nil {
        return make_nil(), false
    }
    cs := strings.clone_to_cstring(res_path, context.temp_allocator)
    v: PLValue
    rc := g_services.resource_fetch(cs, &v)
    return v, rc == 0
}

release_resource :: proc(v: ^PLValue) {
    if g_services != nil && g_services.resource_release != nil {
        g_services.resource_release(v)
    }
    v^ = make_nil()
}

// ── Profiler ──────────────────────────────────────────────────

profiler_scope_begin :: proc(label: string) {
    if g_services == nil || g_services.profiler_begin == nil { return }
    cs := strings.clone_to_cstring(label, context.temp_allocator)
    g_services.profiler_begin(cs)
}

profiler_scope_end :: proc(label: string) {
    if g_services == nil || g_services.profiler_end == nil { return }
    cs := strings.clone_to_cstring(label, context.temp_allocator)
    g_services.profiler_end(cs)
}

// ── Coroutine support (ucontext on Linux/Android) ─────────────
// Odin on Linux exposes sys/unix. We use makecontext/swapcontext.

when ODIN_OS == .Linux || ODIN_OS == .Android {
    import unix "core:sys/unix"

    CORO_STACK_SIZE :: 256 * 1024

    CoroStatus :: enum { Suspended, Done, Failed }

    OdinCoroutine :: struct {
        status:    CoroStatus,
        caller_ctx: unix.ucontext_t,
        coro_ctx:   unix.ucontext_t,
        stack:      []u8,
        send_val:   PLValue,
        yield_val:  PLValue,
        error_msg:  string,
        body:       proc(coro: ^OdinCoroutine),
    }

    // Yield from inside a coroutine body. Sends yield_val back to caller.
    coro_yield :: proc(coro: ^OdinCoroutine, val: PLValue) {
        coro.yield_val = val
        coro.status    = .Suspended
        unix.swapcontext(&coro.coro_ctx, &coro.caller_ctx)
    }

    // Retrieve the send value delivered by the last resume call.
    coro_recv :: proc(coro: ^OdinCoroutine) -> PLValue {
        return coro.send_val
    }

    @(private)
    coro_trampoline :: proc "c" (arg: rawptr) {
        context = runtime.default_context()
        coro := cast(^OdinCoroutine)arg
        coro.body(coro)
        coro.status = .Done
        unix.swapcontext(&coro.coro_ctx, &coro.caller_ctx)
    }

    coroutine_create :: proc(body: proc(coro: ^OdinCoroutine)) -> ^OdinCoroutine {
        coro := new(OdinCoroutine, mem.heap_allocator())
        coro.status = .Suspended
        coro.body   = body
        coro.stack  = make([]u8, CORO_STACK_SIZE, mem.heap_allocator())

        unix.getcontext(&coro.coro_ctx)
        coro.coro_ctx.uc_stack.ss_sp   = raw_data(coro.stack)
        coro.coro_ctx.uc_stack.ss_size = uint(CORO_STACK_SIZE)
        coro.coro_ctx.uc_link          = nil
        unix.makecontext(&coro.coro_ctx, coro_trampoline, 1, rawptr(coro))
        return coro
    }

    // Resume a coroutine, delivering send_val. Returns PL_CORO_SUSPENDED,
    // PL_CORO_DONE, or PL_CORO_FAILED. Fills yield_out with the yielded value.
    coroutine_resume :: proc(coro: ^OdinCoroutine, send_val: PLValue,
                              yield_out: ^PLValue) -> int {
        if coro.status == .Done   { return PL_CORO_DONE }
        if coro.status == .Failed { return PL_CORO_FAILED }
        coro.send_val = send_val
        coro.status   = .Suspended
        unix.swapcontext(&coro.caller_ctx, &coro.coro_ctx)
        if yield_out != nil { yield_out^ = coro.yield_val }
        switch coro.status {
        case .Done:      return PL_CORO_DONE
        case .Failed:    return PL_CORO_FAILED
        case .Suspended: return PL_CORO_SUSPENDED
        }
        return PL_CORO_SUSPENDED
    }

    coroutine_free :: proc(coro: ^OdinCoroutine) {
        if coro == nil { return }
        delete(coro.stack, mem.heap_allocator())
        free(coro, mem.heap_allocator())
    }
} // when Linux || Android

// ── Async support ─────────────────────────────────────────────

AsyncResult :: struct {
    value: PLValue,
    done:  bool,
    err:   string,
}

OdinFuture :: struct {
    t:      ^thread.Thread,
    result: AsyncResult,
    mu:     sync.Mutex,
}

// Run a proc asynchronously. Returns a future handle.
async_run :: proc(body: proc() -> PLValue) -> ^OdinFuture {
    f := new(OdinFuture, mem.heap_allocator())
    f.result.done = false

    ThreadData :: struct { f: ^OdinFuture; body: proc() -> PLValue }
    td := new(ThreadData, mem.heap_allocator())
    td.f    = f
    td.body = body

    worker :: proc(data: rawptr) {
        context = runtime.default_context()
        td := cast(^ThreadData)data
        v  := td.body()
        sync.lock(&td.f.mu)
        td.f.result.value = v
        td.f.result.done  = true
        sync.unlock(&td.f.mu)
        free(td, mem.heap_allocator())
    }

    f.t = thread.create_and_start_with_data(td, worker, context)
    return f
}

// Poll a future. Returns PL_OK + result if done, PL_ERR_ASYNC_PENDING if not.
async_poll :: proc(f: ^OdinFuture, result_out: ^PLValue) -> int {
    if f == nil { return PL_ERR_GENERIC }
    sync.lock(&f.mu)
    done := f.result.done
    if done && result_out != nil { result_out^ = f.result.value }
    sync.unlock(&f.mu)
    return done ? PL_OK : PL_ERR_ASYNC_PENDING
}

async_free :: proc(f: ^OdinFuture) {
    if f == nil { return }
    if f.t != nil { thread.join(f.t); thread.destroy(f.t) }
    free(f, mem.heap_allocator())
}

// ── Sandbox guard ─────────────────────────────────────────────
// Scripts can call this to enforce finer-grained sandbox rules
// from Odin code (e.g. if the script itself knows it is sandboxed).
//
// g_sandbox_caps is set by the C++ adapter via odin_script_set_sandbox_caps()
// which every compiled script .so must export (provided by script_api.odin).

@(private)
g_sandbox_caps: u32 = 0xFFFFFFFF   // default: full (trusted)

@(export, link_name="odin_script_set_sandbox_caps")
set_sandbox_caps :: proc "c" (caps: c.uint32_t) {
    g_sandbox_caps = u32(caps)
}

sandbox_check :: proc(required_cap: u32) -> bool {
    return (g_sandbox_caps & required_cap) != 0
}

// Convenience wrappers for common checks
can_read_files  :: proc() -> bool { return sandbox_check(PL_SANDBOX_FILE_READ)  }
can_write_files :: proc() -> bool { return sandbox_check(PL_SANDBOX_FILE_WRITE) }
can_network     :: proc() -> bool { return sandbox_check(PL_SANDBOX_NETWORK)    }
can_spawn_procs :: proc() -> bool { return sandbox_check(PL_SANDBOX_PROCESS)    }
