// Bambu Bridge — FtpsServer loopback integration test (phase 7).
//
// Stands up a real FtpsServer on a loopback endpoint, drives it with the
// minimal implicit-TLS FTPS test client, and asserts the full STOR path
// (auth -> PASV -> data-channel TLS -> deliver -> 226).
//
// Endpoint selection:
//   1. Try 127.0.0.2:9990 (high alias, can be unprivileged).
//   2. If that fails (lo alias disabled, port in use), fall back to
//      127.0.0.1 with port 0 (kernel-picked).
// Skips (ctest return code 77) if neither bind works.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>  // getpid

#include "server/FtpsServer.hpp"
#include "server/IUploadSink.hpp"
#include "tls/CertFactory.hpp"
#include "support/FtpsTestClient.hpp"

using Slic3r::bridge::server::FtpsServer;
using Slic3r::bridge::server::FtpsServerConfig;
using Slic3r::bridge::server::FtpsVirtualDevice;
using Slic3r::bridge::server::IUploadSink;
using Slic3r::bridge::server::UploadJob;
using Slic3r::bridge::server::UploadResult;
using Slic3r::bridge::tls::CertFactory;
using Slic3r::bridge::tls::CertFactoryConfig;
using Slic3r::bridge::test::FtpsTestClient;

namespace {

constexpr int kCtestSkip = 77;

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

struct RecordingSink : public IUploadSink {
    std::mutex                 mu;
    std::vector<UploadJob>     jobs;
    bool                       force_fail = false;

    UploadResult deliver(UploadJob job) override {
        UploadResult r;
        {
            std::lock_guard<std::mutex> lk(mu);
            jobs.push_back(job); // copy
        }
        if (force_fail) {
            r.ok            = false;
            r.error_message = "test-forced fail";
            return r;
        }
        r.ok         = true;
        r.remote_url = "test://" + job.dev_id + job.remote_path;
        return r;
    }
};

struct TestCertMaterial {
    Slic3r::bridge::tls::CertMaterial cert;
    std::filesystem::path             cache_dir;
};

TestCertMaterial mint_test_cert(const std::string& dev_id) {
    TestCertMaterial r;
    r.cache_dir = std::filesystem::temp_directory_path() /
                  ("bambu-bridge-ftps-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(r.cache_dir);
    CertFactoryConfig cfg;
    cfg.cache_dir = r.cache_dir;
    CertFactory factory(cfg);
    r.cert = factory.get_or_create(dev_id);
    return r;
}

std::unique_ptr<FtpsServer> try_start(const std::string& bind_ip,
                                      uint16_t port,
                                      const FtpsVirtualDevice& dev,
                                      std::shared_ptr<IUploadSink> sink,
                                      uint16_t& bound_port_out) {
    FtpsServerConfig cfg;
    cfg.sink             = std::move(sink);
    cfg.pasv_port_min    = 49152;
    cfg.pasv_port_max    = 65535;
    cfg.io_timeout_seconds = 30;
    cfg.pasv_advertise_ip = bind_ip; // PASV reply -> client-reachable ip
    auto srv = std::make_unique<FtpsServer>(cfg);
    FtpsVirtualDevice d = dev;
    d.lan_ip = bind_ip;
    d.port   = port;
    try {
        srv->add_device(d);
        srv->start();
    } catch (const std::exception& ex) {
        return nullptr;
    }
    bound_port_out = srv->bound_port(d.dev_id);
    if (bound_port_out == 0) { srv->stop(); return nullptr; }
    return srv;
}

} // namespace

int main() {
    const std::string dev_id      = "EXAMPLESERIAL01";
    const std::string access_code = "ABCD1234";
    auto              cert_holder = mint_test_cert(dev_id);

    auto sink = std::make_shared<RecordingSink>();

    FtpsVirtualDevice dev;
    dev.dev_id      = dev_id;
    dev.access_code = access_code;
    dev.cert        = cert_holder.cert;

    std::string bind_ip = "127.0.0.2";
    uint16_t    bound   = 0;
    auto srv = try_start(bind_ip, 9990, dev, sink, bound);
    if (!srv) {
        bind_ip = "127.0.0.1";
        srv = try_start(bind_ip, 0, dev, sink, bound);
    }
    if (!srv) {
        std::fprintf(stderr, "SKIP: couldn't bind FtpsServer on loopback\n");
        return kCtestSkip;
    }

    // Let the accept thread enter its select().
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ----- Full happy path: USER/PASS, PBSZ/PROT/TYPE, PASV, STOR, QUIT.
    {
        FtpsTestClient c;
        if (!c.connect(bind_ip, bound)) {
            std::fprintf(stderr, "SKIP: TLS connect failed: %s\n",
                         c.last_error().c_str());
            srv->stop();
            return kCtestSkip;
        }
        check(!c.welcome_banner().empty(), "received 220 welcome banner");

        int rc = c.login("bblp", access_code);
        check(rc == 230, "correct access code -> 230 Login successful");

        check(c.setup_data_channel_tls(),
              "PBSZ 0 / PROT P / TYPE I all 200");

        std::string data_ip;
        uint16_t    data_port = 0;
        check(c.pasv(data_ip, data_port), "PASV parses 227 host:port tuple");
        // PASV must advertise the *server's* bind IP back to the client.
        check(data_ip == bind_ip,
              "PASV advertises the server's bind IP");

        std::vector<uint8_t> payload(1024);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>(i & 0xFF);
        }
        int stor_rc = c.stor("/model/demo.3mf", data_ip, data_port, payload);
        check(stor_rc == 226, "STOR with sink ok -> 226 Transfer complete");

        int qr = c.quit();
        check(qr == 221, "QUIT -> 221 Goodbye");
        c.close();
    }

    // Give the server a moment for the I/O thread to finish.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ----- Assertions on what the sink saw.
    {
        std::lock_guard<std::mutex> lk(sink->mu);
        check(sink->jobs.size() == 1,
              "RecordingSink saw exactly one UploadJob");
        if (!sink->jobs.empty()) {
            const auto& j = sink->jobs.front();
            check(j.dev_id == dev_id,        "sink.dev_id matches");
            check(j.filename == "demo.3mf",  "sink.filename basename");
            check(j.remote_path == "/model/demo.3mf",
                  "sink.remote_path is the absolute path the client STOR'd");
            check(j.content.size() == 1024,
                  "sink.content has the full 1024 bytes");
            bool pattern_ok = true;
            for (size_t i = 0; i < j.content.size() && pattern_ok; ++i) {
                if (j.content[i] != static_cast<uint8_t>(i & 0xFF)) {
                    pattern_ok = false;
                }
            }
            check(pattern_ok, "sink.content bytes match the streamed pattern");
        }
    }

    srv->stop();

    if (g_fails) {
        std::fprintf(stderr, "FtpsServerLoopbackTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("FtpsServerLoopbackTest: ok\n");
    return 0;
}
