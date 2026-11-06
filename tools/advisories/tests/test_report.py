import contextlib
import datetime
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import report
import scan
from _model import ToolError
from tests.test_scan import FIXTURES, FakeScanners

TODAY = datetime.date(2026, 11, 5)
SHA_A = "a" * 40
SHA_B = "b" * 40

CONFIG = """\
component_map = "components.toml"
ignores = "ignores.toml"
issue_label = "advisory"

[inputs]
module_bazel = "MODULE.bazel"
workflows = [".github/workflows/*.yml"]

[inputs.version_files]
kakadu_extension = "bazel/kakadu_extension.bzl"
"""

COMPONENTS = """\
[[component]]
source = "http_archive"
name = "zlib"
status = "shipped"
cpe = "cpe:2.3:a:zlib:zlib"
version_re = '/zlib-(\\d+(?:\\.\\d+)+)\\.tar\\.gz$'

[[component]]
source = "custom_rule"
name = "kakadu_extension"
status = "no_feed"
reason = "proprietary"
version_re = 'tag = "kakadu-v(\\d+(?:\\.\\d+)+)"'
"""


def component(name, version, source, **props):
    return {"name": name, "version": version, "properties": [{"name": k, "value": v} for k, v in {"advisories:source": source, **props}.items()]}


def recorded_scan(ignores=None):
    """The findings document `scan.py` writes for the recorded scan fixtures."""
    with tempfile.TemporaryDirectory() as tmp:
        cfg = Path(tmp) / "cfg"
        shutil.copytree(FIXTURES, cfg)
        if ignores is not None:
            (cfg / "ignores.toml").write_text(ignores)
        with mock.patch.object(scan, "scanner", lambda name: name):
            result = scan.scan(cfg / "advisories.toml", cfg / "inventory.cdx.json", TODAY, FakeScanners())
    return scan.report(result)


def fixture_components():
    return json.loads((FIXTURES / "inventory.cdx.json").read_text())["components"]


class BodyTest(unittest.TestCase):
    def test_lists_each_open_finding_and_not_the_ignored_one(self):
        doc = recorded_scan()
        body = report.render(doc, fixture_components(), [], TODAY)
        findings = body.split("## Findings")[1].split("##")[0]
        for f in report.open_findings(doc):
            self.assertIn(f"| {f['id']}", findings)
        self.assertNotIn("RUSTSEC-2023-0071", findings)
        self.assertIn("| curl 8.3.0 | CVE-2023-38545 | critical | 8.4.0 |", findings)
        self.assertIn("| registry.example/images/base-debian12 (libc6 2.36-9+deb12u13) | CVE-2010-4756 | negligible | no fix |", findings)
        self.assertIn("**open findings: 6**, ignored: 1", body)

    def test_findings_are_ordered_by_severity(self):
        rows = [line for line in report.findings_section(recorded_scan()).splitlines() if line.startswith("| ") and "Component" not in line]
        severities = [line.split(" | ")[2] for line in rows]
        self.assertEqual(severities, sorted(severities, key=lambda s: -scan.SEVERITIES.index(s)))

    def test_carries_the_grype_database_date(self):
        self.assertIn("Grype advisory database built 2026-11-05T06:21:06Z.", report.render(recorded_scan(), [], [], TODAY))

    def test_no_open_finding_still_renders_the_findings_heading(self):
        doc = {"grype_db_built": None, "findings": []}
        self.assertIn("## Findings\n\nNo open finding.", report.render(doc, [], [], TODAY))

    def test_lists_ignores_expiring_within_14_days(self):
        doc = recorded_scan(
            '[[ignore]]\nid = "RUSTSEC-2023-0071"\ncomponent = "rsa"\nreason = "r"\nexpiry = 2026-11-19\n'
            '[[ignore]]\nid = "CVE-2023-38545"\ncomponent = "curl"\nreason = "r"\nexpiry = 2026-11-20\n'
            '[[ignore]]\nid = "CVE-2023-4911"\ncomponent = "libc6"\nreason = "r"\nexpiry = 2026-11-05\n'
        )
        section = report.expiring_section(doc, TODAY)
        self.assertIn("| rsa | RUSTSEC-2023-0071 | 2026-11-19 |", section)
        self.assertIn("| libc6 | CVE-2023-4911 | 2026-11-05 |", section)
        self.assertNotIn("curl", section)

    def test_no_expiring_ignore_leaves_the_section_out(self):
        self.assertNotIn("Ignores expiring", report.render(recorded_scan(), [], [], TODAY))

    def test_lists_components_reaching_end_of_life_within_90_days(self):
        components = [
            component("soon", "1", "bcr", **{"advisories:eol": "2027-02-03"}),
            component("later", "1", "bcr", **{"advisories:eol": "2027-02-04"}),
            component("today", "1", "bcr", **{"advisories:eol": "2026-11-05"}),
        ]
        section = report.eol_section(components, TODAY)
        self.assertIn("| soon | 1 | 2027-02-03 |", section)
        self.assertNotIn("later", section)
        # Reached today: the scan reports it as an `EOL` finding instead.
        self.assertNotIn("today", section)

