# GDH host provisioning (fork-only)

`install.sh` gives the GDH Linux hosts the scheduling limits MISRC-GUI's capture path
needs, the udev, ALSA and WirePlumber files its capture hardware needs, and gives cs0 a
supervised headless capture server. It never goes upstream. Which deck is wired to which
card, and the settings each rig captures with, are in [`rigs.md`](rigs.md).

| File it installs (under `/etc/systemd/system`) | Hosts | What it does |
|---|---|---|
| `user@.service.d/misrc-limits.conf` | wm, cs0 | `LimitRTPRIO=99`, `LimitNICE=-20` for every user manager: a GUI started from a desktop session or a user service can take SCHED_FIFO 99 and nice -20 (`common/threading.h` takes whatever the limits allow). |
| `<actions.runner.*>.service.d/misrc-guard-limits.conf` | wm, cs0 | `LimitRTPRIO=5`, `LimitNICE=25` for each Actions runner: exactly what the priority guards lower themselves to, so they run in CI instead of reporting SKIP, and no more — a PR's code cannot take FIFO 99 on a host where RT throttling is off. Also `OOMPolicy=continue` and `Restart=on-failure`: on 2026-09-15 a global OOM killed one small process inside wm's runner unit, systemd's default `OOMPolicy=stop` took the whole runner down, and `Restart=no` left it down until someone noticed the queued jobs. |
| `misrc-server.service` | cs0 | `misrc_gui --config ~/.config/misrc/server.json --net-serve` as the capture user, with the GUI's limits, `CPUWeight`/`IOWeight` 10000, `Restart=on-failure`, `TimeoutStopSec=900` (a legacy FLAC finalize on stop takes minutes). Skipped cleanly while `server.json` is missing. |

The capture-host files below were installed by capture-node's deploy until its retirement
(2026-11). They keep the paths and file names it used, and every rule and directive line
it wrote; only the comments changed.

| File it installs | Hosts | What it does |
|---|---|---|
| `/etc/udev/rules.d/70-gdhvc-<host>-usb-capture.rules` | wm, cs0 | Serial-matched ALSA card id `<host>_cg_audio` for the clockgen (MISRC-GUI's `gui_cxadc.c` finds the card by it; card indices move across reboots) and `/dev/gdhvc/usb/*` links for the USB reference dongle. |
| `/etc/udev/rules.d/71-gdhvc-<host>-cxadc-alias.rules` | wm, cs0 | `/dev/cx/<host>-video` and `/dev/cx/<host>-hifi`, by PCI slot. |
| `/etc/asound.conf` | wm, cs0 | The `clockgen-dsnoop` fan-out (S24_3LE/46875/3ch on `hw:CARD=<host>_cg_audio`), which MISRC-GUI opens before any `hw:` device, plus `clockgen-stream` and the `<host>-cg-audio` aliases. |
| `~/.config/wireplumber/wireplumber.conf.d/99-gdhvc-clockgen.conf` | wm, cs0 | Disables the clockgen in PipeWire, which otherwise holds its PCM open and can wedge its USB control endpoint. Written as the capture user (`runuser`), never by root into a home directory. |

A changed udev rules file is followed by `udevadm control --reload-rules` and a `change`
trigger of the `video4linux`, `sound` and `cxadc` subsystems; an unchanged one triggers
nothing. WirePlumber reads its policy when it next starts; the installer prints `pending`
while an older one runs. The installer never deletes a file it does not install: removing
capture-node's leftovers (`capture-limits.conf`, wm's stale copies of cs0's rules) is the
toolkit's `retire-capture-node.sh`. The `72-gdhvc-<host>-nvidia-uvm.rules` rule serves
decode GPUs, not capture: the toolkit's `scripts/fleet/provision-host-root.sh` takes it over.

`./install.sh --print wm|cs0` prints all of it without root; the "GDH host installer" guard
checks that output.

## Why a root-owned copy

The deploy runs the installer as root through `sudo -n`. A sudoers rule pointing at a
script in the runner's checkout would make every process of the runner user root, since
that file is user-writable. So the installer runs from a copy owned by root that you
install by hand, and the files it installs are written out inside it rather than read from
the repository. The deploy never updates that copy: when the repo's `install.sh` differs,
the "Provision host (gdh-host)" step warns with the command to reinstall it.

## One-time setup, per host

On wm and cs0, from a checkout of `main` in the capture user's account (`rdodge`):

```bash
sudo install -D -o root -g root -m 0755 scripts/gdh-host/install.sh /usr/local/libexec/misrc/gdh-host-install
sudo visudo -f /etc/sudoers.d/misrc-gdh-host
```

The sudoers file, on wm:

```
rdodge ALL=(root) NOPASSWD: /usr/local/libexec/misrc/gdh-host-install wm
```

on cs0 (the second command lets the deploy restart an idle server onto a new binary):

```
rdodge ALL=(root) NOPASSWD: /usr/local/libexec/misrc/gdh-host-install cs0, /usr/bin/systemctl restart misrc-server.service
```

Then run it once by hand (`sudo /usr/local/libexec/misrc/gdh-host-install wm`, or `cs0`),
or let the next deploy do it. Rerun the `install -D` line whenever `install.sh` changes.

## When the limits take effect

The installer never restarts anything — restarting `user@` or a runner from a deploy would
kill the deploy. It prints `pending` for each process still on its old limits:

- **GUI (user manager):** at the next login or reboot. Check with
  `grep 'Max realtime' /proc/$(pgrep -x misrc_gui)/limits` (want 99), or the
  `[THREAD] ... using N (resource limit)` line in the GUI's log.
- **Runners:** when the runner service next starts — at an idle moment,
  `sudo systemctl restart actions.runner.<name>.service`, never from a job.
- **cs0 server:** the installer enables `misrc-server.service` and starts it unless
  `server.json` is missing or a `misrc_gui` already runs as the capture user. After each
  deploy the "Restart cs0 misrc-server if idle" step restarts it onto the new binary only
  when `GET /settings` reports `"state": 0`; otherwise it leaves it and says so.

The server binds all interfaces on its `net_server_port`, the same exposure as a manual
`--net-serve`.

## Checking the whole posture

`misrc_tools/test/capture_priority_e2e.sh build-local/misrc_gui` records on the Simulated
device through `--net-serve`, maps each thread to its role with gdb and checks its
scheduling class, realtime priority, nice and I/O class (see its header).
