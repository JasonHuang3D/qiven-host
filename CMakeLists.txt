cmake_minimum_required(VERSION 3.28)
project(qiven-host VERSION 0.1.0 LANGUAGES CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qiven")
include(QivenBuild)
qiven_project_defaults()

option(QIVEN_HOST_BUILD_TESTS "Build qiven-host tests" ON)

add_library(qiven-host-core STATIC src/authority.cpp src/fence_store.cpp)
target_include_directories(qiven-host-core PUBLIC include)
qiven_target_defaults(qiven-host-core SCOPE PUBLIC FOLDER "Libraries")

add_executable(qiven-host app/main.cpp)
add_executable(qiven::host ALIAS qiven-host)
qiven_target_defaults(qiven-host SCOPE PRIVATE FOLDER "Applications")

if(QIVEN_HOST_BUILD_TESTS)
    enable_testing()
    add_executable(qiven-host-core-tests tests/authority_tests.cpp)
    target_link_libraries(qiven-host-core-tests PRIVATE qiven-host-core)
    qiven_test_target_defaults(qiven-host-core-tests)
    qiven_register_test(NAME qiven-host-core-tests TARGET qiven-host-core-tests)
endif()
