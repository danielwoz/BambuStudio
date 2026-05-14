"""MQTT normaliser.

The bridge's MqttBroker accepts MQTT 3.1.1 over TLS 1.2 on port 8883 and
emits CONNACK/SUBACK/PUBACK/PINGRESP frames whose framing is fixed by the
spec.  However, several fields *inside* the on-wire payload differ on
every run:

  1. ``packet_id`` — a 16-bit identifier the client picks for each QoS-1
     PUBLISH / SUBSCRIBE / UNSUBSCRIBE; the broker echoes it back in
     PUBACK / SUBACK / UNSUBACK.  Different runs pick different IDs.

  2. ``client_id`` — a random suffix BambuStudio appends to the LAN
     CONNECT (e.g. ``bblp_<randhex>``).  The broker doesn't reflect it on
     the wire, but the capture's CONNECT frame contains it.

  3. ``sequence_id`` / ``timestamp`` — JSON fields inside published topic
     payloads (``device/<dev>/request``); the request body contains a
     ``"sequence_id": "<int>"`` and ``"command": ...`` pair, where the
     sequence_id increments per request.

  4. TLS record-layer framing differs run-to-run (random ClientHello
     nonce, session-resumption tickets, etc.).  In phase 11 we don't
     diff the TLS layer — captures are taken *after* SSL_accept inside
     the broker, so the bytes we see are post-decryption MQTT frames.

We DO NOT mask:

  - The CONNECT's protocol-name (``MQTT``) + protocol-level (4)
  - The username field (``bblp``)
  - The topic strings on PUBLISH / SUBSCRIBE
  - The QoS bits on PUBLISH / PUBACK
  - CONNACK return code

For binary protocols like MQTT the normaliser works at two granularities:

  a) Bytewise field rewriting using a small parser that walks variable-
     header / payload offsets for each packet type.  Anything we can't
     parse (truncated, unsupported type, malformed) flows through
     unchanged so byte-diff catches it.

  b) JSON payload rewriting — PUBLISH payloads are typically JSON when
     they target ``device/<dev>/request`` or ``.../report``.  We attempt
     a UTF-8 decode + JSON parse and rewrite specific keys.
"""

import json
import re
import struct
from typing import List, Tuple


# Variable-byte remaining length (MQTT 3.1.1 §2.2.3).
def _decode_varint(buf: bytes, off: int) -> Tuple[int, int]:
    """Returns (value, consumed) or (-1, 0) on malformed/truncated."""
    value = 0
    multiplier = 1
    consumed = 0
    for i in range(4):
        if off + i >= len(buf):
            return (-1, 0)
        b = buf[off + i]
        value += (b & 0x7F) * multiplier
        consumed += 1
        if (b & 0x80) == 0:
            return (value, consumed)
        multiplier *= 128
    return (-1, 0)


def _encode_varint(value: int) -> bytes:
    out = bytearray()
    while True:
        digit = value & 0x7F
        value >>= 7
        if value > 0:
            digit |= 0x80
        out.append(digit)
        if value == 0:
            return bytes(out)


# JSON keys we rewrite to a stable placeholder if present in any string-
# valued or int-valued payload.  Real BambuStudio request bodies use:
#   "sequence_id": "20021"
#   "timestamp": 1715180000
#   "msg": 1
# of which sequence_id + timestamp + msg are session-variable.
_MASKED_JSON_KEYS = {
    "sequence_id":  "<NORMALISED-SEQUENCE-ID>",
    "timestamp":    "<NORMALISED-TIMESTAMP>",
    "msg":          "<NORMALISED-MSG-COUNTER>",
    "tray_id":      None,  # leave alone — load-bearing
}


