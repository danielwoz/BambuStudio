# Bambu Bridge — end-to-end test guide (phase 12)

This guide tells you how to run the optional `bridge-e2e` test suite
against your **real** Bambu printers. The suite is deliberately not part
of the standard build — it can only pass on a workstation that has LAN
access to the printers it is asked to talk to.

If you only want the regular bridge tests (the 26-test suite that runs
on every CI machine), this guide is not relevant; just follow the
top-level README.

## What it does

For each printer listed in `BAMBU_BRIDGE_E2E_PRINTERS`, one test
binary (`e2e_<model>_print`) does the following:

1. Spawns `BambuStudio --bridge-only` as a child process bound to
   `127.0.0.1`. The bridge refreshes the cloud inventory and stands
   up per-device MQTT/FTPS/RTSP servers on loopback. (There is no
   standalone bridge daemon binary — the proprietary `bambu_networking`
   plugin fingerprints its host process and only operates inside
   BambuStudio.)
2. Reads the child's stderr until the
   `[bridge-app] add dev_id=… lan_ip=… ports={mqtt=…, ftps=…, rtsp=…}`
   line for the configured printer's LAN IP arrives.
3. Acts as a slicer: TLS+MQTT CONNECT, subscribe to
   `device/<dev_id>/report`, FTPS upload `cube_5mm.3mf`, then publish
   `project_file` → wait for `RUNNING` → publish `pause` → wait for
   `PAUSE` → publish `stop` → wait for `IDLE`/`FINISH`.
4. Disconnects, SIGTERMs the child, and exits 0 (PASS), 1 (FAIL), or
   77 (SKIP) per ctest convention.

The harness reads `$BAMBU_BRIDGE_E2E_BAMBUSTUDIO` for the slicer
binary path, falling back to a `$PATH` lookup of `BambuStudio`. If
neither resolves the tests skip (rc=77) — running the suite therefore
requires a working BambuStudio build, not just the standalone bridge
build.

Total runtime per printer: ~90 s for the round-trip plus a few seconds
each for bridge spin-up and teardown.

## Prerequisites

- A workstation on the same LAN as the printers, with no firewall
  blocking MQTT/8883, FTPS/990, or the per-device loopback ports (the
  test binds 48883/49990/48322 + offset).
- For each printer:
  - **LAN access code** — read it from the printer's LCD under
    *Settings → General → LAN Mode → LAN Access Code*. Treat it like a
    password.
  - **LAN IPv4** — also visible under *Settings → General → WLAN*.
- A logged-in Bambu cloud account that owns the printers. The bridge
  daemon depends on the proprietary `libbambu_networking.so` plugin for
  cloud-side enumeration; install BambuStudio at least once on the
  test workstation so the plugin's config dir is populated.
- A real, sliced `cube_5mm.3mf`. **The fixture shipped in this tree is
  a stub.** See *Replacing the stub fixture* below.

## Replacing the stub fixture

`tests/bridge/e2e/fixtures/cube_5mm.3mf` ships as a tiny placeholder
zip (~200 bytes) that contains only a README warning you it's a stub.
Until you replace it, every `e2e_<model>_print` invocation exits 77
with a one-line message pointing at the file.

To produce a real one:

1. Open BambuStudio on a workstation with the same printer profile as
   the device under test.
2. Drop in a 5 mm × 5 mm × 5 mm cube (any STL of those dimensions; the
   built-in `cube.stl` resized works).
3. Slice (the fast profile is fine; we just need a printable plate).
4. *File → Export → Export plate sliced file…* and save as
   `cube_5mm.3mf`.
5. Overwrite `tests/bridge/e2e/fixtures/cube_5mm.3mf` with the new file
   and edit `cube_5mm.3mf.metadata.json`: set `"is_stub": false`,
   leave `"plate_idx"` at 1 (or match whichever plate index the export
   used).

Once that's done the tests start exercising the print path for real.

> **Tip:** if you have all four printers, slice the cube once with a
> profile each printer can render (e.g. PLA, 0.4 nozzle, 0.2 layer)
> and verify the same `.3mf` is acceptable on each by hitting *Print*
> directly. Phase 12 assumes one .3mf fits all three families — if
> a future printer rejects it you'll need a per-model fixture; the
> loader is keyed on a directory so adding `cube_5mm.h2s.3mf` later
> is a one-file change.

