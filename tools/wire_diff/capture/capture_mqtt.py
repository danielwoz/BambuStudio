#!/usr/bin/env python3
"""Capture probe: MQTT.

Connects TLS to the bridge's MqttBroker on the given host/port, sends a
CONNECT (user=bblp, password=<access-code>), reads CONNACK, sends a
SUBSCRIBE for ``device/<dev_id>/report``, reads SUBACK, sends a QoS-1
PUBLISH on ``device/<dev_id>/request`` with a fixed JSON envelope, reads
PUBACK, sends DISCONNECT.

The output file is a raw byte concatenation of the post-TLS frames the
*client* emits PLUS the frames the *server* sends back, in the order
they appear on the wire (encode + decode).  This is what
``tools/wire_diff/normalisers/mqtt.py`` expects and what the
``mqtt_connect.bin`` + ``mqtt_publish_print.bin`` fixtures were authored
to match.

Exit codes:
    0   exchange completed, capture written
    1   socket / TLS / protocol failure
"""

from __future__ import annotations

import argparse
import socket
import ssl
import struct
import sys
import time
from pathlib import Path


def varint(v: int) -> bytes:
    out = bytearray()
    while True:
        d = v & 0x7F
        v >>= 7
        if v:
            d |= 0x80
        out.append(d)
        if not v:
            return bytes(out)


def decode_varint(stream: ssl.SSLSocket) -> tuple[int, bytes]:
    """Read a varint from `stream`, returning (value, raw_bytes)."""
    raw = bytearray()
    multiplier = 1
    value = 0
    for _ in range(4):
        b = stream.recv(1)
        if not b:
            raise ConnectionError("EOF mid-varint")
        raw += b
        value += (b[0] & 0x7F) * multiplier
        if (b[0] & 0x80) == 0:
            return value, bytes(raw)
        multiplier *= 128
    raise ValueError("varint > 4 bytes")


def mstr(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def recv_exact(stream: ssl.SSLSocket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("EOF mid-packet")
        buf += chunk
    return bytes(buf)


def read_one_packet(stream: ssl.SSLSocket) -> bytes:
    """Read one full MQTT control packet (fixed header + body)."""
    first = stream.recv(1)
    if not first:
        raise ConnectionError("EOF before packet")
    rl, rl_raw = decode_varint(stream)
    body = recv_exact(stream, rl) if rl else b""
    return first + rl_raw + body


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Capture MQTT exchange")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8883)
    ap.add_argument("--dev-id", default="EXAMPLESERIAL01")
    ap.add_argument("--access-code", default="ABCD1234")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args(argv)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    # Bridge speaks TLS 1.2 only (matches MqttBroker / FtpsServer).
    try:
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    except AttributeError:
        pass

    captured = bytearray()
    try:
        raw = socket.create_connection((args.host, args.port),
                                       timeout=args.timeout)
        raw.settimeout(args.timeout)
        tls = ctx.wrap_socket(raw, server_hostname=args.host)
    except OSError as e:
        print(f"capture_mqtt: connect/TLS failed: {e}", file=sys.stderr)
        return 1

    try:
        # ---- CONNECT (we emit) ----
        proto = mstr("MQTT") + bytes([4, 0xC2, 0x00, 0x3C])
        payload = (mstr("bblp_a1b2c3d4")
                   + mstr("bblp")
                   + mstr(args.access_code))
        body = proto + payload
        connect = bytes([0x10]) + varint(len(body)) + body
        captured += connect
        tls.sendall(connect)

        # ---- CONNACK (we read back) ----
        connack = read_one_packet(tls)
        captured += connack
        if len(connack) < 4 or connack[0] != 0x20 or connack[3] != 0x00:
            print(f"capture_mqtt: CONNACK not accepted: {connack.hex()}",
                  file=sys.stderr)
            return 1

        # ---- SUBSCRIBE on device/<dev>/report ----
        filt = mstr(f"device/{args.dev_id}/report") + bytes([0])
        sub_body = b"\x00\x01" + filt
        subscribe = bytes([0x82]) + varint(len(sub_body)) + sub_body
        captured += subscribe
        tls.sendall(subscribe)

        suback = read_one_packet(tls)
        captured += suback

        # ---- PUBLISH QoS 1 on device/<dev>/request ----
        payload_json = (
            b'{"info":{"command":"get_version","sequence_id":"20021"}}'
        )
        pub_var = (mstr(f"device/{args.dev_id}/request")
                   + b"\x00\x02")  # packet_id = 2
        pub_body = pub_var + payload_json
        publish = bytes([0x32]) + varint(len(pub_body)) + pub_body
        captured += publish
        tls.sendall(publish)

        puback = read_one_packet(tls)
        captured += puback

        # ---- DISCONNECT ----
        captured += bytes([0xE0, 0x00])
        tls.sendall(bytes([0xE0, 0x00]))

        # Give the server a moment to settle before we tear down TLS.
        time.sleep(0.05)
    except (OSError, ConnectionError, ValueError) as e:
        print(f"capture_mqtt: I/O failure: {e}", file=sys.stderr)
        return 1
    finally:
        try:
            tls.close()
        except Exception:
            pass

    args.output.write_bytes(bytes(captured))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
