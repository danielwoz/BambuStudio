// Bambu Bridge — BambuSourceHandle implementation.
//
// dlopen / dlsym of the proprietary BambuSource library. See the header
// for the API surface and rationale.

#include "BambuSourceHandle.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>     // getenv
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace Slic3r {
namespace bridge {

namespace {

// ---- ABI mirrors of BambuTunnel.h ----------------------------------------
//
// We don't ship the proprietary header alongside the bridge static lib; the
// caller-facing API uses `void*` for the tunnel / sample / info pointers
// (see BambuSourceHandle.hpp) and we cast to / from these mirror structs
// internally. The layouts MUST match BambuTunnel.h byte-for-byte. Last
// verified against `~/BambuStudio/src/slic3r/GUI/Printer/BambuTunnel.h`
// (also vendored read-only at
// `src/slic3r/GUI/Printer/BambuTunnel.h` in this worktree).

extern "C" {

typedef void* MirrorBambu_Tunnel;
typedef void (*MirrorBambu_Logger)(void* context, int level, char const* msg);

// All exports use the C ABI (`extern "C"`). Function pointer typedefs
// below mirror the BambuTunnel.h declarations.
using fn_init               = int  (*)();
using fn_deinit             = void (*)();
using fn_create             = int  (*)(MirrorBambu_Tunnel* tunnel, char const* url);
using fn_destroy            = void (*)(MirrorBambu_Tunnel tunnel);
using fn_open               = int  (*)(MirrorBambu_Tunnel tunnel);
using fn_close              = void (*)(MirrorBambu_Tunnel tunnel);
using fn_start_stream       = int  (*)(MirrorBambu_Tunnel tunnel, bool video);
using fn_start_stream_ex    = int  (*)(MirrorBambu_Tunnel tunnel, int type);
using fn_get_stream_count   = int  (*)(MirrorBambu_Tunnel tunnel);
using fn_get_stream_info    = int  (*)(MirrorBambu_Tunnel tunnel, int index, void* info_out);
using fn_read_sample        = int  (*)(MirrorBambu_Tunnel tunnel, void* sample_out);
using fn_send_message       = int  (*)(MirrorBambu_Tunnel tunnel, int ctrl,
                                       char const* data, int len);
using fn_set_logger         = void (*)(MirrorBambu_Tunnel tunnel, MirrorBambu_Logger logger, void* ctx);
using fn_free_log_msg       = void (*)(char const* msg);
using fn_get_last_error_msg = char const* (*)();

} // extern "C"

// ---- Library-path probing ------------------------------------------------

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string home_dir() { return env_or_empty("HOME"); }

std::vector<std::string> default_library_candidates() {
    std::vector<std::string> out;
    std::string from_env = env_or_empty("BAMBU_SOURCE_PATH");
    if (!from_env.empty()) out.push_back(from_env);
#if defined(_WIN32)
    out.emplace_back("BambuSource.dll");
#elif defined(__APPLE__)
    out.emplace_back("/usr/local/lib/libBambuSource.dylib");
    std::string h = home_dir();
    if (!h.empty())
        out.push_back(h + "/Library/Application Support/BambuStudio/plugins/libBambuSource.dylib");
#else
    out.emplace_back("/usr/lib/x86_64-linux-gnu/libBambuSource.so");
    out.emplace_back("/usr/local/lib/libBambuSource.so");
    std::string h = home_dir();
    if (!h.empty())
        out.push_back(h + "/.config/BambuStudio/plugins/libBambuSource.so");
#endif
    return out;
}

// ---- Cross-platform dynamic-loader shim ---------------------------------

struct DynLib {
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void*   handle = nullptr;
#endif
    std::string path;

    bool open(const std::string& p) {
        path = p;
#if defined(_WIN32)
        handle = LoadLibraryA(p.c_str());
#else
        handle = dlopen(p.c_str(), RTLD_LAZY | RTLD_LOCAL);
#endif
        return handle != nullptr;
    }
    template <typename FnT>
    FnT sym(const char* name) const {
        if (!handle) return nullptr;
#if defined(_WIN32)
        return reinterpret_cast<FnT>(GetProcAddress(handle, name));
#else
        return reinterpret_cast<FnT>(dlsym(handle, name));
#endif
    }
    void close() {
        if (!handle) return;
#if defined(_WIN32)
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        handle = nullptr;
    }
    ~DynLib() { close(); }
};

} // namespace

// ---- Impl ----------------------------------------------------------------

struct BambuSourceHandle::Impl {
    BambuSourceConfig cfg;

    DynLib            lib;
    bool              inited = false;

    // Resolved entry points (subset we use today).
    fn_init             p_init             = nullptr;
    fn_deinit           p_deinit           = nullptr;
    fn_create           p_create           = nullptr;
    fn_destroy          p_destroy          = nullptr;
    fn_open             p_open             = nullptr;
    fn_close            p_close            = nullptr;
    fn_start_stream     p_start_stream     = nullptr;
    fn_start_stream_ex  p_start_stream_ex  = nullptr;
    fn_get_stream_count p_get_stream_count = nullptr;
    fn_get_stream_info  p_get_stream_info  = nullptr;
    fn_read_sample      p_read_sample      = nullptr;
    fn_send_message     p_send_message     = nullptr;
    fn_set_logger       p_set_logger       = nullptr;
    fn_free_log_msg     p_free_log_msg     = nullptr;
    fn_get_last_error_msg p_get_last_err   = nullptr;

    std::once_flag    init_once;
    std::atomic<bool> ready_flag{false};
    std::atomic<bool> ready_override{false};

    explicit Impl(BambuSourceConfig c) : cfg(std::move(c)) {}

    ~Impl() {
        if (inited && p_deinit) p_deinit();
        inited = false;
    }

    bool load_library() {
        if (lib.handle) return true;
        std::vector<std::string> candidates;
        if (!cfg.library_path.empty()) candidates.push_back(cfg.library_path);
        else                           candidates = default_library_candidates();
        for (const auto& cand : candidates) {
            if (lib.open(cand)) break;
        }
        if (!lib.handle) return false;

        p_init             = lib.sym<fn_init>            ("Bambu_Init");
        p_deinit           = lib.sym<fn_deinit>          ("Bambu_Deinit");
        p_create           = lib.sym<fn_create>          ("Bambu_Create");
        p_destroy          = lib.sym<fn_destroy>         ("Bambu_Destroy");
        p_open             = lib.sym<fn_open>            ("Bambu_Open");
        p_close            = lib.sym<fn_close>           ("Bambu_Close");
        p_start_stream     = lib.sym<fn_start_stream>    ("Bambu_StartStream");
        p_start_stream_ex  = lib.sym<fn_start_stream_ex> ("Bambu_StartStreamEx");
        p_get_stream_count = lib.sym<fn_get_stream_count>("Bambu_GetStreamCount");
        p_get_stream_info  = lib.sym<fn_get_stream_info> ("Bambu_GetStreamInfo");
        p_read_sample      = lib.sym<fn_read_sample>     ("Bambu_ReadSample");
        p_send_message     = lib.sym<fn_send_message>    ("Bambu_SendMessage");
        p_set_logger       = lib.sym<fn_set_logger>      ("Bambu_SetLogger");
        p_free_log_msg     = lib.sym<fn_free_log_msg>    ("Bambu_FreeLogMsg");
        p_get_last_err     = lib.sym<fn_get_last_error_msg>("Bambu_GetLastErrorMsg");

        // Bambu_Create / Bambu_Destroy / Bambu_ReadSample are the absolute
        // floor — without them this handle has no purpose. Bambu_Init
        // itself is optional in some library builds (it returns 0 by
        // default), but every shipped Linux build exports it.
        return p_create && p_destroy && p_read_sample;
    }

    bool ensure_inited() {
        bool ok = false;
        std::call_once(init_once, [&] {
            if (!load_library()) return;
            // INTENTIONALLY DO NOT call p_init / Bambu_Init.
            //
            // The slicer's StaticBambuLib (PrinterFileSystem.cpp's
            // GET_FUNC chain) resolves the function-pointer table and
            // calls Bambu_Create / Open / StartStreamEx directly,
            // never touching Bambu_Init. Observation: in-process the
            // slicer's storage tunnel opens cleanly (StartStreamEx
            // returns 0 on the first call), while the bridge's path
            // — which previously DID call Bambu_Init — saw
            // StartStreamEx spin on Bambu_would_block forever, never
            // receiving a frame from the printer. The most likely
            // explanation is that Bambu_Init flips the library into a
            // mode that's incompatible with the way Bambu_StartStreamEx
            // is invoked, or that it conflicts with a separate init
            // sequence the slicer's own pipeline performs implicitly.
            // Either way, skipping the call matches the slicer's
            // proven path.
            inited = true;
            ready_flag.store(true);
        });
        ok = ready_flag.load();
        return ok;
    }
};

// ---- Public API ----------------------------------------------------------

BambuSourceHandle::BambuSourceHandle(BambuSourceConfig cfg)
    : m_impl(std::make_unique<Impl>(std::move(cfg))) {}

BambuSourceHandle::~BambuSourceHandle() = default;

bool BambuSourceHandle::init() {
    return m_impl->ensure_inited();
}

bool BambuSourceHandle::library_ready() const {
    if (m_impl->ready_override.load()) return true;
    return m_impl->ready_flag.load();
}

int BambuSourceHandle::bambu_create(void** out_tunnel, const std::string& url) {
    if (out_tunnel) *out_tunnel = nullptr;
    if (!m_impl->ready_flag.load() || !m_impl->p_create) return -1;
    MirrorBambu_Tunnel t = nullptr;
    int rc = m_impl->p_create(&t, url.c_str());
    if (out_tunnel) *out_tunnel = t;
    return rc;
}

void BambuSourceHandle::bambu_destroy(void* tunnel) {
    if (!tunnel) return;
    if (!m_impl->ready_flag.load() || !m_impl->p_destroy) return;
    m_impl->p_destroy(tunnel);
}

int BambuSourceHandle::bambu_open(void* tunnel) {
    if (!tunnel) return -1;
    if (!m_impl->ready_flag.load() || !m_impl->p_open) return -1;
    return m_impl->p_open(tunnel);
}

void BambuSourceHandle::bambu_close(void* tunnel) {
    if (!tunnel) return;
    if (!m_impl->ready_flag.load() || !m_impl->p_close) return;
    m_impl->p_close(tunnel);
}

int BambuSourceHandle::bambu_start_stream(void* tunnel, bool video) {
    if (!tunnel) return -1;
    if (!m_impl->ready_flag.load() || !m_impl->p_start_stream) return -1;
    return m_impl->p_start_stream(tunnel, video);
}

int BambuSourceHandle::bambu_get_stream_count(void* tunnel) {
    if (!tunnel) return 0;
    if (!m_impl->ready_flag.load() || !m_impl->p_get_stream_count) return 0;
    return m_impl->p_get_stream_count(tunnel);
}

int BambuSourceHandle::bambu_get_stream_info(void* tunnel, int index, void* info_out) {
    if (!tunnel) return -1;
    if (!m_impl->ready_flag.load() || !m_impl->p_get_stream_info) return -1;
    return m_impl->p_get_stream_info(tunnel, index, info_out);
}

int BambuSourceHandle::bambu_read_sample(void* tunnel, void* sample_out) {
    if (!tunnel) return -1;
    if (!m_impl->ready_flag.load() || !m_impl->p_read_sample) return -1;
    return m_impl->p_read_sample(tunnel, sample_out);
}

int BambuSourceHandle::bambu_start_stream_ex(void* tunnel, int type) {
    if (!tunnel) return -1;
    if (!m_impl->ready_flag.load() || !m_impl->p_start_stream_ex) return -1;
    return m_impl->p_start_stream_ex(tunnel, type);
}

int BambuSourceHandle::bambu_send_message(void* tunnel, int ctrl,
                                          const char* data, int len) {
    if (!tunnel) return -1;
    if (!m_impl->ready_flag.load() || !m_impl->p_send_message) return -1;
    return m_impl->p_send_message(tunnel, ctrl, data, len);
}

void BambuSourceHandle::bambu_set_logger(void* tunnel,
                                         BambuSourceHandle::Logger logger,
                                         void* ctx) {
    if (!tunnel) return;
    if (!m_impl->ready_flag.load() || !m_impl->p_set_logger) return;
    m_impl->p_set_logger(
        tunnel,
        reinterpret_cast<MirrorBambu_Logger>(logger),
        ctx);
}

void BambuSourceHandle::bambu_free_log_msg(char const* msg) {
    if (!msg) return;
    if (!m_impl->ready_flag.load() || !m_impl->p_free_log_msg) return;
    m_impl->p_free_log_msg(msg);
}

std::string BambuSourceHandle::bambu_get_last_error_msg() {
    if (!m_impl->ready_flag.load() || !m_impl->p_get_last_err)
        return std::string{};
    const char* p = m_impl->p_get_last_err();
    return p ? std::string(p) : std::string{};
}

void BambuSourceHandle::set_library_ready_for_test(bool ready) {
    m_impl->ready_override.store(ready);
    m_impl->ready_flag.store(ready);
}

} // namespace bridge
} // namespace Slic3r
