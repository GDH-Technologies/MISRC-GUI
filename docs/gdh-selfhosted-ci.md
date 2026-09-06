# Self-hosted CI on the GDH fork

Fork-local notes for `GDH-Technologies/MISRC-GUI`. None of this exists upstream.

## What runs, and where

CI for this fork runs on four of the org's own runners, via
`.github/workflows/selfhosted-deploy.yml`:

| Runner | Labels | Builds on | Gets |
|---|---|---|---|
| `workflow-master` | `self-hosted, Linux, X64, wm` | every event | binaries + GNOME desktop entry |
| `capture-server-0` | `self-hosted, Linux, X64, cs0` | `main` and `v*` tags only | binaries only |
| `air-0` | `self-hosted, macOS, ARM64, air0` | `main` and `v*` tags only | binaries + `~/Applications/MISRC.app` |
| `WIN-NODE-0` | `self-hosted, Windows, X64, win0` | every event | binaries + Start Menu shortcut in `%LOCALAPPDATA%\Programs\MISRC` |

| Event | Runners | Build + guard tests | Install |
|---|---|---|---|
| Pull request | wm + win0 | yes | no |
| Push to `main` | wm + cs0 + air0 + win0 | yes | **yes** |
| Release tag `v*` | wm + cs0 + air0 + win0 | yes | **yes** |
| `workflow_dispatch` | wm + win0 | yes | no |

cs0 and air0 are kept out of PR builds deliberately, for different reasons: cs0 is a 4-core
capture box where PR churn would contend with capture work, and air0 is a laptop that is
often asleep. Both join only for the refs that actually install. win0 builds PRs on purpose:
it exists to catch Windows breakage before merge, and it is a development PC with nothing
else contending for it.

The runner services run as the machine's own desktop user — `rdodge` on wm, cs0 and win0,
`dodge` on air0 — so the install step reaches the user's own profile without elevation. On
wm that is also the GNOME session user, so the desktop entry is written there too; on air0
the runner is a LaunchAgent (`actions.runner.GDH-Technologies.air-0`) in the login session,
so it can write `~/Applications`; on win0 it is a Windows service
(`actions.runner.GDH-Technologies.WIN-NODE-0`) whose Log On account must be `rdodge` (the
desktop user; its profile folder is still `C:\Users\dodge`) — see below for why the default
is wrong.

### Each leg carries its own runner labels

A self-hosted runner self-reports its OS and architecture, so the single hardcoded
`runs-on: [self-hosted, Linux, X64, …]` that served wm and cs0 could never match air0. The
matrix entries are therefore objects rather than bare names:

```yaml
runner: >-
  ${{ fromJSON( … && '[{"name":"wm","os":"Linux","arch":"X64"}, …,
                        {"name":"air0","os":"macOS","arch":"ARM64"}]' || … ) }}
runs-on: [self-hosted, "${{ matrix.runner.os }}", "${{ matrix.runner.arch }}", "${{ matrix.runner.name }}"]
```

Two things about that shape are deliberate. The labels are interpolated as **scalars inside a
flat sequence**, the form the file already used, rather than as an array-valued `runs-on` —
same result, no new Actions behaviour to depend on. And `matrix.runner.os` is what the
per-machine steps gate on, so behaviour is keyed on the platform rather than on a machine
name that happens to imply one.

Getting this wrong fails silently: a leg naming an OS label no runner has does not error, it
queues forever — the same failure mode as a missing runner-group grant below.

### cs0 is headless, and takes binaries only

cs0 boots to `multi-user.target` with no Xorg and no GNOME. The icon, the `.desktop` entry
and the stale-launcher sweep are gated to `matrix.runner.name == 'wm'`; cs0 gets `misrc_gui`,
`misrc_capture` and `misrc_extract` in `~/.local/bin` and nothing else. (cs0 also has no
ImageMagick, which the icon step shells out to.)

That is enough for the CLI tools and for the GUI's headless paths — `--smoke-test` returns
before `InitWindow`, which is why the smoke test passes on a runner with no `DISPLAY`.

### air0 is a macOS client, and takes a real .app

air0 exists to be a **client**: a laptop you pick up, launch the GUI on, and use to drive
capture hardware on cs0 over the LAN. macOS will not launch a bare Mach-O from the Dock,
Finder or Spotlight, so binaries in `~/.local/bin` — what cs0 gets — are not enough. air0
gets those too, but the deliverable is `~/Applications/MISRC.app`.

