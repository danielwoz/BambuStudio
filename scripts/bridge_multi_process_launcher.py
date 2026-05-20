#!/usr/bin/env python3
"""
Bridge multi-process launcher.

Spawns one `bambu-studio --bridge-only` child per real printer. Each child:
  - Goes through the full GUI init path (wxApp + plugin load + cloud session
    hydrate), so the proprietary plugin's startup fingerprinting can't tell
    it apart from a real slicer launch. Then BridgeBootstrap short-circuits
    into headless --bridge-only mode.
  - Has BAMBU_BRIDGE_TARGET_DEV=<dev_id> set so its cloud-snapshot filter
    keeps only that one printer. Plugin's single LAN slot is dedicated to
    that one device — concurrent multi-printer is achieved by N processes,
    not by working around the plugin.
  - Has its own MQTT/FTPS/RTSP port base via --mqtt-port-base /
    --ftps-port-base / --rtsp-port-base so children don't collide.
  - Shares Xvfb DISPLAY=:100 (UDP 1900 binds with SO_REUSEPORT so multiple
    SSDP responders co-exist).

After spawning, runs paho-mqtt subscribers against each child's port
concurrently for 30 s and prints message counts per dev_id. Success criterion:
all three devices produce non-zero message counts in the same window.

Logs:    /tmp/bridge-multi/<timestamp>/{child_<idx>.log, summary.txt}
"""

import json, os, signal, subprocess, sys, time, pathlib, ssl, threading
from datetime import datetime

BIN = "/home/danielwoz/BambuStudio-bridge/build/src/bambu-studio"
DISPLAY = ":100"

# (dev_id, access_code, label)
PRINTERS = [
    ("0938BC582502312", "64e81956", "H2S"),
    ("03900D610219434", "18b1a572", "A1"),
    ("0948DB561601642", "5c673ee4", "H2D"),
]

# Per-child port-base offsets. Each child takes index 0 internally so the
# port = base + 0 = base. Three children → three bases, three distinct
# MQTT mirror ports.
MQTT_BASES = [8883, 8884, 8885]
FTPS_BASES = [39990, 39991, 39992]
RTSP_BASES = [38322, 38323, 38324]

OUT = pathlib.Path("/tmp/bridge-multi") / datetime.now().strftime("%Y%m%d-%H%M%S")
OUT.mkdir(parents=True, exist_ok=True)


def log(m): print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def kill_existing_bridges():
    subprocess.run(["pkill", "-f", "build/src/bambu-studio"], capture_output=True)
    time.sleep(2)
    subprocess.run(["pkill", "-9", "-f", "build/src/bambu-studio"], capture_output=True)
    time.sleep(1)
    log("killed existing bridges")


def spawn_child(idx, dev_id, label):
    env = {**os.environ, "DISPLAY": DISPLAY}
    args = [
        BIN, "--bridge-only",
        "--only-dev-id", dev_id,
        "--mqtt-port-base", str(MQTT_BASES[idx]),
        "--ftps-port-base", str(FTPS_BASES[idx]),
        "--rtsp-port-base", str(RTSP_BASES[idx]),
    ]
    log_path = OUT / f"child_{idx}_{label}.log"
    proc = subprocess.Popen(
        args, env=env,
        stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
        cwd="/home/danielwoz/BambuStudio-bridge",
        start_new_session=True)  # so children survive shell teardown
    log(f"spawned child {idx} {label} ({dev_id}) pid={proc.pid} → {log_path}")
    return proc


def wait_for_port_listening(port, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        r = subprocess.run(
            ["ss", "-tln"], capture_output=True, text=True)
        if f":{port} " in r.stdout:
            return True
        time.sleep(1)
    return False


def paho_subscribe(idx, dev_id, access, label, port, duration, counts):
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        log("paho not installed; pip install paho-mqtt")
        return
    counts[label] = 0
    def on_connect(c, u, f, rc):
        log(f"  {label} :{port} CONNACK rc={rc}")
        if rc == 0:
            c.subscribe(f"device/FFFF{dev_id[4:]}/report", 0)
    def on_message(c, u, m):
        counts[label] += 1
    c = mqtt.Client(client_id=f"diag-{label}-{os.getpid()}", clean_session=True)
    c.username_pw_set("bblp", access)
    c.tls_set(cert_reqs=ssl.CERT_NONE)
    c.tls_insecure_set(True)
    c.on_connect = on_connect
    c.on_message = on_message
    try:
        c.connect("127.0.0.1", port, keepalive=30)
    except Exception as e:
        log(f"  {label} :{port} connect EXC: {e}")
        return
    end = time.time() + duration
    while time.time() < end:
        c.loop(timeout=0.5)


def main():
    kill_existing_bridges()
    children = []
    for i, (dev_id, _, label) in enumerate(PRINTERS):
        children.append(spawn_child(i, dev_id, label))

    log("waiting up to 90 s for each child's MQTT mirror to come up")
    ready = {}
    for i, (_, _, label) in enumerate(PRINTERS):
        port = MQTT_BASES[i]
        ok = wait_for_port_listening(port, timeout=90)
        ready[label] = ok
        log(f"  child {i} {label} :{port} listening={ok}")

    log("running paho subscribers for 30 s concurrently")
    counts = {}
    threads = []
    for i, (dev_id, access, label) in enumerate(PRINTERS):
        if not ready[label]:
            counts[label] = -1
            continue
        t = threading.Thread(
            target=paho_subscribe,
            args=(i, dev_id, access, label, MQTT_BASES[i], 30, counts))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()

    log("=== RESULTS ===")
    all_good = True
    for label in (l for _, _, l in PRINTERS):
        n = counts.get(label, -1)
        verdict = "OK" if n > 0 else "FAIL"
        if n <= 0: all_good = False
        log(f"  {label}: {n} push_status messages [{verdict}]")

    log("stopping children")
    for p in children:
        try: p.terminate()
        except Exception: pass
    time.sleep(3)
    for p in children:
        try: p.kill()
        except Exception: pass

    summary = OUT / "summary.txt"
    summary.write_text(
        f"Bridge multi-process test\n"
        f"Timestamp: {datetime.now()}\n"
        f"Children:\n" +
        "".join(f"  child_{i}_{l}.log\n" for i, (_, _, l) in enumerate(PRINTERS)) +
        f"\nResults:\n" +
        "".join(f"  {l}: {counts.get(l, -1)} msgs\n" for _, _, l in PRINTERS) +
        f"\nVerdict: {'PASS' if all_good else 'FAIL'}\n")
    log(f"done → {OUT}")
    return 0 if all_good else 1


if __name__ == "__main__":
    sys.exit(main())
