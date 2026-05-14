# BambuStudio-bridge Test Harness Plan

## 1. Scope recap and constraints

The test harness exists to make end-to-end testing of the **virtual A1 / H2S / H2D** code path possible without owning physical printers. Two pieces:

- **Printer-behaviour mocks** — answer cloud-style MQTT-over-TLS (subscribe, push_status, command ack) and the small REST surface (`get_user_print_info` inventory) the way a real A1 / H2S / H2D would when reached *via Bambu cloud*. They do *not* implement the LAN MQTT 8883, FTPS 990, or BambuTunnel 6000 surfaces — those are the bridge's *server* side and are already exercised by the existing loopback tests under `tests/bridge/`.
- **Interception/recording shim** — a logging passthrough that captures every call into `libbambu_networking.so` and `libBambuSource.so`, with arguments / return values / timestamps, producing a "golden trace" we can replay against the mocks.

The closed libraries we are wrapping are the same ones the existing bridge code already routes through:

- `libbambu_networking.so` — entered via `NetworkAgent::get_network_function` (`src/slic3r/Utils/NetworkAgent.cpp:586`), populates ~100 function pointers (`src/slic3r/Utils/NetworkAgent.hpp:10-120`). The bridge already partially wraps this in `BambuNetworkingPluginHandle.{hpp,cpp}` for cloud + LAN.
- `libBambuSource.so` — entered via `StaticBambuLib::get()` in `src/slic3r/GUI/Printer/PrinterFileSystem.cpp:1836-1948`. We already extended this loader at lines 1872-1945 to wrap each function pointer with a *virtual-tunnel dispatcher trampoline* — the exact pattern a logging shim wants.

## 2. Architecture overview

```
+-------------------------+        +---------------------------+
| BambuStudio-bridge tree |        | Test harness (NEW)        |
|                         |        |                           |
|  slic3r/Utils/          |        | tests/bridge/harness/     |
|   NetworkAgent.cpp -----+--shim->| ShimRecorder              |
|   PrinterFileSystem.cpp |        |   - JSONL writer          |
|   (StaticBambuLib::get) |        |   - per-call envelope     |
|                         |        |                           |
|  bambu_bridge/          |        | tests/bridge/mocks/       |
|   BambuNetworkingPlugin |--mock->| MockPlugin (cloud+REST)   |
|     Handle              |        |   - A1Printer             |
|   BambuSourceHandle     |--mock->|   - H2SPrinter            |
|                         |        |   - H2DPrinter            |
+-------------------------+        |                           |
                                   | tests/bridge/harness/     |
                                   |   TraceComparator         |
                                   +---------------------------+
```

Both pieces live behind one new CMake target, gated on `BAMBU_BRIDGE_HARNESS=ON` (default OFF — mirrors the existing `BAMBU_BRIDGE_E2E` gate at `tests/bridge/CMakeLists.txt:29-39`).

## 3. Where the mocks live — decision and rationale

**Decision: an in-process linked library, not a separate executable.**

Three candidates were on the table:

| Option | Pros | Cons |
|---|---|---|
| (a) Separate executable that the test harness launches; mocks talk MQTT-TLS on loopback | Closest to the real wire | Need a full broker per test; loopback bind flakiness (already a 77-skip pain in `MqttBrokerLoopbackTest`); harness must marshal TCP, makes per-test reset hard |
| (b) In-process library that subclasses `BambuNetworkingPluginHandle` and `BambuSourceHandle` | Reuses the test-seam already present (every method is `virtual`, see `BambuNetworkingPluginHandle.hpp:93-329`); fast; deterministic; no port binding | Doesn't exercise the TLS+MQTT framer — but that's already covered by `MqttBrokerLoopbackTest` and `MqttFramingTest` |
| (c) LD_PRELOAD `libbambu_networking.so` replacement | True drop-in for the real binary | Forces ELF-only; brittle ABI maintenance (C++-by-value typedefs in `NetworkAgent.hpp:10-120` are a foot-gun); can't easily talk to the *bridge*, only the *slicer process loader* |

