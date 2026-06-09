# ue-mcp-for-all-versions

A single, version-agnostic **C++ MCP server** that drives **any Unreal Engine
version (4.25 → 5.8)** through the engine's built-in **RemoteControl** HTTP API.

The server links **no Unreal headers**. It is an ordinary native executable that
speaks the [Model Context Protocol](https://modelcontextprotocol.io) over stdio
to an MCP client (Claude Desktop / Claude Code / Cursor / …) and talks plain
HTTP/JSON to the editor. Because it has no engine ABI or `.modules` BuildId
dependency, **one binary works across every engine version**, and APIs that a
given version lacks are reported as a structured `"unsupported"` result at
runtime instead of failing.

## Why this design

A native in-engine C++ plugin **cannot** be a single drop-in binary across
versions: UE enforces an exact-match `.modules` BuildId at load and guarantees
no C++ ABI stability between versions (even 5.x → 5.x). Forcing a mismatched
DLL to load corrupts memory rather than failing gracefully — so "auto-degrade
when an API is missing" is impossible *inside* a compiled engine module.

This project sidesteps the problem entirely: the binary lives **outside** the
engine and uses RemoteControl's HTTP wire protocol, which is far more stable
across versions than the engine's C++ classes. Capability differences are
discovered **at runtime**.

```
[ MCP Client ] <--stdio JSON-RPC--> [ ue-mcp-for-all-versions.exe ] <--HTTP--> [ UE (any version) ]
                                          (one binary)                 :30010 / :8080   RemoteControl
```

## Requirements on the UE side (zero code intrusion)

Enable the engine's stock **RemoteControl** plugin in your project (no source
mod, no custom plugin). Then ensure the web server is running:

- **UE 4.26+** auto-starts it on `:30010` by default.
- **UE 4.25** uses `:8080` and does **not** auto-start — run `WebControl.StartServer`
  in the editor console (or launch with
  `-ExecCmds="WebControl.EnableServerOnStartup 1; WebControl.StartServer"`).

The server probes `:30010` then `:8080` automatically.

## Build

Requirements: CMake ≥ 3.20 and a C++17 compiler (MSVC 2022 on Windows).
Dependencies (`nlohmann/json`, `cpp-httplib`) are vendored under `third_party/`
— nothing to fetch.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/ue-mcp-for-all-versions.exe`. On MSVC it links the static
runtime, so it is genuinely copy-and-run — distribute the single `.exe`.

## Usage

```
ue-mcp-for-all-versions [--host H] [--port P] [--probe]
  --host H   RemoteControl host (default 127.0.0.1)
  --port P   force a single RC port (default: probe 30010 then 8080)
  --probe    connect, print detected engine + capabilities as JSON, then exit
```

Register it with an MCP client (example `.mcp.json`):

```json
{
  "mcpServers": {
    "ue-mcp-for-all-versions": {
      "command": "C:/path/to/ue-mcp-for-all-versions.exe"
    }
  }
}
```

### Registering with CLI clients

Because the server speaks the standard MCP stdio transport, any MCP-capable CLI
can use it. The binary is the same for every UE version.

**Claude Code:**
```bash
claude mcp add ue-mcp-for-all-versions -- C:/path/to/ue-mcp-for-all-versions.exe
```
(or drop the `examples/mcp.json` content into your project's `.mcp.json`.)

**Codex CLI** — in `~/.codex/config.toml`:
```toml
[mcp_servers.ue-mcp-for-all-versions]
command = "C:/path/to/ue-mcp-for-all-versions.exe"
args = []
```

**Gemini CLI** — in `~/.gemini/settings.json`:
```json
{ "mcpServers": { "ue-mcp-for-all-versions": {
    "command": "C:/path/to/ue-mcp-for-all-versions.exe" } } }
```

You can start the CLI before the editor — the server connects lazily (see
*Connection lifecycle* below).

## Tools

48 tools, grouped below. Every tool is **capability-gated**: if the connected
engine lacks the required capability it returns
`{"status":"unsupported", "reason": ..., "missingCapability": ...}` instead of
failing. Tools marked *modern→legacy* call the UE5 editor subsystem first and
fall back to the UE4 library automatically, so one tool spans 4.25 → 5.8.

### Core RemoteControl

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_get_engine_version` | — | Engine version + detected capabilities |
| `ue_call_function` | object.call | Call any UFunction (`/remote/object/call`) |
| `ue_get_property` / `ue_set_property` | object.property | Read / write a property |
| `ue_remote_info` | info (4.26+) | RemoteControl server info / routes |
| `ue_describe_object` | object.describe (4.26+) | Object properties & functions |
| `ue_search_assets` | search.assets (4.26+) | Search project assets |
| `ue_batch` | batch (4.26+) | Multiple RC requests in one round-trip |

### Scripting & editor context

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_python_exec` | python (probed) | Run Python and capture result + log (`ExecutePythonCommandEx`). 4.25→5.8 when the PythonScriptPlugin is enabled |
| `ue_exec_console_command` | object.call | Run an editor/console command (auto-resolves world context on UE4) |
| `ue_get_editor_world` | object.call | Editor UWorld path (*modern→legacy*) |
| `ue_get_project_info` | object.call | Aggregated engine/project metadata |

### Actors

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_list_actors` | object.call | List level actors (*modern→legacy*) |
| `ue_spawn_actor` / `ue_destroy_actor` | object.call | Spawn / destroy an actor (*modern→legacy*) |
| `ue_get_actor_transform` | object.call | Read an actor's transform |
| `ue_set_actor_location` / `_rotation` / `_scale` | object.call | Set actor location / rotation / scale |
| `ue_get_actor_label` / `ue_set_actor_label` | object.call | Read / set the World Outliner label |
| `ue_get_selected_actors` / `ue_select_actors` / `ue_clear_selection` | object.call | Editor selection (*modern→legacy*) |

