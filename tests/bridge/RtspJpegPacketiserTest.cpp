// Bambu Bridge — RFC 2435 RTP/JPEG packetiser unit test (ship-11d).
//
// Black-box test for the two production helpers exposed via
// `server/RtspJpegPacketiser.hpp`:
//
//   - rtp_jpeg_parse  : JFIF → (width, height, type, qtables, scan span)
//   - rtp_jpeg_build_packets : (parse + scan span) → RTP payload byte vectors
//
// We construct a synthetic JFIF (the smallest valid-shape blob the parser
// will accept) feed it through both, and assert on:
//
//   - parser surfaces SOF0 width/height
//   - parser picks Type=0 (yuv422) for H=2,V=1 sampling
//   - parser collects exactly one 64-byte 8-bit quant table from the DQT
//   - parser pins scan_off / scan_len to the entropy-coded bytes between
//     SOS (exclusive) and EOI (exclusive)
//   - parser tolerates restart markers (FF D0..D7) embedded in scan data
//
//   - packet 0 carries: 8-byte JPEG hdr (frag_off=0, type=0, Q=255,
//                       w_field=h_field=2 for 16x16), 4-byte QT hdr
//                       (precision=0, length=64), 64-byte qtable
//   - packet N (last) ends exactly at scan_len: total scan bytes across
//     all packets sums to jp.scan_len
//   - fragment offset advances monotonically by the per-packet scan-chunk
//   - tiny max_payload forces fragmentation (multi-packet); large
//     max_payload produces exactly one packet
//
// No TLS, no sockets — pure byte-level assertions. Skip code 77 reserved
// for future seam additions that need runtime services.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "server/RtspJpegPacketiser.hpp"

using Slic3r::bridge::server::RtpJpegParse;
using Slic3r::bridge::server::rtp_jpeg_parse;
using Slic3r::bridge::server::rtp_jpeg_build_packets;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

// Build a synthetic JFIF that the parser accepts:
//   SOI (FF D8)
//   DQT (FF DB) len=67  : 1 byte (Pq|Tq=0x00) + 64 zeroed quant entries
//   SOF0 (FF C0) len=11 : precision=8, height=16, width=16, Nf=1,
//                         (C1=1, H1V1=0x21, Tq1=0)
//   SOS (FF DA) len=8   : Ns=1, (Cs1=1, Td1=0|Ta1=0), Ss=0, Se=63, AhAl=0
//   scan_bytes          : 6 arbitrary bytes (non-marker), then FF 00
//                         (stuffed byte), then FF D0 (RST0), then 2 bytes
//   EOI (FF D9)
//
// Resulting scan-data span (between SOS-end and EOI-start) is exactly 12 B.
std::vector<uint8_t> build_synthetic_jfif() {
    std::vector<uint8_t> b;
    auto u8 = [&](uint8_t v){ b.push_back(v); };
    auto u16be = [&](uint16_t v){
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v & 0xFF));
    };

    // SOI
    u8(0xFF); u8(0xD8);

    // DQT: 1 + 64 + 2 (len bytes) = 67
    u8(0xFF); u8(0xDB);
    u16be(67);
    u8(0x00);                       // Pq=0 (8-bit), Tq=0
    for (int i = 0; i < 64; ++i) u8(static_cast<uint8_t>(i + 1));

    // SOF0: 11 bytes total incl length
    u8(0xFF); u8(0xC0);
    u16be(11);
    u8(8);                          // precision
    u16be(16);                      // height
    u16be(16);                      // width
    u8(1);                          // Nf
    u8(1); u8(0x21); u8(0);         // C=1, H=2 V=1 (yuv422), Tq=0

    // SOS: 8 bytes total incl length
    u8(0xFF); u8(0xDA);
    u16be(8);
    u8(1);                          // Ns
    u8(1); u8(0x00);                // Cs=1, Td=0|Ta=0
    u8(0); u8(63); u8(0);           // Ss, Se, Ah|Al

    // Scan data: 6 plain bytes, then FF 00 (stuffed), then FF D0 (RST0),
    // then 2 more plain bytes — total 12 entropy-coded bytes.
    u8(0x01); u8(0x02); u8(0x03); u8(0x04); u8(0x05); u8(0x06);
    u8(0xFF); u8(0x00);
    u8(0xFF); u8(0xD0);
    u8(0x07); u8(0x08);

    // EOI
    u8(0xFF); u8(0xD9);
    return b;
}

