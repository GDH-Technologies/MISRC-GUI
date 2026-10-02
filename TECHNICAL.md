# MISRC GUI — Technical Notes

This file consolidates the code-focused technical notes that previously lived as
standalone files (`GUI_README.md`, `MISRC_GUI_README.md`) in the upstream
`harrypm/MISRC` repo. The user-facing project README is `README.md`; this
document is for developers working on the GUI/tool source.

The packages contain two command-line applications, `misrc_capture` and
`misrc_extract`, plus the `misrc_gui` graphical application. For detailed usage
information see `misrc_tools/README.md`.

## Source build dependency note (FFT)

`misrc_gui` requires FFT support via FFTW single-precision (`fftw3f`). Source
builds fail at configure time if FFTW is missing.

Install FFTW development packages before running Meson:

- Debian/Ubuntu/Linux Mint: `libfftw3-dev`
- macOS (Homebrew): `fftw`
- MSYS2 MinGW x86_64: `mingw-w64-x86_64-fftw`
- MSYS2 MinGW arm64: `mingw-w64-clang-aarch64-fftw`

If you already configured a Meson build directory before installing FFTW, wipe
and reconfigure so stale dependency paths are removed:

    meson setup --wipe misrc_tools/build misrc_tools

## Local AppImage test build (Ubuntu 22.04 baseline)

For a reproducible local AppImage build that targets a `glibc 2.35` baseline,
run:

    ./scripts/build-appimage-local.sh

The script uses `docker` or `podman` (prefers docker if both are installed),
installs build dependencies in an `ubuntu:22.04` container, and writes the
output AppImage to `.ci-artifacts/linux-appimage/`.

If you want to run directly on your host (with all dependencies already
installed), use:

    ./scripts/build-appimage-local.sh --native

## Linux AppImage desktop integration (taskbar icon / pinning)

An AppImage is never registered with the desktop by itself. The AppDir ships a
real `assets/appimage/AppRun` and `assets/appimage/misrc_gui.desktop` (shared by
the CI workflow and `scripts/build-appimage-local.sh`; not inline heredocs).

What `AppRun` does:

- Exports `RESOURCE_NAME=misrc_gui` (X11 `WM_CLASS` instance).
- On plain GUI launches (no args, or only `--debug-view` / `--auto-connect` /
  `--config <file>`) it installs
  `~/.local/share/applications/misrc_gui.desktop` (`Exec` = the AppImage file
  itself) and the hicolor icon. It rewrites them only when their content changes,
  so the launcher follows whichever AppImage ran last.
