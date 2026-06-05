// bridge_test_cli — minimal, self-contained MQTT-over-TLS probe for the
// Bambu Bridge. Deliberately depends on NOTHING but OpenSSL + Winsock so it
// links without dragging in libslic3r / bambu_virtual_client.
//
// It speaks just enough MQTT 3.1.1 to act like a slicer's VirtualMqttClient:
//   TLS connect (SSL_VERIFY_NONE) -> CONNECT(user=bblp,pass=access) ->
//   SUBSCRIBE device/<sn>/report -> PUBLISH device/<sn>/request <json> ->
//   print every inbound PUBLISH for N seconds.
//
// Usage:
//   bridge_test_cli <host> <port> <sn> <access> [request_json] [--idle N]
// Examples:
//   bridge_test_cli 127.0.0.1 8884 FFFFBC582502312 18b1a572
//       -> connects, subscribes, sends pushall, prints the report
//   bridge_test_cli 127.0.0.1 8884 FFFFBC582502312 18b1a572 \
//       "{\"print\":{\"command\":\"ams_filament_setting\",\"ams_id\":0,\"tray_id\":0,\"tray_color\":\"FF0000FF\",\"sequence_id\":\"9001\"}}"
//       -> sends a filament-colour change

#include "../bambu_bridge/platform/WinsockShim.hpp"  // sockets first

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void put_str(std::vector<uint8_t>& b, const std::string& s) {
    b.push_back(static_cast<uint8_t>((s.size() >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>(s.size() & 0xFF));
    b.insert(b.end(), s.begin(), s.end());
}

// MQTT remaining-length varint.
void put_remlen(std::vector<uint8_t>& b, size_t n) {
    do {
        uint8_t enc = n & 0x7F;
        n >>= 7;
        if (n) enc |= 0x80;
        b.push_back(enc);
    } while (n);
}

std::vector<uint8_t> mqtt_connect(const std::string& client_id,
                                  const std::string& user,
                                  const std::string& pass) {
    std::vector<uint8_t> vh;
    put_str(vh, "MQTT");
    vh.push_back(0x04);                 // protocol level 4 = 3.1.1
    vh.push_back(0xC2);                 // user + pass + clean session
    vh.push_back(0x00); vh.push_back(0x3C);  // keepalive 60s
    put_str(vh, client_id);
    put_str(vh, user);
    put_str(vh, pass);
    std::vector<uint8_t> pkt;
    pkt.push_back(0x10);
    put_remlen(pkt, vh.size());
    pkt.insert(pkt.end(), vh.begin(), vh.end());
    return pkt;
}

std::vector<uint8_t> mqtt_subscribe(uint16_t pid, const std::string& topic) {
    std::vector<uint8_t> body;
    body.push_back(pid >> 8); body.push_back(pid & 0xFF);
    put_str(body, topic);
    body.push_back(0x00);              // qos 0
    std::vector<uint8_t> pkt;
    pkt.push_back(0x82);
    put_remlen(pkt, body.size());
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

std::vector<uint8_t> mqtt_publish(const std::string& topic, const std::string& payload) {
    std::vector<uint8_t> body;
    put_str(body, topic);             // qos 0 = no packet id
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<uint8_t> pkt;
    pkt.push_back(0x30);              // PUBLISH qos 0
    put_remlen(pkt, body.size());
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

bool ssl_write_all(SSL* ssl, const std::vector<uint8_t>& b) {
    size_t off = 0;
    while (off < b.size()) {
        int n = SSL_write(ssl, b.data() + off, static_cast<int>(b.size() - off));
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

int tcp_connect(const std::string& host, uint16_t port) {
    addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return -1;
    int fd = static_cast<int>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (fd < 0) { ::freeaddrinfo(res); return -1; }
    if (::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        bambu_close_socket(fd); ::freeaddrinfo(res); return -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: %s <host> <port> <sn> <access> [request_json] [--idle N]\n"
            "  default request = pushall; prints inbound PUBLISH for N(=8)s.\n",
            argv[0]);
        return 2;
    }
    ensure_winsock_init();

    const std::string host = argv[1];
    const uint16_t    port = static_cast<uint16_t>(std::atoi(argv[2]));
    const std::string sn   = argv[3];
    const std::string acc  = argv[4];
    std::string req_json;
    int idle_s = 8;
    for (int i = 5; i < argc; ++i) {
        if (std::strcmp(argv[i], "--idle") == 0 && i + 1 < argc) { idle_s = std::atoi(argv[++i]); }
        else req_json = argv[i];
    }
    if (req_json.empty())
        req_json = "{\"pushing\":{\"command\":\"pushall\",\"sequence_id\":\"1\"}}";

    std::printf("[cli] connecting %s:%u sn=%s\n", host.c_str(), port, sn.c_str());
    int fd = tcp_connect(host, port);
    if (fd < 0) { std::fprintf(stderr, "[cli] TCP connect FAILED to %s:%u\n", host.c_str(), port); return 1; }
    std::printf("[cli] TCP ok\n");

    SSL_library_init();
    SSL_load_error_strings();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1) {
        std::fprintf(stderr, "[cli] TLS handshake FAILED: ");
        ERR_print_errors_fp(stderr);
        return 1;
    }
    std::printf("[cli] TLS ok (%s)\n", SSL_get_cipher(ssl));

    if (!ssl_write_all(ssl, mqtt_connect("test-" + sn, "bblp", acc))) {
        std::fprintf(stderr, "[cli] CONNECT write failed\n"); return 1;
    }
    // Read CONNACK (4 bytes: 0x20 0x02 0x00 rc).
    uint8_t ack[4] = {0};
    int an = SSL_read(ssl, ack, 4);
    if (an >= 4 && ack[0] == 0x20) {
        std::printf("[cli] CONNACK rc=%d %s\n", ack[3], ack[3] == 0 ? "(accepted)" : "(REJECTED)");
        if (ack[3] != 0) return 1;
    } else {
        std::fprintf(stderr, "[cli] no/!bad CONNACK (read %d, b0=0x%02x)\n", an, an > 0 ? ack[0] : 0);
        return 1;
    }

    ssl_write_all(ssl, mqtt_subscribe(1, "device/" + sn + "/report"));
    std::printf("[cli] subscribed device/%s/report\n", sn.c_str());

    ssl_write_all(ssl, mqtt_publish("device/" + sn + "/request", req_json));
    std::printf("[cli] published device/%s/request len=%zu: %.120s\n",
                sn.c_str(), req_json.size(), req_json.c_str());

    // Read inbound for idle_s seconds, decode PUBLISH frames.
    // If the request was a filament/AMS setting change, watch the printer's
    // gcode_state so we can warn when a running print blocks it.
    const bool is_filament_cmd =
        req_json.find("ams_filament_setting") != std::string::npos;
    std::string gcode_state;   // latest print state seen in reports
    bambu_set_recv_timeout_ms(fd, 1000);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(idle_s);
    std::vector<uint8_t> buf;
    uint8_t chunk[8192];
    int pub_count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        int n = SSL_read(ssl, chunk, sizeof(chunk));
        if (n <= 0) {
            int e = SSL_get_error(ssl, n);
            if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) continue;
            if (e == SSL_ERROR_ZERO_RETURN) { std::printf("[cli] server closed\n"); break; }
            continue;  // timeout
        }
        buf.insert(buf.end(), chunk, chunk + n);
        // Parse complete MQTT packets out of buf.
        size_t pos = 0;
        while (pos + 2 <= buf.size()) {
            uint8_t type = buf[pos] & 0xF0;
            size_t rl = 0, mult = 1, i = pos + 1; bool ok = false;
            for (int k = 0; k < 4 && i < buf.size(); ++k, ++i) {
                rl += (buf[i] & 0x7F) * mult; mult <<= 7;
                if (!(buf[i] & 0x80)) { ok = true; ++i; break; }
            }
            if (!ok || i + rl > buf.size()) break;       // incomplete
            const uint8_t* body = buf.data() + i;
            if (type == 0x30) {                          // PUBLISH
                uint16_t tl = (body[0] << 8) | body[1];
                std::string topic(reinterpret_cast<const char*>(body + 2), tl);
                std::string payload(reinterpret_cast<const char*>(body + 2 + tl), rl - 2 - tl);
                ++pub_count;
                // Print the FULL payload (not truncated) so state-verifying
                // greps (tray_color, dry_time, dry_setting) can see the
                // whole `print.ams` section even when it lands deep in a
                // 23 KB pushall report.
                std::printf("[cli] <- PUBLISH %s len=%zu\n",
                            topic.c_str(), payload.size());
                std::fwrite(payload.data(), 1, payload.size(), stdout);
                std::fputc('\n', stdout);
                // Track the printer's print state. A running/paused/preparing
                // print makes the firmware REJECT ams_filament_setting.
                {
                    // Handle both compact ("gcode_state":"X") and pretty-printed
                    // ("gcode_state": "X") report formats.
                    static const char key[] = "\"gcode_state\"";
                    size_t p = payload.find(key);
                    if (p != std::string::npos) {
                        p += sizeof(key) - 1;
                        while (p < payload.size() &&
                               (payload[p] == ':' || payload[p] == ' ' ||
                                payload[p] == '\t' || payload[p] == '\n' || payload[p] == '\r'))
                            ++p;
                        if (p < payload.size() && payload[p] == '"') {
                            ++p;
                            size_t e = payload.find('"', p);
                            if (e != std::string::npos) gcode_state = payload.substr(p, e - p);
                        }
                    }
                }
            }
            pos = i + rl;
        }
        buf.erase(buf.begin(), buf.begin() + pos);
    }
    std::printf("[cli] done. inbound PUBLISH frames=%d\n", pub_count);
    if (is_filament_cmd) {
        const bool printing =
            gcode_state == "RUNNING" || gcode_state == "PAUSE" ||
            gcode_state == "PREPARE" || gcode_state == "SLICING";
        if (printing) {
            std::printf(
                "[cli] *** WARNING: printer gcode_state=%s — a print is in "
                "progress. The firmware REJECTS ams_filament_setting / filament "
                "colour changes while printing. The command was relayed and "
                "signed, but it will NOT take effect until the print finishes "
                "or is stopped. ***\n",
                gcode_state.c_str());
        } else if (!gcode_state.empty()) {
            std::printf("[cli] printer gcode_state=%s (not printing) — a filament "
                        "change should apply.\n", gcode_state.c_str());
        } else {
            std::printf("[cli] note: could not read gcode_state from the report; "
                        "if the change doesn't apply, the printer may be busy.\n");
        }
    }
    SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); bambu_close_socket(fd);
    return pub_count > 0 ? 0 : 3;
}
