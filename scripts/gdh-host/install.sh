#!/usr/bin/env bash
# GDH host provisioning for MISRC-GUI's capture-first scheduling. Fork-only.
#
# Installs, under /etc/systemd/system:
#   user@.service.d/misrc-limits.conf         wm, cs0   LimitRTPRIO=99, LimitNICE=-20 for
#                                                       every user manager, so a GUI started
#                                                       from a session can take SCHED_FIFO 99
#                                                       and nice -20
#   <actions.runner.*>.service.d/misrc-guard-limits.conf
#                                             wm, cs0   LimitRTPRIO=5, LimitNICE=25: exactly
#                                                       what the priority guards lower to, and
#                                                       no more
#   misrc-server.service                      cs0       misrc_gui --net-serve as the capture
#                                                       user, with the GUI's limits
#
# It runs as root from a ROOT-OWNED copy, /usr/local/libexec/misrc/gdh-host-install,
# installed once by hand; the deploy calls that copy through a sudoers rule naming it
# and nothing else (see README.md). A rule pointing at this user-writable checkout
# would make every process of the runner user root, which is also why everything it
# installs is written out below instead of read from the repository.
#
# It never restarts anything: restarting user@ or the runner would kill the deploy
# that runs it. New limits apply when those next start (re-login, runner restart,
# reboot), and it reports which are still pending.
#
# usage: gdh-host-install wm|cs0            install (as root, through sudo)
#        gdh-host-install --print wm|cs0    print what would be installed; no root
set -euo pipefail

usage() { echo "usage: ${0##*/} [--print] wm|cs0" >&2; exit 2; }

print_only=0
if [[ "${1:-}" == "--print" ]]; then print_only=1; shift; fi
[[ $# -eq 1 ]] || usage
host="$1"
case "$host" in wm|cs0) ;; *) usage ;; esac

if (( ! print_only )) && [[ "$EUID" -ne 0 ]]; then
  echo "ERROR: ${0##*/} must run as root (through sudo); --print shows what it installs." >&2
  exit 1
fi

# The capture user is whoever ran sudo; in --print mode, the invoking user.
capture_user="${SUDO_USER:-}"
if [[ -z "$capture_user" || "$capture_user" == root ]]; then
  if (( print_only )); then
    capture_user="$(id -un)"
  else
    echo "ERROR: run through sudo from the capture user's account (SUDO_USER is unset)." >&2
    exit 1
  fi
fi
if ! passwd_line="$(getent passwd "$capture_user")"; then
  echo "ERROR: no such user: $capture_user" >&2
  exit 1
fi
capture_home="$(cut -d: -f6 <<<"$passwd_line")"
capture_group="$(id -gn "$capture_user")"
unit_dir=/etc/systemd/system

user_limits() {
  cat <<'EOF'
# Installed by MISRC-GUI's gdh-host-install (fork-only). Lets the GUI's capture
# threads reach SCHED_FIFO 99 and nice -20; common/threading.h takes whatever
# these allow. Sorts after capture-node's capture-limits.conf, so these win.
# Applies when a user manager next starts (re-login or reboot).
[Service]
LimitRTPRIO=99
LimitNICE=-20
EOF
}

runner_limits() {
  cat <<'EOF'
# Installed by MISRC-GUI's gdh-host-install (fork-only). Exactly the limits the
# priority guards (priority_clamp_harness.c, flac_worker_priority_harness.c)
# lower themselves to, so they run in CI instead of reporting SKIP -- and no
# more, so a PR's code cannot take SCHED_FIFO 99 on this host, where RT
# throttling is off. LimitNICE=25 is the raw rlimit: nice down to -5.
# Applies when the runner next starts; never restart it from a job.
#
# OOMPolicy/Restart: on 2026-09-15 a global OOM on wm killed one small process
# inside the runner's unit; systemd's default OOMPolicy=stop then stopped the
# whole runner, Restart=no left it stopped, and every fork PR's CI queued at the
# first job, which only runs on wm. Measured with transient units on wm:
# OOMPolicy=continue keeps the unit running through a child's kill, and
# Restart=on-failure brings it back if the listener itself dies.
[Service]
LimitRTPRIO=5
LimitNICE=25
OOMPolicy=continue
Restart=on-failure
RestartSec=30
EOF
}

