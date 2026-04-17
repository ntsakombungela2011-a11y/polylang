cmake_minimum_required(VERSION 3.22)
project(polylang VERSION 6.7.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Options to control runtime/linker behavior (can mitigate dlopen/runtime crashes)
option(PL_PRESERVE_RPATH "Preserve RPATH in built artifacts to allow runtime linking" OFF)
option(PL_ENABLE_BIND_NOW "Enable linker -z now (immediate symbol binding). May cause dlopen errors." OFF)
option(PL_ENABLE_VISIBILITY_PRESET "Enable hidden symbol visibility preset (may hide symbols required by Godot)" OFF)
option(PL_ENABLE_HARDENING "Enable deep hardening flags (may break plugin runtime)" OFF)

if(PL_PRESERVE_RPATH)
    message(STATUS "PL_PRESERVE_RPATH=ON: preserving RPATH in built artifacts")
else()
    # Strip RPATH information leaks (legacy behavior)
    set(CMAKE_SKIP_RPATH ON)
endif()

# ── Android API guard ─────────────────────────────────────────
# std::shared_mutex + std::atomic require Android API 28+ (Bionic).
if(ANDROID)
    if(NOT DEFINED ANDROID_PLATFORM_LEVEL)
        string(REGEX MATCH "[0-9]+" ANDROID_PLATFORM_LEVEL "${ANDROID_PLATFORM}")
    endif()
    if(ANDROID_PLATFORM_LEVEL LESS 28)
        message(FATAL_ERROR
            "PolyLang v6.7 requires Android API 28+.\n"
            "Pass -DANDROID_PLATFORM=android-28 to CMake.")
    endif()
    message(STATUS "Android API ${ANDROID_PLATFORM_LEVEL} — OK")
endif()

# ── godot-cpp submodule guard ─────────────────────────────────
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/godot-cpp/CMakeLists.txt")
    message(STATUS "godot-cpp submodule missing; attempting to download godot-4.3-stable.zip")
    set(_gcpp_url "https://github.com/godotengine/godot-cpp/archive/refs/tags/godot-4.3-stable.zip")
    set(_gcpp_tmp "${CMAKE_CURRENT_SOURCE_DIR}/_gcpp_tmp")
    file(MAKE_DIRECTORY ${_gcpp_tmp})
    set(_zip_path "${_gcpp_tmp}/godot-cpp.zip")
    if(NOT EXISTS ${_zip_path})
        message(STATUS "Downloading ${_gcpp_url}...")
        file(DOWNLOAD ${_gcpp_url} ${_zip_path} SHOW_PROGRESS)
    endif()
    if(WIN32)
        execute_process(COMMAND powershell -NoProfile -Command "Expand-Archive -LiteralPath '${_zip_path}' -DestinationPath '${_gcpp_tmp}' -Force"
                        RESULT_VARIABLE _exp_res)
        if(NOT _exp_res EQUAL 0)
            message(WARNING "Failed to expand godot-cpp zip with PowerShell; please run 'git submodule update --init --recursive' or provide godot-cpp manually.")
        endif()
    else()
        find_program(UNZIP_EXE unzip)
        if(UNZIP_EXE)
            execute_process(COMMAND ${UNZIP_EXE} -o ${_zip_path} -d ${_gcpp_tmp} RESULT_VARIABLE _exp_res)
        else()
            execute_process(COMMAND ${CMAKE_COMMAND} -E tar xfz ${_zip_path} WORKING_DIRECTORY ${_gcpp_tmp} RESULT_VARIABLE _exp_res)
        endif()
        if(NOT _exp_res EQUAL 0)
            message(WARNING "Failed to extract godot-cpp zip; please ensure 'unzip' or 'tar' is available, or run 'git submodule update --init --recursive'.")
        endif()
    endif()
    # Move inner folder contents to expected location if extraction produced a single top-level dir
    file(GLOB _gcpp_children RELATIVE ${_gcpp_tmp} "${_gcpp_tmp}/*")
    list(LENGTH _gcpp_children _child_count)
    if(_child_count EQUAL 1)
        list(GET _gcpp_children 0 _inner)
        file(RENAME "${_gcpp_tmp}/${_inner}" "${CMAKE_CURRENT_SOURCE_DIR}/godot-cpp")
    else()
        file(RENAME "${_gcpp_tmp}" "${CMAKE_CURRENT_SOURCE_DIR}/godot-cpp")
    endif()
endif()
add_subdirectory(godot-cpp)

# Ensure godot-cpp generated headers (gen/include) are visible to the plugin
# Some CI runs build godot-cpp into the build tree, producing gen/include under the build dir.
find_path(GODOT_CPP_GEN_INCLUDE
    NAMES godot_cpp/core/gdextension_interface_loader.hpp
    PATHS
        "${CMAKE_BINARY_DIR}/godot-cpp/gen/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/godot-cpp/gen/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/build_windows/godot-cpp/gen/include"
        "${CMAKE_SOURCE_DIR}/build_windows/godot-cpp/gen/include"
        "${CMAKE_SOURCE_DIR}/godot-cpp/gen/include"
    NO_DEFAULT_PATH
)
if(GODOT_CPP_GEN_INCLUDE)
    # Convert to absolute real path to avoid issues with relative paths or spaces
    file(REAL_PATH "${GODOT_CPP_GEN_INCLUDE}" POLYLANG_GODOT_CPP_GEN_INCLUDE_REAL)
    if(EXISTS "${POLYLANG_GODOT_CPP_GEN_INCLUDE_REAL}")
        set(POLYLANG_GODOT_CPP_GEN_INCLUDE ${POLYLANG_GODOT_CPP_GEN_INCLUDE_REAL})
        message(STATUS "Found godot-cpp generated headers: ${POLYLANG_GODOT_CPP_GEN_INCLUDE}")
    else()
        # Fall back to original if REAL_PATH failed
        set(POLYLANG_GODOT_CPP_GEN_INCLUDE ${GODOT_CPP_GEN_INCLUDE})
        message(WARNING "Resolved godot-cpp gen include path does not exist: ${POLYLANG_GODOT_CPP_GEN_INCLUDE_REAL} — using original: ${POLYLANG_GODOT_CPP_GEN_INCLUDE}")
    endif()
    # If the specific header exists in the godot-cpp gen include, copy it into our include/ folder
    set(_g_header "${POLYLANG_GODOT_CPP_GEN_INCLUDE}/godot_cpp/core/gdextension_interface_loader.hpp")
    if(EXISTS "${_g_header}")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/godot_cpp/core")
        file(COPY "${_g_header}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/include/godot_cpp/core")
        message(STATUS "Copied godot-cpp generated header to include/: ${_g_header}")
    else()
        message(STATUS "godot-cpp generated header not present at configure time: ${_g_header}")
    endif()
else()
    message(WARNING "godot-cpp generated headers not found (godot_cpp/core/gdextension_interface_loader.hpp). If build fails, ensure godot-cpp 'gen/include' is generated and accessible.")
endif()

# ── iOS requires static libs ──────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(PL_LIB_TYPE STATIC)
    message(STATUS "iOS: building STATIC adapters")
else()
    set(PL_LIB_TYPE SHARED)
endif()

# ── Compiler hardening flags (guarded) ─────────────────────────────────
# DZ-03, DZ-04, DZ-07: Deep Hardening Configuration
# NOTE: These flags can improve safety but may break runtime behavior for GDExtensions.
set(PL_HARDEN_FLAGS "")
if(PL_ENABLE_HARDENING)
    if(MSVC)
        # MSVC deep hardening intentionally skipped to preserve runtime compatibility.
        message(STATUS "PL_ENABLE_HARDENING=ON: MSVC deep hardening is skipped")
    else()
        list(APPEND PL_HARDEN_FLAGS
            -Wall -Wextra -Wno-unused-parameter
            -fstack-protector-strong
            $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
            $<$<NOT:$<CONFIG:Debug>>:-D_GLIBCXX_ASSERTIONS> # DZ-07
        )
        if(NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
            list(APPEND PL_HARDEN_FLAGS -fPIE)
        endif()
        # RELRO is enabled; BIND_NOW is optional and controlled by PL_ENABLE_BIND_NOW.
        if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin|iOS")
            add_link_options(-Wl,-z,relro)
            if(PL_ENABLE_BIND_NOW)
                add_link_options(-Wl,-z,now)
            endif()
        endif()
    endif()
else()
    message(STATUS "PL_ENABLE_HARDENING=OFF: skipping extra hardening flags to improve runtime compatibility")
endif()

# ── Core GDExtension library ──────────────────────────────────
# ZT-18: Respect PL_LIB_TYPE (STATIC on iOS, SHARED elsewhere)
add_library(polylang ${PL_LIB_TYPE}
    # Core runtime
    src/register_types.cpp
    src/polylang_language.cpp
    src/polylang_script.cpp
    src/polylang_script_instance.cpp
    src/runtime_manager.cpp
    src/compile_cache.cpp
    src/hot_reload_scheduler.cpp
    src/variant_bridge.cpp
    # Polyglot scripting
    src/pl_polyglot_parser.cpp
    src/pl_polyglot_script.cpp
    # Signal + bridge
    src/pl_signal_bus.cpp
    src/pl_bridge.cpp
    src/pl_cross_inherit.cpp
    # Resource + engine API
    src/pl_resource_bridge.cpp
    src/pl_engine_api_bridge.cpp
    # Async
    src/pl_async_runtime.cpp
    src/pl_coroutine_scheduler.cpp
    # Tools
    src/pl_export_parser.cpp
    src/pl_profiler.cpp
    src/pl_state_transfer.cpp
    src/mod_loader.cpp
)

target_include_directories(polylang PRIVATE include src)
target_link_libraries(polylang PRIVATE godot::cpp)
target_compile_options(polylang PRIVATE ${PL_HARDEN_FLAGS})
# ZT-20: SOVERSION for ABI versioning on Linux
if(PL_ENABLE_VISIBILITY_PRESET)
    set_target_properties(polylang PROPERTIES
        PREFIX ""
        CXX_VISIBILITY_PRESET    hidden
        VISIBILITY_INLINES_HIDDEN ON
        POSITION_INDEPENDENT_CODE ON
    )
else()
    set_target_properties(polylang PROPERTIES
        PREFIX ""
        POSITION_INDEPENDENT_CODE ON
    )
endif()

if(DEFINED POLYLANG_GODOT_CPP_GEN_INCLUDE)
    # Ensure absolute path and existence before adding to target
    if(EXISTS "${POLYLANG_GODOT_CPP_GEN_INCLUDE}")
        target_include_directories(polylang PRIVATE "${POLYLANG_GODOT_CPP_GEN_INCLUDE}")
        message(STATUS "Added godot-cpp gen include to polylang: ${POLYLANG_GODOT_CPP_GEN_INCLUDE}")
    else()
        message(WARNING "POLYLANG_GODOT_CPP_GEN_INCLUDE set but directory does not exist: ${POLYLANG_GODOT_CPP_GEN_INCLUDE}")
    endif()
endif()

target_compile_definitions(polylang PRIVATE
    POLYLANG_VERSION_MAJOR=6
    POLYLANG_VERSION_MINOR=7
    POLYLANG_VERSION_PATCH=0
)
if(ANDROID)
    target_compile_definitions(polylang PRIVATE POLYLANG_ANDROID=1)
    target_link_libraries(polylang PRIVATE log)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_definitions(polylang PRIVATE POLYLANG_IOS=1 POLYLANG_RUNTIME_ONLY=1)
endif()

# ── Adapter helper macro ──────────────────────────────────────
macro(pl_adapter TARGET_NAME SOURCES)
    add_library(${TARGET_NAME} ${PL_LIB_TYPE} ${SOURCES})
    target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(${TARGET_NAME} PRIVATE ${PL_HARDEN_FLAGS})
    set_target_properties(${TARGET_NAME} PROPERTIES PREFIX "")
    if(ANDROID)
        set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".so")
    endif()
endmacro()

# ── Lua 5.4 ──────────────────────────────────────────────────
find_package(Lua QUIET)
if(LUA_FOUND AND LUA_VERSION_STRING VERSION_GREATER_EQUAL "5.4")
    pl_adapter(polylang_adapter_lua adapters/lua/polylang_lua_adapter.cpp)
    target_link_libraries(polylang_adapter_lua PRIVATE ${LUA_LIBRARIES})
    target_include_directories(polylang_adapter_lua PRIVATE ${LUA_INCLUDE_DIR})
    message(STATUS "Lua ${LUA_VERSION_STRING}: ENABLED")
else()
    message(STATUS "Lua 5.4+: NOT FOUND — skipped")
endif()

# ── Python 3.10+ ─────────────────────────────────────────────
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_package(Python3 3.10 COMPONENTS Development QUIET)
    if(Python3_FOUND)
        pl_adapter(polylang_adapter_python adapters/python/polylang_python_adapter.cpp)
        target_link_libraries(polylang_adapter_python PRIVATE Python3::Python)
        message(STATUS "Python ${Python3_VERSION}: ENABLED")
    else()
        message(STATUS "Python 3.10+: NOT FOUND — skipped")
    endif()
endif()

# ── QuickJS (JS + TS) ─────────────────────────────────────────
function(find_library_with_vendor_fallback var name)
    find_library(${var} NAMES ${name} QUIET)
    if(NOT ${var})
        # Try common vendor build locations as a fallback
        set(_vendor_build_release "${CMAKE_SOURCE_DIR}/../vendor/build-windows/Release")
        set(_vendor_build "${CMAKE_SOURCE_DIR}/../vendor/build-windows")
        find_library(${var} NAMES ${name} PATHS ${CMAKE_SOURCE_DIR}/../vendor/install-windows/lib ${_vendor_build_release} ${_vendor_build} NO_DEFAULT_PATH)
    endif()
    # If find_library still didn't return a usable path, try locating literal .lib files
    if(NOT ${var})
        set(_search_dirs
            "${CMAKE_SOURCE_DIR}/../vendor/install-windows/lib"
            "${CMAKE_SOURCE_DIR}/../vendor/build-windows"
            "${CMAKE_SOURCE_DIR}/../vendor/build-windows/Release"
            "${CMAKE_SOURCE_DIR}/../vendor"
        )
        foreach(_d IN LISTS _search_dirs)
            if(EXISTS "${_d}")
                file(GLOB _found_libs RELATIVE "${_d}" "${_d}/${name}*.lib")
                if(_found_libs)
                    list(GET _found_libs 0 _first)
                    set(_fullpath "${_d}/${_first}")
                    if(EXISTS "${_fullpath}")
                        set(${var} "${_fullpath}" PARENT_SCOPE)
                        break()
                    endif()
                endif()
            endif()
        endforeach()
    endif()
endfunction()

find_library_with_vendor_fallback(QUICKJS_LIB quickjs)
find_path(QUICKJS_INCLUDE NAMES quickjs/quickjs.h QUIET)
if(NOT QUICKJS_INCLUDE)
    find_path(QUICKJS_INCLUDE NAMES quickjs/quickjs.h PATHS ${CMAKE_SOURCE_DIR}/../vendor/install-windows/include ${CMAKE_SOURCE_DIR}/../vendor/build-windows/include ${CMAKE_SOURCE_DIR}/../vendor/include NO_DEFAULT_PATH)
endif()
if(QUICKJS_LIB AND QUICKJS_INCLUDE)
    if(EXISTS "${QUICKJS_LIB}" OR TARGET quickjs)
        pl_adapter(polylang_adapter_js  adapters/javascript/polylang_js_adapter.cpp)
        target_link_libraries(polylang_adapter_js  PRIVATE ${QUICKJS_LIB})
        target_include_directories(polylang_adapter_js  PRIVATE ${QUICKJS_INCLUDE})
        pl_adapter(polylang_adapter_ts  adapters/typescript/polylang_ts_adapter.cpp)
        target_link_libraries(polylang_adapter_ts  PRIVATE ${QUICKJS_LIB})
        target_include_directories(polylang_adapter_ts  PRIVATE ${QUICKJS_INCLUDE})
    else()
        message(WARNING "QuickJS library variable set but file not found: ${QUICKJS_LIB} — skipping JS/TS adapters")
    endif()
    # MSVC: quickjs.h uses NAN (via JS_FLOAT64_NAN) but only includes <stdio.h>.
    # _USE_MATH_DEFINES makes <math.h>/<cmath> expose NAN/M_PI on MSVC.
    if(MSVC)
        target_compile_definitions(polylang_adapter_js PRIVATE _USE_MATH_DEFINES)
        target_compile_definitions(polylang_adapter_ts PRIVATE _USE_MATH_DEFINES)
    endif()
    message(STATUS "JS/TS (QuickJS): ENABLED")
else()
    message(STATUS "QuickJS: NOT FOUND — JS/TS skipped")
endif()

# ── Squirrel ─────────────────────────────────────────────────
find_library_with_vendor_fallback(SQUIRREL_LIB squirrel)
find_path(SQUIRREL_INC NAMES squirrel.h QUIET)
if(NOT SQUIRREL_INC)
    find_path(SQUIRREL_INC NAMES squirrel.h PATHS ${CMAKE_SOURCE_DIR}/../vendor/install-windows/include ${CMAKE_SOURCE_DIR}/../vendor/squirrel/include NO_DEFAULT_PATH)
endif()
if(SQUIRREL_LIB AND SQUIRREL_INC)
    if(EXISTS "${SQUIRREL_LIB}" OR TARGET squirrel)
        pl_adapter(polylang_adapter_squirrel adapters/squirrel/polylang_squirrel_adapter.cpp)
        target_link_libraries(polylang_adapter_squirrel PRIVATE ${SQUIRREL_LIB})
        target_include_directories(polylang_adapter_squirrel PRIVATE ${SQUIRREL_INC})
    else()
        message(WARNING "Squirrel library variable set but file not found: ${SQUIRREL_LIB} — skipping Squirrel adapter")
    endif()
    message(STATUS "Squirrel: ENABLED")
else()
    message(STATUS "Squirrel: NOT FOUND — skipped")
endif()

# ── Wren ─────────────────────────────────────────────────────
find_library_with_vendor_fallback(WREN_LIB wren)
find_path(WREN_INC NAMES wren.h QUIET)
if(NOT WREN_INC)
    find_path(WREN_INC NAMES wren.h PATHS ${CMAKE_SOURCE_DIR}/../vendor/install-windows/include ${CMAKE_SOURCE_DIR}/../vendor/wren/src/include NO_DEFAULT_PATH)
endif()
if(WREN_LIB AND WREN_INC)
    if(EXISTS "${WREN_LIB}" OR TARGET wren)
        pl_adapter(polylang_adapter_wren adapters/wren/polylang_wren_adapter.cpp)
        target_link_libraries(polylang_adapter_wren PRIVATE ${WREN_LIB})
        target_include_directories(polylang_adapter_wren PRIVATE ${WREN_INC})
    else()
        message(WARNING "Wren library variable set but file not found: ${WREN_LIB} — skipping Wren adapter")
    endif()
    message(STATUS "Wren: ENABLED")
else()
    message(STATUS "Wren: NOT FOUND — skipped")
endif()

# ── AngelScript ──────────────────────────────────────────────
# Android arm64 requires as_callfunc_arm64.S assembly which is not
# in the vendored sources — skip AngelScript on Android entirely.
find_library_with_vendor_fallback(AS_LIB angelscript)
find_path(AS_INC NAMES angelscript.h QUIET)
if(NOT AS_INC)
    find_path(AS_INC NAMES angelscript.h PATHS ${CMAKE_SOURCE_DIR}/../vendor/install-windows/include ${CMAKE_SOURCE_DIR}/../vendor/angelscript/angelscript/include NO_DEFAULT_PATH)
endif()
if(AS_LIB AND AS_INC AND NOT ANDROID)
    if(EXISTS "${AS_LIB}" OR TARGET angelscript)
        pl_adapter(polylang_adapter_as adapters/angelscript/polylang_angelscript_adapter.cpp)
        target_link_libraries(polylang_adapter_as PRIVATE ${AS_LIB})
        target_include_directories(polylang_adapter_as PRIVATE ${AS_INC})
    else()
        message(WARNING "AngelScript library variable set but file not found: ${AS_LIB} — skipping AngelScript adapter")
    endif()
    # Add-on headers: scriptstdstring and scriptarray live in named subdirs
    if(DEFINED AS_ADDONS_LIB)
        find_library(AS_STDSTRING_LIB NAMES as_scriptstdstring PATHS ${AS_ADDONS_LIB} NO_DEFAULT_PATH)
        find_library(AS_ARRAY_LIB    NAMES as_scriptarray     PATHS ${AS_ADDONS_LIB} NO_DEFAULT_PATH)
        if(AS_STDSTRING_LIB)
            target_link_libraries(polylang_adapter_as PRIVATE ${AS_STDSTRING_LIB})
        endif()
        if(AS_ARRAY_LIB)
            target_link_libraries(polylang_adapter_as PRIVATE ${AS_ARRAY_LIB})
        endif()
    endif()
    message(STATUS "AngelScript: ENABLED")
else()
    message(STATUS "AngelScript: NOT FOUND — skipped")
endif()

# ── Rust (pre-built .so / .a) ─────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(RUST_LIB "${CMAKE_SOURCE_DIR}/adapters/rust/target/aarch64-apple-ios/release/libpolylang_rust.a")
elseif(WIN32)
    set(RUST_LIB "${CMAKE_SOURCE_DIR}/adapters/rust/target/release/polylang_adapter_rust.dll")
elseif(ANDROID)
    set(RUST_LIB "${CMAKE_SOURCE_DIR}/adapters/rust/target/aarch64-linux-android/release/libpolylang_rust.so")
else()
    set(RUST_LIB "${CMAKE_SOURCE_DIR}/adapters/rust/target/release/libpolylang_rust.so")
endif()
if(EXISTS "${RUST_LIB}")
    add_library(polylang_adapter_rust IMPORTED ${PL_LIB_TYPE})
    set_target_properties(polylang_adapter_rust PROPERTIES IMPORTED_LOCATION "${RUST_LIB}")
    message(STATUS "Rust: ENABLED (${RUST_LIB})")
else()
    message(STATUS "Rust: pre-built not found — run cargo build --release in adapters/rust/")
endif()

# ── Zig (pre-built) ──────────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(ZIG_LIB "${CMAKE_SOURCE_DIR}/adapters/zig/zig-out/lib/libpolylang_zig.a")
elseif(ANDROID)
    set(ZIG_LIB "${CMAKE_SOURCE_DIR}/adapters/zig/zig-out/lib/libpolylang_zig_android.so")
else()
    set(ZIG_LIB "${CMAKE_SOURCE_DIR}/adapters/zig/zig-out/lib/libpolylang_zig.so")
endif()
if(EXISTS "${ZIG_LIB}")
    add_library(polylang_adapter_zig IMPORTED ${PL_LIB_TYPE})
    set_target_properties(polylang_adapter_zig PROPERTIES IMPORTED_LOCATION "${ZIG_LIB}")
    message(STATUS "Zig: ENABLED")
else()
    message(STATUS "Zig: pre-built not found — run zig build in adapters/zig/")
endif()

# ── Go (pre-built, no iOS) ───────────────────────────────────
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    if(ANDROID)
        set(GO_LIB "${CMAKE_SOURCE_DIR}/adapters/go/polylang_go_adapter_android.so")
    elseif(WIN32)
        set(GO_LIB "${CMAKE_SOURCE_DIR}/adapters/go/polylang_go_adapter.dll")
    else()
        set(GO_LIB "${CMAKE_SOURCE_DIR}/adapters/go/polylang_go_adapter.so")
    endif()
    if(EXISTS "${GO_LIB}")
        add_library(polylang_adapter_go IMPORTED SHARED)
        set_target_properties(polylang_adapter_go PROPERTIES IMPORTED_LOCATION "${GO_LIB}")
        message(STATUS "Go: ENABLED")
    else()
        message(STATUS "Go: pre-built not found — run go build -buildmode=c-shared in adapters/go/")
    endif()
endif()

# ── Nim (pre-built) ──────────────────────────────────────────
set(NIM_LIB "${CMAKE_SOURCE_DIR}/adapters/nim/polylang_nim_adapter.so")
if(EXISTS "${NIM_LIB}")
    add_library(polylang_adapter_nim IMPORTED SHARED)
    set_target_properties(polylang_adapter_nim PROPERTIES IMPORTED_LOCATION "${NIM_LIB}")
    message(STATUS "Nim: ENABLED")
else()
    message(STATUS "Nim: pre-built not found — run nim c --app:lib in adapters/nim/")
endif()

# ── Haxe ─────────────────────────────────────────────────────
find_library(HXCPP_LIB NAMES hxcpp QUIET)
if(HXCPP_LIB)
    pl_adapter(polylang_adapter_haxe adapters/haxe/polylang_haxe_adapter.cpp)
    target_link_libraries(polylang_adapter_haxe PRIVATE ${HXCPP_LIB})
    message(STATUS "Haxe: ENABLED")
else()
    message(STATUS "Haxe: NOT FOUND — skipped")
endif()

# ── Kotlin/JVM (no iOS) ──────────────────────────────────────
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND DEFINED ENV{JAVA_HOME})
    set(JVM_INCS $ENV{JAVA_HOME}/include)
    if(UNIX AND NOT APPLE)
        list(APPEND JVM_INCS $ENV{JAVA_HOME}/include/linux)
    elseif(APPLE)
        list(APPEND JVM_INCS $ENV{JAVA_HOME}/include/darwin)
    endif()
    find_library(JVM_LIB jvm
        PATHS $ENV{JAVA_HOME}/lib/server $ENV{JAVA_HOME}/jre/lib/server QUIET)
    if(JVM_LIB)
        pl_adapter(polylang_adapter_kotlin adapters/kotlin/polylang_kotlin_adapter.cpp)
        target_include_directories(polylang_adapter_kotlin PRIVATE ${JVM_INCS})
        target_link_libraries(polylang_adapter_kotlin PRIVATE ${JVM_LIB})
        message(STATUS "Kotlin/JVM: ENABLED")
    endif()