- It does nothing for `capture`, `extract`, `--smoke-test`, `--version`, `--help`,
  when `APPIMAGE` is unset, or when the path contains `" ` $ \`.
- It never writes to `~/Desktop`. A Desktop icon is created only on request:
  `./<MISRC AppImage> --create-shortcut`. (Earlier builds copied one to the
  Desktop on every launch, so it came back after being deleted.)
- `MISRC_GUI_NO_INTEGRATION=1` skips the automatic launcher install.

Window identity (`misrc_tools/misrc_gui/core/misrc_gui.c`): GLFW/raylib builds
the X11 `WM_CLASS` from the window title at creation, so the window is created
with the fixed title `MISRC Capture` and the versioned title is applied
afterwards with `SetWindowTitle()`. Result: `WM_CLASS = "misrc_gui", "MISRC Capture"`
on every release, matching `StartupWMClass=MISRC Capture`. Before this, the class
was the versioned title (e.g. `MISRC Capture v1.2.2`) and changed every release.

The desktop id (`misrc_gui.desktop`), `Icon=misrc` / `misrc.png`, the
`RESOURCE_NAME` instance and `StartupWMClass` must stay in agreement;
`misrc_tools/test/ci_guard_tests.py` enforces this (`window identity contract`,
`AppRun static contract`, `AppRun runtime behavior`).

To verify on X11, match the window by PID (never by name, another instance may
be running) and only read the mapped main window:

    xdotool search --onlyvisible --pid <pid>
    xprop -id <window> WM_CLASS _NET_WM_PID WM_STATE

The same recipe is used by FLAC-Chop and tape-decode-rust
(`docs/DEV_NOTE_linux_appimage_taskbar_integration.md` in those repos).

## Direct native RAW recording (CXADC passthrough) and RAW naming

When the selected device is a running CXADC capture, FLAC compression is
off, and a channel's resampler is off, that channel records *direct*: the
output file is the card's native byte stream — unsigned `.u8` bytes in
8-bit mode, unsigned `.u16` words in tenbit mode — byte-for-byte what
`dd if=/dev/cxadcN` would produce. Everything else keeps the converting
writer (signed samples, `.s8`/`.s16`).

### Data flow (direct channels)

    card read()                                gui_cxadc.c: cxadc_capture_thread
      -> gui_record_direct_tap_push(card,..)  gui_record.c; per card, immediately
                                               after each successful read(),
                                               BEFORE the A/B pairing
      -> gui_record_direct_push(ctx,..)        gui_record_direct.c:
           BUF_RECORD_A/B (2 attempts x 1 ms,  same backpressure as extraction,
           then spill, then logged drop)       or the spill file while a
                                               backlog exists
      -> gui_record_direct_writer_thread       gui_record_direct.c: ring first
           (oldest data), then spill, any     length up to 2 MiB per read
      -> fwrite unchanged -> .u8/.u16 file

The tap sits before the A/B pairing deliberately: the pairing truncates both
reads to the shorter one and drops card A's data entirely when card B
returns 0, and tenbit odd trailing bytes are truncated — the direct path
must not inherit any of that. The display chain is unchanged (the waveform
still decodes); only the recording chain is byte-exact. Extraction skips
`BUF_RECORD_*` writes for direct channels
(`gui_extract_set_recording(..., direct_a, direct_b)`), so each record ring
has exactly one producer.

### Eligibility (per channel, decided at record start)

`gui_record_direct_channel_eligible(app, channel)` in `gui_record.c` —
all must hold:

- the selected device is a `DEVICE_TYPE_CXADC` entry (single-card or
  2-card Clockgen) and the CXADC capture is running;
- `use_flac` is off (RAW output);
- that channel's resampler is off (`enable_resample_a/b`);
- that channel's capture is on;
- the card behind that channel (card index == channel) delivers samples of
  the requested RAW width: `gui_cxadc_direct_record_available()` checks the
  live tenbit mode against `rf_bits` (8-bit mode -> u8 bytes, tenbit ->
  u16 words).

The same predicate drives file naming (`gui_record_apply_auto_names`), the
capture log and writer/tap selection, so filename, log and on-disk content
always agree. Mixed mode works: channel A direct while channel B resamples
(B converts to `.s8/.s16`).

### Naming by content

`raw_ext_for_bits(bits, direct)` in `gui_record.c`: direct -> `.u8`/`.u16`
(truly unsigned, byte-exact), converted -> `.s8`/`.s16`. The load-time
preview copy in `gui_settings.c` maps converted only (it has no device
knowledge at load; record start recomputes the authoritative name). (The
`.u8/.u16` naming introduced in v1.2.3 previously covered converted files
too; those are `.s8/.s16` from this change on.)

### Ordering contract (why the two rules)

Two rules keep the byte stream in order under backpressure:

- Producer: while the spill backlog is non-zero, new bytes go to the spill
  (queuing behind the not-yet-drained spill data).
- Writer: drain the ringbuffer first, then the spill.

Together they hold the invariant "ring data is always older than spill
data": the ring only accumulates while the backlog is zero; once a push
spills, every later push appends to the spill until it is fully drained.
The legacy path violates this (it retries the ring as soon as any space
appears while older data still sits in the spill), which can reorder
blocks. Empirical oracle: the unit harness pushes a deterministic pattern
larger than the ring while the writer drains concurrently — any reordering
shows up as a byte mismatch.

Other semantics:

- Backpressure: identical to the extraction record path (2 attempts x 1 ms,
  then the spill temp file, then a logged drop; a drop mirrors the
  extraction drop handling including optional stop_on_dropout).
- Variable-length reads: the writer reads whatever is available (capped at
  2 MiB), so the final partial block at stop is written too — the legacy
  fixed-block RAW writer lost up to one block (~52 ms of 16-bit/20 MSPS
  data) at every stop.
- Write errors (locked output file): the failed block is held in memory (it
  is the oldest unread data) and retried every ~2 s; nothing is read while
  it is held, so the stream cannot reorder across the outage. A bounded
  final attempt happens at stop (exactly one attempt, never a hang at
  join); if it fails the held block is lost — the same semantics as the
  legacy writer stopping with a locked output.

### Start/stop handshake

- Start (RAW branch of `gui_record_start_confirmed`): writer threads
  started -> 10 ms -> taps enabled (`gui_record_direct_tap_enable`) ->
  `gui_extract_set_recording(true, false, bits, direct_a, direct_b)`.
- Stop (`gui_record_stop`): taps disabled and waited idle
  (`gui_record_direct_tap_disable` + `gui_record_direct_tap_wait_idle`)
  BEFORE `gui_extract_set_recording(false, ...)` and BEFORE `is_recording`
  clears, so the writers' final drain can never race a producer still
  pushing. Tap handshake itself: the producer marks itself in-flight, then
  reads the enabled flag; the stopper clears the flag, then waits for
  in-flight == 0.

### Logging, status and accounting

- Capture log: `capture_format: RAW direct native passthrough` (vs
  `RAW converted`) plus a per-channel `RAW output: A=direct native u16
  B=converted s16` line.
- Status bar: `Recording (RAW direct)...`.
- End-of-recording summary: `Direct RAW channel A/B: bytes_read=...
  bytes_written=...`, with `(MISMATCH: data was dropped or left in the
  spill backlog)` appended when they differ.
- Accounting: `recording_raw_a/b` and `recording_bytes` count bytes actually
  written, so the readout, the stat()-based on-disk readout and the capture
  log all match the real file size.

### Verification

- Unit (`meson test gui_record_direct`; `test/gui_record_direct_harness.c`
  drives the real producer + writer through a real ringbuffer, 56 checks):
  tap handshake; spill fallback once the ring is full; byte-identical output
  over 3x2 MiB + odd tail in mixed chunk sizes 1..999999 bytes (proves
  ordering + tail drain); per-channel accounting; write-error recovery via
  unbuffered `/dev/full` then a swapped-in output (held block first, then
  ordered drain).
- Guards: `ci_guard_tests.py` `raw direct passthrough contract` (static)
  and `direct RAW writer runtime` (compiles + runs the harness).
- Hardware (2026-10-02, 2x CXADC + clockgen rig, 36-core, unattended
  `--auto-record 15` under 24 CPU-load workers): both channels direct,
  599,785,472 bytes each = bytes_read = bytes_written = on-disk size,
  waits=0 drops=0, 39.99 MB/s per channel (true 40 MSPS) on both channels
  concurrently; `.u8` means matched a live `dd if=/dev/cxadc0` read
  (file 126.0 / 126.1 vs card 126.0 — a mislabeled-converted file would
  read ~254 at this DC, i.e. `b-128` wrapped).

## Automated local load + capture testing (CLI -> GUI)

The GUI runs unattended from a terminal for local automated load/capture
testing — no UI clicking and no server/client setup. (The net route stays
available: `--auto-connect` with a Server/Client config, see
`misrc_tools/test/cxadc_remote_capture_ci.sh`; the CLI-side loop suite is
`misrc_tools/test/capture_stability_ci.sh <capture_bin> <extract_bin>
<dir>`.)

### Flags (`misrc_gui --help`)

- `--select-device <index-or-name>` — pick the capture device at launch.
  `<sel>` is either the device-list index (`0`, `1`, ...) or a
  case-insensitive substring of the device name (first match wins, e.g.
  `"CXADC Clockgen"`, `"MS2130"`, `"Simulated"`). Applied after enumeration,
  overriding the hsdaoh > MISRC Clockgen > CXADC auto-selection — required
  for automated runs because that preference otherwise picks the MS2130
  stick on a mixed rig. No match ->
  `[AUTO] --select-device: no device matches '<sel>'` and exit 1.
- `--auto-capture` — arm an unattended capture start ~1 s after launch
  (same reconnect-pending mechanism `--auto-connect` uses in server mode,
  but Local mode and no `--config` required). Use it alone for a
  streaming/load run with no output files.
- `--auto-record <seconds>` — full unattended record cycle; implies the
  auto-capture arming. Phase machine (in the main loop, every transition
  logged with an `[AUTO]` prefix):
  1. wait for capture to start (30 s budget, else failure);
  2. settle 2 s (data flowing) -> start recording. `RECORD_PENDING` (the
     overwrite popup is open) is a failure with an explicit hint: set
     `overwrite_files: true` in the config or use timestamped auto-names;
  3. record until the deadline -> stop recording;
  4. wait for the finalize (writer threads joined, files closed) ->
     stop capture;
  5. clean exit: 0 on a completed cycle, 1 if any phase failed.
- The settings `overwrite_files` toggle ("Overwrite output files") now
  actually skips the overwrite confirmation popup in `gui_record_start` —
  the toggle existed in the UI but was ignored before; without it an
  unattended run stalls on that popup (timestamped auto-names also avoid
  it).

### `[AUTO]` markers (for scripted verification)

    [AUTO] --select-device: <idx>: <name>
    [AUTO] auto-capture armed (device <idx>: <name>)
    [AUTO] capture running; settling 2 s before recording
    [AUTO] recording started (<n> s)
    [AUTO] recording stopped after <n> s
    [AUTO] capture stopped
    [AUTO] record cycle complete[ (with errors)]; exiting
    [AUTO] ERROR: ...   (failure detail; exit code 1)

A backgrounded GUI must be `wait`ed on (or `nohup`/`setsid` detached) or the
launching shell's exit SIGHUPs it — the example's `wait $GUIPID` covers
that.

Copy-paste example — direct native RAW capture on the CXADC Clockgen entry
under CPU load (uses the real X display; wrap the command in `xvfb-run -a`
for a throwaway headless X server):

```bash
# 1) One-off output dir + config (FLAC off, no resample, 8-bit, both cards)
mkdir -p ~/misrc-autotest
cat > ~/misrc-autotest/settings.config <<'EOF'
{
  "output_path": "/home/YOU/misrc-autotest",
  "auto_names_enabled": true,
  "output_base_name": "directtest",
  "append_timestamp_on_capture_start": true,
  "rf_bits_a": 8,
  "rf_bits_b": 8,
  "cxadc_tenbit_mode_a": false,
  "cxadc_tenbit_mode_b": false,
  "capture_a": true,
  "capture_b": true,
  "use_flac": false,
  "enable_resample_a": false,
  "enable_resample_b": false,
  "stop_on_dropout": false,
  "level_autostop_enabled": false,
  "overwrite_files": true
}
EOF

