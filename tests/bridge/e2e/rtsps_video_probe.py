#!/usr/bin/env python3
# Bambu Bridge — RTSPS camera E2E probe (bridge test suite).
#
# Standalone standard RTSPS client that validates the bridge's virtual camera
# server end-to-end, independent of any slicer. Full RTSP-over-TLS handshake
# (implicit TLS 1.2, self-signed cert, no verify), then OPTIONS -> DESCRIBE ->
# SETUP (RTP/AVP/TCP interleaved) -> PLAY, reads interleaved RTP, de-packetises
# RFC 6184 H.264 (single/STAP-A/FU-A), and asserts the stream carries in-band
# SPS+PPS+IDR (i.e. is actually decodable).
#
#   Usage:  python rtsps_video_probe.py [host] [port] [path] [seconds] [outfile]
#   e.g.    python rtsps_video_probe.py 127.0.0.1 38324 /s 10
#
# Requires the bridge running with BAMBU_BRIDGE_RTSP_TLS=1 (real Bambu cameras
# are RTSPS on port 322) and a live printer camera. RTSP port = rtsp_port_base
# (38322) + device-offset. The server ignores the URL path, so any [path] works.
#
# Exit codes: 0 = decodable video (SPS+PPS+IDR) PASS | 5 = RTP but no SPS/PPS/IDR
#             6 = no RTP (relay broken) | 2 = RTSP control failure
import socket, ssl, sys, time, struct, os, tempfile

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 38324
PATH = sys.argv[3] if len(sys.argv) > 3 else "/s"
SECS = int(sys.argv[4]) if len(sys.argv) > 4 else 10
OUT  = sys.argv[5] if len(sys.argv) > 5 else os.path.join(
    tempfile.gettempdir(), "bridge_cam.h264")

# RTSP_PROBE_PLAIN=1 -> plain RTSP (no TLS), to exercise the server's
# per-connection protocol sniff. Default = RTSPS (TLS).
PLAIN = os.environ.get("RTSP_PROBE_PLAIN") == "1"
scheme = "rtsp" if PLAIN else "rtsps"
base = f"{scheme}://{HOST}:{PORT}{PATH}"

raw = socket.create_connection((HOST, PORT), timeout=10)
raw.settimeout(30)
if PLAIN:
    print(f"[probe] TCP connected {HOST}:{PORT}; PLAIN RTSP (no TLS)")
    s = raw
else:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    print(f"[probe] TCP connected {HOST}:{PORT}; starting TLS handshake (30s timeout)...")
    t0 = time.time()
    s = ctx.wrap_socket(raw, server_hostname=HOST)
    print(f"[probe] TLS handshake took {time.time()-t0:.1f}s")
    print(f"[probe] TLS ok: {s.version()} cipher={s.cipher()[0]}")

cseq = 0
session = None
def req(method, url, extra=None):
    global cseq
    cseq += 1
    lines = [f"{method} {url} RTSP/1.0", f"CSeq: {cseq}", "User-Agent: bridge-rtsps-probe"]
    if session: lines.append(f"Session: {session}")
    if extra:
        for k, v in extra.items(): lines.append(f"{k}: {v}")
    msg = "\r\n".join(lines) + "\r\n\r\n"
    s.sendall(msg.encode())
    # read response headers (until blank line). May be followed by SDP body.
    buf = b""
    s.settimeout(25)
    while b"\r\n\r\n" not in buf:
        d = s.recv(4096)
        if not d: break
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    text = head.decode(errors="replace")
    print(f"\n[>>] {method} {url}\n[<<]\n{text}")
    # content-length body
    cl = 0
    for ln in text.split("\r\n"):
        if ln.lower().startswith("content-length:"):
            cl = int(ln.split(":")[1].strip())
    body = rest
    while len(body) < cl:
        d = s.recv(4096)
        if not d: break
        body += d
    if cl:
        print("[sdp]\n" + body[:cl].decode(errors="replace"))
    return text, body[:cl] if cl else b"", body[cl:] if cl else rest

