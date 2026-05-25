# Bambu Bridge — design + implementation plan

**Branch:** `feature/bambu-bridge` (worktree: `~/BambuStudio-bridge`)
**Base:** `origin/master @ 3e96c7e07` (clean upstream HEAD, 2026-05-11)
**Author:** danielwoz
**Status:** COMPLETE — 14 commits across phases 0–13, 26 unit/integration tests + 4 gated E2E tests, all green on the standalone build. See *What landed* at the bottom.

## Goal

When BambuStudio starts (GUI or headless), it connects to each of the
logged-in user's cloud-controlled printers, **mirrors each one as a
virtual LAN printer**, and proxies the LAN protocol traffic from other
slicers on the LAN to either:

- the **real printer over LAN** if it is reachable on the same network, or
- **Bambu's cloud** otherwise.

The bridge must be **on-the-wire indistinguishable** from native Bambu
clients in both directions — neither the upstream printer/cloud nor the
downstream slicer should be able to detect the proxy.

A new headless mode (`--bridge-only`, no GUI) lets a dedicated machine
run as a permanent bridge.

## Why this is doable

The pieces are already mostly present:

| Need                                  | Status in `master` |
|---------------------------------------|--------------------|
| Cloud auth + REST + MQTT              | Proprietary `bambu_networking` (forwarder available) |
| LAN MQTT client + parser              | `bambu_net_oss/core/LanMqttSession`  |
| LAN file-transfer tunnel (port 6000)  | `bambu_net_oss/core/LocalControlTunnel` + `docs/lan_port_6000_observations.md` |
| FTPS upload                           | `bambu_net_oss/core/FtpsClient` |
| SSDP listener (discovery)             | `bambu_net_oss/core/SsdpListener` |
| Local print orchestration             | `bambu_net_oss/core/LocalPrintOrchestrator` |
| CLI entry / arg parsing               | `src/BambuStudio.cpp::CLI::run` |
| Headless start path                   | none yet — needs `--bridge-only` flag |
| **SSDP RESPONDER (announce)**         | **does not exist — new**           |
| **LAN MQTT BROKER (server)**          | **does not exist — new**           |
| **FTPS server (accept uploads)**      | **does not exist — new**           |
| **Camera / RTSP re-serve**            | **does not exist — new**           |
| Cloud printer enumeration             | proprietary `bambu_network_get_my_print_info` etc. |

The new code goes in `src/bambu_bridge/` (peer of `bambu_net_oss/`). It
reuses the existing OSS LAN client code as its **upstream-direction**
plumbing and adds the *downstream-direction* server pieces.

## Architecture

```
                                                    Bambu Cloud (mqtt+REST+OSS)
                                                              ▲
                                                              │  bn_networking
                                                              │  (proprietary
                                                              │   forwarder)
              Slicer on LAN                                   │
                  │                                  ┌─────── ▼ ──────┐
                  │  SSDP M-SEARCH                  │                  │
                  │  MQTT TLS 8883                  │   Bambu Bridge   │   ── direct LAN ── ▶ Real printer
                  │  FTPS 990                       │   (this feature) │      (if reachable)
                  │  RTSPS 322                      │                  │
                  ▼                                  └─────── ▲ ──────┘
            Virtual LAN printer                              │
            (per cloud-bound device)                         │
                                                  Routing decision per session:
                                                    • prefer direct LAN
                                                    • fall back to cloud
```

Per cloud printer (`dev_id`), the bridge stands up a **`MirroredDevice`**:

- a unique listening **IPv4/port quadruple** for SSDP / MQTT / FTPS / RTSPS
- a generated self-signed cert chain (CN = real `dev_id`) so client TLS
  validates the same way as a real printer
- the printer's real LAN **access code** (pulled from cloud inventory) so
  the downstream slicer authenticates exactly as it would to a real one
- a **router** that, for each incoming session, chooses cloud-MQTT or
  direct-LAN based on reachability + protocol-feature support

Each `MirroredDevice` owns four servers and one router:

```
src/bambu_bridge/
├── BridgeService.{hpp,cpp}        # top-level orchestrator
├── MirroredDevice.{hpp,cpp}       # per-printer state + lifecycle
├── DeviceRoster.{hpp,cpp}         # tracks cloud-bound printers + LAN visibility
├── CloudInventory.{hpp,cpp}       # wraps bambu_network_get_my_print_info etc.
├── server/
│   ├── SsdpResponder.{hpp,cpp}    # respond to M-SEARCH with virtual device
│   ├── MqttBroker.{hpp,cpp}       # accept TLS MQTT on 8883, route topics
│   ├── FtpsServer.{hpp,cpp}       # accept print-job uploads
│   └── RtspServer.{hpp,cpp}       # re-serve camera (proxies through BambuTunnel)
├── router/
│   ├── SessionRouter.{hpp,cpp}    # per-session route picker (LAN vs cloud)
│   ├── CloudUplink.{hpp,cpp}      # bridges to proprietary cloud MQTT
│   └── LanUplink.{hpp,cpp}        # bridges to real printer via LanMqttSession
├── tls/
│   ├── CertFactory.{hpp,cpp}      # generates per-device certs at startup
│   └── KeyStore.{hpp,cpp}         # XDG-cached private keys
└── headless/
    └── BridgeApp.{hpp,cpp}        # wxAppConsole — `--bridge-only`
```

