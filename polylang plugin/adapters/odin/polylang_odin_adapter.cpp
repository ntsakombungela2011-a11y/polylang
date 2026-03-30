// =============================================================
// polylang_odin_adapter.cpp  —  PolyLang v6.3 Odin Adapter
// =============================================================
// ARCHITECTURE:
//   Odin compiles ahead-of-time to native shared libraries. This
//   adapter acts as a loader/dispatcher:
//
//   1. pl_compile():
//      • Resolve a .pl.odin source path to a pre-built or AOT .so.
//      • Pre-built: look for <res_path>.so next to the source, or
//        check RuntimeManager::odin_script_so_path().
//      • AOT: invoke the odin_build_pipeline.sh to compile the .odin
//        file to a .so in the odin_cache_dir.
//      • dlopen the .so and verify the OdinScript API surface.
//      • Return an OdinCompiledHandle (pinned via new).
//
//   2. pl_instantiate_class():
//      • Call the script .so's odin_script_create_instance().
//      • Return an OdinInstanceHandle (pinned via new).
//
//   3. Method/property dispatch:
//      • Delegate entirely to the loaded script .so exports.
//      • For sandboxed scripts: deny-list checked at dispatch time
//        before forwarding to the .so.
//
//   4. SignalBus, Bridge, Coroutine, Async, Resource, Profiler:
//      • These are injected as function-pointer tables passed to
//        odin_script_init_runtime_services() immediately after the
//        .so is loaded (called once per script .so).
//      • The Odin shim (polylang_odin_shim.odin) stores these pointers
//        and exposes them as package-level procs to user scripts.
//
// ANDROID:
//   Odin targets linux_arm64 for Android (Odin's -target flag).
//   The build pipeline cross-compiles for arm64-v8a. The adapter
//   itself is built by the Godot NDK cross-compile step.
//
// THREAD SAFETY:
//   OdinCompiledHandle is immutable after construction (shared reads safe).
//   OdinInstanceHandle uses a std::mutex for property and method dispatch.
//   All SignalBus emission is deferred to main thread via PLSignalBus.
//
// SANDBOX:
//   pl_compile_sandboxed() records allowed_caps on the handle.
//   At method dispatch, method names matching the deny-list return
//   PL_ERR_SANDBOX before the call reaches the Odin .so.
//   Per-script Odin code may additionally call odin_sandbox_check()
//   from the shim to enforce finer-grained runtime restrictions.
// =============================================================

#include "../../include/pl_adapter_vtable.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  define ODIN_DLOPEN(p)   (void*)LoadLibraryA(p)
#  define ODIN_DLSYM(h,s)  (void*)GetProcAddress((HMODULE)(h),(s))
#  define ODIN_DLCLOSE(h)  FreeLibrary((HMODULE)(h))
#  define PL_WEAK
#  define DL_ERROR_STR()   "LoadLibrary failed"
#else
#  include <dlfcn.h>
#  define ODIN_DLOPEN(p)   dlopen((p), RTLD_NOW | RTLD_LOCAL)
#  define ODIN_DLSYM(h,s)  dlsym((h),(s))
#  define ODIN_DLCLOSE(h)  dlclose(h)
#  define PL_WEAK          __attribute__((weak))
#  define DL_ERROR_STR()   dlerror()
#endif

// ── Forward declarations of runtime service injectors ─────────
// These are resolved at runtime when the SignalBus / Bridge /
// Resource / Profiler singletons are available.
// The adapter's pl_init_runtime() installs stubs; the core later
// calls odin_set_runtime_services() to patch in real implementations.

extern "C" {
    // Injected from pl_signal_bus.cpp / pl_bridge.cpp at init time
    // via the OdinRuntimeServices table below.
    void polylang_signal_bus_emit(const char* name, PLValue* args, int32_t argc);
    uint64_t polylang_signal_bus_connect_native(const char* name,
                                                void (*cb)(PLValue* args, int32_t argc,
                                                           void* userdata),
                                                void* userdata);
    void polylang_signal_bus_disconnect_native(uint64_t id);
    int  polylang_bridge_call(const char* target_path, const char* method,
                              PLValue* args, int32_t argc, PLValue* ret_out);
    int  polylang_resource_fetch(const char* res_path, PLValue* out);
    void polylang_resource_release(PLValue* v);
    void polylang_profiler_begin(const char* label);
    void polylang_profiler_end(const char* label);
}

