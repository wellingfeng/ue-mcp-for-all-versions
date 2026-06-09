// ue-mcp-for-all-versions — tool registry implementation.
#include "ue_mcp_for_all_versions/tool_registry.hpp"

namespace ue_mcp_for_all_versions {

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

const Tool* ToolRegistry::find(const std::string& name) const {
    for (const auto& t : tools_) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

json ToolRegistry::list_payload() const {
    json arr = json::array();
    for (const auto& t : tools_) {
        arr.push_back({
            {"name", t.name},
            {"description", t.description},
            {"inputSchema", t.input_schema},
        });
    }
    return {{"tools", arr}};
}

ToolResult ToolRegistry::invoke(ToolContext& ctx, const std::string& name,
                                const json& args) const {
    const Tool* tool = find(name);
    if (!tool) {
        return ToolResult::error("unknown tool: " + name);
    }
    // If we've never successfully probed an engine, we don't actually know what
    // is supported — the engine may simply not be running yet. Don't emit a
    // misleading "unsupported"; report a connection error so the user knows to
    // start the engine. (Tools requiring no capability still run.)
    if (!tool->required.empty() && !ctx.caps.probed()) {
        return ToolResult::error(
            "no Unreal Engine RemoteControl server is reachable yet — start the "
            "editor (with RemoteControl's web server running) and retry",
            json{{"tool", name}, {"connected", ctx.rc.connected()}});
    }
    // Runtime auto-degrade: if the connected engine lacks a required
    // capability, return a structured "unsupported" rather than failing.
    for (Capability cap : tool->required) {
        if (!ctx.caps.has(cap)) {
            json extra = {
                {"tool", name},
                {"missingCapability", to_string(cap)},
                {"engineVersion", ctx.caps.engine_version().known()
                                      ? ctx.caps.engine_version().raw
                                      : "unknown"},
            };
            return ToolResult::unsupported(
                std::string("capability '") + to_string(cap) +
                    "' is not available on this engine version",
                extra);
        }
    }
    return tool->handler(ctx, args.is_null() ? json::object() : args);
}

// ---------------------------------------------------------------------------
// Built-in tools. Each maps to RemoteControl operations behind capability
// gates. Helpers below keep handlers concise.
// ---------------------------------------------------------------------------
namespace {

std::string arg_str(const json& args, const char* key, const std::string& def = "") {
    if (args.contains(key) && args[key].is_string()) return args[key].get<std::string>();
    return def;
}

// True if an RcResult is the "this function/object doesn't exist or is blocked
// remotely on this engine version" signal — i.e. we should try a fallback path.
// RemoteControl reports these in different ways across versions: a 404, an HTTP
// 400 with "deprecated"/"unavailable", or even an errorMessage in the body
// (e.g. "Object: ... does not exist." on UE4 when given a UE5-only subsystem
// path). We treat any of these as "try the other version's API".
bool should_try_fallback(const RcResult& res) {
    if (res.status == 404) return true;
    auto body_says = [&](std::initializer_list<const char*> needles) {
        if (!res.body.is_object() || !res.body.contains("errorMessage") ||
            !res.body["errorMessage"].is_string())
            return false;
        const std::string& m = res.body["errorMessage"].get<std::string>();
        for (const char* n : needles)
            if (m.find(n) != std::string::npos) return true;
        return false;
    };
    // Body-level error signals can accompany either a non-2xx OR a 200 (RC
    // sometimes returns 200 with an errorMessage payload).
    if (body_says({"deprecated", "unavailable", "does not exist",
                   "could not be found", "Cannot find", "doesn't resolve",
                   "does not resolve"}))
        return true;
    return false;
}

// Call a function preferring the modern (UE5 subsystem) object path, falling
// back to the legacy (UE4 library) path when the modern one isn't available on
// the connected engine. This keeps a single tool working across 4.25 -> 5.x.
RcResult call_modern_then_legacy(ToolContext& ctx, const char* modern_obj,
                                 const char* legacy_obj, const std::string& fn,
                                 const json& params = json::object(),
                                 bool tx = false) {
    RcResult r = ctx.rc.call_function(modern_obj, fn, params, tx);
    if (should_try_fallback(r)) {
        RcResult legacy = ctx.rc.call_function(legacy_obj, fn, params, tx);
        // Prefer the legacy result unless it ALSO looks like a not-available
        // signal (in which case the original modern error is just as good and
        // we return it for context).
        if (legacy.ok && !should_try_fallback(legacy)) return legacy;
        return r;
    }
    return r;
}

// Wrap an RcResult into a ToolResult, surfacing HTTP errors uniformly.
ToolResult from_rc(const RcResult& res, json ok_extra = json::object()) {
    // RemoteControl sometimes returns HTTP 200 with an {"errorMessage": ...}
    // body (e.g. unresolved object). Treat that as an error too, not success.
    const bool body_error = res.body.is_object() &&
                            res.body.contains("errorMessage") &&
                            res.body["errorMessage"].is_string();
    if (!res.ok || body_error) {
        json extra = {{"httpStatus", res.status}};
        if (!res.body.is_null()) extra["rcBody"] = res.body;
        std::string msg = res.error;
        if (body_error) msg = res.body["errorMessage"].get<std::string>();
        if (msg.empty()) msg = "RemoteControl request failed";
        return ToolResult::error(msg, extra);
    }
    json payload = std::move(ok_extra);
    payload["status"] = "ok";
    payload["result"] = res.body;
    return ToolResult::ok(std::move(payload));
}

}  // namespace

void ToolRegistry::register_builtins() {
    // -- ue_get_engine_version ------------------------------------------------
    add(Tool{
        "ue_get_engine_version",
        "Return the connected Unreal Engine version string and the detected "
        "RemoteControl capabilities. Works on every engine version.",
        json{{"type", "object"}, {"properties", json::object()}},
        {},  // no capability required beyond a live connection
        [](ToolContext& ctx, const json&) -> ToolResult {
            json p = ctx.caps.describe();
            p["status"] = "ok";
            return ToolResult::ok(std::move(p));
        }});

    // -- ue_call_function -----------------------------------------------------
    add(Tool{
        "ue_call_function",
        "Call a UFunction on a Unreal object via RemoteControl "
        "(/remote/object/call). Provide objectPath, functionName, and optional "
        "parameters object. Returns the function's return/out parameters.",
        json{{"type", "object"},
             {"properties",
              {{"objectPath", {{"type", "string"}, {"description", "e.g. /Script/Engine.Default__KismetSystemLibrary"}}},
               {"functionName", {{"type", "string"}}},
               {"parameters", {{"type", "object"}}},
               {"generateTransaction", {{"type", "boolean"}}}}},
             {"required", json::array({"objectPath", "functionName"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string obj = arg_str(args, "objectPath");
            const std::string fn = arg_str(args, "functionName");
            if (obj.empty() || fn.empty()) {
                return ToolResult::error("objectPath and functionName are required");
            }
            json params = args.contains("parameters") && args["parameters"].is_object()
                              ? args["parameters"]
                              : json::object();
            bool tx = args.value("generateTransaction", false);
            return from_rc(ctx.rc.call_function(obj, fn, params, tx));
        }});

    // -- ue_get_property ------------------------------------------------------
    add(Tool{
        "ue_get_property",
        "Read a property on a Unreal object via RemoteControl "
        "(/remote/object/property, READ_ACCESS).",
        json{{"type", "object"},
             {"properties",
              {{"objectPath", {{"type", "string"}}},
               {"propertyName", {{"type", "string"}}}}},
             {"required", json::array({"objectPath", "propertyName"})}},
        {Capability::ObjectProperty},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string obj = arg_str(args, "objectPath");
            const std::string prop = arg_str(args, "propertyName");
            if (obj.empty() || prop.empty()) {
                return ToolResult::error("objectPath and propertyName are required");
            }
            return from_rc(ctx.rc.get_property(obj, prop));
        }});

    // -- ue_set_property ------------------------------------------------------
    add(Tool{
        "ue_set_property",
        "Write a property on a Unreal object via RemoteControl "
        "(/remote/object/property, WRITE_ACCESS).",
        json{{"type", "object"},
             {"properties",
              {{"objectPath", {{"type", "string"}}},
               {"propertyName", {{"type", "string"}}},
               {"value", {{"description", "new property value (any JSON type)"}}},
               {"generateTransaction", {{"type", "boolean"}, {"description", "wrap in an undoable transaction (default true)"}}}}},
             {"required", json::array({"objectPath", "propertyName", "value"})}},
        {Capability::ObjectProperty},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string obj = arg_str(args, "objectPath");
            const std::string prop = arg_str(args, "propertyName");
            if (obj.empty() || prop.empty() || !args.contains("value")) {
                return ToolResult::error("objectPath, propertyName and value are required");
            }
            bool tx = args.value("generateTransaction", true);
            return from_rc(ctx.rc.set_property(obj, prop, args["value"], tx));
        }});

    // -- ue_search_assets (4.26+) --------------------------------------------
    add(Tool{
        "ue_search_assets",
        "Search project assets via RemoteControl (/remote/search/assets). "
        "Requires UE 4.26+; on older engines returns status 'unsupported'.",
        json{{"type", "object"},
             {"properties",
              {{"query", {{"type", "string"}}},
               {"limit", {{"type", "integer"}, {"description", "max results (default 100)"}}}}},
             {"required", json::array({"query"})}},
        {Capability::SearchAssets},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            json body = {
                {"Query", arg_str(args, "query")},
                {"Filter", json::object()},
                {"Limit", args.value("limit", 100)},
            };
            return from_rc(ctx.rc.request("PUT", "/remote/search/assets", body));
        }});

    // -- ue_remote_info (4.26+) ----------------------------------------------
    add(Tool{
        "ue_remote_info",
        "Return the RemoteControl server info, including the list of available "
        "HTTP routes (/remote/info). Requires UE 4.26+.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::Info},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(ctx.rc.get_info());
        }});

    // -- ue_describe_object (4.26+) ------------------------------------------
    add(Tool{
        "ue_describe_object",
        "Describe a Unreal object's properties and functions "
        "(/remote/object/describe). Requires UE 4.26+.",
        json{{"type", "object"},
             {"properties", {{"objectPath", {{"type", "string"}}}}},
             {"required", json::array({"objectPath"})}},
        {Capability::DescribeObject},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            json body = {{"ObjectPath", arg_str(args, "objectPath")}};
            return from_rc(ctx.rc.request("PUT", "/remote/object/describe", body));
        }});

