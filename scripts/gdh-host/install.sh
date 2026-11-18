#!/usr/bin/env bash
# GDH host provisioning for MISRC-GUI's capture hosts. Fork-only.
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
# and the capture-host files capture-node's deploy used to install, at the same paths
# (capture-node is retired; MISRC-GUI is the only capture tool):
#   /etc/udev/rules.d/70-gdhvc-<host>-usb-capture.rules
#                                             wm, cs0   USB reference-device symlinks and the
#                                                       clockgen's ALSA card id <host>_cg_audio,
#                                                       which gui_cxadc.c resolves the card by
#   /etc/udev/rules.d/71-gdhvc-<host>-cxadc-alias.rules
#                                             wm, cs0   /dev/cx/<host>-video, /dev/cx/<host>-hifi
#   /etc/asound.conf                          wm, cs0   the clockgen-dsnoop fan-out the GUI
#                                                       opens first, and the ALSA aliases
#   ~<capture user>/.config/wireplumber/wireplumber.conf.d/99-gdhvc-clockgen.conf
#                                             wm, cs0   keeps PipeWire off the clockgen; written
#                                                       as the capture user, never by root
# Rig wiring (which deck uses which card, cx sysfs values, clockgen clocks) is in
# rigs.md beside this script.
#
# It runs as root from a ROOT-OWNED copy, /usr/local/libexec/misrc/gdh-host-install,
# installed once by hand; the deploy calls that copy through a sudoers rule naming it
# and nothing else (see README.md). A rule pointing at this user-writable checkout
# would make every process of the runner user root, which is also why everything it
# installs is written out below instead of read from the repository.
#
# It never restarts anything: restarting user@ or the runner would kill the deploy
# that runs it. New limits apply when those next start (re-login, runner restart,
# reboot), and it reports which are still pending. A changed udev rule is reloaded and
# re-triggered for the video4linux, sound and cxadc subsystems only; WirePlumber picks
# up its policy at its next start. It never deletes a file it does not install.
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
# these allow. Applies when a user manager next starts (re-login or reboot).
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

# ---- capture-host files ------------------------------------------------------
# Taken over from capture-node (config/host/{udev,alsa,wireplumber}) at its
# retirement, 2026-11. Every rule and directive line is byte-identical to what
# capture-node installed; only the comments changed.

usb_rules_wm() {
  cat <<'EOF'
# GDH: stable /dev/gdhvc/usb/* symlinks and ALSA card ids for the USB devices
# connected to workflow-master (wm).
# Installed by MISRC-GUI's gdh-host-install (fork-only, scripts/gdh-host/install.sh);
# edit it there, not here. Rig wiring: scripts/gdh-host/rigs.md.

# USB port 1-13.3 -> the MS210x capture dongle installed in wm (534d:0021
# "USB Video", generic serial 20200909, hence port-matched), shared by every wm
# rig (CAM0, CAM1, VCR0, VCR1). Port confirmed 2026-09-02: video function
# 1-13.3:1.0, audio function 1-13.3:1.2.
SUBSYSTEM=="video4linux", KERNEL=="video*", ATTR{index}=="0", KERNELS=="1-13.3:1.0", SYMLINK+="gdhvc/usb/wm-ref-video", SYMLINK+="wm-ref-video"
SUBSYSTEM=="sound", KERNEL=="card*", KERNELS=="1-13.3:1.2", ATTR{id}:="wm_ref_audio"
SUBSYSTEM=="sound", KERNEL=="controlC*", KERNELS=="1-13.3:1.2", SYMLINK+="gdhvc/usb/wm-ref-audio"

# wm Clockgen USB audio is a dedicated device (not a USB dongle audio interface).
# Match by USB VID/PID + serial and assign a stable ALSA card id: /etc/asound.conf
# names it, and MISRC-GUI's gui_cxadc.c resolves the clockgen card by it (card
# indices move across reboots and replugs).
# Two units exist for wm (hardware swap found 2026-08-18: the plugged unit's serial
# matched no rule, so the card kept the driver-default id and the clockgen could not
# be found by name). Both serials map to the same id on purpose -- either unit
# works -- but ALSA card ids must be unique, so only ONE may be plugged in at a time
# or the second registration collides on wm_cg_audio.
SUBSYSTEM=="sound", KERNEL=="card*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="5044340580655E1C", ATTR{id}:="wm_cg_audio"
SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="5044340580655E1C", SYMLINK+="gdhvc/usb/wm-cg-audio"
SUBSYSTEM=="sound", KERNEL=="card*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="5044340580BE5C1C", ATTR{id}:="wm_cg_audio"
SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="5044340580BE5C1C", SYMLINK+="gdhvc/usb/wm-cg-audio"
EOF
}