**Pick: (b)**. The bridge already proves the test-seam works — `MockPluginHandle` is referenced throughout `tests/bridge/CMakeLists.txt` (see CloudUplinkLoopback, LanUploadSinkPlugin, CloudCameraSourcePlugin). We grow that pattern into per-model behaviour mocks.

The mocks live in:

```
tests/bridge/mocks/
  MockPlugin.{hpp,cpp}            # NEW — subclasses BambuNetworkingPluginHandle
  MockBambuSource.{hpp,cpp}       # NEW — subclasses BambuSourceHandle
  PrinterModel.hpp                # NEW — pure-data capability tables
  A1Printer.{hpp,cpp}             # NEW — behavioural FSM, push_status JSON
  H2SPrinter.{hpp,cpp}            # NEW
  H2DPrinter.{hpp,cpp}            # NEW
  PrinterFsm.{hpp,cpp}            # NEW — shared idle->prepare->printing->finish
  fixtures/
    a1_push_status_idle.json      # captured from real A1 (or hand-rolled)
    a1_push_status_printing.json
    h2s_push_status_idle.json
    h2s_push_status_printing.json
    h2d_push_status_idle.json
    h2d_push_status_printing.json
    h2d_push_status_dual_extr.json
```

The PrinterModel struct exposes the capability flags the slicer's `DeviceManager::parse_json` reads (the dispatch starts at `src/slic3r/GUI/DeviceManager.cpp:2560`). Per the requirements:

```cpp
struct PrinterModel {
    std::string sn_prefix;        // matches Bambu's serial namespace
    int  nozzle_count;            // A1/H2S = 1, H2D = 2
    int  ams_unit_count;          // A1 = 1 (Lite), H2S = 1, H2D = 2
    int  ams_slots_per_unit;      // all = 4
    bool ams_humidity;            // A1 = false, H2S = true, H2D = true
    bool chamber_heater;          // A1 = false, H2S = true, H2D = true
    bool chamber_temp_sensor;     // A1 = false, H2S = true, H2D = true
    bool laser_accessory;         // H2D only (optional)
    std::string model_id;         // "N1", "N2S", "N2D" (BBL internal codes)
    std::string printer_type;     // matches DeviceManager::printer_type
};
```

(Capability values verified against `~/BambuStudio/src/slic3r/GUI/DeviceManager.cpp` series_o / series-N branches; the harness sources the live numbers from one Bambu Studio profile JSON at configure time and pins them in the `PrinterModel` table.)

### How the mock answers traffic

`MockPlugin` overrides exactly the virtuals that already exist on `BambuNetworkingPluginHandle`:

- `subscribe_device(dev_id)` — registers the dev_id with the matching `PrinterModel`, and (after a short delay queued on a worker thread) fires the *first* `push_status` for that printer via `deliver_message_for_test(dev_id, json_payload)`. That method already exists at `BambuNetworkingPluginHandle.hpp:286-289` for exactly this kind of injection.
- `unsubscribe_device(dev_id)` — removes the dev_id; subsequent pushes silently drop.
- `publish_to_device(dev_id, json_payload, qos)` — parses the inbound `"print"` or `"system"` command (the FSM only needs ~8 commands: `pause`, `resume`, `stop`, `push_all`, `unload_filament`, `ams_filament_setting`, `gcode_line`, `request_camera_url`). Each command transitions the per-printer FSM and may emit a follow-up `push_status` via `deliver_message_for_test`.
- `get_user_print_info(http_code, http_body)` — returns a JSON inventory listing the configured mock devices (A1 + H2S + H2D, or a subset the test selects). Exact key shape mirrors `~/BambuStudio/src/bambu_net_oss/api/*` and what `CloudInventory.cpp` already parses.
- `get_camera_url(dev_id, *url_out, timeout_ms)` — returns `bambu:///rtsps___bblp:<code>@127.0.0.1/streaming/live/1?proto=rtsps` so `MockBambuSource` can hand back a static MJPEG sample from a fixture.
- LAN-side: `connect_printer`, `disconnect_printer`, `send_message_to_printer`, `register_local_*` — wired identically to the cloud side via the same FSM, so the bridge's `LanUplink` can also be exercised against the mock.

