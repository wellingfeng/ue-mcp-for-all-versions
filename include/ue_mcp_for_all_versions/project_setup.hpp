// ue-mcp-for-all-versions
// Unreal project setup helper: configures a .uproject so this single MCP
// binary can be used with UE 4.25 -> 5.x without manual plugin / ini edits.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ue_mcp_for_all_versions/rc_client.hpp"

namespace ue_mcp_for_all_versions {

struct ProjectSetupOptions {
    std::filesystem::path input_path;
    std::string server_command;
    bool enable_python = true;
    bool write_mcp_config = true;
    bool dry_run = false;
};

struct ProjectSetupResult {
    bool ok = false;
    bool changed = false;
    std::string error;
    std::filesystem::path project_dir;
    std::filesystem::path uproject_path;
    std::string engine_association;
    std::vector<std::string> configured_plugins;
    std::vector<std::string> changed_files;
    std::vector<std::string> notes;
    std::vector<std::string> warnings;

    json to_json() const;
};

ProjectSetupResult setup_unreal_project(const ProjectSetupOptions& options);

}  // namespace ue_mcp_for_all_versions