cx_rules_wm() {
  cat <<'EOF'
# GDH: stable /dev/cx/* aliases for workflow-master (wm).
# Installed by MISRC-GUI's gdh-host-install (fork-only, scripts/gdh-host/install.sh);
# edit it there, not here. Rig wiring: scripts/gdh-host/rigs.md.
# The aliases name the cards installed in wm, not a deck: every wm rig (CAM0,
# CAM1, VCR0, VCR1) uses the same two cards. wm will never hold more than two CX
# cards, so these two aliases are the whole set.
# PCI slots confirmed 2026-09-02 (lspci -nn | grep 14f1): 0000:03:00.0 -> cxadc0,
# 0000:08:00.0 -> cxadc1. Cabling: video / combined RF on wm-video, HiFi RF on
# wm-hifi; swap the two KERNELS values here if the decks are wired the other way
# round.
SUBSYSTEM=="cxadc", KERNEL=="cxadc*", KERNELS=="0000:03:00.0", SYMLINK+="cx/wm-video"
SUBSYSTEM=="cxadc", KERNEL=="cxadc*", KERNELS=="0000:08:00.0", SYMLINK+="cx/wm-hifi"
EOF
}

asound_wm() {
  cat <<'EOF'
# ALSA PCM topology for the MISRC-GUI capture host workflow-master (wm).
# Installed by MISRC-GUI's gdh-host-install (fork-only, scripts/gdh-host/install.sh);
# edit it there, not here. The card ids are set by
# /etc/udev/rules.d/70-gdhvc-wm-usb-capture.rules.


####################################################################
# wm USB capture dongle (shared by CAM0 / CAM1 / VCR0 / VCR1)
####################################################################
pcm.wm-ref-audio {
        type plug
        slave.pcm "hw:CARD=wm_ref_audio"
        hint {
                show on
                description "wm USB capture dongle"
        }
}

ctl.wm-ref-audio {
        type hw
        card wm_ref_audio
}

####################################################################
# wm - Clockgen Audio
####################################################################
pcm.wm-cg-audio {
	type plug
	# Route the operator-debug alias through the dsnoop fan-out so a manual
	# `arecord -D wm-cg-audio` shares the card instead of EBUSYing against
	# the capture that holds it.
	slave.pcm "clockgen-dsnoop"
	hint {
		show on
		description "wm clockgen audio"
	}
}

ctl.wm-cg-audio {
	type hw
	card wm_cg_audio
}


####################################################################
# wm - Clockgen linear-audio dsnoop fan-out
#
# MISRC-GUI's clockgen capture opens the shared PCM first (gui_cxadc.c tries
# clockgen-dsnoop before any hw: device), so a reference-video stream or an
# operator's arecord can read the same clockgen without contending for
# exclusive access to the hardware. While a capture runs, NEVER open
# `hw:CARD=wm_cg_audio` directly for capture -- the hardware slave is mutually
# exclusive with the dsnoop instance and a direct open EBUSYs the fan-out.
# amixer control access (clock select / capture switch) is unaffected; it goes
# through the ctl. alias, not the capture PCM.
#
#   clockgen-dsnoop : the raw dsnoop PCM (S24_3LE/46875/3ch)
#   clockgen-stream : plug wrapper for a stream encoder (format/rate conversion)
####################################################################

pcm.clockgen-dsnoop {
	type dsnoop
	# Fixed ipc_key shared by every client of this dsnoop instance. Do NOT
	# set ipc_key_add_uid: a per-uid instance would open a second hardware
	# slave and EBUSY the fan-out.
	ipc_key 8046875
	ipc_perm 0660
	slave {
		pcm "hw:CARD=wm_cg_audio,0"
		format S24_3LE
		rate 46875
		channels 3
		# pow2 period required -- large URB-misaligned periods (e.g.
		# 12000) stall dsnoop's timer sync after one period on this
		# gadget (empirically verified on cs0 2026-07-21). 60 periods
		# of 4096 frames = 245760 (~5.2s) headroom for the reader.
		period_size 4096
		buffer_size 245760
	}
	hint {
		show on
		description "wm clockgen dsnoop (archival arecord)"
	}
}

pcm.clockgen-stream {
	type plug
	slave.pcm "clockgen-dsnoop"
	hint {
		show on
		description "wm clockgen stream (RTSP reference/preview)"
	}
}
EOF
}

