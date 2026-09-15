include_guard(GLOBAL)

function(qiven_target_defaults target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "qiven_target_defaults: unknown target '${target}'")
    endif()

    set(one_value_args SCOPE FOLDER)
    cmake_parse_arguments(QTD "" "${one_value_args}" "" ${ARGN})
    _qiven_require_clean_parse(QTD "qiven_target_defaults")

    get_target_property(_aliased_target ${target} ALIASED_TARGET)
    if(_aliased_target)
        message(FATAL_ERROR "qiven_target_defaults: '${target}' is an alias; configure the owning target")
    endif()
    get_target_property(_imported_target ${target} IMPORTED)
    if(_imported_target)
        message(FATAL_ERROR "qiven_target_defaults: '${target}' is imported; only first-party targets are supported")
    endif()

    get_target_property(target_type ${target} TYPE)
    if(NOT target_type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE|INTERFACE_LIBRARY)$")
        message(FATAL_ERROR "qiven_target_defaults: unsupported target type '${target_type}' for '${target}'")
    endif()

    if(NOT QTD_SCOPE)
        set(QTD_SCOPE PRIVATE)
    endif()
    if(NOT QTD_SCOPE MATCHES "^(PRIVATE|PUBLIC|INTERFACE)$")
        message(FATAL_ERROR "qiven_target_defaults: invalid SCOPE '${QTD_SCOPE}'")
    endif()
    if(target_type STREQUAL "INTERFACE_LIBRARY" AND NOT QTD_SCOPE STREQUAL "INTERFACE")
        message(FATAL_ERROR "qiven_target_defaults: interface targets require SCOPE INTERFACE")
    elseif(NOT target_type STREQUAL "INTERFACE_LIBRARY" AND QTD_SCOPE STREQUAL "INTERFACE")
        message(FATAL_ERROR "qiven_target_defaults: compiled targets cannot use SCOPE INTERFACE")
    endif()

    target_compile_features(${target} ${QTD_SCOPE} cxx_std_20)

    if(NOT target_type STREQUAL "INTERFACE_LIBRARY")
        set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang|GNU)$")
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        else()
            message(FATAL_ERROR
                "qiven_target_defaults: compiler '${CMAKE_CXX_COMPILER_ID}' / frontend "
                "'${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}' has no accepted Qiven warning policy")
        endif()
    endif()

    if(QTD_FOLDER)
        set_target_properties(${target} PROPERTIES FOLDER "${QTD_FOLDER}")
    endif()
endfunction()
