#!/usr/bin/env python3
"""Generate the MQTT reference fixtures.

The captures here are SYNTHETIC but format-correct — they reproduce the
exact bytes a real BambuStudio LAN session would push through the MQTT
3.1.1 wire codec.  Sources:

  - CONNECT framing:
        ~/BambuStudio/src/bambu_net_oss/core/LanMqttSession.cpp
        (paho_mqtt_c invocation with username/password = bblp/<access_code>
         and client_id = "bblp_<random_hex>" assembled in the session
         constructor; protocol level = 4 = MQTT 3.1.1; keep-alive = 60s)

  - Topic shape `device/<dev_id>/request` and `device/<dev_id>/report`:
        ~/BambuStudio/src/bambu_net_oss/core/LanMqttSession.cpp
        + docs/lan_mqtt_command_reference.md

  - JSON payload shape `{"info":{"command":"...","sequence_id":"..."}}`:
        the LAN request envelope used throughout LanMqttSession.cpp.

Phase 12 will REPLACE this fixture with a real-printer capture
(pcap-style dump of the post-TLS MQTT bytes); the wire-diff harness
should ideally surface zero diff against that real capture after our
normaliser runs.
"""

import struct
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


def mstr(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack(">H", len(b)) + b


def build_connect_blob(dev_id: str, access_code: str) -> bytes:
    # CONNECT (type=1, flags=0): protocol-name "MQTT", level 4,
    # connect-flags 0xC2 = user+pass+clean-session, keep-alive 60s.
    proto = mstr("MQTT") + bytes([4, 0xC2, 0x00, 0x3C])
    payload = mstr("bblp_a1b2c3d4") + mstr("bblp") + mstr(access_code)
    body = proto + payload
    connect = bytes([0x10]) + varint(len(body)) + body

    # CONNACK accepted.
    connack = bytes([0x20, 0x02, 0x00, 0x00])

    # SUBSCRIBE packet_id=1, single filter device/<dev>/report QoS 0.
    filt = mstr(f"device/{dev_id}/report") + bytes([0])
    sub_body = b"\x00\x01" + filt
    subscribe = bytes([0x82]) + varint(len(sub_body)) + sub_body
    suback = bytes([0x90, 0x03, 0x00, 0x01, 0x00])

    return connect + connack + subscribe + suback


def build_publish_blob(dev_id: str) -> bytes:
    payload_json = (
        b'{"info":{"command":"get_version","sequence_id":"20021"}}'
    )
    pub_var = mstr(f"device/{dev_id}/request") + b"\x00\x02"
    pub_body = pub_var + payload_json
    # PUBLISH QoS 1, packet_id 2 -> type/flags byte = 0x32 (publish QoS1)
    publish = bytes([0x32]) + varint(len(pub_body)) + pub_body
    puback = bytes([0x40, 0x02, 0x00, 0x02])
    disconnect = bytes([0xE0, 0x00])
    return publish + puback + disconnect


def main() -> None:
    dev_id = "EXAMPLESERIAL01"
    access_code = "ABCD1234"
    here = Path(__file__).resolve().parent
    (here / "mqtt_connect.bin").write_bytes(
        build_connect_blob(dev_id, access_code))
    (here / "mqtt_publish_print.bin").write_bytes(
        build_publish_blob(dev_id))


if __name__ == "__main__":
    main()
