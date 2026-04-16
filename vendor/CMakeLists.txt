cmake_minimum_required(VERSION 3.22)
project(polylang_vendor LANGUAGES C CXX ASM)

# Builds all vendored C/C++ deps from their committed source trees
# into CMAKE_INSTALL_PREFIX so the main build can find them via
# -DCMAKE_PREFIX_PATH=<vendor/install>.
#
# Sources must already be present under vendor/ — run the
# "Vendor Dependencies" workflow first if they are missing.
#
# Usage:
#   cmake -S vendor -B vendor/build \
#         -DCMAKE_INSTALL_PREFIX=vendor/install \
#         [-DCMAKE_TOOLCHAIN_FILE=... for Android]
#   cmake --build vendor/build --parallel
#   cmake --install vendor/build

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Lua 5.4 ──────────────────────────────────────────────────────────────────
file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/lua/src/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "(lua|luac)\\.c$")
add_library(lua54 SHARED ${LUA_SOURCES})
target_include_directories(lua54 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lua/src>
    $<INSTALL_INTERFACE:include/lua5.4>)
set_target_properties(lua54 PROPERTIES OUTPUT_NAME lua5.4 C_STANDARD 99)
if(ANDROID)
    target_compile_definitions(lua54 PRIVATE LUA_USE_POSIX)
elseif(UNIX)
    target_compile_definitions(lua54 PRIVATE LUA_USE_LINUX)
    target_link_libraries(lua54 PRIVATE m dl)
endif()
install(TARGETS lua54
    RUNTIME DESTINATION lib   # Windows: installs lua5.4.dll
    LIBRARY DESTINATION lib   # Linux/Android: installs .so
    ARCHIVE DESTINATION lib)  # Windows: installs lua5.4.lib import lib
install(FILES
    lua/src/lua.h lua/src/luaconf.h lua/src/lualib.h
    lua/src/lauxlib.h lua/src/lua.hpp
    DESTINATION include/lua5.4)

# ── QuickJS ───────────────────────────────────────────────────────────────────
add_library(quickjs SHARED
    quickjs/quickjs.c
    quickjs/quickjs-libc.c
    quickjs/libregexp.c
    quickjs/libunicode.c
    quickjs/cutils.c
    quickjs/dtoa.c)
target_include_directories(quickjs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/quickjs>
    $<INSTALL_INTERFACE:include/quickjs>)
