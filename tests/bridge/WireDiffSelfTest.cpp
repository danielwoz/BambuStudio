// Bambu Bridge - WireDiff self-test (phase 11).
//
// This test gates that the per-protocol normalisers in
// tools/wire_diff/normalisers/ are doing their job. It does NOT need
// any bridge server running. It exercises wire_diff.py three ways
// against each protocol:
//
//   1) IDENTITY            -> diff(fixture, fixture)        must be 0
//   2) NORMALISABLE-CHANGE -> diff(fixture, mutated-variable)
//                             where the mutation rewrites a field the
//                             normaliser is supposed to mask away
//                             (PASV port, MQTT packet_id, RTP SSRC,
//                             LAN IP, etc.). MUST also be 0; otherwise
//                             a future regression that over-tightens
//                             the diff surfaces here.
//   3) STRUCTURAL-CHANGE   -> diff(fixture, mutated-structure)
//                             where the mutation rewrites a field that
//                             SHOULD diff (USN serial, MQTT topic, FTP
//                             reply code, RTSP method verb). MUST be 1.
//
// The test shells out to `python3 wire_diff.py` so it exercises the
// CLI surface the rest of the harness uses. It can SKIP cleanly (return
// 77) when python3 isn't on PATH, but the verification gate in
// docs/bambu_bridge_plan.md notes python3 is required on this host.
//
// Tempfiles live under $TMPDIR/bambu-bridge-wirediff-self-<pid>/.

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr int kCtestSkip = 77;
int g_fails = 0;

