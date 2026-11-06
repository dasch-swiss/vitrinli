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

# A transitive-only module (zlib) is in the lock but has no bazel_dep.
MODULE = """\
bazel_dep(name = "curl", version = "8.21.0.bcr.1")
bazel_dep(name = "openssl", version = "3.5.5.bcr.4")
single_version_override(
    module_name = "openssl",
    version = "3.5.5.bcr.4",
)
bazel_dep(name = "libmagic", version = "5.47")
single_version_override(
    module_name = "libmagic",
    patch_strip = 1,
    patches = ["//bazel/patches:libmagic.patch"],
)
oci.pull(
    name = "distroless_base",
    digest = "sha256:%s",  # captured by hand
    image = "gcr.io/distroless/base-debian12",
)
""" % ("a" * 64)

MODULE_VERSIONS = {"curl": "8.21.0.bcr.1", "openssl": "3.5.5.bcr.4", "libmagic": "5.47", "zlib": "1.3.1.bcr.5"}
BCR = "https://bcr.bazel.build"


def module_lock(versions=MODULE_VERSIONS):
    keys = {f"{BCR}/modules/{n}/{v}/{f}": "0" for n, v in versions.items() for f in ("MODULE.bazel", "source.json")}
    return json.dumps({"lockFileVersion": 24, "registryFileHashes": keys}, indent=2) + "\n"


METADATA = {
    "curl": {"versions": ["8.12.0", "8.21.0", "8.21.0.bcr.1", "8.21.0.bcr.2", "8.22.0", "8.22.0.bcr.1"], "yanked_versions": {"8.22.0": "broken"}},
    "openssl": {"versions": ["3.5.5.bcr.4", "3.5.6", "3.5.6.bcr.1"], "yanked_versions": {}},
    "zlib": {"versions": ["1.3.1.bcr.5", "1.3.2"], "yanked_versions": {}},
}

COMPONENT_MAP = [
    {"source": "bcr", "name": "curl", "status": "shipped", "cpe": ["cpe:2.3:a:haxx:curl", "cpe:2.3:a:haxx:libcurl"]},
    {"source": "bcr", "name": "openssl", "status": "shipped", "cpe": ["cpe:2.3:a:openssl:openssl"]},
]

DEB_MANIFEST = """\
# Snapshot pin: a `YYYYMMDDTHHMMSSZ` timestamp.
sources:
  - channel: bookworm main
    urls:
      - https://snapshot-cloudflare.debian.org/archive/debian/20260101T000000Z
      - https://snapshot.debian.org/archive/debian/20260101T000000Z
  - channel: bookworm-security main
    url: https://snapshot-cloudflare.debian.org/archive/debian-security/20260101T000000Z
packages:
  - "tini"
"""


def deb_lock(libc6="2.36-9+deb12u13"):
    def pkg(name, source, version):
        url = f"https://snapshot.debian.org/archive/debian/20260101T000000Z/pool/main/{source[0]}/{source}/{name}_{version}_amd64.deb"
        return {"arch": "amd64", "name": name, "version": version, "urls": [url]}

    return json.dumps({"version": 1, "packages": [pkg("libc6", "glibc", libc6), pkg("libc-bin", "glibc", libc6), pkg("tini", "tini", "0.19.0-1+b3")]}, indent=2) + "\n"


IMAGE, OLD_DIGEST, NEW_DIGEST = "gcr.io/distroless/base-debian12", "sha256:" + "a" * 64, "sha256:" + "b" * 64

CONFIG = {
    "inputs": {"cargo_lock": "Cargo.Bazel.lock", "module_bazel": "MODULE.bazel", "module_lock": "MODULE.bazel.lock"},
    "base_image": {"oci_pull": "distroless_base", "tracks": f"{IMAGE}:latest", "platforms": ["linux/amd64", "linux/arm64"]},
    "deb": {"manifest": "bookworm.yaml", "lock": "bookworm.lock.json"},
}


def lock(text=LOCK):
    import tomllib

    return tomllib.loads(text)


def finding(component, id, fixed, source="cargo", ignored=None, version="1.40.0"):
    return {"component": component, "version": version, "source": source, "id": id, "aliases": [], "severity": "high", "fixed": fixed, "packages": [], "ignored": ignored}


