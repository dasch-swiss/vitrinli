#!/usr/bin/env python3
"""Scan the CycloneDX inventory for advisories, then apply the ignore file.

Usage:
    scan.py --config advisories.toml --inventory advisories.cdx.json --output findings.json

Rust crates go to OSV-Scanner; every other component with a purl or CPE goes
to Grype, and the base image digest to Grype once per configured platform
(ADR-0029). Both run with JSON output, without `--fail-on` and without an
ignore file of their own; this tool reads each one's exit code and decides
pass or fail. A component whose `advisories:eol` date has come gets an `EOL`
finding. Findings of one component that share an id or alias are one finding.

The `ignores` file the config names is linted on load. An entry suppresses its
component's finding whose id or an alias it names, through its `expiry` date.

Scanner binaries come from `$ADVISORIES_BIN` when it is set, else from PATH.

Exit codes: 0 no unignored finding, 1 findings, 2 usage, 3 tool or scanner
error, including an invalid ignore file and an SBOM for Grype without
components.
"""

import _model  # noqa: F401  (Python version check, before tomllib)

import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
from dataclasses import dataclass, replace
from pathlib import Path

from _model import EXIT_CLEAN, EXIT_ERROR, EXIT_FINDINGS, Finding, ToolError

SEVERITIES = ("unknown", "negligible", "low", "medium", "high", "critical")
_ID_PREFERENCE = ("RUSTSEC-", "GHSA-", "CVE-")


# ── Scanners ──────────────────────────────────────────────────────────────────


def run_process(cmd: list[str], cwd: Path) -> tuple[int, str, str]:
    env = {**os.environ, "GRYPE_CHECK_FOR_APP_UPDATE": "false"}
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=env)
    return r.returncode, r.stdout, r.stderr


def scanner(name: str) -> str:
    bin_dir = os.environ.get("ADVISORIES_BIN")
    path = str(Path(bin_dir) / name) if bin_dir else shutil.which(name)
    if not path or not os.access(path, os.X_OK):
        raise ToolError(f"{name} not found {'in $ADVISORIES_BIN' if bin_dir else 'on PATH'}")
    return path


def _json_output(name: str, code: int, out: str, err: str) -> dict:
    try:
        return json.loads(out)
    except json.JSONDecodeError as e:
        raise ToolError(f"{name} exited {code} without JSON output: {err.strip()[-500:]}") from e


def osv_scan(sbom: Path, run) -> dict:
    # The file name must end in .cdx.json, or OSV-Scanner exits 127. An empty
    # config keeps an osv-scanner.toml next to the input from applying ignores.
    config = sbom.parent / "osv-scanner.toml"
    config.write_text("")
    code, out, err = run([scanner("osv-scanner"), "scan", "source", "--config", str(config), "-L", str(sbom), "--format", "json"], sbom.parent)
    if code not in (0, 1):
        raise ToolError(f"osv-scanner exited {code}: {err.strip()[-500:]}")
    return _json_output("osv-scanner", code, out, err)


def grype_scan(target: str, workdir: Path, run, platform: str | None = None) -> dict:
    config = workdir / "grype.yaml"
    config.write_text("{}\n")
    cmd = [scanner("grype"), target, "--config", str(config), "-o", "json"]
    if platform:
        cmd += ["--platform", platform]
    code, out, err = run(cmd, workdir)
    if code != 0:
        raise ToolError(f"grype {target} exited {code}: {err.strip()[-500:]}")
    return _json_output("grype", code, out, err)


# ── Normalization ─────────────────────────────────────────────────────────────


def _version_key(v: str) -> tuple[int, ...]:
    return tuple(int(n) for n in re.findall(r"\d+", v))


def lowest_fix_above(current: str | None, candidates: list[str]) -> str | None:
    above = [c for c in candidates if current is None or _version_key(c) > _version_key(current)]
    return min(above, key=_version_key) if above else None


def _cvss_severity(score: str) -> str:
    try:
        s = float(score)
    except ValueError:
        return "unknown"
    return "critical" if s >= 9 else "high" if s >= 7 else "medium" if s >= 4 else "low" if s > 0 else "unknown"


def _properties(component: dict) -> dict[str, str]:
    return {p["name"]: p["value"] for p in component.get("properties", [])}


