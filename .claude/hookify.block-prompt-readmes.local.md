---
name: block-prompt-readmes
enabled: true
event: file
action: block
conditions:
  - field: file_path
    operator: regex_match
    pattern: (^|/)PROMPT_[A-Z0-9_]+_README\.md$
---

**Blocked: `PROMPT_*_README.md` files are harrypm's agent working logs.**

`.claude/rules/upstream.md` -> *Upstream owns these*. They record upstream's own
request/design/verification history for a feature and are the best source on *why* a
subsystem is shaped the way it is (`PROMPT_SERVER_CLIENT_README.md` is the whole story of the
net mode). Read them; never edit them — an edit is a guaranteed merge conflict and a change
to someone else's record.

**Do this instead:** fork-side notes go in `docs/gdh-*.md` or `docs/superpowers/`; a
correction to upstream's understanding goes in an upstream PR body or issue.