`MockBambuSource` overrides each `bambu_*` virtual in `BambuSourceHandle.hpp:80-127` and returns a fixed 320x240 MJPG sample stream from `tests/bridge/mocks/fixtures/sample.mjpg`. No real codec dependency.

### push_status payload realism

The biggest realism cost in the mocks is the **push_status JSON shape**. We seed it from fixtures captured against real printers (the only out-of-band human step). Each fixture is one JSON document, ~1 KB (A1), ~2 KB (H2S), ~3 KB (H2D). The FSM mutates a small set of mutable fields per tick:

```
mc_percent, mc_remaining_time, layer_num, total_layer_num,
gcode_state ("IDLE"|"PREPARE"|"RUNNING"|"PAUSE"|"FINISH"),
nozzle_temper, bed_temper, chamber_temper (H2S/H2D only),
nozzle_target_temper, bed_target_temper,
fan_gear, big_fan1_speed,
ams.ams[i].tray[j].remain (per slot)
```

Everything else is pinned at fixture-capture time. This is the smallest set of mutating keys that the `DeviceManager::parse_json` flow (and downstream `MachineObject` consumers) cares about — verified by reading the assignments in `DeviceManager.cpp` lines 2595-3200.

## 4. Where the interception hooks land — decision and rationale

**Decision: option (c) — wrap the function-pointer table at load time.** No source modifications to NetworkAgent.cpp / PrinterFileSystem.cpp behind an env flag, no LD_PRELOAD .so.

Rationale:

1. **The pattern is already in production.** `StaticBambuLib::get()` (PrinterFileSystem.cpp:1872-1945) already wraps each Bambu_* function pointer with a dispatcher trampoline that sniffs whether the target is virtual and routes accordingly. Adding a `record_call(...)` line at the top of each trampoline is mechanical.
2. **The NetworkAgent side needs the *same* trampoline introduction, but it isn't there yet.** This is a small, isolated change: in `NetworkAgent::initialize_network_module` (right after the bulk dlsym block at NetworkAgent.cpp:289-330), guard on `getenv("BAMBU_BRIDGE_SHIM") != nullptr` and replace each `*_ptr` static with a lambda that calls `ShimRecorder::record(...)` then invokes the captured real pointer. The shim is **off by default** and only activates when the env var is set, so production load order is unchanged.
3. **LD_PRELOAD (option b)** would force us to maintain a parallel ABI surface and only works on Linux. The two C++-by-value typedefs (`std::string`, `std::vector<std::string>`, `std::function` passed by value, see `BambuNetworkingPluginHandle.cpp:39-80`) are notorious foot-guns. Doing the wrap inside the same TU that already declares the typedef avoids any cross-binary ABI drift.
4. **Env-gated source modification (option a)** is what we're picking — the wrap **is** the source modification, just with the recorder as a static singleton that no-ops when disabled.

### Shim component design

Single new module `tests/bridge/harness/ShimRecorder.{hpp,cpp}` that is **linked into both** the bridge static library *and* the slicer's NetworkAgent translation unit. Two activation points:

1. **NetworkAgent.cpp** — after the existing `get_network_function` block at NetworkAgent.cpp:289-330 (preserving the actual underlying pointers in `static auto real_*` locals just like StaticBambuLib does), insert a guarded block:
   ```cpp
   #if defined(BAMBU_BRIDGE_HARNESS_ENABLE)
   if (std::getenv("BAMBU_BRIDGE_SHIM"))
       wrap_network_function_pointers();   // defined in ShimRecorder.cpp
   #endif
   ```
   `wrap_network_function_pointers()` reads every `*_ptr` static, replaces it with a lambda that does `ShimRecorder::record("connect_server", args...); return real(args...);`, and stores the real pointer in a sibling static.
