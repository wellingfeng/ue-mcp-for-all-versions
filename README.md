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

Use the one-click setup command below for the normal path. It enables the
engine's stock plugins and writes the version-specific RemoteControl settings
needed by this server (no source mod, no custom plugin).

Manual requirements, if you do not use setup:

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

### One-click project setup

Point the binary at either a `.uproject` file or the project directory:

```bash
ue-mcp-for-all-versions --setup-project C:/path/to/MyGame
```

The setup command is designed for host apps such as FreeUltraCode to call from
a single "Enable Unreal MCP" button. It:

- detects the `.uproject` and `EngineAssociation`
- enables `RemoteControl`, `EditorScriptingUtilities`, and `PythonScriptPlugin`
- writes the correct RemoteControl config file for the engine line:
  - UE 4.25: `Config/DefaultEngine.ini` startup CVar fallback
  - UE 4.26: `Config/DefaultWebRemoteControl.ini`
  - UE 5.x: `Config/DefaultRemoteControl.ini`
- on UE 5.x, writes the **full-access profile** by default so the MCP server
  can drive every editor operation without interactive prompts:
  `bEnableRemotePythonExecution`, `bIgnoreProtectedCheck`, and
  `bIgnoreGetterSetterCheck` are all set to `True`. This avoids the deadlock
  where protected-property edits (e.g. `WidgetTree.ConstructWidget`) require a
  confirmation that cannot be answered over a remote connection. Pass
  `--no-python` to keep the conservative defaults instead.
- writes/merges project `.mcp.json` with this server command
- prints a machine-readable JSON report listing changed files, notes and warnings

> These settings are read only when the editor boots. After setup, **restart the
> Unreal Editor** so the new plugins and RemoteControl flags take effect — a
> running editor will not hot-reload them. The JSON report surfaces this as a
> `RESTART REQUIRED` warning whenever files actually changed.

Useful setup flags:

```bash
ue-mcp-for-all-versions --setup-project C:/path/to/MyGame --dry-run
ue-mcp-for-all-versions --setup-project C:/path/to/MyGame --server-command C:/tools/ue-mcp-for-all-versions.exe
ue-mcp-for-all-versions --setup-project C:/path/to/MyGame --no-python
ue-mcp-for-all-versions --setup-project C:/path/to/MyGame --no-mcp-config
```

After setup, restart the Unreal Editor if plugins or RemoteControl settings
changed. The MCP server itself can be started before the editor; it connects
lazily and auto-reconnects.

Manual MCP client config, if you do not use setup (example `.mcp.json`):

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

104 tools, grouped below. Every tool is **capability-gated**: if the connected
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

### Scene introspection

Query the scene directly instead of guessing it from screenshots. These turn
"which actor is the terrain and what controls its color" into a single lookup
rather than a show/hide-and-screenshot search.

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_find_actors_by_class` | object.call | All actors matching a class path (composes `GetAllLevelActors` + `EditorFilterLibrary.ByClass`) |
| `ue_find_actors_by_label` | object.call | Actors whose Outliner label matches (Contains / Wildcard / Exact) |
| `ue_get_actor_components` | object.call | An actor's components, optionally filtered by class (`K2_GetComponentsByClass`) |
| `ue_get_actor_bounds` | object.call | World-space bounding box (origin + extent) |
| `ue_get_actor_reference` | object.call | Resolve an actor path/name to a concrete reference (UE5) |

### Component transforms, bounds & character scale

Component-level tools for adjusting Pawn internals such as a `HeroineMesh`,
camera, or spring arm without moving the whole actor. Python-backed read helpers
return normalized JSON instead of raw UE structs.

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_set_component_relative_transform` | object.call | Set a child SceneComponent's relative location / rotation / scale (`K2_SetRelativeLocation`, `K2_SetRelativeRotation`, `SetRelativeScale3D`) |
| `ue_get_component_transform` | python | Read component world transform, relative transform, attach parent, owner, class, and world scale |
| `ue_get_component_bounds` | python | Read local/world component bounds, or aggregate filtered actor bounds; defaults `includeEditorVisualization=false` to exclude camera frustums, billboards, arrows, sprites, and editor-only components |
| `ue_fit_mesh_component_to_height` | python | Read StaticMesh/SkeletalMesh local bounds, compute uniform scale for a target height such as 180, and optionally apply it |
| `ue_normalize_imported_character_mesh` | python | Suggest rotation and optional scale for common imported character axis conventions, defaulting to Unreal `targetUp=Z`, `targetForward=X`; can optionally apply to a component |
| `ue_get_blueprint_components` | python | List a Blueprint generated CDO's component tree: name, class, parent, relative transform, and common resource references |
| `ue_set_blueprint_component_template_transform` | python | Persistently edit a Blueprint component template on the generated CDO and exposed SCS template, for defaults such as `BP_HeroineThirdPersonPawn.HeroineMesh` |