The bundling step is lifted from upstream `build.yml`'s `macos-app-build` job: `.icns` via
`sips` + `iconutil`, an inline `Info.plist` (`dev.misrc.gui`), every non-system dylib copied
into `Contents/Frameworks` and rewritten to `@rpath` with `otool` + `install_name_tool`, an
`@executable_path/../Frameworks` rpath, and an ad-hoc `codesign`. Dropped from it: the `lipo`
universal merge, the `hdiutil` DMG and the artifact upload, none of which mean anything for a
single-arch install in place.

Collecting the dylibs at all may look pointless when air0 has the Homebrew originals sitting
right there. It is not: the final `otool -L … | grep -E '/opt/homebrew|/usr/local/opt'`
assertion is what catches a bundle that is quietly not self-contained, and that is a bug that
otherwise only shows up in a release DMG on someone else's Mac.

Ad-hoc signing is enough here. The bundle is built in place and never downloaded, so it
carries no `com.apple.quarantine` attribute and Gatekeeper does not ask for notarization.

`net_mode` is not resumed from saved settings on any platform, so you pick **Client** from the
info page each session, exactly as on wm. macOS settings live in
`~/Library/Preferences/com.misrc.gui.json`. The first time the client's discovery listener
binds its port, the macOS application firewall may ask to allow incoming connections — that is
the UDP beacon on 8091; allow it, or connect by host and port instead.

#### One-time prep on air0

Homebrew is installed at `/opt/homebrew` but is **not** on the runner's non-interactive
`PATH`, which is why the workflow has a `Set up Homebrew environment` step. The formulas
themselves are installed by hand once, matching how cs0's `dnf` packages were handled — CI
does not mutate the laptop:

```bash
ssh air0 'eval "$(/opt/homebrew/bin/brew shellenv)"; \
  HOMEBREW_NO_AUTO_UPDATE=1 brew install cmake meson ninja libusb libuvc libsoxr nasm'
```

`fftw`, `flac` (≥ 1.5.0, which meson hard-requires) and `pkgconf` were already present. A
missing formula is self-diagnosing: `scripts/build-deps-unix.sh` fails naming the pkg-config
module and the brew formula that provides it. `nasm` is not actually needed on arm64 —
`meson.build` only requires it for x86_64 — but it is installed to match upstream's list.

Unlike cs0, air0 needs **no runner-group grant**: it is in the `Default` group, whose
visibility is `all`.

### win0 is a Windows dev PC, and builds under MSYS2 MINGW64

win0 restores the Windows x64 coverage this fork lost when `build.yml` was disabled, and it
runs on every event so a PR that breaks the Windows build fails **before** merge. The leg
mirrors upstream `build.yml`'s `windows-exe` job rather than the fork's
`scripts/build-local.ps1`: the same MSYS2 MINGW64 shell, `scripts/build-deps-windows.sh` for
the static libuvc + hsdaoh + raylib prefix, the job's own `meson setup`/`compile` lines, and
its release assertions — GUI subsystem, and an import table naming only system DLLs.
`build-local.ps1` is not used because it pip-installs meson/ninja into the runner user's
profile, and CI does not mutate the machine.

What the leg proves is narrower than on Linux. Streaming, V4L2 preview, video record and
mediamtx supervision are compiled-out `#else` stubs on Windows, so the guard suite's
Linux/macOS-only runtime harnesses self-skip (9 of 67 at the time of writing). The Windows
leg covers the build contract, the UI/record/FLAC/playback code, and the static-link
assertions. There is no capture hardware on win0.

#### The runner must run as the desktop user, not NETWORK SERVICE

`config.cmd --runasservice` defaults the service Log On to `NT AUTHORITY\NETWORK SERVICE`,
whose profile is `C:\Windows\ServiceProfiles\NetworkService`. Under that account `$HOME`,
`%LOCALAPPDATA%` and the Start Menu all point somewhere no human will ever launch from, and
the account cannot write into `rdodge`'s profile. The first registration of `WIN-NODE-0` was
made that way and had to be redone. The runner's own `.credentials` are DPAPI-protected to
the account that configured them, so changing the Log On account in `services.msc`
afterwards is not enough: remove and re-register, and re-add the `win0` label, which is not
a default label and is lost on removal:

