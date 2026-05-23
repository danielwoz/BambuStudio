// Ship 7 — EncMsgEnvelope unit test.
//
// Validates the native re-implementation of the plugin's
// apply_enc_msg_gate signing path against the structure of captured
// envelopes from /mnt/cephfs/ssd/BambuBridge/ship-6-dr-capture.log.
//
// What this test asserts:
//   1. needs_envelope() correctly classifies print.* vs other payloads.
//   2. wrap() throws on bad input.
//   3. wrap() with a TEST RSA key produces an envelope whose structure
//      byte-matches the captured envelopes for everything EXCEPT the
//      sign_string (which depends on the real slicer app_private_key).
//   4. The sign_string we produce is a valid RSA-SHA256 signature over
//      the canonical bytes — verifiable with the test cert's public key.
//   5. The bytes-being-signed format matches Phase 1's verified formula:
//          '{"print":' + json_compact_sorted(print_obj) + '}'
//
// What this test does NOT do:
//   - Byte-match against the original captured sign_string. That requires
//     the slicer's actual app_private_key (Ship 7 Phase 2 — not yet
//     extracted; see SHIP-7-NATIVE-ENC-MSG.md §"Phase 2 status").
//   - End-to-end test against a real printer (Ship 7 Phase 5).

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include "bambu_bridge/EncMsgEnvelope.hpp"

#include <nlohmann/json.hpp>

using nlohmann::json;
using Slic3r::bridge::EncMsgConfig;
using Slic3r::bridge::EncMsgEnvelope;

// ---- minimal test RSA-2048 key generator -------------------------------

static std::string gen_test_rsa_pem() {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!ctx) { std::fprintf(stderr, "ctx_new failed\n"); std::abort(); }
    if (EVP_PKEY_keygen_init(ctx) <= 0) std::abort();
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0) std::abort();
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) std::abort();
    EVP_PKEY_CTX_free(ctx);

    BIO* mem = BIO_new(BIO_s_mem());
    if (PEM_write_bio_PrivateKey(mem, pkey, nullptr, nullptr, 0, nullptr,
                                  nullptr) != 1) {
        std::abort();
    }
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(mem, &bptr);
    std::string pem(bptr->data, bptr->length);
    BIO_free(mem);
    EVP_PKEY_free(pkey);
    return pem;
}

static std::vector<unsigned char> base64_decode(const std::string& s) {
    BIO* mem = BIO_new_mem_buf(s.data(), static_cast<int>(s.size()));
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    mem = BIO_push(b64, mem);
    std::vector<unsigned char> out(s.size());
    int n = BIO_read(mem, out.data(), static_cast<int>(out.size()));
    BIO_free_all(mem);
    if (n < 0) n = 0;
    out.resize(n);
    return out;
}

static EVP_PKEY* load_pem_priv(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    EVP_PKEY* pk = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return pk;
}

static bool rsa_sha256_verify(EVP_PKEY* pkey, const std::string& data,
                              const std::vector<unsigned char>& sig) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return false;
    bool ok = false;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1
        && EVP_DigestVerifyUpdate(ctx, data.data(), data.size()) == 1
        && EVP_DigestVerifyFinal(ctx, sig.data(), sig.size()) == 1) {
        ok = true;
    }
    EVP_MD_CTX_free(ctx);
    return ok;
}

// ---- test cases --------------------------------------------------------

static void test_needs_envelope() {
    using N = EncMsgEnvelope;
    assert(N::needs_envelope(R"({"print":{"command":"foo"}})"));
    assert(N::needs_envelope("  \n {\n\"print\":{} }"));
    assert(!N::needs_envelope(R"({"pushing":{"command":"pushall"}})"));
    assert(!N::needs_envelope(R"({"info":{"command":"get_version"}})"));
    assert(!N::needs_envelope(R"({"system":{"command":"get_access_code"}})"));
    assert(!N::needs_envelope(""));
    assert(!N::needs_envelope("not json"));
    assert(!N::needs_envelope("[\"print\"]"));
    std::cout << "  PASS test_needs_envelope\n";
}

static void test_throws_on_bad_input() {
    EncMsgConfig cfg{"id1", gen_test_rsa_pem()};
    EncMsgEnvelope env(std::move(cfg));
    bool threw = false;
    try { env.wrap("not json"); } catch (const std::exception&) { threw = true; }
    assert(threw);
    threw = false;
    try { env.wrap(R"({"pushing":{}})"); } catch (const std::exception&) { threw = true; }
    assert(threw);
    std::cout << "  PASS test_throws_on_bad_input\n";
}

static void test_throws_on_missing_key_or_id() {
    bool threw = false;
    try { EncMsgEnvelope e(EncMsgConfig{"", "blob"}); }
    catch (const std::exception&) { threw = true; }
    assert(threw);
    threw = false;
    try { EncMsgEnvelope e(EncMsgConfig{"id", ""}); }
    catch (const std::exception&) { threw = true; }
    assert(threw);
    threw = false;
    try { EncMsgEnvelope e(EncMsgConfig{"id", "garbage"}); }
    catch (const std::exception&) { threw = true; }
    assert(threw);
    std::cout << "  PASS test_throws_on_missing_key_or_id\n";
}

