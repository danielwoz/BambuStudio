#!/usr/bin/env python3
"""
Virtual storage tunnel test client.

Connects to a bridge vtun port over TLS (verify=off, like the slicer-
side VirtualBambuTunnel does), sends one `LIST_INFO` request, prints
the reply. Exits.

Usage:
    python3 vtun_test_client.py <host> <port> <dev_id> <access_code> [type]
        type: 0=timelapse, 1=video, 2=model (default 2)
"""

import json
import socket
import ssl
import struct
import sys
import time


def recv_exact(sock_or_ssl, n: int) -> bytes:
    out = bytearray()
    while len(out) < n:
        chunk = sock_or_ssl.recv(n - len(out))
        if not chunk:
            raise RuntimeError(f"connection closed after {len(out)}/{n} bytes")
        out.extend(chunk)
    return bytes(out)


def send_frame(ssl_sock, payload: bytes) -> None:
    hdr = struct.pack(">I", len(payload))
    ssl_sock.sendall(hdr + payload)


def recv_frame(ssl_sock) -> bytes:
    hdr = recv_exact(ssl_sock, 4)
    (n,) = struct.unpack(">I", hdr)
    if n == 0 or n > 16 * 1024 * 1024:
        raise RuntimeError(f"bad frame length {n}")
    return recv_exact(ssl_sock, n)


def main() -> int:
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    host = sys.argv[1]
    port = int(sys.argv[2])
    dev_id = sys.argv[3]
    access_code = sys.argv[4]
    type_idx = int(sys.argv[5]) if len(sys.argv) > 5 else 2
    types = ["timelapse", "video", "model"]

    print(f"[client] connecting to {host}:{port} dev_id={dev_id} type={types[type_idx]}")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2

    raw = socket.create_connection((host, port), timeout=15)
    sock = ctx.wrap_socket(raw, server_hostname=host)
    print(f"[client] TLS up, cipher={sock.cipher()}")

    # Match the URL the slicer-side VirtualBambuTunnel writes in its
    # session establishment. The bridge's session_loop reads frames and
    # forwards via BridgeStorageBackend → PrinterFileSystem → real
    # printer. The bridge ignores any URL-style preamble; it expects
    # 4-byte BE length + JSON frames straight away.

    request = {
        "cmdtype": 0x0001,           # LIST_INFO
        "sequence": 1,
        "req": {
            "type": types[type_idx],
            "storage": "",
            "api_version": 2,
            "notify": "DETAIL",
        },
    }
    payload = json.dumps(request).encode("utf-8")
    print(f"[client] sending LIST_INFO ({len(payload)} bytes JSON)")
    send_frame(sock, payload)

    # Wait for reply. The bridge's reply envelope is
    # {result: rc, sequence: <orig>, reply: {file_lists: [...]}}.
    sock.settimeout(30.0)
    try:
        reply_bytes = recv_frame(sock)
    except (socket.timeout, ssl.SSLWantReadError):
        print("[client] timed out waiting for reply (30s)")
        return 1
    reply = json.loads(reply_bytes.decode("utf-8", errors="replace"))
    print(f"[client] reply ({len(reply_bytes)} bytes):")
    print(json.dumps(reply, indent=2)[:4000])

    files = reply.get("reply", {}).get("file_lists", [])
    print(f"[client] {len(files)} file(s)")
    for f in files[:20]:
        print(f"  - {f.get('name')!r:60s} size={f.get('size')} time={f.get('time')}")

    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