void check(bool ok, const std::string& what) {
    if (!ok) { ++g_fails; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
    else     {            std::fprintf(stderr, "ok   %s\n", what.c_str()); }
}

// Resolve the wire_diff tree relative to this test binary. The CMake
// rule passes BAMBU_BRIDGE_WIRE_DIFF_DIR as a -D define; if absent we
// guess based on the source tree layout.
std::string wire_diff_dir() {
#ifdef BAMBU_BRIDGE_WIRE_DIFF_DIR
    return std::string(BAMBU_BRIDGE_WIRE_DIFF_DIR);
#else
    return "tools/wire_diff";
#endif
}

// Run `python3 <wire_diff.py> <ref> <bri> --protocol <proto>` and return
// the exit code (or -1 on spawn failure).
int run_wire_diff(const std::string& ref, const std::string& bri,
                  const std::string& protocol, bool quiet = true) {
    std::string py_script = wire_diff_dir() + "/wire_diff.py";

    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: redirect stdout/stderr to /dev/null when quiet.
        if (quiet) {
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("python3"));
        argv.push_back(const_cast<char*>(py_script.c_str()));
        argv.push_back(const_cast<char*>(ref.c_str()));
        argv.push_back(const_cast<char*>(bri.c_str()));
        argv.push_back(const_cast<char*>("--protocol"));
        argv.push_back(const_cast<char*>(protocol.c_str()));
        argv.push_back(nullptr);
        ::execvp("python3", argv.data());
        std::_Exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -1;
    return -1;
}

// Locate the fixtures directory. Same logic as wire_diff_dir(),
// reaching into fixtures/.
std::string fixture(const std::string& name) {
    return wire_diff_dir() + "/fixtures/" + name;
}

// Copy a file to dest. Returns true on success.
bool copy_file(const std::string& src, const std::string& dst) {
    std::error_code ec;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::fprintf(stderr, "copy_file %s -> %s failed: %s\n",
                     src.c_str(), dst.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// Read file into a string of bytes.
std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Write bytes to file.
bool dump(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// Replace one ASCII substring once. Returns true if the search string
// was found.
bool replace_once(std::string& s, const std::string& needle,
                  const std::string& with) {
    auto p = s.find(needle);
    if (p == std::string::npos) return false;
    s.replace(p, needle.size(), with);
    return true;
}

// Quick sanity probe: is `python3` invokable at all? If not, this test
// must SKIP rather than fail (the harness contract).
bool python3_present() {
    int rc = std::system("python3 --version >/dev/null 2>&1");
    return (rc == 0);
}

} // namespace

int main() {
    if (!python3_present()) {
        std::fprintf(stderr, "SKIP: python3 not on PATH\n");
        return kCtestSkip;
    }

    // Working dir for tempfiles.
    fs::path workdir = fs::temp_directory_path() /
                       ("bambu-bridge-wirediff-self-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::create_directories(workdir, ec);
    if (ec) {
        std::fprintf(stderr, "SKIP: tmp dir create failed: %s\n",
                     ec.message().c_str());
        return kCtestSkip;
    }
    struct Cleanup {
        fs::path dir;
        ~Cleanup() { std::error_code e; fs::remove_all(dir, e); }
    } cleanup{workdir};

    // --- SSDP ----------------------------------------------------------
    {
        const std::string fx = fixture("ssdp_reference.txt");
        check(run_wire_diff(fx, fx, "ssdp") == 0,
              "SSDP: identity diff exits 0");

        // Normalisable mutation: change LAN IP in LOCATION + DevSignal.
        // The reference fixture uses the bare-IP LOCATION shape the bridge
        // actually emits (see SsdpResponder.cpp's build_search_response_headers).
        // The normaliser also accepts the legacy URL form, so this test
        // continues to assert that LAN-IP differences get masked.
        std::string body = slurp(fx);
        check(replace_once(body, "LOCATION: 127.0.0.1",
                                 "LOCATION: 192.0.2.42"),
              "SSDP: LOCATION mutation point present in fixture");
        // DevSignal is no longer emitted in M-SEARCH responses (the A1-derived
        // NOTIFY emits the bare integer; M-SEARCH omits it). This replace is
        // a no-op on the current fixture and intentionally tolerant.
        replace_once(body, "-50dBm", "-72dBm");
        const std::string mutated = (workdir / "ssdp_lan_ip_mutated.txt").string();
        check(dump(mutated, body), "SSDP: wrote LAN-IP-mutated tmp");
        check(run_wire_diff(fx, mutated, "ssdp") == 0,
              "SSDP: LAN-IP+signal mutation masked away (exit 0)");

        // Structural mutation: change USN serial.
        std::string body2 = slurp(fx);
        check(replace_once(body2, "USN: EXAMPLESERIAL01",
                                  "USN: WRONGSERIAL12345"),
              "SSDP: USN mutation point present");
        const std::string structural = (workdir / "ssdp_usn_mutated.txt").string();
        check(dump(structural, body2), "SSDP: wrote USN-mutated tmp");
        check(run_wire_diff(fx, structural, "ssdp") == 1,
              "SSDP: USN diff surfaces (exit 1)");
    }

    // --- FTPS ----------------------------------------------------------
    {
        const std::string fx = fixture("ftps_session.txt");
        check(run_wire_diff(fx, fx, "ftps") == 0,
              "FTPS: identity diff exits 0");

        // Normalisable: change PASV port quartet.
        std::string body = slurp(fx);
        check(replace_once(body, "192,168,1,209,234,17",
                                  "127,0,0,2,200,5"),
              "FTPS: PASV tuple present");
        const std::string mutated = (workdir / "ftps_pasv_mutated.txt").string();
        check(dump(mutated, body), "FTPS: wrote PASV-mutated tmp");
        check(run_wire_diff(fx, mutated, "ftps") == 0,
              "FTPS: PASV ip/port mutation masked away (exit 0)");

        // Structural: change PASS reply from 230 to 530.
        std::string body2 = slurp(fx);
        check(replace_once(body2, "230 Login successful.",
                                  "530 Login incorrect."),
              "FTPS: 230 reply present");
        const std::string structural = (workdir / "ftps_auth_mutated.txt").string();
        check(dump(structural, body2), "FTPS: wrote auth-mutated tmp");
        check(run_wire_diff(fx, structural, "ftps") == 1,
              "FTPS: 530 vs 230 surfaces (exit 1)");
    }

    // --- RTSP ----------------------------------------------------------
    {
        const std::string fx = fixture("rtsp_describe.txt");
        check(run_wire_diff(fx, fx, "rtsp") == 0,
              "RTSP/describe: identity diff exits 0");

        std::string body = slurp(fx);
        // Normalisable: CSeq + SDP origin address.
        check(replace_once(body, "CSeq: 1", "CSeq: 42"),
              "RTSP: CSeq present");
        replace_once(body, "CSeq: 2", "CSeq: 43");
        const std::string mutated = (workdir / "rtsp_cseq_mutated.txt").string();
        check(dump(mutated, body), "RTSP: wrote CSeq-mutated tmp");
        check(run_wire_diff(fx, mutated, "rtsp") == 0,
              "RTSP: CSeq mutation masked away (exit 0)");

        // Structural: replace OPTIONS verb (in client request line).
        std::string body2 = slurp(fx);
        check(replace_once(body2, "OPTIONS rtsp://",
                                  "DESCRIB rtsp://"),
              "RTSP: OPTIONS verb present");
        const std::string structural = (workdir / "rtsp_verb_mutated.txt").string();
        check(dump(structural, body2), "RTSP: wrote verb-mutated tmp");
        check(run_wire_diff(fx, structural, "rtsp") == 1,
              "RTSP: verb-change surfaces (exit 1)");
    }

    // --- RTSP play (interleaved RTP) -----------------------------------
    {
        const std::string fx = fixture("rtsp_play.txt");
        check(run_wire_diff(fx, fx, "rtsp") == 0,
              "RTSP/play: identity diff exits 0");
        // Normalisable: change the Session-id (server-randomised).
        std::string body = slurp(fx);
        // 1B57F12A appears in SETUP/PLAY/200 OK Session: headers.
        size_t replaced = 0;
        while (true) {
            auto p = body.find("1B57F12A");
            if (p == std::string::npos) break;
            body.replace(p, 8, "DEADBEEF");
            ++replaced;
        }
        check(replaced > 0, "RTSP/play: Session id mutation point present");
        const std::string mutated = (workdir / "rtsp_play_sid_mutated.txt").string();
        check(dump(mutated, body), "RTSP/play: wrote session-id-mutated tmp");
        check(run_wire_diff(fx, mutated, "rtsp") == 0,
              "RTSP/play: Session id mutation masked away (exit 0)");
    }

    // --- MQTT ----------------------------------------------------------
    {
        const std::string fx = fixture("mqtt_publish_print.bin");
        check(run_wire_diff(fx, fx, "mqtt") == 0,
              "MQTT/publish: identity diff exits 0");

        // Normalisable: mutate packet_id (PUBLISH QoS1 + PUBACK both have
        // packet_id == 0x02 in our fixture; mqtt.py masks both).
        // The packet_id 0x0002 appears at fixed offsets we don't want to
        // compute by hand; instead we rebuild a new blob with a different
        // packet_id via a small Python one-liner so the test stays robust
        // to fixture re-layouts.
        std::string regen = workdir.string() + "/mqtt_pid_mutated.bin";
        std::string cmd =
            "python3 -c \""
            "import struct\n"
            "def vi(v):\n out=bytearray()\n while True:\n  d=v&0x7F; v>>=7\n  "
            "if v: d|=0x80\n  out.append(d)\n  if not v: break\n return bytes(out)\n"
            "def mstr(s):\n b=s.encode(); return struct.pack('>H',len(b))+b\n"
            "dev='EXAMPLESERIAL01'\n"
            "payload=b'{" "\\\"" "info" "\\\"" ":{" "\\\"" "command" "\\\""
                ":" "\\\"" "get_version" "\\\""
                "," "\\\"" "sequence_id" "\\\"" ":" "\\\"" "20021" "\\\"" "}}'\n"
            "pid=0x07A2\n"
            "pub_var=mstr(f'device/{dev}/request') + struct.pack('>H', pid)\n"
            "pub_body=pub_var+payload\n"
            "publish=bytes([0x32])+vi(len(pub_body))+pub_body\n"
            "puback=bytes([0x40,0x02])+struct.pack('>H', pid)\n"
            "blob=publish+puback+bytes([0xE0,0])\n"
            "open(r'" + regen + "','wb').write(blob)\""
            " 2>/dev/null";
        int rc = std::system(cmd.c_str());
        check(rc == 0, "MQTT: regen mutated fixture");
        check(run_wire_diff(fx, regen, "mqtt") == 0,
              "MQTT: packet_id mutation masked away (exit 0)");

        // Structural: change the PUBLISH topic.
        std::string body = slurp(fx);
        // "request" appears once in PUBLISH topic; "reqzest" is same length.
        check(replace_once(body, "request", "reqzest"),
              "MQTT: PUBLISH topic mutation point present");
        const std::string structural =
            (workdir / "mqtt_topic_mutated.bin").string();
        check(dump(structural, body), "MQTT: wrote topic-mutated tmp");
        check(run_wire_diff(fx, structural, "mqtt") == 1,
              "MQTT: PUBLISH-topic diff surfaces (exit 1)");
    }

    if (g_fails) {
        std::fprintf(stderr, "WireDiffSelfTest: %d assertion(s) failed\n",
                     g_fails);
        return 1;
    }
    std::printf("WireDiffSelfTest: ok\n");
    return 0;
}
