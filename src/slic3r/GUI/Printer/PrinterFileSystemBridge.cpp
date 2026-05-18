// Bambu Bridge — PrinterFileSystem static-lib dispatch implementation.
// See PrinterFileSystemBridge.hpp for design rationale.

#include "PrinterFileSystemBridge.hpp"

#if defined(BAMBU_BRIDGE)

#include "VirtualBambuTunnel.hpp"

#include <cstdint>

#if defined(BAMBU_BRIDGE_HARNESS_ENABLE)
// Harness ShimRecorder hooks. Each StaticBambuLib trampoline gets a
// ShimRecorder::record() call so a slicer run can capture a golden
// libBambuSource trace for the comparator. The recorder is OFF unless
// BAMBU_BRIDGE_SHIM env var is set.
//
// Variadic so call sites can pass an in-place `nlohmann::json{{...}}`
// for the args_json without the preprocessor splitting on the inner
// comma.
#include "harness/ShimRecorder.hpp"
#define BB_HARNESS_REC(...)                                                    \
    do {                                                                       \
        ::Slic3r::bridge::harness::ShimRecorder::instance().record(            \
            ::Slic3r::bridge::harness::TraceLib::BambuSource,                  \
            __VA_ARGS__);                                                      \
    } while (0)
#else
#define BB_HARNESS_REC(...) do {} while (0)
#endif

