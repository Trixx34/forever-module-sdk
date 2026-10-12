cmake_minimum_required(VERSION 3.20)
project(forever_module_sdk VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SKELETON ONLY -- see doc/module-system/11-sdk-scope.md "Open items deferred from this scoping
# pass" in the core repo: exact Protobuf/Boost minimum versions, header-only-vs-static-lib tradeoff,
# and CI/release process are all still open and not decided here.

find_package(Protobuf REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

include(cmake/GenerateProto.cmake)

forever_module_generate_proto(
    PROTO_FILES
        proto/handshake.proto
        proto/events.proto
        proto/control.proto
        proto/envelope.proto
    OUT_SOURCES_VAR GENERATED_PROTO_SOURCES
    OUT_HEADERS_VAR GENERATED_PROTO_HEADERS
)

add_library(forever_module_sdk STATIC
    src/module_client.cpp
    src/tier2_session.cpp
    ${GENERATED_PROTO_SOURCES}
)

target_include_directories(forever_module_sdk
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(forever_module_sdk
    PUBLIC
        protobuf::libprotobuf
        Boost::system
)

option(FOREVER_MODULE_SDK_BUILD_EXAMPLES "Build the example modules shipped with the SDK" ON)
if(FOREVER_MODULE_SDK_BUILD_EXAMPLES)
    add_subdirectory(examples/minimal-module)
endif()
