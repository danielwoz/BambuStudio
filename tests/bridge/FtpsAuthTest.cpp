// Bambu Bridge — FtpsServer auth-failure test (phase 7).
//
// Pins the 530 path: USER bblp + a deliberately wrong PASS must produce
// "530 Authentication failed." and a clean control-channel teardown.
// Same dual-endpoint strategy as FtpsServerLoopbackTest: try 127.0.0.2
// first, fall back to 127.0.0.1:0, skip if neither binds.

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include "server/FtpsServer.hpp"
#include "server/IUploadSink.hpp"
#include "router/NullUploadSink.hpp"
#include "tls/CertFactory.hpp"
#include "support/FtpsTestClient.hpp"

using Slic3r::bridge::server::FtpsServer;
using Slic3r::bridge::server::FtpsServerConfig;
using Slic3r::bridge::server::FtpsVirtualDevice;
using Slic3r::bridge::router::NullUploadSink;
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

std::unique_ptr<FtpsServer> try_start(const std::string& bind_ip,
                                      uint16_t port,
                                      const FtpsVirtualDevice& dev,
                                      uint16_t& bound_port_out) {
    FtpsServerConfig cfg;
    cfg.sink                = std::make_shared<NullUploadSink>();
    cfg.io_timeout_seconds  = 30;
    cfg.pasv_advertise_ip   = bind_ip;
    auto srv = std::make_unique<FtpsServer>(cfg);
    FtpsVirtualDevice d = dev;
    d.lan_ip = bind_ip;
    d.port   = port;
    try {
        srv->add_device(d);
        srv->start();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[auth] start at %s:%u failed: %s\n",
                     bind_ip.c_str(), port, ex.what());
        return nullptr;
    }
    bound_port_out = srv->bound_port(d.dev_id);
    if (bound_port_out == 0) { srv->stop(); return nullptr; }
    return srv;
}

} // namespace

int main() {
    const std::string dev_id      = "0938BC582502312";
    const std::string access_code = "ABCD1234";

    // Mint a cert.
    auto cache_dir = std::filesystem::temp_directory_path() /
                     ("bambu-bridge-ftps-auth-" + std::to_string(::getpid()));
    std::filesystem::create_directories(cache_dir);
    CertFactoryConfig cfg;
    cfg.cache_dir = cache_dir;
    CertFactory factory(cfg);
    auto cert = factory.get_or_create(dev_id);

    FtpsVirtualDevice dev;
    dev.dev_id      = dev_id;
    dev.access_code = access_code;
    dev.cert        = cert;

    std::string bind_ip = "127.0.0.2";
    uint16_t    bound   = 0;
    auto srv = try_start(bind_ip, 9991, dev, bound);
    if (!srv) {
        bind_ip = "127.0.0.1";
        srv = try_start(bind_ip, 0, dev, bound);
    }
    if (!srv) {
        std::fprintf(stderr, "SKIP: couldn't bind FtpsServer on loopback\n");
        return kCtestSkip;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ----- Wrong PASS -> 530.
    {
        FtpsTestClient c;
        if (!c.connect(bind_ip, bound)) {
            std::fprintf(stderr, "SKIP: TLS connect failed: %s\n",
                         c.last_error().c_str());
            srv->stop();
            return kCtestSkip;
        }
        int rc = c.login("bblp", "WRONG-PASSWORD");
        check(rc == 530, "wrong PASS -> 530 Authentication failed");
        c.close();
    }

    // ----- Wrong USER -> still 530 on PASS.
    {
        FtpsTestClient c;
        if (!c.connect(bind_ip, bound)) {
            std::fprintf(stderr, "SKIP: TLS connect (2) failed: %s\n",
                         c.last_error().c_str());
            srv->stop();
            return kCtestSkip;
        }
        int rc = c.login("admin", access_code);
        check(rc == 530, "wrong USER -> 530 on PASS");
        c.close();
    }

    // ----- Commands before USER -> 530 (must login first).
    {
        FtpsTestClient c;
        if (!c.connect(bind_ip, bound)) {
            std::fprintf(stderr, "SKIP: TLS connect (3) failed: %s\n",
                         c.last_error().c_str());
            srv->stop();
            return kCtestSkip;
        }
        std::string body;
        int rc = c.raw_command("PWD", body);
        check(rc == 530, "PWD without login -> 530");
        c.close();
    }

    srv->stop();

    if (g_fails) {
        std::fprintf(stderr, "FtpsAuthTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("FtpsAuthTest: ok\n");
    return 0;
}
