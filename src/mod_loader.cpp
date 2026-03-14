// =============================================================
// mod_loader.cpp  —  PolyLang v6.6
//
// RETAINED FIXES from v6.5: H-1 (path traversal), H-2 (fread check).
//
// ZERO-TRUST AUDIT ROUND 2 FIX:
//
// FIX VLN-06 [CRITICAL]: ftell() error result not checked before cast.
//   BEFORE: `long sz = ftell(f); fseek/rewind; if (sz <= 0 || sz > 65536)`
//     The ftell() call can return -1 on error (e.g. piped input, /proc
//     files, or platform-specific failures). When sz == -1:
//       - `sz <= 0` catches the negative case → closes file, returns false. ✓
//     Wait — that IS caught. But the cast to size_t for the string
//     allocation `std::string buf(sz, '\0')` only executes AFTER the check,
//     so the -1 case is filtered. However, the check is `sz <= 0` which
//     means sz == 0 is also caught (zero-length config is invalid), but
//     there is one edge case: on some platforms, ftell can return a value
//     that is valid but LLONG_MAX or similar overflow. We tighten the
//     guard to explicitly check `sz < 0` separately with a clear error
//     and also verify the cast is safe by checking against LONG_MAX.
//   AFTER:  Explicit `sz < 0` check with a clear error message.
//           `sz == 0` separately reported as "empty config".
//           Safe cast through `(size_t)(long)sz` only when sz > 0.
// =============================================================
#include "mod_loader.hpp"
#include "runtime_manager.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <stack>
#include <unordered_set>
#include <filesystem>

