# CI and Release

This page documents Vitrinli's CI pipeline and release automation.

## Release automation (release-please)

Releases are fully automated via
[release-please](https://github.com/googleapis/release-please).
When commits are merged to `main`, release-please reads their
[Conventional Commit](https://www.conventionalcommits.org/) prefixes
to determine the SemVer bump and generate the changelog.

**Configuration files:**

- `.github/release-please/config.json` — changelog sections, release type
- `.github/release-please/manifest.json` — current version
- `.github/workflows/release-please.yml` — GitHub Actions workflow

**How commit types map to releases:** the type → changelog-section → SemVer-bump
table is single-sourced in
[Commit and PR Conventions § Commit message schema](commit-conventions.md#commit-message-schema).
The machine source release-please actually reads is
[`.github/release-please/config.json`](../../../.github/release-please/config.json).

Every commit appears in the release notes in its own section, and — because the
scope is mandatory — each line reads `concern: subject`. A release containing only
internal-work commits (`refactor`/`docs`/`test`/`build`/`chore`) is a patch release:
release-please keeps a release PR open for it, and the maintainer decides when to
merge it.

!!! warning "Correct type + scope are critical, and enforced"
    A commit without a valid type is invisible to release-please — it won't trigger
    a release or appear in the changelog. Both the type (one of the eight allowed)
    and a mandatory scope are enforced in CI by the `commit-lint` job (`commitlint-rs`).
    See [Commit and PR Conventions](commit-conventions.md) for the full schema, the
    scope vocabulary, what `fix:` means, and `just commit-lint`.

## Pull request CI

Workflow: `.github/workflows/ci.yml`. Trigger: `pull_request` plus
manual `workflow_dispatch`. No Nix anywhere in this workflow — every
job provisions Bazel and `just` on a plain GitHub-hosted runner via
the `.github/actions/ci-setup` composite action (see "CI environment
provisioning" below). Nix remains the local dev-shell provisioner only.

### `changes` gate

A `changes` job runs first and inspects the diff to decide whether the
sanitizer job (below) needs to run: it looks for changes under
`src/`, `include/`, `test/`, `bazel/`, `platforms/`, `config/`,
`scripts/`, `MODULE.bazel*`, `.bazelrc`, `.bazelversion`, `BUILD.bazel`,
`justfile`, `.lsan_suppressions.txt`, `ci.yml`, and `.github/actions/`.
A docs-only PR skips the sanitizer job — a skipped required job still
satisfies branch rulesets. The `test` matrix has no `needs:` on this
job and starts immediately, in parallel with it. A manual
`workflow_dispatch` run bypasses the filter and always runs the
sanitizer job.

### Test matrix

The `test` job runs on three platforms — `linux-amd64`,
`linux-arm64`, `darwin-arm64` — and all three legs start immediately;
none of them wait on a separate lint gate. Every leg runs:

1. **Build + test** — `just bazel-test` (fastbuild, no
   instrumentation; unit + approval + e2e in a single Bazel
   invocation).
2. **Docker smoke tests (Linux only)** — `just bazel-test-smoke`
   builds `//src:sipi_image` as a transitive `data` dep of the
   `:docker_smoke` rust_test, the test loads the OCI tarball into
   the local Docker daemon, and runs the smoke suite against the
   loaded container.

The standalone `lint` job is gone. Its lint checks —
`bazel-rustfmt-check` and `bazel-clippy-check` — now run as extra
steps inside the `test / linux-arm64` leg, the shortest of the three
legs, instead of gating the matrix from a separate job.

A separate `docs` job runs `just docs-build` (mkdocs strict-mode
build) on ubuntu-latest. The `docs-build` job is the gate that
catches broken cross-links and stale nav entries on every PR.

### Path-gated sanitizer job

The `sanitizer` job (`asan-ubsan / amd64`) runs ASan+UBSan over the
unit and e2e suites in a single `just bazel-test-sanitized` invocation,
gated by the `changes` job above. Running both suites in one `bazel
test` compiles the instrumented `sipi` tree exactly once — a single
action graph shares the ~5500-file compile across both suites (two
separate jobs would each compile the full tree). Runs on linux-x86_64 only
(the macOS toolchain args omit the sanitizer header path). ASan/LSan
symbol resolution uses the hermetic LLVM 23.1.2 `//bazel:llvm-symbolizer`
(version-matched to clang 23.1.2), which the e2e macro attaches as a test
runfile under `--config=asan` and points `ASAN_SYMBOLIZER_PATH` at — so the
tests (unit + e2e) execute on the RBE worker, symbolizer included, rather
than on the runner.

### Dependency advisory scan

The `advisories` job runs `just advisories-test` and
`just advisories-scan-diff origin/<base branch>`: it fails on an advisory
the PR adds over its base branch, never on one the base already has. See
"Dependency advisory scanning" below. Self-contained (no Bazel, RBE or Nix),
unfiltered by path so it always reports on a PR, and skipped on
`workflow_dispatch`, which has no base branch.

### CI environment provisioning

Every Bazel-invoking job in `ci.yml` (and in `publish.yml`,
`coverage.yml`, `fuzz.yml`) starts with the `.github/actions/ci-setup`
composite action, which wraps:

- `bazelbuild/setup-bazelisk` — bazelisk on PATH (reads
  `.bazelversion`).
- `extractions/setup-just` — `just` on PATH.
- Optional git-LFS restore/pull (test images), when the job needs them.
- The Bazel repository cache (`actions/cache` over
  `~/.cache/bazel-repo`), keyed on `MODULE.bazel.lock`.
- The existing `.github/actions/bazel-rbe` composite action, for the
  RBE mTLS material and cross-compile flags.

`gh` is preinstalled on GitHub-hosted runners, so no setup step
provisions it. `GH_TOKEN` is passed as plain step `env:` on the
`just bazel-*` invocation that needs it (the `gh_release_archive`
repository_rule shells out to `gh release download` for the Kakadu
fetch) — there is no Nix-impure-env hack involved.

### Forked PR behavior

Every Bazel-invoking step sets
`GH_TOKEN: ${{ secrets.DASCHBOT_PAT }}` on its `env:` so the
`gh_release_archive` repository_rule can authenticate. Forked PRs
don't have access to `DASCHBOT_PAT`, so the Kakadu fetch fails
and the build short-circuits. Internal PRs are unaffected.

### Required checks

Branch rulesets require the three `test / *` legs
(`test / linux-amd64`, `test / linux-arm64`, `test / darwin-arm64`)
plus the `asan-ubsan / amd64` job. The `docs` job, the `advisories`
job (not yet required; it reports on every PR, so it can be) and the
`changes` gate itself are not required checks. (The
asan-ubsan job was previously split into `asan-ubsan-unit / amd64` +
`asan-ubsan-e2e / amd64`; the ruleset must be updated to the merged name.)

**`main` is protected by a *ruleset*, not by classic branch protection.**
`GET /repos/dasch-swiss/vitrinli/branches/main/protection` therefore answers
`404 Branch not protected`, which is true only of the classic mechanism and
says nothing about the ruleset. Read the rules from the rulesets API instead,
or just read this section:

```bash
gh api repos/dasch-swiss/vitrinli/rulesets                 # find the ruleset id
gh api repos/dasch-swiss/vitrinli/rulesets/<id> --jq '[.rules[].type]'
```

The same ruleset pins **`rebase` as the only allowed merge method** (alongside
`required_linear_history`), so `gh pr merge --squash` and `--merge` are
rejected. This is the mechanism behind the "a PR lands as one commit by
default" convention in `CLAUDE.md`: every branch commit reaches `main`
verbatim, with no squash to tidy it, which is why branches are cleaned up
before merge. Note that dsp-api and dsp-app are the opposite — squash-only —
so the habit does not carry between repos.

## Post-merge coverage

Workflow: `.github/workflows/coverage.yml`. Trigger:
`workflow_dispatch` only (manual runs) — see the workflow's header
comment for why the `push: branches: [main]` trigger stays disabled
under the current hermetic-llvm toolchain.

A single `linux-amd64` job runs via `ci-setup` and calls
`just bazel-coverage`. The recipe's `collect_cc_coverage.sh` needs
`COVERAGE_GCOV_PATH`/`LLVM_COV` on PATH; those resolve from the
hermetic Bazel toolchain's `//bazel:llvm-profdata`/`//bazel:llvm-cov`
aliases, not from a Nix dev shell — no `.#llvm-tools` shell is
involved in CI. The combined lcov report at
`bazel-out/_coverage/_coverage_report.dat` is uploaded to Codecov.

**Why split out:** Coverage instrumentation adds 1.5–2× compile
overhead and slower test runtime; running it on every PR push
across three platforms wasted CI minutes without commensurate
signal. Per-PR coverage delta in Codecov is the trade-off — drift
shows up immediately after merge instead. To restore PR-scoped
signal selectively, add a `pull_request: paths: ['src/**']` trigger
to this workflow.

## Dependency advisory scanning

The scan reads what the build resolves against, not the image: every Rust
crate and C/C++ library is statically linked into one binary that carries no
package metadata. [ADR-0029](../../adr/0029-advisory-scanning-from-the-bazel-graph.md)
records the design and the scanner choice.

**Pipeline.** All tools live in `tools/advisories/` (stdlib Python 3.11+,
inputs named in `advisories.toml`):

1. `inventory.py` (`just advisories-inventory`) writes `advisories.cdx.json`,
   a CycloneDX inventory of `Cargo.Bazel.lock`, the BCR modules of
   `MODULE.bazel.lock`, the `http_archive` pins, both arches of
   `bazel/distroless/bookworm.lock.json`, the `oci.pull` base digest and every
   `uses:` ref of the workflows. Its drift check fails on a dependency the
   component map does not classify.
2. `scan.py` (`just advisories-scan`) runs OSV-Scanner over the crates and
   Grype over everything else, both without their own ignores, then applies
   `ignores.toml`.
3. `diff.py` (`just advisories-scan-diff <base>`) scans this tree and a
   worktree of `<base>` with this tree's tools, keys findings by component
   name and advisory id (never by version), and applies this tree's ignores to
   what only this tree has. Drift in the base is tolerated, since `main` and
   older tags may predate the component map, and printed as `base drift
   (skipped)`: a base component left out that way makes its findings read as
   new. A base without one of the config's input files exits 3.

In CI, the `advisories` job in `ci.yml` runs the diff against the PR's base
branch. A PR that fixes a finding passes; an advisory published against `main`
does not block unrelated PRs, and a PR adding a second vulnerable version of an
already-flagged crate passes too. Both cases stay visible to the full scan.
The `advisories-gate` job in `publish.yml` runs the same diff once per release,
against the baseline `just advisories-release-baseline <tag>` prints: the
newest older `v*` tag whose image reached Docker Hub (`[release]` in
`advisories.toml`), skipping a tag whose release failed, since diffing against
it would let its findings through. So a release never introduces a vulnerable
pin, and a backlog finding never blocks a hotfix.
Exit codes, for every tool: 0 clean, 1 findings or drift, 3 a tool or scanner
error, which never reads as clean. The scanners are installed by
`.github/actions/advisories-scanners`, pinned by version and sha256; bump them
there. The dev shell carries nixpkgs' `osv-scanner` and `grype`, which can
trail those pins.

The backlog lives in one rolling issue, "Open dependency advisories", labelled
with `issue_label` from `advisories.toml`. `.github/workflows/advisories.yml`
runs `just advisories-scan` on the default branch daily at 05:41 UTC and
`tools/advisories/report.py` renders the body: the open findings, the Grype
database's build time, the ignores expiring within 14 days, the components
whose `eol` falls within 90 days, and a pin table dating every `http_archive`,
Kakadu and Actions pin by the last commit that changed its occurrences (a bump,
or a new or removed use of the same pin; a floating ref such as `@v4` has no
date). A run with open findings updates the issue or opens it; a run without
closes it, and the next finding opens a new one. A scanner error fails the run
and leaves the issue alone; `report.py` only renders, so it exits 0 or 3. The
issue body is capped at 60 000 characters; the run's job summary holds the
whole of it. Dispatch the workflow with `dry_run` to get the body in the job
summary without touching the issue; a dispatch from another branch never
touches it either. Locally, after `just advisories-scan`:

```bash
python3 tools/advisories/report.py --config tools/advisories/advisories.toml \
  --inventory advisories.cdx.json --findings advisories-findings.json --output body.md
```

**Fix PRs.** Between the scan and the issue, the workflow's `fix` job runs
`tools/advisories/fix.py` over the unignored findings, skipping `EOL` findings
and GitHub Actions. A Rust crate gets one PR on the branch
`advisories/<crate>`, one commit `fix(deps): raise <crate> to <version> past
<ids>`: the highest fixed version among its advisories, reached by `just
bazel-repin-crate <crate>@<locked>=<version>` when it is on the locked
version's semver line, and kept only when a re-scan with `ignores.toml` reports
no advisory for the crate left. A fix on a newer line, or an older line's
locked version that the repin leaves flagged, is alert-only, "needs a major
bump of" the nearest `crate.spec` dependency; a fixed version at or below the
locked one, or a repin that changes nothing or clears nothing, is "at or past
the fix, still flagged; needs an ignore".

The other mechanical pins follow the same lifecycle and the same "needs an
ignore" rule:

- **BCR module** (`advisories/<module>`): the lowest non-yanked version in the
  registry's `metadata.json` at or past the fixed version, in Bazel's module
  version order (`8.21.0` < `8.21.0.bcr.1` < `8.22.0`). The `bazel_dep` and a
  `single_version_override` holding a `version` (openssl pins both) are raised,
  `just bazel-lock-update` refreshes `MODULE.bazel.lock`, and a Grype re-scan
  of the module by its component map CPEs must clear. A module held by an
  override with `patches` or `patch_cmds`, reached only transitively, or whose
  fix has no registry version yet is alert-only.
- **Base image** (`advisories/<image>`): the current index digest of the tag
  `[base_image] tracks` names, from `crane digest`, written into the `oci.pull`
  with the tag and date as the line's comment, kept when a Grype re-scan of the
  new digest on every platform clears its advisories that have a released
  fix.
- **Debian packages** (one PR for all of them, `advisories/debian`): the
  snapshot timestamp of `bazel/distroless/bookworm.yaml` moves to the scan
  date, `just bazel-distroless-lock` regenerates the lock, and a Grype re-scan
  of the new lock must clear the advisories with a released fix. Its version
  is the date, new every day, so an open Debian PR is rebuilt only when its
  advisory set changes; a package with no released fix stays alert-only.

`http_archive` libraries, FFmpeg, GitHub Actions, `EOL` findings and every
advisory with no released fix stay alert-only (ADR-0029's alert-only
classes).
An open fix PR is rebuilt from the default branch and force-pushed only when
its advisory set or version changes, never for being behind; one whose branch
holds a maintainer's extra commit is left alone. A fix branch without an open
PR is replaced. A fix PR closed unmerged suppresses a new one while the
advisory set stays the same, and the issue asks for an ignore instead. PRs from
forks never count. A component whose fix fails is recorded and the others still
run; the `fix` job then fails, and the issue job still runs. The issue shows
each finding's fix outcome and the open and declined fix PRs. Pushes and PRs go
through `DASCHBOT_PAT` so their CI runs; a `dry_run` dispatch, or one from
another branch, prints each branch, title and body instead, and runs with the
workflow's own token: the PAT reaches only a run of the default branch. Either
way the fixes start from the default branch's tree, so a dispatch from a branch
does not test that branch's fix tooling.

**Running it locally.** Inside `nix develop`, `just advisories-scan` scans the
working tree and `just advisories-scan-diff origin/main` reproduces the PR
job. Both need network (the Grype database, about 200 MB on first use, the OSV
API, `git ls-remote` for the Actions refs, the base image's registry).
`just advisories-test` runs the offline tests. Set `ADVISORIES_BIN` to a
directory holding `osv-scanner`, `grype` and `crane` to use other binaries
than PATH's.

**Classifying a component.** Every BCR module, `http_archive`, `git_override`
and custom repository rule has exactly one `[[component]]` in
`tools/advisories/components.toml`, keyed by `source` and `name`:

```toml
[[component]]
source = "http_archive"
name = "tiff"
status = "shipped"
cpe = "cpe:2.3:a:libtiff:libtiff"
version_re = '/v(\d+(?:\.\d+)+)\.tar\.gz$'
```

`shipped` takes the NVD `cpe` vendor:product prefix (a list when NVD files the
product under several) and, for an `http_archive`, a `version_re` whose group 1
reads the version from the URL. `not_shipped` (build tooling, dev- or
test-only) and `no_feed` (shipped, no public advisory feed, as for Kakadu) take
a `reason`. The map holds no versions: the scanned tree's URL or lockfile is the
only version record, so the base of a diff reports its own. Adding a dependency
without an entry fails the PR job.

**Ignoring a finding.** Add an `[[ignore]]` to `tools/advisories/ignores.toml`
with the advisory `id` (or one of its aliases, or `EOL`), the inventory
`component`, a non-empty `reason` and an `expiry`:

```toml
[[ignore]]
id = "RUSTSEC-2023-0071"
component = "rsa"
reason = "Why the vulnerable path is unreachable, and when to revisit."
expiry = 2027-02-05
```

`expiry` is a bare TOML date, never quoted; the entry suppresses through that
day and stops after it, so the finding returns for a fresh decision. The tools
reject a malformed entry with exit 3. An ignore matters to the PR job only for
a finding the PR adds; a finding shared with the base never fails it.

**End of life.** A component map entry, or the `[deb]` or `[base_image]` table
of `advisories.toml`, may carry `eol = <bare date>`: the end of regular upstream
support, LTS and paid extended support not counted. From that date the scan
reports a finding with the id `EOL` against it (one per config table, not per
package), which an ignore with an expiry can suppress like any other. A PR that
adds an end-of-life component therefore fails.

**What a clean result means.** A clean crate result is strong: crates match by
exact name and version against RustSec and GHSA. A clean C/C++ library result is
weaker: those match only by CPE, whose NVD ranges are noisy and ignore distro
backports, so expect false positives (triaged with an ignore) and accept that a
CPE miss produces no finding at all. The pin table below stays the check for
them.

**GitHub Actions.** Every `uses:` ref is inventoried, a SHA resolved to the
release tag that points at it; a branch ref or a SHA no tag points at is listed
as unresolved. Actions findings are alert-only: no tool proposes a bump, because
a SHA bump changes code that runs with the workflow's secrets, so a person
reviews it. Dependabot alerts stay on, but they miss the SHA-pinned actions,
which is every action here.

Dependabot opens no version-update PRs; version updates are by hand: GitHub
Actions, the Nix flake inputs and the BCR `bazel_dep`s. The only automated
bumps are the fix PRs above, and only past an advisory. The image layers need
no scan of their own: the Debian packages and the base digest are in the
inventory.

### Native pins: the pin table

The `http_archive` libraries, FFmpeg and Kakadu get no fix PR, and a CPE miss
on them raises nothing, so the rolling issue's **Pins** table is their human
check: every `http_archive` (FFmpeg's two archives included), Kakadu and each
Actions pin, with the pinned version and the date its pin last changed.
Kakadu is `no_feed`: it has no public advisory feed, and its version and date
come from the release `tag` in `bazel/kakadu_extension.bzl`.

When a finding lands on one of them, or the table shows a pin gone stale:

1. Check the project's release notes and the advisory for the fixed version.
2. Bump it in a normal PR, re-deriving the native build as
   [ADR-0015](../../adr/0015-native-cc_library-over-foreign_cc.md) requires
   (`url`, `sha256`, `strip_prefix`, and the `defines` and patches the new
   release needs; exiv2 carries a patch).
3. If the finding does not apply (a CPE range that misses a backport, an
   unreachable code path), add an `[[ignore]]` with its reason and expiry
   instead.

The PR's `advisories` job then fails if the bump introduces an advisory, and
the daily scan drops the finding once it is merged.

## Tag release CI/CD

Workflow: `.github/workflows/publish.yml`. Trigger: tag push
matching `v*`.

Like `ci.yml`, every Bazel-invoking job provisions its environment via
the `.github/actions/ci-setup` composite action — no Nix.

Gate model:

1. `validate-docker / {amd64, arm64}` — each per-arch runner
   builds the Docker image via
   `just bazel-docker-build-${arch}` and runs
   `just bazel-test-smoke` against it.
   `advisories-gate` runs alongside: `just advisories-scan-diff` against
   the release baseline (see "Dependency advisory scanning" above). It
   uploads the release SBOM, `advisories-sbom.cdx.json` as the
   `sbom-<tag>` artifact: one CycloneDX file per release covering both
   arches, every shipped component and no GitHub Actions. The tag and its
   `ignores.toml` are fixed, so re-running a failed gate clears only a
   transient scanner or network error (exit 3); a release that fails on a
   finding, or on a baseline missing one of the config's input files, ships
   only through a new release carrying the fix or an ignore entry.
2. `release-gate` — fires when `validate-docker` and `advisories-gate`
   both succeed, so neither the image push nor the docs deploy happens
   on a new advisory.
3. Publish jobs run after the gate:
   - `publish-docker / {amd64, arm64}` — rebuilds the per-arch
     image, extracts the `.debug` file via
     `just bazel-docker-extract-debug ${arch}`, pushes via
     `just bazel-docker-push-${arch}`, pushes debug symbols to
     Sentry. It does not repeat the smoke test —
     `validate-docker` already validated the same commit.
   - `manifest` — needs `publish-docker`; runs
     `just bazel-docker-publish-manifest` (`crane index append`) to
     assemble the multi-arch manifest at `daschswiss/sipi:v<version>`
     from the two pushed per-arch digests, and tags it `:latest`.
     Provisioned via `imjasonh/setup-crane` + `extractions/setup-just`
     only — no Bazel/RBE setup, since the recipe only shells out to
     `crane`.
   - `sentry` — also needs `publish-docker` directly and runs in
     parallel with `manifest` (not after it), finalising the Sentry
     release.
   - `docs` — needs `release-gate`; mkdocs deploy.

## Remote build execution and caching

CI builds run on a self-hosted **NativeLink Remote Build Execution** backend: a
single x86_64 worker that cross-compiles all three target arches
(`linux-amd64`, `linux-aarch64`, `darwin-aarch64`), fronting a remote cache
(AC + CAS, `:50051`, mTLS) and a `bazel-remote` download cache (`:50052`) for
`http_archive` source tarballs. The connection, mTLS, and
cross-compile flags are assembled by the
[`bazel-rbe`](../../../.github/actions/bazel-rbe) composite action and injected
per workflow step — Bazel does not expand env vars in `.bazelrc`, so they live
in the workflow, not the rc file. The backend-agnostic tuning flags
(`--remote_download_minimal`, `--remote_local_fallback`, `--remote_timeout`,
`--remote_max_connections`) live in `.bazelrc` and are safe no-ops without a
remote configured.

See **[Remote build execution](./rbe.md)** for the full topology, the
cross-compile flag rationale, and the developer-facing workflow, and the
[`ops-tf`](https://github.com/dasch-swiss/ops-tf) /
[`infra`](https://github.com/dasch-swiss/infra) repos (private) for the
VM provisioning and the NativeLink service configuration.

## Local reproduction

Every CI step invokes `just <recipe>` — there are no inline
`bazel ...` calls in any workflow. CI itself runs Nix-free (see
"Pull request CI" above); locally, Nix still provisions the dev
shell. To reproduce any CI job locally, run the same recipe inside
`nix develop`:

```bash
nix develop

# Full PR test job, one arch (matches `ci.yml test`)
just bazel-test
just bazel-test-smoke

# Coverage (matches `coverage.yml`; needs the .#llvm-tools shell locally)
nix develop .#llvm-tools --command just bazel-coverage

# Sanitizer unit + e2e (matches the `sanitizer` job in ci.yml)
just bazel-test-sanitized --config=asan --config=ubsan

# Docker image with split debug symbols (what publish.yml does)
just bazel-docker-build-amd64
just bazel-test-smoke
just bazel-docker-extract-debug amd64
just bazel-docker-push-amd64
just bazel-docker-publish-manifest
```

**CI invokes justfile only.** If a CI step is not a `just <recipe>`
invocation, that's a drift signal — either the step is non-build
glue (e.g. artifact upload, Codecov upload, Sentry push) or the
justfile is missing a recipe and should grow one.

## Fuzz testing

Workflow: `.github/workflows/fuzz.yml`. Trigger: `schedule` at `17 3 * * *`
(03:17 UTC) plus manual `workflow_dispatch`; the repo's other scheduled
workflow is `advisories.yml` (05:41 UTC). One job on `ubuntu-24.04`: it
restores the previous nightly's working corpus from the `fuzz-corpus`
artifact, builds the instrumented
libFuzzer binary with `just bazel-build-fuzz` (RBE-eligible, `bep-fuzz`
artifact attached like the other legs), fuzzes
`//src/iiifparser/fuzz:parse_request_fuzz` for 600s, minimizes and re-uploads
the corpus, then runs a 300s ASan-paired pass.

Linux-amd64 only — `--config=fuzz` cannot link the libFuzzer runtime on
darwin, exactly like the sanitizer gate. macOS coverage is the corpus-replay
mode of the same target, which rides along in the `//src/...` test sweeps on
every PR.

A crash makes libFuzzer exit 77, which fails the job; the reproducers and
logs upload as a `fuzz-crashes` artifact and triage is manual. See
[Fuzzing](fuzzing.md) for the harness, the corpus policy, and the recipes.
