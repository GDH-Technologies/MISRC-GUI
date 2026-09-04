# Self-hosted CI on the GDH fork

Fork-local notes for `GDH-Technologies/MISRC-GUI`. None of this exists upstream.

## What runs, and where

CI for this fork runs on two of the org's own runners, via
`.github/workflows/selfhosted-deploy.yml`:

| Runner | Labels | Builds on |
|---|---|---|
| `workflow-master` | `self-hosted, Linux, X64, wm` | every event |
| `capture-server-0` | `self-hosted, Linux, X64, cs0` | pushes to `main` and `v*` tags only |

| Event | Runners | Build + guard tests | Install to `~/.local` |
|---|---|---|---|
| Pull request | wm | yes | no |
| Push to `main` | wm + cs0 | yes | **yes** |
| Release tag `v*` | wm + cs0 | yes | **yes** |
| `workflow_dispatch` | wm | yes | no |

cs0 is kept out of PR builds deliberately: it is a 4-core capture box, and PR churn there
would contend with capture work. It joins only for the refs that actually install.

Both runner services run as `rdodge`, so the install step reaches `~/.local/bin` without
sudo. On wm that is also the desktop session user, so the GNOME desktop entry is written
there too.

### cs0 is headless, and takes binaries only

cs0 boots to `multi-user.target` with no Xorg and no GNOME. The icon, the `.desktop` entry
and the stale-launcher sweep are gated to `matrix.runner == 'wm'`; cs0 gets `misrc_gui`,
`misrc_capture` and `misrc_extract` in `~/.local/bin` and nothing else. (cs0 also has no
ImageMagick, which the icon step shells out to.)

That is enough for the CLI tools and for the GUI's headless paths — `--smoke-test` returns
before `InitWindow`, which is why the smoke test passes on a runner with no `DISPLAY`.

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
- `misrc_gui` calls `InitWindow` unconditionally. The only arguments that return before it
  are `--version`, `--help`, `--smoke-test` and the `--*-probe` / `--*-test` diagnostics.

So a server on cs0 needs a framebuffer, and cs0 has `xorg-x11-server-Xvfb` installed for
exactly that — no Xorg session, no VNC, no desktop; the framebuffer is created and torn down
with the process:

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
