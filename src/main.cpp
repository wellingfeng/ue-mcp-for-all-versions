// ue-mcp-for-all-versions — entry point.
//
// A standalone MCP server that drives any Unreal Engine version (4.25 -> 5.8)
// through the engine's built-in RemoteControl HTTP API. Speaks MCP over stdio.
//
// Usage:
//   ue-mcp-for-all-versions [--host H] [--port P] [--probe]
//
//   --host H    RemoteControl host (default 127.0.0.1)
//   --port P    force a single RC port (default: probe 30010 then 8080)
//   --probe     connect, print detected engine/capabilities as JSON, exit
//
// With no flags it runs the MCP stdio loop. It connects to RemoteControl
// lazily and tolerates the engine not being up yet (tools will report errors
// until a connection is established on first use).
#include <cstring>
#include <iostream>
#include <string>

#include "ue_mcp_for_all_versions/capability_registry.hpp"
#include "ue_mcp_for_all_versions/mcp_server.hpp"
#include "ue_mcp_for_all_versions/rc_client.hpp"
#include "ue_mcp_for_all_versions/tool_registry.hpp"

namespace uemcp = ue_mcp_for_all_versions;

int main(int argc, char** argv) {
    uemcp::RcConfig cfg;
    bool probe_only = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (a == "--port" && i + 1 < argc) {
            cfg.ports = {std::atoi(argv[++i])};
        } else if (a == "--probe") {
            probe_only = true;
        } else if (a == "--help" || a == "-h") {
            std::cerr << "ue-mcp-for-all-versions " << uemcp::kServerVersion << "\n"
                      << "Drives any UE version via RemoteControl; speaks MCP over stdio.\n"
                      << "Flags: --host H  --port P  --probe\n";
            return 0;
        }
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