namespace Slic3r {
namespace bridge {

namespace {

// File-scope captures of the real libBambuSource entrypoints. These
// stand in for the `static auto real_*` locals that used to live inside
// PrinterFileSystem.cpp's StaticBambuLib::get(): same storage duration
// (program lifetime), same one-time write semantics (the caller
// invokes install_*() exactly once after the libBambuSource symbols
// have been resolved).
decltype(BambuLib::Bambu_Create)         real_Bambu_Create         = nullptr;
decltype(BambuLib::Bambu_Open)           real_Bambu_Open           = nullptr;
decltype(BambuLib::Bambu_StartStream)    real_Bambu_StartStream    = nullptr;
decltype(BambuLib::Bambu_StartStreamEx)  real_Bambu_StartStreamEx  = nullptr;
decltype(BambuLib::Bambu_GetStreamCount) real_Bambu_GetStreamCount = nullptr;
decltype(BambuLib::Bambu_GetStreamInfo)  real_Bambu_GetStreamInfo  = nullptr;
decltype(BambuLib::Bambu_SendMessage)    real_Bambu_SendMessage    = nullptr;
decltype(BambuLib::Bambu_ReadSample)     real_Bambu_ReadSample     = nullptr;
decltype(BambuLib::Bambu_Close)          real_Bambu_Close          = nullptr;
decltype(BambuLib::Bambu_Destroy)        real_Bambu_Destroy        = nullptr;
decltype(BambuLib::Bambu_SetLogger)      real_Bambu_SetLogger      = nullptr;

// Fake_Bambu_Create from PrinterFileSystem.cpp's StaticBambuLib. Stored
// here so the Bambu_Create dispatcher can reach it without needing the
// StaticBambuLib type definition.
BambuCreateFn fake_Bambu_Create_fn = nullptr;

} // namespace

void install_static_bambu_lib_dispatch(BambuLib&     lib,
                                       BambuCreateFn fake_bambu_create)
{
    // Snapshot the real entrypoints before we overwrite them. The
    // captures are file-scope so they outlive this function call.
    real_Bambu_Create         = lib.Bambu_Create;
    real_Bambu_Open           = lib.Bambu_Open;
    real_Bambu_StartStream    = lib.Bambu_StartStream;
    real_Bambu_StartStreamEx  = lib.Bambu_StartStreamEx;
    real_Bambu_GetStreamCount = lib.Bambu_GetStreamCount;
    real_Bambu_GetStreamInfo  = lib.Bambu_GetStreamInfo;
    real_Bambu_SendMessage    = lib.Bambu_SendMessage;
    real_Bambu_ReadSample     = lib.Bambu_ReadSample;
    real_Bambu_Close          = lib.Bambu_Close;
    real_Bambu_Destroy        = lib.Bambu_Destroy;
    real_Bambu_SetLogger      = lib.Bambu_SetLogger;
    fake_Bambu_Create_fn      = fake_bambu_create;

    // Wrap every Bambu_* entrypoint with a dispatcher that sniffs the
    // tunnel pointer's magic header. Virtual tunnels (minted by our own
    // Bambu_Create for `bambu:///virtual/...` URLs) route to
    // `virtual_tunnel::*`; real tunnels fall through to libBambuSource.
    lib.Bambu_Create = [](Bambu_Tunnel* out, char const* url) -> int {
        const int rc = Slic3r::virtual_tunnel::url_is_virtual(url)
            ? Slic3r::virtual_tunnel::Bambu_Create_virtual(out, url)
            : (real_Bambu_Create
                ? real_Bambu_Create(out, url)
                : (fake_Bambu_Create_fn ? fake_Bambu_Create_fn(out, url) : -2));
        BB_HARNESS_REC("Bambu_Create",
            nlohmann::json{{"url", url ? std::string(url) : std::string()}},
            rc);
        return rc;
    };
    lib.Bambu_Open = [](Bambu_Tunnel t) -> int {
        const bool is_v = Slic3r::virtual_tunnel::is_virtual_tunnel(t);
        const int rc = is_v
            ? Slic3r::virtual_tunnel::Bambu_Open_virtual(t)
            : (real_Bambu_Open ? real_Bambu_Open(t) : -1);
        BB_HARNESS_REC("Bambu_Open",
            nlohmann::json{{"tunnel", reinterpret_cast<std::uintptr_t>(t)}},
            rc);
        return rc;
    };
    lib.Bambu_StartStream = [](Bambu_Tunnel t, bool video) -> int {
        const int rc = Slic3r::virtual_tunnel::is_virtual_tunnel(t)
            ? Slic3r::virtual_tunnel::Bambu_StartStream_virtual(t, video)
            : (real_Bambu_StartStream ? real_Bambu_StartStream(t, video) : -1);
        BB_HARNESS_REC("Bambu_StartStream",
            nlohmann::json{
                {"tunnel", reinterpret_cast<std::uintptr_t>(t)},
                {"video", video}},
            rc);
        return rc;
    };
    lib.Bambu_StartStreamEx = [](Bambu_Tunnel t, int type) -> int {
        const bool is_v = Slic3r::virtual_tunnel::is_virtual_tunnel(t);
        const int rc = is_v
            ? Slic3r::virtual_tunnel::Bambu_StartStreamEx_virtual(t, type)
            : (real_Bambu_StartStreamEx ? real_Bambu_StartStreamEx(t, type) : -1);
        return rc;
    };
    lib.Bambu_SendMessage = [](Bambu_Tunnel t, int ctrl,
                               char const* data, int len) -> int {
        const bool is_v = Slic3r::virtual_tunnel::is_virtual_tunnel(t);
        if (is_v)
            return Slic3r::virtual_tunnel::Bambu_SendMessage_virtual(t, ctrl, data, len);
        return real_Bambu_SendMessage
            ? real_Bambu_SendMessage(t, ctrl, data, len)
            : -1;
    };
    lib.Bambu_ReadSample = [](Bambu_Tunnel t, Bambu_Sample* s) -> int {
        const bool is_v = Slic3r::virtual_tunnel::is_virtual_tunnel(t);
        const int rc = is_v
            ? Slic3r::virtual_tunnel::Bambu_ReadSample_virtual(t, s)
            : (real_Bambu_ReadSample ? real_Bambu_ReadSample(t, s) : Bambu_stream_end);
        // Don't dump the buffer — single-frame stream traces are
        // already O(MB). The size + rc is enough for replay diffing.
        BB_HARNESS_REC("Bambu_ReadSample",
            nlohmann::json{
                {"tunnel", reinterpret_cast<std::uintptr_t>(t)},
                {"size", (s ? static_cast<int>(s->size) : 0)}},
            rc);
        return rc;
    };
    lib.Bambu_Close = [](Bambu_Tunnel t) {
        if (Slic3r::virtual_tunnel::is_virtual_tunnel(t)) {
            Slic3r::virtual_tunnel::Bambu_Close_virtual(t);
        } else if (real_Bambu_Close) {
            real_Bambu_Close(t);
        }
        BB_HARNESS_REC("Bambu_Close",
            nlohmann::json{{"tunnel", reinterpret_cast<std::uintptr_t>(t)}},
            nlohmann::json());
    };
    lib.Bambu_Destroy = [](Bambu_Tunnel t) {
        if (Slic3r::virtual_tunnel::is_virtual_tunnel(t)) {
            Slic3r::virtual_tunnel::Bambu_Destroy_virtual(t);
        } else if (real_Bambu_Destroy) {
            real_Bambu_Destroy(t);
        }
        BB_HARNESS_REC("Bambu_Destroy",
            nlohmann::json{{"tunnel", reinterpret_cast<std::uintptr_t>(t)}},
            nlohmann::json());
    };
    lib.Bambu_SetLogger = [](Bambu_Tunnel t, Logger logger, void* ctx) {
        const bool is_v = Slic3r::virtual_tunnel::is_virtual_tunnel(t);
        if (is_v) {
            Slic3r::virtual_tunnel::Bambu_SetLogger_virtual(t, logger, ctx);
            return;
        }
        if (real_Bambu_SetLogger) real_Bambu_SetLogger(t, logger, ctx);
    };
}

} // namespace bridge
} // namespace Slic3r

#endif // BAMBU_BRIDGE
