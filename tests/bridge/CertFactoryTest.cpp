// Bambu Bridge — CertFactory unit test (phase 2).
//
// Coverage:
//   1. get_or_create() is idempotent — second call returns byte-equal
//      cert + key + fingerprint (proves the disk-cache hit).
//   2. regenerate() yields a *different* fingerprint than the cached
//      one (proves we actually re-mint, not just re-load).
//   3. The minted cert has Subject CN == dev_id and Issuer CN == "BBL CA".
//   4. The minted cert is not already expired (notAfter > now).
//   5. On POSIX, the private-key file is stored with mode 0600.
//   6. BridgeService::set_cert_factory + cert_factory() round-trip works.
//
// We deliberately don't touch the real $XDG_CONFIG_HOME — the test runs
// inside its own scratch dir under temp_directory_path() and cleans up
// on exit (success path) regardless.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include "BridgeService.hpp"
#include "tls/CertFactory.hpp"

namespace fs = std::filesystem;
using Slic3r::bridge::tls::CertFactory;
using Slic3r::bridge::tls::CertFactoryConfig;
using Slic3r::bridge::tls::CertMaterial;

namespace {

#define BRIDGE_REQUIRE(cond, msg)                                          \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

fs::path make_scratch_dir() {
    auto pid =
#if defined(_WIN32)
        static_cast<unsigned long>(::_getpid());
#else
        static_cast<unsigned long>(::getpid());
#endif
    fs::path p = fs::temp_directory_path() /
                 ("bridge_cert_test_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// Parse PEM bytes into an X509* the caller must X509_free().
X509* parse_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free_all(bio);
    return cert;
}

// One-line subject/issuer text; safe to substring-match against.
std::string name_oneline(X509_NAME* n) {
    char buf[256] = {0};
    X509_NAME_oneline(n, buf, sizeof(buf));
    return std::string(buf);
}

} // namespace

int main() {
    const fs::path scratch = make_scratch_dir();
    std::printf("scratch dir: %s\n", scratch.string().c_str());

    // Guarded cleanup: we'd like to keep the dir if a check fails so the
    // operator can poke at it, so only delete on the success path below.
    bool keep_scratch = true;

    int rc = [&]() -> int {
        const std::string DEV = "TESTDEV0001";

        CertFactoryConfig cfg;
        cfg.cache_dir     = scratch;
        cfg.validity_days = 30;        // shorter than default; still > now
        cfg.rsa_bits      = 2048;
        CertFactory factory(cfg);

        BRIDGE_REQUIRE(!factory.has_cached(DEV), "fresh scratch dir reports cached");

        // (1) First mint.
        CertMaterial a = factory.get_or_create(DEV);
        BRIDGE_REQUIRE(!a.cert_pem.empty(),       "cert_pem empty on first mint");
        BRIDGE_REQUIRE(!a.privkey_pem.empty(),    "privkey_pem empty on first mint");
        BRIDGE_REQUIRE(a.fingerprint_sha256.size() == 64,
                       "fingerprint isn't 64 hex chars");
        BRIDGE_REQUIRE(factory.has_cached(DEV),   "has_cached() false after mint");
        BRIDGE_REQUIRE(fs::exists(factory.cert_path(DEV)), "cert file not on disk");
        BRIDGE_REQUIRE(fs::exists(factory.key_path (DEV)), "key file not on disk");

        // (2) Second call must be a cache hit: byte-equal.
        CertMaterial b = factory.get_or_create(DEV);
        BRIDGE_REQUIRE(b.cert_pem            == a.cert_pem,
                       "cert_pem differs on second get_or_create()");
        BRIDGE_REQUIRE(b.privkey_pem         == a.privkey_pem,
                       "privkey_pem differs on second get_or_create()");
        BRIDGE_REQUIRE(b.fingerprint_sha256  == a.fingerprint_sha256,
                       "fingerprint differs on second get_or_create()");

        // (3) Parse cert and check subject CN, issuer CN.
        X509* cert = parse_pem(a.cert_pem);
        BRIDGE_REQUIRE(cert != nullptr, "PEM_read_bio_X509 returned NULL");

        const std::string subj  = name_oneline(X509_get_subject_name(cert));
        const std::string issuer = name_oneline(X509_get_issuer_name(cert));
        std::printf("subject:  %s\n", subj.c_str());
        std::printf("issuer:   %s\n", issuer.c_str());
        if (subj.find("CN=" + std::string("TESTDEV0001")) == std::string::npos) {
            std::fprintf(stderr, "FAIL: subject missing CN=TESTDEV0001 (got '%s')\n", subj.c_str());
            X509_free(cert);
            return 1;
        }
        if (issuer.find("CN=BBL CA") == std::string::npos) {
            std::fprintf(stderr, "FAIL: issuer missing CN=BBL CA (got '%s')\n", issuer.c_str());
            X509_free(cert);
            return 1;
        }

        // (4) notAfter > now.
        const ASN1_TIME* not_after = X509_get0_notAfter(cert);
        BRIDGE_REQUIRE(not_after != nullptr, "X509_get0_notAfter returned NULL");
        // ASN1_TIME_diff(*pday, *psec, from, to): from=now → to=notAfter.
        // Positive day/sec means notAfter is in the future.
        int days = 0, secs = 0;
        BRIDGE_REQUIRE(ASN1_TIME_diff(&days, &secs, nullptr, not_after) == 1,
                       "ASN1_TIME_diff failed");
        std::printf("notAfter in: %d days + %d secs\n", days, secs);
        if (days < 0 || (days == 0 && secs <= 0)) {
            std::fprintf(stderr, "FAIL: cert already expired (days=%d, secs=%d)\n", days, secs);
            X509_free(cert);
            return 1;
        }
        X509_free(cert);

        // (5) Key file permissions on POSIX.
#if !defined(_WIN32)
        struct stat st{};
        BRIDGE_REQUIRE(::stat(factory.key_path(DEV).c_str(), &st) == 0,
                       "stat() on key path failed");
        const mode_t perms = st.st_mode & 0777;
        std::printf("key mode: 0%o\n", static_cast<unsigned>(perms));
        if (perms != 0600) {
            std::fprintf(stderr, "FAIL: key mode is 0%o, expected 0600\n",
                         static_cast<unsigned>(perms));
            return 1;
        }
#endif

        // (6) regenerate() must yield a different fingerprint.
        CertMaterial c = factory.regenerate(DEV);
        BRIDGE_REQUIRE(c.fingerprint_sha256 != a.fingerprint_sha256,
                       "regenerate() reused old fingerprint");
        // ...and the cache now reflects the new material on subsequent reads.
        CertMaterial d = factory.get_or_create(DEV);
        BRIDGE_REQUIRE(d.fingerprint_sha256 == c.fingerprint_sha256,
                       "post-regenerate cache returns stale material");

        // (7) BridgeService ownership round-trip.
        Slic3r::bridge::BridgeService svc;
        BRIDGE_REQUIRE(svc.cert_factory() == nullptr,
                       "fresh BridgeService.cert_factory() not null");

        CertFactoryConfig svc_cfg;
        svc_cfg.cache_dir = scratch;
        svc.set_cert_factory(std::make_unique<CertFactory>(svc_cfg));
        BRIDGE_REQUIRE(svc.cert_factory() != nullptr,
                       "set_cert_factory did not stick");
        // Hit the cache through the service-owned factory.
        CertMaterial via_svc = svc.cert_factory()->get_or_create(DEV);
        BRIDGE_REQUIRE(via_svc.fingerprint_sha256 == c.fingerprint_sha256,
                       "service-owned factory missed the on-disk cache");
        svc.set_cert_factory(nullptr);
        BRIDGE_REQUIRE(svc.cert_factory() == nullptr,
                       "set_cert_factory(nullptr) did not detach");

        return 0;
    }();

    if (rc == 0) keep_scratch = false;

    if (!keep_scratch) {
        std::error_code ec;
        fs::remove_all(scratch, ec);
    } else {
        std::fprintf(stderr, "keeping scratch dir for inspection: %s\n",
                     scratch.string().c_str());
    }

    if (rc == 0) std::printf("CertFactoryTest: ok\n");
    return rc;
}
