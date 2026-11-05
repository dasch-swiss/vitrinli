#!/usr/bin/env python3
"""Render the rolling advisory issue body from a scan.

Usage:
    report.py --config advisories.toml --inventory advisories.cdx.json --findings advisories-findings.json --output body.md [--summary FILE] [--run-url URL] [--root DIR]

The body lists the open findings, the Grype advisory database's build time,
the ignores expiring within 14 days, the components whose `eol` falls within
90 days ("Approaching end of life"), and the pin table: every `http_archive`
(its `MODULE.bazel` URL), every component whose version comes from an
`inputs.version_files` entry (Kakadu's tag), and every GitHub Actions ref, one
row per repository and ref, each dated by the last commit that changed how
often its pin occurs in the searched files (`git log -1 --format=%cs -S<pin>`,
so `--root` must have full history): a bump, but also a new or removed use of
the same pin. A ref that is not a commit SHA floats and gets no date.

`--output` gets the body capped at 60 000 characters, cut at a line and ending
in a pointer to the run summary (`--run-url`); `--summary` gets the whole body
appended, as `$GITHUB_STEP_SUMMARY` expects.

Prints `open=<n>`, the unignored findings (`EOL` included; an approaching end
of life alone counts none), and `label=<issue_label>`, in the
`$GITHUB_OUTPUT` format.

Exit codes: 0 body written, 2 usage, 3 unreadable input or a failing `git`.
"""

import _model  # noqa: F401  (Python version check, before tomllib)

import argparse
import datetime
import json
import re
import subprocess
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

import inventory
import scan
from _model import EXIT_CLEAN, EXIT_ERROR, ToolError

CAP = 60_000
IGNORE_EXPIRY_DAYS = 14
EOL_NOTICE_DAYS = 90
_SHA = re.compile(r"^[0-9a-f]{40}$")


# ── Pin table ─────────────────────────────────────────────────────────────────


@dataclass(frozen=True)
class Pin:
    component: str
    version: str
    changed: str | None


def require_full_history(root: Path) -> None:
    shallow = subprocess.run(["git", "-C", str(root), "rev-parse", "--is-shallow-repository"], capture_output=True, text=True)
    if shallow.returncode != 0:
        raise ToolError(f"git rev-parse in {root}: {shallow.stderr.strip()}")
    if shallow.stdout.strip() == "true":
        raise ToolError(f"{root} is a shallow clone; the pin dates need its full history")


def last_change(root: Path, pin: str, paths: list[str]) -> str | None:
    r = subprocess.run(["git", "-C", str(root), "log", "-1", "--no-show-signature", "--format=%cs", f"-S{pin}", "--", *paths], capture_output=True, text=True)
    if r.returncode != 0:
        raise ToolError(f"git log -S{pin}: {r.stderr.strip()}")
    return r.stdout.strip() or None


def pins(config: dict, config_dir: Path, root: Path, components: list[dict]) -> list[Pin]:
    inputs = config["inputs"]
    module = inputs["module_bazel"]
    urls = {
        c.kwargs["name"]: inventory.http_archive_url(c)
        for c in inventory.starlark_calls((root / module).read_text())
        if c.callee == "http_archive" and isinstance(c.kwargs.get("name"), str)
    }
    version_files = inputs.get("version_files", {})
    entries, _ = inventory.load_map(config_dir / config["component_map"])
    version_res = {e["name"]: e["version_re"] for e in entries if e["source"] != "http_archive" and e.get("version_re")}
    workflows = [f":(glob){g}" for g in inputs["workflows"]]

    out: set[Pin] = set()
    for c in components:
        props = {p["name"]: p["value"] for p in c.get("properties", [])}
        name, version, source = c["name"], c.get("version") or "unknown", props["advisories:source"]
        if source == "http_archive":
            if not urls.get(name):
                raise ToolError(f"http_archive {name!r} has no URL in {module}")
            out.add(Pin(name, version, last_change(root, urls[name], [module])))
        elif name in version_files:
            if name not in version_res:
                raise ToolError(f"inputs.version_files.{name} has no version_re in {config['component_map']}")
            m = re.search(version_res[name], (root / version_files[name]).read_text())
            if not m:
                raise ToolError(f"{name}: version_re does not match {version_files[name]}")
            out.add(Pin(name, version, last_change(root, m.group(0), [version_files[name]])))
        elif source == "githubactions":
            version = c.get("version") or f"unresolved: {props.get('advisories:unresolved')}"
            refs = props.get("advisories:ref", "").split()
            if not refs:
                out.add(Pin(name, version, None))
            for ref in refs:
                changed = last_change(root, ref, workflows) if _SHA.match(ref) else None
                out.add(Pin(f"{name}@{ref}", version, changed))
    return sorted(out, key=lambda p: (p.component, p.version, p.changed or ""))


# ── Body ──────────────────────────────────────────────────────────────────────


def _cell(text: str) -> str:
    return text.replace("|", "\\|")


