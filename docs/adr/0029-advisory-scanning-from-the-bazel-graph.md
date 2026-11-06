---
status: accepted
---

# Advisory scanning reads the Bazel dependency graph, not the image

Vitrinli's advisory scan takes its input from the files Bazel resolves
against, not from the built image. `tools/advisories/inventory.py` turns
`Cargo.Bazel.lock`, `MODULE.bazel.lock`, the `http_archive` pins in
`MODULE.bazel`, `bazel/distroless/bookworm.lock.json`, the `oci.pull` base
digest and the workflows' `uses:` refs into one CycloneDX 1.6 inventory. Two
scanners match it: **OSV-Scanner for Rust crates, Grype for Debian packages,
C/C++ libraries (by CPE), the distroless base image and GitHub Actions.**

The scan wrapper `tools/advisories/scan.py` (`just advisories-scan`) applies
one scanner-agnostic `tools/advisories/ignores.toml` with expiry. Scanners are
called only through it, never directly from a workflow step. The `advisories`
PR job (`.github/workflows/ci.yml`) runs `tools/advisories/diff.py`
(`just advisories-scan-diff`) and fails on new findings only. The
`advisories-gate` job (`.github/workflows/publish.yml`) runs the same diff
against the last release whose image reached Docker Hub, before the image
push, and uploads the release SBOM. The daily `.github/workflows/advisories.yml`
runs the full scan and keeps its findings in one rolling issue, rendered by
`tools/advisories/report.py`, and opens the crate fix PRs through
`tools/advisories/fix.py`.

Not yet in code: the fix PRs for BCR modules, the base digest and the Debian
snapshot date.

## Context

**The image hides most of what Vitrinli ships.** Every Rust crate and every
C/C++ library (libtiff, exiv2, lcms2, libpng, OpenSSL, curl, zlib, FFmpeg,
Kakadu) is statically linked into one `sipi` binary that carries no package
metadata. Docker Scout, scanning the image, sees only the Debian packages of
the distroless base and the `rules_distroless` layer. Trivy, Syft and
`cve-bin-tool` share that blind spot. The lockfiles, by contrast, record the
exact version of everything.

**What existed.** `just audit` (cargo-audit plus OSV-Scanner over
`Cargo.Bazel.lock`) covered crates only and failed on any matched advisory,
so an advisory published against `main` turned every open PR red
(RUSTSEC-2026-0387). Scout's release gate ran after the image push. BCR
modules, the `http_archive` libraries, the Debian packages, the base digest
and the SHA-pinned Actions relied on a quarterly manual review or on nothing.

## Decision

### Inventory over image

One component per shipped dependency, sorted for stable diffs:

| Class | Source | Identity |
|---|---|---|
| cargo | `Cargo.Bazel.lock` | `pkg:cargo/<name>@<version>` |
| CPE | BCR modules (`source.json` keys of `MODULE.bazel.lock`, `.bcr.N` stripped) and `http_archive` URLs (`version_re`) | `cpe` from `tools/advisories/components.toml`, no purl |
| deb | `bookworm.lock.json`, both arches | `pkg:deb/debian/<binary>@<version>?arch=<arch>&distro=debian-12[&upstream=<source>]` |
| base image | `oci.pull` digest | scanned as `registry:<image>@<digest>` |
| Actions | `uses:` refs, resolved to a release tag | `pkg:githubactions/<owner>/<repo>@<version>` plus `syft:package:type=github-action` |

`components.toml` classifies every BCR module, `http_archive`,
`git_override` and custom repository rule as `shipped`, `not_shipped` or
`no_feed` (Kakadu: proprietary, no public feed), and holds no versions: the
scanned tree's own files are the only version record, so a base worktree
reports its own versions. The drift check fails on an unclassified or stale
entry.

### Scanner choice, by fixture

The comparison scanned `tools/advisories/tests/fixtures/spike.cdx.json`, one
known-vulnerable component per class, and the real inventory, on
2026-11-05:

- OSV-Scanner 2.7.0, `osv-scanner_linux_amd64` sha256
  `160c6253e2922e9e0a6a9771c5c4c40a4b3c6090b34a3cdd5e80c96cc2e4218b`
  (`darwin_arm64` `d4926ef8da134332543be276dff157e4c170f99ad91fc48e89448a8816bdf70b`).
