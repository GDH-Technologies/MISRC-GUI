#!/usr/bin/env bash
# End-to-end check of the net settings and record-state parity, with no
# window and no capture hardware: a --net-serve server on the Simulated
# device, driven with curl, then --net-client-probe as the client.
#
# Usage: misrc_tools/test/net_settings_e2e.sh <misrc_gui> [port] [scratch-dir]
# Run from the repository root (it reads the settings table source to know
# which keys /settings must carry). Needs curl, jq, python3.
# Exit 0 when every check passed; the checks print PASS/FAIL lines.
set -uo pipefail

BIN=${1:?usage: net_settings_e2e.sh <misrc_gui> [port] [scratch-dir]}
PORT=${2:-18080}
S=${3:-$(mktemp -d)}
TABLE="misrc_tools/misrc_gui/core/gui_settings_table.c"
[ -f "$TABLE" ] || { echo "run from the repository root ($TABLE not found)" >&2; exit 2; }
for tool in curl jq python3; do
  command -v "$tool" >/dev/null || { echo "$tool is required" >&2; exit 2; }
done
if ss -ltn 2>/dev/null | grep -q ":$PORT "; then
  echo "port $PORT is busy; pass another as the second argument" >&2
  exit 2
fi

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; FAILS=$((FAILS + 1)); }
url() { echo "http://127.0.0.1:$PORT$1"; }
code_of() { curl -s -o "$S/last.json" -w '%{http_code}' "$(url "$1")"; }
enc() { python3 -c 'import urllib.parse, sys; print(urllib.parse.quote(sys.argv[1], safe=""))' "$1"; }

mkdir -p "$S/out"
cat > "$S/server.json" <<EOF
{
  "net_mode": 1,
  "net_server_port": $PORT,
  "net_server_port_str": "$PORT",
  "output_path": "$S/out",
  "output_base_name": "e2e",
  "auto_names_enabled": true,
  "overwrite_files": true,
  "use_flac": true,
  "flac_level": 4,
  "capture_a": true,
  "capture_b": true,
  "video_record_enabled": false,
  "rtsp_stream_enabled": false,
  "audio_monitor_playback": false
}
EOF

"$BIN" --config "$S/server.json" --net-serve 300 > "$S/server.log" 2>&1 &
SRV=$!
cleanup() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }
trap cleanup EXIT

for _ in $(seq 1 50); do curl -sf "$(url /version)" >/dev/null && break; sleep 0.2; done
if ! curl -sf "$(url /version)" >/dev/null; then
  fail "the server did not come up"; cat "$S/server.log" >&2; exit 1
fi
pass "--net-serve is listening on :$PORT ($(curl -s "$(url /version)" | tr -d '\r\n'))"

# --- /settings ------------------------------------------------------------
SJ=$(curl -s "$(url /settings)")
if jq -e '.generation >= 1 and .settings.output_path != null and .settings.net_mode == null
          and .settings.ui_scale_percent == null and .settings.aux_filename == null
          and (.misrc_mode_effective | type) == "boolean"' <<<"$SJ" >/dev/null; then
  pass "/settings carries the generation, the effective mode and server-owned keys only"
else
  fail "/settings shape: $SJ"
fi
if python3 - "$TABLE" "$SJ" <<'PY'
import json, re, sys
src = open(sys.argv[1]).read()
want = set()
for m in re.finditer(r'GS_[A-Z0-9]+\s*\(\s*"([a-z0-9_]+)"\s*,\s*[A-Za-z_][A-Za-z0-9_]*(.*?)\)\s*,?\s*$', src, re.M):
    key, rest = m.group(1), m.group(2)
    if any(f in rest for f in ("GS_LOCAL", "GS_CLIENT_LOCAL", "GS_WRITE_ONLY", "GS_LOAD_ONLY")):
        continue
    want.add(key)
got = set(json.loads(sys.argv[2])["settings"].keys())
# a build without DdD support has no ddd_decimation row compiled in
if "ddd_decimation" not in got:
    want.discard("ddd_decimation")
missing, extra = sorted(want - got), sorted(got - want)
if missing or extra:
    print("missing:", missing, "extra:", extra)
    sys.exit(1)
PY
then pass "/settings key set equals the table's server-owned keys"; else fail "/settings key set differs from the table"; fi
G0=$(jq .generation <<<"$SJ")

# --- /set -----------------------------------------------------------------
c=$(code_of "/set?key=flac_level&value=6")
[ "$c" = 200 ] && pass "/set flac_level=6 -> 200 ($(cat "$S/last.json"))" || fail "/set flac_level=6 -> $c $(cat "$S/last.json")"
sleep 0.5
jq -e '.flac_level == 6' "$S/server.json" >/dev/null && pass "the --config file was rewritten with flac_level 6" || fail "the --config file does not show flac_level 6"
curl -s "$(url /settings)" | jq -e ".settings.flac_level == 6 and .generation > $G0" >/dev/null \
  && pass "/settings shows flac_level 6 with a higher generation" || fail "/settings did not pick up flac_level 6"

mkdir -p "$S/out x"
c=$(code_of "/set?key=output_path&value=$(enc "$S/out x")")
[ "$c" = 200 ] || fail "/set output_path (with a space) -> $c $(cat "$S/last.json")"
sleep 0.3
[ "$(curl -s "$(url /settings)" | jq -r .settings.output_path)" = "$S/out x" ] \
  && pass "output_path round-trips percent-decoded, space included" || fail "output_path did not round-trip"
