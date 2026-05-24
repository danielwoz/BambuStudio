# Bambu Bridge — tests.
#
# Phase 0: BridgeServiceSmoke      — trivial construct/destruct sanity check.
# Phase 1: CloudInventoryTest      — absent-plugin safety contract.
# Phase 2: CertFactoryTest         — per-device self-signed cert cache.
# Phase 3: SsdpResponderUnitTest   — byte-format pin for SSDP reply/NOTIFY.
#         SsdpResponderLoopbackTest — exercises recv/respond on 127.0.0.1.
#                                     Returns ctest skip code (77) on hosts
#                                     where lo multicast / port-1900 binding
#                                     is unavailable.
# Phase 4: MqttFramingTest         — MQTT 3.1.1 packet codec round-trips.
#         MqttBrokerLoopbackTest   — TLS broker on 127.0.0.2:8883 (or a
#                                    random free port on 127.0.0.1 if the
#                                    alias is unavailable). Returns 77
#                                    when binding loopback fails outright.
#
# CertFactoryTest + MqttBrokerLoopbackTest need OpenSSL directly. The
# other tests stay OpenSSL-free.
#
# Phase 12 adds an opt-in real-printer E2E suite under e2e/. The suite
# is gated on BAMBU_BRIDGE_E2E=ON (default OFF) — when off, no e2e_*
# binaries are even configured. The per-model executables also accept
# a comma-separated BAMBU_BRIDGE_E2E_PRINTERS env at run time; entries
# absent from that list exit 77 (ctest 'Not Run').
#
# E2EHarnessSkipTest below ALWAYS builds: it pins the empty-endpoint
# contract of E2EHarness so the wiring doesn't bit-rot when no real
# printers are around.
option(BAMBU_BRIDGE_E2E "Build end-to-end tests against real printers" OFF)
if (BAMBU_BRIDGE_E2E)
    set(BAMBU_BRIDGE_E2E_PRINTERS "" CACHE STRING
        "Comma-separated list of model@ip[:access-code] pairs, e.g. h2s@192.168.1.209,a1mini@192.168.1.210")
    if (NOT BAMBU_BRIDGE_E2E_PRINTERS)
        message(WARNING
            "BAMBU_BRIDGE_E2E=ON but BAMBU_BRIDGE_E2E_PRINTERS empty — "
            "E2E tests will configure but skip at run time")
    endif ()
    add_subdirectory(e2e)
endif ()

add_executable(BridgeServiceSmoke BridgeServiceSmoke.cpp)
target_link_libraries(BridgeServiceSmoke PRIVATE bambu_bridge)
set_target_properties(BridgeServiceSmoke PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME BridgeServiceSmoke COMMAND BridgeServiceSmoke)

# BridgeAppCliArgsTest        — exercises the shared CLI parser that
#                               backs BambuStudio's --bridge-only headless
#                               mode. Pins every flag's parse + error path
#                               so flag regressions surface here before
#                               the slicer is rebuilt.
add_executable(BridgeAppCliArgsTest BridgeAppCliArgsTest.cpp)
target_link_libraries(BridgeAppCliArgsTest PRIVATE bambu_bridge)
set_target_properties(BridgeAppCliArgsTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME BridgeAppCliArgsTest COMMAND BridgeAppCliArgsTest)

add_executable(CloudInventoryTest CloudInventoryTest.cpp)
target_link_libraries(CloudInventoryTest PRIVATE bambu_bridge)
set_target_properties(CloudInventoryTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME CloudInventoryTest COMMAND CloudInventoryTest)

