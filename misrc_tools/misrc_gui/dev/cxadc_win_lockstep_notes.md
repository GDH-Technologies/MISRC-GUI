# CXADC-Win (Windows driver) — lock-step dev notes

This file tracks the work needed to bring the Windows CXADC-Win PowerShell
driver path to parity with the Linux cxadc rate-detection / cycle-mode work
landed in Sept 2026. It is a living document: update it as modes are tested
and wired on Windows.

## Current state (as of 2026-10-22)

- **Linux**: `gui_cxadc_get_sample_rate_hz()` reads `crystal` + `tenxfsc`
  from sysfs and derives the real hardware rate (stock 28.6, 40 mod, 54 mod,
  10-bit halves). The UI base-rate helper and the rate-box cycle use this.
  10-bit sysfs write permission denial falls back to 8-bit with a help popup.
- **Windows**: `gui_cxadc_get_sample_rate_hz()` returns `false`. The GUI now
  talks WMI directly (`ROOT\WMI` classes `CxadcWin_Path` / `CxadcWin_Config`)
  instead of shelling out to `Import-Module CxadcWin` — the PowerShell detour
  never worked: it launched Windows PowerShell 5.1 while the module requires
  pwsh 7.4+ Core, and the module is not importable by name unless installed.
  The upstream psm1 also has a `PciBusAddress` vs MOF `PciAddress` mismatch
  in `Get-CxadcWinDevice`. See the "What works on Windows today" section.

## What works on Windows today (post 2026-10-22 WMI rework)

- `cxadc_win_get_tenbit` / `cxadc_win_set_tenbit` — 10-bit toggle via direct
  WMI (`CxadcWin_Path` lookup + `CxadcWin_Config.EnableTenbit` PutInstance,
  with readback verification). No PowerShell involved.
- `cxadc_win_get_center_offset` / `cxadc_win_set_center_offset` — DC offset
  via direct WMI (`CxadcWin_Config.CenterOffset`), readback verified.
- `gui_cxadc_center_offset_max()` — 63 on Windows (driver
  `CX_CTRL_CONFIG_CENTER_OFFSET_MAX`), 255 on Linux; the UI DC-offset raw
  range and the setters clamp to it.
- `cxadc_open_card` — `CreateFileA("\\\\.\\cxadcN")`.
- WASAPI clockgen audio capture (same path as Linux ALSA).
- 10-bit permission fallback: cross-platform. WMI `WBEM_E_ACCESS_DENIED`
  maps to `EACCES` and takes the same fallback-to-8-bit + help-popup path
  as the Linux sysfs EACCES case (Windows popup text: re-run as admin).

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

### 2. 10-bit permission fallback — DONE (2026-10-22)
- `cxadc_apply_tenbit_modes` now has a Windows branch: WMI
  `WBEM_E_ACCESS_DENIED` → `EACCES` → fall back to 8-bit, revert the card's
  tenbit settings/rf_bits, set `cxadc_perm_help_pending` with the Windows
  popup ("re-run as Administrator"). Non-permission failures still abort
  capture with a status line + errno in the stderr log.

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
