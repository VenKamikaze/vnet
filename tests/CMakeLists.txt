# Plain-C smoke test: no GoogleTest, so it also runs in the MinGW (Wine) build.
add_executable(vnet_smoke smoke.c)
target_link_libraries(vnet_smoke PRIVATE vnet)
add_test(NAME vnet_smoke COMMAND vnet_smoke)
set_tests_properties(vnet_smoke PROPERTIES TIMEOUT 60 ENVIRONMENT "VNET_IMPAIR=")

if(NOT VNET_BUILD_TESTS)
    return()
endif()

# (C++ is enabled by the top-level CMakeLists.)
# GoogleTest: prefer the system package (libgtest-dev); otherwise fetch it
# from git (never an archive URL - archive downloads can be blocked).
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GoogleTest)

add_executable(vnet_tests
    test_addr.cpp
    test_c_header.cpp
    c_header_check.c
    test_impair.cpp
    test_loopback.cpp
    test_udp.cpp
    test_lifecycle.cpp
    test_relay_client.cpp
    test_relay_ws_style.cpp
    test_relay_fallback.cpp
    test_api_polish.cpp
    test_inbox.cpp
    test_util.h
    relay_test_util.h)
target_link_libraries(vnet_tests PRIVATE vnet GTest::gtest GTest::gtest_main)
# The relay tests look at the protocol constants and the internal test seam.
target_include_directories(vnet_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(vnet_tests PRIVATE VNET_PROJECT_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vnet_tests PRIVATE -Wall -Wextra)
endif()
# Allocation-counting test: needs GNU ld's --wrap (Linux).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(vnet_tests PRIVATE alloc_counter.cpp)
    target_compile_definitions(vnet_tests PRIVATE VNET_TEST_COUNT_ALLOCS)
    target_link_options(vnet_tests PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
    # IPv6 policy test seam (see ipv6_shim.cpp).
    target_sources(vnet_tests PRIVATE ipv6_shim.cpp)
    target_compile_definitions(vnet_tests PRIVATE VNET_TEST_FAKE_IPV6)
    # Random source failure test seam (see entropy_shim.cpp).
    target_sources(vnet_tests PRIVATE entropy_shim.cpp)
    target_compile_definitions(vnet_tests PRIVATE VNET_TEST_FAKE_ENTROPY)
    target_link_options(vnet_tests PRIVATE -Wl,--wrap=getentropy)
    target_link_options(vnet_tests PRIVATE -Wl,--wrap=socket -Wl,--wrap=setsockopt
                        -Wl,--wrap=bind -Wl,--wrap=close -Wl,--wrap=sendto)
endif()
# Relay: build the Go server next to the tests, run its own Go tests, and
# compile the integration tests that start it.
if(VNET_BUILD_RELAY)
    if(NOT VNET_GO_EXECUTABLE)
        message(FATAL_ERROR "VNET_BUILD_RELAY=ON needs Go (set VNET_GO_EXECUTABLE or put go on PATH)")
    endif()
    set(VNET_RELAY_BIN ${CMAKE_BINARY_DIR}/vnet-relay)
    add_custom_target(vnet-relay ALL
        COMMAND ${CMAKE_COMMAND} -E env CGO_ENABLED=0 ${VNET_GO_EXECUTABLE} build -trimpath
                -o ${VNET_RELAY_BIN} ./cmd/vnet-relay
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/relay
        COMMENT "Building the Go relay"
        VERBATIM)
    add_dependencies(vnet_tests vnet-relay)
    target_sources(vnet_tests PRIVATE test_relay_integration.cpp)
    target_compile_definitions(vnet_tests PRIVATE VNET_RELAY_BIN="${VNET_RELAY_BIN}")
    add_test(NAME relay_go_tests
        COMMAND ${VNET_GO_EXECUTABLE} test -count=1 ./...
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/relay)
    set_tests_properties(relay_go_tests PROPERTIES TIMEOUT 300)
endif()
gtest_discover_tests(vnet_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
