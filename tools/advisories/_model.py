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
        props = {"advisories:source": self.source, **self.properties}
        out["properties"] = [{"name": k, "value": props[k]} for k in sorted(props)]
        return out


def cyclonedx(components: list[Component]) -> dict:
    """The BOM document: no timestamp or serial number, so equal inputs give equal bytes."""
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "version": 1,
        "components": [c.to_cdx() for c in sorted(components, key=lambda c: c.bom_ref)],
    }
