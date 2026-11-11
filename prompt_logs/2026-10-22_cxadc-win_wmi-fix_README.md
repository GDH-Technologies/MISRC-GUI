# Prompt log — CXADC-Win (Windows) support fix — 2026-10-22

## User input (prompt 1)
> git pull and start trying to fix up the messy CXADC-Win Support - git clone
> https://github.com/JuniorIsAJitterbug/cxadc-win and also the driver is
> installed locally but no cards

## User input (prompt 2)
> This is a windows only support mode, currently users see the cards detected
> but get an failed to apply tenbit mode error

## Commands run + hard findings

### Repository state
- `git -C C:\Users\Harry\MISRC-GUI pull --no-rebase` — fast-forward `2236220` →
  `72b33bd`, 45 files changed (8,574+/1,172-), tag `v1.2.6` fetched.
- `git clone https://github.com/JuniorIsAJitterbug/cxadc-win C:\Users\Harry\cxadc-win`
  — failed: path already exists. Existing clone is current at `v1.3`
  (`bbfc930 Add Import-Module examples to README`), `git pull` there = up to date.

### Local machine driver/hardware state (verified hard data)
- `Get-PnpDevice` (VEN_14F1): 4 devnodes — TWO cards' video functions
  (`DEV_8800`, FriendlyName "cxadc-win device", class System, service
  `cxadc-win`, driver version `13.0.46.385`) + their audio functions
  (`DEV_8804`, "Multimedia Controller", no class = unbound).
  - `PCI\VEN_14F1&DEV_8800&SUBSYS_45534E4F&REV_05\7&1D1438DF&0&000030020A`
  - `PCI\VEN_14F1&DEV_8800&SUBSYS_45534E4F&REV_05\7&25390026&0&000038020A`
- `Get-PnpDeviceProperty`: `DEVPKEY_Device_IsPresent = False` on BOTH 8800
  devnodes, no problem code. → The cards are NOT currently enumerated on this
  machine's PCI bus (devnodes are stale registry entries from when the
  driver was installed). GUI's `gui_cxadc_detect_cards` (CreateFileA
  `\\.\cxadcN`) correctly returns 0 → "no cards" locally.
- `pnputil /enum-devices /connected | Select-String 14F1` — no matches (confirms
  no Conexant PCI devices currently connected).
- `Get-CimInstance -Namespace root\wmi -ClassName CxadcWin_Path` — empty
  (driver not running, so no live WMI instances; consistent with no cards).
- `bcdedit /enum {current}` testsigning check — no output (needs admin to read).

### Root cause of "failed to apply tenbit mode" (code + upstream analysis)
GUI path: `gui_cxadc_start` → `cxadc_apply_tenbit_modes` →
`gui_cxadc_set_tenbit` → `cxadc_win_set_tenbit` (gui_cxadc.c) which shells out:
`powershell -NoProfile -ExecutionPolicy Bypass -Command "…Import-Module
CxadcWin -ErrorAction Stop; Set-CxadcWinConfig -Path '\\.\cxadcN' -EnableTenbit
$true…"` via `_popen`. Status line set: "CXADC: failed to apply tenbit mode".

This cannot work, for three stacked reasons:
1. `powershell` = Windows PowerShell 5.1 (Desktop edition). The CxadcWin module
   declares `#Requires -Version 7.4` + `#Requires -PSEdition Core`
   (src/driver/CxadcWin.psm1:29-30) → Import-Module ALWAYS fails under 5.1.
2. `Import-Module CxadcWin` (by name) only resolves if the module is installed
   into a PSModulePath; the cxadc-win release ships loose files (INF/SYS/PSM1),
   so users typically don't have it importable by name at all.
3. Even under pwsh 7.4+: `Get-CxadcWinDevice` reads `$Hw.PciBusAddress`
   (CxadcWin.psm1:91) but the MOF class `CxadcWin_Hardware` defines the
   property as `PciAddress` (src/driver/cxadc-win.mof) → the Location format
   string throws → `Get-CxadcWinConfig -Path` fails upstream of Set.

