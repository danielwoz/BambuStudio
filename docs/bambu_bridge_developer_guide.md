# Bambu Bridge — developer guide

For someone joining the bridge work in a future session. Pairs with
`bambu_bridge_plan.md` (design) and `bambu_bridge_user_guide.md`
(user-facing surface).

---

## 1. Layout

`src/bambu_bridge/` is a peer of `src/bambu_net_oss/` and owns all
bridge-specific code.

```
src/bambu_bridge/
├── CMakeLists.txt                    # standalone bootstrap; option(BAMBU_BRIDGE)
├── BridgeService.{hpp,cpp}           # top-level orchestrator (per-printer composition)
├── CloudInventory.{hpp,cpp}          # wraps plugin enumeration (cloud-bound devices)
├── BambuNetworkingPluginHandle.{hpp,cpp}  # dlopen() handle for libbambu_networking.so
│
├── server/
│   ├── SsdpResponder.{hpp,cpp}       # answer M-SEARCH + send NOTIFY for the virtual device
│   ├── MqttBroker.{hpp,cpp}          # TLS-MQTT 8883 acceptor; per-session routing
│   ├── MqttFraming.{hpp,cpp}         # MQTT 3.1.1 packet codec (phase 4a)
│   ├── FtpsServer.{hpp,cpp}          # accept slicer .3mf uploads
│   ├── RtspServer.{hpp,cpp}          # re-serve printer camera (phase 8 stub-friendly)
│   ├── IUplink.hpp                   # MQTT-side uplink interface
│   ├── IUploadSink.hpp               # FTPS-side upload interface
│   └── ICameraSource.hpp             # RTSP-side video-source interface
│
├── router/
│   ├── SessionRouter.{hpp,cpp}       # per-MQTT-session route picker (LAN | cloud | null)
│   ├── UploadSinkRouter.{hpp,cpp}    # per-upload route picker
│   ├── CameraSourceRouter.{hpp,cpp}  # per-camera-session route picker
│   ├── UplinkHealth.{hpp,cpp}        # health monitor; drives router fallback
│   ├── LanUplink.{hpp,cpp}           # MQTT pass-through via bambu_net_oss::LanMqttSession
│   ├── CloudUplink.{hpp,cpp}         # MQTT pass-through via plugin's cloud agent
│   ├── NullUplink.{hpp,cpp}          # no-op uplink for tests / no-route states
│   ├── LanUploadSink.{hpp,cpp}       # forward .3mf to printer LAN FTPS
│   ├── CloudUploadSink.{hpp,cpp}     # forward .3mf to plugin's OSS-upload path
│   ├── NullUploadSink.{hpp,cpp}      # no-op
│   ├── LanCameraSource.{hpp,cpp}     # stub (phase 8 limitation)
│   ├── CloudCameraSource.{hpp,cpp}   # camera via cloud
│   └── NullCameraSource.{hpp,cpp}    # no-op
│
├── tls/
│   └── CertFactory.{hpp,cpp}         # per-device self-signed certs (CN = dev_id)
│
├── headless/
│   ├── bambu_bridge_daemon.cpp       # main() — hand-rolled argv parser → BridgeApp
│   ├── BridgeApp.{hpp,cpp}           # multi-device daemon: inventory poll + lifecycle
│   └── SignalHandler.{hpp,cpp}       # RAII SIGINT/SIGTERM trampoline
│
├── cli/
│   └── bridge_cli.cpp                # `bridge-cli list-devices` and single-device proxy
│
└── third_party/                      # vendored small deps (kept minimal)
```

Tests:

```
tests/bridge/
├── *Test.cpp / *LoopbackTest.cpp     # 26 standard tests (unit + integration)
├── support/                          # test clients: MQTT, FTPS, RTSP
└── e2e/                              # 4 e2e_<model>_print tests + harness
    ├── E2EHarness.{hpp,cpp}          # spawn daemon, drive it, capture stderr
    ├── E2EPrinterClient.{hpp,cpp}    # acts as the slicer end of an MQTT session
    ├── E2EFixtureLoader.{hpp,cpp}    # loads cube_5mm.3mf (stub-aware)
    └── fixtures/cube_5mm.3mf         # STUB — see phase-12 report
```

