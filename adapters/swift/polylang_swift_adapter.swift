// polylang_swift_adapter.swift — PolyLang Swift Adapter v5
// =============================================================
// SANDBOX:   pl_compile_sandboxed sets sandboxed=true on the compiled handle.
//            At call dispatch, method names matching a deny-list of dangerous
//            Foundation APIs (FileManager, Process, URLSession writes, etc.)
//            are blocked and return PL_ERR_SANDBOX. Swift itself provides no
//            dynamic sandbox; this is an advisory enforcement by the bridge.
// PLATFORM:  Desktop only (macOS/Linux). iOS would need a different Swift
//            runtime configuration and is not wired here.
// =============================================================
import Foundation

// ── Sandbox deny-list ────────────────────────────────────────
private let sandboxDeniedPrefixes = [
    "exec", "run", "shell", "openFile", "writeFile", "deleteFile",
    "createDirectory", "removeItem", "moveItem", "copyItem",
    "createProcess", "launch", "download", "dataTask", "uploadTask"
]

private func isSandboxDenied(_ name: String) -> Bool {
    let lower = name.lowercased()
    return sandboxDeniedPrefixes.contains { lower.hasPrefix($0.lowercased()) }
}

// ── PLValue helpers ──────────────────────────────────────────
private func plValueNil() -> PLValue {
    var v = PLValue(); pl_value_init(&v); return v
}

private func anyToPLValue(_ any: Any?) -> PLValue {
    var v = PLValue(); pl_value_init(&v)
    guard let any = any else { return v }
    switch any {
    case let b as Bool:   v.type = PL_TYPE_BOOL;  v.b = b
    case let i as Int:    v.type = PL_TYPE_INT;   v.i = Int64(i)
    case let i as Int64:  v.type = PL_TYPE_INT;   v.i = i
    case let f as Double: v.type = PL_TYPE_FLOAT; v.f = f
    case let f as Float:  v.type = PL_TYPE_FLOAT; v.f = Double(f)
    case let s as String:
        v.type = PL_TYPE_STRING
        v.s = strdup(s)
    default: break
    }
    return v
}

private func plValueToAny(_ v: PLValue) -> Any? {
    switch Int32(v.type) {
    case PL_TYPE_NIL:    return nil
    case PL_TYPE_BOOL:   return v.b
    case PL_TYPE_INT:    return v.i
    case PL_TYPE_FLOAT:  return v.f
    case PL_TYPE_STRING: return v.s.map { String(cString: $0) }
    default: return nil
    }
}

// ── Compiled / instance handles ──────────────────────────────
class SwiftCompiled {
    var typeName: String
    var typeInfo: AnyClass?
    var sandboxed: Bool

    init(typeName: String, typeInfo: AnyClass?, sandboxed: Bool) {
        self.typeName = typeName
        self.typeInfo = typeInfo
        self.sandboxed = sandboxed
    }
}

class SwiftInstance {
    var compiled: SwiftCompiled
    var object: AnyObject
    var lock = NSLock()

    init(compiled: SwiftCompiled, object: AnyObject) {
        self.compiled = compiled
        self.object = object
    }
}

// Global maps (pointer-keyed)
private var compiledMap  = [UInt: SwiftCompiled]()
private var instanceMap  = [UInt: SwiftInstance]()
private var mapLock      = NSLock()

// ── Core compile ─────────────────────────────────────────────
@_silgen_name("swift_compile_core")
func swiftCompileCore(_ source: UnsafePointer<CChar>?,
                      _ path:   UnsafePointer<CChar>?,
                      _ sandboxed: Bool) -> UnsafeMutableRawPointer? {
    guard let source = source else { return nil }

    var name = path.map { String(cString: $0) } ?? "script"
    if let slash = name.lastIndex(of: "/") { name = String(name[name.index(after: slash)...]) }
    if let dot = name.firstIndex(of: ".") { name = String(name[..<dot]) }

    // Look up type from the Swift type registry via NSClassFromString
    let typeInfo = NSClassFromString(name)

    let c = SwiftCompiled(typeName: name, typeInfo: typeInfo, sandboxed: sandboxed)
    let ptr = UInt(bitPattern: ObjectIdentifier(c))
    mapLock.lock(); compiledMap[ptr] = c; mapLock.unlock()

    return UnsafeMutableRawPointer(bitPattern: ptr)
}

