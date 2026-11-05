#!/usr/bin/env python3
"""Emit the CycloneDX inventory of what a tree ships, from the files Bazel resolves against.

Usage:
    inventory.py --config advisories.toml --output advisories.cdx.json [--root DIR] [--best-effort]

Every input path comes from the config and is read relative to `--root`, so the
head's tools and component map can inventory a base worktree. The drift check
fails (exit 1) on a dependency the component map does not classify, an
`http_archive` URL its `version_re` does not match, a map entry for a dependency
the tree no longer has, and a duplicate map entry. `--best-effort` skips every
drift class instead, for a base tree that predates the map.

GitHub Actions `uses:` refs in the configured workflow globs become
`pkg:githubactions` components, a SHA or tag resolved through
`git ls-remote --tags` to the most specific semver release tag pointing at the
same commit. A branch ref, a SHA no release tag points at, a `docker://` ref and
a missing or private repository are listed as unresolved, not failed.

Exit codes: 0 inventory written, 1 drift, 2 usage, 3 unreadable or unexpected
input, or `git ls-remote` failing after its retries.
"""

import _model  # noqa: F401  (Python version check, before tomllib)

import argparse
import ast
import datetime
import json
import os
import re
import subprocess
import sys
import time
import tomllib
import urllib.parse
from dataclasses import dataclass, replace
from pathlib import Path

from _model import EXIT_CLEAN, EXIT_ERROR, EXIT_FINDINGS, Component, ToolError, cyclonedx

SOURCES = ("bcr", "http_archive", "git_override", "custom_rule")
STATUSES = ("shipped", "not_shipped", "no_feed")
DEBIAN_RELEASES = {"buster": "10", "bullseye": "11", "bookworm": "12", "trixie": "13", "forky": "14"}

# ── MODULE.bazel ──────────────────────────────────────────────────────────────
# A tokenizer rather than line regexes: a `build_file_content` string can hold a
# column-0 `)` before the call's own `url`, and `http_archive(` also appears in
# comments. Only top-level calls with string or string-list arguments are read.

_TOKEN = re.compile(
    r"""
    (?P<comment>\#[^\n]*)
    | (?P<string>\"\"\"(?:\\.|[^\\])*?\"\"\"|'''(?:\\.|[^\\])*?'''|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')
    | (?P<name>[A-Za-z_][A-Za-z0-9_.]*)
    | (?P<op>[()\[\]{},=])
    | (?P<space>\s+)
    | (?P<other>.)
    """,
    re.VERBOSE | re.DOTALL,
)


@dataclass
class Call:
    callee: str
    args: list
    kwargs: dict
    line: int


def _value(tokens: list) -> str | list | None:
    """A string literal, a list of string literals, or None for anything else."""
    if len(tokens) == 1 and tokens[0][0] == "string":
        return ast.literal_eval(tokens[0][1])
    if len(tokens) >= 2 and tokens[0][1] == "[" and tokens[-1][1] == "]":
        inner = [t for t in tokens[1:-1] if t[1] != ","]
        if all(t[0] == "string" for t in inner):
            return [ast.literal_eval(t[1]) for t in inner]
    return None


def starlark_calls(text: str) -> list[Call]:
    toks = [(m.lastgroup, m.group(), m.start()) for m in _TOKEN.finditer(text) if m.lastgroup not in ("comment", "space")]
    calls, depth, i = [], 0, 0
    while i < len(toks):
        kind, val, pos = toks[i]
        if depth == 0 and kind == "name" and i + 1 < len(toks) and toks[i + 1][:2] == ("op", "("):
            groups, cur, d, j = [], [], 1, i + 2
            while j < len(toks) and d > 0:
                t = toks[j]
                if t[0] == "op" and t[1] in "([{":
                    d += 1
                elif t[0] == "op" and t[1] in ")]}":
                    d -= 1
                if d == 0 or (d == 1 and t[:2] == ("op", ",")):
                    if cur:
                        groups.append(cur)
                    cur = []
                else:
                    cur.append(t)
                j += 1
            args, kwargs = [], {}
            for g in groups:
                if len(g) >= 2 and g[0][0] == "name" and g[1][:2] == ("op", "="):
                    kwargs[g[0][1]] = _value(g[2:])
                else:
                    args.append(_value(g))
            calls.append(Call(val, args, kwargs, text.count("\n", 0, pos) + 1))
            i = j
            continue
        if kind == "op" and val in "([{":
            depth += 1
        elif kind == "op" and val in ")]}":
            depth -= 1
        i += 1
    return calls


# ── Inputs ────────────────────────────────────────────────────────────────────

_SOURCE_JSON = re.compile(r"/modules/([^/]+)/([^/]+)/source\.json$")
_BCR_SUFFIX = re.compile(r"\.bcr\.\d+$")