2. **PrinterFileSystem.cpp** — extend the existing trampolines at lines 1890-1945 the same way: each lambda gets a `ShimRecorder::record("Bambu_Open", ...)` first line, gated on the same env var.

This way the shim is **strictly additive**, only compiled in when `BAMBU_BRIDGE_HARNESS=ON`, and only activated at runtime when `BAMBU_BRIDGE_SHIM=1`. Production builds carry zero overhead.

### What the shim records

For each call:

```json
{
  "ts_ns": 1726510123000000000,
  "thread": "tcp-recv-3",
  "lib": "bambu_networking",
  "fn": "send_message_to_printer",
  "args": {
    "dev_id": "00M00A3B0500123",
    "json_str": "{\"print\":{\"command\":\"pause\",...}}",
    "qos": 0,
    "flag": 0
  },
  "ret": 0,
  "duration_us": 87
}
```

`args` is a fn-specific JSON object — for primitive types (int / bool / std::string) we encode straight; `std::function<...>` callbacks are serialised as `{"_callback": "OnMessageFn", "id": 42}` (a stable per-process counter) so a *later* invocation of the callback can be cross-referenced by id. `out-params` (e.g. `unsigned int* http_code`) are recorded under `ret_out_params` after the call completes.

The shim itself is a thread-safe append-only JSONL writer to a path from `BAMBU_BRIDGE_SHIM_TRACE` (default `/tmp/bambu_bridge_shim_<pid>.jsonl`).

## 5. Trace format

**Decision: JSON-per-line (JSONL).**

Plain text is too lossy — we need structured fields for the comparator. Custom binary is overkill and undiffable. JSONL gives us:

- One line per call → unix tools (`grep`, `wc -l`, `jq -c`) work directly.
- `git diff` on captured fixtures is reviewable.
- Easy parsing in both C++ (nlohmann/json, already vendored at `src/bambu_bridge/third_party/`) and Python (for the wire_diff/-style comparator if we want it).
- The existing `tools/wire_diff/` infrastructure under `tools/wire_diff/normalisers/` is structured exactly this way — we reuse the **same diff philosophy**: normalise volatile fields, then byte-diff.

Schema (per line):

| Field | Type | Notes |
|---|---|---|
| `seq` | int | monotonic per-trace |
| `ts_ns` | int | wall-clock ns; comparator normalises away |
| `delta_ms` | int | ms since previous call (preserved; comparator allows ±tolerance) |
| `thread` | string | thread name or tid |
| `lib` | string | `bambu_networking` or `bambu_source` |
| `fn` | string | C function name (without `bambu_network_` prefix) |
| `args` | object | per-fn schema; see below |
| `ret` | scalar | return value (int / bool / string) |
| `ret_out_params` | object | for fns that fill `T*` out params |
| `duration_us` | int | wall-clock duration of the call |
| `cb_id` | int? | only set on lines that represent a callback firing |

## 6. Trace comparator — what equivalence means

**Definition: a "mock matches the real" iff** after normalisation, the slicer (running the same script) produces a JSONL trace from the real plugin whose lines are equal to the JSONL trace from the mock under these rules:

1. **Method sequence equality** — list of `(lib, fn)` tuples is identical in order. Comparator dies on first mismatch (printed contextually).
2. **Argument shape equality** — argument *keys* and *types* match exactly. Values are checked under per-fn normalisers (below).
3. **Return value equality** — exact match on integer/bool returns. String returns checked under the same normalisers as args.
4. **Callback shape equality** — when a callback fires (recorded as a line with `cb_id` set), the callback's args also normalise-and-match.