static void test_wrap_structure_matches_capture_format() {
    // Input from DR capture GATE-23 (ams_filament_setting purple)
    const std::string input = R"({"print":{"command":"ams_filament_setting","sequence_id":"1779533014","ams_id":255,"slot_id":0,"tray_id":254,"tray_info_idx":"GFL96","setting_id":"PFB67E5CD6E2C7C7B5","tray_color":"FF00FFAA","nozzle_temp_min":190,"nozzle_temp_max":240,"tray_type":"PLA"})"
        "}";  // 254B

    const std::string priv_pem = gen_test_rsa_pem();
    EncMsgConfig cfg{
        "a4e8faaa1a38e3650a0ea590d192383fCN=GLOF3813734089.bambulab.com",
        priv_pem,
    };
    EncMsgEnvelope env(std::move(cfg));
    std::string out = env.wrap(input);

    // Parse the output and inspect
    json env_json = json::parse(out);
    assert(env_json.contains("header"));
    assert(env_json.contains("print"));

    const auto& hdr = env_json["header"];
    assert(hdr["cert_id"] == "a4e8faaa1a38e3650a0ea590d192383fCN=GLOF3813734089.bambulab.com");
    assert(hdr["sign_alg"] == "RSA_SHA256");
    assert(hdr["sign_ver"] == "v1.0");
    assert(hdr["payload_len"].get<int64_t>() == 254);
    assert(hdr["sign_string"].get<std::string>().size() > 300);  // ~344 base64 chars for 256B RSA sig

    // The print sub-object should have all input keys, sorted alphabetically
    auto print_obj = env_json["print"];
    assert(print_obj["command"] == "ams_filament_setting");
    assert(print_obj["sequence_id"] == "1779533014");
    assert(print_obj["tray_color"] == "FF00FFAA");

    // The serialised bytes of print_obj's compact dump should match what
    // the captured envelope has (verified via Phase 1).
    std::string print_compact = print_obj.dump(-1);
    const std::string expected_print_compact =
        R"({"ams_id":255,"command":"ams_filament_setting","nozzle_temp_max":240,"nozzle_temp_min":190,"sequence_id":"1779533014","setting_id":"PFB67E5CD6E2C7C7B5","slot_id":0,"tray_color":"FF00FFAA","tray_id":254,"tray_info_idx":"GFL96","tray_type":"PLA"})";
    if (print_compact != expected_print_compact) {
        std::fprintf(stderr, "  expected: %s\n", expected_print_compact.c_str());
        std::fprintf(stderr, "  actual:   %s\n", print_compact.c_str());
    }
    assert(print_compact == expected_print_compact);

    // Header key order in serialised form should be cert_id, payload_len,
    // sign_alg, sign_string, sign_ver — same as captured.
    // Find header substring and verify the order
    size_t hpos = out.find("\"header\":{");
    assert(hpos != std::string::npos);
    size_t pos_cert = out.find("\"cert_id\"", hpos);
    size_t pos_plen = out.find("\"payload_len\"", hpos);
    size_t pos_salg = out.find("\"sign_alg\"", hpos);
    size_t pos_sstr = out.find("\"sign_string\"", hpos);
    size_t pos_sver = out.find("\"sign_ver\"", hpos);
    assert(pos_cert < pos_plen);
    assert(pos_plen < pos_salg);
    assert(pos_salg < pos_sstr);
    assert(pos_sstr < pos_sver);

    std::cout << "  PASS test_wrap_structure_matches_capture_format\n";
}

static void test_signature_verifies_with_test_pubkey() {
    const std::string input = R"({"print":{"command":"ams_filament_setting","sequence_id":"1779533014","ams_id":255,"slot_id":0,"tray_id":254,"tray_info_idx":"GFL96","setting_id":"PFB67E5CD6E2C7C7B5","tray_color":"FF00FFAA","nozzle_temp_min":190,"nozzle_temp_max":240,"tray_type":"PLA"}})";

    const std::string priv_pem = gen_test_rsa_pem();
    EncMsgConfig cfg{
        "test-cert-id",
        priv_pem,
    };
    EncMsgEnvelope env(std::move(cfg));
    std::string out = env.wrap(input);
    json env_json = json::parse(out);

    // The bytes that should have been signed
    json print_obj = env_json["print"];
    std::string print_compact = print_obj.dump(-1);
    std::string to_sign = "{\"print\":" + print_compact + "}";

    // Verify with the test key's public counterpart
    EVP_PKEY* pkey = load_pem_priv(priv_pem);
    assert(pkey);
    std::vector<unsigned char> sig = base64_decode(env_json["header"]["sign_string"]);
    bool ok = rsa_sha256_verify(pkey, to_sign, sig);
    EVP_PKEY_free(pkey);
    assert(ok && "signature did not verify against our own key — wrap() is signing the wrong bytes");

    // Also check the payload_len matches strlen(to_sign)
    assert(env_json["header"]["payload_len"].get<int64_t>() ==
           static_cast<int64_t>(to_sign.size()));

    std::cout << "  PASS test_signature_verifies_with_test_pubkey\n";
}

static void test_deterministic_for_same_input() {
    const std::string input = R"({"print":{"command":"ams_filament_setting","sequence_id":"1","tray_color":"FF00FFAA"}})";
    const std::string priv_pem = gen_test_rsa_pem();
    EncMsgConfig cfg{"cert", priv_pem};
    EncMsgEnvelope env(std::move(cfg));
    std::string out1 = env.wrap(input);
    std::string out2 = env.wrap(input);
    // RSA-PKCS1-v1.5 is deterministic for the same key+input. The plugin's
    // captures showed identical sign_string across GATE-23 and GATE-24
    // (same payload, same key) — confirming PKCS#1 v1.5 not PSS.
    assert(out1 == out2 && "RSA-SHA256 sign should be deterministic with PKCS#1 v1.5");
    std::cout << "  PASS test_deterministic_for_same_input\n";
}

int main() {
    std::cout << "EncMsgEnvelopeTest\n";
    test_needs_envelope();
    test_throws_on_bad_input();
    test_throws_on_missing_key_or_id();
    test_wrap_structure_matches_capture_format();
    test_signature_verifies_with_test_pubkey();
    test_deterministic_for_same_input();
    std::cout << "ALL OK\n";
    return 0;
}
