// Ship 7 — Native implementation of the proprietary plugin's
// `apply_enc_msg_gate` envelope wrapping for print.* control commands.
//
// The Bambu plugin signs every `print.command=*` payload with the slicer's
// installed app_private_key (RSA-SHA256). The printer firmware verifies
// the signature against the slicer's `app_cert` (which was previously
// installed via the `security.app_cert_install` cycle) and accepts the
// command iff:
//   1. `header.sign_alg`     == "RSA_SHA256"
//   2. `header.sign_ver`     == "v1.0"
//   3. `header.cert_id`      identifies an installed slicer cert
//   4. `header.payload_len`  equals strlen({"print":<sorted_print_obj>})
//   5. `header.sign_string`  RSA-verifies against the same bytes using
//                            that cert's public key.
//
// Wire format (Ship 6 DR capture confirmed, n=86 envelopes):
//
//   {
//     "header": {
//       "cert_id":     "<32-hex-cert-serial><CN-of-issuer-cert>",
//       "payload_len": <int — bytes of the signed JSON>,
//       "sign_alg":    "RSA_SHA256",
//       "sign_string": "<base64 RSA-PKCS1v1.5 signature>",
//       "sign_ver":    "v1.0"
//     },
//     "print": <print-object with keys sorted alphabetically>
//   }
//
// Bytes-being-signed (verified Ship 7 Phase 1 against 3 distinct capture
// pairs — see /tmp/ship7/verify_sig.py results "B" and "C"):
//
//   '{"print":' + json_compact_sorted(envelope.print) + '}'
//
// where envelope.print is the input plain.print with keys re-sorted
// alphabetically (recursive) and serialized with compact separators
// (`,` and `:`) and no whitespace.
//
// For `gcode_line` payloads the plugin additionally replaces `param` with
// `param_enc` (RSA-encrypted with the printer's device pubkey) BEFORE
// signing. That transformation is NOT yet implemented in this class —
// `param`/`param_enc` is passed through as-is.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Slic3r {
namespace bridge {

struct EncMsgConfig {
    // Composite `header.cert_id` value:
    //   <32-hex of slicer cert serial> + "CN=" + issuer-CN
    // E.g. "a4e8faaa1a38e3650a0ea590d192383fCN=GLOF3813734089.bambulab.com"
    std::string cert_id;

    // Slicer's app_private_key in PEM (PKCS#1 or PKCS#8). RSA-2048.
    std::string app_private_key_pem;
};

class EncMsgEnvelope {
public:
    explicit EncMsgEnvelope(EncMsgConfig cfg);
    ~EncMsgEnvelope();
    EncMsgEnvelope(const EncMsgEnvelope&) = delete;
    EncMsgEnvelope& operator=(const EncMsgEnvelope&) = delete;
    EncMsgEnvelope(EncMsgEnvelope&&) noexcept;
    EncMsgEnvelope& operator=(EncMsgEnvelope&&) noexcept;

    // Wrap a `{"print":{...}}` JSON payload in an enc_msg envelope.
    // Returns the JSON envelope string ready to publish on
    //   device/<sn>/request
    // Throws std::runtime_error on malformed input or sign failure.
    std::string wrap(const std::string& payload_json) const;

    // Returns true iff the payload's top-level key is one of the families
    // the plugin envelopes ("print", "system" — TBD). For now, just
    // "print", matching what DR captured.
    static bool needs_envelope(const std::string& payload_json);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace bridge
}  // namespace Slic3r
