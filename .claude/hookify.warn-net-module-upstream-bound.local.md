---
name: warn-net-module-upstream-bound
enabled: true
event: file
action: warn
conditions:
  - field: file_path
    operator: regex_match
    pattern: misrc_tools/misrc_gui/net/gui_net\.[ch]$
---

**You are in upstream's server/client mode — this change is upstream-bound.**

`misrc_gui/net/` is upstream's newest subsystem (752182c, 2026-08-31) and is byte-identical
between the fork and `harrypm/MISRC-GUI`; no fork commit has ever touched it. Its `/rf`
fanout is known-broken (replay, unbounded leak, lost wakeup; `server_stop()` frees state under
live client threads — `.claude/CLAUDE.md` *Gotchas*), and GDH has chosen this mode as its
remote-seat transport, so fixes here are the first upstream PRs.

**Do it the upstream way:**

- Work on an `upstream/<topic>` worktree (branched from `upstream/main`), not on a fork
  branch, so the diff is exactly the proposal — `/upstream-pr`.
- Attach evidence harrypm can run: the fanout harness pattern (extract `net_fanout_*`
  verbatim, drive it with a producer and a subscriber, print bytes produced vs delivered and
  malloc/free counts) belongs in `misrc_tools/test/` as a guard harness.
- Keep it C11 + POSIX/Winsock as the file already is; it must build on all six targets.
- Do not add GDH hosts, ports or policy here; the LAN/no-auth posture is upstream's call to
  change and is proposed separately.
