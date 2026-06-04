# Minimal MQTT-over-TLS bridge probe. Self-contained: OpenSSL + Winsock only,
# NO libslic3r / bambu_virtual_client (so it links without the whole slicer).
add_executable(bridge_test_cli bridge_test_cli.cpp)

if (NOT TARGET OpenSSL::SSL)
    find_package(OpenSSL REQUIRED)
endif ()
target_link_libraries(bridge_test_cli PRIVATE OpenSSL::SSL OpenSSL::Crypto)

set_target_properties(bridge_test_cli PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if (WIN32)
    target_link_libraries(bridge_test_cli PRIVATE ws2_32 crypt32 iphlpapi)
    target_compile_definitions(bridge_test_cli PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if (MSVC)
        set_target_properties(bridge_test_cli PROPERTIES VS_GLOBAL_VcpkgEnabled false)
    endif ()
endif ()
