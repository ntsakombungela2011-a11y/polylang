@tool
extends EditorPlugin
# =============================================================
# addons/polylang/plugin.gd  —  PolyLang v6.4 Editor Plugin
# =============================================================

const PROFILER_PANEL  = preload("res://addons/polylang/profiler_panel.gd")
const POLYGLOT_PANEL  = preload("res://addons/polylang/polyglot_editor.gd")
const MOD_LOADER_PANEL= preload("res://addons/polylang/mod_panel.gd")

var _profiler_panel : Control
var _polyglot_panel : Control
var _mod_panel      : Control

func _enter_tree() -> void:
	# ── Profiler dock ─────────────────────────────────────────
	_profiler_panel = PROFILER_PANEL.new()
	add_control_to_dock(DOCK_SLOT_RIGHT_BL, _profiler_panel)

	# ── Polyglot editor dock ──────────────────────────────────
	_polyglot_panel = POLYGLOT_PANEL.new()
	add_control_to_dock(DOCK_SLOT_LEFT_BR, _polyglot_panel)

	# ── Mod loader dock ───────────────────────────────────────
	_mod_panel = MOD_LOADER_PANEL.new()
	add_control_to_dock(DOCK_SLOT_LEFT_BR, _mod_panel)

	# PolyglotScript is a native GDExtension class registered from C++.
	# add_custom_type() is only for script-defined editor types.
	print("[PolyLang v6.7] Editor plugin loaded.")

func _exit_tree() -> void:
	if _profiler_panel:
		remove_control_from_docks(_profiler_panel)
		_profiler_panel.queue_free()
	if _polyglot_panel:
		remove_control_from_docks(_polyglot_panel)
		_polyglot_panel.queue_free()
	if _mod_panel:
		remove_control_from_docks(_mod_panel)
		_mod_panel.queue_free()

func _get_plugin_name() -> String:
	return "PolyLang"

func _has_main_screen() -> bool:
	return false
