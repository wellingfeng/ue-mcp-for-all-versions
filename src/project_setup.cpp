// ue-mcp-for-all-versions — one-click Unreal project setup implementation.
#include "ue_mcp_for_all_versions/project_setup.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace ue_mcp_for_all_versions {
namespace fs = std::filesystem;

namespace {

struct ParsedAssociation {
    bool known = false;
    int major = 0;
    int minor = 0;
};

std::string to_generic_string(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

bool starts_with_utf8_bom(const std::string& s) {
    return s.size() >= 3 &&
           static_cast<unsigned char>(s[0]) == 0xEF &&
           static_cast<unsigned char>(s[1]) == 0xBB &&
           static_cast<unsigned char>(s[2]) == 0xBF;
}

std::string strip_utf8_bom(std::string s) {
    if (starts_with_utf8_bom(s)) s.erase(0, 3);
    return s;
}

std::string read_text_file(const fs::path& path, bool& ok, std::string& error) {
    ok = false;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "failed to open " + to_generic_string(path);
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

bool write_text_file(const fs::path& path, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "failed to create directory " + to_generic_string(path.parent_path()) +
                ": " + ec.message();
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to write " + to_generic_string(path);
        return false;
    }
    out << text;
    return true;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : text) {
        if (c == '\r') continue;
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty() || (!text.empty() && text.back() == '\n')) {
        lines.push_back(current);
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i] << "\r\n";
    }
    return out.str();
}

ParsedAssociation parse_association(const std::string& raw) {
    ParsedAssociation out;
    size_t i = 0;
    while (i < raw.size() && !std::isdigit(static_cast<unsigned char>(raw[i]))) ++i;
    if (i >= raw.size()) return out;

    int parts[2] = {0, 0};
    int idx = 0;
    while (i < raw.size() && idx < 2 &&
           std::isdigit(static_cast<unsigned char>(raw[i]))) {
        int n = 0;
        while (i < raw.size() &&
               std::isdigit(static_cast<unsigned char>(raw[i]))) {
            n = n * 10 + (raw[i] - '0');
            ++i;
        }
        parts[idx++] = n;
        if (i < raw.size() && raw[i] == '.') ++i;
    }
    if (idx >= 1) {
        out.known = true;
        out.major = parts[0];
        out.minor = parts[1];
    }
    return out;
}

bool is_uproject_path(const fs::path& path) {
    return lower(path.extension().string()) == ".uproject";
}

bool find_uproject(const fs::path& input, fs::path& out_path, std::string& error) {
    std::error_code ec;
    fs::path absolute = fs::absolute(input, ec);
    if (ec) absolute = input;

    if (fs::is_regular_file(absolute, ec) && is_uproject_path(absolute)) {
        out_path = absolute;
        return true;
    }
    if (!fs::is_directory(absolute, ec)) {
        error = "input is not a .uproject file or project directory: " +
                to_generic_string(input);
        return false;
    }

    std::vector<fs::path> matches;
    for (const auto& entry : fs::directory_iterator(absolute, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec) && is_uproject_path(entry.path())) {
            matches.push_back(entry.path());
        }
    }
    if (matches.empty()) {
        error = "no .uproject file found directly under " + to_generic_string(absolute);
        return false;
    }
    if (matches.size() > 1) {
        error = "multiple .uproject files found under " + to_generic_string(absolute) +
                "; pass the desired .uproject path explicitly";
        return false;
    }
    out_path = matches.front();
    return true;
}

bool ensure_plugin(json& project, const std::string& name, bool enabled) {
    if (!project.contains("Plugins") || !project["Plugins"].is_array()) {
        project["Plugins"] = json::array();
    }
    for (auto& plugin : project["Plugins"]) {
        if (!plugin.is_object()) continue;
        if (!plugin.contains("Name") || !plugin["Name"].is_string()) continue;
        if (plugin["Name"].get<std::string>() == name) {
            bool changed = false;
            if (!plugin.contains("Enabled") || !plugin["Enabled"].is_boolean() ||
                plugin["Enabled"].get<bool>() != enabled) {
                plugin["Enabled"] = enabled;
                changed = true;
            }
            return changed;
        }
    }
    project["Plugins"].push_back({{"Name", name}, {"Enabled", enabled}});
    return true;
}

