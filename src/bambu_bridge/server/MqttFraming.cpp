// Bambu Bridge — MQTT 3.1.1 packet codec (phase 4a).

#include "MqttFraming.hpp"

#include <cstring>

namespace Slic3r {
namespace bridge {
namespace server {
namespace mqtt {

// ---------------------------------------------------------------------------
// Low-level helpers.
// ---------------------------------------------------------------------------

size_t encode_varint(uint32_t value, uint8_t out[4]) {
    // MQTT 3.1.1 §2.2.3: at most 4 bytes; high bit set means "another byte
    // follows", low 7 bits carry the payload, little-endian over 7-bit groups.
    size_t i = 0;
    do {
        uint8_t byte = value & 0x7F;
        value >>= 7;
        if (value) byte |= 0x80;
        out[i++] = byte;
    } while (value && i < 4);
    return i;
}

VarintDecode decode_varint(const uint8_t* buf, size_t len) {
    VarintDecode r;
    uint32_t multiplier = 1;
    uint32_t value      = 0;
    size_t   i          = 0;
    while (true) {
        if (i >= len) {
            // Truncated — caller should buffer more bytes and retry.
            r.consumed = SIZE_MAX;
            return r;
        }
        uint8_t byte = buf[i++];
        value += static_cast<uint32_t>(byte & 0x7F) * multiplier;
        if ((byte & 0x80) == 0) {
            r.value    = value;
            r.consumed = i;
            return r;
        }
        if (i == 4) {
            // 5th continuation byte → overflow.
            r.consumed = 0;
            r.value    = 0;
            return r;
        }
        multiplier *= 128;
    }
}

std::optional<StringDecode> decode_mqtt_string(const uint8_t* buf, size_t len) {
    if (len < 2) return std::nullopt;
    uint16_t slen = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    if (len < 2u + slen) return std::nullopt;
    StringDecode r;
    r.value.assign(reinterpret_cast<const char*>(buf + 2), slen);
    r.consumed = 2u + slen;
    return r;
}

void append_mqtt_string(std::vector<uint8_t>& out, const std::string& s) {
    uint16_t n = static_cast<uint16_t>(s.size());
    out.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(n & 0xFF));
    out.insert(out.end(), s.begin(), s.end());
}

void append_uint16_be(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

namespace {

// Build the final packet by prepending the fixed header (type+flags +
// varint remaining-length) to the variable-header/payload `body`.
std::vector<uint8_t> finalize(PacketType type, uint8_t flags,
                              const std::vector<uint8_t>& body) {
    uint8_t hdr0 = static_cast<uint8_t>(
        (static_cast<uint8_t>(type) << 4) | (flags & 0x0F));
    uint8_t rl[4];
    size_t  rl_n = encode_varint(static_cast<uint32_t>(body.size()), rl);

    std::vector<uint8_t> out;
    out.reserve(1 + rl_n + body.size());
    out.push_back(hdr0);
    out.insert(out.end(), rl, rl + rl_n);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Encoders.
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_connack(ConnackReturnCode rc, bool session_present) {
    // Fixed header: type=2, flags=0. Remaining length = 2.
    // Variable header: byte0 = session_present (LSB), byte1 = return code.
    std::vector<uint8_t> body;
    body.reserve(2);
    body.push_back(session_present ? 0x01 : 0x00);
    body.push_back(static_cast<uint8_t>(rc));
    return finalize(PacketType::Connack, 0, body);
}

std::vector<uint8_t> encode_publish(const std::string& topic,
                                    const std::vector<uint8_t>& payload,
                                    uint8_t qos, bool retain,
                                    uint16_t packet_id, bool dup) {
    // Flags nibble: DUP(3) | QoS(2..1) | RETAIN(0).
    uint8_t flags = 0;
    if (dup    && qos > 0) flags |= 0x08;
    if (qos == 1)          flags |= 0x02;
    if (qos == 2)          flags |= 0x04;
    if (retain)            flags |= 0x01;

    std::vector<uint8_t> body;
    body.reserve(2 + topic.size() + (qos > 0 ? 2 : 0) + payload.size());
    append_mqtt_string(body, topic);
    if (qos > 0) {
        append_uint16_be(body, packet_id);
    }
    body.insert(body.end(), payload.begin(), payload.end());
    return finalize(PacketType::Publish, flags, body);
}

std::vector<uint8_t> encode_puback(uint16_t packet_id) {
    std::vector<uint8_t> body;
    append_uint16_be(body, packet_id);
    return finalize(PacketType::Puback, 0, body);
}

std::vector<uint8_t> encode_suback(uint16_t packet_id,
                                   const std::vector<uint8_t>& return_codes) {
    std::vector<uint8_t> body;
    body.reserve(2 + return_codes.size());
    append_uint16_be(body, packet_id);
    body.insert(body.end(), return_codes.begin(), return_codes.end());
    return finalize(PacketType::Suback, 0, body);
}

std::vector<uint8_t> encode_unsuback(uint16_t packet_id) {
    std::vector<uint8_t> body;
    append_uint16_be(body, packet_id);
    return finalize(PacketType::Unsuback, 0, body);
}

std::vector<uint8_t> encode_pingresp() {
    // Header-only packet, remaining length = 0.
    return finalize(PacketType::Pingresp, 0, {});
}

// ---------------------------------------------------------------------------
// Decoder.
// ---------------------------------------------------------------------------

namespace {

// Helper that fills out a MqttPacket with an error and returns it.
MqttPacket fail(PacketType type, DecodeError err) {
    MqttPacket p;
    p.type           = type;
    p.error          = err;
    p.bytes_consumed = 0;
    return p;
}

// CONNECT body decoder. `body` points at the variable header (after the
// fixed header + remaining-length). `body_len` is the remaining-length
// value. Populates `out` on success.
bool decode_connect_body(const uint8_t* body, size_t body_len,
                         ConnectPacket& out, DecodeError& err) {
    // Protocol name + level + flags + keep_alive = at least 10 bytes for
    // "MQTT" / 7 bytes for "MQIsdp" -- enforce a minimum.
    auto pn = decode_mqtt_string(body, body_len);
    if (!pn) { err = DecodeError::Truncated; return false; }
    out.protocol_name = std::move(pn->value);
    size_t off = pn->consumed;

    if (off + 4 > body_len) { err = DecodeError::Truncated; return false; }
    out.protocol_level = body[off++];
    out.connect_flags  = body[off++];
    out.keep_alive     = static_cast<uint16_t>((body[off] << 8) | body[off + 1]);
    off += 2;

    // Only accept MQTT 3.1.1 ("MQTT", level 4). 3.1 ("MQIsdp", level 3) is
    // observable in older firmware but Bambu uses 3.1.1 — for the bridge,
    // reject anything else as a protocol violation.
    if (!(out.protocol_name == "MQTT" && out.protocol_level == 4)) {
        err = DecodeError::ProtocolViolation;
        return false;
    }

    // Parse the CONNECT flags byte.
    const bool will_flag   = (out.connect_flags & 0x04) != 0;
    out.has_will           = will_flag;
    out.will_qos           = static_cast<uint8_t>((out.connect_flags >> 3) & 0x03);
    out.will_retain        = (out.connect_flags & 0x20) != 0;
    out.has_password       = (out.connect_flags & 0x40) != 0;
    out.has_username       = (out.connect_flags & 0x80) != 0;
    out.clean_session      = (out.connect_flags & 0x02) != 0;

    // Payload order per §3.1.3: ClientId, [WillTopic, WillMessage],
    // [Username], [Password].
    auto cid = decode_mqtt_string(body + off, body_len - off);
    if (!cid) { err = DecodeError::Truncated; return false; }
    out.client_id = std::move(cid->value);
    off += cid->consumed;

    if (will_flag) {
        auto wt = decode_mqtt_string(body + off, body_len - off);
        if (!wt) { err = DecodeError::Truncated; return false; }
        out.will_topic = std::move(wt->value);
        off += wt->consumed;

        // Will message is a binary-length-prefixed blob (not UTF-8).
        if (off + 2 > body_len) { err = DecodeError::Truncated; return false; }
        uint16_t wlen = static_cast<uint16_t>((body[off] << 8) | body[off + 1]);
        off += 2;
        if (off + wlen > body_len) { err = DecodeError::Truncated; return false; }
        out.will_payload.assign(body + off, body + off + wlen);
        off += wlen;
    }

    if (out.has_username) {
        auto u = decode_mqtt_string(body + off, body_len - off);
        if (!u) { err = DecodeError::Truncated; return false; }
        out.username = std::move(u->value);
        off += u->consumed;
    }
    if (out.has_password) {
        if (off + 2 > body_len) { err = DecodeError::Truncated; return false; }
        uint16_t plen = static_cast<uint16_t>((body[off] << 8) | body[off + 1]);
        off += 2;
        if (off + plen > body_len) { err = DecodeError::Truncated; return false; }
        out.password.assign(body + off, body + off + plen);
        off += plen;
    }

    // Trailing bytes after the password aren't legal in 3.1.1 but we just
    // ignore them rather than reject — Bambu firmware sends none in
    // practice, but quirky clients might pad.
    (void)off;
    return true;
}

bool decode_publish_body(const uint8_t* body, size_t body_len,
                         uint8_t flags, PublishPacket& out, DecodeError& err) {
    out.dup    = (flags & 0x08) != 0;
    out.qos    = static_cast<uint8_t>((flags >> 1) & 0x03);
    out.retain = (flags & 0x01) != 0;
    if (out.qos > 2) { err = DecodeError::Malformed; return false; }

    auto topic = decode_mqtt_string(body, body_len);
    if (!topic) { err = DecodeError::Truncated; return false; }
    out.topic = std::move(topic->value);
    size_t off = topic->consumed;

    if (out.qos > 0) {
        if (off + 2 > body_len) { err = DecodeError::Truncated; return false; }
        out.packet_id =
            static_cast<uint16_t>((body[off] << 8) | body[off + 1]);
        off += 2;
    } else {
        out.packet_id = 0;
    }

    if (off > body_len) { err = DecodeError::Malformed; return false; }
    out.payload.assign(body + off, body + body_len);
    return true;
}

bool decode_subscribe_body(const uint8_t* body, size_t body_len,
                           SubscribePacket& out, DecodeError& err) {
    if (body_len < 2) { err = DecodeError::Truncated; return false; }
    out.packet_id = static_cast<uint16_t>((body[0] << 8) | body[1]);
    size_t off = 2;
    while (off < body_len) {
        auto t = decode_mqtt_string(body + off, body_len - off);
        if (!t) { err = DecodeError::Truncated; return false; }
        off += t->consumed;
        if (off >= body_len) { err = DecodeError::Truncated; return false; }
        uint8_t qos = body[off++];
        if (qos > 2) { err = DecodeError::Malformed; return false; }
        SubscribeFilter f;
        f.topic = std::move(t->value);
        f.qos   = qos;
        out.filters.push_back(std::move(f));
    }
    if (out.filters.empty()) {
        // 3.1.1 §3.8.3: SUBSCRIBE must have at least one filter.
        err = DecodeError::ProtocolViolation;
        return false;
    }
    return true;
}

bool decode_unsubscribe_body(const uint8_t* body, size_t body_len,
                             UnsubscribePacket& out, DecodeError& err) {
    if (body_len < 2) { err = DecodeError::Truncated; return false; }
    out.packet_id = static_cast<uint16_t>((body[0] << 8) | body[1]);
    size_t off = 2;
    while (off < body_len) {
        auto t = decode_mqtt_string(body + off, body_len - off);
        if (!t) { err = DecodeError::Truncated; return false; }
        off += t->consumed;
        out.topics.push_back(std::move(t->value));
    }
    if (out.topics.empty()) {
        err = DecodeError::ProtocolViolation;
        return false;
    }
    return true;
}

bool decode_puback_body(const uint8_t* body, size_t body_len,
                        PubackPacket& out, DecodeError& err) {
    if (body_len < 2) { err = DecodeError::Truncated; return false; }
    out.packet_id = static_cast<uint16_t>((body[0] << 8) | body[1]);
    return true;
}

} // namespace

std::optional<MqttPacket> decode_packet(const uint8_t* buf, size_t len) {
    if (len < 2) return std::nullopt;     // need at least header + first len byte

    const uint8_t header = buf[0];
    const auto    type   = static_cast<PacketType>((header >> 4) & 0x0F);
    const uint8_t flags  = header & 0x0F;

    // Decode remaining-length varint at buf[1..].
    VarintDecode v = decode_varint(buf + 1, len - 1);
    if (v.consumed == SIZE_MAX) return std::nullopt;   // truncated
    if (v.consumed == 0) {
        // Overflow → protocol violation, not recoverable.
        MqttPacket p = fail(type, DecodeError::LengthOverflow);
        return p;
    }

    const size_t header_len    = 1 + v.consumed;
    const size_t total_len     = header_len + v.value;
    if (len < total_len) return std::nullopt;          // need more bytes

    const uint8_t* body     = buf + header_len;
    const size_t   body_len = v.value;

    MqttPacket p;
    p.type           = type;
    p.bytes_consumed = total_len;

    DecodeError err = DecodeError::Ok;
    switch (type) {
    case PacketType::Connect:
        if (!decode_connect_body(body, body_len, p.connect, err))
            return fail(type, err);
        break;

    case PacketType::Publish:
        if (!decode_publish_body(body, body_len, flags, p.publish, err))
            return fail(type, err);
        break;

    case PacketType::Subscribe:
        // 3.1.1 §3.8.1: SUBSCRIBE fixed-header flags MUST be 0x02.
        if (flags != 0x02) return fail(type, DecodeError::ProtocolViolation);
        if (!decode_subscribe_body(body, body_len, p.subscribe, err))
            return fail(type, err);
        break;

    case PacketType::Unsubscribe:
        // 3.1.1 §3.10.1: UNSUBSCRIBE fixed-header flags MUST be 0x02.
        if (flags != 0x02) return fail(type, DecodeError::ProtocolViolation);
        if (!decode_unsubscribe_body(body, body_len, p.unsubscribe, err))
            return fail(type, err);
        break;

    case PacketType::Puback:
        if (!decode_puback_body(body, body_len, p.puback, err))
            return fail(type, err);
        break;

    case PacketType::Pingreq:
    case PacketType::Disconnect:
    case PacketType::Pingresp:
        // Header-only packets; remaining length must be 0.
        if (body_len != 0) return fail(type, DecodeError::ProtocolViolation);
        break;

    case PacketType::Connack:
        // [session_present byte][return_code byte]. We don't populate a
        // typed field (the broker never receives CONNACK) — the test
        // client reads the raw 4 bytes directly. Just accept the body.
        if (body_len != 2) return fail(type, DecodeError::Malformed);
        break;

    case PacketType::Suback:
        // [packet_id MSB][packet_id LSB][per-filter rc...]. Same story —
        // broker doesn't consume SUBACKs; we just need to recognise the
        // packet so a test client can drain it from the recv buffer.
        if (body_len < 3) return fail(type, DecodeError::Malformed);
        break;

    case PacketType::Unsuback:
        if (body_len != 2) return fail(type, DecodeError::Malformed);
        break;

    // Stub-OK types: QoS 2 acks. We don't issue QoS 2 from the broker so
    // these shouldn't appear; flag as unsupported so the broker can drop
    // the connection rather than silently mis-handle.
    case PacketType::Pubrec:
    case PacketType::Pubrel:
    case PacketType::Pubcomp:
        return fail(type, DecodeError::UnsupportedType);

    default:
        return fail(type, DecodeError::Malformed);
    }

    return p;
}

} // namespace mqtt
} // namespace server
} // namespace bridge
} // namespace Slic3r