def _table(header: list[str], rows: list[list[str]]) -> str:
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines += ["| " + " | ".join(_cell(c) for c in row) + " |" for row in rows]
    return "\n".join(lines)


def open_findings(doc: dict) -> list[dict]:
    return [f for f in doc["findings"] if not f["ignored"]]


def summary_section(doc: dict, today: datetime.date) -> str:
    n_open = len(open_findings(doc))
    return (
        f"Advisory scan, {today.isoformat()}: "
        f"**open findings: {n_open}**, ignored: {len(doc['findings']) - n_open}.\n\n"
        f"Grype advisory database built {doc.get('grype_db_built') or 'at an unknown time'}."
    )


def findings_section(doc: dict) -> str:
    def rank(f):
        return (-scan.SEVERITIES.index(f["severity"]), f["component"], f["id"])

    rows = []
    for f in sorted(open_findings(doc), key=rank):
        what = f"{f['component']} ({', '.join(f['packages'])})" if f["packages"] else f"{f['component']} {f['version']}"
        advisory = f["id"] + (f" ({', '.join(f['aliases'])})" if f["aliases"] else "")
        rows.append([what, advisory, f["severity"], f["fixed"] or "no fix"])
    body = _table(["Component", "Advisory", "Severity", "Fixed in"], rows) if rows else "No open finding."
    return "## Findings\n\n" + body


def expiring_section(doc: dict, today: datetime.date) -> str:
    until = today + datetime.timedelta(days=IGNORE_EXPIRY_DAYS)
    rows = [
        [f["component"], f["id"], f["ignored"]["expiry"]]
        for f in doc["findings"]
        if f["ignored"] and today <= datetime.date.fromisoformat(f["ignored"]["expiry"]) <= until
    ]
    if not rows:
        return ""
    return f"## Ignores expiring within {IGNORE_EXPIRY_DAYS} days\n\n" + _table(["Component", "Advisory", "Expires"], sorted(rows))


def eol_section(components: list[dict], today: datetime.date) -> str:
    until = today + datetime.timedelta(days=EOL_NOTICE_DAYS)
    rows = []
    for c in components:
        eol = next((p["value"] for p in c.get("properties", []) if p["name"] == "advisories:eol"), None)
        # From the date on, the scan reports an `EOL` finding instead.
        if eol and today < datetime.date.fromisoformat(eol) <= until:
            rows.append([c["name"], c.get("version") or "unknown", eol])
    if not rows:
        return ""
    return "## Approaching end of life\n\n" + _table(["Component", "Version", "End of life"], sorted(rows))


def pin_section(table: list[Pin]) -> str:
    rows = [[p.component, p.version, p.changed or "-"] for p in table]
    return "## Pins\n\n" + _table(["Component", "Version", "Pin changed"], rows)


def render(doc: dict, components: list[dict], table: list[Pin], today: datetime.date) -> str:
    sections = [summary_section(doc, today), findings_section(doc), expiring_section(doc, today), eol_section(components, today), pin_section(table)]
    return "\n\n".join(s for s in sections if s) + "\n"


def cap(body: str, run_url: str | None, limit: int = CAP) -> str:
    if len(body) <= limit:
        return body
    where = f"[run summary]({run_url})" if run_url else "run summary"
    note = f"\n_Truncated at {limit:,} characters; the full report is in the {where}._\n"
    return body[: body.rfind("\n", 0, limit - len(note)) + 1] + note


def utc_today() -> datetime.date:
    return datetime.datetime.now(datetime.UTC).date()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", required=True, type=Path, help="advisories.toml")
    parser.add_argument("--inventory", required=True, type=Path, help="CycloneDX inventory from inventory.py")
    parser.add_argument("--findings", required=True, type=Path, help="findings JSON from scan.py")
    parser.add_argument("--output", required=True, type=Path, help="capped issue body to write")
    parser.add_argument("--summary", type=Path, help="file to append the whole body to")
    parser.add_argument("--run-url", help="the run whose summary holds the whole body")
    parser.add_argument("--root", default=Path("."), type=Path, help="tree the pins are read from (default: .)")
    args = parser.parse_args(argv)
    args.output.unlink(missing_ok=True)
    try:
        config = tomllib.loads(args.config.read_text())
        doc = json.loads(args.findings.read_text())
        components = json.loads(args.inventory.read_text())["components"]
        label = config["issue_label"]
        require_full_history(args.root)
        table = pins(config, args.config.parent, args.root, components)
        body = render(doc, components, table, utc_today())
        if args.summary:
            with args.summary.open("a") as f:
                f.write(body)
        args.output.write_text(cap(body, args.run_url))
    except Exception as e:  # noqa: BLE001  (a broken report must never reach the issue)
        print(f"report: error: {type(e).__name__}: {e}", file=sys.stderr)
        return EXIT_ERROR
    print(f"open={len(open_findings(doc))}")
    print(f"label={label}")
    return EXIT_CLEAN


if __name__ == "__main__":
    sys.exit(main())
