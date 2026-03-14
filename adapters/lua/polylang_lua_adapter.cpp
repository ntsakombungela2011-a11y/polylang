// =============================================================
// polylang_lua_adapter.cpp  —  Lua 5.4 Adapter (ABI v5)
// SECTION 1: Full source with pl_compile_sandboxed
// =============================================================
// Sandbox policy:
//   Allowed:  math, string, table, pairs, ipairs, type, tostring,
//             tonumber, select, pcall, xpcall, error, assert,
//             next, rawget, rawset, rawequal, setmetatable,
//             getmetatable, print (redirected to stderr)
//   Blocked:  os, io, debug, package, require, dofile, loadfile,
//             load, collectgarbage, rawlen
// Implementation:
//   Loads source with luaL_loadbuffer, then replaces upvalue[1]
//   (_ENV) with a hand-crafted restricted table before execution.
//   Even if the script caches a reference to a blocked global
//   before the sandbox is applied, that global is absent from
//   the custom _ENV so access yields nil, not a real module.
// =============================================================
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>

#include "../../include/pl_adapter_vtable.h"

// ── Value conversion ──────────────────────────────────────────

static void pl_to_lua(lua_State* L, const PLValue& v) {
    switch (v.type) {
        case PL_TYPE_NIL:    lua_pushnil(L); break;
        case PL_TYPE_BOOL:   lua_pushboolean(L, v.b); break;
        case PL_TYPE_INT:    lua_pushinteger(L, (lua_Integer)v.i); break;
        case PL_TYPE_FLOAT:  lua_pushnumber(L, (lua_Number)v.f); break;
        case PL_TYPE_STRING: lua_pushstring(L, v.s ? v.s : ""); break;
        case PL_TYPE_VEC2:
            lua_createtable(L, 2, 0);
            lua_pushnumber(L, v.v2[0]); lua_rawseti(L, -2, 1);
            lua_pushnumber(L, v.v2[1]); lua_rawseti(L, -2, 2);
            break;
        case PL_TYPE_VEC3:
            lua_createtable(L, 3, 0);
            lua_pushnumber(L, v.v3[0]); lua_rawseti(L, -2, 1);
            lua_pushnumber(L, v.v3[1]); lua_rawseti(L, -2, 2);
            lua_pushnumber(L, v.v3[2]); lua_rawseti(L, -2, 3);
            break;
        default: lua_pushnil(L); break;
    }
}

static void lua_to_pl_value(lua_State* L, int idx, PLValue& out) {
    pl_value_init(&out);
    switch (lua_type(L, idx)) {
        case LUA_TNIL:     out.type = PL_TYPE_NIL; break;
        case LUA_TBOOLEAN: out.type = PL_TYPE_BOOL; out.b = lua_toboolean(L, idx) != 0; break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) { out.type = PL_TYPE_INT; out.i = lua_tointeger(L, idx); }
            else                       { out.type = PL_TYPE_FLOAT; out.f = lua_tonumber(L, idx); }
            break;
        case LUA_TSTRING: {
            const char* s = lua_tostring(L, idx);
            out.type = PL_TYPE_STRING; out.s = s ? strdup(s) : nullptr;
            break;
        }
        default: out.type = PL_TYPE_NIL; break;
    }
}

// ── Sandbox redirected print ──────────────────────────────────

static int lua_sandbox_print(lua_State* L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        const char* s = luaL_tolstring(L, i, nullptr);
        fprintf(stderr, "[PolyLang/Lua/sandbox] %s\n", s ? s : "(nil)");
        lua_pop(L, 1);
    }
    return 0;
}

// ── Sandbox env builder ───────────────────────────────────────
// Pushes a new table onto L that contains only the allowlisted symbols.
// This table replaces upvalue[1] (_ENV) of any loaded chunk.

