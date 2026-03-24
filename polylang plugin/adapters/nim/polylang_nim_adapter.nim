# polylang_nim_adapter.nim — PolyLang Nim Adapter v5
# ============================================================
# Compiled with: nim c --app:lib --gc:orc -d:useMalloc
#                    -d:nimNoExceptionMsg
#                    --noMain --header:polylang_nim_adapter.h
#                    polylang_nim_adapter.nim
#
# SANDBOX:   pl_compile_sandboxed sets sandboxed=true on the compiled
#            handle. At method dispatch, calls matching a deny-list of
#            os/osproc/streams/net proc names are blocked with
#            PL_ERR_SANDBOX. Nim has no runtime sandbox; this is
#            bridge-layer advisory enforcement.
# =============================================================
{.pragma: exportc_cdecl, exportc, cdecl.}

import std/[tables, locks, strutils, os]

# ── Vtable constants (mirror pl_adapter_vtable.h) ────────────
const
  PL_ABI_VERSION  = 5
  PL_OK           = 0'i32
  PL_ERR_GENERIC  = -1'i32
  PL_ERR_METHOD   = -2'i32
  PL_ERR_PROP     = -3'i32
  PL_ERR_EXCEPT   = -5'i32
  PL_ERR_NOTIMPL  = -6'i32
  PL_ERR_SANDBOX  = -8'i32

  PL_TYPE_NIL     = 0'i32
  PL_TYPE_BOOL    = 1'i32
  PL_TYPE_INT     = 2'i32
  PL_TYPE_FLOAT   = 3'i32
  PL_TYPE_STRING  = 4'i32

  PL_CAP_ANDROID      = 1'u32
  PL_CAP_IOS          = 2'u32
  PL_CAP_DESKTOP      = 4'u32
  PL_CAP_BUILTIN_CALL = 64'u32
  PL_CAP_SANDBOX      = 128'u32

  PL_METHOD_READY           = 1'i32
  PL_METHOD_PROCESS         = 2'i32
  PL_METHOD_PHYSICS_PROCESS = 3'i32
  PL_METHOD_ENTER_TREE      = 4'i32
  PL_METHOD_EXIT_TREE       = 5'i32

# ── PLValue (32 bytes, matches C struct) ─────────────────────
type
  PLValueRaw {.packed.} = object
    vtype: int32
    pad:   int32
    raw:   array[24, byte]

  PLValue = PLValueRaw

# ── Sandbox deny-list ────────────────────────────────────────
const sandboxDenied = [
  "execCmd", "execCmdEx", "startProcess", "poEchoCmd",
  "openFile", "writeFile", "removeFile", "createDir", "removeDir",
  "getEnv", "putEnv", "newSocket", "connect", "bindAddr",
  "sendStr", "recvStr", "download", "httpGet", "httpPost"
]

proc isSandboxDenied(name: string): bool =
  for d in sandboxDenied:
    if name.toLowerAscii.startsWith(d.toLowerAscii): return true
  false

# ── Compiled / instance handles ──────────────────────────────
type
  NimMethodProc = proc(self: RootRef; args: seq[PLValue]): PLValue {.nimcall.}

  NimMethodEntry = object
    name: string
    fn:   NimMethodProc

  NimCompiled = ref object
    className: string
    methods:   seq[NimMethodEntry]
    ctor:      proc(): RootRef {.nimcall.}
    sandboxed: bool

  NimInstance = ref object
    compiled: NimCompiled
    obj:      RootRef
    lk:       Lock

# Global registry: class name → NimCompiled (set up by generated script stubs)
var
  typeRegistry: Table[string, NimCompiled]
  typeRegistryLock: Lock

initLock(typeRegistryLock)

proc registerNimType*(name: string; c: NimCompiled) =
  withLock typeRegistryLock: typeRegistry[name] = c

# ── Handle maps ───────────────────────────────────────────────
# We use cast[ptr UncheckedArray[...]] tricks for C interop.
# Handles are just raw pointers to ref objects (GC-rooted separately).
type HandleKey = pointer

var
  compiledPins: Table[HandleKey, NimCompiled]
  instancePins: Table[HandleKey, NimInstance]
  handleLock: Lock

initLock(handleLock)

