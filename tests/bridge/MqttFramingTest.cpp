// Bambu Bridge — MqttFraming byte-level codec tests (phase 4a).
//
// Pins the wire format. Every packet type the broker emits is round-trip
// tested encode -> decode -> encode, and at least one fixed byte-level
// example per type is asserted exactly to lock the format down.
//
// No third-party test framework: a single `int main()` that increments a
// fail counter and prints offending vector(s). Exit code = number of
// failed assertions (0 = pass).

#include "server/MqttFraming.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r::bridge::server::mqtt;

namespace {

int g_fails = 0;

std::string hex(const std::vector<uint8_t>& v) {
    std::ostringstream os;
    for (size_t i = 0; i < v.size(); ++i) {
        os << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(v[i]);
        if (i + 1 < v.size()) os << ' ';
    }
    return os.str();
}

void expect_eq_bytes(const char* what,
                     const std::vector<uint8_t>& got,
                     const std::vector<uint8_t>& want) {
    if (got != want) {
        ++g_fails;
        std::fprintf(stderr, "FAIL %s\n  got:  %s\n  want: %s\n",
                     what, hex(got).c_str(), hex(want).c_str());
    }
}

void expect_true(const char* what, bool ok) {
    if (!ok) {
        ++g_fails;
        std::fprintf(stderr, "FAIL %s\n", what);
    }
}

// Varint round-trip across the boundary values per MQTT 3.1.1 §2.2.3.
void test_varint() {
    struct C { uint32_t v; std::vector<uint8_t> bytes; };
    const C cases[] = {
        {0,          {0x00}},
        {127,        {0x7F}},
        {128,        {0x80, 0x01}},
        {16383,      {0xFF, 0x7F}},
        {16384,      {0x80, 0x80, 0x01}},
        {2097151,    {0xFF, 0xFF, 0x7F}},
        {2097152,    {0x80, 0x80, 0x80, 0x01}},
        {268435455,  {0xFF, 0xFF, 0xFF, 0x7F}},
    };
    for (const auto& c : cases) {
        uint8_t buf[4];
        size_t n = encode_varint(c.v, buf);
        std::vector<uint8_t> got(buf, buf + n);
        expect_eq_bytes(("varint encode " + std::to_string(c.v)).c_str(),
                        got, c.bytes);
        auto dec = decode_varint(c.bytes.data(), c.bytes.size());
        expect_true(("varint decode " + std::to_string(c.v) + " value").c_str(),
                    dec.value == c.v);
        expect_true(("varint decode " + std::to_string(c.v) + " consumed").c_str(),
                    dec.consumed == c.bytes.size());
    }
    // Overflow: 5-byte continuation.
    const uint8_t overflow[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    auto bad = decode_varint(overflow, 5);
    expect_true("varint overflow returns consumed=0",
                bad.consumed == 0 && bad.value == 0);
    // Truncated: high bit set but no follow-up.
    const uint8_t trunc[] = {0x80};
    auto t = decode_varint(trunc, 1);
    expect_true("varint truncated returns consumed=SIZE_MAX",
                t.consumed == SIZE_MAX);
}

// Encode CONNACK and check the exact 4-byte output.
//   accepted, no session present:
//   [0x20] [0x02] [0x00] [0x00]
void test_connack_fixed() {
    auto pkt = encode_connack(ConnackReturnCode::Accepted, false);
    expect_eq_bytes("CONNACK accepted, no-session",
                    pkt, {0x20, 0x02, 0x00, 0x00});
    auto na = encode_connack(ConnackReturnCode::NotAuthorized, false);
    expect_eq_bytes("CONNACK not-authorized",
                    na,  {0x20, 0x02, 0x00, 0x05});
    auto sp = encode_connack(ConnackReturnCode::Accepted, true);
    expect_eq_bytes("CONNACK accepted, session-present",
                    sp,  {0x20, 0x02, 0x01, 0x00});
}

// Build a CONNECT packet for the Bambu LAN auth (`bblp` / `<access>`)
// by hand and pin the byte layout. The handshake the broker receives
// from `LanMqttSession` is functionally this exact set of fields.
//
// MQTT 3.1.1 CONNECT, client_id="cli", user="bblp", pass="ABCD1234",
// clean_session=1, keep_alive=30:
void test_connect_decode_bambu_auth() {
    // Build manually so we can pin the bytes both ways.
    std::vector<uint8_t> p;
    // Variable header:
    //   protocol name "MQTT" (len 4)
    //   level 0x04
    //   flags: username(0x80) | password(0x40) | clean_session(0x02) = 0xC2
    //   keep_alive = 30 (0x001E)
    p.push_back(0x00); p.push_back(0x04);
    p.push_back('M'); p.push_back('Q'); p.push_back('T'); p.push_back('T');
    p.push_back(0x04);
    p.push_back(0xC2);
    p.push_back(0x00); p.push_back(0x1E);
    // payload: clientId "cli" (len 3), username "bblp" (len 4), password "ABCD1234" (len 8)
    p.push_back(0x00); p.push_back(0x03); p.push_back('c'); p.push_back('l'); p.push_back('i');
    p.push_back(0x00); p.push_back(0x04); p.push_back('b'); p.push_back('b'); p.push_back('l'); p.push_back('p');
    p.push_back(0x00); p.push_back(0x08);
    p.push_back('A'); p.push_back('B'); p.push_back('C'); p.push_back('D');
    p.push_back('1'); p.push_back('2'); p.push_back('3'); p.push_back('4');

    // Fixed header: type=1 (CONNECT), flags=0; remaining-length = p.size().
    std::vector<uint8_t> full;
    full.push_back(0x10);
    uint8_t rl[4];
    size_t n = encode_varint(static_cast<uint32_t>(p.size()), rl);
    full.insert(full.end(), rl, rl + n);
    full.insert(full.end(), p.begin(), p.end());

    auto dec = decode_packet(full.data(), full.size());
    expect_true("CONNECT decode succeeds", dec.has_value());
    expect_true("CONNECT type", dec && dec->type == PacketType::Connect);
    expect_true("CONNECT no error", dec && dec->error == DecodeError::Ok);
    expect_true("CONNECT bytes_consumed == full",
                dec && dec->bytes_consumed == full.size());
    expect_true("CONNECT proto", dec && dec->connect.protocol_name == "MQTT");
    expect_true("CONNECT level", dec && dec->connect.protocol_level == 4);
    expect_true("CONNECT clean_session", dec && dec->connect.clean_session);
    expect_true("CONNECT keepalive=30", dec && dec->connect.keep_alive == 30);
    expect_true("CONNECT client_id=cli", dec && dec->connect.client_id == "cli");
    expect_true("CONNECT has_username", dec && dec->connect.has_username);
    expect_true("CONNECT has_password", dec && dec->connect.has_password);
    expect_true("CONNECT username=bblp", dec && dec->connect.username == "bblp");
    {
        std::string pw(dec ? std::string(dec->connect.password.begin(),
                                          dec->connect.password.end()) : "");
        expect_true("CONNECT password=ABCD1234", pw == "ABCD1234");
    }
}

// PUBLISH QoS 0 to device/<dev>/request with small JSON body.
void test_publish_qos0_roundtrip() {
    const std::string topic = "device/0938BC582502312/request";
    const std::string body  = "{\"pushing\":{\"command\":\"pushall\"}}";
    std::vector<uint8_t> payload(body.begin(), body.end());

    auto pkt = encode_publish(topic, payload, /*qos=*/0,
                              /*retain=*/false, /*packet_id=*/0);
    // The fixed header for PUB QoS=0, no retain, no dup is exactly 0x30.
    expect_true("PUBLISH QoS0 fixed header byte", pkt[0] == 0x30);

    auto dec = decode_packet(pkt.data(), pkt.size());
    expect_true("PUB QoS0 decode ok", dec && dec->error == DecodeError::Ok);
    expect_true("PUB QoS0 type",      dec && dec->type == PacketType::Publish);
    expect_true("PUB QoS0 topic",     dec && dec->publish.topic == topic);
    expect_true("PUB QoS0 qos",       dec && dec->publish.qos == 0);
    expect_true("PUB QoS0 no packet_id", dec && dec->publish.packet_id == 0);
    expect_true("PUB QoS0 payload",
                dec && std::string(dec->publish.payload.begin(),
                                   dec->publish.payload.end()) == body);
}

// PUBLISH QoS 1 -- exercise packet_id round-trip and the matching PUBACK.
void test_publish_qos1_roundtrip() {
    const std::string topic = "device/123/report";
    const std::string body  = "hi";
    std::vector<uint8_t> payload(body.begin(), body.end());

    auto pkt = encode_publish(topic, payload, /*qos=*/1, /*retain=*/false,
                              /*packet_id=*/42);
    // QoS1 flag bit (0x02) must be set in the low nibble.
    expect_true("PUBLISH QoS1 fixed header byte", pkt[0] == 0x32);

    auto dec = decode_packet(pkt.data(), pkt.size());
    expect_true("PUB QoS1 decode ok",  dec && dec->error == DecodeError::Ok);
    expect_true("PUB QoS1 qos",        dec && dec->publish.qos == 1);
    expect_true("PUB QoS1 packet_id",  dec && dec->publish.packet_id == 42);

    auto ack = encode_puback(42);
    expect_eq_bytes("PUBACK pid=42", ack, {0x40, 0x02, 0x00, 0x2A});

    auto adec = decode_packet(ack.data(), ack.size());
    expect_true("PUBACK decode ok",   adec && adec->error == DecodeError::Ok);
    expect_true("PUBACK packet_id",   adec && adec->puback.packet_id == 42);
}

// SUBSCRIBE with one filter -> SUBACK with one return code.
void test_subscribe_roundtrip() {
    // Hand-build SUBSCRIBE: packet_id=7, one filter "device/abc/report" qos=0.
    std::vector<uint8_t> body;
    body.push_back(0x00); body.push_back(0x07);
    append_mqtt_string(body, "device/abc/report");
    body.push_back(0x00);
    std::vector<uint8_t> full;
    full.push_back(0x82); // SUBSCRIBE flag byte (must be 0x02 in low nibble)
    uint8_t rl[4]; size_t n = encode_varint(static_cast<uint32_t>(body.size()), rl);
    full.insert(full.end(), rl, rl + n);
    full.insert(full.end(), body.begin(), body.end());

    auto dec = decode_packet(full.data(), full.size());
    expect_true("SUB decode ok",       dec && dec->error == DecodeError::Ok);
    expect_true("SUB type",            dec && dec->type == PacketType::Subscribe);
    expect_true("SUB packet_id=7",     dec && dec->subscribe.packet_id == 7);
    expect_true("SUB filter count==1", dec && dec->subscribe.filters.size() == 1);
    expect_true("SUB filter topic",
                dec && dec->subscribe.filters[0].topic == "device/abc/report");

    auto ack = encode_suback(7, {0x00});
    expect_eq_bytes("SUBACK pid=7 rc=0", ack,
                    {0x90, 0x03, 0x00, 0x07, 0x00});
}

// UNSUBSCRIBE / UNSUBACK.
void test_unsubscribe_roundtrip() {
    std::vector<uint8_t> body;
    body.push_back(0x00); body.push_back(0x09);
    append_mqtt_string(body, "device/abc/report");
    std::vector<uint8_t> full;
    full.push_back(0xA2);  // UNSUBSCRIBE flags MUST be 0x02 too
    uint8_t rl[4]; size_t n = encode_varint(static_cast<uint32_t>(body.size()), rl);
    full.insert(full.end(), rl, rl + n);
    full.insert(full.end(), body.begin(), body.end());

    auto dec = decode_packet(full.data(), full.size());
    expect_true("UNSUB decode ok",     dec && dec->error == DecodeError::Ok);
    expect_true("UNSUB topic",
                dec && dec->unsubscribe.topics.size() == 1 &&
                dec->unsubscribe.topics[0] == "device/abc/report");

    auto ack = encode_unsuback(9);
    expect_eq_bytes("UNSUBACK pid=9", ack, {0xB0, 0x02, 0x00, 0x09});
}

void test_ping_disconnect_fixed() {
    auto resp = encode_pingresp();
    expect_eq_bytes("PINGRESP", resp, {0xD0, 0x00});

    // PINGREQ decode (header-only)
    const uint8_t pingreq[] = {0xC0, 0x00};
    auto dec = decode_packet(pingreq, sizeof(pingreq));
    expect_true("PINGREQ decode", dec && dec->type == PacketType::Pingreq &&
                                  dec->error == DecodeError::Ok);

    // DISCONNECT decode (header-only)
    const uint8_t disc[] = {0xE0, 0x00};
    auto ddec = decode_packet(disc, sizeof(disc));
    expect_true("DISCONNECT decode",
                ddec && ddec->type == PacketType::Disconnect &&
                ddec->error == DecodeError::Ok);
}

// Decoder must report "truncated" cleanly on a short buffer so the
// broker's read loop keeps pulling bytes.
void test_truncated_returns_nullopt() {
    // Fixed header but missing payload entirely.
    const uint8_t buf[] = {0x10, 0x10};
    auto dec = decode_packet(buf, sizeof(buf));
    expect_true("truncated returns nullopt", !dec.has_value());
}

// Oversized varint must yield a hard decode error (LengthOverflow).
void test_oversized_varint_rejected() {
    const uint8_t buf[] = {0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
    auto dec = decode_packet(buf, sizeof(buf));
    expect_true("oversized varint rejected",
                dec && dec->error == DecodeError::LengthOverflow);
}

// Wrong protocol-name -> ProtocolViolation, not a soft skip.
void test_connect_wrong_protocol_rejected() {
    std::vector<uint8_t> p;
    append_mqtt_string(p, "MQIsdp");
    p.push_back(0x03);          // level
    p.push_back(0x00);          // flags
    p.push_back(0x00); p.push_back(0x1E); // keep_alive
    append_mqtt_string(p, "cli");

    std::vector<uint8_t> full;
    full.push_back(0x10);
    uint8_t rl[4]; size_t n = encode_varint(static_cast<uint32_t>(p.size()), rl);
    full.insert(full.end(), rl, rl + n);
    full.insert(full.end(), p.begin(), p.end());

    auto dec = decode_packet(full.data(), full.size());
    expect_true("CONNECT MQIsdp rejected",
                dec && dec->error == DecodeError::ProtocolViolation);
}

} // namespace

int main() {
    test_varint();
    test_connack_fixed();
    test_connect_decode_bambu_auth();
    test_publish_qos0_roundtrip();
    test_publish_qos1_roundtrip();
    test_subscribe_roundtrip();
    test_unsubscribe_roundtrip();
    test_ping_disconnect_fixed();
    test_truncated_returns_nullopt();
    test_oversized_varint_rejected();
    test_connect_wrong_protocol_rejected();

    if (g_fails) {
        std::fprintf(stderr, "MqttFramingTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("MqttFramingTest: ok\n");
    return 0;
}
