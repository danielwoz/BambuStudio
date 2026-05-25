// Bambu Bridge — E2E slicer-shaped client (phase 12).

#include "E2EPrinterClient.hpp"

#include "MqttTestClient.hpp"
#include "FtpsTestClient.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {
namespace e2e {

E2EPrinterClient::E2EPrinterClient()
    : m_mqtt(std::make_unique<test::MqttTestClient>()) {}

E2EPrinterClient::~E2EPrinterClient() = default;

bool E2EPrinterClient::connect_mqtt(const std::string& host, uint16_t port,
                                    const std::string& dev_id,
                                    const std::string& access_code) {
    if (!m_mqtt->tcp_tls_connect(host, port)) {
        m_last_error = "tcp_tls_connect: " + m_mqtt->last_error();
        return false;
    }
    // BambuStudio uses ("bblp", access_code) for the LAN MQTT login.
    const int rc = m_mqtt->connect_mqtt(dev_id, "bblp", access_code);
    if (rc != 0) {
        m_last_error = "CONNECT rejected, rc=" + std::to_string(rc) +
            " (" + m_mqtt->last_error() + ")";
        return false;
    }
    return true;
}

bool E2EPrinterClient::subscribe_report(const std::string& dev_id) {
    const std::string topic = "device/" + dev_id + "/report";
    if (!m_mqtt->subscribe_one(topic, /*pid=*/1, /*qos=*/0)) {
        m_last_error = "subscribe " + topic + ": " + m_mqtt->last_error();
        return false;
    }
    return true;
}

std::string E2EPrinterClient::wait_for_status(
    const std::function<bool(const std::string&)>& predicate,
    std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) break;
        auto pub = m_mqtt->recv_publish(remaining);
        if (!pub) continue;
        std::string payload(pub->payload.begin(), pub->payload.end());
        if (predicate(payload)) return payload;
    }
    m_last_error = "wait_for_status timed out";
    return {};
}

bool E2EPrinterClient::upload_3mf(const std::string& host, uint16_t port,
                                  const std::string& /*dev_id*/,
                                  const std::string& access_code,
                                  const std::string& path_to_3mf,
                                  std::string&       uploaded_filename_out) {
    std::ifstream in(path_to_3mf, std::ios::binary);
    if (!in) {
        m_last_error = "could not open " + path_to_3mf;
        return false;
    }
    std::vector<uint8_t> payload(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    if (payload.empty()) {
        m_last_error = path_to_3mf + " is empty (stub fixture?)";
        return false;
    }

    test::FtpsTestClient ftps;
    if (!ftps.connect(host, port)) {
        m_last_error = "FTPS connect: " + ftps.last_error();
        return false;
    }
    if (ftps.login("bblp", access_code) != 230) {
        m_last_error = "FTPS login failed: " + ftps.last_error();
        return false;
    }
    if (!ftps.setup_data_channel_tls()) {
        m_last_error = "PBSZ/PROT/TYPE failed: " + ftps.last_error();
        return false;
    }
    std::string data_ip;
    uint16_t    data_port = 0;
    if (!ftps.pasv(data_ip, data_port)) {
        m_last_error = "PASV failed: " + ftps.last_error();
        return false;
    }
    const std::string base =
        std::filesystem::path(path_to_3mf).filename().string();
    const std::string remote = "/cache/" + base;
    // Some bridges/printers advertise their own LAN IP in PASV; the
    // slicer always connects back to the same host it used for control.
    const int rc = ftps.stor(remote, host, data_port, payload);
    if (rc != 226 && rc != 250 /* tolerate either close-code */) {
        m_last_error = "STOR returned " + std::to_string(rc) +
            " (" + ftps.last_error() + ")";
        ftps.quit();
        return false;
    }
    ftps.quit();
    uploaded_filename_out = base;
    return true;
}

bool E2EPrinterClient::publish_request(const std::string& dev_id,
                                       const std::string& payload) {
    const std::string topic = "device/" + dev_id + "/request";
    std::vector<uint8_t> bytes(payload.begin(), payload.end());
    if (!m_mqtt->send_publish(topic, bytes, /*qos=*/1, /*pid=*/2)) {
        m_last_error = "PUBLISH " + topic + ": " + m_mqtt->last_error();
        return false;
    }
    return true;
}

bool E2EPrinterClient::start_print(const std::string& dev_id,
                                   const std::string& filename,
                                   int                plate_idx) {
    // Minimal Bambu print.gcode_file command. The bridge forwards
    // verbatim to the printer.
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"print\":{\"command\":\"project_file\","
        "\"param\":\"Metadata/plate_%d.gcode\","
        "\"subtask_name\":\"bridge_e2e\","
        "\"url\":\"file:///cache/%s\","
        "\"sequence_id\":\"1\"}}",
        plate_idx, filename.c_str());
    return publish_request(dev_id, buf);
}

bool E2EPrinterClient::pause_print(const std::string& dev_id) {
    return publish_request(dev_id,
        "{\"print\":{\"command\":\"pause\",\"sequence_id\":\"2\"}}");
}

bool E2EPrinterClient::cancel_print(const std::string& dev_id) {
    return publish_request(dev_id,
        "{\"print\":{\"command\":\"stop\",\"sequence_id\":\"3\"}}");
}

bool E2EPrinterClient::disconnect() {
    // MqttTestClient lacks an explicit DISCONNECT-encode helper. Tear
    // the socket down — the bridge handles this as a clean drop.
    m_mqtt->close();
    return true;
}

} // namespace e2e
} // namespace bridge
} // namespace Slic3r
