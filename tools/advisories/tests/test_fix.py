import contextlib
import datetime
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import fix
import scan
from _model import ToolError

TODAY = datetime.date(2026, 11, 6)

# The source-less package is the workspace crate_universe synthesizes from the crate.spec list.
LOCK = """\
version = 4

[[package]]
name = "direct-cargo-bazel-deps"
version = "0.0.1"
dependencies = ["axum", "reqwest", "tokio"]

[[package]]
name = "axum"
version = "0.8.1"
source = "registry+https://github.com/rust-lang/crates.io-index"
dependencies = ["hyper 1.5.0", "tokio"]

[[package]]
name = "reqwest"
version = "0.11.27"
source = "registry+https://github.com/rust-lang/crates.io-index"
dependencies = ["hyper 0.14.30", "tokio"]

[[package]]
name = "hyper"
version = "1.5.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
dependencies = ["h2", "tokio"]

[[package]]
name = "hyper"
version = "0.14.30"
source = "registry+https://github.com/rust-lang/crates.io-index"
dependencies = ["h2", "tokio"]

[[package]]
name = "h2"
version = "0.4.6"
source = "registry+https://github.com/rust-lang/crates.io-index"
dependencies = ["slab", "tokio"]

[[package]]
name = "slab"
version = "0.4.9"
source = "registry+https://github.com/rust-lang/crates.io-index"

[[package]]
name = "tokio"
version = "1.40.0"
source = "registry+https://github.com/rust-lang/crates.io-index"
"""

CONFIG = {"inputs": {"cargo_lock": "Cargo.Bazel.lock", "module_lock": "MODULE.bazel.lock"}}


def lock(text=LOCK):
    import tomllib

    return tomllib.loads(text)


def finding(component, id, fixed, source="cargo", ignored=None, version="1.40.0"):
    return {"component": component, "version": version, "source": source, "id": id, "aliases": [], "severity": "high", "fixed": fixed, "packages": [], "ignored": ignored}


def osv_doc(name, version, id, fixed):
    vuln = {"id": id, "affected": [{"package": {"name": name}, "ranges": [{"events": [{"introduced": "0"}, {"fixed": fixed}]}]}]}
    return {"results": [{"packages": [{"package": {"name": name, "version": version}, "vulnerabilities": [vuln], "groups": [{"ids": [id], "max_severity": "7.5"}]}]}]}


class VersionTest(unittest.TestCase):
    def test_semver_precedence(self):
        ordered = ["1.2.0-alpha", "1.2.0-alpha.1", "1.2.0-alpha.2", "1.2.0-alpha.10", "1.2.0-rc.1", "1.2.0", "1.2.1", "1.10.0"]
        self.assertEqual(sorted(reversed(ordered), key=fix.semver_key), ordered)

    def test_build_metadata_is_ignored_and_garbage_is_an_error(self):
        self.assertEqual(fix.semver_key("1.2.3+build.5"), fix.semver_key("1.2.3"))
        with self.assertRaises(ToolError):
            fix.semver_key("one.two")

    def test_compatible_is_cargo_caret(self):
        self.assertTrue(fix.compatible("1.40.0", "1.43.1"))
        self.assertTrue(fix.compatible("0.4.6", "0.4.9"))
        self.assertFalse(fix.compatible("0.4.6", "0.5.0"))
        self.assertFalse(fix.compatible("0.0.3", "0.0.4"))
        self.assertFalse(fix.compatible("1.2.3", "2.0.0"))