### Materials & visuals

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_set_material_param` | object.call | Create a dynamic material instance on a component element and set a scalar / vector (color) / texture parameter — the precise way to recolor a mesh or landscape |
| `ue_set_material_instance_param` | object.call | Set a scalar / vector parameter on a Material Instance Constant **asset** (`MaterialEditingLibrary`) |
| `ue_get_object_thumbnail` | object.thumbnail (4.26+) | Render an object's thumbnail and return it as an **image** content block (PNG/JPEG per the engine) a vision client can see |

### Editor workflow

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_load_asset` | object.call | Load an asset and return its object path (*modern→legacy*) |
| `ue_spawn_actor_from_asset` | object.call | Spawn an actor from an asset (mesh / Blueprint / …) (*modern→legacy*) |
| `ue_set_actor_transform` | object.call | Set location + rotation + scale together (decomposed setters) |
| `ue_focus_viewport_on_actor` | object.call | Frame the viewport camera on an actor using its bounds |
| `ue_set_game_view` | object.call | Toggle viewport Game View (*modern→legacy*) |
| `ue_get_console_variable` | object.call | Read a CVar value (float / int / bool / string) |

### Scene authoring (Layer 1)

Build and organize scenes faster — batch spawning avoids the per-actor latency
that makes "build a town" painful.

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_batch_spawn_actors` | batch (4.26+) | Spawn many actors in one `/remote/batch` round-trip |
| `ue_duplicate_actor` | object.call | Duplicate a level actor with an optional offset (*modern→legacy*) |
| `ue_attach_actor` / `ue_detach_actor` | object.call | Parent / unparent actors (`K2_AttachToActor` / `K2_DetachFromActor`) |
| `ue_set_actor_folder` | object.property | Set an actor's World Outliner folder path |

### Mesh & lighting (Layer 1)

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_set_static_mesh` | object.call | Swap a StaticMeshComponent's mesh asset |
| `ue_set_actor_material` | object.call | Assign a material **asset** to a component's slot (`SetMaterial`) |
| `ue_set_light_property` | object.call | Set a LightComponent's intensity and/or color |

### Asset & material creation (Layer 1)

Python recipes — each runs as one `ExecutePythonCommandEx` round-trip, not a
sequence of REPL calls. Require the PythonScriptPlugin.

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_create_material` | python | Create a Material asset (optional constant base color) |
| `ue_create_material_instance` | python | Create a Material Instance Constant parented to a material |
| `ue_import_asset` | python | Import a source file (FBX/OBJ/PNG/…) via `AssetImportTask` |
| `ue_import_asset_with_transform_policy` | python | Import FBX/GLB/glTF with explicit convertScene, forceFrontXAxis, convertSceneUnit, importUniformScale, skeletal/static mesh import-data overrides, and material/texture flags where exposed by the engine |
| `ue_create_folder` | python | Create a content-browser folder |

### Data & debug (Layer 1)

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_data_table_get_rows` | python | Read a DataTable's rows + names as JSON |
| `ue_set_cvar` | object.call | **Write** a console variable (counterpart to `ue_get_console_variable`) |
| `ue_start_pie` | pie.control (5.5+) | Start a Play-In-Editor session (`EditorRequestBeginPlay`) |
| `ue_simulate_player_input` | python + pie.control | Inject key/axis input into the PIE PlayerController and report pawn movement delta, with an explicit AddMovementInput fallback when Python does not expose low-level input calls |

### Blueprint, gameplay & UMG authoring (Layer 2)