usb_rules_cs0() {
  cat <<'EOF'
# GDH: stable /dev/gdhvc/usb/* symlinks and ALSA card ids for the USB devices
# connected to capture-server-0 (cs0).
# Installed by MISRC-GUI's gdh-host-install (fork-only, scripts/gdh-host/install.sh);
# edit it there, not here. Rig wiring: scripts/gdh-host/rigs.md.

# USB Port 1-14 -> VCR0 (pv-s7670_0) USB capture dongle
SUBSYSTEM=="video4linux", KERNEL=="video*", ATTR{index}=="0", KERNELS=="1-14:1.0", SYMLINK+="gdhvc/usb/vcr0-ref-video", SYMLINK+="vcr0-ref-video"
# SUBSYSTEM=="sound", KERNEL=="card*", KERNELS=="1-14:1.2", SYMLINK+="gdhvc/usb/vcr0-ref-audio"

# cs0 Clockgen USB audio is a dedicated device (not a USB dongle audio interface).
# Match by USB VID/PID + serial and assign a stable ALSA card id: /etc/asound.conf
# names it, and MISRC-GUI's gui_cxadc.c resolves the clockgen card by it.
# The id is host-scoped, not deck-scoped: cs0's clockgen serves whichever PV-S7670 is
# cabled, so the card id never moves when the deck assignment does.
# Two units map to the same id on purpose -- either works -- but ALSA card ids must be
# unique, so only ONE may be plugged in at a time or the second registration collides
# on cs0_cg_audio:
#   5044340568ADCC1C -- the unit installed in cs0 today (bench-verified 2026-08-29:
#                       both Si5351 outputs CXADC-40MHz, Audio Control Capture Switch
#                       on, 3 s S24_3LE/46875/3ch arecord with a live ADC noise floor).
#                       Before this rule it matched nothing and the card registered
#                       under the driver-default id CXADCADCClockGe, so the clockgen
#                       could not be found by name.
#   50443405A8B9851C -- the unit cs0 carried before that swap.
SUBSYSTEM=="sound", KERNEL=="card*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="5044340568ADCC1C", ATTR{id}:="cs0_cg_audio"
SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="5044340568ADCC1C", SYMLINK+="gdhvc/usb/cs0-cg-audio"
SUBSYSTEM=="sound", KERNEL=="card*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="50443405A8B9851C", ATTR{id}:="cs0_cg_audio"
SUBSYSTEM=="sound", KERNEL=="controlC*", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", ATTRS{serial}=="50443405A8B9851C", SYMLINK+="gdhvc/usb/cs0-cg-audio"
EOF
}

cx_rules_cs0() {
  cat <<'EOF'
# GDH: stable /dev/cx/* aliases for capture-server-0 (cs0).
# Installed by MISRC-GUI's gdh-host-install (fork-only, scripts/gdh-host/install.sh);
# edit it there, not here. Rig wiring: scripts/gdh-host/rigs.md.
# The aliases name the cards installed in cs0, not a deck: both cs0 rigs (VCR0 and
# the staged VCR1) use the same two cards and the operator cables the deck being
# captured. cs0 holds at most two CX cards, so these two aliases are the whole set.
#
# BOTH SLOTS ARE STALE. 0000:04:00.0 was the video card on 2026-09-02; on
# 2026-11-17 cs0 had no CX card seated at all (lspci shows no 14f1 device and
# 04:00.0 is now the Realtek RTL8125 NIC), so neither alias exists. cs0-hifi's
# 0000:06:00.0 was a placeholder from the start. When cards are seated, confirm
# with `lspci -nn | grep 14f1`, correct both KERNELS values here and in rigs.md.
#
# Cabling: video / combined RF on cs0-video, HiFi RF on cs0-hifi; swap the two
# KERNELS values here if the cards are wired the other way round.
SUBSYSTEM=="cxadc", KERNEL=="cxadc*", KERNELS=="0000:04:00.0", SYMLINK+="cx/cs0-video"
SUBSYSTEM=="cxadc", KERNEL=="cxadc*", KERNELS=="0000:06:00.0", SYMLINK+="cx/cs0-hifi"
EOF
}

