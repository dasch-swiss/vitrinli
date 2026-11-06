#!/usr/bin/env python3
"""Open one fix PR per component (one for all Debian packages) for the advisories a scan found.

Usage:
    fix.py --config advisories.toml --findings advisories-findings.json --outcomes advisories-fix.json [--base main] [--root DIR] [--dry-run]

Reads the scan's findings and acts on the unignored ones, one component at a
time; `EOL` findings and GitHub Actions components are skipped, so clearing is
judged over the remaining advisories. Rust crates, BCR modules and the base
image get a fix, and so do the Debian packages, together; every other
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
the cargo lock (`inputs.cargo_lock`, `Cargo.Bazel.lock`), and is alert-only. A
target at or below every locked version, a repin that leaves the lock
unchanged, or one that leaves an advisory standing opens no PR: "at or past
the fix, still flagged; needs an ignore".

A BCR module's target is the lowest non-yanked version in the registry's
`metadata.json` at or past the highest fixed version, in Bazel's module
version order. Its `bazel_dep`, and a `single_version_override` with a
`version`, are raised to it and the lock refreshed with `just
bazel-lock-update`; a Grype re-scan of the module at the new version, by its
component map CPEs, must report no unignored advisory. A module held by an
override with `patches` or `patch_cmds`, or reached only transitively, is
alert-only, as is one whose fix has no registry version yet.

The base image's target is the current index digest of the tag
`[base_image] tracks` names (`crane digest`), written into its `oci.pull`
with the tag and the scan date as the line's comment. Its findings are the
image's packages, so the fix covers those with a released fix: a Grype
re-scan of the new digest on every configured platform must leave none of
them standing. The same digest as pinned needs an ignore.

Every Debian package is fixed by one change, so their findings share one fix
on `advisories/debian`: the snapshot timestamp of the deb manifest's source
URLs moves to the scan date and the lock is regenerated with `just
bazel-distroless-lock`; a Grype re-scan of the regenerated lock must leave
none of the advisories with a released fix standing. Each flagged package
with a released fix gets that fix's outcome; an open PR is rebuilt only when
the advisory set changes, not for the newer date. A snapshot already at the
scan date, or a lock the new snapshot leaves unchanged, needs an ignore.

Each fix lives on `advisories/<component>`, rebuilt from `origin/<base>` as
one commit, never rebased. An open PR is rebuilt and pushed with
`--force-with-lease` only when its advisory set or target changed, and is left
alone when its branch holds more than that one commit over its merge base (a
maintainer's follow-up). A branch without an open PR is replaced. A PR closed
unmerged suppresses a new one while the component's advisory set equals the
ids in its body. PRs from forks never count, whatever their branch name.
The checkout must be clean: each component starts from a forced checkout of
the base. `--dry-run` prints each branch, title and body instead of
pushing; it reads the PRs but changes none.

`--outcomes` gets one entry per flagged component (each Debian package with a
released fix gets the shared Debian fix's, one without is alert-only), rewritten after each, so a crash still leaves the
earlier ones for the issue.

Exit codes: 0 every component handled, 2 usage, 3 unreadable input or a
component whose fix failed (the others still run).
"""

import _model  # noqa: F401  (Python version check, before tomllib)

import argparse
import datetime
import http.client
import json
import re
import sys
import tempfile
import time
import tomllib
import urllib.error
import urllib.request
from collections import deque
from collections.abc import Callable
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path

import inventory
import scan
from _model import EXIT_CLEAN, EXIT_ERROR, Component, ToolError, actionable_finding, branch_name, cyclonedx, fix_component

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


# ── The cargo lock ────────────────────────────────────────────────────────────


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
                raise ToolError(f"cargo lock: dependency {dep!r} names no single locked version")
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
    raise ToolError(f"cargo lock: no crate.spec dependency reaches {name} {version}")


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


# ── BCR modules ───────────────────────────────────────────────────────────────

