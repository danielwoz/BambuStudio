# Bambu Bridge — user guide

This is the canonical user-facing guide for the Bambu Bridge feature.
For a deeper test/integration walk-through against real printers, see
`bambu_bridge_e2e_guide.md`. For release-notes-style "what changed vs
upstream", see `bambu_bridge_release_notes.md`. For developer-facing
internals see `bambu_bridge_developer_guide.md`.

---

## 1. What it is

BambuStudio Bridge mirrors your Bambu Cloud-connected printers as LAN
printers visible to other slicers (OrcaSlicer, etc.) on your network.
Commands and uploads sent to the LAN-mirrored printer are routed to the
real printer over LAN if possible, or via Bambu Cloud if not.

The bridge is on-the-wire indistinguishable from a real BambuStudio
client in both directions: downstream slicers see what looks like a
real LAN printer (correct SSDP advertisement, the same MQTT/FTPS/RTSP
endpoints, a self-signed cert whose CN matches the device serial), and
upstream the bridge routes traffic through either the OSS LAN client
stack (`bambu_net_oss`) or the proprietary `libbambu_networking.so`
plugin's cloud forwarder — the same paths a normal BambuStudio install
already uses.

---

## 2. Quickstart

### 2.1 Build

The bridge ships as a self-contained CMake subproject. Build it
standalone — the full BambuStudio build is not required.

```sh
cd /path/to/BambuStudio-bridge
rm -rf build/bridge_standalone
cmake -S src/bambu_bridge -B build/bridge_standalone -DBAMBU_BRIDGE=ON
cmake --build build/bridge_standalone -- -j$(nproc)
```

Outputs:

- `build/bridge_standalone/bridge_cli` — single-device `bridge-cli`
  helper (`list-devices`, single-device `proxy`). Standalone-buildable
  for diagnostics and unit tests.
- The bridge library (`libbambu_bridge.a`) used by BambuStudio. The
  bridge has **no standalone daemon binary** — see §2.2.

### 2.2 Run

The bridge runs inside BambuStudio. There are two entry points and
they share the same binary:

```sh
# GUI: bridge worker thread starts automatically when BambuStudio
# boots, alongside the slicer. Cloud printers visible in the GUI are
# mirrored to the LAN for the lifetime of the session.
BambuStudio

# Headless: same binary, no GUI, runs only the bridge daemon loop.
BambuStudio --bridge-only --plugin /path/to/libbambu_networking.so
```

Why the same binary: the proprietary `bambu_networking` plugin
fingerprints its host process and refuses to operate when loaded by
anything other than BambuStudio. A standalone daemon binary would
just lock you out of the cloud login. `--bridge-only` short-circuits
the GUI bring-up so a Raspberry Pi / NUC can run BambuStudio as a
permanent bridge without ever showing a window.

Flags:

- `--plugin` is the **proprietary BambuStudio network plugin shared
  library** (`libbambu_networking.so` on Linux,
  `libbambu_networking.dylib` on macOS, `bambu_networking.dll` on
  Windows). It ships inside BambuStudio installs; reuse it from a
  working install if you don't have one. If `--plugin` is omitted
  the bridge falls back to `$BAMBU_BRIDGE_PLUGIN_PATH`, then to the
  plugin handle's default probe.
- `--config-dir` should point at an existing BambuStudio config dir
  where the plugin has already logged your cloud account in. If you
  haven't logged in yet, run BambuStudio's GUI once, sign in, then
  re-run with `--bridge-only`.

### 2.3 Disable the bridge in GUI mode

Set `BAMBU_BRIDGE_GUI_DISABLED=1` before launching `BambuStudio` to
skip starting the bridge worker thread (useful when you want the
slicer without exposing any printers on the LAN). `--bridge-only`
is unaffected.

### 2.4 Discover

On another machine on the same LAN, launch a second slicer
(BambuStudio, OrcaSlicer, …), open *Device → Add LAN device*. Within
about 30 s (one SSDP NOTIFY interval) the bridge's virtual devices
appear — one for every cloud-bound printer the plugin handle
enumerates. Enter the printer's LAN access code (same one the
real printer prints on its LCD) to complete pairing.

---

## 3. `--bridge-only` mode in BambuStudio itself