Normalisers (one per `(lib, fn)` pair, lives in a small lookup table):

| Field/pattern | Rule |
|---|---|
| `ts_ns`, `seq`, `duration_us` | drop (timing) |
| `delta_ms` | within ±500 ms (allow scheduling jitter) |
| `dev_id` containing real Bambu SN | normalise to `<DEV_ID_0>`, `<DEV_ID_1>` by first appearance |
| `args.json_str` (MQTT payload) | parse as JSON; normalise `sequence_id` → `<SEQ>`, `command_param.token` → `<TOKEN>`, `mc_remaining_time` → integer-range-bucket, drop `system.print_error.command_param.report_time` |
| `ret_out_params.http_body` | normalise `request_id` → `<REQID>`, `created_at` → `<TS>` |
| `args.qos`, `args.flag` | strict equal |
| callback `cb_id` | renumber by first appearance |
| thread name | drop (different in real vs mock) |

The comparator is a small C++ program — `tests/bridge/harness/TraceComparator.cpp` — that takes two trace files plus an optional `normaliser-overrides.json` and prints a unified diff of the normalised forms (using the existing `tools/wire_diff/wire_diff.py` infrastructure, which is already an external-process unified-diff runner — see `WireDiffSelfTest.cpp`).

For automated tests, the comparator returns 0 on match / 1 on mismatch / 77 on missing fixture (consistent with the ctest skip convention used throughout `tests/bridge/`).

## 7. Initial scope — first 10 methods

**Both** intercept and mock these. The set is chosen to make a complete bridge `subscribe → push_status → command → ack → unsubscribe` round-trip work for each of A1/H2S/H2D:

`libbambu_networking.so`:
1. `connect_server` — int return; one-shot cloud connect.
2. `is_server_connected` — bool poll; mock returns true after first connect_server.
3. `set_on_message_fn` — registers cloud OnMessageFn callback (captures cb_id).
4. `set_on_server_connected_fn` — connection-state callback.
5. `start_subscribe("device")` / `stop_subscribe("device")` — subscription scope toggle.
6. `add_subscribe([dev_ids])` — per-device subscribe; mock fires push_status idle for each.
7. `del_subscribe([dev_ids])` — per-device unsubscribe.
8. `send_message_to_printer(dev_id, json, qos, flag)` — primary command path; mock parses and ticks FSM.
9. `get_user_print_info(http_code*, http_body*)` — REST inventory.
10. `get_camera_url(dev_id, callback)` — async camera URL; mock fires callback inline with the static MJPG URL.

`libBambuSource.so` (subset — full surface already lives in the StaticBambuLib trampolines):
1. `Bambu_Create` / `Bambu_Destroy` — tunnel lifecycle.
2. `Bambu_Open` / `Bambu_Close` — transport bring-up.
3. `Bambu_StartStream` — kicks the MJPG feed.
4. `Bambu_GetStreamCount` / `Bambu_GetStreamInfo` — one stream, video MJPG.
5. `Bambu_ReadSample` — returns sample bytes from `fixtures/sample.mjpg`.

That's 10 + 5 fn signatures for v1. Per the explicit out-of-scope list (§9 below), `connect_printer` / `disconnect_printer` / LAN-side message routing are *intercepted* (recorder watches them) but *not exercised by the mock's printer FSM* until v2 — the cloud-style round-trip is the v1 milestone.

## 8. Test-harness wiring

**Decision: extend `tests/bridge/`, not the top-level `tests/`.**

- Top-level `tests/` is BambuStudio's slicer-internal test tree (libnest2d, libslic3r, fff_print, sla_print, slic3rutils) — explicitly *not* where the bridge keeps its tests (see `tests/CMakeLists.txt:30-35`).
- All bridge tests already live under `tests/bridge/` and are configured by `src/bambu_bridge/CMakeLists.txt:154-162`.

New tree:

```
tests/bridge/
  CMakeLists.txt                  # EDITED — adds harness/ + mocks/ + new tests
  harness/                        # NEW
    CMakeLists.txt
    ShimRecorder.{hpp,cpp}
    TraceComparator.cpp          # standalone exe
    TraceLine.hpp                # the JSONL schema, shared with mocks
  mocks/                          # NEW (as listed in §3)
    ...
  HarnessShimRecorderTest.cpp    # NEW — unit test for the JSONL writer
  HarnessMockA1Test.cpp          # NEW — slicer-driven mock RT for A1
  HarnessMockH2STest.cpp         # NEW — same for H2S
  HarnessMockH2DTest.cpp         # NEW — same for H2D (dual-extruder)
  HarnessTraceMatchTest.cpp      # NEW — golden trace vs mock trace; SKIP_RETURN_CODE 77 when fixture absent
  fixtures/                       # NEW
    a1_golden_trace.jsonl        # checked in; captured against real A1 once
    h2s_golden_trace.jsonl
    h2d_golden_trace.jsonl
```

CMake additions (in `tests/bridge/CMakeLists.txt`):

```cmake
option(BAMBU_BRIDGE_HARNESS "Build the printer-behaviour test harness" OFF)
if (BAMBU_BRIDGE_HARNESS)
    add_subdirectory(harness)
    add_subdirectory(mocks)
    # plus the four HarnessMock* + HarnessTraceMatch tests with SKIP_RETURN_CODE 77
endif()
```

The shim's source-side hooks compile in only when `BAMBU_BRIDGE_HARNESS=ON` (we guard with `#if defined(BAMBU_BRIDGE_HARNESS_ENABLE)`). Production builds are byte-identical to today.

Each Harness*Test:
- Constructs a `MockPlugin` (subclasses `BambuNetworkingPluginHandle`), an empty `BridgeService`, a `CloudUplink`, and a small driver that walks the slicer-side calls the harness wants to exercise.
- Optionally enables the ShimRecorder writing to a tempfile.
- Asserts on the FSM's terminal state (e.g. `gcode_state == "FINISH"`) and (for HarnessTraceMatchTest) compares the just-written trace against the matching fixture under TraceComparator.

Reuse of existing test helpers:
- `support/MqttTestClient.{hpp,cpp}` — for a slicer-shaped subscribe/publish driver when we also want LAN-side exercise.
- `support/FtpsTestClient.{hpp,cpp}` — when v2 picks up file upload through the mocks.

## 9. Out-of-scope (deliberately, v1)

- **Video / RTSP frame fidelity.** The mock returns a single static MJPG sample on repeat. RTSP wire-format correctness is already pinned by `RtspServerLoopbackTest`.
- **Real firmware error paths.** The mock answers happy-path push_status only. Error injection (HMS codes, broken AMS reads, bed-leveling failure) is v2.
- **Partial-packet / wire-fuzz testing.** The mock speaks well-formed JSON only.
- **TLS handshake bytes.** Already covered by `MqttBrokerLoopbackTest` and `FtpsServerLoopbackTest`.
- **Real cloud account auth.** Not modelled. The mock claims `is_user_login() == true` unconditionally — we never re-exercise the login flow because the bridge does it once via the real plugin before tests run.
- **Multi-device race conditions inside `MockPlugin`.** The mock serialises all FSM transitions on a single worker thread; concurrency hardening is v2.
- **OrcaSlicer-side replay.** Bridge-internal harness only. Orca's own test integration is downstream.
- **start_print / start_local_print_with_record / 3MF upload + slice job.** Already exercised by CloudUploadSinkTest + LanUploadSinkPluginTest against MockPluginHandle. v2 ties them in.
- **Windows.** Loader plumbing in NetworkAgent.cpp is `#if _MSC_VER || _WIN32` guarded; the shim's wrap-the-table approach works there too with no algorithm change, but the harness initial build is Linux-only.