server_unit() {
  cat <<EOF
# Installed by MISRC-GUI's gdh-host-install (fork-only). The GUI's headless
# capture server: a GUI client elsewhere drives this host's capture hardware.
# Paths are absolute because %h is root's home in a system unit, whatever
# User= says.
[Unit]
Description=MISRC-GUI capture server (misrc_gui --net-serve)
Wants=network-online.target
After=network-online.target
ConditionPathExists=${capture_home}/.config/misrc/server.json

[Service]
Type=simple
User=${capture_user}
Group=${capture_group}
ExecStart=${capture_home}/.local/bin/misrc_gui --config ${capture_home}/.config/misrc/server.json --net-serve
Restart=on-failure
RestartSec=5
LimitRTPRIO=99
LimitNICE=-20
CPUWeight=10000
IOWeight=10000
KillSignal=SIGTERM
# On SIGTERM the server stops the recording and waits for the FLAC finalize; a
# legacy file without reserved padding is rewritten whole, which takes minutes.
TimeoutStopSec=900

[Install]
WantedBy=multi-user.target
EOF
}

runner_units() {
  systemctl list-unit-files --no-legend --plain 'actions.runner.*.service' 2>/dev/null \
    | awk '{print $1}'
}

mapfile -t runners < <(runner_units)

if (( print_only )); then
  emit() { echo "== $1"; "$2"; echo; }
  emit "$unit_dir/user@.service.d/misrc-limits.conf" user_limits
  (( ${#runners[@]} )) || runners=("actions.runner.<name>.service")
  for r in "${runners[@]}"; do
    emit "$unit_dir/$r.d/misrc-guard-limits.conf" runner_limits
  done
  if [[ "$host" == cs0 ]]; then
    emit "$unit_dir/misrc-server.service" server_unit
  fi
  exit 0
fi

changed=0
put() {   # destination generator
  local dest="$1" tmp
  tmp="$(mktemp)"
  "$2" > "$tmp"
  if [[ -f "$dest" ]] && cmp -s "$tmp" "$dest"; then
    echo "unchanged  $dest"
  else
    install -D -m 0644 "$tmp" "$dest"
    echo "installed  $dest"
    changed=1
  fi
  rm -f "$tmp"
}

put "$unit_dir/user@.service.d/misrc-limits.conf" user_limits
if (( ${#runners[@]} == 0 )); then
  echo "no actions.runner.*.service units on this host"
fi
for r in "${runners[@]}"; do
  put "$unit_dir/$r.d/misrc-guard-limits.conf" runner_limits
done
if [[ "$host" == cs0 ]]; then
  put "$unit_dir/misrc-server.service" server_unit
fi
if (( changed )); then
  systemctl daemon-reload
  echo "systemctl daemon-reload"
fi

if [[ "$host" == cs0 ]]; then
  systemctl enable --quiet misrc-server.service
  if systemctl is-active --quiet misrc-server.service; then
    echo "misrc-server.service is running; the deploy restarts it when it is idle"
  elif [[ ! -f "$capture_home/.config/misrc/server.json" ]]; then
    echo "NOTE: $capture_home/.config/misrc/server.json is missing; misrc-server.service stays off until it exists"
  elif pgrep -u "$capture_user" -x misrc_gui >/dev/null; then
    echo "NOTE: a misrc_gui already runs as $capture_user; not starting misrc-server.service beside it"
  else
    systemctl start misrc-server.service
    echo "started misrc-server.service"
  fi
fi

# What the RUNNING processes have, from /proc: after daemon-reload `systemctl show`
# reports the new configured value while the old process still runs with the old one.
live_rtprio() {
  local pid
  pid="$(systemctl show "$1" -p MainPID --value 2>/dev/null || true)"
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 0
  awk '/^Max realtime priority/ {print $4}' "/proc/$pid/limits" 2>/dev/null || true
}
report() {   # unit wanted-rtprio
  local have
  have="$(live_rtprio "$1")"
  if [[ "$have" == "$2" ]]; then
    echo "active     $1 runs with RLIMIT_RTPRIO $have"
  else
    echo "pending    $1 runs with RLIMIT_RTPRIO ${have:-unknown}; $2 applies when it next starts"
  fi
}
report "user@$(id -u "$capture_user").service" 99
for r in "${runners[@]}"; do
  report "$r" 5
done
