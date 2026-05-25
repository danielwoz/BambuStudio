// Bambu Bridge — MQTT 3.1.1 packet codec (phase 4a).
//
// Pure-stdlib encode/decode for the subset of MQTT 3.1.1 control packets
// Bambu LAN sessions actually use. The framing logic is deliberately
// isolated from the broker so phase-11's `tools/wire_diff.py` can reuse
// the exact same encoders for byte-for-byte comparisons against captured
// real-printer traffic.
//
// Supported control packets:
//   - CONNECT      (decode)         server side
//   - CONNACK      (encode)         server -> client
//   - PUBLISH      (decode/encode)  bidirectional, QoS 0+1
//   - PUBACK       (decode/encode)  bidirectional, QoS 1 ack
//   - SUBSCRIBE    (decode)         client -> server
//   - SUBACK       (encode)         server -> client
//   - UNSUBSCRIBE  (decode)         client -> server
//   - UNSUBACK     (encode)         server -> client
//   - PINGREQ      (decode)         client -> server (header-only)
//   - PINGRESP     (encode)         server -> client (header-only)
//   - DISCONNECT   (decode)         client -> server (header-only)
//
// Layout notes (MQTT 3.1.1 fixed header):
//   byte 0:    [packet-type:4][flags:4]
//   bytes 1+:  remaining length, variable-byte encoded (1-4 bytes)
//   then:      variable header (per packet type) + payload
//
// The encoder API returns owning `std::vector<uint8_t>` so callers can
// hand the buffer straight to a TLS writer. Decoder API is non-throwing;
// malformed packets surface as `std::nullopt` + a `DecodeError` enum so
// the broker can decide whether to disconnect or just skip.

#ifndef SLIC3R_BAMBU_BRIDGE_SERVER_MQTT_FRAMING_HPP
#define SLIC3R_BAMBU_BRIDGE_SERVER_MQTT_FRAMING_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace server {
namespace mqtt {

// Control-packet types per MQTT 3.1.1 §2.2.1.
enum class PacketType : uint8_t {
    Reserved    = 0,
    Connect     = 1,
    Connack     = 2,
    Publish     = 3,
    Puback      = 4,
    Pubrec      = 5,
    Pubrel      = 6,
    Pubcomp     = 7,
    Subscribe   = 8,
    Suback      = 9,
    Unsubscribe = 10,
    Unsuback    = 11,
    Pingreq     = 12,
    Pingresp    = 13,
    Disconnect  = 14,
};

// CONNACK return codes per MQTT 3.1.1 §3.2.2.3.
enum class ConnackReturnCode : uint8_t {
    Accepted               = 0,
    UnacceptableProtocol   = 1,
    IdentifierRejected     = 2,
    ServerUnavailable      = 3,
    BadCredentials         = 4,
    NotAuthorized          = 5,
};

// Reasons the decoder may reject a packet. The broker maps these onto
// MQTT-level actions (close socket, ignore packet, etc.).
enum class DecodeError {
    Ok                   = 0,
    Truncated            = 1,   // partial buffer; need more bytes
    Malformed            = 2,   // syntactically invalid
    UnsupportedType      = 3,   // PUBREC/PUBREL/PUBCOMP/etc.
    LengthOverflow       = 4,   // remaining-length encoding > 4 bytes
    ProtocolViolation    = 5,   // e.g. CONNECT with wrong protocol-name
};

// ---- Parsed-packet structures -------------------------------------------

struct ConnectPacket {
    std::string protocol_name;     // "MQTT" for 3.1.1, "MQIsdp" for 3.1
    uint8_t     protocol_level   = 0; // 4 for 3.1.1
    uint8_t     connect_flags    = 0;
    uint16_t    keep_alive       = 0;
    std::string client_id;
    // Will/last-will fields — we parse them but the broker doesn't act on
    // them in phase 4 (real Bambu LAN sessions don't use will).
    bool        has_will         = false;
    std::string will_topic;
    std::vector<uint8_t> will_payload;
    uint8_t     will_qos         = 0;
    bool        will_retain      = false;
    bool        has_username     = false;
    bool        has_password     = false;
    std::string username;
    std::vector<uint8_t> password; // MQTT 3.1.1 spec says binary; bblp uses ASCII
    bool        clean_session    = false;
};

struct PublishPacket {
    std::string          topic;
    uint16_t             packet_id = 0;     // 0 for QoS 0
    uint8_t              qos       = 0;
    bool                 retain    = false;
    bool                 dup       = false;
    std::vector<uint8_t> payload;
};

struct SubscribeFilter {
    std::string topic;
    uint8_t     qos = 0;
};

struct SubscribePacket {
    uint16_t                     packet_id = 0;
    std::vector<SubscribeFilter> filters;
};

struct UnsubscribePacket {
    uint16_t                 packet_id = 0;
    std::vector<std::string> topics;
};

struct PubackPacket {
    uint16_t packet_id = 0;
};

// Tagged union returned by decode_packet(). The `type` field tells the
// caller which member of the variant is meaningful; on errors only
// `error` is set and the other fields are default-constructed.
struct MqttPacket {
    PacketType  type  = PacketType::Reserved;
    DecodeError error = DecodeError::Ok;

