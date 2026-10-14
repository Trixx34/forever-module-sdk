find_package(Boost REQUIRED)  # Boost.System is header-only since 1.69; no separate component on Boost 1.90

# The test drives a fake core with Boost.Asio local sockets; the library itself does not use Boost.
add_executable(tier1_roundtrip_test tier1_roundtrip_test.cpp)
target_link_libraries(tier1_roundtrip_test PRIVATE forever_module_sdk Boost::headers)
add_test(NAME tier1_roundtrip COMMAND tier1_roundtrip_test)

add_executable(tier2_attach_test tier2_attach_test.cpp)
target_link_libraries(tier2_attach_test PRIVATE forever_module_sdk Boost::headers)
add_test(NAME tier2_attach COMMAND tier2_attach_test)
