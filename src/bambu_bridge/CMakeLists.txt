# Bambu Bridge — module CMake.
#
# Phase 0 produced a STATIC library `bambu_bridge` with just BridgeService.
# Phase 1 adds CloudInventory and a small `bridge_cli` executable.
# Phase 2 adds tls/CertFactory (per-device self-signed TLS certs) and
#         picks up an OpenSSL dependency. The find_package() call is at
#         REQUIRED severity so a missing libssl/libcrypto fails at cmake
#         time with a clear message instead of at link time.
# Phase 3 adds server/SsdpResponder (pure POSIX sockets, Linux-only for
#         now). Also gains an `announce` subcommand on bridge_cli.
# Phase 7 adds server/FtpsServer (implicit-TLS FTP on port 990 for slicer
#         .3mf uploads) and the IUploadSink interface plus three sinks
#         (Null/Lan/Cloud). Plus an `ftps` subcommand on bridge_cli that
#         logs received uploads to /tmp/bridge_uploads/.
# Phase 10 adds headless/BridgeApp + headless/SignalHandler that are
#         consumed by BambuStudio's `--bridge-only` entry point. The
#         multi-device orchestrator walks the cloud inventory and stands
#         up the full SSDP/MQTT/FTPS/RTSP stack for every cloud-bound
#         device with one shared plugin handle. SIGINT/SIGTERM trigger a
#         clean RTSP→FTPS→MQTT→SSDP teardown. There is no standalone
#         daemon binary: the proprietary `bambu_networking` plugin
#         fingerprints its host process and only operates when loaded
#         by BambuStudio.
#
# Standalone configure (no parent project) is preserved so the bridge tree
# can be smoke-built without the full BambuStudio dep stack. Top-level Boost
# 1.83 etc. is not required here.

cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED PROJECT_NAME)
    # Standalone build (no parent project). Bootstrap just enough to produce
    # the library + tests + bridge_cli without dragging in the rest of Slic3r.
    project(bambu_bridge_standalone CXX)
    enable_testing()
    set(BAMBU_BRIDGE_STANDALONE TRUE)
else ()
    set(BAMBU_BRIDGE_STANDALONE FALSE)
endif ()

# OpenSSL is mandatory from phase 2 onward (CertFactory needs it; phases
# 4+ will reuse it for the MQTT broker's TLS termination). Fail loudly at
# configure time so missing devel headers don't show up as a link error
# half-way through the build.
find_package(OpenSSL REQUIRED)

add_library(bambu_bridge STATIC
    BridgeService.cpp
    BridgeService.hpp
    BambuNetworkingPluginHandle.cpp
    BambuNetworkingPluginHandle.hpp
    BambuSourceHandle.cpp
    BambuSourceHandle.hpp
    CloudInventory.cpp
    CloudInventory.hpp
    tls/CertFactory.cpp
    tls/CertFactory.hpp
    server/SsdpResponder.cpp
    server/SsdpResponder.hpp
    server/SsdpListener.cpp
    server/SsdpListener.hpp
    server/MqttFraming.cpp
    server/MqttFraming.hpp
    server/MqttBroker.cpp
    server/MqttBroker.hpp
    server/IUplink.hpp
    server/FtpsServer.cpp
    server/FtpsServer.hpp
    server/IUploadSink.hpp
    server/ICameraSource.hpp
    server/RtspServer.cpp
    server/RtspServer.hpp
    server/VirtualTunnelServer.cpp
    server/VirtualTunnelServer.hpp
    router/NullUplink.cpp
    router/NullUplink.hpp
    router/LanUplink.cpp
    router/LanUplink.hpp
    router/CloudUplink.cpp
    router/CloudUplink.hpp
    router/NullUploadSink.cpp
    router/NullUploadSink.hpp
    router/LanUploadSink.cpp
    router/LanUploadSink.hpp
    router/CloudUploadSink.cpp
    router/CloudUploadSink.hpp
    router/UploadSpool.cpp
    router/UploadSpool.hpp
    router/NullCameraSource.cpp
    router/NullCameraSource.hpp
    router/LanCameraSource.cpp
    router/LanCameraSource.hpp
    router/CloudCameraSource.cpp
    router/CloudCameraSource.hpp
    router/UplinkHealth.cpp
    router/UplinkHealth.hpp
    router/SessionRouter.cpp
    router/SessionRouter.hpp
    router/UploadSinkRouter.cpp
    router/UploadSinkRouter.hpp
    router/CameraSourceRouter.cpp
    router/CameraSourceRouter.hpp
    headless/BridgeApp.cpp
    headless/BridgeApp.hpp
    headless/BridgeAppCliArgs.cpp
    headless/BridgeAppCliArgs.hpp
    headless/SignalHandler.cpp
    headless/SignalHandler.hpp
)

target_link_libraries(bambu_bridge PUBLIC OpenSSL::SSL OpenSSL::Crypto)

target_include_directories(bambu_bridge
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        # third_party/ holds vendored header-only deps (nlohmann/json). We
        # keep it PRIVATE so callers don't accidentally pin to our vendored
        # copy if the parent project ships its own.
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

set_target_properties(bambu_bridge PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if (UNIX AND NOT APPLE)
    # libdl for dlopen/dlsym on Linux.
    target_link_libraries(bambu_bridge PUBLIC dl)
endif ()

# pthread is needed for std::mutex on most Linux toolchains.
find_package(Threads QUIET)
if (Threads_FOUND)
    target_link_libraries(bambu_bridge PUBLIC Threads::Threads)
endif ()

# ---- bridge_cli executable ----------------------------------------------
#
# Tiny TSV-emitter for headless inspection of the cloud inventory. The
# binary lives next to the library so phase 12's E2E suite can shell out
# to it.
add_executable(bridge_cli
    cli/bridge_cli.cpp
)
target_link_libraries(bridge_cli PRIVATE bambu_bridge)
set_target_properties(bridge_cli PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if (BAMBU_BRIDGE_STANDALONE)
    # In standalone mode the parent CMakeLists.txt would normally pull in
    # tests/bridge/ for us; do it here instead so a bare phase-0/1 configure
    # gives runnable tests.
    set(_bridge_tests_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/bridge")
    if (EXISTS "${_bridge_tests_dir}/CMakeLists.txt")
        add_subdirectory("${_bridge_tests_dir}" "${CMAKE_CURRENT_BINARY_DIR}/tests_bridge")
    endif ()
endif ()
