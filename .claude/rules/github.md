# GitHub conventions

Repo: `GDH-Technologies/MISRC-GUI` (the fork). Upstream: `harrypm/MISRC-GUI`. Assignee on
every issue: `radodge`. PRs target `main`; there are no `dev`/`preview`/`production` tiers
here — the merge to `main` is the deploy (self-hosted CI installs on wm, cs0 and air0).

## Use the MCP tools, not `gh`

Issues and PRs go through `mcp__plugin_github_github__*` — it sets type, labels,
assignees and org issue fields by name in one call. Use `gh` or git only for what it has no
tool for:

- listing labels — `gh label list -R GDH-Technologies/MISRC-GUI`
- labelling a PR — `create_pull_request` takes no `labels` parameter
- proving a fix landed — `git merge-base --is-ancestor <sha> origin/main`
- proving a fix shipped — `~/.local/bin/misrc_gui --version` on the host names the built tag

## Issues

Enabled on 2026-09-04 (a fork starts with issues off). The default `bug` and `enhancement`
labels were deleted the same day — Type covers that axis org-wide — and the component set
was created: `capture-path` · `streaming` · `networking` · `cxadc` · `ui` · `build` · `ci` ·
`upstream`, plus the workflow flags `regression` · `upstream-bound` · `documentation`. Read
the live set before every use anyway (`gh label list -R GDH-Technologies/MISRC-GUI`); an
unknown label fails the create call. #42 (the net-mode fanout) is the first issue and the
shape to copy. If issues are ever found disabled again, write the gap into the PR body's
**Owed after merge** and say so — never flip the repo setting yourself.

Defects in upstream code are filed **here first**, with evidence, and reach harrypm as a PR
through `/upstream-pr`. Never open an issue on `harrypm/MISRC-GUI` without Reece.

## Every issue carries

Type · Priority · Effort · at least one component label · a `## Provenance` section.

**Type is the classification axis:** `Bug` · `Feature` · `Task` · `Optimization` ·
`Extraction` (org-level; `list_issue_types` confirms). Labels are the orthogonal axis —
component/area plus workflow flags (`regression`, `upstream-bound`, `documentation`). A
classifier that belongs in Type never becomes a label.

**Read the live label set before applying one.** An unknown label fails the create call.

## Standing rules

- **Propose before filing.** Draft the issue inline — title, type, priority, effort,
  evidence — and file it once Reece says go.
- **Say which kind every PR is** — upstream-bound or fork-only (`.claude/rules/upstream.md`).
- **Never overwrite a human-set field value.** If your rubric disagrees, say so and leave it.
- **Never reopen or re-close against Reece's decision.**
- **Never hand-write the `🤖 Generated with` line or a provenance table.** The
  `github-conventions.py` hook appends it to issue and PR bodies; writing your own duplicates it.

## Procedures live in skills

| Skill | Use for |
| --- | --- |
| `/file-issue` | Filing an issue: duplicate search, priority/effort rubrics, body structure |
| `/close-issue` | Closing an issue: the gates that must pass first |
| `/open-pr` | Opening a fork PR: body shape, kind, verification, labels |
| `/upstream-pr` | Taking fork commits to harrypm: base, cherry-picks, six-target check |
| `/sync-upstream` | Merging `upstream/main` into the fork on a dated chore branch |
