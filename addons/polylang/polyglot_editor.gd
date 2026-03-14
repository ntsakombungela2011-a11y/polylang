@tool
extends VBoxContainer
# =============================================================
# addons/polylang/polyglot_editor.gd  —  PolyLang v6.4
# =============================================================
# Dock that shows the block structure of the currently open .poly
# file: language, line count, detected methods, export vars.
# Provides "Add Block" UI to scaffold new language sections.
# =============================================================

var _label        : Label
var _block_list   : ItemList
var _add_btn      : Button
var _lang_option  : OptionButton
var _info_label   : Label

const SUPPORTED_LANGS := [
	"lua", "python", "javascript", "typescript",
	"rust", "zig", "go", "swift", "kotlin", "nim",
	"odin", "haxe", "csharp", "squirrel", "wren", "angelscript"
]

func _ready() -> void:
	name = "Polyglot"
	custom_minimum_size = Vector2(280, 200)

	_label = Label.new()
	_label.text = "No .poly file open"
	add_child(_label)

	_block_list = ItemList.new()
	_block_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	add_child(_block_list)

	var add_row := HBoxContainer.new()
	add_child(add_row)

	_lang_option = OptionButton.new()
	for lang in SUPPORTED_LANGS:
		_lang_option.add_item(lang)
	add_row.add_child(_lang_option)

	_add_btn = Button.new()
	_add_btn.text = "Add Block"
	_add_btn.pressed.connect(_on_add_block)
	add_row.add_child(_add_btn)

	_info_label = Label.new()
	_info_label.text = ""
	_info_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	add_child(_info_label)

	# Connect to editor script change.
	var ei := EditorInterface.get_singleton()
	if ei:
		ei.get_script_editor().editor_script_changed.connect(_on_script_changed)

func _on_script_changed(script) -> void:
	_block_list.clear()
	if not script:
		_label.text = "No script open"
		return
	var path : String = script.resource_path
	if not (path.ends_with(".poly") or path.ends_with(".pl.poly")):
		_label.text = "Open a .poly file to see blocks"
		return

	_label.text = path.get_file()
	var src : String = script.source_code
	_parse_and_display(src)

func _parse_and_display(src: String) -> void:
	_block_list.clear()
	var current_lang := ""
	var line_count := 0
	var block_lines := 0

	for line in src.split("\n"):
		line_count += 1
		var t := line.strip_edges()
		if t.begins_with("[") and t.ends_with("]") and not t.begins_with("[/"):
			if current_lang != "":
				_block_list.add_item("[%s]  %d lines" % [current_lang, block_lines])
			current_lang = t.substr(1, t.length()-2)
			block_lines = 0
		elif t.begins_with("[/") and t.ends_with("]"):
			if current_lang != "":
				_block_list.add_item("[%s]  %d lines" % [current_lang, block_lines])
			current_lang = ""
			block_lines = 0
		elif current_lang != "":
			block_lines += 1

	_info_label.text = "Total lines: %d  |  Blocks: %d" % [
		line_count, _block_list.item_count]

func _on_add_block() -> void:
	var lang : String = SUPPORTED_LANGS[_lang_option.selected]
	var script_editor := EditorInterface.get_singleton().get_script_editor()
	var script = script_editor.get_current_script()
	if not script:
		_info_label.text = "No script open"
		return
	var snippet := "\n[%s]\n# Write %s code here\n[/%s]\n" % [lang, lang, lang]
	# Append snippet to source (editor will detect the change).
	script.source_code += snippet
	ResourceSaver.save(script)
	_on_script_changed(script)
	_info_label.text = "Added [%s] block." % lang
