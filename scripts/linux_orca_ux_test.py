#!/usr/bin/env python3
"""
Linux OrcaSlicer UX test for the Bambu bridge.

Goal: validate that, against a running bridge on the same host, the Linux
OrcaSlicer-bridge build can:
  1. Discover all three FFFF virtual printers via SSDP
  2. Auto-connect to H2S and H2D using saved access codes
  3. Render their AMS + filament state
  4. Stream a few seconds of their camera

Phase 1 (this script today): clear discovery state, pre-seed access codes,
record the Xvfb session as video + periodic screenshots, capture bridge
stderr. Phase 2 will add UI click-driving via python-Xlib.

Run with no args. Outputs go to /tmp/orca-ux-test/<timestamp>/.
"""

import os, sys, json, time, shutil, signal, subprocess, pathlib
from datetime import datetime

# ---------------------------------------------------------------------------
# Test config
# ---------------------------------------------------------------------------
ORCA_BIN   = "/home/danielwoz/OrcaSlicer-bridge/build/src/Release/orca-slicer"
ORCA_CONF  = pathlib.Path.home() / ".config" / "OrcaSlicer"
DISPLAY    = ":100"
SCREEN_RES = "1280x800"
RECORD_FPS = 10
RUN_SECS   = 90                 # how long to keep orca up before tear-down
SNAP_AT    = (5, 10, 15, 25, 40, 60, 80)   # screenshot timestamps in seconds

# Access codes the bridge expects for each virtual printer (from the
# bridge-side BambuStudio.conf). Pre-seeded into OrcaSlicer.conf so the
# DevManager auto-binds them on hydration and no access-code prompt is
# needed.
ACCESS_CODES = {
    "FFFFPLESERIAL01": "22222222",  # H2S
    "FFFFPLESERIAL03": "33333333",  # A1
    "FFFFPLESERIAL02": "11111111",  # H2D
}

