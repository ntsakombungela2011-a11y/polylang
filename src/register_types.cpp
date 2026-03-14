// =============================================================
// register_types.cpp  —  PolyLang v6.5
//
// FIX C-7: signal_connect and signal_disconnect shims now implemented
//           and injected into PLRuntimeServices.
//   BEFORE: Both pointers were zero-initialised (nullptr).
//           Any adapter calling svc.signal_connect() crashed.
//
// FIX C-8: svc.call_super = PLCrossInherit::pl_call_super_impl;
//   BEFORE: call_super was never set — any adapter calling it crashed.
//
// FIX C-9: PLCrossInherit::destroy() called in uninitialize_polylang.
//   BEFORE: singleton_ was new'd in get_singleton() but never deleted.
//           On editor hot-reload, stale pointer caused UB on next use.
//
// FIX C-4 (scheduler shutdown): coro_scheduler_singleton->shutdown()
//           called before memdelete to disconnect all signal listeners.
// =============================================================
#include "register_types.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "runtime_manager.hpp"
#include "compile_cache.hpp"
#include "hot_reload_scheduler.hpp"
#include "variant_bridge.hpp"

#include "pl_signal_bus.hpp"
#include "pl_bridge.hpp"
#include "pl_resource_bridge.hpp"
#include "pl_profiler.hpp"
#include "pl_async_runtime.hpp"
#include "pl_coroutine_scheduler.hpp"
#include "pl_engine_api_bridge.hpp"
#include "pl_cross_inherit.hpp"
#include "pl_polyglot_script.hpp"

#include "polylang_language.hpp"
#include "polylang_script.hpp"
#include "polylang_script_instance.hpp"

using namespace godot;
using namespace polylang;

static PolyLangLanguage*      language_singleton        = nullptr;
static PLSignalBus*           signal_bus_singleton      = nullptr;
static PolyLangBridge*        bridge_singleton          = nullptr;
static PLResourceBridge*      resource_bridge_singleton = nullptr;
static PLProfiler*            profiler_singleton        = nullptr;
static PLAsyncRuntime*        async_runtime_singleton   = nullptr;
static PLCoroutineScheduler*  coro_scheduler_singleton  = nullptr;
static PLEngineAPIBridge*     engine_api_singleton      = nullptr;

// ── Native listener cookie type ───────────────────────────────
// Used to bridge PLRuntimeServices signal_connect/disconnect to PLSignalBus.
struct NativeListenerCookie {
    std::string signal_name;// VLN-14: needed to match on disconnect
    uint64_t listener_id{0};
    void*    userdata{nullptr};
    void  (*callback)(PLValue* args, int32_t argc, void* userdata){nullptr};
};

// We keep a global list of cookies so signal_disconnect can find them.
// Protected by a simple mutex — adapter lifecycle is init-time, not hot path.
static std::mutex                                       g_listener_mutex;
static std::vector<NativeListenerCookie>                g_listener_cookies;

// ── C-callable shims (PLRuntimeServices) ─────────────────────

static void c_signal_emit(const char* name, PLValue* args, int32_t argc) {
    auto* bus = PLSignalBus::get_singleton();
    if (!bus) return;
    godot::Array gd_args;
    for (int32_t i = 0; i < argc; ++i)
        gd_args.push_back(VariantBridge::from_pl_value(args[i]));
    bus->emit_native(name ? name : "", gd_args);
}

// FIX C-7: Implemented signal_connect shim.
static void c_signal_connect(const char* signal_name,
                              void (*callback)(PLValue* args, int32_t argc, void* userdata),
                              void* userdata) {
    auto* bus = PLSignalBus::get_singleton();
    if (!bus || !signal_name || !callback) return;

    NativeListenerCookie cookie;
    cookie.signal_name = sname;   // VLN-14: store for disconnect matching
    cookie.userdata    = userdata;
    cookie.callback    = callback;

    std::string sname(signal_name);
    // connect_native bridges the godot::Array args to PLValue args.
    cookie.listener_id = bus->connect_native(sname,
        [callback, userdata](const godot::Array& arr) {
            // Convert Godot args to PLValues on-stack (max 16).
            const int32_t n = std::min((int)arr.size(), 16);
            PLValue pl_args[16];
            for (int32_t i = 0; i < n; ++i) {
                const godot::Variant& v = arr[i];
                VariantBridge::to_pl_value(v, pl_args[i]);
            }
            callback(pl_args, n, userdata);
            for (int32_t i = 0; i < n; ++i)
                VariantBridge::free_pl_value(pl_args[i]);
        });

    std::lock_guard<std::mutex> lk(g_listener_mutex);
    g_listener_cookies.push_back(cookie);
}