else()
    message(STATUS "Kotlin: JAVA_HOME not set or iOS — skipped")
endif()

# ── Julia (desktop only) ─────────────────────────────────────
if(NOT ANDROID AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND DEFINED ENV{JULIA_HOME})
    find_library(JULIA_LIB julia PATHS $ENV{JULIA_HOME}/lib QUIET)
    find_path(JULIA_INC julia.h PATHS $ENV{JULIA_HOME}/include/julia QUIET)
    if(JULIA_LIB AND JULIA_INC)
        pl_adapter(polylang_adapter_julia adapters/julia/polylang_julia_adapter.cpp)
        target_link_libraries(polylang_adapter_julia PRIVATE ${JULIA_LIB})
        target_include_directories(polylang_adapter_julia PRIVATE ${JULIA_INC})
        message(STATUS "Julia: ENABLED")
    else()
        message(STATUS "Julia: JULIA_HOME set but headers/lib not found — skipped")
    endif()
else()
    message(STATUS "Julia: desktop-only — skipped on Android/iOS or JULIA_HOME not set")
endif()

# ── Swift (macOS only) ───────────────────────────────────────
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_program(SWIFTC swiftc QUIET)
    if(SWIFTC)
        add_custom_target(polylang_adapter_swift
            COMMAND ${SWIFTC}
                -emit-library -module-name PolyLangSwift
                -I ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/swift/polylang_swift_adapter.swift
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/swift/polylang_swift_shim.c
                -o ${CMAKE_BINARY_DIR}/libpolylang_adapter_swift.dylib
            COMMENT "Building Swift adapter")
        message(STATUS "Swift: ENABLED")
    else()
        message(STATUS "Swift: swiftc not found — skipped")
    endif()