def grype_doc(hits, id="CVE-2026-0001"):
    return {"matches": [{"vulnerability": {"id": id, "severity": "High", "fix": {"state": "fixed", "versions": ["9.9.9"]}}, "artifact": {"name": n, "version": v}} for n, v in hits]}


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


class BazelVersionTest(unittest.TestCase):
    def test_module_version_ordering(self):
        ordered = ["1.0-alpha", "1.0-rc.1", "1.0-rc.2", "1.0", "1.0.0", "1.0.1", "1.0.bcr.1", "1.0.bcr.2", "1.0.bcr.10", "1.2", "1.10", "1.a", "1.b", ""]
        self.assertEqual(sorted(reversed(ordered), key=fix.bazel_version_key), ordered)

    def test_a_bcr_repack_sorts_after_its_release_and_a_numeric_suffix(self):
        ordered = ["8.21.0", "8.21.0.1", "8.21.0.bcr.1", "8.21.0.bcr.2", "8.22.0"]
        self.assertEqual(sorted(reversed(ordered), key=fix.bazel_version_key), ordered)

    def test_build_metadata_is_ignored_and_garbage_is_an_error(self):
        self.assertEqual(fix.bazel_version_key("1.2+build.5"), fix.bazel_version_key("1.2"))
        for bad in ("1..2", "1.2_3", "1.2-", "1.0-a..b"):
            with self.subTest(bad), self.assertRaises(ToolError):
                fix.bazel_version_key(bad)

    def test_target_is_the_lowest_non_yanked_version_at_or_past_the_fix(self):
        self.assertEqual(fix.bcr_target("8.21.0", METADATA["curl"]), "8.21.0")
        self.assertEqual(fix.bcr_target("8.21.0.bcr.2", METADATA["curl"]), "8.21.0.bcr.2")
        # 8.22.0 is yanked, so its repack is the lowest at or past the fix.
        self.assertEqual(fix.bcr_target("8.22.0", METADATA["curl"]), "8.22.0.bcr.1")
        self.assertIsNone(fix.bcr_target("8.23.0", METADATA["curl"]))
        # A prerelease is never proposed, even as the only version past the fix.
        self.assertIsNone(fix.bcr_target("8.23.0", {"versions": ["9.0.0-rc.1"]}))

    def test_set_digest_rejects_a_one_line_call(self):
        import inventory

        text = 'oci.pull(name = "b", digest = "sha256:aa", image = "i")\n'
        [call] = inventory.starlark_calls(text)
        with self.assertRaisesRegex(ToolError, "expected one line"):
            fix.set_digest(text, call, "sha256:bb", "t on d")

    def test_set_digest_applies_to_the_repository_module_file(self):
        import inventory

        text = (Path(__file__).resolve().parents[3] / "MODULE.bazel").read_text()
        [call] = [c for c in inventory.starlark_calls(text) if c.callee == "oci.pull"]
        new = fix.set_digest(text, call, "sha256:" + "b" * 64, "t on d")
        self.assertEqual(sum(a != b for a, b in zip(text.splitlines(), new.splitlines())), 1)
        self.assertIn('digest = "sha256:' + "b" * 64 + '",  # t on d\n', new)

    def test_set_versions_edits_only_the_named_calls(self):
        import inventory

        calls = [c for c in inventory.starlark_calls(MODULE) if c.kwargs.get("name") == "openssl" or c.kwargs.get("module_name") == "openssl"]
        text = fix.set_versions(MODULE, [(c, "3.5.6") for c in calls])
        self.assertEqual(text, MODULE.replace('"3.5.5.bcr.4"', '"3.5.6"'))
        [patched] = [c for c in inventory.starlark_calls(MODULE) if c.kwargs.get("module_name") == "libmagic"]
        with self.assertRaisesRegex(ToolError, "expected one version, found 0"):
            fix.set_versions(MODULE, [(patched, "5.48")])


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

    def test_no_target_compares_the_advisory_set_alone(self):
        other_target = fix.body("tokio", "1.40.0", "1.44.0", self.IDS)
        self.assertEqual(fix.decide(self.state(head="abc", commits=1, open_body=other_target), self.IDS, None), fix.CURRENT)
        other_ids = fix.body("tokio", "1.40.0", "1.44.0", ("RUSTSEC-2026-0002",))
        self.assertEqual(fix.decide(self.state(head="abc", commits=1, open_body=other_ids), self.IDS, None), fix.REBUILD)