req("OPTIONS", base)
desc_text, sdp, _ = req("DESCRIBE", base, {"Accept": "application/sdp"})
if "200" not in desc_text.split("\r\n")[0]:
    print("[probe] DESCRIBE did not return 200 — stopping. (try a different PATH)")
    sys.exit(2)
# SETUP track (interleaved TCP)
track = base + "/streamid=0"
setup_text, _, _ = req("SETUP", track, {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"})
for ln in setup_text.split("\r\n"):
    if ln.lower().startswith("session:"):
        session = ln.split(":")[1].strip().split(";")[0].strip()
print(f"[probe] session={session}")
req("PLAY", base, {"Range": "npt=0.000-"})

# read interleaved RTP: '$'(0x24) chan(1) len(2) + payload
print(f"\n[probe] reading interleaved RTP for {SECS}s ...")
s.settimeout(2)
deadline = time.time() + SECS
pkts = 0; bytes_rx = 0; fout = open(OUT, "wb")
sbuf = b""
def need(n):
    global sbuf
    while len(sbuf) < n:
        try:
            d = s.recv(65536)
        except socket.timeout:
            return False
        if not d: return False
        sbuf += d
    return True
NALT = {7: "SPS", 8: "PPS", 5: "IDR", 1: "non-IDR", 6: "SEI", 9: "AUD"}
naltypes = {}
SC = b"\x00\x00\x00\x01"
def emit_nal(nri_type, body):
    t = nri_type & 0x1f
    naltypes[t] = naltypes.get(t, 0) + 1
    fout.write(SC + bytes([nri_type]) + body)
fu_buf = None; fu_hdr = 0
while time.time() < deadline:
    if not need(4):
        break
    if sbuf[0] != 0x24:
        sbuf = sbuf[1:]; continue
    chan = sbuf[1]; ln = struct.unpack(">H", sbuf[2:4])[0]
    if not need(4 + ln):
        break
    pkt = sbuf[4:4+ln]; sbuf = sbuf[4+ln:]
    pkts += 1; bytes_rx += ln
    if chan != 0 or ln <= 12:
        continue
    payload = pkt[12:]                      # strip 12-byte RTP header
    if not payload:
        continue
    nt = payload[0] & 0x1f                  # RFC 6184 NAL unit type
    if 1 <= nt <= 23:                       # single NAL unit
        emit_nal(payload[0], payload[1:])
    elif nt == 24:                          # STAP-A (aggregated)
        o = 1
        while o + 2 <= len(payload):
            sz = struct.unpack(">H", payload[o:o+2])[0]; o += 2
            if o + sz > len(payload): break
            emit_nal(payload[o], payload[o+1:o+sz]); o += sz
    elif nt == 28:                          # FU-A (fragmented)
        fu_ind = payload[0]; fu = payload[1]
        start = (fu & 0x80) != 0; end = (fu & 0x40) != 0
        real = (fu_ind & 0xe0) | (fu & 0x1f)
        if start:
            fu_buf = bytearray(); fu_hdr = real
        if fu_buf is not None:
            fu_buf += payload[2:]
            if end:
                emit_nal(fu_hdr, bytes(fu_buf)); fu_buf = None
fout.close()
named = {NALT.get(k, f"type{k}"): v for k, v in sorted(naltypes.items())}
decodable = (7 in naltypes and 8 in naltypes and 5 in naltypes)
print(f"\n[probe] RESULT: RTP packets={pkts} bytes={bytes_rx} -> {OUT}")
print(f"[probe] NAL units: {named}")
print(f"[probe] in-band SPS+PPS+IDR present: {decodable} (decodable by a standard player: {'YES' if decodable else 'NO'})")
print("[probe] " + ("VIDEO FLOWING [OK]" if pkts > 0 else "NO RTP [FAIL]"))
sys.exit(0 if (pkts > 0 and decodable) else (5 if pkts > 0 else 6))
try: s.close()
except: pass
