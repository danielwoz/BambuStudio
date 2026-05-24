// Ship 7 — implementation. See EncMsgEnvelope.hpp for spec and references.

#include "EncMsgEnvelope.hpp"

#include <nlohmann/json.hpp>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {
namespace bridge {

namespace {

// Base64 encode (no newlines) using OpenSSL.
std::string base64_encode(const unsigned char* in, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_write(b64, in, static_cast<int>(len));
    (void)BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string out(bptr->data, bptr->length);
    BIO_free_all(b64);
    return out;
}

// RSA-SHA256 sign with PKCS#1 v1.5 padding (which is what the plugin uses;
// PSS would give a variable-length sig but the captured sigs are all 256B
// exactly for a 2048-bit key — characteristic of PKCS#1 v1.5).
std::vector<unsigned char>
rsa_sha256_sign(const unsigned char* data, size_t data_len, EVP_PKEY* pkey) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestSignInit failed");
    }
    if (EVP_DigestSignUpdate(ctx, data, data_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestSignUpdate failed");
    }
    size_t siglen = 0;
    if (EVP_DigestSignFinal(ctx, nullptr, &siglen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestSignFinal (size) failed");
    }
    std::vector<unsigned char> sig(siglen);
    if (EVP_DigestSignFinal(ctx, sig.data(), &siglen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("EVP_DigestSignFinal failed");
    }
    sig.resize(siglen);
    EVP_MD_CTX_free(ctx);
    return sig;
}

EVP_PKEY* load_priv_key_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) throw std::runtime_error("BIO_new_mem_buf failed");
    EVP_PKEY* pk = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pk) throw std::runtime_error("PEM_read_bio_PrivateKey failed "
                                       "(invalid key PEM)");
    return pk;
}

}  // namespace

struct EncMsgEnvelope::Impl {
    EncMsgConfig cfg;
    EVP_PKEY* pkey = nullptr;

    ~Impl() {
        if (pkey) EVP_PKEY_free(pkey);
    }
};

EncMsgEnvelope::EncMsgEnvelope(EncMsgConfig cfg)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->cfg = std::move(cfg);
    if (m_impl->cfg.cert_id.empty())
        throw std::runtime_error("EncMsgEnvelope: cert_id empty");
    if (m_impl->cfg.app_private_key_pem.empty())
        throw std::runtime_error("EncMsgEnvelope: app_private_key_pem empty");
    // Validate the PEM at construction so callers see errors early.
    m_impl->pkey = load_priv_key_pem(m_impl->cfg.app_private_key_pem);
}

EncMsgEnvelope::~EncMsgEnvelope() = default;
EncMsgEnvelope::EncMsgEnvelope(EncMsgEnvelope&&) noexcept = default;
EncMsgEnvelope& EncMsgEnvelope::operator=(EncMsgEnvelope&&) noexcept = default;

bool EncMsgEnvelope::needs_envelope(const std::string& payload_json) {
    // Cheap test: payload is `{"print":` ...
    // We do a lightweight inspection rather than full JSON parse for the
    // hot path. If the payload is malformed JSON the caller will get an
    // error from wrap().
    auto first_nonspace = [&]() -> size_t {
        for (size_t i = 0; i < payload_json.size(); ++i) {
            char c = payload_json[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                return i;
        }
        return std::string::npos;
    };
    size_t i = first_nonspace();
    if (i == std::string::npos || payload_json[i] != '{') return false;
    // Skip whitespace inside the object
    ++i;
    while (i < payload_json.size() &&
           (payload_json[i] == ' ' || payload_json[i] == '\t' ||
            payload_json[i] == '\n' || payload_json[i] == '\r'))
        ++i;
    static const std::string kPrint = "\"print\"";
    return payload_json.compare(i, kPrint.size(), kPrint) == 0;
}

std::string
EncMsgEnvelope::wrap(const std::string& payload_json) const {
    using nlohmann::json;

    json plain;
    try {
        plain = json::parse(payload_json);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("EncMsgEnvelope::wrap: payload "
                                              "is not valid JSON: ") + ex.what());
    }
    if (!plain.is_object())
        throw std::runtime_error("EncMsgEnvelope::wrap: payload is not an "
                                  "object");
    if (!plain.contains("print"))
        throw std::runtime_error("EncMsgEnvelope::wrap: payload has no "
                                  "'print' key (only print.* is enveloped)");

    // The plugin's gate output recursively sorts keys. nlohmann::json
    // defaults to std::map for objects, which is sorted by key; round-
    // tripping through a default-constructed json object normalises key
    // order automatically.
    //
    // Build the canonical "to be signed" string:
    //   '{"print":' + compact_sorted(print_obj) + '}'
    // (matches Ship 7 Phase 1 verifier candidate "B" — verified
    // against 3 captured pairs.)
    const json print_obj = plain["print"];
    const std::string print_compact = print_obj.dump(
        /*indent=*/-1,
        /*indent_char=*/' ',
        /*ensure_ascii=*/false,
        /*error_handler=*/json::error_handler_t::strict);

    std::string to_sign;
    to_sign.reserve(10 + print_compact.size());
    to_sign.append("{\"print\":");
    to_sign.append(print_compact);
    to_sign.append("}");

    // Sign
    auto sig = rsa_sha256_sign(
        reinterpret_cast<const unsigned char*>(to_sign.data()),
        to_sign.size(),
        m_impl->pkey);
    const std::string sig_b64 = base64_encode(sig.data(), sig.size());

    // Build the envelope. nlohmann emits header fields sorted alphabetically
    // (cert_id < payload_len < sign_alg < sign_string < sign_ver) which
    // matches the captured plugin output exactly.
    json env;
    env["header"]["cert_id"]     = m_impl->cfg.cert_id;
    env["header"]["payload_len"] = static_cast<int64_t>(to_sign.size());
    env["header"]["sign_alg"]    = "RSA_SHA256";
    env["header"]["sign_string"] = sig_b64;
    env["header"]["sign_ver"]    = "v1.0";
    env["print"] = print_obj;

    return env.dump();
}

}  // namespace bridge
}  // namespace Slic3r
