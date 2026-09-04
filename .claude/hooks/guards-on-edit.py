#!/usr/bin/env python3
"""PostToolUse hook: run the static guard suite when a guard-matched file is edited.

Claude Code runs this after every Edit/Write (matcher in .claude/settings.json) with the
tool payload as JSON on stdin. Almost every edit exits 0 immediately. Only a path that the
guards in misrc_tools/test/ci_guard_tests.py substring-match -- the fork deploy workflow,
meson.build, the build scripts, git-version.sh, the suite itself and its C harnesses --
triggers the ~2 s static pass, so a guard regression is caught at edit time instead of at
PR CI on wm.

Exit 0: not guard-matched, suite passed, disabled, or unreadable payload (this hook must
never block an edit on its own bug). Exit 2: the suite failed; the failing guard names go
to stderr, which Claude Code shows to the model. The edit itself is not reverted.

The suite is run from the checkout that owns the edited file (found by walking up from the
path), so a worktree session tests its own tree, not the main checkout.

MISRC_GUARDS_ON_EDIT=0 disables it for a session.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

GUARD_PATHS = re.compile(
    r"(^|/)\.github/workflows/selfhosted-deploy\.ya?ml$"
    r"|(^|/)misrc_tools/meson(\.build|_options\.txt)$"
    r"|(^|/)scripts/(build-[A-Za-z0-9-]+\.(sh|ps1)|fetch-mediamtx\.sh|publish-deps-cache\.sh)$"
    r"|(^|/)misrc_tools/git-version\.sh$"
    r"|(^|/)misrc_tools/test/(ci_guard_tests\.py|[A-Za-z0-9_]+_harness\.c)$"
)
SUITE = Path("misrc_tools") / "test" / "ci_guard_tests.py"
TIMEOUT_S = 90


def checkout_for(path: Path, fallback: Path) -> Path | None:
    """The checkout that owns `path` (nearest ancestor holding the suite), else `fallback`."""
    for parent in path.parents:
        if (parent / SUITE).is_file():
            return parent
    return fallback if (fallback / SUITE).is_file() else None


def main() -> int:
    if os.environ.get("MISRC_GUARDS_ON_EDIT") == "0":
        return 0
    try:
        payload = json.load(sys.stdin)
    except (OSError, ValueError):
        return 0
    if not isinstance(payload, dict):
        return 0
    raw = str((payload.get("tool_input") or {}).get("file_path") or "")
    if not raw or not GUARD_PATHS.search(raw):
        return 0
    fallback = Path(payload.get("cwd") or os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd())
    root = checkout_for(Path(raw), fallback)
    if root is None:
        return 0
    try:
        result = subprocess.run([sys.executable, str(SUITE), "--static-only"], cwd=str(root),
                                capture_output=True, text=True, timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print(f"[guards-on-edit] static guard suite timed out after {TIMEOUT_S}s", file=sys.stderr)
        return 2
    if result.returncode == 0:
        return 0
    lines = (result.stdout + result.stderr).splitlines()
    fails = [ln for ln in lines if ln.startswith(("FAIL", "ERROR", "Traceback"))] or lines[-8:]
    print(f"[guards-on-edit] static guard suite FAILED after editing {raw}", file=sys.stderr)
    for ln in fails[:12]:
        print(f"  {ln}", file=sys.stderr)
    print("  reproduce: python3 misrc_tools/test/ci_guard_tests.py --static-only", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
