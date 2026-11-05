import contextlib
import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import inventory

FIXTURES = Path(__file__).parent / "fixtures"
TREE = FIXTURES / "tree"
LS_REMOTE = FIXTURES / "ls-remote"
REAL_LS_REMOTE = inventory.git_ls_remote


def recorded_ls_remote(repo):
    """Recorded `git ls-remote --tags` output; a repository without a recording is missing."""
    path = LS_REMOTE / (repo.replace("/", "__") + ".txt")
    if not path.exists():
        raise inventory.RepoMissing(repo)
    return path.read_text()


class InventoryTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tree = Path(tmp.name) / "tree"
        shutil.copytree(TREE, self.tree)
        self.out = Path(tmp.name) / "out.cdx.json"
        self.lookups = []

        def ls_remote(repo):
            self.lookups.append(repo)
            return recorded_ls_remote(repo)

        patcher = mock.patch.object(inventory, "git_ls_remote", ls_remote)
        patcher.start()
        self.addCleanup(patcher.stop)

    def run_inventory(self, *extra):
        err = io.StringIO()
        argv = ["--config", str(self.tree / "advisories.toml"), "--root", str(self.tree), "--output", str(self.out), *extra]
        with contextlib.redirect_stderr(err):
            code = inventory.main(argv)
        bom = json.loads(self.out.read_text()) if self.out.exists() else None
        return code, err.getvalue(), bom

    def edit(self, rel, old, new):
        path = self.tree / rel
        text = path.read_text()
        self.assertIn(old, text)
        path.write_text(text.replace(old, new, 1))

    def append(self, rel, text):
        with open(self.tree / rel, "a") as f:
            f.write(text)

    def component(self, bom, ref):
        return next(c for c in bom["components"] if c["bom-ref"] == ref)

    def assert_drift(self, needle):
        code, err, bom = self.run_inventory()
        self.assertEqual(code, 1, err)
        self.assertIn(needle, err)
        self.assertIsNone(bom)
        code, err, bom = self.run_inventory("--best-effort")
        self.assertEqual(code, 0, err)
        return bom

    # ── Golden ──

    def test_golden_cyclonedx(self):
        code, err, bom = self.run_inventory()
        self.assertEqual(code, 0, err)
        self.assertEqual(bom, json.loads((TREE / "expected.cdx.json").read_text()))

    def test_output_is_byte_stable(self):
        self.run_inventory()
        first = self.out.read_bytes()
        self.run_inventory()
        self.assertEqual(self.out.read_bytes(), first)

    # ── Sources ──

    def test_bcr_suffix_stripped(self):
        _, _, bom = self.run_inventory()
        curl = self.component(bom, "bcr:curl@8.21.0")
        self.assertEqual(curl["cpe"], "cpe:2.3:a:haxx:curl:8.21.0:*:*:*:*:*:*:*")

    def test_source_json_keys_over_module_bazel_candidates(self):
        _, _, bom = self.run_inventory()
        names = {c["bom-ref"] for c in bom["components"]}
        self.assertIn("bcr:zlib@1.3.2", names)
        self.assertFalse({"bcr:zlib@1.2.11", "bcr:zlib@1.3.1"} & names)
        # A module with only a MODULE.bazel key is no resolved module, so it needs no map entry.
        self.assertFalse(any("candidate_only" in n for n in names))

    def test_only_the_shipped_mimalloc_source_emitted(self):
        _, _, bom = self.run_inventory()
        mimalloc = [c for c in bom["components"] if c["name"] == "mimalloc"]
        self.assertEqual([c["bom-ref"] for c in mimalloc], ["http_archive:mimalloc@3.4.3"])

    def test_ffmpeg_blocks_and_commented_http_archive(self):
        _, _, bom = self.run_inventory()
        for arch in ("amd64", "arm64"):
            self.component(bom, f"http_archive:ffmpeg_static_linux_{arch}@8.1.1")
        self.assertFalse(any(c["name"] == "commented_out" for c in bom["components"]))

    def test_base_tree_reports_its_own_http_archive_version(self):
        self.edit("deps/module.star", "libtiff/archive/refs/tags/v4.7.2.tar.gz", "libtiff/archive/refs/tags/v4.6.0.tar.gz")
        code, err, bom = self.run_inventory()
        self.assertEqual(code, 0, err)
        self.component(bom, "http_archive:tiff@4.6.0")

    def test_workspace_members_left_out_of_crates(self):
        _, _, bom = self.run_inventory()
        crates = sorted(c["purl"] for c in bom["components"] if c.get("purl", "").startswith("pkg:cargo/"))
        self.assertEqual(crates, ["pkg:cargo/bytes@1.12.1", "pkg:cargo/serde@1.0.229"])

    def test_exact_deb_purl_with_derived_distro(self):
        _, _, bom = self.run_inventory()
        tini = self.component(bom, "pkg:deb/debian/tini@0.19.0-1%2Bb3?arch=arm64&distro=debian-12")
        self.assertEqual(tini["version"], "0.19.0-1+b3")

    def test_deb_purl_names_a_differing_source_package_as_upstream(self):
        _, _, bom = self.run_inventory()
        self.component(bom, "pkg:deb/debian/libc6@2.36-9%2Bdeb12u13?arch=amd64&distro=debian-12&upstream=glibc")

    def test_security_pool_url_names_its_source_package(self):
        # debian-security keeps its pool under pool/updates/<area>/.
        lock = json.loads((self.tree / "image/debs.lock.json").read_text())
        lock["packages"][0]["urls"] = ["https://snapshot.debian.org/archive/debian-security/20260101T000000Z/pool/updates/main/g/glibc/tini_0.19.0-1+b3_amd64.deb"]
        (self.tree / "image/debs.lock.json").write_text(json.dumps(lock))
        _, _, bom = self.run_inventory()
        self.assertTrue(any(c.get("purl", "").startswith("pkg:deb/debian/tini@") and c["purl"].endswith("&upstream=glibc") for c in bom["components"]))

    def test_deb_without_exactly_one_pool_source_is_a_tool_error(self):
        pool = "https://snapshot.debian.org/archive/debian/20260101T000000Z/pool/main"
        for urls in ([], ["https://example.invalid/tini.deb"], [f"{pool}/t/tini/tini_0.19.0-1+b3_amd64.deb", f"{pool}/g/glibc/tini_0.19.0-1+b3_amd64.deb"]):
            with self.subTest(urls=urls):
                lock = json.loads((TREE / "image/debs.lock.json").read_text())
                lock["packages"][0]["urls"] = urls
                (self.tree / "image/debs.lock.json").write_text(json.dumps(lock))
                code, err, _ = self.run_inventory()
                self.assertEqual(code, 3)
                self.assertIn("deb tini 0.19.0-1+b3: expected one source package", err)

    def test_deb_name_percent_encoded_in_purl(self):
        lock = json.loads((self.tree / "image/debs.lock.json").read_text())
        lock["packages"].append({**lock["packages"][0], "name": "libstdc++6", "key": "libstdc++6"})
        (self.tree / "image/debs.lock.json").write_text(json.dumps(lock))
        _, _, bom = self.run_inventory()
        self.assertTrue(any(c.get("purl", "").startswith("pkg:deb/debian/libstdc%2B%2B6@") for c in bom["components"]))

    def test_crate_from_another_source_is_a_tool_error(self):
        self.append("deps/crates.lock", '\n[[package]]\nname = "forked"\nversion = "0.1.0"\nsource = "git+https://example.invalid/forked#abc"\n')
        code, err, _ = self.run_inventory()
        self.assertEqual(code, 3)
        self.assertIn("not from crates.io", err)

    def test_shipped_entry_without_a_version_is_a_tool_error(self):
        self.edit("components.toml", 'name = "approvaltests_cpp"\nstatus = "not_shipped"\nreason = "test-only"', 'name = "approvaltests_cpp"\nstatus = "shipped"\ncpe = "cpe:2.3:a:approvals:approvaltests"')
        code, err, _ = self.run_inventory()
        self.assertEqual(code, 3)
        self.assertIn("carries no version", err)

    def test_failed_run_leaves_no_stale_output(self):
        self.out.write_text("stale")
        self.append("deps/module.star", '\nhttp_archive(\n    name = "newlib",\n    url = "https://example.invalid/newlib-1.0.tar.gz",\n)\n')
        code, _, bom = self.run_inventory()
        self.assertEqual(code, 1)
        self.assertIsNone(bom)

    def test_unknown_debian_codename_is_a_tool_error(self):
        self.edit("image/debs.yaml", "channel: bookworm main", "channel: hamm main")
        self.edit("image/debs.yaml", "channel: bookworm-security main", "channel: hamm-security main")
        code, err, _ = self.run_inventory()
        self.assertEqual(code, 3)
        self.assertIn("unknown Debian codename 'hamm'", err)

    def test_base_image_by_digest(self):
        _, _, bom = self.run_inventory()
        base = next(c for c in bom["components"] if c["type"] == "container")
        self.assertEqual(base["name"], "registry.example/images/base-debian12")
        self.assertEqual(base["version"], "sha256:" + "1" * 64)

    def test_missing_oci_pull_is_a_tool_error(self):
        self.edit("advisories.toml", 'oci_pull = "base"', 'oci_pull = "absent"')
        code, err, _ = self.run_inventory()
        self.assertEqual(code, 3)
        self.assertIn("absent", err)

    def test_no_source_json_key_is_a_tool_error(self):
        (self.tree / "deps/module.lock.json").write_text('{"registryFileHashes": {}}')
        code, err, _ = self.run_inventory()
        self.assertEqual(code, 3)
        self.assertIn("source.json", err)

    def test_every_input_path_from_config(self):
        moved = self.tree / "elsewhere"
        moved.mkdir()
        for rel in ("deps/crates.lock", "deps/module.star", "deps/module.lock.json", "deps/kakadu.bzl", "image/debs.yaml", "image/debs.lock.json"):
            (self.tree / rel).rename(moved / Path(rel).name)
        for rel in ("deps/crates.lock", "deps/module.star", "deps/module.lock.json", "deps/kakadu.bzl", "image/debs.yaml", "image/debs.lock.json"):
            self.edit("advisories.toml", f'"{rel}"', f'"elsewhere/{Path(rel).name}"')
        (self.tree / "ci").rename(moved / "ci")
        self.edit("advisories.toml", '["ci/', '["elsewhere/ci/')
        text = (self.tree / "advisories.toml").read_text()
        (self.tree / "advisories.toml").write_text(text.replace(', "ci/', ', "elsewhere/ci/'))
        code, err, bom = self.run_inventory()
        self.assertEqual(code, 0, err)
        self.assertEqual(bom, json.loads((TREE / "expected.cdx.json").read_text()))

    # ── eol ──

    def test_eol_carried_onto_components(self):
        _, _, bom = self.run_inventory()

        def eol(c):
            return {p["name"]: p["value"] for p in c["properties"]}.get("advisories:eol")

        self.assertEqual(eol(self.component(bom, "bcr:zlib@1.3.2")), "2030-01-01")
        self.assertEqual(eol(self.component(bom, "deb:debian@12")), "2026-07-12")
        self.assertEqual(eol(next(c for c in bom["components"] if c["type"] == "container")), "2026-09-10")
        # A config eol yields one component for its source, not one per package.
        self.assertIsNone(eol(self.component(bom, "pkg:deb/debian/tini@0.19.0-1%2Bb3?arch=amd64&distro=debian-12")))

    def test_quoted_eol_is_rejected(self):
        self.edit("components.toml", "eol = 2030-01-01", 'eol = "2030-01-01"')
        code, err, _ = self.run_inventory()
        self.assertEqual(code, 3)
        self.assertIn("bare TOML date", err)

    # ── GitHub Actions ──

    def action(self, bom, name, ref):
        return next(c for c in bom["components"] if c["name"] == name and ref in self.props(c).get("advisories:ref", "").split())

    def props(self, c):
        return {p["name"]: p["value"] for p in c["properties"]}

    def test_sha_pin_with_two_tags_resolves_to_the_most_specific(self):
        _, _, bom = self.run_inventory()
        checkout = self.action(bom, "actions/checkout", "d23441a48e516b6c34aea4fa41551a30e30af803")
        self.assertEqual(checkout["purl"], "pkg:githubactions/actions/checkout@6.1.0")
        self.assertEqual(self.props(checkout)["advisories:scope"], "ci")
        self.assertEqual(self.props(checkout)["syft:package:type"], "github-action")

    def test_lightweight_tag_ref_from_a_composite_action(self):
        # actions/checkout's v6 and v6.1.0 are lightweight tags; @v6 comes from an action.yaml.
        _, _, bom = self.run_inventory()
        checkout = self.action(bom, "actions/checkout", "v6")
        self.assertEqual(checkout["version"], "6.1.0")
        # Both refs land on one component: one purl, one bom-ref.
        self.assertEqual(self.props(checkout)["advisories:ref"], "d23441a48e516b6c34aea4fa41551a30e30af803 v6")

    def test_annotated_tag_ref_resolves_through_its_peeled_commit(self):
        _, _, bom = self.run_inventory()
        self.assertEqual(self.action(bom, "bazelbuild/setup-bazelisk", "v3")["version"], "3.0.0")

    def test_tag_ref_resolves_to_its_exact_release(self):
        _, _, bom = self.run_inventory()
        self.assertEqual(self.action(bom, "actions/cache", "v4")["version"], "4.3.0")

    def test_sub_path_action_maps_to_its_repository(self):
        _, _, bom = self.run_inventory()
        cache = self.action(bom, "actions/cache", "0057852bfaa89a56745cba8c7296529d2fc39830")
        self.assertEqual(cache["purl"], "pkg:githubactions/actions/cache@4.3.0")

    def test_job_level_reusable_workflow(self):
        _, _, bom = self.run_inventory()
        self.assertEqual(self.action(bom, "extractions/setup-just", "v3")["version"], "3.1.0")

    def test_branch_ref_is_unresolved(self):
        _, _, bom = self.run_inventory()
        crane = self.action(bom, "imjasonh/setup-crane", "main")
        self.assertNotIn("version", crane)
        self.assertEqual(self.props(crane)["advisories:unresolved"], "no release tag on this ref")

    def test_docker_ref_is_unresolved(self):
        _, _, bom = self.run_inventory()
        docker = next(c for c in bom["components"] if c["name"] == "docker://alpine:3.19")
        self.assertEqual(self.props(docker)["advisories:unresolved"], "docker:// image")

    def test_sha_without_tag_is_unresolved(self):
        _, _, bom = self.run_inventory()
        deploy = self.action(bom, "mhausenblas/mkdocs-deploy-gh-pages", "a31c6b13a80e4a4fbb525eeb7a2a78253bb15fa5")
        self.assertNotIn("purl", deploy)
        self.assertNotIn("syft:package:type", self.props(deploy))
        self.assertEqual(self.props(deploy)["advisories:unresolved"], "no release tag on this ref")

    def test_missing_repository_is_unresolved(self):
        _, _, bom = self.run_inventory()
        gone = self.action(bom, "example/gone", "v1")
        self.assertEqual(self.props(gone)["advisories:unresolved"], "repository missing or private")

    def test_local_actions_skipped(self):
        _, _, bom = self.run_inventory()
        self.assertFalse(any(c["name"].startswith("./") for c in bom["components"]))

    def test_each_repository_looked_up_once(self):
        self.run_inventory()
        self.assertEqual(sorted(self.lookups), sorted(set(self.lookups)))
        self.assertIn("actions/cache", self.lookups)

    def test_missing_repository_stderr_is_told_from_a_network_error(self):
        self.assertTrue(inventory.ls_remote_missing((LS_REMOTE / "missing-repository.stderr").read_text()))
        self.assertFalse(inventory.ls_remote_missing("fatal: unable to access 'https://github.com/a/b/': Could not resolve host: github.com"))

    def test_ls_remote_failing_after_retries_is_a_tool_error(self):
        failed = mock.Mock(returncode=128, stdout="", stderr="fatal: unable to access: Could not resolve host: github.com")
        with mock.patch.object(inventory.subprocess, "run", return_value=failed) as run, mock.patch.object(inventory.time, "sleep"):
            with self.assertRaises(inventory.ToolError):
                REAL_LS_REMOTE("a/b")
        self.assertEqual(run.call_count, inventory.LS_REMOTE_ATTEMPTS)
        self.assertEqual(run.call_args.kwargs["env"]["GIT_TERMINAL_PROMPT"], "0")
        self.assertEqual(run.call_args.kwargs["env"]["LC_ALL"], "C")

    def test_missing_repository_is_not_retried(self):
        missing = mock.Mock(returncode=128, stdout="", stderr=(LS_REMOTE / "missing-repository.stderr").read_text())
        with mock.patch.object(inventory.subprocess, "run", return_value=missing) as run:
            with self.assertRaises(inventory.RepoMissing):
                REAL_LS_REMOTE("a/b")
        self.assertEqual(run.call_count, 1)

    # ── Drift ──

    def test_drift_unclassified_bcr_module(self):
        self.edit("deps/module.lock.json", '"https://bcr.bazel.build/bazel_registry.json"', '"https://bcr.bazel.build/modules/newdep/1.0/source.json": "0000",\n    "https://bcr.bazel.build/bazel_registry.json"')
        self.assert_drift("unclassified bcr 'newdep'")
        _, err, _ = self.run_inventory()
        self.assertIn("add a [[component]] to components.toml", err)

    def test_drift_unclassified_http_archive(self):
        self.append("deps/module.star", '\nhttp_archive(\n    name = "newlib",\n    url = "https://example.invalid/newlib-1.0.tar.gz",\n)\n')
        bom = self.assert_drift("unclassified http_archive 'newlib' (deps/module.star:")
        self.assertFalse(any(c["name"] == "newlib" for c in bom["components"]))

    def test_drift_unclassified_git_override(self):
        self.append("deps/module.star", '\ngit_override(\n    module_name = "newfork",\n    commit = "0000",\n    remote = "https://example.invalid/newfork.git",\n)\n')
        self.assert_drift("unclassified git_override 'newfork'")

    def test_drift_unclassified_custom_rule(self):
        self.append("deps/module.star", '\nnew_ext = use_extension("//bazel:new_ext.bzl", "new_ext")\n')
        self.assert_drift("unclassified custom_rule 'new_ext'")

    def test_drift_url_not_matching_version_re(self):
        self.edit("deps/module.star", "libtiff/archive/refs/tags/v4.7.2.tar.gz", "libtiff/archive/refs/heads/master.zip")
        bom = self.assert_drift("http_archive 'tiff': version_re")
        self.assertFalse(any(c["name"] == "tiff" for c in bom["components"]))

    def test_drift_stale_entry(self):
        self.append("components.toml", '\n[[component]]\nsource = "http_archive"\nname = "removed"\nstatus = "not_shipped"\nreason = "gone"\n')
        self.assert_drift("stale component map entry http_archive 'removed'")

    def test_drift_duplicate_entry(self):
        self.append("components.toml", '\n[[component]]\nsource = "bcr"\nname = "curl"\nstatus = "not_shipped"\nreason = "again"\n')
        bom = self.assert_drift("duplicate component map entry bcr 'curl'")
        self.assertEqual(len([c for c in bom["components"] if c["name"] == "curl"]), 1)


if __name__ == "__main__":
    unittest.main()