BCR_METADATA = "https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/modules/{}/metadata.json"
_BAZEL_VERSION = re.compile(r"^([A-Za-z0-9.]*)(?:-([A-Za-z0-9.-]+))?(?:\+[A-Za-z0-9.-]+)?$")


def bazel_version_key(version: str) -> tuple:
    """Bazel's module version ordering, not semver.

    Dot-separated identifiers compare left to right, numeric ones as integers
    and below alphanumeric ones, a shorter release first on a common prefix
    (`8.21.0` < `8.21.0.1` < `8.21.0.bcr.1`), a prerelease below its release,
    build metadata ignored, and the empty version above every other.
    """
    m = _BAZEL_VERSION.match(version)
    if not m or any(part and "" in part.split(".") for part in m.groups()):
        raise ToolError(f"{version!r} is not a Bazel module version")
    release, pre = m.groups()
    if not release:
        return (1,)

    def ids(part: str) -> tuple:
        return tuple((0, int(i), "") if i.isdigit() else (1, 0, i) for i in part.split("."))

    return (0, ids(release), (0, ids(pre)) if pre else (1, ()))


def is_prerelease(version: str) -> bool:
    return bazel_version_key(version)[2][0] == 0 if version else False


def bcr_target(fix: str, metadata: dict) -> str | None:
    """The lowest non-yanked release (no prerelease) in the registry at or above `fix`."""
    yanked = set(metadata.get("yanked_versions") or {})
    releases = [v for v in metadata.get("versions", []) if v not in yanked and not is_prerelease(v)]
    above = [v for v in releases if bazel_version_key(v) >= bazel_version_key(fix)]
    return min(above, key=bazel_version_key) if above else None


def set_versions(text: str, edits: list[tuple["inventory.Call", str]]) -> str:
    """`text` with the `version` of each call set; every call must carry exactly one."""
    for call, new in sorted(edits, key=lambda e: e[0].span, reverse=True):
        a, b = call.span
        part, n = re.subn(r'(\bversion\s*=\s*)"[^"]*"', lambda m: f'{m.group(1)}"{new}"', text[a:b])
        if n != 1:
            raise ToolError(f"{call.callee} at line {call.line}: expected one version, found {n}")
        text = text[:a] + part + text[b:]
    return text


# ── Branches and PRs ──────────────────────────────────────────────────────────

CREATE, REBUILD = "create", "rebuild"
_IDS_LINE = re.compile(r"^Advisories: (.+)$", re.MULTILINE)
_TARGET_LINE = re.compile(r"^Resolved version: (\S+)$", re.MULTILINE)


def title(component: str, target: str, ids: tuple[str, ...]) -> str:
    return f"fix(deps): raise {component} to {target} past {', '.join(ids)}"


def trailer(target: str, ids: tuple[str, ...]) -> str:
    """The end of every fix PR's commit body, which is also the PR body; its last two lines are what `parse_body` reads."""
    return f"Opened by the daily advisory scan (`.github/workflows/advisories.yml`).\n\nAdvisories: {', '.join(ids)}\nResolved version: {target}\n"


def body(component: str, locked: str, target: str, ids: tuple[str, ...]) -> str:
    return (
        f"Repins `{component}` from {locked} to {target}, the lowest version past every open\n"
        f"advisory against it, with `just bazel-repin-crate {component}@{locked}={target}`.\n"
        f"A re-scan of the crates with this tree's ignores reports none of them.\n"
    ) + trailer(target, ids)


def bcr_body(module: str, current: str, target: str, overridden: bool, ids: tuple[str, ...]) -> str:
    held = " and its `single_version_override`" if overridden else ""
    return (
        f"Raises the `bazel_dep` on `{module}`{held} from {current} to {target}, the lowest\n"
        f"non-yanked BCR version past every open advisory against it, and refreshes\n"
        f"MODULE.bazel.lock with `just bazel-lock-update`.\n"
        f"A re-scan of the module with this tree's ignores reports none of them.\n"
    ) + trailer(target, ids)


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