static void lua_push_sandbox_env(lua_State* L) {
    lua_newtable(L); // sandbox_env at top

    // Safe libraries
    const char* safe_libs[] = { "math", "string", "table", nullptr };
    for (int i = 0; safe_libs[i]; ++i) {
        lua_getglobal(L, safe_libs[i]);
        if (!lua_isnil(L, -1)) lua_setfield(L, -2, safe_libs[i]);
        else lua_pop(L, 1);
    }

    // Safe built-in functions
    const char* safe_fns[] = {
        "pairs", "ipairs", "type", "tostring", "tonumber",
        "select", "next", "rawget", "rawset", "rawequal",
        "setmetatable", "getmetatable",
        "pcall", "xpcall", "error", "assert",
        nullptr
    };
    for (int i = 0; safe_fns[i]; ++i) {
        lua_getglobal(L, safe_fns[i]);
        if (!lua_isnil(L, -1)) lua_setfield(L, -2, safe_fns[i]);
        else lua_pop(L, 1);
    }

    // table.unpack aliased as unpack for compatibility
    lua_getglobal(L, "table");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "unpack");
        if (!lua_isnil(L, -1)) lua_setfield(L, -3, "unpack");
        else lua_pop(L, 1);
    }
    lua_pop(L, 1);

    // Redirected print
    lua_pushcfunction(L, lua_sandbox_print);
    lua_setfield(L, -2, "print");

    // Self-reference
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_ENV");
    // sandbox_env is now at top of stack
}

// ── Handles ───────────────────────────────────────────────────

struct LuaCompiled {
    lua_State*  L{nullptr};
    std::mutex  state_mutex;
    std::string class_name;
    int         class_ref{LUA_NOREF};
    bool        sandboxed{false};
};

struct LuaInstance {
    LuaCompiled* compiled{nullptr};
    int          inst_ref{LUA_NOREF};
};

// ── Error handler ─────────────────────────────────────────────

static int lua_traceback_handler(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    luaL_traceback(L, L, msg, 1);
    return 1;
}

// ── Shared compile core ───────────────────────────────────────

