#!/usr/bin/env python3
"""Is GitHub Actions actually able to run our jobs right now, and did the deploy branch's tip get a run?

Answer that before anyone debugs a workflow.

Motivated by 2026-08-06, when a major Actions outage created ZERO runs for pushes to main
for over three hours. Nothing went red — a run that is never created cannot fail — so the
fleet silently served stale code while several hours went into debugging a workflow that
was fine. See tk#222.

TWO independent signals, because either alone is a trap:

  1. githubstatus.com component state — authoritative but LAGS; the 15:22Z incident was
     already an hour old before it read `major_outage`.
  2. Did the deploy workflow's push branch's tip actually produce a run — the symptom
     itself, and the thing that matters. True even when the status page says all clear.

`--fleet` compares each host's deployed SHA against that same branch's tip on origin,
which is the only real proof a deploy landed (run outcomes lie: see the
evicted-pending-run case in tk#222).

## Repo-agnostic by construction

Nothing here names a repository. The slug comes from `origin`, and the deploy workflow —
whichever file under `.github/workflows/` declares `DEPLOY_CANDIDATES` — supplies the host
list, the on-host repo directory, and the `paths-ignore` set. That is what lets this file be
byte-identical in digitization-toolkit and capture-node, whose fleets, deploy paths, and
workflow filenames all differ. Hardcoding any of it is what let the previous generation of
GitHub tooling rot into listing labels that did not exist.

## As a hook

With no arguments and JSON on stdin, this runs as a `PreToolUse` hook. It fires when a
tool call is about to read or act on CI state — `gh run`/`gh workflow`, a PR check-status
read, a merge — and stays **completely silent** when the platform is healthy. Only a
degraded or undetermined verdict produces output, as `additionalContext`. It never blocks.

The verdict is cached briefly so back-to-back CI calls cost nothing.

## As a command

    .claude/hooks/ci-platform-status.py            # platform + push-trigger check
    .claude/hooks/ci-platform-status.py --fleet    # also SSH each host for its SHA
    .claude/hooks/ci-platform-status.py --quiet    # exit code only

Exit codes (command mode only; as a hook it always exits 0):

    0  Actions usable — push triggers are landing runs
    1  DEGRADED — outage, or the deploy branch's tip produced no run: work around it
    2  Undetermined — no network / not authenticated. NOT a green light.

Needs `gh` (authenticated) for the run and fleet checks.
"""

from __future__ import annotations

import configparser
import json
import os
import re
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

STATUS_URL = os.environ.get("STATUS_URL", "https://www.githubstatus.com/api/v2/summary.json")

# A push landing a run is normally seconds. Below this we cannot distinguish "not created"
# from "not created YET", so a young tip is never a symptom.
STALE_PUSH_MINUTES = int(os.environ.get("STALE_PUSH_MINUTES", "10"))

CACHE_TTL_SECONDS = int(os.environ.get("CI_STATUS_CACHE_TTL", "300"))

OK, DEGRADED, UNKNOWN = 0, 1, 2

# Tool calls that mean "about to read or act on CI state". A degraded platform changes how
# every one of these is interpreted: a missing check is not a failing check, and merging on
# checks that never ran is the dangerous case.
PR_READ = "mcp__plugin_github_github__pull_request_read"
CI_SENSITIVE_TOOLS = {
    PR_READ,
    "mcp__plugin_github_github__merge_pull_request",
    "mcp__plugin_github_github__create_pull_request",
    "mcp__plugin_github_github__update_pull_request_branch",
}
PR_READ_METHODS = {"get_check_runs", "get_status", "get"}
GH_CLI_PATTERN = re.compile(r"\bgh\s+(run|workflow)\b")


