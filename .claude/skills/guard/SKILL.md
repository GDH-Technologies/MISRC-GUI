---
name: guard
description: Use when adding, changing or debugging a check in misrc_tools/test/ci_guard_tests.py (a "guard"), when a guard fails after an upstream sync, when a fork feature or an upstream fix needs a regression guard, or when a C harness under misrc_tools/test/*_harness.c must be written or changed.
when_to_use: Use before editing ci_guard_tests.py or any *_harness.c, when a PR body must quote a guard count, and when a guard failure has to be classified as "the guard is stale" versus "the code regressed".
allowed-tools:
  - Bash(python3 misrc_tools/test/ci_guard_tests.py *)
  - Bash(git diff *)
  - Bash(git log *)
  - Bash(grep *)
---

# Guards

## What a guard is

`misrc_tools/test/ci_guard_tests.py` is this repo's real test surface — upstream's and the
fork's. Each guard is a `def check_<thing>(...) -> int` that returns `0` or
`fail("ERROR text")`, registered in `main()`'s
`checks: List[Tuple[str, Callable[[], int]]]` as `("human name", lambda: check_<thing>(paths))`.
The runner prints `PASS: <name>` per check and stops at the first failure with
`FAILED: <name>` on stderr. A check that falls off its end (returns `None`) fails the suite
on purpose — it is a missing `return`.

## Three modes

| Invocation | Runs | Use |
| --- | --- | --- |
| `--static-only` | text/contract guards (44 today, ~2 s) | every edit — the `guards-on-edit.py` hook; PR preflight |
| no flags | + runtime guards inserted in the `if not args.static_only:` block: AppRun behaviour, record ringbuffer fallback, UI scale, FLAC STREAMINFO, preview tap mux, mediamtx config, ALSA device resolution — each compiles and runs a C harness | before a PR that touches what they cover |
| `--post-build --gui-path build-local/misrc_gui` | + binary introspection: vendored hsdaoh linkage, FX3 symbols | after every build; CI |

## Adding a guard

1. **Static or runtime?** Static = a contract over source or workflow *text*
   (`read_text`, `extract_function_body`, `strip_c_comments` helpers exist). Runtime = a
   behaviour you can only prove by compiling the unit with a harness.
2. **Write `check_<thing>(repo_root, ...) -> int`** beside its neighbours. Every failure path
   is `return fail("what is wrong, where, what to do")` — that ERROR line is all CI shows.
   `fail()` on a missing file; never pass because there was nothing to check. End `return 0`.
3. **Register it.** Static: append to the `checks` list *after upstream's entries* so a sync
   never reorders them. Runtime: `checks.insert(N, (...))` inside `if not args.static_only:`,
   keeping the existing order (later guards assume earlier ones passed).
4. **Harness** (copy `check_preview_tap_mux_runtime`): `misrc_tools/test/<thing>_harness.c`
   plus the unit under test, compiled in a `tempfile.TemporaryDirectory` with
   `cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -pthread
   -I<dir> … -o exe`, then run. The harness prints what it measured and exits non-zero on
   the defect. That is the shape harrypm can run himself — it is `/upstream-pr` evidence.
5. **Prove it both ways.** Make it FAIL on the old code (stub the fix, run, see
   `FAILED: <name>`), then PASS on the new. Quote both runs and the pass count in the PR.

## When a guard fails after `/sync-upstream`

Read its ERROR line first. If upstream legitimately changed what the guard matches (a renamed
workflow step, a moved constant), fix the guard's expectation in the same merge and say so in
the PR. Never "fix" it by editing `.github/workflows/build.yml` — upstream's, and blocked. If
the guard is right and upstream regressed, that is an upstream-bound fix and the guard is its
evidence.

## Never

- Never delete or rename an upstream guard. If the fork's layout makes one inapplicable, skip
  it with a printed reason.
- Never write "guards pass" without the count from a run you made.