target_compile_definitions(quickjs PRIVATE CONFIG_VERSION="vendored" _GNU_SOURCE)
# MSVC does not expose NAN from <stdio.h>/<stdint.h> — quickjs.h needs it via <math.h>.
# Define _USE_MATH_DEFINES so that <math.h> exposes NAN/M_PI etc. on MSVC.
if(MSVC)
    target_compile_definitions(quickjs PRIVATE JS_NAN_BOXING)
    target_compile_definitions(quickjs PUBLIC _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(quickjs PRIVATE ws2_32)
endif()
set_target_properties(quickjs PROPERTIES C_STANDARD 11)
if(UNIX)
    target_link_libraries(quickjs PRIVATE m)
endif()
install(TARGETS quickjs
    RUNTIME DESTINATION lib   # Windows: installs quickjs.dll
    LIBRARY DESTINATION lib   # Linux/Android: installs .so
    ARCHIVE DESTINATION lib)  # Windows: installs quickjs.lib import lib
install(FILES quickjs/quickjs.h quickjs/quickjs-libc.h DESTINATION include/quickjs)

# ── Wren ──────────────────────────────────────────────────────────────────────
add_library(wren SHARED
    wren/src/vm/wren_compiler.c
    wren/src/vm/wren_core.c
    wren/src/vm/wren_debug.c
    wren/src/vm/wren_primitive.c
    wren/src/vm/wren_utils.c
    wren/src/vm/wren_value.c
    wren/src/vm/wren_vm.c
    wren/src/optional/wren_opt_meta.c
    wren/src/optional/wren_opt_random.c)
target_include_directories(wren PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wren/src/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wren/src/vm>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/wren/src/optional>
    $<INSTALL_INTERFACE:include>)
install(TARGETS wren
    RUNTIME DESTINATION lib   # Windows: installs wren.dll
    LIBRARY DESTINATION lib   # Linux/Android: installs .so
    ARCHIVE DESTINATION lib)  # Windows: installs wren.lib import lib
install(FILES wren/src/include/wren.h DESTINATION include)

# ── Squirrel ──────────────────────────────────────────────────────────────────
file(GLOB SQ_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/squirrel/squirrel/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/squirrel/sqstdlib/*.cpp")
add_library(squirrel SHARED ${SQ_SOURCES})
target_include_directories(squirrel PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/squirrel/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(squirrel PROPERTIES CXX_STANDARD 14)
install(TARGETS squirrel
    RUNTIME DESTINATION lib   # Windows: installs squirrel.dll
    LIBRARY DESTINATION lib   # Linux/Android: installs .so
    ARCHIVE DESTINATION lib)  # Windows: installs squirrel.lib import lib
install(DIRECTORY squirrel/include/ DESTINATION include)

# ── AngelScript ───────────────────────────────────────────────────────────────
file(GLOB AS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/angelscript/angelscript/source/*.cpp")

# AngelScript arm64 native calls require assembly (.S) files not included
# in the vendor zip. Build STATIC on Android (no native call thunks needed
# at link time — they're resolved at runtime via the JIT). Build SHARED on
# desktop where the x64 assembly file ships as part of the SDK.
if(ANDROID)
    set(AS_LIB_TYPE STATIC)
    set(AS_SOURCES_WITH_ASM ${AS_SOURCES})
else()
    set(AS_LIB_TYPE SHARED)
    set(AS_SOURCES_WITH_ASM ${AS_SOURCES})
    
    # Include MSVC x64 assembly for native calling conventions
    if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        enable_language(ASM_MASM)
        list(APPEND AS_SOURCES_WITH_ASM "${CMAKE_CURRENT_SOURCE_DIR}/angelscript/angelscript/source/as_callfunc_x64_msvc_asm.asm")
    endif()
endif()

add_library(angelscript ${AS_LIB_TYPE} ${AS_SOURCES_WITH_ASM})
target_include_directories(angelscript PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/angelscript/angelscript/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(angelscript PROPERTIES CXX_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # Only apply C++ exception handling to .cpp files, not to MASM .asm files
    target_compile_options(angelscript PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
endif()
install(TARGETS angelscript
    RUNTIME DESTINATION lib   # Windows: installs angelscript.dll
    LIBRARY DESTINATION lib   # Linux/Android: installs .so
    ARCHIVE DESTINATION lib)  # Windows: installs angelscript.lib import lib
install(DIRECTORY angelscript/angelscript/include/ DESTINATION include)

# ── AngelScript add-ons (scriptstdstring, scriptarray) ────────────────────────
# The adapter uses these headers — install them into named subdirs so
# #include <scriptstdstring/scriptstdstring.h> resolves correctly.
set(AS_ADDONS "${CMAKE_CURRENT_SOURCE_DIR}/angelscript/add_on")

add_library(as_scriptstdstring STATIC
    ${AS_ADDONS}/scriptstdstring/scriptstdstring.cpp)
target_include_directories(as_scriptstdstring PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/angelscript/angelscript/include>
    $<BUILD_INTERFACE:${AS_ADDONS}>
    $<INSTALL_INTERFACE:include>)
set_target_properties(as_scriptstdstring PROPERTIES
    CXX_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

add_library(as_scriptarray STATIC
    ${AS_ADDONS}/scriptarray/scriptarray.cpp)
target_include_directories(as_scriptarray PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/angelscript/angelscript/include>
    $<BUILD_INTERFACE:${AS_ADDONS}>
    $<INSTALL_INTERFACE:include>)
set_target_properties(as_scriptarray PROPERTIES
    CXX_STANDARD 11 POSITION_INDEPENDENT_CODE ON)

install(TARGETS as_scriptstdstring as_scriptarray
    RUNTIME DESTINATION lib
    LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
# Install add-on headers into named subdirs matching the #include paths
install(FILES ${AS_ADDONS}/scriptstdstring/scriptstdstring.h
    DESTINATION include/scriptstdstring)
install(FILES ${AS_ADDONS}/scriptarray/scriptarray.h
    DESTINATION include/scriptarray)

message(STATUS "")
message(STATUS "=== PolyLang Vendor Build ===")
message(STATUS "  Sources : committed under vendor/")
message(STATUS "  Install : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "=============================")
message(STATUS "")