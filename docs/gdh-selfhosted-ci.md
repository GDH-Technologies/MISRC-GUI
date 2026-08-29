# Self-hosted CI on the GDH fork

Fork-local notes for `GDH-Technologies/MISRC-GUI`. None of this exists upstream.

## What runs, and where

All CI for this fork runs on the org's own runner **`workflow-master`** (labels
`self-hosted, Linux, X64, wm`), via `.github/workflows/selfhosted-deploy.yml`:

| Event | Build + guard tests | Install to `~/.local` |
|---|---|---|
| Pull request | yes | no |
| Push to `main` | yes | **yes** |
| `workflow_dispatch` | yes | no |

The runner service runs as `rdodge`, the same user as the desktop session, so the install
step reaches `~/.local/bin` and the GNOME desktop entry without sudo.

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

### Consequence: no cross-platform coverage here

Windows, macOS and Android are no longer built on this fork at all. A fork-local change can
break them without CI noticing — this has already nearly happened once (a file-scope `static`
whose only use sat inside `#if !defined(_WIN32)`, i.e. an unused-variable warning on Windows
only). Before sending work upstream, re-enable `build.yml` and run it once via
`workflow_dispatch`.

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

It also rewrites `~/.local/share/applications/misrc_gui.desktop`. Note the `StartupWMClass`:

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

If the dock still shows a generic icon, look for **stale duplicate launchers** before suspecting
the workflow — any leftover `.desktop` in `~/.local/share/applications/` naming an older,
versioned `StartupWMClass` shows up as a second "MISRC GUI" in the app grid and matches nothing:

```bash
grep -l MISRC ~/.local/share/applications/*.desktop
```

## Security note

Self-hosted runners execute workflow code from pull requests on this machine. That is acceptable
while the fork is private and contributors are trusted. Revisit before accepting outside PRs.