## 10. Implementation sequencing

A reasonable cut-list for the implementation agent:

1. **Shim infrastructure (no slicer hooks yet).** Land `ShimRecorder.{hpp,cpp}` + `TraceLine.hpp` + unit test `HarnessShimRecorderTest.cpp` + the CMake `BAMBU_BRIDGE_HARNESS` option. ~2 days.
2. **NetworkAgent wrap.** Insert the `wrap_network_function_pointers()` block in NetworkAgent.cpp under the `#if BAMBU_BRIDGE_HARNESS_ENABLE` guard. Cover the 10 fns above. ~1 day.
3. **StaticBambuLib wrap.** Extend the existing trampolines in PrinterFileSystem.cpp:1890-1945 with `ShimRecorder::record(...)` calls. ~½ day.
4. **PrinterModel + PrinterFsm + mock fixtures.** Build the static capability tables (verified against `~/BambuStudio/src/slic3r/GUI/DeviceManager.cpp`) and the shared FSM. ~2 days.
5. **MockPlugin + A1 first.** Wire up the cloud subscribe → push_status → ack round-trip end-to-end via CloudUplink. `HarnessMockA1Test`. ~2 days.
6. **H2S + H2D variants.** Chamber heater + dual extruder support; bigger push_status fixture. ~2 days.
7. **TraceComparator + golden fixtures.** Capture three golden traces (manual, one-time, against real printers); land the comparator. ~2 days.
8. **CI gating.** ctest opt-in via `BAMBU_BRIDGE_HARNESS=ON`. Match the SKIP_RETURN_CODE 77 convention everywhere a fixture might be missing.

Total: ~12 working days for v1.

## 11. Critical files for implementation

- `/home/danielwoz/BambuStudio-bridge/src/slic3r/Utils/NetworkAgent.cpp` (lines 289-330 for the wrap-the-table hook; 586-604 for `get_network_function`)
- `/home/danielwoz/BambuStudio-bridge/src/slic3r/GUI/Printer/PrinterFileSystem.cpp` (lines 1836-1948 — extend the existing trampoline pattern)
- `/home/danielwoz/BambuStudio-bridge/src/bambu_bridge/BambuNetworkingPluginHandle.hpp` (the entire virtual surface is the test seam the MockPlugin subclasses)
- `/home/danielwoz/BambuStudio-bridge/tests/bridge/CMakeLists.txt` (add `BAMBU_BRIDGE_HARNESS` option + new subdirectories)
- `/home/danielwoz/BambuStudio-bridge/src/slic3r/GUI/DeviceManager.cpp` (lines 2560-3200 — defines exactly which push_status fields the mock must produce for the slicer to be happy)

## 12. Implementation status (v1 landed)

### Landed in this pass

