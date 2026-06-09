#!/usr/bin/env python3
"""ue-mcp-for-all-versions — long-lived-server driver (CLI usage simulation).

Starts ONE ue-mcp-for-all-versions.exe process and keeps it alive, exactly like
Claude Code / Codex would. Sends MCP requests over time and prints each
response's status. Used to prove lazy-connect and auto-reconnect without
restarting the server process.

Usage:
  drive_server.py <port> phase1            # engine assumed DOWN: expect errors
  drive_server.py <port> phase2            # engine assumed UP: expect success
  drive_server.py <port> full             # one process: down-call, wait, up-call

This script does NOT start/stop the engine; the caller does that around it.
For the 'full' mode it polls the port itself between phases.
"""
import json
import subprocess
import sys
import time
import socket
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "ue-mcp-for-all-versions.exe")


def port_open(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        return s.connect_ex(("127.0.0.1", port)) == 0
    finally:
        s.close()


def start_server(port):
    return subprocess.Popen(
        [EXE, "--port", str(port)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, bufsize=1, encoding="utf-8", errors="replace",
    )


def send(proc, obj):
    proc.stdin.write(json.dumps(obj) + "\n")
    proc.stdin.flush()


def read_resp(proc):
    line = proc.stdout.readline()
    if not line:
        return None
    return json.loads(line)


def summarize(rid, resp):
    if resp is None:
        print(f"  id={rid}: <no response>")
        return
    res = resp.get("result", {})
    if "content" in res:
        try:
            payload = json.loads(res["content"][0]["text"])
        except Exception:
            payload = {}
        status = payload.get("status")
        result = payload.get("result")
        rv = result.get("ReturnValue") if isinstance(result, dict) else None
        extra = ""
        if isinstance(rv, str):
            extra = f" value={rv}"
        elif isinstance(rv, list):
            extra = f" count={len(rv)}"
        print(f"  id={rid} isError={res.get('isError')} status={status}{extra}")
    elif "result" in resp:
        eng = res.get("_engine", {}).get("engineVersion")
        print(f"  id={rid} initialize engine={eng}")
    elif "error" in resp:
        print(f"  id={rid} ERROR {resp['error']['code']}: {resp['error']['message'][:60]}")


def main():
    port = int(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else "full"

    proc = start_server(port)
    try:
        # initialize
        send(proc, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
        summarize(1, read_resp(proc))

        down_now = not port_open(port)
        print(f"[phase1] engine reachable now: {not down_now}")
        send(proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                    "params": {"name": "ue_get_engine_version", "arguments": {}}})
        summarize(2, read_resp(proc))
        send(proc, {"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                    "params": {"name": "ue_search_assets", "arguments": {"query": "x"}}})
        summarize(3, read_resp(proc))

        if mode == "full":
            # Wait for the engine to come up (caller starts it in parallel).
            print("[wait] polling for engine to come up (late start)...")
            for _ in range(42):
                if port_open(port):
                    break
                time.sleep(10)
            time.sleep(3)

        print("[phase2] same long-lived server, after engine is up:")
        send(proc, {"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                    "params": {"name": "ue_get_engine_version", "arguments": {}}})
        summarize(4, read_resp(proc))
        send(proc, {"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                    "params": {"name": "ue_call_function",
                               "arguments": {"objectPath": "/Script/Engine.Default__KismetSystemLibrary",
                                             "functionName": "GetEngineVersion"}}})
        summarize(5, read_resp(proc))
        send(proc, {"jsonrpc": "2.0", "id": 6, "method": "tools/call",
                    "params": {"name": "ue_list_actors", "arguments": {}}})
        summarize(6, read_resp(proc))
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        proc.wait(timeout=10)


if __name__ == "__main__":
    main()
