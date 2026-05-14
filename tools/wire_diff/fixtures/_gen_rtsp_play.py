#!/usr/bin/env python3
"""Generate the RTSP PLAY fixture (text + interleaved RTP).

This is a SYNTHETIC capture modelled after what RtspServer.cpp emits
after a successful PLAY: SETUP echoes the Transport, PLAY answers with
RTP-Info, then the server sends N interleaved RTP packets carrying H.264
NALs over TLS.

Phase 12 will replace it with a real-printer capture.
"""

import struct
from pathlib import Path


HEADER_LINES = [
    b"SETUP rtsp://192.168.1.209:322/streaming/live/1/streamid=0 RTSP/1.0",
    b"CSeq: 3",
    b"Transport: RTP/AVP/TCP;unicast;interleaved=0-1",
    b"",
    b"RTSP/1.0 200 OK",
    b"CSeq: 3",
    b"Transport: RTP/AVP/TCP;unicast;interleaved=0-1",
    b"Session: 1B57F12A;timeout=60",
    b"",
    b"PLAY rtsp://192.168.1.209:322/streaming/live/1 RTSP/1.0",
    b"CSeq: 4",
    b"Session: 1B57F12A",
    b"",
    b"RTSP/1.0 200 OK",
    b"CSeq: 4",
    b"Session: 1B57F12A",
    (b"RTP-Info: url=rtsp://192.168.1.209:322/streaming/live/1/"
     b"streamid=0;seq=1;rtptime=0"),
    b"",
]


TRAILER_LINES = [
    b"TEARDOWN rtsp://192.168.1.209:322/streaming/live/1 RTSP/1.0",
    b"CSeq: 5",
    b"Session: 1B57F12A",
    b"",
    b"RTSP/1.0 200 OK",
    b"CSeq: 5",
    b"Session: 1B57F12A",
    b"",
]


def build_text() -> bytes:
    # Each LINE entry is one header; the empty bytes b"" entries emit a
    # blank line, which terminates the message per RFC 2326.  We append
    # an extra trailing CRLF after the join so the LAST message's blank
    # line is fully written (`\r\n`.join only puts CRLF *between*
    # entries).
    return b"\r\n".join(HEADER_LINES) + b"\r\n"


def build_rtp_packet(seq: int, ts: int, ssrc: int, payload: bytes,
                     marker: bool, channel: int = 0) -> bytes:
    """Build one $-framed interleaved RTP packet."""
    b0 = 0x80                                    # V=2, P=0, X=0, CC=0
    pt = 96
    b1 = (0x80 if marker else 0x00) | (pt & 0x7F)
    hdr = struct.pack(">BBHII", b0, b1, seq, ts, ssrc)
    rtp = hdr + payload
    total = len(rtp)
    frame = struct.pack(">BBH", 0x24, channel, total) + rtp
    return frame


def main() -> None:
    text = build_text()
    trailer = b"\r\n".join(TRAILER_LINES) + b"\r\n"
    # 3 small RTP packets (single-NAL units below max payload).
    rtp1 = build_rtp_packet(
        seq=1, ts=0, ssrc=0xDEADBEEF,
        payload=b"\x67\x42\xC0\x0A\x96\x54\x05\x01",  # SPS-shaped
        marker=False)
    rtp2 = build_rtp_packet(
        seq=2, ts=3000, ssrc=0xDEADBEEF,
        payload=b"\x68\xCE\x38\x80",                # PPS-shaped
        marker=False)
    rtp3 = build_rtp_packet(
        seq=3, ts=6000, ssrc=0xDEADBEEF,
        payload=b"\x65\x88\x84\x00\x55\xAA",        # IDR-ish
        marker=True)
    blob = text + rtp1 + rtp2 + rtp3 + trailer
    (Path(__file__).resolve().parent / "rtsp_play.txt").write_bytes(blob)


if __name__ == "__main__":
    main()
