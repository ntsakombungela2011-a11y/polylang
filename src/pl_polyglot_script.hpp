// =============================================================
// pl_polyglot_script.hpp  —  PolyLang v6.5 Polyglot Script
// =============================================================
// PolyglotScript is a Godot ScriptExtension that parses .poly files
// and dispatches method calls, property access, and lifecycle hooks
// across multiple language adapters within a single script resource.
//
// ARCHITECTURE:
//   One PolyglotScript → N BlockHandle (one per language block).
//   One PolyglotInstance → N foreign instances (one per BlockHandle).
//
//   METHOD RESOLUTION ORDER (MRO):
//     On each method/property call, blocks are scanned in file order.
//     The first block whose adapter reports has_method() = true wins.
//     Built-in lifecycle methods (_ready, _process, etc.) call ALL
//     blocks that implement them, in file order. This allows Lua to
//     handle _ready while Rust handles _physics_process.
//
//   PROPERTY RESOLUTION:
//     set/get_property: first block that has the property wins.
//     @export vars from all blocks are aggregated and deduplicated
//     (last-writer wins on name collision).
//
//   CROSS-BLOCK STATE:
//     Blocks share the same Godot Object owner. They communicate via
//     PLSignalBus and PolyLangBridge (cross-language call system).
//     They do NOT share raw memory — each block has its own foreign heap.
//
//   HOT RELOAD:
//     When the .poly file changes, ALL blocks are recompiled and all
//     foreign instances hot-swapped. State is preserved per-block via
//     StateTransfer before the swap and restored after.
//
//   SANDBOX:
//     Each block's sandbox tier is resolved independently.
//     A quarantined block cannot call trusted-block methods directly —
//     the bridge enforces tier checks at the PLSignalBus boundary.
// =============================================================
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "../include/pl_adapter_vtable.h"
#include "runtime_manager.hpp"
#include "pl_polyglot_parser.hpp"

namespace polylang {

class PolyglotInstance;

// ── One compiled language block ───────────────────────────────
struct BlockHandle {
    std::string      language;          // canonical name e.g. "lua"
    LanguageID       lang_id{LanguageID::COUNT};
    PLAdapterVTable* vtable{nullptr};   // non-owning
    void*            compiled{nullptr}; // owned; freed via pl_free_compiled
    bool             sandboxed{false};
    uint32_t         allowed_caps{PL_SANDBOX_NONE};

    ~BlockHandle();
    BlockHandle() = default;
    BlockHandle(const BlockHandle&) = delete;
    BlockHandle& operator=(const BlockHandle&) = delete;
    BlockHandle(BlockHandle&&) noexcept;
};

// ── The polyglot script resource ─────────────────────────────
class PolyglotScript : public godot::ScriptExtension {
    GDCLASS(PolyglotScript, godot::ScriptExtension)

public:
    PolyglotScript();
    ~PolyglotScript() override;

    // ── ScriptExtension overrides ─────────────────────────────
    bool      _can_instantiate() const override;
    bool      _is_valid() const override;
    godot::String _get_source_code() const override;
    void      _set_source_code(const godot::String& code) override;
    godot::Error _reload(bool keep_state) override;
    godot::TypedArray<godot::Dictionary> _get_script_property_list() const override;
    godot::TypedArray<godot::Dictionary> _get_script_method_list() const override;
    bool      _has_method(const godot::StringName& method) const override;
    godot::StringName _get_instance_base_type() const override;
    godot::Ref<godot::Script> _get_base_script() const override;
    godot::StringName _get_global_name() const override;
    bool      _inherits_script(const godot::Ref<godot::Script>& script) const override;
    void*     _instance_create(godot::Object* for_object) const override;
    void*     _placeholder_instance_create(godot::Object* for_object) const override;

    // ── Polyglot-specific ─────────────────────────────────────

    // Number of language blocks in this script.
    int block_count() const;

    // Language of block at index (in file order).
    std::string block_language(int index) const;

    // List all languages present in this script.
    std::vector<std::string> languages() const;

    // Find the first block that has a given method. Returns -1 if none.
    int block_index_for_method(const std::string& method_name) const;

    // Returns true if this is the source path for a .poly file.
    bool is_polyglot() const { return is_polyglot_; }

    // Parsed header from the .poly file.
    const PolyHeader& poly_header() const { return header_; }