class PlanTest(unittest.TestCase):
    def test_target_is_the_fix_within_the_locked_line(self):
        self.assertEqual(fix.plan_crate("tokio", [finding("tokio", "RUSTSEC-2026-0001", "1.43.1")], lock()), ("1.40.0", "1.43.1", []))

    def test_target_is_the_highest_fix_so_no_advisory_reopens(self):
        fs = [finding("tokio", "RUSTSEC-2026-0001", "1.43.1"), finding("tokio", "RUSTSEC-2026-0002", "1.41.0")]
        self.assertEqual(fix.plan_crate("tokio", fs, lock()), ("1.40.0", "1.43.1", []))

    def test_no_released_fix_is_alert_only(self):
        o = fix.plan_crate("tokio", [finding("tokio", "RUSTSEC-2026-0001", "1.43.1"), finding("tokio", "RUSTSEC-2026-0002", None)], lock())
        self.assertEqual((o.status, o.note), (fix.ALERT, "alert only: no released fix for RUSTSEC-2026-0002"))
        self.assertEqual(o.ids, ("RUSTSEC-2026-0001", "RUSTSEC-2026-0002"))

    def test_target_not_above_the_lock_needs_an_ignore(self):
        o = fix.plan_crate("tokio", [finding("tokio", "RUSTSEC-2026-0001", "1.40.0")], lock())
        self.assertEqual((o.status, o.note, o.target), (fix.NEEDS_IGNORE, "at or past the fix, still flagged; needs an ignore", "1.40.0"))

    def test_a_crate_locked_twice_repins_the_version_on_the_fix_line(self):
        # The finding carries the first locked version only; both come from the lock.
        self.assertEqual(fix.plan_crate("hyper", [finding("hyper", "RUSTSEC-2026-0003", "1.5.1", version="0.14.30")], lock()), ("1.5.0", "1.5.1", ["0.14.30"]))
        self.assertEqual(fix.plan_crate("hyper", [finding("hyper", "RUSTSEC-2026-0003", "0.14.31")], lock()), ("0.14.30", "0.14.31", []))

    def test_a_fix_on_a_newer_semver_line_needs_a_major_bump_of_the_nearest_spec(self):
        o = fix.plan_crate("h2", [finding("h2", "RUSTSEC-2026-0004", "0.5.0")], lock())
        self.assertEqual((o.status, o.note), (fix.ALERT, "alert only: needs a major bump of reqwest"))
        o = fix.plan_crate("tokio", [finding("tokio", "RUSTSEC-2026-0001", "2.0.0")], lock())
        self.assertEqual(o.note, "alert only: needs a major bump of tokio")


class ReverseDependencyTest(unittest.TestCase):
    def test_the_nearest_spec_along_reverse_dependencies(self):
        self.assertEqual(fix.nearest_spec_dependent(lock(), "hyper", "1.5.0"), "axum")
        self.assertEqual(fix.nearest_spec_dependent(lock(), "hyper", "0.14.30"), "reqwest")
        self.assertEqual(fix.nearest_spec_dependent(lock(), "slab", "0.4.9"), "reqwest")

    def test_a_dependency_with_a_source_suffix_and_same_named_parents(self):
        text = LOCK.replace('"hyper 0.14.30", "tokio"]', '"hyper 0.14.30 (registry+https://github.com/rust-lang/crates.io-index)", "tokio"]')
        self.assertEqual(fix.nearest_spec_dependent(lock(text), "hyper", "0.14.30"), "reqwest")
        # h2's parents are hyper 0.14.30 and hyper 1.5.0, tied at one hop; the lower version goes first.
        self.assertEqual(fix.nearest_spec_dependent(lock(text), "h2", "0.4.6"), "reqwest")

    def test_a_spec_is_its_own_nearest(self):
        self.assertEqual(fix.nearest_spec_dependent(lock(), "axum", "0.8.1"), "axum")

    def test_an_unreachable_crate_is_an_error(self):
        orphan = LOCK + '\n[[package]]\nname = "orphan"\nversion = "1.0.0"\nsource = "registry+https://github.com/rust-lang/crates.io-index"\n'
        with self.assertRaisesRegex(ToolError, "no crate.spec dependency reaches orphan"):
            fix.nearest_spec_dependent(lock(orphan), "orphan", "1.0.0")

    def test_an_ambiguous_dependency_is_an_error(self):
        with self.assertRaisesRegex(ToolError, "'hyper' names no single locked version"):
            fix.nearest_spec_dependent(lock(LOCK.replace('"hyper 1.5.0"', '"hyper"')), "hyper", "1.5.0")


class NamingTest(unittest.TestCase):
    IDS = ("GHSA-aaaa-bbbb-cccc", "RUSTSEC-2026-0001")

    def test_branch_title_and_body(self):
        self.assertEqual(fix.branch_name("tokio"), "advisories/tokio")
        self.assertEqual(fix.title("tokio", "1.43.1", self.IDS), "fix(deps): raise tokio to 1.43.1 past GHSA-aaaa-bbbb-cccc, RUSTSEC-2026-0001")
        text = fix.body("tokio", "1.40.0", "1.43.1", self.IDS)
        self.assertIn("`just bazel-repin-crate tokio@1.40.0=1.43.1`", text)
        self.assertIn("\nAdvisories: GHSA-aaaa-bbbb-cccc, RUSTSEC-2026-0001\n", text)
        self.assertIn("\nResolved version: 1.43.1\n", text)

    def test_parse_body_reads_what_body_writes(self):
        self.assertEqual(fix.parse_body(fix.body("tokio", "1.40.0", "1.43.1", self.IDS)), (self.IDS, "1.43.1"))
        self.assertEqual(fix.parse_body("Advisories: B, A\r\nsome edit"), (("A", "B"), None))
        self.assertEqual(fix.parse_body(None), ((), None))


