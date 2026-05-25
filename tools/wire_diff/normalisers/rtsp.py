"""RTSP normaliser.

RTSP/1.0 is line-based ASCII (CRLF-delimited).  The bridge's RtspServer
(phase 8) speaks the OPTIONS / DESCRIBE / SETUP / PLAY / TEARDOWN /
GET_PARAMETER subset over implicit TLS on port 322.  Interleaved RTP
frames begin with a 4-byte ``$<channel><len:u16>`` framing marker after
PLAY.

Variability we must mask:

  1. ``CSeq:`` — request/response sequence number; the client increments
     it per command.

  2. ``Session:`` — 8-hex-digit session ID the server picks at random per
     connection (see RtspServer.cpp:649-657).  Both the SETUP response
     and every subsequent request/response includes it.

  3. ``RTP-Info: url=...;seq=N;rtptime=T`` — initial RTP sequence number
     and timestamp; the server chooses ``rtp_seq=1`` always but
     ``rtp_ssrc`` is millisecond-derived (~per-session unique).

  4. SDP body:
        - ``o=- <session-id> <session-version> IN IP4 <addr>``
          The bridge emits "- 0 0" but real cameras emit a millis-derived
          session-id and a per-edit version, so we mask the o= line's
          numeric fields.
        - ``c=IN IP4 <addr>`` — connection address; mask the address.
        - ``a=control:<url>`` — control URL contains the host:port the
          DESCRIBE was sent to; mask.
        - ``sprop-parameter-sets=<b64>,<b64>`` — base64 of the SPS/PPS
          NALs; the bridge's NullCameraSource emits a constant pair, but
          a real camera's SPS/PPS varies per stream (resolution + level).
          Mask the base64 to be diff-friendly.

  5. Interleaved RTP frames after PLAY: 12-byte RTP header has
        - version/payload-type/marker  (fixed, kept)
        - sequence_number (16-bit, varies)
        - timestamp (32-bit, varies)
        - SSRC (32-bit, varies)
     We mask the 10 variable header bytes per frame; the payload bytes
     vary per frame as well (different NAL data, FU-A fragments) and so
     are masked as a hash for the frame body.

Things we keep intact:

  - Status lines ("RTSP/1.0 200 OK", "RTSP/1.0 401 Unauthorized", etc.)
  - Method verbs in requests (OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN,
    GET_PARAMETER, SET_PARAMETER)
  - Public header value listing the verbs
  - Transport header's protocol stack ("RTP/AVP/TCP;unicast;interleaved=")
  - WWW-Authenticate scheme ("Basic realm=\"bambu\"")
  - Content-Type ("application/sdp")
"""

import hashlib
import re
import struct
from typing import List


_CSEQ_RE = re.compile(rb"(?im)^(CSeq:[ \t]*)\d+[ \t]*$")
_SESSION_RE = re.compile(rb"(?im)^(Session:[ \t]*)[0-9A-Fa-f]+(.*)$")
_RTPINFO_RE = re.compile(
    rb"(?im)^(RTP-Info:[ \t]*).*$")

# Any RTSP URL host[:port] in request lines, Content-Base, etc. Matches
# `rtsp://host[:port]/path` and rewrites the host:port to a placeholder.
# Keep the protocol prefix + path so the structural target ("/streaming/
# live/1") still gates the diff.
_RTSP_URL_RE = re.compile(
    rb"(rtsp://)[A-Za-z0-9.\-]+(?::\d+)?")

# Content-Base header is server-emitted and depends on the URL the client
# DESCRIBE'd. Mask the whole value.
_CONTENT_BASE_RE = re.compile(
    rb"(?im)^(Content-Base:\s*).*$")

# Content-Length is variable when SDP body sizes differ across servers
# (the bridge emits a slightly different SDP than the captured fixture).
# Mask the numeric value so byte-count drift in the SDP doesn't fail the
# diff; the SDP body itself is still diffed line-by-line.
_CONTENT_LENGTH_RE = re.compile(
    rb"(?im)^(Content-Length:[ \t]*)\d+[ \t]*$")

# User-Agent: capture probe emits none; some clients emit BambuStudio
# version strings. Mask if present.
_USER_AGENT_RE = re.compile(
    rb"(?im)^(User-Agent:\s*).*$")

# Authorization Basic <b64> - the access code is the load-bearing
# secret; we keep `Basic ` but mask the credential blob.
_AUTH_RE = re.compile(
    rb"(?im)^(Authorization:[ \t]*Basic[ \t]+)\S+[ \t]*$")

