// Bambu Bridge — per-device TLS cert factory (phase 2).
//
// See CertFactory.hpp for the contract. This file is intentionally
// dependency-light: OpenSSL + std::filesystem + POSIX `open(2)` for the
// mode-0600 dance, nothing else. No Boost, no wxWidgets.
//
// Real-printer cert shape we are matching (observed against an H2S +
// an A1 mini):
//
//   Certificate:
//       Data:
//           Version: 3 (0x2)
//           Serial Number: <8 random bytes>
//           Signature Algorithm: sha256WithRSAEncryption
//           Issuer: CN = BBL CA
//           Validity: ~10 years
//           Subject: CN = <printer serial>
//           Public Key Algorithm: rsaEncryption (2048 bit)
//
// We intentionally do NOT set basicConstraints / keyUsage extensions:
// real Bambu printer certs don't carry them either, and BambuStudio's
// verifier accepts a bare CN-only profile. Adding them would diverge
// from real-printer fingerprints in ways downstream verifiers might
// notice.

#include "CertFactory.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#  include <io.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#else
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace Slic3r {
namespace bridge {
namespace tls {

namespace {

// --- OpenSSL RAII --------------------------------------------------------

struct EvpPkeyDeleter { void operator()(EVP_PKEY* p) const noexcept { if (p) EVP_PKEY_free(p); } };
struct X509Deleter    { void operator()(X509*     p) const noexcept { if (p) X509_free(p);     } };
struct BnDeleter      { void operator()(BIGNUM*   p) const noexcept { if (p) BN_free(p);       } };
struct AsnIntDeleter  { void operator()(ASN1_INTEGER* p) const noexcept { if (p) ASN1_INTEGER_free(p); } };
struct BioDeleter     { void operator()(BIO*      p) const noexcept { if (p) BIO_free_all(p);  } };
struct EvpPkeyCtxDeleter { void operator()(EVP_PKEY_CTX* p) const noexcept { if (p) EVP_PKEY_CTX_free(p); } };

using EvpPkeyPtr    = std::unique_ptr<EVP_PKEY,    EvpPkeyDeleter>;
using X509Ptr       = std::unique_ptr<X509,        X509Deleter>;
using BnPtr         = std::unique_ptr<BIGNUM,      BnDeleter>;
using AsnIntPtr     = std::unique_ptr<ASN1_INTEGER, AsnIntDeleter>;
using BioPtr        = std::unique_ptr<BIO,         BioDeleter>;
using EvpPkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDeleter>;

[[noreturn]] void throw_openssl(const char* what) {
    unsigned long e = ERR_get_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    std::ostringstream oss;
    oss << "CertFactory: OpenSSL " << what;
    if (buf[0]) oss << ": " << buf;
    throw std::runtime_error(oss.str());
}

// --- Helpers -------------------------------------------------------------

bool is_safe_dev_id(const std::string& s) {
    // Real Bambu serials are alnum-only; lock the path component down so
    // a malicious dev_id can't traverse out of the cache dir.
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9')
                     || (c >= 'A' && c <= 'Z')
                     || (c >= 'a' && c <= 'z')
                     || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::filesystem::path default_cache_dir() {
    // $XDG_CONFIG_HOME/BambuStudio/bridge/certs (per spec).
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "BambuStudio" / "bridge" / "certs";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "BambuStudio" / "bridge" / "certs";
    }
    // Last-resort fallback so we never hand back an empty path.
    return std::filesystem::temp_directory_path() / "BambuStudio" / "bridge" / "certs";
}

void ensure_dir_700(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        throw std::runtime_error("CertFactory: cannot create cache dir '" +
                                 dir.string() + "': " + ec.message());
    }
#if !defined(_WIN32)
    // Best-effort tighten perms; if chmod fails we keep going (the file-
    // level 0600 is the actual access control).
    ::chmod(dir.c_str(), 0700);
#endif
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void write_file_text(const std::filesystem::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw std::runtime_error("CertFactory: cannot open '" + p.string() + "' for write");
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!f) {
        throw std::runtime_error("CertFactory: short write to '" + p.string() + "'");
    }
}

// POSIX-safe atomic-ish writer with mode 0600 baked in. On Windows we
// fall back to the plain ofstream path (the spec excuses us from the
// mode check there).
void write_file_secret(const std::filesystem::path& p, const std::string& data) {
#if defined(_WIN32)
    write_file_text(p, data);
#else
    // open(O_CREAT|O_WRONLY|O_TRUNC, 0600) — the bullet-proof path per
    // the phase-2 spec. Unlink first so a stale file with looser perms
    // can't survive (open with O_TRUNC keeps the old mode).
    ::unlink(p.c_str());
    int fd = ::open(p.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw std::runtime_error("CertFactory: open(" + p.string() + ", 0600) failed: " +
                                 std::strerror(errno));
    }
    const char*  buf = data.data();
    size_t       remaining = data.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, buf, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            throw std::runtime_error("CertFactory: write(" + p.string() + ") failed: " +
                                     std::strerror(e));
        }
        buf       += n;
        remaining -= static_cast<size_t>(n);
    }
    // Defensive: re-chmod in case umask widened it (open(2) honors umask).
    ::fchmod(fd, 0600);
    ::close(fd);
#endif
}

std::string hex_lower(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i    ] = kHex[(data[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[ data[i]       & 0xF];
    }
    return out;
}

std::string compute_fingerprint(X509* cert) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  md_len = 0;
    if (X509_digest(cert, EVP_sha256(), md, &md_len) != 1 || md_len == 0) {
        throw_openssl("X509_digest");
    }
    return hex_lower(md, md_len);
}

// Parse a PEM cert blob → recompute fingerprint. Used when loading the
// cache so callers don't have to re-hash through OpenSSL themselves.
std::string fingerprint_of_pem(const std::string& cert_pem) {
    BioPtr bio(BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size())));
    if (!bio) throw_openssl("BIO_new_mem_buf");
    X509Ptr cert(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
    if (!cert) throw_openssl("PEM_read_bio_X509 (cached)");
    return compute_fingerprint(cert.get());
}

