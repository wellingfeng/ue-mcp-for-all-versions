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
    // Optional image content block (base64). When set, the MCP server emits an
    // additional {type:"image", data, mimeType} content block alongside the
    // text payload — used by tools that return rendered pixels (thumbnails).
    std::string image_base64;
    std::string image_mime;  // e.g. "image/png"

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
    // Result carrying an image (plus a small JSON summary text block).
    static ToolResult image(std::string base64, std::string mime, json summary) {
        ToolResult r;
        r.is_error = false;
        r.payload = std::move(summary);
        r.payload["status"] = "ok";
        r.image_base64 = std::move(base64);
        r.image_mime = std::move(mime);
        return r;
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
    // Scene introspection tools: find-by-class/label, components, bounds. These
    // let an agent query the scene directly instead of guessing visually.
    void register_introspection_tools();
    // Component-level transform/bounds, Blueprint component templates, character
    // mesh normalization, and PIE input helpers.
    void register_component_tools();
    // Material + visual tools: dynamic material params, object thumbnails.
    void register_material_tools();
    // Editor workflow + convenience tools: undo/redo, spawn-from-asset, etc.
    void register_workflow_tools();
    // Layer 1: scene/actor authoring (batch spawn, duplicate, attach, folders).
    void register_scene_tools();
    // Layer 1: mesh + lighting edits (static mesh swap, materials, light props).
    void register_mesh_light_tools();
    // Layer 1: asset/material creation + import (Python recipes).
    void register_creation_tools();
    // Layer 1: data table read/write + extra debug (cvar write, log, PIE start).
    void register_data_debug_tools();
    // Layer 2: blueprint + UMG authoring (Python recipes; multi-strategy).
    void register_authoring_tools();
    // Pencil -> UMG conversion via the external Pencil2UMG editor plugin:
    // status/version check, confirm-gated download of the latest GitHub
    // release, and the actual .pen -> Widget Blueprint import.
    void register_pencil2umg_tools();
    // Figma -> UMG conversion via the external Figma2UMG editor plugin
    // (Buvi Games, MIT). No prebuilt releases: status/update is tracked by the
    // main-branch commit SHA, confirm-gated install pulls the source zipball,
    // and the conversion tool pre-fills the Figma token / file key settings then
    // guides the (C++-only, non-scriptable) import trigger.
    void register_figma2umg_tools();
    // Layer 3: deeper Blueprint authoring — add components (SubobjectData),
    // set class-default properties, reparent, and add function graphs.
    void register_blueprint_graph_tools();
    // Layer 3: PCG (Procedural Content Generation) — create PCG graphs, spawn
    // PCG volumes, assign a graph and trigger generation. PCG plugin (5.2+).
    void register_pcg_tools();
    // Layer 3: terrain / Landscape — create a flat landscape, set its material,
    // import a heightmap, and read landscape info. Degrades on engines whose
    // Python API can't author landscapes.
    void register_terrain_tools();
    // Layer 3: sky + atmosphere — spawn/configure SkyAtmosphere, the sun
    // (DirectionalLight), SkyLight, VolumetricCloud, and ExponentialHeightFog.
    void register_sky_atmosphere_tools();
    // Layer 3: water — spawn and configure Water plugin bodies (ocean/lake/
    // river/island). Degrades when the Water plugin is not enabled.
    void register_water_tools();
    // Layer 3: material shader authoring — add material expression nodes,
    // connect them to each other or to material properties, and set top-level
    // material properties (blend mode, shading model, two-sided), then recompile.
    void register_material_shader_tools();

    std::vector<Tool> tools_;
};

}  // namespace ue_mcp_for_all_versions
