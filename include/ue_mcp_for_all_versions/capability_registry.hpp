// ue-mcp-for-all-versions
// Capability registry: at runtime, discovers which RemoteControl routes and
// engine features are available on the connected engine, so tools can degrade
// gracefully (return a structured "unsupported" instead of failing) rather
// than assuming a fixed API surface.
#pragma once

#include <set>
#include <string>

#include "ue_mcp_for_all_versions/rc_client.hpp"

namespace ue_mcp_for_all_versions {

// Parsed engine version, e.g. "5.5.4-..." -> {5, 5, 4}. Zeros if unknown.
struct EngineVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string raw;  // full version string as returned by the engine

    bool known() const { return major != 0; }
    bool at_least(int mj, int mn) const {
        return major > mj || (major == mj && minor >= mn);
    }
};

// A single capability the registry tracks. Tools query these by id.
enum class Capability {
    ObjectCall,        // PUT /remote/object/call (all versions)
    ObjectProperty,    // PUT /remote/object/property (all versions)
    Info,              // GET /remote/info (4.26+)
    Batch,             // PUT /remote/batch (4.26+)
    SearchAssets,      // PUT /remote/search/assets (4.26+)
    DescribeObject,    // PUT /remote/object/describe (4.26+)
    PropertyArrayOps,  // append/insert/remove (5.x+)
    Presets,           // /remote/preset/* (4.26+)
    PythonScripting,   // UPythonScriptLibrary present + Python enabled (probed)
    PieControl,        // LevelEditorSubsystem PIE control (5.3+, version-gated)
};

class CapabilityRegistry {
public:
    // Probe the connected engine: read /remote/info routes (when available)
    // and query the engine version. Safe to call once after RcClient::connect.
    void probe(RcClient& client);

    // Re-probe only if the client has (re)connected since the last probe (i.e.
    // its generation advanced) and is currently connected. Cheap no-op
    // otherwise. Call this before serving a tool so capabilities reflect the
    // engine that is actually connected right now (handles late start /
    // restart / version switch between sessions).
    void ensure_probed(RcClient& client);

    // True once a probe has run against some engine. When false (engine never
    // reachable yet), capability gates should not hard-fail tools — the client
    // will attempt to connect on the request itself.
    bool probed() const { return probed_; }

    bool has(Capability cap) const;
    const EngineVersion& engine_version() const { return version_; }

    // Set of raw route paths discovered from /remote/info (empty on 4.25).
    const std::set<std::string>& routes() const { return routes_; }

    // True if /remote/info was reachable (i.e. engine is 4.26+).
    bool info_available() const { return info_available_; }

    // Human-readable summary for diagnostics / MCP server capabilities.
    json describe() const;

    static EngineVersion parse_version(const std::string& raw);

    // Test seam: inject a known capability set + version without a live engine,
    // marking the registry as probed. Used by unit tests to exercise the
    // degradation path deterministically. Not used in normal operation.
    void set_probed_for_test(EngineVersion version, std::set<Capability> caps) {
        version_ = std::move(version);
        caps_ = std::move(caps);
        probed_ = true;
    }

private:
    EngineVersion version_;
    std::set<std::string> routes_;
    std::set<Capability> caps_;
    bool info_available_ = false;
    bool probed_ = false;
    uint64_t probed_generation_ = 0;  // RcClient::generation() at last probe

    void infer_from_version();
};

const char* to_string(Capability cap);

}  // namespace ue_mcp_for_all_versions