def normalize_osv(doc: dict) -> list[Finding]:
    findings = []
    for result in doc.get("results") or []:
        for pkg in result.get("packages") or []:
            name, version = pkg["package"]["name"], pkg["package"].get("version")
            vulns = {v["id"]: v for v in pkg.get("vulnerabilities") or []}
            for group in pkg.get("groups") or []:
                severity = _cvss_severity(group.get("max_severity") or "")
                fixes = []
                for vid in group["ids"]:
                    v = vulns.get(vid, {})
                    if severity == "unknown":
                        # GHSA's database severity, when the group carries no CVSS score.
                        ghsa = (v.get("database_specific") or {}).get("severity", "").lower()
                        severity = "medium" if ghsa == "moderate" else ghsa or "unknown"
                    for affected in v.get("affected") or []:
                        if affected.get("package", {}).get("name") == name:
                            fixes += [e["fixed"] for r in affected.get("ranges") or [] for e in r.get("events") or [] if "fixed" in e]
                ids = sorted({*group["ids"], *(group.get("aliases") or [])})
                findings.append(
                    Finding(
                        component=name,
                        version=version,
                        source="cargo",
                        id=group["ids"][0],
                        aliases=tuple(i for i in ids if i != group["ids"][0]),
                        severity=severity if severity in SEVERITIES else "unknown",
                        fixed=lowest_fix_above(version, fixes),
                    )
                )
    return findings


def normalize_grype(doc: dict, sources: dict[tuple[str, str | None], str], image: tuple[str, str] | None = None) -> list[Finding]:
    """Grype matches as findings; `image` is the base image (name, digest) whose packages they concern."""
    findings = []
    for m in doc.get("matches") or []:
        v, a = m["vulnerability"], m["artifact"]
        fix = v.get("fix") or {}
        fixed = lowest_fix_above(a.get("version"), fix.get("versions") or []) if fix.get("state") == "fixed" else None
        common = dict(
            id=v["id"],
            aliases=tuple(sorted({r["id"] for r in m.get("relatedVulnerabilities") or []} - {v["id"]})),
            severity=s if (s := (v.get("severity") or "").lower()) in SEVERITIES else "unknown",
            fixed=fixed,
        )
        if image:
            findings.append(Finding(component=image[0], version=image[1], source="oci", packages=(f"{a['name']} {a.get('version')}",), **common))
        else:
            source = sources.get((a["name"], a.get("version")))
            if source is None:
                raise ToolError(f"grype reported {a['name']} {a.get('version')}, which is not in the SBOM it was given")
            findings.append(Finding(component=a["name"], version=a.get("version"), source=source, **common))
    return findings


def eol_findings(components: list[dict], today: datetime.date) -> list[Finding]:
    out = []
    for c in components:
        props = _properties(c)
        if "advisories:eol" in props and today >= datetime.date.fromisoformat(props["advisories:eol"]):
            out.append(Finding(component=c["name"], version=c.get("version"), source=props["advisories:source"], id="EOL"))
    return out


def _preferred(ids: set[str]) -> str:
    return min(ids, key=lambda i: (next((n for n, p in enumerate(_ID_PREFERENCE) if i.startswith(p)), len(_ID_PREFERENCE)), i))


def merge(findings: list[Finding]) -> list[Finding]:
    """One finding per component and advisory: findings sharing an id or alias merge (both arches, several CPEs)."""
    # A finding joins every group it shares a name with, so groups it bridges become one.
    by_component: dict[str, list[tuple[set[str], list[Finding]]]] = {}
    for f in findings:
        names, members = {f.id, *f.aliases}, [f]
        kept = []
        for group_names, group_members in by_component.get(f.component, []):
            if group_names & names:
                names |= group_names
                members += group_members
            else:
                kept.append((group_names, group_members))
        by_component[f.component] = [*kept, (names, members)]
    out = []
    for names, fs in (g for groups in by_component.values() for g in groups):
        primary = _preferred(names)
        fixes = [f.fixed for f in fs if f.fixed]
        out.append(
            replace(
                fs[0],
                id=primary,
                aliases=tuple(sorted(names - {primary})),
                severity=max((f.severity for f in fs), key=SEVERITIES.index),
                fixed=min(fixes, key=_version_key) if fixes else None,
                packages=tuple(sorted({p for f in fs for p in f.packages})),
            )
        )
    return sorted(out, key=lambda f: f.key)


# ── Ignores ───────────────────────────────────────────────────────────────────

IGNORE_KEYS = {"id", "component", "reason", "expiry"}


@dataclass(frozen=True)
class Ignore:
    id: str
    component: str
    reason: str
    expiry: datetime.date


def load_ignores(text: str) -> list[Ignore]:
    try:
        doc = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ToolError(f"ignore file: {e}") from e
    errors = [f"unknown top-level key {k!r}" for k in doc if k != "ignore"]
    out = []
    for n, e in enumerate(doc.get("ignore", []), 1):
        where = f"[[ignore]] #{n} ({e.get('id', '?')})"
        errors += [f"{where}: unknown key {k!r}" for k in sorted(set(e) - IGNORE_KEYS)]
        errors += [f"{where}: missing {k!r}" for k in sorted(IGNORE_KEYS - set(e))]
        errors += [f"{where}: {k} must be a non-empty string" for k in ("id", "component", "reason") if k in e and not (isinstance(e[k], str) and e[k].strip())]
        # A quoted date is a string, and comparing it with a date would crash.
        if "expiry" in e and type(e["expiry"]) is not datetime.date:
            errors.append(f"{where}: expiry {e['expiry']!r} must be a bare TOML date (expiry = 2027-01-31)")
        if not errors:
            out.append(Ignore(**{k: e[k] for k in IGNORE_KEYS}))
    if errors:
        raise ToolError("ignore file: " + "; ".join(errors))
    return out


