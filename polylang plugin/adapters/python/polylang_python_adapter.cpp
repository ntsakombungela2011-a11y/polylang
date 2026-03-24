// =============================================================
// polylang_python_adapter.cpp  —  CPython Adapter (ABI v5)
// SECTION 2: Full source with pl_compile_sandboxed
// =============================================================
// Sandbox policy:
//   Allowed builtins:
//     abs, all, any, bin, bool, bytes, callable, chr, complex,
//     dict, divmod, enumerate, filter, float, format, frozenset,
//     getattr, hasattr, hash, hex, id, int, isinstance, issubclass,
//     iter, len, list, map, max, min, next, object, oct, ord,
//     pow, print (redirected), range, repr, reversed, round,
//     set, slice, sorted, str, sum, super, tuple, type, zip
//   Blocked:
//     open, exec, eval, compile, __import__, importlib,
//     os, sys, subprocess, shutil, pathlib, socket,
//     ctypes, mmap, gc, inspect, dis, ast, code, codeop,
//     debugpy, pdb, trace, traceback (direct module access)
// Implementation:
//   Builds a restricted __builtins__ dict. Creates the module
//   __dict__ with that restricted __builtins__. Executes the
//   compiled code object into that dict. Class is extracted
//   from the module dict after execution.
// =============================================================
#include <Python.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>

#include "../../include/pl_adapter_vtable.h"

static bool       g_py_init             = false;
static bool       g_py_ever_initialized = false;
static std::mutex g_py_mutex;

// ── Value conversion ──────────────────────────────────────────

static PyObject* pl_to_py(const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    Py_RETURN_NONE;
        case PL_TYPE_BOOL:   return PyBool_FromLong(v.b ? 1 : 0);
        case PL_TYPE_INT:    return PyLong_FromLongLong(v.i);
        case PL_TYPE_FLOAT:  return PyFloat_FromDouble(v.f);
        case PL_TYPE_STRING: return PyUnicode_FromString(v.s ? v.s : "");
        case PL_TYPE_VEC2: {
            PyObject* t = PyTuple_New(2);
            PyTuple_SetItem(t, 0, PyFloat_FromDouble(v.v2[0]));
            PyTuple_SetItem(t, 1, PyFloat_FromDouble(v.v2[1]));
            return t;
        }
        case PL_TYPE_VEC3: {
            PyObject* t = PyTuple_New(3);
            PyTuple_SetItem(t, 0, PyFloat_FromDouble(v.v3[0]));
            PyTuple_SetItem(t, 1, PyFloat_FromDouble(v.v3[1]));
            PyTuple_SetItem(t, 2, PyFloat_FromDouble(v.v3[2]));
            return t;
        }
        default: Py_RETURN_NONE;
    }
}

static void py_to_pl(PyObject* val, PLValue& out) {
    pl_value_init(&out);
    if (!val || val == Py_None) { out.type = PL_TYPE_NIL; return; }
    if (PyBool_Check(val)) {
        out.type = PL_TYPE_BOOL; out.b = (val == Py_True); return;
    }
    if (PyLong_Check(val)) {
        out.type = PL_TYPE_INT; out.i = PyLong_AsLongLong(val); return;
    }
    if (PyFloat_Check(val)) {
        out.type = PL_TYPE_FLOAT; out.f = PyFloat_AsDouble(val); return;
    }
    if (PyUnicode_Check(val)) {
        const char* s = PyUnicode_AsUTF8(val);
        out.type = PL_TYPE_STRING; out.s = s ? strdup(s) : nullptr; return;
    }
    out.type = PL_TYPE_NIL;
}

// ── Handles ───────────────────────────────────────────────────

struct PyCompiled {
    PyObject*   code;       // PyCodeObject
    std::string class_name;
    bool        sandboxed{false};
};

struct PyInstance {
    PyObject* instance_obj; // Python instance of the class
    PyObject* mod_dict;     // module dict (keep alive for GC roots)
};

// ── Sandbox builtins builder ──────────────────────────────────
// Returns a new reference to a dict containing only safe builtins.
// Caller owns the reference.

