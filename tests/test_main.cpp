// ue-mcp-for-all-versions — unit tests (no external test framework; a tiny
// assert harness so the binary is dependency-free and runs under ctest).
#include <cstdio>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "ue_mcp_for_all_versions/capability_registry.hpp"
#include "ue_mcp_for_all_versions/mcp_server.hpp"
#include "ue_mcp_for_all_versions/project_setup.hpp"
#include "ue_mcp_for_all_versions/rc_client.hpp"
#include "ue_mcp_for_all_versions/tool_registry.hpp"

namespace uemcp = ue_mcp_for_all_versions;
namespace fs = std::filesystem;
using uemcp::json;

static int g_failures = 0;
static int g_checks = 0;
static int g_temp_counter = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_failures;                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
        }                                                                  \
    } while (0)

static fs::path make_temp_dir(const std::string& name) {
    fs::path root = fs::temp_directory_path() /
                    ("uemcp_" + name + "_" + std::to_string(std::time(nullptr)) +
                     "_" + std::to_string(++g_temp_counter));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    CHECK(!ec);
    return root;
}

static void write_file(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    CHECK(!ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    CHECK(static_cast<bool>(out));
}

static std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool plugin_enabled(const json& project, const std::string& name) {
    if (!project.contains("Plugins") || !project["Plugins"].is_array()) return false;
    for (const auto& plugin : project["Plugins"]) {
        if (plugin.is_object() && plugin.value("Name", std::string()) == name &&
            plugin.value("Enabled", false)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Version parsing
// ---------------------------------------------------------------------------
static void test_version_parsing() {
    auto v55 = uemcp::CapabilityRegistry::parse_version("5.5.4-12345+++UE5+Release-5.5");
    CHECK(v55.major == 5);
    CHECK(v55.minor == 5);
    CHECK(v55.patch == 4);
    CHECK(v55.at_least(5, 0));
    CHECK(v55.at_least(4, 26));
    CHECK(!v55.at_least(5, 6));

    auto v425 = uemcp::CapabilityRegistry::parse_version("4.25.4-13144385+++UE4+Release-4.25");
    CHECK(v425.major == 4);
    CHECK(v425.minor == 25);
    CHECK(!v425.at_least(4, 26));
    CHECK(!v425.at_least(5, 0));

    auto v426 = uemcp::CapabilityRegistry::parse_version("4.26.2-...");
    CHECK(v426.at_least(4, 26));
    CHECK(!v426.at_least(5, 0));

    auto bad = uemcp::CapabilityRegistry::parse_version("not-a-version");
    CHECK(!bad.known());
}

// ---------------------------------------------------------------------------
// MCP dispatch: initialize / tools/list / unknown method / notification
// ---------------------------------------------------------------------------
static void test_mcp_dispatch() {
    uemcp::RcClient rc;  // never connected in this test
    uemcp::CapabilityRegistry caps;
    uemcp::ToolRegistry tools;
    tools.register_builtins();
    uemcp::McpServer server(rc, caps, tools);

    // initialize
    json init = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", json::object()}};
    auto r1 = server.handle_message(init);
    CHECK(r1.has_value());
    CHECK((*r1)["result"]["protocolVersion"] == uemcp::kProtocolVersion);
    CHECK((*r1)["result"]["serverInfo"]["name"] == uemcp::kServerName);

    // tools/list returns all builtins
    json list = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    auto r2 = server.handle_message(list);
    CHECK(r2.has_value());
    CHECK((*r2)["result"]["tools"].is_array());
    CHECK((*r2)["result"]["tools"].size() >= 30);

    // Spot-check that a few of the new tools are advertised by name.
    {
        const auto& arr = (*r2)["result"]["tools"];
        auto has_tool = [&](const char* name) {
            for (const auto& t : arr)
                if (t.contains("name") && t["name"] == name) return true;
            return false;
        };
        CHECK(has_tool("ue_python_exec"));
        CHECK(has_tool("ue_set_actor_location"));
        CHECK(has_tool("ue_save_asset"));
        CHECK(has_tool("ue_get_viewport_camera"));
        CHECK(has_tool("ue_list_presets"));
        CHECK(has_tool("ue_property_array_append"));
        // Newer tools: scene introspection, material, visual, workflow.
        CHECK(has_tool("ue_find_actors_by_class"));
        CHECK(has_tool("ue_find_actors_by_label"));
        CHECK(has_tool("ue_get_actor_components"));
        CHECK(has_tool("ue_get_actor_bounds"));
        CHECK(has_tool("ue_set_component_relative_transform"));
        CHECK(has_tool("ue_get_component_transform"));
        CHECK(has_tool("ue_get_component_bounds"));
        CHECK(has_tool("ue_fit_mesh_component_to_height"));
        CHECK(has_tool("ue_get_blueprint_components"));
        CHECK(has_tool("ue_set_blueprint_component_template_transform"));
        CHECK(has_tool("ue_normalize_imported_character_mesh"));
        CHECK(has_tool("ue_import_asset_with_transform_policy"));
        CHECK(has_tool("ue_simulate_player_input"));
        CHECK(has_tool("ue_set_material_param"));
        CHECK(has_tool("ue_get_object_thumbnail"));
        CHECK(has_tool("ue_spawn_actor_from_asset"));
        CHECK(has_tool("ue_focus_viewport_on_actor"));
        CHECK(has_tool("ue_get_console_variable"));
        // Layer 1: scene / mesh / light / creation / data-debug tools.
        CHECK(has_tool("ue_batch_spawn_actors"));
        CHECK(has_tool("ue_duplicate_actor"));
        CHECK(has_tool("ue_attach_actor"));
        CHECK(has_tool("ue_set_actor_folder"));
        CHECK(has_tool("ue_set_static_mesh"));
        CHECK(has_tool("ue_set_actor_material"));
        CHECK(has_tool("ue_set_light_property"));
        CHECK(has_tool("ue_create_material"));
        CHECK(has_tool("ue_create_material_instance"));
        CHECK(has_tool("ue_import_asset"));
        CHECK(has_tool("ue_create_folder"));
        CHECK(has_tool("ue_data_table_get_rows"));
        CHECK(has_tool("ue_set_cvar"));
        CHECK(has_tool("ue_start_pie"));
        // Layer 2: blueprint + UMG authoring tools.
        CHECK(has_tool("ue_create_blueprint"));
        CHECK(has_tool("ue_add_blueprint_variable"));
        CHECK(has_tool("ue_compile_blueprint"));
        CHECK(has_tool("ue_create_character_blueprint"));
        CHECK(has_tool("ue_configure_character_movement"));
        CHECK(has_tool("ue_calibrate_character_collision"));
        CHECK(has_tool("ue_configure_third_person_camera"));
        CHECK(has_tool("ue_create_enhanced_input_assets"));
        CHECK(has_tool("ue_create_locomotion_animation_assets"));
        CHECK(has_tool("ue_set_game_defaults"));
        CHECK(has_tool("ue_validate_third_person_pie"));
        CHECK(has_tool("ue_create_widget_blueprint"));
        CHECK(has_tool("ue_add_widget_to_blueprint"));
        CHECK(has_tool("ue_inspect_widget_blueprint"));
        CHECK(has_tool("ue_add_widget_to_panel"));
        CHECK(has_tool("ue_set_widget_properties"));
        CHECK(has_tool("ue_configure_widget_layout"));
        CHECK(has_tool("ue_create_widget_component_blueprint"));
    }

    // unknown method -> -32601
    json bad = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "does/not/exist"}};
    auto r3 = server.handle_message(bad);
    CHECK(r3.has_value());
    CHECK((*r3)["error"]["code"] == -32601);

    // notification (no id) -> no response
    json note = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    auto r4 = server.handle_message(note);
    CHECK(!r4.has_value());

    // ping
    json ping = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}};
    auto r5 = server.handle_message(ping);
    CHECK(r5.has_value());
    CHECK((*r5).contains("result"));
}