c=$(code_of "/set?key=output_path&value=$(enc "$S/out")")
[ "$c" = 200 ] || fail "/set output_path (restore) -> $c"

for spec in 'key=flac_level&value=9:422' 'key=nope&value=1:400' 'key=net_server_port&value=1:400' \
            'key=ui_scale_percent&value=150:400' 'key=aux_filename&value=x:400' \
            'key=output_path&value=%2Fno%2Fsuch%2Fdir:422' 'key=rf_bits_a&value=10:422' \
            'key=capture_a&value=yes:422' 'key=output_base_name&value=bad%22quote:422' \
            'key=flac_level&value=%G1:400' 'value=1:400'; do
  q=${spec%:*}; want=${spec##*:}
  c=$(code_of "/set?$q")
  [ "$c" = "$want" ] && pass "/set?$q -> $want" || fail "/set?$q -> $c (want $want): $(cat "$S/last.json")"
done

# --- capture + record on the Simulated device ---------------------------
IDX=$(curl -s "$(url /devices)" | jq '[.devices[] | select(.name | test("Simulated"))][0].index')
if [ -z "$IDX" ] || [ "$IDX" = "null" ]; then
  fail "no Simulated device in /devices: $(curl -s "$(url /devices)")"
else
  curl -s "$(url "/device?$IDX")" >/dev/null
  curl -s "$(url /start)" >/dev/null
  for _ in $(seq 1 50); do [ "$(curl -s "$(url /stats)" | jq .state)" = 1 ] && break; sleep 0.2; done
  [ "$(curl -s "$(url /stats)" | jq .state)" = 1 ] && pass "capture started on the Simulated device (state 1)" \
    || fail "capture did not start: $(curl -s "$(url /stats)")"
  curl -s "$(url "/record?on=1")" >/dev/null
  for _ in $(seq 1 50); do [ "$(curl -s "$(url /stats)" | jq .state)" = 2 ] && break; sleep 0.2; done
  ST=$(curl -s "$(url /stats)")
  [ "$(jq .state <<<"$ST")" = 2 ] && pass "recording started (state 2)" || fail "recording did not start: $ST"
  B1=$(jq .rec_raw_a <<<"$ST")
  sleep 1.5
  ST2=$(curl -s "$(url /stats)")
  if jq -e --argjson b1 "$B1" '.state == 2 and .rec_raw_a > $b1 and .rec_drops == 0 and .disk_free > 0
                               and .rec_pending == false and (.status | length) > 0 and .rec_elapsed_ms >= 0
                               and .generation >= 1' <<<"$ST2" >/dev/null; then
    pass "/stats relays a growing recording (raw_a $B1 -> $(jq .rec_raw_a <<<"$ST2"), status '$(jq -r .status <<<"$ST2")')"
  else
    fail "/stats relay: $ST2"
  fi
  c=$(code_of "/set?key=flac_level&value=4")
  [ "$c" = 409 ] && pass "/set while recording -> 409" || fail "/set while recording -> $c $(cat "$S/last.json")"

  PROBE=$("$BIN" --net-client-probe 127.0.0.1 "$PORT" 4 2>"$S/probe.log"); PRC=$?
  [ "$PRC" = 0 ] && pass "--net-client-probe exits 0" || fail "--net-client-probe exited $PRC: $PROBE $(tail -3 "$S/probe.log")"
  if jq -e '.connected == true and .settings_support == 1 and .settings.flac_level == 6 and .peer_state == 2
            and .local_settings_touched == false and .rec_bytes > 0' <<<"$PROBE" >/dev/null; then
    pass "the probe mirrors flac_level 6, the recording state and its bytes, and never touched its own settings"
  else
    fail "probe output: $PROBE"
  fi

  curl -s "$(url "/record?on=0")" >/dev/null
  for _ in $(seq 1 150); do
    ST=$(curl -s "$(url /stats)")
    [ "$(jq .state <<<"$ST")" = 1 ] && [ "$(jq .rec_finalizing <<<"$ST")" = false ] && break
    sleep 0.2
  done
  [ "$(jq .state <<<"$ST")" = 1 ] && pass "recording stopped and finalized (state 1)" || fail "recording did not stop: $ST"
  curl -s "$(url /stop)" >/dev/null
  ls "$S"/out/*.flac >/dev/null 2>&1 && pass "FLAC output written: $(ls "$S"/out/*.flac | xargs -n1 basename | tr '\n' ' ')" \
    || fail "no FLAC output in $S/out"
  c=$(code_of "/set?key=flac_level&value=4")
  [ "$c" = 200 ] && pass "/set after the recording -> 200 again" || fail "/set after the recording -> $c"
fi

kill "$SRV"; wait "$SRV"; SRC=$?
trap - EXIT
[ "$SRC" = 0 ] && pass "--net-serve shut down cleanly on SIGTERM (rc 0)" || fail "--net-serve exited $SRC"
echo "server log: $(grep -c '\[NET\]' "$S/server.log") [NET] lines in $S/server.log"
echo "$FAILS failure(s)"
[ "$FAILS" = 0 ]
