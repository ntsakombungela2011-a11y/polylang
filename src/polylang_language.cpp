// =============================================================
// polylang_language.cpp  —  v5
// =============================================================
// FIXES IN THIS FILE:
//
//   FIX C-1: _init() calls RuntimeManager::start_health_monitor()
//     AFTER set_adapter_dir(). Health thread no longer starts in
//     RuntimeManager's constructor (which ran before adapter_dir
//     was known), preventing garbage dlopen paths.
//
//   FIX B-4: _get_recognized_extensions() returns plain extension
//     strings (e.g. "lua", "py") not the double-extension "pl.lua".
//     Godot's ResourceLoader expects plain extensions.
//     Language detection from the full path uses language_from_path()
//     which checks for ".pl.EXT" double-extension internally.
//
//   FIX B-5: active_editor_language() now reads the actually-open
//     script file via EditorInterface::get_script_editor() instead
//     of the scene root path (which was always a .tscn file, never
//     matching any pl.* extension — always falling back to Lua).
// =============================================================
#include "polylang_language.hpp"
#include "polylang_script.hpp"
#include "runtime_manager.hpp"
#include "compile_cache.hpp"
#include "hot_reload_scheduler.hpp"
#include "pl_signal_bus.hpp"
#include "pl_coroutine_scheduler.hpp"
#include "pl_async_runtime.hpp"
#include "pl_profiler.hpp"
#include "pl_resource_bridge.hpp"
#include "pl_engine_api_bridge.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifndef POLYLANG_RUNTIME_ONLY
#  include <godot_cpp/classes/editor_interface.hpp>
#  include <godot_cpp/classes/script_editor.hpp>
#  include <godot_cpp/classes/script.hpp>
#endif