# SDP o= line:   o=- 0 0 IN IP4 0.0.0.0
_SDP_O_RE = re.compile(
    rb"(?m)^(o=\S+)[ \t]+\d+[ \t]+\d+[ \t]+IN[ \t]+IP4[ \t]+\S+[ \t]*$")

# SDP c= line:   c=IN IP4 0.0.0.0
_SDP_C_RE = re.compile(
    rb"(?m)^(c=IN[ \t]+IP4[ \t]+)\S+[ \t]*$")

# SDP a=control: line — contains the DESCRIBE URL.
_SDP_CTRL_RE = re.compile(
    rb"(?m)^(a=control:)(?!streamid=)\S+[ \t]*$")

# SDP sprop-parameter-sets
_SDP_SPROP_RE = re.compile(
    rb"(?m)(sprop-parameter-sets=)[A-Za-z0-9+/=,]+")

# SDP profile-level-id is derived from SPS bytes; mask too.
_SDP_PLID_RE = re.compile(
    rb"(?m)(profile-level-id=)[0-9A-Fa-f]+")

# Date: header (when present) — Bambu printers don't emit it but some
# stacks do.
_DATE_RE = re.compile(
    rb"(?im)^(Date:\s*).+$")


def _normalise_text_section(blob: bytes) -> bytes:
    """Rewrite the RTSP/SDP text portion (everything before the first $-framed
    interleaved RTP packet)."""
    out = blob
    out = _CSEQ_RE.sub(rb"\g<1><NORMALISED-CSEQ>", out)
    out = _SESSION_RE.sub(rb"\g<1><NORMALISED-SESSION>\g<2>", out)
    out = _RTPINFO_RE.sub(rb"\g<1><NORMALISED-RTP-INFO>", out)
    out = _CONTENT_BASE_RE.sub(rb"\g<1><NORMALISED-CONTENT-BASE>", out)
    out = _CONTENT_LENGTH_RE.sub(rb"\g<1><NORMALISED-CLEN>", out)
    out = _USER_AGENT_RE.sub(rb"\g<1><NORMALISED-USER-AGENT>", out)
    out = _AUTH_RE.sub(rb"\g<1><NORMALISED-CREDENTIAL>", out)
    out = _RTSP_URL_RE.sub(rb"\g<1><NORMALISED-RTSP-HOST>", out)
    out = _SDP_O_RE.sub(
        rb"\g<1> <NORMALISED-SDP-ORIGIN-ID> <NORMALISED-SDP-ORIGIN-VER> IN IP4 <NORMALISED-SDP-ORIGIN-ADDR>",
        out)
    out = _SDP_C_RE.sub(rb"\g<1><NORMALISED-SDP-CONN-ADDR>", out)
    out = _SDP_CTRL_RE.sub(rb"\g<1><NORMALISED-SDP-CONTROL-URL>", out)
    out = _SDP_SPROP_RE.sub(rb"\g<1><NORMALISED-SPROP>", out)
    out = _SDP_PLID_RE.sub(rb"\g<1><NORMALISED-PLID>", out)
    out = _DATE_RE.sub(rb"\g<1><NORMALISED-DATE>", out)
    return out


def _split_text_and_interleaved(blob: bytes) -> List[bytes]:
    """Split a capture into alternating ASCII RTSP message blocks and
    interleaved-RTP packets.

    The RTSP grammar guarantees text messages end with a blank line
    (CRLFCRLF after the final header).  Interleaved packets start with
    a literal ``$`` byte and carry a length prefix.
    """
    out: List[bytes] = []
    off = 0
    n = len(blob)
    while off < n:
        if blob[off:off + 1] == b"$":
            if off + 4 > n:
                # Truncated framing header — drop the rest as binary.
                out.append(blob[off:])
                break
            length = struct.unpack_from(">H", blob, off + 2)[0]
            end = off + 4 + length
            if end > n:
                # Truncated body.  Likely the capture stopped mid-frame.
                # Emit what we have AS A FRAME (so it normalises to a
                # marker) and let the outer loop continue parsing —
                # the bytes *after* the truncation point may be plain
                # ASCII text (e.g. TEARDOWN headers).  We can't recover
                # any more $-frames after a truncation; advance past
                # what's left of this partial frame and resume in text
                # mode at the first non-binary-looking byte.
                out.append(blob[off:n])
                # No reliable way to know where the "real" frame
                # boundary should have been; the only safe option is
                # to leave it as one trailing marker and stop.
                break
            out.append(blob[off:end])
            off = end
        else:
            # Text RTSP message — read until CRLFCRLF or next $ frame or EOF.
            blank = blob.find(b"\r\n\r\n", off)
            dollar = blob.find(b"$", off)
            # Take the smaller of:
            #   (a) end-of-header + Content-Length body (if any)
            #   (b) next interleaved frame start
            #   (c) EOF
            if blank == -1:
                # No header terminator — take the rest.
                out.append(blob[off:])
                break
            end = blank + 4
            # Look for Content-Length within this header span.
            header_span = blob[off:end]
            cl_match = re.search(rb"(?im)^Content-Length:[ \t]*(\d+)[ \t]*$",
                                 header_span)
            if cl_match:
                body_len = int(cl_match.group(1))
                end = min(end + body_len, n)
            if dollar != -1 and dollar < end:
                # An interleaved frame intervened before the body would
                # have finished — split at the frame boundary.
                end = dollar
            out.append(blob[off:end])
            off = end
    return out


