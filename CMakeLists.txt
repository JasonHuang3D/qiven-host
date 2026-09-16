cmake_minimum_required(VERSION 3.28)
project(qiven-host VERSION 0.1.0 LANGUAGES CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qiven")
include(QivenBuild)
qiven_project_defaults()

option(QIVEN_HOST_BUILD_TESTS "Build qiven-host tests" ON)

add_library(qiven-host-core STATIC
    src/authority.cpp
    src/fence_store.cpp
    src/bounded_journal.cpp
    src/broker_instance_guard.cpp
    src/authority_broker.cpp
    src/protocol_codec.cpp
    src/local_pipe_transport.cpp
)
target_include_directories(qiven-host-core PUBLIC include)
qiven_target_defaults(qiven-host-core SCOPE PUBLIC FOLDER "Libraries")
if(WIN32)
    target_link_libraries(qiven-host-core PRIVATE bcrypt advapi32)
endif()

add_executable(qiven-host app/main.cpp)
add_executable(qiven::host ALIAS qiven-host)
qiven_target_defaults(qiven-host SCOPE PRIVATE FOLDER "Applications")

if(QIVEN_HOST_BUILD_TESTS)
    enable_testing()

    add_executable(qiven-host-core-tests tests/authority_tests.cpp)
    target_link_libraries(qiven-host-core-tests PRIVATE qiven-host-core)
    qiven_test_target_defaults(qiven-host-core-tests)
    qiven_register_test(NAME qiven-host-core-tests TARGET qiven-host-core-tests)

    add_executable(qiven-host-protocol-codec-tests tests/protocol_codec_tests.cpp)
    target_link_libraries(qiven-host-protocol-codec-tests PRIVATE qiven-host-core)
    qiven_test_target_defaults(qiven-host-protocol-codec-tests)
    qiven_register_test(NAME qiven-host-protocol-codec-tests TARGET qiven-host-protocol-codec-tests)

    if(WIN32)
        add_executable(qiven-host-fence-store-tests tests/fence_store_tests.cpp)
        target_link_libraries(qiven-host-fence-store-tests PRIVATE qiven-host-core)
        qiven_test_target_defaults(qiven-host-fence-store-tests)
        qiven_register_test(NAME qiven-host-fence-store-tests TARGET qiven-host-fence-store-tests)

        add_executable(qiven-host-bounded-journal-tests tests/bounded_journal_tests.cpp)
        target_link_libraries(qiven-host-bounded-journal-tests PRIVATE qiven-host-core)
        qiven_test_target_defaults(qiven-host-bounded-journal-tests)
        qiven_register_test(NAME qiven-host-bounded-journal-tests TARGET qiven-host-bounded-journal-tests)

        add_executable(qiven-host-broker-instance-tests tests/broker_instance_guard_tests.cpp)
        target_link_libraries(qiven-host-broker-instance-tests PRIVATE qiven-host-core)
        qiven_test_target_defaults(qiven-host-broker-instance-tests)
        qiven_register_test(NAME qiven-host-broker-instance-tests TARGET qiven-host-broker-instance-tests)

        add_executable(qiven-host-authority-broker-tests tests/authority_broker_tests.cpp)
        target_link_libraries(qiven-host-authority-broker-tests PRIVATE qiven-host-core)
        qiven_test_target_defaults(qiven-host-authority-broker-tests)
        qiven_register_test(NAME qiven-host-authority-broker-tests TARGET qiven-host-authority-broker-tests)

        add_executable(qiven-host-local-pipe-tests tests/local_pipe_transport_tests.cpp)
        target_link_libraries(qiven-host-local-pipe-tests PRIVATE qiven-host-core advapi32)
        qiven_test_target_defaults(qiven-host-local-pipe-tests)
        qiven_register_test(NAME qiven-host-local-pipe-tests TARGET qiven-host-local-pipe-tests)
        set_tests_properties(qiven-host-local-pipe-tests PROPERTIES TIMEOUT 20)
    endif()
endif()