class FakeRun:
    """Answers the git, gh, just and osv-scanner calls of a fix run over a temporary checkout."""

    def __init__(self, root, open_prs=(), closed_prs=(), remote_head=None, commits=1, repin_changes=True, still_flagged=False, failing_repin=(), vulnerable=(("tokio", "1.40.0"),), dirty=(), resolves=None, digest=NEW_DIGEST):
        self.root, self.calls = root, []
        self.vulnerable = set(vulnerable)
        self.open_prs, self.closed_prs = list(open_prs), list(closed_prs)
        self.remote_head, self.commits = remote_head, commits
        self.repin_changes, self.still_flagged, self.failing_repin = repin_changes, still_flagged, failing_repin
        # What `just bazel-lock-update` resolves, over the bazel_dep versions; None: those versions.
        self.resolves = resolves
        # What `crane digest` prints for the tracked tag.
        self.digest = digest
        # The base tree, which a checkout restores; `git status` reports what differs from it.
        self.base = {p.name: p.read_text() for p in root.iterdir()}
        self.changed = set(dirty)

    def status(self):
        return self.changed | {n for n, text in self.base.items() if (self.root / n).read_text() != text}

    def __call__(self, cmd, cwd):
        self.calls.append(cmd)
        name = Path(cmd[0]).name
        if name == "crane":
            return 0, self.digest + "\n", ""
        if name == "grype" and cmd[1].startswith("registry:"):
            # An image's matches are its packages; `vulnerable` names the image by digest.
            return 0, json.dumps(grype_doc([("libssl3", "3.0.1")] if (IMAGE, cmd[1].split("@")[1]) in self.vulnerable else [])), ""
        if name == "grype":
            sbom = json.loads(Path(cmd[1].removeprefix("sbom:")).read_text())
            return 0, json.dumps(grype_doc([(c["name"], c["version"]) for c in sbom["components"] if (c["name"], c["version"]) in self.vulnerable])), ""
        if cmd[:2] == ["just", "bazel-distroless-lock"]:
            if self.repin_changes:
                (self.root / "bookworm.lock.json").write_text(deb_lock("2.36-9+deb12u14"))
            return 0, "", ""
        if cmd[:2] == ["just", "bazel-lock-update"]:
            import inventory

            pinned = {c.kwargs["name"]: c.kwargs["version"] for c in inventory.starlark_calls((self.root / "MODULE.bazel").read_text()) if c.callee == "bazel_dep"}
            (self.root / "MODULE.bazel.lock").write_text(module_lock({**MODULE_VERSIONS, **pinned, **(self.resolves or {})}))
            return 0, "", ""
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
            return 0, "".join(f" M {p}\n" for p in sorted(self.status())), ""
        if cmd[:2] == ["git", "rev-parse"]:
            return (0, self.remote_head + "\n", "") if self.remote_head else (1, "", "")
        if cmd[:2] == ["git", "rev-list"]:
            return 0, f"{self.commits}\n", ""
        if cmd[:2] == ["git", "checkout"]:
            for n, text in self.base.items():
                (self.root / n).write_text(text)
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


class FixRunCase(unittest.TestCase):
    """A fix run over a temporary checkout, its commands answered by `FakeRun`."""

    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name) / "tree"
        self.root.mkdir()
        bin_dir = Path(tmp.name) / "bin"
        bin_dir.mkdir()
        for tool in ("osv-scanner", "grype", "crane"):
            (bin_dir / tool).write_text("#!/bin/sh\nexit 99\n")
            (bin_dir / tool).chmod(0o755)
        patcher = mock.patch.dict(os.environ, {"ADVISORIES_BIN": str(bin_dir)})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.outcomes = Path(tmp.name) / "outcomes.json"

    def fetch(self, url):
        self.fetched.append(url)
        name = url.split("/modules/")[1].split("/")[0]
        if name not in METADATA:
            raise ToolError(f"GET {url}: HTTP 404")
        return json.dumps(METADATA[name]).encode()

    def write_tree(self):
        """The base tree; a previous run in the same test leaves its fix branch's edits."""
        (self.root / "Cargo.Bazel.lock").write_text(LOCK)
        (self.root / "MODULE.bazel").write_text(MODULE)
        (self.root / "MODULE.bazel.lock").write_text(module_lock())
        (self.root / "bookworm.yaml").write_text(DEB_MANIFEST)
        (self.root / "bookworm.lock.json").write_text(deb_lock())

    def run_fix(self, findings, dry_run=False, ignores=(), files=None, **fake):
        self.write_tree()
        for name, text in (files or {}).items():
            (self.root / name).write_text(text)
        self.fake = FakeRun(self.root, **fake)
        self.fetched = []
        ctx = fix.Context(self.root, CONFIG, list(ignores), TODAY, "main", dry_run, self.fake, COMPONENT_MAP, self.fetch)
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            outcomes = fix.fix_all(ctx, {"findings": findings}, self.outcomes)
        self.stdout, self.stderr = out.getvalue(), err.getvalue()
        return outcomes