def decide(state: BranchState, ids: tuple[str, ...], target: str | None) -> str:
    """The action for a fix branch; a `target` of None compares the open PR by its advisory set alone."""
    if state.open_pr:
        if state.commits > 1:
            return LEFT_ALONE
        ids_seen, target_seen = parse_body(state.open_pr.body)
        return CURRENT if ids_seen == ids and target in (None, target_seen) else REBUILD
    if any(parse_body(pr.body)[0] == ids for pr in state.declined):
        return DECLINED
    return CREATE


# ── Running it ────────────────────────────────────────────────────────────────


def http_get(url: str, attempts: int = 3) -> bytes:
    """The body of `url`; a connection error, 429 or 5xx is retried with backoff, any other HTTP status is final."""
    for attempt in range(attempts):
        last = attempt + 1 == attempts
        try:
            with urllib.request.urlopen(url, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if (e.code < 500 and e.code != 429) or last:
                raise ToolError(f"GET {url}: HTTP {e.code}") from e
        except (OSError, http.client.HTTPException) as e:
            if last:
                raise ToolError(f"GET {url}: {e}") from e
        time.sleep(2**attempt)
    raise AssertionError("unreachable")


@dataclass
class Context:
    root: Path
    config: dict
    ignores: list
    today: datetime.date
    base: str
    dry_run: bool
    run: Callable[[list[str], Path], tuple[int, str, str]]
    # `inventory.load_map` entries of the component map, for the CPEs a re-scan matches by.
    component_map: list[dict] = field(default_factory=list)
    fetch: Callable[[str], bytes] = http_get

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

    def path(self, key: str) -> Path:
        return self.root / self.config["inputs"][key]


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
    """The locked versions of `name` with an unignored advisory, per OSV-Scanner over the tree's cargo lock (`inputs.cargo_lock`)."""
    with tempfile.TemporaryDirectory(prefix="advisories-fix-") as tmp:
        sbom = Path(tmp) / "cargo.cdx.json"
        sbom.write_text(json.dumps(cyclonedx(inventory.crates(ctx.lock()))))
        # Unmerged: merging would keep one version per advisory.
        found = scan.normalize_osv(scan.osv_scan(sbom, ctx.run))
    return {f.version for f in found if f.component == name and not scan.matching_ignore(f, ctx.ignores, ctx.today)}


def still_flagged(ctx: Context, findings: list) -> list:
    """The findings of a re-scan that no ignore of this tree suppresses."""
    return [f for f in scan.merge(findings) if not scan.matching_ignore(f, ctx.ignores, ctx.today)]


def clears(ctx: Context, rescanned: list, findings: list[dict]) -> bool:
    """Whether a re-scan leaves none of `findings` standing, by component and id or alias, ignores applied."""
    wanted = {(f["component"], n) for f in findings for n in (f["id"], *f["aliases"])}
    return not any((f.component, n) in wanted for f in still_flagged(ctx, rescanned) for n in (f.id, *f.aliases))


def grype_rescan(ctx: Context, components: list[Component]) -> list:
    """Grype's findings over `components` alone."""
    with tempfile.TemporaryDirectory(prefix="advisories-fix-") as tmp:
        sbom = Path(tmp) / "fix.cdx.json"
        sbom.write_text(json.dumps(cyclonedx(components)))
        doc = scan.grype_scan(f"sbom:{sbom}", Path(tmp), ctx.run)
    return scan.normalize_grype(doc, {(c.name, c.version): c.source for c in components})


def changed_paths(ctx: Context) -> set[str]:
    return {line[3:] for line in ctx.cmd("git", "status", "--porcelain", "--untracked-files=no").splitlines() if line}


def propose(ctx: Context, name: str, source: str, ids: tuple[str, ...], target: str, body_text: str, apply: Callable[[], tuple[str, str] | None]) -> Outcome:
    """Bring `advisories/<name>` and its PR to one commit raising `name` to `target`.

    `apply` edits the tree on the freshly rebuilt branch and checks the result;
    it returns `(status, note)` when no PR should open, else None, and what it
    left changed is the commit. The Debian target is the scan date, which moves
    every run: an open Debian PR stays current while its advisory set is
    unchanged, and a newer date alone rebuilds nothing.
    """
    branch = branch_name(name)

    def outcome(status: str, note: str, pr: PR | str | None = None) -> Outcome:
        return Outcome(name, source, ids, status, note, target, branch, pr.url if isinstance(pr, PR) else pr)

    state = branch_state(ctx, branch)
    action = decide(state, ids, None if source == "deb" else target)
    if action == CURRENT:
        return outcome(CURRENT, "fix PR open", state.open_pr)
    if action == LEFT_ALONE:
        return outcome(LEFT_ALONE, "fix PR branch has follow-up commits; left alone", state.open_pr)
    if action == DECLINED:
        declined = next(pr for pr in state.declined if parse_body(pr.body)[0] == ids)
        return outcome(DECLINED, "fix PR closed unmerged; needs an ignore", declined)

    ctx.cmd("git", "checkout", "--quiet", "-B", branch, f"origin/{ctx.base}")
    stop = apply()
    if stop:
        return outcome(*stop)

    subject = title(name, target, ids)
    ctx.cmd("git", "add", "--", *sorted(changed_paths(ctx)))
    ctx.cmd("git", "commit", "--quiet", "-m", subject, "-m", body_text)
    if ctx.dry_run:
        print(f"== {branch} ({action})\n{subject}\n\n{body_text}")
        return outcome(DRY_RUN, f"dry run: would {'open' if action == CREATE else 'update'} a fix PR")
    ctx.cmd("git", "push", "--quiet", f"--force-with-lease=refs/heads/{branch}:{state.head or ''}", "origin", f"HEAD:refs/heads/{branch}")
    if action == REBUILD:
        ctx.cmd("gh", "pr", "edit", str(state.open_pr.number), "--title", subject, "--body", body_text)
        return outcome(UPDATED, "fix PR updated", state.open_pr)
    url = ctx.cmd("gh", "pr", "create", "--base", ctx.base, "--head", branch, "--title", subject, "--body", body_text).strip()
    return outcome(OPENED, "fix PR opened", url)


def changed_within(ctx: Context, allowed: set[str], what: str) -> set[str]:
    """The changed paths, which must all be in `allowed`."""
    changed = changed_paths(ctx)
    if changed - allowed:
        raise ToolError(f"{what} changed {', '.join(sorted(changed - allowed))}, not only {', '.join(sorted(allowed))}")
    return changed


def fix_crate(ctx: Context, name: str, findings: list[dict]) -> Outcome:
    planned = plan_crate(name, findings, ctx.lock())
    if isinstance(planned, Outcome):
        return planned
    locked, target, older = planned
    ids = tuple(sorted(f["id"] for f in findings))

    def apply() -> tuple[str, str] | None:
        ctx.cmd("just", "bazel-repin-crate", f"{name}@{locked}={target}")
        changed = changed_within(ctx, {ctx.cargo_lock, ctx.config["inputs"]["module_lock"]}, "the repin")
        if ctx.cargo_lock not in changed:
            return NEEDS_IGNORE, NEEDS_IGNORE_NOTE
        if target not in locked_versions(ctx.lock(), name):
            raise ToolError(f"the repin did not lock {name} {target}")
        flagged = flagged_versions(ctx, name)
        if flagged & set(older):
            return ALERT, major_bump_note(ctx.lock(), name, min(flagged & set(older), key=semver_key))
        if flagged:
            return NEEDS_IGNORE, NEEDS_IGNORE_NOTE
        return None

    return propose(ctx, name, "cargo", ids, target, body(name, locked, target, ids), apply)


def fix_bcr(ctx: Context, name: str, findings: list[dict]) -> Outcome:
    ids = tuple(sorted(f["id"] for f in findings))

    def outcome(status: str, note: str, target: str | None = None) -> Outcome:
        return Outcome(name, "bcr", ids, status, note, target=target)

    unfixed = sorted(f["id"] for f in findings if not f["fixed"])
    if unfixed:
        return outcome(ALERT, f"alert only: no released fix for {', '.join(unfixed)}")
    calls = inventory.starlark_calls(ctx.path("module_bazel").read_text())
    deps = [c for c in calls if c.callee == "bazel_dep" and c.kwargs.get("name") == name]
    overrides = [c for c in calls if c.callee == "single_version_override" and c.kwargs.get("module_name") == name]
    if not deps:
        return outcome(ALERT, f"alert only: {name} is not a direct bazel_dep")
    # Presence, not value: a non-literal `patches` parses as None.
    if any(k in c.kwargs for c in overrides for k in ("patches", "patch_cmds")):
        # A patch is re-derived against each release, like a native bump (ADR-0015).
        return outcome(ALERT, "alert only: its single_version_override carries patches")
    fix = max((f["fixed"] for f in findings), key=bazel_version_key)
    target = bcr_target(fix, json.loads(ctx.fetch(BCR_METADATA.format(name))))
    if target is None:
        return outcome(ALERT, f"alert only: no BCR version at or past {fix} yet")
    current = inventory.bcr_resolved(json.loads(ctx.path("module_lock").read_text())).get(name)
    if current is None:
        raise ToolError(f"{ctx.config['inputs']['module_lock']} resolves no {name}")
    if bazel_version_key(target) <= bazel_version_key(current):
        return outcome(NEEDS_IGNORE, NEEDS_IGNORE_NOTE, target)
    entry = next((e for e in ctx.component_map if (e.get("source"), e.get("name")) == ("bcr", name)), {})
    cpes = entry.get("cpe") or []
    if not cpes:
        raise ToolError(f"the component map has no cpe for bcr {name!r}")
    held = [c for c in overrides if "version" in c.kwargs]

    def apply() -> tuple[str, str] | None:
        # The rebuilt branch holds the tree `calls` was read from.
        module = ctx.path("module_bazel")
        module.write_text(set_versions(module.read_text(), [(c, target) for c in deps + held]))
        ctx.cmd("just", "bazel-lock-update")
        changed_within(ctx, {ctx.config["inputs"]["module_bazel"], ctx.config["inputs"]["module_lock"]}, "the lock update")
        resolved = inventory.bcr_resolved(json.loads(ctx.path("module_lock").read_text())).get(name)
        if resolved != target:
            raise ToolError(f"the lock update resolved {name} {resolved}, not {target}")
        upstream = inventory.bcr_upstream(target)
        full = [f"{c}:{upstream}:*:*:*:*:*:*:*" for c in cpes]
        if still_flagged(ctx, grype_rescan(ctx, [Component(name, upstream, "bcr", cpe=full[0], extra_cpes=tuple(full[1:]))])):
            return NEEDS_IGNORE, NEEDS_IGNORE_NOTE
        return None

    return propose(ctx, name, "bcr", ids, target, bcr_body(name, current, target, bool(held), ids), apply)


def with_fix(findings: list[dict]) -> list[dict]:
    return [f for f in findings if f["fixed"]]


def set_digest(text: str, call: "inventory.Call", digest: str, comment: str) -> str:
    a, b = call.span
    part, n = re.subn(r'(\bdigest\s*=\s*)"[^"]*",?(?:[ \t]*#[^\n]*)?$', lambda m: f'{m.group(1)}"{digest}",  # {comment}', text[a:b], flags=re.M)
    if n != 1:
        raise ToolError(f'{call.callee} at line {call.line}: expected one line `digest = "sha256:…",  # <tag> on <date>`, found {n}')
    return text[:a] + part + text[b:]


def fix_base_image(ctx: Context, name: str, findings: list[dict]) -> Outcome:
    """A newer digest of the tracked tag, judged over the advisories with a released fix."""
    covered = with_fix(findings)
    ids = tuple(sorted({f["id"] for f in covered}))
    if not ids:
        return Outcome(name, "oci", tuple(sorted(f["id"] for f in findings)), ALERT, "alert only: no released fix for any of its advisories")
    base = ctx.config["base_image"]
    if not base.get("platforms"):
        # No platform would mean no re-scan, which would read as cleared.
        raise ToolError("no base_image.platforms in the config")
    [pull] = [c for c in inventory.starlark_calls(ctx.path("module_bazel").read_text()) if c.callee.endswith(".pull") and c.kwargs.get("name") == base["oci_pull"]]
    current = pull.kwargs["digest"]
    target = ctx.cmd(scan.scanner("crane"), "digest", base["tracks"]).strip()
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", target):
        raise ToolError(f"crane digest {base['tracks']} printed {target!r}")
    if target == current:
        return Outcome(name, "oci", ids, NEEDS_IGNORE, NEEDS_IGNORE_NOTE, target=target)

    def apply() -> tuple[str, str] | None:
        module = ctx.path("module_bazel")
        module.write_text(set_digest(module.read_text(), pull, target, f"{base['tracks']} on {ctx.today.isoformat()}"))
        changed_within(ctx, {ctx.config["inputs"]["module_bazel"]}, "the digest bump")
        with tempfile.TemporaryDirectory(prefix="advisories-fix-") as tmp:
            found = [f for p in base["platforms"] for f in scan.normalize_grype(scan.grype_scan(f"registry:{name}@{target}", Path(tmp), ctx.run, p), {}, image=(name, target))]
        return None if clears(ctx, found, covered) else (NEEDS_IGNORE, NEEDS_IGNORE_NOTE)

    text = (
        f"Moves the `{base['oci_pull']}` pull of `{name}` from {current}\n"
        f"to {target}, the current index digest of `{base['tracks']}` (`crane digest`).\n"
        f"A Grype re-scan of the new digest on every platform, with this tree's\n"
        f"ignores, reports none of the advisories below.\n"
    ) + trailer(target, ids)
    return propose(ctx, name, "oci", ids, target, text, apply)


_SNAPSHOT = re.compile(r"(/archive/[^/\s]+/)(\d{8}T\d{6}Z)")


def fix_debs(ctx: Context, name: str, findings: list[dict]) -> Outcome:
    """The deb manifest's snapshot moved to the scan date, judged over the advisories with a released fix."""
    covered = with_fix(findings)
    ids = tuple(sorted({f["id"] for f in covered}))
    if not ids:
        return Outcome(name, "deb", tuple(sorted(f["id"] for f in findings)), ALERT, "alert only: no released fix for any of its advisories")
    deb = ctx.config["deb"]
    manifest = ctx.root / deb["manifest"]
    stamps = {m.group(2) for m in _SNAPSHOT.finditer(manifest.read_text())}
    if len(stamps) != 1:
        raise ToolError(f"{deb['manifest']}: expected one snapshot timestamp in its source URLs, found {sorted(stamps)}")
    current = stamps.pop()
    target = ctx.today.strftime("%Y%m%dT000000Z")
    if target <= current:
        return Outcome(name, "deb", ids, NEEDS_IGNORE, NEEDS_IGNORE_NOTE, target=target)

    def apply() -> tuple[str, str] | None:
        text = manifest.read_text()
        manifest.write_text(_SNAPSHOT.sub(lambda m: m.group(1) + target, text))
        ctx.cmd("just", "bazel-distroless-lock")
        changed = changed_within(ctx, {deb["manifest"], deb["lock"], ctx.config["inputs"]["module_lock"]}, "the lock regeneration")
        if deb["lock"] not in changed:
            return NEEDS_IGNORE, NEEDS_IGNORE_NOTE
        packages = inventory.debs(json.loads((ctx.root / deb["lock"]).read_text()), inventory.debian_release(text))
        return None if clears(ctx, grype_rescan(ctx, packages), covered) else (NEEDS_IGNORE, NEEDS_IGNORE_NOTE)

    body_text = (
        f"Moves the snapshot.debian.org date of `{deb['manifest']}` from {current}\n"
        f"to {target}, the scan date, and regenerates `{deb['lock']}` with\n"
        f"`just bazel-distroless-lock`. A Grype re-scan of the locked packages, with\n"
        f"this tree's ignores, reports none of the advisories below.\n"
    ) + trailer(target, ids)
    return propose(ctx, name, "deb", ids, target, body_text, apply)


FIXERS = {"cargo": fix_crate, "bcr": fix_bcr, "oci": fix_base_image, "deb": fix_debs}


def fix_one(ctx: Context, name: str, source: str, findings: list[dict]) -> Outcome:
    fixer = FIXERS.get(source)
    if fixer is None:
        return Outcome(name, source, tuple(sorted(f["id"] for f in findings)), ALERT, f"alert only: no fix PR for {source} components")
    # From the base, whatever the previous component left behind; the checkout was clean on entry.
    ctx.cmd("git", "checkout", "--quiet", "--force", "--detach", f"origin/{ctx.base}")
    return fixer(ctx, name, findings)


def actionable(doc: dict) -> dict[tuple[str, str], list[dict]]:
    """The unignored findings by the (component, source) whose fix carries them, without `EOL` and GitHub Actions."""
    out: dict[tuple[str, str], list[dict]] = {}
    for f in doc["findings"]:
        if actionable_finding(f):
            out.setdefault((fix_component(f["component"], f["source"]), f["source"]), []).append(f)
    return dict(sorted(out.items()))


def outcomes_for(o: Outcome, findings: list[dict]) -> list[Outcome]:
    """`o`, the outcome of one fix, for each component whose findings it carries, as the issue looks them up.

    A shared fix (every Debian package) covers only the advisories with a
    released fix; a component with none gets its own alert-only outcome.
    """
    out = []
    for c in sorted({f["component"] for f in findings}):
        own = [f for f in findings if f["component"] == c]
        covered = with_fix(own) if o.status not in (ALERT, ERROR) else own
        if covered:
            out.append(replace(o, component=c, ids=tuple(sorted({f["id"] for f in covered}))))
        else:
            ids = tuple(sorted({f["id"] for f in own}))
            out.append(Outcome(c, o.source, ids, ALERT, f"alert only: no released fix for {', '.join(ids)}"))
    return out


def fix_all(ctx: Context, doc: dict, outcomes_path: Path) -> list[Outcome]:
    outcomes: list[Outcome] = []
    # Each component starts with a forced checkout, which would discard local edits.
    if changed_paths(ctx):
        raise ToolError(f"{ctx.root} has uncommitted changes")
    ctx.cmd("git", "fetch", "--quiet", "--prune", "origin")
    for (name, source), findings in actionable(doc).items():
        try:
            o = fix_one(ctx, name, source, findings)
        except Exception as e:  # noqa: BLE001  (one component failing must not stop the others)
            print(f"fix: {name}: error: {type(e).__name__}: {e}", file=sys.stderr)
            o = Outcome(name, source, tuple(sorted(f["id"] for f in findings)), ERROR, "fix failed; see the run log")
        print(f"fix: {name}: {o.status}: {o.note}", file=sys.stderr)
        outcomes += outcomes_for(o, findings)
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
        component_map, _ = inventory.load_map(args.config.parent / config["component_map"])
        doc = json.loads(args.findings.read_text())
        ctx = Context(args.root, config, ignores, scan.utc_today(), args.base, args.dry_run, scan.run_process, component_map)
        outcomes = fix_all(ctx, doc, args.outcomes)
    except Exception as e:  # noqa: BLE001  (a crash must never read as handled)
        print(f"fix: error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    return EXIT_ERROR if any(o.status == ERROR for o in outcomes) else EXIT_CLEAN


if __name__ == "__main__":
    sys.exit(main())
