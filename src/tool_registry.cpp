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

// Shared validation preamble for the asset-import tools. It defines
// _validate_import_source(), which enforces path/name/content constraints
// BEFORE any UE import runs, so a wrong or stale file is rejected early
// instead of being silently imported. On any violation it emits an error
// result and returns None; on success it returns a dict carrying the
// resolved path plus identity fields (name, sha1, size, mtime) so callers
// can prove they imported the file they intended. Constraints:
//   * sourceFile must be an absolute path to an existing file
//   * extension must be in the allowed list (overridable via allowedExtensions)
//   * if allowedRootDirs is given, the file must live under one of them
//   * if expectedSha1 / expectedSizeBytes are given, they must match
constexpr const char* kImportValidationPreamble = R"PY(
import os, hashlib

def _validate_import_source():
    src = _ARGS.get('sourceFile') or ''
    DEFAULT_EXTS = ['.fbx', '.obj', '.glb', '.gltf', '.abc', '.dae', '.stl',
                    '.ply', '.usd', '.usda', '.usdc', '.usdz', '.png', '.jpg',
                    '.jpeg', '.tga', '.bmp', '.exr', '.hdr', '.psd', '.tif',
                    '.tiff', '.dds', '.wav']
    raw_exts = _ARGS.get('allowedExtensions') or DEFAULT_EXTS
    allowed_exts = [e.lower() if e.startswith('.') else '.' + e.lower()
                    for e in raw_exts]
    allowed_roots = _ARGS.get('allowedRootDirs') or []

    def _fail(msg, **extra):
        d = {'ok': False, 'error': msg, 'sourceFile': src}
        d.update(extra)
        _emit(d)

    if not src:
        _fail('sourceFile is required')
        return None
    norm = os.path.normpath(src)
    if not os.path.isabs(norm):
        _fail('sourceFile must be an absolute path')
        return None
    if not os.path.isfile(norm):
        _fail('sourceFile does not exist or is not a regular file')
        return None
    ext = os.path.splitext(norm)[1].lower()
    if ext not in allowed_exts:
        _fail('file extension %r is not in the allowed list' % ext,
              allowedExtensions=allowed_exts)
        return None
    if allowed_roots:
        real = os.path.realpath(norm)
        ok_root = False
        matched = None
        for root in allowed_roots:
            try:
                rr = os.path.realpath(root)
                if os.path.commonpath([real, rr]) == rr:
                    ok_root = True
                    matched = root
                    break
            except Exception:
                pass
        if not ok_root:
            _fail('sourceFile is not under any of allowedRootDirs',
                  allowedRootDirs=allowed_roots)
            return None
    h = hashlib.sha1()
    with open(norm, 'rb') as f:
        for chunk in iter(lambda: f.read(1048576), b''):
            h.update(chunk)
    sha1 = h.hexdigest()
    size = os.path.getsize(norm)
    exp_sha = _ARGS.get('expectedSha1')
    exp_size = _ARGS.get('expectedSizeBytes')
    if exp_sha and str(exp_sha).lower() != sha1.lower():
        _fail('sha1 mismatch: file content differs from expectedSha1',
              expectedSha1=str(exp_sha).lower(), actualSha1=sha1)
        return None
    if exp_size is not None and int(exp_size) != int(size):
        _fail('size mismatch: file size differs from expectedSizeBytes',
              expectedSizeBytes=int(exp_size), actualSizeBytes=int(size))
        return None
    return {'sourceFile': src, 'resolvedPath': norm,
            'sourceFileName': os.path.basename(norm),
            'sourceFileSha1': sha1, 'sourceFileSizeBytes': int(size),
            'sourceFileMtime': os.path.getmtime(norm)}

