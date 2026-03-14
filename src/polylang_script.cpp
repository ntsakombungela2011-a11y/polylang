// =============================================================
// polylang_script.cpp  —  PolyLang Script v5
// =============================================================
// SANDBOX BYPASS FIX (all three entry points):
//
//   Before this fix:
//     compile_internal(), _reload(), and _instance_create() all called
//     CompileCache::get_or_compile() unconditionally, even for scripts
//     that RuntimeManager had registered as sandboxed.
//
//   After this fix:
//     compile_internal() calls maybe_register_sidecar() first (which
//     reads the .polylang_config sidecar and auto-registers the path
//     in RuntimeManager if needed). Then it checks is_sandboxed() and
//     routes to get_or_compile_sandboxed() when true.
//
//     _reload() enqueues an async sandbox-aware reload via
//     HotReloadScheduler (which was already sandbox-aware from SECTION 7).
//
//     _instance_create() falls through to compile_internal() if no
//     compiled handle exists, which now correctly applies sandbox routing.
// =============================================================
#include "polylang_script.hpp"
#include "polylang_script_instance.hpp"
#include "polylang_language.hpp"
#include "compile_cache.hpp"
#include "runtime_manager.hpp"
#include "hot_reload_scheduler.hpp"
#include "pl_export_parser.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace polylang {

PolyLangScript::PolyLangScript() = default;

PolyLangScript::~PolyLangScript() {
    {
        std::shared_lock lk(instances_mutex_);
        if (!instances_.empty()) {
            ERR_PRINT("[PolyLang] Script destroyed with live instances — Godot lifecycle error.");
        }
    }

    void* h = compiled_handle_.load(std::memory_order_acquire);
    if (h && vtable_) {
        // Release from cache using the correct key (sandboxed or trusted).
        std::string path = get_path().utf8().get_data();
        RuntimeManager* rm = RuntimeManager::get_singleton();
        if (rm->is_sandboxed(path)) {
            uint32_t caps = rm->sandboxed_caps(path);
            std::string key = CompileCache::sandboxed_key(path, caps);
            CompileCache::get_singleton()->release(key, vtable_);
        } else {
            CompileCache::get_singleton()->release(path, vtable_);
        }
        compiled_handle_.store(nullptr, std::memory_order_release);
    }

    if (pending_handle_ && vtable_ && vtable_->pl_free_compiled) {
        vtable_->pl_free_compiled(pending_handle_);
        pending_handle_ = nullptr;
    }

    if (language_id_ != LanguageID::COUNT)
        RuntimeManager::get_singleton()->notify_instance_destroyed(language_id_);
}

// ── Source code ───────────────────────────────────────────────

void PolyLangScript::set_source_code(const godot::String& p_code) {
    std::unique_lock lk(source_mutex_);
    source_code_ = p_code;
}

godot::String PolyLangScript::get_source_code() const {
    std::shared_lock lk(source_mutex_);
    return source_code_;
}

bool PolyLangScript::_has_source_code() const {
    std::shared_lock lk(source_mutex_);
    return !source_code_.is_empty();
}

// ── Language detection ────────────────────────────────────────

void PolyLangScript::detect_language_from_path() {
    std::string path = get_path().utf8().get_data();
    language_id_ = language_from_path(path.c_str());
    if (language_id_ == LanguageID::COUNT) {
        WARN_PRINT(("[PolyLang] Cannot detect language from path: " + path).c_str());
        return;
    }
    vtable_ = RuntimeManager::get_singleton()->require_vtable(language_id_);
}

// ── Compilation ───────────────────────────────────────────────

godot::Error PolyLangScript::trigger_compile() {
    detect_language_from_path();
    if (!vtable_) return godot::ERR_UNAVAILABLE;
    compile_internal();
    return compiled_handle_.load() ? godot::OK : godot::ERR_COMPILATION_FAILED;
}

