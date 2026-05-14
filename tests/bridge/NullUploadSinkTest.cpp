// Bambu Bridge — NullUploadSink unit test (phase 7).
//
// Pins the documented "drop with explanation" contract — every deliver()
// returns ok=false with a non-empty error_message. The body of the message
// is intentionally not asserted character-for-character (it's a human-
// readable hint and may evolve); we only check it's present and non-empty.

#include "router/NullUploadSink.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using Slic3r::bridge::router::NullUploadSink;
using Slic3r::bridge::server::UploadJob;
using Slic3r::bridge::server::UploadResult;

namespace {
int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}
}

int main() {
    NullUploadSink sink;

    UploadJob job;
    job.dev_id      = "0938BC582502312";
    job.filename    = "demo.3mf";
    job.remote_path = "/model/demo.3mf";
    job.content     = std::vector<uint8_t>(64, 0xAB);
    job.received_at = std::chrono::system_clock::now();

    UploadResult r = sink.deliver(std::move(job));
    check(!r.ok, "NullUploadSink::deliver returns ok=false");
    check(!r.error_message.empty(),
          "NullUploadSink::deliver returns a non-empty error_message");
    check(r.remote_url.empty(),
          "NullUploadSink::deliver leaves remote_url empty");

    // Two more calls — sink must remain a pure drop (no hidden state).
    UploadResult r2 = sink.deliver(UploadJob{});
    check(!r2.ok && !r2.error_message.empty(),
          "second call still drops with explanation");
    UploadResult r3 = sink.deliver(UploadJob{});
    check(!r3.ok && !r3.error_message.empty(),
          "third call still drops with explanation");

    if (g_fails) {
        std::fprintf(stderr, "NullUploadSinkTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("NullUploadSinkTest: ok\n");
    return 0;
}