def _mask_json_payload(payload: bytes) -> bytes:
    """Try to JSON-decode and rewrite known variable keys.

    Returns the original payload if it isn't valid JSON.
    """
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        return payload
    text_stripped = text.strip()
    if not text_stripped or text_stripped[0] not in "{[":
        return payload
    try:
        obj = json.loads(text)
    except (json.JSONDecodeError, ValueError):
        return payload

    def walk(node):
        if isinstance(node, dict):
            for k, v in list(node.items()):
                if k in _MASKED_JSON_KEYS and _MASKED_JSON_KEYS[k] is not None:
                    node[k] = _MASKED_JSON_KEYS[k]
                else:
                    walk(v)
        elif isinstance(node, list):
            for v in node:
                walk(v)
    walk(obj)
    return json.dumps(obj, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _normalise_one_packet(pkt: bytes) -> bytes:
    """Normalise one complete MQTT packet (fixed header + body).

    Returns the rewritten packet (possibly with a new remaining-length
    varint if the body changed size).
    """
    if len(pkt) < 2:
        return pkt
    type_flags = pkt[0]
    ptype = (type_flags >> 4) & 0x0F
    rl_val, rl_consumed = _decode_varint(pkt, 1)
    if rl_val < 0:
        return pkt
    body_off = 1 + rl_consumed
    body = pkt[body_off:body_off + rl_val]
    if len(body) != rl_val:
        return pkt

    new_body = body

    if ptype == 1:                       # CONNECT
        # variable header:
        #   2 bytes proto-name length
        #   N bytes proto-name ("MQTT")
        #   1 byte proto-level
        #   1 byte connect-flags
        #   2 bytes keep-alive
        # payload starts with client_id: 2-byte len + bytes.
        if len(body) < 2:
            return pkt
        pn_len = struct.unpack_from(">H", body, 0)[0]
        off = 2 + pn_len + 1 + 1 + 2
        if len(body) < off + 2:
            return pkt
        cid_len = struct.unpack_from(">H", body, off)[0]
        if len(body) < off + 2 + cid_len:
            return pkt
        # Rewrite client_id with a stable placeholder of *the same length*
        # so the remaining-length varint stays valid.  The on-wire client
        # id is bblp_<8 hex digits> per real BambuStudio; we replace the
        # trailing 8 chars with a fixed marker.
        cid = body[off + 2:off + 2 + cid_len].decode("latin-1", errors="replace")
        masked = re.sub(r"_[0-9a-fA-F]{4,}$", "_<NORMRAND>", cid)
        # If the substitution changed length, encode the masked id and
        # rebuild the body with a new length prefix.
        masked_b = masked.encode("latin-1", errors="replace")
        new_body = (
            body[:off]
            + struct.pack(">H", len(masked_b))
            + masked_b
            + body[off + 2 + cid_len:]
        )

    elif ptype in (8, 10):               # SUBSCRIBE, UNSUBSCRIBE
        # First 2 bytes of body = packet_id.  Mask to 0x0000.
        if len(body) < 2:
            return pkt
        new_body = b"\x00\x00" + body[2:]

    elif ptype == 4:                     # PUBACK
        if len(body) < 2:
            return pkt
        new_body = b"\x00\x00"

    elif ptype == 9:                     # SUBACK
        if len(body) < 2:
            return pkt
        new_body = b"\x00\x00" + body[2:]

    elif ptype == 11:                    # UNSUBACK
        if len(body) < 2:
            return pkt
        new_body = b"\x00\x00"

    elif ptype == 3:                     # PUBLISH
        # variable header: 2-byte topic-len + topic + (2-byte packet_id if QoS>0)
        if len(body) < 2:
            return pkt
        topic_len = struct.unpack_from(">H", body, 0)[0]
        topic_end = 2 + topic_len
        if len(body) < topic_end:
            return pkt
        qos = (type_flags >> 1) & 0x03
        if qos > 0:
            if len(body) < topic_end + 2:
                return pkt
            # mask packet_id to 0x0000
            new_var = body[:topic_end] + b"\x00\x00"
            payload_off = topic_end + 2
        else:
            new_var = body[:topic_end]
            payload_off = topic_end
        payload = body[payload_off:]
        new_payload = _mask_json_payload(payload)
        new_body = new_var + new_payload

    # Repack with possibly-changed body length.
    if new_body is body:
        return pkt
    return bytes([type_flags]) + _encode_varint(len(new_body)) + new_body


def _split_packets(blob: bytes) -> List[bytes]:
    out: List[bytes] = []
    off = 0
    while off < len(blob):
        if off + 2 > len(blob):
            out.append(blob[off:])
            break
        rl_val, rl_consumed = _decode_varint(blob, off + 1)
        if rl_val < 0:
            # Truncated or malformed — emit the tail as-is and stop.
            out.append(blob[off:])
            break
        pkt_len = 1 + rl_consumed + rl_val
        if off + pkt_len > len(blob):
            # Truncated tail.
            out.append(blob[off:])
            break
        out.append(blob[off:off + pkt_len])
        off += pkt_len
    return out


def normalise(blob: bytes) -> bytes:
    """Normalise a sequence of concatenated MQTT control packets.

    The capture format is a raw byte stream of one-or-more packets back-
    to-back; the normaliser parses them sequentially, rewrites variable
    fields, and re-emits the stream.
    """
    out = bytearray()
    for pkt in _split_packets(blob):
        out += _normalise_one_packet(pkt)
    return bytes(out)