When BambuStudio is built from this branch with `BAMBU_BRIDGE=ON`
(the default), invoking the slicer binary with `--bridge-only`
short-circuits the GUI bring-up and runs the bridge daemon loop
in-process. The remaining argv is passed to the shared CLI parser
(see §5 for the flag list).

```sh
BambuStudio --bridge-only --plugin /path/to/libbambu_networking.so
```

- Same binary as the GUI mode. No separate daemon executable is
  built or shipped.
- `SIGINT` / `SIGTERM` triggers an orderly shutdown
  (RTSP → FTPS → MQTT → SSDP → plugin handle).
- `--help` (with `--bridge-only` present) prints the bridge daemon's
  usage and exits 0.
- Unknown options print "unknown option '…'" + the bridge usage on
  stderr and exit 2.

**Platform gate:** the in-process `--bridge-only` path is Linux- and
macOS-tested. The build is gated on `BAMBU_BRIDGE=ON`; on Windows the
bridge code itself builds and links but the early-exit dispatcher in
`BambuStudio.cpp` has not been exercised against Windows-specific
stdio quirks yet.

---

## 4. Per-slicer "Add LAN printer" walkthroughs

### 4.1 BambuStudio

1. *Device* tab → *Add LAN device*.
2. The bridge's virtual device appears in the SSDP-driven list with
   the same name (and `dev_id`) as the real printer.
3. Enter the printer's **LAN access code** — same code as on the real
   printer's LCD (`Settings → General → LAN Mode → LAN Access Code`).
4. Confirm. The bridge's self-signed cert (CN = real `dev_id`,
   issuer `BBL CA`) is accepted by the BambuStudio TLS validator the
   same way as a real printer.

### 4.2 OrcaSlicer (and forks)

Same flow — *Device → Add LAN printer* — with minor UI label
variations between forks. Orca's MQTT/FTPS client stack inherits from
the same upstream OSS code BambuStudio uses, so the bridge's
protocol surface is accepted unchanged.

### 4.3 Non-Bambu slicers (Prusa, Cura, …)

**Not supported.** Prusa, Cura, etc. don't speak Bambu's MQTT/FTPS
protocol stack, so they have nothing to talk to even though the
bridge advertises itself over generic SSDP. The bridge is only useful
for slicers that already know how to talk to a real Bambu LAN
printer.

---

## 5. Configuration knobs

Every `--bridge-only` flag (from `BambuStudio --bridge-only --help`):

| Flag                       | Default                                             | What it does                                                                                  |
|----------------------------|-----------------------------------------------------|-----------------------------------------------------------------------------------------------|
| `--plugin <path>`          | `$BAMBU_BRIDGE_PLUGIN_PATH` → plugin handle's probe | Path to `libbambu_networking.so` (the proprietary cloud plugin).                              |
| `--config-dir <path>`      | none (plugin uses its own default)                  | Passed through to the plugin's `set_config_dir()` — where the plugin reads its login cache.    |
| `--country-code <cc>`      | none (plugin default)                               | Passed through to the plugin's `set_country_code()`. Affects which cloud region is used.       |
| `--bind <ip>`              | `0.0.0.0`                                           | Bind IP for every per-device listener (SSDP, MQTT, FTPS, RTSP).                                |
| `--no-ssdp`                | off (SSDP enabled)                                  | Disable the SSDP responder. Other slicers will not auto-discover the bridge.                   |
| `--no-mqtt`                | off (MQTT enabled)                                  | Disable the per-device MQTT broker on 8883.                                                    |
| `--no-ftps`                | off (FTPS enabled)                                  | Disable the per-device FTPS server on 990.                                                     |
| `--no-rtsp`                | off (RTSP enabled)                                  | Disable the per-device RTSP camera re-serve.                                                   |
| `--mqtt-port-base N`       | `38883`                                             | Per-device MQTT port base; printer N listens on `base + N`. Use to avoid collisions with `:8883`. |
| `--ftps-port-base N`       | `39990`                                             | Per-device FTPS port base.                                                                      |
| `--rtsp-port-base N`       | `38322`                                             | Per-device RTSP port base.                                                                      |
| `--cert-cache-dir <path>`  | `$XDG_CONFIG_HOME/BambuStudio/bridge/certs`         | Where the bridge persists per-device self-signed certs. Mode 0600.                              |
| `--inventory-poll-seconds N` | `60`                                              | How often the daemon refreshes the cloud inventory and adds / removes / re-IPs devices.        |
| `-v` / `--verbose`         | off                                                 | Reserved for phase-11 wire-diff capture; currently a no-op in the released build.              |
| `-h` / `--help`            | n/a                                                 | Print the help text and exit 0.                                                                |