## Build invocation

```sh
cd /path/to/BambuStudio-bridge
rm -rf build/bridge_e2e
cmake -S src/bambu_bridge -B build/bridge_e2e \
    -DBAMBU_BRIDGE=ON \
    -DBAMBU_BRIDGE_E2E=ON \
    -DBAMBU_BRIDGE_E2E_PRINTERS="h2s@192.0.2.209,a1mini@192.0.2.210,h2@192.0.2.220"
cmake --build build/bridge_e2e -- -j$(nproc)
```

If `BAMBU_BRIDGE_E2E_PRINTERS` is left empty, the four `e2e_*_print`
binaries are still built but every run reports SKIPPED.

Access codes can be supplied two ways. **Inline** in the printers var,
which is convenient but lands in your `CMakeCache.txt`:

```
-DBAMBU_BRIDGE_E2E_PRINTERS="h2s@192.0.2.209:12345678"
```

Or **out-of-cache** via per-model env vars at test-run time, which is
the recommended path for shared workstations:

```
export BAMBU_BRIDGE_E2E_H2S_CODE=12345678
export BAMBU_BRIDGE_E2E_A1MINI_CODE=87654321
```

## Running the suite

```sh
ctest --test-dir build/bridge_e2e -L bridge-e2e --output-on-failure
```

The `-L bridge-e2e` filter runs only the four `e2e_<model>_print`
tests. Drop it to run everything (including the standard 26-test
suite).

Per-test runtime expectations:

| Test               | Expected                                  |
|--------------------|-------------------------------------------|
| `e2e_h2s_print`    | 60–120 s (faster on a hot printer)        |
| `e2e_h2_print`     | 60–120 s                                  |
| `e2e_a1_print`     | 60–120 s                                  |
| `e2e_a1mini_print` | 60–120 s                                  |

The hard timeout enforced by CTest is 240 s per test.

## Reading the test log

Each test logs to stderr. The interesting lines look like:

```
e2e_h2s_print: bridge bound mqtt=48883 ftps=49990 rtsp=48322 dev_id=01S00ABCD0123456
e2e_h2s_print: RUNNING
e2e_h2s_print: PAUSED
e2e_h2s_print: IDLE/FINISH
e2e_h2s_print: PASS — bridge round-tripped upload+print+pause+cancel
```

If `harness.start() failed`, the test prints the daemon's captured
stderr verbatim. Common causes:

- `plugin init failed` — `libbambu_networking.so` not installed or
  not in `$BAMBU_BRIDGE_PLUGIN_PATH`. Install BambuStudio at least
  once on this host or pass `--plugin <path>` (and re-baked
  `args` in `E2EHarness::start()` will need updating to thread it).
- `no devices reported` — the cloud account isn't logged in. Run the
  full BambuStudio GUI once, sign in, then re-run.
- `auth failed` — wrong access code. Re-read it from the LCD; codes
  rotate when a printer is factory-reset.

## Reconciling against phase-11 wire-diff fixtures

The `tests/bridge/expected/<model>_*` files captured by phase 11 are
byte-level fingerprints of what a real BambuStudio session emits. When
an E2E test fails after a protocol change, the standard diagnostic is:

```sh
python3 tools/wire_diff/wire_diff.py mqtt \
    --before tests/bridge/expected/h2s_mqtt.bin \
    --after  build/bridge_e2e/Testing/Temporary/h2s_mqtt.bin
```

If the diff is non-empty AND the E2E status assertion failed, the
delta is the regression. If the diff is non-empty but the E2E PASSED,
the delta is a normalisable change (e.g. a new `sequence_id` value);
add the field to the normaliser whitelist before promoting the new
capture as the fixture.

## Known limitations

- `cube_5mm.3mf` ships as a STUB. You must produce a real sliced
  cube before the print phase will actually run.
- The harness does not currently expose `--plugin <path>` overrides.
  If your plugin isn't on the default search path, set
  `$BAMBU_BRIDGE_PLUGIN_PATH` in the test environment.
- Only the MQTT and FTPS paths are exercised per print. The RTSP
  camera path is covered separately by `RtspServerLoopbackTest`.
- The harness assumes the bridge binds on loopback. Tests against a
  remote daemon need a custom `E2EHarness` subclass that skips the
  spawn step.