class DecideTest(unittest.TestCase):
    IDS = ("RUSTSEC-2026-0001",)
    BODY = fix.body("tokio", "1.40.0", "1.43.1", IDS)

    def state(self, head=None, commits=0, open_body=None, declined=()):
        pr = fix.PR(7, "https://example/pr/7", open_body) if open_body is not None else None
        return fix.BranchState(head, commits, pr, tuple(fix.PR(n, f"https://example/pr/{n}", b) for n, b in enumerate(declined, 1)))

    def test_lifecycle(self):
        other_target = fix.body("tokio", "1.40.0", "1.44.0", self.IDS)
        other_ids = fix.body("tokio", "1.40.0", "1.43.1", ("RUSTSEC-2026-0001", "RUSTSEC-2026-0002"))
        cases = [
            ("no branch, no PR", self.state(), fix.CREATE),
            ("branch without a PR is replaced", self.state(head="abc", commits=3), fix.CREATE),
            ("open PR, same ids and target", self.state(head="abc", commits=1, open_body=self.BODY), fix.CURRENT),
            ("open PR, target changed", self.state(head="abc", commits=1, open_body=other_target), fix.REBUILD),
            ("open PR, advisory set changed", self.state(head="abc", commits=1, open_body=other_ids), fix.REBUILD),
            ("open PR with a follow-up commit", self.state(head="abc", commits=2, open_body=other_target), fix.LEFT_ALONE),
            ("declined with the same ids", self.state(declined=[other_ids, self.BODY]), fix.DECLINED),
            ("declined with other ids", self.state(declined=[other_ids]), fix.CREATE),
            ("open PR wins over a declined one", self.state(head="abc", commits=1, open_body=self.BODY, declined=[self.BODY]), fix.CURRENT),
        ]
        for name, state, expected in cases:
            with self.subTest(name):
                self.assertEqual(fix.decide(state, self.IDS, "1.43.1"), expected)


class FakeRun:
    """Answers the git, gh, just and osv-scanner calls of a fix run over a temporary checkout."""

    def __init__(self, root, open_prs=(), closed_prs=(), remote_head=None, commits=1, repin_changes=True, still_flagged=False, failing_repin=(), vulnerable=(("tokio", "1.40.0"),), dirty=()):
        self.root, self.calls = root, []
        self.vulnerable = set(vulnerable)
        self.open_prs, self.closed_prs = list(open_prs), list(closed_prs)
        self.remote_head, self.commits = remote_head, commits
        self.repin_changes, self.still_flagged, self.failing_repin = repin_changes, still_flagged, failing_repin
        self.changed = set(dirty)

    def __call__(self, cmd, cwd):
        self.calls.append(cmd)
        name = Path(cmd[0]).name
        if name == "osv-scanner":
            sbom = json.loads(Path(cmd[cmd.index("-L") + 1]).read_text())
            hits = [(c["name"], c["version"]) for c in sbom["components"] if (c["name"], c["version"]) in self.vulnerable or (self.still_flagged and c["name"] == "tokio")]
            packages = [p for n, v in hits for p in osv_doc(n, v, "RUSTSEC-2026-0001", "9.9.9")["results"][0]["packages"]]
            return (1, json.dumps({"results": [{"packages": packages}]}), "") if hits else (0, json.dumps({"results": []}), "")
        if cmd[:2] == ["just", "bazel-repin-crate"]:
            if cmd[2].split("@")[0] in self.failing_repin:
                return 1, "", "cargo update failed"
            if self.repin_changes:
                crate, rest = cmd[2].split("@")
                locked, target = rest.split("=")
                path = self.root / "Cargo.Bazel.lock"
                path.write_text(path.read_text().replace(f'name = "{crate}"\nversion = "{locked}"', f'name = "{crate}"\nversion = "{target}"'))
                self.changed |= {"Cargo.Bazel.lock", "MODULE.bazel.lock"}
            return 0, "", ""
        if cmd[:2] == ["git", "status"]:
            return 0, "".join(f" M {p}\n" for p in sorted(self.changed)), ""
        if cmd[:2] == ["git", "rev-parse"]:
            return (0, self.remote_head + "\n", "") if self.remote_head else (1, "", "")
        if cmd[:2] == ["git", "rev-list"]:
            return 0, f"{self.commits}\n", ""
        if cmd[:2] == ["git", "checkout"]:
            (self.root / "Cargo.Bazel.lock").write_text(LOCK)
            self.changed = set()
            return 0, "", ""
        if cmd[:3] == ["gh", "pr", "list"]:
            prs = self.open_prs if cmd[cmd.index("--state") + 1] == "open" else self.closed_prs
            return 0, json.dumps([{"isCrossRepository": False, "state": "OPEN", **p} for p in prs]), ""
        if cmd[:3] == ["gh", "pr", "create"]:
            return 0, "https://github.com/o/r/pull/99\n", ""
        return 0, "", ""

    def ran(self, *prefix):
        return [c for c in self.calls if c[: len(prefix)] == list(prefix)]


