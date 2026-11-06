#!/usr/bin/env python3
"""Open one fix PR per component for the advisories a scan found.

Usage:
    fix.py --config advisories.toml --findings advisories-findings.json --outcomes advisories-fix.json [--base main] [--root DIR] [--dry-run]

Reads the scan's findings and acts on the unignored ones, one component at a
time; `EOL` findings and GitHub Actions components are skipped, so clearing is
judged over the remaining advisories. Only Rust crates get a fix; every other
component is alert-only.

A crate's target is the highest of its advisories' fixed versions, so no fix
reopens another. Its locked version below the target and semver-compatible
with it (one at most: cargo locks one version per compatible line) is repinned
with `just bazel-repin-crate <crate>@<locked>=<target>`, then the crates are
re-scanned with OSV-Scanner and this tree's ignores. Only when no unignored
advisory for the crate remains does it get a branch. A crate whose only
locked versions below the target are of an older semver line, or whose
older-line version is still flagged after the repin, needs a major bump of the
nearest `crate.spec` dependent, found by walking the reverse dependencies of
`Cargo.Bazel.lock`, and is alert-only. A target at or below
every locked version, a repin that leaves `Cargo.Bazel.lock` unchanged, or
one that leaves an advisory standing opens no PR: "at or past the fix, still
flagged; needs an ignore".

Each fix lives on `advisories/<component>`, rebuilt from `origin/<base>` as
one commit, never rebased. An open PR is rebuilt and pushed with
`--force-with-lease` only when its advisory set or target changed, and is left
alone when its branch holds more than that one commit over its merge base (a
maintainer's follow-up). A branch without an open PR is replaced. A PR closed
unmerged suppresses a new one while the component's advisory set equals the
ids in its body. PRs from forks never count, whatever their branch name.
The checkout must be clean: each crate starts from a forced checkout of the
base. `--dry-run` prints each branch, title and body instead of
pushing; it reads the PRs but changes none.

`--outcomes` gets one entry per component, rewritten after each, so a crash
still leaves the earlier ones for the issue.

Exit codes: 0 every component handled, 2 usage, 3 unreadable input or a
component whose fix failed (the others still run).
"""

import _model  # noqa: F401  (Python version check, before tomllib)

import argparse
import datetime
import json
import re
import sys
import tempfile
import tomllib
from collections import deque
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path

import inventory
import scan
from _model import EXIT_CLEAN, EXIT_ERROR, ToolError, branch_name, cyclonedx

OPENED, UPDATED, CURRENT, DRY_RUN = "opened", "updated", "current", "dry-run"
LEFT_ALONE, DECLINED, NEEDS_IGNORE, ALERT, ERROR = "left-alone", "declined", "needs-ignore", "alert-only", "error"
NEEDS_IGNORE_NOTE = "at or past the fix, still flagged; needs an ignore"


@dataclass(frozen=True)
class Outcome:
    """What the fix job did about one component, as the issue shows it."""

    component: str
    source: str
    ids: tuple[str, ...]
    status: str
    note: str
    target: str | None = None
    branch: str | None = None
    pr: str | None = None

    def to_json(self) -> dict:
        return {**asdict(self), "ids": list(self.ids)}


# ── Versions ──────────────────────────────────────────────────────────────────

_SEMVER = re.compile(r"^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")