class Report:
    """Findings accumulate as flags, never as a max() over the exit codes.

    DEGRADED must outrank UNKNOWN because it is the confident answer, and 2 > 1 numerically
    would have inverted exactly that. Flags also mean no branch can clear a finding an
    earlier branch recorded.
    """

    def __init__(self) -> None:
        self.saw_degraded = False
        self.saw_unknown = False
        # Tracked separately from `saw_degraded`: a stale fleet host is a real finding worth
        # exit 1, but "the platform is the suspect" is the wrong advice for it. Only a
        # platform finding earns the outage playbook.
        self.saw_platform_issue = False
        self.lines: list[tuple[str, str]] = []

    def ok(self, message: str) -> None:
        self.lines.append(("info", message))

    def degraded(self, message: str, *, platform: bool = True) -> None:
        self.saw_degraded = True
        self.saw_platform_issue = self.saw_platform_issue or platform
        self.lines.append(("warn", message))

    def unknown(self, message: str) -> None:
        self.saw_unknown = True
        self.lines.append(("warn", message))

    @property
    def verdict(self) -> int:
        if self.saw_degraded:
            return DEGRADED
        if self.saw_unknown:
            return UNKNOWN
        return OK

    def warnings(self) -> list[str]:
        return [text for level, text in self.lines if level == "warn"]


# --------------------------------------------------------------------------- helpers


def repo_root() -> Path:
    env = os.environ.get("CLAUDE_PROJECT_DIR")
    if env and (Path(env) / ".git").exists():
        return Path(env)
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / ".git").exists():
            return parent
    return here.parent.parent.parent


