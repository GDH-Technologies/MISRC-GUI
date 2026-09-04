---
name: warn-meson-dep-floor
enabled: true
event: file
action: warn
conditions:
  - field: file_path
    operator: regex_match
    pattern: misrc_tools/meson(\.build|_options\.txt)$
---

**Editing the meson build — mind the floors and the six targets.**

- **libFLAC ≥ 1.5.0 is a hard floor** (`flac_writer` needs it; upstream CI fetches a prebuilt
  1.5.0 into `.deps/install` because ubuntu-22.04's apt ships 1.3.3). Lowering it breaks the
  writer; raising it breaks the AppImage baseline.
- **hsdaoh is required** and only ever comes from `.deps/install`; the "system hsdaoh"
  fallback message is a check for a literal path, not a real alternative.
- **Anything you add must build on all six upstream targets** (`.github/CI_RULES.md`) if the
  change is upstream-bound — a new `dependency()` needs an install line in `build.yml`, and
  that file is upstream's to change. If the dependency is fork-only, guard it with a
  `meson_options.txt` feature that defaults off, as the streaming stack does.
- `librtlsdr` is optional by design; keep it that way.

Reconfigure from scratch to prove it (`scripts/build-local.sh --clean`) and run the guard
suite — `bundled_mediamtx_contract` and the link-contract guards read this file.