static void* lua_compile_core(const char* source, const char* path, bool sandboxed) {
    if (!source) return nullptr;

    auto* c = new LuaCompiled();
    c->sandboxed = sandboxed;
    c->L = luaL_newstate();
    if (!c->L) { delete c; return nullptr; }

    if (!sandboxed) {
        luaL_openlibs(c->L);
    } else {
        // Open only safe standard libs. Do NOT open io, os, package, debug.
        luaL_requiref(c->L, "_G",     luaopen_base,   1); lua_pop(c->L, 1);
        luaL_requiref(c->L, "math",   luaopen_math,   1); lua_pop(c->L, 1);
        luaL_requiref(c->L, "string", luaopen_string, 1); lua_pop(c->L, 1);
        luaL_requiref(c->L, "table",  luaopen_table,  1); lua_pop(c->L, 1);

        // Strip dangerous symbols from _G that luaopen_base registers.
        const char* blocked[] = {
            "os", "io", "debug", "package",
            "require", "dofile", "loadfile", "load",
            "collectgarbage", "rawlen",
            nullptr
        };
        for (int i = 0; blocked[i]; ++i) {
            lua_pushnil(c->L);
            lua_setglobal(c->L, blocked[i]);
        }
    }

    // Derive class name from path
    std::string p = path ? path : "script";
    auto slash = p.rfind('/'); if (slash != std::string::npos) p = p.substr(slash + 1);
    auto dot   = p.find('.');  if (dot   != std::string::npos) p = p.substr(0, dot);
    c->class_name = p;

    lua_State* L = c->L;
    lua_settop(L, 0); // clean stack

    int eh_idx = 1;
    lua_pushcfunction(L, lua_traceback_handler); // stack[1] = eh

    const char* chunk_name = path ? path : (sandboxed ? "=(sandbox)" : "=(polylang)");

    if (sandboxed) {
        // Load chunk first (does NOT execute yet)
        if (luaL_loadbuffer(L, source, strlen(source), chunk_name) != LUA_OK) {
            fprintf(stderr, "[PolyLang/Lua/sandbox] Load error: %s\n",
                    lua_tostring(L, -1));
            lua_close(L); delete c; return nullptr;
        }
        // stack[1]=eh, stack[2]=chunk_fn

        // Build sandbox env and replace chunk's _ENV upvalue (upvalue index 1)
        lua_push_sandbox_env(L);  // stack[3] = sandbox_env
        const char* uv = lua_setupvalue(L, 2, 1); // pops sandbox_env, sets it as _ENV
        if (!uv) {
            fprintf(stderr, "[PolyLang/Lua/sandbox] lua_setupvalue failed — bad Lua build?\n");
            lua_close(L); delete c; return nullptr;
        }
        // stack[1]=eh, stack[2]=chunk_fn (now has sandbox _ENV)

        if (lua_pcall(L, 0, 1, 1) != LUA_OK) { // call with eh at stack[1]
            fprintf(stderr, "[PolyLang/Lua/sandbox] Execute error: %s\n",
                    lua_tostring(L, -1));
            lua_close(L); delete c; return nullptr;
        }
        // stack[1]=eh, stack[2]=return_value
    } else {
        if (luaL_loadbuffer(L, source, strlen(source), chunk_name) != LUA_OK) {
            fprintf(stderr, "[PolyLang/Lua] Load error: %s\n", lua_tostring(L, -1));
            lua_close(L); delete c; return nullptr;
        }
        if (lua_pcall(L, 0, 1, 1) != LUA_OK) {
            fprintf(stderr, "[PolyLang/Lua] Execute error: %s\n", lua_tostring(L, -1));
            lua_close(L); delete c; return nullptr;
        }
        // stack[1]=eh, stack[2]=return_value
    }

    // Return value should be the class table
    if (!lua_istable(L, 2)) {
        lua_pop(L, 1); // pop return value
        lua_getglobal(L, c->class_name.c_str());
        if (!lua_istable(L, -1)) {
            fprintf(stderr, "[PolyLang/Lua%s] Script must return a table or define global '%s'\n",
                    sandboxed ? "/sandbox" : "", c->class_name.c_str());
            lua_close(L); delete c; return nullptr;
        }
    } else {
        lua_pushvalue(L, 2); // bring the table to the top
    }

    c->class_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (c->class_ref == LUA_REFNIL || c->class_ref == LUA_NOREF) {
        lua_close(L); delete c; return nullptr;
    }
    lua_settop(L, 0);
    return c;
}

// ── ABI functions ─────────────────────────────────────────────

static int  lua_init_runtime()     { return PL_OK; }
static void lua_shutdown_runtime() {}

static void* lua_compile_pl(const char* source, const char* path) {
    return lua_compile_core(source, path, false);
}

static void* lua_compile_sandboxed_pl(const char* source, const char* path,
                                       uint32_t /*allowed_caps*/) {
    return lua_compile_core(source, path, true);
}

static void lua_free_compiled(void* h) {
    if (!h) return;
    auto* c = static_cast<LuaCompiled*>(h);
    if (c->L) {
        if (c->class_ref != LUA_NOREF)
            luaL_unref(c->L, LUA_REGISTRYINDEX, c->class_ref);
        lua_close(c->L);
    }
    delete c;
}

static void* lua_instantiate_class(void* compiled_handle, const char* /*path*/) {
    auto* c = static_cast<LuaCompiled*>(compiled_handle);
    if (!c || !c->L) return nullptr;

    std::lock_guard<std::mutex> lk(c->state_mutex);
    lua_State* L = c->L;
    lua_settop(L, 0);

    lua_pushcfunction(L, lua_traceback_handler);     // stack[1] = eh
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->class_ref); // stack[2] = class
    if (!lua_istable(L, 2)) { lua_settop(L, 0); return nullptr; }

    lua_newtable(L);  // stack[3] = inst
    lua_newtable(L);  // stack[4] = mt
    lua_pushvalue(L, 2);
    lua_setfield(L, 4, "__index");
    lua_setmetatable(L, 3);

    auto* inst     = new LuaInstance;
    inst->compiled = c;
    inst->inst_ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops inst
    if (inst->inst_ref == LUA_REFNIL || inst->inst_ref == LUA_NOREF) {
        lua_settop(L, 0); delete inst; return nullptr;
    }
    lua_settop(L, 0);
    return inst;
}

