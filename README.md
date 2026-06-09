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

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_get_engine_version` | — | Engine version + detected capabilities |
| `ue_call_function` | object.call | Call a UFunction (`/remote/object/call`) |
| `ue_get_property` | object.property | Read a property |
| `ue_set_property` | object.property | Write a property |
| `ue_remote_info` | info (4.26+) | RemoteControl server info / routes |
| `ue_describe_object` | object.describe (4.26+) | Object properties & functions |
| `ue_search_assets` | search.assets (4.26+) | Search project assets |
| `ue_batch` | batch (4.26+) | Multiple RC requests in one round-trip |
| `ue_list_actors` | object.call | List actors in the current level |
| `ue_spawn_actor` | object.call | Spawn an actor from a class at a location |
| `ue_destroy_actor` | object.call | Destroy an actor by object path |
| `ue_list_assets` | object.call | List assets under a content directory |
| `ue_does_asset_exist` | object.call | Check whether an asset path exists |

The high-level helpers (`ue_list_actors`, `ue_spawn_actor`, …) call the engine's
`EditorScriptingUtilities` library, so enable that plugin too (it ships with the
engine). They work editor-side only.

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

UE 5.8 (which also ships Epic's own experimental `ModelContextProtocol` plugin)
is not yet hardware-tested here; the design adapts at runtime via `/remote/info`
route discovery.

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

