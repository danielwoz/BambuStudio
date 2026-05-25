#!/usr/bin/env python3
"""Phase 2e — pre-set firstguide.finish=true to skip the entire setup wizard.

Then just click Device tab and verify the LAN list has the discovered FFFF
printers. No wizard fighting.
"""
import os, sys, json, time, shutil, signal, subprocess, pathlib
from datetime import datetime

ORCA_BIN  = "/home/danielwoz/OrcaSlicer-bridge/build/src/Release/orca-slicer"
ORCA_CONF = pathlib.Path.home() / ".config" / "OrcaSlicer"
DISPLAY   = ":100"
ACCESS_CODES = {
    "FFFFPLESERIAL01": "22222222",
    "FFFFPLESERIAL03": "33333333",
    "FFFFPLESERIAL02": "11111111",
}

TAB_DEVICE = (498, 55)

OUT = pathlib.Path("/tmp/orca-ux-test") / datetime.now().strftime("%Y%m%d-%H%M%S-p2e")
OUT.mkdir(parents=True, exist_ok=True)


def log(m): print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def snap(name):
    subprocess.run(["scrot", "-z", "-o", str(OUT / f"{name}.png")],
                   env={"DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def xclick(x, y):
    subprocess.run(["xdotool", "mousemove", str(x), str(y), "click", "1"],
                   env={"DISPLAY": DISPLAY, "PATH": "/usr/bin:/bin"},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    subprocess.run(["pkill", "-9", "-f", "Release/orca-slicer"], capture_output=True)
    time.sleep(2)

    # Reset + bypass
    backup = OUT / "orca_config_backup"
    if ORCA_CONF.exists():
        shutil.copytree(ORCA_CONF, backup, dirs_exist_ok=True)
    vlpj = ORCA_CONF / "virtual_lan_printers.json"
    if vlpj.exists(): vlpj.unlink()

    conf_path = ORCA_CONF / "OrcaSlicer.conf"
    conf = json.loads(conf_path.read_text()) if conf_path.exists() else {}
    conf["access_code"]                      = ACCESS_CODES.copy()
    conf["user_access_code"]                 = ACCESS_CODES.copy()
    conf["tls_cert_store_accepted"]          = "yes"
    conf["tls_accepted_cert_store_location"] = "/etc/ssl/certs/ca-certificates.crt"
    # Bypass the first-run wizard. AppConfig::get_bool("firstguide","finish")
    # gates the wizard launcher in GUI_App.cpp.
    conf["firstguide"] = {"finish": "true"}
    conf["stealth_mode"] = "true"
    conf_path.write_text(json.dumps(conf, indent=2))
    log(f"wrote {conf_path} with wizard bypass")

    rec = subprocess.Popen(
        ["ffmpeg", "-loglevel", "warning", "-f", "x11grab",
         "-framerate", "10", "-video_size", "1280x800", "-i", DISPLAY,
         "-vcodec", "libx264", "-pix_fmt", "yuv420p", "-y",
         str(OUT / "session.mp4")],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        preexec_fn=os.setpgrp)
    time.sleep(1)
    snap("t000_pre")

    env = {**os.environ, "DISPLAY": DISPLAY}
    orca = subprocess.Popen(
        [ORCA_BIN], env=env, cwd="/home/danielwoz/OrcaSlicer-bridge",
        stdout=open(OUT / "orca.stdout.log", "w"),
        stderr=open(OUT / "orca.stderr.log", "w"))
    log(f"orca pid {orca.pid}")
    t0 = time.time()

    log("dwell 35 s for boot")
    time.sleep(35); snap(f"t{int(time.time()-t0):03d}_after_boot")

    log("click Device tab")
    xclick(*TAB_DEVICE); time.sleep(5)
    snap(f"t{int(time.time()-t0):03d}_device_tab")

    # Try a few possible LAN-list row positions.
    for y in (140, 180, 220, 260, 300):
        xclick(200, y); time.sleep(3)
        snap(f"t{int(time.time()-t0):03d}_lan_y{y}")

    log("dwell 30 s")
    time.sleep(30); snap(f"t{int(time.time()-t0):03d}_final")

    orca.terminate()
    try: orca.wait(timeout=5)
    except subprocess.TimeoutExpired: orca.kill()
    try:
        rec.send_signal(signal.SIGINT); rec.wait(timeout=5)
    except subprocess.TimeoutExpired:
        rec.kill()
    log(f"done → {OUT}")


if __name__ == "__main__":
    main()
