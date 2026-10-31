find_package(Boost REQUIRED)  # Boost.System is header-only since 1.69; no separate component on Boost 1.90

# tier1 and tier2 use POSIX calls (getpid, unlink, prctl); they run on Linux only.
if(NOT WIN32)
# The test drives a fake core with Boost.Asio local sockets; the library itself does not use Boost.
add_executable(tier1_roundtrip_test tier1_roundtrip_test.cpp)
target_link_libraries(tier1_roundtrip_test PRIVATE forever_module_sdk Boost::headers)
# the version in CMakeLists.txt and kSdkPackageVersion are one number; the test compares them
target_compile_definitions(tier1_roundtrip_test PRIVATE FOREVER_SDK_CMAKE_VERSION="${PROJECT_VERSION}")
add_test(NAME tier1_roundtrip COMMAND tier1_roundtrip_test)

add_executable(tier2_attach_test tier2_attach_test.cpp)
target_link_libraries(tier2_attach_test PRIVATE forever_module_sdk Boost::headers)
add_test(NAME tier2_attach COMMAND tier2_attach_test)
endif()

add_executable(module_client_test module_client_test.cpp)
target_link_libraries(module_client_test PRIVATE forever_module_sdk Boost::headers)
add_test(NAME module_client_tier2 COMMAND module_client_test)
