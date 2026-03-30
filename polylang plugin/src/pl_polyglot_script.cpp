// =============================================================
// pl_polyglot_script.cpp  —  PolyLang v6.5 Polyglot Script
// =============================================================
#include "pl_polyglot_script.hpp"

#include <algorithm>
#include <cstring>

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "pl_export_parser.hpp"
#include "pl_sandbox_tiers.hpp"
#include "runtime_manager.hpp"
#include "variant_bridge.hpp"
#include "pl_state_transfer.hpp"

namespace polylang {

// Forward declaration (defined at end of file)
LanguageID language_from_string(const char* name);

// ── BlockHandle ───────────────────────────────────────────────

BlockHandle::~BlockHandle() {
    if (compiled && vtable && vtable->pl_free_compiled) {
        vtable->pl_free_compiled(compiled);
        compiled = nullptr;
    }
}

BlockHandle::BlockHandle(BlockHandle&& o) noexcept
    : language(std::move(o.language)), lang_id(o.lang_id),
      vtable(o.vtable), compiled(o.compiled),
      sandboxed(o.sandboxed), allowed_caps(o.allowed_caps) {
    o.compiled = nullptr;
    o.vtable   = nullptr;
}

// ── Fan-out methods ───────────────────────────────────────────
// These lifecycle methods are dispatched to EVERY block that
// implements them (not just the first), because e.g. _ready
// initialisation from both Lua and Python should both run.
const std::vector<std::string>& PolyglotInstance::fanout_methods() {
    static const std::vector<std::string> v = {
        "_ready", "_process", "_physics_process",
        "_enter_tree", "_exit_tree", "_input", "_unhandled_input"
    };
    return v;
}

static bool is_fanout(const std::string& name) {
    for (const auto& m : PolyglotInstance::fanout_methods())
        if (m == name) return true;
    return false;
}

// ── PolyglotScript ────────────────────────────────────────────

PolyglotScript::PolyglotScript() = default;
PolyglotScript::~PolyglotScript() = default;

void PolyglotScript::_bind_methods() {
    // Expose to GDScript: get block count, list languages
    godot::ClassDB::bind_method(
        godot::D_METHOD("get_block_count"), &PolyglotScript::block_count);
}

bool PolyglotScript::_can_instantiate() const { return valid_; }
bool PolyglotScript::_is_valid() const { return valid_; }

godot::String PolyglotScript::_get_source_code() const { return source_; }
void PolyglotScript::_set_source_code(const godot::String& code) {
    source_ = code;
    valid_  = false;
    export_cache_dirty_ = true;
}

godot::StringName PolyglotScript::_get_instance_base_type() const {
    return godot::String(header_.base_class.c_str());
}

godot::Ref<godot::Script> PolyglotScript::_get_base_script() const {
    return godot::Ref<godot::Script>();
}

godot::StringName PolyglotScript::_get_global_name() const {
    return godot::StringName();
}

bool PolyglotScript::_inherits_script(const godot::Ref<godot::Script>&) const {
    return false;
}

bool PolyglotScript::_has_method(const godot::StringName& method) const {
    std::string mname = std::string(godot::String(method).utf8().get_data());
    std::shared_lock lk(compile_mutex_);
    for (const auto& bh : blocks_) {
        if (!bh || !bh->vtable || !bh->compiled) continue;
        if (bh->vtable->pl_has_method &&
            bh->vtable->pl_has_method(bh->compiled, mname.c_str()))
            return true;
    }
    return false;
}

int PolyglotScript::block_count() const {
    std::shared_lock lk(compile_mutex_);
    return (int)blocks_.size();
}

std::string PolyglotScript::block_language(int i) const {
    std::shared_lock lk(compile_mutex_);
    if (i < 0 || i >= (int)blocks_.size()) return "";
    return blocks_[i] ? blocks_[i]->language : "";
}

std::vector<std::string> PolyglotScript::languages() const {
    std::shared_lock lk(compile_mutex_);
    std::vector<std::string> out;
    for (const auto& bh : blocks_)
        if (bh) out.push_back(bh->language);
    return out;
}

int PolyglotScript::block_index_for_method(const std::string& method) const {
    std::shared_lock lk(compile_mutex_);
    for (int i = 0; i < (int)blocks_.size(); ++i) {
        const auto& bh = blocks_[i];
        if (!bh || !bh->vtable || !bh->compiled) continue;
        if (bh->vtable->pl_has_method &&
            bh->vtable->pl_has_method(bh->compiled, method.c_str()))
            return i;
    }
    return -1;
}

// ── Compile one block ─────────────────────────────────────────
std::unique_ptr<BlockHandle> PolyglotScript::compile_block(const PolyBlock& block) {
    LanguageID lid = language_from_string(block.language.c_str());
    if (lid == LanguageID::COUNT) {
        ERR_PRINT(godot::String("PolyLang/Poly: unknown language: ")
            + block.language.c_str());
        return nullptr;
    }

    auto* rm  = RuntimeManager::get_singleton();
    auto* vt  = rm ? rm->get_vtable(lid) : nullptr;
    if (!vt) {
        ERR_PRINT(godot::String("PolyLang/Poly: adapter not loaded for language: ")
            + block.language.c_str());
        return nullptr;
    }

    // Determine sandbox tier for this block's contribution to the polyglot path.
    std::string pseudo_path = res_path_ + "#" + block.language;
    rm->maybe_register_sidecar(pseudo_path);

    bool     sandboxed    = rm->is_sandboxed(pseudo_path);
    uint32_t allowed_caps = rm->sandboxed_caps(pseudo_path);

    void* compiled = nullptr;
    if (sandboxed && vt->pl_compile_sandboxed) {
        compiled = vt->pl_compile_sandboxed(block.source.c_str(),
                                             pseudo_path.c_str(),
                                             allowed_caps);
    } else if (vt->pl_compile) {
        compiled = vt->pl_compile(block.source.c_str(), pseudo_path.c_str());
    }

    if (!compiled) {
        ERR_PRINT(godot::String("PolyLang/Poly: compile failed for block [")
            + block.language.c_str() + "] in " + res_path_.c_str());
        return nullptr;
    }

    auto bh         = std::make_unique<BlockHandle>();
    bh->language    = block.language;
    bh->lang_id     = lid;
    bh->vtable      = vt;
    bh->compiled    = compiled;
    bh->sandboxed   = sandboxed;
    bh->allowed_caps = allowed_caps;
    return bh;
}

// ── Full compile pass ─────────────────────────────────────────
godot::Error PolyglotScript::compile_all(bool /*keep_state*/) {
    std::string src = source_.utf8().get_data();
    if (src.empty() && !res_path_.empty()) {
        // Load from file if source not set.
        auto fa = godot::FileAccess::open(res_path_.c_str(), godot::FileAccess::READ);
        if (!fa.is_valid()) {
            ERR_PRINT(godot::String("PolyLang/Poly: cannot open ") + res_path_.c_str());
            return godot::Error::ERR_FILE_NOT_FOUND;
        }
        src = fa->get_as_text().utf8().get_data();
    }

    PolyParseResult parsed = PolyglotParser::parse(src, res_path_);
    if (!parsed.ok) {
        ERR_PRINT(godot::String("PolyLang/Poly: parse error: ") + parsed.error.c_str());
        valid_ = false;
        return godot::Error::ERR_PARSE_ERROR;
    }

    header_ = parsed.header;
    // Merge same-language blocks before compiling.
    auto merged = PolyglotParser::merge_blocks(parsed.blocks);

    std::vector<std::unique_ptr<BlockHandle>> new_blocks;
    bool any_ok = false;
    for (const auto& block : merged) {
        auto bh = compile_block(block);
        if (bh) { any_ok = true; new_blocks.push_back(std::move(bh)); }
        else    { new_blocks.push_back(nullptr); }
    }

    {
        std::unique_lock lk(compile_mutex_);
        blocks_ = std::move(new_blocks);
        export_cache_dirty_ = true;
    }

    valid_ = any_ok;
    return any_ok ? godot::Error::OK : godot::Error::FAILED;
}

godot::Error PolyglotScript::_reload(bool keep_state) {
    return compile_all(keep_state);
}

void PolyglotScript::rebuild_export_cache() const {
    export_prop_cache_.clear();
    std::unordered_map<std::string, godot::Dictionary> seen;

    std::shared_lock lk(compile_mutex_);
    for (const auto& bh : blocks_) {
        if (!bh || !bh->vtable || !bh->compiled) continue;

        // Try vtable export vars first (adapters that implement PL_CAP_EXPORT_VARS).
        if ((bh->vtable->capabilities & PL_CAP_EXPORT_VARS) &&
            bh->vtable->pl_get_export_vars) {
            PLExportVarInfo* vars = nullptr;
            int32_t count = 0;
            bh->vtable->pl_get_export_vars(bh->compiled, &vars, &count);
            for (int32_t i = 0; i < count; ++i) {
                godot::Dictionary d;
                d["name"] = godot::String(vars[i].name ? vars[i].name : "");
                d["type"] = (int)vars[i].type_hint;
                d["usage"] = godot::PROPERTY_USAGE_DEFAULT;
                d["hint"] = godot::PROPERTY_HINT_NONE;
                d["hint_string"] = "";
                std::string n(vars[i].name ? vars[i].name : "");
                seen[n] = d;
            }
            if (vars && bh->vtable->pl_free_export_vars)
                bh->vtable->pl_free_export_vars(vars, count);
            continue;
        }

        // Fallback: use property list introspection + @export parser.
        // (For adapters without PL_CAP_EXPORT_VARS, we rely on source parsing.)
    }

    // Source-level @export parsing as fallback for all blocks.
    // This catches languages that don't expose pl_get_export_vars.
    lk.unlock(); // reacquire is OK — source_ is immutable after compile
    std::string src = const_cast<PolyglotScript*>(this)->source_.utf8().get_data();
    if (!src.empty()) {
        auto props = PLExportParser::parse(src);
        for (const auto& p : props) {
            std::string n = p.name;
            if (seen.count(n) == 0) {
                seen[n] = PLExportParser::to_property_dict(p);
            }
        }
    }

    for (auto& [name, dict] : seen)
        export_prop_cache_.push_back(dict);

    export_cache_dirty_ = false;
}

godot::TypedArray<godot::Dictionary> PolyglotScript::_get_script_property_list() const {
    if (export_cache_dirty_) rebuild_export_cache();
    godot::TypedArray<godot::Dictionary> arr;
    for (const auto& d : export_prop_cache_) arr.push_back(d);
    return arr;
}

godot::TypedArray<godot::Dictionary> PolyglotScript::_get_script_method_list() const {
    godot::TypedArray<godot::Dictionary> arr;
    std::shared_lock lk(compile_mutex_);
    for (const auto& bh : blocks_) {
        if (!bh || !bh->vtable || !bh->compiled) continue;
        if (!bh->vtable->pl_get_method_list) continue;
        PLMethodInfo* methods = nullptr;
        int32_t count = 0;
        bh->vtable->pl_get_method_list(bh->compiled, &methods, &count);
        for (int32_t i = 0; i < count; ++i) {
            godot::Dictionary d;
            d["name"] = godot::String(methods[i].name ? methods[i].name : "");
            d["args"] = godot::Array();
            d["return"] = godot::Dictionary();
            arr.push_back(d);
        }
        if (methods && bh->vtable->pl_free_method_list)
            bh->vtable->pl_free_method_list(methods);
    }
    return arr;
}

void* PolyglotScript::_instance_create(godot::Object* for_object) const {
    if (!valid_) return nullptr;
    auto* inst = new PolyglotInstance(const_cast<PolyglotScript*>(this), for_object);
    // Return as raw pointer for GDExtension; ownership transferred.
    return inst;
}

void* PolyglotScript::_placeholder_instance_create(godot::Object* for_object) const {
    return _instance_create(for_object);
}

// ── PolyglotInstance ──────────────────────────────────────────

PolyglotInstance::PolyglotInstance(PolyglotScript* script, godot::Object* owner)
    : script_(script), owner_(owner) {

    // Create one foreign instance per compiled block.
    std::shared_lock lk(script->compile_mutex());
    for (const auto& bh : script->blocks()) {
        auto bi = std::make_unique<BlockInstance>();
        bi->block = bh.get();
        if (bh && bh->vtable && bh->compiled && bh->vtable->pl_instantiate_class) {
            void* fi = bh->vtable->pl_instantiate_class(
                bh->compiled, bh->language.c_str());
            bi->foreign.store(fi, std::memory_order_release);
        }
        instances_.push_back(std::move(bi));
    }
}

PolyglotInstance::~PolyglotInstance() {
    std::unique_lock lk(inst_mutex_);
    for (auto& bi : instances_) {
        if (!bi) continue;
        void* fi = bi->foreign.exchange(nullptr, std::memory_order_acq_rel);
        if (fi && bi->block && bi->block->vtable && bi->block->vtable->pl_free_instance)
            bi->block->vtable->pl_free_instance(fi);
    }
}

int PolyglotInstance::call_all_blocks(const char* method,
                                       PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    int last_rc = PL_ERR_METHOD_NOT_FOUND;
    std::shared_lock lk(inst_mutex_);
    for (auto& bi : instances_) {
        if (!bi || !bi->block || !bi->block->vtable) continue;
        void* fi = bi->foreign.load(std::memory_order_acquire);
        if (!fi) continue;
        if (!bi->block->vtable->pl_call_method) continue;
        PLValue block_ret;
        pl_value_init(&block_ret);
        int rc = bi->block->vtable->pl_call_method(fi, method, args, argc, &block_ret);
        if (rc == PL_OK) {
            // FIX C-10: Always free ret's heap before overwriting.
            // Fall back to VariantBridge::free_pl_value when pl_free_value_contents
            // is null — otherwise string/array heap inside ret leaks.
            if (bi->block->vtable->pl_free_value_contents) {
                bi->block->vtable->pl_free_value_contents(ret);
            } else {
                VariantBridge::free_pl_value(*ret);
            }
            *ret  = block_ret;
            last_rc = PL_OK;
        } else if (rc != PL_ERR_METHOD_NOT_FOUND) {
            last_rc = rc;
        }
    }
    return last_rc;
}

int PolyglotInstance::call_first_block(const char* method,
                                        PLValue* args, int32_t argc, PLValue* ret) {
    pl_value_init(ret);
    std::shared_lock lk(inst_mutex_);
    for (auto& bi : instances_) {
        if (!bi || !bi->block || !bi->block->vtable) continue;
        void* fi = bi->foreign.load(std::memory_order_acquire);
        if (!fi) continue;
        if (!bi->block->vtable->pl_has_method ||
            !bi->block->vtable->pl_has_method(bi->block->compiled, method))
            continue;
        if (!bi->block->vtable->pl_call_method) continue;
        return bi->block->vtable->pl_call_method(fi, method, args, argc, ret);
    }
    return PL_ERR_METHOD_NOT_FOUND;
}

void PolyglotInstance::hot_swap(
        std::vector<std::unique_ptr<BlockHandle>>&& new_blocks) {
    std::unique_lock lk(inst_mutex_);

    // Save state per block before swapping.
    std::vector<StateSnapshot> snapshots(instances_.size());
    for (size_t i = 0; i < instances_.size(); ++i) {
        auto& bi = *instances_[i];
        void* fi = bi.foreign.load(std::memory_order_acquire);
        if (!fi || !bi.block) continue;
        snapshots[i] = StateTransfer::save(fi, bi.block->vtable, nullptr);
    }

    // Free old foreign instances.
    for (auto& bi : instances_) {
        if (!bi) continue;
        void* fi = bi->foreign.exchange(nullptr, std::memory_order_acq_rel);
        if (fi && bi->block && bi->block->vtable && bi->block->vtable->pl_free_instance)
            bi->block->vtable->pl_free_instance(fi);
    }

    // Install new blocks + create new instances.
    instances_.clear();
    instances_.reserve(new_blocks.size());
    for (size_t i = 0; i < new_blocks.size(); ++i) {
        auto bi = std::make_unique<BlockInstance>();
        bi->block = new_blocks[i].get();
        if (bi->block && bi->block->vtable && bi->block->compiled &&
            bi->block->vtable->pl_instantiate_class) {
            void* fi = bi->block->vtable->pl_instantiate_class(
                bi->block->compiled, bi->block->language.c_str());
            bi->foreign.store(fi, std::memory_order_release);
            // Restore state.
            if (fi && i < snapshots.size() && !snapshots[i].empty())
                StateTransfer::restore(snapshots[i], fi, bi->block->vtable, nullptr);
        }
        instances_.push_back(std::move(bi));
    }
    prop_cache_dirty_ = true;
}

void PolyglotInstance::rebuild_prop_cache() const {
    prop_info_cache_.clear();
    prop_name_storage_.clear();

    auto* s = script_;
    if (!s) return;
    auto list = s->_get_script_property_list();
    for (int i = 0; i < list.size(); ++i) {
        godot::Dictionary d = list[i];
        godot::String name = d.get("name", godot::String());
        prop_name_storage_.push_back(std::string(name.utf8().get_data()));

        GDExtensionPropertyInfo pi{};
        pi.type      = (GDExtensionVariantType)(int)d.get("type", 0);
        pi.name      = reinterpret_cast<GDExtensionStringNamePtr>(
                           (void*)prop_name_storage_.back().c_str());
        pi.class_name = nullptr;
        pi.hint      = (uint32_t)(int)d.get("hint", 0);
        pi.usage     = (uint32_t)(int)d.get("usage",
                           godot::PROPERTY_USAGE_DEFAULT);
        prop_info_cache_.push_back(pi);
    }
    prop_cache_dirty_ = false;
}

// ── GDExtension trampolines ───────────────────────────────────

GDExtensionBool PolyglotInstance::_set(
        GDExtensionScriptInstanceDataPtr p_self,
        GDExtensionConstStringNamePtr    p_name,
        GDExtensionConstVariantPtr       p_val) {
    auto* self = static_cast<PolyglotInstance*>(p_self);
    const godot::StringName& sn =
        *reinterpret_cast<const godot::StringName*>(p_name);
    std::string name = godot::String(sn).utf8().get_data();

    const godot::Variant& var =
        *reinterpret_cast<const godot::Variant*>(p_val);
    PLValue plv;
    VariantBridge::to_pl_value(var, plv);

    std::shared_lock lk(self->inst_mutex_);
    for (auto& bi : self->instances_) {
        if (!bi || !bi->block || !bi->block->vtable || !bi->block->vtable->pl_set_property)
            continue;
        void* fi = bi->foreign.load(std::memory_order_acquire);
        if (!fi) continue;
        int rc = bi->block->vtable->pl_set_property(fi, name.c_str(), &plv);
        if (rc == PL_OK) {
            VariantBridge::free_pl_value(plv);
            return true;
        }
    }
    VariantBridge::free_pl_value(plv);
    return false;
}

GDExtensionBool PolyglotInstance::_get(
        GDExtensionScriptInstanceDataPtr p_self,
        GDExtensionConstStringNamePtr    p_name,
        GDExtensionVariantPtr            p_ret) {
    auto* self = static_cast<PolyglotInstance*>(p_self);
    const godot::StringName& sn =
        *reinterpret_cast<const godot::StringName*>(p_name);
    std::string name = godot::String(sn).utf8().get_data();

    std::shared_lock lk(self->inst_mutex_);
    for (auto& bi : self->instances_) {
        if (!bi || !bi->block || !bi->block->vtable || !bi->block->vtable->pl_get_property)
            continue;
        void* fi = bi->foreign.load(std::memory_order_acquire);
        if (!fi) continue;
        PLValue out;
        pl_value_init(&out);
        int rc = bi->block->vtable->pl_get_property(fi, name.c_str(), &out);
        if (rc == PL_OK) {
            godot::Variant result = VariantBridge::from_pl_value(out);
            if (bi->block->vtable->pl_free_value_contents)
                bi->block->vtable->pl_free_value_contents(&out);
            *reinterpret_cast<godot::Variant*>(p_ret) = result;
            return true;
        }
    }
    return false;
}

void PolyglotInstance::_call(
        GDExtensionScriptInstanceDataPtr  p_self,
        GDExtensionConstStringNamePtr     p_method,
        const GDExtensionConstVariantPtr* p_args,
        GDExtensionInt                    p_argc,
        GDExtensionVariantPtr             p_ret,
        GDExtensionCallError*             p_err) {
    auto* self = static_cast<PolyglotInstance*>(p_self);
    const godot::StringName& sn =
        *reinterpret_cast<const godot::StringName*>(p_method);
    std::string name = godot::String(sn).utf8().get_data();

    // Marshal args.
    std::vector<PLValue> args(p_argc);
    for (GDExtensionInt i = 0; i < p_argc; ++i) {
        const godot::Variant& v =
            *reinterpret_cast<const godot::Variant*>(p_args[i]);
        VariantBridge::to_pl_value(v, args[i]);
    }

    PLValue ret;
    pl_value_init(&ret);

    int rc;
    if (is_fanout(name)) {
        // Fan-out to ALL blocks.
        rc = self->call_all_blocks(name.c_str(),
            args.data(), (int32_t)args.size(), &ret);
    } else {
        // First-win dispatch.
        rc = self->call_first_block(name.c_str(),
            args.data(), (int32_t)args.size(), &ret);
    }

    // Free args.
    for (auto& v : args) VariantBridge::free_pl_value(v);

    godot::Variant result;
    if (rc == PL_OK) {
        result = VariantBridge::from_pl_value(ret);
        // Free string/array contents inside ret.
        // We find the vtable from the winning block to call free_value_contents.
        // For simplicity use the first non-null vtable.
        std::shared_lock lk(self->inst_mutex_);
        for (auto& bi : self->instances_) {
            if (bi && bi->block && bi->block->vtable && bi->block->vtable->pl_free_value_contents) {
                bi->block->vtable->pl_free_value_contents(&ret);
                break;
            }
        }
    }

    *reinterpret_cast<godot::Variant*>(p_ret) = result;

    if (p_err) {
        p_err->error = (rc == PL_OK || rc == PL_ERR_METHOD_NOT_FOUND)
            ? GDEXTENSION_CALL_OK
            : GDEXTENSION_CALL_ERROR_INVALID_METHOD;
    }
}

GDExtensionBool PolyglotInstance::_has_method(
        GDExtensionScriptInstanceDataPtr p_self,
        GDExtensionConstStringNamePtr    p_method) {
    auto* self = static_cast<PolyglotInstance*>(p_self);
    const godot::StringName& sn =
        *reinterpret_cast<const godot::StringName*>(p_method);
    std::string name = godot::String(sn).utf8().get_data();

    std::shared_lock lk(self->inst_mutex_);
    for (const auto& bi : self->instances_) {
        if (!bi || !bi->block || !bi->block->vtable || !bi->block->compiled) continue;
        if (bi->block->vtable->pl_has_method &&
            bi->block->vtable->pl_has_method(bi->block->compiled, name.c_str()))
            return true;
    }
    return false;
}

GDExtensionObjectPtr PolyglotInstance::_get_owner(
        GDExtensionScriptInstanceDataPtr p_self) {
    auto* self = static_cast<PolyglotInstance*>(p_self);
    return self->owner_ ? self->owner_->_owner : nullptr;
}

void PolyglotInstance::_notification(
        GDExtensionScriptInstanceDataPtr p_self,
        int32_t                          p_what,
        GDExtensionBool                  p_reversed) {
    auto* self = static_cast<PolyglotInstance*>(p_self);
    PLValue args[1];
    pl_value_init(&args[0]);
    args[0].type = PL_TYPE_INT;
    args[0].i    = p_what;
    PLValue ret;
    pl_value_init(&ret);
    // Notification fans out to all blocks.
    self->call_all_blocks("_notification", args, 1, &ret);
}

const GDExtensionPropertyInfo* PolyglotInstance::_get_property_list(
        GDExtensionScriptInstanceDataPtr p_self, uint32_t* r_count) {
    auto* self = static_cast<PolyglotInstance*>(p_self);
    if (self->prop_cache_dirty_) self->rebuild_prop_cache();
    *r_count = (uint32_t)self->prop_info_cache_.size();
    return self->prop_info_cache_.data();
}

void PolyglotInstance::_free_property_list(
        GDExtensionScriptInstanceDataPtr,
        const GDExtensionPropertyInfo*, uint32_t) {
    // prop_info_cache_ is owned by the instance, not heap-allocated per-call.
}

void* PolyglotInstance::_create(void* p_userdata) {
    // Not used for PolyglotInstance — creation happens in _instance_create.
    return p_userdata;
}

void PolyglotInstance::_free(void* p_self, GDExtensionScriptInstanceDataPtr p_ri) {
    delete static_cast<PolyglotInstance*>(p_ri);
}

GDExtensionScriptInstanceInfo2 PolyglotInstance::make_info() {
    GDExtensionScriptInstanceInfo2 info{};
    info.set_func          = _set;
    info.get_func          = _get;
    info.call_func         = _call;
    info.has_method_func   = _has_method;
    info.get_owner_func    = _get_owner;
    info.notification_func = _notification;
    info.get_property_list_func  = _get_property_list;
    info.free_property_list_func = reinterpret_cast<decltype(info.free_property_list_func)>(_free_property_list);
    info.free_func         = reinterpret_cast<decltype(info.free_func)>(_free);
    return info;
}

// ── language_from_string helper ───────────────────────────────
// Maps canonical language name string → LanguageID.
// Must stay in sync with runtime_manager.hpp.
LanguageID language_from_string(const char* name) {
    if (!name) return LanguageID::COUNT;
    std::string s(name);
    if (s=="lua")         return LanguageID::Lua;
    if (s=="python")      return LanguageID::Python;
    if (s=="javascript")  return LanguageID::JavaScript;
    if (s=="typescript")  return LanguageID::TypeScript;
    if (s=="squirrel")    return LanguageID::Squirrel;
    if (s=="wren")        return LanguageID::Wren;
    if (s=="angelscript") return LanguageID::AngelScript;
    if (s=="julia")       return LanguageID::Julia;
    if (s=="kotlin")      return LanguageID::Kotlin;
    if (s=="go")          return LanguageID::Go;
    if (s=="swift")       return LanguageID::Swift;
    if (s=="haxe")        return LanguageID::Haxe;
    if (s=="csharp")      return LanguageID::CSharp;
    if (s=="nim")         return LanguageID::Nim;
    if (s=="rust")        return LanguageID::Rust;
    if (s=="zig")         return LanguageID::Zig;
    if (s=="odin")        return LanguageID::Odin;
    return LanguageID::COUNT;
}

} // namespace polylang