class FixRunTest(FixRunCase):
    TOKIO = finding("tokio", "RUSTSEC-2026-0001", "1.43.1")

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
        cfg.write_text('ignores = "ignores.toml"\ncomponent_map = "components.toml"\n[inputs]\ncargo_lock = "Cargo.Bazel.lock"\nmodule_lock = "MODULE.bazel.lock"\n')
        (self.root / "ignores.toml").write_text("")
        (self.root / "components.toml").write_text("")
        findings = self.root / "findings.json"
        findings.write_text(json.dumps({"findings": [self.TOKIO]}))
        self.write_tree()
        fake = FakeRun(self.root, failing_repin=("tokio",))
        argv = ["--config", str(cfg), "--findings", str(findings), "--outcomes", str(self.outcomes), "--root", str(self.root), "--dry-run"]
        with mock.patch.object(scan, "run_process", fake), contextlib.redirect_stderr(io.StringIO()) as err:
            self.assertEqual(fix.main(argv), 3)
        self.assertIn("fix: tokio: error: ToolError: just bazel-repin-crate tokio@1.40.0=1.43.1 exited 1", err.getvalue())
        self.assertEqual(json.loads(self.outcomes.read_text())["outcomes"][0]["status"], "error")



class BcrFixTest(FixRunCase):
    CURL = finding("curl", "CVE-2026-1001", "8.22.0", source="bcr", version="8.21.0")

    def test_raises_the_bazel_dep_past_a_yanked_release_and_refreshes_the_lock(self):
        [o] = self.run_fix([self.CURL])
        self.assertEqual((o.status, o.target, o.branch), (fix.OPENED, "8.22.0.bcr.1", "advisories/curl"))
        self.assertEqual(self.fetched, ["https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/modules/curl/metadata.json"])
        self.assertEqual(self.fake.ran("just"), [["just", "bazel-lock-update"]])
        self.assertEqual(self.fake.ran("git", "add"), [["git", "add", "--", "MODULE.bazel", "MODULE.bazel.lock"]])
        [commit] = self.fake.ran("git", "commit")
        self.assertEqual(commit[4], "fix(deps): raise curl to 8.22.0.bcr.1 past CVE-2026-1001")
        self.assertIn("Raises the `bazel_dep` on `curl` from 8.21.0.bcr.1 to 8.22.0.bcr.1,", commit[6])
        self.assertIn("Advisories: CVE-2026-1001\nResolved version: 8.22.0.bcr.1\n", commit[6])
        # The re-scan matches the upstream version by every CPE of the module.
        [grype] = self.fake.ran(str(Path(os.environ["ADVISORIES_BIN"]) / "grype"))
        self.assertTrue(grype[1].startswith("sbom:"))

    def test_a_single_version_override_is_raised_with_its_bazel_dep(self):
        written = []
        real = FakeRun.__call__

        def spy(fake, cmd, cwd):
            if cmd[:2] == ["git", "add"]:
                written.append((self.root / "MODULE.bazel").read_text())
            return real(fake, cmd, cwd)

        with mock.patch.object(FakeRun, "__call__", spy):
            [o] = self.run_fix([finding("openssl", "CVE-2026-1002", "3.5.6", source="bcr", version="3.5.5")])
        self.assertEqual((o.status, o.target), (fix.OPENED, "3.5.6"))
        self.assertEqual(written, [MODULE.replace('"3.5.5.bcr.4"', '"3.5.6"')])
        self.assertIn("`openssl` and its `single_version_override` from 3.5.5.bcr.4 to 3.5.6", self.fake.ran("git", "commit")[0][6])

    def test_alert_only_cases_fetch_and_change_nothing(self):
        cases = [
            ("patched override", finding("libmagic", "CVE-2026-1003", "5.48", source="bcr", version="5.47"), "alert only: its single_version_override carries patches"),
            ("patches not a literal", finding("openssl", "CVE-2026-1008", "3.5.6", source="bcr", version="3.5.5"), "alert only: its single_version_override carries patches"),
            ("patch_cmds", finding("rules_x", "CVE-2026-1009", "2.0", source="bcr", version="1.0"), "alert only: its single_version_override carries patches"),
            ("not a direct bazel_dep", finding("zlib", "CVE-2026-1004", "1.3.2", source="bcr", version="1.3.1"), "alert only: zlib is not a direct bazel_dep"),
            ("no released fix", finding("curl", "CVE-2026-1005", None, source="bcr", version="8.21.0"), "alert only: no released fix for CVE-2026-1005"),
        ]
        variable_patches = MODULE.replace('    version = "3.5.5.bcr.4",\n)', '    version = "3.5.5.bcr.4",\n    patches = OPENSSL_PATCHES,\n)')
        variable_patches += 'bazel_dep(name = "rules_x", version = "1.0")\nsingle_version_override(\n    module_name = "rules_x",\n    patch_cmds = ["true"],\n)\n'
        for name, f, note in cases:
            with self.subTest(name):
                [o] = self.run_fix([f], files={"MODULE.bazel": variable_patches})
                self.assertEqual((o.status, o.note), (fix.ALERT, note))
                self.assertEqual(self.fetched + self.fake.ran("just") + self.fake.ran("git", "commit"), [])

    def test_no_registry_version_past_the_fix_is_alert_only(self):
        [o] = self.run_fix([finding("curl", "CVE-2026-1006", "8.23.0", source="bcr", version="8.21.0")])
        self.assertEqual((o.status, o.note), (fix.ALERT, "alert only: no BCR version at or past 8.23.0 yet"))

    def test_a_target_at_or_below_the_resolved_version_needs_an_ignore(self):
        [o] = self.run_fix([finding("curl", "CVE-2026-1007", "8.21.0", source="bcr", version="8.21.0")])
        self.assertEqual((o.status, o.note, o.target), (fix.NEEDS_IGNORE, fix.NEEDS_IGNORE_NOTE, "8.21.0"))
        self.assertEqual(self.fake.ran("just"), [])

    def test_still_flagged_after_the_bump_opens_no_pr(self):
        [o] = self.run_fix([self.CURL], vulnerable={("curl", "8.22.0")})
        self.assertEqual((o.status, o.note), (fix.NEEDS_IGNORE, fix.NEEDS_IGNORE_NOTE))
        self.assertEqual(self.fake.ran("git", "commit"), [])

    def test_a_lock_resolving_another_version_is_an_error(self):
        [o] = self.run_fix([self.CURL], resolves={"curl": "8.23.0"})
        self.assertEqual(o.status, fix.ERROR)
        self.assertIn("the lock update resolved curl 8.23.0, not 8.22.0.bcr.1", self.stderr)
        self.assertEqual(self.fake.ran("git", "commit"), [])

    def test_a_registry_read_failure_is_an_error(self):
        METADATA_WITHOUT_CURL = {k: v for k, v in METADATA.items() if k != "curl"}
        with mock.patch.dict(METADATA, clear=True), mock.patch.dict(METADATA, METADATA_WITHOUT_CURL):
            [o] = self.run_fix([self.CURL])
        self.assertEqual(o.status, fix.ERROR)
        self.assertIn("HTTP 404", self.stderr)