void PolyLangScript::compile_internal() {
    if (!vtable_ || !vtable_->pl_compile) return;

    std::string source;
    {
        std::shared_lock lk(source_mutex_);
        source = source_code_.utf8().get_data();
    }
    if (source.empty()) return;

    std::string path = get_path().utf8().get_data();

    // SANDBOX FIX — Step 1:
    //   Check for .polylang_config sidecar and auto-register if found.
    //   This must happen before the is_sandboxed() check below so that
    //   scripts that have not been explicitly pre-registered are covered.
    RuntimeManager::get_singleton()->maybe_register_sidecar(path);

    // SANDBOX FIX — Step 2:
    //   Route to the correct compile path based on sandbox registration.
    RuntimeManager* rm = RuntimeManager::get_singleton();
    void* handle = nullptr;

    if (rm->is_sandboxed(path)) {
        uint32_t caps = rm->sandboxed_caps(path);
        handle = CompileCache::get_singleton()->get_or_compile_sandboxed(
            path, source, vtable_, caps);
    } else {
        handle = CompileCache::get_singleton()->get_or_compile(
            path, source, vtable_);
    }

    if (!handle) {
        ERR_PRINT(("[PolyLang] Compilation failed: " + path).c_str());
        return;
    }
    compiled_handle_.store(handle, std::memory_order_release);
}

// ── _reload ───────────────────────────────────────────────────

void PolyLangScript::_reload(bool /*keep_state*/) {
    // Read source from disk (works inside .pck).
    godot::String gd_path = get_path();
    godot::Ref<godot::FileAccess> fa = godot::FileAccess::open(
        gd_path, godot::FileAccess::READ);
    if (fa.is_valid()) {
        set_source_code(fa->get_as_text());
    }

    detect_language_from_path();
    if (!vtable_) return;

    std::string path = gd_path.utf8().get_data();

    // SANDBOX FIX — Step 3 (async reload path):
    //   Sidecar check first so RuntimeManager is primed before the scheduler
    //   worker thread reads is_sandboxed(). The scheduler also calls
    //   maybe_register_sidecar() internally, but calling it here on the main
    //   thread avoids a race on first load.
    RuntimeManager::get_singleton()->maybe_register_sidecar(path);

    // HotReloadScheduler reads sandbox state from RuntimeManager itself.
    HotReloadScheduler::get_singleton()->enqueue_reload(this, path);
}

// ── Instance registry ─────────────────────────────────────────

void PolyLangScript::register_instance(PolyLangScriptInstance* inst) {
    std::unique_lock lk(instances_mutex_);
    instances_.insert(inst);
    if (language_id_ != LanguageID::COUNT)
        RuntimeManager::get_singleton()->notify_instance_created(language_id_);
}

void PolyLangScript::unregister_instance(PolyLangScriptInstance* inst) {
    std::unique_lock lk(instances_mutex_);
    instances_.erase(inst);
    if (language_id_ != LanguageID::COUNT)
        RuntimeManager::get_singleton()->notify_instance_destroyed(language_id_);
}

std::vector<PolyLangScriptInstance*> PolyLangScript::snapshot_instances() const {
    std::shared_lock lk(instances_mutex_);
    return std::vector<PolyLangScriptInstance*>(instances_.begin(), instances_.end());
}

void PolyLangScript::apply_new_compiled_handle(void* new_handle) {
    compiled_handle_.store(new_handle, std::memory_order_release);
    pending_handle_ = nullptr;
}

// ── ScriptExtension API ───────────────────────────────────────

bool PolyLangScript::_can_instantiate() const {
    return compiled_handle_.load(std::memory_order_acquire) != nullptr;
}

bool PolyLangScript::_is_valid() const {
    return compiled_handle_.load(std::memory_order_acquire) != nullptr;
}

bool PolyLangScript::_has_method(const godot::StringName& p_method) const {
    void* ch = compiled_handle_.load(std::memory_order_acquire);
    if (!ch || !vtable_ || !vtable_->pl_has_method) return false;
    return vtable_->pl_has_method(ch, p_method.utf8().get_data()) != 0;
}