Other bridge-owned trees:

- `tools/wire_diff/` — phase-11 indistinguishability validator
  (Python, stdlib only). See `tools/wire_diff/README.md`.

---

## 2. Build modes

### 2.1 Standalone (current default)

```sh
cmake -S src/bambu_bridge -B build/bridge_standalone -DBAMBU_BRIDGE=ON
cmake --build build/bridge_standalone -- -j$(nproc)
ctest --test-dir build/bridge_standalone --output-on-failure
```

This is the build CI runs. It is self-contained — does NOT require
the rest of BambuStudio's deps tree (no wx, no GL, no Boost ≥ 1.83).
The bridge has its own bootstrap `CMakeLists.txt` rooted at
`src/bambu_bridge/CMakeLists.txt`.

### 2.2 Full BambuStudio (blocked by Boost on most hosts)

If your environment satisfies BambuStudio's Boost ≥ 1.83 requirement
and the rest of the upstream deps:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=... -DBAMBU_BRIDGE=ON
cmake --build build -- -j$(nproc)
```

This builds the slicer AND the bridge in the same tree. The slicer
binary then picks up the phase-10 `--bridge-only` exec hand-off.

### 2.3 E2E build

```sh
cmake -S src/bambu_bridge -B build/bridge_e2e \
    -DBAMBU_BRIDGE=ON \
    -DBAMBU_BRIDGE_E2E=ON \
    -DBAMBU_BRIDGE_E2E_PRINTERS="h2s@192.0.2.209,a1mini@192.0.2.210,h2@192.0.2.220"
