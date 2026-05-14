# Bambu Bridge — phase 12 E2E tests (real-printer suite).
#
# Built only when BAMBU_BRIDGE_E2E=ON (see ../CMakeLists.txt's gate).
# When BAMBU_BRIDGE_E2E_PRINTERS is empty at configure time, the test
# binaries still build but ctest will run them and they will exit 77
# (skipped) because no matching model@ip endpoint is found in the env.
#
# Each per-model test reuses three test-only support libraries from
# tests/bridge/support/ (MqttTestClient, FtpsTestClient, RtspTestClient
# — the RTSP client is built but the per-model flow doesn't use it
# in phase 12; carrying it keeps the link symmetric with the other
# integration tests for future camera-aware E2Es).

set(_e2e_support_dir "${CMAKE_CURRENT_SOURCE_DIR}/../support")
set(_e2e_fixtures    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# Common compile + link of the harness machinery.
set(_e2e_common_srcs
    E2EHarness.cpp
    E2EHarness.hpp
    E2EPrinterClient.cpp
    E2EPrinterClient.hpp
    E2EFixtureLoader.cpp
    E2EFixtureLoader.hpp
    e2e_common.cpp
    e2e_common.hpp
    "${_e2e_support_dir}/MqttTestClient.cpp"
    "${_e2e_support_dir}/MqttTestClient.hpp"
    "${_e2e_support_dir}/FtpsTestClient.cpp"
    "${_e2e_support_dir}/FtpsTestClient.hpp"
)

# The E2E suite spawns BambuStudio with `--bridge-only` — there is no
# standalone bridge daemon binary (the proprietary `bambu_networking`
# plugin fingerprints its host process). The test harness reads the
# slicer path from $BAMBU_BRIDGE_E2E_BAMBUSTUDIO; CMake doesn't know
# where the user's BambuStudio build sits, so we don't bake a default —
# leaving it unset makes the harness fall back to a $PATH lookup of
# `BambuStudio`, and the per-model test exits 77 (skip) if not found.

function(add_e2e_test model)
    add_executable(e2e_${model}_print
        e2e_${model}_print.cpp
        ${_e2e_common_srcs}
    )
    target_link_libraries(e2e_${model}_print PRIVATE
        bambu_bridge
        OpenSSL::SSL
        OpenSSL::Crypto
    )
    if (TARGET Threads::Threads)
        target_link_libraries(e2e_${model}_print PRIVATE Threads::Threads)
    endif ()
    target_include_directories(e2e_${model}_print PRIVATE
        "${_e2e_support_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_definitions(e2e_${model}_print PRIVATE
        BAMBU_BRIDGE_E2E_FIXTURE_DIR="${_e2e_fixtures}"
    )
    set_target_properties(e2e_${model}_print PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    # Don't add_dependencies on BambuStudio — it lives in a parent build
    # tree that the standalone bridge build doesn't know about.

    add_test(NAME e2e_${model}_print COMMAND e2e_${model}_print)
    set_tests_properties(e2e_${model}_print PROPERTIES
        LABELS         "bridge-e2e"
        SKIP_RETURN_CODE 77
        TIMEOUT        240
        ENVIRONMENT
            "BAMBU_BRIDGE_E2E_PRINTERS=${BAMBU_BRIDGE_E2E_PRINTERS};BAMBU_BRIDGE_E2E_FIXTURE_DIR=${_e2e_fixtures}"
    )
endfunction()

foreach(model IN ITEMS h2s a1 a1mini h2)
    add_e2e_test(${model})
endforeach()