proc pinCompiled(c: NimCompiled): HandleKey =
  let raw = cast[HandleKey](addr c[])
  withLock handleLock: compiledPins[raw] = c
  raw

proc getCompiled(h: HandleKey): NimCompiled =
  withLock handleLock:
    if h in compiledPins: return compiledPins[h]
  nil

proc unpinCompiled(h: HandleKey) =
  withLock handleLock: compiledPins.del(h)

proc pinInstance(i: NimInstance): HandleKey =
  let raw = cast[HandleKey](addr i[])
  withLock handleLock: instancePins[raw] = i
  raw

proc getInstance(h: HandleKey): NimInstance =
  withLock handleLock:
    if h in instancePins: return instancePins[h]
  nil

proc unpinInstance(h: HandleKey) =
  withLock handleLock: instancePins.del(h)

# ── Value helpers ─────────────────────────────────────────────
proc makePLNil(): PLValue = PLValue(vtype: PL_TYPE_NIL, pad: 0)

proc objToPL(obj: RootRef): PLValue = makePLNil()  # opaque; extend per-project

proc plToAny(v: PLValue): string =  # simplified; adapters coerce as needed
  case v.vtype
  of PL_TYPE_STRING:
    let ptr = cast[ptr cstring](unsafeAddr v.raw[0])[]
    if ptr != nil: $ptr else: ""
  else: ""

# ── ABI exports ───────────────────────────────────────────────
proc nim_init_runtime(): int32 {.exportc_cdecl.} = PL_OK

proc nim_shutdown_runtime() {.exportc_cdecl.} = discard

proc nim_compile_core(source, path: cstring; sandboxed: bool): pointer =
  if source == nil: return nil
  var name = if path != nil: $path else: "script"
  let slash = name.rfind('/')
  if slash >= 0: name = name[slash+1..^1]
  let dot = name.find('.')
  if dot >= 0: name = name[0..dot-1]

  var c: NimCompiled = nil
  withLock typeRegistryLock:
    if name in typeRegistry: c = typeRegistry[name]
  if c == nil:
    stderr.writeLine("[PolyLang/Nim" & (if sandboxed: "/sandbox" else: "") &
                     "] Class '" & name & "' not registered")
    return nil

  let cc = NimCompiled(
    className: c.className, methods: c.methods,
    ctor: c.ctor, sandboxed: sandboxed)
  cast[pointer](pinCompiled(cc))

proc nim_compile(source, path: cstring): pointer {.exportc_cdecl.} =
  nim_compile_core(source, path, false)

proc nim_compile_sandboxed(source, path: cstring; caps: uint32): pointer {.exportc_cdecl.} =
  nim_compile_core(source, path, true)

proc nim_free_compiled(h: pointer) {.exportc_cdecl.} =
  unpinCompiled(h)

proc nim_instantiate_class(ch: pointer; path: cstring): pointer {.exportc_cdecl.} =
  let c = getCompiled(ch)
  if c == nil or c.ctor == nil: return nil
  let obj = c.ctor()
  if obj == nil: return nil
  var inst = NimInstance(compiled: c, obj: obj)
  initLock(inst.lk)
  cast[pointer](pinInstance(inst))

proc nim_free_instance(raw: pointer) {.exportc_cdecl.} =
  unpinInstance(raw)

proc nim_call_method(raw: pointer; name: cstring;
                     args: ptr PLValue; argc: int32;
                     ret: ptr PLValue): int32 {.exportc_cdecl.} =
  ret[] = makePLNil()
  let inst = getInstance(raw)
  if inst == nil: return PL_ERR_GENERIC

  let mname = $name
  if inst.compiled.sandboxed and isSandboxDenied(mname):
    stderr.writeLine("[PolyLang/Nim/sandbox] method '" & mname & "' blocked")
    return PL_ERR_SANDBOX

  withLock inst.lk:
    for entry in inst.compiled.methods:
      if entry.name == mname:
        var argsSeq: seq[PLValue]
        for k in 0..<argc:
          argsSeq.add(cast[ptr UncheckedArray[PLValue]](args)[k])
        try:
          ret[] = entry.fn(inst.obj, argsSeq)
          return PL_OK
        except:
          stderr.writeLine("[PolyLang/Nim] exception in '" & mname & "'")
          return PL_ERR_EXCEPT
  PL_ERR_METHOD

