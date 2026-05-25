#!/usr/bin/env python3
"""Minimal paho-mqtt mTLS publisher for the bridge's cert+key fallback.

Used by `LanUplink::on_publish` when the payload's top-level key is
`"print"` (a `print.command=*` control payload). The proprietary plugin's
`send_message_to_printer` silently rejects those from non-UI contexts,
and the bridge's bundled libssl is broken for ECDHE (corrupt EC curve
constants), so the in-tree `RawMqttPublisher.cpp` also fails. paho-mqtt
on the system Python uses the system OpenSSL which handles ECDHE just
fine; spawning this script as a short-lived subprocess sidesteps both
problems.

Real printer broker (`<printer-ip>:8883`) requires the per-printer mTLS
client cert + matching RSA-2048 key that `install_device_cert()`
extracted from the slicer/plugin heap. See
`/mnt/cephfs/ssd/BambuBridge/DISCOVERY-2026-05-22.md` for the gate
description; this script is the working publish recipe documented
there.

Exit 0 = PUBACK received (or QoS 0 publish accepted).
Exit 1 = TLS connect failed.
Exit 2 = bad arguments.
Exit 3 = MQTT CONNACK with non-zero rc, or other broker-side error.
Exit 4 = PUBACK timeout.

Diagnostics go to stderr. The C++ caller captures the exit code.

Usage:
    raw_mqtt_publish.py \
        --ip 192.0.2.209 --port 8883 \
        --cert /path/to/chain.pem --key /path/to/key.pem \
        --user bblp --pass 22222222 \
        --client-id slicer:1716728144:abc1 \
        --topic device/EXAMPLESERIAL01/request \
        --qos 1 \
        --payload-file /tmp/payload.json

If --payload-file is omitted the payload is read from stdin.
"""
import argparse
import ssl
import sys
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("paho.mqtt not installed (apt install python3-paho-mqtt)",
          file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip",       required=True)
    ap.add_argument("--port",     type=int, default=8883)
    ap.add_argument("--cert",     required=True,
                    help="PEM chain (leaf + BBL CA), mTLS client cert")
    ap.add_argument("--key",      required=True,
                    help="PKCS#8 RSA key matching the leaf in --cert")
    ap.add_argument("--user",     default="bblp")
    ap.add_argument("--pass", dest="pwd", required=True,
                    help="printer access_code")
    ap.add_argument("--client-id", required=True)
    ap.add_argument("--topic",     required=True)
    ap.add_argument("--qos",       type=int, default=1)
    ap.add_argument("--payload-file",
                    help="path to payload bytes; - or omitted = stdin")
    ap.add_argument("--connect-timeout", type=float, default=5.0)
    ap.add_argument("--io-timeout",      type=float, default=5.0)
    args = ap.parse_args()

    if args.payload_file and args.payload_file != "-":
        with open(args.payload_file, "rb") as f:
            payload = f.read()
    else:
        payload = sys.stdin.buffer.read()

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLSv1_2)
    ctx.check_hostname = False
    ctx.verify_mode    = ssl.CERT_NONE
    try:
        ctx.load_cert_chain(certfile=args.cert, keyfile=args.key)
    except Exception as e:
        print(f"load_cert_chain failed cert={args.cert} key={args.key}: {e}",
              file=sys.stderr)
        return 2

    state = {"connack_rc": None, "error": None, "connected": False}

    def on_connect(c, u, f, rc):
        state["connack_rc"] = rc
        if rc == 0:
            state["connected"] = True
        else:
            state["error"] = f"CONNACK rc={rc}"

    c = mqtt.Client(client_id=args.client_id)
    c.username_pw_set(args.user, args.pwd)
    c.tls_set_context(ctx)
    c.on_connect = on_connect

    try:
        c.connect(args.ip, args.port, keepalive=30)
    except Exception as e:
        print(f"connect {args.ip}:{args.port} failed: {e}",
              file=sys.stderr)
        return 1

    # Background network thread so publish() can be driven from main.
    # Mirrors the working DISCOVERY-2026-05-22.md recipe.
    c.loop_start()
    try:
        deadline = time.time() + args.connect_timeout
        while time.time() < deadline and not state["connected"] \
                and not state["error"]:
            time.sleep(0.02)
        if state["error"]:
            print(state["error"], file=sys.stderr)
            return 3
        if not state["connected"]:
            print(f"connack timeout (rc={state['connack_rc']})",
                  file=sys.stderr)
            return 3

        info = c.publish(args.topic, payload, qos=args.qos)
        if args.qos == 0:
            return 0
        info.wait_for_publish(timeout=args.io_timeout)
        if info.is_published():
            return 0
        print(f"timeout waiting for PUBACK (connack_rc={state['connack_rc']})",
              file=sys.stderr)
        return 4
    finally:
        try: c.disconnect()
        except Exception: pass
        c.loop_stop()


if __name__ == "__main__":
    sys.exit(main())
