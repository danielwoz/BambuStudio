// Bambu Bridge — minimal TLS MQTT client for tests (phase 4).

#include "MqttTestClient.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

namespace Slic3r {
namespace bridge {
namespace test {

namespace {

void init_openssl_once() {
    static struct Init {
        Init() {
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();
        }
    } s_init;
    (void)s_init;
}

// Write the full buffer through SSL_write, looping on partial writes.
bool ssl_write_all(SSL* ssl, const std::vector<uint8_t>& buf) {
    size_t off = 0;
    while (off < buf.size()) {
        int n = SSL_write(ssl, buf.data() + off,
                          static_cast<int>(buf.size() - off));
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

} // namespace

MqttTestClient::MqttTestClient() { init_openssl_once(); }
MqttTestClient::~MqttTestClient() { close(); }

void MqttTestClient::close() {
    if (m_ssl) {
        SSL_shutdown(reinterpret_cast<SSL*>(m_ssl));
        SSL_free(reinterpret_cast<SSL*>(m_ssl));
        m_ssl = nullptr;
    }
    if (m_ctx) {
        SSL_CTX_free(reinterpret_cast<SSL_CTX*>(m_ctx));
        m_ctx = nullptr;
    }
    if (m_fd >= 0) { ::close(m_fd); m_fd = -1; }
}

bool MqttTestClient::tcp_tls_connect(const std::string& host, uint16_t port,
                                     std::chrono::milliseconds /*timeout*/) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        m_last_error = "inet_pton failed for host=" + host;
        return false;
    }
    m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_fd < 0) {
        m_last_error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        m_last_error = std::string("connect: ") + std::strerror(errno);
        close();
        return false;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        m_last_error = "SSL_CTX_new failed";
        close();
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    SSL* ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        m_last_error = "SSL_new failed";
        close();
        return false;
    }
    SSL_set_fd(ssl, m_fd);
    int rc = SSL_connect(ssl);
    if (rc != 1) {
        unsigned long e = ERR_peek_last_error();
        char buf[256] = {0};
        if (e) ERR_error_string_n(e, buf, sizeof(buf));
        m_last_error = std::string("SSL_connect: ") + buf;
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close();
        return false;
    }
    m_ctx = ctx;
    m_ssl = ssl;
    return true;
}

int MqttTestClient::connect_mqtt(const std::string& client_id,
                                 const std::string& username,
                                 const std::string& password,
                                 bool clean_session,
                                 uint16_t keep_alive) {
    using namespace server::mqtt;

    // Build CONNECT (header + payload).
    std::vector<uint8_t> body;
    append_mqtt_string(body, "MQTT");
    body.push_back(0x04); // level
    uint8_t flags = 0;
    if (!username.empty()) flags |= 0x80;
    if (!password.empty()) flags |= 0x40;
    if (clean_session)     flags |= 0x02;
    body.push_back(flags);
    append_uint16_be(body, keep_alive);
    append_mqtt_string(body, client_id);
    if (!username.empty()) append_mqtt_string(body, username);
    if (!password.empty()) append_mqtt_string(body, password);

    std::vector<uint8_t> full;
    full.push_back(0x10);
    uint8_t rl[4];
    size_t n = encode_varint(static_cast<uint32_t>(body.size()), rl);
    full.insert(full.end(), rl, rl + n);
    full.insert(full.end(), body.begin(), body.end());

    SSL* ssl = reinterpret_cast<SSL*>(m_ssl);
    if (!ssl_write_all(ssl, full)) {
        m_last_error = "SSL_write CONNECT failed";
        return -1;
    }

    // Read exactly 4 bytes -- CONNACK is always [0x20][0x02][sp][rc].
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    uint8_t hdr[4] = {0, 0, 0, 0};
    size_t  got    = 0;
    while (got < 4) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            m_last_error = "CONNACK timeout";
            return -1;
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_fd, &rfds);
        timeval tv{};
        tv.tv_sec  = static_cast<long>(remaining.count() / 1000);
        tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
        if (::select(m_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
        int r = SSL_read(ssl, hdr + got, static_cast<int>(4 - got));
        if (r <= 0) {
            int e = SSL_get_error(ssl, r);
            if (e == SSL_ERROR_WANT_READ) continue;
            m_last_error = "SSL_read CONNACK failed";
            return -1;
        }
        got += static_cast<size_t>(r);
    }
    if (hdr[0] != 0x20 || hdr[1] != 0x02) {
        m_last_error = "CONNACK header malformed";
        return -1;
    }
    return static_cast<int>(hdr[3]);
}

bool MqttTestClient::send_publish(const std::string& topic,
                                  const std::vector<uint8_t>& payload,
                                  uint8_t qos, uint16_t packet_id) {
    using namespace server::mqtt;
    auto pkt = encode_publish(topic, payload, qos, false, packet_id);
    if (!ssl_write_all(reinterpret_cast<SSL*>(m_ssl), pkt)) {
        m_last_error = "SSL_write PUBLISH failed";
        return false;
    }
    return true;
}

bool MqttTestClient::subscribe_one(const std::string& topic, uint16_t pid,
                                   uint8_t qos) {
    using namespace server::mqtt;
    std::vector<uint8_t> body;
    append_uint16_be(body, pid);
    append_mqtt_string(body, topic);
    body.push_back(qos);
    std::vector<uint8_t> full;
    full.push_back(0x82);
    uint8_t rl[4];
    size_t n = encode_varint(static_cast<uint32_t>(body.size()), rl);
    full.insert(full.end(), rl, rl + n);
    full.insert(full.end(), body.begin(), body.end());
    if (!ssl_write_all(reinterpret_cast<SSL*>(m_ssl), full)) {
        m_last_error = "SSL_write SUBSCRIBE failed";
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (true) {
        auto pk = try_decode();
        if (pk && pk->error == DecodeError::Ok) {
            if (pk->type == PacketType::Suback) return true;
            continue; // skip stray PUBLISH/PINGRESP
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            m_last_error = "SUBACK timeout";
            return false;
        }
        if (!pump_in(deadline)) {
            m_last_error = "pump_in failed waiting for SUBACK";
            return false;
        }
    }
}

std::optional<ReceivedPublish>
MqttTestClient::recv_publish(std::chrono::milliseconds timeout) {
    using namespace server::mqtt;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto pk = try_decode();
        if (pk && pk->error == DecodeError::Ok) {
            if (pk->type == PacketType::Publish) {
                ReceivedPublish r;
                r.topic   = std::move(pk->publish.topic);
                r.payload = std::move(pk->publish.payload);
                r.qos     = pk->publish.qos;
                return r;
            }
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        if (!pump_in(deadline)) return std::nullopt;
    }
}

bool MqttTestClient::wait_for_puback(uint16_t pid,
                                     std::chrono::milliseconds timeout) {
    using namespace server::mqtt;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto pk = try_decode();
        if (pk && pk->error == DecodeError::Ok) {
            if (pk->type == PacketType::Puback && pk->puback.packet_id == pid)
                return true;
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        if (!pump_in(deadline)) return false;
    }
}

bool MqttTestClient::pump_in(std::chrono::steady_clock::time_point deadline) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(m_fd, &rfds);
    timeval tv{};
    tv.tv_sec  = static_cast<long>(remaining.count() / 1000);
    tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
    int rc = ::select(m_fd + 1, &rfds, nullptr, nullptr, &tv);
    if (rc <= 0) return false;

    uint8_t buf[4096];
    int n = SSL_read(reinterpret_cast<SSL*>(m_ssl), buf, sizeof(buf));
    if (n <= 0) {
        int e = SSL_get_error(reinterpret_cast<SSL*>(m_ssl), n);
        if (e == SSL_ERROR_WANT_READ) return true;
        return false;
    }
    m_recv.insert(m_recv.end(), buf, buf + n);
    return true;
}

std::optional<server::mqtt::MqttPacket> MqttTestClient::try_decode() {
    auto pk = server::mqtt::decode_packet(m_recv.data(), m_recv.size());
    if (!pk) return std::nullopt;
    if (pk->error == server::mqtt::DecodeError::Ok && pk->bytes_consumed > 0) {
        m_recv.erase(m_recv.begin(), m_recv.begin() + pk->bytes_consumed);
    }
    return pk;
}

} // namespace test
} // namespace bridge
} // namespace Slic3r