Upstream truth (driver source, verified):
- Config is plain WMI: `ROOT\WMI` classes `CxadcWin_Path` (Path ↔ InstanceName),
  `CxadcWin_Config` (VideoMux/Level/EnableTenbit/EnableSixDB/CenterOffset, all
  `[read, write]`), `CxadcWin_State`, `CxadcWin_Registers`, `CxadcWin_Hardware`.
- Driver WMI provider implements full-block set (`cx_wmi_config_set`) AND
  per-item set (`cx_wmi_config_set_item`) → PutInstance writes are supported
  (this is what the PowerShell `Set-CimInstance` path uses).
- `CX_CTRL_CONFIG_CENTER_OFFSET_MAX = 63` (src/shared/cx_ctl_codes.h) — the
  Windows driver's center-offset range is 0–63, while the GUI clamps 0–255
  (Linux sysfs range). Values 64–255 are rejected with
  STATUS_INVALID_PARAMETER.

→ Fix direction: talk WMI directly from C (COM: IWbemLocator → ROOT\WMI),
drop the PowerShell detour entirely; add the Windows 10-bit permission
fallback (lock-step item 2 in cxadc_win_lockstep_notes.md); fix the DC-offset
range; add Windows-specific popup/status texts.

## Work performed (this session)
See "Fix implementation" section below — updated as work completes.

## Verification status
- Local end-to-end test IMPOSSIBLE on this machine: no CX cards on the PCI
  bus (IsPresent=False, nothing connected). Needs the real card rig.
- Windows build compile check: TBD (see log below).
- Real-world GUI confirmation on the card machine: PENDING user test.

## Fix implementation (2026-10-22)

### misrc_tools/misrc_gui/input/gui_cxadc.c
- DELETED the whole PowerShell detour (`cxadc_win_run_ps_readline`,
  `_popen("powershell ...Import-Module CxadcWin...")` and the four
  get/set helpers built on it).
- ADDED `#include <wbemcli.h>` to the shielded Win32 include block.
- ADDED a direct WMI layer (`cxadc_win_wmi_*`):
  - `CoInitializeEx(COINIT_MULTITHREADED)` per call (RPC_E_CHANGED_MODE
    tolerated, same pattern as the WASAPI audio open),
    `CoCreateInstance(CLSID_WbemLocator)` → `ConnectServer(ROOT\WMI)` →
    `CoSetProxyBlanket(impersonate)`.
  - `CxadcWin_Path` enumeration matched in C against `\\.\cxadcN` (no WQL
    string escaping), `CxadcWin_Config.InstanceName="..."` GetObject with
    backslash-doubled InstanceName.
  - Reads: `EnableTenbit` (VT_BOOL), `CenterOffset` (VT_I4).
    Writes: Put property + `PutInstance`, then a FRESH GetObject readback
    (mirrors the sysfs readback semantics).
  - HRESULT→errno map: `WBEM_E_ACCESS_DENIED`→`EACCES`,
    `WBEM_E_NOT_FOUND`/`INVALID_CLASS`/`INVALID_OBJECT`/`NOT_AVAILABLE`→`ENOENT`,
    `WBEM_E_INVALID_PARAMETER`→`EINVAL`, else `EIO`. All failures log
    `[CXADC] WMI: ... (hr=0x...)` to stderr.
  - `CXADC_WIN_CENTER_OFFSET_MAX 63` (driver's
    `CX_CTRL_CONFIG_CENTER_OFFSET_MAX`, verified in cxadc-win source).
- `cxadc_apply_tenbit_modes`: the EACCES/EPERM fallback-to-8-bit + help-popup
  path is now cross-platform (was `#if !defined(_WIN32)`); Windows-specific
  stderr/status text ("re-run as administrator").
- `gui_cxadc_start`: Windows failure branch now logs errno and gives the
  admin hint for permission denials.
- `gui_cxadc_center_offset_max()` added; `gui_cxadc_set_center_offset` and
  `gui_cxadc_adjust_center_offset` clamp to it (Windows was clamping 0-255
  into a 0-63 driver range → guaranteed failure for values >63).