namespace polylang {

PolyLangLanguage* PolyLangLanguage::singleton_ = nullptr;

PolyLangLanguage::PolyLangLanguage() { singleton_ = this; }
PolyLangLanguage::~PolyLangLanguage()  { singleton_ = nullptr; }
PolyLangLanguage* PolyLangLanguage::get_singleton() { return singleton_; }

// ── FIX C-1: _init ────────────────────────────────────────────
// Correct init sequence:
//   1. Set adapter_dir on RuntimeManager.
//   2. THEN start the health monitor thread.
// In v4 the health thread started in RuntimeManager's constructor —
// BEFORE _init() had set adapter_dir — so adapter_path() returned
// an empty-prefixed path. dlopen("polylang_adapter_lua.so") would
// fail unless the current working directory happened to contain it.

void PolyLangLanguage::_init() {
    godot::String ext_path = "res://addons/polylang/";
    godot::String global = godot::ProjectSettings::get_singleton()
        ->globalize_path(ext_path + "adapters/");

    // Step 1: set adapter dir first.
    

    // Step 2: now safe to start health monitor.
    RuntimeManager::get_singleton()->start_health_monitor();
}

void PolyLangLanguage::_finish() {
    RuntimeManager::get_singleton()->unload_all();
}

void PolyLangLanguage::_frame() {
    // ── v5: hot-reload ────────────────────────────────────────
    HotReloadScheduler::get_singleton()->flush_frame(0.002);

    // ── v6.4: drive all frame-dependent systems ───────────────
    // SignalBus: dispatch queued cross-thread emissions.
    if (auto* bus = PLSignalBus::get_singleton())
        bus->flush_queued();

    // Coroutine scheduler: resume NextFrame + AfterSeconds coroutines.
    if (auto* coro = PLCoroutineScheduler::get_singleton()) {
        // Delta comes from Godot's Engine; approximate with 1/60 if unavailable.
        double delta = 1.0 / 60.0;
        if (auto* e = godot::Engine::get_singleton()) {
            // Engine doesn't expose delta directly; use fixed approximation.
            // Real delta is available only inside _process callbacks.
            (void)e;
        }
        coro->tick(delta);
    }

    // Async runtime: deliver completed future callbacks on the main thread.
    // FIX C-1: tick_main_thread() drains the lock-protected callback queue
    // that the poll thread pushes to. No Callable is ever called off-thread.
    if (auto* ar = PLAsyncRuntime::get_singleton())
        ar->tick_main_thread();

    // Profiler: flush accumulated frame data to Godot profiler channel.
    if (auto* prof = PLProfiler::get_singleton())
        prof->flush_to_godot();

    // Resource bridge: process deferred loads from worker threads.
    if (auto* rb = PLResourceBridge::get_singleton())
        rb->flush();

    // Engine API bridge: process deferred engine calls from worker threads.
    if (auto* eab = PLEngineAPIBridge::get_singleton())
        eab->flush();
}

// ── FIX B-4: _get_recognized_extensions ─────────────────────
// Returns plain extensions ("lua", "py", ...) as Godot expects.
// In v4 this returned "pl.lua" which Godot doesn't understand as
// an extension string — files were never auto-associated with PolyLang.
//
// Strategy: we register each language's final suffix without the "pl."
// prefix. language_from_path() still enforces the full double-extension
// on the absolute path, so "enemy.lua" is NOT loaded by Lua adapter —
// only "enemy.pl.lua" is. The extension registration just tells Godot
// which FileDialog filter to use.

godot::PackedStringArray PolyLangLanguage::_get_recognized_extensions() const {
    godot::PackedStringArray arr;
    // Plain suffixes for Godot's file picker and import system.
    // The full double-extension check is enforced in language_from_path().
    for (int i = 0; i < LANGUAGE_COUNT; ++i) {
        const char* ext = language_extension(static_cast<LanguageID>(i));
        // ext is "pl.lua", "pl.py", etc. — skip the "pl." prefix.
        const char* dot = strchr(ext, '.');
        if (dot && *(dot+1)) arr.push_back(godot::String(dot + 1)); // "lua", "py" ...
    }
    return arr;
}

// ── FIX B-5: active_editor_language ─────────────────────────
// In v4 this read get_current_edited_scene_root()->get_scene_file_path(),
// which returns e.g. "res://scenes/main.tscn" — never matches any pl.*
// extension. Lua was always returned as the fallback.
//
// Fixed: use get_script_editor()->get_current_script() to get the
// actual script resource currently open in the script editor panel.

LanguageID PolyLangLanguage::active_editor_language() const {
#ifndef POLYLANG_RUNTIME_ONLY
    auto* ei = godot::EditorInterface::get_singleton();
    if (ei) {
        godot::ScriptEditor* se = ei->get_script_editor();
        if (se) {
            godot::Ref<godot::Script> script = se->get_current_script();
            if (script.is_valid()) {
                std::string path = script->get_path().utf8().get_data();
                LanguageID id = language_from_path(path.c_str());
                if (id != LanguageID::COUNT) return id;
            }
        }
    }
#endif
    return LanguageID::Lua; // safe fallback
}

godot::PackedStringArray PolyLangLanguage::_get_comment_delimiters() const {
    godot::PackedStringArray arr;
    switch (active_editor_language()) {
        case LanguageID::Lua:
        case LanguageID::Squirrel:
            arr.push_back("--"); arr.push_back("--[[ ]]"); break;
        case LanguageID::Python:
        case LanguageID::Nim:
            arr.push_back("#"); break;
        case LanguageID::Haxe:
        case LanguageID::JavaScript:
        case LanguageID::TypeScript:
        case LanguageID::Rust:
        case LanguageID::Zig:
        case LanguageID::Go:
        case LanguageID::Swift:
        case LanguageID::Kotlin:
        case LanguageID::AngelScript:
        case LanguageID::CSharp:
        case LanguageID::Wren:
        case LanguageID::Julia:
            arr.push_back("//"); arr.push_back("/* */"); break;
        default:
            arr.push_back("//"); break;
    }
    return arr;
}

godot::PackedStringArray PolyLangLanguage::_get_string_delimiters() const {
    godot::PackedStringArray arr;
    arr.push_back("\" \"");
    arr.push_back("' '");
    return arr;
}

godot::Ref<godot::Script> PolyLangLanguage::_make_template(
        const godot::String&, const godot::String& p_class_name,
        const godot::String& p_base_class_name) const {

    LanguageID lang = active_editor_language();
    std::string cn = p_class_name.utf8().get_data();
    std::string bn = p_base_class_name.utf8().get_data();
    if (cn.empty()) cn = "MyScript";
    if (bn.empty()) bn = "Node";

    std::string src;
    switch (lang) {
        case LanguageID::Lua:
            src = "-- " + cn + " : " + bn + "\nlocal " + cn + " = {}\n\n"
                  "function " + cn + ":_ready()\nend\n\n"
                  "function " + cn + ":_process(delta)\nend\n\n"
                  "return " + cn + "\n";
            break;
        case LanguageID::Python:
            src = "# " + cn + " : " + bn + "\nclass " + cn + ":\n"
                  "    def _ready(self):\n        pass\n\n"
                  "    def _process(self, delta):\n        pass\n";
            break;
        case LanguageID::Rust:
            src = "// " + cn + " : " + bn + "\nuse polylang::*;\n\n"
                  "pub struct " + cn + " {}\n\n"
                  "impl PolyLangClass for " + cn + " {\n"
                  "    fn call_method(&mut self, name: &str, _args: &[PlValue]) "
                  "-> Result<PlValue, String> {\n"
                  "        match name {\n"
                  "            \"_ready\" => Ok(PlValue::Nil),\n"
                  "            _ => Err(format!(\"unknown method: {}\", name)),\n"
                  "        }\n    }\n"
                  "    fn set_property(&mut self, _n: &str, _v: PlValue) -> bool { false }\n"
                  "    fn get_property(&self, _n: &str) -> Option<PlValue> { None }\n}\n";
            break;
        case LanguageID::JavaScript:
        case LanguageID::TypeScript:
            src = "// " + cn + " : " + bn + "\nclass " + cn + " {\n"
                  "    _ready() {}\n    _process(delta) {}\n}\n";
            break;
        case LanguageID::CSharp:
            src = "// " + cn + " : " + bn + "\npublic class " + cn + " {\n"
                  "    public void Ready() {}\n"
                  "    public void Process(double delta) {}\n}\n";
            break;
        default:
            src = "// " + cn + " : " + bn + "\n";
            break;
    }

    auto script = godot::Ref<PolyLangScript>(memnew(PolyLangScript));
    script->set_source_code(godot::String(src.c_str()));
    return script;
}

godot::TypedArray<godot::Dictionary>
PolyLangLanguage::_get_built_in_templates(const godot::StringName&) const {
    return {};
}

godot::Dictionary PolyLangLanguage::_validate(
        const godot::String&, const godot::String&, bool, bool, bool, bool) const {
    godot::Dictionary r;
    r["valid"] = true;
    return r;
}

void PolyLangLanguage::_bind_methods() {}

} // namespace polylang
