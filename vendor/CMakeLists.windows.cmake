# Windows-only cmake overlay for PolyLang vendor

# Adds RUNTIME DESTINATION lib to all SHARED library install rules
macro(install)
    set(_args "${ARGN}")
    list(FIND _args "TARGETS" _idx)
    if(_idx EQUAL 0)
        _install(${ARGN} RUNTIME DESTINATION lib)
    else()
        _install(${ARGN})
    endif()
endmacro()

function(windows_vendor_fixes)
    if(TARGET quickjs)
        # Adds _USE_MATH_DEFINES so quickjs.h can find NAN on MSVC
        target_compile_definitions(quickjs PUBLIC _USE_MATH_DEFINES)
    endif()
endfunction()
cmake_language(DEFER CALL windows_vendor_fixes)