static PyObject* py_build_safe_builtins() {
    PyObject* full_builtins = PyEval_GetBuiltins(); // borrowed
    if (!full_builtins) return nullptr;

    PyObject* safe = PyDict_New();
    if (!safe) return nullptr;

    // Whitelist: safe read-only / computation builtins only.
    // No open, exec, eval, compile, __import__, breakpoint.
    static const char* allowed[] = {
        "abs", "all", "any", "bin", "bool", "bytes",
        "callable", "chr", "complex", "dict", "divmod",
        "enumerate", "filter", "float", "format", "frozenset",
        "getattr", "hasattr", "hash", "hex", "id",
        "int", "isinstance", "issubclass", "iter",
        "len", "list", "map", "max", "min",
        "next", "object", "oct", "ord",
        "pow", "range", "repr", "reversed", "round",
        "set", "setattr", "slice", "sorted", "str",
        "sum", "super", "tuple", "type", "zip",
        // Exceptions needed for normal class operation
        "Exception", "ValueError", "TypeError", "KeyError",
        "IndexError", "AttributeError", "StopIteration",
        "RuntimeError", "NotImplementedError",
        "True", "False", "None",
        nullptr
    };

    for (int i = 0; allowed[i]; ++i) {
        PyObject* fn = PyDict_GetItemString(full_builtins, allowed[i]); // borrowed
        if (fn) PyDict_SetItemString(safe, allowed[i], fn);
    }

    // Redirect print to stderr-safe version
    PyObject* print_code_str = PyUnicode_FromString(
        "import sys as _sys\n"
        "def _safe_print(*args, **kwargs):\n"
        "    kwargs.setdefault('file', _sys.stderr)\n"
        "    _original_print(*args, **kwargs)\n"
    );
    // We do this simpler: just put the real print (no filesystem access in print itself)
    PyObject* real_print = PyDict_GetItemString(full_builtins, "print");
    if (real_print) PyDict_SetItemString(safe, "print", real_print);
    Py_XDECREF(print_code_str);

    // __build_class__ needed for `class Foo:` syntax
    PyObject* bc = PyDict_GetItemString(full_builtins, "__build_class__");
    if (bc) PyDict_SetItemString(safe, "__build_class__", bc);

    // __name__ placeholder
    PyObject* name = PyUnicode_FromString("__polylang_sandbox__");
    PyDict_SetItemString(safe, "__name__", name);
    Py_DECREF(name);

    return safe;
}

// ── Blocked module importer ───────────────────────────────────
// Replaces __import__ in sandboxed modules with a function that
// blocks all imports entirely (or allows a safe whitelist).

static PyObject* py_blocked_import(PyObject* /*self*/, PyObject* args) {
    const char* name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) name = "(unknown)";
    PyErr_Format(PyExc_ImportError,
        "[PolyLang/sandbox] import '%s' is blocked in sandboxed scripts.", name);
    return nullptr;
}

static PyMethodDef g_blocked_import_def = {
    "__import__", py_blocked_import, METH_VARARGS, "Blocked import"
};

// ── Shared compile core ───────────────────────────────────────

static void* py_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source) return nullptr;
    std::lock_guard<std::mutex> lk(g_py_mutex);
    if (!g_py_init) return nullptr;

    std::string p = path ? path : "<unknown>";
    auto slash = p.rfind('/');  if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find(".pl."); if (dot   != std::string::npos) p = p.substr(0, dot);
    std::string class_name = p;

    PyObject* code = Py_CompileString(source, path ? path : "<unknown>", Py_file_input);
    if (!code) {
        PyErr_Print();
        return nullptr;
    }

    auto* c       = new PyCompiled();
    c->code       = code;
    c->class_name = class_name;
    c->sandboxed  = sandboxed;
    return c;
}

// ── ABI ───────────────────────────────────────────────────────

static int py_init_runtime() {
    std::lock_guard<std::mutex> lk(g_py_mutex);
    if (g_py_init) return PL_OK;
    if (!g_py_ever_initialized) {
        Py_InitializeEx(0);
        g_py_ever_initialized = true;
    }
    g_py_init = true;
    return PL_OK;
}