- Grype 0.121.1, `grype_0.121.1_linux_amd64.tar.gz` sha256
  `bc3ea3f9e6fd56ac781c08c6c5e614271b79f438200757f6932085c19b0a4657`
  (`darwin_arm64.tar.gz` `daa9ffdc0022b34fb476f94cbcabac9d850882c895d83e4a5ecf5e0ab13bce77`);
  database schema v6.1.10, built 2026-11-05T06:21:06Z.

| Class | Component, expected advisory | OSV-Scanner | Grype |
|---|---|---|---|
| cargo, fixture | `smallvec` 1.6.0, RUSTSEC-2021-0003 / GHSA-43w2-9j62-hq99 | yes, both ids | yes, GHSA only |
| cargo, real tree | `crypto-bigint` 0.5.5 RUSTSEC-2026-0387 (no GHSA); `rsa` 0.9.10 RUSTSEC-2023-0071 | yes, both | **no**: its Rust data is GHSA only, and the rsa GHSA range ends at 0.9.6 |
| deb, binary name only | `libc6` 2.36-9+deb12u1, CVE-2023-4911 | no | no, with or without `--distro debian:12` |
| deb, `upstream=glibc` | same | no (qualifier ignored) | yes, CVE-2023-4911 (`exact-indirect-match`) |
| deb, purl name `glibc` | same | yes, DSA-5514-1 | yes |
| deb, `distro=bookworm` | same | no | no (read as distro `bookworm`, no version) |
| CPE | `curl` 8.3.0 CVE-2023-46218; `tiff` 4.5.0 CVE-2023-40745 | no: CPE-only components are filtered as unscannable | yes, both (`cpe-match`) |
| base image | `base-debian12@sha256:74ddbf52…1348` (2025-01-12) | yes, from a `docker save` archive (`--archive`) | yes, `registry:…@<digest>` with `--platform`, no daemon |
| Actions | `tj-actions/changed-files` 45.0.0, GHSA-mrrh-fwg8-r2c3 | no: `pkg:githubactions` is an invalid purl to it | yes, only with `syft:package:type=github-action` |

**Why two scanners.** Grype alone reports the fixture's component in every
class (cargo, deb, CPE, base image, and Actions besides), once the deb purl
names the source package. On crates it falls
short where it matters: its Rust data comes from GHSA only, so it misses
RustSec advisories without a GHSA (RUSTSEC-2026-0387, the advisory that
blocked every PR) and RustSec ranges wider than GHSA's (`rsa`). OSV-Scanner
reads RustSec and GHSA. It cannot match a CPE-only component or an Actions
purl, and matches a deb only when the purl name is the source package, which
would rename and merge components (`libgcc-s1` and `gcc-12-base` become
`gcc-12`). So crates go to OSV-Scanner and everything else to Grype. That is
two scanners, the most this design allows.

**Shapes the scanners require.** A deb purl carries `upstream=<source>` when
the source package differs from the binary; `inventory.py` reads the source
from the lock's `pool/<area>/<x>/<source>/` URL segment. An Actions component
carries `syft:package:type=github-action`, since Grype does not map the
`githubactions` purl type to its Actions matcher.

### How the wrapper must read each scanner

Both run with JSON output, no `--fail-on`, no ignore file of their own.

- **OSV-Scanner**: exit 0 clean, 1 findings, 127 general error (an unreadable
  image archive), 128 no packages (an empty inventory, or one holding only
  components it rejects). A file not named `*.cdx.json` exits 127. It queries
  the live OSV API, so its database date is the scan time.
- **Grype**: exit 0 whether or not it finds anything (2 only with
  `--fail-on`), 1 on error, including a database older than
  `db.max-allowed-built-age` (120h) and an unparseable SBOM. **An empty SBOM
  exits 0**, so the wrapper counts the components it sent. The database date
  is `descriptor.db.status.built` in the JSON output. `@<digest>` references
  work. Grype's built-in default ignores (kernel headers, `linux-libc-dev`,
  `linux-kbuild-*`) touch no package Vitrinli ships.
