find_package(Boost REQUIRED COMPONENTS system)

# The test drives a fake core with Boost.Asio local sockets; the library itself does not use Boost.
add_executable(tier1_roundtrip_test tier1_roundtrip_test.cpp)
target_link_libraries(tier1_roundtrip_test PRIVATE forever_module_sdk Boost::system)
add_test(NAME tier1_roundtrip COMMAND tier1_roundtrip_test)
