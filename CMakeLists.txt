cmake_minimum_required(VERSION 3.16)

project(vnet
    VERSION 0.3.0
    DESCRIPTION "Virtual UDP for games: a small non-blocking datagram API with pluggable backends"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(VNET_IS_TOP_LEVEL ON)
else()
    set(VNET_IS_TOP_LEVEL OFF)
endif()

option(VNET_BUILD_TESTS "Build the vnet tests: GoogleTest suite plus the C smoke test" ${VNET_IS_TOP_LEVEL})
option(VNET_BUILD_SMOKE "Build only the dependency-free C smoke test (e.g. MinGW cross builds)" OFF)
option(VNET_INSTALL "Generate install rules for vnet" ${VNET_IS_TOP_LEVEL})
# The relay server (Go) and the tests that need it. On by default when Go is
# found; the tests that start the relay are only built then (never skipped).
find_program(VNET_GO_EXECUTABLE go HINTS /usr/local/go/bin)
if(VNET_GO_EXECUTABLE AND VNET_IS_TOP_LEVEL AND NOT CMAKE_CROSSCOMPILING)
    set(_vnet_relay_default ON)
else()
    set(_vnet_relay_default OFF)
endif()
option(VNET_BUILD_RELAY "Build the Go relay (needs Go) and run its tests and the relay integration tests" ${_vnet_relay_default})
option(VNET_BUILD_EXAMPLES "Build the example programs (examples/)" ${VNET_IS_TOP_LEVEL})
option(VNET_WARNINGS_AS_ERRORS "Treat compiler warnings in vnet's own sources as errors" OFF)

include(GNUInstallDirs)

add_library(vnet STATIC
    src/addr.c
    src/impair.c
    src/loopback.c
    src/random.c
    src/relay_auto.c
    src/relay_client.c
    src/relay_udp.c
    src/relay_ws.c
    src/relay_wt.c
    src/udp.c
    src/vnet.c)
add_library(vnet::vnet ALIAS vnet)

target_include_directories(vnet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(vnet PRIVATE c_std_11)

# Feature-test macros so that -std=c11 (no GNU extensions) still sees POSIX
# sockets and clock_gettime.
if(APPLE)
    target_compile_definitions(vnet PRIVATE _DARWIN_C_SOURCE)
elseif(UNIX)
    target_compile_definitions(vnet PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(EMSCRIPTEN)
    # The browser WebSocket transport (VNET_BACKEND_RELAY_WS) uses Emscripten's
    # <emscripten/websocket.h>. It needs no ports. The WebTransport transport
    # (VNET_BACKEND_RELAY_WT / _AUTO) is EM_JS code inside relay_wt.c: no extra
    # library, flag or file.
    target_link_libraries(vnet PUBLIC websocket.js)
endif()

if(WIN32)
    target_link_libraries(vnet PUBLIC ws2_32)
    # inet/getaddrinfo-era Winsock API (IPV6_V6ONLY, in6addr_any) needs Vista+.
    target_compile_definitions(vnet PRIVATE _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vnet PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    # Developer-build guard against deprecated libc calls. Not for consumers
    # (their flags and libc macros are none of our business), and POSIX only
    # (MinGW's own headers use strcpy). SHELL: stops CMake de-duplicating the
    # option against a consumer's own "-include".
    if(VNET_IS_TOP_LEVEL AND NOT WIN32)
        target_compile_options(vnet PRIVATE
            "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/banned.h")
    endif()
    if(VNET_WARNINGS_AS_ERRORS)
        target_compile_options(vnet PRIVATE -Werror)
    endif()
elseif(MSVC)
    target_compile_options(vnet PRIVATE /W4)
    if(VNET_WARNINGS_AS_ERRORS)
        target_compile_options(vnet PRIVATE /WX)
    endif()
endif()

set_target_properties(vnet PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)

if(VNET_BUILD_EXAMPLES)
    # relay_peer: two players talking through the relay. Native, it uses the
    # relay over UDP; under Emscripten it becomes a web page (relay_peer.html +
    # .js + .wasm) that uses the WebSocket backend.
    add_executable(relay_peer examples/relay_peer.c)
    target_link_libraries(relay_peer PRIVATE vnet)
    if(EMSCRIPTEN)
        set_target_properties(relay_peer PROPERTIES SUFFIX ".html")
        target_link_options(relay_peer PRIVATE
            "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/examples/shell.html")
    endif()
endif()

if(VNET_BUILD_TESTS OR VNET_BUILD_SMOKE)
    if(VNET_BUILD_TESTS)
        enable_language(CXX)
    endif()
    enable_testing()
    add_subdirectory(tests)
endif()

if(VNET_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS vnet
        EXPORT vnetTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES include/vnet.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT vnetTargets
        FILE vnetTargets.cmake
        NAMESPACE vnet::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnet)

    configure_package_config_file(
        cmake/vnetConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/vnetConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnet)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/vnetConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/vnetConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/vnetConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vnet)
endif()