cmake --build build/bridge_e2e -- -j$(nproc)
ctest --test-dir build/bridge_e2e -L bridge-e2e --output-on-failure
```

See `docs/bambu_bridge_e2e_guide.md` for the full E2E walk-through,
prerequisites, and fixture-replacement instructions.

---

## 3. Phase summary

| # | SHA          | Phase                                              | Summary                                                                 |
|--:|--------------|----------------------------------------------------|-------------------------------------------------------------------------|
| 0 | `383fed317`  | Scaffold                                            | `src/bambu_bridge/` tree, `BAMBU_BRIDGE` option, smoke test.            |
| 1 | `645d11b49`  | CloudInventory + bridge-cli list-devices            | Walk cloud-bound printers via the proprietary plugin.                    |
| 2 | `db477a22c`  | CertFactory                                         | Per-device self-signed cert (CN = dev_id, issuer "BBL CA").              |
| 3 | `4f9131d4b`  | SsdpResponder                                        | Answer M-SEARCH + periodic NOTIFY for the virtual device.                |
| 4a| `444591048`  | MqttFraming                                          | MQTT 3.1.1 packet codec.                                                 |
| 4b| `83560246c`  | MqttBroker                                           | TLS 8883 acceptor per virtual device.                                    |
| 5 | `db2fc61d7`  | LanUplink                                            | Direct-LAN passthrough via `bambu_net_oss::LanMqttSession`.              |
| — | `5e7d78b9c`  | refactor                                             | Extract `BambuNetworkingPluginHandle` for shared dlopen state.            |
| 6 | `80a93335d`  | CloudUplink                                          | Cloud fallback uplink via plugin's cloud agent.                          |
| 7 | `e7feb7354`  | FtpsServer                                           | Accept slicer .3mf uploads; route via LanUploadSink / CloudUploadSink.   |
| 8 | `b85f48cf6`  | RtspServer                                           | Camera re-serve (`LanCameraSource` stub, `CloudCameraSource` live).      |
| 9a| `b9bd4de3c`  | UplinkHealthMonitor + per-component routers          | Health-driven `SessionRouter` / `UploadSinkRouter` / `CameraSourceRouter`.|
| 9b| `f392e8a04`  | BridgeService wiring + 'proxy' CLI                   | `bridge-cli proxy` single-device exerciser; service composes per-device. |
| 10| `8d4efc83a`  | Headless multi-device daemon                         | `BridgeApp` + `SignalHandler`; consumed in-process via `BambuStudio --bridge-only`. No standalone daemon binary — the proprietary plugin fingerprints its host process. |
| 11| `21f55d59f`  | Wire-diff indistinguishability framework             | `tools/wire_diff/` + 2 wire-diff tests.                                  |
| 12| `b4b6551bf`  | E2E test suite                                       | 4 `e2e_<model>_print` tests gated behind `BAMBU_BRIDGE_E2E=ON`.          |
| 13| *(this)*     | Docs + release notes                                 | User guide, release notes, developer guide; plan marked COMPLETE.       |

Test count totals: **26 standard + 4 E2E = 30 tests**.

---

## 4. Adding a new server type

If you need a new downstream-facing server (say, a generic HTTP
admin endpoint), follow the pattern the four existing servers share:

1. **Interface.** Add (or pick one of) `server/I*.hpp`. Existing
   examples: `IUplink.hpp`, `IUploadSink.hpp`, `ICameraSource.hpp`.
   Keep the interface narrow — one method per logical operation.
2. **Per-device config struct.** Extend the relevant config struct.
   The headless config is `BridgeAppConfig` in
   `headless/BridgeApp.hpp` (look for `mqtt_port_base`,
   `cert_cache_dir`, `enable_*`). Mirror the existing pattern:
   port-base + enable flag.
3. **Implementation.** Add `server/MyServer.{hpp,cpp}`. Bind in the
   constructor, accept in a worker thread, hand off to a router or
   uplink for the actual work. See `FtpsServer.cpp` for the
   lowest-friction example.
4. **BridgeService wiring.** `BridgeService::add_device()` is where
   per-device servers are constructed. Add the new server alongside
   the existing four.
5. **BridgeApp wiring.** `BridgeApp::on_device_added()` /
   `on_device_removed()` plumb config through to `BridgeService`.
   Add a flag and a port-base lookup there.
6. **CLI flag.** Mirror in `headless/bambu_bridge_daemon.cpp`'s
   argv parser AND in the `print_usage()` text — keep the help in
   sync.
7. **Test.** Add a `MyServerLoopbackTest.cpp` next to
   `FtpsServerLoopbackTest.cpp` and wire it into
   `tests/bridge/CMakeLists.txt`. Tests run on loopback; never bind
   to real interfaces.

---

## 5. Adding a new uplink

Same pattern, but in `router/`:

1. **Interface.** All MQTT-side uplinks implement `IUplink` from
   `server/IUplink.hpp`. (Upload sinks → `IUploadSink`; camera
   sources → `ICameraSource`.)
2. **Implementation.** `router/MyUplink.{hpp,cpp}`. Existing
   examples: `LanUplink.cpp` (direct LAN via `bambu_net_oss`),
   `CloudUplink.cpp` (plugin cloud forwarder), `NullUplink.cpp`
   (no-op fallback).
3. **Health signal.** If your uplink can fail liveness, plug it into
   `UplinkHealth` so `SessionRouter` can flip routes automatically.
   See how `LanUplink` reports unreachable LAN IPs.
4. **Router wiring.** `SessionRouter::pick()` is the entry point
   that decides between LAN / cloud / null per session — adjust the
   priority table if your new uplink should slot in.
5. **Test.** Pair a unit test and a loopback test. Conventions:
   `MyUplinkUnitTest.cpp` (no I/O, mocks the dependency) and
   `MyUplinkLoopbackTest.cpp` (binds on loopback, exercises the real
   socket path).

---

## 6. Test patterns

- **Loopback convention.** Tests bind on `127.0.0.1` with an
  OS-assigned port (`bind(port=0)`); the test reads the chosen port
  out of the server before driving the client. No fixed ports — keeps
  parallel `ctest -j` runs collision-free.
- **Test clients in `tests/bridge/support/`.** `MqttTestClient`,
  `FtpsTestClient`, `RtspTestClient` are minimal stdlib-only clients
  used as the "slicer side" in loopback tests. Reuse them; do not
  pull in paho or any non-vendored TLS client.
- **E2E gate.** `tests/bridge/e2e/` only configures when
  `BAMBU_BRIDGE_E2E=ON`. Each `e2e_<model>_print` test exits 77
  (ctest SKIPPED) if its printer is absent from
  `BAMBU_BRIDGE_E2E_PRINTERS` or its fixture is the stub. The skip
  contract is enforced by `E2EHarnessSkipTest` in the standard
  suite — running it without `BAMBU_BRIDGE_E2E=ON` validates the
  gate doesn't break.

---

## 7. Wire-diff harness

`tools/wire_diff/` is the phase-11 indistinguishability validator.
Pure Python 3 stdlib. Workflow:

1. Capture bytes from a real BambuStudio session (`capture/` probes).
2. Capture bytes from a bridge-mirrored session.
3. Run `wire_diff.py` with the appropriate `normalisers/` mask
   (SSDP, MQTT, FTPS, RTSP). A clean diff = indistinguishability
   preserved.

The diff masks normalise out timestamps, sequence IDs, packet IDs,
RTP SSRCs, and similar non-deterministic but irrelevant fields. When
adding a new field to either side of the wire, decide whether it's a
real protocol change (treat diff as a regression) or a normalisable
artefact (extend the appropriate `normalisers/*.py` whitelist).

See `tools/wire_diff/README.md` for full details.

---

## 8. Known limitations

These are tracked from prior phase reports and remain open at the
end of phase 13. None block the v1 release per the phase-0 plan's
out-of-scope list, but anyone picking up bridge work should know them:

- **`LanCameraSource` is a stub** (phase 8). Direct-LAN camera
  re-serve is not yet implemented; `CameraSourceRouter` will
  currently always pick `CloudCameraSource` (or `NullCameraSource`)
  when a stream is requested.
- **`LanUplink` may need mTLS for real H2S writes** (phase 5
  report). The bridge's LAN uplink uses the OSS TLS path, but some
  H2S firmware revisions require client-cert auth on certain MQTT
  topics. The hooks are in place
  (`BBL_MTLS_CERT` / `BBL_MTLS_KEY` env vars) but field validation
  is incomplete.
- **Cloud upload sink: OSS-upload export not wired** (phase 7
  report). `CloudUploadSink` currently routes through the plugin's
  MQTT path; for very large `.3mf` (> a few MB) the proprietary
  plugin's OSS-upload export should be wired in. Tracked but not
  fixed.
- **Port-6000 BambuTunnel forwarding stubbed** (phase 7). The
  `LocalControlTunnel` upstream side is present but the bridge does
  not currently re-serve the tunnel downstream. Only relevant for
  LAN-streamed gcode bytes, not for the project-file upload path.
- **`cube_5mm.3mf` is a stub fixture** (phase 12). The E2E suite
  cannot actually run a print until you replace it; see
  `bambu_bridge_e2e_guide.md` § "Replacing the stub fixture".
- **TLS-layer fingerprinting not covered by wire-diff** (phase 11).
  Wire-diff operates above the TLS layer (after decrypt at each
  end). JA3/JA4 differences between the bridge's OpenSSL build and
  upstream BambuStudio's bundled OpenSSL are not detected.
- **`--bridge-only` Windows behaviour untested.** The flag parses
  and the bridge code links, but the in-process dispatcher in
  `BambuStudio.cpp` has only been exercised on Linux/macOS.
- **`--verbose` is reserved, not implemented** (phase 10). The flag
  parses but is a no-op in the released build.

---

## 9. Pointers

- Design / phases: `docs/bambu_bridge_plan.md`
- User guide: `docs/bambu_bridge_user_guide.md`
- Release notes: `docs/bambu_bridge_release_notes.md`
- E2E walk-through: `docs/bambu_bridge_e2e_guide.md`
- Wire-diff: `tools/wire_diff/README.md`
- Plugin log decryptor (live observability of the proprietary
  `libbambu_networking.so` spdlog stream): `tools/README.md`
  - quick start: `tools/tail-plugin-log.sh` while the bridge is running
  - cipher details (AES-128-ECB, static key): see `RE-LOG-ENCRYPTION.md`
    in the BambuBridge meta-repo root