asound_cs0() {
  cat <<'EOF'
# ALSA PCM topology for the MISRC-GUI capture host capture-server-0 (cs0).
# Installed by MISRC-GUI's gdh-host-install (fork-only, scripts/gdh-host/install.sh);
# edit it there, not here. The card id is set by
# /etc/udev/rules.d/70-gdhvc-cs0-usb-capture.rules.


####################################################################
# cs0 - Clockgen Audio
####################################################################

pcm.cs0-cg-audio {
        type plug
        # Route the operator-debug alias through the dsnoop fan-out so a manual
        # `arecord -D cs0-cg-audio` shares the card instead of EBUSYing
        # against the capture that holds it.
        slave.pcm "clockgen-dsnoop"
        hint {
                show on
                description "cs0 clockgen audio"
        }
}

ctl.cs0-cg-audio {
        type hw
        card cs0_cg_audio
}


####################################################################
# cs0 - Clockgen linear-audio dsnoop fan-out
#
# MISRC-GUI's clockgen capture opens the shared PCM first (gui_cxadc.c tries
# clockgen-dsnoop before any hw: device), so a reference-video stream or an
# operator's arecord can read the same clockgen without contending for
# exclusive access to the hardware. While a capture runs, NEVER open
# `hw:CARD=cs0_cg_audio` directly for capture -- the hardware slave is mutually
# exclusive with the dsnoop instance and a direct open EBUSYs the fan-out.
# amixer control access (clock select / capture switch) is unaffected; it goes
# through the ctl. alias, not the capture PCM.
#
#   clockgen-dsnoop : the raw dsnoop PCM (S24_3LE/46875/3ch)
#   clockgen-stream : plug wrapper for a stream encoder (format/rate conversion)
####################################################################

pcm.clockgen-dsnoop {
        type dsnoop
        # Fixed ipc_key shared by every client of this dsnoop instance. Do NOT
        # set ipc_key_add_uid: a per-uid instance would open a second hardware
        # slave and EBUSY the fan-out.
        ipc_key 8046875
        ipc_perm 0660
        slave {
                pcm "hw:CARD=cs0_cg_audio,0"
                format S24_3LE
                rate 46875
                channels 3
                # pow2 period required -- large URB-misaligned periods (e.g.
                # 12000) stall dsnoop's timer sync after one period on this
                # gadget (empirically verified on cs0 2026-07-21). 60 periods
                # of 4096 frames = 245760 (~5.2s) headroom for the reader.
                period_size 4096
                buffer_size 245760
        }
        hint {
                show on
                description "cs0 clockgen dsnoop (archival arecord)"
        }
}

pcm.clockgen-stream {
        type plug
        slave.pcm "clockgen-dsnoop"
        hint {
                show on
                description "cs0 clockgen stream (RTSP reference/preview)"
        }
}
EOF
}

wireplumber_clockgen() {
  cat <<'EOF'
# Keep PipeWire/WirePlumber off the CX clockgen ADC.
# Installed by MISRC-GUI's gdh-host-install (fork-only, scripts/gdh-host/install.sh)
# as the capture user; edit it there, not here. Read when WirePlumber next starts
# (`systemctl --user restart wireplumber`, or the next login).
#
# The clockgen's UAC2 capture interface belongs to the capture (MISRC-GUI's
# clockgen audio, through /etc/asound.conf's clockgen-dsnoop, and amixer clock
# control). When the desktop audio stack claims the card it holds the PCM open
# and can wedge the firmware's USB control endpoint (amixer "Connection timed
# out").
#
# Matches on the USB product name, so it works regardless of per-host udev
# ATTR{id} renames (CXADCADCClockGe, wm_cg_audio, cs0_cg_audio, ...).
monitor.alsa.rules = [
  {
    matches = [
      {
        alsa.card_name = "CXADC+ADC-ClockGen"
      }
      {
        # device.name is derived from the USB product string and is present
        # even when the card's ALSA props can't be read (wedged device).
        device.name = "~alsa_card.usb-.*ClockGen.*"
      }
    ]
    actions = {
      update-props = {
        device.disabled = true
      }
    }
  }
]
EOF
}

