#!/usr/bin/env python3
"""Capture probe: FTPS.

Connects implicit-TLS to the bridge's FtpsServer, walks USER / PASS /
PBSZ / PROT / TYPE / PWD / CWD / PASV / LIST / QUIT, and records the
control-channel exchange (commands we sent + reply lines we received)
into the output file as text.

The PASV data channel itself is opened to confirm the server emits the
227 reply and accepts a TLS data-channel connection; we issue LIST
(read-only — no STOR; the bridge can't actually upload to anything
without a real printer) and then close.

Output format is the exact byte stream of:

  C: <client command>\\r\\n
  S: <server reply>\\r\\n

per turn, concatenated.  Phase 12 will swap this for a tcpdump-style
capture of the raw TLS-decrypted control-channel bytes.  For phase 11
the C:/S: tagging makes the normaliser's job line-by-line.

Actually for byte-identical wire-diff we drop the C:/S: tagging — both
the reference fixture and the capture must contain raw FTP wire bytes.
"""

from __future__ import annotations

import argparse
import socket
import ssl
import sys
from pathlib import Path


def recv_reply(tls: ssl.SSLSocket, timeout: float = 5.0) -> bytes:
    """Read one FTP reply (may span multiple lines for multi-line codes)."""
    tls.settimeout(timeout)
    buf = bytearray()
    line = bytearray()
    while True:
        ch = tls.recv(1)
        if not ch:
            break
        buf += ch
        line += ch
        if ch == b"\n":
            text = bytes(line)
            # Multi-line reply uses "code-" on first line, "code " on last.
            if len(text) >= 5 and text[3:4] == b" " and text[:3].isdigit():
                return bytes(buf)
            line = bytearray()
    return bytes(buf)


def send_cmd(tls: ssl.SSLSocket, cmd: str, captured: bytearray) -> None:
    line = (cmd + "\r\n").encode("ascii")
    captured += line
    tls.sendall(line)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Capture FTPS session")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=990)
    ap.add_argument("--access-code", default="ABCD1234")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--timeout", type=float, default=5.0)
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
        print(f"capture_ftps: connect/TLS failed: {e}", file=sys.stderr)
        return 1

    pasv_reply_bytes = b""

    try:
        # 220 welcome banner.
        captured += recv_reply(tls, args.timeout)

        send_cmd(tls, "USER bblp", captured)
        captured += recv_reply(tls, args.timeout)

        send_cmd(tls, f"PASS {args.access_code}", captured)
        captured += recv_reply(tls, args.timeout)

        for cmd in ("PBSZ 0", "PROT P", "TYPE I", "PWD", "CWD /model"):
            send_cmd(tls, cmd, captured)
            captured += recv_reply(tls, args.timeout)

        send_cmd(tls, "PASV", captured)
        pasv_reply_bytes = recv_reply(tls, args.timeout)
        captured += pasv_reply_bytes

        # Parse 227 reply: 227 Entering Passive Mode (a,b,c,d,p1,p2).
        data_host = None
        data_port = 0
        try:
            ascii_line = pasv_reply_bytes.decode("ascii", errors="replace")
            l_par = ascii_line.find("(")
            r_par = ascii_line.find(")", l_par + 1)
            if l_par >= 0 and r_par > l_par:
                parts = ascii_line[l_par + 1:r_par].split(",")
                if len(parts) == 6:
                    data_host = ".".join(parts[:4])
                    data_port = int(parts[4]) * 256 + int(parts[5])
        except (ValueError, IndexError):
            pass

        send_cmd(tls, "LIST", captured)
        captured += recv_reply(tls, args.timeout)  # 150

        # Connect (implicit TLS) to PASV data port + close to let the
        # server emit 226.
        if data_host and data_port:
            try:
                dctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                dctx.check_hostname = False
                dctx.verify_mode = ssl.CERT_NONE
                try:
                    dctx.minimum_version = ssl.TLSVersion.TLSv1_2
                    dctx.maximum_version = ssl.TLSVersion.TLSv1_2
                except AttributeError:
                    pass
                draw = socket.create_connection((data_host, data_port),
                                                timeout=args.timeout)
                draw.settimeout(args.timeout)
                dtls = dctx.wrap_socket(draw, server_hostname=data_host)
                # Drain whatever LIST body the server sends, then close.
                try:
                    while True:
                        chunk = dtls.recv(4096)
                        if not chunk:
                            break
                except (socket.timeout, ssl.SSLError):
                    pass
                try:
                    dtls.close()
                except Exception:
                    pass
            except OSError:
                pass

        # Trailing 226 (after data channel close).
        captured += recv_reply(tls, args.timeout)

        send_cmd(tls, "QUIT", captured)
        captured += recv_reply(tls, args.timeout)
    except (OSError, ConnectionError, socket.timeout) as e:
        print(f"capture_ftps: I/O failure: {e}", file=sys.stderr)
        try:
            tls.close()
        except Exception:
            pass
        # Still write whatever we captured so the diff can show partial
        # progress (helpful when debugging a regression in the server).
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