```powershell
# from C:\Users\dodge\actions-runner, in an elevated PowerShell
gh auth refresh -h github.com -s admin:org            # once; the runner APIs need it
$rm  = gh api -X POST orgs/GDH-Technologies/actions/runners/remove-token --jq .token
.\config.cmd remove --token $rm
$reg = gh api -X POST orgs/GDH-Technologies/actions/runners/registration-token --jq .token
.\config.cmd --url https://github.com/GDH-Technologies --token $reg `
  --name WIN-NODE-0 --labels win0 --runnergroup Default `
  --runasservice --windowslogonaccount rdodge         # prompts for the password
```

`config.cmd` grants the account *Log on as a service* itself. Verify with
`sc.exe qc actions.runner.GDH-Technologies.WIN-NODE-0` (`SERVICE_START_NAME : .\rdodge`) and
`Get-Service actions.runner.GDH-Technologies.WIN-NODE-0` (`Running`). A service runs in
session 0 with no desktop, which is fine: every step here is headless.

#### One-time prep on win0

MSYS2 at `C:\msys64` (the path `scripts/build-local.ps1` hardcodes, and the one the
workflow's `shell:` line names), with the `windows-exe` job's MINGW64 package list plus
`libjpeg-turbo` (CI has it, `INSTALLATION.md` omits it) and `clang-tools-extra` (clangd for
the editor plugin). Installed by hand once, as on air0; CI never runs pacman:

```
pacman -Syu   # twice on a fresh install: the first pass upgrades the runtime and exits
pacman -S --needed git mingw-w64-x86_64-{cmake,fftw,flac,gcc,libjpeg-turbo,libusb,libsoxr,meson,nasm,ninja,pkgconf,clang-tools-extra}
```

Git for Windows must be installed: `actions/checkout` uses its `git`, and its `bash` is what
`shell: bash` resolves to on a Windows runner. The job pins `defaults.run.shell: bash`
because a Windows runner's default for a plain `run:` step is **pwsh** -- the first run of
the leg died on `set -euo pipefail` being parsed as `Set-Variable`. Nothing needs to be on
the user PATH: the MSYS2 steps get their PATH from `/etc/profile` via `MSYSTEM=MINGW64`.

The runner is in the `Default` runner group (visibility `all`), so like wm and air0 it
needs no runner-group grant.

#### Line endings

Git for Windows' *system* gitconfig sets `core.autocrlf=true` and the repo has no
`.gitattributes`, so `actions/checkout` leaves every `.sh` with CRLF endings and MSYS2 bash
fails on `#!/usr/bin/env bash\r`. The leg's first step after checkout re-checks the workspace
out as LF (`git config core.autocrlf false && git rm -r --cached . && git reset --hard`) —
workspace-local, so there is no runner-level git setting to remember. Upstream's own CI
survives CRLF only because `git-version.sh` diffs with `--ignore-cr-at-eol`; that guard
still matters here, and `ci_guard_tests.py` asserts it.

#### Install layout on win0

Binaries go to `%LOCALAPPDATA%\Programs\MISRC` (the per-user application convention), a
`MISRC GUI.lnk` is written to the user's Start Menu programs folder with the icon taken from
the `.exe` (which embeds `MISRC_Icon.ico` through `misrc_gui.rc`), and the install directory
is appended to the **user** PATH once via the .NET environment API (`setx` truncates at
1024 characters). The deps cache is `%USERPROFILE%\.cache\misrc\.deps\install`, the same
`$HOME/.cache/misrc/.deps/install` the other machines use, outside the workspace for the same
reason.

Windows refuses to overwrite or delete a running image but allows it to be renamed, so the
install moves a live `misrc_gui.exe` aside under a unique name before the new one takes its
place, and sweeps the aside copies on the next install. An open GUI therefore never blocks a
deploy; it keeps running the old image until relaunched.

#### Reproducing the win0 leg by hand

```
C:\msys64\usr\bin\env.exe MSYSTEM=MINGW64 CHERE_INVOKING=1 C:\msys64\usr\bin\bash.exe -l
  DEPS_PREFIX="$HOME/.cache/misrc/.deps/install" bash scripts/build-deps-windows.sh
  export PKG_CONFIG_PATH="$HOME/.cache/misrc/.deps/install/lib/pkgconfig:$MINGW_PREFIX/lib/pkgconfig"
  meson setup build-ci misrc_tools --buildtype release
  meson compile -C build-ci misrc_capture misrc_extract misrc_gui
  python misrc_tools/test/ci_guard_tests.py --post-build --gui-path build-ci/misrc_gui.exe
  build-ci/misrc_gui.exe --smoke-test
```

Everyday development on win0 uses `pwsh -File scripts/build-local.ps1` instead (see
`.claude/CLAUDE.md`); the two share `.deps` contents but not the prefix path.

### Adding air0 immediately found two real macOS regressions

Worth recording, because it is the whole argument for the machine being there. This fork had
**no macOS coverage at all** while `build.yml` is disabled, and the first real build on air0
failed twice before it went green:

- `misrc_gui/streaming/gui_mediamtx.c` created its metrics socket with
  `SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC`. Those two flags are Linux extensions to
  `socket()`; the BSDs and macOS accept only the type. Fixed by setting both with `fcntl`
  afterwards on every POSIX host. The root cause is structural: `gui_video_record.c` and
  `gui_rtsp_stream.c` both wrap their implementations in
  `#if defined(__linux__) && !defined(__ANDROID__)` with a non-Linux stub, while
  `gui_mediamtx.c` guards only `_WIN32` — so its "POSIX" branch silently means "Linux".
- `gui_rtsp_stream.c`'s non-Linux stub branch defined 11 of the 12 functions its header
  declares. The missing one, `gui_rtsp_stream_poll`, is called unconditionally from `main`,
  so macOS compiled and then failed at link.

A third failure was in the guard suite rather than the product: `check_mediamtx_config_runtime`
claims macOS support but compiled its harness with a bare `_POSIX_C_SOURCE`, which hides
`INADDR_LOOPBACK` on macOS. The real build does not hit this because meson adds
`-D_DARWIN_C_SOURCE` on darwin. Fixed the same way `check_record_ringbuffer_fallback_runtime`
already did it.

Note what this does **not** buy: air0 does not build PRs, so macOS breakage is caught after
merge, not before. That is a deliberate trade for not running builds on a laptop.

### When air0 is asleep

There is no online preflight. A merge to `main` while air0 is off leaves that leg queued
until the Mac comes back or GitHub times the job out; wm and cs0 still build and install,
because `fail-fast: false` keeps the legs independent. The run stays yellow in the meantime.

This does not accumulate. The workflow-level `concurrency` group `selfhosted-install` has
`cancel-in-progress: true`, so the next merge to `main` cancels the whole stale run, queued
leg included, and starts fresh.

### The tag bump is its own job

`bump-tag` runs before either build leg, on wm, and mints at most one `-gdh.N` tag per
commit. It used to be the second *step* of the single build job, which was correct while
there was one machine and wrong as soon as there were two: both legs check out the same
commit at the same time, so a bump inside the matrix would either have both machines racing
to push the same tag (the loser dies on a rejected push), or — gated to one leg — leave the
other leg having checked out before the tag existed, resolving `dev-<date>-<sha>` instead.
One commit would then install two different version strings. As a dependency job the tag
exists before either leg checks out, so both resolve the same version.

`build-and-deploy` therefore carries
`if: !cancelled() && (needs.bump-tag.result == 'success' || needs.bump-tag.result ==
'skipped')` — without the `skipped` arm, every event other than a push to `main` would skip
the build too.

## Runner group access — invisible in the tree

cs0 and cs1 live in the org runner group **`capture-nodes`**, which is `visibility:
selected`. MISRC-GUI was not on its repository list, so `runs-on: [self-hosted, cs0]` would
have sat queued forever with no error message. Granted once with:

```bash
gh api --method PUT \
  orgs/GDH-Technologies/actions/runner-groups/3/repositories/1334308945
```

Inspect the current list with:

```bash
gh api orgs/GDH-Technologies/actions/runner-groups/3/repositories \
  --jq '.repositories[].full_name'
```

`wm` needs none of this — it is in the `Default` group, whose visibility is `all`. This is
the second thing about this fork's CI that is real but invisible in the working tree (the
first is the disabled `build.yml` above), which is why it is written down here.