void synth_packet_assertions() {
    auto jfif = build_synthetic_jfif();
    RtpJpegParse jp = rtp_jpeg_parse(jfif.data(), jfif.size());

    check(jp.ok,                          "parser: ok flag set");
    check(jp.width  == 16,                "parser: width=16");
    check(jp.height == 16,                "parser: height=16");
    check(jp.type   == 0,                 "parser: type=0 (yuv422)");
    check(jp.qtables.size() == 64,        "parser: one 64-byte quant table");
    if (jp.qtables.size() == 64) {
        check(jp.qtables[0]  == 1 &&
              jp.qtables[63] == 64,        "parser: quant table bytes intact");
    }
    check(jp.scan_len == 12,              "parser: scan_len = 12 bytes");
    check(jp.scan_off > 0 &&
          jp.scan_off + jp.scan_len + 2 == jfif.size(),
                                          "parser: scan span ends 2 B before EOI");

    // -------- One-packet path (max_payload >> scan_len) -----------------
    uint16_t seq = 100;
    auto pkts = rtp_jpeg_build_packets(seq, /*ts=*/0, /*ssrc=*/0,
                                        jp,
                                        jfif.data() + jp.scan_off,
                                        jp.scan_len,
                                        /*max_payload=*/1400);
    check(pkts.size() == 1,               "single packet for tiny frame");
    if (pkts.size() == 1) {
        const auto& p = pkts[0];
        // 8 B JPEG hdr + 4 B QT hdr + 64 B qtable + 12 B scan = 88
        check(p.size() == 8 + 4 + 64 + 12,
              "single-packet size = 8+4+64+12");
        // JPEG header inspection.
        check(p[0] == 0x00,               "type-specific = 0");
        check(p[1] == 0 && p[2] == 0 && p[3] == 0,
                                          "frag offset = 0");
        check(p[4] == 0,                  "type field = 0 (yuv422)");
        check(p[5] == 255,                "Q = 255");
        check(p[6] == 2,                  "width field = ceil(16/8) = 2");
        check(p[7] == 2,                  "height field = 2");
        // QT header.
        check(p[8] == 0,                  "QT MBZ = 0");
        check(p[9] == 0,                  "QT precision = 0 (8-bit)");
        check(p[10] == 0 && p[11] == 64,  "QT length = 64");
        // Scan bytes copied verbatim (parser does not de-stuff).
        const uint8_t expect_scan[12] = {
            0x01,0x02,0x03,0x04,0x05,0x06,
            0xFF,0x00,
            0xFF,0xD0,
            0x07,0x08,
        };
        bool scan_ok = true;
        for (int i = 0; i < 12; ++i) {
            if (p[8 + 4 + 64 + i] != expect_scan[i]) { scan_ok = false; break; }
        }
        check(scan_ok,                    "scan bytes copied verbatim");
    }

    // -------- Fragmented path (force multi-packet) -----------------------
    // Set max_payload to JPEG hdr (8) + QT hdr (4+64) + 5 scan bytes = 81.
    // That puts 5 scan bytes in pkt 0 and forces the remaining 7 into
    // subsequent packets of size 8 + (chunk) — capped at max_payload-8 = 73.
    seq = 200;
    auto frags = rtp_jpeg_build_packets(seq, /*ts=*/0, /*ssrc=*/0,
                                         jp,
                                         jfif.data() + jp.scan_off,
                                         jp.scan_len,
                                         /*max_payload=*/81);
    check(frags.size() >= 2,              "fragmented path produces >=2 packets");
    // Sum of scan bytes across all fragments == jp.scan_len.
    size_t total_scan = 0;
    uint32_t prev_off = 0;
    for (size_t i = 0; i < frags.size(); ++i) {
        const auto& p = frags[i];
        check(p.size() >= 8,              "frag has at least 8-byte JPEG hdr");
        const uint32_t fo =
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) <<  8) |
             static_cast<uint32_t>(p[3]);
        check(i == 0 ? fo == 0 : fo == prev_off,
              "fragment offset advances monotonically");
        check(p[5] == 255,                "Q=255 on every fragment");
        check(p[6] == 2 && p[7] == 2,     "dimensions on every fragment");
        size_t scan_in_pkt;
        if (i == 0) {
            // pkt 0 carries QT header → scan starts at offset 8+4+64=76.
            check(p.size() >= 76,         "pkt 0 carries QT header");
            scan_in_pkt = p.size() - 76;
        } else {
            scan_in_pkt = p.size() - 8;
        }
        total_scan += scan_in_pkt;
        prev_off    = fo + static_cast<uint32_t>(scan_in_pkt);
    }
    check(total_scan == jp.scan_len,
          "fragments sum to jp.scan_len bytes");
}

void degenerate_assertions() {
    // Empty buffer → parser fails cleanly.
    RtpJpegParse jp0 = rtp_jpeg_parse(nullptr, 0);
    check(!jp0.ok,                        "empty buffer: parser rejects");

    // Buffer that's only SOI/EOI (no SOF/SOS/DQT).
    const uint8_t soi_eoi[] = { 0xFF, 0xD8, 0xFF, 0xD9 };
    RtpJpegParse jp1 = rtp_jpeg_parse(soi_eoi, sizeof(soi_eoi));
    check(!jp1.ok,                        "SOI+EOI only: parser rejects");

    // build_rtp_jpeg_packets on zero-length scan → empty packet vector.
    RtpJpegParse jp2;
    jp2.ok = true; jp2.width = 16; jp2.height = 16;
    jp2.qtables.assign(64, 0);
    uint16_t seq = 0;
    auto p = rtp_jpeg_build_packets(seq, 0, 0, jp2, nullptr, 0, 1400);
    check(p.empty(),                      "zero-length scan: no packets");
}

} // namespace

int main() {
    synth_packet_assertions();
    degenerate_assertions();
    if (g_fails) {
        std::fprintf(stderr, "RtspJpegPacketiserTest: %d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("RtspJpegPacketiserTest: ok\n");
    return 0;
}