def matching_ignore(f: Finding, ignores: list[Ignore], today: datetime.date) -> Ignore | None:
    names = {f.id, *f.aliases}
    return next((i for i in ignores if i.component == f.component and i.id in names and today <= i.expiry), None)


# ── Assembly ──────────────────────────────────────────────────────────────────


@dataclass
class Result:
    findings: list[Finding]
    ignored: dict[tuple[str, str], Ignore]
    grype_db_built: str | None

    @property
    def open(self) -> list[Finding]:
        return [f for f in self.findings if f.key not in self.ignored]


def findings(inventory: dict, platforms: list[str], today: datetime.date, run=None) -> tuple[list[Finding], str | None]:
    """Every finding of the inventory before ignores, and the Grype database build time."""
    run = run or run_process
    components = inventory.get("components", [])
    cargo = [c for c in components if c.get("purl", "").startswith("pkg:cargo/")]
    images = [c for c in components if c.get("type") == "container"]
    rest = [c for c in components if c not in cargo and c.get("type") != "container" and (c.get("purl") or c.get("cpe"))]
    if len(images) != 1:
        raise ToolError(f"expected one container component (the base image) in the inventory, found {len(images)}")
    if not rest:
        # Grype exits 0 on an SBOM without components, which would read as clean.
        raise ToolError("no component for Grype in the inventory")
    if not platforms:
        raise ToolError("no base_image.platforms in the config")

    out = eol_findings(components, today)
    with tempfile.TemporaryDirectory(prefix="advisories-scan-") as tmp:
        work = Path(tmp)
        for name, part in (("cargo.cdx.json", cargo), ("grype.cdx.json", rest)):
            (work / name).write_text(json.dumps({**inventory, "components": part}))
        out += normalize_osv(osv_scan(work / "cargo.cdx.json", run))
        doc = grype_scan(f"sbom:{work / 'grype.cdx.json'}", work, run)
        built = (((doc.get("descriptor") or {}).get("db") or {}).get("status") or {}).get("built")
        out += normalize_grype(doc, {(c["name"], c.get("version")): _properties(c)["advisories:source"] for c in rest})
        image = images[0]
        for platform in platforms:
            doc = grype_scan(f"registry:{image['name']}@{image['version']}", work, run, platform)
            out += normalize_grype(doc, {}, image=(image["name"], image["version"]))
    return merge(out), built


def scan(config_path: Path, inventory_path: Path, today: datetime.date, run=None) -> Result:
    config = tomllib.loads(config_path.read_text())
    ignores = load_ignores((config_path.parent / config["ignores"]).read_text())
    found, built = findings(json.loads(inventory_path.read_text()), config["base_image"].get("platforms", []), today, run)
    ignored = {f.key: i for f in found if (i := matching_ignore(f, ignores, today))}
    return Result(found, ignored, built)


def report(result: Result) -> dict:
    return {
        "grype_db_built": result.grype_db_built,
        "findings": [
            {**f.to_json(), "ignored": {"reason": i.reason, "expiry": i.expiry.isoformat()} if (i := result.ignored.get(f.key)) else None}
            for f in result.findings
        ],
    }


def print_open(result: Result, label: str) -> None:
    """Each open finding to stdout, then the counts to stderr."""
    counts: dict[str, int] = {}
    for f in result.open:
        counts[f.source] = counts.get(f.source, 0) + 1
        aliases = f" ({', '.join(f.aliases)})" if f.aliases else ""
        fixed = f", fixed in {f.fixed}" if f.fixed else ""
        packages = f" [{', '.join(f.packages)}]" if f.packages else ""
        print(f"{f.component} {f.version}: {f.id}{aliases}, {f.severity}{fixed}{packages}")
    summary = ", ".join(f"{s} {n}" for s, n in sorted(counts.items())) or "none"
    print(f"{label}{len(result.open)} open, {len(result.ignored)} ignored; open by source: {summary}", file=sys.stderr)


def utc_today() -> datetime.date:
    return datetime.datetime.now(datetime.UTC).date()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", required=True, type=Path, help="advisories.toml")
    parser.add_argument("--inventory", required=True, type=Path, help="CycloneDX inventory from inventory.py")
    parser.add_argument("--output", required=True, type=Path, help="findings JSON to write")
    args = parser.parse_args(argv)
    args.output.unlink(missing_ok=True)
    try:
        result = scan(args.config, args.inventory, utc_today())
        args.output.write_text(json.dumps(report(result), indent=2) + "\n")
    except Exception as e:  # noqa: BLE001  (a crash must never read as findings or a clean scan)
        print(f"scan: error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    print_open(result, "scan: ")
    return EXIT_FINDINGS if result.open else EXIT_CLEAN


if __name__ == "__main__":
    sys.exit(main())
