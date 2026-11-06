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

import diff
import inventory
from tests.test_inventory import TREE, recorded_ls_remote

TODAY = datetime.date(2026, 11, 5)
IMAGE = "registry.example/images/base-debian12"
DIGEST = "sha256:" + "1" * 64


class AdvisoryDb:
    """Fake scanners answering from `vulns`, `(name, version) -> [(id, aliases)]`, over the SBOM or image each call names.

    `fail_call` makes that call (0-based, head scans first) exit with an error code.
    """

    def __init__(self, vulns, fail_call=None):
        self.vulns = vulns
        self.fail_call = fail_call
        self.calls = []

    def __call__(self, cmd, cwd):
        n = len(self.calls)
        self.calls.append(cmd)
        if Path(cmd[0]).name == "osv-scanner":
            if n == self.fail_call:
                return 127, "", "osv-scanner: boom"
            sbom = json.loads(Path(cmd[cmd.index("-L") + 1]).read_text())
            packages = [
                {
                    "package": {"name": c["name"], "version": c["version"]},
                    "vulnerabilities": [{"id": i} for i, _ in hits],
                    "groups": [{"ids": [i], "aliases": list(a), "max_severity": "5.0"} for i, a in hits],
                }
                for c in sbom["components"]
                if (hits := self.vulns.get((c["name"], c["version"])))
            ]
            return (1 if packages else 0), json.dumps({"results": [{"packages": packages}]}), ""
        if n == self.fail_call:
            return 1, "", "grype: boom"
        target = cmd[1]
        if target.startswith("sbom:"):
            sbom = json.loads(Path(target.removeprefix("sbom:")).read_text())
            artifacts = [(c["name"], c.get("version")) for c in sbom["components"]]
        else:
            name, _, version = target.removeprefix("registry:").rpartition("@")
            artifacts = [(name, version)]
        matches = [
            {"vulnerability": {"id": i, "severity": "High"}, "artifact": {"name": name, "version": version}, "relatedVulnerabilities": [{"id": x} for x in a]}
            for name, version in artifacts
            for i, a in self.vulns.get((name, version), [])
        ]
        return 0, json.dumps({"matches": matches, "descriptor": {"db": {"status": {"built": "2026-11-05T06:00:00Z"}}}}), ""


