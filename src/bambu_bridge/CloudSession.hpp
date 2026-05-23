// Bambu Bridge — native cloud session (ship-2).
//
// Replaces the proprietary plugin's "cached login" step for bridge boots:
// the bridge persists its own plaintext session file at
// ~/.config/BambuBridge/session.json (mode 0600) holding the user's
// accessToken / refreshToken / expiry timestamps / uid / region. On boot
// the bridge tries to load it; if the file exists and the access token
// is still valid (or can be refreshed via the cloud refreshtoken
// endpoint) the bridge uses it directly and skips
// app->request_user_handle(1).
//
// Why a separate plaintext file rather than reading the slicer's
// encrypted BambuNetworkEngine.conf? The encryption key has not been
// recovered — see RE-CLOUD-LOGIN.md §6. The slicer-side config stays
// untouched; the bridge maintains its own session store. First-time
// users with no bridge session still get bootstrapped via the plugin
// fallback path in BridgeBootstrap.cpp, which then writes the session
// file for next time.
//
// All HTTPS round-trips run synchronously on the caller's thread via a
// libcurl easy handle. The class itself is not thread-safe: hold the
// load() result by value and pass it around.

#ifndef SLIC3R_BAMBU_BRIDGE_CLOUD_SESSION_HPP
#define SLIC3R_BAMBU_BRIDGE_CLOUD_SESSION_HPP

#include <chrono>
#include <cstdint>
#include <string>

namespace Slic3r {
namespace bridge {

// Persistent cloud session data. Mirrors the fields stored in
// ~/.config/BambuBridge/session.json.
struct CloudSessionData {
    std::string access_token;
    std::string refresh_token;
    // Wall-clock seconds since epoch — when access_token / refresh_token
    // stop being accepted by the cloud. 0 = unknown / never refreshed.
    int64_t     access_expires_at  = 0;
    int64_t     refresh_expires_at = 0;
    int64_t     uid                = 0;
    std::string region;            // "us" | "cn"
    std::string user_email;        // optional, for display only

    // True iff we hold non-empty tokens. Does NOT check expiry.
    bool        has_tokens() const noexcept {
        return !access_token.empty() && !refresh_token.empty();
    }

    // True iff the access token has at least `slack_seconds` left before
    // its server-side expiry. With slack_seconds=60 the caller can
    // safely use this token without racing the cloud's "expired"
    // response.
    bool        access_valid(int slack_seconds = 60) const noexcept;

    // True iff the refresh token itself still has time on it. If false
    // we have to re-bootstrap (plugin fallback or interactive login).
    bool        refresh_valid(int slack_seconds = 60) const noexcept;
};

// File location helper. Returns the absolute path the session file
// would live at for this user. Does not create any directories. Honours
// XDG_CONFIG_HOME, falling back to $HOME/.config.
std::string session_file_path();

// Result of a session-bootstrap attempt.
struct CloudSessionLoadResult {
    bool             ok = false;               // tokens are usable
    bool             refreshed = false;        // had to refresh
    bool             from_file = false;        // started from disk
    CloudSessionData data;
    std::string      error;                    // empty on success
};

class CloudSession {
public:
    CloudSession() = default;

    // Read the session file. On success the access_token will be
    // refreshed if it's within `slack_seconds` of expiring; the refresh
    // result is persisted back to disk before returning.
    //
    // Returns ok=false if the file doesn't exist, can't be parsed,
    // contains no refresh_token, or the refresh call fails. The caller
    // is expected to fall back to the plugin-driven login in that case.
    CloudSessionLoadResult load_and_refresh_if_needed(int slack_seconds = 300);

    // Persist `data` to ~/.config/BambuBridge/session.json with mode
    // 0600. Creates the parent directory if missing. Returns true on
    // success, false on any IO error (error logged via boost::log).
    bool save(const CloudSessionData &data) const;

    // POST /v1/user-service/user/refreshtoken. Fills out a new
    // access_token / refresh_token / expiry triple. Returns ok=true on
    // HTTP 2xx with a valid JSON body. The returned data inherits
    // `region` and `uid` from `current` when the cloud doesn't echo
    // them.
    CloudSessionLoadResult refresh(const CloudSessionData &current);

    // Build a `CloudSessionData` from a plugin-driven login. Looks up
    // the agent's current accessToken/refreshToken via the proprietary
    // plugin's bambu_network_get_my_token + my_profile endpoints and
    // computes expiry from the now-time plus a conservative default
    // (the plugin doesn't expose `expiresIn` directly).
    //
    // `agent` must be a valid NetworkAgent* (the bridge's app->m_agent).
    // Returns ok=false if the agent isn't logged in or the token
    // exchange fails.
    static CloudSessionLoadResult from_plugin_agent(void *agent_ptr,
                                                    const std::string &region_hint = "us");
};

// Region resolution: read $BBL_BRIDGE_REGION first, then check
// ~/.config/BambuStudio/BambuStudio.conf for the "region" field; fall
// back to "us". Returns either "us" or "cn".
std::string region_for_bridge();

} // namespace bridge
} // namespace Slic3r

#endif // SLIC3R_BAMBU_BRIDGE_CLOUD_SESSION_HPP