### misrc_tools/misrc_gui/input/gui_cxadc.h
- Declared `gui_cxadc_center_offset_max()`; fixed the stale
  "clamped to [0,255]" comment on `gui_cxadc_set_center_offset`.

### misrc_tools/misrc_gui/ui/gui_ui.c
- `cxadc_perm_help_pending` popup: Windows branch ("CXADC 10-bit mode needs
  elevation", re-run as Administrator) instead of Linux sudo instructions.
- DC-offset buttons: raw range now clamped via `gui_cxadc_center_offset_max()`
  (was hardcoded 255); EACCES messages Windows-aware.

### misrc_tools/meson.build
- `gui_ldflags += ['-loleaut32']` (Windows GUI branch) for
  VariantClear/SysFreeString.

### misrc_tools/misrc_gui/dev/cxadc_win_lockstep_notes.md
- Updated: current state, what-works list, lock-step item 2 marked DONE.

## Verification performed (hard data)
1. **Compile+link**: `ninja -C build-local misrc_gui.exe` — green
   (`[3/3] Linking target misrc_gui.exe`, exit 0). First build attempt had 6
   `-Wsign-compare` warnings from the WBEM enum vs HRESULT comparisons in
   `cxadc_win_wmi_errno_from_hresult` — fixed with explicit `(HRESULT)`
   casts; rebuild has zero warnings from the new code (remaining warnings
   are pre-existing Windows-branch unused-symbols from the Linux sysfs
   caches).
2. **WMI plumbing smoke test** (`build-local/wmi_smoke_test.c`, compiled with
   the same MinGW gcc + `-lole32 -loleaut32`):
   - First run: `CoCreateInstance(locator) hr=0x80004002` (E_NOINTERFACE) —
     my hand-written `IID_IWbemLocator` tail was wrong
     (`...00a0c906b6da`). Corrected to MinGW `wbemcli.h`'s value
         `{DC12A687-737F-11CF-884D-00AA004B2E24}` and fixed gui_cxadc.c too.
   - Second run: CoInitializeEx 0x0, CoCreateInstance 0x0,
     ConnectServer(ROOT\WMI) 0x0, CoSetProxyBlanket 0x0,
     ExecQuery(CxadcWin_Path) 0x0, **instances: 0** — the whole COM sequence
     the GUI now uses works from this toolchain; the no-live-driver case
     enumerates to 0 instances (→ ENOENT path) exactly as coded. The
     `CxadcWin_Path` class IS registered locally (driver install compiled
     the MOF); only the live instances are absent because the driver isn't
     running (no cards on the PCI bus).
3. **CI guard tests**: `python misrc_tools/test/ci_guard_tests.py` — all
   PASS (incl. "CXADC rate probe contract" and "CXADC synchronized start
   contract").
4. NOT verified (impossible locally): PutInstance write + readback against
   a live card (needs the real CX card rig), the 10-bit permission fallback
   popup, and DC-offset buttons on Windows with cards present.

## Open items for the user — ASKED 2026-10-22, answers recorded
1. End-to-end test on the real card rig (tenbit toggle, 8-bit capture start,
   DC offset buttons, admin-denied fallback popup).
   **User answer: cards not accessible right now — test PENDING.**
   Once confirmed working on the card rig, per house rule also create the
   restore-point zip + fix log note.
2. Local machine shows TWO stale cxadc devnodes but `IsPresent=False` and
   zero Conexant devices connected on the PCI bus.
   **User answer: the cards are in another rig/machine — local "no cards" is
   EXPECTED; the GUI's CreateFileA(\\.\cxadcN) detection is behaving
   correctly here. No local hardware investigation needed.**
3. Upstream cxadc-win issues found while reading their code (worth
   reporting upstream, NOT fixed here): `CxadcWin.psm1` `Get-CxadcWinDevice`
   reads `$Hw.PciBusAddress` but the MOF property is `PciAddress` — that
   would break `-Path` lookups even under pwsh 7.4+.
