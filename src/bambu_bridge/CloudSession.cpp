// Bambu Bridge — native cloud session.
//
// Implementation notes:
//
// - We bake a tiny libcurl helper into this TU rather than depending on
//   Slic3r::Http (which pulls a chain of GUI headers we'd rather not
//   pull into bambu_bridge). The helper mirrors the one in
//   ~/BambuStudio/src/bambu_net_oss/core/CloudRestClient.cpp.
//
// - CA bundle probing: same list of distro paths as the OSS shim. Honour
//   CURL_CA_BUNDLE / SSL_CERT_FILE first.
//
// - The session file is written atomically (write to <path>.tmp, rename)
//   so a crash mid-write can never leave us with a corrupt or
//   half-written JSON file the next boot can't parse.

#include "CloudSession.hpp"

#include "third_party/nlohmann/json.hpp"

#include <curl/curl.h>

#include <boost/log/trivial.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#  include <direct.h>
#  define mkdir_p(path) _mkdir(path)
#else
#  include <unistd.h>
#  define mkdir_p(path) mkdir((path), 0700)
#endif

namespace Slic3r {
namespace bridge {

namespace {

using nlohmann::json;

int64_t now_seconds()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string getenv_str(const char *k)
{
    const char *v = std::getenv(k);
    return (v && *v) ? std::string(v) : std::string();
}

std::string home_dir()
{
    if (auto h = getenv_str("HOME"); !h.empty()) return h;
    return ".";
}

std::string config_root()
{
    if (auto x = getenv_str("XDG_CONFIG_HOME"); !x.empty()) return x;
    return home_dir() + "/.config";
}

void mkdirs(const std::string &dir)
{
    // Create components left-to-right. Best-effort; ignores EEXIST.
    if (dir.empty()) return;
    std::string buf;
    buf.reserve(dir.size() + 1);
    for (size_t i = 0; i < dir.size(); ++i) {
        char c = dir[i];
        buf.push_back(c);
        if (c == '/' && !buf.empty() && buf.size() > 1) {
            (void) mkdir_p(buf.c_str());
        }
    }
    if (!buf.empty()) (void) mkdir_p(buf.c_str());
}

// libcurl write callback: append body bytes into a std::string.
size_t write_to_string(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *s = static_cast<std::string *>(userdata);
    s->append(static_cast<const char *>(ptr), size * nmemb);
    return size * nmemb;
}

void set_common_curl_opts(CURL *curl, char *errbuf)
{
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bambu_bridge_session/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    static const char *kCandidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/cert.pem",
        "/usr/local/share/certs/ca-root-nss.crt",
        nullptr,
    };
    if (auto env = getenv_str("CURL_CA_BUNDLE"); !env.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, env.c_str());
    } else if (auto env = getenv_str("SSL_CERT_FILE"); !env.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, env.c_str());
    } else {
        for (const char *const *p = kCandidates; *p; ++p) {
            if (FILE *f = std::fopen(*p, "rb")) {
                std::fclose(f);
                curl_easy_setopt(curl, CURLOPT_CAINFO, *p);
                break;
            }
        }
    }
}

std::string api_host_for_region(const std::string &region)
{
    std::string r = region;
    for (auto &c : r) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return (r == "cn") ? "api.bambulab.cn" : "api.bambulab.com";
}

std::string j_str(const json &j, const char *key)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return {};
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

int64_t j_int(const json &j, const char *key)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number_integer())  return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
    if (it->is_number_float())    return static_cast<int64_t>(it->get<double>());
    if (it->is_string()) {
        try { return std::stoll(it->get<std::string>()); } catch (...) { return 0; }
    }
    return 0;
}

// POST JSON body, optionally with bearer. Returns http_status and the
// raw response body via out-params; non-2xx is NOT an error here, the
// caller decides.
long http_post_json(const std::string &url,
                    const std::string &body_str,
                    const std::string &bearer,
                    std::string       *out_body,
                    std::string       *out_err)
{
    out_body->clear();
    out_err->clear();

    CURL *curl = curl_easy_init();
    if (!curl) { *out_err = "curl_easy_init failed"; return 0; }

    char errbuf[CURL_ERROR_SIZE] = {};

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    std::string auth_hdr;
    if (!bearer.empty()) {
        auth_hdr = "Authorization: Bearer " + bearer;
        headers = curl_slist_append(headers, auth_hdr.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_str.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_body);
    set_common_curl_opts(curl, errbuf);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    } else {
        *out_err = (errbuf[0] ? errbuf : curl_easy_strerror(rc));
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
}

} // namespace

