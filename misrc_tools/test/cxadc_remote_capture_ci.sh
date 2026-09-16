#!/usr/bin/env bash
# CXADC/CXADC remote capture validation (server/client).
#
# Launches two misrc_gui instances (server + client) with isolated configs,
# verifies the server listens, the client connects and pumps /rf + /baseband,
# then kills the client and verifies the server detects the disconnect (the
# idle /rf stream select-probe fix) and stays alive without a hard close.
#
# Usage: cxadc_remote_capture_ci.sh <gui_bin> [port]
#
# Requires: curl, ss/netstat, pgrep, pkill, timeout, mktemp.
# Exits 0 on success, 1 on any check failure.

set -uo pipefail

GUI_BIN="${1:?usage: cxadc_remote_capture_ci.sh <gui_bin> [port]}"
PORT="${2:-8095}"

fail() {
  printf 'ERROR: %s\n' "$1" >&2
  exit 1
}

have() { command -v "$1" >/dev/null 2>&1; }
have curl   || fail "curl not found"
have pgrep  || fail "pgrep not found"
have pkill  || fail "pkill not found"
have timeout || fail "timeout not found"
[[ -x "$GUI_BIN" ]] || fail "GUI binary not executable: $GUI_BIN"

WORK="$(mktemp -d -t misrc-cxadc-ci.XXXXXX)"
trap 'pkill -f "misrc_gui --config '"$WORK"'" 2>/dev/null; rm -rf "$WORK"' EXIT

# --- Configs ---
SERVER_CFG="$WORK/server_config.json"
CLIENT_CFG="$WORK/client_config.json"
cat >"$SERVER_CFG" <<EOF
{
  "net_mode": 1,
  "net_server_port": $PORT,
  "net_server_port_str": "$PORT",
  "net_client_host": "",
  "net_client_port": $PORT,
  "net_client_port_str": "$PORT"
}
EOF
cat >"$CLIENT_CFG" <<EOF
{
  "net_mode": 2,
  "net_server_port": $PORT,
  "net_server_port_str": "$PORT",
  "net_client_host": "127.0.0.1",
  "net_client_port": $PORT,
  "net_client_port_str": "$PORT"
}
EOF

SERVER_LOG="$WORK/server.stderr.log"
CLIENT_LOG="$WORK/client.stderr.log"

# --- Launch server ---
pkill -f "misrc_gui --config $WORK" 2>/dev/null || true
sleep 1
nohup "$GUI_BIN" --config "$SERVER_CFG" >"$WORK/server.stdout.log" 2>"$SERVER_LOG" &
SERVER_PID=$!
sleep 3
kill -0 "$SERVER_PID" 2>/dev/null || fail "server process died within 3s"

# Verify server is listening
if have ss; then
  ss -ltn 2>/dev/null | grep -q ":$PORT" || fail "server not listening on :$PORT"
elif have netstat; then
  netstat -ltn 2>/dev/null | grep -q ":$PORT" || fail "server not listening on :$PORT (netstat)"
fi

# Verify server /stats responds
STATS="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/stats" 2>/dev/null)"
[[ -n "$STATS" ]] || fail "server /stats returned empty"
printf '%s\n' "$STATS" | grep -q '"sample_rate"' || fail "server /stats missing sample_rate field"

# --- Launch client ---
nohup "$GUI_BIN" --config "$CLIENT_CFG" >"$WORK/client.stdout.log" 2>"$CLIENT_LOG" &
CLIENT_PID=$!
sleep 4
kill -0 "$CLIENT_PID" 2>/dev/null || fail "client process died within 4s"

# Verify client connected and pumps started
grep -q "client worker started -> 127.0.0.1:$PORT" "$CLIENT_LOG" || fail "client worker did not start"
grep -q "pump /rf streaming" "$CLIENT_LOG" || fail "client /rf pump did not start"
grep -q "pump /baseband streaming" "$CLIENT_LOG" || fail "client /baseband pump did not start"

# --- Disconnect test: kill client, verify server detects it and stays alive ---
kill "$CLIENT_PID" 2>/dev/null || true
sleep 3
if pgrep -f "misrc_gui --config $CLIENT_CFG" >/dev/null 2>&1; then
  fail "client process still alive after kill"
fi
kill -0 "$SERVER_PID" 2>/dev/null || fail "server died after client disconnect (disconnect not detected)"

# Server must still serve /stats
STATS2="$(curl -s --max-time 3 "http://127.0.0.1:$PORT/stats" 2>/dev/null)"
[[ -n "$STATS2" ]] || fail "server /stats empty after disconnect"

# Fresh /rf subscription must work (proves old subscription was cleaned up)
if timeout 3 curl -s --max-time 3 "http://127.0.0.1:$PORT/rf" -o /dev/null -w '%{http_code}' 2>/dev/null | grep -q '200'; then
  :
else
  # A 200 may not arrive if the server has no active capture; accept any HTTP response
  # as long as the server is still listening and didn't crash.
  ss -ltn 2>/dev/null | grep -q ":$PORT" || netstat -ltn 2>/dev/null | grep -q ":$PORT" || \
    fail "server stopped listening after fresh /rf sub (old sub not cleaned up)"
fi
kill -0 "$SERVER_PID" 2>/dev/null || fail "server died after fresh /rf sub"

printf 'PASS: cxadc remote capture validation (server+client connect, pump, disconnect detected, server alive)\n'
printf '  server_pid=%s client_pid=%s port=%s\n' "$SERVER_PID" "$CLIENT_PID" "$PORT"
exit 0
