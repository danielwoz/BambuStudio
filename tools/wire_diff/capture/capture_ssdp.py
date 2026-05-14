#!/usr/bin/env python3
"""Capture probe: SSDP.

Sends a single ``M-SEARCH * HTTP/1.1`` packet at the bridge's SsdpResponder
on a configurable UDP port, captures the bridge's unicast 200-OK reply,
and writes it (text, CRLF on the wire) to the output path.

Real BambuStudio listens on udp/1900; the bridge mirrors that.  Tests
override `--port` if the responder was bound somewhere else.

Exit codes:
    0   reply captured, written to --output
    1   no reply within timeout / socket failure
"""

from __future__ import annotations

import argparse
import socket
import sys
import time
from pathlib import Path


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Capture SSDP M-SEARCH reply")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1900)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--timeout", type=float, default=2.0)
    ap.add_argument("--st", default="ssdp:all",
                    help="Search Target header value")
    args = ap.parse_args(argv)

    msearch = (
        f"M-SEARCH * HTTP/1.1\r\n"
        f"HOST: 239.255.255.250:1900\r\n"
        f"MAN: \"ssdp:discover\"\r\n"
        f"MX: 1\r\n"
        f"ST: {args.st}\r\n"
        f"\r\n"
    ).encode("ascii")

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(("127.0.0.1", 0))
        s.settimeout(args.timeout)
        s.sendto(msearch, (args.host, args.port))
        # Give the responder a beat to reply before we block on recv.
        time.sleep(0.05)
        try:
            data, _ = s.recvfrom(8192)
        except socket.timeout:
            print(f"capture_ssdp: no reply within {args.timeout}s",
                  file=sys.stderr)
            return 1
    except OSError as e:
        print(f"capture_ssdp: socket error: {e}", file=sys.stderr)
        return 1
    finally:
        s.close()

    args.output.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