// FIX C-7: Implemented signal_disconnect shim.
// FIX VLN-14: Match on BOTH signal_name AND userdata.
// Matching on userdata alone caused the wrong listener to be disconnected
// when two adapters registered different signals with the same userdata (e.g. nullptr).
static void c_signal_disconnect(const char* signal_name, void* userdata) {
    auto* bus = PLSignalBus::get_singleton();
    if (!bus) return;

    std::string sname = signal_name ? signal_name : "";
    std::lock_guard<std::mutex> lk(g_listener_mutex);
    for (auto it = g_listener_cookies.begin(); it != g_listener_cookies.end(); ++it) {
        if (it->userdata == userdata && it->signal_name == sname) {
            bus->disconnect_native(it->listener_id);
            g_listener_cookies.erase(it);
            return;
        }
    }
}

static int c_resource_fetch(const char* path, PLValue* out) {
    return PLResourceBridge::pl_resource_fetch_impl(path, out, SandboxTier::Trusted);
}
static void c_resource_release(PLValue* v) {
    PLResourceBridge::pl_resource_release_impl(v);
}
static void c_profiler_begin(const char* lbl) { PLProfiler::pl_profiler_begin_impl(lbl); }
static void c_profiler_end(const char* lbl)   { PLProfiler::pl_profiler_end_impl(lbl);   }
static int  c_engine_call(const char* g, const char* m, PLValue* a, int32_t n, PLValue* r) {
    return PLEngineAPIBridge::pl_engine_call_trusted(g, m, a, n, r);
}

// Odin-specific injection (weak symbol from adapter).
extern "C" void polylang_odin_inject_services(
    void(*)(const char*, PLValue*, int32_t),
    void(*)(const char*, void(*)(PLValue*, int32_t, void*), void*),
    int(*)(const char*, PLValue*),
    void(*)(PLValue*),
    void(*)(const char*),
    void(*)(const char*),
    int(*)(const char*, const char*, PLValue*, int32_t, PLValue*)
) __attribute__((weak));

static void inject_all_adapter_services() {
    // Build the full services struct once.
    PLRuntimeServices svc{};
    svc.signal_emit       = c_signal_emit;
    svc.signal_connect    = c_signal_connect;    // FIX C-7
    svc.signal_disconnect = c_signal_disconnect; // FIX C-7
    svc.resource_fetch    = c_resource_fetch;
    svc.resource_release  = c_resource_release;
    svc.profiler_begin    = c_profiler_begin;
    svc.profiler_end      = c_profiler_end;
    svc.engine_call       = c_engine_call;
    svc.call_super        = PLCrossInherit::pl_call_super_impl; // FIX C-8

    // Odin adapter (dedicated injection entry point).
    if (polylang_odin_inject_services) {
        polylang_odin_inject_services(
            c_signal_emit, c_signal_connect,
            c_resource_fetch, c_resource_release,
            c_profiler_begin, c_profiler_end, c_engine_call);
    }

    // Generic pl_inject_services hook for all other adapters.
    auto* rm = RuntimeManager::get_singleton();
    if (!rm) return;
    for (int i = 0; i < LANGUAGE_COUNT; ++i) {
        auto lid = static_cast<LanguageID>(i);
        auto* vt = rm->get_vtable(lid);
        if (!vt || !vt->pl_inject_services) continue;
        vt->pl_inject_services(&svc);
    }
}

// ── initialize ────────────────────────────────────────────────

