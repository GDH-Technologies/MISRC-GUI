---
name: block-git-stash
enabled: true
event: bash
action: block
pattern: (^|[;&|]|\n)\s*git\s+stash(?!\s+(list|show))
---

**Blocked: `git stash` is banned in this repo.**

`.claude/CLAUDE.md` -> *Worktrees and git hygiene*: "Never `git stash` — the stack is shared
with Reece's live work; make a WIP commit."

The stash stack is per-repository, not per-worktree and not per-session. Anything pushed
onto it lands in the same stack Reece is using in his own trees, and a later pop by either
of you replays the wrong changes into the wrong tree. Subagents have violated this rule
before, so it is enforced rather than suggested.

**Do this instead:**

- Parking work in progress -> make a **WIP commit** on the branch, amend or drop later.
- Needing a clean tree to switch branches -> commit, or use a worktree under
  `.claude/worktrees/`.
- Inspecting the stack -> `git stash list` and `git stash show` are still allowed.

If you are editing this rule file itself, use the Edit tool, not a Bash heredoc.
