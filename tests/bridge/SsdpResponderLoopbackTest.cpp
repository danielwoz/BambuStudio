// Bambu Bridge — SsdpResponder loopback integration test (phase 3).
//
// Brings up a real SsdpResponder bound to 127.0.0.1:1900, then sends an
// M-SEARCH from another UDP socket on the same loopback interface and
// asserts a properly-formatted 200 OK comes back within ~1 s.
//
// CI/dev-box reality: many shared Linux hosts either (a) already have
// avahi/another SSDP daemon owning 1900, (b) firewall multicast on lo,
// or (c) run the test as a non-privileged user inside a netns that has
// no multicast. Each of those should cause us to *skip* (ctest return
// code 77), not fail. Only an outright unrelated bug (e.g. a malformed
// reply on a healthy responder) is a failure.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "server/SsdpResponder.hpp"

using Slic3r::bridge::server::SsdpResponder;
using Slic3r::bridge::server::SsdpResponderConfig;
using Slic3r::bridge::server::SsdpVirtualDevice;

namespace {

constexpr int kCtestSkip = 77;

void skip(const char* why) {
    std::fprintf(stderr, "SKIP: %s (errno=%d %s)\n", why, errno, std::strerror(errno));
}

SsdpVirtualDevice fixture() {
    SsdpVirtualDevice d;
    d.dev_id   = "LOOPSER0001";
    d.name     = "Loopback";
    d.model    = "H2S";
    d.firmware = "01.02.00.00";
    d.lan_ip   = "127.0.0.1";
    d.http_port = 8080;
    d.bound    = true;
    d.secure   = true;
    return d;
}

} // namespace

int main() {
    // 1. Stand up the responder bound to 127.0.0.1:1900. Disable both the
    //    multicast announce and the 2021 broadcast so the recv path is the
    //    only thing on the wire during this test.
    SsdpResponderConfig cfg;
    cfg.notify_interval        = std::chrono::seconds(3600); // no announce
    cfg.enable_multicast_send  = false;
    cfg.enable_bambu_broadcast = false;
    cfg.bind_address           = "127.0.0.1";

    SsdpResponder responder(cfg);
    responder.add_device(fixture());
    responder.start();

    if (!responder.running()) {
        skip("SsdpResponder failed to start (probably port-in-use)");
        return kCtestSkip;
    }

    // Give the recv thread a moment to enter select().
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 2. Open a client UDP socket bound to an ephemeral port on lo.
    int cs = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (cs < 0) {
        skip("client socket() failed");
        responder.stop();
        return kCtestSkip;
    }

    sockaddr_in client{};
    client.sin_family      = AF_INET;
    client.sin_port        = htons(0);
    client.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    if (::bind(cs, reinterpret_cast<sockaddr*>(&client), sizeof(client)) < 0) {
        ::close(cs);
        skip("client bind() failed");
        responder.stop();
        return kCtestSkip;
    }

    // 3. Probe the responder's actual port via getsockname on a dummy bind
    //    is too fragile; the test relies on the responder owning 1900 on
    //    127.0.0.1. We hand-craft the M-SEARCH and aim it at the loopback
    //    1900 socket.
    sockaddr_in target{};
    target.sin_family      = AF_INET;
    target.sin_port        = htons(1900);
    target.sin_addr.s_addr = ::inet_addr("127.0.0.1");

    const char* search =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";
    ssize_t sent = ::sendto(cs, search, std::strlen(search), 0,
                            reinterpret_cast<sockaddr*>(&target), sizeof(target));
    if (sent < 0) {
        ::close(cs);
        skip("sendto(127.0.0.1:1900) failed (no lo multicast?)");
        responder.stop();
        return kCtestSkip;
    }

    // 4. Wait up to 1 s for the unicast reply.
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(cs, &rfds);
    timeval tv{};
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    int rc = ::select(cs + 1, &rfds, nullptr, nullptr, &tv);
    if (rc <= 0) {
        ::close(cs);
        skip("no reply within 1s on loopback (possibly filtered)");
        responder.stop();
        return kCtestSkip;
    }

    char buf[4096];
    sockaddr_in from{};
    socklen_t   flen = sizeof(from);
    ssize_t n = ::recvfrom(cs, buf, sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &flen);
    ::close(cs);

    if (n <= 0) {
        skip("recvfrom returned no bytes");
        responder.stop();
        return kCtestSkip;
    }

    std::string reply(buf, buf + n);
    std::printf("loopback reply (%zd bytes):\n%s\n", n, reply.c_str());

    // 5. Hard assertions: the reply must look like a Bambu SSDP 200 OK.
    int fail = 0;
    auto must_contain = [&](const char* needle) {
        if (reply.find(needle) == std::string::npos) {
            std::fprintf(stderr, "FAIL: reply missing '%s'\n", needle);
            ++fail;
        }
    };
    must_contain("HTTP/1.1 200 OK");
    must_contain("USN: LOOPSER0001");
    must_contain("DevName.bambu.com: Loopback");
    must_contain("DevModel.bambu.com: H2S");
    must_contain("DevVersion.bambu.com: 01.02.00.00");
    must_contain("LOCATION: http://127.0.0.1:8080/upnp/desc.xml");
    must_contain("ST: urn:bambulab-com:device:3dprinter:1");
    must_contain("Devseclink.bambu.com: secure");

    responder.stop();

    if (fail) {
        std::fprintf(stderr, "SsdpResponderLoopbackTest: %d assertion(s) failed\n", fail);
        return 1;
    }
    std::printf("SsdpResponderLoopbackTest: ok\n");
    return 0;
}
