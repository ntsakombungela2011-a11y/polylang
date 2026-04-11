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
    message(FATAL_ERROR
        "godot-cpp submodule missing.\n"
        "Run: git submodule update --init --recursive")
endif()
add_subdirectory(godot-cpp)

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
find_library(QUICKJS_LIB NAMES quickjs QUIET)
find_path(QUICKJS_INCLUDE NAMES quickjs/quickjs.h QUIET)
if(QUICKJS_LIB AND QUICKJS_INCLUDE)
    pl_adapter(polylang_adapter_js  adapters/javascript/polylang_js_adapter.cpp)
    target_link_libraries(polylang_adapter_js  PRIVATE ${QUICKJS_LIB})
    target_include_directories(polylang_adapter_js  PRIVATE ${QUICKJS_INCLUDE})
    pl_adapter(polylang_adapter_ts  adapters/typescript/polylang_ts_adapter.cpp)
    target_link_libraries(polylang_adapter_ts  PRIVATE ${QUICKJS_LIB})
    target_include_directories(polylang_adapter_ts  PRIVATE ${QUICKJS_INCLUDE})
    message(STATUS "JS/TS (QuickJS): ENABLED")
else()
    message(STATUS "QuickJS: NOT FOUND — JS/TS skipped")
endif()

# ── Squirrel ─────────────────────────────────────────────────
find_library(SQUIRREL_LIB NAMES squirrel QUIET)
find_path(SQUIRREL_INC NAMES squirrel.h QUIET)
if(SQUIRREL_LIB AND SQUIRREL_INC)
    pl_adapter(polylang_adapter_squirrel adapters/squirrel/polylang_squirrel_adapter.cpp)
    target_link_libraries(polylang_adapter_squirrel PRIVATE ${SQUIRREL_LIB})
    target_include_directories(polylang_adapter_squirrel PRIVATE ${SQUIRREL_INC})
    message(STATUS "Squirrel: ENABLED")
else()
    message(STATUS "Squirrel: NOT FOUND — skipped")
endif()

# ── Wren ─────────────────────────────────────────────────────
find_library(WREN_LIB NAMES wren QUIET)
find_path(WREN_INC NAMES wren.h QUIET)
if(WREN_LIB AND WREN_INC)
    pl_adapter(polylang_adapter_wren adapters/wren/polylang_wren_adapter.cpp)
    target_link_libraries(polylang_adapter_wren PRIVATE ${WREN_LIB})
    target_include_directories(polylang_adapter_wren PRIVATE ${WREN_INC})
    message(STATUS "Wren: ENABLED")
else()
    message(STATUS "Wren: NOT FOUND — skipped")
endif()

# ── AngelScript ──────────────────────────────────────────────
find_library(AS_LIB NAMES angelscript QUIET)
find_path(AS_INC NAMES angelscript.h QUIET)
if(AS_LIB AND AS_INC)
    pl_adapter(polylang_adapter_as adapters/angelscript/polylang_angelscript_adapter.cpp)
    target_link_libraries(polylang_adapter_as PRIVATE ${AS_LIB})
    target_include_directories(polylang_adapter_as PRIVATE ${AS_INC})
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
    set(RUST_LIB "${CMAKE_SOURCE_DIR}/adapters/rust/target/release/polylang_rust.dll")
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