def run(cmd: list[str], *, cwd: Path | None = None, timeout: int = 30) -> str:
    """Run a command, returning stdout. Empty string on any failure — never raises."""
    try:
        result = subprocess.run(
            cmd,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def succeeds(cmd: list[str], *, timeout: int = 30) -> bool:
    try:
        return subprocess.run(cmd, capture_output=True, timeout=timeout).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


# ------------------------------------------------------------------------- 0. discovery


def _git_dir(root: Path) -> Path | None:
    """Resolve the real .git directory, following the `gitdir:` file a worktree leaves."""
    dot = root / ".git"
    if dot.is_dir():
        return dot
    if not dot.is_file():
        return None
    try:
        match = re.match(r"gitdir:\s*(.+)", dot.read_text(encoding="utf-8").strip())
    except OSError:
        return None
    if not match:
        return None
    gitdir = Path(match.group(1))
    if not gitdir.is_absolute():
        gitdir = (root / gitdir).resolve()
    # A linked worktree's gitdir has no `config` of its own; commondir points at the one
    # that does.
    commondir = gitdir / "commondir"
    if commondir.is_file():
        try:
            target = Path(commondir.read_text(encoding="utf-8").strip())
        except OSError:
            return gitdir
        gitdir = target if target.is_absolute() else (gitdir / target).resolve()
    return gitdir


def _slug_from_url(url: str) -> str:
    match = re.search(r"[:/]([^/:]+/[^/:]+?)(?:\.git)?/?$", url.strip())
    return match.group(1) if match else ""


def repo_slug(root: Path) -> str:
    """`owner/name` for origin. Never hardcoded — this file ships to more than one repo."""
    override = os.environ.get("REPO_SLUG")
    if override:
        return override

    slug = _slug_from_url(run(["git", "remote", "get-url", "origin"], cwd=root))
    if slug:
        return slug

    # Fall back to reading .git/config directly. `git` is not always on PATH — a hardened
    # hook environment is the obvious case, and it is exactly when this must still work.
    gitdir = _git_dir(root)
    if gitdir is None:
        return ""
    parser = configparser.ConfigParser()
    try:
        parser.read(gitdir / "config", encoding="utf-8")
        return _slug_from_url(parser.get('remote "origin"', "url"))
    except (OSError, configparser.Error):
        return ""


def _glob_to_regex(pattern: str) -> re.Pattern[str]:
    """Translate a GitHub Actions path glob. `**` crosses directories, `*` does not."""
    out = ""
    index = 0
    while index < len(pattern):
        if pattern.startswith("**/", index):
            out += "(?:.*/)?"
            index += 3
        elif pattern.startswith("**", index):
            out += ".*"
            index += 2
        elif pattern[index] == "*":
            out += "[^/]*"
            index += 1
        elif pattern[index] == "?":
            out += "[^/]"
            index += 1
        else:
            out += re.escape(pattern[index])
            index += 1
    return re.compile(f"^{out}$")


def _extract_push_branch(text: str) -> str:
    """First branch named under this workflow's `on: push: branches:` block.

    Deliberately loose: it scans forward from the first `push:` occurrence for the next
    bracketed `branches:` list, rather than fully parsing YAML structure, so a comment line
    (every deploy workflow here carries one) between `push:` and `branches:` cannot defeat
    it. Good enough for a hook whose job is a dispatch hint, not a YAML validator.
    """
    push_index = text.find("push:")
    if push_index == -1:
        return ""
    match = re.search(r"branches:\s*\[([^\]]*)\]", text[push_index:])
    if not match:
        return ""
    branches = [item.strip().strip("'\"") for item in match.group(1).split(",")]
    return branches[0] if branches and branches[0] else ""


class DeploySpec:
    """Everything repo-specific, read from the deploy workflow rather than hardcoded.

    The workflow is the single source of truth on purpose: this check exists to say whether
    a deploy landed, so reading the host list or the on-host path from anywhere else would
    let the checker and the thing it checks drift apart silently.
    """

    def __init__(self, root: Path, slug: str) -> None:
        self.slug = slug
        self.workflow: str = ""
        self.candidates: list[str] = []
        self.default_repo_dir: str = str(root)
        self.repo_dirs: dict[str, str] = {}
        self.ignore: list[re.Pattern[str]] = []
        self.push_branch: str = ""

        path = self._find_workflow(root)
        if path is None:
            return
        self.workflow = path.name
        try:
            text = path.read_text(encoding="utf-8")
        except OSError:
            return

        match = re.search(r"^\s*DEPLOY_CANDIDATES:\s*(.+)$", text, re.MULTILINE)
        if match:
            self.candidates = match.group(1).split()

        # Three idioms in the wild: a bash DEFAULT_REPO_DIR with a REPO_DIRS override map
        # (the legacy toolkit shape, which needed a macOS exception), a single YAML
        # REPO_DIR (legacy capture-node, uniform hosts), or -- since the Phase 6 env-root
        # flip -- NO literal at all: the deploy job computes
        # "${HOME}/gdhvc-${ENV_NAME}/${REPO_NAME}" on-host. For that shape the fleet path
        # is `$HOME/gdhvc-<env>/<repo>`, uniform across every host INCLUDING macOS (the
        # /home-vs-/Users prefix is exactly what $HOME absorbs); consumers expand it
        # locally with os.path.expandvars and remotely by double-quoting inside the ssh
        # command so the remote shell expands it.
        match = re.search(r"^\s*DEFAULT_REPO_DIR=(\S+)", text, re.MULTILINE) or re.search(
            r"^\s*REPO_DIR:\s*(\S+)", text, re.MULTILINE
        )
        if match:
            self.default_repo_dir = match.group(1)
        else:
            env_m = re.search(r"^\s*ENV_NAME:\s*(\w+)", text, re.MULTILINE)
            repo_m = re.search(r"^\s*REPO_NAME:\s*(\S+)", text, re.MULTILINE)
            if env_m and repo_m and "gdhvc-${ENV_NAME}" in text:
                self.default_repo_dir = f"$HOME/gdhvc-{env_m.group(1)}/{repo_m.group(1)}"

        block = re.search(r"declare\s+-A\s+REPO_DIRS=\(([^)]*)\)", text, re.DOTALL)
        if block:
            self.repo_dirs = dict(re.findall(r"\[([\w.-]+)\]=(\S+)", block.group(1)))

        match = re.search(r"^\s*paths-ignore:\s*\[(.+)\]", text, re.MULTILINE)
        if match:
            self.ignore = [
                _glob_to_regex(glob) for glob in re.findall(r"['\"]([^'\"]+)['\"]", match.group(1))
            ]

        self.push_branch = _extract_push_branch(text)

    @staticmethod
    def _find_workflow(root: Path) -> Path | None:
        """The workflow file that supplies the deploy-fleet facts.

        `deploy-preview.yml` and `deploy-production.yml` both carry `DEPLOY_CANDIDATES`
        (the env-root migration's replacement for the retired `deploy-self-hosted.yml`),
        so more than one file can match. When that happens, prefer the workflow whose push
        trigger targets `production` — that is the one whose fleet state this hook (and
        its dispatch hint) actually needs to reason about. A single match, e.g. in a repo
        that has not migrated, is returned as-is.
        """
        directory = root / ".github" / "workflows"
        try:
            files = sorted(
                path for path in directory.iterdir() if path.suffix in (".yml", ".yaml")
            )
        except OSError:
            return None
        candidates: list[tuple[Path, str]] = []
        for path in files:
            try:
                text = path.read_text(encoding="utf-8")
            except OSError:
                continue
            if "DEPLOY_CANDIDATES" in text:
                candidates.append((path, text))
        if not candidates:
            return None
        if len(candidates) == 1:
            return candidates[0][0]
        for path, text in candidates:
            if _extract_push_branch(text) == "production":
                return path
        return candidates[0][0]

    @property
    def branch(self) -> str:
        """The branch push-delivery and fleet-staleness checks compare against.

        Falls back to `production` — the fleet's shipped bar post-cutover — rather than
        any literal `main`, since this file must stay correct in a repo whose deploy
        workflow's `on: push: branches:` names something else entirely.
        """
        return self.push_branch or "production"

    @property
    def dispatch_command(self) -> str:
        workflow = self.workflow or "<deploy workflow>"
        target = f" -R {self.slug}" if self.slug else ""
        return f"gh workflow run {workflow}{target} --ref {self.branch}"

    def repo_dir(self, host: str) -> str:
        return self.repo_dirs.get(host, self.default_repo_dir)

    def is_deployable(self, path: str) -> bool:
        """True when a changed path would actually trigger the deploy workflow."""
        return not any(pattern.match(path) for pattern in self.ignore)


def degraded_guidance(spec: DeploySpec) -> str:
    return f"""\
Do NOT debug the workflow first — the platform is the suspect.

  • Deliver by dispatch, not by push. workflow_dispatch served jobs normally throughout the
    2026-08-06 outage while push created nothing:
        {spec.dispatch_command}
  • Verify by fleet SHA, never by run outcome. Run outcomes lie during an outage.
        .claude/hooks/ci-platform-status.py --fleet
  • Cancel runs that can never start. A run queued with no runner holds the single pending
    slot of its concurrency group and will evict the next legitimate run.
  • A workflow fix does not repair already-queued runs. push and workflow_dispatch runs
    execute the workflow file from the SHA being deployed, not from {spec.branch}'s tip.
    Dispatch on --ref {spec.branch} to get the fixed definition.
  • Say the platform is degraded in the summary you give Reece, with the timestamp.
    "CI is red" and "CI never ran" demand completely different responses.

This is not a licence to skip verification — a degraded platform changes HOW you verify
(SHA on the host instead of a green check), never WHETHER."""


def stale_fleet_guidance(spec: DeploySpec) -> str:
    return f"""\
The platform is fine — these hosts are simply behind origin/{spec.branch}. That is a
deploy question, not a workflow bug.

  • Re-run the deploy for the stale hosts:
        {spec.dispatch_command}
  • A host can also be behind because its deploy was busy-deferred (a running capture or
    decode blocks the restart), which is by design. Check before re-deploying.
  • An unreachable host is not necessarily stale — it may just be powered off."""


# ------------------------------------------------------------------- 1. platform status


def check_platform(report: Report) -> None:
    try:
        with urllib.request.urlopen(STATUS_URL, timeout=15) as response:  # noqa: S310
            summary = json.loads(response.read().decode("utf-8"))
    except Exception:  # noqa: BLE001 - any failure means "unknown", never "healthy"
        report.unknown("Could not reach githubstatus.com — platform state UNKNOWN, not healthy.")
        return

    state = "unknown"
    for component in summary.get("components") or []:
        if component.get("name") == "Actions":
            state = component.get("status") or "unknown"
            break

    if state == "operational":
        report.ok("Actions: operational")
    elif state in ("degraded_performance", "partial_outage"):
        report.degraded(f"Actions: {state} — expect slow or missing runs")
    elif state == "major_outage":
        report.degraded("Actions: MAJOR OUTAGE — runs may not be created at all")
    else:
        report.unknown(f"Actions: '{state}' (unrecognised) — treating as undetermined")

    # A component can read operational while an incident is still being worked; the
    # incident is the earlier signal of the two.
    for incident in summary.get("incidents") or []:
        if incident.get("status") in ("resolved", "postmortem"):
            continue
        names = " ".join(
            [incident.get("name") or ""]
            + [c.get("name") or "" for c in incident.get("components") or []]
        )
        if "actions" in names.lower():
            report.degraded(
                f"Open incident touching Actions: [{incident.get('status')}] "
                f"{incident.get('name')} (opened {incident.get('created_at')})"
            )


# --------------------------------------------------------------- 2. push-trigger delivery


def check_push_delivery(report: Report, root: Path, spec: DeploySpec) -> None:
    if not succeeds(["gh", "auth", "status"]):
        report.unknown("gh unavailable or not authenticated — skipping run checks.")
        return
    if not spec.slug:
        report.unknown("Could not determine the origin repo — skipping run checks.")
        return

    branch = spec.branch
    subprocess.run(
        ["git", "fetch", "origin", branch, "--quiet"],
        cwd=str(root),
        capture_output=True,
        timeout=60,
    )
    tip = run(["git", "rev-parse", f"origin/{branch}"], cwd=root)
    if not tip:
        report.unknown(f"Could not resolve origin/{branch} — skipping.")
        return

    # Commit date, not push date — GitHub does not expose push time here. A rebased-then-
    # pushed commit reads older than it is, which only ever makes this check MORE eager.
    tip_epoch = int(run(["git", "log", "-1", "--format=%ct", tip], cwd=root) or "0")
    age_min = int((time.time() - tip_epoch) / 60) if tip_epoch else 0

    # The deploy workflow carries paths-ignore, so a docs-only or tests-only merge
    # LEGITIMATELY produces zero runs. Classify the tip the same way the workflow does — with
    # the workflow's own globs — or every docs merge reads as an outage.
    changed = run(["git", "diff", "--name-only", f"{tip}^", tip], cwd=root).splitlines()
    deployable = [path for path in changed if path and spec.is_deployable(path)]

    raw = run(
        [
            "gh", "run", "list", "-R", spec.slug, "-L", "40",
            "--json", "databaseId,headSha,event,status,conclusion,createdAt,workflowName",
        ],
        timeout=60,
    )
    try:
        runs = json.loads(raw) if raw else None
    except ValueError:
        runs = None
    if runs is None:
        report.unknown("Could not list workflow runs — state undetermined.")
        return

    # Ask for the tip's runs BY COMMIT, never by filtering the recent listing: a branch
    # that has been quiet for days has rotated its runs out of any recent-N window, and
    # filtering that window reads as "no run was ever created" — the outage signature —
    # for what is actually a normal quiet week. On 2026-08-18 this fired ~15 times
    # against a production tip 6.7 days old whose runs completed fine on 08-11, while
    # runs were being created and finishing normally all day (githubstatus: operational).
    # The recent listing above still feeds the queued-with-no-runner sweep below, which
    # genuinely wants recency.
    tip_raw = run(
        [
            "gh", "run", "list", "-R", spec.slug, "-L", "40", "--commit", tip,
            "--json", "databaseId,headSha,event,status,conclusion,createdAt,workflowName",
        ],
        timeout=60,
    )
    try:
        tip_runs = json.loads(tip_raw) if tip_raw else None
    except ValueError:
        tip_runs = None
    if tip_runs is None:
        report.unknown("Could not list the tip's workflow runs — state undetermined.")
        return
    report.ok(f"origin/{branch} tip: {tip[:7]} ({age_min}m old) — {len(tip_runs)} run(s)")

    if not tip_runs:
        if not spec.workflow:
            report.unknown("No push-triggered deploy workflow found — cannot judge delivery.")
        elif not deployable:
            report.ok("Tip touches only ignored paths — paths-ignore means no run is expected.")
        elif age_min >= STALE_PUSH_MINUTES:
            report.degraded(
                f"No workflow run exists for {branch}'s tip after {age_min}m. This is the "
                "outage signature: a run that is never created never fails."
            )
        else:
            report.ok(f"Tip is only {age_min}m old — too young to call. Re-check shortly.")

    # A run queued with no runner assigned is the OTHER freeze mode (unsatisfiable runs-on,
    # or no runner able to accept work).
    for entry in runs:
        if entry.get("status") != "queued":
            continue
        run_id = entry.get("databaseId")
        assigned = run(
            [
                "gh", "api", f"repos/{spec.slug}/actions/runs/{run_id}/jobs",
                "--jq", '[.jobs[].runner_name // ""] | map(select(length > 0)) | length',
            ]
        )
        if assigned == "0":
            report.degraded(
                f"Run {run_id} is queued with NO runner assigned — it may never start. "
                f"Inspect: gh run view {run_id} -R {spec.slug}"
            )


# ------------------------------------------------------------------------- 3. fleet SHAs


def check_fleet(report: Report, root: Path, spec: DeploySpec) -> None:
    if not spec.candidates:
        report.unknown("No DEPLOY_CANDIDATES found in any workflow — cannot check the fleet.")
        return

    tip = run(["git", "rev-parse", f"origin/{spec.branch}"], cwd=root)

    # The host running this must not be probed over SSH. `ssh wm` from wm fails host-key
    # verification and would report the local machine "unreachable" — the single most
    # misleading answer this check can give. Match on IP: the /etc/hosts aliases (wm) and
    # the real hostnames (workflow-master) never compare equal.
    local_ips = set((run(["hostname", "-I"]) or "").split()) | {"127.0.0.1"}

    for host in spec.candidates:
        repo_dir = spec.repo_dir(host)

        resolved = run(["getent", "hosts", host]).split()
        host_ip = resolved[0] if resolved else ""

        if host_ip and host_ip in local_ips:
            host_sha = run(["git", "rev-parse", "HEAD"], cwd=Path(os.path.expandvars(repo_dir)))
        else:
            # Double quotes, not single: the env-root idiom's `$HOME/...` path must be
            # expanded by the REMOTE shell (each host's own home differs, /home/rdodge
            # vs /Users/dodge). Literal absolute paths pass through both quotings
            # identically.
            host_sha = run(
                [
                    "ssh", "-o", "ConnectTimeout=6", "-o", "BatchMode=yes", host,
                    f'git -C "{repo_dir}" rev-parse HEAD 2>/dev/null',
                ],
                timeout=20,
            )

        if not host_sha:
            report.unknown(f"{host}: unreachable")
        elif host_sha == tip:
            report.ok(f"{host}: {host_sha[:7]} — current")
        else:
            report.degraded(
                f"{host}: {host_sha[:7]} — STALE (origin/{spec.branch} is {tip[:7]})",
                platform=False,
            )


# ------------------------------------------------------------------------------ evaluate


def evaluate(
    *, fleet: bool = False, root: Path | None = None, spec: DeploySpec | None = None
) -> tuple[Report, DeploySpec]:
    report = Report()
    root = root or repo_root()
    spec = spec or DeploySpec(root, repo_slug(root))
    check_platform(report)
    check_push_delivery(report, root, spec)
    if fleet:
        check_fleet(report, root, spec)
    return report, spec


# --------------------------------------------------------------------------- hook mode


def cache_path(slug: str) -> Path:
    """Per-repo cache file. The push-delivery half of the verdict is repo-specific, so a
    single shared file would let one repo serve the other repo's answer."""
    cache_home = os.environ.get("XDG_CACHE_HOME") or (Path.home() / ".cache")
    key = re.sub(r"[^A-Za-z0-9_.-]", "-", slug) or "unknown-repo"
    return Path(cache_home) / "gdhvc" / f"ci-platform-status.{key}.json"


def cached_verdict(slug: str) -> dict | None:
    path = cache_path(slug)
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if time.time() - payload.get("checked_at", 0) > CACHE_TTL_SECONDS:
        return None
    return payload


def store_verdict(slug: str, payload: dict) -> None:
    path = cache_path(slug)
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(payload), encoding="utf-8")
    except OSError:
        pass


