cmake_minimum_required(VERSION 3.28)
project(qiven-host VERSION 0.1.0 LANGUAGES CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qiven")
include(QivenBuild)
qiven_project_defaults()

option(QIVEN_HOST_BUILD_TESTS "Build qiven-host tests" ON)

add_executable(qiven-host app/main.cpp)
add_executable(qiven::host ALIAS qiven-host)
qiven_target_defaults(qiven-host SCOPE PRIVATE FOLDER "Applications")

if(QIVEN_HOST_BUILD_TESTS)
    enable_testing()
endif()
