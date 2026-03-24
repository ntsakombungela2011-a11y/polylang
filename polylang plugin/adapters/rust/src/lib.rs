// polylang_rust_adapter/src/lib.rs — PolyLang Rust Adapter v5
// =============================================================
// SANDBOX: pl_compile_sandboxed sets sandboxed=true on the handle.
//   At dispatch, method names matching a deny-list return PL_ERR_SANDBOX.
//   Rust provides no OS-level runtime sandbox; this is bridge-layer
//   advisory enforcement blocking common dangerous method names.
//
// OWNERSHIP MODEL (fixed):
//   Compiled/instance handles are raw pointers to Box<T> pinned via
//   Box::into_raw(). Freed by the corresponding free_* function via
//   Box::from_raw(). No side-maps needed — the raw pointer IS the
//   allocation. The compiled handle stays valid until rs_free_compiled;
//   the instance handle stays valid until rs_free_instance.
// =============================================================
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_uint};
use std::sync::{Mutex, OnceLock};

const PL_ABI_VERSION: u32 = 5;
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

const SANDBOX_DENIED: &[&str] = &[
    "open_file","write_file","delete_file","create_dir","remove_dir",
    "exec","run_command","spawn","shell",
    "connect","bind","send","recv","download","upload",
    "get_env","set_env","remove_env","load_lib","dlopen",
];
fn is_sandbox_denied(name: &str) -> bool {
    let lower = name.to_lowercase();
    SANDBOX_DENIED.iter().any(|d| lower == *d || lower.starts_with(d))
}

#[repr(C)]
#[derive(Clone)]
pub struct PLValue {
    pub vtype: i32,
    pub pad:   i32,
    pub raw:   [u8; 24],
}
impl PLValue {
    pub fn nil() -> Self { Self { vtype: PL_TYPE_NIL, pad: 0, raw: [0u8; 24] } }
    pub fn from_bool(b: bool) -> Self {
        let mut v = Self::nil(); v.vtype = PL_TYPE_BOOL; v.raw[0] = b as u8; v
    }
    pub fn from_i64(i: i64) -> Self {
        let mut v = Self::nil(); v.vtype = PL_TYPE_INT;
        v.raw[..8].copy_from_slice(&i.to_ne_bytes()); v
    }
    pub fn from_f64(f: f64) -> Self {
        let mut v = Self::nil(); v.vtype = PL_TYPE_FLOAT;
        v.raw[..8].copy_from_slice(&f.to_bits().to_ne_bytes()); v
    }
    pub fn from_string(s: &str) -> Self {
        let mut v = Self::nil(); v.vtype = PL_TYPE_STRING;
        let cs = CString::new(s).unwrap_or_default();
        let ptr = cs.into_raw() as usize;
        v.raw[..8].copy_from_slice(&ptr.to_ne_bytes()); v
    }
    pub fn str_ptr(&self) -> *const c_char {
        usize::from_ne_bytes(self.raw[..8].try_into().unwrap_or([0u8;8])) as *const c_char
    }
}

pub trait RustScript: Send {
    fn call_method(&mut self, name: &str, args: &[PLValue]) -> Result<PLValue, String>;
    fn set_property(&mut self, name: &str, value: &PLValue) -> bool;
    fn get_property(&self,  name: &str) -> Option<PLValue>;
    fn has_method(&self,    name: &str) -> bool;
}
pub type ScriptFactory = Box<dyn Fn() -> Box<dyn RustScript> + Send + Sync>;

static REGISTRY: OnceLock<Mutex<HashMap<String, ScriptFactory>>> = OnceLock::new();
fn registry() -> &'static Mutex<HashMap<String, ScriptFactory>> {
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}
pub fn register_type(name: &'static str, factory: ScriptFactory) {
    registry().lock().unwrap().insert(name.to_owned(), factory);
}

struct RsCompiled { class_name: String, sandboxed: bool }
struct RsInstance  { class_name: String, sandboxed: bool, script: Box<dyn RustScript> }

fn class_name_from_path(path: *const c_char) -> String {
    let s: String = if path.is_null() { "script".into() }
    else { unsafe { CStr::from_ptr(path).to_string_lossy().into_owned() } };
    let mut n = s;
    if let Some(i) = n.rfind('/')  { n = n[i+1..].to_string(); }
    if let Some(i) = n.find('.')   { n = n[..i].to_string();   }
    n
}

#[no_mangle] pub extern "C" fn rs_init_runtime()     -> c_int { PL_OK }
#[no_mangle] pub extern "C" fn rs_shutdown_runtime()           {}

fn compile_core(source: *const c_char, path: *const c_char, sandboxed: bool)
    -> *mut std::ffi::c_void {
    if source.is_null() { return std::ptr::null_mut(); }
    let name = class_name_from_path(path);
    if !registry().lock().unwrap().contains_key(&name) {
        eprintln!("[PolyLang/Rust{}] type '{}' not registered",
            if sandboxed {"/sandbox"} else {""}, name);
        return std::ptr::null_mut();
    }
    // Pin: Box::into_raw transfers ownership to caller. Freed in rs_free_compiled.
    Box::into_raw(Box::new(RsCompiled { class_name: name, sandboxed }))
        as *mut std::ffi::c_void
}