endif()

# ── C# NativeAOT ─────────────────────────────────────────────
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    find_program(DOTNET dotnet QUIET)
    if(DOTNET)
        add_custom_target(polylang_adapter_csharp
            COMMAND ${DOTNET} publish
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/csharp
                -r linux-x64 -p:NativeLib=Shared
                --output ${CMAKE_BINARY_DIR}/csharp_out
            COMMAND ${CMAKE_C_COMPILER} -shared -fPIC
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/csharp/polylang_cs_shim.c
                -L${CMAKE_BINARY_DIR}/csharp_out -lPolyLangCSharpAdapter
                -I${CMAKE_CURRENT_SOURCE_DIR}/include
                -o ${CMAKE_BINARY_DIR}/libpolylang_adapter_csharp.so
            COMMENT "Building C# NativeAOT adapter")
        message(STATUS "C#: ENABLED")
    else()
        message(STATUS "C#: dotnet not found — skipped")
    endif()
endif()

# ── Odin adapter ─────────────────────────────────────────────
option(POLYLANG_BUILD_ODIN_ADAPTER "Build the Odin adapter" ON)
option(POLYLANG_BUILD_ODIN_TESTS   "Build Odin adapter unit tests" OFF)

if(POLYLANG_BUILD_ODIN_ADAPTER)
    add_library(polylang_odin SHARED
        adapters/odin/polylang_odin_adapter.cpp
        adapters/odin/polylang_odin_shim.c)
    target_include_directories(polylang_odin PRIVATE include)
    target_compile_options(polylang_odin PRIVATE ${PL_HARDEN_FLAGS})
    if(ANDROID)
        target_link_libraries(polylang_odin PRIVATE dl)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(polylang_odin PRIVATE dl pthread)
    endif()
    set_target_properties(polylang_odin PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
        OUTPUT_NAME "polylang_odin" PREFIX "lib" SUFFIX ".so")
    install(TARGETS polylang_odin LIBRARY DESTINATION addons/polylang/bin)
    install(PROGRAMS
        adapters/odin/odin_build_pipeline.sh
        adapters/odin/odin_build_pipeline_android.sh
        adapters/odin/odin_build_pipeline.ps1
        DESTINATION addons/polylang/odin)
    message(STATUS "Odin adapter: ENABLED")