class CapTest(unittest.TestCase):
    def test_a_body_under_the_cap_is_unchanged(self):
        body = "x\n" * 100
        self.assertEqual(report.cap(body, "https://run"), body)

    def test_a_body_of_exactly_the_cap_is_unchanged(self):
        body = "x" * 99 + "\n"
        self.assertEqual(report.cap(body, "https://run", limit=100), body)

    def test_the_cut_keeps_whole_lines_within_the_limit(self):
        body = "".join(f"line {n:02}\n" for n in range(40))
        capped = report.cap(body, None, limit=120)
        note = "\n_Truncated at 120 characters; the full report is in the run summary._\n"
        self.assertTrue(capped.endswith(note))
        kept = capped.removesuffix(note)
        self.assertEqual(kept, body[: len(kept)])
        self.assertTrue(kept.endswith("\n"))
        self.assertLessEqual(len(capped), 120)
        self.assertGreater(len(capped) + len("line 00\n"), 120)

    def test_a_long_body_is_cut_at_a_line_and_links_the_run_summary(self):
        body = "".join(f"| row {n} | {'y' * 50} |\n" for n in range(5000))
        capped = report.cap(body, "https://example/runs/1")
        self.assertLessEqual(len(capped), report.CAP)
        self.assertGreater(len(capped), report.CAP - 100)
        kept, note = capped.rsplit("\n\n", 1)
        self.assertTrue(body.startswith(kept + "\n"))
        self.assertIn("[run summary](https://example/runs/1)", note)

    def test_the_recorded_scan_with_many_findings_is_capped(self):
        doc = recorded_scan()
        f = doc["findings"][0]
        doc["findings"] = [{**f, "id": f"CVE-2026-{n:05}"} for n in range(2000)]
        body = report.render(doc, [], [], TODAY)
        self.assertGreater(len(body), report.CAP)
        self.assertLessEqual(len(report.cap(body, "https://run")), report.CAP)


def outcome(component, source, status, note, pr=None):
    return {"component": component, "source": source, "ids": [], "status": status, "note": note, "target": None, "branch": None, "pr": pr}


def pr(number, head, state, title="t", fork=False):
    return {"number": number, "headRefName": head, "state": state, "title": title, "isCrossRepository": fork}