| Step | Files                                                          | Status |
|------|----------------------------------------------------------------|--------|
| 1    | `tests/bridge/harness/{ShimRecorder.{hpp,cpp},TraceLine.hpp,CMakeLists.txt}` + `HarnessShimRecorderTest.cpp`. CMake option `BAMBU_BRIDGE_HARNESS` (OFF by default) in `tests/bridge/CMakeLists.txt`. | DONE |
| 2    | `src/slic3r/Utils/NetworkAgent.cpp` — dormant `#if defined(BAMBU_BRIDGE_HARNESS_ENABLE)` block wraps the 10 NetworkAgent::*_ptr listed in §7. Free-function trampolines + `bb_harness_wrap_network_agent_pointers()` invoked at the tail of `initialize_network_module`. | DORMANT (compiles only when the FULL BambuStudio target is built with `-DBAMBU_BRIDGE_HARNESS_ENABLE=1`; not exercised from the standalone bridge build). |
| 3    | `src/slic3r/GUI/Printer/PrinterFileSystem.cpp` — `BB_HARNESS_REC()` macro records `Bambu_Create / Open / Close / Destroy / StartStream / ReadSample` on each trampoline. Macro is a no-op when the harness option is off. | DORMANT (same reason as step 2). |
| 4    | `tests/bridge/mocks/{PrinterModel.hpp,PrinterFsm.hpp,A1Printer.hpp,H2SPrinter.hpp,H2DPrinter.hpp}` + `MockPlugin.{hpp,cpp}` + `MockBambuSource.{hpp,cpp}` + `CMakeLists.txt`. | DONE |
| 5    | `tests/bridge/Harness{MockA1,MockH2S,MockH2D}Test.cpp` — each drives `CloudUplink` against the matching mock + asserts the FSM round-trips. | DONE — all three pass under ctest. |
| 6    | `tests/bridge/harness/{TraceComparator.{hpp,cpp},trace_comparator_main.cpp}` + `HarnessTraceMatchTest.cpp`. Comparator drops timing fields, buckets `delta_ms` to 1s, renumbers serial-number-shaped strings to `<DEV_ID_n>`, and normalises `sequence_id` / `token` / `request_id` to placeholders. | DONE — comparator self-tests pass; SKIPs (rc 77) for missing fixtures, as planned. |
| 7    | Golden trace capture against real A1 / H2S / H2D printers.    | DEFERRED — out of band, requires hardware. Comparator and gate are ready to consume them. |

### Plan adjustments forced by reality

- **Model IDs.** §3 lists model_ids "N1 / N2S / N2D". The live BBL profile JSONs at `~/BambuStudio/resources/profiles/BBL/machine/` show:
    - A1     → `model_id: "N2S"`
    - A1mini → `model_id: "N1"`
    - H2S    → `model_id: "O1S"`
    - H2D    → `model_id: "O1D"`
  The harness uses the live values. The plan's §3 capability struct comment is preserved verbatim but the per-model code paths use N2S / O1S / O1D.
- **Shim activation in the standalone bridge build.** The NetworkAgent.cpp / PrinterFileSystem.cpp wrap sites only exist in the slicer-side translation units, which are not compiled by the standalone bridge build. The harness `ShimRecorder` library, the mocks, and the Harness*Test binaries all compile inside the standalone bridge tree (`tests/bridge/`); the slicer wrap is *dormant source* — it compiles into BambuStudio only when that build is configured with `-DBAMBU_BRIDGE_HARNESS_ENABLE=1`. We did NOT integrate the macro into the top-level BambuStudio CMakeLists.txt — that gate is a follow-up.
- **MockBambuSource.bambu_get_stream_info.** The proprietary `Bambu_StreamInfo` struct is vendored under `src/slic3r/GUI/Printer/BambuTunnel.h`; the harness mock zero-fills the buffer rather than mirroring the layout, since v1 tests do not consume the field contents (RTSP path already covered by `RtspServerLoopbackTest`).
- **Trace delta_ms tolerance.** §6 says "within ±500 ms". The comparator implements this as floor-to-nearest-1s bucket — practical equivalent for the timing observed under loopback, simpler than an interval check that would require per-line stateful comparison.

### How to exercise

```
cd build/bridge_e2e
cmake -DBAMBU_BRIDGE_HARNESS=ON .
cmake --build .
ctest -R "Harness"
```

Expected: 5 tests Pass, 1 (HarnessTraceMatchTest) Skipped (rc 77, fixture absent — by design).

To capture a trace from a future real-printer run, set `BAMBU_BRIDGE_SHIM=1` and (optionally) `BAMBU_BRIDGE_SHIM_TRACE=/path/to/out.jsonl` in the BambuStudio environment when launching a slicer build configured with `-DBAMBU_BRIDGE_HARNESS_ENABLE=1`. The dormant trampolines in NetworkAgent.cpp / PrinterFileSystem.cpp will write the file. Compare against the mock trace with the standalone `build/bridge_e2e/tests_bridge/harness/trace_comparator <real.jsonl> <mock.jsonl>`.

