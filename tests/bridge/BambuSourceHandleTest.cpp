// Bambu Bridge — BambuSourceHandle missing-library safety contract.
//
// Mirrors the phase-6 BambuNetworkingPluginHandleTest. The contract is:
//
//   * init() with a bogus library_path returns false and DOES NOT crash.
//   * library_ready() returns false.
//   * Every C-ABI wrapper is a safe no-op returning a documented sentinel:
//       - bambu_create        -> rc != 0, *out_tunnel set to nullptr.
//       - bambu_destroy(t)    -> no-op for any t (incl. nullptr).
//       - bambu_open(t)       -> -1.
//       - bambu_close(t)      -> no-op.
//       - bambu_start_stream  -> -1.
//       - bambu_get_stream_count -> 0.
//       - bambu_get_stream_info  -> -1.
//       - bambu_read_sample      -> -1.
//
// Real round-trips against the proprietary library would only be
// exercisable in an E2E suite with a real printer in the loop; not
// covered here.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "BambuSourceHandle.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what); }
    else     {            std::fprintf(stderr, "ok   %s\n", what); }
}

} // namespace

int main() {
    using namespace Slic3r::bridge;

    // Make sure a dev env hasn't redirected our probe list at a real lib.
#if defined(_WIN32)
    _putenv_s("BAMBU_SOURCE_PATH", "");
#else
    ::unsetenv("BAMBU_SOURCE_PATH");
#endif

    BambuSourceConfig cfg;
    cfg.library_path = "/nonexistent/libBambuSource.so";

    BambuSourceHandle handle(cfg);

    check(!handle.init(),           "init() with bogus path returns false");
    check(!handle.library_ready(),  "library_ready() false with no library");

    // bambu_create: must report an error and zero the out param.
    void* tunnel = reinterpret_cast<void*>(0xDEAD); // sentinel
    int   rc     = handle.bambu_create(&tunnel, "bambu:///rtsps___bblp:x@10.0.0.1/streaming/live/1?proto=rtsps");
    check(rc != 0,             "bambu_create returns non-zero with no lib");
    check(tunnel == nullptr,   "bambu_create zeroes out_tunnel even on failure");

    // Destroying nullptr is always safe.
    handle.bambu_destroy(nullptr);
    check(true, "bambu_destroy(nullptr) does not crash");

    // open / close / start_stream on a nullptr (or stale) tunnel must
    // return the documented sentinel and NEVER deref.
    check(handle.bambu_open(nullptr) == -1,           "bambu_open(nullptr) == -1");
    handle.bambu_close(nullptr);
    check(true,                                       "bambu_close(nullptr) no-op");
    check(handle.bambu_start_stream(nullptr, true) == -1, "bambu_start_stream(nullptr) == -1");

    check(handle.bambu_get_stream_count(nullptr) == 0,    "stream count nullptr == 0");
    check(handle.bambu_get_stream_info(nullptr, 0, nullptr) == -1,
                                                          "get_stream_info nullptr == -1");
    check(handle.bambu_read_sample(nullptr, nullptr) == -1,
                                                          "read_sample nullptr == -1");

    // Even after a successful init() *would* have happened, a fake
    // tunnel pointer should be ignored harmlessly because the symbols
    // were never resolved. The reverse case (real lib but bogus tunnel)
    // is the library's responsibility, not ours.
    void* fake = reinterpret_cast<void*>(0xFEED);
    check(handle.bambu_open(fake) == -1,               "bambu_open(fake) == -1 (no lib)");
    check(handle.bambu_start_stream(fake, true) == -1, "bambu_start_stream(fake) == -1 (no lib)");

    if (g_fails) {
        std::fprintf(stderr,
                     "BambuSourceHandleTest: %d assertion(s) failed\n", g_fails);
        return 1;
    }
    std::printf("BambuSourceHandleTest: ok\n");
    return 0;
}
