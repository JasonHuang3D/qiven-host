# Intentionally no include_guard: every snapshot must participate in the API handshake.
set(_qiven_requested_build_api "1")
set(_qiven_requested_snapshot "0.2.0")

get_property(_qiven_build_api_set GLOBAL PROPERTY QIVEN_BUILD_SYSTEM_API SET)
if(_qiven_build_api_set)
    get_property(_qiven_active_build_api GLOBAL PROPERTY QIVEN_BUILD_SYSTEM_API)
    get_property(_qiven_active_snapshot GLOBAL PROPERTY QIVEN_BUILD_SYSTEM_SNAPSHOT)
    get_property(_qiven_active_source GLOBAL PROPERTY QIVEN_BUILD_SYSTEM_SOURCE)
    if(NOT _qiven_active_build_api STREQUAL _qiven_requested_build_api)
        message(FATAL_ERROR
            "Incompatible Qiven build-system APIs in one configure graph: active=${_qiven_active_build_api} "
            "(${_qiven_active_snapshot} from ${_qiven_active_source}), requested=${_qiven_requested_build_api} "
            "(${_qiven_requested_snapshot} from ${CMAKE_CURRENT_LIST_DIR})")
    endif()
    return()
endif()

set_property(GLOBAL PROPERTY QIVEN_BUILD_SYSTEM_API "${_qiven_requested_build_api}")
set_property(GLOBAL PROPERTY QIVEN_BUILD_SYSTEM_SNAPSHOT "${_qiven_requested_snapshot}")
set_property(GLOBAL PROPERTY QIVEN_BUILD_SYSTEM_SOURCE "${CMAKE_CURRENT_LIST_DIR}")

include(CMakeParseArguments)

function(_qiven_require_clean_parse prefix command_name)
    if(DEFINED ${prefix}_UNPARSED_ARGUMENTS AND NOT "${${prefix}_UNPARSED_ARGUMENTS}" STREQUAL "")
        message(FATAL_ERROR "${command_name}: unknown arguments: ${${prefix}_UNPARSED_ARGUMENTS}")
    endif()
    if(DEFINED ${prefix}_KEYWORDS_MISSING_VALUES AND NOT "${${prefix}_KEYWORDS_MISSING_VALUES}" STREQUAL "")
        message(FATAL_ERROR "${command_name}: missing values for: ${${prefix}_KEYWORDS_MISSING_VALUES}")
    endif()
endfunction()

function(_qiven_require_variable_name value context)
    if(NOT "${value}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "${context}: invalid CMake variable name '${value}'")
    endif()
endfunction()

function(_qiven_require_safe_relative_path value context)
    set(_candidate "${value}")
    cmake_path(IS_ABSOLUTE _candidate _is_absolute)
    if("${value}" STREQUAL "" OR _is_absolute OR "${value}" MATCHES "(^|[/\\])\.\.($|[/\\])")
        message(FATAL_ERROR "${context}: expected a safe relative path, got '${value}'")
    endif()
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/QivenProject.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/QivenTarget.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/QivenDependency.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/QivenTest.cmake")
