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

**You are in upstream's server/client code — decide which kind this change is before editing.**

`misrc_gui/net/` is upstream's subsystem (752182c, 2026-08-31), but the fork has diverged
from it since 2026-09-04: `923ad60` moved the `/rf` fanout into the fork's own
`gui_net_fanout.c` (fixing the replay, leak, lost wakeup and `server_stop()` use-after-free
of fork issue #42, closed 2026-09-06), and the `fix(net)`/`feat(net)` commits since add
fork behaviour: a client recording on this machine, the settings proxy, mode changes on the
main thread, the refused M-Client Local Save. Upstream rewrote its own fanout separately
(the `rf/audio_fanout_*` `/stats` telemetry). `.claude/CLAUDE.md` *Gotchas* (Net mode,
Monitor & Remote) is the map of what the fork keeps; read it first.

**Before you edit:**

- Say in the PR which kind the change is (`.claude/rules/upstream.md`). A generic fix to
  upstream's own code is upstream-bound and lands on fork `main` first; it reaches harrypm
  through `/upstream-pr` only when Reece asks for that.
- Keep the fork's net paths intact on every edit and sync: the legacy-client-only tests
  (`net_mode == GUI_NET_MODE_CLIENT`), `gui_app_effective_recording` /
  `gui_app_control_capturing`, and `gui_net_request_apply_mode` staying on the main thread.
- Attach evidence that runs headless: `misrc_tools/test/net_settings_e2e.sh` and the
  `--net-serve` / `--net-client-probe` modes, or a harness in `misrc_tools/test/`.
- Keep it C11 + POSIX/Winsock as the file already is; it must build on all six targets.
- Do not add GDH hosts, ports or policy here; the LAN/no-auth posture is upstream's call to
  change and is proposed separately.