## Indistinguishability requirements

For every packet our bridge emits (cloud-bound *or* LAN-printer-bound),
it must look like it came from a real BambuStudio session. Practically:

1. **Cloud-bound** (upstream): every MQTT/REST/OSS call uses the
   proprietary `bambu_networking` forwarder so the framing, headers,
   user-agent and TLS fingerprint match. We do not synthesize HTTPS
   requests ourselves.
2. **LAN-printer-bound** (real device): every byte comes from
   `bambu_net_oss/core/LanMqttSession` or `LocalControlTunnel`, which
   already speak the exact framing observed in
   `docs/lan_port_6000_observations.md` and
   `docs/lan_mqtt_command_reference.md`.
3. **LAN-slicer-facing** (downstream): we present the same protocol
   surface a real printer presents — same TLS cert shape (self-signed,
   CN = serial, "BBL CA" issuer), same SSDP response, same MQTT topic
   names, same FTPS auth string. Verified by replaying our SSDP/MQTT
   responses through `tools/tunnel_probe.py` against a real printer.

Anything we add of our own (e.g., logging hooks, retry policies) is
fenced off behind ifdef'd test-only entry points so the release build is
literally a pipe between the two endpoints.

## Phases & milestones

Each phase ends with a green test, not just a code drop. Effort estimates
are working-developer-days (8h) assuming the protocol observations in
`docs/` are accurate.

| # | Phase                                              | Deliverable / test                                                                                                            | Est.  |
|--:|----------------------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|-------|
| 0 | Branch + this plan + CI scaffolding                | `cmake -DBAMBU_BRIDGE=ON` builds an empty target; CI smoke-tests it.                                                          | 0.5d  |
| 1 | `CloudInventory` reads bound printers from cloud   | Headless test: `bambu-bridge list-devices` prints user's cloud printers (uses proprietary plugin).                            | 1d    |
| 2 | `CertFactory` + per-device self-signed certs       | Unit: cert validates against the same root-trust path BambuStudio uses for real LAN printers.                                 | 1d    |
| 3 | `SsdpResponder` — answer M-SEARCH                  | Integration: a real BambuStudio on the LAN sees our virtual device in "Add printer" within 10 s.                              | 1.5d  |
| 4 | `MqttBroker` — TLS 8883 accept + topic routing     | Loopback test: paho client publishes/subscribes; broker echoes; observed framing == captured real-printer framing byte-for-byte. | 3d    |
| 5 | `LanUplink` — direct-LAN passthrough               | E2E: slicer sends `print` command → bridge → real H2S over LAN → printer accepts. Capture diff on both ends == 0.              | 2d    |
| 6 | `CloudUplink` — cloud fallback                     | E2E: same `print` command with printer off-LAN → bridge → cloud MQTT → printer accepts.                                       | 3d    |
| 7 | `FtpsServer` — accept job uploads                  | E2E: slicer uploads `.3mf` → bridge stores locally → forwards to printer LAN FTPS *or* cloud OSS.                              | 2d    |
| 8 | `RtspServer` — camera re-serve                     | E2E: slicer connects to virtual camera URL → bridge pulls stream from BambuTunnel → video frames arrive in slicer.            | 3d    |
| 9 | `SessionRouter` — LAN preferred, cloud fallback    | Integration: simulate LAN drop mid-session, verify graceful fallback without slicer-visible disconnect.                       | 2d    |
| 10| Headless mode (`--bridge-only`)                    | `BambuStudio --bridge-only` starts no GUI, full bridge runs; SIGTERM shuts down cleanly.                                       | 1d    |
| 11| Indistinguishability validation                    | Pass `tools/wire_diff.py` (new): captures bytes from real BambuStudio session AND from bridge-mirrored session; diff is empty. | 2d    |
| 12| E2E suite against H2S / A1 / H2                    | `ctest -L bridge-e2e` runs `tests/bridge/e2e_*` against each of the three printer families. Pass = all green.                  | 3d    |
| 13| Docs + release notes                               | `docs/bambu_bridge_user_guide.md` + `--bridge-only` mention in `--help`.                                                       | 0.5d  |

**Total estimate:** ~25 days = 4–5 calendar weeks for a single developer.

## Phase-0 concrete first steps

1. `mkdir -p src/bambu_bridge/{server,router,tls,headless} tests/bridge`
2. Top-level `CMakeLists.txt`: option `BAMBU_BRIDGE` (default ON for now).
3. `src/bambu_bridge/CMakeLists.txt`: empty `bambu_bridge` static lib +
   placeholder test executable.