endif()

if(POLYLANG_BUILD_ODIN_TESTS)
    add_executable(polylang_odin_tests
        tests/odin_adapter_test.cpp
        adapters/odin/polylang_odin_adapter.cpp
        adapters/odin/polylang_odin_shim.c)
    target_include_directories(polylang_odin_tests PRIVATE include)
    target_compile_options(polylang_odin_tests PRIVATE ${PL_HARDEN_FLAGS})
    if(UNIX AND NOT APPLE)
        target_link_libraries(polylang_odin_tests PRIVATE dl pthread)
    endif()
    add_test(NAME OdinAdapterTests COMMAND polylang_odin_tests)
endif()

# ── Tests ────────────────────────────────────────────────────
option(POLYLANG_BUILD_TESTS         "Build standalone test binaries" OFF)
option(POLYLANG_BUILD_SANDBOX_TESTS "Build sandbox escape tests"     OFF)

if(POLYLANG_BUILD_TESTS)
    enable_testing()
    add_executable(polylang_sidecar_tests tests/sidecar_config_test.cpp)
    target_include_directories(polylang_sidecar_tests PRIVATE include)
    target_compile_options(polylang_sidecar_tests PRIVATE ${PL_HARDEN_FLAGS})
    add_test(NAME SidecarTests COMMAND polylang_sidecar_tests)
    message(STATUS "Sidecar tests: ENABLED")