add_executable(CertFactoryTest CertFactoryTest.cpp)
target_link_libraries(CertFactoryTest PRIVATE bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(CertFactoryTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME CertFactoryTest COMMAND CertFactoryTest)

add_executable(SsdpResponderUnitTest SsdpResponderUnitTest.cpp)
target_link_libraries(SsdpResponderUnitTest PRIVATE bambu_bridge)
set_target_properties(SsdpResponderUnitTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME SsdpResponderUnitTest COMMAND SsdpResponderUnitTest)

add_executable(SsdpResponderLoopbackTest SsdpResponderLoopbackTest.cpp)
target_link_libraries(SsdpResponderLoopbackTest PRIVATE bambu_bridge)
set_target_properties(SsdpResponderLoopbackTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME SsdpResponderLoopbackTest COMMAND SsdpResponderLoopbackTest)
# Honour ctest's "skipped" convention (return code 77 → skipped, not failed).
set_tests_properties(SsdpResponderLoopbackTest PROPERTIES
    SKIP_RETURN_CODE 77
)

# ---- Phase 4a -----------------------------------------------------------

add_executable(MqttFramingTest MqttFramingTest.cpp)
target_link_libraries(MqttFramingTest PRIVATE bambu_bridge)
set_target_properties(MqttFramingTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME MqttFramingTest COMMAND MqttFramingTest)

# ---- Phase 4b -----------------------------------------------------------

add_executable(MqttBrokerLoopbackTest
    MqttBrokerLoopbackTest.cpp
    support/MqttTestClient.cpp
    support/MqttTestClient.hpp
)
target_link_libraries(MqttBrokerLoopbackTest PRIVATE
    bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(MqttBrokerLoopbackTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME MqttBrokerLoopbackTest COMMAND MqttBrokerLoopbackTest)
set_tests_properties(MqttBrokerLoopbackTest PROPERTIES
    SKIP_RETURN_CODE 77
)

# ---- Phase 5 (refactored to plugin route) ------------------------------
#
# LanUplinkUnitTest    — pure-state-machine test using MockPluginHandle.
# LanUplinkPluginTest  — plugin-routed loopback test. Replaces the old
#                        LanUplinkLoopbackTest (which had spun up a TLS+
#                        MQTT stand-in printer — meaningless now that
#                        LanUplink goes through the plugin).

add_executable(LanUplinkUnitTest LanUplinkUnitTest.cpp)
target_link_libraries(LanUplinkUnitTest PRIVATE bambu_bridge)
set_target_properties(LanUplinkUnitTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME LanUplinkUnitTest COMMAND LanUplinkUnitTest)

add_executable(LanUplinkPluginTest LanUplinkPluginTest.cpp)
target_link_libraries(LanUplinkPluginTest PRIVATE bambu_bridge)
set_target_properties(LanUplinkPluginTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME LanUplinkPluginTest COMMAND LanUplinkPluginTest)

# ---- Phase 6 ------------------------------------------------------------
#
# BambuNetworkingPluginHandleTest — missing-plugin safety contract for
#                                   the shared cloud-agent handle.
# CloudUplinkUnitTest              — plugin-less CloudUplink: every
#                                   IUplink call must be a safe no-op.
# CloudUplinkLoopbackTest          — drives CloudUplink against a
#                                   MockPluginHandle subclass that
#                                   records send/recv in memory.

add_executable(BambuNetworkingPluginHandleTest BambuNetworkingPluginHandleTest.cpp)
target_link_libraries(BambuNetworkingPluginHandleTest PRIVATE bambu_bridge)
set_target_properties(BambuNetworkingPluginHandleTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME BambuNetworkingPluginHandleTest COMMAND BambuNetworkingPluginHandleTest)

add_executable(CloudUplinkUnitTest CloudUplinkUnitTest.cpp)
target_link_libraries(CloudUplinkUnitTest PRIVATE bambu_bridge)
set_target_properties(CloudUplinkUnitTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME CloudUplinkUnitTest COMMAND CloudUplinkUnitTest)

add_executable(CloudUplinkLoopbackTest CloudUplinkLoopbackTest.cpp)
target_link_libraries(CloudUplinkLoopbackTest PRIVATE bambu_bridge)
set_target_properties(CloudUplinkLoopbackTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME CloudUplinkLoopbackTest COMMAND CloudUplinkLoopbackTest)

# ---- Phase 7 ------------------------------------------------------------
#
# NullUploadSinkTest          — unit; pins the documented drop contract.
# FtpsServerLoopbackTest      — TLS-implicit FTPS server on 127.0.0.2:9990
#                               (or 127.0.0.1:0 fallback). Drives the full
#                               STOR path via a recording IUploadSink and
#                               an inline FtpsTestClient. Skips (rc 77)
#                               when loopback binding is unavailable.
# FtpsAuthTest                — pins the 530 path for wrong PASS / wrong
#                               USER / commands-before-login.

add_executable(NullUploadSinkTest NullUploadSinkTest.cpp)
target_link_libraries(NullUploadSinkTest PRIVATE bambu_bridge)
set_target_properties(NullUploadSinkTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME NullUploadSinkTest COMMAND NullUploadSinkTest)

# CloudUploadSinkTest          — exercises the real plugin-routed path
#                                via a MockPluginHandle that subclasses
#                                BambuNetworkingPluginHandle. Covers the
#                                no-plugin path, happy path with byte-level
#                                spool verification, missing-export path
#                                (rc=-2), and plugin-side error propagation.
add_executable(CloudUploadSinkTest CloudUploadSinkTest.cpp)
target_link_libraries(CloudUploadSinkTest PRIVATE bambu_bridge)
set_target_properties(CloudUploadSinkTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME CloudUploadSinkTest COMMAND CloudUploadSinkTest)

# LanUploadSinkPluginTest — mirror of CloudUploadSinkTest. Drives the
# plugin-routed LanUploadSink (now using start_local_print_with_record
# instead of a raw FTPS client) against a MockPluginHandle that reads
# back the spooled tempfile to byte-verify the upload payload.
add_executable(LanUploadSinkPluginTest LanUploadSinkPluginTest.cpp)
target_link_libraries(LanUploadSinkPluginTest PRIVATE bambu_bridge)
set_target_properties(LanUploadSinkPluginTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME LanUploadSinkPluginTest COMMAND LanUploadSinkPluginTest)

add_executable(FtpsServerLoopbackTest
    FtpsServerLoopbackTest.cpp
    support/FtpsTestClient.cpp
    support/FtpsTestClient.hpp
)
target_link_libraries(FtpsServerLoopbackTest PRIVATE
    bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(FtpsServerLoopbackTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME FtpsServerLoopbackTest COMMAND FtpsServerLoopbackTest)
set_tests_properties(FtpsServerLoopbackTest PROPERTIES
    SKIP_RETURN_CODE 77
)

add_executable(FtpsAuthTest
    FtpsAuthTest.cpp
    support/FtpsTestClient.cpp
    support/FtpsTestClient.hpp
)
target_link_libraries(FtpsAuthTest PRIVATE
    bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(FtpsAuthTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME FtpsAuthTest COMMAND FtpsAuthTest)
set_tests_properties(FtpsAuthTest PROPERTIES
    SKIP_RETURN_CODE 77
)

# ---- Phase 8 ------------------------------------------------------------
#
# NullCameraSourceTest      — unit; verifies the embedded test-pattern
#                             fixture's SPS/PPS shape + ~30fps cadence.
# RtspServerLoopbackTest    — full OPTIONS -> TEARDOWN over TLS using the
#                             inline RtspTestClient and NullCameraSource.
#                             Returns 77 if loopback bind fails.

add_executable(NullCameraSourceTest NullCameraSourceTest.cpp)
target_link_libraries(NullCameraSourceTest PRIVATE bambu_bridge)
set_target_properties(NullCameraSourceTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME NullCameraSourceTest COMMAND NullCameraSourceTest)

# ---- Phase 8 follow-up: LAN/Cloud camera source plugin route -----------
#
# LanCameraSourcePluginTest    — drives LanCameraSource against a mock
#                                BambuSourceHandle that records every
#                                bambu_create/open/start_stream/
#                                read_sample call. Pins the URL format
#                                (triple underscore + ?proto=rtsps) and
#                                the lifecycle (create → open → stream →
#                                read → close → destroy).
# CloudCameraSourcePluginTest  — same shape, but URL flows in from a
#                                mock BambuNetworkingPluginHandle's
#                                get_camera_url. Both agora and rtsps
#                                URL strings are exercised to pin the
#                                "scheme-agnostic relay" contract.
# BambuSourceHandleTest        — missing-library safety contract for
#                                the C-ABI wrapper itself.

add_executable(LanCameraSourcePluginTest LanCameraSourcePluginTest.cpp)
target_link_libraries(LanCameraSourcePluginTest PRIVATE bambu_bridge)
set_target_properties(LanCameraSourcePluginTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME LanCameraSourcePluginTest COMMAND LanCameraSourcePluginTest)

add_executable(CloudCameraSourcePluginTest CloudCameraSourcePluginTest.cpp)
target_link_libraries(CloudCameraSourcePluginTest PRIVATE bambu_bridge)
set_target_properties(CloudCameraSourcePluginTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME CloudCameraSourcePluginTest COMMAND CloudCameraSourcePluginTest)

# ---- Ship-10e: A1/P1 JPEG-on-port-6000 camera source -------------------
#
# JpegCameraSourceTest         — unit; pins the OpenBambuAPI/video.md
#                                wire format (80-byte auth packet,
#                                16-byte LE frame header) and the
#                                end-to-end open / next_frame / close
#                                lifecycle via a faked socket IO seam.
#                                Also pins the model-gating predicate
#                                that selects JPEG for A1/P1-series
#                                models and skips X1/H2 series.

add_executable(JpegCameraSourceTest JpegCameraSourceTest.cpp)
target_link_libraries(JpegCameraSourceTest PRIVATE bambu_bridge)
set_target_properties(JpegCameraSourceTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME JpegCameraSourceTest COMMAND JpegCameraSourceTest)

add_executable(BambuSourceHandleTest BambuSourceHandleTest.cpp)
target_link_libraries(BambuSourceHandleTest PRIVATE bambu_bridge)
set_target_properties(BambuSourceHandleTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME BambuSourceHandleTest COMMAND BambuSourceHandleTest)

add_executable(RtspServerLoopbackTest
    RtspServerLoopbackTest.cpp
    support/RtspTestClient.cpp
    support/RtspTestClient.hpp
)
target_link_libraries(RtspServerLoopbackTest PRIVATE
    bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(RtspServerLoopbackTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME RtspServerLoopbackTest COMMAND RtspServerLoopbackTest)
set_tests_properties(RtspServerLoopbackTest PROPERTIES
    SKIP_RETURN_CODE 77
)

# ---- Ship-11d: RFC 2435 RTP/JPEG packetiser ----------------------------
#
# RtspJpegPacketiserTest        — unit; pins the JFIF parser (SOF/DQT/SOS
#                                 walk) and the RTP-JPEG packet builder
#                                 (8 B JPEG hdr + 4 B QT hdr + qtables +
#                                 scan bytes; fragmentation with monotonic
#                                 frag-offset). No TLS, no sockets.

add_executable(RtspJpegPacketiserTest RtspJpegPacketiserTest.cpp)
target_link_libraries(RtspJpegPacketiserTest PRIVATE bambu_bridge)
set_target_properties(RtspJpegPacketiserTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME RtspJpegPacketiserTest COMMAND RtspJpegPacketiserTest)

# RtspServerMjpegLoopbackTest    — integration test exercising the full
#                                  DESCRIBE/SETUP/PLAY/TEARDOWN flow against
#                                  a stub MJPEG ICameraSource. Asserts SDP
#                                  advertises PT 26 (JPEG/90000) and the
#                                  resulting RTP packets carry the RFC 2435
#                                  header + Quantization Table header that
#                                  matches the source's DQT bytes.
add_executable(RtspServerMjpegLoopbackTest
    RtspServerMjpegLoopbackTest.cpp
    support/RtspTestClient.cpp
    support/RtspTestClient.hpp
)
target_link_libraries(RtspServerMjpegLoopbackTest PRIVATE
    bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(RtspServerMjpegLoopbackTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME RtspServerMjpegLoopbackTest COMMAND RtspServerMjpegLoopbackTest)
set_tests_properties(RtspServerMjpegLoopbackTest PROPERTIES
    SKIP_RETURN_CODE 77
)

# ---- Phase 9 ------------------------------------------------------------
#
# SessionRouterTest               — pure unit; stub LanUplink + CloudUplink,
#                                   exercises debounce + None/LAN/Cloud
#                                   routing + dual-attach downstream.
# UploadSinkRouterTest            — pure unit; stub LanUploadSink +
#                                   CloudUploadSink with toggleable
#                                   success; verifies fallback flow.
# CameraSourceRouterTest          — pure unit; three stub sources;
#                                   verifies sticky open + open-failure
#                                   fallback + no-mid-stream-failover.
# SessionRouterIntegrationTest    — full TLS+MQTT slicer broker on top
#                                   of SessionRouter, LanUplink to a
#                                   stand-in MqttBroker, CloudUplink with
#                                   MockPluginHandle. Failover by tearing
#                                   down LAN session; verifies slicer
#                                   broker session stays up. Skips (rc 77)
#                                   on loopback bind failure.

add_executable(SessionRouterTest SessionRouterTest.cpp)
target_link_libraries(SessionRouterTest PRIVATE bambu_bridge)
set_target_properties(SessionRouterTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME SessionRouterTest COMMAND SessionRouterTest)

add_executable(UploadSinkRouterTest UploadSinkRouterTest.cpp)
target_link_libraries(UploadSinkRouterTest PRIVATE bambu_bridge)
set_target_properties(UploadSinkRouterTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME UploadSinkRouterTest COMMAND UploadSinkRouterTest)

add_executable(CameraSourceRouterTest CameraSourceRouterTest.cpp)
target_link_libraries(CameraSourceRouterTest PRIVATE bambu_bridge)
set_target_properties(CameraSourceRouterTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME CameraSourceRouterTest COMMAND CameraSourceRouterTest)

add_executable(SessionRouterIntegrationTest
    SessionRouterIntegrationTest.cpp
    support/MqttTestClient.cpp
    support/MqttTestClient.hpp
)
target_link_libraries(SessionRouterIntegrationTest PRIVATE
    bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(SessionRouterIntegrationTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME SessionRouterIntegrationTest COMMAND SessionRouterIntegrationTest)
set_tests_properties(SessionRouterIntegrationTest PROPERTIES
    SKIP_RETURN_CODE 77
)

# ---- Phase 10 -----------------------------------------------------------
#
# BridgeAppLifecycleTest    — init-fail (bad plugin_path) + clean-shutdown
#                             (all servers disabled, mock plugin) lifecycle
#                             paths. No network listeners are bound — it's
#                             a pure orchestrator-state test.
# BridgeAppMultiDeviceTest  — drives poll_inventory_once() with a mock
#                             plugin that reports two devices; asserts
#                             both land on every enabled server with
#                             DISTINCT ports per role. SSDP is disabled to
#                             avoid the 1900 bind; mqtt/ftps/rtsp ports
#                             are rebased into the ephemeral range so the
#                             listeners come up on CI.

add_executable(BridgeAppLifecycleTest BridgeAppLifecycleTest.cpp)
target_link_libraries(BridgeAppLifecycleTest PRIVATE bambu_bridge)
set_target_properties(BridgeAppLifecycleTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME BridgeAppLifecycleTest COMMAND BridgeAppLifecycleTest)

add_executable(BridgeAppMultiDeviceTest BridgeAppMultiDeviceTest.cpp)
target_link_libraries(BridgeAppMultiDeviceTest PRIVATE bambu_bridge)
set_target_properties(BridgeAppMultiDeviceTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME BridgeAppMultiDeviceTest COMMAND BridgeAppMultiDeviceTest)
set_tests_properties(BridgeAppMultiDeviceTest PROPERTIES
    SKIP_RETURN_CODE 77
)

# ---- Phase 11 -----------------------------------------------------------
#
# WireDiffSelfTest    - shells out to tools/wire_diff/wire_diff.py and
#                       verifies each protocol's normaliser (identity,
#                       normalisable-change, structural-change). Skips
#                       (rc 77) if python3 is absent.
# WireDiffBridgeTest  - stands up SsdpResponder, MqttBroker, FtpsServer,
#                       RtspServer on loopback ports, runs the matching
#                       capture_*.py probe against each, and diffs the
#                       result against the fixture. Skips (rc 77) if all
#                       protocols can't be reached on this host.

# Resolve the wire_diff path at configure time. The source tree layout
# is `<repo>/tools/wire_diff` and the bambu_bridge module's CMakeLists.txt
# lives at `<repo>/src/bambu_bridge`, so we walk two levels up from
# this file's parent (`<repo>/tests/bridge` -> `<repo>`).
get_filename_component(_bb_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_bb_wire_diff "${_bb_repo_root}/tools/wire_diff")

find_package(Python3 COMPONENTS Interpreter QUIET)

add_executable(WireDiffSelfTest WireDiffSelfTest.cpp)
target_link_libraries(WireDiffSelfTest PRIVATE bambu_bridge)
target_compile_definitions(WireDiffSelfTest PRIVATE
    BAMBU_BRIDGE_WIRE_DIFF_DIR="${_bb_wire_diff}"
)
set_target_properties(WireDiffSelfTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME WireDiffSelfTest COMMAND WireDiffSelfTest)
set_tests_properties(WireDiffSelfTest PROPERTIES
    SKIP_RETURN_CODE 77
)

add_executable(WireDiffBridgeTest WireDiffBridgeTest.cpp)
target_link_libraries(WireDiffBridgeTest PRIVATE
    bambu_bridge OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(WireDiffBridgeTest PRIVATE
    BAMBU_BRIDGE_WIRE_DIFF_DIR="${_bb_wire_diff}"
)
set_target_properties(WireDiffBridgeTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME WireDiffBridgeTest COMMAND WireDiffBridgeTest)
set_tests_properties(WireDiffBridgeTest PROPERTIES
    SKIP_RETURN_CODE 77
)

if (NOT Python3_FOUND)
    # No python3 at configure time -> mark BOTH WireDiff tests DISABLED
    # so ctest reports them as not-run rather than a failure. The C++
    # main()s also probe python3 at runtime and SKIP if missing, but the
    # DISABLED flag short-circuits the build target dependency.
    set_tests_properties(WireDiffSelfTest    PROPERTIES DISABLED TRUE)
    set_tests_properties(WireDiffBridgeTest  PROPERTIES DISABLED TRUE)
endif ()

# ---- Phase 12 -----------------------------------------------------------
#
# E2EHarnessSkipTest    — wiring-rot sentry. Always built (independent of
#                         BAMBU_BRIDGE_E2E). Constructs an E2EHarness
#                         with an empty endpoint list, asserts that
#                         start() returns false, stop() is a no-op, and
#                         that ports_for() returns nullopt. No network.
#
# The four real e2e_<model>_print binaries live under e2e/ and are only
# configured when BAMBU_BRIDGE_E2E=ON.

add_executable(E2EHarnessSkipTest
    E2EHarnessSkipTest.cpp
    e2e/E2EHarness.cpp
    e2e/E2EHarness.hpp
)
target_link_libraries(E2EHarnessSkipTest PRIVATE bambu_bridge)
if (TARGET Threads::Threads)
    target_link_libraries(E2EHarnessSkipTest PRIVATE Threads::Threads)
endif ()
target_include_directories(E2EHarnessSkipTest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/e2e"
)
set_target_properties(E2EHarnessSkipTest PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
add_test(NAME E2EHarnessSkipTest COMMAND E2EHarnessSkipTest)

# ---- Phase 13 — printer-behaviour test harness ---------------------------
#
# BAMBU_BRIDGE_HARNESS=ON pulls in:
#   * harness/                  — ShimRecorder JSONL writer, TraceLine,
#                                 TraceComparator (cmp tool + lib).
#   * mocks/                    — MockPlugin + MockBambuSource + the
#                                 per-printer FSM and capability tables.
#   * HarnessShimRecorderTest   — unit test for the JSONL writer.
#   * HarnessMockA1Test / H2S / H2D
#                               — drive CloudUplink against the in-process
#                                 mock plugin; one model per binary.
#   * HarnessTraceMatchTest     — replay-vs-fixture comparator; SKIPs
#                                 with rc 77 when the golden fixture is
#                                 absent (one-time capture against real
#                                 printers is out of scope for v1).
#
# Production (BAMBU_BRIDGE_HARNESS=OFF) builds are byte-identical to today;
# nothing in src/ references the harness library at link time when the
# option is off.
option(BAMBU_BRIDGE_HARNESS "Build the printer-behaviour test harness" OFF)
if (BAMBU_BRIDGE_HARNESS)
    add_subdirectory(harness)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mocks/CMakeLists.txt")
        add_subdirectory(mocks)
    endif ()

    add_executable(HarnessShimRecorderTest HarnessShimRecorderTest.cpp)
    target_link_libraries(HarnessShimRecorderTest PRIVATE
        bambu_bridge_harness
        bambu_bridge
    )
    set_target_properties(HarnessShimRecorderTest PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    add_test(NAME HarnessShimRecorderTest COMMAND HarnessShimRecorderTest)

    # Per-printer harness tests + the trace-comparator gate. They follow
    # the SKIP_RETURN_CODE 77 convention so missing fixtures (the golden
    # traces are captured-from-real-hardware one-time blobs) mark the
    # test as Not Run rather than Failed.
    foreach (_model A1 H2S H2D)
        set(_tgt HarnessMock${_model}Test)
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_tgt}.cpp")
            add_executable(${_tgt} ${_tgt}.cpp)
            target_link_libraries(${_tgt} PRIVATE
                bambu_bridge_harness
                bambu_bridge_mocks
                bambu_bridge
            )
            set_target_properties(${_tgt} PROPERTIES
                CXX_STANDARD 17
                CXX_STANDARD_REQUIRED ON
            )
            add_test(NAME ${_tgt} COMMAND ${_tgt})
            set_tests_properties(${_tgt} PROPERTIES
                SKIP_RETURN_CODE 77
            )
        endif ()
    endforeach ()

    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/HarnessTraceMatchTest.cpp")
        add_executable(HarnessTraceMatchTest HarnessTraceMatchTest.cpp)
        target_link_libraries(HarnessTraceMatchTest PRIVATE
            bambu_bridge_harness
            bambu_bridge
        )
        target_compile_definitions(HarnessTraceMatchTest PRIVATE
            BAMBU_BRIDGE_HARNESS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        )
        set_target_properties(HarnessTraceMatchTest PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
        )
        add_test(NAME HarnessTraceMatchTest COMMAND HarnessTraceMatchTest)
        set_tests_properties(HarnessTraceMatchTest PROPERTIES
            SKIP_RETURN_CODE 77
        )
    endif ()
endif ()