void initialize_polylang(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

    ClassDB::register_class<PolyLangLanguage>();
    ClassDB::register_class<PolyLangScript>();
    ClassDB::register_class<PolyglotScript>();
    ClassDB::register_class<PLSignalBus>();
    ClassDB::register_class<PolyLangBridge>();
    ClassDB::register_class<PLResourceBridge>();
    ClassDB::register_class<PLProfiler>();
    ClassDB::register_class<PLAsyncRuntime>();
    ClassDB::register_class<PLCoroutineScheduler>();
    ClassDB::register_class<PLEngineAPIBridge>();

    RuntimeManager::create();
    CompileCache::create();
    HotReloadScheduler::create();
    PLScriptRegistry::create();
    PLCrossInherit::get_singleton(); // lazy-init singleton

    signal_bus_singleton = memnew(PLSignalBus);
    PLSignalBus::singleton_ = signal_bus_singleton;
    Engine::get_singleton()->register_singleton("PLSignalBus", signal_bus_singleton);

    bridge_singleton = memnew(PolyLangBridge);
    PolyLangBridge::singleton_ = bridge_singleton;
    Engine::get_singleton()->register_singleton("PolyLangBridge", bridge_singleton);

    resource_bridge_singleton = memnew(PLResourceBridge);
    PLResourceBridge::singleton_ = resource_bridge_singleton;
    Engine::get_singleton()->register_singleton("PLResourceBridge", resource_bridge_singleton);

    profiler_singleton = memnew(PLProfiler);
    PLProfiler::singleton_ = profiler_singleton;
    Engine::get_singleton()->register_singleton("PLProfiler", profiler_singleton);

    async_runtime_singleton = memnew(PLAsyncRuntime);
    PLAsyncRuntime::singleton_ = async_runtime_singleton;
    async_runtime_singleton->start();
    Engine::get_singleton()->register_singleton("PLAsyncRuntime", async_runtime_singleton);

    coro_scheduler_singleton = memnew(PLCoroutineScheduler);
    PLCoroutineScheduler::singleton_ = coro_scheduler_singleton;
    Engine::get_singleton()->register_singleton("PLCoroutineScheduler", coro_scheduler_singleton);

    engine_api_singleton = memnew(PLEngineAPIBridge);
    PLEngineAPIBridge::singleton_ = engine_api_singleton;
    Engine::get_singleton()->register_singleton("PLEngineAPIBridge", engine_api_singleton);

    language_singleton = memnew(PolyLangLanguage);
    Engine::get_singleton()->register_script_language(language_singleton);

    // Inject services LAST — all singletons must be live first.
    inject_all_adapter_services();
}

// ── uninitialize ──────────────────────────────────────────────

void uninitialize_polylang(ModuleInitializationLevel p_level) {
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;

    // Stop background threads before any singleton teardown.
    if (async_runtime_singleton)       async_runtime_singleton->stop();

    // FIX C-4: Disconnect all signal listeners before scheduler is deleted.
    if (coro_scheduler_singleton)      coro_scheduler_singleton->shutdown();

    if (language_singleton) {
        Engine::get_singleton()->unregister_script_language(language_singleton);
        memdelete(language_singleton); language_singleton = nullptr;
    }

    auto unreg = [](const char* n, auto*& p) {
        if (!p) return;
        Engine::get_singleton()->unregister_singleton(n);
        memdelete(p); p = nullptr;
    };
    unreg("PLEngineAPIBridge",    engine_api_singleton);
    unreg("PLCoroutineScheduler", coro_scheduler_singleton);
    unreg("PLAsyncRuntime",       async_runtime_singleton);
    unreg("PLProfiler",           profiler_singleton);
    unreg("PLResourceBridge",     resource_bridge_singleton);
    unreg("PolyLangBridge",       bridge_singleton);
    unreg("PLSignalBus",          signal_bus_singleton);

    PLSignalBus::singleton_          = nullptr;
    PolyLangBridge::singleton_       = nullptr;
    PLResourceBridge::singleton_     = nullptr;
    PLProfiler::singleton_           = nullptr;
    PLAsyncRuntime::singleton_       = nullptr;
    PLCoroutineScheduler::singleton_ = nullptr;
    PLEngineAPIBridge::singleton_    = nullptr;

    // FIX C-9: Destroy PLCrossInherit singleton — was never deleted in v6.4.
    PLCrossInherit::destroy();

    // Clear native listener cookie table.
    {
        std::lock_guard<std::mutex> lk(g_listener_mutex);
        g_listener_cookies.clear();
    }

    PLScriptRegistry::destroy();
    HotReloadScheduler::destroy();
    CompileCache::destroy();
    RuntimeManager::destroy();
}

extern "C" {
GDExtensionBool GDE_EXPORT polylang_library_init(
        GDExtensionInterfaceGetProcAddress p_get_proc_address,
        const GDExtensionClassLibraryPtr   p_library,
        GDExtensionInitialization*         r_initialization) {
    godot::GDExtensionBinding::InitObject init_obj(
        p_get_proc_address, p_library, r_initialization);
    init_obj.register_initializer(initialize_polylang);
    init_obj.register_terminator(uninitialize_polylang);
    init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
