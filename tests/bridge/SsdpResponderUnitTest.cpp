// Bambu Bridge — SsdpResponder unit test (phase 3).
//
// Pins the exact byte format of an M-SEARCH 200 OK reply and a NOTIFY
// alive/byebye for a fixed SsdpVirtualDevice. The fixture device matches
// the example in docs/bambu_bridge_plan.md phase 3 so future protocol
// edits show up as visible diffs here.
//
// No sockets are opened.

#include <cassert>
#include <cstdio>
#include <string>

#include "BridgeService.hpp"
#include "server/SsdpResponder.hpp"

using Slic3r::bridge::server::SsdpResponder;
using Slic3r::bridge::server::SsdpResponderConfig;
using Slic3r::bridge::server::SsdpVirtualDevice;

namespace {

#define EXPECT(cond, msg)                                                      \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);\
            return 1;                                                           \
        }                                                                       \
    } while (0)

SsdpVirtualDevice fixture_device() {
    SsdpVirtualDevice d;
    d.dev_id   = "TESTSER0001";
    d.name     = "Test";
    d.model    = "H2S";
    d.firmware = "01.02.00.00";
    d.lan_ip   = "192.168.1.42";
    d.http_port = 80;
    d.bound    = true;
    d.secure   = true;
    return d;
}

const char* kExpectedResponse =
    "HTTP/1.1 200 OK\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "EXT:\r\n"
    "LOCATION: http://192.168.1.42:80/upnp/desc.xml\r\n"
    "SERVER: Bambu Lab/H2S/01.02.00.00\r\n"
    "ST: urn:bambulab-com:device:3dprinter:1\r\n"
    "USN: TESTSER0001\r\n"
    "DevName.bambu.com: Test\r\n"
    "DevModel.bambu.com: H2S\r\n"
    "DevVersion.bambu.com: 01.02.00.00\r\n"
    "DevConnect.bambu.com: lan\r\n"
    "DevBind.bambu.com: occupied\r\n"
    "Devseclink.bambu.com: secure\r\n"
    "DevSecure.bambu.com: 1\r\n"
    "DevSignal.bambu.com: -50dBm\r\n"
    "DevCap.bambu.com: 1\r\n"
    "\r\n";

const char* kExpectedNotifyAlive =
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "NT: urn:bambulab-com:device:3dprinter:1\r\n"
    "NTS: ssdp:alive\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "EXT:\r\n"
    "LOCATION: http://192.168.1.42:80/upnp/desc.xml\r\n"
    "SERVER: Bambu Lab/H2S/01.02.00.00\r\n"
    "ST: urn:bambulab-com:device:3dprinter:1\r\n"
    "USN: TESTSER0001\r\n"
    "DevName.bambu.com: Test\r\n"
    "DevModel.bambu.com: H2S\r\n"
    "DevVersion.bambu.com: 01.02.00.00\r\n"
    "DevConnect.bambu.com: lan\r\n"
    "DevBind.bambu.com: occupied\r\n"
    "Devseclink.bambu.com: secure\r\n"
    "DevSecure.bambu.com: 1\r\n"
    "DevSignal.bambu.com: -50dBm\r\n"
    "DevCap.bambu.com: 1\r\n"
    "\r\n";

const char* kExpectedNotifyByebye =
    "NOTIFY * HTTP/1.1\r\n"
    "HOST: 239.255.255.250:1900\r\n"
    "NT: urn:bambulab-com:device:3dprinter:1\r\n"
    "NTS: ssdp:byebye\r\n"
    "CACHE-CONTROL: max-age=1800\r\n"
    "EXT:\r\n"
    "LOCATION: http://192.168.1.42:80/upnp/desc.xml\r\n"
    "SERVER: Bambu Lab/H2S/01.02.00.00\r\n"
    "ST: urn:bambulab-com:device:3dprinter:1\r\n"
    "USN: TESTSER0001\r\n"
    "DevName.bambu.com: Test\r\n"
    "DevModel.bambu.com: H2S\r\n"
    "DevVersion.bambu.com: 01.02.00.00\r\n"
    "DevConnect.bambu.com: lan\r\n"
    "DevBind.bambu.com: occupied\r\n"
    "Devseclink.bambu.com: secure\r\n"
    "DevSecure.bambu.com: 1\r\n"
    "DevSignal.bambu.com: -50dBm\r\n"
    "DevCap.bambu.com: 1\r\n"
    "\r\n";

