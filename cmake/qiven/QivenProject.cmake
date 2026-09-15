include_guard(GLOBAL)

function(qiven_project_defaults)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
        message(FATAL_ERROR "Qiven repositories require out-of-source builds")
    endif()

    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

    if(MSVC)
        set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")
    endif()
endfunction()