def is_ci_sensitive(tool_name: str, tool_input: dict) -> bool:
    if tool_name in CI_SENSITIVE_TOOLS:
        if tool_name == PR_READ:
            return tool_input.get("method") in PR_READ_METHODS
        return True
    if tool_name in ("Bash", "PowerShell"):
        return bool(GH_CLI_PATTERN.search(tool_input.get("command") or ""))
    return False


def hook_context(payload: dict) -> str | None:
    """Return additionalContext for a degraded/undetermined platform, else None."""
    if not is_ci_sensitive(payload.get("tool_name", ""), payload.get("tool_input") or {}):
        return None

    # Built up front, not just on a cache miss: the guidance text is repo-specific and has
    # to match this repo even when the verdict itself came from the cache.
    root = repo_root()
    spec = DeploySpec(root, repo_slug(root))

    cached = cached_verdict(spec.slug)
    if cached is None:
        report, _ = evaluate(root=root, spec=spec)
        cached = {
            "checked_at": time.time(),
            "verdict": report.verdict,
            "warnings": report.warnings(),
        }
        store_verdict(spec.slug, cached)

    verdict = cached.get("verdict", OK)
    if verdict == OK:
        return None

    findings = "\n".join(f"  • {line}" for line in cached.get("warnings") or [])

    if verdict == DEGRADED:
        return (
            "GitHub Actions looks DEGRADED — check the platform before blaming the "
            f"workflow.\n\n{findings}\n\n{degraded_guidance(spec)}"
        )
    return (
        "GitHub Actions status is UNDETERMINED — a check could not complete. This is NOT a "
        f"green light.\n\n{findings}\n\n"
        "Re-check once network and gh auth are available before concluding anything about "
        "a workflow."
    )