// ── ABI exports ───────────────────────────────────────────────
@_silgen_name("swift_init_runtime")
public func swiftInitRuntime() -> Int32 { return Int32(PL_OK) }

@_silgen_name("swift_shutdown_runtime")
public func swiftShutdownRuntime() {}

@_silgen_name("swift_compile")
public func swiftCompile(_ source: UnsafePointer<CChar>?,
                         _ path:   UnsafePointer<CChar>?) -> UnsafeMutableRawPointer? {
    return swiftCompileCore(source, path, false)
}

@_silgen_name("swift_compile_sandboxed")
public func swiftCompileSandboxed(_ source: UnsafePointer<CChar>?,
                                  _ path:   UnsafePointer<CChar>?,
                                  _ caps:   UInt32) -> UnsafeMutableRawPointer? {
    return swiftCompileCore(source, path, true)
}

@_silgen_name("swift_free_compiled")
public func swiftFreeCompiled(_ h: UnsafeMutableRawPointer?) {
    guard let h = h else { return }
    let key = UInt(bitPattern: h)
    mapLock.lock(); compiledMap.removeValue(forKey: key); mapLock.unlock()
}

@_silgen_name("swift_instantiate_class")
public func swiftInstantiateClass(_ ch:   UnsafeMutableRawPointer?,
                                  _ path: UnsafePointer<CChar>?) -> UnsafeMutableRawPointer? {
    guard let ch = ch else { return nil }
    let key = UInt(bitPattern: ch)
    mapLock.lock(); let c = compiledMap[key]; mapLock.unlock()
    guard let c = c else { return nil }

    guard let cls = c.typeInfo as? NSObject.Type else {
        fputs("[PolyLang/Swift] Class '\(c.typeName)' is not NSObject subclass\n", stderr)
        return nil
    }
    let obj = cls.init()
    let inst = SwiftInstance(compiled: c, object: obj)
    let instKey = UInt(bitPattern: ObjectIdentifier(inst))
    mapLock.lock(); instanceMap[instKey] = inst; mapLock.unlock()
    return UnsafeMutableRawPointer(bitPattern: instKey)
}

@_silgen_name("swift_free_instance")
public func swiftFreeInstance(_ raw: UnsafeMutableRawPointer?) {
    guard let raw = raw else { return }
    let key = UInt(bitPattern: raw)
    mapLock.lock(); instanceMap.removeValue(forKey: key); mapLock.unlock()
}

@_silgen_name("swift_call_method")
public func swiftCallMethod(_ raw:  UnsafeMutableRawPointer?,
                             _ name: UnsafePointer<CChar>?,
                             _ args: UnsafeMutablePointer<PLValue>?,
                             _ argc: Int32,
                             _ ret:  UnsafeMutablePointer<PLValue>?) -> Int32 {
    guard let raw = raw, let ret = ret, let name = name else { return Int32(PL_ERR_GENERIC) }
    ret.pointee = plValueNil()

    let key = UInt(bitPattern: raw)
    mapLock.lock(); let inst = instanceMap[key]; mapLock.unlock()
    guard let inst = inst else { return Int32(PL_ERR_GENERIC) }

    let methodName = String(cString: name)

    // Sandbox check
    if inst.compiled.sandboxed && isSandboxDenied(methodName) {
        fputs("[PolyLang/Swift/sandbox] method '\(methodName)' blocked\n", stderr)
        return Int32(PL_ERR_SANDBOX)
    }

    inst.lock.lock()
    defer { inst.lock.unlock() }

    // Use ObjC runtime to call methods on NSObject subclasses
    let sel = NSSelectorFromString(methodName)
    guard inst.object.responds(to: sel) else { return Int32(PL_ERR_METHOD_NOT_FOUND) }

    // Build argument array
    var goArgs: [Any?] = []
    if argc > 0, let argsPtr = args {
        for k in 0..<Int(argc) {
            goArgs.append(plValueToAny(argsPtr[k]))
        }
    }

    // Invoke via ObjC runtime (NSInvocation not needed for simple cases)
    let result = inst.object.perform(sel, with: goArgs.first ?? nil)
    if let r = result?.takeUnretainedValue() {
        ret.pointee = anyToPLValue(r)
    }
    return Int32(PL_OK)
}