// ── Runtime services table injected into each script .so ─────
// Matches OdinRuntimeServices in polylang_odin_shim.odin.
typedef struct OdinRuntimeServices {
    void     (*signal_emit)(const char* name, PLValue* args, int32_t argc);
    uint64_t (*signal_connect)(const char* name,
                                void (*cb)(PLValue*, int32_t, void*), void* ud);
    void     (*signal_disconnect)(uint64_t id);
    int      (*bridge_call)(const char* path, const char* method,
                             PLValue* args, int32_t argc, PLValue* ret);
    int      (*resource_fetch)(const char* path, PLValue* out);
    void     (*resource_release)(PLValue* v);
    void     (*profiler_begin)(const char* label);
    void     (*profiler_end)(const char* label);
} OdinRuntimeServices;

// ── Script .so exports the adapter expects ────────────────────
// All are prefixed odin_script_ and have C linkage in the shim.
typedef struct OdinScriptAPI {
    // Mandatory
    void* (*create_instance)(void);
    void  (*free_instance)(void* inst);
    int   (*call_method)(void* inst, const char* name,
                          PLValue* args, int32_t argc, PLValue* ret);
    int   (*set_property)(void* inst, const char* name, const PLValue* v);
    int   (*get_property)(void* inst, const char* name, PLValue* out);
    uint8_t (*has_method)(const char* name);
    // Optional — check for null before calling
    void  (*get_method_list)(PLMethodInfo** out, int32_t* count);
    void  (*free_method_list)(PLMethodInfo* list);
    void  (*get_property_list)(PLPropertyInfo** out, int32_t* count);
    void  (*free_property_list)(PLPropertyInfo* list);
    int   (*serialize_state)(void* inst, PLValue* out);
    int   (*deserialize_state)(void* inst, const PLValue* state);
    void  (*get_export_vars)(PLExportVarInfo** out, int32_t* count);
    void  (*free_export_vars)(PLExportVarInfo* vars, int32_t count);
    // Coroutine (optional)
    void* (*coroutine_create)(void* inst, const char* method);
    int   (*coroutine_resume)(void* coro, const PLValue* send, PLValue* yield_out);
    void  (*coroutine_free)(void* coro);
    // Async (optional)
    void* (*async_begin)(void* inst, const char* method, PLValue* args, int32_t argc);
    int   (*async_poll)(void* future, PLValue* result_out);
    void  (*async_free)(void* future);
    // Runtime services injection (mandatory)
    void  (*init_runtime_services)(const OdinRuntimeServices* svc);
} OdinScriptAPI;

// ── Sandbox deny-list ─────────────────────────────────────────
static const char* const ODIN_SANDBOX_DENIED[] = {
    "os_open","os_create","os_remove","os_rename","os_mkdir",
    "os_read_entire_file","os_write_entire_file",
    "os_exec","os_run","net_dial","net_listen","net_get","net_post",
    "http_get","http_post","os_get_env","os_set_env",
    "dynlib_load","dynlib_symbol",
    nullptr
};

static bool odin_is_sandbox_denied(const char* name) {
    if (!name) return false;
    for (int i = 0; ODIN_SANDBOX_DENIED[i]; ++i) {
        if (strcmp(name, ODIN_SANDBOX_DENIED[i]) == 0) return true;
        // prefix match: os_open_file → denied if starts with os_open
        size_t dl = strlen(ODIN_SANDBOX_DENIED[i]);
        if (strncmp(name, ODIN_SANDBOX_DENIED[i], dl) == 0) return true;
    }
    return false;
}

// ── Compiled handle ───────────────────────────────────────────
// One per distinct .pl.odin script file; shared across instances.
struct OdinCompiledHandle {
    std::string    res_path;
    std::string    so_path;
    void*          dl_handle{nullptr};
    OdinScriptAPI  api{};
    bool           sandboxed{false};
    uint32_t       allowed_caps{PL_SANDBOX_NONE};
};

// ── Instance handle ───────────────────────────────────────────
struct OdinInstanceHandle {
    OdinCompiledHandle* compiled;       // non-owning borrow
    void*               odin_inst;      // owned by odin_script_free_instance
    mutable std::mutex  call_mutex;
};