- **Ids differ**: OSV-Scanner reports RUSTSEC ids with GHSA aliases, Grype
  GHSA and CVE ids. Findings are keyed `(component, id)`, so the wrapper
  maps aliases to one id before diffing head against base.

### Alert-only classes

A finding opens a fix PR only where the fix is a mechanical pin bump: a
crate repin, a BCR module bump, the base digest, the Debian snapshot date.
These stay alert-only, listed in the daily issue:

- `http_archive` libraries and FFmpeg: ADR-0015 makes every native bump a
  re-derivation of `defines`, and exiv2 carries a patch.
- A BCR module held by a `single_version_override` with `patches`, for the
  same reason.
- GitHub Actions: a SHA bump changes code that runs with the workflows'
  secrets, so a person reviews it.
- A crate that only a semver-major bump of a dependent can fix.
- Kakadu (`no_feed`) and every finding with no released fix.
- End of life (`EOL`) findings.

**CPE matches are weak evidence.** NVD CPE ranges are noisy for C libraries
and ignore distro backports, and a wrong vendor:product misses silently
(`abseil:abseil-cpp` matched nothing; NVD files Abseil under
`abseil:common_libraries`). The daily issue therefore carries a pin table of
every `http_archive`, FFmpeg, Kakadu and Actions pin with the date it last
changed, which replaces the quarterly review.

### Decisions this reverses

- **Docker Scout stays the image-layer gate** (security hardening wave 2,
  S2-30), because nothing else saw the Debian layers; the same finding added
  `just audit` and the quarterly native-pin review, because nothing else
  covered those surfaces. Reversed: the inventory reads the layers from their
  Bazel inputs and covers the native pins, so Scout's only unique coverage
  moves into this pipeline, and the release gate moves before the push.
- **SHA-pinned Actions kept fresh by Dependabot** (wave 2, S2-41). Dependabot
  is already off, and GitHub raises no alerts for SHA pins. SHA pins keep their
  protection against tag hijacking; advisories against them come from the
  inventory; their freshness is routine currency, out of scope.
- **Trivy for image CVEs and Syft for the image SBOM**, the planned Scout
  replacement, chosen as the cleanest single-tool swap and because no Bazel
  rule emitted an image SBOM. The target is no longer the image: both inherit
  Scout's blind spot, and the lockfiles carry exact versions. Image signing
  and attestation with cosign is untouched and can attest this inventory.

## Considered options

**Grype alone.** It covers every class on the fixture, and one scanner
means one database and one exit-code contract. Rejected: on the real tree it
misses the RustSec advisories Vitrinli has hit.

**OSV-Scanner for cargo and deb, Grype for CPE and the base image.**
Rejected for deb: OSV-Scanner matches a binary package only under its
source name, so the inventory would stop naming what is installed.

**Self-hosted Renovate.** Its advisory mode covers direct dependencies only,
and every crate advisory Vitrinli has hit was transitive. It does not parse
`http_archive` or recompute sha256s, and solves routine currency, a
non-goal.

**GitHub dependency submission API.** Raises Dependabot alerts for crates
only, not for C libraries or BCR modules, and no fix PRs for a graph
Dependabot cannot edit.

**Bazel-native SBOM rules** (`rules_license` `write_sbom`,
`eclipse-score/sbom-tool`). Young, and they still need the same name-to-CPE
map for C libraries.

**Scanning the image with Trivy and Syft**, or **the linked binary with
`cve-bin-tool`**. Same blind spot; `cve-bin-tool` fingerprints version
strings, misses lcms2, Kakadu and backports, and finds nothing the
inventory does not.

## Consequences

- Two scanner pins move together: one composite action installs both, so
  the PR job, the release gate and the daily job run the same versions.
- Grype needs its database (about 200 MB to download, 3.4 GB unpacked, under 120h old) in every job;
  OSV-Scanner needs network to the OSV API.
- A deb lock entry without a `pool/` URL stops the inventory (exit 3).
- A CPE miss produces no alert at all; the pin table is the backstop.
  `cli11`, `highway` and `libdeflate` have no NVD CPE product, so their
  guessed CPEs produce no finding until NVD adds one.
