# GDH capture rigs: wiring and settings (fork-only)

Which deck is cabled to which card on each MISRC-GUI capture host, and the settings each rig
captures with. Ported from capture-node's `config/capture-node/capture-setup/prod/{wm,cs0}.yml`
(`origin/dev` at 7be7bf6, 2026-11-17) when capture-node was retired. preview and dev
listed the same rigs and differed only in ports. The device names here are created by
[`install.sh`](install.sh): the udev rules give the `/dev/cx/*` and `/dev/gdhvc/usb/*` links
and the ALSA card ids, and `/etc/asound.conf` gives the PCMs.

MISRC-GUI applies these settings itself only in part:

- On Linux it writes `level`, `tenbit` and `center_offset` (`input/gui_cxadc.c`).
- It only reads `tenxfsc` and `crystal`, to work out the card's sample rate.
- It does **not** set `vmux` or `sixdb`; only the macOS backend does.
- Set what it leaves alone by hand (`echo N > /sys/class/cxadc/cxadcN/device/parameters/<name>`;
  `cxvalues` from cxadc-linux shows them), or the card keeps its driver default.
- It does not set the clockgen clocks either: they are the clockgen's ALSA mixer controls
  (`alsamixer`/`amixer -c <host>_cg_audio`; cxadc-linux `clockgen/README.md`).

## Settings every rig uses

Every rig below, on both hosts, used the same values on every CX card:

| `vmux` | `level` | `sixdb` | `tenxfsc` | `tenbit` | `center_offset` | `crystal` |
|---|---|---|---|---|---|---|
| 1 | 8 | 0 | 0 | 0 (8-bit) | 2 | 40000000 |

Every rig with a clockgen ran both Si5351 outputs at `CXADC-40MHz` (`cx_clock_0`, `cx_clock_1`).
The clockgen's linear audio is S24_3LE / 46875 Hz / 3 ch through `clockgen-dsnoop`.

## RTSP

MISRC-GUI publishes one reference stream per host, not one per rig:
`rtsp://<host>.gdhvc.lan:8654/misrc-preview`. It is reachable off the host only with
`usbref_rtsp_lan` on; the default is loopback. capture-node's per-rig URLs are retired and
are listed in each table below so old notes and bookmarks can be matched up.

## wm (workflow-master)

| Device | Name | Where |
|---|---|---|
| CX card, video / combined RF | `/dev/cx/wm-video` | PCI 0000:03:00.0 (`cxadc0`), confirmed 2026-09-02 |
| CX card, HiFi RF | `/dev/cx/wm-hifi` | PCI 0000:08:00.0 (`cxadc1`), confirmed 2026-09-02 |
| Clockgen | ALSA card `wm_cg_audio`; PCMs `clockgen-dsnoop`, `clockgen-stream`, `wm-cg-audio` | USB 1209:0001, serial 5044340580655E1C or 5044340580BE5C1C (one plugged at a time) |
| USB reference dongle (MS210x, 534d:0021) | video `/dev/gdhvc/usb/wm-ref-video`; audio ALSA card `wm_ref_audio`, PCM `wm-ref-audio` | USB port 1-13.3 |

All four rigs share these devices. The operator cables the deck being captured; two rigs
on the same cards can never capture at once.

| Rig | Deck | Formats | CX cards | Clockgen | Reference video / audio | capture-node URL (retired) |
|---|---|---|---|---|---|---|
| CAM1 (`ccd-trv615_1`) | Sony Handycam CCD-TRV615 #1 | Video8, Hi8 | combined RF on `wm-video` | none (FM video and AFM audio share one RF carrier) | `wm-ref-video` / `wm-ref-audio` | `rtsp://workflow-master.gdhvc.lan:8554/CAM1` |
| CAM0 (`ccd-trv615_0`) | Sony Handycam CCD-TRV615 #0 | Video8, Hi8 | combined RF on `wm-video` | none | `wm-ref-video` / `wm-ref-audio` | `…:8554/CAM0` |
| VCR0 (`pv-s7670_0`) | Panasonic PV-S7670 #0 | VHS, VHS-C, S-VHS | video RF on `wm-video`, HiFi RF on `wm-hifi` | `wm-cg-audio`; carries the deck's single audio output | `wm-ref-video` / clockgen L/R via `clockgen-stream` | `…:8554/VCR0` |
| VCR1 (`pv-s7670_1`) | Panasonic PV-S7670 #1 | VHS, VHS-C, S-VHS | video RF on `wm-video`, HiFi RF on `wm-hifi` | `wm-cg-audio` | `wm-ref-video` / clockgen L/R via `clockgen-stream` | `…:8554/VCR1` |

wm also has a StarTech USB 2828x (em28xx) video grabber, ALSA card `Device`. No udev rule names
it; MISRC-GUI's USB reference settings pick it by device.

## cs0 (capture-server-0)

| Device | Name | Where |
|---|---|---|
| CX card, video RF | `/dev/cx/cs0-video` | rule names PCI 0000:04:00.0, **stale** (see below) |
| CX card, HiFi RF | `/dev/cx/cs0-hifi` | rule names PCI 0000:06:00.0, a placeholder that never held a CX card |
| Clockgen | ALSA card `cs0_cg_audio`; PCMs `clockgen-dsnoop`, `clockgen-stream`, `cs0-cg-audio` | USB 1209:0001, serial 5044340568ADCC1C (installed, bench-verified 2026-08-29) or 50443405A8B9851C |
| USB reference dongle (MS210x) | video `/dev/gdhvc/usb/vcr0-ref-video` | USB port 1-14; its audio has no rule (the deck's audio goes to the clockgen) |

**CX cards on cs0 today.** On 2026-11-17 cs0 had no CX card seated: `lspci` shows no `14f1`
device, there is no `/dev/cxadc*`, and 0000:04:00.0 is now the Realtek RTL8125 NIC. Neither
`/dev/cx` alias exists. Earlier, 0000:04:00.0 held the video card, and a second card at
0000:08:00.0 was faulted ("not answering mmio reads"; a PCI rescan of it hung cs0). Before
capturing on cs0 again, seat the cards, read their slots with `lspci -nn | grep 14f1`, and correct
both `KERNELS` values in `install.sh`'s `cx_rules_cs0` and in this table.

| Rig | Deck | Formats | CX cards | Clockgen | Reference video / audio | capture-node URL (retired) |
|---|---|---|---|---|---|---|
| VCR0 (`pv-s7670_0`) | Panasonic PV-S7670 #0 | VHS, VHS-C, S-VHS | video RF on `cs0-video`, HiFi RF on `cs0-hifi` | `cs0-cg-audio`; carries the deck's single audio output | `vcr0-ref-video` / clockgen L/R via `clockgen-stream` | `rtsp://capture-server-0.gdhvc.lan:8554/VCR0` |
| VCR1 (`pv-s7670_1`), **staged, was disabled** | Panasonic PV-S7670 #1 | VHS, VHS-C, S-VHS | same as VCR0 | `cs0-cg-audio` | `vcr1-ref-video` (the dongle on port 1-14, still named `vcr0-ref-video` by the rule) / clockgen L/R | `…:8554/VCR1` |

VCR0 has since moved to wm, which carries the same rig id. On cs0, VCR1 was to become the
only rig once the HiFi card was seated. When that happens, rename the `vcr0-*` USB alias to
`vcr1-*` in `usb_rules_cs0`.
