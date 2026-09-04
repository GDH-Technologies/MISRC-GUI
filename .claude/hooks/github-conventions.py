#!/usr/bin/env python3
"""PreToolUse hook for the GitHub MCP tools.

Repo-agnostic: nothing here names a repository, so this file is byte-identical in
digitization-toolkit and capture-node. The conventions it enforces are org-wide.

Two jobs, both on `PreToolUse`:

1. **Provenance injection.** Appends a session-provenance footer to the body of a new
   issue or a new pull request, via `hookSpecificOutput.updatedInput`. Idempotent: a body
   that already carries the footer marker is left alone.

2. **Convention checks.** Warns — never blocks — when an `issue_write` create is missing a
   required field, or when an issue is being closed, in which case it re-states the four
   gates that must pass first.

Everything here reads UNDOCUMENTED Claude Code internals: `~/.claude/sessions/<pid>.json`,
whose schema the docs call internal and subject to change on any release. So every field is
optional and every failure is soft — a row whose source has moved or vanished is dropped,
and the hook still exits 0 with a usable footer. A provenance footer that blocked
`create_pull_request` would be worse than no footer.

Unlike the bash predecessor this replaces, the hook does not have to guess at its own
identity: `session_id` and `transcript_path` arrive on stdin.

Run it directly to render the footer for debugging:

    CLAUDE_CODE_SESSION_ID=<id> .claude/hooks/github-conventions.py --render
"""

from __future__ import annotations

import json
import os
import socket
import sys
from datetime import datetime
from pathlib import Path

FOOTER_MARKER = "🤖 Generated with"
FOOTER_LINK = "[Claude Code](https://claude.com/claude-code)"

ISSUE_WRITE = "mcp__plugin_github_github__issue_write"
CREATE_PR = "mcp__plugin_github_github__create_pull_request"

REQUIRED_ASSIGNEE = "radodge"
REQUIRED_FIELDS = ("Priority", "Effort")

CLOSING_GATES = """\
Closing gates — confirm all four before this close lands (see /close-issue):

  1. On origin/dev?  `git fetch origin --quiet && git merge-base --is-ancestor <sha> origin/dev`
     Merged is not on dev, and a branch is not dev at all. (production is the shipped/fleet
     bar, not the closing gate.)
  2. Whole issue covered?  Map every numbered defect to a state, as a table, in the comment.
     Partial delivery is the most common false close.
  3. Operational half done?  Fleet restart, backfill, DKMS rebuild, live-rig smoke.
     Verify the deployed state; do not assume it.
  4. Sub-issues closed?  A parent closes last. Sub-issues can be cross-repo.

A guard is not a fix. A prior comment claiming "fixed" is not proof. Age is not a reason.
Post the closing comment, with real command output as evidence, before closing."""


# --------------------------------------------------------------------------- provenance


def _config_dir() -> Path:
    return Path(os.environ.get("CLAUDE_CONFIG_DIR") or Path.home() / ".claude")


def _session_record(session_id: str) -> dict:
    """Find ~/.claude/sessions/<pid>.json whose sessionId matches ours."""
    if not session_id:
        return {}
    try:
        candidates = sorted((_config_dir() / "sessions").glob("*.json"))
    except OSError:
        return {}
    for path in candidates:
        try:
            record = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(record, dict) and record.get("sessionId") == session_id:
            return record
    return {}


