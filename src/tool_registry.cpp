// ue-mcp-for-all-versions — tool registry implementation.
#include "ue_mcp_for_all_versions/tool_registry.hpp"

#include <cmath>
#include <cstring>

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

// -- Shared object-path constants (CDOs) used across helper groups. ----------// Legacy (UE4) BlueprintFunctionLibrary CDOs — present 4.25 -> 5.x but some
// functions are deprecated/blocked-remotely on UE5.
constexpr const char* kEditorLevelLib =
    "/Script/EditorScriptingUtilities.Default__EditorLevelLibrary";
constexpr const char* kEditorAssetLib =
    "/Script/EditorScriptingUtilities.Default__EditorAssetLibrary";
constexpr const char* kEditorFilterLib =
    "/Script/EditorScriptingUtilities.Default__EditorFilterLibrary";
// Modern (UE5) editor subsystem CDOs — only exist on UE5; preferred there.
constexpr const char* kEditorActorSubsystem =
    "/Script/UnrealEd.Default__EditorActorSubsystem";
constexpr const char* kEditorAssetSubsystem =
    "/Script/UnrealEd.Default__EditorAssetSubsystem";
constexpr const char* kUnrealEditorSubsystem =
    "/Script/UnrealEd.Default__UnrealEditorSubsystem";
constexpr const char* kLevelEditorSubsystem =
    "/Script/LevelEditor.Default__LevelEditorSubsystem";
constexpr const char* kKismetSystemLib =
    "/Script/Engine.Default__KismetSystemLibrary";
constexpr const char* kPythonScriptLib =
    "/Script/PythonScriptPlugin.Default__PythonScriptLibrary";
constexpr const char* kAutomationLib =
    "/Script/FunctionalTesting.Default__AutomationBlueprintFunctionLibrary";
constexpr const char* kMaterialEditingLib =
    "/Script/MaterialEditor.Default__MaterialEditingLibrary";
// Build an {X,Y,Z} object from an args sub-object with lower-case x/y/z keys.
json xyz(const json& src, double dx = 0.0, double dy = 0.0, double dz = 0.0) {
    return json{{"X", src.value("x", dx)},
                {"Y", src.value("y", dy)},
                {"Z", src.value("z", dz)}};
}

// Build a {Pitch,Yaw,Roll} object from an args sub-object.
json pyr(const json& src) {
    return json{{"Pitch", src.value("pitch", 0.0)},
                {"Yaw", src.value("yaw", 0.0)},
                {"Roll", src.value("roll", 0.0)}};
}