def bcr_modules(lock: dict) -> dict[str, str]:
    """Resolved BCR modules, name -> upstream version.

    Only `source.json` keys name the resolved set; `MODULE.bazel` keys also
    cover the candidates MVS rejected.
    """
    found = {}
    for key in lock.get("registryFileHashes", {}):
        m = _SOURCE_JSON.search(key)
        if m:
            found[m.group(1)] = _BCR_SUFFIX.sub("", m.group(2))
    if not found:
        raise ToolError("no `…/modules/<name>/<version>/source.json` key in the module lockfile")
    return found


CRATES_IO = "registry+https://github.com/rust-lang/crates.io-index"


def crates(lock: dict) -> list[Component]:
    # Packages without a `source` are the lockfile's own workspace members.
    out = []
    for p in lock.get("package", []):
        if "source" not in p:
            continue
        if p["source"] != CRATES_IO:
            raise ToolError(f"crate {p['name']} {p['version']} is not from crates.io ({p['source']}); its pkg:cargo purl would name another package")
        out.append(Component(name=p["name"], version=p["version"], source="cargo", purl=f"pkg:cargo/{p['name']}@{p['version']}"))
    return out


def debian_release(manifest: str) -> str:
    codenames = {m.group(1).split("-")[0] for m in re.finditer(r"^\s*-?\s*channel:\s*[\"']?([a-z-]+)", manifest, re.M)}
    if len(codenames) != 1:
        raise ToolError(f"expected one Debian codename in the deb manifest channels, found {sorted(codenames)}")
    codename = codenames.pop()
    if codename not in DEBIAN_RELEASES:
        raise ToolError(f"unknown Debian codename {codename!r}")
    return DEBIAN_RELEASES[codename]


_POOL = re.compile(r"/pool/(?:updates/)?[^/]+/[^/]+/([^/]+)/[^/]+\.deb$")


def debs(lock: dict, release: str) -> list[Component]:
    # Debian advisories name the source package (glibc), not the binary (libc6);
    # Grype matches a binary through the `upstream` qualifier, ADR-0029.
    out = []
    for p in lock["packages"]:
        sources = {m.group(1) for u in p.get("urls", []) if (m := _POOL.search(u))}
        if len(sources) != 1:
            raise ToolError(f"deb {p['name']} {p['version']}: expected one source package in its pool URLs, found {sorted(sources)}")
        source = sources.pop()
        name = urllib.parse.quote(p["name"], safe=".-_~")
        version = urllib.parse.quote(p["version"], safe=".-_~:")
        purl = f"pkg:deb/debian/{name}@{version}?arch={p['arch']}&distro=debian-{release}"
        if source != p["name"]:
            purl += f"&upstream={urllib.parse.quote(source, safe='.-_~')}"
        out.append(Component(name=p["name"], version=p["version"], source="deb", purl=purl))
    return out


# ── Component map ─────────────────────────────────────────────────────────────


def load_map(path: Path) -> tuple[list[dict], list[str]]:
    entries, drift, seen = [], [], set()
    for e in tomllib.loads(path.read_text()).get("component", []):
        key = (e.get("source"), e.get("name"))
        if e.get("source") not in SOURCES or e.get("status") not in STATUSES or not e.get("name"):
            raise ToolError(f"component map entry {key}: needs a name, source in {SOURCES} and status in {STATUSES}")
        cpes = e.get("cpe")
        if isinstance(cpes, str):
            e["cpe"] = cpes = [cpes]  # `build` reads `cpe` as a list
        if e["status"] == "shipped" and not (isinstance(cpes, list) and cpes and all(isinstance(c, str) and c for c in cpes)):
            raise ToolError(f"component map entry {key}: shipped needs a cpe string or a non-empty list of them")
        if e["status"] == "shipped" and e["source"] == "http_archive" and not e.get("version_re"):
            raise ToolError(f"component map entry {key}: shipped http_archive without version_re")
        if e["status"] != "shipped" and not e.get("reason"):
            raise ToolError(f"component map entry {key}: {e['status']} without reason")
        if key in seen:
            drift.append(f"duplicate component map entry {key[0]} {key[1]!r}")
            continue
        seen.add(key)
        entries.append(e)
    return entries, drift


def _eol(table: dict) -> dict[str, str]:
    if "eol" not in table:
        return {}
    if type(table["eol"]) is not datetime.date:
        raise ToolError(f"eol {table['eol']!r} must be a bare TOML date")
    return {"advisories:eol": table["eol"].isoformat()}


# ── GitHub Actions ────────────────────────────────────────────────────────────

_USES = re.compile(r"^\s*(?:-\s+)?uses:\s*[\"']?([^\s\"'#]+)", re.M)
_RELEASE_TAG = re.compile(r"^v?(\d+)(?:\.(\d+))?(?:\.(\d+))?$")
_SHA = re.compile(r"^[0-9a-f]{40}$")
LS_REMOTE_ATTEMPTS = 3


