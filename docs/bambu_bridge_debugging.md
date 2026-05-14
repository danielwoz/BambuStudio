# Bambu Bridge — debug instrumentation (`bridge-debug`)

This branch stacks one commit on `bridge-necessary` that puts back all
the diagnostic scaffolding that built the bridge: stderr log lines, env-
gated trace helpers, and an abort-time backtrace trampoline. Reverting
that single commit returns to the silent bridge.

Total delta vs `bridge-necessary`: **+1,485 lines across 44 files**.

---

## 1. Stderr log lines (302 `fprintf` statements, 42 files)

Pattern: `std::fprintf(stderr, "[<tag>] …", …);`. Always on, never
silenceable. They report bridge state in real time and were how every
problem in this branch's history got triaged. Tags are stable so
operators can `grep -E '\[(bridge-app|lan-uplink|…)\]'` over a captured
log.

Tag → what it logs:

| Tag | File(s) | Reports |
| --- | --- | --- |
| `[bridge-app]` | `bambu_bridge/headless/BridgeApp.cpp` | Lifecycle (servers up / down, device add / remove / `lan_ip` flips, inventory reconcile, plugin handle attach). |
| `[bambu-bridge]` | `BambuStudio.cpp`, `slic3r/GUI/GUI_App.cpp` | `--bridge-only` boot + worker thread start/exit + signal handling. |
| `[bridge-cli]` | `bambu_bridge/cli/bridge_cli.cpp` | Standalone CLI output. |
| `[lan-uplink]` | `bambu_bridge/router/LanUplink.cpp` | `connect_printer` / `send_message_to_printer` calls + return codes. |
| `[cloud-uplink]` | `bambu_bridge/router/CloudUplink.cpp` | Inbound cloud reports with downstream-attached y/n. |
| `[lan-upload-sink]` / `[cloud-upload-sink]` / `[upload-router]` | `router/*UploadSink*.cpp`, `router/UploadSinkRouter.cpp` | Upload pipeline progress. |
| `[lan-camera-source]` / `[cloud-camera-source]` / `[camera-router]` | `router/*CameraSource.cpp`, `router/CameraSourceRouter.cpp` | Bambu_Tunnel create/open/start_stream rc + retries. |
| `[session-router]` | `bambu_bridge/router/SessionRouter.cpp` | LAN vs Cloud route picks. |
| `[uplink-health]` | `bambu_bridge/router/UplinkHealth.cpp` | Health snapshot transitions. |
| `[ssdp-responder]` / `[ssdp-listener]` | `bambu_bridge/server/Ssdp*.cpp` | NOTIFY/M-SEARCH traffic + LAN IP discoveries. |
| `[mqtt-broker]` | `bambu_bridge/server/MqttBroker.cpp` | CONNECT auth fails, decode errors, max-clients rejections, unexpected packet types. |
| `[ftps-server]` / `[ftps-cli]` | `bambu_bridge/server/FtpsServer.cpp` | TLS / FTP control-channel events. |
| `[rtsp-server]` / `[rtsp-loopback]` | `bambu_bridge/server/RtspServer.cpp` | RTSP DESCRIBE / SETUP / PLAY / TEARDOWN. |
| `[virtual-tunnel]` | `bambu_bridge/server/VirtualTunnelServer.cpp` | Per-dev-id accept loop, vtun session lifecycle, storage-delegate attach. |
| `[null-uplink]` / `[null-upload-sink]` | `bambu_bridge/router/Null*.cpp` | Stub paths in advertise-only / test mode. |
| `[network-agent]` | `slic3r/Utils/NetworkAgent.cpp` | `VIRTUAL connect_printer` / `send_message` / `disconnect_printer` for FFFF dev-ids, plus suppressed `on_local_connect` events. |
| `[virtual-mqtt]` | `slic3r/Utils/VirtualMqttClient.cpp` | TCP/TLS handshake, CONNACK, PUBLISH, reconnect backoff. |
| `[virtual-ftps]` | `slic3r/Utils/VirtualFtpsClient.cpp` | FTPS client-side TLS + control-channel events. |
| `[virtual-store]` | `slic3r/Utils/VirtualLanPrinterStore.cpp` | Persisted FFFF entries hydrated on slicer launch. |
| `[bridge-storage]` | `slic3r/GUI/Printer/BridgeStorageBackend.cpp` | JSON-RPC frames between vtun server and PrinterFileSystem. |
| `[pfs]` | `slic3r/GUI/Printer/PrinterFileSystem.cpp` | PFS state transitions when a virtual dev-id is involved. |
| `[adapter]` | `slic3r/Utils/NetworkAgentPluginAdapter.cpp` | Plugin-handle adapter wiring. |
| `[plugin]` | `bambu_bridge/BambuNetworkingPluginHandle.cpp` | Plugin dlopen + symbol resolution. |
| `[cloud-inv]` | `bambu_bridge/CloudInventory.cpp` | Inventory REST poll responses. |
| `[click]` | `slic3r/GUI/GUI_App.cpp` (debug filter event) | Left-click event logger (used while driving the slicer GUI from x11vnc). |
| `[trampoline]` | `slic3r/GUI/GUI_App.cpp` | SIGABRT handler emission. |
| `[trace]` | `slic3r/Utils/BambuTrace.hpp` | BS_TRACE macro emission (see §2). |