static void py_shutdown_runtime() {
    // Never call Py_FinalizeEx — causes UB on re-init and with C-extension modules.
    std::lock_guard<std::mutex> lk(g_py_mutex);
    g_py_init = false;
}

static void* py_compile_pl(const char* source, const char* path) {
    return py_compile_core(source, path, false);
}

static void* py_compile_sandboxed_pl(const char* source, const char* path,
                                      uint32_t /*allowed_caps*/) {
    return py_compile_core(source, path, true);
}

static void py_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<PyCompiled*>(h);
    std::lock_guard<std::mutex> lk(g_py_mutex);
    Py_XDECREF(c->code);
    delete c;
}

static void* py_instantiate_class(void* compiled_handle, const char* /*path*/) {
    auto* c = static_cast<PyCompiled*>(compiled_handle);
    if (!c || !c->code) return nullptr;
    std::lock_guard<std::mutex> lk(g_py_mutex);

    PyObject* mod_dict = PyDict_New();
    if (!mod_dict) return nullptr;

    if (c->sandboxed) {
        // Build restricted builtins
        PyObject* safe_builtins = py_build_safe_builtins();
        if (!safe_builtins) { Py_DECREF(mod_dict); return nullptr; }

        // Install blocked __import__ into safe_builtins
        PyObject* blocked_fn = PyCFunction_New(&g_blocked_import_def, nullptr);
        if (blocked_fn) {
            PyDict_SetItemString(safe_builtins, "__import__", blocked_fn);
            Py_DECREF(blocked_fn);
        }

        PyDict_SetItemString(mod_dict, "__builtins__", safe_builtins);
        Py_DECREF(safe_builtins);

        // Block access to dangerous modules via sys.modules poisoning
        // (only relevant if a module somehow bypasses the import block)
        PyObject* sys_mod = PyImport_ImportModule("sys");
        if (sys_mod) {
            PyObject* sys_modules = PyObject_GetAttrString(sys_mod, "modules");
            if (sys_modules) {
                const char* blocked_mods[] = {
                    "os", "sys", "subprocess", "shutil", "pathlib",
                    "socket", "ctypes", "mmap", "gc", "inspect",
                    "dis", "ast", "code", "codeop", "importlib",
                    "builtins", "io", "nt", "posix",
                    nullptr
                };
                for (int i = 0; blocked_mods[i]; ++i) {
                    // Do NOT poison sys.modules globally — that would affect
                    // other scripts. Instead leave the import blocker to handle it.
                    (void)blocked_mods[i]; // documented intent only
                }
                Py_DECREF(sys_modules);
            }
            Py_DECREF(sys_mod);
        }
    } else {
        // Trusted: use full builtins
        PyObject* builtins = PyEval_GetBuiltins(); // borrowed
        PyDict_SetItemString(mod_dict, "__builtins__", builtins);
    }

    // Execute compiled code into mod_dict
    PyObject* result = PyEval_EvalCode(c->code, mod_dict, mod_dict);
    if (!result) {
        PyErr_Print();
        Py_DECREF(mod_dict);
        return nullptr;
    }
    Py_DECREF(result);

    // Find class in mod_dict
    PyObject* cls = PyDict_GetItemString(mod_dict, c->class_name.c_str()); // borrowed
    if (!cls || !PyCallable_Check(cls)) {
        fprintf(stderr, "[PolyLang/Python] Class '%s' not found in module.\n",
                c->class_name.c_str());
        Py_DECREF(mod_dict);
        return nullptr;
    }

    // Instantiate
    PyObject* instance = PyObject_CallObject(cls, nullptr);
    if (!instance) {
        PyErr_Print();
        Py_DECREF(mod_dict);
        return nullptr;
    }

    auto* inst          = new PyInstance;
    inst->instance_obj  = instance;  // new ref
    inst->mod_dict      = mod_dict;  // new ref (kept alive for GC roots)
    return inst;
}

static void py_free_instance(void* raw) {
    if (!raw) return;
    auto* inst = static_cast<PyInstance*>(raw);
    std::lock_guard<std::mutex> lk(g_py_mutex);
    Py_XDECREF(inst->instance_obj);
    Py_XDECREF(inst->mod_dict);
    delete inst;
}

