// polylang_zig_adapter/src/main.zig — PolyLang Zig Adapter v5
// =============================================================
// SANDBOX:   pl_compile_sandboxed sets sandboxed=true on the compiled
//            handle. At method dispatch, calls matching a deny-list of
//            std.fs/std.process/std.os names return PL_ERR_SANDBOX.
//            Zig itself provides no runtime sandbox; this is advisory.
// =============================================================
const std = @import("std");
const c   = @cImport({ @cInclude("pl_adapter_vtable.h"); });

// ── Constants ────────────────────────────────────────────────
const PL_OK:                   i32 = 0;
const PL_ERR_GENERIC:          i32 = -1;
const PL_ERR_METHOD_NOT_FOUND: i32 = -2;
const PL_ERR_PROP_NOT_FOUND:   i32 = -3;
const PL_ERR_EXCEPTION:        i32 = -5;
const PL_ERR_NOT_IMPLEMENTED:  i32 = -6;
const PL_ERR_SANDBOX:          i32 = -8;

const PL_TYPE_NIL:    i32 = 0;
const PL_TYPE_BOOL:   i32 = 1;
const PL_TYPE_INT:    i32 = 2;
const PL_TYPE_FLOAT:  i32 = 3;
const PL_TYPE_STRING: i32 = 4;

const PL_CAP_ANDROID:      u32 = 1 << 0;
const PL_CAP_IOS:          u32 = 1 << 1;
const PL_CAP_DESKTOP:      u32 = 1 << 2;
const PL_CAP_BUILTIN_CALL: u32 = 1 << 6;
const PL_CAP_SANDBOX:      u32 = 1 << 7;

const PL_METHOD_READY:           i32 = 1;
const PL_METHOD_PROCESS:         i32 = 2;
const PL_METHOD_PHYSICS_PROCESS: i32 = 3;
const PL_METHOD_ENTER_TREE:      i32 = 4;
const PL_METHOD_EXIT_TREE:       i32 = 5;

// ── Sandbox deny-list ────────────────────────────────────────
const sandbox_denied = [_][]const u8{
    "openFile", "writeFile", "deleteFile", "createDir", "removeDir",
    "execCommand", "spawnProcess", "runCommand",
    "connectSocket", "bindSocket", "sendData", "recvData",
    "getEnvVar", "setEnvVar",
};

fn isSandboxDenied(name: []const u8) bool {
    for (sandbox_denied) |d| {
        if (std.ascii.eqlIgnoreCase(name[0..@min(name.len, d.len)], d))
            return true;
    }
    return false;
}

// ── Type registry ─────────────────────────────────────────────
pub const MethodFn = *const fn (self: *anyopaque, args: []const c.PLValue) c.PLValue;

pub const MethodEntry = struct {
    name: []const u8,
    func: MethodFn,
};

pub const ZigTypeInfo = struct {
    class_name: []const u8,
    create_fn:  *const fn () *anyopaque,
    destroy_fn: *const fn (ptr: *anyopaque) void,
    methods:    []const MethodEntry,
};

var type_registry: std.StringHashMap(*const ZigTypeInfo) = undefined;
var registry_lock: std.Thread.Mutex = .{};
var gpa_instance: std.heap.GeneralPurposeAllocator(.{}) = .{};
var gpa: std.mem.Allocator = undefined;
var runtime_inited: bool = false;

pub fn registerZigType(info: *const ZigTypeInfo) void {
    registry_lock.lock();
    defer registry_lock.unlock();
    type_registry.put(info.class_name, info) catch {};
}

// ── Handles ───────────────────────────────────────────────────
const ZigCompiled = struct {
    class_name: []const u8,
    type_info:  *const ZigTypeInfo,
    sandboxed:  bool,
};

const ZigInstance = struct {
    compiled: *const ZigCompiled,
    obj:      *anyopaque,
    mutex:    std.Thread.Mutex,
};

var compiled_map: std.AutoHashMap(usize, *ZigCompiled) = undefined;
var instance_map: std.AutoHashMap(usize, *ZigInstance) = undefined;
var map_lock: std.Thread.Mutex = .{};

// ── ABI exports ───────────────────────────────────────────────
export fn zig_init_runtime() callconv(.C) i32 {
    if (runtime_inited) return PL_OK;
    gpa = gpa_instance.allocator();
    type_registry  = std.StringHashMap(*const ZigTypeInfo).init(gpa);
    compiled_map   = std.AutoHashMap(usize, *ZigCompiled).init(gpa);
    instance_map   = std.AutoHashMap(usize, *ZigInstance).init(gpa);
    runtime_inited = true;
    return PL_OK;
}

