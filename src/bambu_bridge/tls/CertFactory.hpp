// Bambu Bridge — per-device TLS cert factory (phase 2).
//
// Each cloud-bound printer gets one virtual-printer cert. To convince
// downstream slicers (BambuStudio + OrcaSlicer forks) that our virtual
// printer is "real", the cert must match the shape of a real Bambu
// printer's self-signed cert:
//
//   - CN  = the printer's serial (`dev_id`, e.g. "0938BC582502312")
//   - O   = (optional, unused in v1 — left empty)
//   - Issuer CN = "BBL CA"   ← string-matched by BambuStudio's TLS verifier
//   - Signature: RSA-SHA256, 2048-bit key
//   - Validity: ~10 years from issue
//
// Certs are minted once per dev_id and cached on disk so process
// restarts keep the same fingerprint. The cache lives under
// `$XDG_CONFIG_HOME/BambuStudio/bridge/certs/` by default; the private
// key is written with POSIX mode 0600.
//
// This module is intentionally OpenSSL-only — no Boost, no wxWidgets —
// so it builds in the standalone configure path.

#ifndef SLIC3R_BAMBU_BRIDGE_TLS_CERT_FACTORY_HPP
#define SLIC3R_BAMBU_BRIDGE_TLS_CERT_FACTORY_HPP

#include <filesystem>
#include <string>

namespace Slic3r {
namespace bridge {
namespace tls {

struct CertMaterial {
    // X509 certificate, PEM-encoded.
    std::string cert_pem;
    // Private key in PKCS#8 PEM form (unencrypted; the cache file itself
    // is the only access control — written mode 0600 on POSIX).
    std::string privkey_pem;
    // Hex-encoded SHA-256 of the DER cert, lowercase, no separators.
    std::string fingerprint_sha256;
};

struct CertFactoryConfig {
    // Where to cache `<dev_id>.crt` and `<dev_id>.key`. If left at the
    // default, resolves to `$XDG_CONFIG_HOME/BambuStudio/bridge/certs`
    // (or `~/.config/BambuStudio/bridge/certs` if XDG_CONFIG_HOME unset)
    // on first use.
    std::filesystem::path cache_dir;
    // Issuer CN — must match what BambuStudio's TLS verifier accepts.
    std::string issuer_cn = "BBL CA";
    // Real printers use 2048-bit RSA.
    int rsa_bits = 2048;
    // ~10 years; real printer certs are minted ~10y out from build.
    int validity_days = 3650;
};

class CertFactory {
public:
    explicit CertFactory(CertFactoryConfig cfg);
    ~CertFactory();

    CertFactory(const CertFactory&) = delete;
    CertFactory& operator=(const CertFactory&) = delete;

    // Idempotent: reads cache if present, otherwise mints + saves.
    // Throws std::runtime_error on hard failure (OpenSSL key-gen
    // failure, unwritable cache dir, malformed cached PEM, etc.).
    CertMaterial get_or_create(const std::string& dev_id);

    // Always mints fresh material and overwrites the cache (used when
    // a serial gets reassigned to a new physical printer).
    CertMaterial regenerate(const std::string& dev_id);

    // True iff both the .crt and .key files exist on disk for dev_id.
    bool has_cached(const std::string& dev_id) const;

    std::filesystem::path cert_path(const std::string& dev_id) const;
    std::filesystem::path key_path (const std::string& dev_id) const;

    const CertFactoryConfig& config() const { return m_cfg; }

private:
    // Resolves m_cfg.cache_dir lazily — handles XDG default + auto-
    // creates the dir with mode 0700 on first use.
    const std::filesystem::path& resolved_cache_dir() const;

    // Loads cached material from disk; returns true if both files
    // existed and parsed cleanly.
    bool load_cached(const std::string& dev_id, CertMaterial& out) const;

    // Mints fresh material and atomically writes both files (key first,
    // mode 0600). Throws on OpenSSL or IO failure.
    CertMaterial mint_and_save(const std::string& dev_id) const;

    mutable CertFactoryConfig     m_cfg;
    mutable std::filesystem::path m_resolved_cache_dir;
    mutable bool                  m_cache_dir_ready = false;
};

} // namespace tls
} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_TLS_CERT_FACTORY_HPP