## Running the GUI's server mode on cs0

To drive cs0's capture hardware from the GUI on wm, cs0 runs `misrc_gui` in **Server** mode
and wm runs it in **Client** mode. Two facts make this less obvious than it looks:

- The server lives **only in the GUI binary**. `misrc_gui/net/gui_net.c` is listed in
  `sources_gui` and never in `sources_capture` (`misrc_tools/meson.build`), so
  `misrc_capture` and `misrc_extract` carry no HTTP server, no `/rf` stream and no discovery
  beacon. There is no headless CLI server.
- `misrc_gui` calls `InitWindow` unconditionally for the GUI. The arguments that return
  before it are `--version`, `--help`, `--smoke-test`, the `--*-probe` / `--*-test`
  diagnostics, and `--net-serve`.

The way to run a server with no window is the headless flag: everything the GUI's main loop
does for the server (the command poller, the settings publisher, capture and record
start/stop) minus the window. It refuses to run without `--config`, so it can never run on
the host's live settings file:

```bash
misrc_gui --config ~/.config/misrc/server.json --net-serve        # until SIGTERM/SIGINT
misrc_gui --config ~/.config/misrc/server.json --net-serve 600    # bounded, seconds
```

The config needs `net_mode` 1 and **both** `net_server_port` and `net_server_port_str`
(the string mirror is what the mode applier reads). Its output folder must exist. An
overwrite prompt cannot be answered headlessly, so set `overwrite_files` true or use
auto-naming with timestamps. On any host, `misrc_tools/test/net_settings_e2e.sh
build-local/misrc_gui 18080` drives such a server on the Simulated device through the
whole protocol, then probes it as a client.