runner_units() {
  systemctl list-unit-files --no-legend --plain 'actions.runner.*.service' 2>/dev/null \
    | awk '{print $1}'
}

mapfile -t runners < <(runner_units)

udev_dir=/etc/udev/rules.d
usb_rules="$udev_dir/70-gdhvc-$host-usb-capture.rules"
cx_rules="$udev_dir/71-gdhvc-$host-cxadc-alias.rules"
asound=/etc/asound.conf
wireplumber="$capture_home/.config/wireplumber/wireplumber.conf.d/99-gdhvc-clockgen.conf"

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
  emit "$usb_rules" "usb_rules_$host"
  emit "$cx_rules" "cx_rules_$host"
  emit "$asound" "asound_$host"
  emit "$wireplumber" wireplumber_clockgen
  exit 0
fi

changed=0
put_changed=0
put() {   # destination generator
  local dest="$1" tmp
  tmp="$(mktemp)"
  "$2" > "$tmp"
  put_changed=0
  if [[ -f "$dest" ]] && cmp -s "$tmp" "$dest"; then
    echo "unchanged  $dest"
  else
    install -D -m 0644 "$tmp" "$dest"
    echo "installed  $dest"
    put_changed=1
  fi
  rm -f "$tmp"
}

# A file in the capture user's home is compared and written AS that user: root
# never writes into a user-writable directory, where a planted symlink could
# redirect the write onto any file on the host.
put_user() {   # destination generator
  local dest="$1"
  put_changed=0
  if "$2" | runuser -u "$capture_user" -- cmp -s - "$dest"; then
    echo "unchanged  $dest"
    return
  fi
  # shellcheck disable=SC2016  # expanded by the inner sh, not here
  "$2" | runuser -u "$capture_user" -- sh -c '
    set -e
    dir="$(dirname "$1")"
    mkdir -p "$dir"
    tmp="$(mktemp "$dir/.gdh-host.XXXXXX")"
    cat > "$tmp"
    chmod 0644 "$tmp"
    mv -f "$tmp" "$1"' sh "$dest"
  echo "installed  $dest"
  put_changed=1
}

put "$unit_dir/user@.service.d/misrc-limits.conf" user_limits
(( put_changed )) && changed=1
if (( ${#runners[@]} == 0 )); then
  echo "no actions.runner.*.service units on this host"
fi
for r in "${runners[@]}"; do
  put "$unit_dir/$r.d/misrc-guard-limits.conf" runner_limits
  (( put_changed )) && changed=1
done
if [[ "$host" == cs0 ]]; then
  put "$unit_dir/misrc-server.service" server_unit
  (( put_changed )) && changed=1
fi

udev_changed=0
put "$usb_rules" "usb_rules_$host"
(( put_changed )) && udev_changed=1
put "$cx_rules" "cx_rules_$host"
(( put_changed )) && udev_changed=1
if (( udev_changed )); then
  # Only the subsystems these rules match, as capture-node's installer did. A change
  # event re-applies the same card ids and symlinks; nothing is unbound.
  udevadm control --reload-rules
  udevadm trigger --action=change --subsystem-match=video4linux
  udevadm trigger --action=change --subsystem-match=sound
  udevadm trigger --action=change --subsystem-match=cxadc
  echo "udevadm: rules reloaded; video4linux, sound and cxadc re-triggered"
fi
put "$asound" "asound_$host"     # ALSA reads it at each open; nothing to reload
put_user "$wireplumber" wireplumber_clockgen
wireplumber_changed=$put_changed

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
if (( wireplumber_changed )); then
  wp_pid="$(pgrep -u "$capture_user" -x wireplumber | head -n1 || true)"
  if [[ -n "$wp_pid" ]]; then
    echo "pending    wireplumber (pid $wp_pid) reads 99-gdhvc-clockgen.conf when it next starts"
  fi
fi
