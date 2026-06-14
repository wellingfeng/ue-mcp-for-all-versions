#!/usr/bin/env python3
"""ue-mcp-for-all-versions — live self-test for the Layer-1/Layer-2 tools.

Drives ONE long-lived MCP server against a running editor and exercises the new
creation/authoring tools end to end:

  create folder -> create material -> create blueprint -> create Character
  blueprint -> configure movement / collision / camera -> create Enhanced Input
  assets -> create widget blueprint (multi-strategy root) -> add a button +
  text to it -> compile.

Optional environment variables:
  UEMCP_SELFTEST_SKELETON=/Game/.../SK_Mannequin_Skeleton
      Also creates Animation Blueprint + speed BlendSpace assets.
  UEMCP_SELFTEST_GAME_MAP=/Game/Maps/MyMap
  UEMCP_SELFTEST_GAME_MODE=/Game/.../BP_GameMode
      Also persists GameMapsSettings.
  UEMCP_SELFTEST_RUN_PIE=1
      Also runs the third-person PIE smoke test.

Every step reports status; the widget-blueprint step prints strategiesTried so a
stripped-Python build's behavior is visible rather than hidden.

Usage: selftest_authoring.py <port>   (editor must already be up on that port)
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "ue-mcp-for-all-versions.exe")

_id = [0]


def nid():
    _id[0] += 1
    return _id[0]


def call(proc, name, args):
    rid = nid()
    proc.stdin.write(json.dumps({
        "jsonrpc": "2.0", "id": rid, "method": "tools/call",
        "params": {"name": name, "arguments": args},
    }) + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    resp = json.loads(line)
    res = resp.get("result", {})
    payload = {}
    for block in res.get("content", []):
        if block.get("type") == "text":
            try:
                payload = json.loads(block["text"])
            except Exception:
                payload = {"_raw": block["text"]}
    return res.get("isError"), payload


def show(label, isErr, payload):
    status = payload.get("status")
    line = f"[{label}] isError={isErr} status={status}"
    for k in ("assetPath", "assetPaths", "rootSet", "addedToRoot", "variable",
              "compiled", "cameraAdded", "targetHeightCm", "missingCapability",
              "reason"):
        if k in payload:
            line += f" {k}={payload[k]}"
    print(line)
    if "strategiesTried" in payload:
        for s in payload["strategiesTried"]:
            print(f"    strategy: {s}")
    if status == "error" and "error" in payload:
        print(f"    error: {payload['error']}")
    if "diagnostics" in payload:
        print(f"    diagnostics: {json.dumps(payload['diagnostics'], ensure_ascii=False)}")
    return payload


def main():
    port = int(sys.argv[1])
    proc = subprocess.Popen(
        [EXE, "--port", str(port)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, bufsize=1, encoding="utf-8", errors="replace",
    )
    try:
        proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid(),
                                     "method": "initialize", "params": {}}) + "\n")
        proc.stdin.flush()
        init = json.loads(proc.stdout.readline())
        eng = init.get("result", {}).get("_engine", {})
        print(f"engine={eng.get('engineVersion')} caps={eng.get('capabilities')}")

        base = "/Game/UEMCPSelfTest"

        # 1) Folder.
        isErr, p = call(proc, "ue_create_folder", {"path": base})
        show("create_folder", isErr, p)

        # 2) Material with a base color.
        isErr, p = call(proc, "ue_create_material",
                        {"name": "M_SelfTest", "packagePath": base,
                         "baseColor": {"r": 0.2, "g": 0.6, "b": 0.9}})
        show("create_material", isErr, p)

        # 3) Blueprint (Actor-derived).
        isErr, p = call(proc, "ue_create_blueprint",
                        {"name": "BP_SelfTest", "packagePath": base,
                         "parentClass": "/Script/Engine.Actor"})
        show("create_blueprint", isErr, p)

        # 4) Gameplay Character Blueprint with standard movement defaults.
        isErr, p = call(proc, "ue_create_character_blueprint",
                        {"name": "BP_SelfTestCharacter", "packagePath": base,
                         "reuseExisting": True})
        show("create_character_blueprint", isErr, p)
        character_path = p.get("assetPath")

        if character_path:
            isErr, p = call(proc, "ue_configure_character_movement",
                            {"blueprintPath": character_path})
            show("configure_character_movement", isErr, p)

            isErr, p = call(proc, "ue_calibrate_character_collision",
                            {"blueprintPath": character_path,
                             "targetHeightCm": 180.0})
            show("calibrate_character_collision", isErr, p)

            isErr, p = call(proc, "ue_configure_third_person_camera",
                            {"blueprintPath": character_path,
                             "targetArmLength": 350.0,
                             "socketOffset": {"x": 0.0, "y": 45.0, "z": 65.0}})
            show("configure_third_person_camera", isErr, p)

        isErr, p = call(proc, "ue_create_enhanced_input_assets",
                        {"packagePath": base, "reuseExisting": True})
        show("create_enhanced_input_assets", isErr, p)

        skeleton = os.environ.get("UEMCP_SELFTEST_SKELETON", "")
        if skeleton:
            isErr, p = call(proc, "ue_create_locomotion_animation_assets",
                            {"packagePath": base, "namePrefix": "SelfTest",
                             "skeletonPath": skeleton, "reuseExisting": True})
            show("create_locomotion_animation_assets", isErr, p)
        else:
            print("    [skip] create_locomotion_animation_assets — set UEMCP_SELFTEST_SKELETON")

        game_map = os.environ.get("UEMCP_SELFTEST_GAME_MAP", "")
        game_mode = os.environ.get("UEMCP_SELFTEST_GAME_MODE", "")
        if game_map or game_mode:
            isErr, p = call(proc, "ue_set_game_defaults",
                            {"gameDefaultMap": game_map,
                             "editorStartupMap": game_map,
                             "gameModePath": game_mode})
            show("set_game_defaults", isErr, p)
        else:
            print("    [skip] set_game_defaults — set UEMCP_SELFTEST_GAME_MAP or UEMCP_SELFTEST_GAME_MODE")

        if os.environ.get("UEMCP_SELFTEST_RUN_PIE") == "1":
            isErr, p = call(proc, "ue_validate_third_person_pie",
                            {"expectedPawnClassPath": character_path or "",
                             "startPie": True, "stopPie": True})
            show("validate_third_person_pie", isErr, p)
        else:
            print("    [skip] validate_third_person_pie — set UEMCP_SELFTEST_RUN_PIE=1")

        # 5) Widget Blueprint with a root (the login-screen scenario).
        isErr, p = call(proc, "ue_create_widget_blueprint",
                        {"name": "WBP_SelfTest", "packagePath": base,
                         "rootType": "CanvasPanel"})
        show("create_widget_blueprint", isErr, p)
        wbp_path = p.get("assetPath")
        root_ok = p.get("rootSet") or p.get("status") == "ok"

        # 6) Add widgets only if the root exists.
        if wbp_path and root_ok:
            isErr, p = call(proc, "ue_inspect_widget_blueprint",
                            {"blueprintPath": wbp_path})
            show("inspect_widget(initial)", isErr, p)

            isErr, p = call(proc, "ue_add_widget_to_panel",
                            {"blueprintPath": wbp_path,
                             "widgetType": "TextBlock",
                             "widgetName": "TitleText",
                             "properties": {
                                 "text": "Login",
                                 "fontSize": 32,
                                 "color": {"r": 1.0, "g": 1.0, "b": 1.0, "a": 1.0},
                             },
                             "layout": {
                                 "position": {"x": 64, "y": 64},
                                 "size": {"x": 360, "y": 64},
                                 "anchors": {"minX": 0, "minY": 0, "maxX": 0, "maxY": 0},
                             }})
            show("add_widget_to_panel(TextBlock)", isErr, p)

            isErr, p = call(proc, "ue_add_widget_to_panel",
                            {"blueprintPath": wbp_path,
                             "widgetType": "Button",
                             "widgetName": "LoginButton",
                             "layout": {
                                 "position": {"x": 64, "y": 144},
                                 "size": {"x": 220, "y": 56},
                             }})
            show("add_widget_to_panel(Button)", isErr, p)

            isErr, p = call(proc, "ue_set_widget_properties",
                            {"blueprintPath": wbp_path,
                             "widgetName": "LoginButton",
                             "properties": {
                                 "tooltipText": "Submit login",
                                 "renderOpacity": 0.95,
                             }})
            show("set_widget_properties(Button)", isErr, p)

            isErr, p = call(proc, "ue_configure_widget_layout",
                            {"blueprintPath": wbp_path,
                             "widgetName": "LoginButton",
                             "layout": {
                                 "position": {"x": 64, "y": 144},
                                 "size": {"x": 240, "y": 56},
                                 "zOrder": 1,
                             }})
            show("configure_widget_layout(Button)", isErr, p)

            isErr, p = call(proc, "ue_inspect_widget_blueprint",
                            {"blueprintPath": wbp_path})
            show("inspect_widget(final)", isErr, p)

            if os.environ.get("UEMCP_SELFTEST_WIDGET_COMPONENT") == "1":
                isErr, p = call(proc, "ue_create_widget_component_blueprint",
                                {"name": "BP_SelfTestWidgetActor",
                                 "packagePath": base,
                                 "widgetBlueprintPath": wbp_path,
                                 "space": "World",
                                 "drawSize": {"x": 500, "y": 300},
                                 "reuseExisting": True})
                show("create_widget_component_blueprint", isErr, p)
            else:
                print("    [skip] create_widget_component_blueprint — set UEMCP_SELFTEST_WIDGET_COMPONENT=1")
        else:
            print("    [skip] add_widget — no root widget was set on this build")

        print("\nNOTE: if create_widget_blueprint reports status=unsupported, the "
              "build's Python cannot author a UMG root (expected on stripped "
              "builds); the Layer-3 in-engine plugin is required there.")

    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
