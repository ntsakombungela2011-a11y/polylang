// =============================================================
// sandbox_escape_test.cpp  —  PolyLang v5 Sandbox Security Tests
// =============================================================
// Headless (no Godot) test binary that loads each adapter shared
// library via dlopen, calls pl_compile_sandboxed(), instantiates
// the result, and invokes a method that attempts a dangerous OS
// operation.  Each test PASSES only if the dangerous call is
// blocked (returns PL_ERR_SANDBOX or PL_ERR_EXCEPTION, or returns
// a nil/null value without executing the dangerous side-effect).
//
// Build:
//   g++ -std=c++17 -O0 -g -I../include sandbox_escape_test.cpp \
//       -ldl -o sandbox_escape_test
//
// Run:
//   ./sandbox_escape_test                    # all tests
//   ./sandbox_escape_test lua python quickjs # specific adapters
//
// Exit code:
//   0  — all tests passed
//   N  — N tests failed
// =============================================================
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <dlfcn.h>

#include "../include/pl_adapter_vtable.h"

// ── ANSI colour helpers ───────────────────────────────────────
#define RED   "\033[31m"
#define GREEN "\033[32m"
#define RESET "\033[0m"
#define BOLD  "\033[1m"

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

// ── Test case descriptor ──────────────────────────────────────
struct TestCase {
    const char* name;           // adapter display name
    const char* lib_path;       // .so path relative to build dir
    const char* sandbox_script; // script source that tries the dangerous op
    const char* trusted_script; // safe script (must compile + run cleanly)
    const char* method;         // method to call in sandbox_script
    // If side-effect_file is set, the test additionally checks the
    // file does NOT exist after the sandboxed call (belt-and-suspenders).
    const char* side_effect_file;
};

// ── Individual test runner ────────────────────────────────────
static void run_test(const TestCase& tc) {
    printf("\n" BOLD "[ %s ]" RESET "\n", tc.name);

    // --- Load shared library ---
    void* lib = dlopen(tc.lib_path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf(BOLD "  SKIP" RESET " — library not available: %s\n", dlerror());
        g_skip++;
        return;
    }

    typedef void (*GetVTableFn)(PLAdapterVTable*);
    auto get_vtable = reinterpret_cast<GetVTableFn>(dlsym(lib, "pl_get_vtable"));
    if (!get_vtable) {
        printf(RED "  FAIL" RESET " — pl_get_vtable not found\n");
        g_fail++; dlclose(lib); return;
    }

    PLAdapterVTable vt{};
    get_vtable(&vt);

    // --- Check PL_CAP_SANDBOX is advertised ---
    if (!(vt.capabilities & PL_CAP_SANDBOX)) {
        printf(RED "  FAIL" RESET " — PL_CAP_SANDBOX not set in capabilities (got 0x%x)\n",
               vt.capabilities);
        g_fail++; dlclose(lib); return;
    }
    printf("  PL_CAP_SANDBOX ........... " GREEN "OK" RESET "\n");

    // --- pl_compile_sandboxed must exist ---
    if (!vt.pl_compile_sandboxed) {
        printf(RED "  FAIL" RESET " — pl_compile_sandboxed is null\n");
        g_fail++; dlclose(lib); return;
    }

    // --- Init runtime ---
    if (vt.pl_init_runtime) vt.pl_init_runtime();

    // ── Test 1: sandboxed compile must succeed ────────────────
    void* h = vt.pl_compile_sandboxed(tc.sandbox_script,
                                       "test_sandbox.pl", PL_SANDBOX_NONE);
    if (!h) {
        printf(RED "  FAIL" RESET " — pl_compile_sandboxed returned null\n");
        g_fail++; if (vt.pl_shutdown_runtime) vt.pl_shutdown_runtime();
        dlclose(lib); return;
    }
    printf("  Sandboxed compile ........ " GREEN "OK" RESET "\n");

    // ── Test 2: instantiate must succeed ──────────────────────
    void* inst = vt.pl_instantiate_class(h, "test_sandbox.pl");
    if (!inst) {
        printf(RED "  FAIL" RESET " — pl_instantiate_class returned null\n");
        vt.pl_free_compiled(h);
        g_fail++; if (vt.pl_shutdown_runtime) vt.pl_shutdown_runtime();
        dlclose(lib); return;
    }
    printf("  Instantiate .............. " GREEN "OK" RESET "\n");

    // ── Test 3: dangerous method must be blocked ──────────────
    PLValue ret{}; pl_value_init(&ret);
    int r = vt.pl_call_method(inst, tc.method, nullptr, 0, &ret);

    bool blocked = false;
    if (r == PL_ERR_SANDBOX) {
        printf("  Blocked (PL_ERR_SANDBOX) . " GREEN "OK" RESET "\n");
        blocked = true;
    } else if (r == PL_ERR_EXCEPTION) {
        printf("  Blocked (PL_ERR_EXCEPTION) " GREEN "OK" RESET "\n");
        blocked = true;
    } else if (r == PL_OK && ret.type == PL_TYPE_NIL) {
        // Many adapters return nil when the dangerous call was blocked at
        // the language level (e.g. the function is overridden to do nothing).
        printf("  Blocked (nil return) ..... " GREEN "OK" RESET "\n");
        blocked = true;
    } else {
        printf(RED "  FAIL" RESET " — dangerous call was NOT blocked"
               " (ret=%d, value_type=%d)\n", r, ret.type);
        blocked = false;
    }

    // Optional: verify no side-effect file was created.
    if (tc.side_effect_file && blocked) {
        FILE* chk = fopen(tc.side_effect_file, "r");
        if (chk) {
            fclose(chk); remove(tc.side_effect_file);
            printf(RED "  FAIL" RESET " — side-effect file was created: %s\n",
                   tc.side_effect_file);
            blocked = false;
        } else {
            printf("  No side-effect file ...... " GREEN "OK" RESET "\n");
        }
    }

    if (vt.pl_free_value_contents) vt.pl_free_value_contents(&ret);
    vt.pl_free_instance(inst);
    vt.pl_free_compiled(h);

    // ── Test 4: trusted compile must still work ───────────────
    void* h2 = vt.pl_compile(tc.trusted_script, "test_trusted.pl");
    if (!h2) {
        printf(RED "  FAIL" RESET " — pl_compile (trusted) returned null\n");
        blocked = false; // count as failure
    } else {
        printf("  Trusted compile .......... " GREEN "OK" RESET "\n");
        vt.pl_free_compiled(h2);
    }

    if (vt.pl_shutdown_runtime) vt.pl_shutdown_runtime();
    dlclose(lib);

    if (blocked) g_pass++;
    else         g_fail++;
}