class RepoMissing(Exception):
    pass


def ls_remote_missing(stderr: str) -> bool:
    """Whether a failed `git ls-remote` means the repository is gone or private, not a network error."""
    s = stderr.lower()
    return "not found" in s or "could not read username" in s or "terminal prompts disabled" in s


def git_ls_remote(repo: str) -> str:
    url = f"https://github.com/{repo}"
    # LC_ALL=C keeps git's messages in English for `ls_remote_missing`.
    env = {**os.environ, "GIT_TERMINAL_PROMPT": "0", "LC_ALL": "C"}
    error = ""
    for attempt in range(LS_REMOTE_ATTEMPTS):
        if attempt:
            time.sleep(2**attempt)
        try:
            r = subprocess.run(["git", "ls-remote", "--tags", url], capture_output=True, text=True, env=env, timeout=60)
        except subprocess.TimeoutExpired:
            error = "timed out"
            continue
        if r.returncode == 0:
            return r.stdout
        if ls_remote_missing(r.stderr):
            raise RepoMissing(repo)
        error = r.stderr.strip()
    raise ToolError(f"git ls-remote {url} failed {LS_REMOTE_ATTEMPTS} times: {error}")


def release_tag(ls_remote: str, ref: str) -> str | None:
    """The most specific release tag (no prerelease) on the commit `ref` names, without its `v`."""
    commits: dict[str, str] = {}
    for line in ls_remote.splitlines():
        sha, _, name = line.partition("\t")
        if not name.startswith("refs/tags/"):
            continue
        tag = name.removeprefix("refs/tags/")
        if tag.endswith("^{}"):
            commits[tag[:-3]] = sha  # an annotated tag's commit is its peeled line
        else:
            commits.setdefault(tag, sha)
    commit = ref if _SHA.match(ref) else commits.get(ref)
    if commit is None:
        return None
    candidates = [m for t, c in commits.items() if c == commit and (m := _RELEASE_TAG.match(t))]
    if not candidates:
        return None
    best = max(candidates, key=lambda m: (sum(g is not None for g in m.groups()), [int(g or 0) for g in m.groups()]))
    return best.group(0).removeprefix("v")


def actions(root: Path, globs: list[str], ls_remote) -> list[Component]:
    uses = set()
    for pattern in globs:
        for path in root.glob(pattern):
            uses.update(m.group(1) for m in _USES.finditer(path.read_text()))
    outputs: dict[str, str | None] = {}
    merged: dict[str, tuple[Component, set[str]]] = {}
    for u in sorted(uses):
        if u.startswith("./"):
            continue
        ci = {"advisories:scope": "ci"}
        if u.startswith("docker://"):
            c = Component(name=u, version=None, source="githubactions", properties={**ci, "advisories:unresolved": "docker:// image"})
        else:
            spec, _, ref = u.partition("@")
            repo = "/".join(spec.split("/")[:2])
            if repo not in outputs:
                try:
                    outputs[repo] = ls_remote(repo)
                except RepoMissing:
                    outputs[repo] = None
            props = {**ci, "advisories:ref": ref}
            version = release_tag(outputs[repo], ref) if outputs[repo] is not None else None
            if version:
                # Grype does not map the `githubactions` purl type to its Actions matcher; the Syft type property does, ADR-0029.
                props["syft:package:type"] = "github-action"
                c = Component(name=repo, version=version, source="githubactions", purl=f"pkg:githubactions/{repo}@{version}", properties=props)
            else:
                reason = "repository missing or private" if outputs[repo] is None else "no release tag on this ref"
                c = Component(name=repo, version=None, source="githubactions", properties={**props, "advisories:unresolved": reason})
        merged.setdefault(c.bom_ref, (c, set()))[1].add(c.properties.get("advisories:ref"))
    # Refs resolving to one release share its purl, so they become one component.
    return [
        replace(c, properties={**c.properties, "advisories:ref": " ".join(sorted(refs))}) if "advisories:ref" in c.properties else c
        for c, refs in merged.values()
    ]


# ── Assembly ──────────────────────────────────────────────────────────────────


