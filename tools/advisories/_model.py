"""Shared types of the advisory tools.

Imported first by every tool: the version check runs before any import that
needs Python 3.11 (`tomllib`), so an older interpreter exits 3 with a message
instead of an ImportError traceback.
"""

import sys

if sys.version_info < (3, 11):
    sys.stderr.write(f"advisories: Python 3.11 or newer required, found {sys.version.split()[0]}\n")
    sys.exit(3)

from dataclasses import dataclass, field

EXIT_CLEAN = 0
EXIT_FINDINGS = 1
EXIT_USAGE = 2
EXIT_ERROR = 3

# Prefix of the branches `fix.py` opens fix PRs from, one per component (one for all Debian packages).
BRANCH_PREFIX = "advisories/"


def branch_name(component: str) -> str:
    return BRANCH_PREFIX + component


# Every Debian package is fixed by the one snapshot bump, so their findings
# share the fix of the release component the inventory names `debian`.
DEB_FIX_COMPONENT = "debian"


def fix_component(component: str, source: str) -> str:
    """The component whose fix branch carries a finding's fix."""
    return DEB_FIX_COMPONENT if source == "deb" else component


def actionable_finding(f: dict) -> bool:
    """Whether `fix.py` acts on a finding: unignored, and neither `EOL` nor a GitHub Actions component."""
    return not f["ignored"] and f["id"] != "EOL" and f["source"] != "githubactions"


class ToolError(Exception):
    """An input the tool cannot read or interpret; `main` maps it to exit 3."""


@dataclass(frozen=True)
class Component:
    """One inventory entry, rendered as a CycloneDX 1.6 component."""

    name: str
    version: str | None
    source: str
    type: str = "library"
    purl: str | None = None
    cpe: str | None = None
    # Further CPEs, emitted as repeated `syft:cpe23` properties, which Grype matches like `cpe`.
    extra_cpes: tuple[str, ...] = ()
    properties: dict[str, str] = field(default_factory=dict)

    @property
    def bom_ref(self) -> str:
        return self.purl or f"{self.source}:{self.name}@{self.version or self.properties.get('advisories:ref', '')}"

    def to_cdx(self) -> dict:
        out: dict = {"type": self.type, "bom-ref": self.bom_ref, "name": self.name}
        if self.version is not None:
            out["version"] = self.version
        if self.purl:
            out["purl"] = self.purl
        if self.cpe:
            out["cpe"] = self.cpe
        props = [("advisories:source", self.source), *self.properties.items(), *(("syft:cpe23", c) for c in self.extra_cpes)]
        out["properties"] = [{"name": k, "value": v} for k, v in sorted(props)]
        return out


@dataclass(frozen=True)
class Finding:
    """One advisory (or `EOL`) against one inventory component, keyed `(component, id)`."""

    component: str
    id: str
    version: str | None = None
    source: str = ""
    aliases: tuple[str, ...] = ()
    severity: str = "unknown"
    fixed: str | None = None
    # The image packages a base-image finding concerns; empty for every other component.
    packages: tuple[str, ...] = ()

    @property
    def key(self) -> tuple[str, str]:
        return (self.component, self.id)

    def to_json(self) -> dict:
        return {
            "component": self.component,
            "version": self.version,
            "source": self.source,
            "id": self.id,
            "aliases": list(self.aliases),
            "severity": self.severity,
            "fixed": self.fixed,
            "packages": list(self.packages),
        }


def cyclonedx(components: list[Component]) -> dict:
    """The BOM document: no timestamp or serial number, so equal inputs give equal bytes."""
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "version": 1,
        "components": [c.to_cdx() for c in sorted(components, key=lambda c: c.bom_ref)],
    }
