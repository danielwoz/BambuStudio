# Bambu Bridge — feature inventory (`bridge-necessary`)

What this branch adds on top of `master`. The bridge runs as the slicer's
`--bridge-only` mode (no MainFrame), proxying SSDP discovery, MQTT,
FTPS, RTSP and the storage tunnel from any number of cloud-bound Bambu
printers down to a single LAN endpoint where slicers (or any MQTT-over-
TLS client) can connect to "FFFF…"-mangled virtual serial numbers.

Total delta vs `master`: **+63,489 / −1,457 across 238 files**. No
runtime log lines, no trace macros, no SIGABRT trampoline — those live
in the `bridge-debug` overlay branch.

---

## Top-level shape

```
bambu-studio --bridge-only
  ├── GUI_App (trimmed; no MainFrame, Plater, WebView)
  ├── NetworkAgent (slicer's existing, shared with bridge via adapter)
  ├── DeviceManager (slicer's existing; pushed into BridgeApp every 5 s)
  └── BridgeApp (worker thread)
       ├── SSDP responder + listener (UDP/2021)
       ├── MqttBroker  (per-printer port; mqtt_port_base + index)
       ├── FtpsServer  (per-printer port; ftps_port_base + index)
       ├── RtspServer  (per-printer port; rtsp_port_base + index)
       ├── VirtualTunnelServer (storage tunnel; per-printer port)
       ├── SessionRouter (broker uplink — fan-outs slicer ↔ LAN/Cloud)
       ├── LanUplink + CloudUplink (uplink-side via the plugin)
       ├── UploadSinkRouter (LanUploadSink + CloudUploadSink)
       └── CameraSourceRouter (LanCameraSource + CloudCameraSource)
```

---

## New modules

### `src/bambu_bridge/`
Standalone-buildable static library (`libbambu_bridge.a`) plus a small
`bridge_cli` for headless poking.

| Component | Purpose |
| --- | --- |
| `BridgeApp.{cpp,hpp}` | The orchestrator. Owns the bridge servers + uplinks + inventory polling + worker-thread lifecycle. `set_virtual_printers()` is the host-driven inventory entry point. `mqtt_port_for_dev_id()` resolves a virtual dev-id back to the bound MQTT port. |
| `BridgeAppCliArgs.{cpp,hpp}` | Shared `--bridge-only`/`bambu-bridge-daemon` argument parser (`--plugin`, `--bind`, `--mqtt-port-base`, `--http-header`, `--cert-dir`, …). |
| `SignalHandler.{cpp,hpp}` | SIGINT/SIGTERM trampoline that drives `BridgeApp::shutdown()` from the wx main thread via `CallAfter`. |
| `BambuNetworkingPluginHandle.{cpp,hpp}` | C++ wrapper around `libbambu_networking.so`. The proprietary plugin allows ONE agent per process — when the slicer is the host, `NetworkAgentPluginAdapter` exposes it here so the bridge doesn't dlopen a second copy. |
| `BambuSourceHandle.{cpp,hpp}` | C++ wrapper around `libBambuSource.so` for `Bambu_Tunnel*` / `Bambu_Create*` calls used by the camera + storage paths. |
| `CloudInventory.{cpp,hpp}` | Cloud REST poller for the per-user printer list (id, name, dev_ip, access_code, model, firmware). |
| `BridgeService.{cpp,hpp}` | Pre-`BridgeApp` static lifecycle helper for `bridge_cli` testbed mode. |
| `tls/CertFactory.{cpp,hpp}` | Per-device self-signed TLS certs minted on demand for the MQTT/FTPS/vtun TLS handshakes the slicer makes against the bridge. |
| `cli/bridge_cli.cpp` | `bridge_cli list-devices`, `mint-cert`, `announce`, `ftps`, `mqtt` subcommands. Standalone smoke-tester. |

### `src/bambu_bridge/server/`