class BaseImageFixTest(FixRunCase):
    SSL = finding(IMAGE, "CVE-2026-0001", "3.0.2", source="oci", version=OLD_DIGEST)

    def bin(self, tool):
        return str(Path(os.environ["ADVISORIES_BIN"]) / tool)

    def test_moves_the_digest_to_the_tracked_tag_and_dates_it(self):
        written = []
        real = FakeRun.__call__

        def spy(fake, cmd, cwd):
            if cmd[:2] == ["git", "add"]:
                written.append((self.root / "MODULE.bazel").read_text())
            return real(fake, cmd, cwd)

        with mock.patch.object(FakeRun, "__call__", spy):
            [o] = self.run_fix([self.SSL])
        self.assertEqual((o.status, o.target, o.branch), (fix.OPENED, NEW_DIGEST, f"advisories/{IMAGE}"))
        self.assertEqual(self.fake.ran(self.bin("crane")), [[self.bin("crane"), "digest", f"{IMAGE}:latest"]])
        expected = MODULE.replace(f'digest = "{OLD_DIGEST}",  # captured by hand', f'digest = "{NEW_DIGEST}",  # {IMAGE}:latest on 2026-11-06')
        self.assertEqual(written, [expected])
        # The new digest is re-scanned once per platform, never by Bazel.
        scans = self.fake.ran(self.bin("grype"))
        self.assertEqual([(c[1], c[c.index("--platform") + 1]) for c in scans], [(f"registry:{IMAGE}@{NEW_DIGEST}", "linux/amd64"), (f"registry:{IMAGE}@{NEW_DIGEST}", "linux/arm64")])
        self.assertEqual(self.fake.ran("just"), [])
        [commit] = self.fake.ran("git", "commit")
        self.assertEqual(commit[4], f"fix(deps): raise {IMAGE} to {NEW_DIGEST} past CVE-2026-0001")
        self.assertIn(f"Resolved version: {NEW_DIGEST}\n", commit[6])

    def test_the_same_digest_needs_an_ignore(self):
        [o] = self.run_fix([self.SSL], digest=OLD_DIGEST)
        self.assertEqual((o.status, o.note, o.target), (fix.NEEDS_IGNORE, fix.NEEDS_IGNORE_NOTE, OLD_DIGEST))
        self.assertEqual(self.fake.ran("git", "checkout", "--quiet", "-B"), [])

    def test_a_new_digest_still_flagged_opens_no_pr(self):
        [o] = self.run_fix([self.SSL], vulnerable={(IMAGE, NEW_DIGEST)})
        self.assertEqual(o.status, fix.NEEDS_IGNORE)
        self.assertEqual(self.fake.ran("git", "commit"), [])

    def test_clearing_is_judged_over_the_advisories_with_a_fix(self):
        unfixed = finding(IMAGE, "CVE-2026-0002", None, source="oci", version=OLD_DIGEST)
        [o] = self.run_fix([self.SSL, unfixed])
        self.assertEqual((o.status, o.ids), (fix.OPENED, ("CVE-2026-0001",)))
        [o] = self.run_fix([unfixed])
        self.assertEqual((o.status, o.note), (fix.ALERT, "alert only: no released fix for any of its advisories"))
        self.assertEqual(self.fake.ran(self.bin("crane")), [])

    def test_a_malformed_digest_is_an_error(self):
        [o] = self.run_fix([self.SSL], digest="latest")
        self.assertEqual(o.status, fix.ERROR)
        self.assertIn("printed 'latest'", self.stderr)



