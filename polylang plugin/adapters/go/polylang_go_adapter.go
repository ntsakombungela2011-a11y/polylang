// polylang_go_adapter.go — PolyLang Go Adapter v5
// =============================================================
// CGO PATTERN:
//   All vtable function pointers are filled by polylang_go_shim.c
//   (a plain C file compiled in the same package). The shim declares
//   extern prototypes for the //export'd Go functions and assigns them
//   to the vtable via a fill_vtable() helper. This is the correct CGO
//   approach; you cannot assign //export'd symbols to C function pointer
//   fields directly from Go code.
//
// SANDBOX: pl_compile_sandboxed records sandboxed=true on the compiled
//   handle (stored as a Go object pinned via cgo.Handle). Method dispatch
//   checks a deny-list and returns PL_ERR_SANDBOX for blocked names.
// =============================================================
package main

/*
#include "../../include/pl_adapter_vtable.h"
#include <stdlib.h>
#include <string.h>
*/
import "C"

import (
	"fmt"
	"reflect"
	"runtime/cgo"
	"strings"
	"sync"
	"unsafe"
)

// ── Sandbox deny-list ─────────────────────────────────────────
var sandboxDenied = []string{
	"Open", "Create", "Remove", "Mkdir", "ReadFile", "WriteFile",
	"Exec", "Run", "Command", "Dial", "Listen", "Get", "Post",
	"Getenv", "Setenv", "LoadLib",
}

func isSandboxDenied(name string) bool {
	for _, d := range sandboxDenied {
		if strings.EqualFold(name, d) || strings.HasPrefix(strings.ToLower(name), strings.ToLower(d)) {
			return true
		}
	}
	return false
}

// ── Type registry (set up by generated script stubs) ─────────
var (
	typeRegistryMu sync.Mutex
	typeRegistry   = make(map[string]reflect.Type)
)

// RegisterGoType must be called by generated script stubs at init() time.
func RegisterGoType(name string, t reflect.Type) {
	typeRegistryMu.Lock()
	typeRegistry[name] = t
	typeRegistryMu.Unlock()
}

func lookupType(name string) (reflect.Type, bool) {
	typeRegistryMu.Lock()
	defer typeRegistryMu.Unlock()
	t, ok := typeRegistry[name]
	return t, ok
}

// ── Handle types (pinned via cgo.Handle) ─────────────────────
type goCompiled struct {
	typeName  string
	rtype     reflect.Type
	sandboxed bool
}

type goInstance struct {
	compiled *goCompiled
	value    reflect.Value
	mu       sync.Mutex
}

// ── Value conversion ──────────────────────────────────────────
func plToGo(v *C.PLValue) interface{} {
	switch v._type {
	case C.PL_TYPE_BOOL:
		return *(*bool)(unsafe.Pointer(&v.anon0[0]))
	case C.PL_TYPE_INT:
		return *(*int64)(unsafe.Pointer(&v.anon0[0]))
	case C.PL_TYPE_FLOAT:
		return *(*float64)(unsafe.Pointer(&v.anon0[0]))
	case C.PL_TYPE_STRING:
		p := *(**C.char)(unsafe.Pointer(&v.anon0[0]))
		if p != nil {
			return C.GoString(p)
		}
	}
	return nil
}

func goToPlValue(val interface{}, out *C.PLValue) {
	C.pl_value_init(out)
	if val == nil {
		return
	}
	rv := reflect.ValueOf(val)
	switch rv.Kind() {
	case reflect.Bool:
		out._type = C.PL_TYPE_BOOL
		*(*bool)(unsafe.Pointer(&out.anon0[0])) = rv.Bool()
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		out._type = C.PL_TYPE_INT
		*(*int64)(unsafe.Pointer(&out.anon0[0])) = rv.Int()
	case reflect.Float32, reflect.Float64:
		out._type = C.PL_TYPE_FLOAT
		*(*float64)(unsafe.Pointer(&out.anon0[0])) = rv.Float()
	case reflect.String:
		out._type = C.PL_TYPE_STRING
		cs := C.CString(rv.String())
		*(**C.char)(unsafe.Pointer(&out.anon0[0])) = cs
	}
}