class Trees(unittest.TestCase):
    """A head and a base copy of the inventory fixture tree, scanned through `AdvisoryDb`."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        self.head = self.tree("head")
        self.base = self.tree("base")
        # The head's tools and map drive both scans; the base's copies are never read.
        config = self.head / "advisories.toml"
        config.write_text('ignores = "ignores.toml"\n' + config.read_text() + 'platforms = ["linux/amd64"]\n')
        self.config = config
        self.write_ignores("")
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        for name in ("osv-scanner", "grype"):
            (bin_dir / name).write_text("#!/bin/sh\nexit 99\n")
            (bin_dir / name).chmod(0o755)
        self.lookups = []

        def ls_remote(repo):
            self.lookups.append(repo)
            return recorded_ls_remote(repo)

        for patcher in (mock.patch.dict(os.environ, {"ADVISORIES_BIN": str(bin_dir)}), mock.patch.object(inventory, "git_ls_remote", ls_remote)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def tree(self, name):
        path = self.dir / name
        shutil.copytree(TREE, path)
        return path

    def edit(self, tree, rel, old, new):
        path = tree / rel
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1))

    def write_ignores(self, text):
        (self.head / "ignores.toml").write_text(text)

    def run_diff(self, vulns, today=TODAY, fail_call=None):
        self.db = AdvisoryDb(vulns, fail_call)
        _, drift, result = diff.diff(self.config, self.head, self.base, today, self.db)
        return drift, result

    def ids(self, result):
        return sorted(f.key for f in result.open)


class DiffTest(Trees):
    # ── Head minus base ──

    def test_shared_finding_passes(self):
        drift, r = self.run_diff({("curl", "8.21.0"): [("CVE-2026-0001", ())]})
        self.assertEqual((drift, r.findings), ([], []))

    def test_head_only_finding_fails(self):
        self.edit(self.base, "deps/crates.lock", 'name = "bytes"\nversion = "1.12.1"', 'name = "bytes-old"\nversion = "1.12.1"')
        _, r = self.run_diff({("bytes", "1.12.1"): [("RUSTSEC-2026-0001", ("GHSA-aaaa-bbbb-cccc",))]})
        self.assertEqual(self.ids(r), [("bytes", "RUSTSEC-2026-0001")])

    def test_finding_fixed_at_head_passes(self):
        self.edit(self.base, "deps/crates.lock", 'version = "1.12.1"', 'version = "1.12.0"')
        _, r = self.run_diff({("bytes", "1.12.0"): [("RUSTSEC-2026-0001", ())]})
        self.assertEqual(r.findings, [])

    def test_version_bump_of_an_already_vulnerable_component_passes(self):
        self.edit(self.base, "deps/crates.lock", 'version = "1.12.1"', 'version = "1.12.0"')
        hit = [("RUSTSEC-2026-0001", ())]
        _, r = self.run_diff({("bytes", "1.12.0"): hit, ("bytes", "1.12.1"): hit})
        self.assertEqual(r.findings, [])

    def test_pin_bumped_to_a_vulnerable_version_fails(self):
        # The map holds no versions: the base's own URL is what the base reports.
        self.edit(self.base, "deps/module.star", "libtiff-4.7.2", "libtiff-4.7.1")
        self.edit(self.base, "deps/module.star", "tags/v4.7.2.tar.gz", "tags/v4.7.1.tar.gz")
        _, r = self.run_diff({("tiff", "4.7.2"): [("CVE-2026-0002", ())]})
        self.assertEqual(self.ids(r), [("tiff", "CVE-2026-0002")])

    def test_a_shared_alias_is_a_shared_finding(self):
        # Head reports the RUSTSEC id with a GHSA alias, base the GHSA id alone.
        self.edit(self.base, "deps/crates.lock", 'version = "1.12.1"', 'version = "1.12.0"')
        _, r = self.run_diff({("bytes", "1.12.0"): [("GHSA-aaaa-bbbb-cccc", ())], ("bytes", "1.12.1"): [("RUSTSEC-2026-0001", ("GHSA-aaaa-bbbb-cccc",))]})
        self.assertEqual(r.findings, [])

    def test_eol_on_both_sides_is_shared(self):
        with mock.patch.object(diff, "new_findings", wraps=diff.new_findings) as subtract:
            _, r = self.run_diff({})
        head, base = subtract.call_args.args
        for side in (head, base):
            self.assertIn(("debian", "EOL"), [f.key for f in side])
        self.assertEqual(r.findings, [])

    def test_the_component_is_part_of_the_key(self):
        hit = [("CVE-2026-0003", ())]
        self.edit(self.base, "deps/crates.lock", 'version = "1.12.1"', 'version = "1.12.0"')
        _, r = self.run_diff({("bytes", "1.12.1"): hit, ("serde", "1.0.229"): hit})
        self.assertEqual(self.ids(r), [("bytes", "CVE-2026-0003")])

    # ── Ignores ──

    def test_ignored_head_only_finding_passes(self):
        self.edit(self.base, "deps/crates.lock", 'name = "bytes"\nversion = "1.12.1"', 'name = "bytes-old"\nversion = "1.12.1"')
        self.write_ignores('[[ignore]]\nid = "GHSA-aaaa-bbbb-cccc"\ncomponent = "bytes"\nreason = "unreachable"\nexpiry = 2026-12-31\n')
        _, r = self.run_diff({("bytes", "1.12.1"): [("RUSTSEC-2026-0001", ("GHSA-aaaa-bbbb-cccc",))]})
        self.assertEqual((r.open, list(r.ignored)), ([], [("bytes", "RUSTSEC-2026-0001")]))

    def test_head_only_finding_whose_ignore_expired_fails(self):
        self.edit(self.base, "deps/crates.lock", 'name = "bytes"\nversion = "1.12.1"', 'name = "bytes-old"\nversion = "1.12.1"')
        self.write_ignores('[[ignore]]\nid = "RUSTSEC-2026-0001"\ncomponent = "bytes"\nreason = "unreachable"\nexpiry = 2026-11-04\n')
        _, r = self.run_diff({("bytes", "1.12.1"): [("RUSTSEC-2026-0001", ())]})
        self.assertEqual(self.ids(r), [("bytes", "RUSTSEC-2026-0001")])

    def test_shared_finding_whose_ignore_expired_passes(self):
        self.write_ignores('[[ignore]]\nid = "CVE-2026-0001"\ncomponent = "curl"\nreason = "unreachable"\nexpiry = 2026-11-04\n')
        _, r = self.run_diff({("curl", "8.21.0"): [("CVE-2026-0001", ())]})
        self.assertEqual(r.findings, [])

    # ── Drift ──

    def test_base_without_the_tools_is_scanned_best_effort(self):
        for name in ("advisories.toml", "components.toml"):
            (self.base / name).unlink()
        self.edit(self.base, "deps/module.lock.json", '"https://bcr.bazel.build/bazel_registry.json"', '"https://bcr.bazel.build/modules/newdep/1.0/source.json": "0000",\n    "https://bcr.bazel.build/bazel_registry.json"')
        with contextlib.redirect_stderr(io.StringIO()) as err:
            drift, r = self.run_diff({("curl", "8.21.0"): [("CVE-2026-0001", ())]})
        self.assertEqual((drift, r.findings), ([], []), "the base was scanned and its finding subtracted")
        self.assertIn("diff: base drift (skipped): unclassified bcr 'newdep'", err.getvalue())

    def test_base_without_an_input_file_raises(self):
        (self.base / "deps/crates.lock").unlink()
        with self.assertRaises(FileNotFoundError):
            self.run_diff({})

    def test_head_drift_fails_before_scanning(self):
        self.edit(self.head, "deps/module.lock.json", '"https://bcr.bazel.build/bazel_registry.json"', '"https://bcr.bazel.build/modules/newdep/1.0/source.json": "0000",\n    "https://bcr.bazel.build/bazel_registry.json"')
        drift, r = self.run_diff({})
        self.assertIn("unclassified bcr 'newdep'", drift[0])
        self.assertIsNone(r)
        self.assertEqual(self.db.calls, [])

    # ── Errors and sharing ──

    def test_scanner_error_on_either_side_raises(self):
        # Calls 0-2 scan the head (osv, grype sbom, grype image), 3-5 the base.
        for call in (0, 1, 2, 3, 4, 5):
            with self.subTest(call=call), self.assertRaises(diff.ToolError):
                self.run_diff({}, fail_call=call)

    def test_each_actions_repository_is_looked_up_once(self):
        self.run_diff({})
        self.assertIn("example/gone", self.lookups, "a missing repository is looked up too")
        self.assertEqual(sorted(self.lookups), sorted(set(self.lookups)))


class DiffMainTest(Trees):
    """`main` against a worktree of a committed base."""

    def setUp(self):
        super().setUp()
        # The base copy becomes the repository: its commit is the base, its working tree the head.
        self.repo = self.base
        git = ["git", "-C", str(self.repo), "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false"]
        for cmd in (["init", "-q"], ["add", "-A"], ["commit", "-q", "-m", "base"]):
            subprocess.run(git + cmd, check=True, capture_output=True)
        # The head's config, map and ignores sit outside the repository, as tools a base predates.
        self.out = self.dir / "out"
        self.out.mkdir()
        # Stale outputs of an earlier run, which a failing run must not leave behind.
        for name in ("inv.cdx.json", "diff.json", "sbom.cdx.json"):
            (self.out / name).write_text("stale")
        self.addCleanup(lambda: self.assertEqual(len(self.worktrees()), 1, "the base worktree is removed"))

    def run_main(self, vulns, todays=(TODAY,), fail_call=None, extra=()):
        self.db = AdvisoryDb(vulns, fail_call)
        stdout, stderr = io.StringIO(), io.StringIO()
        argv = ["--config", str(self.config), "--root", str(self.repo), "--base", "HEAD", "--inventory", str(self.out / "inv.cdx.json"), "--output", str(self.out / "diff.json"), *extra]
        with mock.patch.object(diff.scan, "run_process", self.db), mock.patch.object(diff.scan, "utc_today", mock.Mock(side_effect=todays)):
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                code = diff.main(argv)
        return code, stdout.getvalue(), stderr.getvalue()

    def worktrees(self):
        return subprocess.run(["git", "-C", str(self.repo), "worktree", "list"], check=True, capture_output=True, text=True).stdout.splitlines()

    def test_head_only_finding_exits_1_and_is_written(self):
        self.edit(self.repo, "deps/crates.lock", 'version = "1.12.1"', 'version = "1.12.2"')
        code, out, err = self.run_main({("bytes", "1.12.2"): [("RUSTSEC-2026-0001", ())]})
        self.assertEqual(code, 1, err)
        self.assertIn("bytes 1.12.2: RUSTSEC-2026-0001", out)
        report = json.loads((self.out / "diff.json").read_text())
        self.assertEqual([f["id"] for f in report["findings"]], ["RUSTSEC-2026-0001"])
        self.assertTrue((self.out / "inv.cdx.json").exists())

    def test_nothing_new_exits_0(self):
        code, _, err = self.run_main({("curl", "8.21.0"): [("CVE-2026-0001", ())]})
        self.assertEqual(code, 0, err)

    def test_sbom_leaves_out_the_ci_components(self):
        code, _, err = self.run_main({}, extra=("--sbom", str(self.out / "sbom.cdx.json")))
        self.assertEqual(code, 0, err)

        def scopes(name):
            components = json.loads((self.out / name).read_text())["components"]
            return [next((p["value"] for p in c.get("properties", []) if p["name"] == "advisories:scope"), None) for c in components]

        self.assertIn("ci", scopes("inv.cdx.json"))
        sbom = scopes("sbom.cdx.json")
        self.assertNotIn("ci", sbom)
        self.assertEqual(len(sbom), scopes("inv.cdx.json").count(None))

    def test_mixed_up_modes_are_usage_errors(self):
        for argv in (
            ["--base", "HEAD", "--output", "o"],
            ["--base", "HEAD", "--inventory", "i"],
            ["--release-baseline", "v1.0.0", "--sbom", "s"],
            ["--release-baseline", "v1.0.0", "--base", "HEAD", "--inventory", "i", "--output", "o"],
        ):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as e:
                diff.main(["--config", str(self.config), *argv])
            self.assertEqual(e.exception.code, 2)

    def test_today_is_taken_once_for_both_scans(self):
        # A second date for the base scan would make the head's Debian 12 EOL finding (2026-07-12) head-only.
        code, _, err = self.run_main({}, todays=(datetime.date(2026, 7, 12), datetime.date(2026, 7, 11)))
        self.assertEqual(code, 0, err)

    def test_head_drift_exits_1(self):
        self.edit(self.repo, "deps/module.lock.json", '"https://bcr.bazel.build/bazel_registry.json"', '"https://bcr.bazel.build/modules/newdep/1.0/source.json": "0000",\n    "https://bcr.bazel.build/bazel_registry.json"')
        code, _, err = self.run_main({}, extra=("--sbom", str(self.out / "sbom.cdx.json")))
        self.assertEqual(code, 1)
        self.assertIn("diff: drift: unclassified bcr 'newdep'", err)
        for name in ("diff.json", "inv.cdx.json", "sbom.cdx.json"):
            self.assertFalse((self.out / name).exists(), name)

    def test_scanner_error_exits_3(self):
        for call in (0, 4):
            with self.subTest(call=call):
                code, _, err = self.run_main({}, fail_call=call, extra=("--sbom", str(self.out / "sbom.cdx.json")))
                self.assertEqual(code, 3)
                self.assertIn("diff: error:", err)
                self.assertFalse((self.out / "diff.json").exists())
                self.assertFalse((self.out / "sbom.cdx.json").exists())

    def test_unknown_base_ref_exits_3(self):
        argv = ["--config", str(self.config), "--root", str(self.repo), "--base", "no-such-ref", "--inventory", str(self.out / "i"), "--output", str(self.out / "o")]
        with contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(diff.main(argv), 3)
        self.assertIn("git worktree add no-such-ref", err.getvalue())


class ReleaseBaselineTest(unittest.TestCase):
    TAGS = ["v10.0.1", "v10.0.0", "v9.1.1", "v9.1.0"]

    def test_the_next_older_published_tag(self):
        self.assertEqual(diff.release_baseline("v10.0.1", self.TAGS, lambda t: True), "v10.0.0")

    def test_a_tag_whose_image_never_reached_docker_hub_is_skipped(self):
        # v10.0.0 failed its gate; diffing against it would wave its findings through.
        self.assertEqual(diff.release_baseline("v10.0.1", self.TAGS, lambda t: t != "v10.0.0"), "v9.1.1")

    def test_a_hotfix_on_an_older_line_diffs_against_that_line(self):
        tags = ["v10.0.1", "v10.0.0", "v9.1.2", "v9.1.1"]
        self.assertEqual(diff.release_baseline("v9.1.2", tags, lambda t: True), "v9.1.1")

    def test_newer_tags_are_never_the_baseline(self):
        published = []
        diff.release_baseline("v10.0.0", self.TAGS, lambda t: published.append(t) or True)
        self.assertEqual(published, ["v9.1.1"])

    def test_no_published_older_tag_raises(self):
        with self.assertRaisesRegex(diff.ToolError, "no tag older than v9.1.0"):
            diff.release_baseline("v9.1.0", self.TAGS, lambda t: True)
        with self.assertRaisesRegex(diff.ToolError, "no tag older than v10.0.1"):
            diff.release_baseline("v10.0.1", self.TAGS, lambda t: False)

    def test_an_unknown_tag_raises(self):
        with self.assertRaisesRegex(diff.ToolError, "v11.0.0 is not among"):
            diff.release_baseline("v11.0.0", self.TAGS, lambda t: True)


class VersionTagsTest(unittest.TestCase):
    def test_v_tags_newest_first_by_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            git = ["git", "-C", tmp, "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false"]
            subprocess.run(git + ["init", "-q"], check=True)
            subprocess.run(git + ["commit", "-q", "--allow-empty", "-m", "c"], check=True)
            for tag in ("v1.9.0", "v1.10.0", "v2.0.0", "other"):
                subprocess.run(git + ["tag", tag], check=True)
            self.assertEqual(diff.version_tags(Path(tmp)), ["v2.0.0", "v1.10.0", "v1.9.0"])


class DockerHubTest(unittest.TestCase):
    def urlopen(self, *responses):
        def answer(url, timeout):
            self.urls.append(url)
            r = responses[len(self.urls) - 1]
            if isinstance(r, Exception):
                raise r
            return contextlib.nullcontext()

        self.urls = []
        return mock.patch.object(diff.urllib.request, "urlopen", answer)

    def http_error(self, code):
        return diff.urllib.error.HTTPError("u", code, "m", {}, None)

    def test_200_is_published(self):
        with self.urlopen(None):
            self.assertTrue(diff.docker_hub_has_tag("org/image", "v1.0.0"))
        self.assertEqual(self.urls, ["https://hub.docker.com/v2/repositories/org/image/tags/v1.0.0"])

    def test_404_is_not_published(self):
        with self.urlopen(self.http_error(404)):
            self.assertFalse(diff.docker_hub_has_tag("org/image", "v1.0.0"))

    def test_other_errors_are_retried_then_raise(self):
        with self.urlopen(self.http_error(500), diff.http.client.RemoteDisconnected("eof"), None), mock.patch.object(diff.time, "sleep"):
            self.assertTrue(diff.docker_hub_has_tag("org/image", "v1.0.0"))
        with self.urlopen(diff.urllib.error.URLError("down"), None), mock.patch.object(diff.time, "sleep"):
            self.assertTrue(diff.docker_hub_has_tag("org/image", "v1.0.0"))
        with self.urlopen(*[self.http_error(503)] * diff.HUB_ATTEMPTS), mock.patch.object(diff.time, "sleep"):
            with self.assertRaisesRegex(diff.ToolError, "HTTP 503"):
                diff.docker_hub_has_tag("org/image", "v1.0.0")


class ReleaseBaselineMainTest(unittest.TestCase):
    def run_main(self, config, published):
        stdout, stderr = io.StringIO(), io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "advisories.toml"
            path.write_text(config)
            with mock.patch.object(diff, "version_tags", lambda root: ["v2.0.0", "v1.1.0", "v1.0.0"]), mock.patch.object(diff, "docker_hub_has_tag", published):
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    code = diff.main(["--config", str(path), "--release-baseline", "v2.0.0"])
        return code, stdout.getvalue(), stderr.getvalue()

    def test_prints_the_baseline_looked_up_in_the_configured_repository(self):
        lookups = []
        code, out, err = self.run_main('[release]\ndocker_hub_repository = "org/image"\n', lambda repo, tag: lookups.append((repo, tag)) or tag == "v1.0.0")
        self.assertEqual(code, 0, err)
        self.assertEqual(out, "v1.0.0\n")
        self.assertEqual(lookups, [("org/image", "v1.1.0"), ("org/image", "v1.0.0")])

    def test_no_baseline_exits_3_and_prints_nothing(self):
        code, out, err = self.run_main('[release]\ndocker_hub_repository = "org/image"\n', lambda repo, tag: False)
        self.assertEqual((code, out), (3, ""))
        self.assertIn("diff: error:", err)

    def test_a_config_without_the_repository_exits_3(self):
        code, out, _ = self.run_main("", lambda repo, tag: True)
        self.assertEqual((code, out), (3, ""))


if __name__ == "__main__":
    unittest.main()