#[no_mangle]
pub extern "C" fn rs_compile(src: *const c_char, path: *const c_char)
    -> *mut std::ffi::c_void { compile_core(src, path, false) }

#[no_mangle]
pub extern "C" fn rs_compile_sandboxed(src: *const c_char, path: *const c_char, _: c_uint)
    -> *mut std::ffi::c_void { compile_core(src, path, true) }

#[no_mangle]
pub extern "C" fn rs_free_compiled(h: *mut std::ffi::c_void) {
    if h.is_null() { return; }
    // SAFETY: h was produced by Box::into_raw in compile_core.
    unsafe { drop(Box::from_raw(h as *mut RsCompiled)); }
}

#[no_mangle]
pub extern "C" fn rs_instantiate_class(ch: *mut std::ffi::c_void, _: *const c_char)
    -> *mut std::ffi::c_void {
    if ch.is_null() { return std::ptr::null_mut(); }
    // SAFETY: ch is still valid (rs_free_compiled not yet called).
    let c = unsafe { &*(ch as *const RsCompiled) };
    let reg = registry().lock().unwrap();
    let Some(factory) = reg.get(&c.class_name) else { return std::ptr::null_mut(); };
    let script = factory();
    let name = c.class_name.clone();
    let sb = c.sandboxed;
    drop(reg);
    // Pin: freed in rs_free_instance.
    Box::into_raw(Box::new(RsInstance { class_name: name, sandboxed: sb, script }))
        as *mut std::ffi::c_void
}

#[no_mangle]
pub extern "C" fn rs_free_instance(raw: *mut std::ffi::c_void) {
    if raw.is_null() { return; }
    unsafe { drop(Box::from_raw(raw as *mut RsInstance)); }
}

#[no_mangle]
pub unsafe extern "C" fn rs_call_method(raw: *mut std::ffi::c_void, name: *const c_char,
    args: *mut PLValue, argc: i32, ret: *mut PLValue) -> c_int {
    if raw.is_null() || name.is_null() || ret.is_null() { return PL_ERR_GENERIC; }
    *ret = PLValue::nil();
    let inst = &mut *(raw as *mut RsInstance);
    let mn = CStr::from_ptr(name).to_string_lossy();
    if inst.sandboxed && is_sandbox_denied(&mn) {
        eprintln!("[PolyLang/Rust/sandbox] method '{}' blocked", mn);
        return PL_ERR_SANDBOX;
    }
    let sl: &[PLValue] = if argc > 0 && !args.is_null()
        { std::slice::from_raw_parts(args, argc as usize) } else { &[] };
    match inst.script.call_method(&mn, sl) {
        Ok(v)  => { *ret = v; PL_OK }
        Err(e) => { eprintln!("[PolyLang/Rust] ex: {}", e); PL_ERR_EXCEPTION }
    }
}

#[no_mangle]
pub unsafe extern "C" fn rs_call_builtin(raw: *mut std::ffi::c_void, id: i32,
    args: *mut PLValue, argc: i32, ret: *mut PLValue) -> c_int {
    let n = match id {
        PL_METHOD_READY           => "_ready\0",
        PL_METHOD_PROCESS         => "_process\0",
        PL_METHOD_PHYSICS_PROCESS => "_physics_process\0",
        PL_METHOD_ENTER_TREE      => "_enter_tree\0",
        PL_METHOD_EXIT_TREE       => "_exit_tree\0",
        _                         => return PL_ERR_NOT_IMPLEMENTED,
    };
    rs_call_method(raw, n.as_ptr() as *const c_char, args, argc, ret)
}

#[no_mangle]
pub unsafe extern "C" fn rs_set_property(raw: *mut std::ffi::c_void,
    name: *const c_char, v: *const PLValue) -> c_int {
    if raw.is_null() || name.is_null() || v.is_null() { return PL_ERR_GENERIC; }
    let inst = &mut *(raw as *mut RsInstance);
    let p = CStr::from_ptr(name).to_string_lossy();
    if inst.script.set_property(&p, &*v) { PL_OK } else { PL_ERR_PROP_NOT_FOUND }
}

#[no_mangle]
pub unsafe extern "C" fn rs_get_property(raw: *mut std::ffi::c_void,
    name: *const c_char, out: *mut PLValue) -> c_int {
    if raw.is_null() || name.is_null() || out.is_null() { return PL_ERR_GENERIC; }
    *out = PLValue::nil();
    let inst = &*(raw as *const RsInstance);
    let p = CStr::from_ptr(name).to_string_lossy();
    match inst.script.get_property(&p) {
        Some(v) => { *out = v; PL_OK }
        None    => PL_ERR_PROP_NOT_FOUND
    }
}