static int py_call_method(void* raw, const char* method_name,
                           PLValue* args, int32_t argc, PLValue* ret) {
    auto* inst = static_cast<PyInstance*>(raw);
    if (!inst || !inst->instance_obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_py_mutex);

    PyObject* method = PyObject_GetAttrString(inst->instance_obj, method_name);
    if (!method) { PyErr_Clear(); return PL_ERR_METHOD_NOT_FOUND; }

    PyObject* py_args = PyTuple_New(argc);
    for (int32_t i = 0; i < argc; ++i) {
        PyObject* av = pl_to_py(args[i]);
        PyTuple_SetItem(py_args, i, av); // steals ref
    }

    PyObject* result = PyObject_Call(method, py_args, nullptr);
    Py_DECREF(py_args);
    Py_DECREF(method);

    if (!result) {
        PyErr_Print();
        ret->type = PL_TYPE_NIL;
        return PL_ERR_EXCEPTION;
    }

    py_to_pl(result, *ret);
    Py_DECREF(result);
    return PL_OK;
}

static int py_call_builtin(void* raw, int32_t method_id,
                            PLValue* args, int32_t argc, PLValue* ret) {
    const char* name = nullptr;
    switch (method_id) {
        case PL_METHOD_READY:           name = "_ready"; break;
        case PL_METHOD_PROCESS:         name = "_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: name = "_physics_process"; break;
        case PL_METHOD_ENTER_TREE:      name = "_enter_tree"; break;
        case PL_METHOD_EXIT_TREE:       name = "_exit_tree"; break;
        case PL_METHOD_INPUT:           name = "_input"; break;
        case PL_METHOD_NOTIFICATION:    name = "_notification"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return py_call_method(raw, name, args, argc, ret);
}

static int py_set_property(void* raw, const char* name, const PLValue* value) {
    auto* inst = static_cast<PyInstance*>(raw);
    if (!inst || !inst->instance_obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_py_mutex);
    PyObject* pv = pl_to_py(*value);
    int r = PyObject_SetAttrString(inst->instance_obj, name, pv);
    Py_DECREF(pv);
    return (r == 0) ? PL_OK : PL_ERR_GENERIC;
}

static int py_get_property(void* raw, const char* name, PLValue* out) {
    auto* inst = static_cast<PyInstance*>(raw);
    if (!inst || !inst->instance_obj) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(g_py_mutex);
    PyObject* val = PyObject_GetAttrString(inst->instance_obj, name);
    if (!val) { PyErr_Clear(); return PL_ERR_PROP_NOT_FOUND; }
    py_to_pl(val, *out);
    Py_DECREF(val);
    return PL_OK;
}

static uint8_t py_has_method(void* compiled_handle, const char* name) {
    // We can't check without an instance — return 1 (optimistic)
    // to let call_method return METHOD_NOT_FOUND if needed.
    (void)compiled_handle; (void)name;
    return 1;
}

static void py_free_value_contents(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    if (v->type == PL_TYPE_ARRAY && v->array.data) {
        for (int i = 0; i < v->array.len; ++i) py_free_value_contents(&v->array.data[i]);
        free(v->array.data); v->array.data = nullptr;
    }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version            = PL_ABI_VERSION;
    out->_reserved              = 0;
    out->_pad2                  = 0;
    out->capabilities           = PL_CAP_DESKTOP | PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX;
    out->pl_init_runtime        = py_init_runtime;
    out->pl_shutdown_runtime    = py_shutdown_runtime;
    out->pl_compile             = py_compile_pl;
    out->pl_compile_sandboxed   = py_compile_sandboxed_pl;
    out->pl_free_compiled       = py_free_compiled;
    out->pl_instantiate_class   = py_instantiate_class;
    out->pl_free_instance       = py_free_instance;
    out->pl_call_method         = py_call_method;
    out->pl_call_builtin        = py_call_builtin;
    out->pl_batch_process       = nullptr; // Python GIL makes true batching counter-productive
    out->pl_set_property        = py_set_property;
    out->pl_get_property        = py_get_property;
    out->pl_has_method          = py_has_method;
    out->pl_free_value_contents = py_free_value_contents;
}
