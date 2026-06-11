// ue-mcp-for-all-versions — capability registry implementation.
#include "ue_mcp_for_all_versions/capability_registry.hpp"

#include <cctype>
#include <cstdlib>

namespace ue_mcp_for_all_versions {

const char* to_string(Capability cap) {
    switch (cap) {
        case Capability::ObjectCall: return "object.call";
        case Capability::ObjectProperty: return "object.property";
        case Capability::Info: return "info";
        case Capability::Batch: return "batch";
        case Capability::SearchAssets: return "search.assets";
        case Capability::DescribeObject: return "object.describe";
        case Capability::PropertyArrayOps: return "object.property.arrayops";
        case Capability::Presets: return "presets";
        case Capability::PythonScripting: return "python";
        case Capability::PieControl: return "pie.control";
        case Capability::Thumbnail: return "object.thumbnail";
    }
    return "unknown";
}

EngineVersion CapabilityRegistry::parse_version(const std::string& raw) {
    EngineVersion v;
    v.raw = raw;
    // Expected forms: "5.5.4-...", "4.25.4-...". Take the leading numeric dotted
    // prefix, ignore anything after a non-version char.
    int parts[3] = {0, 0, 0};
    int idx = 0;
    size_t i = 0;
    while (i < raw.size() && idx < 3) {
        if (std::isdigit(static_cast<unsigned char>(raw[i]))) {
            int n = 0;
            while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i]))) {
                n = n * 10 + (raw[i] - '0');
                ++i;
            }
            parts[idx++] = n;
            if (i < raw.size() && raw[i] == '.') {
                ++i;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    return v;
}

void CapabilityRegistry::probe(RcClient& client) {
    probed_ = true;
    probed_generation_ = client.generation();
    caps_.clear();
    routes_.clear();
    version_ = EngineVersion{};

    // object.call and object.property exist on every version (4.25+).
    caps_.insert(Capability::ObjectCall);
    caps_.insert(Capability::ObjectProperty);

    // /remote/info is 4.26+. Use it both as a capability and as a route source.
    RcResult info = client.get_info();
    info_available_ = info.ok;
    if (info.ok && info.body.is_object()) {
        caps_.insert(Capability::Info);
        if (info.body.contains("HttpRoutes") && info.body["HttpRoutes"].is_array()) {
            for (const auto& route : info.body["HttpRoutes"]) {
                if (route.contains("Path") && route["Path"].is_string()) {
                    routes_.insert(route["Path"].get<std::string>());
                }
            }
        }
    }

    // Query engine version regardless (works on all versions via call route).
    RcResult ver = client.call_function(
        "/Script/Engine.Default__KismetSystemLibrary", "GetEngineVersion");
    if (ver.ok && ver.body.is_object() && ver.body.contains("ReturnValue") &&
        ver.body["ReturnValue"].is_string()) {
        version_ = parse_version(ver.body["ReturnValue"].get<std::string>());
    }

    // Python is NOT a version-based capability: the PythonScriptPlugin can be
    // disabled in a project even on a version that ships it. Probe it directly.
    // A clean {ReturnValue:true} means Python is available; anything else
    // (false, 404 object-not-resolved, transport error) means it is not.
    RcResult py = client.call_function(
        "/Script/PythonScriptPlugin.Default__PythonScriptLibrary",
        "IsPythonAvailable");
    if (py.ok && py.body.is_object() && py.body.contains("ReturnValue") &&
        py.body["ReturnValue"].is_boolean() &&
        py.body["ReturnValue"].get<bool>()) {
        caps_.insert(Capability::PythonScripting);
    }

    infer_from_version();
}

void CapabilityRegistry::ensure_probed(RcClient& client) {
    // (Re)probe when the client has a live connection whose generation differs
    // from what we last probed. This covers: first connection, engine started
    // late, and engine restart (new generation). When the client is not
    // connected we leave the last-known capabilities in place — the request
    // itself will try to reconnect.
    if (client.connected() && client.generation() != probed_generation_) {
        probe(client);
    }
}

void CapabilityRegistry::infer_from_version() {
    // Prefer route-presence evidence from /remote/info; fall back to version
    // inference when info wasn't available (4.25) or routes weren't listed.
    auto route_has = [&](const char* prefix) {
        for (const auto& r : routes_) {
            if (r.rfind(prefix, 0) == 0) return true;
        }
        return false;
    };

    const bool v426 = version_.known() ? version_.at_least(4, 26) : info_available_;
    const bool v5 = version_.known() && version_.at_least(5, 0);

    if (v426 || route_has("/remote/batch")) caps_.insert(Capability::Batch);
    if (v426 || route_has("/remote/search/assets")) caps_.insert(Capability::SearchAssets);
    if (v426 || route_has("/remote/object/describe")) caps_.insert(Capability::DescribeObject);
    if (v426 || route_has("/remote/preset")) caps_.insert(Capability::Presets);
    // /remote/object/thumbnail is an editor route present 4.26+. Prefer route
    // evidence; fall back to version.
    if (v426 || route_has("/remote/object/thumbnail")) caps_.insert(Capability::Thumbnail);
    if (v5 || route_has("/remote/object/property/append"))
        caps_.insert(Capability::PropertyArrayOps);
    // PIE control (IsInPlayInEditor / EditorRequestEndPlay) lives on
    // ULevelEditorSubsystem, which only exists on UE 5.x (verified absent on
    // 4.25/4.26). There is no /remote/info route for it, so this is
    // version-gated only.
    if (v5) caps_.insert(Capability::PieControl);
}

bool CapabilityRegistry::has(Capability cap) const {
    return caps_.find(cap) != caps_.end();
}

json CapabilityRegistry::describe() const {
    json caps = json::array();
    for (Capability c : {Capability::ObjectCall, Capability::ObjectProperty,
                         Capability::Info, Capability::Batch,
                         Capability::SearchAssets, Capability::DescribeObject,
                         Capability::PropertyArrayOps, Capability::Presets,
                         Capability::PythonScripting, Capability::PieControl,
                         Capability::Thumbnail}) {
        if (has(c)) caps.push_back(to_string(c));
    }
    json out = {
        {"engineVersion", version_.known() ? version_.raw : "unknown"},
        {"engineMajor", version_.major},
        {"engineMinor", version_.minor},
        {"enginePatch", version_.patch},
        {"infoAvailable", info_available_},
        {"capabilities", caps},
        {"routeCount", static_cast<int>(routes_.size())},
    };
    return out;
}

}  // namespace ue_mcp_for_all_versions
