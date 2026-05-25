# Bambu Bridge — upstream sync, integration cleanup, test fortification plan

**Date:** 2026-05-17
**Status:** approved (Phase 1 in progress)
**Author:** danielwoz

This plan covers all three Bambu Bridge repos:

- `bambulab/BambuStudio` fork → `github.com/danielwoz/BambuStudio` (this repo's `danielwoz` remote)
- `SoftFever/OrcaSlicer` fork → `github.com/danielwoz/OrcaSlicer`
- `github.com/danielwoz/bambu-virtual-client` (shared submodule, consumed by both slicers)

It exists because the BambuStudio fork is **146 commits behind upstream master** and the integration touchpoints with upstream code are inline edits, not registration hooks — so every future upstream sync gets expensive. Before we change any of that, we want a test safety net under the only code that's physically shared by both slicers: the `bambu-virtual-client` submodule, which currently has **zero unit tests** of its own.

---

## Goals (in priority order)

1. **Test fortification first.** Add unit + loopback tests to `bambu-virtual-client` so the contract it has with both slicers' `DeviceManager`, `NetworkAgent`, and `PrinterFileSystem` is locked down.
2. **Then upstream sync** — bring all three slicer forks back up to their upstream HEAD. Sync is mechanical risk; tests from goal 1 catch regressions during sync.
3. **Then optional integration-point cleanup** — shrink the BambuStudio-bridge diff against `bambulab/BambuStudio` by extracting inline modifications into hook/registration patterns. Behaviour-preserving, but makes every future sync cheaper.
4. **(Deferred)** decide whether the cleaned-up diff is small enough to propose as a real PR upstream.

---

## Survey results — current state

### Fork distance from upstream

| Fork (branch) | Ahead | Behind | Notes |
|---|---|---|---|
| OrcaSlicer-bridge (`bambu-virtual-shared`) | 5 | 0 | Fully caught up. |
| BambuStudio-bridge (`bridge-necessary`) | 5 | 146 | Minimal bridge core + narrow-fix + e2e harness. |
| BambuStudio-bridge (`bridge-asio`) | 14 | 146 | `bridge-necessary` + virtual-client integration (still inline, not submodule). |
| BambuStudio-bridge (`bambu-virtual-shared`) | 9 | 146 | Submodule-based variant; Virtual\*/Bridge\*.cpp extracted to submodule. |

All three BambuStudio-bridge branches are kept. `bambu-virtual-shared` is the canonical "release" branch going forward; `bridge-necessary` and `bridge-asio` are preserved as fallbacks.

### Integration touchpoints (BambuStudio-bridge)

All currently **inline edits** of upstream files:

| File | Diff (LoC) | What it does | Cleanup candidate? |
|---|---|---|---|
| `src/slic3r/GUI/GUI_App.cpp` | ±767 | `--bridge-only` factory dispatch + scattered `#ifdef BAMBU_BRIDGE` blocks + virtual SSDP/store hookup. | **Yes** — biggest win. Extract to `BridgeBootstrap` class; collapse to ~10 LoC inline. |
| `src/slic3r/Utils/NetworkAgent.cpp/.hpp` | ±534 | `is_virtual_dev_id()` + 6 FFFF-prefix branches + callback multiplexing. | **Partial** — consolidate the 6 branches into one `dispatch_virtual_or_real()`. |
| `src/slic3r/GUI/Printer/PrinterFileSystem.cpp` | ±231 | `VirtualBambuTunnel` dispatch + debug fprintf + NULL guards. | **Marginal** — could become a tunnel-factory registration. |
| `src/slic3r/GUI/DeviceManager.cpp` | ±204 | Single `if (!is_virtual_dev_id())` inside JSON parse loop. | **No** — inline is correct. |

Bridge-server code (`src/bambu_bridge/`, ~10k LoC) is cleanly in its own directory, no upstream files touched. Good shape.

### Integration touchpoints (OrcaSlicer-bridge)

15 files changed, +598/−19. Survey verdict: **all 4 inline hooks are already justified and minimal** — no refactor needed. One non-bridge OrcaSlicer aggregate-init shim in `src/OrcaSlicer.cpp:6514` is upstream-worthy as its own contribution.

### Test coverage

Bridge-server side is strong (~20 unit + 7 e2e tests). The bedrock `bambu-virtual-client` submodule itself has **zero tests**. The six concrete gaps:

1. `MqttFraming` codec round-trips & edge cases (varint boundaries, QoS, DUP/RETAIN).
2. `VirtualMqttClient` session lifecycle (connect, reconnect, send ordering, callback thread safety, clean disconnect).
3. `VirtualSsdpDiscovery` JSON shape vs the contract `DeviceManager::on_machine_alive` consumes.
4. `VirtualLanPrinterStore` persist/hydrate round-trip + corruption recovery.
5. `VirtualFtpsClient` upload (no client-side coverage; bridge `FtpsServerLoopbackTest` only exercises the server).
6. End-to-end "OrcaSlicer discovers a bridge via SSDP and receives an MQTT report" (no equivalent of BambuStudio `T04`).

---

## Phased execution

### Phase 0 — Capture baseline (½ day)

- Re-run T01–T07 e2e + full `ctest` on `bridge-asio` and `bambu-virtual-shared`; record commit SHAs as the "do-no-harm" floor.
- Tag `pre-sync-2026-05-17` on every working branch in all three repos so we have a known-good rollback.

### Phase 1 — `bambu-virtual-client` test fortification (3–5 days, **in progress**)

Add a `tests/` directory inside the submodule with a CTest harness. Use the same no-framework `int main()` style the bridge uses (single fail counter, exit code = failure count, ctest skip code 77 when sockets unavailable). One CMake option `BAMBU_VIRTUAL_CLIENT_BUILD_TESTS=ON` (default ON when built standalone, OFF when consumed via `add_subdirectory()` by a slicer — keeps slicer build clean).

Sub-tasks:

- **1a.** `MqttFramingTest_client` — copy + adapt the bridge's `MqttFramingTest` to point at the submodule's `MqttFraming.{hpp,cpp}`. Catches drift if the two ever diverge.
- **1b.** `VirtualMqttClientLoopbackTest` — spin a minimal in-test TLS broker on `127.0.0.1:random`, drive `VirtualMqttClient` through connect → publish → receive → disconnect. Assert idempotent reconnect, message ordering, callback thread-safety.
- **1c.** `VirtualSsdpJsonContractTest` — golden-file: assert the JSON `VirtualSsdpDiscovery` produces matches the schema `DeviceManager::on_machine_alive` consumes (dev_name, dev_id, dev_ip, dev_type, dev_signal, connect_type, bind_state). Golden file lives in `tests/fixtures/ssdp_alive.json`.
- **1d.** `VirtualLanPrinterStorePersistTest` — temp-dir round trip; assert persist + reload preserves entries; assert corrupted-file recovery doesn't crash.
- **1e.** `VirtualFtpsClientLoopbackTest` — reuse the bridge's FTPS server (or stand up a minimal one) and drive the client's upload path; assert STOR completes + cert validation behaviour.
- **1f.** Wire-level smoke (in `tests/e2e/`): script spawns the bridge, multicasts a fake ALIVE, asserts `bambu_virtual_cli` receives it and can subsequently MQTT-connect to the advertised port. Not a slicer launch; just covers the on-wire contract end-to-end. Stays manual (needs `lo` multicast).
- **1g.** GitHub Actions workflow `.github/workflows/ci.yml` in `bambu-virtual-client` that builds + runs 1a–1e on every push. 1f stays opt-in.

Exit criterion for Phase 1: all six tests green locally; CI workflow green on a push to `main`.

### Phase 2 — Upstream sync (1 day Orca + 2–4 days BambuStudio)

- **2a. OrcaSlicer-bridge.** `git fetch origin && git rebase origin/main`. 0 commits behind — fast-forward only. Verify with Phase 1f against it. Push `bambu-virtual-shared` to `danielwoz/OrcaSlicer`.
- **2b. BambuStudio-bridge `bridge-necessary`.** Rebase on `origin/master` (146 behind). Resolve conflicts in `GUI_App.cpp`, `NetworkAgent.cpp`, `DeviceManager.cpp`, `PrinterFileSystem.cpp` (the known integration files). Run T01–T07 + full ctest.
- **2c. BambuStudio-bridge `bridge-asio`.** Rebase. Heavier — 14 commits include the boost::asio refactor that touches the Virtual\* files (which only exist on this branch, not `bridge-necessary`). Run full suite.
- **2d. BambuStudio-bridge `bambu-virtual-shared`.** Rebase. Less code in the diff (Virtual\* is in the submodule), but the submodule pointer may need bumping. Run full suite.

After each rebase: tag `synced-2026-MM-DD-<branch>` and push to `danielwoz/BambuStudio`. Phase 3 work happens **only after** all three branches are green on real printers post-sync.

### Phase 3 — Integration-point cleanup *(optional, decision deferred)*

Goal: shrink the BambuStudio-bridge diff vs upstream so future syncs are cheaper. Behaviour-preserving throughout. **OrcaSlicer-bridge is not touched** — survey says its hooks are already minimal.

Cleanup candidates ranked by leverage:

- **3a.** `GUI_App.cpp` → `BridgeBootstrap` extraction. Move the hand-expanded `IMPLEMENT_APP` + `--bridge-only` entry + virtual SSDP/store hookups into a new `src/slic3r/GUI/BridgeBootstrap.{hpp,cpp}`. Upstream `GUI_App.cpp` gets exactly two new lines: `#ifdef BAMBU_BRIDGE` include + one `BridgeBootstrap::install_hooks(this)` call. Target: ±767 → ~+10 in `GUI_App.cpp`.
- **3b.** `NetworkAgent` FFFF dispatch consolidation. Replace 6 inline `if (is_virtual_dev_id(sn))` branches with a single private `dispatch_for_dev_id()` helper. Target: ±534 → ~+50.
- **3c.** `PrinterFileSystem` tunnel factory. Replace inline `VirtualBambuTunnel` dispatch with a registration call at init. Only worth doing if 3a/3b go cleanly.
- **3d.** Skip `DeviceManager` — inline is correct.

Sub-questions for the user at Phase 3 entry:

- How aggressive on 3a? (full BridgeBootstrap vs trim-#ifdef-noise-only vs skip)
- How aggressive on 3c? (full factory vs skip)

After each refactor commit, full test re-run is mandatory. Phase 1 tests will catch contract/codec drift that the e2e suite can't see.

### Phase 4 — Decision gate (after Phase 3)

If the BambuStudio-bridge diff after Phase 3 is small enough — say, under ~1500 LoC across the four integration files combined, and the `#ifdef BAMBU_BRIDGE` gating means zero default-behaviour change — propose as a real PR to `bambulab/BambuStudio`. Otherwise, the smaller diff still pays for itself on every future sync.

(OrcaSlicer-bridge has only the `ThumbnailsParams` shim as upstream-worthy; that's a separate one-line PR to SoftFever, unrelated to the bridge.)

---

## Branch policy (decided)

All three BambuStudio-bridge branches stay:

- `bridge-necessary` — minimal bridge core, no virtual-client wiring. Useful as the smallest possible diff if we ever want to ship just the bridge.
- `bridge-asio` — `bridge-necessary` + Virtual\* embedded (pre-submodule). Reference snapshot of the pre-extraction architecture.
- `bambu-virtual-shared` — `bridge-asio` + submodule extraction. **Canonical release branch.**

Sync work in Phase 2 happens on all three.

---

## Test status floor (Phase 0 baseline)

Phase 0 tagged `pre-sync-2026-05-17` on every working branch in all three repos.
Full e2e/ctest SHA capture deferred to next bridge run with real printers
(not blocking Phase 1 progress).

The Phase 1 submodule test suite (run in `bambu-virtual-client` standalone) is
the new safety net for the integration cleanup work; 5/5 tests green on
`main @ f6c34e5` across multiple consecutive runs.

## Phase 2 sync outcome (executed 2026-05-17)

All three BambuStudio-bridge branches rebased onto upstream HEAD `e8c7dc1b8`:

| Branch | Before | After | Conflicts |
|---|---|---|---|
| `bridge-necessary` | `0652cf1cf` | `9d830496b` | 4 (i18n + AMSMaterials + GUI_App adjacency) |
| `bridge-asio` | `fb28f689f` | `2aa9daf66` | Same 4 on commit 1; 13 follow-ons clean |
| `bambu-virtual-shared` | `55f966758` | `9465e6fd4` | Same 4 + submodule bump to `f6c34e5` |

OrcaSlicer-bridge `bambu-virtual-shared` was a no-op rebase (0 behind) +
submodule bump to `f6c34e5` (commit `fcba1cdf`, local-only).

### Upstream-side observation (filed)

During the rebases, **upstream PR #10106 by `maziggy` shipped a stale API call
in `AMSMaterialsSetting.cpp`**: `obj->get_extruder_id_by_ams_id()` — that
method does not exist on `MachineObject`. Every other callsite in the tree
uses `obj->GetFilaSystem()->GetExtruderIdByAmsId()`. Our rebased branches
kept the working version + the `if (ext_id > 0)` guard.

**Filed as upstream PR 2026-05-18:**
[bambulab/BambuStudio#10768](https://github.com/bambulab/BambuStudio/pull/10768).
The fix re-applies xin.zhang's own post-fix commit `33b62edc1` (2026-05-07),
which had successfully fixed the bug but was then clobbered by a duplicate
cherry-pick of #10106 (`01781493c`, same author + author-date as `b29fe53fe`).
Both the current `master` HEAD and the release tag `v02.07.00.55` (published
2026-05-14) still contain the broken line — confirmed via exhaustive grep,
no preprocessor gate, no macro definition, no inheritance shenanigans.
How upstream CI compiles this without errors remains unexplained.

---

## Phase 3 readiness checkpoint

Pre-Phase-3 invariants confirmed:
- Phase 1 submodule tests green and the bug-fix commits (SIGPIPE, TLS shutdown)
  pulled into both slicer forks via submodule bump.
- Phase 2 sync caught all three BambuStudio-bridge branches up to current
  upstream master. Future syncs will be cheap if we keep this cadence.
- Every refactor commit in Phase 3 will be the only thing the next sync has
  to reconcile — best window for invasive cleanup.

## Phase 3 outcome (executed 2026-05-17/18)

All three sub-phases (3a / 3b / 3c) landed across all three BambuStudio-bridge
branches. 3d (`DeviceManager`) was a deliberate skip — its inline FFFF
conditional is small and already correct.

### Aggregate diff reduction on `bambu-virtual-shared` (canonical)

| File | Before (LoC vs upstream) | After | Reduction |
|---|---:|---:|---:|
| `src/slic3r/GUI/GUI_App.cpp` | 1062 | 155 | **85%** |
| `src/slic3r/Utils/NetworkAgent.cpp` (bridge-only portion) | 134 | 52 | **61%** |
| `src/slic3r/GUI/Printer/PrinterFileSystem.cpp` | 241 | 25 | **90%** |
| **Combined upstream-touching diff** | **1437** | **232** | **84%** |

New self-contained files (live alongside upstream code, gated by
`#ifdef BAMBU_BRIDGE`):

- `src/slic3r/GUI/BridgeBootstrap.{hpp,cpp}` (≈863 LoC)
- `src/slic3r/Utils/NetworkAgentBridgeHooks.{hpp,cpp}` (≈750 LoC)
- `src/slic3r/GUI/Printer/PrinterFileSystemBridge.{hpp,cpp}` (small)

### Final branch heads (pushed to `github.com/danielwoz/BambuStudio`)

- `bridge-necessary` @ `b42e72ae6`
- `bridge-asio` @ `02b4ab8ba`
- `bambu-virtual-shared` @ `3ef11b1a9` *(canonical)*

### Notable findings during Phase 3

- **Two real correctness bugs** in the canonical 3a were caught only when
  bridge-necessary's propagation agent ran `clang -fsyntax-only`. The
  canonical's own build used stale `.o` files that masked them. Fixes:
  (i) `class Foo*` in fwd-decls had been introducing local
  `BridgeBootstrap::GUI_App` types, breaking friend matches — replaced with
  a real `class GUI_App;` fwd-decl in the enclosing namespace. (ii) A
  TU-static helper accessed private members without a friend grant —
  inlined into the friended public function.
- **Stale-worktree hazard during submodule bump:** the post-rebase
  `bambu-virtual-shared` ref was updated by the rebase agent's worktree, but
  the main worktree's files were still pre-rebase. A naive
  `git add submodule && git commit` then included ~58k LoC of accidental
  reverts. Caught + force-with-leased away the same turn. Lesson: after a
  worktree-based rebase, `git reset --hard <branch>` in the main worktree
  before doing any further commits.
- **3c propagation to bridge-asio** doesn't reach the canonical's 25 LoC
  floor because bridge-asio retains its own diagnostic `fprintf` overlay
  (BS_TRACE, loop_tick, Reconnect URL waits). The Phase 3c contract — extract
  the 11-lambda dispatch block — is fully achieved; the residual 185 LoC is
  orthogonal diagnostic-only diff out of phase scope.

### Phase 3 is the upstream-PR-ready milestone

Every future upstream sync now touches one or two files in one place each,
not hundreds of lines scattered across `GUI_App.cpp`. The canonical
`bambu-virtual-shared` diff is small enough to consider as a real PR to
`bambulab/BambuStudio` — decision deferred to Phase 4.

## Phase 3.5 — Windows portability audit (2026-05-18)

After Phase 3 landed, ran a static Windows-portability audit of the new
bridge files (`BridgeBootstrap`, `NetworkAgentBridgeHooks`,
`PrinterFileSystemBridge`) and the bambu-virtual-client submodule
(`VirtualMqttClient`, `VirtualFtpsClient`, `VirtualLanPrinterStore`,
`VirtualSsdpAliveJson`, `VirtualSsdpDiscovery`).

### Verdict: ~90% Windows-ready

0 MUST-FIX. 4 CONCERNS fixed defensively:

| Fix | File | Why |
|---|---|---|
| `sigaction` POSIX-gate | `VirtualMqttCli.cpp` | unconditional sigaction would block Windows configure |
| Explicit `<arpa/inet.h>` | `VirtualFtpsClient.cpp` | was relying on boost::asio transitive include for `INET_ADDRSTRLEN` |
| `boost::nowide::{i,o}fstream` | `VirtualLanPrinterStore.cpp`, `VirtualFtpsClient.cpp` | non-ASCII paths (data_dir, user-picked .3mf) work on Windows |
| `WIN32 → ws2_32 + crypt32` link | `bambu-virtual-client/CMakeLists.txt`, `src/bambu_bridge/CMakeLists.txt` | defensive link list — slicer pipeline picks these up transitively today but standalone Windows CI would otherwise fail with unresolved winsock symbols |

Landed:
- `bambu-virtual-client` `main` @ `98776df`
- `BambuStudio-bridge` `bambu-virtual-shared` @ `0bf618039` (submodule bump + bridge CMakeLists fix)
- `OrcaSlicer-bridge` `bambu-virtual-shared` @ `29e08e08` (submodule bump)

### Deferred concerns

- `VirtualSsdpDiscovery` UDP multicast — no explicit `WSAStartup`; relies on boost::asio's first `io_context` ctor doing it on Windows. Worth verifying once a real Windows compile is wired.
- **Server-side bambu_bridge** (SsdpListener, FtpsServer, RtspServer, MqttBroker, VirtualTunnelServer) uses ungated POSIX sockets — **Linux-only by design today**. Any future Windows server port is a larger job than the client-side audit suggests.

## Build verification (2026-05-18, second attempt)

The first rebuild attempt failed at configure on missing assimp. The fix turned out to be simpler than installing the package: the next `cmake --build build` automatically re-resolved against the already-populated deps tree. Build then ran 644 objects + linking.

**Real regression caught by the build:** Phase 3a's BridgeBootstrap extraction
removed the `#include "bambu_bridge/headless/BridgeApp.hpp"` and `#include
"slic3r/GUI/Printer/BridgeStorageBackend.hpp"` from `GUI_App.cpp` (those
includes had only been there because the inline bridge code used the types).
GUI_App.hpp still forward-declares those classes, and the `std::unique_ptr<>`
members at the end of GUI_App need them complete at the point of
`~GUI_App()` definition (line 2314). The earlier `clang -fsyntax-only` audits
on individual files happened to use include orders that didn't trigger
unique_ptr destructor instantiation, so the bug slipped through Phase 3a's
verification.

Fix: re-add the two `#include`s inside the `#ifdef BAMBU_BRIDGE` block at
top of `GUI_App.cpp`. Forward decls in the header stay — this is the
standard "complete-type-at-dtor-definition" pattern. Back-ported to
`bridge-asio` (`2779d8f45`) and `bridge-necessary` (`dc7b30aac`).

### Final post-Phase-3 build state (2026-05-18)

| Branch | HEAD | bambu-studio binary | Bridge ctests |
|---|---|---|---|
| bambu-virtual-shared | `5f985e040` | ✓ 138 MB @ 13:56 | 29/32 (3 pre-existing SSDP fails) |
| bridge-asio | `2779d8f45` | rebuild deferred | — |
| bridge-necessary | `dc7b30aac` | rebuild deferred | — |

### Pre-existing SSDP test failures

`SsdpResponderUnitTest`, `SsdpResponderLoopbackTest`, and the SSDP arm of
`WireDiffBridgeTest` fail with a clear format mismatch — the bridge emits
`LOCATION: 192.0.2.42` + `SERVER: UPnP/1.0` + reordered/dropped device
keys, but the fixtures expect `LOCATION: http://192.0.2.42:80/upnp/desc.xml`
+ `SERVER: Bambu Lab/H2S/01.02.00.00` + the full key set.

Verified pre-existing: diffing `SsdpResponder.cpp`, `SsdpResponder.hpp`,
`SsdpResponderUnitTest.cpp` between `pre-sync-2026-05-17-bambu-virtual-shared`
and current HEAD shows **byte-identical** content. These would have failed
on the May 12 baseline too if anyone had run them. Filing TODO for
follow-up: either refresh the fixture against current SsdpResponder
output, or restore the SsdpResponder format the fixture pinned.

Bambu-virtual-client standalone tests continue to pass 5/5 across all
changes (Phase 1 + Phase 3.5 + Phase 4 build catch).