| File | Role |
| --- | --- |
| `SsdpResponder.{cpp,hpp}` | Answers `M-SEARCH` and broadcasts NOTIFYs for the FFFF-mangled virtual SNs every device pushes. |
| `SsdpListener.{cpp,hpp}` | Passive listener on UDP/2021 that learns real-printer LAN IPs from the broadcasts they emit. |
| `MqttBroker.{cpp,hpp}` + `MqttFraming.{cpp,hpp}` | TLS-1.2 MQTT 3.1.1 broker. One accept thread per `(ip, port)` device; one I/O thread per session. Auth: `bblp` / access_code. Translates `device/<virtual_sn>/…` ↔ `device/<real_sn>/…` so the slicer's topic filters match. |
| `IUplink.hpp` | Pure-virtual interface the broker calls on PUBLISH/SUBSCRIBE/UNSUBSCRIBE/DISCONNECT and to attach a downstream publisher per dev-id. |
| `FtpsServer.{cpp,hpp}` | Implicit-TLS FTPS on port 990 + (port_base + index) with `LIST`, `STOR`, `RETR`, `PASV` understood. Slicer .3mf uploads land here. |
| `RtspServer.{cpp,hpp}` | RTSP/RTP H.264 server for live-view streams sourced from `CameraSourceRouter`. |
| `VirtualTunnelServer.{cpp,hpp}` | The storage tunnel. Each dev-id gets a TLS listener; the slicer's `VirtualBambuTunnel` connects here for `bambu:///` storage URLs. Delegates JSON-RPC frames to `BridgeStorageBackend → PrinterFileSystem` so the GUI's known-good Bambu_Tunnel consumer drives storage. |
| `ICameraSource.hpp` / `IUploadSink.hpp` | Bridge-side interfaces for camera / upload routing. |

### `src/bambu_bridge/router/`

| File | Role |
| --- | --- |
| `LanUplink.{cpp,hpp}` | Routes slicer publishes to a LAN-MQTT session via the plugin's `send_message_to_printer`. The plugin holds ONE LAN session at a time. |
| `CloudUplink.{cpp,hpp}` | Cloud-MQTT counterpart. Slicer publishes hop via the plugin's `publish_to_device`. The plugin's local-message receiver fans inbound reports back. |
| `NullUplink.{cpp,hpp}` | No-op for tests / advertise-only deploys. |
| `LanUploadSink.{cpp,hpp}` | Wraps `start_send_gcode` for LAN-mode SD-card uploads. |
| `CloudUploadSink.{cpp,hpp}` | Wraps `start_send_gcode_to_sdcard` for cloud-relayed uploads. |
| `NullUploadSink.{cpp,hpp}` | No-op for advertise-only. |
| `LanCameraSource.{cpp,hpp}` | Bambu_Tunnel-backed live-view stream from a LAN printer (port 6000). Accepts a pre-resolved `url_override`. |
| `CloudCameraSource.{cpp,hpp}` | Bambu_Tunnel-backed live-view stream from cloud (TUTK/agora). Accepts a pre-resolved `url_override`. |
| `NullCameraSource.{cpp,hpp}` | Black-frame source. |
| `UplinkHealth.{cpp,hpp}` | Snapshot of each uplink's plugin-side liveness, fed to `SessionRouter`. |
| `SessionRouter.{cpp,hpp}` | Picks LAN vs Cloud per dev-id based on `UplinkHealth`. `attach_downstream` fans the broker's downstream publisher into BOTH sub-uplinks so reports from either side still reach the slicer. |
| `UploadSinkRouter.{cpp,hpp}` | Same idea for uploads. |
| `CameraSourceRouter.{cpp,hpp}` | Same for camera streams. |
| `UploadSpool.{cpp,hpp}` | Disk-backed staging for in-flight uploads. |

### `src/bambu_bridge/headless/`

| File | Role |
| --- | --- |
| `BridgeApp.{cpp,hpp}` | Worker thread that boots SSDP/MQTT/FTPS/RTSP/vtun in order, runs the inventory tick, and tears down in reverse on `shutdown()`. |
| `BridgeAppCliArgs.{cpp,hpp}` | (See above — used identically by `--bridge-only` and `bridge_cli`.) |
| `SignalHandler.{cpp,hpp}` | SIGINT/SIGTERM → `ExitMainLoop` via `CallAfter`. |

