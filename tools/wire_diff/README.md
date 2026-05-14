# wire_diff — Bambu Bridge indistinguishability validator (phase 11)

This tree gives the bridge a byte-level "are we still indistinguishable
from a real BambuStudio session?" check.  See
`docs/bambu_bridge_plan.md` -> *Indistinguishability requirements* for
the higher-level rationale.

```
tools/wire_diff/
|-- wire_diff.py             # main CLI: normalise + diff two captures
|-- normalisers/
|   |-- ssdp.py              # mask LOCATION ip/port + DevSignal
|   |-- mqtt.py              # mask packet_id, client_id suffix, JSON
|   |                        # sequence_id / timestamp / msg counters
|   |-- ftps.py              # mask PASV/EPSV port, welcome banner,
|   |                        # LIST timestamps
|   `-- rtsp.py              # mask CSeq, Session, RTP-Info,
|                            # SDP o=/c=/a=control/sprop, RTP
|                            # seq/timestamp/SSRC, RTP payload hashes
|-- capture/                 # live probes that drive the bridge
|   |-- capture_ssdp.py
|   |-- capture_mqtt.py
|   |-- capture_ftps.py
|   `-- capture_rtsp.py
`-- fixtures/                # reference captures (synthetic for now)
    |-- ssdp_reference.txt
    |-- mqtt_connect.bin
    |-- mqtt_publish_print.bin
    |-- ftps_session.txt
    |-- rtsp_describe.txt
    `-- rtsp_play.txt
```

All code is **Python 3 stdlib only**.  No pip installs.

## Quick start

```sh
python3 tools/wire_diff/wire_diff.py \
    tools/wire_diff/fixtures/ssdp_reference.txt \
    /tmp/bridge_capture.txt \
    --protocol ssdp
```

`wire_diff.py` exits 0 if the two files compare equal after the
protocol-specific normaliser runs, 1 with a unified diff to stdout if
they differ, and 2 on usage error.

`--protocol` is optional; the CLI sniffs from the filename / first
bytes when not supplied.

## Test integration

Two ctest jobs gate this tree:

- **WireDiffSelfTest** — self-tests the normalisers (idempotent;
  identity-diff; normalisable-change-diff; structural-change-diff).
- **WireDiffBridgeTest** — stands up each bridge server on a loopback
  port, runs the matching `capture_*.py`, and asserts the result diffs
  cleanly against the fixture.  Skips (ctest rc 77) on hosts without
  `python3` or where the loopback bind fails.

## Adding a new normaliser

1. Drop `normalisers/<proto>.py` exposing `normalise(blob: bytes) -> bytes`.
2. Add it to the dispatch table in `wire_diff.py`.
3. Update `_sniff_protocol` so file-extension / first-byte hints route to it.
4. Add a fixture in `fixtures/<proto>_*.{txt,bin}` with a leading
   comment that pins its provenance (where the bytes came from).
5. Extend `WireDiffSelfTest.cpp` with identity/normalisable/structural
   coverage for the new protocol.

## Refreshing fixtures from real captures (phase 12)

Today's fixtures are synthetic — they reproduce the *format* a real
BambuStudio session would emit, not the literal byte stream from a real
printer.  When phase 12 swaps in real-printer captures:

1. Run a real BambuStudio session against each of H2S / A1 / H2 and use
   `tcpdump -i <iface> -w out.pcap port 1900 or port 8883 or port 990 or
   port 322` (combine with a TLS keylog file so the bridge's
   post-decryption bytes are available).
2. Extract the post-TLS plaintext to a flat file matching the format
   each fixture expects (text for SSDP/FTPS/RTSP, raw bytes for MQTT).
3. Drop the file at `fixtures/<proto>_<model>_<scenario>.{txt,bin}`.
4. Run `wire_diff.py --protocol <proto> <fixture> <bridge-capture>` to
   confirm the normaliser still masks everything that should be masked.
5. If new variability appears (e.g. an extra header), add the mask to
   the normaliser AND add it to the docstring's "Variability we must
   mask" list so future readers know why.

`wire_diff.py` does NOT currently have a "first-capture seeds the
fixture" mode — it's deliberately read-only so accidental
"automation-overwrites-its-own-reference" mistakes can't happen.  When
you're authoring a new fixture, just `cp /tmp/bridge_capture.bin
tools/wire_diff/fixtures/...` by hand.

## Phase 11 known-limitations

- TLS handshake bytes are not part of the diff — captures are taken
  post-SSL_accept on the server side (and post-`ssl.wrap_socket()` on
  the client side).  Phase 12 may extend this to include the
  ClientHello / ServerHello fingerprint via a separate normaliser.
- The fixtures cover ONE session each.  Real long-running prints emit
  thousands of `report` PUBLISHes; phase 12 will add multi-message
  fixtures so the JSON-key normalisations get more exercise.