def build(config_path: Path, root: Path) -> tuple[list[Component], list[str]]:
    """The tree's components and its drift findings; a component a drift finding concerns is left out."""
    config = tomllib.loads(config_path.read_text())
    inputs = config["inputs"]
    entries, drift = load_map(config_path.parent / config["component_map"])

    module_bazel = root / inputs["module_bazel"]
    calls = starlark_calls(module_bazel.read_text())
    found: dict[tuple[str, str], dict] = {}
    for c in calls:
        if c.callee == "http_archive" and isinstance(c.kwargs.get("name"), str):
            url = c.kwargs.get("url") or (c.kwargs.get("urls") or [None])[0]
            found[("http_archive", c.kwargs["name"])] = {"url": url, "line": c.line}
        elif c.callee == "git_override" and isinstance(c.kwargs.get("module_name"), str):
            found[("git_override", c.kwargs["module_name"])] = {"line": c.line}
        elif c.callee in ("use_extension", "use_repo_rule") and len(c.args) >= 2:
            label, symbol = c.args[0], c.args[1]
            if isinstance(label, str) and isinstance(symbol, str) and label.startswith(("//", ":", "@//")):
                found[("custom_rule", symbol)] = {"line": c.line}
    for name, version in bcr_modules(json.loads((root / inputs["module_lock"]).read_text())).items():
        found[("bcr", name)] = {"version": version}

    components: list[Component] = []
    classified = set()
    for e in entries:
        key = (e["source"], e["name"])
        classified.add(key)
        dep = found.get(key)
        if dep is None:
            drift.append(f"stale component map entry {key[0]} {key[1]!r}: not in {inputs['module_bazel']} or {inputs['module_lock']}")
            continue
        if e["status"] == "not_shipped":
            continue
        version = dep.get("version")
        if e.get("version_re"):
            if key[0] == "http_archive":
                text, where = dep["url"] or "", "URL"
            else:
                rel = inputs.get("version_files", {}).get(key[1])
                if rel is None:
                    raise ToolError(f"component map entry {key}: version_re but no inputs.version_files.{key[1]}")
                text, where = (root / rel).read_text(), rel
            m = re.search(e["version_re"], text)
            if not m:
                drift.append(f"{key[0]} {key[1]!r}: version_re {e['version_re']!r} does not match its {where}")
                continue
            version = m.group(1)
        if version is None and e["status"] == "shipped":
            raise ToolError(f"component map entry {key}: shipped, but its source carries no version; add version_re")
        props = _eol(e)
        cpes = []
        if e["status"] == "no_feed":
            props["advisories:no_feed"] = e["reason"]
        else:
            cpes = [f"{c}:{version}:*:*:*:*:*:*:*" for c in e["cpe"]]
        components.append(Component(name=key[1], version=version, source=key[0], cpe=cpes[0] if cpes else None, extra_cpes=tuple(cpes[1:]), properties=props))

    for key in sorted(set(found) - classified):
        where = f" ({inputs['module_bazel']}:{found[key]['line']})" if "line" in found[key] else ""
        drift.append(f"unclassified {key[0]} {key[1]!r}{where}: add a [[component]] to {config['component_map']} as shipped (cpe; version_re for http_archive), not_shipped or no_feed (reason)")

    components += crates(tomllib.loads((root / inputs["cargo_lock"]).read_text()))

    deb = config["deb"]
    release = debian_release((root / deb["manifest"]).read_text())
    components += debs(json.loads((root / deb["lock"]).read_text()), release)
    components.append(Component(name="debian", version=release, source="deb", type="operating-system", properties=_eol(deb)))

    base = config["base_image"]
    pulls = [c for c in calls if c.callee.endswith(".pull") and c.kwargs.get("name") == base["oci_pull"]]
    if len(pulls) != 1 or not isinstance(pulls[0].kwargs.get("image"), str) or not isinstance(pulls[0].kwargs.get("digest"), str):
        raise ToolError(f"expected one `.pull(name = {base['oci_pull']!r}, image = …, digest = …)` in {inputs['module_bazel']}")
    image, digest = pulls[0].kwargs["image"], pulls[0].kwargs["digest"]
    components.append(
        Component(
            name=image,
            version=digest,
            source="oci",
            type="container",
            purl=f"pkg:oci/{image.rsplit('/', 1)[-1]}@{urllib.parse.quote(digest)}?repository_url={image}",
            properties={"advisories:tracks": base["tracks"], **_eol(base)},
        )
    )
    components += actions(root, inputs["workflows"], git_ls_remote)
    return components, sorted(drift)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", required=True, type=Path, help="advisories.toml")
    parser.add_argument("--output", required=True, type=Path, help="CycloneDX JSON to write")
    parser.add_argument("--root", default=Path("."), type=Path, help="tree to inventory (default: .)")
    parser.add_argument("--best-effort", action="store_true", help="skip every drift class instead of failing")
    args = parser.parse_args(argv)
    args.output.unlink(missing_ok=True)
    try:
        components, drift = build(args.config, args.root)
        if drift and not args.best_effort:
            for d in drift:
                print(f"inventory: drift: {d}", file=sys.stderr)
            return EXIT_FINDINGS
        args.output.write_text(json.dumps(cyclonedx(components), indent=2) + "\n")
    except Exception as e:  # noqa: BLE001  (a crash must never read as findings)
        print(f"inventory: error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    return EXIT_CLEAN


if __name__ == "__main__":
    sys.exit(main())