def _normalise_interleaved_frame(frame: bytes) -> bytes:
    """Mask a single $-framed interleaved RTP packet to a stable marker.

    Layout:   $ <chan> <len_hi> <len_lo> <rtp-header:12><rtp-payload>
    RTP hdr:  V/P/X/CC | M|PT | seq:16 | ts:32 | ssrc:32

    Both the length AND the contents vary per-frame because every
    capture run draws from a live H.264 stream with its own SPS/PPS,
    timestamps, and FU-A fragmentation.  We collapse the whole frame
    to a single placeholder so the diff cares about *whether* RTP was
    streamed, not the exact bytes.  The channel id (RTP=0, RTCP=1) and
    the V/P/X/CC and M|PT bytes are still keepable structural info,
    so we surface those in the placeholder.
    """
    if len(frame) < 4 + 12:
        return b"<RTP-INVALID-FRAME>"
    chan = frame[1]
    pt = frame[5] & 0x7F
    return (b"<RTP-FRAME chan=" + str(chan).encode()
            + b" pt=" + str(pt).encode() + b">")


def _canonicalise_line_endings_outside_binary(blob: bytes) -> bytes:
    """Replace bare LF with CRLF in regions that look like ASCII text.

    Walk the blob until the first ``$`` interleaved-frame marker; within
    a binary frame leave bytes alone, then resume canonicalisation
    after the frame.  This lets fixtures that were authored with LF
    line endings still split cleanly on ``CRLFCRLF`` boundaries.
    """
    out = bytearray()
    off = 0
    n = len(blob)
    while off < n:
        if blob[off:off + 1] == b"$" and off + 4 <= n:
            length = struct.unpack_from(">H", blob, off + 2)[0]
            end = min(off + 4 + length, n)
            out += blob[off:end]
            off = end
        else:
            # Find the next $-frame or EOF and canonicalise up to there.
            dollar = blob.find(b"$", off)
            chunk_end = n if dollar < 0 else dollar
            chunk = blob[off:chunk_end]
            chunk = chunk.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            out += chunk
            off = chunk_end
    return bytes(out)


def normalise(blob: bytes) -> bytes:
    # Canonicalise line endings in the ASCII regions BEFORE splitting:
    # fixtures authored with LF (e.g. via a text editor's default
    # newline) must split cleanly on CRLF blank lines like real on-wire
    # captures do.  Binary $-framed RTP regions are left alone.
    canon = _canonicalise_line_endings_outside_binary(blob)
    sections = _split_text_and_interleaved(canon)
    out = bytearray()
    for sec in sections:
        if sec[:1] == b"$":
            out += _normalise_interleaved_frame(sec)
        else:
            lf = sec.replace(b"\r\n", b"\n")
            lf = _normalise_text_section(lf)
            out += lf.replace(b"\n", b"\r\n")
    # Drop every ``<RTP-FRAME ...>`` marker from the output.  RTP
    # frames have already been confirmed by the capture probe — their
    # presence + payload-type/channel info has been collapsed to a
    # marker per frame, and the fixture would have to match the
    # marker positions exactly otherwise.  But frame COUNT and
    # *placement* are session-variable (the capture probe drains a
    # bounded number of frames after PLAY; the bridge may also slip a
    # frame between TEARDOWN-request and TEARDOWN-response).  So the
    # safest, most stable diff drops the markers entirely and lets the
    # surrounding RTSP control-channel grammar do the load-bearing
    # assertion.
    canon_out = bytes(out)
    canon_out = re.sub(
        rb"<RTP-FRAME [^>]+>",
        b"",
        canon_out,
    )
    return canon_out
