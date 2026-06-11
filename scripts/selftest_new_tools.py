#!/usr/bin/env python3
"""ue-mcp-for-all-versions — live self-test for the iteration-4 tools.

Drives ONE long-lived MCP server against a running editor and exercises the new
scene-introspection / material / visual / workflow tools end to end:

  spawn a cube -> find it by class -> read its components -> set a material
  color on the mesh component -> read bounds -> focus viewport -> thumbnail.

Usage: selftest_new_tools.py <port>   (editor must already be up on that port)
"""
import base64
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
    content = res.get("content", [])
    payload = {}
    image = None
    for block in content:
        if block.get("type") == "text":
            try:
                payload = json.loads(block["text"])
            except Exception:
                payload = {"_raw": block["text"]}
        elif block.get("type") == "image":
            image = block
    return res.get("isError"), payload, image


def show(label, isErr, payload, image=None):
    status = payload.get("status")
    line = f"[{label}] isError={isErr} status={status}"
    for k in ("count", "missingCapability", "materialInstance", "framedActor", "bytes", "mimeType"):
        if k in payload:
            line += f" {k}={payload[k]}"
    if "result" in payload and isinstance(payload["result"], dict):
        rv = payload["result"].get("ReturnValue")
        if isinstance(rv, (str, int, float, bool)):
            line += f" rv={rv}"
    if image:
        line += f" IMG={len(image.get('data',''))}b64 mime={image.get('mimeType')}"
    print(line)
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

        # 1) Spawn a StaticMeshActor from the engine cube asset.
        cube = "/Engine/BasicShapes/Cube.Cube"
        isErr, p, _ = call(proc, "ue_spawn_actor_from_asset",
                           {"assetPath": cube, "location": {"x": 0, "y": 0, "z": 200}})
        show("spawn_from_asset", isErr, p)
        spawned = None
        rv = p.get("result", {})
        if isinstance(rv, dict):
            spawned = rv.get("ReturnValue")
        print(f"   spawned actor path = {spawned}")

        # 2) Find actors by class (StaticMeshActor) — should include our cube.
        isErr, p, _ = call(proc, "ue_find_actors_by_class",
                           {"actorClass": "/Script/Engine.StaticMeshActor"})
        show("find_by_class StaticMeshActor", isErr, p)
        actors = p.get("actors", [])
        target = spawned if spawned in actors else (actors[0] if actors else spawned)
        print(f"   target actor = {target}")

        # 3) Components of the target actor (mesh component).
        comp = None
        if target:
            isErr, p, _ = call(proc, "ue_get_actor_components",
                               {"actorPath": target,
                                "componentClass": "/Script/Engine.StaticMeshComponent"})
            show("get_components(StaticMesh)", isErr, p)
            comps = p.get("components", [])
            comp = comps[0] if comps else None
            print(f"   mesh component = {comp}")

        # 4) Bounds.
        if target:
            isErr, p, _ = call(proc, "ue_get_actor_bounds", {"actorPath": target})
            show("get_actor_bounds", isErr, p)

        # 5) Set a material color (vector param) on the mesh component.
        #    Engine cube uses "BasicShapeMaterial"; param name "Color" is typical
        #    but may differ — we report the result either way.
        if comp:
            isErr, p, _ = call(proc, "ue_set_material_param",
                               {"componentPath": comp, "elementIndex": 0,
                                "paramName": "Color", "paramType": "vector",
                                "color": {"r": 0.1, "g": 0.8, "b": 0.2, "a": 1.0}})
            show("set_material_param(Color->green)", isErr, p)

        # 6) Find by label (the spawned actor's default label).
        isErr, p, _ = call(proc, "ue_find_actors_by_label",
                           {"label": "Cube", "match": "Contains"})
        show("find_by_label 'Cube'", isErr, p)

        # 7) Focus viewport on the actor.
        if target:
            isErr, p, _ = call(proc, "ue_focus_viewport_on_actor",
                               {"actorPath": target, "distance": 3.0})
            show("focus_viewport_on_actor", isErr, p)

        # 8) Console variable read.
        isErr, p, _ = call(proc, "ue_get_console_variable",
                           {"name": "r.ScreenPercentage", "type": "float"})
        show("get_console_variable r.ScreenPercentage", isErr, p)

        # 9) Thumbnail of the cube asset (binary -> base64 image block).
        isErr, p, img = call(proc, "ue_get_object_thumbnail", {"objectPath": cube})
        show("get_object_thumbnail(Cube)", isErr, p, img)
        if img and img.get("data"):
            raw = base64.b64decode(img["data"])
            mime = img.get("mimeType", "image/png")
            ext = "jpg" if "jpeg" in mime or "jpg" in mime else "png"
            out = os.path.join(ROOT, "build", f"selftest_thumbnail.{ext}")
            with open(out, "wb") as f:
                f.write(raw)
            is_png = raw[:4] == b"\x89PNG"
            is_jpg = raw[:2] == b"\xff\xd8"
            print(f"   wrote {out} ({len(raw)} bytes) valid_image={is_png or is_jpg} ({'png' if is_png else 'jpeg' if is_jpg else '?'})")

        # 10) Material instance asset param — expected to fail gracefully if the
        #     path isn't a MIC (we just confirm clean error handling).
        isErr, p, _ = call(proc, "ue_set_material_instance_param",
                           {"instancePath": "/Engine/BasicShapes/BasicShapeMaterial",
                            "paramName": "Color", "paramType": "vector",
                            "color": {"r": 1, "g": 0, "b": 0, "a": 1}})
        show("set_material_instance_param(non-MIC, expect clean err)", isErr, p)

    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