static void lua_free_instance_pl(void* raw) {
    if (!raw) return;
    auto* inst = static_cast<LuaInstance*>(raw);
    if (inst->compiled && inst->compiled->L && inst->inst_ref != LUA_NOREF) {
        std::lock_guard<std::mutex> lk(inst->compiled->state_mutex);
        luaL_unref(inst->compiled->L, LUA_REGISTRYINDEX, inst->inst_ref);
        inst->inst_ref = LUA_NOREF;
    }
    delete inst;
}

// ── Internal call (caller holds state_mutex) ──────────────────

static int lua_call_unlocked(LuaInstance* inst, const char* method_name,
                              PLValue* args, int32_t argc, PLValue* ret) {
    lua_State* L = inst->compiled->L;
    int base = lua_gettop(L);

    lua_pushcfunction(L, lua_traceback_handler);
    int eh = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->inst_ref);
    if (!lua_istable(L, -1)) { lua_settop(L, base); return PL_ERR_GENERIC; }

    lua_getfield(L, -1, method_name);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        return PL_ERR_METHOD_NOT_FOUND;
    }

    lua_pushvalue(L, eh + 1); // push self (instance table)
    for (int32_t i = 0; i < argc; ++i) pl_to_lua(L, args[i]);

    if (lua_pcall(L, argc + 1, 1, eh) != LUA_OK) {
        fprintf(stderr, "[PolyLang/Lua] Error in %s: %s\n",
                method_name, lua_tostring(L, -1));
        lua_settop(L, base);
        ret->type = PL_TYPE_NIL;
        return PL_ERR_EXCEPTION;
    }

    lua_to_pl_value(L, -1, *ret);
    lua_settop(L, base);
    return PL_OK;
}

static int lua_call_method_pl(void* raw, const char* name,
                               PLValue* args, int32_t argc, PLValue* ret) {
    auto* inst = static_cast<LuaInstance*>(raw);
    if (!inst || !inst->compiled || !inst->compiled->L) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(inst->compiled->state_mutex);
    return lua_call_unlocked(inst, name, args, argc, ret);
}

static int lua_call_builtin_pl(void* raw, int32_t method_id,
                                PLValue* args, int32_t argc, PLValue* ret) {
    const char* name = nullptr;
    switch (method_id) {
        case PL_METHOD_READY:           name = "_ready"; break;
        case PL_METHOD_PROCESS:         name = "_process"; break;
        case PL_METHOD_PHYSICS_PROCESS: name = "_physics_process"; break;
        case PL_METHOD_ENTER_TREE:      name = "_enter_tree"; break;
        case PL_METHOD_EXIT_TREE:       name = "_exit_tree"; break;
        case PL_METHOD_INPUT:           name = "_input"; break;
        case PL_METHOD_NOTIFICATION:    name = "_notification"; break;
        default: return PL_ERR_NOT_IMPLEMENTED;
    }
    return lua_call_method_pl(raw, name, args, argc, ret);
}

static void lua_batch_process_pl(PLBatchEntry* entries, int32_t count) {
    if (!entries || count <= 0) return;

    std::sort(entries, entries + count, [](const PLBatchEntry& a, const PLBatchEntry& b) {
        return static_cast<LuaInstance*>(a.instance)->compiled
             < static_cast<LuaInstance*>(b.instance)->compiled;
    });

    LuaCompiled* cur = nullptr;
    for (int32_t i = 0; i < count; ++i) {
        auto* inst = static_cast<LuaInstance*>(entries[i].instance);
        if (!inst || !inst->compiled) continue;

        if (inst->compiled != cur) {
            if (cur) cur->state_mutex.unlock();
            cur = inst->compiled;
            cur->state_mutex.lock();
        }

        PLValue dv; pl_value_init(&dv);
        dv.type = PL_TYPE_FLOAT; dv.f = entries[i].delta;
        PLValue ret; pl_value_init(&ret);
        int r = lua_call_unlocked(inst, "_process", &dv, 1, &ret);
        if (r != PL_OK && r != PL_ERR_METHOD_NOT_FOUND) {
            entries[i].error.type = PL_TYPE_STRING;
            entries[i].error.s    = strdup("_process failed");
        }
        if (ret.type == PL_TYPE_STRING && ret.s) { free(ret.s); ret.s = nullptr; }
    }
    if (cur) cur->state_mutex.unlock();
}

