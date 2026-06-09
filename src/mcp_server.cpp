// ue-mcp-for-all-versions — MCP stdio JSON-RPC server implementation.
#include "ue_mcp_for_all_versions/mcp_server.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace ue_mcp_for_all_versions {

McpServer::McpServer(RcClient& rc, CapabilityRegistry& caps, ToolRegistry& tools)
    : rc_(rc), caps_(caps), tools_(tools) {}

json McpServer::make_result(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json McpServer::make_error(const json& id, int code, const std::string& message,
                           json data) {
    json err = {{"code", code}, {"message", message}};
    if (!data.is_null()) err["data"] = std::move(data);
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(err)}};
}

json McpServer::handle_initialize(const json&) {
    initialized_ = true;
    return {
        {"protocolVersion", kProtocolVersion},
        {"serverInfo", {{"name", kServerName}, {"version", kServerVersion}}},
        {"capabilities", {{"tools", json::object()}}},
        // Non-standard but useful: advertise the connected engine state.
        {"_engine", caps_.describe()},
    };
}

json McpServer::handle_tools_list(const json&) { return tools_.list_payload(); }

json McpServer::handle_tools_call(const json& params) {
    const std::string name = params.value("name", std::string());
    json args = params.contains("arguments") ? params["arguments"] : json::object();

    // Self-heal before each call: lazily (re)connect to the engine (it may have
    // been started after us, or restarted mid-session) and refresh capabilities
    // if the connection generation advanced.
    rc_.ensure_connected();
    caps_.ensure_probed(rc_);

    ToolContext ctx{rc_, caps_};
    ToolResult tr = tools_.invoke(ctx, name, args);

    // MCP tools/call result: content blocks + isError. We serialize the
    // structured payload as a single text block of pretty JSON.
    return json{
        {"content", json::array({json{{"type", "text"}, {"text", tr.payload.dump(2)}}})},
        {"isError", tr.is_error},
    };
}

std::optional<json> McpServer::handle_message(const json& msg) {
    // Notifications (no "id") never get a response.
    const bool has_id = msg.contains("id") && !msg["id"].is_null();
    json id = has_id ? msg["id"] : json(nullptr);
    const std::string method = msg.value("method", std::string());

    if (method.empty()) {
        if (!has_id) return std::nullopt;
        return make_error(id, -32600, "Invalid Request: missing method");
    }

    // Notifications we accept silently.
    if (!has_id) {
        // e.g. "notifications/initialized" — acknowledge by doing nothing.
        return std::nullopt;
    }

    const json params = msg.contains("params") ? msg["params"] : json::object();

    try {
        if (method == "initialize") {
            return make_result(id, handle_initialize(params));
        }
        if (method == "ping") {
            return make_result(id, json::object());
        }
        if (method == "tools/list") {
            return make_result(id, handle_tools_list(params));
        }
        if (method == "tools/call") {
            return make_result(id, handle_tools_call(params));
        }
        return make_error(id, -32601, "Method not found: " + method);
    } catch (const std::exception& e) {
        return make_error(id, -32603, std::string("Internal error: ") + e.what());
    }
}

int McpServer::run(std::istream& in, std::ostream& out) {
    // Escape non-ASCII to \uXXXX on the wire so the stdout stream is valid for
    // any MCP client regardless of its locale/codec (e.g. a GBK console on a
    // Chinese Windows install). RemoteControl error messages can contain
    // non-ASCII text.
    auto emit = [&out](const json& j) {
        out << j.dump(/*indent=*/-1, /*indent_char=*/' ',
                      /*ensure_ascii=*/true)
            << "\n"
            << std::flush;
    };
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        json msg = json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (msg.is_discarded()) {
            emit(make_error(nullptr, -32700, "Parse error"));
            continue;
        }
        // Support a JSON-RPC batch (array of messages).
        if (msg.is_array()) {
            json responses = json::array();
            for (const auto& m : msg) {
                auto r = handle_message(m);
                if (r) responses.push_back(*r);
            }
            if (!responses.empty()) emit(responses);
            continue;
        }
        auto resp = handle_message(msg);
        if (resp) emit(*resp);
    }
    return 0;
}

}  // namespace ue_mcp_for_all_versions