    // Get all BlockHandles (read-only; guarded by compile_mutex_).
    const std::vector<std::unique_ptr<BlockHandle>>& blocks() const { return blocks_; }

    // Access compile mutex for locked iteration from PolyglotInstance.
    std::shared_mutex& compile_mutex() { return compile_mutex_; }

protected:
    static void _bind_methods();

private:
    // Load and parse source, compile all blocks.
    godot::Error compile_all(bool keep_state);

    // Compile a single block. Returns nullptr on failure.
    std::unique_ptr<BlockHandle> compile_block(const PolyBlock& block);

    godot::String source_;
    std::string   res_path_;
    bool          is_polyglot_{false};
    bool          valid_{false};
    PolyHeader    header_;

    mutable std::shared_mutex                     compile_mutex_;
    std::vector<std::unique_ptr<BlockHandle>>     blocks_;

    // Aggregated export variables from all blocks (in block order,
    // deduplicated by name — last block with a given name wins).
    mutable std::vector<godot::Dictionary>        export_prop_cache_;
    mutable bool                                  export_cache_dirty_{true};

    void rebuild_export_cache() const;
};

// ── One foreign instance per block ───────────────────────────
struct BlockInstance {
    BlockHandle*        block{nullptr};   // non-owning borrow
    std::atomic<void*>  foreign{nullptr}; // owned
};

// ── The polyglot script instance ─────────────────────────────
class PolyglotInstance {
public:
    explicit PolyglotInstance(PolyglotScript* script, godot::Object* owner);
    ~PolyglotInstance();

    // ── GDExtension trampolines ───────────────────────────────
    static GDExtensionScriptInstanceInfo2  make_info();

    static void*          _create(void* p_userdata);
    static void           _free(void* p_self, GDExtensionScriptInstanceDataPtr);
    static GDExtensionBool _set(GDExtensionScriptInstanceDataPtr,
                                GDExtensionConstStringNamePtr,
                                GDExtensionConstVariantPtr);
    static GDExtensionBool _get(GDExtensionScriptInstanceDataPtr,
                                GDExtensionConstStringNamePtr,
                                GDExtensionVariantPtr);
    static void            _call(GDExtensionScriptInstanceDataPtr,
                                 GDExtensionConstStringNamePtr,
                                 const GDExtensionConstVariantPtr*,
                                 GDExtensionInt,
                                 GDExtensionVariantPtr,
                                 GDExtensionCallError*);
    static GDExtensionBool _has_method(GDExtensionScriptInstanceDataPtr,
                                       GDExtensionConstStringNamePtr);
    static GDExtensionObjectPtr _get_owner(GDExtensionScriptInstanceDataPtr);
    static void _notification(GDExtensionScriptInstanceDataPtr, int32_t, GDExtensionBool);
    static const GDExtensionPropertyInfo* _get_property_list(
        GDExtensionScriptInstanceDataPtr, uint32_t* r_count);
    static void _free_property_list(GDExtensionScriptInstanceDataPtr,
                                    const GDExtensionPropertyInfo*,
                                    uint32_t);

    // Hot-swap all blocks atomically with state preservation.
    void hot_swap(std::vector<std::unique_ptr<BlockHandle>>&& new_blocks);

    godot::Object* owner() const { return owner_; }
    PolyglotScript* script() const { return script_; }

    // Built-in lifecycle methods that fan-out to ALL implementing blocks.
    static const std::vector<std::string>& fanout_methods();

private:
    // Dispatch a method call to ALL blocks that implement it.
    // Returns the last non-nil result.
    int call_all_blocks(const char* method,
                        PLValue* args, int32_t argc, PLValue* ret);

    // Dispatch to the FIRST block that has the method.
    int call_first_block(const char* method,
                         PLValue* args, int32_t argc, PLValue* ret);

    PolyglotScript*              script_;
    godot::Object*               owner_;
    std::vector<BlockInstance>   instances_;
    mutable std::shared_mutex    inst_mutex_;

    // Cached GDExtension property list (built from aggregated @export vars).
    mutable std::vector<GDExtensionPropertyInfo> prop_info_cache_;
    mutable std::vector<std::string>              prop_name_storage_;
    mutable bool                                  prop_cache_dirty_{true};

    void rebuild_prop_cache() const;


};

} // namespace polylang