endif()

if(POLYLANG_BUILD_SANDBOX_TESTS)
    add_executable(polylang_sandbox_escape_test tests/sandbox_escape_test.cpp)
    target_include_directories(polylang_sandbox_escape_test PRIVATE include)
    target_compile_options(polylang_sandbox_escape_test PRIVATE ${PL_HARDEN_FLAGS})
    if(UNIX AND NOT APPLE)
        target_link_libraries(polylang_sandbox_escape_test PRIVATE dl)
    endif()
    add_test(NAME SandboxEscapeTests COMMAND polylang_sandbox_escape_test)
    message(STATUS "Sandbox escape tests: ENABLED")
endif()

# ── Install ──────────────────────────────────────────────────
install(TARGETS polylang LIBRARY DESTINATION addons/polylang/bin)
install(FILES include/pl_adapter_vtable.h DESTINATION addons/polylang/include)

# ── Build summary ────────────────────────────────────────────
message(STATUS "")
message(STATUS "=== PolyLang v6.7 Build Configuration ===")
message(STATUS "  Platform : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Arch     : ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Lib type : ${PL_LIB_TYPE}")
message(STATUS "  Compiler : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ std  : ${CMAKE_CXX_STANDARD}")
message(STATUS "==========================================")
message(STATUS "")
