// ue-mcp-for-all-versions
// RemoteControl HTTP client: talks to Unreal Engine's built-in WebRemoteControl
// server. Knows nothing about a specific engine version at compile time; all
// version differences are discovered at runtime via /remote/info probing.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace ue_mcp_for_all_versions {

using json = nlohmann::json;

// Outcome of a RemoteControl HTTP request. Never throws across the boundary;
// callers branch on `ok`.
struct RcResult {
    bool ok = false;        // transport + HTTP 2xx succeeded
    int status = 0;         // HTTP status code (0 = no connection)
    json body;              // parsed JSON body (null if none / unparseable)
    std::string error;      // human-readable error when !ok

    static RcResult fail(std::string msg, int status = 0) {
        RcResult r;
        r.ok = false;
        r.status = status;
        r.error = std::move(msg);
        return r;
    }
};

// Outcome of a request whose response body is binary (e.g. an image), not JSON.
// Used by routes like /remote/object/thumbnail that return raw PNG bytes rather
// than a JSON document. `data` holds the undecoded bytes; `content_type` is the
// response's Content-Type header (e.g. "image/png").
struct RcBinaryResult {
    bool ok = false;
    int status = 0;
    std::string data;          // raw response bytes (may be binary)
    std::string content_type;  // Content-Type header, if any
    std::string error;         // human-readable error when !ok

    static RcBinaryResult fail(std::string msg, int status = 0) {
        RcBinaryResult r;
        r.ok = false;
        r.status = status;
        r.error = std::move(msg);
        return r;
    }
};

// Connection config. Host is always loopback in practice; we never bind or
// reach a non-localhost address.
struct RcConfig {
    std::string host = "127.0.0.1";
    // Candidate ports tried in order. 30010 = UE 4.26+; 8080 = UE 4.25.
    std::vector<int> ports = {30010, 8080};
    std::chrono::milliseconds connect_timeout{1500};
    std::chrono::milliseconds read_timeout{15000};
    // Minimum gap between reconnect probes while disconnected (avoids paying a
    // full multi-port timeout on every tool call when the engine is down).
    std::chrono::milliseconds reconnect_throttle{2000};
};

class RcClient {
public:
    explicit RcClient(RcConfig config = {});
    ~RcClient();

    RcClient(const RcClient&) = delete;
    RcClient& operator=(const RcClient&) = delete;

    // Probe the candidate ports; on success records the live port and returns
    // true. Tries GET /remote/info first (4.26+), then falls back to a probe
    // that works on 4.25 (which lacks /remote/info). Bumps generation() on a
    // fresh connection so callers can re-probe capabilities.
    bool connect();

    // Lazy-connect helper: if already connected, returns true immediately;
    // otherwise attempts connect() (rate-limited so a down engine doesn't stall
    // every call). Used by request() so the engine may be started AFTER the MCP
    // client launches us, and so a dropped connection self-heals.
    bool ensure_connected();

    bool connected() const { return connected_; }
    int active_port() const { return active_port_; }

    // Monotonic counter incremented on every successful (re)connection. The
    // capability registry compares this to know when to re-probe after the
    // engine was (re)started.
    uint64_t generation() const { return generation_; }

    // Generic request to a /remote/* route. `verb` is "GET"/"PUT"/"DELETE".
    // Lazily (re)connects first; on a transport drop, invalidates the
    // connection so the next call re-probes.
    RcResult request(const std::string& verb, const std::string& path,
                     const json& body = json::object());

    // Like request(), but does NOT parse the response as JSON — returns the raw
    // bytes plus Content-Type. For routes that answer with binary payloads
    // (images), e.g. PUT /remote/object/thumbnail. Same lazy-connect /
    // reconnect-on-drop semantics as request().
    RcBinaryResult request_raw(const std::string& verb, const std::string& path,
                               const json& body = json::object());

    // Convenience: PUT /remote/object/call
    RcResult call_function(const std::string& object_path,
                           const std::string& function_name,
                           const json& parameters = json::object(),
                           bool generate_transaction = false);

    // Convenience: PUT /remote/object/property (read or write).
    RcResult get_property(const std::string& object_path,
                          const std::string& property_name);
    RcResult set_property(const std::string& object_path,
                          const std::string& property_name,
                          const json& value,
                          bool generate_transaction = true);

    // GET /remote/info (4.26+). Returns fail() with status 404 on 4.25.
    RcResult get_info();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    RcConfig config_;
    bool connected_ = false;
    int active_port_ = 0;
    uint64_t generation_ = 0;
    // Throttle reconnect attempts when the engine is down, so a long MCP
    // session doesn't pay a full multi-port probe timeout on every tool call.
    std::chrono::steady_clock::time_point last_attempt_{};
    bool attempted_once_ = false;
};

}  // namespace ue_mcp_for_all_versions