# ------------------------------------------------------------------------ command mode


def emit(report: Report, spec: DeploySpec, quiet: bool) -> None:
    if quiet:
        return
    tty = sys.stdout.isatty()
    green, yellow, reset = ("\033[0;32m", "\033[0;33m", "\033[0m") if tty else ("", "", "")
    for level, text in report.lines:
        tag = f"{green}[INFO]{reset}" if level == "info" else f"{yellow}[WARN]{reset}"
        print(f"{tag} {text}")

    print()
    if report.verdict == OK:
        print(f"{green}[INFO]{reset} Actions is usable and push triggers are landing runs.")
        print("  If a workflow still misbehaves, the workflow is genuinely at fault.")
    elif report.verdict == DEGRADED and report.saw_platform_issue:
        print(f"{yellow}[WARN]{reset} DEGRADED — do not debug the workflow first.")
        print(degraded_guidance(spec))
    elif report.verdict == DEGRADED:
        print(f"{yellow}[WARN]{reset} Actions is healthy, but the fleet is not in sync.")
        print(stale_fleet_guidance(spec))
    else:
        print(f"{yellow}[WARN]{reset} UNDETERMINED — a check could not complete. Not a green light.")
        print("  Re-run once network / gh auth is available before concluding anything.")


def main(argv: list[str]) -> int:
    if "-h" in argv or "--help" in argv:
        print(__doc__)
        return 0

    unknown = [arg for arg in argv if arg not in ("--fleet", "--quiet", "-q")]
    if unknown:
        print(f"unknown argument: {unknown[0]}", file=sys.stderr)
        return 2

    # Mode is decided by what is actually on stdin, not by tty detection: run from a
    # terminal, from a pipeline, or from a non-interactive shell, an empty or unparseable
    # stdin always means "the human asked for a report".
    payload = None
    if not argv and not sys.stdin.isatty():
        try:
            raw = sys.stdin.read()
            candidate = json.loads(raw) if raw.strip() else None
            if isinstance(candidate, dict) and "hook_event_name" in candidate:
                payload = candidate
        except (ValueError, OSError):
            payload = None

    if payload is None:
        report, spec = evaluate(fleet="--fleet" in argv)
        emit(report, spec, quiet="--quiet" in argv or "-q" in argv)
        return report.verdict

    # Hook mode: always exit 0.
    try:
        context = hook_context(payload)
    except Exception:  # noqa: BLE001 - a broken hook must never wedge a tool call
        return 0

    if context:
        json.dump(
            {
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "additionalContext": context,
                }
            },
            sys.stdout,
        )
        sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