@_silgen_name("swift_call_builtin")
public func swiftCallBuiltin(_ raw:  UnsafeMutableRawPointer?,
                              _ id:   Int32,
                              _ args: UnsafeMutablePointer<PLValue>?,
                              _ argc: Int32,
                              _ ret:  UnsafeMutablePointer<PLValue>?) -> Int32 {
    var name: String
    switch id {
    case Int32(PL_METHOD_READY):           name = "_ready"
    case Int32(PL_METHOD_PROCESS):         name = "_process"
    case Int32(PL_METHOD_PHYSICS_PROCESS): name = "_physicsProcess"
    case Int32(PL_METHOD_ENTER_TREE):      name = "_enterTree"
    case Int32(PL_METHOD_EXIT_TREE):       name = "_exitTree"
    default: return Int32(PL_ERR_NOT_IMPLEMENTED)
    }
    return name.withCString { swiftCallMethod(raw, $0, args, argc, ret) }
}

@_silgen_name("swift_set_property")
public func swiftSetProperty(_ raw:  UnsafeMutableRawPointer?,
                              _ name: UnsafePointer<CChar>?,
                              _ v:    UnsafePointer<PLValue>?) -> Int32 {
    guard let raw = raw, let name = name, let v = v else { return Int32(PL_ERR_GENERIC) }
    let key = UInt(bitPattern: raw)
    mapLock.lock(); let inst = instanceMap[key]; mapLock.unlock()
    guard let inst = inst else { return Int32(PL_ERR_GENERIC) }
    let propName = String(cString: name)
    let val = plValueToAny(v.pointee)
    inst.object.setValue(val, forKey: propName)
    return Int32(PL_OK)
}

@_silgen_name("swift_get_property")
public func swiftGetProperty(_ raw:  UnsafeMutableRawPointer?,
                              _ name: UnsafePointer<CChar>?,
                              _ out:  UnsafeMutablePointer<PLValue>?) -> Int32 {
    guard let raw = raw, let name = name, let out = out else { return Int32(PL_ERR_GENERIC) }
    let key = UInt(bitPattern: raw)
    mapLock.lock(); let inst = instanceMap[key]; mapLock.unlock()
    guard let inst = inst else { return Int32(PL_ERR_GENERIC) }
    let val = inst.object.value(forKey: String(cString: name))
    out.pointee = anyToPLValue(val)
    return Int32(PL_OK)
}

@_silgen_name("swift_has_method")
public func swiftHasMethod(_ ch: UnsafeMutableRawPointer?,
                            _ name: UnsafePointer<CChar>?) -> UInt8 {
    return 1  // Dynamic dispatch; assume method may exist
}

@_silgen_name("swift_free_value_contents")
public func swiftFreeValueContents(_ v: UnsafeMutablePointer<PLValue>?) {
    guard let v = v else { return }
    if Int32(v.pointee.type) == PL_TYPE_STRING {
        free(v.pointee.s)
        v.pointee.s = nil
    }
    v.pointee.type = CInt(PL_TYPE_NIL)
}

// pl_get_vtable is wired from the thin C shim (polylang_swift_shim.c)
// which calls the above @_silgen_name exports. The shim is compiled
// alongside this file by the CMake build for this adapter.
