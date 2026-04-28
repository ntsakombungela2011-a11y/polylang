# Windows-only cmake overlay for PolyLang vendor

function(windows_vendor_fixes)
    if(TARGET quickjs)
        # Adds _USE_MATH_DEFINES so quickjs.h can find NAN on MSVC
        target_compile_definitions(quickjs PUBLIC _USE_MATH_DEFINES)
    endif()

    # Get all targets in the current directory
    get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(tgt IN LISTS targets)
        get_target_property(type ${tgt} TYPE)
        if(type STREQUAL "SHARED_LIBRARY")
            # Adds RUNTIME DESTINATION lib to all SHARED library install rules
            install(TARGETS ${tgt} RUNTIME DESTINATION lib)
        endif()
    endforeach()
endfunction()
cmake_language(DEFER CALL windows_vendor_fixes)
