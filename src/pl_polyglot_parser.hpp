// =============================================================
// pl_polyglot_parser.hpp  —  PolyLang v6.5 Polyglot Script Parser
// =============================================================
// Parses .poly files — the world-first multi-language script format.
//
// FILE FORMAT:
//   # Optional header comments
//   # base_class: Node3D          (optional, defaults to RefCounted)
//   # author: ...
//
//   [lua]
//   -- @export var speed: float = 5.0
//   function _ready() print("hello from Lua") end
//   [/lua]
//
//   [python]
//   # @export var health: int = 100
//   def _process(self, delta): pass
//   [/python]
//
//   [rust]
//   fn _physics_process(delta: f64) { /* heavy math */ }
//   [/rust]
//
//   [odin]
//   import pl "polylang"
//   // @export var name: String = "Gol'D Roger"
//   _enter_tree :: proc() { }
//   [/odin]
//
// RULES:
//   • Any supported language tag is valid: lua, python, js, ts, rust, zig,
//     go, swift, kotlin, nim, odin, haxe, csharp, squirrel, wren, angelscript
//   • The same language may appear multiple times — blocks are concatenated.
//   • @export vars are parsed from all blocks and aggregated.
//   • Method resolution order: blocks in file order, first hit wins.
//   • Each block gets its own compiled handle via its adapter.
//   • Header directives are # key: value lines at the top of the file
//     before any language block.
// =============================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace polylang {

// One language block extracted from a .poly file.
struct PolyBlock {
    std::string language;     // e.g. "lua", "python", "odin"
    std::string source;       // raw source text of this block
    int         start_line{0}; // for error reporting
};

// Parsed header metadata from the .poly file.
struct PolyHeader {
    std::string base_class{"RefCounted"};
    std::string author;
    std::string version{"1"};
    std::string description;
    // Extra directives stored as key → value.
    std::unordered_map<std::string, std::string> extra;
};

// Full result of parsing one .poly file.
struct PolyParseResult {
    bool              ok{false};
    std::string       error;       // set if ok == false
    PolyHeader        header;
    std::vector<PolyBlock> blocks; // in file order
};

class PolyglotParser {
public:
    // Parse a .poly file from source text.
    // res_path is used for error messages only.
    static PolyParseResult parse(const std::string& source,
                                  const std::string& res_path = "");

    // Returns true if the file extension is a polyglot script.
    // Recognised: .poly, .pl.poly
    static bool is_polyglot_path(const std::string& path);

    // Collapse same-language blocks by concatenating their source.
    // Result has at most one block per language, in first-appearance order.
    static std::vector<PolyBlock> merge_blocks(const std::vector<PolyBlock>& blocks);

    // List of valid language tags (lowercase).
    static bool is_valid_language_tag(const std::string& tag);
};

} // namespace polylang