Note on port bases: real Bambu printers listen on the standard 8883 /
990 / 322 ports. The defaults here are intentionally high so the
daemon can run on the same host as the slicer without colliding with
any in-process LAN MQTT client and without needing root. Downstream
slicers that have learned the bridge's port via SSDP do not care.

---

## 6. AMS caveats

The phase-12 E2E test suite exercises only the **basic print flow**
(upload `.3mf`, start, pause, cancel) on the four supported printer
families. The following AMS-adjacent workflows are **not validated**:

- **AMS slot selection** during print job submission. Slot/filament
  mapping is encoded in the `.3mf` and printer-side; it may work end
  to end through the bridge but no test asserts that.
- **AMS RFID re-scan / "Read RFID" button** from the slicer.
- **AMS firmware updates** — explicitly out of scope per the phase-0
  plan.
- **Filament-runout mid-print continuation** through the bridge.

These paths may all work, especially over the cloud route (the
upstream forwarder is unchanged), but treat them as unverified. If
you exercise them, please file findings against the bridge plan
document.

---

## 7. Privacy & security

- **Credentials.** The bridge does not implement Bambu cloud auth
  itself — it delegates the entire cloud-auth surface to the
  proprietary `libbambu_networking.so` plugin, which stores its token
  cache under `--config-dir`. Per-printer **LAN access codes** are
  pulled from the cloud inventory at runtime and live only in process
  memory.
- **Logging.** The release build logs daemon lifecycle events (add /
  remove device, listener bind ports, errors) to stderr. It does
  **not** log access codes, MQTT payloads, or FTPS contents.
- **Verbose mode.** `-v` / `--verbose` is reserved for wire-level
  capture (phase 11) and is currently a no-op in the released build.
  When enabled in a future revision it will dump full MQTT framing
  including request/report JSON payloads, which can contain device
  serials, LAN IPs, and print-job names. Do not enable verbose mode
  while sharing logs unless you've reviewed the output.
- **Cert cache.** Per-device self-signed certs and private keys live
  under `$XDG_CONFIG_HOME/BambuStudio/bridge/certs/`. Files are
  created mode 0600 and the directory is per-user — do not chmod or
  share. Re-running the daemon reuses cached certs so each device's
  cert stays stable across restarts.
- **Sharing stderr for support.** Before pasting daemon logs into a
  bug report or chat, redact: `dev_id` values, LAN IPs, the cert
  cache path if it leaks your home directory, and any print job
  names. The cloud account email never appears in daemon stderr but
  may appear in plugin-side logs if you've enabled them via plugin
  env vars.

---

## 8. Architecture sketch

```
                                                  Bambu Cloud
                                                       ▲
                                                       │  libbambu_networking.so
                                                       │  (proprietary forwarder)
              Slicer on LAN                            │
                  │                          ┌──────── ▼ ────────┐
                  │  SSDP M-SEARCH           │                   │
                  │  MQTT TLS  (port-base+N) │   Bambu Bridge    │ ── direct LAN ── ▶ Real printer
                  │  FTPS      (port-base+N) │                   │      (LanUplink,   if reachable)
                  │  RTSPS     (port-base+N) │                   │
                  ▼                          └──────── ▲ ────────┘
            Virtual LAN printer                        │
            (per cloud-bound device)                   │
                                                SessionRouter:
                                                  • prefer direct LAN (LanUplink)
                                                  • fall back to cloud (CloudUplink)
```

For each cloud-enumerated printer, the daemon stands up a
`MirroredDevice` composed of:

- an `SsdpResponder` advertising the virtual device,
- an `MqttBroker` terminating the slicer's TLS-MQTT connection,
- an `FtpsServer` accepting `.3mf` job uploads,
- an `RtspServer` re-serving the camera (where supported),

and a `SessionRouter` that, per session, routes traffic through
either a `LanUplink` (`bambu_net_oss::LanMqttSession`, direct printer
LAN) or a `CloudUplink` (forwarded through the proprietary plugin's
cloud agent).