func pathToName(path *C.char) string {
	s := C.GoString(path)
	if i := strings.LastIndex(s, "/"); i >= 0 {
		s = s[i+1:]
	}
	if i := strings.Index(s, "."); i >= 0 {
		s = s[:i]
	}
	return s
}

// ── Exported functions (called from polylang_go_shim.c) ──────

//export go_init_runtime
func go_init_runtime() C.int { return C.PL_OK }

//export go_shutdown_runtime
func go_shutdown_runtime() {}

//export go_compile
func go_compile(source *C.char, path *C.char) unsafe.Pointer {
	return goCompileCore(source, path, false)
}

//export go_compile_sandboxed
func go_compile_sandboxed(source *C.char, path *C.char, caps C.uint32_t) unsafe.Pointer {
	return goCompileCore(source, path, true)
}

func goCompileCore(source *C.char, path *C.char, sandboxed bool) unsafe.Pointer {
	_ = source
	name := pathToName(path)
	rtype, ok := lookupType(name)
	if !ok {
		fmt.Fprintf(debugWriter{}, "[PolyLang/Go%s] type '%s' not registered\n",
			sandboxSuffix(sandboxed), name)
		return nil
	}
	c := &goCompiled{typeName: name, rtype: rtype, sandboxed: sandboxed}
	h := cgo.NewHandle(c)
	return unsafe.Pointer(uintptr(h))
}

//export go_free_compiled
func go_free_compiled(h unsafe.Pointer) {
	if h == nil {
		return
	}
	cgo.Handle(uintptr(h)).Delete()
}

//export go_instantiate_class
func go_instantiate_class(ch unsafe.Pointer, path *C.char) unsafe.Pointer {
	if ch == nil {
		return nil
	}
	c := cgo.Handle(uintptr(ch)).Value().(*goCompiled)
	var val reflect.Value
	if c.rtype.Kind() == reflect.Ptr {
		val = reflect.New(c.rtype.Elem())
	} else {
		val = reflect.New(c.rtype).Elem()
	}
	inst := &goInstance{compiled: c, value: val}
	h := cgo.NewHandle(inst)
	return unsafe.Pointer(uintptr(h))
}

//export go_free_instance
func go_free_instance(raw unsafe.Pointer) {
	if raw == nil {
		return
	}
	cgo.Handle(uintptr(raw)).Delete()
}

//export go_call_method
func go_call_method(raw unsafe.Pointer, name *C.char, args *C.PLValue, argc C.int32_t, ret *C.PLValue) C.int {
	if raw == nil || ret == nil {
		return C.PL_ERR_GENERIC
	}
	C.pl_value_init(ret)
	inst := cgo.Handle(uintptr(raw)).Value().(*goInstance)
	methodName := C.GoString(name)

	if inst.compiled.sandboxed && isSandboxDenied(methodName) {
		fmt.Fprintf(debugWriter{}, "[PolyLang/Go/sandbox] method '%s' blocked\n", methodName)
		return C.PL_ERR_SANDBOX
	}

	inst.mu.Lock()
	defer inst.mu.Unlock()

	method := inst.value.MethodByName(methodName)
	if !method.IsValid() {
		return C.PL_ERR_METHOD_NOT_FOUND
	}

	nArgs := int(argc)
	in := make([]reflect.Value, nArgs)
	if nArgs > 0 && args != nil {
		argSlice := (*[1 << 20]C.PLValue)(unsafe.Pointer(args))[:nArgs:nArgs]
		for k := 0; k < nArgs; k++ {
			goVal := plToGo(&argSlice[k])
			in[k] = reflect.ValueOf(goVal)
		}
	}

	results := method.Call(in)
	if len(results) > 0 {
		goToPlValue(results[0].Interface(), ret)
	}
	return C.PL_OK
}

