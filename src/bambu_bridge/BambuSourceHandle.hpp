// Bambu Bridge — BambuSourceHandle.
//
// dlopen / dlsym wrapper for the proprietary `libBambuSource.so` (Linux),
// `libBambuSource.dylib` (macOS), or `BambuSource.dll` (Windows). This is
// the same library real BambuStudio hands a camera URL to (see
// `~/BambuStudio/src/slic3r/GUI/MediaPlayCtrl.cpp` — the slicer never
// implements its own RTSP/Agora/TUTK client, it always goes through
// BambuSource). The library dispatches internally by URL scheme:
//
//   - `bambu:///rtsps___user:pw@ip/path?proto=rtsps` -> live555 RTSPS
//   - `bambu:///rtsp___user:pw@ip/path?proto=rtsp`   -> live555 RTSP
//   - `bambu:///agora/...`                           -> Agora P2P
//   - `bambu:///tutk/...`                            -> TUTK P2P
//   - `bambu:///local/<ip>?port=6000&...`            -> port-6000 tunnel
//
// All four cases reach us as opaque URLs from either the LAN side (built
// locally from the printer's known LAN credentials) or the cloud side
// (returned by the `bambu_networking` plugin's `get_camera_url`). We
// just forward them; BambuSource takes care of the transport.
//
// API surface (`BambuTunnel.h`, C-ABI, simpler than the C++-by-value
// `bambu_networking` ABI):
//
//   int  Bambu_Init   (void);
//   void Bambu_Deinit (void);
//   int  Bambu_Create (Bambu_Tunnel* out, char const* url);
//   void Bambu_Destroy(Bambu_Tunnel  t);
//   int  Bambu_Open   (Bambu_Tunnel  t);
//   void Bambu_Close  (Bambu_Tunnel  t);
//   int  Bambu_StartStream  (Bambu_Tunnel t, bool video);
//   int  Bambu_GetStreamCount(Bambu_Tunnel t);
//   int  Bambu_GetStreamInfo (Bambu_Tunnel t, int index, Bambu_StreamInfo*);
//   int  Bambu_ReadSample    (Bambu_Tunnel t, Bambu_Sample*);
//
// Per `BambuTunnel.h`:
//   * `Bambu_Sample.buffer` is a contiguous payload, `size` bytes.
//   * `Bambu_Sample.flags & 1` (Bambu_SampleFlag::f_sync) marks an IDR.
//   * `Bambu_Sample.decode_time` is `unsigned long long` (PTS-ish).
//   * `Bambu_ReadSample` returns 0 (success) / 1 (stream_end) /
//     2 (would_block) / 3 (buffer_limit) — `Bambu_Error` enum.
//
// Threading: BambuSource itself is thread-safe per-tunnel. We don't
// serialise; callers (`LanCameraSource` / `CloudCameraSource`) only ever
// hand one `next_frame` call at a time per source (`RtspServer`'s
// invariant).
//
// Test seam: every method is virtual so test mocks can subclass and
// record calls without dlopen. Same pattern as
// `BambuNetworkingPluginHandle`.

#ifndef SLIC3R_BAMBU_BRIDGE_BAMBU_SOURCE_HANDLE_HPP
#define SLIC3R_BAMBU_BRIDGE_BAMBU_SOURCE_HANDLE_HPP

#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r {
namespace bridge {

struct BambuSourceConfig {
    // Absolute path to the BambuSource shared library. Empty -> probe the
    // default candidate list (BAMBU_SOURCE_PATH env, /usr/lib paths,
    // ~/.config/BambuStudio/plugins/...).
    std::string library_path;
};

// `void*` aliases for the opaque tunnel + sample / stream-info structs.
// Avoids spilling `<BambuTunnel.h>` (proprietary header — vendored only
// for reference) into every translation unit that includes this file.
// The real struct definitions live inside BambuSourceHandle.cpp.
class BambuSourceHandle {
public:
    explicit BambuSourceHandle(BambuSourceConfig cfg = {});
    virtual ~BambuSourceHandle();

    BambuSourceHandle(const BambuSourceHandle&)            = delete;
    BambuSourceHandle& operator=(const BambuSourceHandle&) = delete;

    // dlopen the library + resolve every export + call `Bambu_Init`. Idempotent;
    // safe to call from concurrent threads (init runs under a once_flag).
    // Returns true on success. False when the library can't be opened, a
    // required export is missing, or `Bambu_Init` returns non-zero.
    virtual bool init();

