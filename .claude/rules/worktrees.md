# Worktrees: hook-built, `.deps`-linked, single repo

- A worktree of this repo lives at `<main>/.claude/worktrees/<dir>/`, built by
  `.claude/hooks/worktree-deps.py` (the `WorktreeCreate` hook) whenever you run
  `claude --worktree <topic>`, call `EnterWorktree` with a name, or start a Claude Desktop
  session whose environment has **Worktree** checked for this repo. Nothing else creates
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
- **Claude Desktop.** An environment has a **Worktree** box for each of its folders.
  - **Checked:** Desktop runs this same hook for this repo at session start, whether MISRC-GUI
    is the main folder or an extra one. Verified 2026-11-26: a real `.deps/` and a
    `claude/<name>` branch.
  - **Name and base:** Desktop picks the name, so the tree always branches from `origin/main`.
    `/upstream-pr` needs an `upstream/<topic>` tree from `EnterWorktree`, which reaches only the
    main folder's repo. Do upstream work in a session whose main folder is MISRC-GUI.
  - **Unchecked:** the session's path for this repo is the main checkout, where Reece edits
    and commits and where `.deps/` is built. Treat it as read-only. To change code there, say
    the session has no worktree of this repo and ask for one started with the box checked.
    Desktop's "Add folder" adds no worktree, and `EnterWorktree` cannot make one for an extra
    folder.
- **As an extra folder, this repo's hooks do not run.** Only the main folder's
  `.claude/settings.json` applies. When MISRC-GUI is an extra folder, none of these fire:
  `guards-on-edit.py`, `build-on-stop.sh`, or its permission denies (`git push --force`,
  `pip install`, `git worktree add`). Run `/build` and the guard suite yourself before
  claiming a change works.
- If creation fails, stop and report the hook's stderr. Do not fall back to working in the
  main checkout, and do not create a worktree by hand.
- A worktree Claude Code created natively before the hook existed (branch `worktree-…`)
  opens by name unchanged; the hook links its `.deps` on entry.
- Remove through `ExitWorktree`. The hook refuses a dirty tree or commits not on the
  worktree's base (`origin/main`, or `upstream/main` for `upstream/*`); it never forces, and
  it deletes the branch only when nothing would be lost. `.claude/hooks/worktree-deps.py
  status` lists worktrees; `remove <topic>` runs the same gate from a shell.
- Finish work with `/open-pr` (fork `main`) or `/upstream-pr`. Never merge locally into `main`.
