#!/usr/bin/env bash
# ue-mcp-for-all-versions — lazy-connect / late-start integration test.
#
# Simulates the CLI usage pattern (Claude Code / Codex): the MCP server is a
# long-lived process that is started BEFORE the engine. It must:
#   1) report a connection error while the engine is down, then
#   2) self-heal and serve tools once the engine comes up,
# all WITHOUT restarting the server process.
#
# We drive a single long-lived server via a FIFO on its stdin so we can inject
# requests over time while the engine starts in parallel.
#
# Usage: scripts/late_start_test.sh <ver> <assoc> <editor_exe> <port>
set -u
ROOT="E:/uug_mcp/ue-mcp-for-all-versions"
VER="$1"; ASSOC="$2"; EXE_NAME="$3"; PORT="$4"
EPIC="/c/Program Files/Epic Games/UE_${ASSOC}/Engine/Binaries/Win64/${EXE_NAME}.exe"
PROJ="${ROOT}/test_project_ue${VER}/RcTest${VER}.uproject"
MCP="${ROOT}/build/Release/ue-mcp-for-all-versions.exe"
LOG="${ROOT}/build/ue_editor_${VER}_late.log"
OUT="${ROOT}/build/late_out_${VER}.jsonl"
FIFO="${ROOT}/build/late_in_${VER}.fifo"

echo "### UE ${ASSOC} late-start / reconnect test (port ${PORT}) ###"
[ -f "$EPIC" ] || { echo "MISSING editor: $EPIC"; exit 2; }
rm -f "$OUT" "$FIFO"; mkfifo "$FIFO"

# Start the long-lived MCP server reading from the FIFO. Keep the FIFO open for
# writing via fd 3 so the server doesn't see EOF.
"$MCP" --port "$PORT" < "$FIFO" > "$OUT" 2>/dev/null &
MCPPID=$!
exec 3>"$FIFO"

send(){ printf '%s\n' "$1" >&3; }

# Phase 1: engine DOWN. initialize + a tool call -> expect connection error.
send '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
send '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"ue_get_engine_version","arguments":{}}}'
sleep 2

# Phase 2: start the engine in the background.
echo "starting engine (this is the late start)..."
"$EPIC" "$PROJ" -ExecCmds="WebControl.EnableServerOnStartup 1,WebControl.StartServer" -nosplash > "$LOG" 2>&1 &
EDPID=$!

# Wait (out-of-band probe with a SEPARATE short-lived process) for RC up.
UP=0
for i in $(seq 1 42); do
  if "$MCP" --port "$PORT" --probe >/dev/null 2>&1; then UP=1; break; fi
  sleep 10
done
echo "engine RC up=$UP after ~$((i*10))s"

# Phase 3: same long-lived server, call again -> expect self-heal + success.
send '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"ue_get_engine_version","arguments":{}}}'
send '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"ue_call_function","arguments":{"objectPath":"/Script/Engine.Default__KismetSystemLibrary","functionName":"GetEngineVersion"}}}'
send '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"ue_list_actors","arguments":{}}}'
sleep 3

# Close stdin -> server exits cleanly.
exec 3>&-
wait $MCPPID 2>/dev/null

echo "--- server responses (id -> status) ---"
python -c "
import sys,json
for l in open(r'$OUT',encoding='utf-8'):
    l=l.strip()
    if not l: continue
    m=json.loads(l); rid=m.get('id'); res=m.get('result',{})
    if 'content' in res:
        try: payload=json.loads(res['content'][0]['text'])
        except Exception: payload={}
        st=payload.get('status'); rv=payload.get('result',{})
        rv=(rv.get('ReturnValue') if isinstance(rv,dict) else None)
        n=len(rv) if isinstance(rv,list) else ''
        print(f'  id={rid} isError={res.get(\"isError\")} status={st} '+(f'ver={rv}' if rv else (f'count={len(payload.get(\"result\",{}).get(\"ReturnValue\",[]))}' if False else '')))
    elif 'result' in m:
        print(f'  id={rid} initialize engine='+str(res.get('_engine',{}).get('engineVersion')))
"

echo "--- shutting down editor ---"
powershell -NoProfile -Command "Get-Process ${EXE_NAME} -ErrorAction SilentlyContinue | Stop-Process -Force" 2>/dev/null
rm -f "$FIFO"
echo "### UE ${ASSOC} late-start test done ###"