def semver_key(version: str) -> tuple:
    """Semver precedence: a prerelease sorts before its release, its parts numerically where numeric."""
    m = _SEMVER.match(version)
    if not m:
        raise ToolError(f"{version!r} is not a semver version")
    core = tuple(int(n or 0) for n in m.group(1, 2, 3))
    pre = m.group(4)
    if pre is None:
        return (*core, 1, ())
    return (*core, 0, tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")))


def compatible(locked: str, target: str) -> bool:
    """Whether a caret requirement on `locked` (cargo's default) admits `target`."""
    a, b = semver_key(locked)[:3], semver_key(target)[:3]
    # The leftmost non-zero component and every one before it must match.
    n = next((i for i, x in enumerate(a) if x), 2) + 1
    return a[:n] == b[:n]


# ── Cargo.Bazel.lock ──────────────────────────────────────────────────────────


def locked_versions(lock: dict, name: str) -> list[str]:
    return sorted((p["version"] for p in lock.get("package", []) if p["name"] == name and "source" in p), key=semver_key)


def nearest_spec_dependent(lock: dict, name: str, version: str) -> str:
    """The `crate.spec` crate closest to `name@version` along reverse dependencies; itself when direct.

    The specs are the dependencies of the lock's source-less package, the
    workspace crate_universe synthesizes from them.
    """
    packages = lock.get("package", [])
    versions: dict[str, list[str]] = {}
    for p in packages:
        versions.setdefault(p["name"], []).append(p["version"])

    def ref(dep: str) -> tuple[str, str]:
        # A dependency names a version only when the lock holds several of that crate.
        dep_name, _, dep_version = dep.split(" (")[0].partition(" ")
        if not dep_version:
            if len(versions.get(dep_name, [])) != 1:
                raise ToolError(f"Cargo.Bazel.lock: dependency {dep!r} names no single locked version")
            dep_version = versions[dep_name][0]
        return dep_name, dep_version

    specs = {ref(d) for p in packages if "source" not in p for d in p.get("dependencies", [])}
    dependents: dict[tuple[str, str], list[tuple[str, str]]] = {}
    for p in packages:
        if "source" in p:
            for d in p.get("dependencies", []):
                dependents.setdefault(ref(d), []).append((p["name"], p["version"]))

    start = (name, version)
    seen, queue = {start}, deque([start])
    while queue:
        node = queue.popleft()
        if node in specs:
            return node[0]
        for parent in sorted(dependents.get(node, []), key=lambda n: (n[0], semver_key(n[1]))):
            if parent not in seen:
                seen.add(parent)
                queue.append(parent)
    raise ToolError(f"Cargo.Bazel.lock: no crate.spec dependency reaches {name} {version}")


def plan_crate(name: str, findings: list[dict], lock: dict) -> tuple[str, str, list[str]] | Outcome:
    """`(locked, target, older)` to repin, or the outcome that makes a repin pointless.

    `older` are the locked versions below the target on an older semver line:
    the scan names one version per merged finding, so whether they are
    affected shows only in the re-scan after the repin.
    """
    ids = tuple(sorted(f["id"] for f in findings))

    def outcome(status: str, note: str, target: str | None = None) -> Outcome:
        return Outcome(name, "cargo", ids, status, note, target=target)

    unfixed = sorted(f["id"] for f in findings if not f["fixed"])
    if unfixed:
        return outcome(ALERT, f"alert only: no released fix for {', '.join(unfixed)}")
    target = max((f["fixed"] for f in findings), key=semver_key)
    # The scan merges a crate's versions into one finding, so the lock says which are locked.
    below = [v for v in locked_versions(lock, name) if semver_key(v) < semver_key(target)]
    if not below:
        return outcome(NEEDS_IGNORE, NEEDS_IGNORE_NOTE, target)
    same_line = [v for v in below if compatible(v, target)]
    older = [v for v in below if not compatible(v, target)]
    if same_line:
        return same_line[0], target, older
    return outcome(ALERT, major_bump_note(lock, name, older[-1]), target)


def major_bump_note(lock: dict, name: str, version: str) -> str:
    return f"alert only: needs a major bump of {nearest_spec_dependent(lock, name, version)}"


# ── Branches and PRs ──────────────────────────────────────────────────────────

CREATE, REBUILD = "create", "rebuild"
_IDS_LINE = re.compile(r"^Advisories: (.+)$", re.MULTILINE)
_TARGET_LINE = re.compile(r"^Resolved version: (\S+)$", re.MULTILINE)


def title(component: str, target: str, ids: tuple[str, ...]) -> str:
    return f"fix(deps): raise {component} to {target} past {', '.join(ids)}"


def body(component: str, locked: str, target: str, ids: tuple[str, ...]) -> str:
    """The commit body, which is also the PR body; its last two lines are what `parse_body` reads."""
    return (
        f"Repins `{component}` from {locked} to {target}, the lowest version past every open\n"
        f"advisory against it, with `just bazel-repin-crate {component}@{locked}={target}`.\n"
        f"A re-scan of the crates with this tree's ignores reports none of them.\n"
        f"Opened by the daily advisory scan (`.github/workflows/advisories.yml`).\n"
        f"\n"
        f"Advisories: {', '.join(ids)}\n"
        f"Resolved version: {target}\n"
    )


def parse_body(text: str) -> tuple[tuple[str, ...], str | None]:
    """The advisory ids and the resolved version a fix PR's body names."""
    ids = _IDS_LINE.search(text or "")
    target = _TARGET_LINE.search(text or "")
    return (tuple(sorted(i.strip() for i in ids.group(1).split(","))) if ids else ()), (target.group(1) if target else None)


@dataclass(frozen=True)
class PR:
    number: int
    url: str
    body: str


@dataclass(frozen=True)
class BranchState:
    head: str | None
    commits: int
    open_pr: PR | None
    declined: tuple[PR, ...]


def decide(state: BranchState, ids: tuple[str, ...], target: str) -> str:
    if state.open_pr:
        if state.commits > 1:
            return LEFT_ALONE
        return CURRENT if parse_body(state.open_pr.body) == (ids, target) else REBUILD
    if any(parse_body(pr.body)[0] == ids for pr in state.declined):
        return DECLINED
    return CREATE


# ── Running it ────────────────────────────────────────────────────────────────


@dataclass
class Context:
    root: Path
    config: dict
    ignores: list
    today: datetime.date
    base: str
    dry_run: bool
    run: Callable[[list[str], Path], tuple[int, str, str]]

    def cmd(self, *cmd: str) -> str:
        code, out, err = self.run(list(cmd), self.root)
        if code:
            raise ToolError(f"{' '.join(cmd[:3])} exited {code}: {err.strip()[-500:]}")
        return out

    @property
    def cargo_lock(self) -> str:
        return self.config["inputs"]["cargo_lock"]

    def lock(self) -> dict:
        return tomllib.loads((self.root / self.cargo_lock).read_text())


def branch_state(ctx: Context, branch: str) -> BranchState:
    code, out, _ = ctx.run(["git", "rev-parse", "--verify", "--quiet", f"refs/remotes/origin/{branch}"], ctx.root)
    head = out.strip() if code == 0 else None
    commits = int(ctx.cmd("git", "rev-list", "--count", f"origin/{ctx.base}..{head}")) if head else 0

    def prs(state: str) -> list[dict]:
        out = ctx.cmd("gh", "pr", "list", "--head", branch, "--state", state, "--json", "number,url,body,state,isCrossRepository")
        # `--head` matches the branch name only; a fork's branch of that name is not ours.
        return [p for p in json.loads(out) if not p["isCrossRepository"]]

    def pr(p: dict) -> PR:
        return PR(p["number"], p["url"], p.get("body") or "")

    open_prs = prs("open")
    # `--state closed` includes merged PRs (state MERGED), which decline nothing.
    declined = tuple(pr(p) for p in prs("closed") if p["state"] == "CLOSED")
    return BranchState(head, commits, pr(open_prs[0]) if open_prs else None, declined)


def flagged_versions(ctx: Context, name: str) -> set[str]:
    """The locked versions of `name` with an unignored advisory, per OSV-Scanner over the tree's `Cargo.Bazel.lock`."""
    with tempfile.TemporaryDirectory(prefix="advisories-fix-") as tmp:
        sbom = Path(tmp) / "cargo.cdx.json"
        sbom.write_text(json.dumps(cyclonedx(inventory.crates(ctx.lock()))))
        # Unmerged: merging would keep one version per advisory.
        found = scan.normalize_osv(scan.osv_scan(sbom, ctx.run))
    return {f.version for f in found if f.component == name and not scan.matching_ignore(f, ctx.ignores, ctx.today)}


def changed_paths(ctx: Context) -> set[str]:
    return {line[3:] for line in ctx.cmd("git", "status", "--porcelain", "--untracked-files=no").splitlines() if line}


def fix_crate(ctx: Context, name: str, findings: list[dict]) -> Outcome:
    planned = plan_crate(name, findings, ctx.lock())
    if isinstance(planned, Outcome):
        return planned
    locked, target, older = planned
    ids = tuple(sorted(f["id"] for f in findings))
    branch = branch_name(name)

    def outcome(status: str, note: str, pr: PR | str | None = None) -> Outcome:
        return Outcome(name, "cargo", ids, status, note, target, branch, pr.url if isinstance(pr, PR) else pr)

    state = branch_state(ctx, branch)
    action = decide(state, ids, target)
    if action == CURRENT:
        return outcome(CURRENT, "fix PR open", state.open_pr)
    if action == LEFT_ALONE:
        return outcome(LEFT_ALONE, "fix PR branch has follow-up commits; left alone", state.open_pr)
    if action == DECLINED:
        declined = next(pr for pr in state.declined if parse_body(pr.body)[0] == ids)
        return outcome(DECLINED, "fix PR closed unmerged; needs an ignore", declined)

    ctx.cmd("git", "checkout", "--quiet", "-B", branch, f"origin/{ctx.base}")
    ctx.cmd("just", "bazel-repin-crate", f"{name}@{locked}={target}")
    changed = changed_paths(ctx)
    allowed = {ctx.cargo_lock, ctx.config["inputs"]["module_lock"]}
    if changed - allowed:
        raise ToolError(f"the repin changed {', '.join(sorted(changed - allowed))}, not only {', '.join(sorted(allowed))}")
    if ctx.cargo_lock not in changed:
        return outcome(NEEDS_IGNORE, NEEDS_IGNORE_NOTE)
    if target not in locked_versions(ctx.lock(), name):
        raise ToolError(f"the repin did not lock {name} {target}")
    flagged = flagged_versions(ctx, name)
    if flagged & set(older):
        return outcome(ALERT, major_bump_note(ctx.lock(), name, min(flagged & set(older), key=semver_key)))
    if flagged:
        return outcome(NEEDS_IGNORE, NEEDS_IGNORE_NOTE)

    subject, text = title(name, target, ids), body(name, locked, target, ids)
    ctx.cmd("git", "add", "--", *sorted(changed))
    ctx.cmd("git", "commit", "--quiet", "-m", subject, "-m", text)
    if ctx.dry_run:
        print(f"== {branch} ({action})\n{subject}\n\n{text}")
        return outcome(DRY_RUN, f"dry run: would {'open' if action == CREATE else 'update'} a fix PR")
    ctx.cmd("git", "push", "--quiet", f"--force-with-lease=refs/heads/{branch}:{state.head or ''}", "origin", f"HEAD:refs/heads/{branch}")
    if action == REBUILD:
        ctx.cmd("gh", "pr", "edit", str(state.open_pr.number), "--title", subject, "--body", text)
        return outcome(UPDATED, "fix PR updated", state.open_pr)
    url = ctx.cmd("gh", "pr", "create", "--base", ctx.base, "--head", branch, "--title", subject, "--body", text).strip()
    return outcome(OPENED, "fix PR opened", url)


def fix_component(ctx: Context, name: str, source: str, findings: list[dict]) -> Outcome:
    if source == "cargo":
        # From the base, whatever the previous component left behind; the checkout was clean on entry.
        ctx.cmd("git", "checkout", "--quiet", "--force", "--detach", f"origin/{ctx.base}")
        return fix_crate(ctx, name, findings)
    return Outcome(name, source, tuple(sorted(f["id"] for f in findings)), ALERT, f"alert only: no fix PR for {source} components")


def actionable(doc: dict) -> dict[tuple[str, str], list[dict]]:
    """The unignored findings by (component, source), without `EOL` and GitHub Actions."""
    out: dict[tuple[str, str], list[dict]] = {}
    for f in doc["findings"]:
        if not f["ignored"] and f["id"] != "EOL" and f["source"] != "githubactions":
            out.setdefault((f["component"], f["source"]), []).append(f)
    return dict(sorted(out.items()))


def fix_all(ctx: Context, doc: dict, outcomes_path: Path) -> list[Outcome]:
    outcomes: list[Outcome] = []
    # Each crate starts with a forced checkout, which would discard local edits.
    if changed_paths(ctx):
        raise ToolError(f"{ctx.root} has uncommitted changes")
    ctx.cmd("git", "fetch", "--quiet", "--prune", "origin")
    for (name, source), findings in actionable(doc).items():
        try:
            o = fix_component(ctx, name, source, findings)
        except Exception as e:  # noqa: BLE001  (one component failing must not stop the others)
            print(f"fix: {name}: error: {type(e).__name__}: {e}", file=sys.stderr)
            o = Outcome(name, source, tuple(sorted(f["id"] for f in findings)), ERROR, "fix failed; see the run log")
        print(f"fix: {name}: {o.status}: {o.note}", file=sys.stderr)
        outcomes.append(o)
        outcomes_path.write_text(json.dumps({"outcomes": [x.to_json() for x in outcomes]}, indent=2) + "\n")
    return outcomes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", required=True, type=Path, help="advisories.toml")
    parser.add_argument("--findings", required=True, type=Path, help="findings JSON from scan.py")
    parser.add_argument("--outcomes", required=True, type=Path, help="per-component outcomes JSON to write")
    parser.add_argument("--base", default="main", help="branch the fixes start from and target (default: main)")
    parser.add_argument("--root", default=Path("."), type=Path, help="checkout to repin in (default: .)")
    parser.add_argument("--dry-run", action="store_true", help="print branches, titles and bodies; push nothing")
    args = parser.parse_args(argv)
    try:
        args.outcomes.write_text(json.dumps({"outcomes": []}) + "\n")
        config = tomllib.loads(args.config.read_text())
        ignores = scan.load_ignores((args.config.parent / config["ignores"]).read_text())
        doc = json.loads(args.findings.read_text())
        ctx = Context(args.root, config, ignores, scan.utc_today(), args.base, args.dry_run, scan.run_process)
        outcomes = fix_all(ctx, doc, args.outcomes)
    except Exception as e:  # noqa: BLE001  (a crash must never read as handled)
        print(f"fix: error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    return EXIT_ERROR if any(o.status == ERROR for o in outcomes) else EXIT_CLEAN


if __name__ == "__main__":
    sys.exit(main())
