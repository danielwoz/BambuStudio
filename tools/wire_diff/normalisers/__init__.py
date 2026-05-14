"""Bambu Bridge wire-diff per-protocol normalisers (phase 11).

Each normaliser exposes a single function:

    normalise(blob: bytes) -> bytes

The function rewrites variable-but-irrelevant fields (timestamps, random
session IDs, ephemeral PASV ports, packet IDs, RTP sequence numbers,
TLS handshake nonces, etc.) with stable ``<NORMALISED-name>`` placeholders
so the byte-stream of two valid recordings of the same logical session
compares equal after normalisation.

The normalisers operate on raw bytes so they're safe for binary
protocols (MQTT) as well as ASCII-line protocols (SSDP, FTPS, RTSP).

The dispatch table in ``wire_diff.py`` keys on ``--protocol``.
"""

from . import ssdp as ssdp        # noqa: F401  (re-export)
from . import mqtt as mqtt        # noqa: F401
from . import ftps as ftps        # noqa: F401
from . import rtsp as rtsp        # noqa: F401

__all__ = ["ssdp", "mqtt", "ftps", "rtsp"]
