#!/usr/bin/env python3
"""Bambu Bridge wire-diff CLI (phase 11).

Compares two capture files (a reference + a bridge-produced one) after
running them through the appropriate protocol normaliser, and prints the
remaining diff (if any).

Exit codes:
    0   captures are byte-identical after normalisation
    1   non-empty diff (prints unified diff to stdout)
    2   usage error (bad protocol, missing file, etc.)

Usage:
    wire_diff.py REFERENCE BRIDGE [--protocol PROTO]

PROTO is one of: ssdp, mqtt, ftps, rtsp.  Omitted -> sniffed from the
file extension / contents (see `_sniff_protocol`).

The tool depends only on the Python 3 stdlib — no pip installs.  Run it
from anywhere; the normalisers package is loaded relative to this file.
"""

from __future__ import annotations

import argparse
import difflib
import os
import sys
from pathlib import Path
from typing import Callable, Optional


# Allow `python3 path/to/wire_diff.py ...` invocations from any cwd.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))


from normalisers import ssdp, mqtt, ftps, rtsp  # noqa: E402


# Normaliser dispatch table.  Each entry: (name, callable, "binary-or-text").
_NORMALISERS: dict[str, Callable[[bytes], bytes]] = {
    "ssdp": ssdp.normalise,
    "mqtt": mqtt.normalise,
    "ftps": ftps.normalise,
    "rtsp": rtsp.normalise,
}

_BINARY_PROTOCOLS = {"mqtt", "rtsp"}    # rtsp can have interleaved binary RTP


def _sniff_protocol(path: Path, raw: bytes) -> Optional[str]:
    """Best-effort protocol detection by extension + content."""
    ext = path.suffix.lower()
    name = path.name.lower()
    if "ssdp" in name:
        return "ssdp"
    if "mqtt" in name:
        return "mqtt"
    if "ftps" in name or "ftp" in name:
        return "ftps"
    if "rtsp" in name or "rtp" in name:
        return "rtsp"
    if ext == ".bin":
        # Default binary captures to MQTT (only binary protocol with a
        # well-known on-wire signature in this tree).
        return "mqtt"
    if raw.startswith((b"HTTP/1.1 200 OK", b"NOTIFY ", b"M-SEARCH ")):
        return "ssdp"
    if raw.lstrip().startswith(b"220 "):
        return "ftps"
    if raw.lstrip().startswith(b"RTSP/1.0 ") or raw.lstrip().startswith(b"OPTIONS rtsp"):
        return "rtsp"
    return None


def _load(path: Path) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def _format_diff(a: bytes, b: bytes, a_label: str, b_label: str,
                 binary: bool) -> str:
    """Produce a human-readable diff between two byte-strings.

    For text protocols this is a unified diff.  For binary, we fall back
    to a hexdump-style side-by-side around the first differing byte.
    """
    if not binary:
        a_lines = a.decode("latin-1").splitlines(keepends=True)
        b_lines = b.decode("latin-1").splitlines(keepends=True)
        diff = difflib.unified_diff(
            a_lines, b_lines, fromfile=a_label, tofile=b_label, n=3,
        )
        return "".join(diff)
    # Binary diff — find first differing byte.
    common = min(len(a), len(b))
    first = next((i for i in range(common) if a[i] != b[i]), common)
    a_excerpt = a[max(0, first - 16):first + 32]
    b_excerpt = b[max(0, first - 16):first + 32]
    return (
        f"--- {a_label}\n+++ {b_label}\n"
        f"@@ first differing byte at offset {first} "
        f"(a-len={len(a)}, b-len={len(b)}) @@\n"
        f"- {a_excerpt.hex(' ')}\n"
        f"+ {b_excerpt.hex(' ')}\n"
    )


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="wire_diff.py",
        description=(
            "Normalise two protocol captures and report any remaining "
            "byte-level diff.  Phase 11 of the Bambu Bridge plan; see "
            "tools/wire_diff/README.md for usage and how to add new "
            "protocols."),
    )
    ap.add_argument("reference", help="Path to the reference capture.")
    ap.add_argument("bridge",
                    help="Path to the bridge-produced capture (or the "
                         "second file in any pairing).")
    ap.add_argument(
        "--protocol",
        choices=sorted(_NORMALISERS.keys()),
        help=("Protocol name; if omitted, sniffed from the file "
              "extension or first bytes."),
    )
    ap.add_argument(
        "--quiet", "-q", action="store_true",
        help="Suppress diff output (exit code still reflects result).",
    )
    args = ap.parse_args(argv)

    ref_path = Path(args.reference)
    bri_path = Path(args.bridge)
    if not ref_path.is_file():
        print(f"wire_diff: reference file not found: {ref_path}",
              file=sys.stderr)
        return 2
    if not bri_path.is_file():
        print(f"wire_diff: bridge file not found: {bri_path}",
              file=sys.stderr)
        return 2

    ref_raw = _load(ref_path)
    bri_raw = _load(bri_path)

    proto = args.protocol or _sniff_protocol(ref_path, ref_raw) \
                          or _sniff_protocol(bri_path, bri_raw)
    if proto is None:
        print("wire_diff: cannot determine protocol; pass --protocol",
              file=sys.stderr)
        return 2
    if proto not in _NORMALISERS:
        print(f"wire_diff: unknown protocol '{proto}'", file=sys.stderr)
        return 2

    normalise = _NORMALISERS[proto]
    ref_norm = normalise(ref_raw)
    bri_norm = normalise(bri_raw)

    if ref_norm == bri_norm:
        return 0

    if not args.quiet:
        diff = _format_diff(
            ref_norm, bri_norm,
            a_label=f"{ref_path.name} (normalised)",
            b_label=f"{bri_path.name} (normalised)",
            binary=proto in _BINARY_PROTOCOLS,
        )
        sys.stdout.write(diff)
        if not diff.endswith("\n"):
            sys.stdout.write("\n")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