    // Total number of bytes consumed from the input buffer. Callers
    // advance their read cursor by this amount on success.
    size_t bytes_consumed = 0;

    // Only the field matching `type` is populated.
    ConnectPacket     connect;
    PublishPacket     publish;
    SubscribePacket   subscribe;
    UnsubscribePacket unsubscribe;
    PubackPacket      puback;
};

// ---- Decoder ------------------------------------------------------------

// Reads exactly one MQTT control packet from the start of `buf`. On
// success returns a populated MqttPacket with `bytes_consumed` set;
// on truncation returns nullopt (caller should buffer more bytes and
// retry); on protocol error returns a packet with `error != Ok` and
// `bytes_consumed == 0` (caller should disconnect).
//
// `len` is the number of valid bytes in `buf`.
std::optional<MqttPacket> decode_packet(const uint8_t* buf, size_t len);

// ---- Encoders -----------------------------------------------------------
//
// All encoders return a freshly-allocated byte buffer. None of them
// touch any shared state, so they're safe to call from any thread.

std::vector<uint8_t> encode_connack(ConnackReturnCode rc,
                                    bool session_present = false);

std::vector<uint8_t> encode_publish(const std::string& topic,
                                    const std::vector<uint8_t>& payload,
                                    uint8_t  qos,
                                    bool     retain,
                                    uint16_t packet_id /* 0 for QoS 0 */,
                                    bool     dup = false);

std::vector<uint8_t> encode_puback(uint16_t packet_id);

std::vector<uint8_t> encode_suback(uint16_t packet_id,
                                   const std::vector<uint8_t>& return_codes);

std::vector<uint8_t> encode_unsuback(uint16_t packet_id);

std::vector<uint8_t> encode_pingresp();

// ---- Low-level helpers (exposed for testing + wire-diff reuse) ---------

// Variable-byte remaining-length encoder per MQTT 3.1.1 §2.2.3.
//
// Encodes values 0..268435455 (0x0FFFFFFF). Returns number of bytes
// written into `out` (1..4). Caller must guarantee `out` has 4 bytes.
size_t encode_varint(uint32_t value, uint8_t out[4]);

// Variable-byte remaining-length decoder. Returns:
//   { value, consumed_bytes }   on success
//   { 0, 0 }                    on overflow (>4 bytes) — caller treats as error
//   { 0, SIZE_MAX }             on truncated (need more bytes)
struct VarintDecode {
    uint32_t value    = 0;
    size_t   consumed = 0;
};
VarintDecode decode_varint(const uint8_t* buf, size_t len);

// MQTT UTF-8 string: 2-byte big-endian length + bytes. `len` is the
// total remaining buffer; returns the parsed string and number of
// bytes consumed (2 + body_len), or nullopt if truncated/malformed.
struct StringDecode {
    std::string value;
    size_t      consumed = 0;
};
std::optional<StringDecode> decode_mqtt_string(const uint8_t* buf, size_t len);

// Appends a 2-byte big-endian length prefix followed by the UTF-8 body.
void append_mqtt_string(std::vector<uint8_t>& out, const std::string& s);

// Appends a 2-byte big-endian uint16.
void append_uint16_be(std::vector<uint8_t>& out, uint16_t v);

} // namespace mqtt
} // namespace server
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_SERVER_MQTT_FRAMING_HPP
