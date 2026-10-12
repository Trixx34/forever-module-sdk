cmake_minimum_required(VERSION 3.20)
project(minimal_module LANGUAGES CXX)

# Smallest possible skeleton using forever_module_sdk -- connect, handshake, one OnNotify hook,
# no Tier 2. This is the starting point the core repo's "Hello Module" walkthrough template
# (doc/module-system/05-phase3-build-discovery-design.md §5) forks from.
#
# SKELETON ONLY: since ModuleClient's methods are stubs (see src/module_client.cpp in the SDK),
# this example compiles and links but does not actually connect to anything yet.

add_executable(minimal_module main.cpp)
target_link_libraries(minimal_module PRIVATE forever_module_sdk)