### `src/slic3r/Utils/` (slicer-side)

| File | Role |
| --- | --- |
| `VirtualMqttClient.{cpp,hpp}` | Slicer-side MQTT client used when the dev-id starts with `FFFF`. Bypasses the plugin's Bambu-CA chain check (TLS verify=false against the bridge's self-signed cert). One session per virtual dev-id. Dials the per-printer port via `set_port_resolver(fn)` — registered by `GUI_App` after `BridgeApp` is constructed. |
| `VirtualMqttCli.{cpp,hpp}` | Public-API surface that `NetworkAgent::connect_printer` / `send_message_to_printer` / `disconnect_printer` route to for virtual dev-ids. |
| `VirtualFtpsClient.{cpp,hpp}` | Slicer-side FTPS client for virtual dev-id uploads. Same verify=false TLS posture. |
| `VirtualLanPrinterStore.{cpp,hpp}` | Persists the virtual-printer entries the slicer hydrates on next launch so user-bound FFFF dev-ids survive restart. |
| `NetworkAgentPluginAdapter.{cpp,hpp}` | Subclass of `BambuNetworkingPluginHandle` that wraps the slicer's existing `NetworkAgent`. Installs a `BridgeMessageTap` so inbound traffic the slicer receives on non-virtual dev-ids also fans out to the bridge's per-dev-id receivers. |

### `src/slic3r/GUI/` (slicer-side)

| File | Role |
| --- | --- |
| `BridgeOnlyFlag.hpp` | Globals (`g_bridge_only`, `g_bridge_only_cfg`) that the CLI prepass sets so `GUI_App::on_init_inner` takes the headless branch. |
| `BridgeOnlyConsoleApp.{cpp,hpp}` | Legacy wxAppConsole subclass (now bypassed in favour of `init_bridge_only_headless` on the regular wxApp). Kept for the standalone test harness. |
| `Printer/BridgeStorageBackend.{cpp,hpp}` | Sits between the bridge's vtun server and `PrinterFileSystem`. Marshals JSON-RPC frames between the two, hops to the wx main thread for PFS lifecycle. |
| `Printer/MediaUrlBuilder.{cpp,hpp}` | Shared URL ladder (LAN → RTSP-S/RTSP → TUTK → Agora) used by `MediaFilePanel` (storage), `MediaPlayCtrl` (live view) and `Lan/CloudCameraSource` so all three pick the same URL for a given printer. Exposes `build_media_storage_url` and `build_media_live_url`. |
| `Printer/VirtualBambuTunnel.{cpp,hpp}` | A `Bambu_Tunnel*`-look-alike that talks TLS to the bridge's `VirtualTunnelServer` for FFFF dev-ids. `PrinterFileSystem` swaps this in transparently. |

### `src/slic3r/GUI/GUI_App.{cpp,hpp}` — `--bridge-only` mode

When `g_bridge_only`, `GUI_App`:

- Skips `Label::initSysFont`, `ImGuiWrapper`, `HMSQuery`,
  `RemovableDriveManager`, `OtherInstanceMessageHandler` (ctor gates).
- Calls `init_bridge_only_headless()` from `on_init_inner` instead of
  building MainFrame/Plater/PresetBundle.
- Installs `init_networking_callbacks_bridge_only()` (three callbacks:
  `set_on_message_fn`, `set_on_local_message_fn`,
  `set_queue_on_main_fn`) — the full version would deref
  `plater()`/`mainframe`/`m_server_error_dialog`.
- Constructs `BridgeStorageBackend` + `BridgeApp`, registers
  `VirtualMqttClient::set_port_resolver` pointing at
  `BridgeApp::mqtt_port_for_dev_id`.
- Runs the 5 s `wxTimer` push pump that feeds DeviceManager snapshots
  into `BridgeApp::set_virtual_printers`.