// ── Global runtime services (filled by odin_adapter_set_services) ──
static OdinRuntimeServices g_services = {
    polylang_signal_bus_emit,
    polylang_signal_bus_connect_native,
    polylang_signal_bus_disconnect_native,
    polylang_bridge_call,
    polylang_resource_fetch,
    polylang_resource_release,
    polylang_profiler_begin,
    polylang_profiler_end,
};

// ── Weak symbol stubs (overridden by linker when core provides real ones) ─
PL_WEAK
void polylang_signal_bus_emit(const char*, PLValue*, int32_t) {}

PL_WEAK
uint64_t polylang_signal_bus_connect_native(const char*,
    void (*)(PLValue*, int32_t, void*), void*) { return 0; }

PL_WEAK
void polylang_signal_bus_disconnect_native(uint64_t) {}

PL_WEAK
int polylang_bridge_call(const char*, const char*, PLValue*, int32_t, PLValue* ret) {
    if (ret) pl_value_init(ret);
    return PL_ERR_NOT_IMPLEMENTED;
}

PL_WEAK
int polylang_resource_fetch(const char*, PLValue* out) {
    if (out) pl_value_init(out);
    return PL_ERR_NOT_IMPLEMENTED;
}

PL_WEAK
void polylang_resource_release(PLValue*) {}

PL_WEAK
void polylang_profiler_begin(const char*) {}

PL_WEAK
void polylang_profiler_end(const char*) {}