4. `tests/bridge/CMakeLists.txt`: one `BridgeServiceSmoke` test that just
   instantiates the (empty) service and exits 0.
5. CI: add `BAMBU_BRIDGE=ON` to one matrix row so it doesn't bit-rot.

## E2E test infrastructure

`tests/bridge/e2e_*` is gated behind `BAMBU_BRIDGE_E2E=ON` and an env
list of reachable printers:

```
BAMBU_BRIDGE_E2E_PRINTERS=h2s@192.0.2.209,a1mini@192.0.2.210,h2@192.0.2.220
BAMBU_BRIDGE_E2E_USER=danielwoz@...  # bound to those devices on the cloud
```

Each printer test:

1. Builds and starts the bridge daemon against the cloud account.
2. Waits for SSDP announcement of the virtual device.
3. Runs a scripted BambuStudio session against the virtual device:
   - load a fixed `.3mf` from `tests/bridge/fixtures/`
   - send `print` command
   - wait for printer to reach `RUNNING` state on the bed
   - send `pause` then `cancel`
4. Captures stderr from the bridge to a per-test artifact.
5. Asserts: no panics, the wire-diff vs `tests/bridge/expected/<model>_print.bin` is empty (within timestamp masks).

The fixtures are small (a 5×5 mm cube, ~20 KB sliced) so the print itself
is over in well under a minute.

## Out of scope (v1)

- TUTK / Agora P2P relay when neither LAN nor cloud is reachable. The
  proprietary stack supports it; we punt until v2.
- AMS firmware update via the bridge. Forward-only; no provisioning.
- Multi-user / shared bridge on the same LAN. The bridge runs under a
  single Bambu cloud account.

## Resolved questions (filled in during phase 0)

- **Port-collision policy** if the host already has SSDP listeners: We bind on `0.0.0.0` for SSDP recv but answer with our virtual device's IP only.
- **Multi-NIC**: which interface to announce on. Default: every UP interface with a private-range IPv4.
- **TLS cert trust**: Other slicers do not strictly pin certs; they rely on serial-matches-CN check.
- **Concurrent sessions**: Real Bambu printers only allow one LAN MQTT client. The bridge enforces the same to mirror the printer's behaviour.

## Future / follow-ups

- "Soft cloud" mode: bridge can accept slicer commands when no real
  printer exists, and merely log them — useful for slicer development.
- Web UI in the bridge daemon for status + per-device toggle.
- mTLS option for the LAN side, gated behind a CLI flag, so slicers in
  hardened networks can require client certs.

## What landed (phase-13 close-out)

| # | SHA          | Phase                                              |
|--:|--------------|----------------------------------------------------|
| 0 | `383fed317`  | Scaffold `src/bambu_bridge/` + smoke test          |
| 1 | `645d11b49`  | `CloudInventory` + `bridge-cli list-devices`       |
| 2 | `db477a22c`  | `CertFactory` per-device TLS certs                  |
| 3 | `4f9131d4b`  | `SsdpResponder`                                     |
| 4a| `444591048`  | `MqttFraming` (MQTT 3.1.1 codec)                    |
| 4b| `83560246c`  | `MqttBroker` (TLS 8883 per virtual device)          |
| 5 | `db2fc61d7`  | `LanUplink` (direct-LAN passthrough)                |
| — | `5e7d78b9c`  | refactor: extract `BambuNetworkingPluginHandle`     |
| 6 | `80a93335d`  | `CloudUplink` (cloud fallback uplink)               |
| 7 | `e7feb7354`  | `FtpsServer` (slicer .3mf uploads)                  |
| 8 | `b85f48cf6`  | `RtspServer` (camera re-serve)                       |
| 9a| `b9bd4de3c`  | `UplinkHealthMonitor` + per-component routers       |
| 9b| `f392e8a04`  | `BridgeService` wiring + `proxy` CLI                |
| 10| `8d4efc83a`  | Headless multi-device daemon + `--bridge-only`      |
| 11| `21f55d59f`  | Wire-diff indistinguishability framework            |
| 12| `b4b6551bf`  | E2E suite (H2S / A1 / A1mini / H2)                  |
| 13| *(this commit)* | Docs + release notes                             |

Total: **14 feature commits + 1 refactor** = 15 commits on the
feature branch. Test counts: **26 unit/integration tests** in the
standard build + **4 E2E tests** gated by `BAMBU_BRIDGE_E2E=ON`, for
**30 tests total**.

User-facing docs landed in phase 13:

- `docs/bambu_bridge_user_guide.md` — canonical user-facing guide.
- `docs/bambu_bridge_release_notes.md` — what's new vs upstream.
- `docs/bambu_bridge_developer_guide.md` — onboarding for future
  bridge work.
- `docs/bambu_bridge_e2e_guide.md` (phase 12) — running E2E against
  real printers.