class FixTest(unittest.TestCase):
    def test_each_finding_shows_its_components_fix_outcome(self):
        outcomes = [
            outcome("opentelemetry_sdk", "cargo", "opened", "fix PR opened", "https://github.com/o/r/pull/9"),
            # Same name, other source, listed first: must not attach to the bcr curl.
            outcome("curl", "cargo", "needs-ignore", "at or past the fix, still flagged; needs an ignore"),
            outcome("curl", "bcr", "alert-only", "alert only: no fix PR for bcr components"),
        ]
        section = report.findings_section(recorded_scan(), outcomes)
        self.assertIn("| Component | Advisory | Severity | Fixed in | Fix |", section)
        self.assertIn("| GHSA-w9wp-h8wv-79jx (CVE-2026-48504) | medium | 0.32.1 | fix PR opened (https://github.com/o/r/pull/9) |", section)
        self.assertIn("| curl 8.3.0 | CVE-2023-38545 | critical | 8.4.0 | alert only: no fix PR for bcr components |", section)
        self.assertIn("| libc6 2.36-9+deb12u1 | CVE-2023-4911 | high | 2.36-9+deb12u3 | - |", section)
        self.assertNotIn("needs an ignore", section)

    def test_eol_findings_get_no_outcome(self):
        outcomes = [outcome("debian", "deb", "alert-only", "alert only: no fix PR for deb components")]
        rows = [line for line in report.findings_section(recorded_scan(), outcomes).splitlines() if line.startswith("| debian ")]
        self.assertEqual([r.rsplit(" | ", 1)[1] for r in rows], ["- |"])

    def test_a_finding_without_a_version_shows_unknown(self):
        doc = recorded_scan()
        doc["findings"] = [{**f, "version": None, "packages": []} for f in doc["findings"] if f["component"] == "curl"][:1]
        self.assertIn("| curl unknown |", report.findings_section(doc))

    def test_without_outcomes_there_is_no_fix_column(self):
        self.assertNotIn("| Fix |", report.findings_section(recorded_scan()))

    def test_lists_open_and_declined_fix_prs(self):
        prs = [
            pr(1, "advisories/opentelemetry_sdk", "CLOSED", "older decline"),
            pr(4, "advisories/opentelemetry_sdk", "CLOSED", "latest decline"),
            pr(5, "advisories/curl", "OPEN", "raise curl"),
            pr(6, "advisories/curl", "CLOSED", "declined while another is open"),
            pr(7, "advisories/tokio", "CLOSED", "declined, no longer flagged"),
            pr(2, "advisories/libc6", "CLOSED", "declined, then fixed"),
            pr(8, "advisories/libc6", "MERGED", "merged"),
            pr(9, "feature/x", "OPEN", "not a fix PR"),
            pr(10, "advisories/curl", "OPEN", "from a fork", fork=True),
            pr(11, "advisories/debian", "CLOSED", "from a fork", fork=True),
        ]
        section = report.fix_prs_section(prs, recorded_scan())
        self.assertEqual(
            section,
            "## Fix PRs\n\n| PR | Title | State |\n|---|---|---|\n| #5 | raise curl | open |\n| #4 | latest decline | closed unmerged; needs an ignore |",
        )

    def test_a_declined_debian_pr_shows_while_any_package_is_flagged(self):
        # Every Debian package's fix is the one `advisories/debian` branch.
        doc = recorded_scan()
        doc["findings"] = [f for f in doc["findings"] if f["component"] == "libc6"]
        section = report.fix_prs_section([pr(3, "advisories/debian", "CLOSED", "raise debian")], doc)
        self.assertIn("| #3 | raise debian | closed unmerged; needs an ignore |", section)

    def test_an_eol_finding_alone_keeps_no_declined_pr(self):
        # fix.py skips `EOL` findings, so they ask for no ignore.
        doc = recorded_scan()
        doc["findings"] = [f for f in doc["findings"] if f["component"] == "debian"]
        self.assertEqual({f["id"] for f in doc["findings"]}, {"EOL"})
        self.assertEqual(report.fix_prs_section([pr(3, "advisories/debian", "CLOSED", "raise debian")], doc), "")

    def test_with_outcomes_the_declined_prs_are_fix_pys(self):
        prs = [
            pr(1, "advisories/opentelemetry_sdk", "CLOSED", "the decline fix.py matched"),
            pr(4, "advisories/opentelemetry_sdk", "CLOSED", "latest decline, other advisories"),
            pr(3, "advisories/debian", "CLOSED", "raise debian"),
        ]
        declined = outcome("opentelemetry_sdk", "cargo", "declined", "fix PR closed unmerged; needs an ignore", "https://github.com/o/r/pull/1")
        section = report.fix_prs_section(prs, recorded_scan(), [declined])
        self.assertEqual(
            section,
            "## Fix PRs\n\n| PR | Title | State |\n|---|---|---|\n| #1 | the decline fix.py matched | closed unmerged; needs an ignore |",
        )
        self.assertEqual(report.fix_prs_section(prs, recorded_scan(), [outcome("opentelemetry_sdk", "cargo", "opened", "fix PR opened")]), "")

    def test_no_fix_pr_leaves_the_section_out(self):
        self.assertEqual(report.fix_prs_section([pr(9, "feature/x", "OPEN")], recorded_scan()), "")
        self.assertEqual(report.fix_prs_section(None, recorded_scan()), "")
        self.assertNotIn("## Fix PRs", report.render(recorded_scan(), [], [], TODAY))


