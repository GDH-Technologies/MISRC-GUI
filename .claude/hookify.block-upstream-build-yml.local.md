---
name: block-upstream-build-yml
enabled: true
event: file
action: block
conditions:
  - field: file_path
    operator: regex_match
    pattern: \.github/workflows/build\.ya?ml$
---

**Blocked: `.github/workflows/build.yml` belongs to upstream.**

`.claude/rules/upstream.md` -> *Upstream owns these*: the fork keeps this file **byte-identical**
to `harrypm/MISRC-GUI`. It is disabled as a *repository setting* (`gh workflow disable`),
never by editing, because:

- `misrc_tools/test/ci_guard_tests.py` substring-matches its contents — an edit fails guards;
- any local change conflicts on every `/sync-upstream` merge.

**Do this instead:**

- Fork CI changes go in `.github/workflows/selfhosted-deploy.yml`.
- A genuine fix to upstream's workflow is an upstream PR (`/upstream-pr`) — cherry-picked
  onto `upstream/main`, never applied to the fork first.
- `.github/CI_RULES.md` is upstream's contract for that file; read it before proposing.
