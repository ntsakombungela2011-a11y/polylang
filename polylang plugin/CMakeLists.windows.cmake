# Windows-only cmake overlay for PolyLang plugin

function(windows_plugin_fixes)
    if(TARGET polylang_adapter_js)
        target_compile_definitions(polylang_adapter_js PRIVATE _USE_MATH_DEFINES)
    endif()
    if(TARGET polylang_adapter_ts)
        target_compile_definitions(polylang_adapter_ts PRIVATE _USE_MATH_DEFINES)
    endif()
endfunction()
cmake_language(DEFER CALL windows_plugin_fixes)