class DebFixTest(FixRunCase):
    LIBC6 = finding("libc6", "CVE-2026-0001", "2.36-9+deb12u14", source="deb", version="2.36-9+deb12u13")
    LIBC_BIN = finding("libc-bin", "CVE-2026-0003", "2.36-9+deb12u14", source="deb", version="2.36-9+deb12u13")

    def test_one_snapshot_bump_fixes_every_flagged_package(self):
        written = []
        real = FakeRun.__call__

        def spy(fake, cmd, cwd):
            if cmd[:2] == ["git", "add"]:
                written.append((self.root / "bookworm.yaml").read_text())
            return real(fake, cmd, cwd)

        with mock.patch.object(FakeRun, "__call__", spy):
            outcomes = self.run_fix([self.LIBC6, self.LIBC_BIN])
        self.assertEqual([(o.component, o.source, o.status, o.branch, o.pr) for o in outcomes], [(c, "deb", fix.OPENED, "advisories/debian", "https://github.com/o/r/pull/99") for c in ("libc-bin", "libc6")])
        self.assertEqual(written, [DEB_MANIFEST.replace("20260101T000000Z", "20261106T000000Z")])
        self.assertEqual(self.fake.ran("just"), [["just", "bazel-distroless-lock"]])
        self.assertEqual(self.fake.ran("git", "add"), [["git", "add", "--", "bookworm.lock.json", "bookworm.yaml"]])
        [commit] = self.fake.ran("git", "commit")
        self.assertEqual(commit[4], "fix(deps): raise debian to 20261106T000000Z past CVE-2026-0001, CVE-2026-0003")
        self.assertIn("from 20260101T000000Z\nto 20261106T000000Z, the scan date", commit[6])
        [create] = self.fake.ran("gh", "pr", "create")
        self.assertEqual(create[6], "advisories/debian")

    def test_a_snapshot_at_the_scan_date_needs_an_ignore(self):
        [o] = self.run_fix([self.LIBC6], files={"bookworm.yaml": DEB_MANIFEST.replace("20260101T000000Z", "20261106T000000Z")})
        self.assertEqual((o.component, o.status, o.target), ("libc6", fix.NEEDS_IGNORE, "20261106T000000Z"))
        self.assertEqual(self.fake.ran("just"), [])

    def test_a_lock_the_snapshot_leaves_unchanged_needs_an_ignore(self):
        [o] = self.run_fix([self.LIBC6], repin_changes=False)
        self.assertEqual(o.status, fix.NEEDS_IGNORE)
        self.assertEqual(self.fake.ran("git", "commit"), [])

    def test_a_package_still_flagged_in_the_new_lock_opens_no_pr_and_another_does_not_count(self):
        [o] = self.run_fix([self.LIBC6], vulnerable={("libc6", "2.36-9+deb12u14")})
        self.assertEqual(o.status, fix.NEEDS_IGNORE)
        [o] = self.run_fix([self.LIBC6], vulnerable={("tini", "0.19.0-1+b3")})
        self.assertEqual(o.status, fix.OPENED)

    def test_an_open_pr_with_the_same_advisories_stays_on_its_older_snapshot(self):
        body = fix.trailer("20261101T000000Z", ("CVE-2026-0001",))
        pr = {"number": 7, "url": "https://example/pr/7", "body": body}
        [o] = self.run_fix([self.LIBC6], open_prs=[pr], remote_head="abc")
        self.assertEqual((o.status, o.pr), (fix.CURRENT, "https://example/pr/7"))
        outcomes = self.run_fix([self.LIBC6, self.LIBC_BIN], open_prs=[pr], remote_head="abc")
        self.assertEqual({o.status for o in outcomes}, {fix.UPDATED})

    def test_a_package_without_a_released_fix_is_not_covered_by_the_pr(self):
        tini = finding("tini", "CVE-2026-0004", None, source="deb", version="0.19.0-1+b3")
        outcomes = self.run_fix([self.LIBC6, self.LIBC_BIN, tini])
        # Each package's outcome names only its own advisories.
        self.assertEqual([(o.component, o.status, o.ids) for o in outcomes], [("libc-bin", fix.OPENED, ("CVE-2026-0003",)), ("libc6", fix.OPENED, ("CVE-2026-0001",)), ("tini", fix.ALERT, ("CVE-2026-0004",))])
        self.assertEqual(outcomes[2].note, "alert only: no released fix for CVE-2026-0004")

    def test_mixed_snapshot_timestamps_are_an_error(self):
        mixed = DEB_MANIFEST.replace("debian-security/20260101T000000Z", "debian-security/20260202T000000Z")
        [o] = self.run_fix([self.LIBC6], files={"bookworm.yaml": mixed})
        self.assertEqual(o.status, fix.ERROR)
        self.assertIn("expected one snapshot timestamp", self.stderr)



