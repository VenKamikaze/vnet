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
    test_util.h)
target_link_libraries(vnet_tests PRIVATE vnet GTest::gtest GTest::gtest_main)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vnet_tests PRIVATE -Wall -Wextra)
endif()
# Allocation-counting test: needs GNU ld's --wrap (Linux).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(vnet_tests PRIVATE alloc_counter.cpp)
    target_compile_definitions(vnet_tests PRIVATE VNET_TEST_COUNT_ALLOCS)
    target_link_options(vnet_tests PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
endif()
gtest_discover_tests(vnet_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 60)
