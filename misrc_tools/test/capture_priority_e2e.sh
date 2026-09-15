#!/usr/bin/env bash
# Runtime check of the capture-first scheduling posture, in the real binary.
# A --net-serve server captures (and records FLAC) on a device, gdb maps each
# thread id to its entry function, and each role's class / rtprio / nice /
# ioprio is compared against literals derived from this shell's limits. A
# second phase replays the recording through the Playback device.
#
# usage: misrc_tools/test/capture_priority_e2e.sh <misrc_gui> [port] [scratch-dir]
#   DEVICE_RE       anchored regex for the capture device's /devices name
#                   (default '^\[Simulated\]'); anything else is real hardware
#   ALLOW_HARDWARE  set to 1 to let it capture from a real device (refused otherwise)
# Manual, not CI: Linux only, needs gdb, curl, jq, ionice and python3, and gdb
# must be allowed to attach to a sibling process (kernel.yama.ptrace_scope 0).
set -uo pipefail

BIN=${1:?usage: capture_priority_check.sh <misrc_gui> [port] [scratch-dir]}
PORT=${2:-18090}
S=${3:-$(mktemp -d)}
DEVICE_RE=${DEVICE_RE:-'^\[Simulated\]'}
for tool in curl jq gdb ps ionice python3; do
  command -v "$tool" >/dev/null || { echo "$tool is required" >&2; exit 2; }
done
if [ "$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)" != 0 ]; then
  echo "kernel.yama.ptrace_scope is not 0: gdb cannot attach to the server" >&2
  exit 2
fi

RTLIM=$(ulimit -r)
NICELIM=$(ulimit -e)
if [ "$RTLIM" = unlimited ] || [ "$RTLIM" -ge 99 ]; then WANT_RT=99; else WANT_RT=$RTLIM; fi
if [ "$NICELIM" = unlimited ] || [ "$NICELIM" -ge 40 ]; then FLOOR=-20; else FLOOR=$((20 - NICELIM)); fi
echo "limits: RLIMIT_RTPRIO=$RTLIM RLIMIT_NICE=$NICELIM -> want writers FF $WANT_RT, libFLAC workers TS nice $FLOOR"

FAILS=0
SRV=
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }
url() { echo "http://127.0.0.1:$PORT$1"; }
cleanup() { [ -n "$SRV" ] && { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }; }
trap cleanup EXIT

start_server() {   # config-file log-file
  "$BIN" --config "$1" --net-serve 120 > "$2" 2>&1 &
  SRV=$!
  for _ in $(seq 1 50); do curl -sf "$(url /version)" >/dev/null && return 0; sleep 0.2; done
  fail "server did not come up"; cat "$2"; exit 1
}

select_and_start() {   # anchored device-name regex
  local idx= sel= name=
  # The device scan finishes after the listener is up; wait for the entry itself.
  for _ in $(seq 1 50); do
    idx=$(curl -s "$(url /devices)" | jq --arg re "$1" '[.devices[] | select(.name | test($re))][0].index')
    [ -n "$idx" ] && [ "$idx" != null ] && break
    sleep 0.2
  done
  if [ -z "$idx" ] || [ "$idx" = null ]; then
    fail "no device matching /$1/ in /devices: $(curl -s "$(url /devices)")"; return 1
  fi
  name=$(curl -s "$(url /devices)" | jq -r --argjson i "$idx" '.devices[$i].name')
  case "$name" in
    "[Simulated]"*|"[Playback]"*) ;;
    *) if [ "${ALLOW_HARDWARE:-0}" != 1 ]; then
         fail "refusing to capture from '$name' (real hardware); set ALLOW_HARDWARE=1 to allow"
         return 1
       fi ;;
  esac
  # /device only queues the selection for the main loop; a /start sent before it
  # lands starts whatever was selected before (entry 0 -- a real card on hosts
  # that have one). Confirm the selection first.
  curl -s "$(url "/device?$idx")" >/dev/null
  for _ in $(seq 1 50); do
    sel=$(curl -s "$(url /devices)" | jq .selected)
    [ "$sel" = "$idx" ] && break
    sleep 0.2
  done
  if [ "$sel" != "$idx" ]; then
    fail "device selection did not land (selected $sel, want $idx '$name')"; return 1
  fi
  curl -s "$(url /start)" >/dev/null
  for _ in $(seq 1 50); do
    if [ "$(curl -s "$(url /stats)" | jq .state)" = 1 ]; then
      echo "capturing on '$name'"
      return 0
    fi
    sleep 0.2
  done
  fail "capture on '$name' did not start: $(curl -s "$(url /stats)")"; return 1
}

map_roles() {   # out-file
  gdb -p "$SRV" -batch -ex "thread apply all bt" > "$S/threads.txt" 2>/dev/null
  python3 - "$S/threads.txt" "$SRV" > "$1" <<'PY'
import re, sys
roles, tid, frames = {}, None, []
def flush():
    if tid is None:
        return
    text = "\n".join(frames)
    for fn in ("flac_writer_thread", "raw_writer_thread", "extraction_thread",
               "audio_thread_main", "display_thread_func", "playback_thread_func"):
        if re.search(rf"\b{fn}\b", text):
            roles[tid] = fn
            return
    if "libFLAC" in text:
        roles[tid] = "libflac_worker"
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"Thread \d+ \(Thread 0x[0-9a-f]+ \(LWP (\d+)\)", line)
    if m:
        flush()
        tid, frames = m.group(1), []
    elif tid is not None:
        frames.append(line.rstrip())