bool CloudSessionData::access_valid(int slack_seconds) const noexcept
{
    if (access_token.empty()) return false;
    if (access_expires_at <= 0) return false;  // unknown — treat as expired
    return access_expires_at - now_seconds() > slack_seconds;
}

bool CloudSessionData::refresh_valid(int slack_seconds) const noexcept
{
    if (refresh_token.empty()) return false;
    if (refresh_expires_at <= 0) return true;  // unknown — assume still good
    return refresh_expires_at - now_seconds() > slack_seconds;
}

std::string session_file_path()
{
    return config_root() + "/BambuBridge/session.json";
}

std::string region_for_bridge()
{
    if (auto env = getenv_str("BBL_BRIDGE_REGION"); !env.empty()) {
        for (auto &c : env) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return (env == "cn") ? "cn" : "us";
    }
    // Try the slicer's BambuStudio.conf — plaintext JSON, "region" field.
    const std::string conf = config_root() + "/BambuStudio/BambuStudio.conf";
    std::ifstream f(conf);
    if (f) {
        try {
            json j;
            f >> j;
            std::string r = j_str(j, "region");
            for (auto &c : r) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            if (r.find("china") != std::string::npos || r == "cn") return "cn";
        } catch (...) {}
    }
    return "us";
}

CloudSessionLoadResult CloudSession::load_and_refresh_if_needed(int slack_seconds)
{
    CloudSessionLoadResult r;
    const std::string path = session_file_path();

    std::ifstream f(path);
    if (!f) {
        r.error = "session file not found: " + path;
        return r;
    }
    try {
        json j;
        f >> j;
        r.data.access_token       = j_str(j, "access_token");
        r.data.refresh_token      = j_str(j, "refresh_token");
        r.data.access_expires_at  = j_int(j, "access_expires_at");
        r.data.refresh_expires_at = j_int(j, "refresh_expires_at");
        r.data.uid                = j_int(j, "uid");
        r.data.region             = j_str(j, "region");
        if (r.data.region.empty()) r.data.region = region_for_bridge();
        r.data.user_email         = j_str(j, "user_email");
        r.from_file = true;
    } catch (const std::exception &e) {
        r.error = std::string("session file parse failed: ") + e.what();
        return r;
    }

    if (!r.data.has_tokens()) {
        r.error = "session file has no tokens";
        return r;
    }

    if (r.data.access_valid(slack_seconds)) {
        // Token is fine, use as-is.
        r.ok = true;
        return r;
    }

    if (!r.data.refresh_valid(slack_seconds)) {
        r.error = "refresh_token expired";
        return r;
    }

    // Need a refresh round-trip.
    BOOST_LOG_TRIVIAL(info)
        << "CloudSession: access_token near/past expiry, refreshing "
        << "(expires_at=" << r.data.access_expires_at
        << " now=" << now_seconds() << ")";
    auto refreshed = refresh(r.data);
    if (!refreshed.ok) {
        r.error = "refresh failed: " + refreshed.error;
        return r;
    }
    r.data = refreshed.data;
    r.refreshed = true;
    r.ok = true;
    // Persist the new tokens for next boot.
    if (!save(r.data)) {
        BOOST_LOG_TRIVIAL(warning)
            << "CloudSession: refresh succeeded but save failed; "
               "next boot will refresh again";
    }
    return r;
}

