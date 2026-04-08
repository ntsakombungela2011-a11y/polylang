cmake_minimum_required(VERSION 3.22)
project(polylang_vendor LANGUAGES C CXX)

# ─────────────────────────────────────────────────────────────────────────────
# PolyLang Vendored Dependencies
#
# Builds Lua 5.4, QuickJS, Wren, Squirrel, and AngelScript from source into
# CMAKE_INSTALL_PREFIX (default: vendor/install/).
#
# Usage from CI:
#   cmake -S vendor -B vendor/build -DCMAKE_INSTALL_PREFIX=vendor/install \
#         [-DANDROID_... flags if cross-compiling]
#   cmake --build vendor/build --parallel
#   cmake --install vendor/build
#
# Then pass -DCMAKE_PREFIX_PATH=<abs-path-to-vendor/install> to the main build.
# ─────────────────────────────────────────────────────────────────────────────

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ── Shared flags ─────────────────────────────────────────────────────────────
if(ANDROID)
    # Respect NDK toolchain; caller must pass CMAKE_TOOLCHAIN_FILE
    set(VENDOR_PIC ON)
else()
    set(VENDOR_PIC ON)
endif()

# ── Lua 5.4.7 ────────────────────────────────────────────────────────────────
FetchContent_Declare(lua_src
    URL      https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbff182577eba58d4c1729a9f02d80e07e46779fc7e2eb3e30f7de55ddc2494
)
FetchContent_MakeAvailable(lua_src)

file(GLOB LUA_SOURCES "${lua_src_SOURCE_DIR}/src/*.c")
# Remove executables — we only want the library
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac)\\.c$")