bool upsert_ini_value(std::vector<std::string>& lines, const std::string& section,
                      const std::string& key, const std::string& value) {
    const std::string wanted = key + "=" + value;
    const std::string target_section = lower(section);
    const std::string target_key = lower(key);

    bool changed = false;
    bool in_section = false;
    bool saw_section = false;
    bool wrote_key = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = trim(lines[i]);
        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            if (in_section && !wrote_key) {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(i), wanted);
                changed = true;
                wrote_key = true;
                ++i;
            }
            in_section = lower(t) == target_section;
            saw_section = saw_section || in_section;
            continue;
        }

        if (!in_section) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string existing_key = lower(trim(t.substr(0, eq)));
        if (existing_key != target_key) continue;

        if (!wrote_key) {
            if (lines[i] != wanted) {
                lines[i] = wanted;
                changed = true;
            }
            wrote_key = true;
        } else {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
            --i;
            changed = true;
        }
    }

    if (saw_section && in_section && !wrote_key) {
        lines.push_back(wanted);
        changed = true;
        wrote_key = true;
    }
    if (!saw_section) {
        if (!lines.empty() && !lines.back().empty()) lines.push_back("");
        lines.push_back(section);
        lines.push_back(wanted);
        changed = true;
    }
    return changed;
}

bool upsert_ini_file(const fs::path& path,
                     const std::vector<std::pair<std::string, std::string>>& values,
                     bool dry_run, std::string& error) {
    bool read_ok = false;
    std::string text;
    if (fs::exists(path)) {
        text = strip_utf8_bom(read_text_file(path, read_ok, error));
        if (!read_ok) return false;
    } else {
        read_ok = true;
    }

    std::vector<std::string> lines = split_lines(text);
    bool changed = false;
    for (const auto& value : values) {
        const size_t pos = value.first.find('|');
        if (pos == std::string::npos) continue;
        changed = upsert_ini_value(lines, value.first.substr(0, pos),
                                   value.first.substr(pos + 1), value.second) ||
                  changed;
    }
    if (!changed) return false;
    if (!dry_run && !write_text_file(path, join_lines(lines), error)) {
        return false;
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> remote_control_settings_426() {
    const std::string section = "[/Script/WebRemoteControl.WebRemoteControlSettings]";
    return {
        {section + "|bAutoStartWebServer", "True"},
        {section + "|bAutoStartWebSocketServer", "True"},
        {section + "|RemoteControlHttpServerPort", "30010"},
        {section + "|RemoteControlWebSocketServerPort", "30020"},
    };
}

std::vector<std::pair<std::string, std::string>> remote_control_settings_5x(
    bool enable_python) {
    const std::string section = "[/Script/RemoteControlCommon.RemoteControlSettings]";
    std::vector<std::pair<std::string, std::string>> values = {
        {section + "|bAutoStartWebServer", "True"},
        {section + "|bAutoStartWebSocketServer", "True"},
        {section + "|RemoteControlHttpServerPort", "30010"},
        {section + "|RemoteControlWebSocketServerPort", "30020"},
        {section + "|RemoteControlWebsocketServerBindAddress", "127.0.0.1"},
    };
    if (enable_python) {
        // Full-access profile (the default): let the MCP server drive every
        // editor operation without hitting safety gates that can only be
        // cleared interactively and therefore cannot be answered over a remote
        // connection (which is what causes the "switch is itself protected, so
        // it cannot be flipped remotely" deadlock). All flags below are
        // URemoteControlSettings config properties that live in
        // DefaultRemoteControl.ini and are only read at editor startup.
        //   - bEnableRemotePythonExecution: allow remote Python evaluation.
        //   - bIgnoreProtectedCheck: allow writing protected properties (e.g.
        //     WidgetTree edits) over RemoteControl.
        //   - bIgnoreGetterSetterCheck: allow direct property writes that would
        //     otherwise require a blueprint getter/setter.
        values.push_back({section + "|bEnableRemotePythonExecution", "True"});
        values.push_back({section + "|bIgnoreProtectedCheck", "True"});
        values.push_back({section + "|bIgnoreGetterSetterCheck", "True"});
    }
    return values;
}

std::vector<std::pair<std::string, std::string>> remote_control_settings_425() {
    return {{"[SystemSettings]|WebControl.EnableServerOnStartup", "1"}};
}

bool merge_mcp_json(const fs::path& path, const std::string& server_command,
                    bool dry_run, std::string& error) {
    json root = json::object();
    if (fs::exists(path)) {
        bool ok = false;
        std::string text = strip_utf8_bom(read_text_file(path, ok, error));
        if (!ok) return false;
        if (!trim(text).empty()) {
            try {
                root = json::parse(text);
            } catch (const std::exception& e) {
                error = "failed to parse " + to_generic_string(path) + ": " + e.what();
                return false;
            }
        }
    }

    if (!root.is_object()) {
        error = ".mcp.json root must be an object: " + to_generic_string(path);
        return false;
    }
    if (!root.contains("mcpServers") || !root["mcpServers"].is_object()) {
        root["mcpServers"] = json::object();
    }

    json next = {{"command", server_command}, {"args", json::array()}};
    const json* existing = nullptr;
    if (root["mcpServers"].contains("ue-mcp-for-all-versions")) {
        existing = &root["mcpServers"]["ue-mcp-for-all-versions"];
    }
    if (existing && *existing == next) return false;

    root["mcpServers"]["ue-mcp-for-all-versions"] = std::move(next);
    if (!dry_run && !write_text_file(path, root.dump(2) + "\n", error)) {
        return false;
    }
    return true;
}

std::string default_server_command() {
    return "ue-mcp-for-all-versions";
}

void mark_file_changed(ProjectSetupResult& result, const fs::path& path) {
    const std::string p = to_generic_string(path);
    if (std::find(result.changed_files.begin(), result.changed_files.end(), p) ==
        result.changed_files.end()) {
        result.changed_files.push_back(p);
    }
    result.changed = true;
}

}  // namespace

json ProjectSetupResult::to_json() const {
    return {
        {"ok", ok},
        {"changed", changed},
        {"error", error},
        {"projectDir", to_generic_string(project_dir)},
        {"uprojectPath", to_generic_string(uproject_path)},
        {"engineAssociation", engine_association},
        {"configuredPlugins", configured_plugins},
        {"changedFiles", changed_files},
        {"notes", notes},
        {"warnings", warnings},
    };
}

ProjectSetupResult setup_unreal_project(const ProjectSetupOptions& options) {
    ProjectSetupResult result;

    fs::path uproject;
    if (!find_uproject(options.input_path, uproject, result.error)) {
        return result;
    }
    result.uproject_path = uproject;
    result.project_dir = uproject.parent_path();

    bool read_ok = false;
    std::string read_error;
    std::string uproject_text = strip_utf8_bom(read_text_file(uproject, read_ok, read_error));
    if (!read_ok) {
        result.error = read_error;
        return result;
    }

    json project;
    try {
        project = json::parse(uproject_text);
    } catch (const std::exception& e) {
        result.error = "failed to parse " + to_generic_string(uproject) + ": " + e.what();
        return result;
    }
    if (!project.is_object()) {
        result.error = ".uproject root must be an object: " + to_generic_string(uproject);
        return result;
    }

    if (project.contains("EngineAssociation") && project["EngineAssociation"].is_string()) {
        result.engine_association = project["EngineAssociation"].get<std::string>();
    }
    ParsedAssociation version = parse_association(result.engine_association);

    bool project_changed = false;
    const std::vector<std::string> required_plugins = options.enable_python
        ? std::vector<std::string>{"RemoteControl", "EditorScriptingUtilities",
                                   "PythonScriptPlugin"}
        : std::vector<std::string>{"RemoteControl", "EditorScriptingUtilities"};
    for (const auto& plugin : required_plugins) {
        if (ensure_plugin(project, plugin, true)) {
            project_changed = true;
        }
        result.configured_plugins.push_back(plugin);
    }

    if (project_changed) {
        if (!options.dry_run &&
            !write_text_file(uproject, project.dump(1, '\t') + "\n", result.error)) {
            return result;
        }
        mark_file_changed(result, uproject);
    }

    const fs::path config_dir = result.project_dir / "Config";
    if (version.known && version.major == 4 && version.minor <= 25) {
        const fs::path engine_ini = config_dir / "DefaultEngine.ini";
        if (upsert_ini_file(engine_ini, remote_control_settings_425(), options.dry_run,
                            result.error)) {
            mark_file_changed(result, engine_ini);
        } else if (!result.error.empty()) {
            return result;
        }
        result.notes.push_back(
            "UE 4.25 uses RemoteControl on port 8080; the MCP server probes it automatically.");
        result.notes.push_back(
            "If the editor was already open, run WebControl.StartServer once or restart the editor.");
    } else if (version.known && version.major == 4) {
        const fs::path web_ini = config_dir / "DefaultWebRemoteControl.ini";
        if (upsert_ini_file(web_ini, remote_control_settings_426(), options.dry_run,
                            result.error)) {
            mark_file_changed(result, web_ini);
        } else if (!result.error.empty()) {
            return result;
        }
    } else if (version.known && version.major >= 5) {
        const fs::path rc_ini = config_dir / "DefaultRemoteControl.ini";
        if (upsert_ini_file(rc_ini, remote_control_settings_5x(options.enable_python),
                            options.dry_run, result.error)) {
            mark_file_changed(result, rc_ini);
        } else if (!result.error.empty()) {
            return result;
        }
        if (options.enable_python) {
            result.notes.push_back(
                "UE 5.x full-access profile written to DefaultRemoteControl.ini: remote "
                "Python plus bIgnoreProtectedCheck / bIgnoreGetterSetterCheck, so the MCP "
                "server can edit protected properties (e.g. WidgetTree) without interactive "
                "confirmation prompts.");
        }
    } else {
        const fs::path engine_ini = config_dir / "DefaultEngine.ini";
        const fs::path web_ini = config_dir / "DefaultWebRemoteControl.ini";
        const fs::path rc_ini = config_dir / "DefaultRemoteControl.ini";
        if (upsert_ini_file(engine_ini, remote_control_settings_425(), options.dry_run,
                            result.error)) {
            mark_file_changed(result, engine_ini);
        } else if (!result.error.empty()) {
            return result;
        }
        if (upsert_ini_file(web_ini, remote_control_settings_426(), options.dry_run,
                            result.error)) {
            mark_file_changed(result, web_ini);
        } else if (!result.error.empty()) {
            return result;
        }
        if (upsert_ini_file(rc_ini, remote_control_settings_5x(options.enable_python),
                            options.dry_run, result.error)) {
            mark_file_changed(result, rc_ini);
        } else if (!result.error.empty()) {
            return result;
        }
        result.warnings.push_back(
            "EngineAssociation is missing or custom; wrote compatible settings for UE 4.25, 4.26 and 5.x.");
    }

    if (options.write_mcp_config) {
        const fs::path mcp_path = result.project_dir / ".mcp.json";
        const std::string command = options.server_command.empty()
                                        ? default_server_command()
                                        : options.server_command;
        if (merge_mcp_json(mcp_path, command, options.dry_run, result.error)) {
            mark_file_changed(result, mcp_path);
        } else if (!result.error.empty()) {
            return result;
        }
    }

    if (options.enable_python) {
        result.notes.push_back(
            "Python MCP tools are enabled by adding PythonScriptPlugin; UE 5.x also enables Remote Python execution.");
    }
    // The RemoteControl security flags and the WebControl startup CVars are read
    // only when the editor boots, so a hot reload will not pick them up. Surface
    // this as a warning (not just a note) whenever we actually wrote files, so
    // callers / users do not silently keep talking to a stale editor.
    if (result.changed) {
        result.warnings.push_back(
            "RESTART REQUIRED: close and reopen the Unreal Editor so the new plugin and "
            "RemoteControl settings take effect. They are only read at editor startup; a "
            "running editor will not pick them up.");
    }
    result.notes.push_back(
        "Restart the Unreal Editor after plugin or RemoteControl settings change.");
    result.notes.push_back(
        "The MCP server keeps lazy-connect behavior, so it can be started before the editor.");

    result.ok = true;
    return result;
}

}  // namespace ue_mcp_for_all_versions
