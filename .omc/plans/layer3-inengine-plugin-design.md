# Layer 3 — In-Engine Helper Plugin (Design Only)

Status: **design, not implemented.** Awaiting explicit greenlight before any
engine-side code is written.

## Why a plugin is needed at all

Layers 1 and 2 reach Unreal purely through RemoteControl + editor Python. That
covers reading the scene, editing properties, calling exposed UFunctions, and
running multi-step Python recipes (asset/material/blueprint creation). It does
NOT reliably cover three things, because they touch engine internals that
Python and RemoteControl are not allowed to:

1. **Setting a Widget Blueprint's root widget** when the build's Python lacks a
   `UWidgetTree` binding and `RootWidget` is C++ `protected:` (the original
   login-screen failure). `ue_create_widget_blueprint` tries hard and then
   honestly returns `unsupported` on such builds.
2. **Authoring Blueprint graph nodes** — adding K2 nodes, wiring pins, setting
   defaults. The Python surface for this is partial and version-fragile.
3. **Writing arbitrary `protected`/`private` reflected members** that
   `bIgnoreProtectedCheck` doesn't lift (it lifts the RC getter/setter gate, not
   C++ access specifiers).

Every open-source UE MCP that does clean UMG/Blueprint authoring (chongdashu,
ChiR24, aadeshrao123) ships an in-engine plugin for exactly this reason.

## Design

A minimal editor-only UE plugin, `UEMCPBridge`, exposing a small set of
`UFUNCTION(BlueprintCallable)` entry points on a `UEMCPBridgeLibrary`
(BlueprintFunctionLibrary). The existing C++ MCP server calls them through the
SAME RemoteControl `call_function` path it already uses — no new transport.

### Candidate UFUNCTIONs (call via `/Script/UEMCPBridge.Default__UEMCPBridgeLibrary`)

```
// UMG
FString  CreateWidgetBlueprintWithRoot(FString Name, FString PackagePath, FString RootClass);
bool     SetWidgetRoot(FString WidgetBlueprintPath, FString RootClass);
FString  AddWidgetChild(FString WidgetBlueprintPath, FString ParentName, FString WidgetClass, FString WidgetName);
bool     SetWidgetSlot(FString WidgetBlueprintPath, FString WidgetName, FString SlotJson);

// Blueprint graph
bool     AddK2Node(FString BlueprintPath, FString GraphName, FString NodeSpecJson);   // returns node id in out
bool     ConnectPins(FString BlueprintPath, FString FromNodeId, FString FromPin, FString ToNodeId, FString ToPin);
bool     SetNodeDefault(FString BlueprintPath, FString NodeId, FString PinName, FString ValueJson);

// Generic escape hatch
bool     SetProtectedProperty(FString ObjectPath, FString PropertyName, FString ValueJson);
```

All take/return JSON-encoded strings so the wire contract stays simple and
matches the recipe harness's existing style.

### MCP-server integration

- New `Capability::InEngineBridge`, probed by calling a no-op
  `UEMCPBridgeLibrary.Ping()` — present ⇒ plugin installed; 404 ⇒ absent.
- Layer-2 tools gain a "prefer bridge if present" path: e.g.
  `ue_create_widget_blueprint` calls `CreateWidgetBlueprintWithRoot` when the
  bridge capability is set, otherwise falls back to today's multi-strategy
  Python recipe, otherwise `unsupported`. No tool names change.
- When the bridge is absent, behavior is exactly what it is today. The plugin is
  a pure enhancement, never a requirement.

## The tension with "for-all-versions"

The project's headline is a single ABI-free binary that drives 4.25 → 5.8. An
in-engine plugin breaks that for the plugin itself: UE plugins are compiled
per-engine-version (module API/BuildId). Mitigations:

- Ship the bridge as an **optional add-on**, not part of the core server. The
  core stays one binary; the bridge is a separate per-version artifact users opt
  into when they need UMG/graph authoring.
- Provide the plugin as **source** with a one-line build step per engine
  version, plus prebuilt binaries for the versions in `test_project_ue*`.
- Keep the UFUNCTION surface tiny and use only long-stable engine APIs
  (`UWidgetBlueprintFactory`, `FKismetEditorUtilities`, `UEdGraphSchema_K2`,
  `FBlueprintEditorUtils`) so the same source compiles across versions with
  minimal `#if ENGINE_MAJOR_VERSION` guards.

## Recommendation

Implement only if/when UMG-root or Blueprint-graph authoring proves to be a
recurring need that the Python recipes can't satisfy on the user's actual
builds. Until then, Layers 1–2 cover the majority of scene/model/material/asset
work, and the multi-strategy widget recipe handles UMG on non-stripped builds.

## If greenlit — implementation steps

1. Scaffold `UEMCPBridge.uplugin` + editor module `UEMCPBridge` + `UEMCPBridgeLibrary`.
2. Implement UMG functions first (highest value, fixes the login-screen case).
3. Add `Capability::InEngineBridge` probe + bridge-preferred paths in Layer-2 tools.
4. Per-version build scripts + prebuilt binaries for 5.3/5.5/5.7; document install.
5. Live selftest: bridge-backed `ue_create_widget_blueprint` builds a rooted WBP
   on a stripped 5.3 build where the Python path returns `unsupported`.
