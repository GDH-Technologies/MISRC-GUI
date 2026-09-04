#!/usr/bin/env python3
"""Worktrees for MISRC-GUI -- WORKTREE-DEPS-CONTRACT v1

Claude Code calls this script as its `WorktreeCreate`, `WorktreeRemove` and `SessionEnd`
hook. A worktree of this repo is a plain single-repo checkout under
`<main>/.claude/worktrees/<dir>/`, branched from `origin/main`, with the main checkout's
vendored dependency prefixes reachable through symlinks so `scripts/build-local.sh` works
unchanged inside it. `.deps/` is gitignored and only ever built in the main checkout
(`scripts/build-deps-unix.sh`, `scripts/build-appimage-local.sh --native`); a worktree gets
a real `.deps/` directory holding `install*` symlinks back to the main checkout's, because
the ignore pattern `.deps/` matches a directory and not a bare symlink.

Naming: a topic with a `/` (`fix/foo`) becomes branch `fix/foo` in directory `fix+foo`; a
bare topic (`foo`) becomes branch `claude/foo` in directory `foo`. The directory mapping is
the one Claude Code applies natively, so hook-built and native worktrees sit side by side.
A topic under `upstream/` is branched from `upstream/main` instead of `origin/main` -- that
is the shape `/upstream-pr` needs, a branch that carries only cherry-picked fork commits.

Hook mode (no arguments): reads the JSON payload on stdin and dispatches on
`hook_event_name`. `WorktreeCreate` prints the absolute worktree path as the last line of
stdout and everything else on stderr. `WorktreeRemove` refuses (exit 2) unless the tree is
clean and holds no commit missing from its base; it never forces a removal and never deletes
a branch that is ahead. `SessionEnd` is a no-op here -- the global
`~/.claude/hooks/worktree-session-reclaim.py` owns that.

CLI: `status` lists this repo's worktrees with dirty/ahead/behind; `remove <topic>` runs the
same gate as the hook (`--force` skips only the commits-ahead gate, never the dirty gate).
Exit codes: 0 ok, 1 failed (a git command failed), 2 refused (a gate).

Bump the contract token above when the on-disk contract (layout, naming, base refs, exit
codes) changes, never silently.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

CONTRACT = 1
ROOT_SUBDIR = Path(".claude") / "worktrees"
DEFAULT_BASE = "origin/main"
UPSTREAM_BASE = "upstream/main"
BARE_PREFIX = "claude/"
NAME_MAX = 64
SEGMENT_RE = re.compile(r"^[A-Za-z0-9._-]+$")
FETCH_TIMEOUT = 20
CLAUDE_LOCK = re.compile(r"^claude (?:agent|session) .* \(pid (\d+)")
DEPS_DIR = ".deps"


class Refused(Exception):
    """A safety gate said no; nothing was changed that needs undoing."""


class Failed(Exception):
    """A git command failed."""


def log(msg: str) -> None:
    print(f"[worktree-deps] {msg}", file=sys.stderr, flush=True)


def git(args: list[str], cwd: Path | None = None, timeout: int = 60) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=str(cwd) if cwd else None, capture_output=True,
                          text=True, timeout=timeout, check=False)


def git_out(args: list[str], cwd: Path | None = None, timeout: int = 60) -> str | None:
    result = git(args, cwd, timeout)
    if result.returncode != 0:
        return None
    return result.stdout.strip()


# --------------------------------------------------------------------------- discovery


def main_checkout(path: Path) -> Path | None:
    """The main (non-linked) checkout that owns `path`, or None outside a git tree."""
    common = git_out(["-C", str(path), "rev-parse", "--path-format=absolute", "--git-common-dir"])
    if not common:
        return None
    return Path(common).resolve().parent


@dataclass
class Repo:
    main: Path

    @property
    def root(self) -> Path:
        return self.main / ROOT_SUBDIR


def discover(cwd: Path) -> Repo:
    main = main_checkout(cwd)
    if main is None:
        raise Refused(f"{cwd} is not inside a git checkout")
    return Repo(main)


def worktree_list(repo: Repo) -> list[dict[str, str]]:
    out = git_out(["-C", str(repo.main), "worktree", "list", "--porcelain"]) or ""
    entries: list[dict[str, str]] = []
    cur: dict[str, str] = {}
    for line in out.splitlines():
        if not line.strip():
            if cur:
                entries.append(cur)
            cur = {}
            continue
        key, _, value = line.partition(" ")
        cur[key] = value
    if cur:
        entries.append(cur)
    return entries


def registered(repo: Repo, path: Path) -> dict[str, str] | None:
    want = path.resolve()
    for entry in worktree_list(repo):
        if Path(entry.get("worktree", "")).resolve() == want:
            return entry
    return None


def branch_exists(repo: Repo, branch: str) -> bool:
    return git(["-C", str(repo.main), "rev-parse", "--verify", "--quiet",
                f"refs/heads/{branch}"]).returncode == 0


def branch_checkout_path(repo: Repo, branch: str) -> Path | None:
    for entry in worktree_list(repo):
        if entry.get("branch") == f"refs/heads/{branch}":
            return Path(entry["worktree"])
    return None


def fetch(repo: Repo, base: str) -> None:
    remote, _, ref = base.partition("/")
    result = git(["-C", str(repo.main), "fetch", "--quiet", remote, ref], timeout=FETCH_TIMEOUT)
    if result.returncode != 0:
        log(f"WARNING: fetch {remote} {ref} failed ({result.stderr.strip() or 'timeout'}); "
            f"using the last-fetched {base}")


# --------------------------------------------------------------------------- naming


def validate_name(name: str) -> None:
    if not name:
        raise Refused("worktree name is empty")
    if len(name) > NAME_MAX:
        raise Refused(f"worktree name is longer than {NAME_MAX} characters")
    segments = name.split("/")
    if len(segments) > 2:
        raise Refused(f"worktree name {name!r} has more than one '/'; use type/topic")
    for seg in segments:
        if seg in {"", ".", "..", ".git"} or not SEGMENT_RE.match(seg):
            raise Refused(f"invalid worktree name {name!r}: letters, digits, dot, underscore, "
                          f"dash, and at most one '/'")


def dir_for(name: str) -> str:
    return name.replace("/", "+")


def branch_for(name: str) -> str:
    return name if "/" in name else BARE_PREFIX + name


def base_for(branch: str) -> str:
    return UPSTREAM_BASE if branch.startswith("upstream/") else DEFAULT_BASE


# --------------------------------------------------------------------------- state


@dataclass
class State:
    path: Path
    registered: bool = False
    branch: str = ""
    head: str = ""
    base: str = DEFAULT_BASE
    dirty: list[str] = field(default_factory=list)
    ahead: int = 0
    behind: int = 0
    locked: str | None = None

    @property
    def clean(self) -> bool:
        return not self.dirty


def state(repo: Repo, path: Path) -> State:
    st = State(path=path)
    entry = registered(repo, path)
    if entry is None:
        return st
    st.registered = True
    st.locked = entry.get("locked")
    st.branch = entry.get("branch", "").removeprefix("refs/heads/")
    st.head = (entry.get("HEAD") or "")[:9]
    st.base = base_for(st.branch) if st.branch else DEFAULT_BASE
    porcelain = git_out(["-C", str(path), "status", "--porcelain", "--untracked-files=all"]) or ""
    st.dirty = [line[3:] for line in porcelain.splitlines() if line.strip()]
    counts = git_out(["-C", str(path), "rev-list", "--left-right", "--count", f"{st.base}...HEAD"])
    if counts:
        behind, ahead = counts.split()
        st.behind, st.ahead = int(behind), int(ahead)
    return st


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


# --------------------------------------------------------------------------- deps


def link_deps(repo: Repo, half: Path) -> None:
    src = repo.main / DEPS_DIR
    prefixes = sorted(p for p in src.iterdir() if p.is_dir() and p.name.startswith("install")) \
        if src.is_dir() else []
    if not prefixes:
        log(f"WARNING: {src} has no install* prefix; builds in this worktree will fail at "
            f"`meson setup` (hsdaoh not found). Run `scripts/build-appimage-local.sh --native` "
            f"or `scripts/build-deps-unix.sh` in {repo.main} once, then re-enter the worktree.")
        return
    dst_dir = half / DEPS_DIR
    dst_dir.mkdir(exist_ok=True)
    for prefix in prefixes:
        dst = dst_dir / prefix.name
        if dst.is_symlink() or dst.exists():
            continue
        os.symlink(prefix, dst)
    log(f"linked {DEPS_DIR}/{{{','.join(p.name for p in prefixes)}}} -> {src}")


def copy_settings_local(repo: Repo, half: Path) -> None:
    src = repo.main / ".claude" / "settings.local.json"
    dst = half / ".claude" / "settings.local.json"
    if src.is_file() and not dst.exists():
        try:
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(src.read_bytes())
        except OSError as exc:
            log(f"WARNING: could not copy settings.local.json into {half}: {exc}")


# --------------------------------------------------------------------------- create


def hook_create(payload: dict) -> int:
    cwd = Path(payload.get("cwd") or os.getcwd())
    name = payload.get("name") or Path(payload.get("worktree_path") or "").name
    validate_name(name)
    repo = discover(cwd)
    branch = branch_for(name)
    base = base_for(branch)
    half = repo.root / dir_for(name)

    if half.exists():
        entry = registered(repo, half)
        if entry is None or not (half / ".git").is_file():
            raise Refused(f"{half} exists and is not a registered worktree; inspect or remove it by hand")
        have = entry.get("branch", "").removeprefix("refs/heads/")
        if have and have != branch:
            log(f"WARNING: {half} is on {have}, not {branch}; using it as-is")
        st = state(repo, half)
        log(f"resuming {half} on {st.branch or 'detached HEAD'} @ {st.head}: "
            f"ahead {st.ahead} / behind {st.behind} of {st.base}, {len(st.dirty)} changed file(s)")
        link_deps(repo, half)
        print(str(half.resolve()))
        return 0

    fetch(repo, base)
    base_sha = git_out(["-C", str(repo.main), "rev-parse", "--verify", "--quiet", base])
    if not base_sha:
        raise Refused(f"{repo.main} has no {base}; add the remote and fetch it first")
    if branch_exists(repo, branch):
        elsewhere = branch_checkout_path(repo, branch)
        if elsewhere is not None:
            raise Refused(f"{branch} is already checked out at {elsewhere}")
        counts = git_out(["-C", str(repo.main), "rev-list", "--left-right", "--count",
                          f"{base}...{branch}"]) or "? ?"
        log(f"reusing existing branch {branch} (behind/ahead of {base}: {counts.replace(chr(9), ' ')})")
        args = ["worktree", "add", str(half), branch]
    else:
        args = ["worktree", "add", "--no-track", "-b", branch, str(half), base]
    repo.root.mkdir(parents=True, exist_ok=True)
    result = git(["-C", str(repo.main), *args], timeout=300)
    if result.returncode != 0:
        raise Failed(f"git {' '.join(args)} failed: {result.stderr.strip()}")
    log(f"created {half} on {branch} @ {base_sha[:9]} ({base})")
    link_deps(repo, half)
    copy_settings_local(repo, half)
    print(str(half.resolve()))
    return 0


# --------------------------------------------------------------------------- remove


def gate(st: State, force: bool) -> None:
    reasons = []
    if st.dirty:
        shown = ", ".join(st.dirty[:5]) + (f" (+{len(st.dirty) - 5} more)" if len(st.dirty) > 5 else "")
        reasons.append(f"{st.path} has uncommitted or untracked files: {shown}")
    if st.ahead > 0 and not force:
        reasons.append(f"{st.branch or 'detached HEAD'} in {st.path} has {st.ahead} commit(s) not on {st.base}")
    if reasons:
        raise Refused("; ".join(reasons) + "; worktree left in place")


def unlock_if_stale(repo: Repo, st: State) -> None:
    if not st.locked:
        return
    match = CLAUDE_LOCK.match(st.locked)
    if match and not pid_alive(int(match.group(1))):
        git(["-C", str(repo.main), "worktree", "unlock", str(st.path)])
        log(f"unlocked {st.path} (stale lock from pid {match.group(1)})")
    elif match:
        raise Refused(f"{st.path} is locked by a live Claude process (pid {match.group(1)}); "
                      f"end that session first")
    else:
        raise Refused(f"{st.path} is locked ({st.locked}); `git worktree unlock` it by hand")


def remove_worktree(repo: Repo, half: Path, force: bool) -> None:
    st = state(repo, half)
    if not st.registered:
        raise Refused(f"{half} is not a registered worktree of {repo.main}")
    gate(st, force)
    unlock_if_stale(repo, st)
    result = git(["-C", str(repo.main), "worktree", "remove", str(half)], timeout=120)
    if result.returncode != 0:
        raise Failed(f"git worktree remove {half} failed: {result.stderr.strip()}")
    log(f"removed {half}")
    if st.branch and st.ahead == 0 and branch_exists(repo, st.branch):
        on_main = git_out(["-C", str(repo.main), "symbolic-ref", "--quiet", "--short", "HEAD"])
        if on_main == st.branch:
            log(f"WARNING: {repo.main} is on {st.branch}; branch kept")
        else:
            # ahead == 0 against its base was just verified, so nothing is lost either way;
            # -d first because it is the documented rule, -D only where -d cannot see the
            # base (the main checkout's HEAD may itself be behind origin/main).
            result = git(["-C", str(repo.main), "branch", "-d", st.branch])
            if result.returncode != 0:
                result = git(["-C", str(repo.main), "branch", "-D", st.branch])
            if result.returncode == 0:
                log(f"deleted branch {st.branch} (fully contained in {st.base})")
            else:
                log(f"WARNING: could not delete {st.branch}: {result.stderr.strip()}")
    elif st.branch and st.ahead > 0:
        log(f"kept branch {st.branch} ({st.ahead} commit(s) ahead of {st.base})")
    git(["-C", str(repo.main), "worktree", "prune"])


def hook_remove(payload: dict) -> int:
    raw = payload.get("worktree_path")
    if not raw:
        raise Refused("no worktree_path in the payload; nothing removed")
    path = Path(raw)
    if not path.is_absolute() or not path.exists():
        raise Refused(f"{raw} is not an existing absolute path; nothing removed")
    main = main_checkout(path)
    if main is None:
        raise Refused(f"{path} is not a git worktree; nothing removed")
    repo = Repo(main)
    if path.resolve().parent != repo.root.resolve():
        raise Refused(f"{path} is not directly under {repo.root}; nothing removed")
    os.chdir(repo.main)
    remove_worktree(repo, path, force=False)
    return 0


# --------------------------------------------------------------------------- CLI


def cmd_status(args: argparse.Namespace) -> int:
    repo = discover(Path.cwd())
    rows = []
    for entry in worktree_list(repo):
        path = Path(entry["worktree"])
        if path.resolve() == repo.main.resolve():
            continue
        st = state(repo, path)
        rows.append(st)
    if args.json:
        print(json.dumps([{
            "path": str(s.path), "branch": s.branch, "head": s.head, "base": s.base,
            "dirty": s.dirty, "ahead": s.ahead, "behind": s.behind, "locked": s.locked,
        } for s in rows], indent=2))
        return 0
    if not rows:
        print("no worktrees")
        return 0
    for s in rows:
        flag = "locked " if s.locked else ""
        print(f"{s.path.name:34} {s.branch or 'detached':34} +{s.ahead}/-{s.behind} vs {s.base}  "
              f"dirty={len(s.dirty)} {flag}")
    return 0


def cmd_remove(args: argparse.Namespace) -> int:
    repo = discover(Path.cwd())
    half = repo.root / dir_for(args.topic)
    if not half.exists():
        raise Refused(f"{half} does not exist")
    os.chdir(repo.main)
    remove_worktree(repo, half, force=args.force)
    return 0


def run_hook() -> int:
    try:
        payload = json.load(sys.stdin)
    except (OSError, ValueError):
        log("unreadable hook payload")
        return 1
    if not isinstance(payload, dict):
        log("hook payload is not an object")
        return 1
    event = payload.get("hook_event_name")
    if event == "WorktreeCreate":
        return hook_create(payload)
    if event == "WorktreeRemove":
        return hook_remove(payload)
    return 0  # SessionEnd and anything else: nothing to do here


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="worktree-deps.py", description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command")
    p = sub.add_parser("status", help="list this repo's worktrees with dirty/ahead/behind")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_status)
    p = sub.add_parser("remove", help="remove a worktree under the safety gate")
    p.add_argument("topic")
    p.add_argument("--force", action="store_true", help="skip the commits-ahead gate (never the dirty gate)")
    p.set_defaults(func=cmd_remove)
    return parser


def main(argv: list[str]) -> int:
    try:
        if not argv:
            return run_hook()
        args = build_parser().parse_args(argv)
        if not getattr(args, "func", None):
            build_parser().print_help()
            return 1
        return args.func(args)
    except Refused as exc:
        log(f"REFUSED: {exc}")
        return 2
    except Failed as exc:
        log(f"FAILED: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
