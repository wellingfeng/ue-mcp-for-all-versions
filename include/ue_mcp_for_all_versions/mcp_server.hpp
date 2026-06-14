// ue-mcp-for-all-versions
// MCP server: speaks JSON-RPC 2.0 over stdio per the Model Context Protocol
// stdio transport (newline-delimited JSON messages). Handles initialize,
// tools/list, tools/call.
#pragma once

#include <iosfwd>
#include <string>

#include "ue_mcp_for_all_versions/capability_registry.hpp"
#include "ue_mcp_for_all_versions/rc_client.hpp"
#include "ue_mcp_for_all_versions/tool_registry.hpp"

namespace ue_mcp_for_all_versions {

constexpr const char* kServerName = "ue-mcp-for-all-versions";
constexpr const char* kServerVersion = "0.5.0";
constexpr const char* kProtocolVersion = "2024-11-05";

class McpServer {
public:
    McpServer(RcClient& rc, CapabilityRegistry& caps, ToolRegistry& tools);

    // Run the stdio read/dispatch loop until EOF on `in`. Returns process exit
    // code. Reads newline-delimited JSON-RPC messages from `in`, writes
    // responses to `out`.
    int run(std::istream& in, std::ostream& out);

    // Dispatch a single parsed request and produce a response. Exposed for
    // unit testing without a real stdio loop. Returns std::nullopt for
    // notifications (no id) that need no response.
    std::optional<json> handle_message(const json& msg);

private:
    json handle_initialize(const json& params);
    json handle_tools_list(const json& params);
    json handle_tools_call(const json& params);

    static json make_result(const json& id, json result);
    static json make_error(const json& id, int code, const std::string& message,
                           json data = nullptr);

    RcClient& rc_;
    CapabilityRegistry& caps_;
    ToolRegistry& tools_;
    bool initialized_ = false;
};

}  // namespace ue_mcp_for_all_versions
