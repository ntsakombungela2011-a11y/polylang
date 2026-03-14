#pragma once
// =============================================================
// polylang_language.hpp  —  Godot ScriptLanguageExtension host
// =============================================================
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "runtime_manager.hpp"

namespace polylang {

class PolyLangLanguage final : public godot::ScriptLanguageExtension {
    GDCLASS(PolyLangLanguage, godot::ScriptLanguageExtension)
public:
    PolyLangLanguage();
    ~PolyLangLanguage() override;

    static PolyLangLanguage* get_singleton();

    // ScriptLanguageExtension overrides
    godot::String _get_name()      const override { return "PolyLang"; }
    godot::String _get_type()      const override { return "PolyLangScript"; }
    godot::String _get_extension() const override { return "plscript"; }

    void _init()   override;
    void _finish() override;

    // Called every frame from Engine's main loop — drains reload queue.
    void _frame() override;

    godot::PackedStringArray _get_recognized_extensions() const override;
    godot::PackedStringArray _get_comment_delimiters()    const override;
    godot::PackedStringArray _get_string_delimiters()     const override;

    bool _can_make_function()      const override { return false; }
    bool _supports_documentation() const override { return false; }
    bool _supports_builtin_mode()  const override { return false; }

    godot::Ref<godot::Script> _make_template(
        const godot::String& p_template,
        const godot::String& p_class_name,
        const godot::String& p_base_class_name) const override;

    godot::TypedArray<godot::Dictionary> _get_built_in_templates(
        const godot::StringName& p_object) const override;

    godot::Dictionary _validate(
        const godot::String& p_script,
        const godot::String& p_path,
        bool, bool, bool, bool) const override;

    static void _bind_methods();

private:
    // Determine which language's comment/string delimiters to use
    // based on the file currently open in the editor.
    LanguageID active_editor_language() const;

    static PolyLangLanguage* singleton_;
};

} // namespace polylang
