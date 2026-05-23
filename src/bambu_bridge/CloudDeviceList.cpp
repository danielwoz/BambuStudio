// Bambu Bridge — native cloud device-list implementation.

#include "CloudDeviceList.hpp"
#include "CloudSession.hpp"

#include "third_party/nlohmann/json.hpp"

#include <curl/curl.h>
#include <boost/log/trivial.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace Slic3r {
namespace bridge {

namespace {

using nlohmann::json;

size_t write_to_string(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *s = static_cast<std::string *>(userdata);
    s->append(static_cast<const char *>(ptr), size * nmemb);
    return size * nmemb;
}

std::string getenv_str(const char *k)
{
    const char *v = std::getenv(k);
    return (v && *v) ? std::string(v) : std::string();
}

void set_common_curl_opts(CURL *curl, char *errbuf)
{
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bambu_bridge_devlist/1.0");
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

bool j_bool(const json &j, const char *key)
{
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number())  return it->get<double>() != 0.0;
    if (it->is_string()) {
        const auto s = it->get<std::string>();
        return s == "true" || s == "1";
    }
    return false;
}

} // namespace

CloudDeviceListResult CloudDeviceList::fetch(const CloudSessionData &session)
{
    CloudDeviceListResult r;
    if (session.access_token.empty()) {
        r.error = "empty access_token";
        return r;
    }

    CURL *curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }
    char errbuf[CURL_ERROR_SIZE] = {};

    const std::string url = "https://" + api_host_for_region(session.region)
                          + "/v1/iot-service/api/user/print?force=true";
    const std::string auth_hdr = "Authorization: Bearer " + session.access_token;

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, auth_hdr.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    set_common_curl_opts(curl, errbuf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    r.http_status = status;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    BOOST_LOG_TRIVIAL(info)
        << "CloudDeviceList::fetch url=" << url
        << " status=" << r.http_status
        << " body_len=" << r.body.size();

    if (status < 200 || status >= 300) {
        if (r.error.empty())
            r.error = "HTTP " + std::to_string(status) + " body=" + r.body.substr(0, 200);
        return r;
    }

    try {
        json j = json::parse(r.body);
        auto devs_it = j.find("devices");
        if (devs_it != j.end() && devs_it->is_array()) {
            for (const auto &d : *devs_it) {
                CloudListPrinter p;
                p.dev_id = j_str(d, "dev_id");
                // /print?force=true uses dev_name/dev_online; the legacy
                // bind endpoint used bare name/online. Tolerate both.
                p.name = !j_str(d, "dev_name").empty() ? j_str(d, "dev_name") : j_str(d, "name");
                p.online = d.contains("dev_online") ? j_bool(d, "dev_online") : j_bool(d, "online");
                p.model           = j_str(d, "dev_model_name");
                p.access_code     = j_str(d, "dev_access_code");
                p.nozzle_diameter = j_str(d, "nozzle_diameter");
                p.print_status    = j_str(d, "print_status");
                if (!p.dev_id.empty()) r.printers.push_back(std::move(p));
            }
        }
        r.ok = true;
    } catch (const std::exception &e) {
        r.error = std::string("parse devices JSON: ") + e.what()
                + " body=" + r.body.substr(0, 200);
    }
    return r;
}

} // namespace bridge
} // namespace Slic3r