export fn zig_shutdown_runtime() callconv(.C) void {
    if (!runtime_inited) return;
    type_registry.deinit();
    compiled_map.deinit();
    instance_map.deinit();
    _ = gpa_instance.deinit();
    runtime_inited = false;
}

fn compileCore(source: ?[*:0]const u8, path: ?[*:0]const u8, sandboxed: bool) callconv(.C) ?*anyopaque {
    _ = source;  // Zig scripts are pre-compiled; source is unused at runtime
    const p_str: []const u8 = if (path) |p| std.mem.span(p) else "script";

    // Strip path prefix and extensions
    var name = p_str;
    if (std.mem.lastIndexOf(u8, name, "/")) |i| name = name[i+1..];
    if (std.mem.indexOf(u8, name, "."))      |i| name = name[0..i];

    registry_lock.lock();
    const maybe_info = type_registry.get(name);
    registry_lock.unlock();

    const info = maybe_info orelse {
        std.debug.print("[PolyLang/Zig{s}] type '{s}' not registered\n",
            .{ if (sandboxed) "/sandbox" else "", name });
        return null;
    };

    const c_obj = gpa.create(ZigCompiled) catch return null;
    c_obj.* = .{ .class_name = name, .type_info = info, .sandboxed = sandboxed };
    const key = @intFromPtr(c_obj);
    map_lock.lock();
    compiled_map.put(key, c_obj) catch { gpa.destroy(c_obj); map_lock.unlock(); return null; }
    map_lock.unlock();
    return @ptrFromInt(key);
}

export fn zig_compile(source: ?[*:0]const u8, path: ?[*:0]const u8) callconv(.C) ?*anyopaque {
    return compileCore(source, path, false);
}

export fn zig_compile_sandboxed(source: ?[*:0]const u8, path: ?[*:0]const u8,
                                  _caps: u32) callconv(.C) ?*anyopaque {
    return compileCore(source, path, true);
}

export fn zig_free_compiled(h: ?*anyopaque) callconv(.C) void {
    const key = @intFromPtr(h orelse return);
    map_lock.lock();
    if (compiled_map.fetchRemove(key)) |entry| gpa.destroy(entry.value);
    map_lock.unlock();
}

export fn zig_instantiate_class(ch: ?*anyopaque, _path: ?[*:0]const u8) callconv(.C) ?*anyopaque {
    const key = @intFromPtr(ch orelse return null);
    map_lock.lock();
    const maybe_c = compiled_map.get(key);
    map_lock.unlock();
    const compiled = maybe_c orelse return null;

    const obj = compiled.type_info.create_fn();
    const inst = gpa.create(ZigInstance) catch return null;
    inst.* = .{ .compiled = compiled, .obj = obj, .mutex = .{} };
    const ikey = @intFromPtr(inst);
    map_lock.lock();
    instance_map.put(ikey, inst) catch { gpa.destroy(inst); map_lock.unlock(); return null; }
    map_lock.unlock();
    return @ptrFromInt(ikey);
}

export fn zig_free_instance(raw: ?*anyopaque) callconv(.C) void {
    const key = @intFromPtr(raw orelse return);
    map_lock.lock();
    if (instance_map.fetchRemove(key)) |entry| {
        entry.value.compiled.type_info.destroy_fn(entry.value.obj);
        gpa.destroy(entry.value);
    }
    map_lock.unlock();
}

export fn zig_call_method(raw: ?*anyopaque, name: ?[*:0]const u8,
                            args: ?[*]c.PLValue, argc: i32,
                            ret: ?*c.PLValue) callconv(.C) i32 {
    var out = ret orelse return PL_ERR_GENERIC;
    out.* = std.mem.zeroes(c.PLValue);

    const key = @intFromPtr(raw orelse return PL_ERR_GENERIC);
    map_lock.lock();
    const maybe_inst = instance_map.get(key);
    map_lock.unlock();
    const inst = maybe_inst orelse return PL_ERR_GENERIC;

    const method_name = std.mem.span(name orelse return PL_ERR_GENERIC);

    // Sandbox check
    if (inst.compiled.sandboxed and isSandboxDenied(method_name)) {
        std.debug.print("[PolyLang/Zig/sandbox] method '{s}' blocked\n", .{method_name});
        return PL_ERR_SANDBOX;
    }

    inst.mutex.lock();
    defer inst.mutex.unlock();

    const args_slice: []const c.PLValue = if (argc > 0 and args != null)
        (args.?)[0..@intCast(argc)]
    else &[_]c.PLValue{};

    for (inst.compiled.type_info.methods) |entry| {
        if (std.mem.eql(u8, entry.name, method_name)) {
            out.* = entry.func(inst.obj, args_slice);
            return PL_OK;
        }
    }
    return PL_ERR_METHOD_NOT_FOUND;
}