    // -- ue_batch (4.26+) -----------------------------------------------------
    add(Tool{
        "ue_batch",
        "Execute multiple RemoteControl requests in one round-trip "
        "(/remote/batch). Pass an array of {requestId, url, verb, body}. "
        "Requires UE 4.26+.",
        json{{"type", "object"},
             {"properties",
              {{"requests",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"requestId", {{"type", "integer"}}},
                     {"url", {{"type", "string"}}},
                     {"verb", {{"type", "string"}}},
                     {"body", {{"type", "object"}}}}}}}}}}},
             {"required", json::array({"requests"})}},
        {Capability::Batch},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            if (!args.contains("requests") || !args["requests"].is_array()) {
                return ToolResult::error("requests array is required");
            }
            json reqs = json::array();
            for (const auto& r : args["requests"]) {
                reqs.push_back({
                    {"RequestId", r.value("requestId", 0)},
                    {"URL", r.value("url", std::string())},
                    {"Verb", r.value("verb", std::string("PUT"))},
                    {"Body", r.contains("body") ? r["body"] : json::object()},
                });
            }
            return from_rc(ctx.rc.request("PUT", "/remote/batch", json{{"Requests", reqs}}));
        }});

    register_editor_helpers();
}

// ---------------------------------------------------------------------------
// High-level editor convenience tools. These wrap UEditorLevelLibrary /
// UEditorActorSubsystem / UEditorAssetLibrary calls so callers don't have to
// hand-craft object paths. They are cross-version-robust because they call the
// editor-scripting BlueprintFunctionLibraries on their class-default object
// (Default__...), which exist 4.25 -> 5.x (the libraries are deprecated but
// still functional in UE5). They require the engine-bundled
// EditorScriptingUtilities plugin; if a call's target is absent the engine
// returns 404 and we surface that as an error.
// ---------------------------------------------------------------------------
void ToolRegistry::register_editor_helpers() {
    // Legacy (UE4) BlueprintFunctionLibrary CDOs — present 4.25 -> 5.x but
    // deprecated/blocked-remotely for some functions on UE5.
    constexpr const char* kEditorLevelLib =
        "/Script/EditorScriptingUtilities.Default__EditorLevelLibrary";
    constexpr const char* kEditorAssetLib =
        "/Script/EditorScriptingUtilities.Default__EditorAssetLibrary";
    // Modern (UE5) editor subsystem CDOs — only exist on UE5; preferred there.
    constexpr const char* kEditorActorSubsystem =
        "/Script/UnrealEd.Default__EditorActorSubsystem";

    // -- ue_list_actors -------------------------------------------------------
    add(Tool{
        "ue_list_actors",
        "List all actors in the current editor level. Uses EditorActorSubsystem "
        "on UE5 and falls back to EditorLevelLibrary on UE4. Returns an array of "
        "actor object paths. Requires EditorScriptingUtilities (editor only).",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [kEditorActorSubsystem, kEditorLevelLib](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "GetAllLevelActors"));
        }});

    // -- ue_spawn_actor -------------------------------------------------------
    add(Tool{
        "ue_spawn_actor",
        "Spawn an actor from a class into the current level at an optional "
        "location/rotation. Uses EditorActorSubsystem on UE5, EditorLevelLibrary "
        "on UE4. actorClass is an object path, e.g. /Script/Engine.PointLight or "
        "a Blueprint-generated class path.",
        json{{"type", "object"},
             {"properties",
              {{"actorClass", {{"type", "string"}, {"description", "class object path"}}},
               {"location",
                {{"type", "object"},
                 {"properties",
                  {{"x", {{"type", "number"}}},
                   {"y", {{"type", "number"}}},
                   {"z", {{"type", "number"}}}}}}},
               {"rotation",
                {{"type", "object"},
                 {"properties",
                  {{"pitch", {{"type", "number"}}},
                   {"yaw", {{"type", "number"}}},
                   {"roll", {{"type", "number"}}}}}}}}},
             {"required", json::array({"actorClass"})}},
        {Capability::ObjectCall},
        [kEditorActorSubsystem, kEditorLevelLib](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string cls = arg_str(args, "actorClass");
            if (cls.empty()) return ToolResult::error("actorClass is required");
            json loc = args.value("location", json::object());
            json rot = args.value("rotation", json::object());
            json params = {
                {"ActorClass", cls},
                {"Location",
                 {{"X", loc.value("x", 0.0)},
                  {"Y", loc.value("y", 0.0)},
                  {"Z", loc.value("z", 0.0)}}},
                {"Rotation",
                 {{"Pitch", rot.value("pitch", 0.0)},
                  {"Yaw", rot.value("yaw", 0.0)},
                  {"Roll", rot.value("roll", 0.0)}}},
            };
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "SpawnActorFromClass",
                                                   params, /*tx=*/true));
        }});

    // -- ue_destroy_actor -----------------------------------------------------
    add(Tool{
        "ue_destroy_actor",
        "Destroy an actor in the current level by its object path. Uses "
        "EditorActorSubsystem on UE5, EditorLevelLibrary on UE4.",
        json{{"type", "object"},
             {"properties", {{"actorPath", {{"type", "string"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [kEditorActorSubsystem, kEditorLevelLib](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            json params = {{"ActorToDestroy", actor}};
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "DestroyActor",
                                                   params, /*tx=*/true));
        }});

    // -- ue_list_assets -------------------------------------------------------
    add(Tool{
        "ue_list_assets",
        "List asset paths under a content directory (via "
        "EditorAssetLibrary.ListAssets). directoryPath e.g. \"/Game\".",
        json{{"type", "object"},
             {"properties",
              {{"directoryPath", {{"type", "string"}, {"description", "e.g. /Game"}}},
               {"recursive", {{"type", "boolean"}}}}},
             {"required", json::array({"directoryPath"})}},
        {Capability::ObjectCall},
        [kEditorAssetLib](ToolContext& ctx, const json& args) -> ToolResult {
            json params = {
                {"DirectoryPath", arg_str(args, "directoryPath", "/Game")},
                {"bRecursive", args.value("recursive", true)},
                {"bIncludeFolder", false},
            };
            return from_rc(ctx.rc.call_function(kEditorAssetLib, "ListAssets", params));
        }});

    // -- ue_does_asset_exist --------------------------------------------------
    add(Tool{
        "ue_does_asset_exist",
        "Check whether an asset exists at a given path (via "
        "EditorAssetLibrary.DoesAssetExist). assetPath e.g. "
        "\"/Game/Maps/MyMap\".",
        json{{"type", "object"},
             {"properties", {{"assetPath", {{"type", "string"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [kEditorAssetLib](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            json params = {{"AssetPath", path}};
            return from_rc(ctx.rc.call_function(kEditorAssetLib, "DoesAssetExist", params));
        }});
}

}  // namespace ue_mcp_for_all_versions