OUT = pathlib.Path("/tmp/orca-ux-test") / datetime.now().strftime("%Y%m%d-%H%M%S")
OUT.mkdir(parents=True, exist_ok=True)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
def preflight():
    log(f"artifacts → {OUT}")
    # Bridge running?
    r = subprocess.run(["pgrep", "-f", "build/src/bambu-studio"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("bridge (bambu-studio --bridge-only) is not running; start it first")
    bridge_pid = r.stdout.strip().splitlines()[0]
    log(f"bridge pid {bridge_pid}")
    # Xvfb on the target DISPLAY?
    r = subprocess.run(["pgrep", "-fa", f"Xvfb {DISPLAY}"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"Xvfb on {DISPLAY} not running; start it first")
    log(f"Xvfb on {DISPLAY} ok")
    # OrcaSlicer binary?
    if not pathlib.Path(ORCA_BIN).is_file():
        sys.exit(f"OrcaSlicer binary missing: {ORCA_BIN}")
    log(f"orca binary {ORCA_BIN}")
    return bridge_pid


# ---------------------------------------------------------------------------
# Stop any running OrcaSlicer
# ---------------------------------------------------------------------------
def stop_running_orca():
    subprocess.run(["pkill", "-f", "Release/orca-slicer"],
                   capture_output=True)
    time.sleep(2)
    subprocess.run(["pkill", "-9", "-f", "Release/orca-slicer"],
                   capture_output=True)
    time.sleep(1)


# ---------------------------------------------------------------------------
# Backup + clear OrcaSlicer state
# ---------------------------------------------------------------------------
def reset_state():
    backup = OUT / "orca_config_backup"
    if ORCA_CONF.exists():
        shutil.copytree(ORCA_CONF, backup, dirs_exist_ok=True)
        log(f"backed up OrcaSlicer config → {backup}")
    else:
        ORCA_CONF.mkdir(parents=True, exist_ok=True)
        log("created OrcaSlicer config dir (was missing)")

    # Wipe discovered-printer cache so SSDP repopulates fresh.
    vlpj = ORCA_CONF / "virtual_lan_printers.json"
    if vlpj.exists():
        vlpj.unlink()
        log(f"deleted {vlpj}")

    # Pre-seed access codes via OrcaSlicer.conf so DevManager auto-binds
    # them when the printer enters the device list (no prompt needed).
    conf_path = ORCA_CONF / "OrcaSlicer.conf"
    conf = {}
    if conf_path.exists():
        try:
            conf = json.loads(conf_path.read_text())
        except Exception as e:
            log(f"warn: existing OrcaSlicer.conf unparseable, recreating ({e})")
    conf["access_code"]      = ACCESS_CODES.copy()
    conf["user_access_code"] = ACCESS_CODES.copy()
    # Avoid the TLS-cert-store confirmation dialog under Xvfb. The Linux
    # AppConfig::set_defaults patch normally does this on first run, but
    # being explicit here makes the test deterministic.
    conf["tls_cert_store_accepted"]       = "yes"
    conf["tls_accepted_cert_store_location"] = "/etc/ssl/certs/ca-certificates.crt"
    conf_path.write_text(json.dumps(conf, indent=2))
    log(f"wrote {conf_path} with {len(ACCESS_CODES)} access codes pre-seeded")


# ---------------------------------------------------------------------------
# ffmpeg helpers
# ---------------------------------------------------------------------------
def start_recording():
    video = OUT / "session.mp4"
    proc = subprocess.Popen(
        ["ffmpeg", "-loglevel", "warning",
         "-f", "x11grab", "-framerate", str(RECORD_FPS),
         "-video_size", SCREEN_RES, "-i", DISPLAY,
         "-vcodec", "libx264", "-pix_fmt", "yuv420p", "-y", str(video)],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        preexec_fn=os.setpgrp)
    log(f"ffmpeg recording → {video} (pid {proc.pid})")
    return proc, video


def stop_recording(proc):
    try:
        proc.send_signal(signal.SIGINT)
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def snap(name):
    """Single-frame screenshot via ffmpeg."""
    p = OUT / f"{name}.png"
    subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-f", "x11grab",
         "-video_size", SCREEN_RES, "-i", DISPLAY,
         "-frames:v", "1", "-update", "1", "-y", str(p)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p


# ---------------------------------------------------------------------------
# Launch OrcaSlicer
# ---------------------------------------------------------------------------
def launch_orca():
    env = {**os.environ, "DISPLAY": DISPLAY}
    proc = subprocess.Popen(
        [ORCA_BIN],
        env=env,
        cwd="/home/danielwoz/OrcaSlicer-bridge",
        stdout=open(OUT / "orca.stdout.log", "w"),
        stderr=open(OUT / "orca.stderr.log", "w"))
    log(f"orca-slicer launched (pid {proc.pid})")
    return proc


# ---------------------------------------------------------------------------
# Snapshot the bridge stderr (last few hundred lines) so we have a
# correlated record of CONNECT/SUBSCRIBE/DOWNSTREAM events.
# ---------------------------------------------------------------------------
def find_bridge_stderr_log():
    """Heuristic: pick the most recent task-output file in the harness
    tasks dir whose first line looks like the bridge banner."""
    tasks = pathlib.Path(
        "/tmp/claude-1000/-mnt-cephfs-Source-lost-library-plugins-win-02-05-03-63"
        "/704d316c-41ac-4592-bb04-a6f15f812e4b/tasks")
    candidates = sorted(tasks.glob("*.output"), key=lambda p: p.stat().st_mtime, reverse=True)
    for c in candidates:
        try:
            with c.open() as f:
                head = "".join([next(f, "") for _ in range(3)])
        except Exception:
            continue
        if "BambuStudio --bridge-only" in head:
            return c
    return None


def snapshot_bridge_log():
    src = find_bridge_stderr_log()
    if src is None:
        log("bridge stderr log not located; skipping copy")
        return
    dst = OUT / "bridge.stderr.log"
    shutil.copy(src, dst)
    log(f"copied bridge stderr ({src.stat().st_size} B) → {dst}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    bridge_pid = preflight()
    stop_running_orca()
    reset_state()

    rec_proc, _video = start_recording()
    time.sleep(1)
    snap("t000_before_launch")

    orca = launch_orca()
    started_at = time.time()
    next_snap = iter(SNAP_AT)
    target = next(next_snap, None)
    while True:
        elapsed = time.time() - started_at
        if target is not None and elapsed >= target:
            snap(f"t{int(elapsed):03d}")
            log(f"  snapshot at t{int(elapsed):03d}s")
            target = next(next_snap, None)
        if elapsed >= RUN_SECS:
            break
        time.sleep(0.5)
        if orca.poll() is not None:
            log(f"orca-slicer exited early with rc={orca.returncode}")
            break

    snap("t_final")
    log("tearing down")
    orca.terminate()
    try:
        orca.wait(timeout=5)
    except subprocess.TimeoutExpired:
        orca.kill()
    stop_recording(rec_proc)
    snapshot_bridge_log()

    # Save a quick summary
    (OUT / "summary.txt").write_text(
        f"Linux OrcaSlicer UX test — phase 1\n"
        f"Timestamp:  {datetime.now()}\n"
        f"Bridge PID: {bridge_pid}\n"
        f"Orca PID:   {orca.pid}\n"
        f"Run secs:   {RUN_SECS}\n"
        f"Artifacts:  {OUT}\n"
        f"\nFiles produced:\n" +
        "\n".join(f"  {p.name}  ({p.stat().st_size} B)"
                  for p in sorted(OUT.iterdir()) if p.is_file()))
    log(f"done. artifacts in {OUT}")


if __name__ == "__main__":
    main()
