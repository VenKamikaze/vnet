# Standalone project used by CI to check that an INSTALLED vnet is usable via
# find_package(vnet). Not part of the main build.
cmake_minimum_required(VERSION 3.16)
project(vnet_consumer C)
find_package(vnet 0.1 REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE vnet::vnet)