EvpPkeyPtr generate_rsa_key(int bits) {
    EvpPkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    if (!ctx) throw_openssl("EVP_PKEY_CTX_new_id");
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0) throw_openssl("EVP_PKEY_keygen_init");
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), bits) <= 0) {
        throw_openssl("EVP_PKEY_CTX_set_rsa_keygen_bits");
    }
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) throw_openssl("EVP_PKEY_keygen");
    return EvpPkeyPtr(raw);
}

X509Ptr build_cert(const std::string& dev_id,
                   const std::string& issuer_cn,
                   int validity_days,
                   EVP_PKEY* pkey) {
    X509Ptr cert(X509_new());
    if (!cert) throw_openssl("X509_new");

    // v3.
    if (X509_set_version(cert.get(), 2) != 1) throw_openssl("X509_set_version");

    // 8 random bytes → ASN1_INTEGER serial.
    {
        BnPtr bn(BN_new());
        if (!bn) throw_openssl("BN_new");
        if (BN_rand(bn.get(), 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1) {
            throw_openssl("BN_rand");
        }
        AsnIntPtr serial(BN_to_ASN1_INTEGER(bn.get(), nullptr));
        if (!serial) throw_openssl("BN_to_ASN1_INTEGER");
        if (X509_set_serialNumber(cert.get(), serial.get()) != 1) {
            throw_openssl("X509_set_serialNumber");
        }
    }

    // Validity.
    if (!X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0)) {
        throw_openssl("X509_gmtime_adj notBefore");
    }
    if (!X509_gmtime_adj(X509_getm_notAfter(cert.get()),
                         static_cast<long>(validity_days) * 86400L)) {
        throw_openssl("X509_gmtime_adj notAfter");
    }

    // Public key.
    if (X509_set_pubkey(cert.get(), pkey) != 1) throw_openssl("X509_set_pubkey");

    // Subject: CN = <dev_id>.
    X509_NAME* subj = X509_get_subject_name(cert.get());
    if (X509_NAME_add_entry_by_txt(
            subj, "CN", MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(dev_id.c_str()),
            -1, -1, 0) != 1) {
        throw_openssl("X509_NAME_add_entry_by_txt(subject CN)");
    }

    // Issuer: CN = "BBL CA". Real Bambu printers are self-signed but
    // present this as the issuer DN — so a string match against
    // CN=BBL CA passes the slicer's verifier.
    X509_NAME* issuer = X509_NAME_new();
    if (!issuer) throw_openssl("X509_NAME_new(issuer)");
    if (X509_NAME_add_entry_by_txt(
            issuer, "CN", MBSTRING_UTF8,
            reinterpret_cast<const unsigned char*>(issuer_cn.c_str()),
            -1, -1, 0) != 1) {
        X509_NAME_free(issuer);
        throw_openssl("X509_NAME_add_entry_by_txt(issuer CN)");
    }
    if (X509_set_issuer_name(cert.get(), issuer) != 1) {
        X509_NAME_free(issuer);
        throw_openssl("X509_set_issuer_name");
    }
    X509_NAME_free(issuer);

    // Self-sign with SHA-256.
    if (X509_sign(cert.get(), pkey, EVP_sha256()) == 0) {
        throw_openssl("X509_sign");
    }

    return cert;
}

std::string pem_encode_cert(X509* cert) {
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) throw_openssl("BIO_new(mem)");
    if (PEM_write_bio_X509(bio.get(), cert) != 1) throw_openssl("PEM_write_bio_X509");
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio.get(), &mem);
    return std::string(mem->data, mem->length);
}