    // True iff init() has completed successfully. Cheap; safe to poll.
    virtual bool library_ready() const;

    // ---- 1:1 wrappers over the C-ABI exports -----------------------------

    // Allocates a tunnel for `url`. The handle returns the tunnel via
    // `*out_tunnel`. Returns the library's int (0 = success). Returns -1
    // when the library isn't loaded.
    virtual int bambu_create(void** out_tunnel, const std::string& url);

    // Destroys a tunnel previously returned by `bambu_create`. No-op when
    // `tunnel` is nullptr or the library isn't loaded.
    virtual void bambu_destroy(void* tunnel);

    // Opens the tunnel (transport-level connect). Returns the library's
    // int (0 = success). Returns -1 when the library isn't loaded.
    virtual int bambu_open(void* tunnel);

    // Closes the tunnel. No-op when `tunnel` is nullptr / library
    // not loaded. Symmetric to bambu_open.
    virtual void bambu_close(void* tunnel);

    // Begins streaming (video=true selects video, false audio). Returns
    // the library's int rc (0 = success). -1 when not loaded.
    virtual int bambu_start_stream(void* tunnel, bool video);

    // Begins streaming with an explicit channel type — e.g. the
    // CTRL_TYPE=0x3001 control channel the slicer's PrinterFileSystem
    // uses for storage JSON-RPC. Returns the library's int rc
    // (0 = success). Returns -1 when the library isn't loaded.
    virtual int bambu_start_stream_ex(void* tunnel, int type);

    // Sends a control-channel message to the tunnel. `ctrl` is the same
    // CTRL_TYPE the slicer uses (0x3001 for storage JSON-RPC). `data`
    // points to `len` bytes of payload — the slicer can optionally
    // append `\n\n` + binary blob after the JSON. Returns the library's
    // int rc (0 = success). -1 when not loaded.
    virtual int bambu_send_message(void* tunnel, int ctrl,
                                   const char* data, int len);

    // Logger callback the library invokes for log strings on this
    // tunnel. Mirrors the C API surface; the bridge plumbs this through
    // for parity with the real slicer's per-tunnel diagnostics.
    //
    // IMPORTANT: the slicer's DumpLog callback always calls
    // Bambu_FreeLogMsg(msg) before returning. The proprietary lib
    // appears to wait on its internal log queue being drained before
    // start_stream_ex will advance — skipping the free leaves the
    // tunnel stuck on frame_count=0. Any consumer of this API MUST
    // call bambu_free_log_msg(msg) inside the callback.
    typedef void (*Logger)(void* context, int level, char const* msg);
    virtual void bambu_set_logger(void* tunnel, Logger logger, void* ctx);

    // Frees a `char const* msg` that the library handed to a Logger
    // callback. The slicer's PrinterFileSystem calls this on every log
    // message inside DumpLog. Returns silently when the library isn't
    // loaded.
    virtual void bambu_free_log_msg(char const* msg);

    // Returns whatever string the proprietary lib has set as its
    // "last error" — Bambu_GetLastErrorMsg. Empty string when the
    // library isn't loaded or hasn't recorded an error. Useful after
    // a Bambu_would_block / Bambu_stream_end failure to surface the
    // library's own diagnostic.
    virtual std::string bambu_get_last_error_msg();

    // Returns the number of streams discovered after `bambu_start_stream`.
    // 0 on no streams / not loaded.
    virtual int bambu_get_stream_count(void* tunnel);

    // Fills `*info_out` (a Bambu_StreamInfo*) with stream `index`. Returns
    // the library's int rc (0 = success). -1 when not loaded.
    virtual int bambu_get_stream_info(void* tunnel, int index, void* info_out);

    // Pulls the next sample into `*sample_out` (a Bambu_Sample*). Returns
    // the library's `Bambu_Error` (0=success, 1=stream_end, 2=would_block,
    // 3=buffer_limit). Returns -1 when not loaded.
    //
    // The `Bambu_Sample::buffer` pointer is owned by the library; the
    // caller MUST copy out the bytes before the next call.
    virtual int bambu_read_sample(void* tunnel, void* sample_out);

protected:
    // For test subclasses that want to skip dlopen but still report
    // "ready" to anyone who polls library_ready().
    void set_library_ready_for_test(bool ready);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_BAMBU_SOURCE_HANDLE_HPP
