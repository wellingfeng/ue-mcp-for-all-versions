#!/usr/bin/env python3
"""ue-mcp-for-all-versions — live self-test for the Layer-1/Layer-2 tools.

Drives ONE long-lived MCP server against a running editor and exercises the new
creation/authoring tools end to end:

  create folder -> create material -> create blueprint -> create widget
  blueprint (multi-strategy root) -> add a button + text to it -> compile.

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
    for k in ("assetPath", "rootSet", "addedToRoot", "variable", "compiled",
              "missingCapability", "reason"):
        if k in payload:
            line += f" {k}={payload[k]}"
    print(line)
    if "strategiesTried" in payload:
        for s in payload["strategiesTried"]:
            print(f"    strategy: {s}")
    if status == "error" and "error" in payload:
        print(f"    error: {payload['error']}")
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

        # 4) Widget Blueprint with a root (the login-screen scenario).
        isErr, p = call(proc, "ue_create_widget_blueprint",
                        {"name": "WBP_SelfTest", "packagePath": base,
                         "rootType": "CanvasPanel"})
        show("create_widget_blueprint", isErr, p)
        wbp_path = p.get("assetPath")
        root_ok = p.get("rootSet") or p.get("status") == "ok"

        # 5) Add widgets only if the root exists.
        if wbp_path and root_ok:
            isErr, p = call(proc, "ue_add_widget_to_blueprint",
                            {"blueprintPath": wbp_path, "widgetType": "TextBlock",
                             "widgetName": "TitleText", "text": "Login"})
            show("add_widget(TextBlock)", isErr, p)

            isErr, p = call(proc, "ue_add_widget_to_blueprint",
                            {"blueprintPath": wbp_path, "widgetType": "Button",
                             "widgetName": "LoginButton"})
            show("add_widget(Button)", isErr, p)
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