Creation tasks pure RemoteControl can't express — building an asset's internal
structure. Python recipes; require the PythonScriptPlugin (and
`BlueprintEditorLibrary`, UE5, for the blueprint tools).

| Tool | Requires | Description |
|------|----------|-------------|
| `ue_create_blueprint` | python | Create a Blueprint class asset with a chosen parent |
| `ue_add_blueprint_variable` | python | Add a member variable (UE5 `BlueprintEditorLibrary`) |
| `ue_compile_blueprint` | python | Compile and save a Blueprint |
| `ue_create_character_blueprint` | python | Create a `Character` Blueprint, configure capsule / mesh / `CharacterMovement`, and try to add a SpringArm + Camera |
| `ue_configure_character_movement` | python | Configure an existing Character Blueprint for grounded walking, gravity, jump, step height, floor angle, capsule size, mesh transform, and optional Anim Blueprint |
| `ue_calibrate_character_collision` | python | Calibrate capsule radius / half-height and mesh offset for a target character height, with Pawn / CharacterMesh collision profiles |
| `ue_configure_third_person_camera` | python | Add or update a SpringArm + Camera for third-person follow, shoulder offset, collision testing, and camera lag |
| `ue_create_enhanced_input_assets` | python | Create IA_Move / IA_Look / IA_Jump and an Input Mapping Context with WASD, mouse, SpaceBar, and common modifiers |
| `ue_create_locomotion_animation_assets` | python | Create an Animation Blueprint and 1D speed BlendSpace scaffold for a skeleton, with optional idle/walk/run samples where Python exposes sample editing |
| `ue_set_game_defaults` | python | Persist GameMapsSettings: game default map, editor startup map, and default GameMode |
| `ue_validate_third_person_pie` | python + pie.control | PIE smoke test for Player0 pawn class, possession, CharacterMovement, camera, and movement-input displacement |
| `ue_create_widget_blueprint` | python | Create a UMG Widget Blueprint **with a root panel** — tries several strategies and reports `strategiesTried`; returns `unsupported` (pointing at the Layer-3 plugin) on stripped builds where the root can't be set, rather than looping |
| `ue_add_widget_to_blueprint` | python | Add a child widget (Button / TextBlock / EditableTextBox / …) to a Widget Blueprint's root |
| `ue_inspect_widget_blueprint` | python | Inspect a Widget Blueprint tree: root, descendants, parent/child names, slot types, and common text/value properties |
| `ue_add_widget_to_panel` | python | Add a widget to a named panel/content widget with optional text, properties, and layout in one call |
| `ue_set_widget_properties` | python | Set common UMG properties such as text, tooltip, visibility, enabled state, opacity, font size, color, brush color, and image |
| `ue_configure_widget_layout` | python | Configure CanvasPanelSlot position / size / anchors / alignment / z-order and common panel-slot padding/alignment |
| `ue_create_widget_component_blueprint` | python | Create an Actor Blueprint with a WidgetComponent assigned to a UMG Widget Blueprint for world-space or screen-space UI |

> **UMG note (empirically confirmed on UE 5.5):** setting a Widget Blueprint's
> *root* widget is the one creation task that can't be done from outside the
> engine. `UWidgetTree` has no Python type binding (even after
> `load_module("UMG"/"UMGEditor")`), and `WidgetTree.RootWidget` carries the
> `EditConst` flag — so Python's `set_editor_property` AND the RemoteControl
> write path (even with `bIgnoreProtectedCheck`) both refuse it. RC *can* read
> the protected WidgetTree and `new_object(CanvasPanel, tree)` *can* create a
> panel, but the final assignment is blocked. `ue_create_widget_blueprint` is
> honest about this: it creates the asset, tries each path, reports
> `strategiesTried`, and returns `unsupported` — it does NOT loop (the failure
> mode that cost 27 minutes). Authoring the root needs the optional Layer-3
> in-engine plugin (see `.omc/plans/layer3-inengine-plugin-design.md`).

The one-click setup command enables the stock engine plugins needed by these
helpers. If configuring manually, enable `RemoteControl`,
`EditorScriptingUtilities`, and `PythonScriptPlugin`. For Python tools on
UE 5.x, setup also enables RemoteControl's remote Python execution project
setting by default.

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
