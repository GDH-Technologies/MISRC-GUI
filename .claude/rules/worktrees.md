# Worktrees: hook-built, `.deps`-linked, single repo

- A worktree of this repo lives at `<main>/.claude/worktrees/<dir>/`, built by
  `.claude/hooks/worktree-deps.py` (the `WorktreeCreate` hook) whenever you run
  `claude --worktree <topic>` or call `EnterWorktree` with a name. Nothing else creates
  worktrees: never `git worktree add` by hand (the global location guard refuses it anyway),
  never edit `.gitignore` for worktrees.
- **Naming.** `type/topic` (e.g. `fix/clay-text-lifetime`) becomes branch `fix/clay-text-lifetime`
  in directory `fix+clay-text-lifetime`; a bare `topic` becomes branch `claude/topic` in
  directory `topic`. A topic under `upstream/` is branched from `upstream/main` instead of
  `origin/main` — that is the branch shape `/upstream-pr` needs.
- **Deps.** `.deps/` (vendored hsdaoh, raylib) is gitignored and exists only in the main
  checkout. The hook gives the worktree a real `.deps/` directory containing `install*`
  symlinks back to the main checkout's prefixes, so `scripts/build-local.sh` works unchanged.
  If the hook warns that no prefix exists, run `scripts/build-appimage-local.sh --native`
  in the main checkout once and re-enter. Build dirs (`build-local/`) are per-worktree.
- If creation fails, stop and report the hook's stderr. Do not fall back to working in the
  main checkout, and do not create a worktree by hand.
- A worktree Claude Code created natively before the hook existed (branch `worktree-…`)
  opens by name unchanged; the hook links its `.deps` on entry.
- Remove through `ExitWorktree`. The hook refuses a dirty tree or commits not on the
  worktree's base (`origin/main`, or `upstream/main` for `upstream/*`); it never forces, and
  it deletes the branch only when nothing would be lost. `.claude/hooks/worktree-deps.py
  status` lists worktrees; `remove <topic>` runs the same gate from a shell.
- Finish work with `/open-pr` (fork `main`) or `/upstream-pr`. Never merge locally into `main`.
