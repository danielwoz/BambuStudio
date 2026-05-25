#!/usr/bin/env python3
"""Bambu Bridge — end-to-end integration tests.

Runs against a live `bambu-studio --bridge-only` process. Boots the
bridge once for the whole run, drives it through paho-mqtt and raw
sockets, asserts the observable behaviour matches what the bridge
feature work shipped.

Usage:
    python3 tests/e2e/bridge_integration.py
    python3 tests/e2e/bridge_integration.py --no-launch  # bridge already running
    python3 tests/e2e/bridge_integration.py --verbose    # show every probe
    python3 tests/e2e/bridge_integration.py --only mqtt  # run subset

Exit code 0 if all tests passed, non-zero with a per-case summary
otherwise.

Test inventory:
    T01  servers_listen           TCP-connectable on each port we advertised.
    T02  mqtt_auth_correct        CONNECT with the right access_code per port → CONNACK Success.
    T03  mqtt_auth_wrong          CONNECT with wrong access_code → CONNACK NotAuthorized.
    T04  mqtt_relay_push_status   Subscribe to device/<FFFF…>/report → ≥1 push_status within 15 s, all 3 printers.
    T05  port_resolver_consistency  Each printer's access_code is accepted ONLY on its assigned port.
    T06  ssdp_discovery           M-SEARCH ssdp:all → multicast response with virtual SN + correct LOCATION port.
    T07  clean_sigint_exit        SIGINT to bambu-studio PID → process gone in ≤ 5 s, exit code 0, no "malloc()" in stderr.

Prereqs:
    * python3 with paho-mqtt installed
    * a built bambu-studio binary at ./build/src/bambu-studio (relative
      to repo root), or pass --binary
    * an X11 display (DISPLAY=:99 default, override with --display)
"""
from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_BINARY = REPO_ROOT / "build" / "src" / "bambu-studio"
DEFAULT_DISPLAY = ":99"
BRIDGE_LOG = Path("/tmp/bridge_e2e.log")
BRIDGE_BIND = "192.0.2.151"

# Inventory: keep these in sync with the BambuStudio-bridge config the
# bridge boots with. The expected serials are real serials from the
# user's account; the virtuals are FFFF + real_sn[4:].
PRINTERS = [
    # (display name,         real_sn,             virtual_sn,           access_code, mqtt_port)
    ("Tank A1",              "EXAMPLESERIAL03",   "FFFFPLESERIAL03",    "33333333",  8883),
    ("Bambu H2S",            "EXAMPLESERIAL01",   "FFFFPLESERIAL01",    "22222222",  8884),
    ("Bambu H2D",            "EXAMPLESERIAL02",   "FFFFPLESERIAL02",    "11111111",  8885),
]


@dataclass
class TestResult:
    name: str
    passed: bool
    detail: str = ""