std::string pem_encode_pkcs8_key(EVP_PKEY* pkey) {
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (!bio) throw_openssl("BIO_new(mem)");
    // NULL cipher → unencrypted PKCS#8.
    if (PEM_write_bio_PKCS8PrivateKey(bio.get(), pkey,
                                      /*enc=*/nullptr,
                                      /*kstr=*/nullptr, /*klen=*/0,
                                      /*cb=*/nullptr,   /*u=*/nullptr) != 1) {
        throw_openssl("PEM_write_bio_PKCS8PrivateKey");
    }
    BUF_MEM* mem = nullptr;
    BIO_get_mem_ptr(bio.get(), &mem);
    return std::string(mem->data, mem->length);
}

} // namespace

// --- CertFactory ---------------------------------------------------------

CertFactory::CertFactory(CertFactoryConfig cfg)
    : m_cfg(std::move(cfg)) {}

CertFactory::~CertFactory() = default;

const std::filesystem::path& CertFactory::resolved_cache_dir() const {
    if (!m_cache_dir_ready) {
        m_resolved_cache_dir = m_cfg.cache_dir.empty()
                                ? default_cache_dir()
                                : m_cfg.cache_dir;
        ensure_dir_700(m_resolved_cache_dir);
        m_cache_dir_ready = true;
    }
    return m_resolved_cache_dir;
}

std::filesystem::path CertFactory::cert_path(const std::string& dev_id) const {
    if (!is_safe_dev_id(dev_id)) {
        throw std::runtime_error("CertFactory: unsafe dev_id '" + dev_id + "'");
    }
    return resolved_cache_dir() / (dev_id + ".crt");
}

std::filesystem::path CertFactory::key_path(const std::string& dev_id) const {
    if (!is_safe_dev_id(dev_id)) {
        throw std::runtime_error("CertFactory: unsafe dev_id '" + dev_id + "'");
    }
    return resolved_cache_dir() / (dev_id + ".key");
}

bool CertFactory::has_cached(const std::string& dev_id) const {
    if (!is_safe_dev_id(dev_id)) return false;
    std::error_code ec;
    const auto cp = resolved_cache_dir() / (dev_id + ".crt");
    const auto kp = resolved_cache_dir() / (dev_id + ".key");
    return std::filesystem::exists(cp, ec) && std::filesystem::exists(kp, ec);
}

bool CertFactory::load_cached(const std::string& dev_id, CertMaterial& out) const {
    const auto cp = cert_path(dev_id);
    const auto kp = key_path (dev_id);
    std::string cert_pem = read_file(cp);
    std::string key_pem  = read_file(kp);
    if (cert_pem.empty() || key_pem.empty()) return false;

    // Validate by re-parsing + hashing. A corrupt cache file should
    // look like a cache miss to the caller (the mint path will
    // overwrite it), but we treat a malformed PEM as a hard error so
    // the operator gets an actionable failure instead of silently
    // regenerating credentials behind their back.
    out.cert_pem    = std::move(cert_pem);
    out.privkey_pem = std::move(key_pem);
    out.fingerprint_sha256 = fingerprint_of_pem(out.cert_pem);
    return true;
}

CertMaterial CertFactory::mint_and_save(const std::string& dev_id) const {
    (void) resolved_cache_dir(); // ensures it exists with 0700

    EvpPkeyPtr pkey = generate_rsa_key(m_cfg.rsa_bits);
    X509Ptr    cert = build_cert(dev_id, m_cfg.issuer_cn, m_cfg.validity_days, pkey.get());

    CertMaterial out;
    out.cert_pem    = pem_encode_cert(cert.get());
    out.privkey_pem = pem_encode_pkcs8_key(pkey.get());
    out.fingerprint_sha256 = compute_fingerprint(cert.get());

    // Key first, mode 0600 — then cert. Order matters: if the cert
    // exists but the key is missing/looser, has_cached() would still
    // say "yes" and we'd hand out a half-valid pair.
    write_file_secret(key_path (dev_id), out.privkey_pem);
    write_file_text  (cert_path(dev_id), out.cert_pem);

    return out;
}

CertMaterial CertFactory::get_or_create(const std::string& dev_id) {
    if (!is_safe_dev_id(dev_id)) {
        throw std::runtime_error("CertFactory: unsafe dev_id '" + dev_id + "'");
    }
    CertMaterial cached;
    if (has_cached(dev_id) && load_cached(dev_id, cached)) {
        return cached;
    }
    return mint_and_save(dev_id);
}

CertMaterial CertFactory::regenerate(const std::string& dev_id) {
    if (!is_safe_dev_id(dev_id)) {
        throw std::runtime_error("CertFactory: unsafe dev_id '" + dev_id + "'");
    }
    // Spec calls for a forced fresh mint — explicitly drop the old
    // pair before we re-mint so a partial failure mid-mint leaves
    // has_cached() correctly false.
    std::error_code ec;
    std::filesystem::remove(cert_path(dev_id), ec);
    std::filesystem::remove(key_path (dev_id), ec);
    return mint_and_save(dev_id);
}

} // namespace tls
} // namespace bridge
} // namespace Slic3r