Before `--net-serve` existed, a server on cs0 needed a framebuffer, and cs0 has
`xorg-x11-server-Xvfb` installed for exactly that — no Xorg session, no VNC, no desktop; the
framebuffer is created and torn down with the process. That still works, and it is the way
to get a server whose *window* something else needs:

```bash
xvfb-run -a misrc_gui --config ~/.config/misrc/server.json
```

`net_mode` is deliberately **not** resumed from saved settings: `gui_app_init` forces Local
on every normal launch so a restart never resurrects a stale server or client. The single
exception is `--config`, which the code carves out for scripted launches — `gui_app_init`
then calls `gui_net_apply_mode()` and the server comes up at startup. That is why the server
config is a separate file passed with `--config` rather than a flag or the stock settings
file.

GL under Xvfb comes from Mesa llvmpipe (software). That is fine for a process nobody watches
— it is a data pump. If it ever proves too slow, the upgrade path is a real Xorg on cs0's
GTX 1070 with `AllowEmptyInitialConfiguration`, not a bigger Xvfb.

**The Xvfb trick has no macOS equivalent**, but `--net-serve` does not need one: it is the
same GUI binary with no window, so a Mac can host a server the same way. The server still
lives only in `misrc_gui`; `misrc_capture` and `misrc_extract` carry no HTTP server.

## Upstream's build.yml is DISABLED — as a repo setting, not a file edit

`.github/workflows/build.yml` is byte-identical to `harrypm/MISRC-GUI` and must stay that
way. It is turned off on this fork with:

```bash
gh workflow disable "Build and release binary" --repo GDH-Technologies/MISRC-GUI
# undo:
gh workflow enable  "Build and release binary" --repo GDH-Technologies/MISRC-GUI
```

**This is invisible in the working tree** — hence this file. If you ever wonder why pushing a
`v*` tag or opening a PR produces no ubuntu/windows/macOS jobs, that's why.

Two reasons it is a setting rather than an `if:` guard on each job:

1. `misrc_tools/test/ci_guard_tests.py` is a **raw substring matcher over `build.yml`**. It
   hard-codes `runs-on: windows-2022`, `runner: macos-14`, `runner: macos-15-intel`, every job
   key, the `release` job's `needs:` list, and asserts `pkg-config --modversion fftw3f` appears
   *exactly four times*. Editing that file risks tripping guards in non-obvious ways.
2. `build.yml` is the file upstream changes most. Leaving it untouched keeps every upstream
   merge conflict-free.

### Consequence: partial cross-platform coverage here

With `build.yml` off, cross-platform coverage comes only from the self-hosted legs: Windows
x64 on win0 (every event, so before merge) and macOS arm64 on air0 (after merge). Android,
Windows arm64 and macOS Intel are not built on this fork at all. A fork-local change can
still break those without CI noticing — this nearly happened once before win0 existed (a
file-scope `static` whose only use sat inside `#if !defined(_WIN32)`, i.e. an
unused-variable warning on Windows only). Before sending work upstream, re-enable
`build.yml` and run it once via `workflow_dispatch`.

## The deps cache

`MISRC_DEPS_PREFIX` points at **`~/.cache/misrc/.deps/install`**, deliberately outside the
runner workspace. Two constraints shaped that path:

- **Outside the workspace**, because `actions/checkout` defaults to `clean: true`
  (`git clean -ffdx`). A workspace-local `.deps` would be deleted every run — rebuilding
  hsdaoh and raylib on each merge, and leaving the installed binary's `RUNPATH` pointing at a
  directory the next checkout removes.
- **Still containing the literal `.deps/install`**, because `ci_guard_tests.py`'s
  `check_built_gui_links_vendored_hsdaoh` asserts that exact substring appears in `ldd` output
  (`ci_guard_tests.py:259`). A prefix like `~/.cache/misrc/deps` builds fine and then fails the
  guard.

`scripts/build-deps-unix.sh` is stamp-gated (`.build-stamp`), so a warm cache exits immediately
with `deps already built (stamp matches)`. It is invoked as `bash scripts/build-deps-unix.sh`
because the script is tracked mode `100644`, not `100755` — `scripts/build-local.sh:85` does the
same thing for the same reason.

To rebuild the cache from scratch:

```bash
rm -rf ~/.cache/misrc/.deps/install
DEPS_PREFIX=~/.cache/misrc/.deps/install bash scripts/build-deps-unix.sh
```

## Reproducing a CI run by hand

```bash
DEPS_PREFIX=~/.cache/misrc/.deps/install bash scripts/build-deps-unix.sh
MISRC_DEPS_PREFIX=~/.cache/misrc/.deps/install BUILD_DIR=build-ci bash scripts/build-local.sh --all
python3 misrc_tools/test/ci_guard_tests.py --post-build --gui-path build-ci/misrc_gui
build-ci/misrc_gui --smoke-test
```

## What the install step does

Installs `misrc_gui`, `misrc_capture` and `misrc_extract` into `~/.local/bin`, writing each via a
temp name plus `mv -f` — the rename is atomic, so a merge cannot fail with `ETXTBSY` while the GUI
is open.

That step runs on both machines. A second step, **gated to wm**, then rewrites
`~/.local/share/applications/misrc_gui.desktop`. Note the `StartupWMClass`:

```
Exec=/home/rdodge/.local/bin/misrc_gui %U
StartupWMClass=MISRC Capture
```

`StartupWMClass` must equal `GUI_WINDOW_CLASS_NAME` in `misrc_tools/misrc_gui/core/misrc_gui.c`.
The GUI passes that constant to `InitWindow` and only then applies the versioned title with
`SetWindowTitle`, which touches `_NET_WM_NAME` and not `WM_CLASS` — so every build reports the same
class and any launcher keeps matching the running window. `ci_guard_tests.py` enforces the
equality, so renaming the constant without updating this workflow fails CI.

Earlier revisions instead set `Exec=env RESOURCE_NAME=misrc_gui …` with a matching
`StartupWMClass=misrc_gui`, from when the title still fed `WM_CLASS`. That shim pinned only the
*instance* half of `WM_CLASS`, and only for a process started through that exact `Exec` line —
launching the same binary from a terminal, the Desktop shortcut or a second launcher fell back to
the title and lost the dock icon regardless. Don't reintroduce it; the guard tests reject it.

Verify with:

```bash
xprop WM_CLASS      # then click the window
# => WM_CLASS(STRING) = "MISRC Capture", "MISRC Capture"
```

The install step then **sweeps stale MISRC launchers** it did not write. A leftover `.desktop`
naming an older, versioned `StartupWMClass` shows up as a second "MISRC GUI" in the app grid and
matches no window, which looks identical to the bug the constant class name fixes. The sweep is
deliberately narrow, because it deletes files in a real home directory:

- only `*.desktop` directly in `~/.local/share/applications`
- only where `StartupWMClass` is a versioned `MISRC Capture <ver>`, or the retired `misrc_gui` /
  `misrc-gui` shim values
- never `misrc_gui.desktop`, the entry it just wrote

A launcher carrying the correct `StartupWMClass` is left alone, and anything not naming a MISRC
class is never considered. Removals are echoed in the job log.

The sweep does not reach `~/Desktop`. A shortcut there pointing at an older AppImage
(`~/.local/bin/misrc_gui.AppImage`) still runs that build, and a pre-v1.1.5 AppImage reports a
versioned `WM_CLASS` of its own — repoint or delete it by hand:

```bash
grep -l MISRC ~/.local/share/applications/*.desktop ~/Desktop/*.desktop
```

## Security note

Self-hosted runners execute workflow code from pull requests on this machine. That is acceptable
while the fork is private and contributors are trusted. Revisit before accepting outside PRs.