static int lua_set_prop(void* raw, const char* name, const PLValue* value) {
    auto* inst = static_cast<LuaInstance*>(raw);
    if (!inst || !inst->compiled || !inst->compiled->L) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(inst->compiled->state_mutex);
    lua_State* L = inst->compiled->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->inst_ref);
    pl_to_lua(L, *value);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
    return PL_OK;
}

static int lua_get_prop(void* raw, const char* name, PLValue* out) {
    auto* inst = static_cast<LuaInstance*>(raw);
    if (!inst || !inst->compiled || !inst->compiled->L) return PL_ERR_GENERIC;
    std::lock_guard<std::mutex> lk(inst->compiled->state_mutex);
    lua_State* L = inst->compiled->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, inst->inst_ref);
    lua_getfield(L, -1, name);
    if (lua_isnil(L, -1)) { lua_pop(L, 2); return PL_ERR_PROP_NOT_FOUND; }
    lua_to_pl_value(L, -1, *out);
    lua_pop(L, 2);
    return PL_OK;
}

static uint8_t lua_has_method_pl(void* compiled_handle, const char* name) {
    auto* c = static_cast<LuaCompiled*>(compiled_handle);
    if (!c || !c->L || !name) return 0;
    std::lock_guard<std::mutex> lk(c->state_mutex);
    lua_rawgeti(c->L, LUA_REGISTRYINDEX, c->class_ref);
    lua_getfield(c->L, -1, name);
    uint8_t has = lua_isfunction(c->L, -1) ? 1 : 0;
    lua_pop(c->L, 2);
    return has;
}

static void lua_free_value_contents_pl(PLValue* v) {
    if (!v) return;
    if (v->type == PL_TYPE_STRING) { free(v->s); v->s = nullptr; }
    if (v->type == PL_TYPE_ARRAY && v->array.data) {
        for (int i = 0; i < v->array.len; ++i)
            lua_free_value_contents_pl(&v->array.data[i]);
        free(v->array.data); v->array.data = nullptr;
    }
    v->type = PL_TYPE_NIL;
}

extern "C" PL_EXPORT void pl_get_vtable(PLAdapterVTable* out) {
    *out = PLAdapterVTable{};
    out->abi_version            = PL_ABI_VERSION;
    out->_reserved              = 0;
    out->_pad2                  = 0;
    out->capabilities           = PL_CAP_ANDROID | PL_CAP_IOS | PL_CAP_DESKTOP
                                 | PL_CAP_BATCH   | PL_CAP_BUILTIN_CALL
                                 | PL_CAP_SANDBOX;
    out->pl_init_runtime        = lua_init_runtime;
    out->pl_shutdown_runtime    = lua_shutdown_runtime;
    out->pl_compile             = lua_compile_pl;
    out->pl_compile_sandboxed   = lua_compile_sandboxed_pl;
    out->pl_free_compiled       = lua_free_compiled;
    out->pl_instantiate_class   = lua_instantiate_class;
    out->pl_free_instance       = lua_free_instance_pl;
    out->pl_call_method         = lua_call_method_pl;
    out->pl_call_builtin        = lua_call_builtin_pl;
    out->pl_batch_process       = lua_batch_process_pl;
    out->pl_set_property        = lua_set_prop;
    out->pl_get_property        = lua_get_prop;
    out->pl_has_method          = lua_has_method_pl;
    out->pl_free_value_contents = lua_free_value_contents_pl;
}