// ── Path helpers ──────────────────────────────────────────────
// EZ-01: Bash strict escaping prevents OS command injection
static std::string escape_sh_arg(const std::string& in) {
    std::string out = "'";
    for (char c : in) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Derive the pre-built .so path from the script res:// path.
// Convention: script.pl.odin → script.pl.odin.so
static std::string derive_so_path(const std::string& res_path) {
    return res_path + ".so";
}

// ── Build pipeline invocation (AOT mode) ─────────────────────
// Calls odin_build_pipeline.sh or odin_build_pipeline_android.sh.
// Returns path to compiled .so on success, empty string on failure.
static std::string invoke_odin_build_pipeline(const std::string& src_path,
                                               const std::string& cache_dir,
                                               bool               is_android) {
    // Hash the src path for a stable output filename.
    size_t h = std::hash<std::string>{}(src_path);
    char hash_buf[24];
    snprintf(hash_buf, sizeof(hash_buf), "%016zx", h);
    std::string out_so = cache_dir + "/odin_" + hash_buf + (is_android ? ".so" : ".dll");
    if (!is_android && !strstr(out_so.c_str(), ".dll")) out_so += ".so"; // generic .so for linux/mac

    std::string script;
#if defined(_WIN32)
    script = "odin_build_pipeline.ps1";
    // PowerShell command with triple-quoted strings for safety
    std::string cmd = "powershell -ExecutionPolicy Bypass -File \"" + script + "\" \"" + src_path + "\" \"" + out_so + "\"";
#else
    script = is_android ? "odin_build_pipeline_android.sh"
                         : "odin_build_pipeline.sh";
    // Build command: <script> <src> <out.so>
    std::string cmd = escape_sh_arg(script) + " " + escape_sh_arg(src_path) + " " + escape_sh_arg(out_so);
#endif

    int rc = system(cmd.c_str());
    if (rc != 0) {
        fprintf(stderr, "[PolyLang/Odin] Build pipeline failed (rc=%d) for %s\n",
                rc, src_path.c_str());
        return "";
    }
    return out_so;
}

// ── Load and verify an Odin script .so ───────────────────────
static bool load_odin_so(OdinCompiledHandle* h) {
    h->dl_handle = ODIN_DLOPEN(h->so_path.c_str());
    if (!h->dl_handle) {
        fprintf(stderr, "[PolyLang/Odin] dlopen failed: %s — %s\n",
                h->so_path.c_str(), DL_ERROR_STR());
        return false;
    }

#define LOAD_SYM(field, sym) \
    h->api.field = reinterpret_cast<decltype(h->api.field)>(ODIN_DLSYM(h->dl_handle, sym)); \
    if (!h->api.field) { \
        fprintf(stderr, "[PolyLang/Odin] missing mandatory export: %s in %s\n", sym, h->so_path.c_str()); \
        ODIN_DLCLOSE(h->dl_handle); h->dl_handle = nullptr; return false; \
    }

    LOAD_SYM(create_instance,       "odin_script_create_instance")
    LOAD_SYM(free_instance,         "odin_script_free_instance")
    LOAD_SYM(call_method,           "odin_script_call_method")
    LOAD_SYM(set_property,          "odin_script_set_property")
    LOAD_SYM(get_property,          "odin_script_get_property")
    LOAD_SYM(has_method,            "odin_script_has_method")
    LOAD_SYM(init_runtime_services, "odin_script_init_runtime_services")
#undef LOAD_SYM

    // Optional exports — null if not present in script .so.
#define LOAD_OPT(field, sym) \
    h->api.field = reinterpret_cast<decltype(h->api.field)>(ODIN_DLSYM(h->dl_handle, sym));
    LOAD_OPT(get_method_list,    "odin_script_get_method_list")
    LOAD_OPT(free_method_list,   "odin_script_free_method_list")
    LOAD_OPT(get_property_list,  "odin_script_get_property_list")
    LOAD_OPT(free_property_list, "odin_script_free_property_list")
    LOAD_OPT(serialize_state,    "odin_script_serialize_state")
    LOAD_OPT(deserialize_state,  "odin_script_deserialize_state")
    LOAD_OPT(get_export_vars,    "odin_script_get_export_vars")
    LOAD_OPT(free_export_vars,   "odin_script_free_export_vars")
    LOAD_OPT(coroutine_create,   "odin_script_coroutine_create")
    LOAD_OPT(coroutine_resume,   "odin_script_coroutine_resume")
    LOAD_OPT(coroutine_free,     "odin_script_coroutine_free")
    LOAD_OPT(async_begin,        "odin_script_async_begin")
    LOAD_OPT(async_poll,         "odin_script_async_poll")
    LOAD_OPT(async_free,         "odin_script_async_free")
#undef LOAD_OPT

    // Inject runtime services into the newly loaded script .so.
    h->api.init_runtime_services(&g_services);
    return true;
}

// ── Adapter globals ───────────────────────────────────────────
static bool g_initialized = false;

// ── Vtable implementations ────────────────────────────────────

static int odin_init_runtime() {
    g_initialized = true;
    return PL_OK;
}

static void odin_shutdown_runtime() {
    g_initialized = false;
}

// Shared compile core: handles pre-built and AOT modes.
static OdinCompiledHandle* compile_core(const char* src, const char* path,
                                         bool sandboxed, uint32_t caps) {
    if (!path) return nullptr;

    std::string res_path(path);
    std::string so_path;

    // 1. Check if the core has registered an explicit .so path.
    //    (Set via RuntimeManager::register_odin_script_so.)
    //    We access g_services to call back through the bridge rather
    //    than linking directly against RuntimeManager here.
    //    For simplicity, check the conventional path first.
    so_path = derive_so_path(res_path);

    // 2. Try to open the .so directly (pre-built or previously AOT'd).
    FILE* probe = fopen(so_path.c_str(), "rb");
    if (!probe) {
        // 3. AOT: invoke build pipeline.
        // Detect Android by checking for android ABI in path or env.
        bool is_android = (getenv("ANDROID_NDK_HOME") != nullptr);
        const char* cache = getenv("POLYLANG_ODIN_CACHE");
        std::string cache_dir = cache ? std::string(cache) : "/tmp/polylang_odin_cache";
        so_path = invoke_odin_build_pipeline(res_path, cache_dir, is_android);
        if (so_path.empty()) return nullptr;
    } else {
        fclose(probe);
    }

    auto* h = new OdinCompiledHandle();
    h->res_path     = res_path;
    h->so_path      = so_path;
    h->sandboxed    = sandboxed;
    h->allowed_caps = caps;

    if (!load_odin_so(h)) {
        delete h;
        return nullptr;
    }
    return h;
}

static void* odin_compile(const char* src, const char* path) {
    return compile_core(src, path, false, PL_SANDBOX_FULL);
}

static void* odin_compile_sandboxed(const char* src, const char* path, uint32_t caps) {
    return compile_core(src, path, true, caps);
}

static void odin_free_compiled(void* h) {
    if (!h) return;
    auto* ch = static_cast<OdinCompiledHandle*>(h);
    if (ch->dl_handle) ODIN_DLCLOSE(ch->dl_handle);
    delete ch;
}

static void* odin_instantiate_class(void* compiled, const char*) {
    if (!compiled) return nullptr;
    auto* ch = static_cast<OdinCompiledHandle*>(compiled);
    void* odin_inst = ch->api.create_instance();
    if (!odin_inst) return nullptr;
    auto* ih  = new OdinInstanceHandle();
    ih->compiled  = ch;
    ih->odin_inst = odin_inst;
    return ih;
}

static void odin_free_instance(void* raw) {
    if (!raw) return;
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    ih->compiled->api.free_instance(ih->odin_inst);
    delete ih;
}

static int odin_call_method(void* raw, const char* name,
                             PLValue* args, int32_t argc, PLValue* ret) {
    if (!raw || !name || !ret) return PL_ERR_GENERIC;
    pl_value_init(ret);
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    const auto* ch = ih->compiled;

    if (ch->sandboxed && odin_is_sandbox_denied(name)) {
        fprintf(stderr, "[PolyLang/Odin/sandbox] method '%s' denied\n", name);
        return PL_ERR_SANDBOX;
    }

    // Profiler hook
    std::string label = ch->res_path + ":" + name;
    polylang_profiler_begin(label.c_str());

    int rc;
    {
        std::lock_guard<std::mutex> lk(ih->call_mutex);
        rc = ch->api.call_method(ih->odin_inst, name, args, argc, ret);
    }

    polylang_profiler_end(label.c_str());
    return rc;
}

static int odin_call_builtin(void* raw, int32_t id,
                              PLValue* args, int32_t argc, PLValue* ret) {
    const char* name = nullptr;
    switch (id) {
        case PL_METHOD_READY:           name = "_ready";           break;
        case PL_METHOD_PROCESS:         name = "_process";         break;
        case PL_METHOD_PHYSICS_PROCESS: name = "_physics_process"; break;
        case PL_METHOD_ENTER_TREE:      name = "_enter_tree";      break;
        case PL_METHOD_EXIT_TREE:       name = "_exit_tree";       break;
        case PL_METHOD_INPUT:           name = "_input";           break;
        case PL_METHOD_UNHANDLED_INPUT: name = "_unhandled_input"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return odin_call_method(raw, name, args, argc, ret);
}

static int odin_set_property(void* raw, const char* name, const PLValue* v) {
    if (!raw || !name || !v) return PL_ERR_GENERIC;
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    if (ih->compiled->sandboxed && odin_is_sandbox_denied(name))
        return PL_ERR_SANDBOX;
    std::lock_guard<std::mutex> lk(ih->call_mutex);
    return ih->compiled->api.set_property(ih->odin_inst, name, v);
}

static int odin_get_property(void* raw, const char* name, PLValue* out) {
    if (!raw || !name || !out) return PL_ERR_GENERIC;
    pl_value_init(out);
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    std::lock_guard<std::mutex> lk(ih->call_mutex);
    return ih->compiled->api.get_property(ih->odin_inst, name, out);
}

static uint8_t odin_has_method(void* compiled, const char* name) {
    if (!compiled || !name) return 0;
    auto* ch = static_cast<OdinCompiledHandle*>(compiled);
    return ch->api.has_method(name);
}

static void odin_get_method_list(void* compiled,
                                  PLMethodInfo** out, int32_t* count) {
    if (!out || !count) return;
    *out = nullptr; *count = 0;
    if (!compiled) return;
    auto* ch = static_cast<OdinCompiledHandle*>(compiled);
    if (ch->api.get_method_list) ch->api.get_method_list(out, count);
}

static void odin_free_method_list(PLMethodInfo* m) {
    // Odin shim owns the allocation; free via the last-loaded .so is not
    // safe across .so reloads. We use malloc in the shim, so free() here.
    free(m);
}

static void odin_get_property_list(void* compiled,
                                    PLPropertyInfo** out, int32_t* count) {
    if (!out || !count) return;
    *out = nullptr; *count = 0;
    if (!compiled) return;
    auto* ch = static_cast<OdinCompiledHandle*>(compiled);
    if (ch->api.get_property_list) ch->api.get_property_list(out, count);
}

static void odin_free_property_list(PLPropertyInfo* p) { free(p); }

static int odin_serialize_state(void* raw, PLValue* out) {
    if (!raw || !out) return PL_ERR_GENERIC;
    pl_value_init(out);
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    if (!ih->compiled->api.serialize_state) return PL_ERR_NOT_IMPLEMENTED;
    std::lock_guard<std::mutex> lk(ih->call_mutex);
    return ih->compiled->api.serialize_state(ih->odin_inst, out);
}

static int odin_deserialize_state(void* raw, const PLValue* state) {
    if (!raw || !state) return PL_ERR_GENERIC;
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    if (!ih->compiled->api.deserialize_state) return PL_ERR_NOT_IMPLEMENTED;
    std::lock_guard<std::mutex> lk(ih->call_mutex);
    return ih->compiled->api.deserialize_state(ih->odin_inst, state);
}

// ── v6 Coroutine ─────────────────────────────────────────────

static void* odin_coroutine_create(void* raw, const char* method_name) {
    if (!raw || !method_name) return nullptr;
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    if (!ih->compiled->api.coroutine_create) return nullptr;
    return ih->compiled->api.coroutine_create(ih->odin_inst, method_name);
}

static int odin_coroutine_resume(void* coro, const PLValue* send, PLValue* yield_out) {
    // coro is the raw Odin coroutine handle from the script .so.
    // We cannot know which compiled handle it belongs to without
    // a side-table. We tag coro handles as {OdinCompiledHandle*, void*}.
    struct TaggedCoro { OdinCompiledHandle* ch; void* inner; };
    if (!coro || !yield_out) return PL_ERR_GENERIC;
    pl_value_init(yield_out);
    auto* tc = static_cast<TaggedCoro*>(coro);
    if (!tc->ch->api.coroutine_resume) return PL_ERR_NOT_IMPLEMENTED;
    return tc->ch->api.coroutine_resume(tc->inner, send, yield_out);
}

static void odin_coroutine_free(void* coro) {
    struct TaggedCoro { OdinCompiledHandle* ch; void* inner; };
    if (!coro) return;
    auto* tc = static_cast<TaggedCoro*>(coro);
    if (tc->ch->api.coroutine_free) tc->ch->api.coroutine_free(tc->inner);
    delete tc;
}

// Tagged coroutine factory — wraps the raw handle from the script .so.
static void* odin_make_tagged_coro(OdinCompiledHandle* ch, void* inner) {
    struct TaggedCoro { OdinCompiledHandle* ch; void* inner; };
    auto* tc = new TaggedCoro{ch, inner};
    return tc;
}

// Override coroutine_create to produce tagged handles.
static void* odin_coroutine_create_tagged(void* raw, const char* method_name) {
    if (!raw || !method_name) return nullptr;
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    if (!ih->compiled->api.coroutine_create) return nullptr;
    void* inner = ih->compiled->api.coroutine_create(ih->odin_inst, method_name);
    if (!inner) return nullptr;
    return odin_make_tagged_coro(ih->compiled, inner);
}

// ── v6 Async ─────────────────────────────────────────────────

struct OdinFuture { OdinCompiledHandle* ch; void* inner; };

static void* odin_async_begin(void* raw, const char* method,
                               PLValue* args, int32_t argc) {
    if (!raw || !method) return nullptr;
    auto* ih = static_cast<OdinInstanceHandle*>(raw);
    if (!ih->compiled->api.async_begin) return nullptr;
    void* inner = ih->compiled->api.async_begin(ih->odin_inst, method, args, argc);
    if (!inner) return nullptr;
    auto* f = new OdinFuture{ih->compiled, inner};
    return f;
}

static int odin_async_poll(void* future, PLValue* result_out) {
    if (!future || !result_out) return PL_ERR_GENERIC;
    pl_value_init(result_out);
    auto* f = static_cast<OdinFuture*>(future);
    if (!f->ch->api.async_poll) return PL_ERR_NOT_IMPLEMENTED;
    return f->ch->api.async_poll(f->inner, result_out);
}

static void odin_async_free(void* future) {
    if (!future) return;
    auto* f = static_cast<OdinFuture*>(future);
    if (f->ch->api.async_free) f->ch->api.async_free(f->inner);
    delete f;
}

// ── v6 Resource ───────────────────────────────────────────────

static int odin_resource_fetch(const char* res_path, PLValue* out) {
    if (!res_path || !out) return PL_ERR_GENERIC;
    return polylang_resource_fetch(res_path, out);
}

static void odin_resource_release(PLValue* v) {
    if (v) polylang_resource_release(v);
}

// ── v6 Profiler ───────────────────────────────────────────────

static void odin_profiler_begin(const char* label) { polylang_profiler_begin(label); }
static void odin_profiler_end(const char* label)   { polylang_profiler_end(label);   }

// ── v6 Export vars ────────────────────────────────────────────

static void odin_get_export_vars(void* compiled,
                                  PLExportVarInfo** out, int32_t* count) {
    if (!out || !count) return;
    *out = nullptr; *count = 0;
    if (!compiled) return;
    auto* ch = static_cast<OdinCompiledHandle*>(compiled);
    if (ch->api.get_export_vars) ch->api.get_export_vars(out, count);
}

// Forward declaration — defined below, used by odin_free_export_vars.
static void pl_free_value_contents_impl(PLValue* v);

static void odin_free_export_vars(PLExportVarInfo* vars, int32_t count) {
    if (!vars) return;
    for (int32_t i = 0; i < count; ++i)
        pl_free_value_contents_impl(&vars[i].default_val);
    free(vars);
}

// pl_free_value_contents_impl — can't call the vtable from within itself.
static void pl_free_value_contents_impl(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING && v->s) { free(v->s); v->s = nullptr; }
    else if (v->type == PL_TYPE_ARRAY && v->array.data) {
        for (int32_t i = 0; i < v->array.len; ++i)
            pl_free_value_contents_impl(&v->array.data[i]);
        free(v->array.data);
        v->array.data = nullptr;
    }
    v->type = PL_TYPE_NIL;
}

static void odin_free_value_contents(PLValue* v) { pl_free_value_contents_impl(v); }

// ── Public API: update runtime services ──────────────────────
// Called by register_types.cpp after singletons are ready.
extern "C" PL_EXPORT
void odin_adapter_set_services(const OdinRuntimeServices* svc) {
    if (svc) g_services = *svc;
}

// ── VTable population (done by polylang_odin_shim.c) ─────────
// The shim's pl_get_vtable() calls odin_fill_vtable() defined here.
extern "C" PL_EXPORT
void odin_fill_vtable(PLAdapterVTable* out) {
    memset(out, 0, sizeof(*out));

    out->abi_version  = PL_ABI_VERSION;
    out->_reserved    = 0;
    out->capabilities =
        PL_CAP_ANDROID | PL_CAP_DESKTOP |
        PL_CAP_BUILTIN_CALL | PL_CAP_SANDBOX |
        PL_CAP_COROUTINE | PL_CAP_ASYNC |
        PL_CAP_RESOURCE  | PL_CAP_PROFILER |
        PL_CAP_EXPORT_VARS;

    out->pl_init_runtime        = odin_init_runtime;
    out->pl_shutdown_runtime    = odin_shutdown_runtime;
    out->pl_compile             = odin_compile;
    out->pl_free_compiled       = odin_free_compiled;
    out->pl_instantiate_class   = odin_instantiate_class;
    out->pl_free_instance       = odin_free_instance;
    out->pl_call_method         = odin_call_method;
    out->pl_call_builtin        = odin_call_builtin;
    out->pl_batch_process       = nullptr;
    out->pl_set_property        = odin_set_property;
    out->pl_get_property        = odin_get_property;
    out->pl_has_method          = odin_has_method;
    out->pl_get_method_list     = odin_get_method_list;
    out->pl_free_method_list    = odin_free_method_list;
    out->pl_get_property_list   = odin_get_property_list;
    out->pl_free_property_list  = odin_free_property_list;
    out->pl_serialize_state     = odin_serialize_state;
    out->pl_deserialize_state   = odin_deserialize_state;
    out->pl_compile_sandboxed   = odin_compile_sandboxed;
    out->pl_free_value_contents = odin_free_value_contents;

    // v6 extensions
    out->pl_coroutine_create  = odin_coroutine_create_tagged;
    out->pl_coroutine_resume  = odin_coroutine_resume;
    out->pl_coroutine_free    = odin_coroutine_free;
    out->pl_async_begin       = odin_async_begin;
    out->pl_async_poll        = odin_async_poll;
    out->pl_async_free        = odin_async_free;
    out->pl_resource_fetch    = odin_resource_fetch;
    out->pl_resource_release  = odin_resource_release;
    out->pl_profiler_begin    = odin_profiler_begin;
    out->pl_profiler_end      = odin_profiler_end;
    out->pl_get_export_vars   = odin_get_export_vars;
    out->pl_free_export_vars  = odin_free_export_vars;
}
