---
name: file-issue
description: File a GitHub issue on the MISRC-GUI fork with every required field set — type, priority, effort, labels, assignee, evidence, and SHA-pinned source permalinks (fork and upstream). Use when a bug, gap, deferred fix, or follow-up surfaces and belongs in the tracker.
when_to_use: Use when work surfaces a known or honest gap — an unexplained failure, a latent bug, a defect in upstream code, a deferred fix, a "works but shouldn't" — or when the user asks to file, open, or create an issue.
allowed-tools:
  - mcp__plugin_github_github__list_issue_fields
  - mcp__plugin_github_github__list_issue_types
  - mcp__plugin_github_github__search_issues
  - mcp__plugin_github_github__list_issues
  - mcp__plugin_github_github__issue_read
  - Bash(gh label list *)
  - Bash(gh repo view *)
---

# Filing an issue

Honest gaps get verbally acknowledged and then lost. They belong in the tracker at the
moment of discovery, in a state where a future session can pick one up cold — which
requires the evidence and the source pointers to live *in the issue*, not in a conversation.

> [!IMPORTANT]
> **Issues are currently disabled on `GDH-Technologies/MISRC-GUI`** (GitHub's default for a
> fork). Check first: `gh repo view GDH-Technologies/MISRC-GUI --json hasIssuesEnabled`. If
> they are off, draft the issue anyway, show it to Reece, and ask whether to enable issues
> (`gh repo edit --enable-issues`) — his call, not yours. Until then the gap is written into
> the PR body's **Owed after merge** or the active plan file, and your reply says so.

## Procedure

1. **Search for duplicates first.** `search_issues` / `list_issues` before drafting, so a
   finding lands on the existing issue as a comment instead of a new duplicate.
2. **Read the live label set** — `gh label list -R GDH-Technologies/MISRC-GUI`. An unknown
   label fails the create call outright. Today the set is GitHub's defaults only; component
   labels (`capture-path`, `streaming`, `networking`, `cxadc`, `ui`, `build`, `ci`,
   `upstream`) get created when issues are enabled — propose them, do not assume them.
3. **Draft the issue and show it to Reece.** Title, type, priority, effort, labels, body.
   File it with `issue_write` once he approves.
4. **Read back after writing.** `issue_read` the created issue and confirm Priority and
   Effort hold the values you set. Re-apply if either is absent.

## Required on every issue

| Field | Value |
| --- | --- |
| `assignees` | `["radodge"]`, always |
| `type` | One of `Bug` · `Feature` · `Task` · `Optimization` · `Extraction` |
| `issue_fields` | `Priority` and `Effort`, both set — see rubrics below |
| `labels` | At least one component label, from the live set |

Set the org fields by name — no node IDs, no GraphQL:

```json
"issue_fields": [
  {"field_name": "Priority", "field_option_name": "High"},
  {"field_name": "Effort",   "field_option_name": "Medium"}
]
```

Leave `Start date` and `Target date` blank — a guessed schedule is worse than none.

## Priority rubric

Captures are **irreplaceable analog tape**, so "can a bad capture pass as good?" outranks
"is something broken?".

| Level | Test |
| --- | --- |
| **Urgent** | Samples being lost or corrupted right now, or no rig can capture at all. |
| **High** | Silent integrity risk — a capture with missing samples finishes looking clean — or wrong metadata written into a recorded file. |
| **Medium** | Real defect with a workaround, wrong information shown to an operator, or enabling work that unblocks a High. |
| **Low** | Cosmetic, display-only, dead code with no callers. Would shipping it change any recorded byte? |

## Effort rubric

| Level | Shape |
| --- | --- |
| **Low** | One file, mechanical, obvious guard. One sitting. |
| **Medium** | Several files, a new harness or guard, or one real design decision. |
| **High** | Touches the capture path or a vendored dependency, needs all six upstream targets or a live-rig check — or root cause still unknown. |

## Body

Detailed Markdown, written for both Reece and a future session picking this up cold.
Headings, tables, `> [!IMPORTANT]` alerts, fenced evidence.

- **Quote decisive log lines or harness output inline** so the issue survives loss of the
  original artifact.
- **Pin source permalinks to a SHA**, never to `main`:
  `https://github.com/GDH-Technologies/MISRC-GUI/blob/<sha>/<path>#L<a>-L<b>`. For code
  that is upstream's, pin the same lines on **both** repos —
  `https://github.com/harrypm/MISRC-GUI/blob/<sha>/<path>#L<a>-L<b>` — so the eventual
  upstream PR can cite it.
- **Say whether the fix is upstream-bound or fork-only** (`.claude/rules/upstream.md`), and
  if upstream-bound, name the `/upstream-pr` shape you expect.
- End with a `## Provenance` section holding what you were doing when you found it, plus
  those permalinks. **Do not write a session table or a `🤖 Generated with` line** — a hook
  appends that automatically, and writing your own duplicates it.

## Never

- **Never open an issue on `harrypm/MISRC-GUI`** without Reece; upstream defects are filed
  here first and reach harrypm as a PR with evidence.
- **Never overwrite a human-set field value.** If your rubric disagrees, say so and leave it.
- **Never apply `bug` or `enhancement`.** Type covers that axis; those labels are slated for
  deletion here the moment issues are enabled.