// ── Test data ─────────────────────────────────────────────────

static const char* LUA_SANDBOX_SCRIPT =
    "local LuaScript = {}\n"
    "function LuaScript:_ready() end\n"
    "function LuaScript:attempt_escape()\n"
    "  -- os.execute must be nil in sandboxed env\n"
    "  if os and os.execute then\n"
    "    os.execute('touch /tmp/polylang_sandbox_escape_lua')\n"
    "    return 'ESCAPED'\n"
    "  end\n"
    "  return nil\n"
    "end\n"
    "return LuaScript\n";

static const char* LUA_TRUSTED_SCRIPT =
    "local LuaScript = {}\n"
    "function LuaScript:_ready() end\n"
    "function LuaScript:hello() return 'hello' end\n"
    "return LuaScript\n";

static const char* PYTHON_SANDBOX_SCRIPT =
    "class PyScript:\n"
    "    def _ready(self): pass\n"
    "    def attempt_escape(self):\n"
    "        try:\n"
    "            # open() must raise in sandboxed env\n"
    "            open('/tmp/polylang_sandbox_escape_py', 'w').write('escaped')\n"
    "            return 'ESCAPED'\n"
    "        except Exception:\n"
    "            return None\n";

static const char* PYTHON_TRUSTED_SCRIPT =
    "class PyScript:\n"
    "    def _ready(self): pass\n"
    "    def hello(self): return 'hello'\n";

static const char* JS_SANDBOX_SCRIPT =
    "class JsScript {\n"
    "  _ready() {}\n"
    "  attemptEscape() {\n"
    "    // eval must throw TypeError in sandboxed env\n"
    "    try {\n"
    "      return eval('1+1');\n"
    "    } catch(e) {\n"
    "      return null;\n"
    "    }\n"
    "  }\n"
    "}\n";

static const char* JS_TRUSTED_SCRIPT =
    "class JsScript {\n"
    "  _ready() {}\n"
    "  hello() { return 'hello'; }\n"
    "}\n";

static const TestCase TESTS[] = {
    {
        "Lua — os.execute() must be blocked",
        "build/adapters/lua/libpolylang_lua.so",
        LUA_SANDBOX_SCRIPT, LUA_TRUSTED_SCRIPT,
        "attempt_escape",
        "/tmp/polylang_sandbox_escape_lua",
    },
    {
        "Python — open() must be blocked",
        "build/adapters/python/libpolylang_python.so",
        PYTHON_SANDBOX_SCRIPT, PYTHON_TRUSTED_SCRIPT,
        "attempt_escape",
        "/tmp/polylang_sandbox_escape_py",
    },
    {
        "QuickJS/JS — eval() must be blocked",
        "build/adapters/javascript/libpolylang_js.so",
        JS_SANDBOX_SCRIPT, JS_TRUSTED_SCRIPT,
        "attemptEscape",
        nullptr,
    },
};

// ── main ──────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // If specific adapter names were passed, filter to those only.
    std::vector<std::string> filter;
    for (int i = 1; i < argc; ++i) filter.emplace_back(argv[i]);

    printf(BOLD "\n=== PolyLang v5 Sandbox Escape Tests ===\n\n" RESET);

    for (const auto& tc : TESTS) {
        if (!filter.empty()) {
            bool match = false;
            for (const auto& f : filter)
                if (std::string(tc.name).find(f) != std::string::npos) { match=true; break; }
            if (!match) continue;
        }
        run_test(tc);
    }

    printf(BOLD "\n=== Results: %d passed  %d failed  %d skipped ===\n\n" RESET,
           g_pass, g_fail, g_skip);

    if (g_fail > 0) {
        printf(RED BOLD "SANDBOX TESTS FAILED — do not merge.\n" RESET);
    } else {
        printf(GREEN BOLD "All sandbox tests passed.\n" RESET);
    }
    return g_fail;  // Non-zero exit fails CI
}