class FixRunTest(unittest.TestCase):
    TOKIO = finding("tokio", "RUSTSEC-2026-0001", "1.43.1")

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "tree"
        self.root.mkdir()
        (self.root / "Cargo.Bazel.lock").write_text(LOCK)
        bin_dir = Path(tmp.name) / "bin"
        bin_dir.mkdir()
        (bin_dir / "osv-scanner").write_text("#!/bin/sh\nexit 99\n")
        (bin_dir / "osv-scanner").chmod(0o755)
        patcher = mock.patch.dict(os.environ, {"ADVISORIES_BIN": str(bin_dir)})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.outcomes = Path(tmp.name) / "outcomes.json"

    def run_fix(self, findings, dry_run=False, ignores=(), **fake):
        self.fake = FakeRun(self.root, **fake)
        ctx = fix.Context(self.root, CONFIG, list(ignores), TODAY, "main", dry_run, self.fake)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            outcomes = fix.fix_all(ctx, {"findings": findings}, self.outcomes)
        self.stdout = out.getvalue()
        return outcomes

    def test_opens_a_pr_with_one_commit_from_the_base(self):
        [o] = self.run_fix([self.TOKIO])
        self.assertEqual((o.status, o.target, o.branch, o.pr), (fix.OPENED, "1.43.1", "advisories/tokio", "https://github.com/o/r/pull/99"))
        self.assertEqual(
            self.fake.ran("git", "checkout"),
            [["git", "checkout", "--quiet", "--force", "--detach", "origin/main"], ["git", "checkout", "--quiet", "-B", "advisories/tokio", "origin/main"]],
        )
        self.assertEqual(self.fake.ran("just"), [["just", "bazel-repin-crate", "tokio@1.40.0=1.43.1"]])
        self.assertEqual(self.fake.ran("git", "add"), [["git", "add", "--", "Cargo.Bazel.lock", "MODULE.bazel.lock"]])
        [commit] = self.fake.ran("git", "commit")
        self.assertEqual(commit[4], "fix(deps): raise tokio to 1.43.1 past RUSTSEC-2026-0001")
        self.assertIn("Advisories: RUSTSEC-2026-0001\nResolved version: 1.43.1\n", commit[6])
        # An empty lease: the push fails if someone created the branch meanwhile.
        self.assertEqual(self.fake.ran("git", "push"), [["git", "push", "--quiet", "--force-with-lease=refs/heads/advisories/tokio:", "origin", "HEAD:refs/heads/advisories/tokio"]])
        [create] = self.fake.ran("gh", "pr", "create")
        self.assertEqual(create[3:7], ["--base", "main", "--head", "advisories/tokio"])

    def test_a_branch_without_a_pr_is_replaced_under_a_lease_on_its_head(self):
        [o] = self.run_fix([self.TOKIO], remote_head="abc", commits=3)
        self.assertEqual(o.status, fix.OPENED)
        [push] = self.fake.ran("git", "push")
        self.assertIn("--force-with-lease=refs/heads/advisories/tokio:abc", push)

    def test_a_dirty_checkout_is_refused_before_anything_runs(self):
        with self.assertRaisesRegex(ToolError, "uncommitted changes"):
            self.run_fix([self.TOKIO], dirty={"src/main.rs"})
        self.assertEqual(self.fake.ran("git", "checkout"), [])

    def test_a_fork_pr_on_the_branch_name_is_ignored(self):
        body = fix.body("tokio", "1.40.0", "1.43.1", ("RUSTSEC-2026-0001",))
        fork = {"number": 6, "url": "https://example/pr/6", "body": body, "isCrossRepository": True}
        [o] = self.run_fix([self.TOKIO], open_prs=[fork], closed_prs=[{**fork, "state": "CLOSED"}])
        self.assertEqual(o.status, fix.OPENED)
        self.assertEqual(self.fake.ran("gh", "pr", "edit"), [])

    def test_an_older_line_still_flagged_after_the_repin_needs_a_major_bump(self):
        hyper = finding("hyper", "RUSTSEC-2026-0003", "1.5.1", version="0.14.30")
        [o] = self.run_fix([hyper], vulnerable={("hyper", "0.14.30"), ("hyper", "1.5.0")})
        self.assertEqual((o.status, o.note), (fix.ALERT, "alert only: needs a major bump of reqwest"))
        self.assertEqual(self.fake.ran("just"), [["just", "bazel-repin-crate", "hyper@1.5.0=1.5.1"]])
        [o] = self.run_fix([hyper], vulnerable={("hyper", "1.5.0")})
        self.assertEqual(o.status, fix.OPENED)
        # The same line still flagged after the repin is not the older line's doing.
        [o] = self.run_fix([hyper], vulnerable={("hyper", "1.5.0"), ("hyper", "1.5.1")})
        self.assertEqual(o.status, fix.NEEDS_IGNORE)

    def test_dry_run_prints_and_pushes_nothing(self):
        [o] = self.run_fix([self.TOKIO], dry_run=True)
        self.assertEqual((o.status, o.note), (fix.DRY_RUN, "dry run: would open a fix PR"))
        self.assertIn("== advisories/tokio (create)\nfix(deps): raise tokio to 1.43.1 past RUSTSEC-2026-0001\n", self.stdout)
        self.assertIn("Resolved version: 1.43.1", self.stdout)
        self.assertEqual(self.fake.ran("git", "push") + self.fake.ran("gh", "pr", "create") + self.fake.ran("gh", "pr", "edit"), [])

    def test_an_advisory_left_standing_opens_no_pr(self):
        [o] = self.run_fix([self.TOKIO], still_flagged=True)
        self.assertEqual((o.status, o.note), (fix.NEEDS_IGNORE, fix.NEEDS_IGNORE_NOTE))
        self.assertEqual(self.fake.ran("git", "commit"), [])

    def test_an_ignored_advisory_after_the_repin_does_not_count(self):
        ignore = scan.Ignore("RUSTSEC-2026-0001", "tokio", "accepted", TODAY)
        [o] = self.run_fix([self.TOKIO], still_flagged=True, ignores=[ignore])
        self.assertEqual(o.status, fix.OPENED)

    def test_an_unchanged_lock_opens_no_pr(self):
        [o] = self.run_fix([self.TOKIO], repin_changes=False)
        self.assertEqual(o.status, fix.NEEDS_IGNORE)
        self.assertEqual(self.fake.ran("osv-scanner") + self.fake.ran("git", "commit"), [])

    def test_a_current_open_pr_is_left_as_is(self):
        pr = {"number": 7, "url": "https://example/pr/7", "body": fix.body("tokio", "1.40.0", "1.43.1", ("RUSTSEC-2026-0001",))}
        [o] = self.run_fix([self.TOKIO], open_prs=[pr], remote_head="abc")
        self.assertEqual((o.status, o.pr), (fix.CURRENT, "https://example/pr/7"))
        self.assertEqual(self.fake.ran("just") + [c for c in self.fake.ran("git", "checkout") if "-B" in c], [])

    def test_a_changed_target_rebuilds_and_force_pushes_against_the_seen_head(self):
        pr = {"number": 7, "url": "https://example/pr/7", "body": fix.body("tokio", "1.40.0", "1.42.0", ("RUSTSEC-2026-0001",))}
        [o] = self.run_fix([self.TOKIO], open_prs=[pr], remote_head="abc")
        self.assertEqual(o.status, fix.UPDATED)
        [push] = self.fake.ran("git", "push")
        self.assertIn("--force-with-lease=refs/heads/advisories/tokio:abc", push)
        [edit] = self.fake.ran("gh", "pr", "edit")
        self.assertEqual(edit[3:6], ["7", "--title", "fix(deps): raise tokio to 1.43.1 past RUSTSEC-2026-0001"])
        self.assertEqual(self.fake.ran("git", "rev-list"), [["git", "rev-list", "--count", "origin/main..abc"]])

    def test_a_dry_run_rebuild_says_update(self):
        pr = {"number": 7, "url": "https://example/pr/7", "body": fix.body("tokio", "1.40.0", "1.42.0", ("RUSTSEC-2026-0001",))}
        [o] = self.run_fix([self.TOKIO], open_prs=[pr], remote_head="abc", dry_run=True)
        self.assertEqual((o.status, o.note), (fix.DRY_RUN, "dry run: would update a fix PR"))
        self.assertIn("== advisories/tokio (rebuild)", self.stdout)

    def test_a_follow_up_commit_is_left_alone(self):
        pr = {"number": 7, "url": "https://example/pr/7", "body": ""}
        [o] = self.run_fix([self.TOKIO], open_prs=[pr], remote_head="abc", commits=2)
        self.assertEqual(o.status, fix.LEFT_ALONE)
        self.assertEqual(self.fake.ran("just") + self.fake.ran("git", "push"), [])

    def test_a_pr_closed_unmerged_suppresses_while_the_ids_match(self):
        body = fix.body("tokio", "1.40.0", "1.43.1", ("RUSTSEC-2026-0001",))
        merged = {"number": 5, "url": "https://example/pr/5", "body": body, "state": "MERGED"}
        declined = {"number": 6, "url": "https://example/pr/6", "body": body, "state": "CLOSED"}
        [o] = self.run_fix([self.TOKIO], closed_prs=[merged, declined])
        self.assertEqual((o.status, o.pr), (fix.DECLINED, "https://example/pr/6"))
        [o] = self.run_fix([self.TOKIO], closed_prs=[merged])
        self.assertEqual(o.status, fix.OPENED)

    def test_skips_eol_actions_and_ignored_and_alerts_on_other_sources(self):
        findings = [
            finding("tokio", "EOL", None),
            finding("actions/checkout", "GHSA-x", "6.0.1", source="githubactions"),
            finding("rsa", "RUSTSEC-2023-0071", None, ignored={"reason": "r", "expiry": "2027-01-01"}),
            finding("zlib", "CVE-2026-1", "1.3.2", source="http_archive"),
        ]
        [o] = self.run_fix(findings)
        self.assertEqual((o.component, o.status, o.note), ("zlib", fix.ALERT, "alert only: no fix PR for http_archive components"))

    def test_one_failing_component_does_not_stop_the_others(self):
        findings = [self.TOKIO, finding("zlib", "CVE-2026-1", "1.3.2", source="http_archive")]
        outcomes = self.run_fix(findings, failing_repin=("tokio",))
        self.assertEqual([(o.component, o.status) for o in outcomes], [("tokio", fix.ERROR), ("zlib", fix.ALERT)])
        written = json.loads(self.outcomes.read_text())["outcomes"]
        self.assertEqual([(o["component"], o["status"]) for o in written], [("tokio", "error"), ("zlib", "alert-only")])

    def test_main_exits_3_when_a_component_failed(self):
        cfg = self.root / "advisories.toml"
        cfg.write_text('ignores = "ignores.toml"\n[inputs]\ncargo_lock = "Cargo.Bazel.lock"\nmodule_lock = "MODULE.bazel.lock"\n')
        (self.root / "ignores.toml").write_text("")
        findings = self.root / "findings.json"
        findings.write_text(json.dumps({"findings": [self.TOKIO]}))
        fake = FakeRun(self.root, failing_repin=("tokio",))
        argv = ["--config", str(cfg), "--findings", str(findings), "--outcomes", str(self.outcomes), "--root", str(self.root), "--dry-run"]
        with mock.patch.object(scan, "run_process", fake), contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(fix.main(argv), 3)
        self.assertIn("fix: tokio: error: ToolError: just bazel-repin-crate tokio@1.40.0=1.43.1 exited 1", err.getvalue())
        self.assertEqual(json.loads(self.outcomes.read_text())["outcomes"][0]["status"], "error")


if __name__ == "__main__":
    unittest.main()