def _models_from_transcript(transcript_path: str) -> list[str]:
    """The set of models used. A session can switch models mid-run, so report all of them.

    `<synthetic>` is the model field on locally generated error/interrupt messages, not a
    model anyone used.
    """
    if not transcript_path:
        return []
    seen: dict[str, None] = {}
    try:
        with open(transcript_path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if '"model"' not in line:
                    continue
                try:
                    entry = json.loads(line)
                except ValueError:
                    continue
                if entry.get("type") != "assistant":
                    continue
                model = (entry.get("message") or {}).get("model")
                if model and model != "<synthetic>":
                    seen.setdefault(model, None)
    except OSError:
        return []
    return list(seen)


def _format_elapsed(seconds: float) -> str:
    seconds = int(seconds)
    if seconds < 60:
        return "<1m"
    hours, minutes = divmod(seconds // 60, 60)
    return f"{hours}h {minutes:02d}m" if hours else f"{minutes}m"


def _row(label: str, value: str) -> str:
    return f"| **{label}** | {value} |\n"


def render_footer(payload: dict) -> str:
    """Build the provenance block. Any row whose source is unavailable is dropped."""
    session_id = payload.get("session_id") or os.environ.get("CLAUDE_CODE_SESSION_ID", "")
    record = _session_record(session_id)

    body = f"{FOOTER_MARKER} {FOOTER_LINK}\n\n| | |\n| --- | --- |\n"

    name = record.get("name")
    if name:
        value = f"`{name}`"
        if session_id:
            value += f" · `{session_id[:8]}`"
        body += _row("Session", value)

    started = record.get("startedAt")
    if isinstance(started, (int, float)):
        try:
            when = datetime.fromtimestamp(started / 1000).astimezone()
            elapsed = _format_elapsed(datetime.now().astimezone().timestamp() - started / 1000)
            body += _row("Started", f"{when:%Y-%m-%d %H:%M %Z} · {elapsed} earlier")
        except (OverflowError, OSError, ValueError):
            pass

    models = _models_from_transcript(payload.get("transcript_path", ""))
    if models:
        body += _row("Model(s)", ", ".join(f"`{model}`" for model in models))

    effort = (payload.get("effort") or {}).get("level") or os.environ.get("CLAUDE_EFFORT")
    parts = [part for part in (record.get("version"), record.get("entrypoint")) if part]
    if effort:
        parts.append(f"effort `{effort}`")
    if parts:
        body += _row("Claude Code", " · ".join(parts))

    cwd = payload.get("cwd") or os.getcwd()
    body += _row("Host", f"{socket.gethostname().split('.')[0]} · `{cwd}`")

    if session_id:
        body += _row("Resume", f"`claude --resume {session_id}`")

    # Only Remote Control gives a browsable URL, and only once the bridge has connected.
    # Its absence is normal; never invent one.
    bridge = record.get("bridgeSessionId")
    if bridge:
        body += f"\nhttps://claude.ai/code/{bridge}\n"

    return body


def append_footer(tool_input: dict, payload: dict) -> dict | None:
    """Return a copy of tool_input with the footer appended, or None if nothing to do."""
    existing = tool_input.get("body") or ""
    if FOOTER_MARKER in existing:
        return None
    try:
        footer = render_footer(payload)
    except Exception:  # noqa: BLE001 - never block a call on footer breakage
        return None
    updated = dict(tool_input)
    updated["body"] = f"{existing.rstrip()}\n\n---\n\n{footer}" if existing.strip() else footer
    return updated


# ---------------------------------------------------------------------------- checks


def check_issue_create(tool_input: dict) -> list[str]:
    missing = []

    assignees = tool_input.get("assignees") or []
    if REQUIRED_ASSIGNEE not in assignees:
        missing.append(f"assignees — must include `{REQUIRED_ASSIGNEE}`")

    if not tool_input.get("type"):
        missing.append("type — one of Bug · Feature · Task · Optimization · Extraction")

    if not (tool_input.get("labels") or []):
        # No -R: `gh label list` reads the repo of the working directory, which keeps this
        # hint correct in whichever repo the hook is installed in.
        missing.append(
            "labels — at least one component label, from the live set (`gh label list`)"
        )

    set_fields = {
        (field.get("field_name") or "").lower()
        for field in tool_input.get("issue_fields") or []
        if isinstance(field, dict)
    }
    for field in REQUIRED_FIELDS:
        if field.lower() not in set_fields:
            missing.append(f"issue_fields — {field} is not set")

    return missing


def evaluate(payload: dict) -> dict:
    """Build the hook response for one PreToolUse payload."""
    tool_name = payload.get("tool_name", "")
    tool_input = payload.get("tool_input") or {}
    output: dict = {"hookEventName": "PreToolUse"}
    notes: list[str] = []

    if tool_name == CREATE_PR:
        updated = append_footer(tool_input, payload)
        if updated is not None:
            output["updatedInput"] = updated

    elif tool_name == ISSUE_WRITE:
        method = tool_input.get("method")

        if method == "create":
            updated = append_footer(tool_input, payload)
            if updated is not None:
                output["updatedInput"] = updated

            missing = check_issue_create(tool_input)
            if missing:
                notes.append(
                    "This issue is missing required fields (see /file-issue):\n"
                    + "\n".join(f"  • {item}" for item in missing)
                )

        if tool_input.get("state") == "closed":
            notes.append(CLOSING_GATES)
            if not tool_input.get("state_reason"):
                notes.append(
                    "state_reason is not set — close with `completed`, `not_planned`, "
                    "or `duplicate` (with duplicate_of). Never close silently."
                )

    if notes:
        output["additionalContext"] = "\n\n".join(notes)

    return output


# ------------------------------------------------------------------------------ main


def main() -> int:
    if "--render" in sys.argv[1:]:
        print(render_footer({"session_id": os.environ.get("CLAUDE_CODE_SESSION_ID", "")}), end="")
        return 0

    try:
        payload = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0
    if not isinstance(payload, dict):
        return 0

    try:
        output = evaluate(payload)
    except Exception:  # noqa: BLE001 - a broken hook must never wedge a tool call
        return 0

    if len(output) > 1:  # more than just hookEventName
        json.dump({"hookSpecificOutput": output}, sys.stdout)
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