def _validate_destination():
    dst = _ARGS.get('destinationPath') or ''
    if not dst.startswith('/'):
        _emit({'ok': False,
               'error': 'destinationPath must be a content path starting with "/" (e.g. /Game/Imported)',
               'destinationPath': dst})
        return None
    return dst
)PY";

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
    register_component_tools();
    register_material_tools();
    register_workflow_tools();
    register_scene_tools();
    register_mesh_light_tools();
    register_creation_tools();
    register_data_debug_tools();
    register_authoring_tools();
    register_pencil2umg_tools();
    register_figma2umg_tools();
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
// Component-level helpers. These cover the gap between actor-level transform
// tools and authoring workflows that need to adjust a pawn's internal mesh,
// camera, spring arm, or Blueprint component template.
// ---------------------------------------------------------------------------
void ToolRegistry::register_component_tools() {
    // -- ue_set_component_relative_transform ---------------------------------
    add(Tool{
        "ue_set_component_relative_transform",
        "Set a SceneComponent's relative transform. Pass componentPath plus any "
        "of location {x,y,z}, rotation {pitch,yaw,roll}, and scale {x,y,z}. "
        "Uses K2_SetRelativeLocation, K2_SetRelativeRotation, and "
        "SetRelativeScale3D on the component instance, so it works for child "
        "components such as SkeletalMeshComponent, CameraComponent, and "
        "SpringArmComponent.",
        json{{"type", "object"},
             {"properties",
              {{"componentPath", {{"type", "string"}}},
               {"location", {{"type", "object"}}},
               {"rotation", {{"type", "object"}}},
               {"scale", {{"type", "object"}}},
               {"sweep", {{"type", "boolean"}}},
               {"teleport", {{"type", "boolean"}}}}},
             {"required", json::array({"componentPath"})}},
        {Capability::ObjectCall},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            const std::string comp = arg_str(args, "componentPath");
            if (comp.empty()) return ToolResult::error("componentPath is required");
            const bool sweep = args.value("sweep", false);
            const bool teleport = args.value("teleport", true);
            json applied = json::object();
            json errors = json::object();

            auto record = [&](const char* key, const RcResult& r) {
                const bool body_error = r.body.is_object() && r.body.contains("errorMessage");
                if (r.ok && !body_error) {
                    applied[key] = "ok";
                } else {
                    std::string msg = r.error.empty() ? "RemoteControl request failed" : r.error;
                    if (body_error && r.body["errorMessage"].is_string())
                        msg = r.body["errorMessage"].get<std::string>();
                    errors[key] = msg;
                }
            };

            if (args.contains("location")) {
                record("location",
                       ctx.rc.call_function(comp, "K2_SetRelativeLocation",
                                            json{{"NewLocation", xyz(args["location"])},
                                                 {"bSweep", sweep},
                                                 {"bTeleport", teleport}},
                                            true));
            }
            if (args.contains("rotation")) {
                record("rotation",
                       ctx.rc.call_function(comp, "K2_SetRelativeRotation",
                                            json{{"NewRotation", pyr(args["rotation"])},
                                                 {"bSweep", sweep},
                                                 {"bTeleport", teleport}},
                                            true));
            }
            if (args.contains("scale")) {
                record("scale",
                       ctx.rc.call_function(comp, "SetRelativeScale3D",
                                            json{{"NewScale3D", xyz(args["scale"], 1.0, 1.0, 1.0)}},
                                            true));
            }
            if (applied.empty() && errors.empty())
                return ToolResult::error("provide at least one of location, rotation, scale");
            json payload = {{"status", errors.empty() ? "ok" : "error"},
                            {"componentPath", comp},
                            {"applied", applied}};
            if (!errors.empty()) payload["errors"] = errors;
            return errors.empty() ? ToolResult::ok(payload) : ToolResult{true, payload};
        }});

    // -- ue_get_component_transform ------------------------------------------
    add(Tool{
        "ue_get_component_transform",
        "Read a SceneComponent's world transform, relative transform, attach "
        "parent, owner, class, and world scale. componentPath should come from "
        "ue_get_actor_components. Supports SkeletalMeshComponent, "
        "CameraComponent, SpringArmComponent, and other SceneComponents. Python "
        "recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties", {{"componentPath", {{"type", "string"}}}}},
             {"required", json::array({"componentPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
path = _ARGS['componentPath']

def resolve(path):
    for fn_name in ('load_object', 'find_object'):
        fn = getattr(unreal, fn_name, None)
        if fn is None:
            continue
        try:
            obj = fn(None, path)
            if obj is not None:
                return obj
        except Exception:
            pass
    return None

def vdict(v):
    return {'x': float(v.x), 'y': float(v.y), 'z': float(v.z)}

def rdict(r):
    return {'pitch': float(r.pitch), 'yaw': float(r.yaw), 'roll': float(r.roll)}

def qdict(q):
    return {'x': float(q.x), 'y': float(q.y), 'z': float(q.z), 'w': float(q.w)}

def call_any(obj, names, default=None):
    for name in names:
        fn = getattr(obj, name, None)
        if fn is not None:
            try:
                return fn()
            except Exception:
                pass
    return default

def prop_any(obj, names, default=None):
    for name in names:
        try:
            return obj.get_editor_property(name)
        except Exception:
            pass
    return default

comp = resolve(path)
if comp is None:
    _emit({'ok': False, 'error': 'could not resolve componentPath: %s' % path})
else:
    world_location = call_any(comp, ['get_component_location'])
    world_rotation = call_any(comp, ['get_component_rotation'])
    world_scale = call_any(comp, ['get_component_scale'])
    rel_location = call_any(comp, ['get_relative_location'], prop_any(comp, ['relative_location']))
    rel_rotation = call_any(comp, ['get_relative_rotation'], prop_any(comp, ['relative_rotation']))
    rel_scale = call_any(comp, ['get_relative_scale3d'], prop_any(comp, ['relative_scale3d']))
    world_transform = call_any(comp, ['get_component_transform'])
    parent = call_any(comp, ['get_attach_parent'], prop_any(comp, ['attach_parent']))
    owner = call_any(comp, ['get_owner'])
    attach_socket = call_any(comp, ['get_attach_socket_name'], '')

    result = {
        'ok': True,
        'componentPath': comp.get_path_name(),
        'componentName': comp.get_name(),
        'classPath': comp.get_class().get_path_name(),
        'ownerPath': owner.get_path_name() if owner is not None else '',
        'attachParent': parent.get_path_name() if parent is not None else '',
        'attachParentName': parent.get_name() if parent is not None else '',
        'attachSocket': str(attach_socket),
        'worldScale': vdict(world_scale) if world_scale is not None else None,
        'worldTransform': {
            'location': vdict(world_location) if world_location is not None else None,
            'rotation': rdict(world_rotation) if world_rotation is not None else None,
            'scale': vdict(world_scale) if world_scale is not None else None
        },
        'relativeTransform': {
            'location': vdict(rel_location) if rel_location is not None else None,
            'rotation': rdict(rel_rotation) if rel_rotation is not None else None,
            'scale': vdict(rel_scale) if rel_scale is not None else None
        }
    }
    if world_transform is not None:
        try:
            result['worldTransform']['quaternion'] = qdict(world_transform.rotation)
        except Exception:
            pass
    _emit(result)
)PY";
            if (arg_str(args, "componentPath").empty())
                return ToolResult::error("componentPath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_get_component_bounds --------------------------------------------
    add(Tool{
        "ue_get_component_bounds",
        "Return local and/or world bounds for a component, or aggregate filtered "
        "bounds for an actor when actorPath is supplied. By default "
        "includeEditorVisualization=false excludes CameraComponent, "
        "DrawFrustumComponent, BillboardComponent, ArrowComponent, SpriteComponent, "
        "and editor-only components so camera frustums do not explode character "
        "bounds. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"componentPath", {{"type", "string"}}},
               {"actorPath", {{"type", "string"}}},
               {"boundsSpace", {{"type", "string"}, {"enum", json::array({"both", "local", "world"})}}},
               {"includeEditorVisualization", {{"type", "boolean"}}},
               {"includePerComponent", {{"type", "boolean"}}}}},
             {"required", json::array({})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
component_path = _ARGS.get('componentPath') or ''
actor_path = _ARGS.get('actorPath') or ''
bounds_space = _ARGS.get('boundsSpace') or 'both'
include_visual = bool(_ARGS.get('includeEditorVisualization', False))
include_per_component = bool(_ARGS.get('includePerComponent', True))

VISUAL_CLASS_TOKENS = (
    'CameraComponent', 'DrawFrustumComponent', 'BillboardComponent',
    'ArrowComponent', 'SpriteComponent', 'TextRenderComponent'
)

def resolve(path):
    for fn_name in ('load_object', 'find_object'):
        fn = getattr(unreal, fn_name, None)
        if fn is None:
            continue
        try:
            obj = fn(None, path)
            if obj is not None:
                return obj
        except Exception:
            pass
    return None

def vdict(v):
    return {'x': float(v.x), 'y': float(v.y), 'z': float(v.z)}

def vec(x, y, z):
    return unreal.Vector(float(x), float(y), float(z))

def bounds_dict(origin, extent, radius=None):
    if origin is None or extent is None:
        return None
    d = {'origin': vdict(origin), 'extent': vdict(extent),
         'min': vdict(vec(origin.x - extent.x, origin.y - extent.y, origin.z - extent.z)),
         'max': vdict(vec(origin.x + extent.x, origin.y + extent.y, origin.z + extent.z))}
    if radius is not None:
        d['sphereRadius'] = float(radius)
    return d

def component_class_name(comp):
    try:
        return comp.get_class().get_name()
    except Exception:
        return type(comp).__name__

def is_editor_only(comp):
    for name in ('is_editor_only', 'is_editor_only_component'):
        fn = getattr(comp, name, None)
        if fn is not None:
            try:
                return bool(fn())
            except Exception:
                pass
    for prop in ('is_editor_only', 'bIsEditorOnly'):
        try:
            return bool(comp.get_editor_property(prop))
        except Exception:
            pass
    return False

def is_visual_component(comp):
    name = component_class_name(comp)
    if any(token in name for token in VISUAL_CLASS_TOKENS):
        return True
    if is_editor_only(comp):
        return True
    return False

def prop_any(obj, names):
    for name in names:
        try:
            val = obj.get_editor_property(name)
            if val is not None:
                return val
        except Exception:
            pass
    return None

def call_any(obj, names):
    for name in names:
        fn = getattr(obj, name, None)
        if fn is not None:
            try:
                return fn()
            except Exception:
                pass
    return None

def mesh_asset(comp):
    for name in ('get_static_mesh', 'get_skeletal_mesh_asset', 'get_skeletal_mesh'):
        fn = getattr(comp, name, None)
        if fn is not None:
            try:
                asset = fn()
                if asset is not None:
                    return asset
            except Exception:
                pass
    return prop_any(comp, ('static_mesh', 'skeletal_mesh_asset', 'skeletal_mesh'))

def local_bounds(comp):
    glb = getattr(comp, 'get_local_bounds', None)
    if glb is not None:
        try:
            lo, hi = glb()
            origin = vec((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5)
            extent = vec(abs(hi.x - lo.x) * 0.5, abs(hi.y - lo.y) * 0.5, abs(hi.z - lo.z) * 0.5)
            return origin, extent, None, 'component.get_local_bounds'
        except Exception:
            pass
    asset = mesh_asset(comp)
    if asset is not None:
        for name in ('get_bounds', 'get_imported_bounds'):
            fn = getattr(asset, name, None)
            if fn is not None:
                try:
                    b = fn()
                    return b.origin, b.box_extent, getattr(b, 'sphere_radius', None), 'asset.%s' % name
                except Exception:
                    pass
        fn = getattr(asset, 'get_bounding_box', None)
        if fn is not None:
            try:
                box = fn()
                lo = box.min
                hi = box.max
                origin = vec((lo.x + hi.x) * 0.5, (lo.y + hi.y) * 0.5, (lo.z + hi.z) * 0.5)
                extent = vec(abs(hi.x - lo.x) * 0.5, abs(hi.y - lo.y) * 0.5, abs(hi.z - lo.z) * 0.5)
                return origin, extent, None, 'asset.get_bounding_box'
            except Exception:
                pass
    return None, None, None, ''

def world_bounds(comp):
    for name in ('bounds', 'Bounds'):
        try:
            b = comp.get_editor_property(name)
            return b.origin, b.box_extent, getattr(b, 'sphere_radius', None), 'component.bounds'
        except Exception:
            pass
    origin, extent, radius, src = local_bounds(comp)
    if origin is not None and extent is not None:
        scale = call_any(comp, ('get_component_scale',))
        loc = call_any(comp, ('get_component_location',))
        if scale is not None and loc is not None:
            e = vec(abs(extent.x * scale.x), abs(extent.y * scale.y), abs(extent.z * scale.z))
            return loc, e, radius, 'local_bounds_scaled_fallback'
    return None, None, None, ''

def component_bounds_payload(comp):
    excluded = (not include_visual) and is_visual_component(comp)
    item = {
        'componentPath': comp.get_path_name(),
        'componentName': comp.get_name(),
        'classPath': comp.get_class().get_path_name(),
        'excluded': bool(excluded)
    }
    if excluded:
        item['excludeReason'] = 'editor visualization component'
        return item
    if bounds_space in ('both', 'local'):
        o, e, r, src = local_bounds(comp)
        item['localBounds'] = bounds_dict(o, e, r)
        item['localBoundsSource'] = src
    if bounds_space in ('both', 'world'):
        o, e, r, src = world_bounds(comp)
        item['worldBounds'] = bounds_dict(o, e, r)
        item['worldBoundsSource'] = src
    return item

def aggregate_world(items):
    mins = []
    maxs = []
    for item in items:
        b = item.get('worldBounds')
        if not b:
            continue
        mins.append(b['min'])
        maxs.append(b['max'])
    if not mins:
        return None
    mn = {'x': min(v['x'] for v in mins), 'y': min(v['y'] for v in mins), 'z': min(v['z'] for v in mins)}
    mx = {'x': max(v['x'] for v in maxs), 'y': max(v['y'] for v in maxs), 'z': max(v['z'] for v in maxs)}
    origin = {'x': (mn['x'] + mx['x']) * 0.5, 'y': (mn['y'] + mx['y']) * 0.5, 'z': (mn['z'] + mx['z']) * 0.5}
    extent = {'x': (mx['x'] - mn['x']) * 0.5, 'y': (mx['y'] - mn['y']) * 0.5, 'z': (mx['z'] - mn['z']) * 0.5}
    return {'origin': origin, 'extent': extent, 'min': mn, 'max': mx}

if not component_path and not actor_path:
    _emit({'ok': False, 'error': 'componentPath or actorPath is required'})
elif component_path:
    comp = resolve(component_path)
    if comp is None:
        _emit({'ok': False, 'error': 'could not resolve componentPath: %s' % component_path})
    else:
        _emit({'ok': True, 'mode': 'component',
               'includeEditorVisualization': include_visual,
               'bounds': component_bounds_payload(comp)})
else:
    actor = resolve(actor_path)
    if actor is None:
        _emit({'ok': False, 'error': 'could not resolve actorPath: %s' % actor_path})
    else:
        try:
            comps = list(actor.get_components_by_class(unreal.ActorComponent))
        except Exception:
            comps = []
        items = [component_bounds_payload(c) for c in comps]
        included = [i for i in items if not i.get('excluded')]
        payload = {'ok': True, 'mode': 'actor', 'actorPath': actor.get_path_name(),
                   'includeEditorVisualization': include_visual,
                   'componentCount': len(items), 'includedComponentCount': len(included),
                   'excludedComponentCount': len(items) - len(included),
                   'worldBounds': aggregate_world(included)}
        if include_per_component:
            payload['components'] = items
        _emit(payload)
)PY";
            if (arg_str(args, "componentPath").empty() &&
                arg_str(args, "actorPath").empty()) {
                return ToolResult::error("componentPath or actorPath is required");
            }
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_fit_mesh_component_to_height -------------------------------------
    add(Tool{
        "ue_fit_mesh_component_to_height",
        "Read a StaticMeshComponent or SkeletalMeshComponent asset's local "
        "bounds, compute a uniform relative scale for a target height in UE "
        "units (centimeters), and optionally apply it to the component. Useful "
        "for AI-generated characters imported at 0.01, 1, 100, or 110 scale. "
        "Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"componentPath", {{"type", "string"}}},
               {"targetHeight", {{"type", "number"}}},
               {"heightAxis", {{"type", "string"}, {"enum", json::array({"X", "Y", "Z"})}}},
               {"apply", {{"type", "boolean"}}}}},
             {"required", json::array({"componentPath", "targetHeight"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
path = _ARGS['componentPath']
target = float(_ARGS['targetHeight'])
axis = (_ARGS.get('heightAxis') or 'Z').upper()
apply = bool(_ARGS.get('apply', True))

def resolve(path):
    for fn_name in ('load_object', 'find_object'):
        fn = getattr(unreal, fn_name, None)
        if fn is None:
            continue
        try:
            obj = fn(None, path)
            if obj is not None:
                return obj
        except Exception:
            pass
    return None

def vdict(v):
    return {'x': float(v.x), 'y': float(v.y), 'z': float(v.z)}

def prop_any(obj, names):
    for name in names:
        try:
            val = obj.get_editor_property(name)
            if val is not None:
                return val
        except Exception:
            pass
    return None

def mesh_asset(comp):
    for name in ('get_static_mesh', 'get_skeletal_mesh_asset', 'get_skeletal_mesh'):
        fn = getattr(comp, name, None)
        if fn is not None:
            try:
                asset = fn()
                if asset is not None:
                    return asset
            except Exception:
                pass
    return prop_any(comp, ('static_mesh', 'skeletal_mesh_asset', 'skeletal_mesh'))

def local_min_max(comp):
    glb = getattr(comp, 'get_local_bounds', None)
    if glb is not None:
        try:
            return glb(), 'component.get_local_bounds'
        except Exception:
            pass
    asset = mesh_asset(comp)
    if asset is None:
        return None, ''
    for name in ('get_bounds', 'get_imported_bounds'):
        fn = getattr(asset, name, None)
        if fn is not None:
            try:
                b = fn()
                lo = unreal.Vector(b.origin.x - b.box_extent.x,
                                   b.origin.y - b.box_extent.y,
                                   b.origin.z - b.box_extent.z)
                hi = unreal.Vector(b.origin.x + b.box_extent.x,
                                   b.origin.y + b.box_extent.y,
                                   b.origin.z + b.box_extent.z)
                return (lo, hi), 'asset.%s' % name
            except Exception:
                pass
    fn = getattr(asset, 'get_bounding_box', None)
    if fn is not None:
        try:
            box = fn()
            return (box.min, box.max), 'asset.get_bounding_box'
        except Exception:
            pass
    return None, ''

comp = resolve(path)
if comp is None:
    _emit({'ok': False, 'error': 'could not resolve componentPath: %s' % path})
else:
    mm, source = local_min_max(comp)
    if mm is None:
        _emit({'ok': False, 'error': 'could not read mesh local bounds for component'})
    else:
        lo, hi = mm
        local_height = abs(getattr(hi, axis.lower()) - getattr(lo, axis.lower()))
        if local_height <= 0.000001:
            _emit({'ok': False, 'error': 'mesh local height is zero on axis %s' % axis})
        else:
            uniform = target / local_height
            prev = None
            try:
                prev = comp.get_relative_scale3d()
            except Exception:
                try:
                    prev = comp.get_editor_property('relative_scale3d')
                except Exception:
                    prev = unreal.Vector(1.0, 1.0, 1.0)
            applied = False
            if apply:
                new_scale = unreal.Vector(uniform, uniform, uniform)
                try:
                    comp.set_relative_scale3d(new_scale)
                    applied = True
                except Exception:
                    comp.set_editor_property('relative_scale3d', new_scale)
                    applied = True
            _emit({'ok': True, 'componentPath': comp.get_path_name(),
                   'meshPath': mesh_asset(comp).get_path_name() if mesh_asset(comp) is not None else '',
                   'boundsSource': source,
                   'heightAxis': axis,
                   'localHeight': local_height,
                   'targetHeight': target,
                   'uniformScale': uniform,
                   'previousRelativeScale': vdict(prev),
                   'newRelativeScale': {'x': uniform, 'y': uniform, 'z': uniform},
                   'applied': applied})
)PY";
            if (arg_str(args, "componentPath").empty() || !args.contains("targetHeight"))
                return ToolResult::error("componentPath and targetHeight are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_get_blueprint_components -----------------------------------------
    add(Tool{
        "ue_get_blueprint_components",
        "Load a Blueprint asset and list its component tree from the generated "
        "CDO: component name, class, parent component, relative transform, and "
        "common asset references such as StaticMesh, SkeletalMesh, AnimClass, "
        "and materials. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties", {{"blueprintPath", {{"type", "string"}}}}},
             {"required", json::array({"blueprintPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
bp_path = _ARGS['blueprintPath']
bp = unreal.EditorAssetLibrary.load_asset(bp_path)

def vdict(v):
    return {'x': float(v.x), 'y': float(v.y), 'z': float(v.z)}

def rdict(r):
    return {'pitch': float(r.pitch), 'yaw': float(r.yaw), 'roll': float(r.roll)}

def call_any(obj, names, default=None):
    for name in names:
        fn = getattr(obj, name, None)
        if fn is not None:
            try:
                return fn()
            except Exception:
                pass
    return default

def prop_any(obj, names, default=None):
    for name in names:
        try:
            val = obj.get_editor_property(name)
            if val is not None:
                return val
        except Exception:
            pass
    return default

def obj_path(obj):
    try:
        return obj.get_path_name() if obj is not None else ''
    except Exception:
        return str(obj) if obj is not None else ''

def generated_class(asset):
    try:
        return asset.get_editor_property('generated_class')
    except Exception:
        return getattr(asset, 'generated_class', None)

def rel_transform(comp):
    loc = call_any(comp, ['get_relative_location'], prop_any(comp, ['relative_location']))
    rot = call_any(comp, ['get_relative_rotation'], prop_any(comp, ['relative_rotation']))
    scale = call_any(comp, ['get_relative_scale3d'], prop_any(comp, ['relative_scale3d']))
    return {'location': vdict(loc) if loc is not None else None,
            'rotation': rdict(rot) if rot is not None else None,
            'scale': vdict(scale) if scale is not None else None}

def resource_refs(comp):
    refs = {}
    for key, names in {
        'staticMesh': ('static_mesh',),
        'skeletalMesh': ('skeletal_mesh_asset', 'skeletal_mesh'),
        'animClass': ('anim_class', 'anim_instance_class'),
        'cameraFilmback': ('filmback',)
    }.items():
        val = prop_any(comp, names)
        if val is not None:
            refs[key] = obj_path(val)
    for method_key, method_name in (('staticMesh', 'get_static_mesh'),
                                    ('skeletalMesh', 'get_skeletal_mesh_asset')):
        fn = getattr(comp, method_name, None)
        if fn is not None and method_key not in refs:
            try:
                refs[method_key] = obj_path(fn())
            except Exception:
                pass
    mats = []
    get_num = getattr(comp, 'get_num_materials', None)
    get_mat = getattr(comp, 'get_material', None)
    if get_num is not None and get_mat is not None:
        try:
            for i in range(int(get_num())):
                mats.append(obj_path(get_mat(i)))
        except Exception:
            pass
    if mats:
        refs['materials'] = mats
    extras = {}
    for key in ('target_arm_length', 'socket_offset', 'target_offset',
                'use_pawn_control_rotation', 'field_of_view', 'aspect_ratio'):
        val = prop_any(comp, (key,))
        if val is not None:
            if hasattr(val, 'x') and hasattr(val, 'y') and hasattr(val, 'z'):
                extras[key] = vdict(val)
            else:
                try:
                    extras[key] = float(val)
                except Exception:
                    extras[key] = str(val)
    if extras:
        refs['settings'] = extras
    return refs

if bp is None:
    _emit({'ok': False, 'error': 'could not load blueprint: %s' % bp_path})
else:
    gen = generated_class(bp)
    if gen is None:
        _emit({'ok': False, 'error': 'blueprint has no generated_class: %s' % bp_path})
    else:
        cdo = unreal.get_default_object(gen)
        try:
            comps = list(cdo.get_components_by_class(unreal.ActorComponent))
        except Exception:
            comps = []
        items = []
        for comp in comps:
            parent = call_any(comp, ['get_attach_parent'], prop_any(comp, ['attach_parent']))
            children = []
            for other in comps:
                other_parent = call_any(other, ['get_attach_parent'], prop_any(other, ['attach_parent']))
                if other_parent is comp:
                    children.append(other.get_name())
            items.append({
                'name': comp.get_name(),
                'path': comp.get_path_name(),
                'classPath': comp.get_class().get_path_name(),
                'parent': parent.get_name() if parent is not None else '',
                'parentPath': obj_path(parent),
                'children': children,
                'relativeTransform': rel_transform(comp),
                'resources': resource_refs(comp)
            })
        _emit({'ok': True, 'blueprintPath': bp.get_path_name(),
               'generatedClassPath': gen.get_path_name(),
               'componentCount': len(items),
               'components': items})
)PY";
            if (arg_str(args, "blueprintPath").empty())
                return ToolResult::error("blueprintPath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_set_blueprint_component_template_transform -----------------------
    add(Tool{
        "ue_set_blueprint_component_template_transform",
        "Persistently set a Blueprint component template's relative location, "
        "rotation, and/or scale on the Blueprint generated CDO and, when Python "
        "exposes it, the SCS component template. This is for fixing defaults "
        "such as BP_HeroineThirdPersonPawn.HeroineMesh rather than only the "
        "current level instance. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"componentName", {{"type", "string"}}},
               {"location", {{"type", "object"}}},
               {"rotation", {{"type", "object"}}},
               {"scale", {{"type", "object"}}},
               {"compile", {{"type", "boolean"}}},
               {"save", {{"type", "boolean"}}}}},
             {"required", json::array({"blueprintPath", "componentName"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
bp_path = _ARGS['blueprintPath']
wanted = _ARGS['componentName']
compile_bp = bool(_ARGS.get('compile', True))
save_bp = bool(_ARGS.get('save', True))
bp = unreal.EditorAssetLibrary.load_asset(bp_path)
warnings = []
updated = []

def generated_class(asset):
    try:
        return asset.get_editor_property('generated_class')
    except Exception:
        return getattr(asset, 'generated_class', None)

def vec_arg(key, default=None):
    if key not in _ARGS:
        return default
    src = _ARGS.get(key) or {}
    return unreal.Vector(float(src.get('x', 0.0)),
                         float(src.get('y', 0.0)),
                         float(src.get('z', 0.0)))

def rot_arg(key, default=None):
    if key not in _ARGS:
        return default
    src = _ARGS.get(key) or {}
    return unreal.Rotator(float(src.get('pitch', 0.0)),
                          float(src.get('yaw', 0.0)),
                          float(src.get('roll', 0.0)))

def vdict(v):
    return {'x': float(v.x), 'y': float(v.y), 'z': float(v.z)}

def rdict(r):
    return {'pitch': float(r.pitch), 'yaw': float(r.yaw), 'roll': float(r.roll)}

def call_any(obj, names, *args):
    for name in names:
        fn = getattr(obj, name, None)
        if fn is not None:
            try:
                return fn(*args)
            except Exception:
                pass
    return None

def prop_any(obj, names, default=None):
    for name in names:
        try:
            return obj.get_editor_property(name)
        except Exception:
            pass
    return default

def matches(comp, extra_name=''):
    names = [extra_name, comp.get_name()]
    try:
        names.append(str(comp.get_fname()))
    except Exception:
        pass
    try:
        names.append(comp.get_path_name())
    except Exception:
        pass
    return any(n and (n == wanted or wanted in n) for n in names)

def apply_transform(comp, source):
    if comp is None:
        return False
    try:
        comp.modify()
    except Exception:
        pass
    loc = vec_arg('location')
    rot = rot_arg('rotation')
    scale = vec_arg('scale')
    if loc is not None:
        try:
            comp.set_editor_property('relative_location', loc)
        except Exception:
            call_any(comp, ['set_relative_location'], loc)
    if rot is not None:
        try:
            comp.set_editor_property('relative_rotation', rot)
        except Exception:
            call_any(comp, ['set_relative_rotation'], rot)
    if scale is not None:
        try:
            comp.set_editor_property('relative_scale3d', scale)
        except Exception:
            call_any(comp, ['set_relative_scale3d'], scale)
    try:
        comp.post_edit_change()
    except Exception:
        pass
    rloc = call_any(comp, ['get_relative_location']) or prop_any(comp, ['relative_location'])
    rrot = call_any(comp, ['get_relative_rotation']) or prop_any(comp, ['relative_rotation'])
    rscale = call_any(comp, ['get_relative_scale3d']) or prop_any(comp, ['relative_scale3d'])
    updated.append({'source': source, 'name': comp.get_name(),
                    'path': comp.get_path_name(),
                    'classPath': comp.get_class().get_path_name(),
                    'relativeTransform': {
                        'location': vdict(rloc) if rloc is not None else None,
                        'rotation': rdict(rrot) if rrot is not None else None,
                        'scale': vdict(rscale) if rscale is not None else None}})
    return True

def scs_nodes(asset):
    scs = prop_any(asset, ['simple_construction_script'])
    if scs is None:
        scs = getattr(asset, 'simple_construction_script', None)
    if scs is None:
        return []
    for name in ('get_all_nodes', 'get_root_nodes'):
        fn = getattr(scs, name, None)
        if fn is not None:
            try:
                return list(fn())
            except Exception as e:
                warnings.append('SCS %s failed: %s' % (name, e))
    return []

if bp is None:
    _emit({'ok': False, 'error': 'could not load blueprint: %s' % bp_path})
else:
    # SCS templates first: authored Blueprint components live here when exposed.
    for node in scs_nodes(bp):
        node_name = ''
        try:
            node_name = str(node.get_variable_name())
        except Exception:
            pass
        tmpl = None
        for name in ('get_component_template',):
            fn = getattr(node, name, None)
            if fn is not None:
                try:
                    tmpl = fn()
                    break
                except Exception:
                    pass
        if tmpl is None:
            tmpl = prop_any(node, ['component_template'])
        if tmpl is not None and matches(tmpl, node_name):
            apply_transform(tmpl, 'SCS')

    gen = generated_class(bp)
    if gen is None:
        warnings.append('blueprint has no generated_class')
    else:
        cdo = unreal.get_default_object(gen)
        try:
            comps = list(cdo.get_components_by_class(unreal.ActorComponent))
        except Exception:
            comps = []
        for comp in comps:
            if matches(comp):
                apply_transform(comp, 'generatedCDO')

    if not updated:
        _emit({'ok': False, 'error': 'component not found on blueprint: %s' % wanted,
               'warnings': warnings})
    else:
        try:
            bp.modify()
        except Exception:
            pass
        if compile_bp and hasattr(unreal, 'BlueprintEditorLibrary'):
            try:
                unreal.BlueprintEditorLibrary.compile_blueprint(bp)
            except Exception as e:
                warnings.append('compile_blueprint failed: %s' % e)
        if save_bp:
            try:
                unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
            except Exception as e:
                warnings.append('save_asset failed: %s' % e)
        _emit({'ok': True, 'blueprintPath': bp.get_path_name(),
               'componentName': wanted, 'updated': updated, 'warnings': warnings})
)PY";
            if (arg_str(args, "blueprintPath").empty() ||
                arg_str(args, "componentName").empty())
                return ToolResult::error("blueprintPath and componentName are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_normalize_imported_character_mesh --------------------------------
    add(Tool{
        "ue_normalize_imported_character_mesh",
        "Compute a rotation and optional uniform scale recommendation for a "
        "character mesh imported with a different up/forward convention "
        "(Y-up/Z-up, X-forward/Y-forward, etc.). Defaults targetUp=Z and "
        "targetForward=X for Unreal. Pass applyToComponentPath plus apply=true "
        "to write the suggested relative rotation/scale to a component; otherwise "
        "the tool only returns the recommendation. Python recipe; requires "
        "PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"assetPath", {{"type", "string"}}},
               {"sourceUp", {{"type", "string"}}},
               {"sourceForward", {{"type", "string"}}},
               {"targetUp", {{"type", "string"}}},
               {"targetForward", {{"type", "string"}}},
               {"targetHeight", {{"type", "number"}}},
               {"applyToComponentPath", {{"type", "string"}}},
               {"apply", {{"type", "boolean"}}}}},
             {"required", json::array({})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
source_up = (_ARGS.get('sourceUp') or 'Y').upper()
source_forward = (_ARGS.get('sourceForward') or 'Z').upper()
target_up = (_ARGS.get('targetUp') or 'Z').upper()
target_forward = (_ARGS.get('targetForward') or 'X').upper()
asset_path = _ARGS.get('assetPath') or ''
target_height = _ARGS.get('targetHeight')
apply_path = _ARGS.get('applyToComponentPath') or ''
apply = bool(_ARGS.get('apply', False))
warnings = []

def axis(name):
    sign = -1.0 if name.startswith('-') else 1.0
    base = name[1:] if name.startswith('-') else name
    if base == 'X':
        return (sign, 0.0, 0.0)
    if base == 'Y':
        return (0.0, sign, 0.0)
    if base == 'Z':
        return (0.0, 0.0, sign)
    raise RuntimeError('axis must be X, Y, Z, -X, -Y, or -Z: %s' % name)

def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])

def norm(a):
    l = (dot(a, a)) ** 0.5
    if l <= 0.000001:
        raise RuntimeError('zero-length axis')
    return (a[0] / l, a[1] / l, a[2] / l)

def basis(up_name, forward_name):
    f = norm(axis(forward_name))
    u = norm(axis(up_name))
    if abs(dot(f, u)) > 0.999:
        raise RuntimeError('up and forward axes must not be parallel')
    r = norm(cross(u, f))
    u = norm(cross(f, r))
    return (f, r, u)

def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

def mat_transpose(a):
    return [[a[j][i] for j in range(3)] for i in range(3)]

def basis_matrix(b):
    # Columns are forward(X), right(Y), up(Z) basis vectors in world axes.
    return [[b[0][0], b[1][0], b[2][0]],
            [b[0][1], b[1][1], b[2][1]],
            [b[0][2], b[1][2], b[2][2]]]

def quat_from_matrix(m):
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0.0:
        s = (tr + 1.0) ** 0.5 * 2.0
        w = 0.25 * s
        x = (m[2][1] - m[1][2]) / s
        y = (m[0][2] - m[2][0]) / s
        z = (m[1][0] - m[0][1]) / s
    elif m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = (1.0 + m[0][0] - m[1][1] - m[2][2]) ** 0.5 * 2.0
        w = (m[2][1] - m[1][2]) / s
        x = 0.25 * s
        y = (m[0][1] + m[1][0]) / s
        z = (m[0][2] + m[2][0]) / s
    elif m[1][1] > m[2][2]:
        s = (1.0 + m[1][1] - m[0][0] - m[2][2]) ** 0.5 * 2.0
        w = (m[0][2] - m[2][0]) / s
        x = (m[0][1] + m[1][0]) / s
        y = 0.25 * s
        z = (m[1][2] + m[2][1]) / s
    else:
        s = (1.0 + m[2][2] - m[0][0] - m[1][1]) ** 0.5 * 2.0
        w = (m[1][0] - m[0][1]) / s
        x = (m[0][2] + m[2][0]) / s
        y = (m[1][2] + m[2][1]) / s
        z = 0.25 * s
    return (x, y, z, w)

def resolve(path):
    for fn_name in ('load_object', 'find_object'):
        fn = getattr(unreal, fn_name, None)
        if fn is None:
            continue
        try:
            obj = fn(None, path)
            if obj is not None:
                return obj
        except Exception:
            pass
    return None

def asset_height(asset, up_name):
    if asset is None:
        return None
    b = None
    for name in ('get_bounds', 'get_imported_bounds'):
        fn = getattr(asset, name, None)
        if fn is not None:
            try:
                b = fn()
                break
            except Exception:
                pass
    if b is None:
        return None
    vec = axis(up_name)
    extent = abs(vec[0]) * b.box_extent.x + abs(vec[1]) * b.box_extent.y + abs(vec[2]) * b.box_extent.z
    return extent * 2.0

src_b = basis(source_up, source_forward)
tgt_b = basis(target_up, target_forward)
matrix = mat_mul(basis_matrix(tgt_b), mat_transpose(basis_matrix(src_b)))
qx, qy, qz, qw = quat_from_matrix(matrix)
rotation = None
try:
    rot = unreal.Quat(qx, qy, qz, qw).rotator()
    rotation = {'pitch': float(rot.pitch), 'yaw': float(rot.yaw), 'roll': float(rot.roll)}
except Exception as e:
    warnings.append('could not convert quaternion to Rotator: %s' % e)

uniform_scale = None
height = None
asset = unreal.EditorAssetLibrary.load_asset(asset_path) if asset_path else None
if target_height is not None and asset is not None:
    height = asset_height(asset, source_up)
    if height and height > 0.000001:
        uniform_scale = float(target_height) / height
    else:
        warnings.append('could not compute asset height from bounds')

applied = False
if apply and apply_path:
    comp = resolve(apply_path)
    if comp is None:
        warnings.append('could not resolve applyToComponentPath')
    else:
        if rotation is not None:
            try:
                comp.set_relative_rotation(unreal.Rotator(rotation['pitch'], rotation['yaw'], rotation['roll']))
            except Exception:
                comp.set_editor_property('relative_rotation', unreal.Rotator(rotation['pitch'], rotation['yaw'], rotation['roll']))
        if uniform_scale is not None:
            s = unreal.Vector(uniform_scale, uniform_scale, uniform_scale)
            try:
                comp.set_relative_scale3d(s)
            except Exception:
                comp.set_editor_property('relative_scale3d', s)
        applied = True

_emit({'ok': True,
       'source': {'up': source_up, 'forward': source_forward},
       'target': {'up': target_up, 'forward': target_forward},
       'suggestedRelativeRotation': rotation,
       'suggestedQuaternion': {'x': qx, 'y': qy, 'z': qz, 'w': qw},
       'suggestedRotationMatrix': matrix,
       'assetPath': asset_path,
       'assetHeight': height,
       'targetHeight': target_height,
       'suggestedUniformScale': uniform_scale,
       'applied': applied,
       'warnings': warnings})
)PY";
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_simulate_player_input --------------------------------------------
    add(Tool{
        "ue_simulate_player_input",
        "Inject key and axis input into the PIE PlayerController, then report "
        "the possessed pawn movement delta. Supports keys like W/A/S/D/SpaceBar "
        "and raw axis samples. If PlayerController input injection is not exposed "
        "on the engine's Python API, the tool falls back to AddMovementInput and "
        "reports that fallback explicitly. Python recipe; requires "
        "PythonScriptPlugin and PIE control.",
        json{{"type", "object"},
             {"properties",
              {{"playerIndex", {{"type", "integer"}}},
               {"startPie", {{"type", "boolean"}}},
               {"stopPie", {{"type", "boolean"}}},
               {"durationSeconds", {{"type", "number"}}},
               {"steps", {{"type", "integer"}}},
               {"keys", {{"type", "array"}}},
               {"axes", {{"type", "array"}}},
               {"fallbackMovementInput", {{"type", "boolean"}}}}},
             {"required", json::array({})}},
        {Capability::PythonScripting, Capability::PieControl},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import time
warnings = []
strategies = []
player_index = int(_ARGS.get('playerIndex', 0))
duration = float(_ARGS.get('durationSeconds', 0.25))
steps = max(1, int(_ARGS.get('steps', 6)))
keys = list(_ARGS.get('keys') or [{'key': 'W', 'event': 'Pressed'}])
axes = list(_ARGS.get('axes') or [])
fallback_movement = bool(_ARGS.get('fallbackMovementInput', True))

def call_any(obj, names):
    for name in names:
        fn = getattr(obj, name, None)
        if fn is not None:
            return fn()
    raise RuntimeError('none of methods found: %s' % names)

def vdict(v):
    return {'x': float(v.x), 'y': float(v.y), 'z': float(v.z)}

def dist(a, b):
    return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2) ** 0.5

def enum_value(enum_names, event_name):
    normalized = str(event_name or 'Pressed').upper()
    candidates = {
        'PRESSED': ('IE_PRESSED', 'PRESSED'),
        'RELEASED': ('IE_RELEASED', 'RELEASED'),
        'REPEAT': ('IE_REPEAT', 'REPEAT'),
        'DOUBLECLICK': ('IE_DOUBLE_CLICK', 'DOUBLE_CLICK'),
        'AXIS': ('IE_AXIS', 'AXIS')
    }.get(normalized, ('IE_PRESSED', 'PRESSED'))
    for enum_name in enum_names:
        enum = getattr(unreal, enum_name, None)
        if enum is None:
            continue
        for c in candidates:
            if hasattr(enum, c):
                return getattr(enum, c)
    return None

def key_value(name):
    for ctor in (getattr(unreal, 'Key', None),):
        if ctor is not None:
            try:
                return ctor(str(name))
            except Exception:
                pass
    return str(name)

level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem) if hasattr(unreal, 'LevelEditorSubsystem') else None
if bool(_ARGS.get('startPie', False)):
    if level is None:
        warnings.append('LevelEditorSubsystem unavailable; could not start PIE')
    else:
        try:
            call_any(level, ['editor_request_begin_play', 'EditorRequestBeginPlay'])
            strategies.append('LevelEditorSubsystem.beginPlay')
            time.sleep(0.25)
        except Exception as e:
            warnings.append('start PIE failed: %s' % e)

world = None
try:
    editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    world = editor_subsystem.get_game_world()
except Exception as e:
    warnings.append('get_game_world failed: %s' % e)
if world is None:
    try:
        editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        world = editor_subsystem.get_editor_world()
    except Exception as e:
        warnings.append('get_editor_world failed: %s' % e)
if world is None:
    raise RuntimeError('could not resolve PIE or editor world')

pc = unreal.GameplayStatics.get_player_controller(world, player_index)
if pc is None:
    raise RuntimeError('PlayerController %d not found' % player_index)
pawn = pc.get_pawn()
if pawn is None:
    raise RuntimeError('PlayerController %d has no pawn' % player_index)

before = pawn.get_actor_location()
pc_input_calls = 0
event_enum = ('InputEvent', 'EInputEvent')

for item in keys:
    key = key_value(item.get('key', 'W'))
    ev = enum_value(event_enum, item.get('event', 'Pressed'))
    amount = float(item.get('amount', 1.0))
    gamepad = bool(item.get('gamepad', False))
    if ev is None:
        warnings.append('InputEvent enum unavailable for key %s' % item.get('key'))
        continue
    fn = getattr(pc, 'input_key', None)
    if fn is not None:
        for attempt in ((key, ev, amount, gamepad), (key, ev, amount), (key, ev)):
            try:
                fn(*attempt)
                pc_input_calls += 1
                strategies.append('PlayerController.input_key')
                break
            except Exception:
                pass

dt = duration / float(steps)
for _i in range(steps):
    for item in axes:
        key = key_value(item.get('key', item.get('axis', 'W')))
        value = float(item.get('value', 1.0))
        gamepad = bool(item.get('gamepad', False))
        fn = getattr(pc, 'input_axis', None)
        if fn is not None:
            for attempt in ((key, value, dt, 1, gamepad), (key, value, dt, 1), (key, value, dt)):
                try:
                    fn(*attempt)
                    pc_input_calls += 1
                    strategies.append('PlayerController.input_axis')
                    break
                except Exception:
                    pass
    time.sleep(dt)

fallback_used = False
if pc_input_calls == 0 and fallback_movement:
    forward = 0.0
    right = 0.0
    for item in keys:
        k = str(item.get('key', '')).upper()
        if k in ('W', 'UP'):
            forward += 1.0
        elif k in ('S', 'DOWN'):
            forward -= 1.0
        elif k in ('D', 'RIGHT'):
            right += 1.0
        elif k in ('A', 'LEFT'):
            right -= 1.0
    for item in axes:
        axis = str(item.get('axis', item.get('key', ''))).lower()
        value = float(item.get('value', 0.0))
        if 'forward' in axis or axis.endswith('y'):
            forward += value
        elif 'right' in axis or axis.endswith('x'):
            right += value
    for _i in range(steps):
        if abs(forward) > 0.0001:
            pawn.add_movement_input(pawn.get_actor_forward_vector(), forward, False)
        if abs(right) > 0.0001:
            pawn.add_movement_input(pawn.get_actor_right_vector(), right, False)
        move = pawn.get_component_by_class(unreal.CharacterMovementComponent)
        if move is not None:
            try:
                move.tick_component(dt, unreal.LevelTick.LEVELTICK_All, None)
            except Exception:
                pass
        time.sleep(dt)
    fallback_used = True
    strategies.append('Pawn.add_movement_input fallback')

after = pawn.get_actor_location()

if bool(_ARGS.get('stopPie', False)):
    if level is None:
        warnings.append('LevelEditorSubsystem unavailable; could not stop PIE')
    else:
        try:
            call_any(level, ['editor_request_end_play', 'EditorRequestEndPlay'])
            strategies.append('LevelEditorSubsystem.endPlay')
        except Exception as e:
            warnings.append('stop PIE failed: %s' % e)

_emit({'ok': True,
       'playerIndex': player_index,
       'controllerPath': pc.get_path_name(),
       'pawnPath': pawn.get_path_name(),
       'locationBefore': vdict(before),
       'locationAfter': vdict(after),
       'movedDistance': dist(before, after),
       'playerControllerInputCalls': pc_input_calls,
       'fallbackMovementInputUsed': fallback_used,
       'strategies': list(dict.fromkeys(strategies)),
       'warnings': warnings})
)PY";
            return run_python_recipe(ctx, kRecipe, args);
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
        "destinationPath (a content folder, e.g. \"/Game/Imported\"). The "
        "source file is validated before import: it must be an absolute path "
        "to an existing file with an allowed extension, and (optionally) live "
        "under allowedRootDirs and match expectedSha1/expectedSizeBytes. This "
        "guards against importing a wrong or stale cached file. Returns the "
        "imported asset path(s) plus the source file's name/sha1/size so the "
        "caller can confirm it imported the intended file. Python recipe using "
        "AssetImportTask; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"sourceFile", {{"type", "string"}}},
               {"destinationPath", {{"type", "string"}}},
               {"allowedExtensions",
                {{"type", "array"},
                 {"items", {{"type", "string"}}},
                 {"description", "Override the default allowed extension list, "
                                 "e.g. [\".fbx\", \".glb\"]."}}},
               {"allowedRootDirs",
                {{"type", "array"},
                 {"items", {{"type", "string"}}},
                 {"description", "If set, sourceFile must reside under one of "
                                 "these directories (blocks stale cache dirs)."}}},
               {"expectedSha1",
                {{"type", "string"},
                 {"description", "If set, the file's SHA1 must match exactly."}}},
               {"expectedSizeBytes",
                {{"type", "integer"},
                 {"description", "If set, the file's byte size must match."}}}}},
             {"required", json::array({"sourceFile", "destinationPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const std::string kRecipe = std::string(kImportValidationPreamble) + R"PY(
def _do_import(_v, dst):
    src = _v['resolvedPath']
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', src)
    task.set_editor_property('destination_path', dst)
    task.set_editor_property('automated', True)
    task.set_editor_property('save', True)
    task.set_editor_property('replace_existing', True)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    imported = list(task.get_editor_property('imported_object_paths') or [])
    _emit({'ok': len(imported) > 0,
           'error': '' if imported else 'no objects imported (check source path/format)',
           'imported': imported,
           'sourceFileName': _v['sourceFileName'],
           'sourceFileSha1': _v['sourceFileSha1'],
           'sourceFileSizeBytes': _v['sourceFileSizeBytes']})

_v = _validate_import_source()
if _v is not None:
    _dst = _validate_destination()
    if _dst is not None:
        _do_import(_v, _dst)
)PY";
            if (arg_str(args, "sourceFile").empty() || arg_str(args, "destinationPath").empty())
                return ToolResult::error("sourceFile and destinationPath are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_import_asset_with_transform_policy -------------------------------
    add(Tool{
        "ue_import_asset_with_transform_policy",
        "Import FBX/GLB/glTF or other assets while explicitly setting import "
        "transform policy options such as convertScene, forceFrontXAxis, "
        "convertSceneUnit, importUniformScale, importAsSkeletal, and nested "
        "skeletalMeshImportData/staticMeshImportData overrides where the engine "
        "Python API exposes them. The source file is validated before import "
        "(absolute path, existing file, allowed extension, optional "
        "allowedRootDirs / expectedSha1 / expectedSizeBytes) so a wrong or "
        "stale cached file is rejected early. Returns the source file's "
        "name/sha1/size alongside the imported paths. Python recipe using "
        "AssetImportTask; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"sourceFile", {{"type", "string"}}},
               {"destinationPath", {{"type", "string"}}},
               {"replaceExisting", {{"type", "boolean"}}},
               {"save", {{"type", "boolean"}}},
               {"importAsSkeletal", {{"type", "boolean"}}},
               {"importMaterials", {{"type", "boolean"}}},
               {"importTextures", {{"type", "boolean"}}},
               {"convertScene", {{"type", "boolean"}}},
               {"forceFrontXAxis", {{"type", "boolean"}}},
               {"convertSceneUnit", {{"type", "boolean"}}},
               {"importUniformScale", {{"type", "number"}}},
               {"allowedExtensions",
                {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"allowedRootDirs",
                {{"type", "array"}, {"items", {{"type", "string"}}}}},
               {"expectedSha1", {{"type", "string"}}},
               {"expectedSizeBytes", {{"type", "integer"}}},
               {"skeletalMeshImportData", {{"type", "object"}}},
                {"staticMeshImportData", {{"type", "object"}}}}},
             {"required", json::array({"sourceFile", "destinationPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const std::string kRecipe = std::string(kImportValidationPreamble) + R"PY(
warnings = []
applied = {}
failed = {}

def jsonable(value):
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    try:
        return value.get_path_name()
    except Exception:
        return str(value)

def set_prop(obj, prop, value, label=None):
    label = label or prop
    if obj is None:
        failed[label] = 'target object unavailable'
        return False
    try:
        obj.set_editor_property(prop, value)
        applied[label] = jsonable(value)
        return True
    except Exception as e:
        failed[label] = str(e)
        return False

def get_prop(obj, prop):
    try:
        return obj.get_editor_property(prop)
    except Exception:
        return None

def apply_import_data(data, prefix, extra):
    if data is None:
        failed[prefix] = 'import data object unavailable'
        return
    mapping = (
        ('convertScene', 'convert_scene'),
        ('forceFrontXAxis', 'force_front_x_axis'),
        ('convertSceneUnit', 'convert_scene_unit'),
        ('importUniformScale', 'import_uniform_scale')
    )
    for arg_key, prop in mapping:
        if arg_key in _ARGS:
            set_prop(data, prop, _ARGS[arg_key], '%s.%s' % (prefix, prop))
    for key, value in (extra or {}).items():
        prop = ''.join(['_' + c.lower() if c.isupper() else c for c in str(key)]).lstrip('_')
        if not set_prop(data, prop, value, '%s.%s' % (prefix, prop)):
            set_prop(data, str(key), value, '%s.%s' % (prefix, key))

def _run(_v, dst):
    src = _v['resolvedPath']
    replace = bool(_ARGS.get('replaceExisting', True))
    save = bool(_ARGS.get('save', True))
    task = unreal.AssetImportTask()
    set_prop(task, 'filename', src)
    set_prop(task, 'destination_path', dst)
    set_prop(task, 'automated', True)
    set_prop(task, 'save', save)
    set_prop(task, 'replace_existing', replace)

    lower = src.lower()
    options = None
    if lower.endswith('.fbx') and hasattr(unreal, 'FbxImportUI'):
        options = unreal.FbxImportUI()
        set_prop(options, 'automated_import_should_detect_type', not bool(_ARGS.get('importAsSkeletal', False)),
                 'fbx.automated_import_should_detect_type')
        if 'importAsSkeletal' in _ARGS:
            skeletal = bool(_ARGS.get('importAsSkeletal'))
            set_prop(options, 'import_as_skeletal', skeletal, 'fbx.import_as_skeletal')
            enum = getattr(unreal, 'FBXImportType', None)
            if enum is not None:
                if skeletal and hasattr(enum, 'FBXIT_SKELETAL_MESH'):
                    set_prop(options, 'mesh_type_to_import', enum.FBXIT_SKELETAL_MESH, 'fbx.mesh_type_to_import')
                elif (not skeletal) and hasattr(enum, 'FBXIT_STATIC_MESH'):
                    set_prop(options, 'mesh_type_to_import', enum.FBXIT_STATIC_MESH, 'fbx.mesh_type_to_import')
        if 'importMaterials' in _ARGS:
            set_prop(options, 'import_materials', bool(_ARGS.get('importMaterials')), 'fbx.import_materials')
        if 'importTextures' in _ARGS:
            set_prop(options, 'import_textures', bool(_ARGS.get('importTextures')), 'fbx.import_textures')
        apply_import_data(get_prop(options, 'static_mesh_import_data'), 'staticMeshImportData',
                          _ARGS.get('staticMeshImportData') or {})
        apply_import_data(get_prop(options, 'skeletal_mesh_import_data'), 'skeletalMeshImportData',
                          _ARGS.get('skeletalMeshImportData') or {})
    elif lower.endswith('.glb') or lower.endswith('.gltf'):
        # GLTF importer option class names have changed across engine/plugin
        # versions. Try the known names and set matching properties when present.
        opt_cls = None
        for cls_name in ('GLTFImportOptions', 'GltfImportOptions', 'GLTFImportSettings'):
            opt_cls = getattr(unreal, cls_name, None)
            if opt_cls is not None:
                break
        if opt_cls is not None:
            try:
                options = opt_cls()
                for arg_key, prop in (('importUniformScale', 'import_uniform_scale'),
                                      ('convertScene', 'convert_scene'),
                                      ('forceFrontXAxis', 'force_front_x_axis'),
                                      ('convertSceneUnit', 'convert_scene_unit')):
                    if arg_key in _ARGS:
                        set_prop(options, prop, _ARGS[arg_key], 'gltf.%s' % prop)
            except Exception as e:
                warnings.append('could not create GLTF import options: %s' % e)
        else:
            warnings.append('GLTF import options class not exposed; task will use plugin defaults')
    else:
        warnings.append('no specialized transform import UI for this extension; task will use factory defaults')

    if options is not None:
        set_prop(task, 'options', options, 'task.options')

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    imported = list(task.get_editor_property('imported_object_paths') or [])
    _emit({'ok': len(imported) > 0,
           'error': '' if imported else 'no objects imported (check source path/format/import plugin)',
           'sourceFile': src,
           'destinationPath': dst,
           'imported': imported,
           'sourceFileName': _v['sourceFileName'],
           'sourceFileSha1': _v['sourceFileSha1'],
           'sourceFileSizeBytes': _v['sourceFileSizeBytes'],
           'appliedOptions': applied,
           'failedOptions': failed,
           'warnings': warnings})

_v = _validate_import_source()
if _v is not None:
    _dst = _validate_destination()
    if _dst is not None:
        _run(_v, _dst)
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

    // -- ue_create_character_blueprint ---------------------------------------
    add(Tool{
        "ue_create_character_blueprint",
        "Create a gameplay-ready Character Blueprint and configure its inherited "
        "CapsuleComponent, Mesh, and CharacterMovementComponent. Optional "
        "skeletalMeshPath and animBlueprintPath assign the visible character and "
        "animation class. The recipe also tries to add a SpringArm + Camera using "
        "SubobjectDataSubsystem when available, returning cameraAdded=false with "
        "strategiesTried if this engine's Python API cannot author components. "
        "Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"packagePath", {{"type", "string"}}},
               {"skeletalMeshPath", {{"type", "string"}}},
               {"animBlueprintPath", {{"type", "string"}}},
               {"meshOffset", {{"type", "object"}}},
               {"meshRotation", {{"type", "object"}}},
               {"meshScale", {{"type", "object"}}},
               {"capsuleRadius", {{"type", "number"}}},
               {"capsuleHalfHeight", {{"type", "number"}}},
               {"maxWalkSpeed", {{"type", "number"}}},
               {"jumpZVelocity", {{"type", "number"}}},
               {"airControl", {{"type", "number"}}},
               {"rotationRateYaw", {{"type", "number"}}},
               {"orientRotationToMovement", {{"type", "boolean"}}},
               {"useControllerYawRotation", {{"type", "boolean"}}},
               {"addCamera", {{"type", "boolean"}}},
               {"cameraBoomLength", {{"type", "number"}}},
               {"cameraSocketOffset", {{"type", "object"}}},
               {"reuseExisting", {{"type", "boolean"}}}}},
             {"required", json::array({"name", "packagePath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
name = _ARGS['name']
pkg = _ARGS['packagePath'].rstrip('/')
asset_path = pkg + '/' + name
configured = []
warnings = []
strategies = []

def num(key, default):
    try:
        return float(_ARGS.get(key, default))
    except Exception:
        return float(default)

def bool_arg(key, default):
    v = _ARGS.get(key, default)
    return bool(v)

def vec_arg(key, default):
    src = _ARGS.get(key) or {}
    return unreal.Vector(float(src.get('x', default[0])),
                         float(src.get('y', default[1])),
                         float(src.get('z', default[2])))

def rot_arg(key, default):
    src = _ARGS.get(key) or {}
    return unreal.Rotator(float(src.get('pitch', default[0])),
                          float(src.get('yaw', default[1])),
                          float(src.get('roll', default[2])))

def warn(msg):
    warnings.append(str(msg))

def safe_set(obj, prop, value, label):
    try:
        obj.set_editor_property(prop, value)
        configured.append(label)
        return True
    except Exception as e:
        warn('%s failed: %s' % (label, e))
        return False

def load_asset_or_fail(path, label):
    if not path:
        return None
    obj = unreal.EditorAssetLibrary.load_asset(path)
    if obj is None:
        raise RuntimeError('could not load %s: %s' % (label, path))
    return obj

def get_bp_generated_class(bp):
    try:
        cls = bp.get_editor_property('generated_class')
        if cls is not None:
            return cls
    except Exception:
        pass
    try:
        return bp.generated_class
    except Exception:
        return None

def compile_bp(bp):
    if hasattr(unreal, 'BlueprintEditorLibrary'):
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)

def first_component(cdo, prop_names, cls):
    for prop in prop_names:
        try:
            obj = cdo.get_editor_property(prop)
            if obj is not None:
                return obj
        except Exception:
            pass
    try:
        comps = cdo.get_components_by_class(cls)
        if comps:
            return comps[0]
    except Exception:
        pass
    try:
        return cdo.get_component_by_class(cls)
    except Exception:
        return None

if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
    if not bool_arg('reuseExisting', False):
        raise RuntimeError('asset already exists; pass reuseExisting=true to configure it: %s' % asset_path)
    bp = unreal.EditorAssetLibrary.load_asset(asset_path)
    if bp is None:
        raise RuntimeError('asset exists but could not be loaded: %s' % asset_path)
else:
    factory = unreal.BlueprintFactory()
    factory.set_editor_property('parent_class', unreal.Character)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    bp = tools.create_asset(name, pkg, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError('create_asset returned None: %s' % asset_path)

compile_bp(bp)
generated = get_bp_generated_class(bp)
if generated is None:
    raise RuntimeError('blueprint has no generated class after compile: %s' % bp.get_path_name())

cdo = unreal.get_default_object(generated)
if cdo is None:
    raise RuntimeError('could not get blueprint class default object: %s' % bp.get_path_name())

safe_set(cdo, 'use_controller_rotation_yaw', bool_arg('useControllerYawRotation', False), 'character.useControllerYawRotation')
safe_set(cdo, 'use_controller_rotation_pitch', False, 'character.useControllerRotationPitch')
safe_set(cdo, 'use_controller_rotation_roll', False, 'character.useControllerRotationRoll')

capsule = first_component(cdo, ['capsule_component'], unreal.CapsuleComponent)
if capsule is None:
    warn('CapsuleComponent not found')
else:
    radius = num('capsuleRadius', 34.0)
    half_height = num('capsuleHalfHeight', 88.0)
    try:
        capsule.set_capsule_size(radius, half_height, True)
        configured.append('capsule.size')
    except Exception as e:
        warn('capsule.set_capsule_size failed: %s' % e)
        safe_set(capsule, 'capsule_radius', radius, 'capsule.radius')
        safe_set(capsule, 'capsule_half_height', half_height, 'capsule.halfHeight')

mesh = first_component(cdo, ['mesh'], unreal.SkeletalMeshComponent)
if mesh is None:
    warn('SkeletalMeshComponent not found')
else:
    skel_path = _ARGS.get('skeletalMeshPath') or ''
    if skel_path:
        skel = load_asset_or_fail(skel_path, 'skeletal mesh')
        try:
            mesh.set_skeletal_mesh(skel)
            configured.append('mesh.skeletalMesh')
        except Exception as e:
            warn('mesh.set_skeletal_mesh failed: %s' % e)
            safe_set(mesh, 'skeletal_mesh', skel, 'mesh.skeletalMesh')

    safe_set(mesh, 'relative_location', vec_arg('meshOffset', (0.0, 0.0, -88.0)), 'mesh.relativeLocation')
    safe_set(mesh, 'relative_rotation', rot_arg('meshRotation', (0.0, -90.0, 0.0)), 'mesh.relativeRotation')
    safe_set(mesh, 'relative_scale3d', vec_arg('meshScale', (1.0, 1.0, 1.0)), 'mesh.relativeScale')

    anim_path = _ARGS.get('animBlueprintPath') or ''
    if anim_path:
        anim = load_asset_or_fail(anim_path, 'animation blueprint')
        anim_class = None
        for prop in ('generated_class', 'skeleton_generated_class'):
            try:
                anim_class = anim.get_editor_property(prop)
                if anim_class is not None:
                    break
            except Exception:
                pass
        if anim_class is None and anim_path.endswith('_C'):
            try:
                anim_class = unreal.load_object(None, anim_path)
            except Exception:
                anim_class = None
        if anim_class is None:
            warn('animation blueprint loaded but generated class was unavailable: %s' % anim_path)
        else:
            try:
                mesh.set_animation_mode(unreal.AnimationMode.ANIMATION_BLUEPRINT)
                configured.append('mesh.animationMode')
            except Exception as e:
                warn('mesh.set_animation_mode failed: %s' % e)
            try:
                mesh.set_anim_instance_class(anim_class)
                configured.append('mesh.animClass')
            except Exception as e:
                warn('mesh.set_anim_instance_class failed: %s' % e)
                safe_set(mesh, 'anim_class', anim_class, 'mesh.animClass')

movement = first_component(cdo, ['character_movement'], unreal.CharacterMovementComponent)
if movement is None:
    warn('CharacterMovementComponent not found')
else:
    safe_set(movement, 'max_walk_speed', num('maxWalkSpeed', 500.0), 'movement.maxWalkSpeed')
    safe_set(movement, 'jump_z_velocity', num('jumpZVelocity', 700.0), 'movement.jumpZVelocity')
    safe_set(movement, 'air_control', num('airControl', 0.35), 'movement.airControl')
    safe_set(movement, 'orient_rotation_to_movement', bool_arg('orientRotationToMovement', True), 'movement.orientRotationToMovement')
    safe_set(movement, 'rotation_rate', unreal.Rotator(0.0, num('rotationRateYaw', 540.0), 0.0), 'movement.rotationRate')

camera_added = False
camera_components = []

def obj_from_handle(bp, handle):
    bfl = unreal.SubobjectDataBlueprintFunctionLibrary
    data = bfl.get_data(handle)
    try:
        return bfl.get_object_for_blueprint(data, bp)
    except Exception:
        return bfl.get_object(data)

def is_valid_handle(handle):
    try:
        return unreal.SubobjectDataBlueprintFunctionLibrary.is_handle_valid(handle)
    except Exception:
        return handle is not None

def find_parent_handle(bp, subsystem, handles):
    bfl = unreal.SubobjectDataBlueprintFunctionLibrary
    fallback = handles[0] if handles else None
    for handle in handles:
        try:
            data = bfl.get_data(handle)
            name_text = str(bfl.get_variable_name(data))
            if name_text == 'CapsuleComponent' or bfl.is_root_component(data):
                return handle
        except Exception:
            pass
    return fallback

def add_component(bp, parent_handle, comp_class, comp_name):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    params = unreal.AddNewSubobjectParams(parent_handle=parent_handle,
                                          new_class=comp_class,
                                          blueprint_context=bp,
                                          skip_mark_blueprint_modified=False,
                                          conform_transform_to_parent=False)
    result = subsystem.add_new_subobject(params)
    if isinstance(result, tuple):
        handle = result[0]
        fail_reason = result[1] if len(result) > 1 else ''
    else:
        handle = result
        fail_reason = ''
    if not is_valid_handle(handle):
        raise RuntimeError('add_new_subobject(%s) returned invalid handle: %s' % (comp_name, fail_reason))
    try:
        subsystem.rename_subobject(handle, unreal.Text(comp_name))
    except Exception:
        try:
            subsystem.rename_subobject(handle, comp_name)
        except Exception as e:
            strategies.append('rename %s failed: %s' % (comp_name, e))
    return handle, obj_from_handle(bp, handle)

if bool_arg('addCamera', True):
    if not hasattr(unreal, 'SubobjectDataSubsystem') or not hasattr(unreal, 'AddNewSubobjectParams'):
        strategies.append('SubobjectDataSubsystem unavailable; camera components not authored')
    else:
        try:
            subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
            handles = subsystem.k2_gather_subobject_data_for_blueprint(context=bp)
            parent = find_parent_handle(bp, subsystem, handles)
            if parent is None:
                raise RuntimeError('no parent subobject handle found')
            boom_handle, boom = add_component(bp, parent, unreal.SpringArmComponent, 'CameraBoom')
            if boom is not None:
                safe_set(boom, 'target_arm_length', num('cameraBoomLength', 350.0), 'cameraBoom.targetArmLength')
                safe_set(boom, 'use_pawn_control_rotation', True, 'cameraBoom.usePawnControlRotation')
                safe_set(boom, 'do_collision_test', True, 'cameraBoom.doCollisionTest')
                safe_set(boom, 'socket_offset', vec_arg('cameraSocketOffset', (0.0, 45.0, 65.0)), 'cameraBoom.socketOffset')
                camera_components.append('CameraBoom')
            cam_handle, cam = add_component(bp, boom_handle, unreal.CameraComponent, 'FollowCamera')
            if cam is not None:
                safe_set(cam, 'use_pawn_control_rotation', False, 'camera.usePawnControlRotation')
                camera_components.append('FollowCamera')
            camera_added = 'CameraBoom' in camera_components and 'FollowCamera' in camera_components
            strategies.append('SubobjectDataSubsystem SpringArm+Camera OK')
        except Exception as e:
            strategies.append('SubobjectDataSubsystem SpringArm+Camera failed: %s' % e)

compile_bp(bp)
unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
_emit({'ok': True,
       'assetPath': bp.get_path_name(),
       'parentClass': 'Character',
       'configured': configured,
       'warnings': warnings,
       'cameraAdded': camera_added,
       'cameraComponents': camera_components,
       'strategiesTried': strategies})
)PY";
            if (arg_str(args, "name").empty() || arg_str(args, "packagePath").empty())
                return ToolResult::error("name and packagePath are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_configure_character_movement ------------------------------------
    add(Tool{
        "ue_configure_character_movement",
        "Configure an existing Character Blueprint for grounded third-person "
        "movement: CharacterMovement walking speed, gravity, jump, air control, "
        "step height, walkable floor angle, rotation-to-movement, capsule size, "
        "mesh transform, and optional animation blueprint. Python recipe; "
        "requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"animBlueprintPath", {{"type", "string"}}},
               {"meshOffset", {{"type", "object"}}},
               {"meshRotation", {{"type", "object"}}},
               {"meshScale", {{"type", "object"}}},
               {"capsuleRadius", {{"type", "number"}}},
               {"capsuleHalfHeight", {{"type", "number"}}},
               {"maxWalkSpeed", {{"type", "number"}}},
               {"maxAcceleration", {{"type", "number"}}},
               {"brakingDecelerationWalking", {{"type", "number"}}},
               {"groundFriction", {{"type", "number"}}},
               {"gravityScale", {{"type", "number"}}},
               {"jumpZVelocity", {{"type", "number"}}},
               {"airControl", {{"type", "number"}}},
               {"maxStepHeight", {{"type", "number"}}},
               {"walkableFloorAngle", {{"type", "number"}}},
               {"rotationRateYaw", {{"type", "number"}}},
               {"orientRotationToMovement", {{"type", "boolean"}}},
               {"useControllerYawRotation", {{"type", "boolean"}}}}},
             {"required", json::array({"blueprintPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
bp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if bp is None:
    raise RuntimeError('could not load blueprint: %s' % _ARGS['blueprintPath'])

configured = []
warnings = []

def num(key, default):
    try:
        return float(_ARGS.get(key, default))
    except Exception:
        return float(default)

def bool_arg(key, default):
    return bool(_ARGS.get(key, default))

def vec_arg(key, default):
    src = _ARGS.get(key) or {}
    return unreal.Vector(float(src.get('x', default[0])),
                         float(src.get('y', default[1])),
                         float(src.get('z', default[2])))

def rot_arg(key, default):
    src = _ARGS.get(key) or {}
    return unreal.Rotator(float(src.get('pitch', default[0])),
                          float(src.get('yaw', default[1])),
                          float(src.get('roll', default[2])))

def warn(msg):
    warnings.append(str(msg))

def safe_set(obj, prop, value, label):
    try:
        obj.set_editor_property(prop, value)
        configured.append(label)
        return True
    except Exception as e:
        warn('%s failed: %s' % (label, e))
        return False

def compile_bp(asset):
    if hasattr(unreal, 'BlueprintEditorLibrary'):
        unreal.BlueprintEditorLibrary.compile_blueprint(asset)

def generated_class(asset):
    try:
        return asset.get_editor_property('generated_class')
    except Exception:
        return getattr(asset, 'generated_class', None)

def first_component(cdo, prop_names, cls):
    for prop in prop_names:
        try:
            obj = cdo.get_editor_property(prop)
            if obj is not None:
                return obj
        except Exception:
            pass
    try:
        comps = cdo.get_components_by_class(cls)
        if comps:
            return comps[0]
    except Exception:
        pass
    try:
        return cdo.get_component_by_class(cls)
    except Exception:
        return None

compile_bp(bp)
cls = generated_class(bp)
if cls is None:
    raise RuntimeError('blueprint has no generated class: %s' % bp.get_path_name())
cdo = unreal.get_default_object(cls)
if cdo is None:
    raise RuntimeError('could not get class default object: %s' % bp.get_path_name())

safe_set(cdo, 'use_controller_rotation_yaw', bool_arg('useControllerYawRotation', False), 'character.useControllerYawRotation')
safe_set(cdo, 'use_controller_rotation_pitch', False, 'character.useControllerRotationPitch')
safe_set(cdo, 'use_controller_rotation_roll', False, 'character.useControllerRotationRoll')

capsule = first_component(cdo, ['capsule_component'], unreal.CapsuleComponent)
if capsule is None:
    warn('CapsuleComponent not found')
else:
    radius = num('capsuleRadius', 34.0)
    half_height = num('capsuleHalfHeight', 88.0)
    try:
        capsule.set_capsule_size(radius, half_height, True)
        configured.append('capsule.size')
    except Exception as e:
        warn('capsule.set_capsule_size failed: %s' % e)
        safe_set(capsule, 'capsule_radius', radius, 'capsule.radius')
        safe_set(capsule, 'capsule_half_height', half_height, 'capsule.halfHeight')

mesh = first_component(cdo, ['mesh'], unreal.SkeletalMeshComponent)
if mesh is None:
    warn('SkeletalMeshComponent not found')
else:
    safe_set(mesh, 'relative_location', vec_arg('meshOffset', (0.0, 0.0, -88.0)), 'mesh.relativeLocation')
    safe_set(mesh, 'relative_rotation', rot_arg('meshRotation', (0.0, -90.0, 0.0)), 'mesh.relativeRotation')
    safe_set(mesh, 'relative_scale3d', vec_arg('meshScale', (1.0, 1.0, 1.0)), 'mesh.relativeScale')
    anim_path = _ARGS.get('animBlueprintPath') or ''
    if anim_path:
        anim = unreal.EditorAssetLibrary.load_asset(anim_path)
        if anim is None:
            warn('could not load animation blueprint: %s' % anim_path)
        else:
            anim_class = None
            try:
                anim_class = anim.get_editor_property('generated_class')
            except Exception:
                pass
            if anim_class is None and anim_path.endswith('_C'):
                try:
                    anim_class = unreal.load_object(None, anim_path)
                except Exception:
                    anim_class = None
            if anim_class is None:
                warn('animation blueprint generated class unavailable: %s' % anim_path)
            else:
                try:
                    mesh.set_animation_mode(unreal.AnimationMode.ANIMATION_BLUEPRINT)
                    configured.append('mesh.animationMode')
                except Exception as e:
                    warn('mesh.set_animation_mode failed: %s' % e)
                try:
                    mesh.set_anim_instance_class(anim_class)
                    configured.append('mesh.animClass')
                except Exception as e:
                    warn('mesh.set_anim_instance_class failed: %s' % e)

movement = first_component(cdo, ['character_movement'], unreal.CharacterMovementComponent)
if movement is None:
    warn('CharacterMovementComponent not found; blueprint may not inherit Character')
else:
    safe_set(movement, 'movement_mode', getattr(unreal.MovementMode, 'MOVE_Walking', 1), 'movement.modeWalking')
    safe_set(movement, 'max_walk_speed', num('maxWalkSpeed', 500.0), 'movement.maxWalkSpeed')
    safe_set(movement, 'max_acceleration', num('maxAcceleration', 2048.0), 'movement.maxAcceleration')
    safe_set(movement, 'braking_deceleration_walking', num('brakingDecelerationWalking', 2048.0), 'movement.brakingDecelerationWalking')
    safe_set(movement, 'ground_friction', num('groundFriction', 8.0), 'movement.groundFriction')
    safe_set(movement, 'gravity_scale', num('gravityScale', 1.0), 'movement.gravityScale')
    safe_set(movement, 'jump_z_velocity', num('jumpZVelocity', 700.0), 'movement.jumpZVelocity')
    safe_set(movement, 'air_control', num('airControl', 0.35), 'movement.airControl')
    safe_set(movement, 'max_step_height', num('maxStepHeight', 45.0), 'movement.maxStepHeight')
    safe_set(movement, 'walkable_floor_angle', num('walkableFloorAngle', 44.0), 'movement.walkableFloorAngle')
    safe_set(movement, 'orient_rotation_to_movement', bool_arg('orientRotationToMovement', True), 'movement.orientRotationToMovement')
    safe_set(movement, 'rotation_rate', unreal.Rotator(0.0, num('rotationRateYaw', 540.0), 0.0), 'movement.rotationRate')

compile_bp(bp)
unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
_emit({'ok': True, 'assetPath': bp.get_path_name(), 'configured': configured, 'warnings': warnings})
)PY";
            if (arg_str(args, "blueprintPath").empty())
                return ToolResult::error("blueprintPath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_calibrate_character_collision -----------------------------------
    add(Tool{
        "ue_calibrate_character_collision",
        "Calibrate a Character Blueprint's capsule collision and mesh offset for "
        "a target real-world height. Defaults targetHeightCm=180, "
        "capsuleRadiusCm=34, and footOffsetCm=0. Also sets sensible Pawn "
        "collision profiles when exposed by Python. Python recipe; requires "
        "PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"targetHeightCm", {{"type", "number"}}},
               {"capsuleRadiusCm", {{"type", "number"}}},
               {"capsuleHalfHeightCm", {{"type", "number"}}},
               {"footOffsetCm", {{"type", "number"}}},
               {"meshOffset", {{"type", "object"}}}}},
             {"required", json::array({"blueprintPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
bp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if bp is None:
    raise RuntimeError('could not load blueprint: %s' % _ARGS['blueprintPath'])

configured = []
warnings = []

def num(key, default):
    try:
        return float(_ARGS.get(key, default))
    except Exception:
        return float(default)

def safe_set(obj, prop, value, label):
    try:
        obj.set_editor_property(prop, value)
        configured.append(label)
        return True
    except Exception as e:
        warnings.append('%s failed: %s' % (label, e))
        return False

def compile_bp(asset):
    if hasattr(unreal, 'BlueprintEditorLibrary'):
        unreal.BlueprintEditorLibrary.compile_blueprint(asset)

def generated_class(asset):
    try:
        return asset.get_editor_property('generated_class')
    except Exception:
        return getattr(asset, 'generated_class', None)

def first_component(cdo, prop_names, cls):
    for prop in prop_names:
        try:
            obj = cdo.get_editor_property(prop)
            if obj is not None:
                return obj
        except Exception:
            pass
    try:
        comps = cdo.get_components_by_class(cls)
        if comps:
            return comps[0]
    except Exception:
        pass
    try:
        return cdo.get_component_by_class(cls)
    except Exception:
        return None

compile_bp(bp)
cls = generated_class(bp)
if cls is None:
    raise RuntimeError('blueprint has no generated class: %s' % bp.get_path_name())
cdo = unreal.get_default_object(cls)
if cdo is None:
    raise RuntimeError('could not get class default object: %s' % bp.get_path_name())

target_height = num('targetHeightCm', 180.0)
half_height = num('capsuleHalfHeightCm', target_height * 0.5)
radius = num('capsuleRadiusCm', min(half_height * 0.45, max(target_height * 0.19, 22.0)))
foot_offset = num('footOffsetCm', 0.0)
mesh_offset_arg = _ARGS.get('meshOffset') or {}
mesh_offset = unreal.Vector(float(mesh_offset_arg.get('x', 0.0)),
                            float(mesh_offset_arg.get('y', 0.0)),
                            float(mesh_offset_arg.get('z', -half_height + foot_offset)))

capsule = first_component(cdo, ['capsule_component'], unreal.CapsuleComponent)
if capsule is None:
    warnings.append('CapsuleComponent not found')
else:
    try:
        capsule.set_capsule_size(radius, half_height, True)
        configured.append('capsule.size')
    except Exception as e:
        warnings.append('capsule.set_capsule_size failed: %s' % e)
        safe_set(capsule, 'capsule_radius', radius, 'capsule.radius')
        safe_set(capsule, 'capsule_half_height', half_height, 'capsule.halfHeight')
    try:
        capsule.set_collision_profile_name('Pawn')
        configured.append('capsule.collisionProfilePawn')
    except Exception as e:
        warnings.append('capsule.set_collision_profile_name failed: %s' % e)
    if hasattr(unreal, 'CollisionEnabled'):
        safe_set(capsule, 'collision_enabled', getattr(unreal.CollisionEnabled, 'QUERY_AND_PHYSICS', getattr(unreal.CollisionEnabled, 'QueryAndPhysics', 3)), 'capsule.collisionEnabled')

mesh = first_component(cdo, ['mesh'], unreal.SkeletalMeshComponent)
if mesh is None:
    warnings.append('SkeletalMeshComponent not found')
else:
    safe_set(mesh, 'relative_location', mesh_offset, 'mesh.relativeLocation')
    try:
        mesh.set_collision_profile_name('CharacterMesh')
        configured.append('mesh.collisionProfileCharacterMesh')
    except Exception as e:
        warnings.append('mesh.set_collision_profile_name failed: %s' % e)
    if hasattr(unreal, 'CollisionEnabled'):
        safe_set(mesh, 'collision_enabled', getattr(unreal.CollisionEnabled, 'NO_COLLISION', getattr(unreal.CollisionEnabled, 'NoCollision', 0)), 'mesh.noCollision')

compile_bp(bp)
unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
_emit({'ok': True, 'assetPath': bp.get_path_name(),
       'targetHeightCm': target_height,
       'capsuleRadiusCm': radius,
       'capsuleHalfHeightCm': half_height,
       'meshOffset': {'x': mesh_offset.x, 'y': mesh_offset.y, 'z': mesh_offset.z},
       'configured': configured,
       'warnings': warnings})
)PY";
            if (arg_str(args, "blueprintPath").empty())
                return ToolResult::error("blueprintPath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_configure_third_person_camera -----------------------------------
    add(Tool{
        "ue_configure_third_person_camera",
        "Add or update a Character Blueprint's third-person SpringArm + Camera. "
        "Configures arm length, shoulder socket offset, collision testing, camera "
        "lag / rotation lag, and camera control rotation. Component authoring uses "
        "SubobjectDataSubsystem when available. Python recipe; requires "
        "PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"cameraBoomName", {{"type", "string"}}},
               {"cameraName", {{"type", "string"}}},
               {"targetArmLength", {{"type", "number"}}},
               {"socketOffset", {{"type", "object"}}},
               {"relativeLocation", {{"type", "object"}}},
               {"doCollisionTest", {{"type", "boolean"}}},
               {"probeSize", {{"type", "number"}}},
               {"enableCameraLag", {{"type", "boolean"}}},
               {"cameraLagSpeed", {{"type", "number"}}},
               {"enableCameraRotationLag", {{"type", "boolean"}}},
               {"cameraRotationLagSpeed", {{"type", "number"}}},
               {"usePawnControlRotation", {{"type", "boolean"}}}}},
             {"required", json::array({"blueprintPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
bp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if bp is None:
    raise RuntimeError('could not load blueprint: %s' % _ARGS['blueprintPath'])

configured = []
warnings = []
strategies = []
boom_name = _ARGS.get('cameraBoomName') or 'CameraBoom'
camera_name = _ARGS.get('cameraName') or 'FollowCamera'

def num(key, default):
    try:
        return float(_ARGS.get(key, default))
    except Exception:
        return float(default)

def bool_arg(key, default):
    return bool(_ARGS.get(key, default))

def vec_arg(key, default):
    src = _ARGS.get(key) or {}
    return unreal.Vector(float(src.get('x', default[0])),
                         float(src.get('y', default[1])),
                         float(src.get('z', default[2])))

def safe_set(obj, prop, value, label):
    try:
        obj.set_editor_property(prop, value)
        configured.append(label)
        return True
    except Exception as e:
        warnings.append('%s failed: %s' % (label, e))
        return False

def compile_bp(asset):
    if hasattr(unreal, 'BlueprintEditorLibrary'):
        unreal.BlueprintEditorLibrary.compile_blueprint(asset)

def generated_class(asset):
    try:
        return asset.get_editor_property('generated_class')
    except Exception:
        return getattr(asset, 'generated_class', None)

def comp_name(comp):
    try:
        return comp.get_name()
    except Exception:
        return ''

def components(cdo, cls):
    try:
        return list(cdo.get_components_by_class(cls))
    except Exception:
        try:
            c = cdo.get_component_by_class(cls)
            return [c] if c is not None else []
        except Exception:
            return []

def find_component(cdo, cls, wanted):
    comps = components(cdo, cls)
    for c in comps:
        if comp_name(c) == wanted:
            return c
    return comps[0] if comps else None

def obj_from_handle(asset, handle):
    bfl = unreal.SubobjectDataBlueprintFunctionLibrary
    data = bfl.get_data(handle)
    try:
        return bfl.get_object_for_blueprint(data, asset)
    except Exception:
        return bfl.get_object(data)

def is_valid_handle(handle):
    try:
        return unreal.SubobjectDataBlueprintFunctionLibrary.is_handle_valid(handle)
    except Exception:
        return handle is not None

def root_handle(asset):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = subsystem.k2_gather_subobject_data_for_blueprint(context=asset)
    bfl = unreal.SubobjectDataBlueprintFunctionLibrary
    fallback = handles[0] if handles else None
    for handle in handles:
        try:
            data = bfl.get_data(handle)
            if bfl.is_root_component(data) or str(bfl.get_variable_name(data)) == 'CapsuleComponent':
                return handle
        except Exception:
            pass
    return fallback

def add_component(asset, parent_handle, comp_class, name):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    params = unreal.AddNewSubobjectParams(parent_handle=parent_handle,
                                          new_class=comp_class,
                                          blueprint_context=asset,
                                          skip_mark_blueprint_modified=False,
                                          conform_transform_to_parent=False)
    result = subsystem.add_new_subobject(params)
    handle = result[0] if isinstance(result, tuple) else result
    fail_reason = result[1] if isinstance(result, tuple) and len(result) > 1 else ''
    if not is_valid_handle(handle):
        raise RuntimeError('add_new_subobject(%s) returned invalid handle: %s' % (name, fail_reason))
    try:
        subsystem.rename_subobject(handle, unreal.Text(name))
    except Exception:
        try:
            subsystem.rename_subobject(handle, name)
        except Exception as e:
            strategies.append('rename %s failed: %s' % (name, e))
    return handle, obj_from_handle(asset, handle)

compile_bp(bp)
cls = generated_class(bp)
if cls is None:
    raise RuntimeError('blueprint has no generated class: %s' % bp.get_path_name())
cdo = unreal.get_default_object(cls)
if cdo is None:
    raise RuntimeError('could not get class default object: %s' % bp.get_path_name())

boom = find_component(cdo, unreal.SpringArmComponent, boom_name)
cam = find_component(cdo, unreal.CameraComponent, camera_name)

if (boom is None or cam is None):
    if not hasattr(unreal, 'SubobjectDataSubsystem') or not hasattr(unreal, 'AddNewSubobjectParams'):
        warnings.append('SubobjectDataSubsystem unavailable; missing camera components were not authored')
    else:
        try:
            parent = root_handle(bp)
            if parent is None:
                raise RuntimeError('no parent component handle found')
            if boom is None:
                boom_handle, boom = add_component(bp, parent, unreal.SpringArmComponent, boom_name)
                strategies.append('added SpringArmComponent')
            else:
                boom_handle = parent
            if cam is None:
                cam_handle, cam = add_component(bp, boom_handle, unreal.CameraComponent, camera_name)
                strategies.append('added CameraComponent')
        except Exception as e:
            warnings.append('component authoring failed: %s' % e)

compile_bp(bp)
cls = generated_class(bp)
cdo = unreal.get_default_object(cls)
boom = find_component(cdo, unreal.SpringArmComponent, boom_name)
cam = find_component(cdo, unreal.CameraComponent, camera_name)

if boom is None:
    warnings.append('SpringArmComponent not found after configure attempt')
else:
    safe_set(boom, 'target_arm_length', num('targetArmLength', 350.0), 'cameraBoom.targetArmLength')
    safe_set(boom, 'socket_offset', vec_arg('socketOffset', (0.0, 45.0, 65.0)), 'cameraBoom.socketOffset')
    safe_set(boom, 'relative_location', vec_arg('relativeLocation', (0.0, 0.0, 65.0)), 'cameraBoom.relativeLocation')
    safe_set(boom, 'use_pawn_control_rotation', bool_arg('usePawnControlRotation', True), 'cameraBoom.usePawnControlRotation')
    safe_set(boom, 'do_collision_test', bool_arg('doCollisionTest', True), 'cameraBoom.doCollisionTest')
    safe_set(boom, 'probe_size', num('probeSize', 12.0), 'cameraBoom.probeSize')
    safe_set(boom, 'enable_camera_lag', bool_arg('enableCameraLag', True), 'cameraBoom.enableCameraLag')
    safe_set(boom, 'camera_lag_speed', num('cameraLagSpeed', 12.0), 'cameraBoom.cameraLagSpeed')
    safe_set(boom, 'enable_camera_rotation_lag', bool_arg('enableCameraRotationLag', False), 'cameraBoom.enableCameraRotationLag')
    safe_set(boom, 'camera_rotation_lag_speed', num('cameraRotationLagSpeed', 12.0), 'cameraBoom.cameraRotationLagSpeed')

if cam is None:
    warnings.append('CameraComponent not found after configure attempt')
else:
    safe_set(cam, 'use_pawn_control_rotation', False, 'camera.usePawnControlRotation')

compile_bp(bp)
unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
_emit({'ok': True, 'assetPath': bp.get_path_name(),
       'cameraAdded': boom is not None and cam is not None,
       'cameraBoom': comp_name(boom) if boom is not None else '',
       'camera': comp_name(cam) if cam is not None else '',
       'configured': configured,
       'warnings': warnings,
       'strategiesTried': strategies})
)PY";
            if (arg_str(args, "blueprintPath").empty())
                return ToolResult::error("blueprintPath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_create_enhanced_input_assets ------------------------------------
    add(Tool{
        "ue_create_enhanced_input_assets",
        "Create a standard UE5 Enhanced Input third-person asset set: IA_Move "
        "(Axis2D), IA_Look (Axis2D), IA_Jump (Boolean), and an Input Mapping "
        "Context with WASD, mouse X/Y, and SpaceBar mappings. It also tries to "
        "author the usual Negate / Swizzle / Scalar modifiers for keyboard "
        "2D movement and look sensitivity. Python recipe; requires "
        "PythonScriptPlugin and the EnhancedInput editor classes.",
        json{{"type", "object"},
             {"properties",
              {{"packagePath", {{"type", "string"}}},
               {"contextName", {{"type", "string"}}},
               {"moveActionName", {{"type", "string"}}},
               {"lookActionName", {{"type", "string"}}},
               {"jumpActionName", {{"type", "string"}}},
               {"mouseSensitivity", {{"type", "number"}}},
               {"invertMouseY", {{"type", "boolean"}}},
               {"reuseExisting", {{"type", "boolean"}}}}},
             {"required", json::array({"packagePath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
pkg = _ARGS['packagePath'].rstrip('/')
context_name = _ARGS.get('contextName') or 'IMC_Default'
move_name = _ARGS.get('moveActionName') or 'IA_Move'
look_name = _ARGS.get('lookActionName') or 'IA_Look'
jump_name = _ARGS.get('jumpActionName') or 'IA_Jump'
reuse = bool(_ARGS.get('reuseExisting', False))
mouse_sensitivity = float(_ARGS.get('mouseSensitivity', 1.0))
invert_mouse_y = bool(_ARGS.get('invertMouseY', False))
created = {}
configured = []
warnings = []
mapped = []

def unsupported(msg):
    raise RuntimeError('__UNSUPPORTED__:' + msg)

def cls_any(names):
    for name in names:
        obj = getattr(unreal, name, None)
        if obj is not None:
            return obj
    return None

input_action_cls = cls_any(['InputAction'])
input_context_cls = cls_any(['InputMappingContext'])
input_action_factory_cls = cls_any(['InputAction_Factory', 'InputActionFactory'])
input_context_factory_cls = cls_any(['InputMappingContext_Factory', 'InputMappingContextFactory'])
if input_action_cls is None or input_context_cls is None:
    unsupported('EnhancedInput runtime classes are unavailable; enable the EnhancedInput plugin')
if input_action_factory_cls is None or input_context_factory_cls is None:
    unsupported('EnhancedInput editor factories are unavailable; enable the InputEditor module/plugin')

def asset_path(name):
    return pkg + '/' + name

def create_or_load(name, asset_cls, factory_cls, factory_prop=None):
    path = asset_path(name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        if not reuse:
            raise RuntimeError('asset already exists; pass reuseExisting=true: %s' % path)
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if asset is None:
            raise RuntimeError('asset exists but could not be loaded: %s' % path)
        created[name] = path
        return asset
    factory = factory_cls()
    if factory_prop:
        try:
            factory.set_editor_property(factory_prop, asset_cls)
        except Exception as e:
            warnings.append('factory.%s failed for %s: %s' % (factory_prop, name, e))
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, pkg, asset_cls, factory)
    if asset is None:
        raise RuntimeError('create_asset returned None: %s' % path)
    created[name] = asset.get_path_name()
    return asset

def enum_value(enum_names, candidates):
    for enum_name in enum_names:
        enum_cls = getattr(unreal, enum_name, None)
        if enum_cls is None:
            continue
        for cand in candidates:
            if hasattr(enum_cls, cand):
                return getattr(enum_cls, cand)
    return None

def set_value_type(action, logical):
    if logical == 'axis2d':
        val = enum_value(['InputActionValueType', 'EInputActionValueType'],
                         ['AXIS2D', 'AXIS_2D', 'Axis2D'])
    else:
        val = enum_value(['InputActionValueType', 'EInputActionValueType'],
                         ['BOOLEAN', 'Boolean'])
    if val is None:
        warnings.append('InputActionValueType enum value unavailable for %s' % logical)
        return
    try:
        action.set_editor_property('value_type', val)
        configured.append(action.get_name() + '.valueType')
    except Exception as e:
        warnings.append('set value_type failed for %s: %s' % (action.get_name(), e))

move = create_or_load(move_name, input_action_cls, input_action_factory_cls, 'input_action_class')
look = create_or_load(look_name, input_action_cls, input_action_factory_cls, 'input_action_class')
jump = create_or_load(jump_name, input_action_cls, input_action_factory_cls, 'input_action_class')
context = create_or_load(context_name, input_context_cls, input_context_factory_cls, 'input_mapping_context_class')

set_value_type(move, 'axis2d')
set_value_type(look, 'axis2d')
set_value_type(jump, 'boolean')
try:
    context.unmap_all()
    configured.append(context.get_name() + '.unmapAll')
except Exception as e:
    warnings.append('context.unmap_all failed; duplicate mappings may remain: %s' % e)

def new_modifier(cls_name, props=None):
    cls = cls_any([cls_name])
    if cls is None:
        warnings.append('modifier class unavailable: %s' % cls_name)
        return None
    obj = unreal.new_object(cls, context)
    for key, value in (props or {}).items():
        try:
            obj.set_editor_property(key, value)
        except Exception as e:
            warnings.append('modifier.%s set %s failed: %s' % (cls_name, key, e))
    return obj

def negate():
    return new_modifier('InputModifierNegate')

def swizzle_yxz():
    order = enum_value(['InputAxisSwizzle', 'EInputAxisSwizzle'], ['YXZ'])
    props = {'order': order} if order is not None else {}
    return new_modifier('InputModifierSwizzleAxis', props)

def scalar(x=1.0, y=1.0, z=1.0):
    return new_modifier('InputModifierScalar', {'scalar': unreal.Vector(float(x), float(y), float(z))})

def key_obj(name):
    try:
        return unreal.Key(name)
    except Exception:
        return name

def apply_modifiers(mapping, modifiers, label):
    mods = [m for m in modifiers if m is not None]
    if not mods:
        return
    try:
        mapping.set_editor_property('modifiers', mods)
        configured.append(label + '.modifiers')
    except Exception as e:
        warnings.append('%s set modifiers failed: %s' % (label, e))

def map_key(action, key_name, modifiers=None):
    try:
        mapping = context.map_key(action, key_obj(key_name))
        label = '%s:%s' % (action.get_name(), key_name)
        mapped.append(label)
        apply_modifiers(mapping, modifiers or [], label)
    except Exception as e:
        warnings.append('map_key %s -> %s failed: %s' % (action.get_name(), key_name, e))

map_key(move, 'W', [swizzle_yxz()])
map_key(move, 'S', [negate(), swizzle_yxz()])
map_key(move, 'A', [negate()])
map_key(move, 'D', [])
map_key(look, 'MouseX', [scalar(mouse_sensitivity, mouse_sensitivity, 1.0)] if mouse_sensitivity != 1.0 else [])
look_y_mods = [swizzle_yxz()]
if invert_mouse_y:
    look_y_mods.insert(0, negate())
if mouse_sensitivity != 1.0:
    look_y_mods.append(scalar(mouse_sensitivity, mouse_sensitivity, 1.0))
map_key(look, 'MouseY', look_y_mods)
map_key(jump, 'SpaceBar', [])

for asset in (move, look, jump, context):
    unreal.EditorAssetLibrary.save_asset(asset.get_path_name())

_emit({'ok': True,
       'assetPaths': {'move': move.get_path_name(), 'look': look.get_path_name(),
                      'jump': jump.get_path_name(), 'mappingContext': context.get_path_name()},
       'configured': configured,
       'mapped': mapped,
       'warnings': warnings})
)PY";
            if (arg_str(args, "packagePath").empty())
                return ToolResult::error("packagePath is required");
            ToolResult r = run_python_recipe(ctx, kRecipe, args);
            if (r.is_error && r.payload.is_object()) {
                std::string err = r.payload.value("error", "");
                if (err.find("__UNSUPPORTED__:") != std::string::npos) {
                    json extra = r.payload;
                    extra.erase("status");
                    extra.erase("error");
                    return ToolResult::unsupported(
                        err.substr(err.find("__UNSUPPORTED__:") + 16),
                        std::move(extra));
                }
            }
            return r;
        }});

    // -- ue_create_locomotion_animation_assets ------------------------------
    add(Tool{
        "ue_create_locomotion_animation_assets",
        "Create a locomotion animation asset scaffold for a Character: an "
        "Animation Blueprint and a 1D speed BlendSpace targeting a skeleton, "
        "with optional idle/walk/run AnimationSequence samples when this engine's "
        "Python API exposes BlendSpace sample editing. Python recipe; requires "
        "PythonScriptPlugin and animation editor factories.",
        json{{"type", "object"},
             {"properties",
              {{"packagePath", {{"type", "string"}}},
               {"namePrefix", {{"type", "string"}}},
               {"skeletonPath", {{"type", "string"}}},
               {"previewMeshPath", {{"type", "string"}}},
               {"idleAnimationPath", {{"type", "string"}}},
               {"walkAnimationPath", {{"type", "string"}}},
               {"runAnimationPath", {{"type", "string"}}},
               {"walkSpeed", {{"type", "number"}}},
               {"runSpeed", {{"type", "number"}}},
               {"reuseExisting", {{"type", "boolean"}}}}},
             {"required", json::array({"packagePath", "skeletonPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
pkg = _ARGS['packagePath'].rstrip('/')
prefix = _ARGS.get('namePrefix') or 'Hero'
reuse = bool(_ARGS.get('reuseExisting', False))
walk_speed = float(_ARGS.get('walkSpeed', 200.0))
run_speed = float(_ARGS.get('runSpeed', 500.0))
created = {}
configured = []
warnings = []

def unsupported(msg):
    raise RuntimeError('__UNSUPPORTED__:' + msg)

def cls_any(names):
    for name in names:
        obj = getattr(unreal, name, None)
        if obj is not None:
            return obj
    return None

anim_bp_factory_cls = cls_any(['AnimBlueprintFactory'])
blend_factory_cls = cls_any(['BlendSpaceFactory1D'])
anim_bp_cls = cls_any(['AnimBlueprint'])
blend_cls = cls_any(['BlendSpace1D'])
if anim_bp_factory_cls is None:
    unsupported('AnimBlueprintFactory is unavailable in this Python environment')
if blend_factory_cls is None:
    unsupported('BlendSpaceFactory1D is unavailable in this Python environment')
if anim_bp_cls is None:
    unsupported('AnimBlueprint class is unavailable in this Python environment')
if blend_cls is None:
    unsupported('BlendSpace1D class is unavailable in this Python environment')

skeleton = unreal.EditorAssetLibrary.load_asset(_ARGS['skeletonPath'])
if skeleton is None:
    raise RuntimeError('could not load skeleton: %s' % _ARGS['skeletonPath'])
preview = None
if _ARGS.get('previewMeshPath'):
    preview = unreal.EditorAssetLibrary.load_asset(_ARGS['previewMeshPath'])
    if preview is None:
        warnings.append('could not load preview mesh: %s' % _ARGS['previewMeshPath'])

def asset_path(name):
    return pkg + '/' + name

def create_or_load(name, asset_cls, factory):
    path = asset_path(name)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        if not reuse:
            raise RuntimeError('asset already exists; pass reuseExisting=true: %s' % path)
        asset = unreal.EditorAssetLibrary.load_asset(path)
        if asset is None:
            raise RuntimeError('asset exists but could not be loaded: %s' % path)
        created[name] = path
        return asset
    asset = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, pkg, asset_cls, factory)
    if asset is None:
        raise RuntimeError('create_asset returned None: %s' % path)
    created[name] = asset.get_path_name()
    return asset

anim_factory = anim_bp_factory_cls()
try:
    anim_factory.set_editor_property('target_skeleton', skeleton)
    configured.append('animBlueprint.targetSkeleton')
except Exception as e:
    warnings.append('set anim target_skeleton failed: %s' % e)
try:
    anim_factory.set_editor_property('parent_class', unreal.AnimInstance)
    configured.append('animBlueprint.parentClass')
except Exception as e:
    warnings.append('set anim parent_class failed: %s' % e)
if preview is not None:
    try:
        anim_factory.set_editor_property('preview_skeletal_mesh', preview)
        configured.append('animBlueprint.previewMesh')
    except Exception as e:
        warnings.append('set anim preview_skeletal_mesh failed: %s' % e)
anim_bp = create_or_load('ABP_' + prefix + '_Locomotion', anim_bp_cls, anim_factory)

blend_factory = blend_factory_cls()
try:
    blend_factory.set_editor_property('target_skeleton', skeleton)
    configured.append('blendSpace.targetSkeleton')
except Exception as e:
    warnings.append('set blend target_skeleton failed: %s' % e)
if preview is not None:
    try:
        blend_factory.set_editor_property('preview_skeletal_mesh', preview)
        configured.append('blendSpace.previewMesh')
    except Exception as e:
        warnings.append('set blend preview_skeletal_mesh failed: %s' % e)
blend = create_or_load('BS_' + prefix + '_Speed', blend_cls, blend_factory)

try:
    params = list(blend.get_editor_property('blend_parameters'))
    if params:
        p0 = params[0]
        p0.set_editor_property('display_name', 'Speed')
        p0.set_editor_property('min', 0.0)
        p0.set_editor_property('max', run_speed)
        p0.set_editor_property('grid_num', 5)
        blend.set_editor_property('blend_parameters', params)
        configured.append('blendSpace.speedAxis')
except Exception as e:
    warnings.append('configure blend parameters failed: %s' % e)

def add_sample(label, path, speed):
    if not path:
        return
    seq = unreal.EditorAssetLibrary.load_asset(path)
    if seq is None:
        warnings.append('could not load %s animation: %s' % (label, path))
        return
    try:
        blend.add_sample(seq, unreal.Vector(float(speed), 0.0, 0.0))
        configured.append('blendSpace.sample.' + label)
    except Exception as e:
        warnings.append('add blend sample %s failed; graph/sample editing may need Layer-3 plugin: %s' % (label, e))

add_sample('idle', _ARGS.get('idleAnimationPath') or '', 0.0)
add_sample('walk', _ARGS.get('walkAnimationPath') or '', walk_speed)
add_sample('run', _ARGS.get('runAnimationPath') or '', run_speed)

if hasattr(unreal, 'BlueprintEditorLibrary'):
    try:
        unreal.BlueprintEditorLibrary.compile_blueprint(anim_bp)
        configured.append('animBlueprint.compile')
    except Exception as e:
        warnings.append('compile animation blueprint failed: %s' % e)
unreal.EditorAssetLibrary.save_asset(anim_bp.get_path_name())
unreal.EditorAssetLibrary.save_asset(blend.get_path_name())

_emit({'ok': True,
       'assetPaths': {'animBlueprint': anim_bp.get_path_name(),
                      'blendSpace': blend.get_path_name()},
       'configured': configured,
       'warnings': warnings})
)PY";
            if (arg_str(args, "packagePath").empty() ||
                arg_str(args, "skeletonPath").empty())
                return ToolResult::error("packagePath and skeletonPath are required");
            ToolResult r = run_python_recipe(ctx, kRecipe, args);
            if (r.is_error && r.payload.is_object()) {
                std::string err = r.payload.value("error", "");
                if (err.find("__UNSUPPORTED__:") != std::string::npos) {
                    json extra = r.payload;
                    extra.erase("status");
                    extra.erase("error");
                    return ToolResult::unsupported(
                        err.substr(err.find("__UNSUPPORTED__:") + 16),
                        std::move(extra));
                }
            }
            return r;
        }});

    // -- ue_set_game_defaults -----------------------------------------------
    add(Tool{
        "ue_set_game_defaults",
        "Persist project GameMapsSettings: Game Default Map, Editor Startup Map, "
        "and default GameMode class. Accepts Blueprint asset paths for gameModePath "
        "and converts them to generated classes. Optionally saves the current "
        "level first. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"gameDefaultMap", {{"type", "string"}}},
               {"editorStartupMap", {{"type", "string"}}},
               {"gameModePath", {{"type", "string"}}},
               {"saveCurrentLevel", {{"type", "boolean"}}}}},
             {"required", json::array({})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
configured = []
warnings = []

def unsupported(msg):
    raise RuntimeError('__UNSUPPORTED__:' + msg)

settings_cls = getattr(unreal, 'GameMapsSettings', None)
if settings_cls is None:
    unsupported('GameMapsSettings is unavailable in this Python environment')
settings = unreal.get_default_object(settings_cls)
if settings is None:
    raise RuntimeError('could not get GameMapsSettings default object')

def object_path(path):
    if not path:
        return ''
    if '.' in path:
        return path
    leaf = path.rsplit('/', 1)[-1]
    return path + '.' + leaf

def class_from_path(path):
    if not path:
        return None
    if path.endswith('_C'):
        try:
            return unreal.load_object(None, path)
        except Exception:
            return None
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is not None:
        try:
            cls = asset.get_editor_property('generated_class')
            if cls is not None:
                return cls
        except Exception:
            pass
    try:
        return unreal.load_object(None, object_path(path) + '_C')
    except Exception:
        return None

def safe_set(prop, value, label):
    try:
        settings.set_editor_property(prop, value)
        configured.append(label)
        return True
    except Exception as e:
        warnings.append('%s failed: %s' % (label, e))
        return False

if bool(_ARGS.get('saveCurrentLevel', False)):
    try:
        unreal.EditorLevelLibrary.save_current_level()
        configured.append('level.saveCurrent')
    except Exception as e:
        warnings.append('save_current_level failed: %s' % e)

game_map = _ARGS.get('gameDefaultMap') or ''
editor_map = _ARGS.get('editorStartupMap') or game_map
if game_map:
    safe_set('game_default_map', object_path(game_map), 'settings.gameDefaultMap')
if editor_map:
    safe_set('editor_startup_map', object_path(editor_map), 'settings.editorStartupMap')

gm_path = _ARGS.get('gameModePath') or ''
gm_cls = class_from_path(gm_path)
if gm_path and gm_cls is None:
    warnings.append('could not resolve gameModePath to class: %s' % gm_path)
elif gm_cls is not None:
    safe_set('global_default_game_mode', gm_cls, 'settings.globalDefaultGameMode')

try:
    settings.save_config()
    configured.append('settings.saveConfig')
except Exception as e:
    warnings.append('settings.save_config failed: %s' % e)

_emit({'ok': True, 'configured': configured, 'warnings': warnings,
       'gameDefaultMap': game_map, 'editorStartupMap': editor_map,
       'gameModePath': gm_path})
)PY";
            ToolResult r = run_python_recipe(ctx, kRecipe, args);
            if (r.is_error && r.payload.is_object()) {
                std::string err = r.payload.value("error", "");
                if (err.find("__UNSUPPORTED__:") != std::string::npos) {
                    json extra = r.payload;
                    extra.erase("status");
                    extra.erase("error");
                    return ToolResult::unsupported(
                        err.substr(err.find("__UNSUPPORTED__:") + 16),
                        std::move(extra));
                }
            }
            return r;
        }});

    // -- ue_validate_third_person_pie ---------------------------------------
    add(Tool{
        "ue_validate_third_person_pie",
        "Start or inspect a PIE session and report the Player0 pawn class, "
        "CharacterMovement presence, camera presence, controller possession, and "
        "whether a short movement-input tick changes location. Useful as a final "
        "third-person setup smoke test. Python recipe; requires PythonScriptPlugin "
        "and PIE control capability.",
        json{{"type", "object"},
             {"properties",
              {{"expectedPawnClassPath", {{"type", "string"}}},
               {"startPie", {{"type", "boolean"}}},
               {"stopPie", {{"type", "boolean"}}},
               {"moveDirection", {{"type", "object"}}},
               {"moveScale", {{"type", "number"}}},
               {"steps", {{"type", "integer"}}},
               {"deltaSeconds", {{"type", "number"}}}}},
             {"required", json::array({})}},
        {Capability::PythonScripting, Capability::PieControl},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import time
warnings = []
diagnostics = {}

def bool_arg(key, default):
    return bool(_ARGS.get(key, default))

def num(key, default):
    try:
        return float(_ARGS.get(key, default))
    except Exception:
        return float(default)

def call_any(obj, names):
    for name in names:
        fn = getattr(obj, name, None)
        if fn is not None:
            return fn()
    raise RuntimeError('none of methods found: %s' % names)

def vec_arg(key, default):
    src = _ARGS.get(key) or {}
    return unreal.Vector(float(src.get('x', default[0])),
                         float(src.get('y', default[1])),
                         float(src.get('z', default[2])))

def dist(a, b):
    return ((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2) ** 0.5

level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem) if hasattr(unreal, 'LevelEditorSubsystem') else None
if bool_arg('startPie', True):
    if level is None:
        warnings.append('LevelEditorSubsystem unavailable; could not start PIE')
    else:
        try:
            call_any(level, ['editor_request_begin_play', 'EditorRequestBeginPlay'])
            diagnostics['startPieRequested'] = True
            time.sleep(0.25)
        except Exception as e:
            warnings.append('start PIE failed: %s' % e)

world = None
try:
    editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    world = editor_subsystem.get_game_world()
    diagnostics['worldSource'] = 'gameWorld'
except Exception as e:
    warnings.append('get_game_world failed: %s' % e)
if world is None:
    try:
        editor_subsystem = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
        world = editor_subsystem.get_editor_world()
        diagnostics['worldSource'] = 'editorWorld'
    except Exception as e:
        warnings.append('get_editor_world failed: %s' % e)
if world is None:
    raise RuntimeError('could not resolve PIE or editor world')

pc = unreal.GameplayStatics.get_player_controller(world, 0)
if pc is None:
    raise RuntimeError('PlayerController 0 not found')
pawn = pc.get_pawn()
if pawn is None:
    raise RuntimeError('PlayerController 0 has no pawn')

pawn_class = pawn.get_class().get_path_name()
expected = _ARGS.get('expectedPawnClassPath') or ''
diagnostics['pawnPath'] = pawn.get_path_name()
diagnostics['pawnClassPath'] = pawn_class
diagnostics['expectedPawnClassPath'] = expected
diagnostics['expectedPawnClassMatches'] = (not expected) or (expected in pawn_class or pawn_class in expected)
diagnostics['controllerPath'] = pc.get_path_name()
diagnostics['possessed'] = True

movement = None
try:
    movement = pawn.get_component_by_class(unreal.CharacterMovementComponent)
except Exception:
    movement = None
diagnostics['hasCharacterMovement'] = movement is not None
if movement is not None:
    try:
        diagnostics['movementMode'] = str(movement.get_editor_property('movement_mode'))
    except Exception:
        pass

try:
    cameras = list(pawn.get_components_by_class(unreal.CameraComponent))
except Exception:
    cameras = []
diagnostics['cameraCount'] = len(cameras)
diagnostics['hasCamera'] = len(cameras) > 0

before = pawn.get_actor_location()
after = before
manual_tick_used = False
if movement is not None:
    direction = vec_arg('moveDirection', (1.0, 0.0, 0.0))
    scale = num('moveScale', 1.0)
    steps = int(_ARGS.get('steps', 8))
    dt = num('deltaSeconds', 1.0 / 30.0)
    for _i in range(max(1, steps)):
        try:
            pawn.add_movement_input(direction, scale, False)
        except Exception as e:
            warnings.append('add_movement_input failed: %s' % e)
            break
        try:
            movement.tick_component(dt, unreal.LevelTick.LEVELTICK_All, None)
            manual_tick_used = True
        except Exception as e:
            warnings.append('manual movement tick failed: %s' % e)
            break
    after = pawn.get_actor_location()

diagnostics['locationBefore'] = {'x': before.x, 'y': before.y, 'z': before.z}
diagnostics['locationAfter'] = {'x': after.x, 'y': after.y, 'z': after.z}
diagnostics['movedDistance'] = dist(before, after)
diagnostics['movementInputMoved'] = diagnostics['movedDistance'] > 0.1
diagnostics['manualTickUsed'] = manual_tick_used
diagnostics['okThirdPersonSmoke'] = bool(diagnostics['expectedPawnClassMatches'] and
                                         diagnostics['hasCharacterMovement'] and
                                         diagnostics['hasCamera'])

if bool_arg('stopPie', True):
    if level is None:
        warnings.append('LevelEditorSubsystem unavailable; could not stop PIE')
    else:
        try:
            call_any(level, ['editor_request_end_play', 'EditorRequestEndPlay'])
            diagnostics['stopPieRequested'] = True
        except Exception as e:
            warnings.append('stop PIE failed: %s' % e)

_emit({'ok': True, 'diagnostics': diagnostics, 'warnings': warnings})
)PY";
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

    // -- ue_inspect_widget_blueprint -----------------------------------------
    add(Tool{
        "ue_inspect_widget_blueprint",
        "Inspect a UMG Widget Blueprint's widget tree. Returns root widget, "
        "named descendants, parent/child relationships, slot types, and common "
        "text/value properties. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"includeSlotProperties", {{"type", "boolean"}}}}},
             {"required", json::array({"blueprintPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
def class_name(obj):
    try:
        return obj.get_class().get_name()
    except Exception:
        return type(obj).__name__

def obj_name(obj):
    try:
        return obj.get_name()
    except Exception:
        return str(obj)

def jsonable(value):
    if value is None:
        return None
    for keys in (('x', 'y'), ('r', 'g', 'b', 'a'),
                 ('left', 'top', 'right', 'bottom')):
        try:
            return {k: float(getattr(value, k)) for k in keys}
        except Exception:
            pass
    try:
        return {
            'minimum': jsonable(value.minimum),
            'maximum': jsonable(value.maximum),
        }
    except Exception:
        pass
    if isinstance(value, (str, int, float, bool)):
        return value
    return str(value)

def children_of(widget):
    out = []
    for method in ('get_children_count', 'GetChildrenCount'):
        if hasattr(widget, method):
            try:
                count = int(getattr(widget, method)())
                for i in range(count):
                    child = widget.get_child_at(i)
                    if child is not None:
                        out.append(child)
                return out
            except Exception:
                pass
    for method in ('get_all_children', 'GetAllChildren'):
        if hasattr(widget, method):
            try:
                return list(getattr(widget, method)() or [])
            except Exception:
                pass
    return out

def slot_for(widget):
    try:
        return widget.get_editor_property('slot')
    except Exception:
        return None

def slot_props(slot):
    if slot is None or not bool(_ARGS.get('includeSlotProperties', True)):
        return {}
    props = {'slotType': class_name(slot)}
    for prop in ('position', 'size', 'offsets', 'anchors', 'alignment',
                 'padding', 'horizontal_alignment', 'vertical_alignment',
                 'z_order', 'auto_size'):
        try:
            props[prop] = jsonable(slot.get_editor_property(prop))
        except Exception:
            pass
    return props

def text_value(widget):
    for method in ('get_text', 'GetText'):
        if hasattr(widget, method):
            try:
                return str(getattr(widget, method)())
            except Exception:
                pass
    try:
        return str(widget.get_editor_property('text'))
    except Exception:
        return None

wbp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if wbp is None:
    _emit({'ok': False, 'error': 'could not load widget blueprint'})
else:
    try:
        tree = wbp.get_editor_property('widget_tree')
    except Exception as e:
        tree = None
        tree_error = str(e)
    if tree is None:
        _emit({'ok': False, 'error': 'widget_tree unavailable', 'details': tree_error if 'tree_error' in globals() else ''})
    else:
        try:
            root = tree.get_editor_property('root_widget')
        except Exception:
            root = None
        widgets = []
        seen = set()
        def visit(widget, parent_name):
            if widget is None:
                return
            key = obj_name(widget)
            if key in seen:
                return
            seen.add(key)
            child_objs = children_of(widget)
            item = {
                'name': key,
                'type': class_name(widget),
                'parent': parent_name,
                'children': [obj_name(c) for c in child_objs],
            }
            value = text_value(widget)
            if value is not None:
                item['text'] = value
            item.update(slot_props(slot_for(widget)))
            widgets.append(item)
            for child in child_objs:
                visit(child, key)
        visit(root, '')
        _emit({'ok': True,
               'assetPath': wbp.get_path_name(),
               'root': {'name': obj_name(root), 'type': class_name(root)} if root else None,
               'widgetCount': len(widgets),
               'widgets': widgets})
)PY";
            if (arg_str(args, "blueprintPath").empty())
                return ToolResult::error("blueprintPath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_add_widget_to_panel ----------------------------------------------
    add(Tool{
        "ue_add_widget_to_panel",
        "Add a widget to a named UMG panel or content widget. Supports root "
        "fallback, parentName, text, a properties object, and a layout object "
        "for CanvasPanelSlot/box/overlay padding and alignment. Python recipe; "
        "requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"parentName", {{"type", "string"}, {"description", "empty means root widget"}}},
               {"widgetType", {{"type", "string"}}},
               {"widgetName", {{"type", "string"}}},
               {"text", {{"type", "string"}}},
               {"properties", {{"type", "object"}}},
               {"layout", {{"type", "object"}}}}},
             {"required", json::array({"blueprintPath", "widgetType", "widgetName"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
applied = []
warnings = []

def warn(msg):
    warnings.append(str(msg))

def vec2(src, default=(0.0, 0.0)):
    src = src or {}
    return unreal.Vector2D(float(src.get('x', default[0])),
                           float(src.get('y', default[1])))

def margin(src):
    src = src or {}
    return unreal.Margin(float(src.get('left', 0.0)),
                         float(src.get('top', 0.0)),
                         float(src.get('right', 0.0)),
                         float(src.get('bottom', 0.0)))

def anchors(src):
    src = src or {}
    mn = src.get('minimum') or src.get('min') or {}
    mx = src.get('maximum') or src.get('max') or {}
    minx = float(mn.get('x', src.get('minX', 0.0)))
    miny = float(mn.get('y', src.get('minY', 0.0)))
    maxx = float(mx.get('x', src.get('maxX', minx)))
    maxy = float(mx.get('y', src.get('maxY', miny)))
    try:
        return unreal.Anchors(minimum=unreal.Vector2D(minx, miny),
                              maximum=unreal.Vector2D(maxx, maxy))
    except Exception:
        return unreal.Anchors(minx, miny, maxx, maxy)

def enum_value(enum_type, name, prefix):
    raw = str(name or '').replace('-', '_').replace(' ', '_')
    candidates = [
        raw,
        raw.upper(),
        prefix + raw.upper(),
        prefix + '_' + raw.upper(),
        prefix + raw.capitalize(),
        prefix.capitalize() + '_' + raw.capitalize(),
    ]
    for cand in candidates:
        if hasattr(enum_type, cand):
            return getattr(enum_type, cand)
    raise RuntimeError('unknown enum value %s for %s' % (name, enum_type))

def safe_call(obj, method, value, label):
    if hasattr(obj, method):
        try:
            getattr(obj, method)(value)
            applied.append(label)
            return True
        except Exception as e:
            warn('%s failed: %s' % (label, e))
    return False

def safe_set(obj, prop, value, label):
    try:
        obj.set_editor_property(prop, value)
        applied.append(label)
        return True
    except Exception as e:
        warn('%s failed: %s' % (label, e))
        return False

def children_of(widget):
    try:
        return [widget.get_child_at(i) for i in range(int(widget.get_children_count()))]
    except Exception:
        try:
            return list(widget.get_all_children() or [])
        except Exception:
            return []

def obj_name(obj):
    try:
        return obj.get_name()
    except Exception:
        return str(obj)

def find_widget(tree, root, name):
    if not name:
        return root
    try:
        found = tree.find_widget(name)
        if found is not None:
            return found
    except Exception:
        pass
    stack = [root]
    while stack:
        item = stack.pop(0)
        if item is None:
            continue
        if obj_name(item) == name:
            return item
        stack.extend(children_of(item))
    return None

def class_for_widget(widget_type):
    cls = getattr(unreal, widget_type, None)
    if cls is not None:
        return cls
    if widget_type.startswith('/'):
        for path in (widget_type, widget_type + '_C'):
            try:
                cls = unreal.load_object(None, path)
                if cls is not None:
                    return cls
            except Exception:
                pass
    return None

def widget_slot(widget):
    try:
        return widget.get_editor_property('slot')
    except Exception:
        return None

def apply_properties(widget, props):
    props = dict(props or {})
    if 'text' in props:
        safe_call(widget, 'set_text', unreal.Text(str(props['text'])), 'text')
        if 'text' not in applied:
            safe_set(widget, 'text', unreal.Text(str(props['text'])), 'text')
    if 'tooltipText' in props:
        safe_set(widget, 'tool_tip_text', unreal.Text(str(props['tooltipText'])), 'tooltipText')
    if 'visibility' in props and hasattr(unreal, 'SlateVisibility'):
        try:
            safe_call(widget, 'set_visibility',
                      enum_value(unreal.SlateVisibility, props['visibility'], ''),
                      'visibility')
        except Exception as e:
            warn('visibility failed: %s' % e)
    if 'isEnabled' in props:
        safe_call(widget, 'set_is_enabled', bool(props['isEnabled']), 'isEnabled')
        safe_set(widget, 'is_enabled', bool(props['isEnabled']), 'isEnabled')
    if 'renderOpacity' in props:
        safe_set(widget, 'render_opacity', float(props['renderOpacity']), 'renderOpacity')
    if 'fontSize' in props:
        try:
            font = widget.get_editor_property('font')
            font.set_editor_property('size', int(props['fontSize']))
            if safe_call(widget, 'set_font', font, 'fontSize') is False:
                safe_set(widget, 'font', font, 'fontSize')
        except Exception as e:
            warn('fontSize failed: %s' % e)
    if 'color' in props:
        c = props['color'] or {}
        color = unreal.LinearColor(float(c.get('r', 1.0)), float(c.get('g', 1.0)),
                                   float(c.get('b', 1.0)), float(c.get('a', 1.0)))
        if not safe_call(widget, 'set_color_and_opacity', color, 'color'):
            try:
                safe_call(widget, 'set_color_and_opacity', unreal.SlateColor(color), 'color')
            except Exception:
                pass
            safe_set(widget, 'color_and_opacity', color, 'color')
    if 'brushColor' in props:
        c = props['brushColor'] or {}
        color = unreal.LinearColor(float(c.get('r', 1.0)), float(c.get('g', 1.0)),
                                   float(c.get('b', 1.0)), float(c.get('a', 1.0)))
        safe_call(widget, 'set_brush_color', color, 'brushColor')
    if 'imagePath' in props:
        asset = unreal.EditorAssetLibrary.load_asset(str(props['imagePath']))
        if asset is None:
            warn('imagePath could not be loaded: %s' % props['imagePath'])
        elif hasattr(widget, 'set_brush_from_texture'):
            safe_call(widget, 'set_brush_from_texture', asset, 'imagePath')
        elif hasattr(widget, 'set_brush_resource_object'):
            safe_call(widget, 'set_brush_resource_object', asset, 'imagePath')

def apply_layout(slot, layout):
    layout = dict(layout or {})
    if slot is None:
        warn('layout skipped: widget has no slot')
        return
    if 'position' in layout:
        safe_call(slot, 'set_position', vec2(layout['position']), 'slot.position')
    if 'size' in layout:
        safe_call(slot, 'set_size', vec2(layout['size']), 'slot.size')
    if 'anchors' in layout:
        safe_call(slot, 'set_anchors', anchors(layout['anchors']), 'slot.anchors')
    if 'alignment' in layout:
        safe_call(slot, 'set_alignment', vec2(layout['alignment']), 'slot.alignment')
    if 'offsets' in layout:
        safe_call(slot, 'set_offsets', margin(layout['offsets']), 'slot.offsets')
    if 'padding' in layout:
        safe_call(slot, 'set_padding', margin(layout['padding']), 'slot.padding')
    if 'autoSize' in layout:
        safe_call(slot, 'set_auto_size', bool(layout['autoSize']), 'slot.autoSize')
    if 'zOrder' in layout:
        safe_call(slot, 'set_z_order', int(layout['zOrder']), 'slot.zOrder')
    if 'horizontalAlignment' in layout and hasattr(unreal, 'HorizontalAlignment'):
        try:
            safe_call(slot, 'set_horizontal_alignment',
                      enum_value(unreal.HorizontalAlignment, layout['horizontalAlignment'], 'HALIGN'),
                      'slot.horizontalAlignment')
        except Exception as e:
            warn('horizontalAlignment failed: %s' % e)
    if 'verticalAlignment' in layout and hasattr(unreal, 'VerticalAlignment'):
        try:
            safe_call(slot, 'set_vertical_alignment',
                      enum_value(unreal.VerticalAlignment, layout['verticalAlignment'], 'VALIGN'),
                      'slot.verticalAlignment')
        except Exception as e:
            warn('verticalAlignment failed: %s' % e)

wbp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if wbp is None:
    _emit({'ok': False, 'error': 'could not load widget blueprint'})
else:
    tree = wbp.get_editor_property('widget_tree')
    root = tree.get_editor_property('root_widget') if tree else None
    if tree is None or root is None:
        _emit({'ok': False, 'error': 'widget blueprint has no root widget'})
    else:
        parent = find_widget(tree, root, _ARGS.get('parentName') or '')
        if parent is None:
            _emit({'ok': False, 'error': 'parent widget not found: %s' % (_ARGS.get('parentName') or '')})
        else:
            cls = class_for_widget(_ARGS['widgetType'])
            if cls is None:
                _emit({'ok': False, 'error': 'unknown widget type: %s' % _ARGS['widgetType']})
            else:
                child = tree.construct_widget(cls, _ARGS['widgetName'])
                slot = None
                added = False
                if hasattr(parent, 'add_child'):
                    try:
                        slot = parent.add_child(child)
                        added = True
                    except Exception as e:
                        warn('parent.add_child failed: %s' % e)
                if not added and hasattr(parent, 'set_content'):
                    try:
                        parent.set_content(child)
                        slot = widget_slot(child)
                        added = True
                    except Exception as e:
                        warn('parent.set_content failed: %s' % e)
                if not added:
                    _emit({'ok': False, 'error': 'parent does not accept children/content: %s' % obj_name(parent), 'warnings': warnings})
                else:
                    props = dict(_ARGS.get('properties') or {})
                    if _ARGS.get('text') is not None and 'text' not in props:
                        props['text'] = _ARGS.get('text')
                    apply_properties(child, props)
                    apply_layout(slot or widget_slot(child), _ARGS.get('layout') or {})
                    if hasattr(unreal, 'BlueprintEditorLibrary'):
                        unreal.BlueprintEditorLibrary.compile_blueprint(wbp)
                    unreal.EditorAssetLibrary.save_asset(wbp.get_path_name())
                    _emit({'ok': True, 'assetPath': wbp.get_path_name(),
                           'widget': obj_name(child), 'type': _ARGS['widgetType'],
                           'parent': obj_name(parent), 'added': added,
                           'applied': applied, 'warnings': warnings})
)PY";
            if (arg_str(args, "blueprintPath").empty() ||
                arg_str(args, "widgetType").empty() ||
                arg_str(args, "widgetName").empty())
                return ToolResult::error("blueprintPath, widgetType and widgetName are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_set_widget_properties --------------------------------------------
    add(Tool{
        "ue_set_widget_properties",
        "Set common UMG widget properties by widgetName: text, tooltipText, "
        "visibility, isEnabled, renderOpacity, fontSize, color, brushColor, and "
        "imagePath. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"widgetName", {{"type", "string"}, {"description", "empty means root widget"}}},
               {"properties", {{"type", "object"}}}}},
             {"required", json::array({"blueprintPath", "properties"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
applied = []
warnings = []

def warn(msg):
    warnings.append(str(msg))

def children_of(widget):
    try:
        return [widget.get_child_at(i) for i in range(int(widget.get_children_count()))]
    except Exception:
        try:
            return list(widget.get_all_children() or [])
        except Exception:
            return []

def obj_name(obj):
    try:
        return obj.get_name()
    except Exception:
        return str(obj)

def find_widget(tree, root, name):
    if not name:
        return root
    try:
        found = tree.find_widget(name)
        if found is not None:
            return found
    except Exception:
        pass
    stack = [root]
    while stack:
        item = stack.pop(0)
        if item is None:
            continue
        if obj_name(item) == name:
            return item
        stack.extend(children_of(item))
    return None

def enum_value(enum_type, name):
    raw = str(name or '').replace('-', '_').replace(' ', '_')
    candidates = [raw, raw.upper(), raw.capitalize()]
    for cand in candidates:
        if hasattr(enum_type, cand):
            return getattr(enum_type, cand)
    raise RuntimeError('unknown enum value %s for %s' % (name, enum_type))

def safe_call(obj, method, value, label):
    if hasattr(obj, method):
        try:
            getattr(obj, method)(value)
            applied.append(label)
            return True
        except Exception as e:
            warn('%s failed: %s' % (label, e))
    return False

def safe_set(obj, prop, value, label):
    try:
        obj.set_editor_property(prop, value)
        applied.append(label)
        return True
    except Exception as e:
        warn('%s failed: %s' % (label, e))
        return False

def apply_properties(widget, props):
    props = dict(props or {})
    if 'text' in props:
        if not safe_call(widget, 'set_text', unreal.Text(str(props['text'])), 'text'):
            safe_set(widget, 'text', unreal.Text(str(props['text'])), 'text')
    if 'tooltipText' in props:
        safe_set(widget, 'tool_tip_text', unreal.Text(str(props['tooltipText'])), 'tooltipText')
    if 'visibility' in props and hasattr(unreal, 'SlateVisibility'):
        try:
            safe_call(widget, 'set_visibility',
                      enum_value(unreal.SlateVisibility, props['visibility']),
                      'visibility')
        except Exception as e:
            warn('visibility failed: %s' % e)
    if 'isEnabled' in props:
        if not safe_call(widget, 'set_is_enabled', bool(props['isEnabled']), 'isEnabled'):
            safe_set(widget, 'is_enabled', bool(props['isEnabled']), 'isEnabled')
    if 'renderOpacity' in props:
        safe_set(widget, 'render_opacity', float(props['renderOpacity']), 'renderOpacity')
    if 'fontSize' in props:
        try:
            font = widget.get_editor_property('font')
            font.set_editor_property('size', int(props['fontSize']))
            if not safe_call(widget, 'set_font', font, 'fontSize'):
                safe_set(widget, 'font', font, 'fontSize')
        except Exception as e:
            warn('fontSize failed: %s' % e)
    if 'color' in props:
        c = props['color'] or {}
        color = unreal.LinearColor(float(c.get('r', 1.0)), float(c.get('g', 1.0)),
                                   float(c.get('b', 1.0)), float(c.get('a', 1.0)))
        if not safe_call(widget, 'set_color_and_opacity', color, 'color'):
            try:
                safe_call(widget, 'set_color_and_opacity', unreal.SlateColor(color), 'color')
            except Exception:
                pass
            safe_set(widget, 'color_and_opacity', color, 'color')
    if 'brushColor' in props:
        c = props['brushColor'] or {}
        color = unreal.LinearColor(float(c.get('r', 1.0)), float(c.get('g', 1.0)),
                                   float(c.get('b', 1.0)), float(c.get('a', 1.0)))
        safe_call(widget, 'set_brush_color', color, 'brushColor')
    if 'imagePath' in props:
        asset = unreal.EditorAssetLibrary.load_asset(str(props['imagePath']))
        if asset is None:
            warn('imagePath could not be loaded: %s' % props['imagePath'])
        elif hasattr(widget, 'set_brush_from_texture'):
            safe_call(widget, 'set_brush_from_texture', asset, 'imagePath')
        elif hasattr(widget, 'set_brush_resource_object'):
            safe_call(widget, 'set_brush_resource_object', asset, 'imagePath')

wbp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if wbp is None:
    _emit({'ok': False, 'error': 'could not load widget blueprint'})
else:
    tree = wbp.get_editor_property('widget_tree')
    root = tree.get_editor_property('root_widget') if tree else None
    widget = find_widget(tree, root, _ARGS.get('widgetName') or '') if root else None
    if widget is None:
        _emit({'ok': False, 'error': 'widget not found: %s' % (_ARGS.get('widgetName') or '<root>')})
    else:
        apply_properties(widget, _ARGS.get('properties') or {})
        if hasattr(unreal, 'BlueprintEditorLibrary'):
            unreal.BlueprintEditorLibrary.compile_blueprint(wbp)
        unreal.EditorAssetLibrary.save_asset(wbp.get_path_name())
        _emit({'ok': True, 'assetPath': wbp.get_path_name(),
               'widget': obj_name(widget), 'applied': applied, 'warnings': warnings})
)PY";
            if (arg_str(args, "blueprintPath").empty() || !args.contains("properties"))
                return ToolResult::error("blueprintPath and properties are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_configure_widget_layout ------------------------------------------
    add(Tool{
        "ue_configure_widget_layout",
        "Configure a widget's UMG panel slot. Supports CanvasPanelSlot "
        "position/size/anchors/alignment/offsets/autoSize/zOrder and common "
        "panel-slot padding/horizontalAlignment/verticalAlignment. Python "
        "recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"blueprintPath", {{"type", "string"}}},
               {"widgetName", {{"type", "string"}}},
               {"layout", {{"type", "object"}}}}},
             {"required", json::array({"blueprintPath", "widgetName", "layout"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
applied = []
warnings = []

def warn(msg):
    warnings.append(str(msg))

def vec2(src, default=(0.0, 0.0)):
    src = src or {}
    return unreal.Vector2D(float(src.get('x', default[0])),
                           float(src.get('y', default[1])))

def margin(src):
    src = src or {}
    return unreal.Margin(float(src.get('left', 0.0)),
                         float(src.get('top', 0.0)),
                         float(src.get('right', 0.0)),
                         float(src.get('bottom', 0.0)))

def anchors(src):
    src = src or {}
    mn = src.get('minimum') or src.get('min') or {}
    mx = src.get('maximum') or src.get('max') or {}
    minx = float(mn.get('x', src.get('minX', 0.0)))
    miny = float(mn.get('y', src.get('minY', 0.0)))
    maxx = float(mx.get('x', src.get('maxX', minx)))
    maxy = float(mx.get('y', src.get('maxY', miny)))
    try:
        return unreal.Anchors(minimum=unreal.Vector2D(minx, miny),
                              maximum=unreal.Vector2D(maxx, maxy))
    except Exception:
        return unreal.Anchors(minx, miny, maxx, maxy)

def enum_value(enum_type, name, prefix):
    raw = str(name or '').replace('-', '_').replace(' ', '_')
    candidates = [raw, raw.upper(), prefix + raw.upper(), prefix + '_' + raw.upper()]
    for cand in candidates:
        if hasattr(enum_type, cand):
            return getattr(enum_type, cand)
    raise RuntimeError('unknown enum value %s for %s' % (name, enum_type))

def safe_call(obj, method, value, label):
    if hasattr(obj, method):
        try:
            getattr(obj, method)(value)
            applied.append(label)
            return True
        except Exception as e:
            warn('%s failed: %s' % (label, e))
    return False

def children_of(widget):
    try:
        return [widget.get_child_at(i) for i in range(int(widget.get_children_count()))]
    except Exception:
        try:
            return list(widget.get_all_children() or [])
        except Exception:
            return []

def obj_name(obj):
    try:
        return obj.get_name()
    except Exception:
        return str(obj)

def find_widget(tree, root, name):
    try:
        found = tree.find_widget(name)
        if found is not None:
            return found
    except Exception:
        pass
    stack = [root]
    while stack:
        item = stack.pop(0)
        if item is None:
            continue
        if obj_name(item) == name:
            return item
        stack.extend(children_of(item))
    return None

def slot_for(widget):
    try:
        return widget.get_editor_property('slot')
    except Exception:
        return None

def slot_type(slot):
    try:
        return slot.get_class().get_name()
    except Exception:
        return type(slot).__name__

def apply_layout(slot, layout):
    layout = dict(layout or {})
    if slot is None:
        raise RuntimeError('widget has no slot')
    if 'position' in layout:
        safe_call(slot, 'set_position', vec2(layout['position']), 'slot.position')
    if 'size' in layout:
        safe_call(slot, 'set_size', vec2(layout['size']), 'slot.size')
    if 'anchors' in layout:
        safe_call(slot, 'set_anchors', anchors(layout['anchors']), 'slot.anchors')
    if 'alignment' in layout:
        safe_call(slot, 'set_alignment', vec2(layout['alignment']), 'slot.alignment')
    if 'offsets' in layout:
        safe_call(slot, 'set_offsets', margin(layout['offsets']), 'slot.offsets')
    if 'padding' in layout:
        safe_call(slot, 'set_padding', margin(layout['padding']), 'slot.padding')
    if 'autoSize' in layout:
        safe_call(slot, 'set_auto_size', bool(layout['autoSize']), 'slot.autoSize')
    if 'zOrder' in layout:
        safe_call(slot, 'set_z_order', int(layout['zOrder']), 'slot.zOrder')
    if 'horizontalAlignment' in layout and hasattr(unreal, 'HorizontalAlignment'):
        try:
            safe_call(slot, 'set_horizontal_alignment',
                      enum_value(unreal.HorizontalAlignment, layout['horizontalAlignment'], 'HALIGN'),
                      'slot.horizontalAlignment')
        except Exception as e:
            warn('horizontalAlignment failed: %s' % e)
    if 'verticalAlignment' in layout and hasattr(unreal, 'VerticalAlignment'):
        try:
            safe_call(slot, 'set_vertical_alignment',
                      enum_value(unreal.VerticalAlignment, layout['verticalAlignment'], 'VALIGN'),
                      'slot.verticalAlignment')
        except Exception as e:
            warn('verticalAlignment failed: %s' % e)

wbp = unreal.EditorAssetLibrary.load_asset(_ARGS['blueprintPath'])
if wbp is None:
    _emit({'ok': False, 'error': 'could not load widget blueprint'})
else:
    tree = wbp.get_editor_property('widget_tree')
    root = tree.get_editor_property('root_widget') if tree else None
    widget = find_widget(tree, root, _ARGS['widgetName']) if root else None
    if widget is None:
        _emit({'ok': False, 'error': 'widget not found: %s' % _ARGS['widgetName']})
    else:
        slot = slot_for(widget)
        apply_layout(slot, _ARGS.get('layout') or {})
        if hasattr(unreal, 'BlueprintEditorLibrary'):
            unreal.BlueprintEditorLibrary.compile_blueprint(wbp)
        unreal.EditorAssetLibrary.save_asset(wbp.get_path_name())
        _emit({'ok': True, 'assetPath': wbp.get_path_name(),
               'widget': obj_name(widget), 'slotType': slot_type(slot),
               'applied': applied, 'warnings': warnings})
)PY";
            if (arg_str(args, "blueprintPath").empty() ||
                arg_str(args, "widgetName").empty() || !args.contains("layout"))
                return ToolResult::error("blueprintPath, widgetName and layout are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_create_widget_component_blueprint --------------------------------
    add(Tool{
        "ue_create_widget_component_blueprint",
        "Create an Actor Blueprint with a WidgetComponent assigned to a UMG "
        "Widget Blueprint class for world-space or screen-space UI. Configures "
        "draw size, widget space, pivot, two-sided rendering, and draw-at-desired "
        "size where available. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"name", {{"type", "string"}}},
               {"packagePath", {{"type", "string"}}},
               {"widgetBlueprintPath", {{"type", "string"}}},
               {"componentName", {{"type", "string"}}},
               {"space", {{"type", "string"}, {"description", "World or Screen"}}},
               {"drawSize", {{"type", "object"}}},
               {"pivot", {{"type", "object"}}},
               {"drawAtDesiredSize", {{"type", "boolean"}}},
               {"twoSided", {{"type", "boolean"}}},
               {"reuseExisting", {{"type", "boolean"}}}}},
             {"required", json::array({"name", "packagePath", "widgetBlueprintPath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
name = _ARGS['name']
pkg = _ARGS['packagePath'].rstrip('/')
asset_path = pkg + '/' + name
component_name = _ARGS.get('componentName') or 'Widget'
warnings = []
configured = []
strategies = []

def warn(msg):
    warnings.append(str(msg))

def vec2(src, default):
    src = src or {}
    return unreal.Vector2D(float(src.get('x', default[0])),
                           float(src.get('y', default[1])))

def bool_arg(key, default):
    return bool(_ARGS.get(key, default))

def safe_call(obj, method, value, label):
    if hasattr(obj, method):
        try:
            getattr(obj, method)(value)
            configured.append(label)
            return True
        except Exception as e:
            warn('%s failed: %s' % (label, e))
    return False

def safe_set(obj, prop, value, label):
    try:
        obj.set_editor_property(prop, value)
        configured.append(label)
        return True
    except Exception as e:
        warn('%s failed: %s' % (label, e))
        return False

def generated_class(asset):
    for prop in ('generated_class', 'skeleton_generated_class'):
        try:
            cls = asset.get_editor_property(prop)
            if cls is not None:
                return cls
        except Exception:
            pass
    return None

def compile_bp(bp):
    if hasattr(unreal, 'BlueprintEditorLibrary'):
        unreal.BlueprintEditorLibrary.compile_blueprint(bp)

def is_valid_handle(handle):
    try:
        return unreal.SubobjectDataBlueprintFunctionLibrary.is_handle_valid(handle)
    except Exception:
        return handle is not None

def obj_from_handle(bp, handle):
    bfl = unreal.SubobjectDataBlueprintFunctionLibrary
    data = bfl.get_data(handle)
    try:
        return bfl.get_object_for_blueprint(data, bp)
    except Exception:
        return bfl.get_object(data)

def add_component(bp, parent_handle, comp_class, comp_name):
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    params = unreal.AddNewSubobjectParams(parent_handle=parent_handle,
                                          new_class=comp_class,
                                          blueprint_context=bp,
                                          skip_mark_blueprint_modified=False,
                                          conform_transform_to_parent=False)
    result = subsystem.add_new_subobject(params)
    if isinstance(result, tuple):
        handle = result[0]
        fail_reason = result[1] if len(result) > 1 else ''
    else:
        handle = result
        fail_reason = ''
    if not is_valid_handle(handle):
        raise RuntimeError('add_new_subobject(%s) returned invalid handle: %s' % (comp_name, fail_reason))
    try:
        subsystem.rename_subobject(handle, unreal.Text(comp_name))
    except Exception:
        try:
            subsystem.rename_subobject(handle, comp_name)
        except Exception as e:
            strategies.append('rename %s failed: %s' % (comp_name, e))
    return obj_from_handle(bp, handle)

def enum_widget_space(name):
    raw = str(name or 'World').lower()
    if not hasattr(unreal, 'WidgetSpace'):
        raise RuntimeError('WidgetSpace enum unavailable')
    candidates = ['WORLD' if raw == 'world' else 'SCREEN',
                  'E_WIDGET_SPACE_WORLD' if raw == 'world' else 'E_WIDGET_SPACE_SCREEN',
                  'World' if raw == 'world' else 'Screen']
    for cand in candidates:
        if hasattr(unreal.WidgetSpace, cand):
            return getattr(unreal.WidgetSpace, cand)
    raise RuntimeError('unknown WidgetSpace value: %s' % name)

widget_bp = unreal.EditorAssetLibrary.load_asset(_ARGS['widgetBlueprintPath'])
if widget_bp is None:
    raise RuntimeError('could not load widget blueprint: %s' % _ARGS['widgetBlueprintPath'])
widget_cls = generated_class(widget_bp)
if widget_cls is None:
    raise RuntimeError('widget blueprint has no generated class: %s' % _ARGS['widgetBlueprintPath'])

if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
    if not bool_arg('reuseExisting', False):
        raise RuntimeError('asset already exists; pass reuseExisting=true to configure it: %s' % asset_path)
    bp = unreal.EditorAssetLibrary.load_asset(asset_path)
else:
    factory = unreal.BlueprintFactory()
    factory.set_editor_property('parent_class', unreal.Actor)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    bp = tools.create_asset(name, pkg, unreal.Blueprint, factory)
    if bp is None:
        raise RuntimeError('create_asset returned None: %s' % asset_path)

compile_bp(bp)
if not hasattr(unreal, 'SubobjectDataSubsystem') or not hasattr(unreal, 'AddNewSubobjectParams'):
    _emit({'ok': False, 'unsupported': True,
           'error': 'SubobjectDataSubsystem unavailable; cannot author WidgetComponent on this build',
           'assetPath': bp.get_path_name(), 'componentAdded': False,
           'strategiesTried': strategies})
else:
    subsystem = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    handles = list(subsystem.k2_gather_subobject_data_for_blueprint(bp))
    parent = handles[0] if handles else None
    if parent is None:
        _emit({'ok': False, 'unsupported': True,
               'error': 'could not find a parent subobject handle for Actor Blueprint',
               'assetPath': bp.get_path_name(), 'componentAdded': False,
               'strategiesTried': strategies})
    else:
        widget_comp = None
        try:
            widget_comp = add_component(bp, parent, unreal.WidgetComponent, component_name)
            strategies.append('SubobjectDataSubsystem.add_new_subobject WidgetComponent OK')
        except Exception as e:
            _emit({'ok': False, 'unsupported': True,
                   'error': 'WidgetComponent authoring failed: %s' % e,
                   'assetPath': bp.get_path_name(), 'componentAdded': False,
                   'strategiesTried': strategies})
        if widget_comp is not None:
            safe_call(widget_comp, 'set_widget_class', widget_cls, 'widgetClass')
            safe_set(widget_comp, 'widget_class', widget_cls, 'widgetClass')
            safe_call(widget_comp, 'set_draw_size', vec2(_ARGS.get('drawSize'), (500.0, 300.0)), 'drawSize')
            safe_set(widget_comp, 'draw_size', vec2(_ARGS.get('drawSize'), (500.0, 300.0)), 'drawSize')
            try:
                safe_call(widget_comp, 'set_widget_space', enum_widget_space(_ARGS.get('space') or 'World'), 'space')
                safe_set(widget_comp, 'space', enum_widget_space(_ARGS.get('space') or 'World'), 'space')
            except Exception as e:
                warn('space failed: %s' % e)
            safe_set(widget_comp, 'pivot', vec2(_ARGS.get('pivot'), (0.5, 0.5)), 'pivot')
            safe_call(widget_comp, 'set_draw_at_desired_size', bool_arg('drawAtDesiredSize', False), 'drawAtDesiredSize')
            safe_set(widget_comp, 'draw_at_desired_size', bool_arg('drawAtDesiredSize', False), 'drawAtDesiredSize')
            safe_set(widget_comp, 'two_sided', bool_arg('twoSided', True), 'twoSided')
            compile_bp(bp)
            unreal.EditorAssetLibrary.save_asset(bp.get_path_name())
            _emit({'ok': True, 'assetPath': bp.get_path_name(),
                   'widgetBlueprintPath': _ARGS['widgetBlueprintPath'],
                   'componentName': component_name, 'componentAdded': True,
                   'configured': configured, 'warnings': warnings,
                   'strategiesTried': strategies})
)PY";
            if (arg_str(args, "name").empty() || arg_str(args, "packagePath").empty() ||
                arg_str(args, "widgetBlueprintPath").empty())
                return ToolResult::error("name, packagePath and widgetBlueprintPath are required");
            ToolResult r = run_python_recipe(ctx, kRecipe, args);
            if (r.is_error && r.payload.is_object() &&
                r.payload.value("unsupported", false) == true) {
                json extra = r.payload;
                extra.erase("status");
                extra.erase("error");
                extra.erase("unsupported");
                return ToolResult::unsupported(
                    "WidgetComponent could not be authored via this engine's "
                    "Python API; see strategiesTried.",
                    std::move(extra));
            }
            return r;
        }});
}

// ---------------------------------------------------------------------------
// Pencil -> UMG conversion via the external Pencil2UMG editor plugin
// (https://github.com/wellingfeng/pencil2umg).
//
// The plugin ships as a prebuilt editor module in GitHub Releases and exposes
// UPencil2UMGImporterLibrary::ImportPenFile(PenFilePath, PackagePath) as a
// BlueprintCallable, so once it is installed and the editor has loaded it we
// can drive the conversion from a UE Python recipe.
//
// Lifecycle implemented here (all inside UE's editor Python, which has TLS +
// filesystem access the standalone C++ httplib lacks):
//   1. ue_pencil2umg_status  — report installed vs. latest GitHub release.
//   2. ue_pencil2umg_install — confirm-gated: query the LATEST release on
//      GitHub, and only when called with confirm=true download that release's
//      .zip asset and extract it into <Project>/Plugins/Pencil2UMG, then enable
//      it in the .uproject. A fresh install and every later update each require
//      one explicit confirmation (confirm=true); without it the tool returns a
//      non-error needsConfirmation payload describing what would happen.
//   3. ue_pencil_to_umg      — the actual conversion. If the importer is not
//      loaded it returns an actionable needsInstall / needsRestart payload so
//      the agent can route to the install tool (and then ask the editor to be
//      restarted) before retrying.
// ---------------------------------------------------------------------------
void ToolRegistry::register_pencil2umg_tools() {
    // -- ue_pencil2umg_status -------------------------------------------------
    add(Tool{
        "ue_pencil2umg_status",
        "Report the Pencil2UMG plugin state for Pencil (.pen) -> UMG conversion: "
        "the version installed in this project's Plugins folder (if any), the "
        "latest version published in the GitHub releases of "
        "wellingfeng/pencil2umg, whether an update is available, and whether the "
        "importer class is currently loaded by the editor. Call this when the "
        "user wants to turn a Pencil design into a UMG widget, to decide whether "
        "ue_pencil2umg_install is needed. Optional repo (owner/name) overrides "
        "the default. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"repo", {{"type", "string"},
                         {"description", "owner/name, default wellingfeng/pencil2umg"}}}}}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import urllib.request, json as _json, os
repo = _ARGS.get('repo') or 'wellingfeng/pencil2umg'
plugins_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_plugins_dir())
plugin_dir = os.path.join(plugins_dir, 'Pencil2UMG')
uplugin = os.path.join(plugin_dir, 'Pencil2UMG.uplugin')
installed_version = None
if os.path.isfile(uplugin):
    try:
        with open(uplugin, 'r', encoding='utf-8') as f:
            installed_version = _json.load(f).get('VersionName')
    except Exception:
        installed_version = '(unreadable)'
latest_version = None
asset_name = None
asset_url = None
asset_size = None
net_error = None
try:
    req = urllib.request.Request('https://api.github.com/repos/%s/releases/latest' % repo,
                                 headers={'User-Agent': 'ue-mcp-for-all-versions',
                                          'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=20) as resp:
        rel = _json.loads(resp.read().decode('utf-8'))
    latest_version = (rel.get('tag_name') or '').lstrip('vV') or rel.get('name')
    for a in rel.get('assets', []):
        if (a.get('name') or '').lower().endswith('.zip'):
            asset_name = a.get('name'); asset_url = a.get('browser_download_url'); asset_size = a.get('size')
            break
except Exception as e:
    net_error = str(e)
def _norm(v):
    return (v or '').lstrip('vV')
importer_loaded = hasattr(unreal, 'Pencil2UMGImporterLibrary')
update_available = bool(latest_version and _norm(installed_version) != _norm(latest_version))
_emit({'ok': True,
       'repo': repo,
       'installed': installed_version is not None,
       'installedVersion': installed_version,
       'latestVersion': latest_version,
       'assetName': asset_name,
       'assetUrl': asset_url,
       'assetSizeBytes': asset_size,
       'updateAvailable': update_available,
       'importerLoaded': importer_loaded,
       'restartRequiredToLoad': bool(installed_version is not None and not importer_loaded),
       'pluginDir': plugin_dir,
       'netError': net_error})
)PY";
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_pencil2umg_install ------------------------------------------------
    add(Tool{
        "ue_pencil2umg_install",
        "Download and install (or update to) the LATEST Pencil2UMG release from "
        "GitHub (wellingfeng/pencil2umg) into this project's "
        "Plugins/Pencil2UMG folder, then enable it in the .uproject. This is "
        "CONFIRM-GATED: call with confirm=false (or omit it) first to get a "
        "needsConfirmation payload describing the exact action (fresh install or "
        "the from->to version update); only call with confirm=true after the user "
        "has explicitly agreed. The user must confirm once for the initial "
        "install and again for any later update. 'Latest' always means the "
        "current newest GitHub release at call time, not a version pinned in this "
        "server. Set force=true to reinstall even when already up to date. After "
        "a successful install or update the Unreal Editor must be RESTARTED so it "
        "loads the new editor module before ue_pencil_to_umg can run. Python "
        "recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"confirm", {{"type", "boolean"},
                            {"description", "must be true to actually download/install"}}},
               {"force", {{"type", "boolean"},
                          {"description", "reinstall even if already on the latest version"}}},
               {"repo", {{"type", "string"},
                         {"description", "owner/name, default wellingfeng/pencil2umg"}}}}}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import urllib.request, json as _json, os, zipfile, tempfile, shutil
repo = _ARGS.get('repo') or 'wellingfeng/pencil2umg'
confirm = bool(_ARGS.get('confirm'))
force = bool(_ARGS.get('force'))
plugins_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_plugins_dir())
plugin_dir = os.path.join(plugins_dir, 'Pencil2UMG')
uplugin = os.path.join(plugin_dir, 'Pencil2UMG.uplugin')
def _norm(v):
    return (v or '').lstrip('vV')
installed_version = None
if os.path.isfile(uplugin):
    try:
        with open(uplugin, 'r', encoding='utf-8') as f:
            installed_version = _json.load(f).get('VersionName')
    except Exception:
        installed_version = '(unreadable)'
req = urllib.request.Request('https://api.github.com/repos/%s/releases/latest' % repo,
                             headers={'User-Agent': 'ue-mcp-for-all-versions',
                                      'Accept': 'application/vnd.github+json'})
with urllib.request.urlopen(req, timeout=20) as resp:
    rel = _json.loads(resp.read().decode('utf-8'))
latest_version = (rel.get('tag_name') or '').lstrip('vV') or rel.get('name')
asset_url = None; asset_name = None; asset_size = None
for a in rel.get('assets', []):
    if (a.get('name') or '').lower().endswith('.zip'):
        asset_url = a.get('browser_download_url'); asset_name = a.get('name'); asset_size = a.get('size')
        break
if not asset_url:
    _emit({'ok': False, 'error': 'latest release %s has no .zip asset to install' % latest_version})
else:
    is_update = installed_version is not None
    up_to_date = is_update and _norm(installed_version) == _norm(latest_version)
    action = 'reinstall' if (up_to_date and force) else ('update' if is_update else 'install')
    if up_to_date and not force:
        _emit({'ok': True, 'action': 'none', 'installedVersion': installed_version,
               'latestVersion': latest_version,
               'message': 'Pencil2UMG is already on the latest version; pass force=true to reinstall.'})
    elif not confirm:
        _emit({'ok': True, 'needsConfirmation': True, 'action': action,
               'installedVersion': installed_version, 'latestVersion': latest_version,
               'assetName': asset_name, 'assetSizeBytes': asset_size, 'pluginDir': plugin_dir,
               'message': ('A new Pencil2UMG version (%s) is available; updating from %s. '
                           'Confirm to download and install.' % (latest_version, installed_version))
                          if is_update else
                          ('Pencil2UMG %s will be downloaded from GitHub and installed into the '
                           'project Plugins folder. Confirm to proceed.' % latest_version)})
    else:
        tmpdir = tempfile.mkdtemp(prefix='pencil2umg_')
        zpath = os.path.join(tmpdir, asset_name or 'pencil2umg.zip')
        dreq = urllib.request.Request(asset_url, headers={'User-Agent': 'ue-mcp-for-all-versions'})
        with urllib.request.urlopen(dreq, timeout=120) as resp, open(zpath, 'wb') as out:
            shutil.copyfileobj(resp, out)
        extract_root = os.path.join(tmpdir, 'extract')
        with zipfile.ZipFile(zpath) as zf:
            zf.extractall(extract_root)
        src_plugin = None
        for root, _dirs, files in os.walk(extract_root):
            if 'Pencil2UMG.uplugin' in files:
                src_plugin = root; break
        if src_plugin is None:
            shutil.rmtree(tmpdir, ignore_errors=True)
            _emit({'ok': False, 'error': 'downloaded archive did not contain Pencil2UMG.uplugin'})
        else:
            if os.path.isdir(plugin_dir):
                shutil.rmtree(plugin_dir, ignore_errors=True)
            shutil.copytree(src_plugin, plugin_dir)
            shutil.rmtree(tmpdir, ignore_errors=True)
            uproject_enabled = False
            try:
                proj_file = unreal.Paths.convert_relative_path_to_full(unreal.Paths.get_project_file_path())
                with open(proj_file, 'r', encoding='utf-8') as f:
                    proj = _json.load(f)
                plugins = proj.setdefault('Plugins', [])
                found = False
                for p in plugins:
                    if isinstance(p, dict) and p.get('Name') == 'Pencil2UMG':
                        p['Enabled'] = True; found = True; break
                if not found:
                    plugins.append({'Name': 'Pencil2UMG', 'Enabled': True})
                with open(proj_file, 'w', encoding='utf-8') as f:
                    _json.dump(proj, f, indent='\t')
                uproject_enabled = True
            except Exception:
                uproject_enabled = False
            _emit({'ok': True, 'action': action, 'installedVersion': latest_version,
                   'previousVersion': installed_version, 'pluginDir': plugin_dir,
                   'uprojectEnabled': uproject_enabled, 'restartRequired': True,
                   'message': ('Pencil2UMG %s installed. RESTART the Unreal Editor so it loads '
                               'the plugin before running ue_pencil_to_umg.' % latest_version)})
)PY";
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_pencil_to_umg -----------------------------------------------------
    add(Tool{
        "ue_pencil_to_umg",
        "Convert a Pencil design file (.pen) into a UMG Widget Blueprint using the "
        "installed Pencil2UMG plugin. Pass penFilePath (absolute path to the .pen "
        "file) and optional packagePath (content path for the generated assets, "
        "default \"/Game/UI\"). If the plugin is not installed this returns a "
        "needsInstall payload; if installed but not yet loaded it returns a "
        "needsRestart payload -- in both cases route through ue_pencil2umg_install "
        "/ an editor restart first. Returns the created Widget Blueprint path plus "
        "any importer warnings. Python recipe; requires PythonScriptPlugin and the "
        "Pencil2UMG editor plugin.",
        json{{"type", "object"},
             {"properties",
              {{"penFilePath", {{"type", "string"},
                                {"description", "absolute path to a .pen file"}}},
               {"packagePath", {{"type", "string"},
                                {"description", "content path, default /Game/UI"}}}}},
             {"required", json::array({"penFilePath"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import os
pen = _ARGS.get('penFilePath') or ''
pkg = _ARGS.get('packagePath') or '/Game/UI'
plugins_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_plugins_dir())
installed = os.path.isfile(os.path.join(plugins_dir, 'Pencil2UMG', 'Pencil2UMG.uplugin'))
if not pen:
    _emit({'ok': False, 'error': 'penFilePath is required'})
elif not os.path.isfile(pen):
    _emit({'ok': False, 'error': 'pen file not found: %s' % pen})
elif not hasattr(unreal, 'Pencil2UMGImporterLibrary'):
    if installed:
        _emit({'ok': False, 'needsRestart': True,
               'error': 'Pencil2UMG is installed but its editor module is not loaded; '
                        'restart the Unreal Editor and retry.'})
    else:
        _emit({'ok': False, 'needsInstall': True,
               'error': 'Pencil2UMG plugin is not installed; run ue_pencil2umg_install (with '
                        'confirm=true) and restart the editor, then retry.'})
else:
    res = unreal.Pencil2UMGImporterLibrary.import_pen_file(pen, pkg)
    _emit({'ok': bool(res.success), 'assetPath': res.asset_path,
           'importedAssets': list(res.imported_assets),
           'warnings': list(res.warnings), 'errors': list(res.errors),
           'error': '; '.join(res.errors) if (not res.success and res.errors) else None})
)PY";
            if (arg_str(args, "penFilePath").empty())
                return ToolResult::error("penFilePath is required");
            return run_python_recipe(ctx, kRecipe, args);
        }});
}

// ---------------------------------------------------------------------------
// Figma -> UMG conversion via the external Figma2UMG editor plugin
// (Buvi Games, https://github.com/wellingfeng/figma2umg, MIT).
//
// Unlike Pencil2UMG this plugin has NO GitHub releases and ships as a SOURCE
// editor module, so:
//   * "latest" is tracked by the main-branch HEAD commit SHA (recorded in a
//     .figma2umg_version marker we drop next to the .uplugin), and install
//     pulls the source zipball.
//   * After install/update the project must be (re)compiled — the editor builds
//     the module on next startup — before the importer is usable.
//   * Its import entry (UFigmaImportSubsystem::Request) is NOT a UFUNCTION, so
//     it cannot be triggered from Python. ue_figma_to_umg therefore pre-fills
//     the Figma AccessToken / FileKey / ContentRootFolder project settings and
//     returns a manualStep payload telling the user to launch the import from
//     the Content Browser context menu (the only supported trigger).
//
// The install is confirm-gated exactly like Pencil2UMG: one confirmation for
// the initial install and one for each later update.
// ---------------------------------------------------------------------------
void ToolRegistry::register_figma2umg_tools() {
    // -- ue_figma2umg_status --------------------------------------------------
    add(Tool{
        "ue_figma2umg_status",
        "Report the Figma2UMG plugin state for Figma -> UMG conversion: the "
        "version installed in this project's Plugins folder (its .uplugin "
        "VersionName plus the source commit recorded at install), the latest "
        "main-branch commit on GitHub (wellingfeng/figma2umg, which has no "
        "tagged releases), whether an update is available, and whether the "
        "Figma2UMG editor module is currently loaded/compiled. Call this when the "
        "user wants to import a Figma design into UMG, to decide whether "
        "ue_figma2umg_install is needed. Optional repo (owner/name) and branch "
        "override the defaults. Python recipe; requires PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"repo", {{"type", "string"},
                         {"description", "owner/name, default wellingfeng/figma2umg"}}},
               {"branch", {{"type", "string"},
                           {"description", "default main"}}}}}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import urllib.request, json as _json, os
repo = _ARGS.get('repo') or 'wellingfeng/figma2umg'
branch = _ARGS.get('branch') or 'main'
plugins_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_plugins_dir())
plugin_dir = os.path.join(plugins_dir, 'Figma2UMG')
uplugin = os.path.join(plugin_dir, 'Figma2UMG.uplugin')
marker = os.path.join(plugin_dir, '.figma2umg_version')
installed_version = None
installed_commit = None
if os.path.isfile(uplugin):
    try:
        with open(uplugin, 'r', encoding='utf-8') as f:
            installed_version = _json.load(f).get('VersionName')
    except Exception:
        installed_version = '(unreadable)'
if os.path.isfile(marker):
    try:
        with open(marker, 'r', encoding='utf-8') as f:
            installed_commit = (f.read() or '').strip() or None
    except Exception:
        installed_commit = None
latest_commit = None
latest_date = None
net_error = None
try:
    req = urllib.request.Request('https://api.github.com/repos/%s/commits/%s' % (repo, branch),
                                 headers={'User-Agent': 'ue-mcp-for-all-versions',
                                          'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=20) as resp:
        c = _json.loads(resp.read().decode('utf-8'))
    latest_commit = c.get('sha')
    latest_date = (c.get('commit', {}).get('committer', {}) or {}).get('date')
except Exception as e:
    net_error = str(e)
module_loaded = hasattr(unreal, 'FigmaImportSubsystem')
update_available = bool(latest_commit and installed_commit and latest_commit != installed_commit)
# If installed but we never recorded a commit, treat an update as available so
# the user can refresh to a known state.
if latest_commit and installed_version is not None and not installed_commit:
    update_available = True
_emit({'ok': True, 'repo': repo, 'branch': branch,
       'installed': installed_version is not None,
       'installedVersion': installed_version,
       'installedCommit': installed_commit,
       'latestCommit': latest_commit,
       'latestCommitDate': latest_date,
       'updateAvailable': update_available,
       'moduleLoaded': module_loaded,
       'compileRequired': bool(installed_version is not None and not module_loaded),
       'pluginDir': plugin_dir,
       'netError': net_error})
)PY";
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_figma2umg_install -------------------------------------------------
    add(Tool{
        "ue_figma2umg_install",
        "Download and install (or update to) the latest Figma2UMG SOURCE from "
        "GitHub (wellingfeng/figma2umg main branch) into this project's "
        "Plugins/Figma2UMG folder, enable it in the .uproject, and record the "
        "source commit. The repo has no tagged releases, so 'latest' means the "
        "current main HEAD commit at call time. This is CONFIRM-GATED: call with "
        "confirm=false (or omit it) first to get a needsConfirmation payload "
        "describing the exact action (fresh install or from->to commit update); "
        "only call with confirm=true after the user has explicitly agreed. The "
        "user must confirm once for the initial install and again for any later "
        "update. Set force=true to reinstall even when already on the latest "
        "commit. Because Figma2UMG is a SOURCE plugin, after install/update the "
        "Unreal Editor must RESTART and COMPILE the module before the importer "
        "works (a C++ toolchain is required). Python recipe; requires "
        "PythonScriptPlugin.",
        json{{"type", "object"},
             {"properties",
              {{"confirm", {{"type", "boolean"},
                            {"description", "must be true to actually download/install"}}},
               {"force", {{"type", "boolean"},
                          {"description", "reinstall even if already on the latest commit"}}},
               {"repo", {{"type", "string"},
                         {"description", "owner/name, default wellingfeng/figma2umg"}}},
               {"branch", {{"type", "string"}, {"description", "default main"}}}}}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import urllib.request, json as _json, os, zipfile, tempfile, shutil
repo = _ARGS.get('repo') or 'wellingfeng/figma2umg'
branch = _ARGS.get('branch') or 'main'
confirm = bool(_ARGS.get('confirm'))
force = bool(_ARGS.get('force'))
plugins_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_plugins_dir())
plugin_dir = os.path.join(plugins_dir, 'Figma2UMG')
uplugin = os.path.join(plugin_dir, 'Figma2UMG.uplugin')
marker = os.path.join(plugin_dir, '.figma2umg_version')
installed_version = None
installed_commit = None
if os.path.isfile(uplugin):
    try:
        with open(uplugin, 'r', encoding='utf-8') as f:
            installed_version = _json.load(f).get('VersionName')
    except Exception:
        installed_version = '(unreadable)'
if os.path.isfile(marker):
    try:
        with open(marker, 'r', encoding='utf-8') as f:
            installed_commit = (f.read() or '').strip() or None
    except Exception:
        installed_commit = None
# Resolve latest commit.
creq = urllib.request.Request('https://api.github.com/repos/%s/commits/%s' % (repo, branch),
                              headers={'User-Agent': 'ue-mcp-for-all-versions',
                                       'Accept': 'application/vnd.github+json'})
with urllib.request.urlopen(creq, timeout=20) as resp:
    c = _json.loads(resp.read().decode('utf-8'))
latest_commit = c.get('sha')
if not latest_commit:
    _emit({'ok': False, 'error': 'could not resolve latest commit for %s@%s' % (repo, branch)})
else:
    is_update = installed_version is not None
    up_to_date = is_update and installed_commit == latest_commit
    action = 'reinstall' if (up_to_date and force) else ('update' if is_update else 'install')
    if up_to_date and not force:
        _emit({'ok': True, 'action': 'none', 'installedCommit': installed_commit,
               'latestCommit': latest_commit,
               'message': 'Figma2UMG is already on the latest commit; pass force=true to reinstall.'})
    elif not confirm:
        _emit({'ok': True, 'needsConfirmation': True, 'action': action,
               'installedVersion': installed_version, 'installedCommit': installed_commit,
               'latestCommit': latest_commit, 'pluginDir': plugin_dir,
               'sourceInstall': True, 'compileRequired': True,
               'message': ('A newer Figma2UMG source commit (%s) is available; updating from %s. '
                           'Confirm to download and install (editor must recompile after).'
                           % (latest_commit[:10], (installed_commit or 'unknown')[:10]))
                          if is_update else
                          ('Figma2UMG source (commit %s) will be downloaded from GitHub and '
                           'installed into the project Plugins folder. It is a C++ source plugin, '
                           'so the editor must recompile it on next startup. Confirm to proceed.'
                           % latest_commit[:10])})
    else:
        tmpdir = tempfile.mkdtemp(prefix='figma2umg_')
        zpath = os.path.join(tmpdir, 'src.zip')
        zip_url = 'https://codeload.github.com/%s/zip/%s' % (repo, latest_commit)
        dreq = urllib.request.Request(zip_url, headers={'User-Agent': 'ue-mcp-for-all-versions'})
        with urllib.request.urlopen(dreq, timeout=180) as resp, open(zpath, 'wb') as out:
            shutil.copyfileobj(resp, out)
        extract_root = os.path.join(tmpdir, 'extract')
        with zipfile.ZipFile(zpath) as zf:
            zf.extractall(extract_root)
        src_plugin = None
        for root, _dirs, files in os.walk(extract_root):
            if 'Figma2UMG.uplugin' in files:
                src_plugin = root; break
        if src_plugin is None:
            shutil.rmtree(tmpdir, ignore_errors=True)
            _emit({'ok': False, 'error': 'downloaded archive did not contain Figma2UMG.uplugin'})
        else:
            if os.path.isdir(plugin_dir):
                shutil.rmtree(plugin_dir, ignore_errors=True)
            shutil.copytree(src_plugin, plugin_dir)
            shutil.rmtree(tmpdir, ignore_errors=True)
            try:
                with open(os.path.join(plugin_dir, '.figma2umg_version'), 'w', encoding='utf-8') as f:
                    f.write(latest_commit)
            except Exception:
                pass
            new_version = None
            try:
                with open(os.path.join(plugin_dir, 'Figma2UMG.uplugin'), 'r', encoding='utf-8') as f:
                    new_version = _json.load(f).get('VersionName')
            except Exception:
                new_version = None
            uproject_enabled = False
            try:
                proj_file = unreal.Paths.convert_relative_path_to_full(unreal.Paths.get_project_file_path())
                with open(proj_file, 'r', encoding='utf-8') as f:
                    proj = _json.load(f)
                plugins = proj.setdefault('Plugins', [])
                found = False
                for p in plugins:
                    if isinstance(p, dict) and p.get('Name') == 'Figma2UMG':
                        p['Enabled'] = True; found = True; break
                if not found:
                    plugins.append({'Name': 'Figma2UMG', 'Enabled': True})
                with open(proj_file, 'w', encoding='utf-8') as f:
                    _json.dump(proj, f, indent='\t')
                uproject_enabled = True
            except Exception:
                uproject_enabled = False
            _emit({'ok': True, 'action': action, 'installedVersion': new_version,
                   'installedCommit': latest_commit, 'previousCommit': installed_commit,
                   'pluginDir': plugin_dir, 'uprojectEnabled': uproject_enabled,
                   'restartRequired': True, 'compileRequired': True,
                   'message': ('Figma2UMG source (commit %s) installed. It is a C++ source plugin: '
                               'RESTART the Unreal Editor and let it COMPILE the module (a C++ '
                               'toolchain is required) before running ue_figma_to_umg.'
                               % latest_commit[:10])})
)PY";
            return run_python_recipe(ctx, kRecipe, args);
        }});

    // -- ue_figma_to_umg ------------------------------------------------------
    add(Tool{
        "ue_figma_to_umg",
        "Prepare a Figma -> UMG import using the installed Figma2UMG plugin. Pass "
        "accessToken (a Figma personal access token) and fileKey (the Figma file "
        "or branch key), optional contentRootFolder (default \"/Game/Figma\"), and "
        "optional nodeIds (array of Figma node ids to limit the import). This "
        "writes those values into the Figma2UMG project settings so they are "
        "pre-filled. NOTE: Figma2UMG's importer is a C++-only editor subsystem "
        "with no scriptable (BlueprintCallable) entry point, so the actual import "
        "must be launched by the user from the Content Browser context menu "
        "(\"Import Figma file\") — this tool returns a manualStep payload with "
        "those instructions. Returns needsInstall / needsCompile when the plugin "
        "is not installed or not yet compiled. Python recipe; requires "
        "PythonScriptPlugin and the Figma2UMG editor plugin.",
        json{{"type", "object"},
             {"properties",
              {{"accessToken", {{"type", "string"},
                                {"description", "Figma personal access token"}}},
               {"fileKey", {{"type", "string"},
                            {"description", "Figma file or branch key"}}},
               {"contentRootFolder", {{"type", "string"},
                                       {"description", "content path, default /Game/Figma"}}},
               {"nodeIds", {{"type", "array"}, {"items", {{"type", "string"}}},
                            {"description", "optional Figma node ids to limit import"}}}}},
             {"required", json::array({"accessToken", "fileKey"})}},
        {Capability::PythonScripting},
        [](ToolContext& ctx, const json& args) -> ToolResult {
            static const char* kRecipe = R"PY(
import os
token = _ARGS.get('accessToken') or ''
file_key = _ARGS.get('fileKey') or ''
root = _ARGS.get('contentRootFolder') or '/Game/Figma'
node_ids = _ARGS.get('nodeIds') or []
plugins_dir = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_plugins_dir())
installed = os.path.isfile(os.path.join(plugins_dir, 'Figma2UMG', 'Figma2UMG.uplugin'))
if not token or not file_key:
    _emit({'ok': False, 'error': 'accessToken and fileKey are required'})
elif not installed:
    _emit({'ok': False, 'needsInstall': True,
           'error': 'Figma2UMG plugin is not installed; run ue_figma2umg_install (with '
                    'confirm=true), restart and compile the editor, then retry.'})
elif not hasattr(unreal, 'Figma2UMGSettings'):
    _emit({'ok': False, 'needsCompile': True,
           'error': 'Figma2UMG is installed but its editor module is not loaded; restart the '
                    'Unreal Editor and let it compile the C++ plugin, then retry.'})
else:
    saved = False
    detail = None
    try:
        settings = unreal.get_default_object(unreal.Figma2UMGSettings)
        settings.set_editor_property('AccessToken', token)
        settings.set_editor_property('FileKey', file_key)
        settings.set_editor_property('ContentRootFolder', root)
        try:
            settings.save_config()
        except Exception:
            pass
        saved = True
    except Exception as e:
        detail = str(e)
    _emit({'ok': saved, 'manualStep': True, 'settingsPrefilled': saved,
           'contentRootFolder': root, 'nodeIds': list(node_ids), 'detail': detail,
           'error': None if saved else ('could not pre-fill Figma2UMG settings: %s' % detail),
           'instructions': ['The Figma2UMG importer has no scriptable entry point.',
                            'In the Unreal Editor, right-click a folder in the Content Browser and '
                            'choose "Import Figma file".',
                            'The Access Token, File Key and import path have been pre-filled from '
                            'the project settings; review them and click Import.']})
)PY";
            if (arg_str(args, "accessToken").empty() || arg_str(args, "fileKey").empty())
                return ToolResult::error("accessToken and fileKey are required");
            return run_python_recipe(ctx, kRecipe, args);
        }});
}

}  // namespace ue_mcp_for_all_versions

