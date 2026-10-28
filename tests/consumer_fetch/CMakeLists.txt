# Standalone project used by CI to check consuming vnet through FetchContent
# with GIT_REPOSITORY (no archive download).
#   cmake -S tests/consumer_fetch -B build -DVNET_REPO=file:///path/to/vnet -DVNET_REV=<commit>
cmake_minimum_required(VERSION 3.16)
project(vnet_consumer_fetch C)
set(VNET_REPO "https://github.com/VenKamikaze/vnet.git" CACHE STRING "vnet git URL")
set(VNET_REV "claude" CACHE STRING "vnet branch, tag or commit")
include(FetchContent)
FetchContent_Declare(vnet GIT_REPOSITORY ${VNET_REPO} GIT_TAG ${VNET_REV})
FetchContent_MakeAvailable(vnet)
add_executable(consumer ../consumer/main.c)
target_link_libraries(consumer PRIVATE vnet::vnet)
