#!/usr/bin/env bash
# ue-mcp-for-all-versions — cross-version integration self-test.
#
# Launches a given UE version's editor on a minimal RemoteControl-enabled
# project, waits for the RC web server, then drives the MCP server through a
# scripted JSON-RPC session and prints per-tool results.
#
# Usage: scripts/integration_test.sh <ver> <assoc> <editor_exe> <rc_port>
#   e.g. scripts/integration_test.sh 425 4.25 UE4Editor 8080
#        scripts/integration_test.sh 55  5.5  UnrealEditor 30010
set -u
ROOT="E:/uug_mcp/ue-mcp-for-all-versions"
VER="$1"; ASSOC="$2"; EXE_NAME="$3"; PORT="$4"
EPIC="/c/Program Files/Epic Games/UE_${ASSOC}/Engine/Binaries/Win64/${EXE_NAME}.exe"
PROJ="${ROOT}/test_project_ue${VER}/RcTest${VER}.uproject"
MCP="${ROOT}/build/Release/ue-mcp-for-all-versions.exe"
LOG="${ROOT}/build/ue_editor_${VER}.log"

echo "### UE ${ASSOC} integration test (port ${PORT}) ###"
[ -f "$EPIC" ] || { echo "MISSING editor: $EPIC"; exit 2; }
[ -f "$PROJ" ] || { echo "MISSING project: $PROJ"; exit 2; }

# UE -ExecCmds splits on COMMA (not semicolon). StartServer is what actually
# binds the port; the cvar matters only for the next launch on 4.25.
"$EPIC" "$PROJ" -ExecCmds="WebControl.EnableServerOnStartup 1,WebControl.StartServer,WebControl.StartWebSocketServer" -nosplash > "$LOG" 2>&1 &
EDPID=$!
echo "launched editor pid=$EDPID; waiting for RC on :${PORT} ..."

UP=0
for i in $(seq 1 42); do  # up to 7 min for cold first-open
  if "$MCP" --port "$PORT" --probe >/tmp/probe_${VER}.json 2>/dev/null; then
    echo "RC UP after ~$((i*10))s"; UP=1; break
  fi
  sleep 10
done

if [ "$UP" != "1" ]; then
  echo "RC DID NOT COME UP. editor log tail:"; tail -20 "$LOG"
  powershell -NoProfile -Command "Get-Process ${EXE_NAME} -ErrorAction SilentlyContinue | Stop-Process -Force" 2>/dev/null
  exit 1
fi

echo "--- probe ---"; cat /tmp/probe_${VER}.json

echo "--- MCP tools/call session ---"
printf '%s\n' \
'{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"ue_get_engine_version","arguments":{}}}' \
'{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"ue_call_function","arguments":{"objectPath":"/Script/Engine.Default__KismetSystemLibrary","functionName":"GetEngineVersion"}}}' \
'{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"ue_search_assets","arguments":{"query":"","limit":3}}}' \
'{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"ue_remote_info","arguments":{}}}' \
'{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"ue_list_actors","arguments":{}}}' \
'{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"ue_list_assets","arguments":{"directoryPath":"/Game","recursive":true}}}' \
| "$MCP" --port "$PORT" 2>/dev/null \
| python -c "
import sys,json
for l in sys.stdin:
    if not l.strip(): continue
    m=json.loads(l); rid=m.get('id'); res=m.get('result',{})
    if 'content' in res:
        p=json.loads(res['content'][0]['text']); st=p.get('status'); r=p.get('result',{})
        rv=r.get('ReturnValue') if isinstance(r,dict) else None
        extra=(f'count={len(rv)}' if isinstance(rv,list) else (f'val={rv}' if isinstance(rv,str) else ''))
        mc=p.get('missingCapability','')
        print(f'[id={rid} isError={res.get(\"isError\")}] status={st} {extra}{mc}')
    else:
        print(f'[id={rid} initialize] engine='+str(res.get('_engine',{}).get('engineVersion')))
"

echo "--- shutting down editor ---"
powershell -NoProfile -Command "Get-Process ${EXE_NAME} -ErrorAction SilentlyContinue | Stop-Process -Force" 2>/dev/null
echo "### UE ${ASSOC} test done ###"
