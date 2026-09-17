# CXADC-Win (Windows driver) — lock-step dev notes

This file tracks the work needed to bring the Windows CXADC-Win PowerShell
driver path to parity with the Linux cxadc rate-detection / cycle-mode work
landed in Sept 2026. It is a living document: update it as modes are tested
and wired on Windows.

## Current state (as of 2026-09-17)

- **Linux**: `gui_cxadc_get_sample_rate_hz()` reads `crystal` + `tenxfsc`
  from sysfs and derives the real hardware rate (stock 28.6, 40 mod, 54 mod,
  10-bit halves). The UI base-rate helper and the rate-box cycle use this.
  10-bit sysfs write permission denial falls back to 8-bit with a help popup.
- **Windows**: `gui_cxadc_get_sample_rate_hz()` returns `false` (line ~491
  of `gui_cxadc.c`). The CxadcWin PowerShell module (`Get-CxadcWinConfig` /
  `Set-CxadcWinConfig`) exposes `EnableTenbit` and `CenterOffset` but does
  **not** expose `crystal` or `tenxfsc`. So the Windows path always uses
  the 40/20 MSPS clockgen baseline and never detects a stock 28.6 card.

## What works on Windows today

- `cxadc_win_get_tenbit` / `cxadc_win_set_tenbit` — 10-bit toggle via
  `Get/Set-CxadcWinConfig -EnableTenbit`.
- `cxadc_win_get_center_offset` / `cxadc_win_set_center_offset` — DC
  offset via `Get/Set-CxadcWinConfig -CenterOffset`.
- `cxadc_open_card` — `CreateFileA("\\.\cxadcN")`.
- WASAPI clockgen audio capture (same path as Linux ALSA).
- 10-bit permission fallback: the `EACCES/EPERM` path in
  `cxadc_apply_tenbit_modes` is Linux-only (`#if !defined(_WIN32)`). On
  Windows, a `Set-CxadcWinConfig` failure returns -1 and aborts capture.
  **This needs a Windows equivalent** (PowerShell execution policy or
  module-not-installed error → fall back to 8-bit + help popup).

## What does NOT work on Windows (lock-step items)

### 1. Rate detection (`gui_cxadc_get_sample_rate_hz`)
- **Blocker**: CxadcWin has no `crystal` / `tenxfsc` config property.
- **Options**:
  - (a) Add a `Get-CxadcWinConfig -SampleRate` (or `-CrystalClock`)
    property to the CxadcWin PowerShell module upstream, then read it
    here like the Linux sysfs path.
  - (b) If CxadcWin always runs at a fixed rate (no crystal mod support),
    document that and hardcode the Windows baseline to 40/20 (current
    behavior, just make it explicit instead of "detection failed → fallback").
  - **Recommended**: (b) for now (CxadcWin cards are clockgen-driven),
    with a TODO for (a) if crystal-modded Windows cards ever appear.

### 2. 10-bit permission fallback
- **Blocker**: `cxadc_apply_tenbit_modes` has `#if !defined(_WIN32)` around
  the `EACCES/EPERM` fallback. On Windows, a `Set-CxadcWinConfig` failure
  (e.g. module not installed, PowerShell execution policy) returns -1 and
  aborts capture.
- **Fix needed**: add a Windows branch that detects the failure reason
  (PowerShell error, module missing) and falls back to 8-bit + sets
  `cxadc_perm_help_pending` with Windows-appropriate instructions
  (install CxadcWin module, set execution policy).

### 3. Rate-box cycle presets
- **Current**: `gui_ui_cxadc_cycle_rate` and `cycle_resample_khz` use
  `gui_ui_cxadc_hw_rate_khz` which calls `gui_cxadc_get_sample_rate_hz`.
  On Windows this returns false → falls back to 40/20. So the cycle works
  but always assumes clockgen (never stock 28.6).
- **Fix**: once rate detection is wired (item 1), the cycle inherits it
  automatically. No separate change needed in the cycle code.

### 4. CXADC-Win driver version detection
- **Needed**: a way to detect which CxadcWin driver version is installed
  (e.g. `Get-Module CxadcWin | Select-Object Version`) so the GUI can
  warn if the installed version is too old for a feature (e.g. 10-bit,
  center offset, audio). This is a Windows-only startup check.
- **Where**: `gui_cxadc_detect_cards` on Windows could probe the module
  version and log/warn if below a minimum.

## Lock-step plan

1. **Document** Windows = 40/20 baseline explicitly (option 1b above).
   Remove the "detection failed" framing; Windows is always clockgen-rate.
2. **Add Windows 10-bit fallback**: detect `Set-CxadcWinConfig` failure,
   fall back to 8-bit, set `cxadc_perm_help_pending` with Windows
   instructions.
3. **Test** with a real CxadcWin card on Windows (much more testing
   needed — driver version, PowerShell policy, audio capture).
4. **Future**: if CxadcWin adds crystal/tenxfsc properties, wire
   `gui_cxadc_get_sample_rate_hz` for Windows like the Linux sysfs path.

## Testing checklist (Windows CXADC-Win)

- [ ] Stock CxadcWin card (no mod): rate shows 40 MSPS, 8-bit default
- [ ] 10-bit toggle: `Set-CxadcWinConfig -EnableTenbit $true` succeeds →
      rate shows 20 MSPS
- [ ] 10-bit toggle with module not installed: falls back to 8-bit +
      help popup with Windows instructions
- [ ] 10-bit toggle with PowerShell execution policy restricted: falls
      back to 8-bit + help popup
- [ ] Rate-box cycle: HW 40 → HW 20 → SW rates → HW 40
- [ ] DC offset adjust: `Set-CxadcWinConfig -CenterOffset` readback
- [ ] WASAPI clockgen audio: audio capture opens, 3ch + headswitch
- [ ] CxadcWin driver version detection + warning if too old
- [ ] Server/client remote capture with CxadcWin card as server
- [ ] Client (Linux) connecting to CxadcWin server: RF feed flows
