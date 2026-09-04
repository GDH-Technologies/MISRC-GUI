---
name: warn-guard-suite-edit
enabled: true
event: file
action: warn
conditions:
  - field: file_path
    operator: regex_match
    pattern: misrc_tools/test/ci_guard_tests\.py$
---

**Editing the guard suite — three things to keep straight.**

- **Fork guards vs upstream guards.** The file is shared with upstream; the fork added ~18
  guards on top of upstream's ~35. Add fork guards in the fork's block, name them so the
  intent survives a merge, and never rename or reorder upstream's. A guard that fails only
  on this fork's layout must skip cleanly upstream.
- **Some guards are C harnesses**, not greps: they compile and run
  `misrc_tools/test/*_harness.c`. Changing a harness contract means changing both files.
- **Upstream's `build.yml` is matched by raw substring here.** Never "fix" a guard by editing
  `build.yml` (blocked); fix the guard's expectation and say why in the PR.

Run `python3 misrc_tools/test/ci_guard_tests.py --static-only` before and after, and quote
the pass count in the PR body. `--post-build --gui-path build-local/misrc_gui` for the
binary-introspection guards.