proc nim_call_builtin(raw: pointer; id: int32;
                      args: ptr PLValue; argc: int32;
                      ret: ptr PLValue): int32 {.exportc_cdecl.} =
  let n =
    case id
    of PL_METHOD_READY:           "_ready"
    of PL_METHOD_PROCESS:         "_process"
    of PL_METHOD_PHYSICS_PROCESS: "_physicsProcess"
    of PL_METHOD_ENTER_TREE:      "_enterTree"
    of PL_METHOD_EXIT_TREE:       "_exitTree"
    else: ""
  if n.len == 0: return PL_ERR_NOTIMPL
  nim_call_method(raw, n.cstring, args, argc, ret)

proc nim_set_property(raw: pointer; name: cstring;
                      v: ptr PLValue): int32 {.exportc_cdecl.} =
  PL_ERR_PROP  # Subclass override required

proc nim_get_property(raw: pointer; name: cstring;
                      out_v: ptr PLValue): int32 {.exportc_cdecl.} =
  out_v[] = makePLNil()
  PL_ERR_PROP  # Subclass override required

proc nim_has_method(ch: pointer; name: cstring): uint8 {.exportc_cdecl.} =
  let c = getCompiled(ch)
  if c == nil: return 0
  let n = $name
  for entry in c.methods:
    if entry.name == n: return 1
  0

proc nim_free_value_contents(v: ptr PLValue) {.exportc_cdecl.} =
  if v == nil: return
  if v[].vtype == PL_TYPE_STRING:
    let pp = cast[ptr pointer](unsafeAddr v[].raw[0])
    if pp[] != nil:
      dealloc(pp[])
      pp[] = nil
  v[].vtype = PL_TYPE_NIL

# ── vtable struct (repr C, matches PLAdapterVTable exactly) ──
type
  PLAdapterVTable {.packed.} = object
    abi_version:  uint32
    reserved:     uint32
    capabilities: uint32
    pad2:         uint32
    pl_init_runtime:      pointer
    pl_shutdown_runtime:  pointer
    pl_compile:           pointer
    pl_free_compiled:     pointer
    pl_instantiate_class: pointer
    pl_free_instance:     pointer
    pl_call_method:       pointer
    pl_call_builtin:      pointer
    pl_batch_process:     pointer
    pl_set_property:      pointer
    pl_get_property:      pointer
    pl_has_method:        pointer
    pl_get_method_list:   pointer
    pl_free_method_list:  pointer
    pl_get_property_list: pointer
    pl_free_property_list:pointer
    pl_serialize_state:   pointer
    pl_deserialize_state: pointer
    pl_compile_sandboxed: pointer
    pl_free_value_contents: pointer

proc pl_get_vtable(out_vt: ptr PLAdapterVTable) {.exportc_cdecl.} =
  zeroMem(out_vt, sizeof(PLAdapterVTable))
  out_vt[].abi_version  = PL_ABI_VERSION
  out_vt[].capabilities = PL_CAP_ANDROID or PL_CAP_IOS or PL_CAP_DESKTOP or
                          PL_CAP_BUILTIN_CALL or PL_CAP_SANDBOX
  out_vt[].pl_init_runtime       = cast[pointer](nim_init_runtime)
  out_vt[].pl_shutdown_runtime   = cast[pointer](nim_shutdown_runtime)
  out_vt[].pl_compile            = cast[pointer](nim_compile)
  out_vt[].pl_compile_sandboxed  = cast[pointer](nim_compile_sandboxed)
  out_vt[].pl_free_compiled      = cast[pointer](nim_free_compiled)
  out_vt[].pl_instantiate_class  = cast[pointer](nim_instantiate_class)
  out_vt[].pl_free_instance      = cast[pointer](nim_free_instance)
  out_vt[].pl_call_method        = cast[pointer](nim_call_method)
  out_vt[].pl_call_builtin       = cast[pointer](nim_call_builtin)
  out_vt[].pl_set_property       = cast[pointer](nim_set_property)
  out_vt[].pl_get_property       = cast[pointer](nim_get_property)
  out_vt[].pl_has_method         = cast[pointer](nim_has_method)
  out_vt[].pl_free_value_contents= cast[pointer](nim_free_value_contents)
