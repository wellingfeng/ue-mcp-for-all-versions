// ue-mcp-for-all-versions — entry point.
//
// A standalone MCP server that drives any Unreal Engine version (4.25 -> 5.8)
// through the engine's built-in RemoteControl HTTP API. Speaks MCP over stdio.
//
// Usage:
//   ue-mcp-for-all-versions [--host H] [--port P] [--probe]
//   ue-mcp-for-all-versions --setup-project PATH [--dry-run]
//
//   --host H    RemoteControl host (default 127.0.0.1)
//   --port P    force a single RC port (default: probe 30010 then 8080)
//   --probe     connect, print detected engine/capabilities as JSON, exit
//   --setup-project PATH
//               configure a .uproject or project directory for one-click MCP use
//
// With no flags it runs the MCP stdio loop. It connects to RemoteControl
// lazily and tolerates the engine not being up yet (tools will report errors
// until a connection is established on first use).
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "ue_mcp_for_all_versions/capability_registry.hpp"
#include "ue_mcp_for_all_versions/mcp_server.hpp"
#include "ue_mcp_for_all_versions/project_setup.hpp"
#include "ue_mcp_for_all_versions/rc_client.hpp"
#include "ue_mcp_for_all_versions/tool_registry.hpp"

namespace uemcp = ue_mcp_for_all_versions;
namespace fs = std::filesystem;

namespace {

std::string absolute_command_path(const char* argv0) {
    if (!argv0 || std::strlen(argv0) == 0) return "ue-mcp-for-all-versions";
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(argv0), ec);
    if (ec) return argv0;
    if (!fs::exists(p, ec)) return argv0;
    return p.lexically_normal().generic_string();
}

void print_help() {
    std::cerr << "ue-mcp-for-all-versions " << uemcp::kServerVersion << "\n"
              << "Drives any UE version via RemoteControl; speaks MCP over stdio.\n\n"
              << "Runtime flags:\n"
              << "  --host H                 RemoteControl host (default 127.0.0.1)\n"
              << "  --port P                 Force a single RC port\n"
              << "  --probe                  Print detected engine/capabilities JSON and exit\n\n"
              << "One-click project setup:\n"
              << "  --setup-project PATH     Configure a .uproject or project directory\n"
              << "  --server-command PATH    Command written to .mcp.json\n"
              << "  --no-python              Do not enable PythonScriptPlugin / remote Python\n"
              << "  --no-mcp-config          Do not write project .mcp.json\n"
              << "  --dry-run                Report changes without writing files\n";
}

}  // namespace

int main(int argc, char** argv) {
    uemcp::RcConfig cfg;
    bool probe_only = false;
    uemcp::ProjectSetupOptions setup_options;
    setup_options.server_command = absolute_command_path(argc > 0 ? argv[0] : nullptr);
    bool setup_project = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            cfg.ports = {std::atoi(argv[++i])};
        } else if (a == "--probe") {
            probe_only = true;
        } else if (a == "--setup-project" && i + 1 < argc) {
            setup_project = true;
            setup_options.input_path = argv[++i];
        } else if (a.rfind("--setup-project=", 0) == 0) {
            setup_project = true;
            setup_options.input_path = a.substr(std::strlen("--setup-project="));
        } else if (a == "--server-command" && i + 1 < argc) {
            setup_options.server_command = argv[++i];
        } else if (a.rfind("--server-command=", 0) == 0) {
            setup_options.server_command = a.substr(std::strlen("--server-command="));
        } else if (a == "--no-python") {
            setup_options.enable_python = false;
        } else if (a == "--no-mcp-config") {
            setup_options.write_mcp_config = false;
        } else if (a == "--dry-run") {
            setup_options.dry_run = true;
        } else if (a == "--help" || a == "-h") {
            print_help();
            return 0;
        }
    }

    if (setup_project) {
        uemcp::ProjectSetupResult result = uemcp::setup_unreal_project(setup_options);
        std::cout << result.to_json().dump(2) << "\n";
        return result.ok ? 0 : 1;
    }

    uemcp::RcClient rc(cfg);
    uemcp::CapabilityRegistry caps;
    uemcp::ToolRegistry tools;
    tools.register_builtins();

    if (rc.connect()) {
        caps.probe(rc);
        std::cerr << "[ue-mcp] connected to RemoteControl on port " << rc.active_port()
                  << "; engine=" << (caps.engine_version().known()
                                         ? caps.engine_version().raw
                                         : "unknown")
                  << "\n";
    } else {
        std::cerr << "[ue-mcp] no RemoteControl server reachable yet (tried ";
        for (size_t i = 0; i < cfg.ports.size(); ++i)
            std::cerr << (i ? "," : "") << cfg.ports[i];
        std::cerr << "). Start UE and run 'WebControl.StartServer' in the console.\n";
    }

    if (probe_only) {
        std::cout << caps.describe().dump(2) << "\n";
        return rc.connected() ? 0 : 1;
    }

    uemcp::McpServer server(rc, caps, tools);
    return server.run(std::cin, std::cout);
}