export fn zig_call_builtin(raw: ?*anyopaque, id: i32,
                             args: ?[*]c.PLValue, argc: i32,
                             ret: ?*c.PLValue) callconv(.C) i32 {
    const name: [*:0]const u8 = switch (id) {
        PL_METHOD_READY            => "_ready",
        PL_METHOD_PROCESS          => "_process",
        PL_METHOD_PHYSICS_PROCESS  => "_physicsProcess",
        PL_METHOD_ENTER_TREE       => "_enterTree",
        PL_METHOD_EXIT_TREE        => "_exitTree",
        else                       => return PL_ERR_NOT_IMPLEMENTED,
    };
    return zig_call_method(raw, name, args, argc, ret);
}

export fn zig_set_property(raw: ?*anyopaque, name: ?[*:0]const u8,
                             v: ?*const c.PLValue) callconv(.C) i32 {
    _ = raw; _ = name; _ = v;
    return PL_ERR_PROP_NOT_FOUND; // Subclass override required
}

export fn zig_get_property(raw: ?*anyopaque, name: ?[*:0]const u8,
                             out: ?*c.PLValue) callconv(.C) i32 {
    _ = raw; _ = name;
    if (out) |o| o.* = std.mem.zeroes(c.PLValue);
    return PL_ERR_PROP_NOT_FOUND; // Subclass override required
}

export fn zig_has_method(ch: ?*anyopaque, name: ?[*:0]const u8) callconv(.C) u8 {
    const key = @intFromPtr(ch orelse return 0);
    map_lock.lock();
    const maybe_c = compiled_map.get(key);
    map_lock.unlock();
    const compiled = maybe_c orelse return 0;
    const n = std.mem.span(name orelse return 0);
    for (compiled.type_info.methods) |entry| {
        if (std.mem.eql(u8, entry.name, n)) return 1;
    }
    return 0;
}

export fn zig_free_value_contents(v: ?*c.PLValue) callconv(.C) void {
    const vp = v orelse return;
    if (vp.*.type == PL_TYPE_STRING) {
        // _raw is the named byte-array member inside PLValue's anonymous union.
        // In Zig @cImport, anonymous union members are flattened onto the parent
        // struct, so _raw is accessed directly on PLValue.
        const pp: **anyopaque = @ptrCast(@alignCast(&vp.*._raw));
        if (pp.* != null) {
            gpa.free(@as([*]u8, @ptrCast(pp.*))[0..std.mem.len(@as([*:0]u8, @ptrCast(pp.*)))]);
            pp.* = null;
        }
    }
    vp.*.type = PL_TYPE_NIL;
}

// ── vtable ────────────────────────────────────────────────────
export fn pl_get_vtable(out: *c.PLAdapterVTable) callconv(.C) void {
    out.* = std.mem.zeroes(c.PLAdapterVTable);
    out.*.abi_version  = 5;
    out.*.capabilities = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP |
                         PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out.*.pl_init_runtime       = zig_init_runtime;
    out.*.pl_shutdown_runtime   = zig_shutdown_runtime;
    out.*.pl_compile            = zig_compile;
    out.*.pl_compile_sandboxed  = zig_compile_sandboxed;
    out.*.pl_free_compiled      = zig_free_compiled;
    out.*.pl_instantiate_class  = zig_instantiate_class;
    out.*.pl_free_instance      = zig_free_instance;
    out.*.pl_call_method        = zig_call_method;
    out.*.pl_call_builtin       = zig_call_builtin;
    out.*.pl_set_property       = zig_set_property;
    out.*.pl_get_property       = zig_get_property;
    out.*.pl_has_method         = zig_has_method;
    out.*.pl_free_value_contents = zig_free_value_contents;
}