// ---------------------------------------------------------------------------
// Tool degradation: two distinct behaviors.
//  (a) Engine never probed (not running) + capability-requiring tool -> a
//      connection ERROR (we don't know what's supported yet).
//  (b) Engine probed as an older version lacking a capability -> structured
//      "unsupported" (true runtime auto-degrade), NOT an error.
// ---------------------------------------------------------------------------
static void test_tool_degradation() {
    // Pin to a port no RC server listens on, so "connection failed -> error"
    // checks are deterministic even when a real editor is running on the default
    // RC ports during local dev/CI.
    uemcp::RcConfig closed;
    closed.ports = {59998};
    closed.connect_timeout = std::chrono::milliseconds(200);
    uemcp::RcClient rc(closed);
    uemcp::ToolRegistry tools;
    tools.register_builtins();

    // (a) Unprobed registry: search_assets -> connection error.
    {
        uemcp::CapabilityRegistry caps_unprobed;
        uemcp::ToolContext ctx{rc, caps_unprobed};
        auto search = tools.invoke(ctx, "ue_search_assets", json{{"query", "x"}});
        CHECK(search.is_error);  // unknown support -> error, not "unsupported"
        CHECK(search.payload["status"] == "error");

        // get_engine_version requires no capability -> still runs (ok).
        auto ver = tools.invoke(ctx, "ue_get_engine_version", json::object());
        CHECK(!ver.is_error);
        CHECK(ver.payload["status"] == "ok");

        // unknown tool -> error
        auto unk = tools.invoke(ctx, "nope", json::object());
        CHECK(unk.is_error);
    }

    // (b) Probed as UE 4.25 (only call+property): search_assets -> unsupported.
    {
        uemcp::CapabilityRegistry caps_425;
        caps_425.set_probed_for_test(
            uemcp::CapabilityRegistry::parse_version("4.25.4-x+++UE4+Release-4.25"),
            {uemcp::Capability::ObjectCall, uemcp::Capability::ObjectProperty});
        uemcp::ToolContext ctx{rc, caps_425};

        auto search = tools.invoke(ctx, "ue_search_assets", json{{"query", "x"}});
        CHECK(!search.is_error);  // unsupported is NOT an error
        CHECK(search.payload["status"] == "unsupported");
        CHECK(search.payload["missingCapability"] == "search.assets");

        auto info = tools.invoke(ctx, "ue_remote_info", json::object());
        CHECK(!info.is_error);
        CHECK(info.payload["status"] == "unsupported");

        // Python is NOT inferred from version — a 4.25 registry without the
        // PythonScripting capability degrades ue_python_exec to unsupported.
        auto py = tools.invoke(ctx, "ue_python_exec", json{{"code", "pass"}});
        CHECK(!py.is_error);
        CHECK(py.payload["status"] == "unsupported");
        CHECK(py.payload["missingCapability"] == "python");

        // PIE control is UE5-only -> unsupported on 4.25.
        auto pie = tools.invoke(ctx, "ue_is_pie", json::object());
        CHECK(!pie.is_error);
        CHECK(pie.payload["status"] == "unsupported");

        // Property array ops are 5.x-only -> unsupported on 4.25.
        auto arr = tools.invoke(ctx, "ue_property_array_append",
                                json{{"objectPath", "/X"}, {"propertyName", "P"}, {"value", 1}});
        CHECK(!arr.is_error);
        CHECK(arr.payload["status"] == "unsupported");

        // Thumbnails need the editor route (4.26+) -> unsupported on 4.25.
        auto thumb = tools.invoke(ctx, "ue_get_object_thumbnail",
                                  json{{"objectPath", "/Game/X"}});
        CHECK(!thumb.is_error);
        CHECK(thumb.payload["status"] == "unsupported");
        CHECK(thumb.payload["missingCapability"] == "object.thumbnail");

        // Layer-2 creation recipes are Python-gated: on a 4.25 registry without
        // PythonScripting they degrade to 'unsupported', not error.
        for (const char* t : {"ue_create_material", "ue_create_blueprint",
                              "ue_create_character_blueprint",
                              "ue_configure_character_movement",
                              "ue_calibrate_character_collision",
                              "ue_configure_third_person_camera",
                              "ue_create_enhanced_input_assets",
                              "ue_create_locomotion_animation_assets",
                              "ue_set_game_defaults",
                              "ue_validate_third_person_pie",
                              "ue_create_widget_blueprint",
                              "ue_inspect_widget_blueprint",
                              "ue_add_widget_to_panel",
                              "ue_set_widget_properties",
                              "ue_configure_widget_layout",
                              "ue_create_widget_component_blueprint",
                              "ue_import_asset",
                              "ue_get_component_transform",
                              "ue_get_component_bounds",
                              "ue_fit_mesh_component_to_height",
                              "ue_get_blueprint_components",
                              "ue_set_blueprint_component_template_transform",
                              "ue_normalize_imported_character_mesh",
                              "ue_import_asset_with_transform_policy",
                              "ue_simulate_player_input"}) {
            auto r = tools.invoke(ctx, t, json{{"name", "X"}, {"packagePath", "/Game"}});
            CHECK(!r.is_error);
            CHECK(r.payload["status"] == "unsupported");
            CHECK(r.payload["missingCapability"] == "python");
        }

        // ue_start_pie is PieControl-gated -> unsupported on 4.25.
        auto startpie = tools.invoke(ctx, "ue_start_pie", json::object());
        CHECK(!startpie.is_error);
        CHECK(startpie.payload["status"] == "unsupported");

        // Batch spawn needs the batch route (4.26+) -> unsupported on 4.25.
        auto bspawn = tools.invoke(ctx, "ue_batch_spawn_actors",
                                   json{{"actors", json::array()}});
        CHECK(!bspawn.is_error);
        CHECK(bspawn.payload["status"] == "unsupported");

        // Scene introspection only needs object.call -> reaches handler, which
        // then reports the connection is down (not "unsupported").
        auto find = tools.invoke(ctx, "ue_find_actors_by_class",
                                 json{{"actorClass", "/Script/Engine.StaticMeshActor"}});
        CHECK(find.payload["status"] == "error");  // gate passed; RC not connected

        // object.call IS available on 4.25 -> call_function passes the gate and
        // reaches the handler (which then reports the connection is down).
        auto call = tools.invoke(ctx, "ue_call_function",
                                 json{{"objectPath", "/X"}, {"functionName", "F"}});
        CHECK(call.payload["status"] == "error");  // handler ran, RC not connected
    }

    // (c) Probed as UE 5.5 with Python available: python + PIE tools pass the
    //     gate and reach their handlers (which then fail on no connection).
    {
        uemcp::CapabilityRegistry caps_55;
        caps_55.set_probed_for_test(
            uemcp::CapabilityRegistry::parse_version("5.5.1-x+++UE5+Release-5.5"),
            {uemcp::Capability::ObjectCall, uemcp::Capability::ObjectProperty,
             uemcp::Capability::PythonScripting, uemcp::Capability::PieControl,
             uemcp::Capability::PropertyArrayOps});
        uemcp::ToolContext ctx{rc, caps_55};

        auto py = tools.invoke(ctx, "ue_python_exec", json{{"code", "pass"}});
        CHECK(py.payload["status"] == "error");  // gate passed; RC not connected
        auto pie = tools.invoke(ctx, "ue_is_pie", json::object());
        CHECK(pie.payload["status"] == "error");

        // A Python-gated creation recipe passes the capability gate on 5.5+Python
        // and reaches the handler, which then fails on no connection (not
        // 'unsupported').
        auto mat = tools.invoke(ctx, "ue_create_material",
                                json{{"name", "M"}, {"packagePath", "/Game"}});
        CHECK(mat.payload["status"] == "error");  // gate passed; RC not connected
    }
}