#[no_mangle]
pub extern "C" fn rs_has_method(ch: *mut std::ffi::c_void, name: *const c_char) -> u8 {
    if ch.is_null() || name.is_null() { return 0; }
    let c = unsafe { &*(ch as *const RsCompiled) };
    let reg = registry().lock().unwrap();
    let Some(factory) = reg.get(&c.class_name) else { return 0 };
    let inst = factory();
    let n = unsafe { CStr::from_ptr(name).to_string_lossy() };
    if inst.has_method(&n) { 1 } else { 0 }
}

#[no_mangle]
pub unsafe extern "C" fn rs_free_value_contents(v: *mut PLValue) {
    if v.is_null() { return; }
    let vr = &mut *v;
    if vr.vtype == PL_TYPE_STRING {
        let p = vr.str_ptr();
        if !p.is_null() {
            drop(CString::from_raw(p as *mut c_char));
            vr.raw[..8].fill(0);
        }
    }
    vr.vtype = PL_TYPE_NIL;
}

// ── vtable ────────────────────────────────────────────────────
#[repr(C)]
pub struct PLAdapterVTable {
    pub abi_version:            u32,
    pub _reserved:              u32,
    pub capabilities:           u32,
    pub _pad2:                  u32,
    pub pl_init_runtime:        unsafe extern "C" fn() -> c_int,
    pub pl_shutdown_runtime:    unsafe extern "C" fn(),
    pub pl_compile:             unsafe extern "C" fn(*const c_char, *const c_char) -> *mut std::ffi::c_void,
    pub pl_free_compiled:       unsafe extern "C" fn(*mut std::ffi::c_void),
    pub pl_instantiate_class:   unsafe extern "C" fn(*mut std::ffi::c_void, *const c_char) -> *mut std::ffi::c_void,
    pub pl_free_instance:       unsafe extern "C" fn(*mut std::ffi::c_void),
    pub pl_call_method:         unsafe extern "C" fn(*mut std::ffi::c_void, *const c_char, *mut PLValue, i32, *mut PLValue) -> c_int,
    pub pl_call_builtin:        unsafe extern "C" fn(*mut std::ffi::c_void, i32, *mut PLValue, i32, *mut PLValue) -> c_int,
    pub pl_batch_process:       *mut std::ffi::c_void,
    pub pl_set_property:        unsafe extern "C" fn(*mut std::ffi::c_void, *const c_char, *const PLValue) -> c_int,
    pub pl_get_property:        unsafe extern "C" fn(*mut std::ffi::c_void, *const c_char, *mut PLValue) -> c_int,
    pub pl_has_method:          extern "C" fn(*mut std::ffi::c_void, *const c_char) -> u8,
    pub pl_get_method_list:     *mut std::ffi::c_void,
    pub pl_free_method_list:    *mut std::ffi::c_void,
    pub pl_get_property_list:   *mut std::ffi::c_void,
    pub pl_free_property_list:  *mut std::ffi::c_void,
    pub pl_serialize_state:     *mut std::ffi::c_void,
    pub pl_deserialize_state:   *mut std::ffi::c_void,
    pub pl_compile_sandboxed:   unsafe extern "C" fn(*const c_char, *const c_char, c_uint) -> *mut std::ffi::c_void,
    pub pl_free_value_contents: unsafe extern "C" fn(*mut PLValue),
}

#[no_mangle]
pub unsafe extern "C" fn pl_get_vtable(out: *mut PLAdapterVTable) {
    if out.is_null() { return; }
    std::ptr::write_bytes(out, 0, 1);
    let vt = &mut *out;
    vt.abi_version           = PL_ABI_VERSION;
    vt.capabilities          = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                              | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    vt.pl_init_runtime        = rs_init_runtime;
    vt.pl_shutdown_runtime    = rs_shutdown_runtime;
    vt.pl_compile             = rs_compile;
    vt.pl_free_compiled       = rs_free_compiled;
    vt.pl_instantiate_class   = rs_instantiate_class;
    vt.pl_free_instance       = rs_free_instance;
    vt.pl_call_method         = rs_call_method;
    vt.pl_call_builtin        = rs_call_builtin;
    vt.pl_set_property        = rs_set_property;
    vt.pl_get_property        = rs_get_property;
    vt.pl_has_method          = rs_has_method;
    vt.pl_compile_sandboxed   = rs_compile_sandboxed;
    vt.pl_free_value_contents = rs_free_value_contents;
    vt.pl_batch_process       = std::ptr::null_mut();
    vt.pl_get_method_list     = std::ptr::null_mut();
    vt.pl_free_method_list    = std::ptr::null_mut();
    vt.pl_get_property_list   = std::ptr::null_mut();
    vt.pl_free_property_list  = std::ptr::null_mut();
    vt.pl_serialize_state     = std::ptr::null_mut();
    vt.pl_deserialize_state   = std::ptr::null_mut();
}