// -- base64 (RFC 4648) -------------------------------------------------------
// Shared by the Python-recipe harness (encode args, decode the printed result)
// and by the material thumbnail tool (encode image bytes). Defined once here
// because all unnamed namespaces in this translation unit are the same
// namespace — a second definition would be an ODR violation.
std::string base64_encode(const std::string& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned n = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8) |
                     static_cast<unsigned char>(in[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back(tbl[n & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned n = static_cast<unsigned char>(in[i]) << 16;
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == in.size()) {
        unsigned n = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back(tbl[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// Decode a base64 string. Ignores whitespace; stops at padding. Returns the
// decoded bytes; sets ok=false on an invalid character.
std::string base64_decode(const std::string& in, bool& ok) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    ok = true;
    std::string out;
    int buf = 0, bits = 0;
    for (unsigned char c : in) {
        if (c == '=' ) break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = val(c);
        if (v < 0) { ok = false; return out; }
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Python recipe harness. Layer-2 authoring tools (create blueprint / widget /
// material, etc.) can't be expressed as a single RemoteControl UFunction call —
// they need a short sequence of editor-Python steps. Rather than have the agent
// drive that sequence one REPL round-trip at a time (the failure mode that cost
// 27 minutes on the login-screen task), we ship the whole sequence as one
// script and run it in a single ExecutePythonCommandEx call.
//
// Contract for a recipe script body:
//   * It may read its arguments from the pre-defined dict `_ARGS`.
//   * It must end by calling `_emit(result_dict)` exactly once. `_emit` prints
//     a sentinel-wrapped JSON line that we parse out of the captured log.
//   * Uncaught exceptions are caught by the harness preamble and emitted as
//     `{"ok": false, "error": "..."}` so a failing recipe never hangs or leaks
//     a raw traceback as the tool result.
// This keeps Python-side logic readable while the C++ side stays a thin,
// injection-safe transport.
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kRecipeSentinel = "<<<UEMCP_RESULT>>>";

// Assemble the full script: a preamble that decodes _ARGS and defines _emit,
// then the recipe body wrapped in try/except so any error is reported through
// the same sentinel channel instead of as an unstructured Python failure.
std::string build_recipe_script(const std::string& body, const json& args) {
    const std::string args_b64 = base64_encode(args.dump());
    std::string s;
    s += "import json, base64, traceback\n";
    s += "import unreal\n";
    s += "_ARGS = json.loads(base64.b64decode('" + args_b64 + "').decode('utf-8'))\n";
    s += "def _emit(_d):\n";
    s += "    print('" + std::string(kRecipeSentinel) + "' + json.dumps(_d))\n";
    s += "try:\n";
    // Indent every line of the body by 4 spaces so it sits inside the try.
    {
        std::string line;
        for (char c : body) {
            if (c == '\n') {
                s += "    ";
                s += line;
                s += "\n";
                line.clear();
            } else {
                line.push_back(c);
            }
        }
        if (!line.empty()) {
            s += "    ";
            s += line;
            s += "\n";
        }
    }
    s += "except Exception as _e:\n";
    s += "    _emit({'ok': False, 'error': str(_e), 'trace': traceback.format_exc()})\n";
    return s;
}

// Parse the sentinel-wrapped JSON result out of an ExecutePythonCommandEx
// response. The response shape is {ReturnValue, CommandResult, LogOutput:[{Type,
// Output}]}; the recipe's _emit() print lands in LogOutput. Returns true and
// fills `out` if a sentinel line was found and parsed.
bool extract_recipe_result(const RcResult& rc, json& out) {
    if (!rc.ok || !rc.body.is_object()) return false;
    auto scan = [&](const std::string& text) -> bool {
        std::string::size_type pos = text.rfind(kRecipeSentinel);
        if (pos == std::string::npos) return false;
        std::string tail = text.substr(pos + std::strlen(kRecipeSentinel));
        // Trim to the end of the JSON (the log line may carry a trailing \n or
        // extra log decoration); parse the first balanced JSON object.
        std::string::size_type nl = tail.find('\n');
        if (nl != std::string::npos) tail = tail.substr(0, nl);
        try {
            out = json::parse(tail);
            return true;
        } catch (...) {
            return false;
        }
    };
    if (rc.body.contains("LogOutput") && rc.body["LogOutput"].is_array()) {
        // Concatenate then scan from the end so the last _emit wins.
        std::string all;
        for (const auto& entry : rc.body["LogOutput"]) {
            if (entry.is_object() && entry.contains("Output") &&
                entry["Output"].is_string()) {
                all += entry["Output"].get<std::string>();
            }
        }
        if (scan(all)) return true;
    }
    // Some engines surface the print on CommandResult instead.
    if (rc.body.contains("CommandResult") && rc.body["CommandResult"].is_string()) {
        if (scan(rc.body["CommandResult"].get<std::string>())) return true;
    }
    return false;
}

// Run a recipe end to end and turn it into a ToolResult. `body` is the Python
// recipe (using _ARGS / _emit); `args` is passed through as _ARGS. On a missing
// sentinel we surface the raw RC body so failures are diagnosable, never silent.
ToolResult run_python_recipe(ToolContext& ctx, const std::string& body,
                             const json& args) {
    const std::string script = build_recipe_script(body, args);
    RcResult rc = ctx.rc.call_function(
        kPythonScriptLib, "ExecutePythonCommandEx",
        json{{"PythonCommand", script},
             // ExecuteFile compiles the whole string as a module (multi-line OK).
             // ExecuteStatement would reject our multi-line preamble with
             // "multiple statements found while compiling a single statement".
             {"ExecutionMode", "ExecuteFile"},
             {"FileExecutionScope", "Private"}});
    if (!rc.ok) return from_rc(rc, json{{"hint", "Python recipe transport failed"}});
    json result;
    if (!extract_recipe_result(rc, result)) {
        return ToolResult::error(
            "Python recipe produced no parseable result (no sentinel in log)",
            json{{"rcBody", rc.body}});
    }
    // The recipe reports success/failure via an "ok" boolean in its result.
    const bool ok = result.value("ok", false);
    if (!ok) {
        json extra = result;
        extra.erase("ok");
        return ToolResult::error(result.value("error", "recipe reported failure"),
                                 std::move(extra));
    }
    result.erase("ok");
    result["status"] = "ok";
    return ToolResult::ok(std::move(result));
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
    register_scripting_tools();
    register_actor_tools();
    register_asset_tools();
    register_level_tools();
    register_rc_route_tools();
    register_introspection_tools();
    register_material_tools();
    register_workflow_tools();
    register_scene_tools();
    register_mesh_light_tools();
    register_creation_tools();
    register_data_debug_tools();
    register_authoring_tools();
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
    // -- ue_list_actors -------------------------------------------------------
    add(Tool{
        "ue_list_actors",
        "List all actors in the current editor level. Uses EditorActorSubsystem "
        "on UE5 and falls back to EditorLevelLibrary on UE4. Returns an array of "
        "actor object paths. Requires EditorScriptingUtilities (editor only).",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json&) -> ToolResult {
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
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string cls = arg_str(args, "actorClass");
            if (cls.empty()) return ToolResult::error("actorClass is required");
            json loc = args.value("location", json::object());
            json rot = args.value("rotation", json::object());
            json params = {
                {"ActorClass", cls},
                {"Location", xyz(loc)},
                {"Rotation", pyr(rot)},
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
        [](ToolContext& ctx, const json& args) -> ToolResult {
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
        "List asset paths under a content directory. Uses EditorAssetSubsystem "
        "on UE5 and falls back to EditorAssetLibrary on UE4. directoryPath e.g. "
        "\"/Game\".",
        json{{"type", "object"},
             {"properties",
              {{"directoryPath", {{"type", "string"}, {"description", "e.g. /Game"}}},
               {"recursive", {{"type", "boolean"}}}}},
             {"required", json::array({"directoryPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            json params = {
                {"DirectoryPath", arg_str(args, "directoryPath", "/Game")},
                {"bRecursive", args.value("recursive", true)},
                {"bIncludeFolder", false},
            };
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "ListAssets", params));
        }});

    // -- ue_does_asset_exist --------------------------------------------------
    add(Tool{
        "ue_does_asset_exist",
        "Check whether an asset exists at a given path. Uses EditorAssetSubsystem "
        "on UE5 and falls back to EditorAssetLibrary on UE4. assetPath e.g. "
        "\"/Game/Maps/MyMap\".",
        json{{"type", "object"},
             {"properties", {{"assetPath", {{"type", "string"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            json params = {{"AssetPath", path}};
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "DoesAssetExist", params));
        }});
}

// ---------------------------------------------------------------------------
// Scripting tools: Python execution, console commands, and editor context.
// Python is stable across 4.25 -> 5.8 (UPythonScriptLibrary), but gated on the
// PythonScriptPlugin being enabled (probed via IsPythonAvailable). Console
// commands go through KismetSystemLibrary, which on UE4 needs a resolved world
// context; we resolve it lazily and retry.
// ---------------------------------------------------------------------------
void ToolRegistry::register_scripting_tools() {
    // -- ue_python_exec -------------------------------------------------------
    add(Tool{
        "ue_python_exec",
        "Execute a Python command/script string in the editor and capture its "
        "result and log output (UPythonScriptLibrary.ExecutePythonCommandEx). "
        "mode: ExecuteFile (default; run as a script), ExecuteStatement (exec "
        "one statement), or EvaluateStatement (eval and return the value as "
        "commandResult). Requires the PythonScriptPlugin to be enabled; on "
        "engines without Python this returns status 'unsupported'. Works "
        "4.25 -> 5.8.",
        json{{"type", "object"},
             {"properties",
              {{"code", {{"type", "string"}, {"description", "Python code, a .py file path, or an expression"}}},
               {"mode", {{"type", "string"}, {"enum", json::array({"ExecuteFile", "ExecuteStatement", "EvaluateStatement"})}}},
               {"scope", {{"type", "string"}, {"enum", json::array({"Private", "Public"})}, {"description", "FileExecutionScope (ExecuteFile only)"}}}}},
             {"required", json::array({"code"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string code = arg_str(args, "code");
            if (code.empty()) return ToolResult::error("code is required");
            json params = {
                {"PythonCommand", code},
                {"ExecutionMode", arg_str(args, "mode", "ExecuteFile")},
                {"FileExecutionScope", arg_str(args, "scope", "Private")},
            };
            return from_rc(ctx.rc.call_function(kPythonScriptLib,
                                                "ExecutePythonCommandEx", params));
        }});

    // -- ue_exec_console_command ---------------------------------------------
    add(Tool{
        "ue_exec_console_command",
        "Run an editor/console command (e.g. 'stat fps', 'r.ScreenPercentage "
        "50', 'HighResShot 1920x1080'). On UE 5.3+ the world context is "
        "optional; on UE 4.25/4.26 we resolve the editor world automatically "
        "and retry. Works 4.25 -> 5.8.",
        json{{"type", "object"},
             {"properties", {{"command", {{"type", "string"}}}}},
             {"required", json::array({"command"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string cmd = arg_str(args, "command");
            if (cmd.empty()) return ToolResult::error("command is required");
            // First attempt: no world context (works on 5.3+ via
            // CallableWithoutWorldContext, and sometimes on UE4).
            RcResult r = ctx.rc.call_function(kKismetSystemLib,
                                              "ExecuteConsoleCommand",
                                              json{{"Command", cmd}});
            if (r.ok && !(r.body.is_object() && r.body.contains("errorMessage")))
                return from_rc(r);
            // Retry path (UE4): resolve the editor world, pass it explicitly.
            RcResult world = call_modern_then_legacy(ctx, kUnrealEditorSubsystem,
                                                     kEditorLevelLib, "GetEditorWorld");
            if (world.ok && world.body.is_object() &&
                world.body.contains("ReturnValue") &&
                world.body["ReturnValue"].is_string()) {
                json params = {
                    {"WorldContextObject", world.body["ReturnValue"]},
                    {"Command", cmd},
                };
                RcResult r2 = ctx.rc.call_function(kKismetSystemLib,
                                                   "ExecuteConsoleCommand", params);
                if (r2.ok) return from_rc(r2);
            }
            return from_rc(r);  // surface the original error if retry didn't help
        }});

    // -- ue_get_editor_world --------------------------------------------------
    add(Tool{
        "ue_get_editor_world",
        "Return the editor UWorld object path. Uses UnrealEditorSubsystem on "
        "UE5 and falls back to EditorLevelLibrary on UE4.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(call_modern_then_legacy(ctx, kUnrealEditorSubsystem,
                                                   kEditorLevelLib, "GetEditorWorld"));
        }});

    // -- ue_get_project_info --------------------------------------------------
    add(Tool{
        "ue_get_project_info",
        "Return aggregated project/engine metadata (engine version, project "
        "name, and the project/content/saved directories) by composing several "
        "KismetSystemLibrary calls. Works 4.25 -> 5.8.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json&) -> ToolResult {
            struct Field { const char* key; const char* fn; };
            const Field fields[] = {
                {"engineVersion", "GetEngineVersion"},
                {"gameName", "GetGameName"},
                {"projectDirectory", "GetProjectDirectory"},
                {"contentDirectory", "GetProjectContentDirectory"},
                {"savedDirectory", "GetProjectSavedDirectory"},
            };
            json info = json::object();
            for (const auto& f : fields) {
                RcResult r = ctx.rc.call_function(kKismetSystemLib, f.fn);
                if (r.ok && r.body.is_object() && r.body.contains("ReturnValue"))
                    info[f.key] = r.body["ReturnValue"];
                else
                    info[f.key] = nullptr;
            }
            return ToolResult::ok(json{{"status", "ok"}, {"projectInfo", info}});
        }});
}

// ---------------------------------------------------------------------------
// Actor tools: transform get/set, label get/set, selection. Transform/label
// functions are called on the actor INSTANCE path (obtained from
// ue_list_actors), not a CDO. They map to AActor UFUNCTIONs that are stable
// 4.25 -> 5.8. Selection uses the modern/legacy editor pair.
// ---------------------------------------------------------------------------
void ToolRegistry::register_actor_tools() {
    auto require_actor = [](const json& args, std::string& out) -> bool {
        out = arg_str(args, "actorPath");
        return !out.empty();
    };

    // -- ue_get_actor_transform ----------------------------------------------
    add(Tool{
        "ue_get_actor_transform",
        "Read an actor's world transform (location, rotation as quaternion, and "
        "scale). actorPath is an actor instance path from ue_list_actors.",
        json{{"type", "object"},
             {"properties", {{"actorPath", {{"type", "string"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [require_actor](ToolContext& ctx, const json& args) -> ToolResult {
            std::string actor;
            if (!require_actor(args, actor)) return ToolResult::error("actorPath is required");
            // GetTransform is the UFunction symbol (DisplayName GetActorTransform).
            return from_rc(ctx.rc.call_function(actor, "GetTransform"));
        }});

    // -- ue_set_actor_location -----------------------------------------------
    add(Tool{
        "ue_set_actor_location",
        "Set an actor's world location. Pass actorPath and location {x,y,z}.",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"location", {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}}}}},
             {"required", json::array({"actorPath", "location"})}},
        {Capability::ObjectCall},
        [require_actor](ToolContext& ctx, const json& args) -> ToolResult {
            std::string actor;
            if (!require_actor(args, actor)) return ToolResult::error("actorPath is required");
            json params = {{"NewLocation", xyz(args.value("location", json::object()))},
                           {"bSweep", false},
                           {"bTeleport", true}};
            return from_rc(ctx.rc.call_function(actor, "K2_SetActorLocation", params, true));
        }});

    // -- ue_set_actor_rotation -----------------------------------------------
    add(Tool{
        "ue_set_actor_rotation",
        "Set an actor's world rotation. Pass actorPath and rotation "
        "{pitch,yaw,roll} (degrees).",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"rotation", {{"type", "object"}, {"properties", {{"pitch", {{"type", "number"}}}, {"yaw", {{"type", "number"}}}, {"roll", {{"type", "number"}}}}}}}}},
             {"required", json::array({"actorPath", "rotation"})}},
        {Capability::ObjectCall},
        [require_actor](ToolContext& ctx, const json& args) -> ToolResult {
            std::string actor;
            if (!require_actor(args, actor)) return ToolResult::error("actorPath is required");
            json params = {{"NewRotation", pyr(args.value("rotation", json::object()))},
                           {"bTeleportPhysics", true}};
            return from_rc(ctx.rc.call_function(actor, "K2_SetActorRotation", params, true));
        }});

    // -- ue_set_actor_scale --------------------------------------------------
    add(Tool{
        "ue_set_actor_scale",
        "Set an actor's world scale. Pass actorPath and scale {x,y,z}.",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"scale", {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}}}}},
             {"required", json::array({"actorPath", "scale"})}},
        {Capability::ObjectCall},
        [require_actor](ToolContext& ctx, const json& args) -> ToolResult {
            std::string actor;
            if (!require_actor(args, actor)) return ToolResult::error("actorPath is required");
            json params = {{"NewScale3D", xyz(args.value("scale", json::object()), 1.0, 1.0, 1.0)}};
            return from_rc(ctx.rc.call_function(actor, "SetActorScale3D", params, true));
        }});

    // -- ue_get_actor_label / ue_set_actor_label -----------------------------
    add(Tool{
        "ue_get_actor_label",
        "Read an actor's editor display label. actorPath is an actor instance "
        "path.",
        json{{"type", "object"},
             {"properties", {{"actorPath", {{"type", "string"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [require_actor](ToolContext& ctx, const json& args) -> ToolResult {
            std::string actor;
            if (!require_actor(args, actor)) return ToolResult::error("actorPath is required");
            return from_rc(ctx.rc.call_function(actor, "GetActorLabel"));
        }});
    add(Tool{
        "ue_set_actor_label",
        "Set an actor's editor display label (the name shown in the World "
        "Outliner). actorPath + label.",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}}, {"label", {{"type", "string"}}}}},
             {"required", json::array({"actorPath", "label"})}},
        {Capability::ObjectCall},
        [require_actor](ToolContext& ctx, const json& args) -> ToolResult {
            std::string actor;
            if (!require_actor(args, actor)) return ToolResult::error("actorPath is required");
            json params = {{"NewActorLabel", arg_str(args, "label")}, {"bMarkDirty", true}};
            return from_rc(ctx.rc.call_function(actor, "SetActorLabel", params, true));
        }});

    // -- ue_get_selected_actors / ue_select_actors / ue_clear_selection ------
    add(Tool{
        "ue_get_selected_actors",
        "Return the actors currently selected in the editor. Uses "
        "EditorActorSubsystem on UE5, EditorLevelLibrary on UE4.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "GetSelectedLevelActors"));
        }});
    add(Tool{
        "ue_select_actors",
        "Select a set of actors in the editor (replaces the current selection). "
        "Pass actorPaths: an array of actor instance paths.",
        json{{"type", "object"},
             {"properties",
              {{"actorPaths", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
             {"required", json::array({"actorPaths"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            if (!args.contains("actorPaths") || !args["actorPaths"].is_array())
                return ToolResult::error("actorPaths array is required");
            json params = {{"ActorsToSelect", args["actorPaths"]}};
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "SetSelectedLevelActors",
                                                   params, true));
        }});
    add(Tool{
        "ue_clear_selection",
        "Clear the editor actor selection. Uses EditorActorSubsystem on UE5, "
        "EditorLevelLibrary on UE4.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "SelectNothing",
                                                   json::object(), true));
        }});
}

// ---------------------------------------------------------------------------
// Extended asset tools. All use EditorAssetSubsystem (UE5) -> EditorAssetLibrary
// (UE4) via modern_then_legacy. Require the EditorScriptingUtilities plugin.
// ---------------------------------------------------------------------------
void ToolRegistry::register_asset_tools() {
    // -- ue_save_asset --------------------------------------------------------
    add(Tool{
        "ue_save_asset",
        "Save a single asset to disk. assetPath e.g. \"/Game/Maps/MyMap\". By "
        "default only saves if dirty.",
        json{{"type", "object"},
             {"properties",
              {{"assetPath", {{"type", "string"}}},
               {"onlyIfDirty", {{"type", "boolean"}, {"description", "default true"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            json params = {{"AssetToSave", path}, {"bOnlyIfIsDirty", args.value("onlyIfDirty", true)}};
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "SaveAsset", params, true));
        }});

    // -- ue_save_directory ----------------------------------------------------
    add(Tool{
        "ue_save_directory",
        "Save all assets under a content directory. directoryPath e.g. "
        "\"/Game/Maps\".",
        json{{"type", "object"},
             {"properties",
              {{"directoryPath", {{"type", "string"}}},
               {"onlyIfDirty", {{"type", "boolean"}}},
               {"recursive", {{"type", "boolean"}}}}},
             {"required", json::array({"directoryPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "directoryPath");
            if (path.empty()) return ToolResult::error("directoryPath is required");
            json params = {{"DirectoryPath", path},
                           {"bOnlyIfIsDirty", args.value("onlyIfDirty", true)},
                           {"bRecursive", args.value("recursive", true)}};
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "SaveDirectory", params, true));
        }});

    // -- ue_duplicate_asset ---------------------------------------------------
    add(Tool{
        "ue_duplicate_asset",
        "Duplicate an asset to a new path. Pass sourcePath and destinationPath "
        "(both like \"/Game/...\").",
        json{{"type", "object"},
             {"properties",
              {{"sourcePath", {{"type", "string"}}},
               {"destinationPath", {{"type", "string"}}}}},
             {"required", json::array({"sourcePath", "destinationPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string src = arg_str(args, "sourcePath");
            const std::string dst = arg_str(args, "destinationPath");
            if (src.empty() || dst.empty())
                return ToolResult::error("sourcePath and destinationPath are required");
            json params = {{"SourceAssetPath", src}, {"DestinationAssetPath", dst}};
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "DuplicateAsset", params, true));
        }});

    // -- ue_rename_asset ------------------------------------------------------
    add(Tool{
        "ue_rename_asset",
        "Rename/move an asset. Pass sourcePath and destinationPath.",
        json{{"type", "object"},
             {"properties",
              {{"sourcePath", {{"type", "string"}}},
               {"destinationPath", {{"type", "string"}}}}},
             {"required", json::array({"sourcePath", "destinationPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string src = arg_str(args, "sourcePath");
            const std::string dst = arg_str(args, "destinationPath");
            if (src.empty() || dst.empty())
                return ToolResult::error("sourcePath and destinationPath are required");
            json params = {{"SourceAssetPath", src}, {"DestinationAssetPath", dst}};
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "RenameAsset", params, true));
        }});

    // -- ue_delete_asset ------------------------------------------------------
    add(Tool{
        "ue_delete_asset",
        "Delete an asset from the project. DESTRUCTIVE: permanently removes the "
        "asset file. assetPath e.g. \"/Game/Junk/Old\".",
        json{{"type", "object"},
             {"properties", {{"assetPath", {{"type", "string"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            json params = {{"AssetPathToDelete", path}};
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "DeleteAsset", params, true));
        }});

    // -- ue_find_asset_data ---------------------------------------------------
    add(Tool{
        "ue_find_asset_data",
        "Return registry metadata for an asset (package, name, class). "
        "assetPath e.g. \"/Game/Maps/MyMap\". The asset class is reported under "
        "AssetClass (UE<=5.0) or AssetClassPath (UE5.1+).",
        json{{"type", "object"},
             {"properties", {{"assetPath", {{"type", "string"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            json params = {{"AssetPath", path}};
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "FindAssetData", params));
        }});
}

// ---------------------------------------------------------------------------
// Level / viewport / PIE tools. Level + viewport ops use modern (UE5
// subsystem) -> legacy (UE4 EditorLevelLibrary) fallback. PIE control lives on
// ULevelEditorSubsystem (UE5 only) and is gated by Capability::PieControl, so
// it auto-degrades to "unsupported" on UE4.
// ---------------------------------------------------------------------------
void ToolRegistry::register_level_tools() {
    // -- ue_save_current_level ------------------------------------------------
    add(Tool{
        "ue_save_current_level",
        "Save the current editor level. Uses LevelEditorSubsystem on UE5, "
        "EditorLevelLibrary on UE4.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(call_modern_then_legacy(ctx, kLevelEditorSubsystem,
                                                   kEditorLevelLib, "SaveCurrentLevel"));
        }});

    // -- ue_load_level --------------------------------------------------------
    add(Tool{
        "ue_load_level",
        "Open a level/map by asset path, e.g. \"/Game/Maps/MyMap\". Uses "
        "LevelEditorSubsystem on UE5, EditorLevelLibrary on UE4.",
        json{{"type", "object"},
             {"properties", {{"assetPath", {{"type", "string"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            json params = {{"AssetPath", path}};
            return from_rc(call_modern_then_legacy(ctx, kLevelEditorSubsystem,
                                                   kEditorLevelLib, "LoadLevel", params));
        }});

    // -- ue_new_level ---------------------------------------------------------
    add(Tool{
        "ue_new_level",
        "Create a new empty level at assetPath, e.g. \"/Game/Maps/New\". Uses "
        "LevelEditorSubsystem on UE5, EditorLevelLibrary on UE4.",
        json{{"type", "object"},
             {"properties", {{"assetPath", {{"type", "string"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            json params = {{"AssetPath", path}};
            // bIsPartitionedWorld is a 5.5+ parameter; RC ignores unknown keys
            // on older engines, but we only add it when the engine is >= 5.5 to
            // avoid surprising behavior.
            const EngineVersion& v = ctx.caps.engine_version();
            if (v.known() && v.at_least(5, 5)) params["bIsPartitionedWorld"] = false;
            return from_rc(call_modern_then_legacy(ctx, kLevelEditorSubsystem,
                                                   kEditorLevelLib, "NewLevel", params, true));
        }});

    // -- ue_get_viewport_camera / ue_set_viewport_camera ---------------------
    add(Tool{
        "ue_get_viewport_camera",
        "Read the active editor level viewport camera location and rotation. "
        "Uses UnrealEditorSubsystem on UE5, EditorLevelLibrary on UE4.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(call_modern_then_legacy(ctx, kUnrealEditorSubsystem,
                                                   kEditorLevelLib, "GetLevelViewportCameraInfo"));
        }});
    add(Tool{
        "ue_set_viewport_camera",
        "Move the active editor level viewport camera. Pass location {x,y,z} "
        "and rotation {pitch,yaw,roll}. Uses UnrealEditorSubsystem on UE5, "
        "EditorLevelLibrary on UE4.",
        json{{"type", "object"},
             {"properties",
              {{"location", {{"type", "object"}}}, {"rotation", {{"type", "object"}}}}},
             {"required", json::array({"location", "rotation"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            json params = {{"CameraLocation", xyz(args.value("location", json::object()))},
                           {"CameraRotation", pyr(args.value("rotation", json::object()))}};
            return from_rc(call_modern_then_legacy(ctx, kUnrealEditorSubsystem,
                                                   kEditorLevelLib, "SetLevelViewportCameraInfo",
                                                   params));
        }});

    // -- ue_take_screenshot ---------------------------------------------------
    add(Tool{
        "ue_take_screenshot",
        "Take a high-resolution editor screenshot saved under the project's "
        "Saved/Screenshots/HighRes folder. Optional resX/resY/filename. Uses "
        "AutomationBlueprintFunctionLibrary (editor, 4.25 -> 5.8).",
        json{{"type", "object"},
             {"properties",
              {{"resX", {{"type", "integer"}}},
               {"resY", {{"type", "integer"}}},
               {"filename", {{"type", "string"}}}}}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            json params = {
                {"ResX", args.value("resX", 1920)},
                {"ResY", args.value("resY", 1080)},
                {"Filename", arg_str(args, "filename", "")},
            };
            return from_rc(ctx.rc.call_function(kAutomationLib, "TakeHighResScreenshot", params));
        }});

    // -- ue_is_pie ------------------------------------------------------------
    add(Tool{
        "ue_is_pie",
        "Report whether the editor is currently in Play-In-Editor "
        "(LevelEditorSubsystem.IsInPlayInEditor). UE 5.x only; returns "
        "'unsupported' on UE4.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::PieControl},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(ctx.rc.call_function(kLevelEditorSubsystem, "IsInPlayInEditor"));
        }});

    // -- ue_stop_pie ----------------------------------------------------------
    add(Tool{
        "ue_stop_pie",
        "Request the editor end any active Play-In-Editor session "
        "(LevelEditorSubsystem.EditorRequestEndPlay). UE 5.x only; returns "
        "'unsupported' on UE4.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::PieControl},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(ctx.rc.call_function(kLevelEditorSubsystem, "EditorRequestEndPlay"));
        }});
}

// ---------------------------------------------------------------------------
// Raw RemoteControl route tools (no engine UFunction): property array
// operations (5.0+) and preset access (4.26+). These hit /remote/* routes
// directly via RcClient::request.
// ---------------------------------------------------------------------------
namespace {
// Minimal percent-encoding for a single URL path segment (preset/property
// names may contain spaces or reserved characters). Encodes everything that
// isn't an RFC 3986 unreserved character.
std::string url_encode_segment(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}
}  // namespace

void ToolRegistry::register_rc_route_tools() {
    // -- ue_property_array_append --------------------------------------------
    add(Tool{
        "ue_property_array_append",
        "Append an element to an array property on a remote object "
        "(/remote/object/property/append). Requires UE 5.0+; older engines "
        "return 'unsupported'. value is the element itself (not wrapped in an "
        "array).",
        json{{"type", "object"},
             {"properties",
              {{"objectPath", {{"type", "string"}}},
               {"propertyName", {{"type", "string"}}},
               {"value", {{"description", "the new array element (any JSON type)"}}},
               {"generateTransaction", {{"type", "boolean"}}}}},
             {"required", json::array({"objectPath", "propertyName", "value"})}},
        {Capability::PropertyArrayOps},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string obj = arg_str(args, "objectPath");
            const std::string prop = arg_str(args, "propertyName");
            if (obj.empty() || prop.empty() || !args.contains("value"))
                return ToolResult::error("objectPath, propertyName and value are required");
            json body = {{"ObjectPath", obj},
                         {"PropertyName", prop},
                         {"PropertyValue", args["value"]},
                         {"GenerateTransaction", args.value("generateTransaction", true)}};
            return from_rc(ctx.rc.request("PUT", "/remote/object/property/append", body));
        }});

    // -- ue_property_array_insert / ue_property_array_remove -----------------
    // Both take the index as a URL query parameter (?index=N), not in the body.
    add(Tool{
        "ue_property_array_insert",
        "Insert an element at an index in an array property "
        "(/remote/object/property/insert?index=N). Requires UE 5.0+.",
        json{{"type", "object"},
             {"properties",
              {{"objectPath", {{"type", "string"}}},
               {"propertyName", {{"type", "string"}}},
               {"index", {{"type", "integer"}}},
               {"value", {{"description", "the new array element"}}},
               {"generateTransaction", {{"type", "boolean"}}}}},
             {"required", json::array({"objectPath", "propertyName", "index", "value"})}},
        {Capability::PropertyArrayOps},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string obj = arg_str(args, "objectPath");
            const std::string prop = arg_str(args, "propertyName");
            if (obj.empty() || prop.empty() || !args.contains("value"))
                return ToolResult::error("objectPath, propertyName and value are required");
            const int index = args.value("index", 0);
            json body = {{"ObjectPath", obj},
                         {"PropertyName", prop},
                         {"PropertyValue", args["value"]},
                         {"GenerateTransaction", args.value("generateTransaction", true)}};
            std::string path = "/remote/object/property/insert?index=" + std::to_string(index);
            return from_rc(ctx.rc.request("PUT", path, body));
        }});
    add(Tool{
        "ue_property_array_remove",
        "Remove the element at an index from an array property "
        "(/remote/object/property/remove?index=N). Requires UE 5.0+.",
        json{{"type", "object"},
             {"properties",
              {{"objectPath", {{"type", "string"}}},
               {"propertyName", {{"type", "string"}}},
               {"index", {{"type", "integer"}}},
               {"generateTransaction", {{"type", "boolean"}}}}},
             {"required", json::array({"objectPath", "propertyName", "index"})}},
        {Capability::PropertyArrayOps},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string obj = arg_str(args, "objectPath");
            const std::string prop = arg_str(args, "propertyName");
            if (obj.empty() || prop.empty())
                return ToolResult::error("objectPath and propertyName are required");
            const int index = args.value("index", 0);
            json body = {{"ObjectPath", obj},
                         {"PropertyName", prop},
                         {"GenerateTransaction", args.value("generateTransaction", true)}};
            std::string path = "/remote/object/property/remove?index=" + std::to_string(index);
            return from_rc(ctx.rc.request("PUT", path, body));
        }});

    // -- ue_list_presets ------------------------------------------------------
    add(Tool{
        "ue_list_presets",
        "List all RemoteControl presets in the project (GET /remote/presets). "
        "Requires UE 4.26+.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::Presets},
        [](ToolContext& ctx, const json&) -> ToolResult {
            return from_rc(ctx.rc.request("GET", "/remote/presets"));
        }});

    // -- ue_get_preset --------------------------------------------------------
    add(Tool{
        "ue_get_preset",
        "Get a RemoteControl preset's full description, including its exposed "
        "properties, functions and actors (GET /remote/preset/:preset). preset "
        "is the preset name or id. Requires UE 4.26+.",
        json{{"type", "object"},
             {"properties", {{"preset", {{"type", "string"}}}}},
             {"required", json::array({"preset"})}},
        {Capability::Presets},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string preset = arg_str(args, "preset");
            if (preset.empty()) return ToolResult::error("preset is required");
            return from_rc(ctx.rc.request("GET", "/remote/preset/" + url_encode_segment(preset)));
        }});

    // -- ue_preset_call_function ---------------------------------------------
    add(Tool{
        "ue_preset_call_function",
        "Invoke a function exposed on a preset by its label "
        "(PUT /remote/preset/:preset/function/:label). Pass preset, label, and "
        "optional parameters. Requires UE 4.26+.",
        json{{"type", "object"},
             {"properties",
              {{"preset", {{"type", "string"}}},
               {"label", {{"type", "string"}, {"description", "the exposed function label"}}},
               {"parameters", {{"type", "object"}}},
               {"generateTransaction", {{"type", "boolean"}}}}},
             {"required", json::array({"preset", "label"})}},
        {Capability::Presets},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string preset = arg_str(args, "preset");
            const std::string label = arg_str(args, "label");
            if (preset.empty() || label.empty())
                return ToolResult::error("preset and label are required");
            json body = {
                {"Parameters", args.contains("parameters") && args["parameters"].is_object()
                                   ? args["parameters"]
                                   : json::object()},
                {"GenerateTransaction", args.value("generateTransaction", true)}};
            std::string path = "/remote/preset/" + url_encode_segment(preset) +
                               "/function/" + url_encode_segment(label);
            return from_rc(ctx.rc.request("PUT", path, body));
        }});

    // -- ue_preset_get_property / ue_preset_set_property ---------------------
    add(Tool{
        "ue_preset_get_property",
        "Read an exposed property value on a preset by its label "
        "(GET /remote/preset/:preset/property/:label). Requires UE 4.26+.",
        json{{"type", "object"},
             {"properties",
              {{"preset", {{"type", "string"}}}, {"label", {{"type", "string"}}}}},
             {"required", json::array({"preset", "label"})}},
        {Capability::Presets},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string preset = arg_str(args, "preset");
            const std::string label = arg_str(args, "label");
            if (preset.empty() || label.empty())
                return ToolResult::error("preset and label are required");
            std::string path = "/remote/preset/" + url_encode_segment(preset) +
                               "/property/" + url_encode_segment(label);
            return from_rc(ctx.rc.request("GET", path));
        }});
    add(Tool{
        "ue_preset_set_property",
        "Write an exposed property value on a preset by its label "
        "(PUT /remote/preset/:preset/property/:label). Requires UE 4.26+.",
        json{{"type", "object"},
             {"properties",
              {{"preset", {{"type", "string"}}},
               {"label", {{"type", "string"}}},
               {"value", {{"description", "the new property value"}}},
               {"generateTransaction", {{"type", "boolean"}}}}},
             {"required", json::array({"preset", "label", "value"})}},
        {Capability::Presets},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string preset = arg_str(args, "preset");
            const std::string label = arg_str(args, "label");
            if (preset.empty() || label.empty() || !args.contains("value"))
                return ToolResult::error("preset, label and value are required");
            json body = {{"PropertyValue", args["value"]},
                         {"GenerateTransaction", args.value("generateTransaction", true)}};
            std::string path = "/remote/preset/" + url_encode_segment(preset) +
                               "/property/" + url_encode_segment(label);
            return from_rc(ctx.rc.request("PUT", path, body));
        }});
}

// ---------------------------------------------------------------------------
// Scene introspection tools. These are the antidote to "guess the scene from a
// screenshot": they let an agent directly enumerate actors by class/label,
// walk an actor's components, and read its bounds. find-by-class composes
// GetAllLevelActors (modern/legacy) with EditorFilterLibrary.ByClass, which is
// present and NOT remote-blocked 4.25 -> 5.x.
// ---------------------------------------------------------------------------
namespace {
// Extract a string array out of an RcResult's {"ReturnValue":[...]} body.
// Returns an empty optional if the shape isn't as expected.
bool extract_return_array(const RcResult& r, json& out) {
    if (!r.ok || !r.body.is_object() || !r.body.contains("ReturnValue") ||
        !r.body["ReturnValue"].is_array())
        return false;
    out = r.body["ReturnValue"];
    return true;
}
}  // namespace

void ToolRegistry::register_introspection_tools() {
    // -- ue_find_actors_by_class ---------------------------------------------
    add(Tool{
        "ue_find_actors_by_class",
        "Find all actors in the level whose class matches actorClass (an object "
        "path, e.g. /Script/Landscape.Landscape, /Script/Engine.StaticMeshActor, "
        "or a Blueprint class path). Composes GetAllLevelActors with "
        "EditorFilterLibrary.ByClass so it works 4.25 -> 5.x without a "
        "show/hide-and-screenshot search. Returns the matching actor paths.",
        json{{"type", "object"},
             {"properties",
              {{"actorClass", {{"type", "string"}, {"description", "class object path to match"}}}}},
             {"required", json::array({"actorClass"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string cls = arg_str(args, "actorClass");
            if (cls.empty()) return ToolResult::error("actorClass is required");
            RcResult all = call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "GetAllLevelActors");
            json actors;
            if (!extract_return_array(all, actors))
                return from_rc(all);  // surface whatever went wrong
            json params = {{"TargetArray", actors}, {"ObjectClass", cls},
                           {"FilterType", "Include"}};
            RcResult filtered = ctx.rc.call_function(kEditorFilterLib, "ByClass", params);
            json matched;
            if (!extract_return_array(filtered, matched)) return from_rc(filtered);
            return ToolResult::ok(json{{"status", "ok"},
                                       {"count", static_cast<int>(matched.size())},
                                       {"actors", matched}});
        }});

    // -- ue_find_actors_by_label ---------------------------------------------
    add(Tool{
        "ue_find_actors_by_label",
        "Find actors whose editor label (World Outliner name) matches a string. "
        "match: Contains (default), MatchesWildcard, or ExactMatch. Composes "
        "GetAllLevelActors with EditorFilterLibrary.ByActorLabel. Works "
        "4.25 -> 5.x.",
        json{{"type", "object"},
             {"properties",
              {{"label", {{"type", "string"}}},
               {"match", {{"type", "string"}, {"enum", json::array({"Contains", "MatchesWildcard", "ExactMatch"})}}},
               {"ignoreCase", {{"type", "boolean"}, {"description", "default true"}}}}},
             {"required", json::array({"label"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string label = arg_str(args, "label");
            if (label.empty()) return ToolResult::error("label is required");
            RcResult all = call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "GetAllLevelActors");
            json actors;
            if (!extract_return_array(all, actors)) return from_rc(all);
            json params = {{"TargetArray", actors},
                           {"NameSubString", label},
                           {"StringMatch", arg_str(args, "match", "Contains")},
                           {"FilterType", "Include"},
                           {"bIgnoreCase", args.value("ignoreCase", true)}};
            RcResult filtered = ctx.rc.call_function(kEditorFilterLib, "ByActorLabel", params);
            json matched;
            if (!extract_return_array(filtered, matched)) return from_rc(filtered);
            return ToolResult::ok(json{{"status", "ok"},
                                       {"count", static_cast<int>(matched.size())},
                                       {"actors", matched}});
        }});

    // -- ue_get_actor_components ---------------------------------------------
    add(Tool{
        "ue_get_actor_components",
        "List an actor's components, optionally filtered to a component class "
        "(componentClass object path, default /Script/Engine.ActorComponent for "
        "all). actorPath is an actor instance path from ue_list_actors or a "
        "find tool. Calls AActor.K2_GetComponentsByClass (stable 4.25 -> 5.x). "
        "Returns the component object paths — feed these to ue_set_material_param "
        "or ue_get_property.",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"componentClass", {{"type", "string"}, {"description", "default /Script/Engine.ActorComponent"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            json params = {{"ComponentClass",
                            arg_str(args, "componentClass", "/Script/Engine.ActorComponent")}};
            RcResult r = ctx.rc.call_function(actor, "K2_GetComponentsByClass", params);
            json comps;
            if (!extract_return_array(r, comps)) return from_rc(r);
            return ToolResult::ok(json{{"status", "ok"},
                                       {"count", static_cast<int>(comps.size())},
                                       {"components", comps}});
        }});

    // -- ue_get_actor_bounds -------------------------------------------------
    add(Tool{
        "ue_get_actor_bounds",
        "Return an actor's world-space bounding box (origin + box extent). "
        "Useful for framing the viewport camera or understanding scale without a "
        "screenshot. Calls AActor.GetActorBounds (4.25 -> 5.x). "
        "onlyColliding restricts to collision-enabled components (default false).",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"onlyColliding", {{"type", "boolean"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            json params = {{"bOnlyCollidingComponents", args.value("onlyColliding", false)}};
            return from_rc(ctx.rc.call_function(actor, "GetActorBounds", params));
        }});

    // -- ue_get_actor_reference ----------------------------------------------
    add(Tool{
        "ue_get_actor_reference",
        "Resolve an actor object reference from a path/name string "
        "(EditorActorSubsystem.GetActorReference on UE5). Handy to turn a label "
        "or partial path into a concrete actor path. UE5 subsystem; on UE4 use "
        "ue_find_actors_by_label instead.",
        json{{"type", "object"},
             {"properties", {{"pathToActor", {{"type", "string"}}}}},
             {"required", json::array({"pathToActor"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string p = arg_str(args, "pathToActor");
            if (p.empty()) return ToolResult::error("pathToActor is required");
            return from_rc(ctx.rc.call_function(kEditorActorSubsystem,
                                                "GetActorReference", json{{"PathToActor", p}}));
        }});
}

// ---------------------------------------------------------------------------
// Material + visual tools. ue_set_material_param edits a component's material
// at runtime by creating a Dynamic Material Instance on the chosen element and
// setting a scalar/vector/texture parameter on it — the direct fix for tasks
// like "make the terrain greener". ue_get_object_thumbnail returns rendered
// pixels (base64 PNG) so a vision client can confirm a result.
// ---------------------------------------------------------------------------
namespace {
// Standard base64 encoder lives at the top of this file (shared with the
// Python-recipe harness). Detect an image's media type from its leading magic
// bytes. RemoteControl
// sometimes mislabels the Content-Type (e.g. UE 5.3 returns an SVG placeholder
// icon for assets without a rendered thumbnail but still tags it image/png), so
// we trust the bytes over the header. Returns an empty string if unrecognized.
std::string sniff_image_mime(const std::string& d) {
    auto starts = [&](const char* sig, size_t n) {
        return d.size() >= n && std::memcmp(d.data(), sig, n) == 0;
    };
    if (starts("\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (starts("\xFF\xD8\xFF", 3)) return "image/jpeg";
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) return "image/gif";
    if (starts("BM", 2)) return "image/bmp";
    // WEBP: "RIFF"...."WEBP"
    if (d.size() >= 12 && std::memcmp(d.data(), "RIFF", 4) == 0 &&
        std::memcmp(d.data() + 8, "WEBP", 4) == 0)
        return "image/webp";
    // SVG: text starting with "<svg" or an XML prologue that contains <svg.
    {
        size_t j = 0;
        while (j < d.size() && (d[j] == ' ' || d[j] == '\t' || d[j] == '\r' ||
                                d[j] == '\n' || d[j] == '\xEF' || d[j] == '\xBB' ||
                                d[j] == '\xBF'))
            ++j;
        if (d.compare(j, 4, "<svg") == 0) return "image/svg+xml";
        if (d.compare(j, 5, "<?xml") == 0 && d.find("<svg") != std::string::npos)
            return "image/svg+xml";
    }
    return "";
}
}  // namespace

void ToolRegistry::register_material_tools() {
    // -- ue_set_material_param -----------------------------------------------
    add(Tool{
        "ue_set_material_param",
        "Set a material parameter on a primitive component at runtime. Creates a "
        "Dynamic Material Instance on the given element index (so the change is "
        "live and undoable) and sets a scalar, vector (color), or texture "
        "parameter on it. componentPath is a primitive/mesh component path (from "
        "ue_get_actor_components). paramType: scalar | vector | texture. For "
        "scalar pass value (number); for vector pass color {r,g,b,a}; for "
        "texture pass textureValue (an asset path). This is the precise way to "
        "e.g. recolor a mesh or landscape material without trial and error.",
        json{{"type", "object"},
             {"properties",
              {{"componentPath", {{"type", "string"}}},
               {"elementIndex", {{"type", "integer"}, {"description", "material slot (default 0)"}}},
               {"paramName", {{"type", "string"}}},
               {"paramType", {{"type", "string"}, {"enum", json::array({"scalar", "vector", "texture"})}}},
               {"value", {{"type", "number"}, {"description", "scalar value"}}},
               {"color", {{"type", "object"}, {"properties", {{"r", {{"type", "number"}}}, {"g", {{"type", "number"}}}, {"b", {{"type", "number"}}}, {"a", {{"type", "number"}}}}}}},
               {"textureValue", {{"type", "string"}, {"description", "texture asset path"}}}}},
             {"required", json::array({"componentPath", "paramName", "paramType"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string comp = arg_str(args, "componentPath");
            const std::string param = arg_str(args, "paramName");
            const std::string type = arg_str(args, "paramType");
            if (comp.empty() || param.empty() || type.empty())
                return ToolResult::error("componentPath, paramName and paramType are required");
            const int element = args.value("elementIndex", 0);
            // 1) Create (or fetch) a dynamic material instance on the element.
            RcResult mid = ctx.rc.call_function(
                comp, "CreateDynamicMaterialInstance",
                json{{"ElementIndex", element}}, /*tx=*/true);
            if (!mid.ok || !mid.body.is_object() || !mid.body.contains("ReturnValue") ||
                !mid.body["ReturnValue"].is_string() ||
                mid.body["ReturnValue"].get<std::string>().empty()) {
                return from_rc(mid, json{{"hint",
                    "could not create a dynamic material instance on this element; "
                    "is componentPath a primitive component with a material slot?"}});
            }
            const std::string mid_path = mid.body["ReturnValue"].get<std::string>();
            // 2) Set the requested parameter on the MID.
            RcResult set;
            if (type == "scalar") {
                if (!args.contains("value"))
                    return ToolResult::error("value is required for paramType=scalar");
                set = ctx.rc.call_function(mid_path, "SetScalarParameterValue",
                                           json{{"ParameterName", param},
                                                {"Value", args["value"]}}, true);
            } else if (type == "vector") {
                json c = args.value("color", json::object());
                json color = {{"R", c.value("r", 0.0)}, {"G", c.value("g", 0.0)},
                              {"B", c.value("b", 0.0)}, {"A", c.value("a", 1.0)}};
                set = ctx.rc.call_function(mid_path, "SetVectorParameterValue",
                                           json{{"ParameterName", param},
                                                {"Value", color}}, true);
            } else if (type == "texture") {
                const std::string tex = arg_str(args, "textureValue");
                if (tex.empty())
                    return ToolResult::error("textureValue is required for paramType=texture");
                set = ctx.rc.call_function(mid_path, "SetTextureParameterValue",
                                           json{{"ParameterName", param},
                                                {"Value", tex}}, true);
            } else {
                return ToolResult::error("paramType must be scalar, vector or texture");
            }
            return from_rc(set, json{{"materialInstance", mid_path}});
        }});

    // -- ue_set_material_instance_param --------------------------------------
    add(Tool{
        "ue_set_material_instance_param",
        "Set a parameter on a Material Instance Constant ASSET (persisted), via "
        "MaterialEditingLibrary.SetMaterialInstance{Scalar,Vector}ParameterValue. "
        "instancePath is the MIC asset path. paramType scalar|vector. Unlike "
        "ue_set_material_param (which edits a live actor's dynamic instance), "
        "this edits the saved asset. Requires the asset be a "
        "UMaterialInstanceConstant. Works 4.25 -> 5.x.",
        json{{"type", "object"},
             {"properties",
              {{"instancePath", {{"type", "string"}}},
               {"paramName", {{"type", "string"}}},
               {"paramType", {{"type", "string"}, {"enum", json::array({"scalar", "vector"})}}},
               {"value", {{"type", "number"}}},
               {"color", {{"type", "object"}}}}},
             {"required", json::array({"instancePath", "paramName", "paramType"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string inst = arg_str(args, "instancePath");
            const std::string param = arg_str(args, "paramName");
            const std::string type = arg_str(args, "paramType");
            if (inst.empty() || param.empty() || type.empty())
                return ToolResult::error("instancePath, paramName and paramType are required");
            if (type == "scalar") {
                if (!args.contains("value"))
                    return ToolResult::error("value is required for paramType=scalar");
                return from_rc(ctx.rc.call_function(
                    kMaterialEditingLib, "SetMaterialInstanceScalarParameterValue",
                    json{{"Instance", inst}, {"ParameterName", param},
                         {"Value", args["value"]}}, true));
            }
            if (type == "vector") {
                json c = args.value("color", json::object());
                json color = {{"R", c.value("r", 0.0)}, {"G", c.value("g", 0.0)},
                              {"B", c.value("b", 0.0)}, {"A", c.value("a", 1.0)}};
                return from_rc(ctx.rc.call_function(
                    kMaterialEditingLib, "SetMaterialInstanceVectorParameterValue",
                    json{{"Instance", inst}, {"ParameterName", param},
                         {"Value", color}}, true));
            }
            return ToolResult::error("paramType must be scalar or vector");
        }});

    // -- ue_get_object_thumbnail ---------------------------------------------
    add(Tool{
        "ue_get_object_thumbnail",
        "Render and return an object's thumbnail as an image "
        "(PUT /remote/object/thumbnail, editor, 4.26+). objectPath is an asset "
        "or actor path. Returns an image content block (PNG or JPEG, per the "
        "engine's Content-Type) a vision-capable client can see directly — use "
        "this to confirm a visual result instead of blindly editing. Returns "
        "'unsupported' on engines without the route.",
        json{{"type", "object"},
             {"properties", {{"objectPath", {{"type", "string"}}}}},
             {"required", json::array({"objectPath"})}},
        {Capability::Thumbnail},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string obj = arg_str(args, "objectPath");
            if (obj.empty()) return ToolResult::error("objectPath is required");
            RcBinaryResult r = ctx.rc.request_raw("PUT", "/remote/object/thumbnail",
                                                  json{{"ObjectPath", obj}});
            if (!r.ok)
                return ToolResult::error(r.error.empty() ? "thumbnail request failed" : r.error,
                                         json{{"httpStatus", r.status}});
            std::string mime = r.content_type.empty() ? "image/png" : r.content_type;
            // Content-Type may include parameters (e.g. "image/png; charset=..."):
            // keep only the media type for the MCP image block.
            auto semi = mime.find(';');
            if (semi != std::string::npos) mime = mime.substr(0, semi);
            // Trust the bytes over the header: some engine versions mislabel the
            // Content-Type (e.g. 5.3 returns an SVG placeholder tagged image/png
            // for assets lacking a rendered thumbnail). Correct it when the magic
            // bytes say otherwise so the client renders it right.
            std::string sniffed = sniff_image_mime(r.data);
            bool corrected = false;
            if (!sniffed.empty() && sniffed != mime) {
                mime = sniffed;
                corrected = true;
            }
            json summary = {{"objectPath", obj},
                            {"bytes", static_cast<int>(r.data.size())},
                            {"mimeType", mime}};
            if (corrected) {
                summary["serverContentType"] = r.content_type;
                // An SVG placeholder means the engine has no rendered thumbnail
                // for this object yet — flag it so the caller doesn't mistake the
                // generic icon for the asset's actual appearance.
                if (mime == "image/svg+xml")
                    summary["note"] =
                        "engine returned a placeholder icon (no rendered "
                        "thumbnail for this object); not the asset's appearance";
            }
            return ToolResult::image(base64_encode(r.data), mime, summary);
        }});
}

// ---------------------------------------------------------------------------
// Editor workflow + convenience tools: load an asset, spawn from an asset
// (e.g. drop a StaticMesh/Blueprint), pilot the viewport camera onto an actor,
// toggle game view, read console variables, and focus the viewport on an actor.
// All use stable UFunctions or the modern/legacy editor pair.
// ---------------------------------------------------------------------------
void ToolRegistry::register_workflow_tools() {
    // -- ue_load_asset --------------------------------------------------------
    add(Tool{
        "ue_load_asset",
        "Load an asset into memory and return its object path "
        "(EditorAssetLibrary/Subsystem.LoadAsset). assetPath e.g. "
        "\"/Game/Meshes/SM_Rock\". The returned path can be passed to "
        "ue_spawn_actor_from_asset or material tools. Works 4.25 -> 5.x.",
        json{{"type", "object"},
             {"properties", {{"assetPath", {{"type", "string"}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string path = arg_str(args, "assetPath");
            if (path.empty()) return ToolResult::error("assetPath is required");
            return from_rc(call_modern_then_legacy(ctx, kEditorAssetSubsystem,
                                                   kEditorAssetLib, "LoadAsset",
                                                   json{{"AssetPath", path}}));
        }});

    // -- ue_spawn_actor_from_asset -------------------------------------------
    add(Tool{
        "ue_spawn_actor_from_asset",
        "Spawn an actor in the current level from an ASSET (e.g. a StaticMesh, "
        "Blueprint, or particle system) at an optional location/rotation. Uses "
        "EditorActorSubsystem.SpawnActorFromObject on UE5, EditorLevelLibrary on "
        "UE4. assetPath is a content path; the editor picks an appropriate actor "
        "type for it (StaticMeshActor for a mesh, etc.).",
        json{{"type", "object"},
             {"properties",
              {{"assetPath", {{"type", "string"}}},
               {"location", {{"type", "object"}, {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}}},
               {"rotation", {{"type", "object"}, {"properties", {{"pitch", {{"type", "number"}}}, {"yaw", {{"type", "number"}}}, {"roll", {{"type", "number"}}}}}}}}},
             {"required", json::array({"assetPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string asset = arg_str(args, "assetPath");
            if (asset.empty()) return ToolResult::error("assetPath is required");
            json params = {{"ObjectToUse", asset},
                           {"Location", xyz(args.value("location", json::object()))},
                           {"Rotation", pyr(args.value("rotation", json::object()))}};
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "SpawnActorFromObject",
                                                   params, /*tx=*/true));
        }});

    // -- ue_set_actor_transform ----------------------------------------------
    add(Tool{
        "ue_set_actor_transform",
        "Set an actor's full world transform in one call via "
        "EditorActorSubsystem.SetActorTransform (UE5) / EditorLevelLibrary (UE4). "
        "Pass actorPath, location {x,y,z}, rotation {pitch,yaw,roll} (degrees), "
        "and scale {x,y,z}. Any omitted component defaults (0 loc/rot, 1 scale). "
        "Prefer this over the per-axis setters when changing several at once.",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"location", {{"type", "object"}}},
               {"rotation", {{"type", "object"}}},
               {"scale", {{"type", "object"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            // FTransform over RC takes a quaternion rotation, which is awkward
            // for Euler input. Instead apply the three decomposed setters on the
            // actor instance (each a stable UFunction 4.25 -> 5.x).
            json results = json::object();
            bool any_err = false;
            if (args.contains("location")) {
                RcResult r = ctx.rc.call_function(actor, "K2_SetActorLocation",
                    json{{"NewLocation", xyz(args["location"])}, {"bSweep", false}, {"bTeleport", true}}, true);
                results["location"] = r.ok ? "ok" : r.error;
                any_err = any_err || !r.ok;
            }
            if (args.contains("rotation")) {
                RcResult r = ctx.rc.call_function(actor, "K2_SetActorRotation",
                    json{{"NewRotation", pyr(args["rotation"])}, {"bTeleportPhysics", true}}, true);
                results["rotation"] = r.ok ? "ok" : r.error;
                any_err = any_err || !r.ok;
            }
            if (args.contains("scale")) {
                RcResult r = ctx.rc.call_function(actor, "SetActorScale3D",
                    json{{"NewScale3D", xyz(args["scale"], 1.0, 1.0, 1.0)}}, true);
                results["scale"] = r.ok ? "ok" : r.error;
                any_err = any_err || !r.ok;
            }
            if (results.empty())
                return ToolResult::error("provide at least one of location, rotation, scale");
            json p = {{"status", any_err ? "error" : "ok"}, {"applied", results}};
            return any_err ? ToolResult{true, p} : ToolResult::ok(p);
        }});

    // -- ue_focus_actor / ue_pilot_actor -------------------------------------
    add(Tool{
        "ue_focus_viewport_on_actor",
        "Move the editor viewport camera to frame an actor, by reading its "
        "bounds and pointing the camera at it. actorPath is an actor instance "
        "path. Uses GetActorBounds + SetLevelViewportCameraInfo. distance "
        "multiplies the bounds radius for camera pull-back (default 2.5).",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"distance", {{"type", "number"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            RcResult b = ctx.rc.call_function(actor, "GetActorBounds",
                                              json{{"bOnlyCollidingComponents", false}});
            if (!b.ok || !b.body.is_object() || !b.body.contains("Origin"))
                return from_rc(b, json{{"hint", "could not read actor bounds"}});
            json origin = b.body["Origin"];
            json extent = b.body.value("BoxExtent", json{{"X", 100}, {"Y", 100}, {"Z", 100}});
            double ex = extent.value("X", 100.0), ey = extent.value("Y", 100.0),
                   ez = extent.value("Z", 100.0);
            double radius = std::sqrt(ex * ex + ey * ey + ez * ez);
            double dist = args.value("distance", 2.5) * (radius > 1.0 ? radius : 100.0);
            // Place the camera back along -X/-Y/+Z looking down at the origin.
            json cam_loc = {{"X", origin.value("X", 0.0) - dist},
                            {"Y", origin.value("Y", 0.0) - dist},
                            {"Z", origin.value("Z", 0.0) + dist}};
            // Yaw 45 toward +X+Y, pitch down ~ -35.
            json cam_rot = {{"Pitch", -35.0}, {"Yaw", 45.0}, {"Roll", 0.0}};
            RcResult set = call_modern_then_legacy(
                ctx, kUnrealEditorSubsystem, kEditorLevelLib, "SetLevelViewportCameraInfo",
                json{{"CameraLocation", cam_loc}, {"CameraRotation", cam_rot}});
            return from_rc(set, json{{"framedActor", actor}, {"cameraLocation", cam_loc}});
        }});

    // -- ue_set_game_view -----------------------------------------------------
    add(Tool{
        "ue_set_game_view",
        "Toggle the editor viewport between Game View (hides editor-only "
        "billboards/icons) and editor view. Uses LevelEditorSubsystem on UE5, "
        "EditorLevelLibrary on UE4. Pass gameView (boolean).",
        json{{"type", "object"},
             {"properties", {{"gameView", {{"type", "boolean"}}}}},
             {"required", json::array({"gameView"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            json params = {{"bGameView", args.value("gameView", true)}};
            return from_rc(call_modern_then_legacy(ctx, kLevelEditorSubsystem,
                                                   kEditorLevelLib, "EditorSetGameView", params));
        }});

    // -- ue_get_console_variable ---------------------------------------------
    add(Tool{
        "ue_get_console_variable",
        "Read the value of a console variable (CVar) such as r.ScreenPercentage "
        "or r.ShadowQuality. type: float (default), int, bool, or string. Uses "
        "KismetSystemLibrary.GetConsoleVariable*Value (4.25 -> 5.x).",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"type", {{"type", "string"}, {"enum", json::array({"float", "int", "bool", "string"})}}}}},
             {"required", json::array({"name"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string name = arg_str(args, "name");
            if (name.empty()) return ToolResult::error("name is required");
            const std::string t = arg_str(args, "type", "float");
            const char* fn = "GetConsoleVariableFloatValue";
            if (t == "int") fn = "GetConsoleVariableIntValue";
            else if (t == "bool") fn = "GetConsoleVariableBoolValue";
            else if (t == "string") fn = "GetConsoleVariableStringValue";
            return from_rc(ctx.rc.call_function(kKismetSystemLib, fn,
                                                json{{"VariableName", name}}));
        }});
}

// ---------------------------------------------------------------------------
// Layer 1 — scene/actor authoring. Batch spawn (one /remote/batch round-trip
// instead of N calls — the antidote to the per-actor latency that makes
// "build a town" painfully slow), plus duplicate/attach/detach/folders and a
// batched whole-scene transform read. All use stable UFunctions or the
// modern/legacy editor pair, so they work 4.25 -> 5.x (batch needs 4.26+).
// ---------------------------------------------------------------------------
void ToolRegistry::register_scene_tools() {
    // -- ue_batch_spawn_actors ------------------------------------------------
    add(Tool{
        "ue_batch_spawn_actors",
        "Spawn many actors in one RemoteControl round-trip (/remote/batch). Pass "
        "actors: an array of {actorClass, location?{x,y,z}, rotation?"
        "{pitch,yaw,roll}}. Far faster than calling ue_spawn_actor N times when "
        "building a scene (town, maze, grid). Requires UE 4.26+ (batch route); on "
        "4.25 returns 'unsupported'. Each sub-request targets EditorActorSubsystem "
        "(UE5) — on UE4.26 use ue_spawn_actor per actor instead.",
        json{{"type", "object"},
             {"properties",
              {{"actors",
                {{"type", "array"},
                 {"items",
                  {{"type", "object"},
                   {"properties",
                    {{"actorClass", {{"type", "string"}}},
                     {"location", {{"type", "object"}}},
                     {"rotation", {{"type", "object"}}}}}}}}}}},
             {"required", json::array({"actors"})}},
        {Capability::Batch},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            if (!args.contains("actors") || !args["actors"].is_array())
                return ToolResult::error("actors array is required");
            json reqs = json::array();
            int id = 1;
            for (const auto& a : args["actors"]) {
                json params = {
                    {"ActorClass", a.value("actorClass", std::string())},
                    {"Location", xyz(a.value("location", json::object()))},
                    {"Rotation", pyr(a.value("rotation", json::object()))},
                };
                reqs.push_back({
                    {"RequestId", id++},
                    {"URL", "/remote/object/call"},
                    {"Verb", "PUT"},
                    {"Body", {{"ObjectPath", kEditorActorSubsystem},
                              {"FunctionName", "SpawnActorFromClass"},
                              {"Parameters", params},
                              {"GenerateTransaction", true}}},
                });
            }
            return from_rc(ctx.rc.request("PUT", "/remote/batch",
                                          json{{"Requests", reqs}}),
                           json{{"requested", static_cast<int>(args["actors"].size())}});
        }});

    // -- ue_duplicate_actor ---------------------------------------------------
    add(Tool{
        "ue_duplicate_actor",
        "Duplicate an existing level actor (optionally offset). Pass actorPath "
        "and optional offset {x,y,z}. Uses EditorActorSubsystem.DuplicateActor on "
        "UE5, EditorLevelLibrary on UE4. Returns the new actor path.",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"offset", {{"type", "object"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            json params = {{"ActorToDuplicate", actor},
                           {"OffsetLocation", xyz(args.value("offset", json::object()))}};
            return from_rc(call_modern_then_legacy(ctx, kEditorActorSubsystem,
                                                   kEditorLevelLib, "DuplicateActor",
                                                   params, /*tx=*/true));
        }});

    // -- ue_attach_actor / ue_detach_actor -----------------------------------
    add(Tool{
        "ue_attach_actor",
        "Attach one actor to another (parent/child in the World Outliner and "
        "transform hierarchy). Pass childPath and parentPath. socketName and "
        "weldSimulatedBodies are optional. Calls AActor.K2_AttachToActor "
        "(4.25 -> 5.x) with KeepWorld transform rules.",
        json{{"type", "object"},
             {"properties",
              {{"childPath", {{"type", "string"}}},
               {"parentPath", {{"type", "string"}}},
               {"socketName", {{"type", "string"}}}}},
             {"required", json::array({"childPath", "parentPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string child = arg_str(args, "childPath");
            const std::string parent = arg_str(args, "parentPath");
            if (child.empty() || parent.empty())
                return ToolResult::error("childPath and parentPath are required");
            json params = {{"ParentActor", parent},
                           {"SocketName", arg_str(args, "socketName", "")},
                           {"LocationRule", "KeepWorld"},
                           {"RotationRule", "KeepWorld"},
                           {"ScaleRule", "KeepWorld"},
                           {"bWeldSimulatedBodies", args.value("weldSimulatedBodies", false)}};
            return from_rc(ctx.rc.call_function(child, "K2_AttachToActor", params, true));
        }});
    add(Tool{
        "ue_detach_actor",
        "Detach an actor from its parent, keeping its current world transform. "
        "Pass actorPath. Calls AActor.K2_DetachFromActor (4.25 -> 5.x).",
        json{{"type", "object"},
             {"properties", {{"actorPath", {{"type", "string"}}}}},
             {"required", json::array({"actorPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            json params = {{"LocationRule", "KeepWorld"},
                           {"RotationRule", "KeepWorld"},
                           {"ScaleRule", "KeepWorld"}};
            return from_rc(ctx.rc.call_function(actor, "K2_DetachFromActor", params, true));
        }});

    // -- ue_set_actor_folder --------------------------------------------------
    add(Tool{
        "ue_set_actor_folder",
        "Set an actor's World Outliner folder path (organizes the outliner; e.g. "
        "\"Lights/Interior\"). Pass actorPath and folderPath. Sets the actor's "
        "FolderPath property via RemoteControl. Editor-only.",
        json{{"type", "object"},
             {"properties",
              {{"actorPath", {{"type", "string"}}},
               {"folderPath", {{"type", "string"}}}}},
             {"required", json::array({"actorPath", "folderPath"})}},
        {Capability::ObjectProperty},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string actor = arg_str(args, "actorPath");
            const std::string folder = arg_str(args, "folderPath");
            if (actor.empty()) return ToolResult::error("actorPath is required");
            return from_rc(ctx.rc.set_property(actor, "FolderPath", folder, true));
        }});
}

// ---------------------------------------------------------------------------
// Layer 1 — mesh + lighting. Swap a component's static mesh, assign a material
// asset to a slot (the basic counterpart to ue_set_material_param's dynamic
// instance), and edit light component properties (intensity/color/sun angle).
// These act on COMPONENT instance paths (from ue_get_actor_components) and use
// stable UFunctions / property writes, working 4.25 -> 5.x.
// ---------------------------------------------------------------------------
void ToolRegistry::register_mesh_light_tools() {
    // -- ue_set_static_mesh ---------------------------------------------------
    add(Tool{
        "ue_set_static_mesh",
        "Swap the mesh on a StaticMeshComponent. Pass componentPath (from "
        "ue_get_actor_components, class StaticMeshComponent) and meshPath (a "
        "StaticMesh asset path, e.g. /Engine/BasicShapes/Sphere.Sphere). Calls "
        "UStaticMeshComponent.SetStaticMesh (4.25 -> 5.x).",
        json{{"type", "object"},
             {"properties",
              {{"componentPath", {{"type", "string"}}},
               {"meshPath", {{"type", "string"}}}}},
             {"required", json::array({"componentPath", "meshPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string comp = arg_str(args, "componentPath");
            const std::string mesh = arg_str(args, "meshPath");
            if (comp.empty() || mesh.empty())
                return ToolResult::error("componentPath and meshPath are required");
            return from_rc(ctx.rc.call_function(comp, "SetStaticMesh",
                                                json{{"NewMesh", mesh}}, true));
        }});

    // -- ue_set_actor_material ------------------------------------------------
    add(Tool{
        "ue_set_actor_material",
        "Assign a material ASSET to a primitive component's material slot. Pass "
        "componentPath, elementIndex (default 0), and materialPath (a Material or "
        "MaterialInstance asset path). Unlike ue_set_material_param (which makes a "
        "live dynamic instance and tweaks a parameter), this sets the slot's "
        "material outright. Calls UPrimitiveComponent.SetMaterial (4.25 -> 5.x).",
        json{{"type", "object"},
             {"properties",
              {{"componentPath", {{"type", "string"}}},
               {"elementIndex", {{"type", "integer"}}},
               {"materialPath", {{"type", "string"}}}}},
             {"required", json::array({"componentPath", "materialPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string comp = arg_str(args, "componentPath");
            const std::string mat = arg_str(args, "materialPath");
            if (comp.empty() || mat.empty())
                return ToolResult::error("componentPath and materialPath are required");
            json params = {{"ElementIndex", args.value("elementIndex", 0)},
                           {"Material", mat}};
            return from_rc(ctx.rc.call_function(comp, "SetMaterial", params, true));
        }});

    // -- ue_set_light_property ------------------------------------------------
    add(Tool{
        "ue_set_light_property",
        "Set intensity and/or color on a LightComponent. Pass componentPath (a "
        "Light component path from ue_get_actor_components; class e.g. "
        "PointLightComponent, DirectionalLightComponent). intensity (number) and "
        "color {r,g,b} are both optional — pass whichever you want to change. "
        "Calls SetIntensity / SetLightColor (4.25 -> 5.x).",
        json{{"type", "object"},
             {"properties",
              {{"componentPath", {{"type", "string"}}},
               {"intensity", {{"type", "number"}}},
               {"color", {{"type", "object"}, {"properties", {{"r", {{"type", "number"}}}, {"g", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}}}}},
             {"required", json::array({"componentPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string comp = arg_str(args, "componentPath");
            if (comp.empty()) return ToolResult::error("componentPath is required");
            json applied = json::object();
            bool any = false, any_err = false;
            if (args.contains("intensity")) {
                RcResult r = ctx.rc.call_function(comp, "SetIntensity",
                                                  json{{"NewIntensity", args["intensity"]}}, true);
                applied["intensity"] = r.ok ? "ok" : r.error;
                any = true; any_err = any_err || !r.ok;
            }
            if (args.contains("color") && args["color"].is_object()) {
                const json& c = args["color"];
                json color = {{"R", c.value("r", 1.0)}, {"G", c.value("g", 1.0)},
                              {"B", c.value("b", 1.0)}, {"A", 1.0}};
                RcResult r = ctx.rc.call_function(comp, "SetLightColor",
                                                  json{{"NewLightColor", color},
                                                       {"bSRGB", true}}, true);
                applied["color"] = r.ok ? "ok" : r.error;
                any = true; any_err = any_err || !r.ok;
            }
            if (!any) return ToolResult::error("provide intensity and/or color");
            json p = {{"status", any_err ? "error" : "ok"}, {"applied", applied}};
            return any_err ? ToolResult{true, p} : ToolResult::ok(p);
        }});
}

// ---------------------------------------------------------------------------
// Layer 1 — asset/material creation + import. These can't be one UFunction
// call (they create a factory, run it, and save), so they're Python recipes run
// in a single ExecutePythonCommandEx round-trip via run_python_recipe. They
// require the PythonScriptPlugin (auto-degrade to 'unsupported' otherwise).
// ---------------------------------------------------------------------------
void ToolRegistry::register_creation_tools() {
    // -- ue_create_material ---------------------------------------------------
    add(Tool{
        "ue_create_material",
        "Create a new Material asset. Pass name and packagePath (e.g. "
        "\"/Game/Materials\"), and optional baseColor {r,g,b} to set the "
        "material's constant base color. Returns the created asset path. Runs as "
        "a single Python recipe; requires the PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"packagePath", {{"type", "string"}}},
               {"baseColor", {{"type", "object"}, {"properties", {{"r", {{"type", "number"}}}, {"g", {{"type", "number"}}}, {"b", {{"type", "number"}}}}}}}}},
             {"required", json::array({"name", "packagePath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
name = _ARGS['name']
pkg = _ARGS['packagePath']
tools = unreal.AssetToolsHelpers.get_asset_tools()
mat = tools.create_asset(name, pkg, unreal.Material, unreal.MaterialFactoryNew())
if mat is None:
    _emit({'ok': False, 'error': 'create_asset returned None'})
else:
    bc = _ARGS.get('baseColor')
    if bc is not None:
        node = unreal.MaterialEditingLibrary.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector)
        node.set_editor_property('constant', unreal.LinearColor(bc.get('r',0.5), bc.get('g',0.5), bc.get('b',0.5), 1.0))
        unreal.MaterialEditingLibrary.connect_material_property(node, '', unreal.MaterialProperty.MP_BASE_COLOR)
        unreal.MaterialEditingLibrary.recompile_material(mat)
    unreal.EditorAssetLibrary.save_asset(mat.get_path_name())
    _emit({'ok': True, 'assetPath': mat.get_path_name()})
)PY";
            const std::string name = arg_str(args, "name");
            const std::string pkg = arg_str(args, "packagePath");
            if (name.empty() || pkg.empty())
                return ToolResult::error("name and packagePath are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_create_material_instance -----------------------------------------
    add(Tool{
        "ue_create_material_instance",
        "Create a Material Instance Constant (MIC) asset parented to a material. "
        "Pass name, packagePath, and parentMaterialPath. The result can be tuned "
        "with ue_set_material_instance_param. Python recipe; requires "
        "PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"packagePath", {{"type", "string"}}},
               {"parentMaterialPath", {{"type", "string"}}}}},
             {"required", json::array({"name", "packagePath", "parentMaterialPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
name = _ARGS['name']
pkg = _ARGS['packagePath']
parent = _ARGS['parentMaterialPath']
tools = unreal.AssetToolsHelpers.get_asset_tools()
mic = tools.create_asset(name, pkg, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
if mic is None:
    _emit({'ok': False, 'error': 'create_asset returned None'})
else:
    pm = unreal.EditorAssetLibrary.load_asset(parent)
    if pm is not None:
        unreal.MaterialEditingLibrary.set_material_instance_parent(mic, pm)
    unreal.EditorAssetLibrary.save_asset(mic.get_path_name())
    _emit({'ok': True, 'assetPath': mic.get_path_name(), 'parentSet': pm is not None})
)PY";
            if (arg_str(args, "name").empty() || arg_str(args, "packagePath").empty() ||
                arg_str(args, "parentMaterialPath").empty())
                return ToolResult::error("name, packagePath and parentMaterialPath are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_import_asset ------------------------------------------------------
    add(Tool{
        "ue_import_asset",
        "Import a source file (FBX, OBJ, PNG, etc.) into the project. Pass "
        "sourceFile (an absolute path on the machine running the editor) and "
        "destinationPath (a content folder, e.g. \"/Game/Imported\"). Returns the "
        "imported asset path(s). Python recipe using AssetImportTask; requires "
        "PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"sourceFile", {{"type", "string"}}},
               {"destinationPath", {{"type", "string"}}}}},
             {"required", json::array({"sourceFile", "destinationPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
src = _ARGS['sourceFile']
dst = _ARGS['destinationPath']
task = unreal.AssetImportTask()
task.set_editor_property('filename', src)
task.set_editor_property('destination_path', dst)
task.set_editor_property('automated', True)
task.set_editor_property('save', True)
task.set_editor_property('replace_existing', True)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
imported = list(task.get_editor_property('imported_object_paths') or [])
_emit({'ok': len(imported) > 0, 'error': '' if imported else 'no objects imported (check source path/format)', 'imported': imported})
)PY";
            if (arg_str(args, "sourceFile").empty() || arg_str(args, "destinationPath").empty())
                return ToolResult::error("sourceFile and destinationPath are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_create_folder -----------------------------------------------------
    add(Tool{
        "ue_create_folder",
        "Create a content-browser folder, e.g. \"/Game/MyStuff\". Python recipe; "
        "requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties", {{"path", {{"type", "string"}}}}},
             {"required", json::array({"path"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
path = _ARGS['path']
made = unreal.EditorAssetLibrary.make_directory(path)
_emit({'ok': bool(made), 'error': '' if made else 'make_directory failed', 'path': path})
)PY";
            if (arg_str(args, "path").empty())
                return ToolResult::error("path is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});
}

// ---------------------------------------------------------------------------
// Layer 1 — data tables + extra debug. Data table row read/write are Python
// recipes (the data-table API isn't cleanly RC-callable). Console-variable
// WRITE complements the existing read-only ue_get_console_variable. Log read
// and PIE-start round out the debug surface. PIE start is 5.5+ only.
// ---------------------------------------------------------------------------
void ToolRegistry::register_data_debug_tools() {
    // -- ue_data_table_get_rows ----------------------------------------------
    add(Tool{
        "ue_data_table_get_rows",
        "Read a DataTable's rows as JSON. Pass tablePath (a DataTable asset "
        "path). Returns rowNames and the rows export. Python recipe; requires "
        "PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties", {{"tablePath", {{"type", "string"}}}}},
             {"required", json::array({"tablePath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
dt = unreal.EditorAssetLibrary.load_asset(_ARGS['tablePath'])
if dt is None:
    _emit({'ok': False, 'error': 'could not load table'})
else:
    names = [str(n) for n in unreal.DataTableFunctionLibrary.get_data_table_row_names(dt)]
    export = unreal.DataTableFunctionLibrary.get_data_table_as_json(dt) if hasattr(unreal.DataTableFunctionLibrary, 'get_data_table_as_json') else ''
    _emit({'ok': True, 'rowNames': names, 'json': export})
)PY";
            if (arg_str(args, "tablePath").empty())
                return ToolResult::error("tablePath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_set_cvar ----------------------------------------------------------
    add(Tool{
        "ue_set_cvar",
        "Set a console variable's value (the write counterpart to "
        "ue_get_console_variable). Pass name and value (string form, e.g. \"50\" "
        "for r.ScreenPercentage). Routes through a console command so it works "
        "4.25 -> 5.x.",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"value", {{"type", "string"}}}}},
             {"required", json::array({"name", "value"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string name = arg_str(args, "name");
            const std::string value = arg_str(args, "value");
            if (name.empty()) return ToolResult::error("name is required");
            const std::string cmd = name + " " + value;
            RcResult r = ctx.rc.call_function(kKismetSystemLib, "ExecuteConsoleCommand",
                                              json{{"Command", cmd}});
            if (r.ok && !(r.body.is_object() && r.body.contains("errorMessage")))
                return from_rc(r, json{{"applied", cmd}});
            // UE4 world-context retry, mirroring ue_exec_console_command.
            RcResult world = call_modern_then_legacy(ctx, kUnrealEditorSubsystem,
                                                     kEditorLevelLib, "GetEditorWorld");
            if (world.ok && world.body.is_object() &&
                world.body.contains("ReturnValue") &&
                world.body["ReturnValue"].is_string()) {
                RcResult r2 = ctx.rc.call_function(
                    kKismetSystemLib, "ExecuteConsoleCommand",
                    json{{"WorldContextObject", world.body["ReturnValue"]}, {"Command", cmd}});
                if (r2.ok) return from_rc(r2, json{{"applied", cmd}});
            }
            return from_rc(r);
        }});

    // -- ue_start_pie ---------------------------------------------------------
    add(Tool{
        "ue_start_pie",
        "Start a Play-In-Editor session (LevelEditorSubsystem."
        "EditorRequestBeginPlay). UE 5.5+ only; returns 'unsupported' on earlier "
        "engines. Use ue_stop_pie to end it.",
        json{{"type", "object"}, {"properties", json::object()}},
        {Capability::PieControl},
        [](ToolContext& ctx, const json&) -> ToolResult {
            const EngineVersion& v = ctx.caps.engine_version();
            if (v.known() && !v.at_least(5, 5)) {
                return ToolResult::unsupported(
                    "EditorRequestBeginPlay requires UE 5.5+",
                    json{{"engineVersion", v.raw}});
            }
            return from_rc(ctx.rc.call_function(kLevelEditorSubsystem,
                                                "EditorRequestBeginPlay"));
        }});
}

// ---------------------------------------------------------------------------
// Layer 2 — blueprint + UMG authoring. These are the creation tasks pure
// RemoteControl can't express (the login-screen failure): building an asset's
// internal structure. Each is a single Python recipe (one ExecutePythonCommandEx
// round-trip) rather than the agent driving the editor one statement at a time.
//
// ue_create_widget_blueprint is deliberately MULTI-STRATEGY: setting a Widget
// Blueprint's root widget hits a wall on stripped Python builds (WidgetTree has
// no binding; RootWidget is C++ protected). We try the clean path first, then
// progressively fall back, and if all fail we return a structured 'unsupported'
// that names the Layer-3 in-engine plugin — we never loop forever.
// ---------------------------------------------------------------------------
void ToolRegistry::register_authoring_tools() {
    // -- ue_create_blueprint --------------------------------------------------
    add(Tool{
        "ue_create_blueprint",
        "Create a Blueprint class asset. Pass name, packagePath (e.g. "
        "\"/Game/Blueprints\"), and parentClass (a class path or name, default "
        "\"/Script/Engine.Actor\"). Returns the created asset path. Python recipe; "
        "requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"packagePath", {{"type", "string"}}},
               {"parentClass", {{"type", "string"}, {"description", "default /Script/Engine.Actor"}}}}},
             {"required", json::array({"name", "packagePath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
name = _ARGS['name']
pkg = _ARGS['packagePath']
parent_path = _ARGS.get('parentClass') or '/Script/Engine.Actor'
parent = unreal.load_object(None, parent_path) if parent_path.startswith('/') else None
if parent is None:
    try:
        parent = getattr(unreal, parent_path)
    except Exception:
        parent = unreal.Actor
factory = unreal.BlueprintFactory()
factory.set_editor_property('parent_class', parent)
tools = unreal.AssetToolsHelpers.get_asset_tools()
bp = tools.create_asset(name, pkg, unreal.Blueprint, factory)
if bp is None:
    _emit({'ok': False, 'error': 'create_asset returned None'})
else:
    unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
    _emit({'ok': True, 'assetPath': bp.get_path_name()})
)PY";
            if (arg_str(args, "name").empty() || arg_str(args, "packagePath").empty())
                return ToolResult::error("name and packagePath are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_add_blueprint_variable -------------------------------------------
    add(Tool{
        "ue_add_blueprint_variable",
        "Add a member variable to a Blueprint. Pass blueprintPath, variableName, "
        "and variableType (a friendly name: bool, int, float, string, vector, "
        "rotator, or an object/class path for object refs). isInstanceEditable "
        "exposes it in the details panel. Python recipe; requires "
        "PythonScriptPlugin and BlueprintEditorLibrary (UE5).",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"variableName", {{"type", "string"}}},
               {"variableType", {{"type", "string"}}},
               {"isInstanceEditable", {{"type", "boolean"}}}}},
             {"required", json::array({"blueprintPath", "variableName", "variableType"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
bp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if bp is None:
    _emit({'ok': False, 'error': 'could not load blueprint'})
else:
    vname = _ARGS['variableName']
    vtype = _ARGS['variableType']
    prim = {
        'bool': unreal.EdGraphPinType('bool'),
        'int': unreal.EdGraphPinType('int'),
        'float': unreal.EdGraphPinType('real', sub_category='double') if hasattr(unreal,'EdGraphPinType') else None,
        'string': unreal.EdGraphPinType('string'),
    }
    if not hasattr(unreal, 'BlueprintEditorLibrary'):
        _emit({'ok': False, 'error': 'BlueprintEditorLibrary unavailable on this engine (UE5+ only)'})
    else:
        try:
            pin = unreal.EdGraphPinType(vtype)
        except Exception:
            pin = unreal.EdGraphPinType('bool')
        unreal.BlueprintEditorLibrary.add_member_variable(bp, vname, pin)
        if _ARGS.get('isInstanceEditable'):
            try:
                unreal.BlueprintEditorLibrary.set_blueprint_variable_instance_editable(bp, vname, True)
            except Exception:
                pass
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)
        unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
        _emit({'ok': True, 'variable': vname, 'type': vtype})
)PY";
            if (arg_str(args, "blueprintPath").empty() ||
                arg_str(args, "variableName").empty() ||
                arg_str(args, "variableType").empty())
                return ToolResult::error("blueprintPath, variableName and variableType are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_compile_blueprint -------------------------------------------------
    add(Tool{
        "ue_compile_blueprint",
        "Compile and save a Blueprint. Pass blueprintPath. Python recipe; "
        "requires PythonScriptPlugin and BlueprintEditorLibrary (UE5).",
        json{{"type", "object"},
             {"properties", {{"blueprintPath", {{"type", "string"}}}}},
             {"required", json::array({"blueprintPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
bp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if bp is None:
    _emit({'ok': False, 'error': 'could not load blueprint'})
elif not hasattr(unreal, 'BlueprintEditorLibrary'):
    _emit({'ok': False, 'error': 'BlueprintEditorLibrary unavailable (UE5+ only)'})
else:
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
    _emit({'ok': True, 'compiled': _ARGS['blueprintPath']})
)PY";
            if (arg_str(args, "blueprintPath").empty())
                return ToolResult::error("blueprintPath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_create_widget_blueprint (MULTI-STRATEGY) -------------------------
    add(Tool{
        "ue_create_widget_blueprint",
        "Create a UMG Widget Blueprint WITH a root panel — the task that fails "
        "via naive scripting because WidgetTree often has no Python binding and "
        "RootWidget is C++ protected. This recipe tries several strategies in "
        "order (factory root_widget_class, construct_widget on the tree, then "
        "set_editor_property) and reports which one worked in strategiesTried. "
        "Pass name, packagePath, and optional rootType (CanvasPanel default). If "
        "every strategy fails it returns 'unsupported' pointing at the Layer-3 "
        "in-engine plugin — it does NOT loop. Requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"packagePath", {{"type", "string"}}},
               {"rootType", {{"type", "string"}, {"description", "CanvasPanel (default), VerticalBox, Overlay, ..."}}}}},
             {"required", json::array({"name", "packagePath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
name = _ARGS['name']
pkg = _ARGS['packagePath']
root_type = _ARGS.get('rootType') or 'CanvasPanel'
tried = []
root_cls = getattr(unreal, root_type, unreal.CanvasPanel)

# Build the factory; some builds expose parent_class / root_widget_class.
factory = unreal.WidgetBlueprintFactory()
try:
    factory.set_editor_property('parent_class', unreal.UserWidget)
except Exception as e:
    tried.append('set parent_class failed: %s' % e)

tools = unreal.AssetToolsHelpers.get_asset_tools()
wbp = tools.create_asset(name, pkg, unreal.WidgetBlueprint, factory)
if wbp is None:
    _emit({'ok': False, 'error': 'create_asset returned None', 'strategiesTried': tried})
else:
    have_root = False
    # Inspect the existing tree/root if any.
    try:
        tree = wbp.get_editor_property('widget_tree')
    except Exception as e:
        tree = None
        tried.append('get widget_tree failed: %s' % e)

    # Strategy A: construct_widget on the tree and assign as root_widget.
    if tree is not None and not have_root:
        try:
            existing = tree.get_editor_property('root_widget')
            if existing is not None:
                have_root = True
                tried.append('A: already had root %s' % type(existing).__name__)
        except Exception:
            pass
    if tree is not None and not have_root:
        try:
            w = tree.construct_widget(root_cls)
            tree.set_editor_property('root_widget', w)
            have_root = True
            tried.append('A: construct_widget+set root_widget OK')
        except Exception as e:
            tried.append('A failed: %s' % e)

    # Strategy B: load UMGEditor module then retry construct_widget.
    if tree is not None and not have_root:
        try:
            unreal.load_module('UMGEditor')
            w = tree.construct_widget(root_cls)
            tree.set_editor_property('root_widget', w)
            have_root = True
            tried.append('B: load_module(UMGEditor)+construct OK')
        except Exception as e:
            tried.append('B failed: %s' % e)

    # Persist whatever we achieved.
    try:
        if hasattr(unreal, 'BlueprintEditorLibrary'):
            unreal.BlueprintEditorLibrary.compile_blueprint(wbp)
    except Exception as e:
        tried.append('compile failed: %s' % e)
    unreal.EditorAssetLibrary.save_asset(wbp.get_path_name())

    _emit({'ok': have_root, 'assetPath': wbp.get_path_name(),
           'rootSet': have_root, 'strategiesTried': tried,
           'error': '' if have_root else 'created the asset but could not set a root widget via Python (WidgetTree/RootWidget restricted on this build); use the Layer-3 in-engine plugin'})
)PY";
            if (arg_str(args, "name").empty() || arg_str(args, "packagePath").empty())
                return ToolResult::error("name and packagePath are required");
            ToolResult r = run_python_recipe(ctx, kRecipe, args);
            // If the recipe created the asset but couldn't set a root, surface it
            // as a structured 'unsupported' (not a hard error) so the agent stops
            // rather than retrying — and still learns the asset path.
            if (r.is_error && r.payload.is_object() &&
                r.payload.value("rootSet", true) == false) {
                json extra = r.payload;
                extra.erase("status");
                extra.erase("error");
                return ToolResult::unsupported(
                    "Widget Blueprint created but root widget could not be set via "
                    "Python on this build; see strategiesTried. A Layer-3 in-engine "
                    "plugin is required to author the root.",
                    std::move(extra));
            }
            return r;
        }});

    // -- ue_add_widget_to_blueprint ------------------------------------------
    add(Tool{
        "ue_add_widget_to_blueprint",
        "Add a child widget (Button, TextBlock, EditableTextBox, Image, "
        "VerticalBox, ...) to a Widget Blueprint's root/tree. Pass blueprintPath, "
        "widgetType, and widgetName. Optional text (for text-bearing widgets) and "
        "addToRoot (default true). Returns the new widget name. Requires the "
        "blueprint to already have a root panel (see ue_create_widget_blueprint). "
        "Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"widgetType", {{"type", "string"}}},
               {"widgetName", {{"type", "string"}}},
               {"text", {{"type", "string"}}}}},
             {"required", json::array({"blueprintPath", "widgetType", "widgetName"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
wbp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if wbp is None:
    _emit({'ok': False, 'error': 'could not load widget blueprint'})
else:
    wtype = _ARGS['widgetType']
    wname = _ARGS['widgetName']
    cls = getattr(unreal, wtype, None)
    if cls is None:
        _emit({'ok': False, 'error': 'unknown widget type: %s' % wtype})
    else:
        tree = wbp.get_editor_property('widget_tree')
        child = tree.construct_widget(cls, wname)
        root = tree.get_editor_property('root_widget')
        added = False
        if root is not None and hasattr(root, 'add_child'):
            try:
                root.add_child(child)
                added = True
            except Exception as e:
                added = False
        txt = _ARGS.get('text')
        if txt is not None:
            try:
                child.set_text(unreal.Text(txt))
            except Exception:
                try:
                    child.set_editor_property('text', unreal.Text(txt))
                except Exception:
                    pass
        if hasattr(unreal, 'BlueprintEditorLibrary'):
            unreal.BlueprintEditorLibrary.compile_blueprint(wbp)
        unreal.EditorAssetLibrary.save_asset(wbp.get_path_name())
        _emit({'ok': True, 'widget': wname, 'type': wtype, 'addedToRoot': added})
)PY";
            if (arg_str(args, "blueprintPath").empty() ||
                arg_str(args, "widgetType").empty() ||
                arg_str(args, "widgetName").empty())
                return ToolResult::error("blueprintPath, widgetType and widgetName are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});
}

}  // namespace ue_mcp_for_all_versions