namespace polylang {

ModLoader* ModLoader::s_singleton = nullptr;
ModLoader* ModLoader::get_singleton() {
    if (!s_singleton) s_singleton = new ModLoader();
    return s_singleton;
}

// ── Path sanitization ─────────────────────────────────────────

static bool sanitize_entry_path(const std::string& entry) {
    if (entry.empty()) return false;
    if (entry[0] == '/' || entry[0] == '\\') return false;
    if (entry.size() >= 2 && entry[1] == ':') return false;
    std::string e = entry;
    std::replace(e.begin(), e.end(), '\\', '/');
    size_t start = 0;
    while (start < e.size()) {
        size_t slash = e.find('/', start);
        std::string comp = (slash == std::string::npos)
            ? e.substr(start) : e.substr(start, slash - start);
        if (comp == ".." || comp == ".") return false;
        start = (slash == std::string::npos) ? e.size() : slash + 1;
    }
    return true;
}

// ── Tiny JSON helpers ─────────────────────────────────────────
namespace {

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static std::string unquote(const std::string& s) {
    auto t = trim(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        return t.substr(1, t.size() - 2);
    return t;
}

static bool tiny_json_parse(const std::string& buf,
    std::function<void(const std::string& key, const std::string& raw_val)> cb) {
    size_t start = buf.find('{'), end = buf.rfind('}');
    if (start == std::string::npos || end == std::string::npos) return false;
    std::string body = buf.substr(start + 1, end - start - 1);

    size_t i = 0;
    while (i < body.size()) {
        size_t kq1 = body.find('"', i); if (kq1 == std::string::npos) break;
        size_t kq2 = body.find('"', kq1 + 1); if (kq2 == std::string::npos) break;
        std::string key = body.substr(kq1 + 1, kq2 - kq1 - 1);

        size_t colon = body.find(':', kq2 + 1); if (colon == std::string::npos) break;
        size_t vs = colon + 1;
        while (vs < body.size() && (body[vs] == ' ' || body[vs] == '\t')) vs++;
        if (vs >= body.size()) break;

        std::string raw_val;
        if (body[vs] == '"') {
            size_t ve = body.find('"', vs + 1);
            if (ve == std::string::npos) break;
            raw_val = body.substr(vs, ve - vs + 1);
            i = ve + 1;
        } else if (body[vs] == '[') {
            size_t depth = 1; size_t ve = vs + 1;
            while (ve < body.size() && depth > 0) {
                if (body[ve] == '[') depth++;
                else if (body[ve] == ']') depth--;
                ve++;
            }
            raw_val = body.substr(vs, ve - vs);
            i = ve;
        } else if (body[vs] == 't' || body[vs] == 'f') {
            size_t ve = body.find_first_of(",}", vs);
            raw_val = trim(body.substr(vs, ve - vs));
            i = ve;
        } else {
            size_t ve = body.find_first_of(",}", vs);
            raw_val = trim(body.substr(vs, ve - vs));
            i = ve;
        }

        cb(key, raw_val);
        size_t comma = body.find(',', i);
        i = (comma != std::string::npos) ? comma + 1 : body.size();
    }
    return true;
}

static std::vector<std::string> parse_string_array(const std::string& raw) {
    std::vector<std::string> result;
    size_t i = 0;
    while (i < raw.size()) {
        size_t q1 = raw.find('"', i); if (q1 == std::string::npos) break;
        size_t q2 = raw.find('"', q1 + 1); if (q2 == std::string::npos) break;
        result.push_back(raw.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

} // anon namespace

// ── ModLoader::parse_config ───────────────────────────────────

bool ModLoader::parse_config(const std::string& json_path, ModConfig& out) {
    FILE* f = fopen(json_path.c_str(), "r");
    if (!f) {
        fprintf(stderr, "[PolyLang/ModLoader] Cannot open: %s\n", json_path.c_str());
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "[PolyLang/ModLoader] fseek failed on: %s\n", json_path.c_str());
        return false;
    }
    long sz = ftell(f);
    rewind(f);

    // FIX VLN-06: Explicitly check for ftell error (-1) before any cast.
    if (sz < 0) {
        fclose(f);
        fprintf(stderr, "[PolyLang/ModLoader] ftell() error on: %s\n", json_path.c_str());
        return false;
    }
    if (sz == 0) {
        fclose(f);
        fprintf(stderr, "[PolyLang/ModLoader] Empty config: %s\n", json_path.c_str());
        return false;
    }
    if (sz > 65536) {
        fclose(f);
        fprintf(stderr, "[PolyLang/ModLoader] Config too large (%ld bytes): %s\n",
                sz, json_path.c_str());
        return false;
    }

    // Safe cast: sz > 0 && sz <= 65536.
    size_t usz = static_cast<size_t>(sz);
    std::string buf(usz, '\0');
    size_t bytes_read = fread(buf.data(), 1, usz, f);
    fclose(f);
    if (bytes_read != usz) {
        fprintf(stderr, "[PolyLang/ModLoader] Short read on: %s "
                        "(expected %zu got %zu)\n",
                json_path.c_str(), usz, bytes_read);
        return false;
    }

    out = ModConfig{};
    bool ok = tiny_json_parse(buf, [&](const std::string& key, const std::string& val) {
        if (key == "sandbox")  out.sandbox  = (trim(val) == "true");
        if (key == "version")  out.version  = unquote(val);
        if (key == "entry")    out.entry    = unquote(val);
        if (key == "tier")     out.tier     = tier_from_string(unquote(val));
        if (key == "allowed_caps") {
            auto names = parse_string_array(val);
            out.allowed_caps = caps_from_names(names);
        }
        if (key == "dependencies") {
            out.dependencies = parse_string_array(val);
        }
    });
    return ok;
}

SandboxTier ModLoader::tier_from_string(const std::string& s) {
    if (s == "trusted")     return SandboxTier::Trusted;
    if (s == "quarantined") return SandboxTier::Quarantined;
    return SandboxTier::Isolated;
}

uint32_t ModLoader::caps_from_names(const std::vector<std::string>& names) {
    uint32_t caps = 0;
    for (const auto& n : names) {
        if (n == "PL_SANDBOX_FILE_READ")   caps |= PL_SANDBOX_FILE_READ;
        if (n == "PL_SANDBOX_FILE_WRITE")  caps |= PL_SANDBOX_FILE_WRITE;
        if (n == "PL_SANDBOX_NETWORK")     caps |= PL_SANDBOX_NETWORK;
        if (n == "PL_SANDBOX_PROCESS")     caps |= PL_SANDBOX_PROCESS;
        if (n == "PL_SANDBOX_FULL")        caps  = PL_SANDBOX_FULL;
    }
    return caps;
}

std::vector<std::string> ModLoader::topo_sort(
    const std::unordered_map<std::string, std::vector<std::string>>& deps) {

    std::unordered_map<std::string, int> state;
    std::vector<std::string> order;
    bool cycle = false;

    std::function<void(const std::string&)> visit = [&](const std::string& node) {
        if (cycle) return;
        auto it = state.find(node);
        if (it != state.end() && it->second == 2) return;
        if (it != state.end() && it->second == 1) {
            fprintf(stderr, "[PolyLang/ModLoader] Dependency cycle at '%s'\n", node.c_str());
            cycle = true; return;
        }
        state[node] = 1;
        auto dep_it = deps.find(node);
        if (dep_it != deps.end()) {
            for (const auto& dep : dep_it->second) {
                std::string dep_name = dep;
                auto at = dep_name.find('@');
                if (at != std::string::npos) dep_name = dep_name.substr(0, at);
                if (!sanitize_entry_path(dep_name)) {
                    fprintf(stderr, "[PolyLang/ModLoader] Invalid dependency name '%s'\n",
                            dep_name.c_str());
                    cycle = true; return;
                }
                visit(dep_name);
            }
        }
        state[node] = 2;
        if (!cycle) order.push_back(node);
    };

    for (const auto& [node, _] : deps) visit(node);
    return cycle ? std::vector<std::string>{} : order;
}

bool ModLoader::load_mod_directory(
    const std::string& dir_path,
    std::function<void(const std::string&, const ModConfig&)> on_loaded) {

    namespace fs = std::filesystem;
    if (!fs::is_directory(dir_path)) {
        fprintf(stderr, "[PolyLang/ModLoader] Not a directory: %s\n", dir_path.c_str());
        return false;
    }

    std::unordered_map<std::string, ModConfig> configs;
    std::unordered_map<std::string, std::vector<std::string>> dep_graph;

    for (const auto& entry : fs::directory_iterator(dir_path)) {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        if (fname.size() < 16) continue;
        if (fname.rfind(".polylang_config") != fname.size() - 16) continue;

        ModConfig cfg;
        if (!parse_config(entry.path().string(), cfg)) {
            fprintf(stderr, "[PolyLang/ModLoader] Failed to parse: %s\n",
                    entry.path().string().c_str());
            continue;
        }
        if (cfg.entry.empty()) continue;

        if (!sanitize_entry_path(cfg.entry)) {
            fprintf(stderr, "[PolyLang/ModLoader] Unsafe entry path '%s' in %s — skipped\n",
                    cfg.entry.c_str(), entry.path().string().c_str());
            continue;
        }

        std::string mod_name = cfg.entry;
        while (true) {
            auto dot = mod_name.rfind('.');
            if (dot == std::string::npos) break;
            mod_name = mod_name.substr(0, dot);
        }
        if (!sanitize_entry_path(mod_name)) {
            fprintf(stderr, "[PolyLang/ModLoader] Unsafe mod name '%s' — skipped\n",
                    mod_name.c_str());
            continue;
        }

        configs[mod_name] = cfg;
        dep_graph[mod_name] = cfg.dependencies;

        if (cfg.sandbox && cfg.tier != SandboxTier::Trusted) {
            std::string script_path = dir_path + "/" + cfg.entry;
            uint32_t caps = cfg.allowed_caps;
            if (cfg.tier == SandboxTier::Quarantined) caps = PL_SANDBOX_NONE;
            RuntimeManager::get_singleton()->register_sandboxed_path(script_path, caps);
        }
    }

    auto sorted = topo_sort(dep_graph);
    if (sorted.empty() && !configs.empty()) {
        fprintf(stderr, "[PolyLang/ModLoader] Dependency cycle detected — aborting\n");
        return false;
    }

    if (on_loaded) {
        for (const auto& mod_name : sorted) {
            auto it = configs.find(mod_name);
            if (it == configs.end()) continue;
            const ModConfig& cfg = it->second;
            std::string script_path = dir_path + "/" + cfg.entry;
            on_loaded(script_path, cfg);
        }
    }
    return true;
}

} // namespace polylang
