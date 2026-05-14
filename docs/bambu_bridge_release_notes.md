# Bambu Bridge — release notes (phase 13)

What's new in `feature/bambu-bridge` vs upstream BambuStudio
(`origin/master @ 3e96c7e07`).

## Feature

**Bambu Bridge** — cloud-to-LAN printer mirroring. Run one BambuStudio
install (or the standalone `bambu-bridge-daemon`) as a gateway that
exposes your cloud-connected Bambu printers as discoverable LAN
printers for any Bambu-protocol-compatible slicer on your network.

## What you can do that you couldn't before

- Run a single BambuStudio install as a gateway that exposes your
  cloud-bound printers to other LAN slicers — no per-slicer login,
  no per-slicer LAN-mode toggle.
- Use **OrcaSlicer** (or any Bambu-protocol-compatible slicer) against
  your cloud-only printers as if they were on your LAN.
- Run a headless `bambu-bridge-daemon` on a Raspberry Pi, NUC, or
  always-on workstation as a permanent gateway. SIGTERM-clean,
  no GUI, no wx dependency.
- Hand off from the slicer with `BambuStudio --bridge-only` on Linux
  / macOS — execs the standalone daemon in place, bypassing
  wxApp/GL/locale init.

## Build flags added

| Flag                          | Default | Purpose                                                                 |
|-------------------------------|---------|-------------------------------------------------------------------------|
| `BAMBU_BRIDGE`                | ON      | Build `bambu-bridge-daemon`, `bridge-cli`, and the bridge static lib.   |
| `BAMBU_BRIDGE_E2E`            | OFF     | Build the four `e2e_<model>_print` end-to-end tests.                    |
| `BAMBU_BRIDGE_E2E_PRINTERS`   | (empty) | `model@ip[:code]` list. Empty ⇒ E2E tests configure but report SKIP.    |

## Binaries added

| Binary                  | Source                                | Role                                                                            |
|-------------------------|---------------------------------------|---------------------------------------------------------------------------------|
| `bambu-bridge-daemon`   | `src/bambu_bridge/headless/`          | Multi-device headless daemon. Walks cloud inventory, stands up per-device stack. |
| `bridge_cli`            | `src/bambu_bridge/cli/bridge_cli.cpp` | Single-device helper: `list-devices`, single-device `proxy`.                     |

## Tests added

- **26 unit/integration tests** in the standard build (`ctest`).
  Covers framing, brokers, routers, the headless `BridgeApp`
  lifecycle, and the wire-diff harness.
- **4 E2E tests** (`e2e_h2s_print`, `e2e_h2_print`, `e2e_a1_print`,
  `e2e_a1mini_print`), gated behind `BAMBU_BRIDGE_E2E=ON` and a
  populated `BAMBU_BRIDGE_E2E_PRINTERS` list. Each spawns the daemon
  against a real printer and runs an upload/print/pause/cancel
  round-trip.

**Total: 30 tests across both gates.** All 26 standard tests green on
the standalone build.

## Out of scope (v1)

Lifted verbatim from `bambu_bridge_plan.md`:

- TUTK / Agora P2P relay when neither LAN nor cloud is reachable. The
  proprietary stack supports it; punted to v2.
- AMS firmware update via the bridge. Forward-only; no provisioning.
- Multi-user / shared bridge on the same LAN. The bridge runs under a
  single Bambu cloud account.

## Compatibility

- The bridge builds via its **own standalone CMake project** at
  `src/bambu_bridge/CMakeLists.txt`. This is the supported build mode
  in this branch.
- The full BambuStudio build (top-level `CMakeLists.txt` with
  `-DBAMBU_BRIDGE=ON`) is wired but currently blocked on most hosts
  by BambuStudio's Boost ≥ 1.83 requirement. If your environment
  already builds upstream BambuStudio, `-DBAMBU_BRIDGE=ON` adds the
  bridge target alongside the slicer.
- Runtime requires the proprietary `libbambu_networking.so` plugin
  for cloud auth and inventory enumeration. The standalone build does
  not vendor it; install BambuStudio once to obtain it, or supply
  `--plugin <path>` / `$BAMBU_BRIDGE_PLUGIN_PATH`.
- `--bridge-only` exec hand-off is Linux / macOS only. On Windows,
  run `bambu-bridge-daemon.exe` directly.

## Pointers

- User guide: `docs/bambu_bridge_user_guide.md`
- E2E guide: `docs/bambu_bridge_e2e_guide.md`
- Developer guide: `docs/bambu_bridge_developer_guide.md`
- Design / phase plan: `docs/bambu_bridge_plan.md`
- Wire-diff harness: `tools/wire_diff/README.md`