### Assets

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_list_assets` / `ue_does_asset_exist` | object.call | List / check assets (*modern→legacy*) |
| `ue_find_asset_data` | object.call | Asset registry metadata |
| `ue_save_asset` / `ue_save_directory` | object.call | Save an asset / a directory |
| `ue_duplicate_asset` / `ue_rename_asset` | object.call | Duplicate / rename an asset |
| `ue_delete_asset` | object.call | **Delete** an asset (destructive) |

### Level, viewport & play

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_save_current_level` / `ue_load_level` / `ue_new_level` | object.call | Level open / save / new (*modern→legacy*) |
| `ue_get_viewport_camera` / `ue_set_viewport_camera` | object.call | Read / move the editor viewport camera |
| `ue_take_screenshot` | object.call | High-res editor screenshot |
| `ue_is_pie` / `ue_stop_pie` | pie.control (5.x) | Query / end Play-In-Editor |

### Property array ops & presets

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_property_array_append` / `_insert` / `_remove` | object.property.arrayops (5.0+) | In-place array property edits |
| `ue_list_presets` / `ue_get_preset` | presets (4.26+) | List / inspect RemoteControl presets |
| `ue_preset_call_function` | presets (4.26+) | Invoke a function exposed on a preset |
| `ue_preset_get_property` / `ue_preset_set_property` | presets (4.26+) | Read / write an exposed preset property |

The editor-side helpers call the engine's `EditorScriptingUtilities` library /
UE5 editor subsystems, so enable that plugin too (it ships with the engine).
For `ue_python_exec`, enable the **PythonScriptPlugin**. For
`ue_exec_console_command`, enable *Allow Console Command Remote Execution* in
Project Settings → Remote Control.

A tool whose capability is absent on the connected engine returns
`{"status":"unsupported", "reason": ..., "missingCapability": ...}` — never a
hard failure.

## Connection lifecycle (works with long-lived CLI clients)

MCP CLIs (Claude Code, Codex, Gemini, Cursor, Cline) start the server once and
keep it alive for the whole session. The server is built for that:

- **Lazy connect.** It does not require the engine to be running at launch. It
  connects on first use, so you can start the CLI first and the editor later.
- **Auto-reconnect.** If the editor is closed and reopened (or restarted) mid
  session, the next tool call transparently re-probes ports, reconnects, and
  refreshes capabilities — no need to restart the CLI.
- **Throttled.** While the engine is down, reconnect attempts are rate-limited
  so a tool call fails fast instead of stalling on a full port-probe timeout.

While disconnected, capability-gated tools return a connection *error* (the
engine isn't there to tell us what it supports); once connected they behave per
the capability table above.


## Verified versions

The **same single binary** was integration-tested against four installed engine
versions (one process, no recompilation). Per-version capability detection and
runtime degradation behave as designed:

| Engine | Port | `/remote/info` | Capabilities | `ue_list_actors` | 4.26+ tools |
|--------|------|----------------|--------------|------------------|-------------|
| 4.25.4 | 8080 | absent | 2 (call, property) | ✓ (legacy lib) | `unsupported` |
| 4.26.2 | 30010 | 14 routes | 7 (no array-ops) | ✓ (legacy lib) | ✓ |
| 5.3.2  | 30010 | 31 routes | 8 | ✓ (subsystem) | ✓ |
| 5.5.1  | 30010 | 31 routes | 8 | ✓ (subsystem) | ✓ |

`ue_list_actors`/`ue_spawn_actor`/`ue_destroy_actor` prefer the UE5
`EditorActorSubsystem` and fall back to the UE4 `EditorLevelLibrary`
automatically, so one tool works across the 4.x ↔ 5.x boundary.

The expanded tool set (Python, actor transform/label/selection, asset
save/duplicate/rename/delete, level/viewport, PIE, property array ops, presets)
was re-verified live against UE 5.5.1: spawning an actor, setting its
location/rotation/scale, reading the transform back, setting/reading its label,
selecting it, and saving — all confirmed end-to-end. Capability degradation was
confirmed too (e.g. `ue_python_exec` returns `unsupported` when the
PythonScriptPlugin is disabled rather than erroring).

UE 5.8 (which also ships Epic's own experimental `ModelContextProtocol` plugin)
is not yet hardware-tested here; the design adapts at runtime via `/remote/info`
route discovery and engine-version inference.

## Tests

```bash
# unit tests (no engine required)
ctest --test-dir build -C Release --output-on-failure

# cross-version integration (launches a real editor on a minimal project)
bash scripts/integration_test.sh 55  5.5  UnrealEditor 30010
bash scripts/integration_test.sh 425 4.25 UE4Editor    8080

# long-lived server / late-start / auto-reconnect (CLI usage pattern)
python scripts/drive_server.py 30010 full
```

## Layout

```
include/ue_mcp_for_all_versions/   public headers (namespace ue_mcp_for_all_versions)
src/                               rc_client / capability_registry / tool_registry / mcp_server / main
third_party/                       vendored nlohmann/json + cpp-httplib (header-only)
tests/                             dependency-free unit tests
scripts/                           cross-version integration harness
```

## License

MIT. See `LICENSE`. Vendored third-party headers keep their own licenses
(`nlohmann/json` — MIT; `cpp-httplib` — MIT).

