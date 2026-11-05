#!/usr/bin/env python3
"""Fail on the advisories the working tree adds over a base ref.

Usage:
    diff.py --config advisories.toml --base REF --inventory advisories.cdx.json --output advisories-diff.json [--root DIR]

The working tree (`--root`) and a temporary worktree of `REF` are both
inventoried with the working tree's `inventory.py`, config and component map,
and both scanned with its `scan.py`, without ignores. Drift in the working
tree fails; drift in the base is reported and skipped, because a base may
predate the component map or `tools/advisories/` itself. A base without one of
the config's input files (a lockfile, the deb manifest) exits 3.

A working-tree finding is new unless a base finding of the same component
shares its id or an alias. Components match by name, never by version, so a
version bump of a component already flagged for that advisory is not new,
while a pin bumped into a vulnerable release is. The working tree's ignore
file then applies to the new findings once; a finding the base shares never
fails here, whatever its ignore says, and the daily scan carries it.

`today` is taken once for both scans, so an `eol` date reached between them
cannot give one side alone an `EOL` finding, and each Actions repository is
looked up once for both inventories.

Writes the working tree's inventory to `--inventory` and the new findings to
`--output`, neither on drift or error.

Exit codes: 0 nothing new, 1 a new unignored finding or working-tree drift,
2 usage, 3 tool or scanner error on either side.
"""

import _model  # noqa: F401  (Python version check, before tomllib)

import argparse
import contextlib
import datetime
import json
import os
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

import inventory
import scan
from _model import EXIT_CLEAN, EXIT_ERROR, EXIT_FINDINGS, Component, Finding, ToolError, cyclonedx


def shared_ls_remote():
    """A `git ls-remote` lookup that queries each repository once, a missing one included."""
    answers: dict[str, str | None] = {}

    def ls_remote(repo: str) -> str:
        if repo not in answers:
            try:
                answers[repo] = inventory.git_ls_remote(repo)
            except inventory.RepoMissing:
                answers[repo] = None
        if answers[repo] is None:
            raise inventory.RepoMissing(repo)
        return answers[repo]

    return ls_remote


def new_findings(head: list[Finding], base: list[Finding]) -> list[Finding]:
    """The head findings no base finding of the same component shares an id or alias with."""
    shared = {(f.component, n) for f in base for n in (f.id, *f.aliases)}
    return [f for f in head if not any((f.component, n) in shared for n in (f.id, *f.aliases))]


def diff(config_path: Path, head_root: Path, base_root: Path, today: datetime.date, run=None) -> tuple[list[Component], list[str], scan.Result | None]:
    """The head's components, its drift, and the new findings; no scan runs when the head drifts."""
    config = tomllib.loads(config_path.read_text())
    ignores = scan.load_ignores((config_path.parent / config["ignores"]).read_text())
    ls_remote = shared_ls_remote()
    head, drift = inventory.build(config_path, head_root, ls_remote)
    if drift:
        return head, drift, None
    base, base_drift = inventory.build(config_path, base_root, ls_remote)
    # A drifting base component is left out of the base, so its findings read as new.
    for d in base_drift:
        print(f"diff: base drift (skipped): {d}", file=sys.stderr)
    platforms = config["base_image"].get("platforms", [])
    head_found, built = scan.findings(cyclonedx(head), platforms, today, run)
    base_found, _ = scan.findings(cyclonedx(base), platforms, today, run)
    new = new_findings(head_found, base_found)
    ignored = {f.key: i for f in new if (i := scan.matching_ignore(f, ignores, today))}
    return head, drift, scan.Result(new, ignored, built)


@contextlib.contextmanager
def worktree(root: Path, ref: str):
    """A detached worktree of `ref`, removed and pruned afterwards."""
    git = ["git", "-C", str(root), "worktree"]
    try:
        with tempfile.TemporaryDirectory(prefix="advisories-base-") as tmp:
            path = Path(tmp) / "base"
            # The inventory reads no LFS content; smudging would download every test image.
            env = {**os.environ, "GIT_LFS_SKIP_SMUDGE": "1"}
            r = subprocess.run([*git, "add", "--detach", "--quiet", str(path), ref], capture_output=True, text=True, env=env)
            if r.returncode:
                raise ToolError(f"git worktree add {ref} exited {r.returncode}: {r.stderr.strip()}")
            yield path
    finally:
        # `prune` also drops a worktree that `add` registered before failing.
        subprocess.run([*git, "prune"], capture_output=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", required=True, type=Path, help="advisories.toml of the working tree")
    parser.add_argument("--base", required=True, help="git ref to diff against, e.g. origin/main")
    parser.add_argument("--root", default=Path("."), type=Path, help="working tree to scan (default: .)")
    parser.add_argument("--inventory", required=True, type=Path, help="CycloneDX JSON of the working tree to write")
    parser.add_argument("--output", required=True, type=Path, help="new findings JSON to write")
    args = parser.parse_args(argv)
    args.inventory.unlink(missing_ok=True)
    args.output.unlink(missing_ok=True)
    try:
        with worktree(args.root, args.base) as base_root:
            head, drift, result = diff(args.config, args.root, base_root, scan.utc_today())
        if drift:
            for d in drift:
                print(f"diff: drift: {d}", file=sys.stderr)
            return EXIT_FINDINGS
        args.inventory.write_text(json.dumps(cyclonedx(head), indent=2) + "\n")
        args.output.write_text(json.dumps(scan.report(result), indent=2) + "\n")
    except Exception as e:  # noqa: BLE001  (a crash must never read as findings or a clean diff)
        print(f"diff: error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    scan.print_open(result, f"diff: new against {args.base}: ")
    return EXIT_FINDINGS if result.open else EXIT_CLEAN


if __name__ == "__main__":
    sys.exit(main())
