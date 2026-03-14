// =============================================================
// sidecar_config_test.cpp  —  .polylang_config sidecar unit tests
// =============================================================
// Self-contained: does NOT include runtime_manager.hpp or any
// Godot header. The sidecar parsing logic is tested directly by
// extracting it into a local reimplementation that mirrors exactly
// what RuntimeManager::maybe_register_sidecar does.
//
// Build:
//   g++ -std=c++17 -O0 -g -I../include \
//       sidecar_config_test.cpp -o sidecar_config_test
//
// Run:  ./sidecar_config_test
// Exit: 0 = all pass, N = N failures
// =============================================================
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <unordered_map>

#include "../include/pl_adapter_vtable.h"

// ── Minimal replica of RuntimeManager's sidecar types ────────
// These mirror the production code in runtime_manager.cpp exactly.

struct SidecarResult {
    bool     sandbox{false};
    uint32_t caps{PL_SANDBOX_NONE};
};

static std::string trim_ws(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    size_t e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

static SidecarResult parse_sidecar(const std::string& filepath) {
    SidecarResult r{};
    FILE* f = fopen(filepath.c_str(), "r");
    if (!f) return r;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';

        char* eq = strchr(line, '=');
        if (!eq) continue;

        std::string key = trim_ws(std::string(line, eq));
        std::string val = trim_ws(std::string(eq + 1));

        if (key == "sandbox") {
            if (val == "isolated" || val == "quarantined")
                r.sandbox = true;
            // "trusted" → r.sandbox stays false
        } else if (key == "allowed_caps") {
            size_t start = 0;
            while (start <= val.size()) {
                size_t comma = val.find(',', start);
                std::string cap = trim_ws(val.substr(start, comma - start));
                if      (cap == "file_read")  r.caps |= PL_SANDBOX_FILE_READ;
                else if (cap == "file_write") r.caps |= PL_SANDBOX_FILE_WRITE;
                else if (cap == "network")    r.caps |= PL_SANDBOX_NETWORK;
                else if (cap == "process")    r.caps |= PL_SANDBOX_PROCESS;
                else if (cap == "full")       r.caps  = PL_SANDBOX_FULL;
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
    }
    fclose(f);
    return r;
}

static std::pair<std::string,std::string>
sidecar_candidates(const std::string& path) {
    std::string per_script = path + ".polylang_config";
    std::string dir = path;
    auto slash = dir.rfind('/');
    dir = (slash != std::string::npos) ? dir.substr(0, slash + 1) : "";
    return { per_script, dir + ".polylang_config" };
}

// Minimal in-process sandbox registry (mirrors RuntimeManager's map)
struct Registry {
    std::unordered_map<std::string, SidecarResult> map;

    bool is_sandboxed(const std::string& p) const {
        auto it = map.find(p);
        return (it != map.end()) && it->second.sandbox;
    }
    uint32_t caps(const std::string& p) const {
        auto it = map.find(p);
        return (it != map.end()) ? it->second.caps : PL_SANDBOX_NONE;
    }
    void maybe_register(const std::string& path) {
        if (is_sandboxed(path)) return;  // idempotent
        auto [per_script, per_dir] = sidecar_candidates(path);
        SidecarResult r = parse_sidecar(per_script);
        if (!r.sandbox) r = parse_sidecar(per_dir);
        if (r.sandbox) map[path] = r;
    }
};

// ── Lightweight test harness ──────────────────────────────────
static int g_pass = 0, g_fail = 0;

#define EXPECT(cond, msg) \
    do { if (cond) { printf("  PASS  %s\n", msg); g_pass++; } \
         else { printf("  FAIL  %s  [line %d]\n", msg, __LINE__); g_fail++; } } while(0)

static void write_config(const char* path, const char* content) {
    FILE* f = fopen(path, "w"); assert(f);
    fputs(content, f); fclose(f);
}

// ── Test cases ────────────────────────────────────────────────

static void test_isolated_registers() {
    printf("\n[isolated registers sandbox]\n");
    std::string script  = "/tmp/pl_test_isolated.pl.lua";
    std::string sidecar = script + ".polylang_config";
    write_config(sidecar.c_str(), "sandbox = isolated\n");

    Registry reg;
    EXPECT(!reg.is_sandboxed(script),    "not registered before call");
    reg.maybe_register(script);
    EXPECT( reg.is_sandboxed(script),    "registered after call");
    EXPECT( reg.caps(script) == PL_SANDBOX_NONE, "caps = NONE");
    remove(sidecar.c_str());
}

static void test_quarantined_registers() {
    printf("\n[quarantined registers sandbox]\n");
    std::string script  = "/tmp/pl_test_quarantined.pl.py";
    std::string sidecar = script + ".polylang_config";
    write_config(sidecar.c_str(), "# mod config\nsandbox = quarantined\n");

    Registry reg;
    reg.maybe_register(script);
    EXPECT(reg.is_sandboxed(script), "registered");
    EXPECT(reg.caps(script) == PL_SANDBOX_NONE, "caps = NONE");
    remove(sidecar.c_str());
}

static void test_trusted_not_registered() {
    printf("\n[trusted is NOT sandboxed]\n");
    std::string script  = "/tmp/pl_test_trusted.pl.lua";
    std::string sidecar = script + ".polylang_config";
    write_config(sidecar.c_str(), "sandbox = trusted\n");

    Registry reg;
    reg.maybe_register(script);
    EXPECT(!reg.is_sandboxed(script), "trusted not registered");
    remove(sidecar.c_str());
}

static void test_allowed_caps_parsed() {
    printf("\n[allowed_caps parsed correctly]\n");
    std::string script  = "/tmp/pl_test_caps.pl.js";
    std::string sidecar = script + ".polylang_config";
    write_config(sidecar.c_str(),
        "sandbox = isolated\n"
        "allowed_caps = file_read, network\n");

    Registry reg;
    reg.maybe_register(script);
    EXPECT(reg.is_sandboxed(script), "registered");
    uint32_t c = reg.caps(script);
    EXPECT(c & PL_SANDBOX_FILE_READ,   "FILE_READ cap set");
    EXPECT(c & PL_SANDBOX_NETWORK,     "NETWORK cap set");
    EXPECT(!(c & PL_SANDBOX_FILE_WRITE),"FILE_WRITE not set");
    EXPECT(!(c & PL_SANDBOX_PROCESS),  "PROCESS not set");
    remove(sidecar.c_str());
}

static void test_directory_sidecar_fallback() {
    printf("\n[directory .polylang_config fallback]\n");
    std::string script   = "/tmp/pl_test_dir_fb.pl.lua";
    std::string dir_cfg  = "/tmp/.polylang_config";
    // Per-script sidecar does NOT exist; directory one does.
    remove((script + ".polylang_config").c_str());
    write_config(dir_cfg.c_str(), "sandbox = quarantined\n");

    Registry reg;
    reg.maybe_register(script);
    EXPECT(reg.is_sandboxed(script), "registered via directory sidecar");
    remove(dir_cfg.c_str());
}

static void test_per_script_overrides_directory() {
    printf("\n[per-script sidecar overrides directory]\n");
    std::string script     = "/tmp/pl_test_override.pl.lua";
    std::string per_script = script + ".polylang_config";
    std::string dir_cfg    = "/tmp/.polylang_config";

    // Directory says trusted (would not sandbox)
    write_config(dir_cfg.c_str(),    "sandbox = trusted\n");
    // Per-script says isolated (must win)
    write_config(per_script.c_str(), "sandbox = isolated\n");

    Registry reg;
    reg.maybe_register(script);
    EXPECT(reg.is_sandboxed(script), "per-script isolated wins over directory trusted");
    remove(per_script.c_str());
    remove(dir_cfg.c_str());
}

static void test_idempotent() {
    printf("\n[idempotent multiple calls]\n");
    std::string script  = "/tmp/pl_test_idem.pl.lua";
    std::string sidecar = script + ".polylang_config";
    write_config(sidecar.c_str(), "sandbox = isolated\n");

    Registry reg;
    reg.maybe_register(script);
    reg.maybe_register(script);
    reg.maybe_register(script);
    EXPECT(reg.is_sandboxed(script), "still registered after 3 calls");
    remove(sidecar.c_str());
}

static void test_no_sidecar_not_sandboxed() {
    printf("\n[no sidecar → not sandboxed]\n");
    std::string script = "/tmp/pl_test_no_sidecar_xyz99.pl.lua";
    remove((script + ".polylang_config").c_str());

    Registry reg;
    reg.maybe_register(script);
    EXPECT(!reg.is_sandboxed(script), "no sidecar means not sandboxed");
}

static void test_comments_ignored() {
    printf("\n[comments and blank lines ignored]\n");
    std::string script  = "/tmp/pl_test_comments.pl.lua";
    std::string sidecar = script + ".polylang_config";
    write_config(sidecar.c_str(),
        "# This is a comment\n"
        "\n"
        "  # Another comment\n"
        "sandbox = isolated  # inline comment\n"
        "allowed_caps = process\n");

    Registry reg;
    reg.maybe_register(script);
    EXPECT(reg.is_sandboxed(script), "registered despite comments");
    EXPECT(reg.caps(script) & PL_SANDBOX_PROCESS, "PROCESS cap set");
    remove(sidecar.c_str());
}

static void test_full_cap_alias() {
    printf("\n[allowed_caps = full sets all bits]\n");
    std::string script  = "/tmp/pl_test_full_cap.pl.lua";
    std::string sidecar = script + ".polylang_config";
    write_config(sidecar.c_str(),
        "sandbox = isolated\n"
        "allowed_caps = full\n");

    Registry reg;
    reg.maybe_register(script);
    EXPECT(reg.is_sandboxed(script), "registered");
    EXPECT(reg.caps(script) == PL_SANDBOX_FULL, "caps = FULL");
    remove(sidecar.c_str());
}

// ── main ──────────────────────────────────────────────────────
int main(void) {
    printf("=== PolyLang v5 Sidecar Config Unit Tests ===\n");

    test_isolated_registers();
    test_quarantined_registers();
    test_trusted_not_registered();
    test_allowed_caps_parsed();
    test_directory_sidecar_fallback();
    test_per_script_overrides_directory();
    test_idempotent();
    test_no_sidecar_not_sandboxed();
    test_comments_ignored();
    test_full_cap_alias();

    printf("\n=== %d passed  %d failed ===\n", g_pass, g_fail);
    if (g_fail > 0)
        printf("SIDECAR TESTS FAILED\n");
    else
        printf("All sidecar tests passed.\n");
    return g_fail;
}