CloudSessionLoadResult CloudSession::refresh(const CloudSessionData &current)
{
    CloudSessionLoadResult r;
    if (current.refresh_token.empty()) {
        r.error = "no refresh_token";
        return r;
    }

    const std::string url = "https://" + api_host_for_region(current.region)
                          + "/v1/user-service/user/refreshtoken";
    json req;
    req["refreshToken"] = current.refresh_token;
    const std::string body_str = req.dump();

    std::string body, err;
    long status = http_post_json(url, body_str, /*bearer=*/"", &body, &err);

    BOOST_LOG_TRIVIAL(info)
        << "CloudSession::refresh url=" << url << " status=" << status
        << " body_len=" << body.size();

    if (status < 200 || status >= 300) {
        r.error = "HTTP " + std::to_string(status) + (err.empty() ? "" : (" curl=" + err));
        return r;
    }

    try {
        json j = json::parse(body);
        CloudSessionData d = current;  // inherit uid / region / email
        std::string at = j_str(j, "accessToken");
        std::string rt = j_str(j, "refreshToken");
        int64_t     ei = j_int(j, "expiresIn");
        int64_t     re = j_int(j, "refreshExpiresIn");
        int64_t     uid = j_int(j, "uid");
        std::string region = j_str(j, "region");
        if (at.empty() || rt.empty()) {
            r.error = "refresh response missing tokens; body=" + body.substr(0, 200);
            return r;
        }
        d.access_token       = at;
        d.refresh_token      = rt;
        d.access_expires_at  = (ei > 0) ? now_seconds() + ei : (now_seconds() + 28800);
        d.refresh_expires_at = (re > 0) ? now_seconds() + re : d.refresh_expires_at;
        if (uid > 0) d.uid = uid;
        if (!region.empty()) {
            for (auto &c : region) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
            d.region = (region == "cn") ? "cn" : "us";
        }
        r.data = d;
        r.ok = true;
    } catch (const std::exception &e) {
        r.error = std::string("parse refresh response: ") + e.what()
                + " body=" + body.substr(0, 200);
    }
    return r;
}

bool CloudSession::save(const CloudSessionData &data) const
{
    const std::string path = session_file_path();
    const std::string dir  = path.substr(0, path.find_last_of('/'));
    mkdirs(dir);

    const std::string tmp = path + ".tmp";

    json j;
    j["access_token"]       = data.access_token;
    j["refresh_token"]      = data.refresh_token;
    j["access_expires_at"]  = data.access_expires_at;
    j["refresh_expires_at"] = data.refresh_expires_at;
    j["uid"]                = data.uid;
    j["region"]             = data.region;
    j["user_email"]         = data.user_email;
    j["schema_version"]     = 1;
    j["written_at"]         = now_seconds();

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            BOOST_LOG_TRIVIAL(error)
                << "CloudSession::save: cannot open " << tmp;
            return false;
        }
        out << j.dump(2);
        if (!out.good()) {
            BOOST_LOG_TRIVIAL(error)
                << "CloudSession::save: write failed for " << tmp;
            return false;
        }
    }

#if !defined(_WIN32)
    // Tighten perms before the rename so the final file is never
    // world-readable, even briefly.
    if (chmod(tmp.c_str(), S_IRUSR | S_IWUSR) != 0) {
        BOOST_LOG_TRIVIAL(warning)
            << "CloudSession::save: chmod 0600 failed for " << tmp
            << ": " << std::strerror(errno);
    }
#endif

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        BOOST_LOG_TRIVIAL(error)
            << "CloudSession::save: rename " << tmp << " -> " << path
            << " failed: " << std::strerror(errno);
        return false;
    }
    BOOST_LOG_TRIVIAL(info)
        << "CloudSession::save: persisted to " << path
        << " (access_expires_at=" << data.access_expires_at
        << " uid=" << data.uid << " region=" << data.region << ")";
    return true;
}

// ---------------------------------------------------------------------------
// from_plugin_agent
//
// Wraps the plugin's accessor functions to extract the live tokens out
// of a plugin-loaded NetworkAgent. We don't include NetworkAgent.hpp
// here (would drag the GUI layer into the bridge static lib); instead
// the GUI side calls this with a raw void* and we ABI-match the
// NetworkAgent C++ class layout via a tiny shim header is messy. So
// the GUI side is responsible for building the CloudSessionData itself
// and just calls CloudSession::save(). This stub remains for
// completeness — it returns ok=false unconditionally; the production
// path is GUI_App::populate_session_from_agent() in BridgeBootstrap.cpp.
// ---------------------------------------------------------------------------

CloudSessionLoadResult CloudSession::from_plugin_agent(void *agent_ptr,
                                                       const std::string &region_hint)
{
    (void) agent_ptr;
    (void) region_hint;
    CloudSessionLoadResult r;
    r.error = "from_plugin_agent: call site must use GUI_App helper instead";
    return r;
}

} // namespace bridge
} // namespace Slic3r