add_library(lua54 SHARED ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC
    $<BUILD_INTERFACE:${lua_src_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include/lua5.4>
)
set_target_properties(lua54 PROPERTIES
    OUTPUT_NAME lua5.4
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
)
if(ANDROID)
    target_compile_definitions(lua54 PRIVATE LUA_USE_POSIX)
elseif(UNIX)
    target_compile_definitions(lua54 PRIVATE LUA_USE_LINUX)
    target_link_libraries(lua54 PRIVATE m dl)
endif()

install(TARGETS lua54 LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES
    ${lua_src_SOURCE_DIR}/src/lua.h
    ${lua_src_SOURCE_DIR}/src/luaconf.h
    ${lua_src_SOURCE_DIR}/src/lualib.h
    ${lua_src_SOURCE_DIR}/src/lauxlib.h
    ${lua_src_SOURCE_DIR}/src/lua.hpp
    DESTINATION include/lua5.4
)

# ── QuickJS (JS + TypeScript adapter) ────────────────────────────────────────
FetchContent_Declare(quickjs_src
    GIT_REPOSITORY https://github.com/bellard/quickjs.git
    GIT_TAG        2024-01-13
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(quickjs_src)

add_library(quickjs SHARED
    ${quickjs_src_SOURCE_DIR}/quickjs.c
    ${quickjs_src_SOURCE_DIR}/libregexp.c
    ${quickjs_src_SOURCE_DIR}/libunicode.c
    ${quickjs_src_SOURCE_DIR}/cutils.c
    ${quickjs_src_SOURCE_DIR}/libbf.c
)
target_include_directories(quickjs PUBLIC
    $<BUILD_INTERFACE:${quickjs_src_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/quickjs>
)
set_target_properties(quickjs PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
)
target_compile_definitions(quickjs PRIVATE
    CONFIG_VERSION="2024-01-13"
    _GNU_SOURCE
)
if(UNIX)
    target_link_libraries(quickjs PRIVATE m)
endif()

install(TARGETS quickjs LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES
    ${quickjs_src_SOURCE_DIR}/quickjs.h
    ${quickjs_src_SOURCE_DIR}/quickjs-libc.h
    DESTINATION include/quickjs
)

# ── Wren 0.4.0 ───────────────────────────────────────────────────────────────
FetchContent_Declare(wren_src
    URL      https://github.com/wren-lang/wren/archive/refs/tags/0.4.0.tar.gz
    URL_HASH SHA256=1c0b6f5def5e617fc3e1d261cc8f0e524f53ec38a29bf5f8d8b83bcc6668a6e0
)
FetchContent_MakeAvailable(wren_src)

add_library(wren SHARED
    ${wren_src_SOURCE_DIR}/src/vm/wren_compiler.c
    ${wren_src_SOURCE_DIR}/src/vm/wren_core.c
    ${wren_src_SOURCE_DIR}/src/vm/wren_debug.c
    ${wren_src_SOURCE_DIR}/src/vm/wren_primitive.c
    ${wren_src_SOURCE_DIR}/src/vm/wren_utils.c
    ${wren_src_SOURCE_DIR}/src/vm/wren_value.c
    ${wren_src_SOURCE_DIR}/src/vm/wren_vm.c
    ${wren_src_SOURCE_DIR}/src/optional/wren_opt_meta.c
    ${wren_src_SOURCE_DIR}/src/optional/wren_opt_random.c
)
target_include_directories(wren PUBLIC
    $<BUILD_INTERFACE:${wren_src_SOURCE_DIR}/src/include>
    $<BUILD_INTERFACE:${wren_src_SOURCE_DIR}/src/vm>
    $<BUILD_INTERFACE:${wren_src_SOURCE_DIR}/src/optional>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(wren PROPERTIES POSITION_INDEPENDENT_CODE ON)

install(TARGETS wren LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES ${wren_src_SOURCE_DIR}/src/include/wren.h DESTINATION include)

# ── Squirrel 3.2 ─────────────────────────────────────────────────────────────
FetchContent_Declare(squirrel_src
    URL      https://github.com/albertodemichelis/squirrel/archive/refs/tags/v3.2.tar.gz
    URL_HASH SHA256=b19a8f3f49f29e44e7d00b6e29f70b9e1c2c91e0b32cd0f9a2a1c4d5a2af8afc
)
FetchContent_MakeAvailable(squirrel_src)

file(GLOB SQ_SOURCES
    "${squirrel_src_SOURCE_DIR}/squirrel/*.cpp"
    "${squirrel_src_SOURCE_DIR}/sqstdlib/*.cpp"
)
add_library(squirrel SHARED ${SQ_SOURCES})
target_include_directories(squirrel PUBLIC
    $<BUILD_INTERFACE:${squirrel_src_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(squirrel PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 14
)

install(TARGETS squirrel LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY ${squirrel_src_SOURCE_DIR}/include/ DESTINATION include)

# ── AngelScript 2.36.1 ───────────────────────────────────────────────────────
FetchContent_Declare(angelscript_src
    URL      https://www.angelcode.com/angelscript/sdk/files/angelscript_2.36.1.zip
    URL_HASH SHA256=c0c833c1c95b7d8c9b4f6c9c9d9e3d3c9f9e9b9a9d9c9b9a8d8c8b8a8d8c8b8  # placeholder — update with real hash
)
FetchContent_MakeAvailable(angelscript_src)

file(GLOB AS_SOURCES "${angelscript_src_SOURCE_DIR}/sdk/angelscript/source/*.cpp")
add_library(angelscript SHARED ${AS_SOURCES})
target_include_directories(angelscript PUBLIC
    $<BUILD_INTERFACE:${angelscript_src_SOURCE_DIR}/sdk/angelscript/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(angelscript PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 11
)
if(MSVC)
    target_compile_options(angelscript PRIVATE /EHsc)
endif()

install(TARGETS angelscript LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY ${angelscript_src_SOURCE_DIR}/sdk/angelscript/include/
        DESTINATION include)

# ── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "=== PolyLang Vendor Build ===")
message(STATUS "  Lua 5.4.7        : YES")
message(STATUS "  QuickJS 2024-01  : YES")
message(STATUS "  Wren 0.4.0       : YES")
message(STATUS "  Squirrel 3.2     : YES")
message(STATUS "  AngelScript 2.36 : YES")
message(STATUS "  Install prefix   : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "============================")
message(STATUS "")