//export go_call_builtin
func go_call_builtin(raw unsafe.Pointer, id C.int32_t, args *C.PLValue, argc C.int32_t, ret *C.PLValue) C.int {
	var name string
	switch id {
	case C.PL_METHOD_READY:           name = "Ready"
	case C.PL_METHOD_PROCESS:         name = "Process"
	case C.PL_METHOD_PHYSICS_PROCESS: name = "PhysicsProcess"
	case C.PL_METHOD_ENTER_TREE:      name = "EnterTree"
	case C.PL_METHOD_EXIT_TREE:       name = "ExitTree"
	default:                           return C.PL_ERR_NOT_IMPLEMENTED
	}
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return go_call_method(raw, cs, args, argc, ret)
}

//export go_set_property
func go_set_property(raw unsafe.Pointer, name *C.char, v *C.PLValue) C.int {
	if raw == nil {
		return C.PL_ERR_GENERIC
	}
	inst := cgo.Handle(uintptr(raw)).Value().(*goInstance)
	inst.mu.Lock()
	defer inst.mu.Unlock()
	field := inst.value.FieldByName(C.GoString(name))
	if !field.IsValid() || !field.CanSet() {
		return C.PL_ERR_PROP_NOT_FOUND
	}
	goVal := plToGo(v)
	if goVal != nil {
		rv := reflect.ValueOf(goVal)
		if rv.Type().AssignableTo(field.Type()) {
			field.Set(rv)
		}
	}
	return C.PL_OK
}

//export go_get_property
func go_get_property(raw unsafe.Pointer, name *C.char, out *C.PLValue) C.int {
	if raw == nil {
		return C.PL_ERR_GENERIC
	}
	C.pl_value_init(out)
	inst := cgo.Handle(uintptr(raw)).Value().(*goInstance)
	inst.mu.Lock()
	defer inst.mu.Unlock()
	field := inst.value.FieldByName(C.GoString(name))
	if !field.IsValid() {
		return C.PL_ERR_PROP_NOT_FOUND
	}
	goToPlValue(field.Interface(), out)
	return C.PL_OK
}

//export go_has_method
func go_has_method(ch unsafe.Pointer, name *C.char) C.uint8_t {
	if ch == nil {
		return 0
	}
	c := cgo.Handle(uintptr(ch)).Value().(*goCompiled)
	typeRegistryMu.Lock()
	rtype, ok := typeRegistry[c.typeName]
	typeRegistryMu.Unlock()
	if !ok {
		return 0
	}
	var val reflect.Value
	if rtype.Kind() == reflect.Ptr {
		val = reflect.New(rtype.Elem())
	} else {
		val = reflect.New(rtype).Elem()
	}
	if val.MethodByName(C.GoString(name)).IsValid() {
		return 1
	}
	return 0
}

//export go_free_value_contents
func go_free_value_contents(v *C.PLValue) {
	if v == nil {
		return
	}
	if v._type == C.PL_TYPE_STRING {
		pp := (**C.char)(unsafe.Pointer(&v.anon0[0]))
		if *pp != nil {
			C.free(unsafe.Pointer(*pp))
			*pp = nil
		}
	}
	v._type = C.PL_TYPE_NIL
}

// pl_get_vtable is implemented in polylang_go_shim.c (fills vtable from Go exports).
// This file must NOT define pl_get_vtable.

func sandboxSuffix(s bool) string {
	if s {
		return "/sandbox"
	}
	return ""
}

// debugWriter satisfies io.Writer for fmt.Fprintf without importing os.
type debugWriter struct{}

func (debugWriter) Write(p []byte) (int, error) {
	// Writes to stderr via C fprintf.
	cs := C.CString(string(p))
	C.fprintf(C.stderr, cs)
	C.free(unsafe.Pointer(cs))
	return len(p), nil
}

func main() {}