def git(root, *args, date=None):
    env = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull}
    if date:
        env |= {"GIT_AUTHOR_DATE": f"{date}T12:00:00Z", "GIT_COMMITTER_DATE": f"{date}T12:00:00Z"}
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t", *args], check=True, capture_output=True, env=env)


class PinTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "tree"
        (self.root / "bazel").mkdir(parents=True)
        (self.root / ".github/workflows").mkdir(parents=True)
        (self.root / "cfg").mkdir()
        (self.root / "cfg/advisories.toml").write_text(CONFIG)
        (self.root / "cfg/components.toml").write_text(COMPONENTS)
        git(self.root, "init", "-q")
        self.commit("2026-01-10", zlib="1.3", kakadu="8.6", uses=f"actions/checkout@{SHA_A}")
        self.commit("2026-03-20", zlib="1.3.1", kakadu="8.6", uses=f"actions/checkout@{SHA_A}")
        self.commit("2026-05-30", zlib="1.3.1", kakadu="8.7", uses=f"actions/checkout@{SHA_A}\n  - uses: actions/cache@v4\n  - uses: actions/cache/restore@v4")
        self.commit("2026-07-01", zlib="1.3.1", kakadu="8.7", uses=f"actions/checkout@{SHA_A}\n  - uses: actions/cache@v4\n  - uses: actions/cache/restore@v4", also=f"actions/checkout@{SHA_B}")

    def commit(self, date, zlib, kakadu, uses, also=None):
        (self.root / "MODULE.bazel").write_text(f'http_archive(\n    name = "zlib",\n    url = "https://example/zlib-{zlib}.tar.gz",\n)\n')
        (self.root / "bazel/kakadu_extension.bzl").write_text(f'tag = "kakadu-v{kakadu}"\n')
        (self.root / ".github/workflows/ci.yml").write_text(f"steps:\n  - uses: {uses}\n")
        if also:
            (self.root / ".github/workflows/other.yml").write_text(f"steps:\n  - uses: {also}\n")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", date, date=date)

    def pins(self, components):
        config = report.tomllib.loads(CONFIG)
        return report.pins(config, self.root / "cfg", self.root, components)

    def test_dates_each_pin_by_its_last_change(self):
        table = self.pins(
            [
                component("zlib", "1.3.1", "http_archive"),
                component("kakadu_extension", "8.7", "custom_rule"),
                component("actions/checkout", "6.0.0", "githubactions", **{"advisories:ref": f"{SHA_A} {SHA_B}"}),
                component("actions/cache", "4.2.3", "githubactions", **{"advisories:ref": "v4"}),
            ]
        )
        self.assertEqual(
            table,
            [
                report.Pin("actions/cache@v4", "4.2.3", None),
                report.Pin(f"actions/checkout@{SHA_A}", "6.0.0", "2026-01-10"),
                report.Pin(f"actions/checkout@{SHA_B}", "6.0.0", "2026-07-01"),
                report.Pin("kakadu_extension", "8.7", "2026-05-30"),
                report.Pin("zlib", "1.3.1", "2026-03-20"),
            ],
        )

    def test_unresolved_actions_are_listed_without_a_date(self):
        table = self.pins(
            [
                component("docker://alpine:3", None, "githubactions", **{"advisories:unresolved": "docker:// image"}),
                component("acme/gone", None, "githubactions", **{"advisories:ref": "main", "advisories:unresolved": "no release tag on this ref"}),
            ]
        )
        self.assertEqual(
            table,
            [
                report.Pin("acme/gone@main", "unresolved: no release tag on this ref", None),
                report.Pin("docker://alpine:3", "unresolved: docker:// image", None),
            ],
        )

    def test_crates_and_debs_have_no_pin_row(self):
        self.assertEqual(self.pins([component("rsa", "0.9.10", "cargo"), component("libc6", "2.36", "deb")]), [])

    def test_a_version_file_without_a_version_re_is_an_error(self):
        (self.root / "cfg/components.toml").write_text(COMPONENTS.split("[[component]]\nsource = \"custom_rule\"")[0])
        with self.assertRaisesRegex(ToolError, "no version_re"):
            self.pins([component("kakadu_extension", "8.7", "custom_rule")])

    def test_a_shallow_clone_is_an_error(self):
        clone = self.root.parent / "shallow"
        subprocess.run(["git", "clone", "-q", "--depth", "1", f"file://{self.root}", str(clone)], check=True, capture_output=True)
        with self.assertRaisesRegex(ToolError, "shallow"):
            report.require_full_history(clone)


class MainTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.findings = self.dir / "findings.json"
        self.findings.write_text(json.dumps(recorded_scan()))
        (self.dir / "advisories.toml").write_text(CONFIG)
        (self.dir / "components.toml").write_text(COMPONENTS)
        inv = fixture_components()
        self.inventory = self.dir / "inventory.cdx.json"
        self.inventory.write_text(json.dumps({"components": [c for c in inv if c["name"] != "kakadu_extension"]}))
        self.output = self.dir / "body.md"

    def run_main(self, root, *extra):
        stdout, stderr = io.StringIO(), io.StringIO()
        argv = ["--config", str(self.dir / "advisories.toml"), "--inventory", str(self.inventory), "--findings", str(self.findings), "--output", str(self.output), "--root", str(root), *extra]
        with mock.patch.object(report.scan, "utc_today", lambda: TODAY), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = report.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def git_root(self):
        root = self.dir / "tree"
        root.mkdir()
        (root / "MODULE.bazel").write_text("")
        git(root, "init", "-q")
        git(root, "add", "-A")
        git(root, "commit", "-q", "-m", "init")
        return root

    def test_writes_the_body_and_prints_the_open_count_and_label(self):
        summary = self.dir / "summary.md"
        summary.write_text("before\n")
        code, out, _ = self.run_main(self.git_root(), "--summary", str(summary), "--run-url", "https://run")
        self.assertEqual(code, 0)
        self.assertEqual(out, "open=6\nlabel=advisory\n")
        body = self.output.read_text()
        self.assertIn("CVE-2023-38545", body)
        self.assertEqual(summary.read_text(), "before\n" + body)

    def test_an_approaching_end_of_life_alone_is_not_open(self):
        doc = json.loads(self.findings.read_text())
        doc["findings"] = [{**f, "ignored": {"reason": "r", "expiry": "2027-01-31"}} for f in doc["findings"]]
        self.findings.write_text(json.dumps(doc))
        self.inventory.write_text(json.dumps({"components": [component("soon", "1", "bcr", **{"advisories:eol": "2026-12-01"})]}))
        code, out, _ = self.run_main(self.git_root())
        self.assertEqual(code, 0)
        self.assertEqual(out, "open=0\nlabel=advisory\n")
        self.assertIn("## Approaching end of life", self.output.read_text())

    def test_reads_the_fix_outcomes_and_prs(self):
        outcomes, prs = self.dir / "fix.json", self.dir / "prs.json"
        outcomes.write_text(json.dumps({"outcomes": [outcome("opentelemetry_sdk", "cargo", "dry-run", "dry run: would open a fix PR")]}))
        prs.write_text(json.dumps([pr(3, "advisories/opentelemetry_sdk", "OPEN", "raise opentelemetry_sdk")]))
        code, _, _ = self.run_main(self.git_root(), "--fix-outcomes", str(outcomes), "--fix-prs", str(prs))
        self.assertEqual(code, 0)
        body = self.output.read_text()
        self.assertIn("| dry run: would open a fix PR |", body)
        self.assertIn("| #3 | raise opentelemetry_sdk | open |", body)

    def test_a_config_without_issue_label_exits_3(self):
        (self.dir / "advisories.toml").write_text(CONFIG.replace('issue_label = "advisory"\n', ""))
        code, out, err = self.run_main(self.git_root())
        self.assertEqual((code, out), (3, ""))
        self.assertIn("issue_label", err)
        self.assertFalse(self.output.exists())

    def test_a_tree_without_git_history_exits_3_and_writes_no_body(self):
        root = self.dir / "plain"
        root.mkdir()
        self.output.write_text("stale")
        code, out, err = self.run_main(root)
        self.assertEqual(code, 3)
        self.assertEqual(out, "")
        self.assertIn("report: error:", err)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
