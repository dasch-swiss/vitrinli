import contextlib
import datetime
import io
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import scan
from _model import ToolError

FIXTURES = Path(__file__).parent / "fixtures" / "scan"
TODAY = datetime.date(2026, 11, 5)
IMAGE = "registry.example/images/base-debian12"


class FakeScanners:
    """Answers scanner calls with recorded JSON; `codes` overrides an exit code per scanner."""

    def __init__(self, codes=None, outputs=None):
        self.codes = codes or {}
        self.outputs = outputs or {}
        self.calls = []

    def __call__(self, cmd, cwd):
        name = Path(cmd[0]).name
        self.calls.append(cmd)
        if name == "osv-scanner":
            key = "osv"
            self.sbom = json.loads(Path(cmd[cmd.index("-L") + 1]).read_text())
        elif cmd[1].startswith("sbom:"):
            key = "grype-sbom"
        else:
            key = "grype-image-" + cmd[cmd.index("--platform") + 1].split("/")[1]
        out = self.outputs.get(key, (FIXTURES / f"{key}.json").read_text())
        default = 1 if key == "osv" else 0
        return self.codes.get(key, default), out, f"{key} stderr"


class ScanTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)
        shutil.copytree(FIXTURES, self.dir / "cfg")
        self.config = self.dir / "cfg" / "advisories.toml"
        self.inventory = self.dir / "cfg" / "inventory.cdx.json"
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        for name in ("osv-scanner", "grype"):
            (bin_dir / name).write_text("#!/bin/sh\nexit 99\n")
            (bin_dir / name).chmod(0o755)
        patcher = mock.patch.dict(os.environ, {"ADVISORIES_BIN": str(bin_dir)})
        patcher.start()
        self.addCleanup(patcher.stop)

    def write_ignores(self, text):
        (self.dir / "cfg" / "ignores.toml").write_text(text)

    def edit_inventory(self, fn):
        bom = json.loads(self.inventory.read_text())
        fn(bom)
        self.inventory.write_text(json.dumps(bom))

    def run_scan(self, today=TODAY, **fake):
        self.fake = FakeScanners(**fake)
        return scan.scan(self.config, self.inventory, today, self.fake)

    def run_main(self, today=TODAY, **fake):
        self.fake = FakeScanners(**fake)
        out = self.dir / "findings.json"
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(scan, "run_process", self.fake), mock.patch.object(scan, "utc_today", lambda: today):
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                code = scan.main(["--config", str(self.config), "--inventory", str(self.inventory), "--output", str(out)])
        return code, stdout.getvalue(), stderr.getvalue(), json.loads(out.read_text()) if out.exists() else None

    def finding(self, result, component, id):
        return next(f for f in result.findings if f.key == (component, id))

    # ── Normalization ──

    def test_osv_finding_carries_group_aliases_severity_and_fix(self):
        r = self.run_scan()
        rsa = self.finding(r, "rsa", "RUSTSEC-2023-0071")
        self.assertEqual((rsa.version, rsa.source, rsa.severity, rsa.fixed), ("0.9.10", "cargo", "medium", None))
        self.assertIn("GHSA-4grx-2x9w-596c", rsa.aliases)
        otel = self.finding(r, "opentelemetry_sdk", "GHSA-w9wp-h8wv-79jx")
        self.assertEqual((otel.severity, otel.fixed, otel.aliases), ("medium", "0.32.1", ("CVE-2026-48504",)))

    def test_osv_severity_from_ghsa_database_when_group_has_no_score(self):
        osv = json.loads((FIXTURES / "osv.json").read_text())
        group = osv["results"][0]["packages"][1]["groups"][0]
        self.assertEqual(osv["results"][0]["packages"][1]["package"]["name"], "opentelemetry_sdk")
        group["max_severity"] = ""
        r = self.run_scan(outputs={"osv": json.dumps(osv)})
        self.assertEqual(self.finding(r, "opentelemetry_sdk", "GHSA-w9wp-h8wv-79jx").severity, "medium")

    def test_grype_finding_keeps_inventory_source_and_lowest_fix_above(self):
        r = self.run_scan()
        f = self.finding(r, "curl", "CVE-2023-38545")
        self.assertEqual((f.source, f.version, f.fixed, f.severity), ("bcr", "8.3.0", "8.4.0", "critical"))

    def test_deb_finding_on_both_arches_is_one_finding(self):
        r = self.run_scan()
        libc = [f for f in r.findings if f.component == "libc6" and f.id == "CVE-2023-4911"]
        self.assertEqual(len(libc), 1)
        self.assertEqual(libc[0].source, "deb")

    def test_base_image_findings_name_the_image_and_its_packages(self):
        r = self.run_scan()
        f = self.finding(r, IMAGE, "CVE-2010-4756")
        self.assertEqual((f.source, f.severity, f.fixed), ("oci", "negligible", None))
        self.assertEqual(f.packages, ("libc6 2.36-9+deb12u13",))
        self.assertEqual(len([x for x in r.findings if x.key == f.key]), 1, "both platform scans merge into one image finding")
        platforms = [c[c.index("--platform") + 1] for c in self.fake.calls if "--platform" in c]
        self.assertEqual(platforms, ["linux/amd64", "linux/arm64"])
        self.assertTrue(all(c[1] == f"registry:{IMAGE}@sha256:{'a' * 64}" for c in self.fake.calls if "--platform" in c))

    def test_findings_sharing_an_alias_merge_under_the_preferred_id(self):
        grype = json.loads((FIXTURES / "grype-sbom.json").read_text())
        curl = next(m for m in grype["matches"] if m["artifact"]["name"] == "curl")
        twin = json.loads(json.dumps(curl))
        twin["vulnerability"]["id"] = "GHSA-0000-0000-0000"
        twin["relatedVulnerabilities"] = [{"id": "CVE-2023-38545"}]
        grype["matches"].append(twin)
        r = self.run_scan(outputs={"grype-sbom": json.dumps(grype)})
        curl_findings = [f for f in r.findings if f.component == "curl" and "CVE-2023-38545" in (f.id, *f.aliases)]
        self.assertEqual([(f.id, f.aliases) for f in curl_findings], [("GHSA-0000-0000-0000", ("CVE-2023-38545",))])

    def test_a_finding_bridging_two_groups_merges_all_three(self):
        grype = json.loads((FIXTURES / "grype-sbom.json").read_text())
        curl = next(m for m in grype["matches"] if m["artifact"]["name"] == "curl")
        other = json.loads(json.dumps(curl))
        other["vulnerability"].update(id="CVE-2099-0001", severity="Low", fix={"versions": ["8.3.1"], "state": "fixed"})
        other["relatedVulnerabilities"] = []
        bridge = json.loads(json.dumps(other))
        bridge["vulnerability"].update(id="GHSA-0000-0000-0000", fix={"versions": [], "state": "not-fixed"})
        bridge["relatedVulnerabilities"] = [{"id": "CVE-2023-38545"}, {"id": "CVE-2099-0001"}]
        grype["matches"] += [other, bridge]
        r = self.run_scan(outputs={"grype-sbom": json.dumps(grype)})
        merged = [f for f in r.findings if f.component == "curl"]
        self.assertEqual([(f.id, f.aliases) for f in merged], [("GHSA-0000-0000-0000", ("CVE-2023-38545", "CVE-2099-0001"))])
        self.assertEqual((merged[0].severity, merged[0].fixed), ("critical", "8.3.1"))

    def test_scanners_get_their_class_only(self):
        self.run_scan()
        self.assertEqual({c["name"] for c in self.fake.sbom["components"]}, {"rsa", "opentelemetry_sdk"})
        grype_sbom = next(c[1] for c in self.fake.calls if c[1].startswith("sbom:"))
        self.assertTrue(grype_sbom.endswith("grype.cdx.json"))

    def test_scanners_run_without_their_own_ignores(self):
        self.run_scan()
        for cmd in self.fake.calls:
            self.assertNotIn("--fail-on", cmd)
            config = Path(cmd[cmd.index("--config") + 1])
            self.assertIn(config.name, ("osv-scanner.toml", "grype.yaml"))

    # ── EOL ──

    def test_eol_finding_from_the_eol_date_on(self):
        r = self.run_scan(today=datetime.date(2026, 9, 10))
        self.assertIn((IMAGE, "EOL"), {f.key for f in r.findings})
        r = self.run_scan(today=datetime.date(2026, 9, 9))
        self.assertNotIn((IMAGE, "EOL"), {f.key for f in r.findings})

    def test_one_eol_finding_per_config_source(self):
        r = self.run_scan()
        self.assertEqual(sorted(f.component for f in r.findings if f.id == "EOL"), sorted(["debian", IMAGE]))

    def test_ignored_eol(self):
        self.write_ignores('[[ignore]]\nid = "EOL"\ncomponent = "debian"\nreason = "move to base-debian13"\nexpiry = 2027-01-31\n')
        r = self.run_scan()
        self.assertIn(("debian", "EOL"), r.ignored)
        self.assertNotIn(("debian", "EOL"), {f.key for f in r.open})

    # ── Ignores ──

    def test_ignore_matches_an_alias(self):
        r = self.run_scan()
        self.assertIn(("rsa", "RUSTSEC-2023-0071"), r.ignored)

    def test_ignore_for_another_component_does_not_suppress(self):
        self.write_ignores('[[ignore]]\nid = "CVE-2010-4756"\ncomponent = "libc6"\nreason = "r"\nexpiry = 2027-01-31\n')
        r = self.run_scan()
        self.assertIn((IMAGE, "CVE-2010-4756"), {f.key for f in r.open})

    def test_expired_ignore_stops_suppressing(self):
        expiry = datetime.date(2027, 1, 31)
        self.assertIn(("rsa", "RUSTSEC-2023-0071"), self.run_scan(today=expiry).ignored)
        self.assertNotIn(("rsa", "RUSTSEC-2023-0071"), self.run_scan(today=expiry + datetime.timedelta(days=1)).ignored)

    def lint_error(self, text):
        with self.assertRaises(ToolError) as cm:
            scan.load_ignores(text)
        return str(cm.exception)

    def test_lint_rejects_empty_reason(self):
        self.assertIn("reason", self.lint_error('[[ignore]]\nid = "X"\ncomponent = "c"\nreason = "  "\nexpiry = 2027-01-31\n'))

    def test_lint_rejects_quoted_expiry(self):
        self.assertIn("bare TOML date", self.lint_error('[[ignore]]\nid = "X"\ncomponent = "c"\nreason = "r"\nexpiry = "2027-01-31"\n'))

    def test_lint_rejects_datetime_expiry(self):
        self.assertIn("bare TOML date", self.lint_error('[[ignore]]\nid = "X"\ncomponent = "c"\nreason = "r"\nexpiry = 2027-01-31T00:00:00\n'))

    def test_lint_rejects_missing_and_unknown_keys(self):
        err = self.lint_error('[[ignore]]\nid = "X"\ncomponent = "c"\nreason = "r"\nexpires = 2027-01-31\n')
        self.assertIn("unknown key 'expires'", err)
        self.assertIn("missing 'expiry'", err)

    def test_invalid_ignore_file_exits_3(self):
        self.write_ignores('[[ignore]]\nid = "X"\ncomponent = "c"\nreason = ""\nexpiry = 2027-01-31\n')
        code, _, err, out = self.run_main()
        self.assertEqual(code, 3, err)
        self.assertIsNone(out)

    # ── Exit codes ──

    def test_open_findings_exit_1_and_are_written(self):
        code, stdout, err, out = self.run_main()
        self.assertEqual(code, 1, err)
        self.assertIn("libc6 2.36-9+deb12u1: CVE-2023-4911", stdout)
        rsa = next(f for f in out["findings"] if f["component"] == "rsa")
        self.assertEqual(rsa["ignored"]["expiry"], "2027-01-31")
        self.assertEqual(out["grype_db_built"], "2026-11-05T06:21:06Z")

    def test_everything_ignored_exits_0(self):
        r = self.run_scan()
        self.write_ignores("".join(f'[[ignore]]\nid = "{f.id}"\ncomponent = "{f.component}"\nreason = "r"\nexpiry = 2027-01-31\n' for f in r.findings))
        code, _, err, _ = self.run_main()
        self.assertEqual(code, 0, err)

    def test_osv_exit_0_and_1_are_results(self):
        clean = json.dumps({"results": []})
        self.assertEqual(self.run_main(codes={"osv": 0}, outputs={"osv": clean})[0], 1)
        self.assertEqual(self.run_main(codes={"osv": 1})[0], 1)

    def test_osv_other_exit_codes_exit_3(self):
        for code in (127, 128, 2):
            with self.subTest(code=code):
                rc, _, err, out = self.run_main(codes={"osv": code})
                self.assertEqual(rc, 3)
                self.assertIn(f"osv-scanner exited {code}", err)
                self.assertIsNone(out)

    def test_grype_non_zero_exits_3(self):
        for key in ("grype-sbom", "grype-image-arm64"):
            with self.subTest(key=key):
                rc, _, err, _ = self.run_main(codes={key: 1})
                self.assertEqual(rc, 3)
                self.assertIn("exited 1", err)

    def test_scanner_without_json_exits_3(self):
        rc, _, err, _ = self.run_main(outputs={"grype-sbom": "not json"})
        self.assertEqual(rc, 3)
        self.assertIn("without JSON output", err)

    def test_empty_grype_sbom_exits_3_before_scanning(self):
        self.edit_inventory(lambda bom: bom.update(components=[c for c in bom["components"] if c.get("purl", "").startswith("pkg:cargo/") or c["type"] != "library"]))
        rc, _, err, _ = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("no component for Grype", err)
        self.assertEqual(self.fake.calls, [])

    def test_missing_scanner_exits_3(self):
        (Path(os.environ["ADVISORIES_BIN"]) / "grype").unlink()
        rc, _, err, _ = self.run_main()
        self.assertEqual(rc, 3)
        self.assertIn("grype not found in $ADVISORIES_BIN", err)

    def test_scanner_from_advisories_bin_over_path(self):
        on_path = self.dir / "path"
        on_path.mkdir()
        shutil.copy(Path(os.environ["ADVISORIES_BIN"]) / "grype", on_path / "grype")
        with mock.patch.dict(os.environ, {"PATH": f"{on_path}{os.pathsep}{os.environ['PATH']}"}):
            self.assertEqual(scan.scanner("grype"), str(Path(os.environ["ADVISORIES_BIN"]) / "grype"))
            del os.environ["ADVISORIES_BIN"]
            self.assertEqual(scan.scanner("grype"), str(on_path / "grype"))


class RepoIgnoresTest(unittest.TestCase):
    def test_repo_ignore_file_lints(self):
        text = (Path(__file__).parents[1] / "ignores.toml").read_text()
        ignores = scan.load_ignores(text)
        keys = [(i.component, i.id) for i in ignores]
        self.assertEqual(len(keys), len(set(keys)), "duplicate (component, id) entries")


if __name__ == "__main__":
    unittest.main()