A typical clean run:

```
[bambu-bridge] --bridge-only: GUI_App entered headless mode
[bridge-app] using injected plugin handle
[bridge-app] BambuSource library loaded
[bridge-app] servers up: ssdp=1 mqtt=1 ftps=1 rtsp=1 vtun=1 bind=0.0.0.0
[ssdp-listener] dev_id=0938BC… lan_ip: (none) -> 192.168.1.209
[bridge-app] dev_id=0938BC… lan_ip flipped  -> 192.168.1.209; reconfiguring LAN endpoints
[lan-uplink] connect_printer dev_id=0938BC… rc=0 local_connected=1
[network-agent] VIRTUAL connect_printer dev_id=FFFFBC… ip=192.168.1.151
[virtual-mqtt] CONNACK for dev_id=FFFFBC… — subscribing
[virtual-mqtt] PUBLISH dev_id=FFFFBC… topic=device/FFFFBC…/report payload_len=22301
…
[bambu-bridge] --bridge-only: signal received, exiting wx loop
[bambu-bridge] --bridge-only worker exited rc=0
[bambu-bridge] --bridge-only: clean exit (skipping static dtors — see GUI_App::OnExit for rationale)
```

---

## 2. Env-gated trace helpers

### `src/bambu_bridge/Verbose.hpp` — `Slic3r::bridge::verbose()`

One-shot probe of `BAMBU_BRIDGE_VERBOSE`. Cached at first call.
Bridge-side code that emits chatty per-poll diagnostics (inventory
body previews, reconcile-snapshot lines) wraps them in
`if (Slic3r::bridge::verbose()) { … }`. Silent by default; chatty when
the operator opts in with:

```
BAMBU_BRIDGE_VERBOSE=1 bambu-studio --bridge-only
```

Startup-once logs (servers up, listener bound, plugin symbol
resolution) stay unconditional — those land in §1 above.

### `src/slic3r/Utils/BambuTrace.hpp` — `BS_TRACE` / `BS_TRACE_ENTER`

Two macros for tracing method entry. Gated on `BAMBU_TRACE`:

```cpp
BS_TRACE_ENTER("NA::connect_printer");
BS_TRACE("NA::connect_printer", "dev_id=%s ip=%s",
         dev_id.c_str(), dev_ip.c_str());
```

Off, the macros expand to nothing (zero overhead beyond the env probe,
which is cached). On with `BAMBU_TRACE=1`, every traced call emits one
stderr line:

```
[trace] NA::connect_printer dev_id=03900D… ip=192.168.1.247
```

Used to capture a "golden" trace from a real-printer slicer session and
diff it against a virtual-printer session to find what's missing.

`BS_TRACE_ENTER` is scattered across `DeviceManager.cpp` (170+ calls)
and `DeviceCore/DevManager.cpp` (27 calls) — every public method that
takes a `MachineObject*`.

---

## 3. SIGABRT backtrace trampoline

Installed in `GUI_App::init_bridge_only_headless` early during boot.
glibc's malloc heap-corruption check raises `SIGABRT`; without a
handler we'd see only the bare `malloc(): mismatching next->prev_size`
line and a SIGKILL. The trampoline writes:

```
[bambu-bridge] SIGABRT — backtrace:
<backtrace_symbols_fd output>
```

`SA_RESETHAND | SA_NODEFER` so the kernel's default core dump still
fires after the handler returns. `backtrace_symbols_fd` is the
async-signal-safe variant (no malloc from inside a signal handler).

In practice the heap is corrupted by the time the handler runs, so
the symbol output is sometimes empty. The trampoline is kept anyway —
useful for any future malloc abort that happens earlier.

---

## 4. `[bambu-bridge] --bridge-only: clean exit (…)`

Last log line before `std::_Exit(0)`. Tells the operator (and any
service supervisor reading the log) the process is exiting on purpose,
without static-dtor unwind. Paired with the comment in
`GUI_App::OnExit` that explains why.

---

## 5. Inventory of the stripped vs added lines

| Category | Files touched | Net adds |
| --- | --- | --- |
| `fprintf(stderr, "[<bridge-tag>] …")` | 42 | +302 |
| `BS_TRACE_ENTER(…)` call sites | 2 | +198 |
| Empty `if (verbose()) { }` blocks (post-strip residue elsewhere) | — | — |
| SIGABRT trampoline + `<execinfo.h>` / `<csignal>` includes | 1 | +30 |
| `Verbose.hpp` (env-probe helper) | new | +30 |
| `BambuTrace.hpp` (macros + helper) | new | +60 |
| Misc `#include` lines for the above | ~6 | +6 |

The `bridge-necessary` branch has zero of any of the above. Diff
against `bridge-debug^1` (the necessary HEAD) shows exactly what
returns when this overlay is applied.