// ---------------------------------------------------------------------------
// Edge cases in version parsing + capability inference by version.
// ---------------------------------------------------------------------------
static void test_rc_request_shapes() {
    auto v = uemcp::CapabilityRegistry::parse_version("5");
    CHECK(v.major == 5);
    CHECK(v.minor == 0);

    // A probed 4.26 registry must expose search.assets but NOT the 5.x-only
    // array ops (matches the real per-version behavior verified on hardware).
    uemcp::CapabilityRegistry caps_426;
    caps_426.set_probed_for_test(
        uemcp::CapabilityRegistry::parse_version("4.26.2"),
        {uemcp::Capability::ObjectCall, uemcp::Capability::ObjectProperty,
         uemcp::Capability::Info, uemcp::Capability::SearchAssets});
    CHECK(caps_426.has(uemcp::Capability::SearchAssets));
    CHECK(!caps_426.has(uemcp::Capability::PropertyArrayOps));
}

// ---------------------------------------------------------------------------
// Lazy-connect / reconnect bookkeeping (no live server):
//  - a never-connected client reports generation 0 and not connected
//  - ensure_probed is a no-op while disconnected (keeps last-known caps)
//  - the "editor restarted" path is modeled by a generation bump: the registry
//    re-probes only when generation advances AND the client is connected.
// ---------------------------------------------------------------------------
static void test_lazy_connect_state() {
    // Use a port that is essentially never an RC server, so this test is
    // isolated from any editor that may be running during CI/dev.
    uemcp::RcConfig closed;
    closed.ports = {59999};
    closed.connect_timeout = std::chrono::milliseconds(300);
    uemcp::RcClient rc(closed);
    CHECK(!rc.connected());
    CHECK(rc.generation() == 0);

    // ensure_connected() must not throw and must report failure quickly.
    bool ok = rc.ensure_connected();
    CHECK(!ok);
    CHECK(!rc.connected());

    // A registry that was probed (test seam) keeps its capabilities even though
    // the client is disconnected; ensure_probed leaves them intact (no live
    // connection to re-probe from), so previously-known tools still gate
    // correctly rather than flipping to "unknown".
    uemcp::CapabilityRegistry caps;
    caps.set_probed_for_test(
        uemcp::CapabilityRegistry::parse_version("5.5.1"),
        {uemcp::Capability::ObjectCall, uemcp::Capability::SearchAssets});
    caps.ensure_probed(rc);  // disconnected -> no-op
    CHECK(caps.probed());
    CHECK(caps.has(uemcp::Capability::SearchAssets));
}

