@tool
extends VBoxContainer
# =============================================================
# addons/polylang/profiler_panel.gd  —  PolyLang v6.4
# =============================================================
# Displays per-scope profiling data from PLProfiler singleton.
# Refreshes every 0.5s while the editor is running.
# =============================================================

var _tree    : Tree
var _timer   : Timer
var _enabled_check : CheckButton
var _reset_btn : Button

func _ready() -> void:
	name = "PL Profiler"
	custom_minimum_size = Vector2(300, 180)

	# Toolbar
	var toolbar := HBoxContainer.new()
	add_child(toolbar)

	_enabled_check = CheckButton.new()
	_enabled_check.text = "Enable"
	_enabled_check.button_pressed = true
	_enabled_check.toggled.connect(_on_enable_toggled)
	toolbar.add_child(_enabled_check)

	_reset_btn = Button.new()
	_reset_btn.text = "Reset"
	_reset_btn.pressed.connect(_on_reset)
	toolbar.add_child(_reset_btn)

	# Data tree
	_tree = Tree.new()
	_tree.columns = 5
	_tree.set_column_title(0, "Scope")
	_tree.set_column_title(1, "Calls")
	_tree.set_column_title(2, "Total µs")
	_tree.set_column_title(3, "Avg µs")
	_tree.set_column_title(4, "Max µs")
	_tree.column_titles_visible = true
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_tree)

	# Refresh timer
	_timer = Timer.new()
	_timer.wait_time = 0.5
	_timer.autostart = true
	_timer.timeout.connect(_refresh)
	add_child(_timer)

func _refresh() -> void:
	if not Engine.has_singleton("PLProfiler"):
		return
	var profiler = Engine.get_singleton("PLProfiler")
	var stats : Dictionary = profiler.get_scope_stats()

	_tree.clear()
	var root := _tree.create_item()
	_tree.hide_root = true

	for scope_name in stats:
		var s : Dictionary = stats[scope_name]
		var item := _tree.create_item(root)
		item.set_text(0, scope_name)
		item.set_text(1, str(s.get("calls", 0)))
		item.set_text(2, str(s.get("total_usec", 0)))
		item.set_text(3, str(s.get("avg_usec", 0)))
		item.set_text(4, str(s.get("max_usec", 0)))

func _on_enable_toggled(pressed: bool) -> void:
	if Engine.has_singleton("PLProfiler"):
		Engine.get_singleton("PLProfiler").set_enabled(pressed)

func _on_reset() -> void:
	if Engine.has_singleton("PLProfiler"):
		Engine.get_singleton("PLProfiler").reset()
	_tree.clear()