void* PolyLangScript::_instance_create(godot::Object* p_object) const {
    void* ch = compiled_handle_.load(std::memory_order_acquire);

    // SANDBOX FIX — Step 4:
    //   If no compiled handle exists (script was attached at runtime before
    //   _reload fired), compile now. compile_internal() applies the full
    //   sandbox routing, including sidecar detection.
    if (!ch && vtable_) {
        const_cast<PolyLangScript*>(this)->compile_internal();
        ch = compiled_handle_.load(std::memory_order_acquire);
    }

    if (!ch || !vtable_ || !vtable_->pl_instantiate_class) return nullptr;

    std::string path = get_path().utf8().get_data();
    void* foreign = vtable_->pl_instantiate_class(ch, path.c_str());
    if (!foreign) return nullptr;

    auto* inst = new PolyLangScriptInstance(
        const_cast<PolyLangScript*>(this), p_object, foreign);
    const_cast<PolyLangScript*>(this)->register_instance(inst);
    return inst;
}

godot::ScriptLanguage* PolyLangScript::_get_language() const {
    return PolyLangLanguage::get_singleton();
}

godot::Variant PolyLangScript::_get_method_info(const godot::StringName& p_method) const {
    godot::Dictionary d;
    d["name"]  = p_method;
    d["flags"] = 1;
    return d;
}

godot::TypedArray<godot::Dictionary> PolyLangScript::_get_script_method_list() const {
    godot::TypedArray<godot::Dictionary> arr;
    void* ch = compiled_handle_.load(std::memory_order_acquire);
    if (!ch || !vtable_ || !vtable_->pl_get_method_list) return arr;
    PLMethodInfo* methods = nullptr;
    int32_t count = 0;
    vtable_->pl_get_method_list(ch, &methods, &count);
    for (int32_t i = 0; i < count; ++i) {
        godot::Dictionary d;
        d["name"]  = godot::String(methods[i].name);
        d["args"]  = godot::Array();
        d["flags"] = 1;
        arr.push_back(d);
    }
    if (methods && vtable_->pl_free_method_list)
        vtable_->pl_free_method_list(methods);
    return arr;
}

godot::TypedArray<godot::Dictionary> PolyLangScript::_get_script_property_list() const {
    // Priority 1: adapter-provided property list (adapters that introspect types)
    void* ch = compiled_handle_.load(std::memory_order_acquire);
    if (ch && vtable_ && vtable_->pl_get_property_list) {
        PLPropertyInfo* props = nullptr;
        int32_t count = 0;
        vtable_->pl_get_property_list(ch, &props, &count);
        if (count > 0) {
            godot::TypedArray<godot::Dictionary> arr;
            for (int32_t i = 0; i < count; ++i) {
                godot::Dictionary d;
                d["name"]        = godot::String(props[i].name);
                d["type"]        = props[i].type_hint;
                d["hint"]        = 0;
                d["hint_string"] = godot::String("");
                d["usage"]       = 6;
                arr.push_back(d);
            }
            if (props && vtable_->pl_free_property_list)
                vtable_->pl_free_property_list(props);
            return arr;
        }
        if (props && vtable_->pl_free_property_list)
            vtable_->pl_free_property_list(props);
    }

    // Priority 2: @export var annotation parser (language-agnostic)
    std::string source;
    {
        std::shared_lock lk(source_mutex_);
        source = source_code_.utf8().get_data();
    }
    if (!source.empty()) {
        auto props = PLExportParser::parse(source);
        if (!props.empty())
            return PLExportParser::to_property_array(props);
    }

    return godot::TypedArray<godot::Dictionary>();
}

void PolyLangScript::_bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("set_source_code", "code"),
        &PolyLangScript::set_source_code);
    godot::ClassDB::bind_method(
        godot::D_METHOD("get_source_code"),
        &PolyLangScript::get_source_code);
    godot::ClassDB::bind_method(
        godot::D_METHOD("trigger_compile"),
        &PolyLangScript::trigger_compile);
}

} // namespace polylang