// ---------------------------------------------------------------------------
// One-click project setup:
//  - UE 5.x writes DefaultRemoteControl.ini and enables full plugin coverage.
//  - UE 4.26 writes DefaultWebRemoteControl.ini.
//  - UE 4.25 writes the startup CVar fallback used by that older plugin.
// ---------------------------------------------------------------------------
static void test_project_setup_ue5() {
    fs::path dir = make_temp_dir("setup_ue5");
    write_file(dir / "Game.uproject",
               "{\n"
               "  \"FileVersion\": 3,\n"
               "  \"EngineAssociation\": \"5.3\",\n"
               "  \"Plugins\": [\n"
               "    { \"Name\": \"RemoteControl\", \"Enabled\": false }\n"
               "  ]\n"
               "}\n");

    uemcp::ProjectSetupOptions options;
    options.input_path = dir;
    options.server_command = "C:/Tools/ue-mcp-for-all-versions.exe";
    auto result = uemcp::setup_unreal_project(options);
    CHECK(result.ok);
    CHECK(result.changed);

    json project = json::parse(read_file(dir / "Game.uproject"));
    CHECK(plugin_enabled(project, "RemoteControl"));
    CHECK(plugin_enabled(project, "EditorScriptingUtilities"));
    CHECK(plugin_enabled(project, "PythonScriptPlugin"));

    std::string rc_ini = read_file(dir / "Config" / "DefaultRemoteControl.ini");
    CHECK(rc_ini.find("[/Script/RemoteControlCommon.RemoteControlSettings]") !=
          std::string::npos);
    CHECK(rc_ini.find("bAutoStartWebServer=True") != std::string::npos);
    CHECK(rc_ini.find("RemoteControlHttpServerPort=30010") != std::string::npos);
    CHECK(rc_ini.find("bEnableRemotePythonExecution=True") != std::string::npos);
    // Full-access profile: protected/getter-setter gates are also lifted so the
    // MCP server can edit protected properties without interactive prompts.
    CHECK(rc_ini.find("bIgnoreProtectedCheck=True") != std::string::npos);
    CHECK(rc_ini.find("bIgnoreGetterSetterCheck=True") != std::string::npos);

    // A restart warning must be surfaced whenever files actually changed.
    bool saw_restart_warning = false;
    for (const auto& w : result.warnings) {
        if (w.find("RESTART REQUIRED") != std::string::npos) saw_restart_warning = true;
    }
    CHECK(saw_restart_warning);

    json mcp = json::parse(read_file(dir / ".mcp.json"));
    CHECK(mcp["mcpServers"]["ue-mcp-for-all-versions"]["command"] ==
          "C:/Tools/ue-mcp-for-all-versions.exe");

    auto second = uemcp::setup_unreal_project(options);
    CHECK(second.ok);
    CHECK(!second.changed);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void test_project_setup_ue426() {
    fs::path dir = make_temp_dir("setup_ue426");
    write_file(dir / "Game.uproject",
               "{\n"
               "  \"FileVersion\": 3,\n"
               "  \"EngineAssociation\": \"4.26\"\n"
               "}\n");

    uemcp::ProjectSetupOptions options;
    options.input_path = dir / "Game.uproject";
    options.server_command = "C:/Tools/ue-mcp-for-all-versions.exe";
    auto result = uemcp::setup_unreal_project(options);
    CHECK(result.ok);

    json project = json::parse(read_file(dir / "Game.uproject"));
    CHECK(plugin_enabled(project, "RemoteControl"));
    CHECK(plugin_enabled(project, "EditorScriptingUtilities"));
    CHECK(plugin_enabled(project, "PythonScriptPlugin"));

    std::string web_ini = read_file(dir / "Config" / "DefaultWebRemoteControl.ini");
    CHECK(web_ini.find("[/Script/WebRemoteControl.WebRemoteControlSettings]") !=
          std::string::npos);
    CHECK(web_ini.find("RemoteControlHttpServerPort=30010") != std::string::npos);
    CHECK(!fs::exists(dir / "Config" / "DefaultRemoteControl.ini"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

static void test_project_setup_ue425() {
    fs::path dir = make_temp_dir("setup_ue425");
    write_file(dir / "Game.uproject",
               "{\n"
               "  \"FileVersion\": 3,\n"
               "  \"EngineAssociation\": \"4.25\",\n"
               "  \"Plugins\": []\n"
               "}\n");

    uemcp::ProjectSetupOptions options;
    options.input_path = dir;
    options.server_command = "C:/Tools/ue-mcp-for-all-versions.exe";
    auto result = uemcp::setup_unreal_project(options);
    CHECK(result.ok);

    std::string engine_ini = read_file(dir / "Config" / "DefaultEngine.ini");
    CHECK(engine_ini.find("[SystemSettings]") != std::string::npos);
    CHECK(engine_ini.find("WebControl.EnableServerOnStartup=1") != std::string::npos);
    CHECK(!fs::exists(dir / "Config" / "DefaultWebRemoteControl.ini"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

int main() {
    test_version_parsing();
    test_mcp_dispatch();
    test_tool_degradation();
    test_rc_request_shapes();
    test_lazy_connect_state();
    test_project_setup_ue5();
    test_project_setup_ue426();
    test_project_setup_ue425();

    std::fprintf(stderr, "\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::fprintf(stderr, "ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