class HttpGetTest(unittest.TestCase):
    def get(self, *responses):
        def urlopen(url, timeout):
            r = responses[len(self.calls)]
            self.calls.append(url)
            if isinstance(r, Exception):
                raise r
            return contextlib.nullcontext(io.BytesIO(r))

        self.calls = []
        with mock.patch("urllib.request.urlopen", urlopen), mock.patch("time.sleep") as sleep:
            try:
                return fix.http_get("https://example/m.json")
            finally:
                self.slept = [c.args[0] for c in sleep.call_args_list]

    def http_error(self, code):
        return fix.urllib.error.HTTPError("https://example/m.json", code, "x", {}, None)

    def test_a_missing_page_is_not_retried(self):
        with self.assertRaisesRegex(ToolError, "HTTP 404"):
            self.get(self.http_error(404), b"{}")
        self.assertEqual(len(self.calls), 1)

    def test_server_errors_rate_limits_and_connection_errors_are_retried(self):
        self.assertEqual(self.get(self.http_error(503), self.http_error(429), b"{}"), b"{}")
        self.assertEqual(self.slept, [1, 2])
        with self.assertRaisesRegex(ToolError, "refused"):
            self.get(OSError("refused"), OSError("refused"), OSError("refused"))
        self.assertEqual(len(self.calls), 3)


if __name__ == "__main__":
    unittest.main()