# 2) Unattended 15 s record run on CXADC, with CPU load while it records
GUI=./build-local/misrc_gui
$GUI --config ~/misrc-autotest/settings.config \
     --select-device "CXADC Clockgen" --auto-record 15 \
     >~/misrc-autotest/gui.log 2>&1 &
GUIPID=$!
# keep most cores busy while it records (scale to your core count)
for i in $(seq 1 24); do timeout 25 sha256sum /dev/zero >/dev/null & done
wait $GUIPID; echo "gui exit: $?"   # 0 = full cycle completed

# 3) Verify the direct native RAW output
grep -E "capture_format|RAW output|Direct RAW channel" ~/misrc-autotest/*_misrc_capture.log
ls -l ~/misrc-autotest/*.u8
# bytes_read == bytes_written == file size proves ordering + no loss;
# the file mean must match the card's own live DC (unsigned native bytes)
dd if=/dev/cxadc0 bs=64k count=64 2>/dev/null | \
  python3 -c "import sys; d=sys.stdin.buffer.read(); print('card mean %.1f' % (sum(d)/len(d)))"
python3 - <<'EOF'
import glob
for f in sorted(glob.glob('/home/YOU/misrc-autotest/*.u8')):
    with open(f, 'rb') as fh:
        d = fh.read(4 << 20)
    print(f, 'file mean %.1f' % (sum(d) / len(d)))
EOF
```

Notes:

- A resample-on run of the same config produces `.s8/.s16` (converted) —
  flip `enable_resample_a/b` to `true` and set `resample_rate_a/b` to A/B
  the naming and converted output.
- The same flags drive any device: `--select-device 0` (list index) or a
  substring like `--select-device MS2130`.

### Validated on real hardware (2026-10-02)

The exact example above, on the 2x CXADC + clockgen rig (36-core, 24
sha256sum load workers, 15 s record): `GUI_EXIT=0`, both channels direct,
599,785,472 bytes per channel = bytes_read = bytes_written = on-disk size,
waits=0 drops=0, 39.99 MB/s per channel (true 40 MSPS) on both channels
concurrently under load, `.u8` means matching a live `dd if=/dev/cxadc0`
read (126.0). Tier-snap and rate-check lines:
`[CXADC] rate check: card 0/1 measured 40.000 MHz, sysfs 40.000 MHz
(agree)` at startup and again at capture start.

## CXADC measured feed-rate check (sysfs crystal can lie)

sysfs `crystal`/`tenxfsc` report what the cxadc driver *believes*. A
hardware-modded card (e.g. a 40 MHz oscillator swap) can still carry the
stock 28.636 MHz `crystal` parameter — reported by a user with a single
40 MHz-modded card: resample cycle options capped at 28.6, which is only
correct if the sysfs is right. The GUI therefore *measures* the feed rate
by timing a real blocking read from `/dev/cxadcN`.

### Algorithm (`cxadc_measure_card_rate_hz`, gui_cxadc.c) — drain, then time

1. **Drain the backlog until the live stream is detected.** The card
   free-runs since boot/driver load, so the driver holds a backlog that
   reads return far faster than the live feed — a naive timed window over
   the backlog reports a bogus rate (observed on a real 40 MSPS card:
   50-72 MHz "measurements"). The probe reads 256 KiB chunks until one
   chunk takes as long as a live one — `>= 4 ms` (256 KiB at <= 64 MB/s;
   every real tier feeds at <= 54 MB/s so a live chunk takes >= 4.8 ms,
   a buffered chunk far less) — bounded by 1 GiB / 3 s caps.
2. **Time the all-live window.** 2 MiB at the startup probe, 4 MiB at
   capture start; `rate = bytes / elapsed / bytes-per-sample`, requiring
   >= 20 ms elapsed and sanity `0 < rate <= 100 MSPS`.

The measured rate is snapped to a known tier (2% tolerance) from
`misrc_tools/common/cxadc_rate_tiers.h`:

    14.318 (stock 10-bit)   17.898 (stock 10-bit tenxfsc=1)
    20 (40-mod 10-bit)      27 (54-mod 10-bit)
    28.636 (stock crystal)  40 (40 MHz mod / clockgen)   54 (54 MHz mod)

35.8 MHz (35795454 Hz = 28.636 x 10/8, the upsampled 8-bit tenxfsc=1 feed)
is deliberately NOT a tier: that mode's sysfs presentation is the crystal
rate, and an unsnapped measurement falls back to sysfs.

Failures are conservative: live stream not reached within the caps,
measurement failed, or no tier match -> the measurement is negatively
cached for the session (per card, per tenbit mode) and the sysfs-derived
rate stays — never a wrong override.

### Probe points and consumers

- Startup probe: `gui_cxadc_probe_card_rates()` from device enumeration,
  once per (card, mode) per session. Skipped while a CXADC capture is
  running, and via `MISRC_GUI_NO_CXADC_RATE_PROBE=1` (it briefly reads the
  cards — it must not run while another process is capturing on them).
- Capture-start probe: `gui_cxadc_start`, per card, using the already-open
  fds, after tenbit modes are applied and before the RF thread starts; the
  measured tier overrides `card_sample_rate_hz[i]` and refreshes
  `rf_sample_rate_hz`, so the rate readout, the resample options and the
  direct RAW width check all use the measured truth.
- `gui_cxadc_get_effective_rate_hz()` (measured tier if cached, else
  sysfs) is used by the UI rate getters (`gui_ui_cxadc_base_rate_khz`,
  `gui_ui_cxadc_hw_rate_for_tenbit` — the resample cycle max), the CXADC
  settings-profile sync in `gui_capture.c`, and `gui_cxadc_start` — so the
  resample cycle options follow the measured rate (a 40-mod card gets the
  `5/10/20/40` preset set instead of the stock `5/14.3/17.9/28.6`).

### Log lines

    [CXADC] rate check: card 0 measured 40.000 MHz, sysfs 40.000 MHz (agree)
    [CXADC] rate check: card 0 sysfs says 28.636 MHz but the card feeds 40.000 MHz
                   (measured) - using the measured rate (stale sysfs crystal
                   parameter on a modded card?)          [+ a status-bar line]
    [CXADC] rate check: card 0 measured 50.320 MHz matches no known rate tier;
                   using the sysfs-derived rate
    [CXADC] rate check: card 0 could not reach the live stream while draining
                   the backlog; using the sysfs-derived rate

### Verification

- Unit: `meson test cxadc_rate_tiers` (`test/cxadc_rate_tiers_test.c`, 17
  checks — jittery-but-known feeds snap; unknown/ambiguous rates and the
  excluded 35.8 do not).
- Guard: `ci_guard_tests.py` `CXADC rate probe contract` (probe wiring,
  drain phase, tier exclusion, effective-rate consumers, capture-start
  ordering).
- Hardware (2026-10-02, 2x CXADC + clockgen rig): the first attempt
  measured the driver backlog (72.5/61.5/50.3 MHz -> no tier -> sysfs kept,
  capture unaffected) — which is exactly what motivated the drain phase;
  after the drain fix both cards measured exactly `40.000 MHz, sysfs
  40.000 MHz (agree)` at startup and again at capture start. The mismatch
  path (stale sysfs 28.6 -> measured 40 override) is pinned by the tier
  unit test; a genuinely stale-sysfs card (the reporting user's machine)
  is the final end-to-end confirmation.

## GUI Readout + Stats Breakdown

This section documents what the GUI stats/readouts show, where each value comes
from, and what the wait/drop counters actually mean.

### Scope

- Focus is the live GUI readout/counter behavior in `misrc_gui`.
- Definitions below are based on current code paths in:
  - `misrc_tools/misrc_gui/ui/gui_ui.c`
  - `misrc_tools/misrc_gui/input/gui_capture.c`
  - `misrc_tools/misrc_gui/processing/gui_extract.c`
  - `misrc_tools/misrc_gui/output/gui_record.c`
  - `misrc_tools/common/buffer_manager.c`

### Data flow (where counters are produced)

- Capture callback writes raw RF into `BUF_CAPTURE_RF`.
- CXADC direct channels: the capture thread's tap also pushes the card's
  native bytes into `BUF_RECORD_A/B` (see "Direct native RAW recording"
  above); extraction skips those channels while direct.
- Extraction thread reads `BUF_CAPTURE_RF`, updates sample/clip/peak stats, writes:
  - display frames to `BUF_DISPLAY`
  - recording data to `BUF_RECORD_A` / `BUF_RECORD_B` (only while recording,
    and only for channels not recorded direct)
- Writer threads drain `BUF_RECORD_A/B` and update recording byte counters
  (FLAC/converted RAW writers, or the byte-exact direct writer for direct
  channels).

### Bottom status bar readouts

Rendered in `render_status_bar()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `REC dot + HH:MM:SS`
  - Shown only while recording.
  - Time is `GetTime() - recording_start_time`.
- Status text (when not recording)
  - Shows `app->status_message`.
- `Sync: OK` / `Sync: --`
  - Uses `stream_synced`.
- `XX MSPS`
  - From `sample_rate` displayed as integer `sample_rate / 1000000`.
- `Samples`
  - Uses `samples_a` (channel A sample counter).
  - Formatted as raw count with `K/M/G` suffixes.
- `Frames`
  - Uses `frame_count`.
- `Missed`
  - Uses `missed_frame_count`.
  - This counter is debounced in GUI capture callback logic: isolated single miss events are suppressed, and only persistent/consecutive miss conditions increment it.
- `Errors`
  - Uses total `error_count`.
  - This counter is debounced in GUI capture callback logic and tracks persistent parser-error events rather than summing every per-line parser error value.
- `RF Buffer`
  - Percent fill computed from `BUF_CAPTURE_RF` ringbuffer head/tail.
- `Audio Buffer`
  - Percent fill computed from `BUF_CAPTURE_AUDIO` ringbuffer head/tail.

### Side channel stats panels (CH A / CH B)

Rendered by `render_channel_stats()` in `misrc_tools/misrc_gui/ui/gui_ui.c`.

- `Peak: +X% -Y%`
  - Based on `vu_*.peak_pos/peak_neg` (VU peak-hold values, not instantaneous raw ADC values).
  - Source peaks are derived from extraction stats and then smoothed/held in `gui_app_update_vu_meters()`.
- `Clip: +N -M`
  - Cumulative clip counts from extraction thread:
    - positive clip when sample `>= 2047`
    - negative clip when sample `<= -2048`
- `RST` button
  - Clears that channel's clip counters only.
- `Errors`
  - Displays `error_count_a` / `error_count_b`.
  - Current code resets these counters at capture start but does not increment them in active processing paths, so they remain `0` unless future wiring is added.
- During recording only:
  - `RAW: X MB`
    - From `recording_raw_a` / `recording_raw_b`.
  - `FLAC: Y MB` (FLAC mode only)
    - From `recording_compressed_a` / `recording_compressed_b`.
  - `Ratio: Zx` (FLAC mode only)
    - `raw_bytes / compressed_bytes`.

### Record counter placement

- Recording duration counter is in the bottom bar (left side), next to the red record indicator.
- Side channel panels carry per-channel recording size stats (`RAW/FLAC/Ratio`), not the global timer.

### Wait/drop counters: exact meaning

Wait/drop are backpressure metrics tied to ringbuffer write behavior.

#### Buffer-manager definition

In `bufmgr_write_begin()` (`misrc_tools/common/buffer_manager.c`):
- `write_waits` increments when producer must wait for space.
- `write_drops` increments when write is dropped (immediate-drop policy or retries exhausted).

So:
- **wait** = write had to pause because buffer was full.
- **drop** = write could not be queued and was discarded.

#### Policies by path

Default policies in `misrc_tools/common/buffer_manager.c`:
- `BUF_CAPTURE_RF`: wait up to 10 attempts × 5 ms, then drop.
- `BUF_CAPTURE_AUDIO`: immediate drop (no waiting).
- `BUF_RECORD_A/B`: default 200 × 5 ms, but GUI extract record path overrides this.
- `BUF_DISPLAY`: short wait, then drop (display is intentionally lossy).

GUI capture callback overrides in `misrc_tools/misrc_gui/input/gui_capture.c` (aligned to CLI timing):
- RF callback writes use `8 attempts × 1 ms`.
- Audio callback writes use `8 attempts × 1 ms`.

Record-path override in `misrc_tools/misrc_gui/processing/gui_extract.c`:
- `s_record_write_policy = 2 wait attempts x 1 ms timeout, then drop` (a very
  short wait window so a single scheduler hiccup does not force spill mode).
- The CXADC direct tap uses the same policy (`s_direct_write_policy` in
  `gui_record_direct.c`): full buffer -> brief wait -> spill temp file ->
  logged drop (see "Direct native RAW recording" above).
- Recording writes stay effectively non-blocking for extraction; when
  record buffers stay full, record frames spill to disk and only spill
  failure drops them.

#### Counters used by GUI app state

`gui_app_t` has:
- `rb_wait_count`
- `rb_drop_count`

Current behavior:
- `rb_wait_count` and `rb_drop_count` are updated from `BUF_CAPTURE_RF` buffer-manager write stats deltas each callback.
- In upstream mode, `rb_drop_count` can also be incremented by parsed hsdaoh overrun messages.

#### Where wait/drop is visible today

- Capture stop log (`gui_app_stop_capture()`):
  - prints `waits` and `drops` from app-level counters.
- Recording stop logs (`gui_record_stop()`):
  - prints recording-session wait/drop totals computed from `BUF_RECORD_A/B` deltas.
  - also prints per-buffer `A` and `B` wait/drop deltas.
- Periodic debug log from buffer manager:
  - one-line per-buffer fill/wait/drop summary.

### Rawness and interpretation notes

- Most counters are monotonic event counts since capture start.
- `Missed` is an event count ("missed at least one frame" events), not an exact per-frame-loss total.
- `Missed` and `Errors` are intentionally debounced in GUI capture mode to avoid one-off transient spikes from dominating the UI readout.
- GUI capture also applies a callback-gap resync guard: if callback timing stalls for >100 ms (for example due to system/display interruptions), parser sync state is reset before continuing so stale parser state does not generate a long burst of follow-on errors.
- `Peak` in side panels is VU peak-hold representation, not raw unsmoothed instantaneous sample.
- Buffer percentages are instantaneous snapshot values.

## Capture / regression development notes

In-tree capture-path constraints and historical regression notes (macOS
scheduling, parser CRC, tolerated-frame behavior, etc.) live in:

    misrc_tools/misrc_gui/dev/dev_notes_README.md

Key constraints to preserve when touching capture/parser/audio paths:

- Preserve tolerated-frame behavior in MISRC frame mode: only drop frames when
  `result.error_count > 0 && result.report_errors`. Do not reject tolerated
  CRC-only frames, or GUI RF feed can stall while CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null
  checks), before width/height early returns. This prevents false
  timeout/reconnect loops when callback activity exists.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start,
  explicitly restore audio capture state:
  `atomic_store(&s_capture_handler.capture_audio, true);`
  Without this, the audio monitor path (`stream1 -> BUF_CAPTURE_AUDIO ->
  gui_audio`) remains empty.
- Validate RF and monitor audio as separate end-to-end checks after
  capture-path edits:
  - RF: waveform/scope feed present and stable.
  - Audio monitor: `Audio Mon` audible and `BUF_CAPTURE_AUDIO` no longer pinned at 0%.
- Prefer minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`,
  and `gui_audio`; avoid unrelated UI/settings churn during capture debugging.
