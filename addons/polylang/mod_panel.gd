@tool
extends VBoxContainer
# =============================================================
# addons/polylang/mod_panel.gd  —  PolyLang v6.4
# =============================================================
# Editor dock for mod management: browse directories, load mods,
# show dependency order, sandbox tier, and load status.
# =============================================================

var _dir_label  : Label
var _mod_tree   : Tree
var _load_btn   : Button
var _dir_btn    : Button
var _status_label : Label
var _file_dialog : EditorFileDialog

func _ready() -> void:
	name = "PL Mods"
	custom_minimum_size = Vector2(300, 200)

	var toolbar := HBoxContainer.new()
	add_child(toolbar)

	_dir_btn = Button.new()
	_dir_btn.text = "Browse..."
	_dir_btn.pressed.connect(_on_browse)
	toolbar.add_child(_dir_btn)

	_load_btn = Button.new()
	_load_btn.text = "Load Mods"
	_load_btn.pressed.connect(_on_load_mods)
	toolbar.add_child(_load_btn)

	_dir_label = Label.new()
	_dir_label.text = "No directory selected"
	_dir_label.clip_text = true
	add_child(_dir_label)

	_mod_tree = Tree.new()
	_mod_tree.columns = 4
	_mod_tree.set_column_title(0, "Mod")
	_mod_tree.set_column_title(1, "Version")
	_mod_tree.set_column_title(2, "Tier")
	_mod_tree.set_column_title(3, "Status")
	_mod_tree.column_titles_visible = true
	_mod_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_mod_tree)

	_status_label = Label.new()
	_status_label.text = ""
	add_child(_status_label)

	_file_dialog = EditorFileDialog.new()
	_file_dialog.file_mode = EditorFileDialog.FILE_MODE_OPEN_DIR
	_file_dialog.dir_selected.connect(_on_dir_selected)
	add_child(_file_dialog)

var _selected_dir := ""

func _on_browse() -> void:
	_file_dialog.popup_centered(Vector2i(700, 500))

func _on_dir_selected(dir: String) -> void:
	_selected_dir = dir
	_dir_label.text = dir

func _on_load_mods() -> void:
	if _selected_dir.is_empty():
		_status_label.text = "Select a directory first."
		return
	_mod_tree.clear()
	var root := _mod_tree.create_item()
	_mod_tree.hide_root = true

	# Call into C++ ModLoader (exposed via PolyLangBridge or direct autoload).
	# For now we scan for .polylang_config files ourselves and display them.
	var dir := DirAccess.open(_selected_dir)
	if not dir:
		_status_label.text = "Cannot open: " + _selected_dir
		return

	var found := 0
	dir.list_dir_begin()
	var fname := dir.get_next()
	while fname != "":
		if fname.ends_with(".polylang_config"):
			_display_config(_selected_dir.path_join(fname), root)
			found += 1
		fname = dir.get_next()
	dir.list_dir_end()

	_status_label.text = "Found %d mod(s) in %s" % [found, _selected_dir]

func _display_config(path: String, root: TreeItem) -> void:
	var text := FileAccess.get_file_as_string(path)
	var entry := JSON.parse_string(text)
	if not entry is Dictionary:
		return
	var item := _mod_tree.create_item(root)
	item.set_text(0, path.get_file().replace(".polylang_config", ""))
	item.set_text(1, str(entry.get("version", "?")))
	item.set_text(2, str(entry.get("tier", "isolated")))
	item.set_text(3, "Ready")
