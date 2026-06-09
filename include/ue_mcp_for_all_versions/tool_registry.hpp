// ue-mcp-for-all-versions
// Tool registry: defines the MCP tools exposed to the client and maps each to
// RemoteControl operations, guarded by the capability registry.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ue_mcp_for_all_versions/capability_registry.hpp"
#include "ue_mcp_for_all_versions/rc_client.hpp"

namespace ue_mcp_for_all_versions {

// Result of executing a tool. Mirrors MCP's tools/call result: a block of text
// content plus an is_error flag. We always return structured JSON as text.
struct ToolResult {
    bool is_error = false;
    json payload;  // serialized into a text content block

    static ToolResult ok(json p) { return ToolResult{false, std::move(p)}; }
    static ToolResult error(std::string msg, json extra = json::object()) {
        json p = std::move(extra);
        p["status"] = "error";
        p["error"] = std::move(msg);
        return ToolResult{true, std::move(p)};
    }
    // Structured "unsupported" — used when a capability is missing on the
    // connected engine version. This is the runtime auto-degrade path.
    static ToolResult unsupported(std::string reason, json extra = json::object()) {
        json p = std::move(extra);
        p["status"] = "unsupported";
        p["reason"] = std::move(reason);
        return ToolResult{false, std::move(p)};
    }
};

// Context handed to each tool handler.
struct ToolContext {
    RcClient& rc;
    CapabilityRegistry& caps;
};

struct Tool {
    std::string name;
    std::string description;
    json input_schema;  // JSON Schema for arguments
    // Capabilities the tool requires; checked before invocation.
    std::vector<Capability> required;
    std::function<ToolResult(ToolContext&, const json& args)> handler;
};

class ToolRegistry {
public:
    // Register the built-in tool set.
    void register_builtins();

    void add(Tool tool);

    const std::vector<Tool>& tools() const { return tools_; }
    const Tool* find(const std::string& name) const;

    // tools/list payload.
    json list_payload() const;

    // Execute a tool by name, enforcing capability gates.
    ToolResult invoke(ToolContext& ctx, const std::string& name, const json& args) const;

private:
    // High-level editor convenience tools (actor/asset ops). Called by
    // register_builtins().
    void register_editor_helpers();
    // Python execution + console command + editor context tools.
    void register_scripting_tools();
    // Actor transform / label / selection tools.
    void register_actor_tools();
    // Extended asset operation tools (save/duplicate/delete/rename/metadata).
    void register_asset_tools();
    // Level / viewport camera / PIE control tools.
    void register_level_tools();
    // Raw RemoteControl route tools: property array ops + presets.
    void register_rc_route_tools();

    std::vector<Tool> tools_;
};

}  // namespace ue_mcp_for_all_versions