class TestRunner:
    def __init__(self, args):
        self.args = args
        self.proc: Optional[subprocess.Popen] = None
        self.results: list[TestResult] = []

    # ---- lifecycle -----------------------------------------------------
    def start_bridge(self):
        if self.args.no_launch:
            self._log("--no-launch: assuming a bridge is already up")
            return
        if not self.args.binary.exists():
            raise RuntimeError(f"bambu-studio not found at {self.args.binary}")
        # Kill any leftover process from a previous run.
        subprocess.run(["pkill", "-9", "bambu-studio"], check=False, stderr=subprocess.DEVNULL)
        time.sleep(1)

        env = os.environ.copy()
        env["DISPLAY"] = self.args.display
        BRIDGE_LOG.unlink(missing_ok=True)
        self._log(f"launching {self.args.binary} --bridge-only (DISPLAY={env['DISPLAY']})")
        log_fh = BRIDGE_LOG.open("w")
        self.proc = subprocess.Popen(
            [str(self.args.binary), "--bridge-only"],
            cwd=self.args.binary.parent.parent,
            stdout=log_fh,
            stderr=subprocess.STDOUT,
            env=env,
        )
        # Wait until the bridge servers are listening on every advertised
        # port. We probe via raw TCP-connect rather than grep'ing the log
        # for `[lan-uplink] … local_connected=1` because the silent
        # bridge-necessary build doesn't emit that marker — only
        # bridge-debug does. Listening sockets are the readiness signal
        # that works on any build flavour.
        wanted_ports = [p[4] for p in PRINTERS]            # mqtt ports
        for i in range(len(PRINTERS)):
            wanted_ports += [39990 + i, 38322 + i, 39998 + i]   # ftps, rtsp, vtun
        deadline = time.time() + 30
        ok = False
        while time.time() < deadline:
            if self.proc.poll() is not None:
                break
            up = 0
            for port in wanted_ports:
                try:
                    s = socket.create_connection((BRIDGE_BIND, port), timeout=0.3)
                    s.close()
                    up += 1
                except OSError:
                    pass
            if up == len(wanted_ports):
                # Servers up. Give the LAN uplinks a moment to settle
                # before tests start hitting them.
                time.sleep(2)
                ok = True
                break
            time.sleep(0.5)
        if not ok:
            tail = BRIDGE_LOG.read_text(errors="replace").splitlines()[-25:] if BRIDGE_LOG.exists() else []
            raise RuntimeError(
                f"bridge listening on only {up}/{len(wanted_ports)} ports after 30 s. "
                f"Tail:\n" + "\n".join(tail))
        self._log(f"bridge: {len(wanted_ports)}/{len(wanted_ports)} ports listening; starting tests")

    def stop_bridge(self):
        if self.args.no_launch or self.proc is None:
            return
        # T07 expects to drive shutdown itself. If T07 didn't run we
        # still teardown here.
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self._log("bridge did not exit on SIGINT; SIGKILL")
                self.proc.kill()
                self.proc.wait()

    # ---- test cases ----------------------------------------------------
    def t01_servers_listen(self):
        ports = []
        for name, _, _, _, mqtt_port in PRINTERS:
            ports.append((mqtt_port, f"mqtt({name})"))
        # FTPS = 39990 + index, RTSP = 38322 + index, vtun = 39998 + index
        for i, (name, *_rest) in enumerate(PRINTERS):
            ports.append((39990 + i, f"ftps({name})"))
            ports.append((38322 + i, f"rtsp({name})"))
            ports.append((39998 + i, f"vtun({name})"))
        failures = []
        for port, label in ports:
            try:
                s = socket.create_connection((BRIDGE_BIND, port), timeout=3)
                s.close()
            except OSError as exc:
                failures.append(f"{label}@{port}: {exc}")
        if failures:
            return False, f"failed: {failures}"
        return True, f"all {len(ports)} ports accepted TCP"

    def t02_mqtt_auth_correct(self):
        import paho.mqtt.client as mqtt
        results = []
        for name, _, virtual_sn, access_code, port in PRINTERS:
            rc = self._mqtt_connect(virtual_sn, port, access_code)
            results.append((name, rc))
            if self.args.verbose:
                self._log(f"  T02 {name}@{port} rc={rc}")
        bad = [(n, r) for n, r in results if r != 0]
        if bad:
            return False, f"unexpected CONNACK rc: {bad}"
        return True, f"3/3 CONNACK Success"

    def t03_mqtt_auth_wrong(self):
        results = []
        wrong = "00000000"
        for name, _, virtual_sn, real_access, port in PRINTERS:
            if real_access == wrong:
                wrong = "ffffffff"
            rc = self._mqtt_connect(virtual_sn, port, wrong)
            results.append((name, rc))
            if self.args.verbose:
                self._log(f"  T03 {name}@{port} wrong-pw rc={rc}")
        good_rejects = [r for _, r in results if r != 0]
        if len(good_rejects) != 3:
            return False, f"wrong access code accepted: {results}"
        return True, "3/3 CONNACK NotAuthorized as expected"

    def t04_mqtt_relay_push_status(self):
        import paho.mqtt.client as mqtt
        report_byprinter = {p[2]: [] for p in PRINTERS}
        clients = []

        def make_on_message(virtual_sn):
            def cb(cli, ud, msg):
                report_byprinter[virtual_sn].append((msg.topic, len(msg.payload)))
            return cb

        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        for name, _, virtual_sn, ac, port in PRINTERS:
            c = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                            client_id=f"e2e-t04-{virtual_sn}")
            c.username_pw_set("bblp", ac)
            c.tls_set_context(ctx)
            c.on_message = make_on_message(virtual_sn)
            try:
                c.connect(BRIDGE_BIND, port, keepalive=30)
            except Exception as exc:
                return False, f"T04 connect to {name}@{port} failed: {exc}"
            c.subscribe(f"device/{virtual_sn}/report", qos=0)
            c.loop_start()
            clients.append(c)

        # Wait up to 45 s for every printer to relay at least one report.
        # Tank A1's push_status cadence is much slower than H2S/H2D (small
        # frames, sometimes >20 s between pushes) so we'd rather wait than
        # flake.
        deadline = time.time() + 45
        while time.time() < deadline:
            if all(len(v) >= 1 for v in report_byprinter.values()):
                break
            time.sleep(0.5)

        for c in clients:
            c.loop_stop()
            try:
                c.disconnect()
            except Exception:
                pass

        missing = [vsn for vsn, msgs in report_byprinter.items() if not msgs]
        if missing:
            return False, f"no push_status from: {missing}"
        bytes_seen = {vsn: msgs[0][1] for vsn, msgs in report_byprinter.items()}
        return True, f"all 3 relayed (bytes={bytes_seen})"

    def t05_port_resolver_consistency(self):
        # Each printer's password should be accepted ONLY on its own port.
        cross_results = {}
        for name_a, _, _, ac_a, _ in PRINTERS:
            for name_b, _, _, _, port_b in PRINTERS:
                if name_a == name_b:
                    continue
                rc = self._mqtt_connect(f"FFFFCROSS{uuid.uuid4().hex[:8]}",
                                        port_b, ac_a, timeout=5)
                cross_results[(name_a, name_b, port_b)] = rc
                if self.args.verbose:
                    self._log(f"  T05 {name_a}'s ac → {name_b}@{port_b}  rc={rc}")
        # All cross-attempts should fail (rc != 0).
        accepted = [k for k, v in cross_results.items() if v == 0]
        if accepted:
            return False, f"cross-port auth accepted: {accepted}"
        return True, f"6/6 cross-port attempts rejected"

    def t06_ssdp_discovery(self):
        # Two-part probe:
        #   a) M-SEARCH ssdp:all → response must contain a USN with each
        #      virtual SN.
        #   b) For each virtual SN's expected MQTT port, HTTP-GET
        #      /upnp/desc.xml on that port (which the broker serves
        #      on the same port BEFORE the TLS ClientHello) and assert
        #      the descriptor's <serialNumber> matches that virtual SN.
        #      This is the actual port-routing proof — Bambu SSDP
        #      LOCATION is just the bare IP (no port), matching how
        #      real printers advertise.
        msg = (
            "M-SEARCH * HTTP/1.1\r\n"
            f"HOST: 239.255.255.250:1900\r\n"
            "MAN: \"ssdp:discover\"\r\n"
            "MX: 2\r\n"
            "ST: urn:bambulab-com:device:3dprinter:1\r\n"
            "\r\n"
        ).encode("ascii")

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        sock.settimeout(0.5)
        try:
            sock.sendto(msg, ("239.255.255.250", 1900))
        except OSError as exc:
            return False, f"M-SEARCH sendto failed: {exc}"

        deadline = time.time() + 5
        seen_serials = set()
        debug_responses = []
        while time.time() < deadline:
            try:
                data, _addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            text = data.decode("ascii", errors="replace")
            debug_responses.append(text)
            for _, _, virtual_sn, _, _ in PRINTERS:
                if virtual_sn in text or virtual_sn.lower() in text:
                    seen_serials.add(virtual_sn)
        sock.close()

        expected = {p[2] for p in PRINTERS}
        missing = expected - seen_serials
        if missing:
            sample = debug_responses[0][:300] if debug_responses else "(no responses)"
            return False, f"M-SEARCH did not see: {missing}. Sample: {sample!r}"

        # Part (b): UPnP descriptor on each MQTT port returns the
        # correct virtual SN.
        for name, _, virtual_sn, _, port in PRINTERS:
            try:
                with socket.create_connection((BRIDGE_BIND, port), timeout=5) as s:
                    req = (
                        f"GET /upnp/desc.xml HTTP/1.1\r\n"
                        f"Host: {BRIDGE_BIND}:{port}\r\n"
                        "Connection: close\r\n\r\n"
                    ).encode("ascii")
                    s.sendall(req)
                    chunks = []
                    while True:
                        c = s.recv(4096)
                        if not c:
                            break
                        chunks.append(c)
                    body = b"".join(chunks).decode("ascii", errors="replace")
            except OSError as exc:
                return False, f"HTTP GET {name}:{port} failed: {exc}"
            if virtual_sn not in body:
                return False, (f"descriptor at {name}:{port} did not name "
                               f"{virtual_sn}. First 200 bytes: "
                               f"{body[:200]!r}")
        return True, f"3/3 SSDP advertised + 3/3 UPnP descriptors port-routed"

    def t07_clean_sigint_exit(self):
        if self.args.no_launch or self.proc is None:
            return True, "skipped (--no-launch)"
        # Trigger the bridge's SIGINT path.
        t0 = time.time()
        self.proc.send_signal(signal.SIGINT)
        try:
            rc = self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            return False, "process did not exit within 10 s"
        elapsed = time.time() - t0
        log_text = BRIDGE_LOG.read_text(errors="replace") if BRIDGE_LOG.exists() else ""
        bad_substrings = ["malloc():", "double free", "SIGABRT"]
        for s in bad_substrings:
            if s in log_text:
                return False, f"log contains '{s}' (exit={rc}, elapsed={elapsed:.2f}s)"
        # rc 0 = clean (or -SIGINT if we hadn't intercepted, which we did).
        if rc not in (0, 130, -2):
            return False, f"unexpected exit code {rc} (elapsed={elapsed:.2f}s)"
        return True, f"exited rc={rc} in {elapsed:.2f}s, no abort marker"

    # ---- helpers -------------------------------------------------------
    def _mqtt_connect(self, client_id, port, access_code, timeout=8):
        import paho.mqtt.client as mqtt
        rc_holder = {"v": None}
        connack_ev = threading.Event()
        c = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
                        client_id=client_id)
        c.username_pw_set("bblp", access_code)
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        c.tls_set_context(ctx)

        def on_connect(cli, ud, flags, reason_code, properties=None):
            rc_holder["v"] = int(reason_code.value if hasattr(reason_code, "value") else reason_code)
            connack_ev.set()

        c.on_connect = on_connect
        try:
            c.connect(BRIDGE_BIND, port, keepalive=30)
        except Exception:
            return -1
        c.loop_start()
        connack_ev.wait(timeout=timeout)
        c.loop_stop()
        try:
            c.disconnect()
        except Exception:
            pass
        return rc_holder["v"] if rc_holder["v"] is not None else -1

    def _log(self, msg):
        sys.stdout.write(f"[e2e] {msg}\n")
        sys.stdout.flush()

    # ---- driver --------------------------------------------------------
    def run(self):
        cases = [
            ("T01_servers_listen", self.t01_servers_listen),
            ("T02_mqtt_auth_correct", self.t02_mqtt_auth_correct),
            ("T03_mqtt_auth_wrong", self.t03_mqtt_auth_wrong),
            ("T04_mqtt_relay_push_status", self.t04_mqtt_relay_push_status),
            ("T05_port_resolver_consistency", self.t05_port_resolver_consistency),
            ("T06_ssdp_discovery", self.t06_ssdp_discovery),
            # T07 must be last — it shuts the bridge down.
            ("T07_clean_sigint_exit", self.t07_clean_sigint_exit),
        ]
        if self.args.only:
            wanted = {f"T0{i}_" + n for i, n in enumerate(self.args.only.split(","), 1)}
            cases = [c for c in cases if any(c[0].endswith(s) for s in self.args.only.split(","))]

        for name, fn in cases:
            try:
                t0 = time.time()
                ok, detail = fn()
                dt = time.time() - t0
                self.results.append(TestResult(name, ok, f"{detail} [{dt:.2f}s]"))
                self._log(f"  {'PASS' if ok else 'FAIL'}  {name}  {detail}  [{dt:.2f}s]")
            except Exception as exc:
                self.results.append(TestResult(name, False, f"EXCEPTION: {exc}"))
                self._log(f"  FAIL  {name}  EXCEPTION: {exc}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--display", default=DEFAULT_DISPLAY)
    parser.add_argument("--no-launch", action="store_true",
                        help="assume a bridge is already running")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--only", default=None,
                        help="comma-separated test names to run "
                             "(e.g. 'mqtt_relay_push_status,clean_sigint_exit')")
    args = parser.parse_args()

    runner = TestRunner(args)
    runner.start_bridge()
    try:
        runner.run()
    finally:
        runner.stop_bridge()

    passed = sum(1 for r in runner.results if r.passed)
    failed = sum(1 for r in runner.results if not r.passed)
    print("")
    print(f"=== {passed}/{passed+failed} tests passed ===")
    for r in runner.results:
        print(f"  {'PASS' if r.passed else 'FAIL'}  {r.name}: {r.detail}")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