- In `OnExit`, after stopping the worker, calls `std::_Exit(0)` to skip
  the static-dtor unwind (a heap corruption inside the proprietary
  plugin's static teardown would otherwise abort).
- `current_language_code_safe()` early-returns `"en_US"` when
  `m_wxLocale` is null (bridge-only skips `load_language`).

### `src/slic3r/Utils/NetworkAgent.{cpp,hpp}`

- `set_on_message_fn` wraps the slicer's callback with a fanout that
  also fires the registered `BridgeMessageTap` for non-virtual dev-ids.
- `connect_printer` / `send_message_to_printer` / `disconnect_printer`
  branch on `is_virtual_dev_id(dev_id)` (prefix `"FFFF"`) to route
  through `VirtualMqttClient` instead of the plugin.
- Suppresses plugin-originated `on_local_connect` for virtual dev-ids
  (the plugin's SSDP auto-discovery would otherwise try Bambu-CA-chain
  verifying the bridge's self-signed cert and call `Failed`,
  flushing our entry out of the UI).

### `src/BambuStudio.cpp`

- `--bridge-only` CLI prepass before `set_current_thread_name` /
  `XInitThreads` so the headless path doesn't trip wxGTK init.
- Probes for `slicer_base64.cer` and `SSL_CERT_FILE` so the plugin's
  curl session has a valid CA bundle.
- Sets `resources_dir` / `var_dir` / `local_dir` / `sys_shapes_dir`
  before constructing `GUI_App`.

### `tests/bridge/`

Self-contained gtests:

- Loopback: `MqttBrokerLoopbackTest`, `FtpsServerLoopbackTest`,
  `RtspServerLoopbackTest`, `SsdpResponderLoopbackTest`,
  `VirtualTunnelLoopbackTest`.
- Integration: `SessionRouterIntegrationTest`,
  `UplinkHealthIntegrationTest`, `BridgeAppMultiDeviceTest`,
  `BridgeServiceTest`.
- Authentication: `FtpsAuthTest`, `MqttBrokerAuthTest`,
  `CertFactoryTest`.
- Harness mocks: `harness/`, `mocks/` — `A1Printer`, `H2SPrinter`,
  `H2DPrinter` finite-state models + `MockPlugin` + `MockBambuSource`
  for hermetic E2E (`HarnessMockA1Test`, `HarnessMockH2STest`,
  `HarnessMockH2DTest`, `HarnessShimRecorderTest`,
  `HarnessTraceMatchTest`).
- `BridgeAppCliArgsTest` covers the shared CLI parser.

### `tools/`

| File | Role |
| --- | --- |
| `vtun_test_client.py` | Connects to the vtun port with a TLS handshake + access_code so you can poke storage RPCs without booting a slicer. |
| `wire_diff/` | Normalises and diffs captured wire traffic (SSDP / MQTT / FTPS / RTSP / vtun). Useful when comparing a real-printer session against a bridged one. |
| `no_debugger.c` | LD_PRELOAD shim that defeats anti-debug `ptrace(PTRACE_TRACEME)` calls in the proprietary `libbambu_networking.so`. |

---

## Behaviour

Startup (cold, in `--bridge-only` mode against a logged-in slicer):

| Milestone | Wall-time |
| --- | --- |
| Servers listening (SSDP/MQTT/FTPS/RTSP/vtun) | ≈ 2.3 s |
| 3/3 LAN uplinks `connect_printer rc=0` | ≈ 7.4 s |
| Slicer probe receives `push_status` per virtual dev-id | 1–4 s after connect |

Steady-state: ~157 MB RSS, ~60 threads (boost::asio + plugin + network
agent worker pools), no WebKit subprocesses, no zenity subprocesses.
SIGINT → exit in ~1.5 s via the `std::_Exit(0)` shortcut.

---

## What's on the next branch up (`bridge-debug`)

`bridge-debug` stacks one commit on top: ~302 `fprintf(stderr,
"[<tag>] …")` log statements, `Verbose.hpp` and `BambuTrace.hpp` helpers
(env-gated), and the `SIGABRT` backtrace trampoline. See
`bambu_bridge_debugging.md` on that branch for the inventory.
