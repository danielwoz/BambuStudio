#!/usr/bin/env python3
"""Capture probe: RTSP.

Connects implicit-TLS to the bridge's RtspServer, walks
OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN, and records the entire
session (text RTSP messages + interleaved RTP frames if PLAY produced
any) into the output file.

The capture stops cleanly after TEARDOWN; if PLAY is in progress when
the timeout elapses, whatever was read is still flushed to --output so
the diff covers the partial stream.

Phase 12 will replace this with a tcpdump-style capture; for phase 11
this approximates "what one slicer session looks like on the wire".
"""

from __future__ import annotations

import argparse
import base64
import socket
import ssl
import struct
import sys
import time
from pathlib import Path
from typing import Optional


def b64_basic(user: str, password: str) -> str:
    return base64.b64encode(f"{user}:{password}".encode("ascii")).decode("ascii")


def send_request(tls: ssl.SSLSocket, captured: bytearray,
                 verb: str, target: str, cseq: int,
                 extra_headers: dict | None = None) -> None:
    lines = [f"{verb} {target} RTSP/1.0\r\n",
             f"CSeq: {cseq}\r\n"]
    for k, v in (extra_headers or {}).items():
        lines.append(f"{k}: {v}\r\n")
    lines.append("\r\n")
    raw = "".join(lines).encode("ascii")
    captured += raw
    tls.sendall(raw)


def _recv_exact(tls: ssl.SSLSocket, n: int, deadline: float) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        remain = deadline - time.monotonic()
        if remain <= 0:
            break
        tls.settimeout(remain)
        try:
            ch = tls.recv(n - len(buf))
        except (socket.timeout, ssl.SSLWantReadError, ssl.SSLError):
            break
        if not ch:
            break
        buf += ch
    return bytes(buf)


def read_response(tls: ssl.SSLSocket, captured: bytearray,
                  timeout: float, max_frames: int = 0) -> bytes:
    """Read one RTSP response off the wire.

    Reads header lines until CRLFCRLF, then drains Content-Length bytes
    (if present) of body, then optionally drains up to ``max_frames``
    complete ``$``-framed interleaved RTP packets so they get included
    in the capture without trailing truncation.
    """
    tls.settimeout(timeout)
    head = bytearray()
    while b"\r\n\r\n" not in head:
        try:
            ch = tls.recv(1)
        except (socket.timeout, ssl.SSLWantReadError):
            break
        if not ch:
            break
        head += ch
    captured += head
    body = bytearray()
    blank_at = head.find(b"\r\n\r\n")
    if blank_at >= 0:
        clen = 0
        for line in head[:blank_at].split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                try:
                    clen = int(line.split(b":", 1)[1].strip())
                except ValueError:
                    clen = 0
        while len(body) < clen:
            try:
                ch = tls.recv(clen - len(body))
            except (socket.timeout, ssl.SSLWantReadError):
                break
            if not ch:
                break
            body += ch
        captured += body

    # Drain up to `max_frames` complete interleaved RTP frames.
    if max_frames > 0:
        deadline = time.monotonic() + timeout
        frames_read = 0
        while frames_read < max_frames and time.monotonic() < deadline:
            marker = _recv_exact(tls, 4, deadline)
            if len(marker) < 4 or marker[0:1] != b"$":
                # Either no more frames (EOF/timeout) or stream went
                # back to text mode — stop draining.
                captured += marker
                break
            length = struct.unpack(">H", marker[2:4])[0]
            payload = _recv_exact(tls, length, deadline)
            captured += marker + payload
            if len(payload) < length:
                break
            frames_read += 1
    return bytes(head + body)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Capture RTSP session")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=322)
    ap.add_argument("--access-code", default="ABCD1234")
    ap.add_argument("--target",
                    default="rtsp://127.0.0.1/streaming/live/1")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument(
        "--play-frames", type=int, default=3,
        help=("Number of complete interleaved RTP frames to drain after "
              "PLAY before sending TEARDOWN.  Phase-11 tests use a small "
              "value so the capture stays bounded AND ends on a clean "
              "frame boundary."),
    )
    args = ap.parse_args(argv)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
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
        print(f"capture_rtsp: connect/TLS failed: {e}", file=sys.stderr)
        return 1

    auth = b64_basic("bblp", args.access_code)
    try:
        send_request(tls, captured, "OPTIONS", args.target, 1)
        read_response(tls, captured, args.timeout)

        send_request(tls, captured, "DESCRIBE", args.target, 2,
                     {"Accept": "application/sdp",
                      "Authorization": f"Basic {auth}"})
        read_response(tls, captured, args.timeout)

        send_request(tls, captured, "SETUP",
                     args.target + "/streamid=0", 3,
                     {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
        setup_resp = read_response(tls, captured, args.timeout)

        # Pull out Session id for the PLAY/TEARDOWN requests.
        session_id: Optional[str] = None
        for line in setup_resp.split(b"\r\n"):
            if line.lower().startswith(b"session:"):
                v = line.split(b":", 1)[1].strip().decode("latin-1")
                session_id = v.split(";", 1)[0]
                break

        play_hdrs = {"Session": session_id} if session_id else {}
        send_request(tls, captured, "PLAY", args.target, 4, play_hdrs)
        read_response(tls, captured, args.timeout,
                      max_frames=args.play_frames)

        send_request(tls, captured, "TEARDOWN", args.target, 5, play_hdrs)
        read_response(tls, captured, args.timeout)
    except (OSError, ConnectionError, ssl.SSLError) as e:
        print(f"capture_rtsp: I/O failure: {e}", file=sys.stderr)
        try:
            tls.close()
        except Exception:
            pass
        if captured:
            args.output.write_bytes(bytes(captured))
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
