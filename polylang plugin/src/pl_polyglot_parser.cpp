// =============================================================
// pl_polyglot_parser.cpp  —  PolyLang v6.6
//
// RETAINED FIXES from v6.5: M-2 (1 MB source size limit).
//
// ZERO-TRUST AUDIT ROUND 2 FIX:
//
// FIX VLN-07 [HIGH]: Unbounded block count — memory exhaustion.
//   BEFORE: A .poly file could contain thousands of tiny [lang][/lang]
//           block pairs all under the 1 MB total limit (e.g. 8,000 pairs
//           of 64-byte blocks = 512 KB source, 8,000 PolyBlock objects).
//           Each PolyBlock stores a std::string source; the result.blocks
//           vector is returned to callers that may further process it.
//           This can exhaust memory or produce pathologically large
//           intermediate data structures.
//   AFTER:  PL_POLY_MAX_BLOCKS (256) hard cap. When exceeded, parse()
//           returns an error immediately.
// =============================================================
#include "pl_polyglot_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace polylang {

static constexpr size_t PL_POLY_MAX_SOURCE_BYTES = 1u * 1024u * 1024u; // 1 MiB
static constexpr size_t PL_POLY_MAX_BLOCKS       = 256;                 // FIX VLN-07

static const std::unordered_set<std::string> VALID_LANG_TAGS = {
    "lua", "python", "js", "javascript", "ts", "typescript",
    "rust", "zig", "go", "swift", "kotlin", "nim", "odin",
    "haxe", "csharp", "cs", "squirrel", "wren", "angelscript", "as",
    "julia"
};

static std::string canonical_lang(const std::string& tag) {
    if (tag == "js")  return "javascript";
    if (tag == "ts")  return "typescript";
    if (tag == "cs")  return "csharp";
    if (tag == "as")  return "angelscript";
    return tag;
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

bool PolyglotParser::is_valid_language_tag(const std::string& tag) {
    return VALID_LANG_TAGS.count(to_lower(tag)) > 0;
}

bool PolyglotParser::is_polyglot_path(const std::string& path) {
    if (path.size() >= 5 && path.substr(path.size()-5) == ".poly") return true;
    if (path.size() >= 9 && path.substr(path.size()-9) == ".pl.poly") return true;
    return false;
}

static bool parse_header_line(const std::string& line,
                               std::string& key, std::string& val) {
    std::string t = trim(line);
    if (t.empty()) return false;
    size_t start = 0;
    if (t[0] == '#') start = 1;
    else if (t.size() >= 2 && t[0] == '/' && t[1] == '/') start = 2;
    else return false;
    t = trim(t.substr(start));
    size_t colon = t.find(':');
    if (colon == std::string::npos) return false;
    key = trim(to_lower(t.substr(0, colon)));
    val = trim(t.substr(colon + 1));
    return !key.empty();
}

PolyParseResult PolyglotParser::parse(const std::string& source,
                                       const std::string& res_path) {
    PolyParseResult result;
    result.ok = false;

    // Source size limit (v6.5).
    if (source.size() > PL_POLY_MAX_SOURCE_BYTES) {
        result.error = res_path + ": source too large ("
            + std::to_string(source.size()) + " bytes; max "
            + std::to_string(PL_POLY_MAX_SOURCE_BYTES) + ")";
        return result;
    }

    std::istringstream ss(source);
    std::string line;
    int line_no = 0;

    enum State { HEADER, OUTSIDE, INSIDE_BLOCK };
    State state = HEADER;

    std::string current_lang;
    std::ostringstream current_src;
    int block_start_line = 0;
    bool header_done = false;

    while (std::getline(ss, line)) {
        ++line_no;
        std::string trimmed = trim(line);

        if (trimmed.size() >= 3 &&
            trimmed.front() == '[' && trimmed.back() == ']') {

            std::string tag = trimmed.substr(1, trimmed.size() - 2);

            if (!tag.empty() && tag[0] == '/') {
                std::string close_lang = canonical_lang(to_lower(tag.substr(1)));
                if (state != INSIDE_BLOCK) {
                    result.error = res_path + ":" + std::to_string(line_no)
                        + ": unexpected closing tag [/" + tag.substr(1) + "]";
                    return result;
                }
                if (close_lang != current_lang) {
                    result.error = res_path + ":" + std::to_string(line_no)
                        + ": mismatched closing tag [/" + tag.substr(1)
                        + "] (expected [/" + current_lang + "])";
                    return result;
                }
                // FIX VLN-07: enforce block count limit.
                if (result.blocks.size() >= PL_POLY_MAX_BLOCKS) {
                    result.error = res_path + ": too many language blocks (max "
                        + std::to_string(PL_POLY_MAX_BLOCKS) + ")";
                    return result;
                }
                PolyBlock block;
                block.language   = current_lang;
                block.source     = current_src.str();
                block.start_line = block_start_line;
                result.blocks.push_back(std::move(block));
                current_src.str(""); current_src.clear();
                current_lang.clear();
                state = OUTSIDE;
                continue;
            }

            std::string lang = canonical_lang(to_lower(tag));
            if (!is_valid_language_tag(lang)) {
                result.error = res_path + ":" + std::to_string(line_no)
                    + ": unknown language tag [" + tag + "]";
                return result;
            }
            if (state == INSIDE_BLOCK) {
                result.error = res_path + ":" + std::to_string(line_no)
                    + ": nested language block [" + tag
                    + "] inside [" + current_lang + "]";
                return result;
            }
            header_done    = true;
            state          = INSIDE_BLOCK;
            current_lang   = lang;
            block_start_line = line_no + 1;
            current_src.str(""); current_src.clear();
            continue;
        }

        if (state == INSIDE_BLOCK) {
            current_src << line << "\n";
            continue;
        }

        if (!header_done && !trimmed.empty()) {
            std::string key, val;
            if (parse_header_line(line, key, val)) {
                if      (key == "base_class")   result.header.base_class  = val;
                else if (key == "author")        result.header.author      = val;
                else if (key == "version")       result.header.version     = val;
                else if (key == "description")   result.header.description = val;
                else                             result.header.extra[key]  = val;
            }
        }
    }

    if (state == INSIDE_BLOCK) {
        result.error = res_path + ": unterminated language block [" + current_lang + "]";
        return result;
    }

    if (result.blocks.empty()) {
        result.error = res_path + ": no language blocks found";
        return result;
    }

    result.ok = true;
    return result;
}

std::vector<PolyBlock> PolyglotParser::merge_blocks(
        const std::vector<PolyBlock>& blocks) {
    std::vector<PolyBlock>                   merged;
    std::unordered_map<std::string, size_t>  index;

    for (const auto& b : blocks) {
        auto it = index.find(b.language);
        if (it == index.end()) {
            index[b.language] = merged.size();
            merged.push_back(b);
        } else {
            merged[it->second].source += "\n" + b.source;
        }
    }
    return merged;
}

} // namespace polylang