flush()
roles[sys.argv[2]] = "main_thread"   # the headless loop / render thread is the pid's own tid
for t, r in roles.items():
    print(t, r)
PY
}

check_role() {   # roles-file role want_cls want_rtprio_or_- want_nice_or_- want_ioprio_or_-
  local roles=$1 role=$2 wcls=$3 wrt=$4 wni=$5 wio=$6 seen=0
  while read -r tid r; do
    [ "$r" = "$role" ] || continue
    seen=1
    read -r cls rt ni < <(ps -L -o tid=,cls=,rtprio=,ni= -p "$SRV" | awk -v t="$tid" '$1==t {print $2, $3, $4}')
    io=$(ionice -p "$tid" 2>/dev/null)
    local ok=1
    [ "$cls" = "$wcls" ] || ok=0
    [ "$wrt" = - ] || [ "$rt" = "$wrt" ] || ok=0
    [ "$wni" = - ] || [ "$ni" = "$wni" ] || ok=0
    [ "$wio" = - ] || [ "$io" = "$wio" ] || ok=0
    if [ "$ok" = 1 ]; then pass "$role $tid: $cls rtprio $rt nice $ni io '$io'"
    else fail "$role $tid: $cls rtprio $rt nice $ni io '$io' (want $wcls rtprio $wrt nice $wni io '$wio')"; fi
  done < "$roles"
  [ "$seen" = 1 ] || echo "SKIP: no $role thread in this run"
}

stop_server() {
  kill "$SRV"; wait "$SRV" 2>/dev/null
  SRV=
}

# --- phase 1: capture + FLAC record on $DEVICE_RE ---------------------------
mkdir -p "$S/out"
cat > "$S/record.json" <<EOF
{
  "net_mode": 1,
  "net_server_port": $PORT,
  "net_server_port_str": "$PORT",
  "output_path": "$S/out",
  "output_base_name": "prio",
  "auto_names_enabled": true,
  "overwrite_files": true,
  "use_flac": true,
  "flac_level": 8,
  "capture_a": true,
  "capture_b": true,
  "video_record_enabled": false,
  "rtsp_stream_enabled": false,
  "audio_monitor_playback": false
}
EOF
echo "--- phase 1: record on /$DEVICE_RE/"
start_server "$S/record.json" "$S/record.log"
if select_and_start "$DEVICE_RE"; then
  curl -s "$(url "/record?on=1")" >/dev/null
  for _ in $(seq 1 50); do [ "$(curl -s "$(url /stats)" | jq .state)" = 2 ] && break; sleep 0.2; done
  if [ "$(curl -s "$(url /stats)" | jq .state)" = 2 ]; then
    sleep 4   # let libFLAC start its workers
    map_roles "$S/roles-record.txt"
    check_role "$S/roles-record.txt" main_thread         TS - - -
    check_role "$S/roles-record.txt" flac_writer_thread  FF "$WANT_RT" - "best-effort: prio 0"
    check_role "$S/roles-record.txt" raw_writer_thread   FF "$WANT_RT" - "best-effort: prio 0"
    check_role "$S/roles-record.txt" extraction_thread   FF "$WANT_RT" - "best-effort: prio 0"
    check_role "$S/roles-record.txt" audio_thread_main   FF "$WANT_RT" - "best-effort: prio 0"
    check_role "$S/roles-record.txt" display_thread_func TS - -5 -
    check_role "$S/roles-record.txt" libflac_worker      TS - "$FLOOR" "best-effort: prio 0"
    curl -s "$(url "/record?on=0")" >/dev/null
    for _ in $(seq 1 150); do
      ST=$(curl -s "$(url /stats)")
      [ "$(jq .state <<<"$ST")" = 1 ] && [ "$(jq .rec_finalizing <<<"$ST")" = false ] && break
      sleep 0.2
    done
  else
    fail "recording did not start: $(curl -s "$(url /stats)")"
  fi
  curl -s "$(url /stop)" >/dev/null
fi
stop_server
echo "phase 1 [THREAD] lines:"; grep 'THREAD' "$S/record.log" | sort | uniq -c

# --- phase 2: replay the recording through the Playback device --------------
RFA=$(ls "$S"/out/rfA*.flac 2>/dev/null | head -1)
if [ -z "$RFA" ]; then
  echo "SKIP: phase 2 (no rfA FLAC from phase 1)"
else
  PORT=$((PORT + 1))
  cat > "$S/playback.json" <<EOF
{
  "net_mode": 1,
  "net_server_port": $PORT,
  "net_server_port_str": "$PORT",
  "output_path": "$S/out",
  "playback_file_a": "$RFA",
  "playback_file_b": "",
  "video_record_enabled": false,
  "rtsp_stream_enabled": false,
  "audio_monitor_playback": false
}
EOF
  echo "--- phase 2: playback of $(basename "$RFA")"
  start_server "$S/playback.json" "$S/playback.log"
  if select_and_start '^\[Playback\]'; then
    sleep 2
    map_roles "$S/roles-playback.txt"
    check_role "$S/roles-playback.txt" main_thread          TS - - -
    check_role "$S/roles-playback.txt" playback_thread_func TS - -5 -
    curl -s "$(url /stop)" >/dev/null
  fi
  stop_server
fi

echo "$FAILS failure(s)"
[ "$FAILS" = 0 ]