void dump_diff(const char* label, const std::string& got, const std::string& want) {
    std::fprintf(stderr, "----- %s -----\n", label);
    std::fprintf(stderr, "GOT (%zu bytes):\n%s\n", got.size(), got.c_str());
    std::fprintf(stderr, "WANT (%zu bytes):\n%s\n", want.size(), want.c_str());
}

} // namespace

int main() {
    const auto dev = fixture_device();

    // (1) M-SEARCH 200 OK response — exact byte match.
    {
        const std::string got  = SsdpResponder::format_search_response(dev);
        const std::string want = kExpectedResponse;
        if (got != want) {
            dump_diff("search_response", got, want);
            return 1;
        }
    }

    // (2) NOTIFY ssdp:alive.
    {
        const std::string got  = SsdpResponder::format_notify(dev, /*alive=*/true);
        const std::string want = kExpectedNotifyAlive;
        if (got != want) {
            dump_diff("notify alive", got, want);
            return 1;
        }
    }

    // (3) NOTIFY ssdp:byebye.
    {
        const std::string got  = SsdpResponder::format_notify(dev, /*alive=*/false);
        const std::string want = kExpectedNotifyByebye;
        if (got != want) {
            dump_diff("notify byebye", got, want);
            return 1;
        }
    }

    // (4) bound=false, secure=false flips the DevBind / Devseclink / DevSecure
    //     headers without touching anything else.
    {
        SsdpVirtualDevice unbound = dev;
        unbound.bound  = false;
        unbound.secure = false;
        const std::string s = SsdpResponder::format_search_response(unbound);
        EXPECT(s.find("DevBind.bambu.com: free")     != std::string::npos,
               "expected DevBind: free for unbound device");
        EXPECT(s.find("Devseclink.bambu.com: free")  != std::string::npos,
               "expected Devseclink: free for insecure device");
        EXPECT(s.find("DevSecure.bambu.com: 0")      != std::string::npos,
               "expected DevSecure: 0 for insecure device");
        EXPECT(s.find("DevBind.bambu.com: occupied") == std::string::npos,
               "leaked occupied for unbound device");
    }

    // (5) BridgeService round-trip: install responder, retrieve, detach.
    {
        Slic3r::bridge::BridgeService svc;
        EXPECT(svc.ssdp_responder() == nullptr,
               "fresh BridgeService.ssdp_responder() not null");

        SsdpResponderConfig cfg;
        cfg.notify_interval          = std::chrono::seconds(60);
        cfg.enable_multicast_send    = false;
        cfg.enable_bambu_broadcast   = false;
        cfg.bind_address             = "127.0.0.1";
        svc.set_ssdp_responder(std::make_unique<SsdpResponder>(cfg));
        EXPECT(svc.ssdp_responder() != nullptr,
               "set_ssdp_responder did not stick");
        svc.ssdp_responder()->add_device(dev);
        EXPECT(svc.ssdp_responder()->devices().size() == 1,
               "device add did not register");
        svc.ssdp_responder()->remove_device(dev.dev_id);
        EXPECT(svc.ssdp_responder()->devices().empty(),
               "device remove did not unregister");

        svc.set_ssdp_responder(nullptr);
        EXPECT(svc.ssdp_responder() == nullptr,
               "set_ssdp_responder(nullptr) did not detach");
    }

    std::printf("SsdpResponderUnitTest: ok\n");
    return 0;
}
