---
title: "Advisory scanning and fix PRs over the Bazel dependency graph"
date: 2026-10-30
author: "Ivan Subotic"
status: implemented
repositories: []
---

# Advisory scanning and fix PRs over the Bazel dependency graph

## Overview

Replace Docker Scout and the Rust-only `dependency-audit` job with one advisory
pipeline whose input is the Bazel dependency graph, not the built image. A script
derives a CycloneDX inventory of everything Vitrinli ships from the files Bazel
resolves against (`Cargo.Bazel.lock`, `MODULE.bazel.lock`, the `http_archive`
pins, `bazel/distroless/bookworm.lock.json`, the `oci.pull` digest). A scanner
matches it against advisory databases on every PR (new findings only), daily on
`main` (one rolling issue), and at release (new findings since the previous
release). Where a fix is a mechanical pin bump, the daily run opens the fix PR.

The same inventory covers the GitHub Actions the workflows run, and records
end-of-life dates. Scope is advisory-driven only: a PR opens when a known
vulnerability hits a pinned component, and a component past its end-of-life date
is a finding. Routine "newer version exists" bumps are a non-goal. The inputs
come from a per-repository config, so the scripts carry nothing specific to
Vitrinli.

## Problem Statement / Motivation

- **Image scanning sees the wrong layer.** Docker Scout scans the distroless
  image. The C/C++ libraries (libtiff, exiv2, lcms2, libpng, OpenSSL, curl,
  zlib, …) and every Rust crate are statically linked into one binary that
  carries no package metadata, so Scout cannot see them; the comment at the
  `publish.yml` CVE gate already says so. It sees only the Debian packages of
  the base image and the `rules_distroless` layer, filtered to Critical/High.
- **Dependabot never understood the graph.** Its `bazel` entry opened none of
  72 Dependabot PRs; its `cargo` entry pointed at `/test/e2e`, which has no
  manifest. It is now removed.
- **Coverage today is Rust crates only.** `just audit` (cargo-audit and
  OSV-Scanner over `Cargo.Bazel.lock`) is the only automated feed. BCR C/C++
  modules, the `http_archive` libraries, FFmpeg, the Debian packages and the
  distroless base rely on a quarterly manual review.
- **A new advisory blocks every PR.** `dependency-audit` fails on any matched
  advisory, so an advisory published against `main` turns every open PR red
  (RUSTSEC-2026-0387 did exactly this).
- **The release gate runs after the push.** The Scout CVE gate in
  `publish.yml` runs after "Push image to Docker Hub", so a failing gate
  reports on an image that is already published.
- **Pins drift silently.** The distroless digest dates from 2026-05-08 and the
  Debian snapshot from 2026-01-01; nothing reports when they carry a fixed
  vulnerability.
- **The image carries a curl stack nobody uses.** `bookworm.yaml` installs
  Debian `curl`, which pulls in 31 packages, for an ops-deploy healthcheck that
  has since moved to `sipi health` (ops-deploy and dsp-api both probe with
  `/sbin/sipi health`). 29 of the lock's 33 packages per arch leave with it
  (gnutls, krb5, ldap, ssh2, rtmp, sasl, libssl3); tini keeps libc6, libgcc-s1
  and gcc-12-base. `libssl3` stays in the image through the distroless base.
- **Ignores live in two places** (cargo-audit flags in the `justfile`,
  `[[IgnoredVulns]]` in `osv-scanner.toml`) and never expire.
- **An unsupported version looks clean.** Scanners match known advisories, and
  an advisory may not assess versions past end of life. Debian 12 left regular
  security support on 2026-07-12, and the distroless
  project now publishes Debian 13 images only, so `base-debian12` gets no new
  digests.
- **Pinned Actions are unwatched.** Dependabot alerts are on, but GitHub raises
  none for actions pinned by commit SHA, which is how most of `.github/` pins
  them. These workflows hold `DASCHBOT_PAT`, the Docker Hub token and the
  NativeLink mTLS client key, and a compromised action reaches all of them.

## Proposed Solution

1. **Inventory from Bazel inputs.** `tools/advisories/inventory.py` emits
   `advisories.cdx.json` (CycloneDX 1.6), one component per shipped
   dependency, sorted for stable diffs:

   | Source | Component identity |
   |---|---|
   | `Cargo.Bazel.lock` | `pkg:cargo/<name>@<version>` |
   | `MODULE.bazel.lock` `registryFileHashes` keys `…/modules/<name>/<version>/source.json` (the file has no resolved-module list; its `…/MODULE.bazel` keys include MVS candidates) | upstream version (`.bcr.N` stripped) plus the CPE from the component map |
   | `http_archive` pins (exiv2, jansson, mimalloc, libexpat, jbigkit, lcms2, tiff, both FFmpeg archives; tracy is `not_shipped`) | version parsed from the scanned tree's `url` with the entry's `version_re`; CPE from the component map |
   | `bazel/distroless/bookworm.lock.json` (both arches) | `pkg:deb/debian/<name>@<version>?arch=<arch>&distro=debian-12`, distro mapped from the `bookworm` codename in `bookworm.yaml` (an unknown codename exits 3) |
   | `oci.pull` `distroless_base` digest | the image reference by digest, scanned without a build |
   | `uses:` refs in `.github/workflows/*.yml` and `.github/actions/*/action.yml` (local `./` actions skipped) | `pkg:githubactions/<owner>/<repo>@<version>`, a SHA or tag resolved to its release tag; a SHA with no tag is listed as unresolved |

   The **component map** `tools/advisories/components.toml` is the one
   hand-maintained classification. Every BCR module, `http_archive`, `git_override`
   and custom repository rule (`kakadu_extension`, `approvaltests_cpp`,
   `benchmark_fixtures_extension`) appears exactly once per `(source, name)`, as
   `shipped` (with CPE; for `http_archive`, a `version_re`), `not_shipped` (build
   tooling, dev-only, test-only; with reason) or `no_feed` (Kakadu: proprietary,
   no public advisory feed; with reason). The key includes the source because
   BCR `mimalloc` arrives through `llvm` and is not linked, while the
   `http_archive` `mimalloc` is. The map holds no versions: the URL is the only
   version record, so a scanned tree always reports its own. A version copied
   into the map would label the base worktree with the head's version, and a PR
   bumping a pin to a vulnerable release would diff clean. The **drift check**
   fails on: an unclassified module, `http_archive`, `git_override` or custom
   repository rule; an `http_archive` URL that `version_re` does not match; a
   map entry for a dependency no longer in MODULE.bazel; a duplicate entry.
   Adding a dependency without classifying it breaks CI.

   The **repo config** `tools/advisories/advisories.toml` names the inputs: the
   lockfile paths, the deb manifest and lock, the `oci.pull` repository and the
   tag its pin tracks, the workflow globs and the issue label. No script
   hard-codes them, so dsp-app adopts the tools with its own config instead of
   a diverging copy. Where the shared scripts live across repositories is a
   follow-up; this plan keeps them repo-agnostic.

   **End of life.** A map or config entry may carry `eol = <date>`, recorded on
   the component. From 90 days before, the rolling issue lists it under
   "Approaching end of life"; from the date on, the scan reports a finding with
   the id `EOL`, keyed `(component, EOL)` like any advisory. A PR that adds an
   end-of-life component therefore fails, while an existing one stays with the
   daily issue. A config `eol` yields one finding for its source (the Debian
   release, the base image), not one per package. `ignores.toml` accepts `EOL`
   with the component and an expiry, like any id. `eol` is the end of regular
   upstream support. LTS and paid extended support do not count: they serve
   software that cannot move, and a component with a safe, supported successor
   moves to it. Initial dates: Debian 12 2026-07-12; the distroless base its
   end under the distroless support policy. Both are past, so the first daily
   run reports them, and H3 ignores them with an expiry until the move to
   `base-debian13` (DEV-7785), a separate change rather than a pin bump.
   `fix.py` never acts on an `EOL` finding.

2. **Scan.** `just advisories-scan` runs the scanner(s) chosen in Phase 3 over
   the inventory and the base image digest, **without the scanners' own ignore
   mechanisms and with JSON output**. Vitrinli's scripts then apply one
   scanner-agnostic ignore file, `tools/advisories/ignores.toml` (advisory id,
   component, reason, expiry date; an expired entry stops suppressing, so its
   finding returns), and decide
   pass or fail themselves. That keeps one ignore list whichever scanner wins,
   and enforces expiry, which Grype's ignore rules cannot express.
   cargo-audit and `osv-scanner.toml` are dropped.

3. **Where it runs.**
   - **PR** (`advisories` job, replaces `dependency-audit`): scans the PR merge
     commit and the current tip of its base branch without ignores (the head's
     tools run against a base worktree; base-side drift is tolerated), takes head
     minus base keyed by `(component name, advisory id)` (never by version), then
     applies `ignores.toml` from the head once. Fails on a non-empty result, on
     any scanner error, and on a drift-check failure. A PR that fixes a finding
     passes; an advisory published against `main` does not block unrelated PRs.
     Shared findings are subtracted before ignores apply, so an ignore expiring
     cannot redden unrelated PRs; the daily issue carries it. The same key means
     a PR adding a second vulnerable version of an already-flagged crate passes;
     the daily scan still reports the crate.
   - **Daily on `main`** (`.github/workflows/advisories.yml`): full scan, one
     rolling issue.
   - **Release** (`advisories-gate` job in `publish.yml`, needed by `release-gate`,
     so neither the image nor the docs deploy on a failure; once per release): the
     same diff, between the baseline tag and this tag (head tools against
     the tag's worktree; tags before this work have no `tools/advisories/`), so a
     release never introduces a vulnerable pin, and a backlog finding never blocks
     a hotfix. Uploads the shipped part of the inventory as the release SBOM;
     Actions components carry the property `advisories:scope=ci` and are left out. The baseline is the newest
     earlier tag whose image reached Docker Hub, not simply the previous tag: a
     tag that failed the gate shipped nothing, and diffing against it would wave
     its finding through on the next release. The tag and its `ignores.toml` are
     fixed, so re-running cannot clear a finding (exit 1); shipping takes a new
     release carrying the fix or an ignore entry. A scanner error (exit 3) is
     recovered by re-running the job.

4. **Fix PRs** from the daily run, opened with `DASCHBOT_PAT` so CI runs on
   them. One branch per component, `advisories/<component>`, one commit
   `fix(deps): raise <component> to <resolved> past <ids>` whose body names
   every advisory id and the resolved version.
   - Rust crate: precise repin
     (`CARGO_BAZEL_REPIN=<crate>@<locked>=<target>`, `target` the lowest
     version at or above the fix that the locked requirement allows), re-scan;
     a branch only if every advisory for the crate clears. A crate fixable only
     by a semver-major bump of a dependent is alert-only with the reason "needs
     a major bump of <nearest crate.spec dependent>", found by walking
     `Cargo.Bazel.lock` reverse dependencies.
   - BCR module: lowest non-yanked BCR version at or above the fix, compared
     with Bazel's module version ordering (not semver), lock refreshed. A
     `single_version_override` holding the module is bumped with its `bazel_dep`
     (openssl pins both); one carrying `patches` is alert-only, for the reason
     native bumps are.
   - Distroless base: current index digest of
     `gcr.io/distroless/base-debian12:latest` (the tag the pin tracks).
   - Debian packages: snapshot date moved to the scan date, lock regenerated.
   - `http_archive` libraries, FFmpeg, and findings with no released
     fix: **alert only**. ADR-0015 makes every native bump a re-derivation of
     `defines`, and exiv2 carries a patch; a URL and sha256 swap is not a safe
     proposal.

   `fix.py` skips `EOL` findings and every Actions component, and judges
   clearing over the remaining advisories, so a permanent `EOL` cannot block a
   real fix. A target at or below the current version, or a change that leaves
   the diff empty or the advisories uncleared, opens no PR for any class; the issue lists
   the finding as "at or past the fix, still flagged; needs an ignore". CPE false
   positives and returning expired ignores reach this path.

   Lifecycle: a fix branch is always rebuilt from `origin/main`, never rebased.
   An open PR is rebuilt and pushed with `--force-with-lease` only when its
   advisory set or target version changed; being behind `main` alone triggers
   nothing, because every merge would otherwise restart CI on every fix PR. A
   branch holding more than the job's one commit over its merge base is left
   alone and the issue says so, because a maintainer's follow-up (a config
   header, an MVS override) must survive. A branch without a PR is deleted and
   rebuilt. A fix PR closed unmerged suppresses a new PR while the component's
   advisory set equals the ids in that PR's body, read through `gh pr list
   --state closed --head advisories/<component>`; the issue then asks for an
   ignore entry. A concurrency group serializes runs.

Docker Scout leaves `ci.yml` and `publish.yml`, with the PR-comment and SARIF
permissions and the Scout Docker Hub organisation/environment use. The Docker
Hub credentials stay; the push needs them.

## Alternative Approaches Considered

- **Self-hosted Renovate** (`bazel-module` manager). Bumps `bazel_dep`,
  `oci.pull` and direct `crate.spec`, but its advisory mode
  (`osvVulnerabilityAlerts`, experimental) covers direct dependencies only.
  Every crate advisory Vitrinli has hit (rsa, crypto-bigint, block-buffer,
  rustls-webpki, quinn-proto) was transitive. It does not parse `http_archive`
  in MODULE.bazel, cannot recompute sha256s, and needs a GitHub App. It solves
  routine currency, a non-goal. Rejected; the option if routine bumps come
  into scope.
- **GitHub dependency submission API.** Submitted SBOMs raise Dependabot alerts
  only for GitHub Advisory Database ecosystems: crates yes, `pkg:generic` C
  libraries and BCR modules no, and no fix PRs for a graph Dependabot cannot
  edit. Rejected.
- **Bazel-native SBOM rules** (`rules_license` `write_sbom`,
  `eclipse-score/sbom-tool`). Young or experimental, and they still need the
  same name-to-CPE map for C libraries. Rejected for a script over the
  lockfiles.
- **Image scanning with Trivy and Syft** (the replacement direction recorded
  for Scout during the Bazel migration). Same blind spot as Scout: the image
  carries no metadata for the statically linked code. Rejected; see Prior
  decisions.
- **Scanning the linked binary** (`cve-bin-tool`). Fingerprints version
  strings, misses lcms2, Kakadu and backports, and finds nothing the inventory
  does not. The drift check covers the pin side. Rejected.

## Prior decisions

This plan reverses three recorded decisions.

- **Security hardening wave 2, finding S2-30 (Phase 9).** Kept Docker Scout as
  the image-layer gate and made the release scan fail on Critical, "image layers
  only", because "the only CVE gate in CI (Docker Scout) scans
  image layers and cannot see the statically linked C libraries" and "does not
  fail the build". The reason for keeping Scout was that nothing else saw the
  Debian layers. What changed: the inventory reads those layers from their
  Bazel inputs (`bookworm.lock.json`, the base digest), so Scout's only unique
  coverage moves into the pipeline, and the release gate moves before the push.
  The same phase created `just audit` and the quarterly native-pin review, which
  this plan replaces; their reason, that nothing else covered those surfaces,
  stops holding once the inventory does.
- **Security hardening wave 2, finding S2-41.** Pinned third-party actions by
  commit SHA because "Dependabot keeps them fresh". Dependabot is removed, so
  the freshness half no longer holds. SHA pins keep their protection against
  tag hijacking; freshness of action pins becomes a routine-currency concern,
  a non-goal here. Advisories against pinned actions are covered by the
  inventory, which Dependabot never did for SHA pins.
- **Bazel migration plan, Future Considerations Y+9 and Y+10, with their Scout
  replacement research note.** Planned to swap
  Scout for Trivy (image CVE scan, SARIF diff against production) and Syft
  (image SBOM), because Trivy was "the cleanest single-tool replacement for
  Scout's CVE scanning" and "no production-grade Bazel rule emits a
  container-image SBOM into the build graph". What changed: the target is no
  longer the image. Trivy and Syft inherit Scout's blind spot for statically
  linked code, while the lockfiles carry exact versions for everything. Y+11
  (cosign image signing and attestation) is untouched and stays a separate
  follow-up; it can attest the inventory produced here.

ADR-0029 records these reversals so they outlive this plan.

## Technical Considerations

- **Scanner choice is decided by evidence in Phase 3.** Known before the spike:
  OSV-Scanner reads CycloneDX by filename (`*.cdx.json`), matches purls, and
  scans images through the `docker` CLI or from an exported archive
  (`--archive`), so a Docker-less job needs `crane pull` to a tarball first; exit
  codes are 0 clean, 1 findings, 127 general error, 128 no packages found. Grype
  reads CycloneDX, matches CPE-only components by default
  (`match.stock.using-cpes: true`; the `dpkg` and `rust` matchers default to
  false, so deb and crate purls are never CPE-matched), scans `registry:` images
  without a daemon, needs `--distro debian:12` (or a parsed purl `distro`
  qualifier) for deb matching, refuses a database older than 120h
  (`db.max-allowed-built-age`), and its ignore rules have no expiry. Unverified
  and settled by the fixture: CycloneDX 1.6 parsing, whether OSV-Scanner matches
  CPE-only components, the exact deb qualifier each scanner honours, Grype's
  `@<digest>` reference, exit codes and database date field. Rule: one
  scanner if it finds the fixture's vulnerable component in all four classes
  (cargo, deb, CPE C library, base image); otherwise OSV-Scanner for cargo and
  deb plus Grype for CPE and the base image, never more than two. If no
  candidate matches CPE C libraries, that class produces no findings and rests on
  the pin table, and the ADR records it. Committed (ADR-0029, provisional
  until the owner answers): OSV-Scanner for cargo only, Grype for deb, CPE,
  the base image and Actions, because Grype missed two real crate advisories
  and OSV-Scanner matches a deb only under its source name.
- **Scanner invocation.** Each scanner runs with JSON output, without
  `--fail-on` and without its own ignore config. The wrapper interprets exit
  codes per scanner (OSV-Scanner: 0 and 1 are results, anything else is an
  error, including 128 for an empty or misnamed inventory; Grype: non-zero is an
  error, including a stale database) and reads findings from the JSON.
- **Tool conventions** (`tools/advisories/`). Python 3.11 or newer (for
  `tomllib`), stdlib only, run as `python3 tools/advisories/<tool>.py`, the part
  `tools/loadtest/loadgen.py` sets as precedent. Shared dataclasses and the version check in
  `_model.py`. `argparse`, `main(argv) -> int`. Exit codes: 0 clean, 1 findings
  or drift, 2 usage, 3 tool or scanner error; exceptions are caught only in
  `main`, mapped to 3, diagnostics to stderr, so a crash never reads as
  findings. TOML and JSON through `tomllib` and `json`; MODULE.bazel
  (Starlark) through tested regexes that exit 3 on no match; the same for
  `MODULE.bazel.lock` when no `source.json` key matches. The hard cases are real:
  FFmpeg's `build_file_content` has a column-0 `)` before its `url`, and
  `http_archive` also appears in comments. Output sorted. `today` is a parameter,
  `main` passes the UTC date and tests a fixed one, so a fixture ignore cannot
  expire and fail every PR. `expiry` is a bare TOML date; lint rejects a quoted
  one, which would otherwise crash at comparison. Fixture files avoid the names
  `BUILD` and `MODULE.bazel`, which would change Bazel package boundaries. Tests in
  `tools/advisories/tests/test_*.py`, offline, fixtures under
  `tools/advisories/tests/fixtures/`, run by
  `python3 -m unittest discover -s tools/advisories/tests -t tools/advisories`.
- **CPE results are weak evidence.** NVD CPE ranges are noisy for C libraries
  and ignore distro backports. Expect false positives; triage is an expiring
  ignore. "No finding" for a C library is weaker than for a crate; the docs say
  so. A CPE miss produces no alert at all, so the rolling issue also lists
  a **pin table**: every `http_archive`, FFmpeg, Kakadu and GitHub Actions component with its
  pinned version and the date its pin last changed
  (`git log -1 --format=%cs -S<pin> -- <file>`; `MODULE.bazel` URLs; Kakadu's
  version and date come from the `tag` in `bazel/kakadu_extension.bzl`; a
  floating tag ref such as `@v4` gets no date; actions dedupe on `(repo, ref)`), which
  replaces the quarterly review as the human glance.
- **BCR versions** carry `.bcr.N` (`curl 8.21.0.bcr.1`); the inventory strips
  it for matching. A BCR repack that backports a fix is an ignore with that
  reason. BCR candidates come from
  `https://raw.githubusercontent.com/bazelbuild/bazel-central-registry/main/modules/<name>/metadata.json`
  (`versions`, and `yanked_versions`, an object whose keys are the versions).
- **libtiff** comes from the libsdl-org mirror; its CPE stays
  `cpe:2.3:a:libtiff:libtiff`. **libexpat** is an `http_archive`, not the BCR
  module `CLAUDE.md` lists.
- **Crate repin and the lockfile.** `CARGO_BAZEL_REPIN` maps to cargo's
  update: `<crate>` updates that package, `<crate>@<locked>=<target>` pins it
  precisely (needed when a crate is locked at two versions). The repin is
  `bazelisk fetch @crates//:all` with the variable set, followed by the same
  fetch without it: the crate extension records `CARGO_BAZEL_REPIN` in
  `MODULE.bazel.lock`, and only the second fetch writes it back as unset.
  `.bazelrc` sets no `lockfile_mode`, so the default `update` applies.
- **Kakadu stays out of fix jobs.** `@crates`, `@bookworm` and `bazel mod deps`
  do not touch the Kakadu extension. Phases 8 and 9 verify each fix recipe runs
  with `GH_TOKEN` unset and `GH_CONFIG_DIR` at an empty directory, because
  `gh_release_archive` falls back to `gh auth token` (`bazel/gh_release.bzl`).
- **No `PATH` change in Bazel steps.** `action_env=PATH` puts `PATH` into every
  action key, so a changed `PATH` is a full remote-cache rebuild. Scans run in
  jobs without Bazel; the fix job gets its tools through
  `ADVISORIES_BIN`.
- **Python in CI.** Every job uses the `python3` of its `ubuntu-24.04` runner,
  checked by `_model.py`. The tools are stdlib only, so `setup-python` adds
  nothing, and in the fix job it would change `PATH`.
- **Pinned scanner installs.** The composite action
  `.github/actions/advisories-scanners` downloads each scanner, and `crane` when
  the chosen base-image scan needs it, by exact version into
  `$RUNNER_TEMP/advisories-bin`, verified against a sha256 as the current
  `osv-scanner` install does, and exports `ADVISORIES_BIN` without touching
  `PATH`. `scan.py` prefers `ADVISORIES_BIN` and falls back to `PATH` in the dev
  shell. One action because the PR job, the release gate and the daily job must
  run one scanner version; a pin bumped in only one would make them disagree.
- **`just bazel-*` invariant.** `advisories-*` recipes read files and run
  scanners; they never invoke Bazel. Recipes that do are named `bazel-*`
  (`bazel-distroless-lock`, `bazel-repin-crate`, `bazel-lock-update`).
- **GitHub mechanics.** Scheduled and dispatched runs only exist once the
  workflow file is on the default branch; pre-merge verification therefore
  runs the same Python entry points locally. Pushes and PRs made with
  `GITHUB_TOKEN` trigger no CI, so fix PRs use `DASCHBOT_PAT`. The release baseline is the next entry after the current tag in
  `git tag --list 'v*' --sort=-v:refname` for which
  `https://hub.docker.com/v2/repositories/daschswiss/sipi/tags/<tag>` returns 200. The PR job checks out with
  `fetch-depth: 0` and diffs against a worktree of
  `origin/${{ github.base_ref }}`, because a PR stacked on another branch would
  otherwise count the whole stack's findings as new. It takes no path filter: a
  filtered required check never reports and blocks merge (`ci.yml`, the note
  above the `changes` job).
- **Diffing against a base without the tooling.** `origin/main` before this
  work lands, and every release tag before it, have no `tools/advisories/`. The
  diff therefore runs the head's `inventory.py`, `components.toml` and scanner
  wrapper, with the head's `advisories.toml`, against the base worktree's
  lockfiles, in `--best-effort` mode: every drift class on the base side is
  skipped, head drift still fails. `diff.py` computes `today` once and passes it
  to both scans, so a run crossing midnight UTC on an `eol` date cannot give
  only the head an `EOL` finding.
- **Permissions.** Scan job: `contents: read`. Issue job: `issues: write`, `pull-requests: read` (open and declined fix PRs). Fix job:
  `contents: read`, `pull-requests: read`; on a writing run (the schedule, or a
  default-branch dispatch without `dry_run`) `gh`, pushes and PRs go through
  `DASCHBOT_PAT`, as the checkout `token` and as `GH_TOKEN` on the fix step only; a
  dry run or another branch's dispatch uses `github.token` (owner decision 2026-11-06).
- **Rolling issue.** Found by `gh issue list --label advisory --state open`
  with the fixed title "Open dependency advisories"; created when absent,
  closed when no unignored finding remains (an "Approaching end of life" entry
  alone does not keep it open), a new one opened next time. A separate issue job
  runs after the fix job with `if: !cancelled() && needs.scan.result == 'success'`,
  so a failing fix cannot leave the issue stale and a PR opened in a run appears
  in that run's issue; it reads the scan's JSON and, when the fix job finished,
  its outcomes from artifacts. The body carries the advisory-database date (Grype's
  if chosen; otherwise the scan time, since OSV-Scanner queries the live API), ignores expiring within 14 days, and the
  pin table, capped at 60 000 characters with a link to the run summary
  for the rest. A scanner or network error fails the job and leaves the issue
  untouched; only a successful clean scan closes it. `advisories.yml` creates
  the `advisory` label if missing.
- **Schedules.** `fuzz.yml` runs at 03:17 UTC; `advisories.yml` uses 05:41 UTC.
- **Notification is a GitHub issue,** not Linear: no API key secret needed.
- **Actions pins.** A SHA pin resolves to the most specific semver release tag
  (no prereleases) pointing at that commit, from
  `git ls-remote --tags https://github.com/<owner>/<repo>`: the peeled `^{}`
  commit for annotated tags, else the plain line for lightweight ones.
  `actions/checkout`'s pinned SHA carries both `v6` and `v6.1.0`, so it resolves
  to `6.1.0`. A tag ref such as `@v4` resolves the same way to its exact release,
  since the `# v6` comments name only a major. A sub-path action
  (`actions/cache/save`) maps to its repository. Step-level and job-level
  (reusable workflow) `uses:` are read; a branch ref (`@main`), a SHA with no
  tag (`mhausenblas/mkdocs-deploy-gh-pages`, pinned at a `master` commit) and a
  `docker://` ref are listed as unresolved, not failed. Actions inside an
  external composite action are out of scope. Globs match `*.yml` and `*.yaml`. OSV carries GitHub Actions
  advisories (ecosystem `GitHub Actions`, versions without the `v`, for example
  GHSA-mrrh-fwg8-r2c3); whether each scanner reads `pkg:githubactions` from
  CycloneDX is a Phase 3 fixture class. Findings are alert-only: freshness stays
  a non-goal, and a SHA bump changes code that runs with secrets, so a person
  reviews it. The lookup needs network: each repository is resolved once per
  run and shared by the head and base scans, with `GIT_TERMINAL_PROMPT=0` so a
  missing repository cannot hang on a credential prompt. A repository that is
  gone or private is listed as unresolved; a network error is retried and exits
  3 only when the retries run out. The offline tests use recorded `ls-remote`
  output.
- **Specs are a sink.** ADR-0029, the scripts, docs and comments reference
  code, the ADR and the glossary, never this plan.
- **Non-goals:** GitHub Actions pin freshness, Nix flake inputs (dev shell only), mkdocs
  and benchmark Python requirements (not shipped), toolchain modules, routine
  version bumps.

## Implementation Phases

#### Phase 1: Drop the Debian curl from the image

- [x] Add `just bazel-distroless-lock` (`bazel run @bookworm//:lock`)
- [x] Remove `curl` from `packages:` in `bazel/distroless/bookworm.yaml` and run `just bazel-distroless-lock` with `GH_TOKEN` unset and `GH_CONFIG_DIR` at an empty directory; the amd64 and arm64 package sets in `bazel/distroless/bookworm.lock.json` contain no `curl`, `libcurl4`, `libssl3` or `libgnutls30`; record the amd64 and arm64 package counts in the PR description
- [x] Rewrite the comments that name the image's Debian curl as the ops-deploy healthcheck probe (`bazel/distroless/bookworm.yaml`, the `apt` section of `MODULE.bazel`, the runtime-extras block in `src/BUILD.bazel`): the container healthcheck is `sipi health`; the BCR curl statically linked into sipi is unrelated and stays
- [x] Drop `curl` from `docs/src/development/downstream-dependencies.md` (the package table and the `knora-sipi` row)
- [x] Run `just bazel-build` on macOS; it passes (and `just bazel-test`, the macOS CI path: 78/78)
- [ ] Run `just bazel-coverage` on macOS; it passes (locally on macOS 27.0.1 instrumented test binaries segfault at exit, at the phase base too; CI's linux-amd64 coverage leg is the gate)
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 2: Inventory from Bazel inputs

- [x] Add `tools/advisories/components.toml` classifying every BCR module with a `source.json` key in `MODULE.bazel.lock` and every `http_archive`, `git_override` and custom repository rule in MODULE.bazel as `shipped` (with CPE and, for `http_archive`, `version_re`), `not_shipped` or `no_feed` (with reason)
- [x] Add `tools/advisories/advisories.toml` naming this repository's inputs (lockfile, deb and Kakadu extension paths, the `oci.pull` repository and its tracked tag `gcr.io/distroless/base-debian12:latest`, workflow globs, issue label); every tool reads paths and references from it
- [x] Support an optional `eol` date on map and config entries; set Debian 12 to 2026-07-12, the end of regular security support (never an LTS date), and the distroless base to its end date under `SUPPORT_POLICY.md` in GoogleContainerTools/distroless (check both at write time)
- [x] Add `tools/advisories/_model.py` (Python version check, `Component` dataclass) and `tools/advisories/inventory.py` emitting a sorted `advisories.cdx.json` from `Cargo.Bazel.lock`, `MODULE.bazel.lock`, `MODULE.bazel` (`http_archive` pins, `oci.pull` digest), the component map and `bazel/distroless/bookworm.lock.json`, with the deb distro qualifier derived from `bazel/distroless/bookworm.yaml`, plus one `pkg:githubactions` component per non-local `uses:` ref, resolved through `git ls-remote --tags`
- [x] Implement the drift check in `inventory.py` (with a `--best-effort` flag that skips every drift class instead of failing, for scanning old bases): unclassified module, `http_archive`, `git_override` or custom repository rule, URL not matching `version_re`, stale map entry, duplicate entry
- [x] Add `tools/advisories/tests/__init__.py` and `tools/advisories/tests/test_inventory.py` with fixtures under `tools/advisories/tests/fixtures/`: one test per drift case, the `.bcr.N` strip, `source.json` keys chosen over `MODULE.bazel` candidates, both `mimalloc` sources, the FFmpeg blocks and commented `http_archive` lines verbatim, a base tree reporting its own `http_archive` version, every input path taken from a fixture `advisories.toml`, `eol` carried onto the component, `uses:` refs resolved from recorded `ls-remote` output (SHA pin with two tags, lightweight tag, tag ref, sub-path action, job-level `uses:`, branch ref, `docker://` ref, unresolved SHA, missing repository), the exact deb PURL with the derived distro, golden CycloneDX compared as parsed JSON
- [x] Add `just advisories-inventory` and `just advisories-test`; gitignore `advisories.cdx.json`
- [x] Correct `CLAUDE.md`'s dependency list: libexpat is an `http_archive`, not a BCR module
- [x] Run `just advisories-test`; it passes. Run `just advisories-inventory`; it emits a component for every `shipped` map entry, every crate in `Cargo.Bazel.lock` except the synthetic `direct-cargo-bazel-deps`, every package (both arches) in `bookworm.lock.json` and the base-image digest
- [x] Run `just bazel-build` on macOS; it passes (justfile changed; and `just bazel-test`, the macOS CI path: 78/78)
- [ ] Run `just bazel-coverage` on macOS; it passes (locally on macOS 27.0.1 instrumented test binaries segfault at exit, at the phase base too; CI's linux-amd64 coverage leg is the gate)
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 3: Scanner choice and ADR

- [x] Add a fixture inventory `tools/advisories/tests/fixtures/spike.cdx.json` with one known-vulnerable component per class: an old crate, an old deb with the derived distro qualifier, libtiff and curl at versions with published CVEs, `tj-actions/changed-files` 45.0.0 (GHSA-mrrh-fwg8-r2c3); and record an older `distroless/base-debian12` digest for the base-image class
- [x] Download pinned OSV-Scanner and Grype release binaries (version and sha256 checked) into a scratch directory outside the repo, and run them against the fixture and the old base digest (network required; spike evidence, not part of `just advisories-test`); record per class which scanner reports the expected advisory, the deb qualifier each honours, whether Grype accepts `@<digest>`, and Grype's exit codes and database date field; apply the decision rule
- [x] Write `docs/adr/0029-advisory-scanning-from-the-bazel-graph.md` (re-check the number at write time): inventory over image, the chosen scanner(s) with the per-class fixture results, the alert-only classes, the reversals named in Prior decisions with their original reasons and what changed, the rejected alternatives
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 4: Scan recipe and ignore consolidation

- [x] Add `tools/advisories/scan.py`: runs the chosen scanner(s) without ignores and with JSON output, interprets exit codes per scanner, normalizes findings to `(component name, advisory id, aliases, severity, fixed version)`, adds an `EOL` finding for each component past its `eol`, lints `ignores.toml` on load and applies it
- [x] Add `tools/advisories/ignores.toml` with the four current entries (RUSTSEC-2023-0071, GHSA-w9wp-h8wv-79jx, RUSTSEC-2026-0387, GHSA-qwgh-2vcv-g2f7), each with component, reason and expiry date
- [x] Add `tools/advisories/tests/test_scan.py`: normalization from recorded scanner JSON fixtures, alias matching, an expired ignore stops suppressing at a fixed `today`, an `EOL` finding on the `eol` date and none the day before, one `EOL` per config source, an ignored `EOL`, ignore-file lint (non-empty reason, expiry a bare TOML date), scanner exit codes per scanner (OSV-Scanner 0 and 1 are results, anything else exits 3; Grype non-zero exits 3)
- [x] Add `just advisories-scan` (inventory, then `scan.py`); exit codes 0, 1 or 3 as in Technical Considerations
- [x] In `flake.nix`, add the chosen scanner(s) to the dev shell, so `just advisories-scan` runs locally
- [x] (from Phase 3) Let a component map entry carry several CPEs and emit the extra ones as `syft:cpe23` properties, which Grype reads; give curl both `cpe:2.3:a:haxx:curl` and `cpe:2.3:a:haxx:libcurl` (NVD files CVE-2023-38545 under libcurl only)
- [x] (from Phase 3) In `scan.py`, exit 3 when the SBOM handed to Grype holds no components (Grype exits 0 on an empty SBOM), and scan the base image index once per arch with `--platform`
- [x] Run `just advisories-test` and `just advisories-scan`; the test passes and the scan reports the baseline (counts in the journal's Phase 4 side findings, for the PR description)
- [x] Run `just bazel-build` on macOS; it passes (justfile and flake.nix changed; and `just bazel-test`, the macOS CI path: 78/78)
- [ ] Run `just bazel-coverage` on macOS; it passes (locally on macOS 27.0.1 instrumented test binaries segfault at exit, at the phase base too; CI's linux-amd64 coverage leg is the gate)
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 5: PR gate

- [x] Add `tools/advisories/diff.py` and `just advisories-scan-diff <base-ref>`: runs the head's inventory, map and scanner wrapper against `<base-ref>` in a temporary worktree and against the working tree, without ignores (head-side drift exits 1; base-side drift is tolerated through `inventory.py --best-effort`); keys findings by `(component name, advisory id)`, applies the working tree's `ignores.toml` to head minus base; exit 1 on a non-empty result, 3 on a scanner error
- [x] Add `tools/advisories/tests/test_diff.py`: shared finding passes, head-only finding fails, finding fixed at head passes, version bump of an already-vulnerable component passes, ignored head-only finding passes, a shared finding whose ignore expired passes, a pin bumped to a vulnerable version fails, a base without `tools/advisories/` is scanned best-effort, head drift exits 1 while base drift does not, a scanner error on either side exits 3
- [x] Add `.github/actions/advisories-scanners`: the chosen scanner(s), and `crane` if the base-image scan needs it, pinned by version and sha256 into `$RUNNER_TEMP/advisories-bin`, exporting `ADVISORIES_BIN`
- [x] Replace the `dependency-audit` job in `.github/workflows/ci.yml` with an `advisories` job, no path filter (`fetch-depth: 0`, `setup-just`, `advisories-scanners`) running `just advisories-test` and `just advisories-scan-diff origin/${{ github.base_ref }}`
- [x] Remove `just audit`, its justfile comments and the cargo-audit flags; delete `osv-scanner.toml`
- [x] (from Phase 4) Delete `test_osv_scanner_toml_mirrors_the_repo_ignore_ids` from `tools/advisories/tests/test_scan.py` with `osv-scanner.toml`, and the mirror note it enforces
- [x] In `flake.nix`, remove `cargo-audit`, and `osv-scanner` if it was not chosen, from the dev shell, and rewrite the comment above them
- [x] Drop the `osv-scanner.toml` glob from `ARCH-MAP.md` through `dune:dune-map`
- [x] Point the `cargo_lockfile` comment in `MODULE.bazel` at `just advisories-scan` instead of cargo-audit
- [x] Rewrite "Dependency advisory scanning" in `docs/src/development/ci.md`: the pipeline, how to classify a component in `components.toml`, how to write an ignore with expiry, how to run the scan locally, that a clean CPE result is weaker evidence than a clean crate result, how to set `eol`, that Actions findings are alert-only, and that Dependabot alerts stay on but miss SHA pins; replace every mention of `dependency-audit`, `just audit` and cargo-audit
- [x] Run `just bazel-build` and `just bazel-test` on macOS; both pass
- [ ] Run `just bazel-coverage` on macOS; passes (unenforceable locally: instrumented binaries segfault at exit on macOS 27.0.1, at the phase base too; CI's coverage leg is the gate)
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 6: Remove Docker Scout and gate releases

- [x] Add an `advisories-gate` job to `.github/workflows/publish.yml` (`fetch-depth: 0`, `setup-just`, `advisories-scanners`), running once per release and added to `release-gate`'s `needs`: resolve the baseline per Technical Considerations (a tag that never reached Docker Hub is skipped; tested in `test_diff.py`), run `just advisories-scan-diff <baseline tag>`, upload `advisories.cdx.json` without the `advisories:scope=ci` components as the release SBOM artifact
- [x] Delete the Scout `environment`, `cves` and `sbom` steps, the SBOM upload and the matrix `sbom_file`/`sbom_name` keys from `publish.yml`
- [x] Delete both Scout steps, the SARIF upload, the `pull-requests: write` and `security-events: write` permissions and the Scout comments from `.github/workflows/ci.yml`
- [x] Remove the Scout notes from `justfile` comments (image-in-daemon note, version-based steps list)
- [x] Remove the "Docker Scout warm-cache fix" section and the Scout steps from `docs/src/development/rbe.md`
- [x] Replace every Scout mention in `docs/src/development/ci.md` (test-matrix list, publish gate model, coverage-gap paragraph); state that the release SBOM is one CycloneDX file per release covering both arches, and that a failed gate ships only through a new release
- [x] In `RELEASING.md`, name `advisories-gate` and move SBOM generation to it
- [x] Run `grep -rni scout .github justfile docs/src`; it returns nothing
- [x] Run `nix run nixpkgs#actionlint -- .github/workflows/publish.yml` and `just advisories-scan-diff <latest tag>` locally; both complete, since `publish.yml` otherwise first runs on a real tag
- [x] Run `just bazel-build` and `just bazel-test` on macOS; both pass (justfile changed)
- [ ] Run `just bazel-coverage` on macOS (unenforceable locally: macOS 27.0.1 coverage segfault, pre-existing; CI's linux-amd64 coverage leg is the gate)
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 7: Daily scan and rolling issue

- [x] Add `tools/advisories/report.py` rendering the issue body: findings (component, advisory id, severity, fixed version), the advisory-database date, ignores expiring within 14 days, an "Approaching end of life" section for `eol` within 90 days, the pin table; capped at 60 000 characters
- [x] Add `tools/advisories/tests/test_report.py` over recorded scan fixtures, including the cap
- [x] Add `.github/workflows/advisories.yml`: cron `41 5 * * *`, `workflow_dispatch` with a `dry_run` input, concurrency group `advisories` (no cancel-in-progress), a scan job (`contents: read`) and an issue job (`issues: write`, `pull-requests: read`), `fetch-depth: 0` (the pin table reads history), `setup-just`, `advisories-scanners`; runs `just advisories-scan` (exit 1 leaves the job green, the issue being the signal; exit 3 fails it) and uploads the scan JSON as an artifact; the issue job, skipped when the scan failed, runs `report.py`, finds the issue by the `advisory` label and the fixed title, creates the label if missing, creates, updates or closes the issue; `dry_run` writes the body to the job summary instead
- [x] Rewrite the scheduling note above the cron in `fuzz.yml`, which says no other workflow is scheduled, to name both workflows and their times
- [x] Run `just advisories-scan` and `python3 tools/advisories/report.py` locally on the scan output; the body renders with the baseline findings
- [x] Describe the daily workflow and the rolling issue in `docs/src/development/ci.md`, and drop the daily scan from ADR-0029's "Not yet in code" line
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 8: Fix PRs for Rust crates

- [x] Add `just bazel-repin-crate <spec>`: `CARGO_BAZEL_REPIN=<spec> bazelisk fetch @crates//:all`, then the same fetch without the variable
- [x] Add `tools/advisories/fix.py` crate path: per component, choose the target version, repin with `<crate>@<locked>=<target>`, re-scan, keep the change only when every advisory for it clears; otherwise list it as "at or past the fix, still flagged; needs an ignore" when the target is not above current, or classify it alert-only (no released fix, or "needs a major bump of <dependent>" via `Cargo.Bazel.lock` reverse dependencies)
- [x] Implement the branch lifecycle in `fix.py`: `advisories/<component>` naming, rebuild from `origin/main`, `--force-with-lease` for an open PR whose advisory set or target changed, leave alone a branch holding extra commits, recreate a branch without a PR, suppress a new PR after an unmerged close while the advisory set equals the ids in the closed PR's body
- [x] Add a `fix` job to `advisories.yml` (`needs` the scan, reads its JSON from an artifact, checkout with `fetch-depth: 0` and `token: secrets.DASCHBOT_PAT`, `GH_TOKEN: secrets.DASCHBOT_PAT`, `ci-setup`, `advisories-scanners`) that pushes and opens PRs, one commit each per the Proposed Solution, with one component failing not stopping the others, and uploads per-component outcomes as an artifact; `dry_run` prints branches, titles and bodies instead. The issue job now `needs` the fix job, under the `if` from Technical Considerations
- [x] Show each finding's fix outcome, and open and declined fix PRs from `gh pr list`, in the rolling issue
- [x] Add `tools/advisories/tests/test_fix.py` covering target-version choice, naming, title, commit body, the lifecycle decisions and the reverse-dependency walk; extend `test_report.py` for the open and declined fix-PR section; run `just advisories-test`; it passes
- [x] Run `nix run nixpkgs#actionlint -- .github/workflows/advisories.yml`; it passes
- [x] Describe the crate fix PRs in `docs/src/development/ci.md` and narrow ADR-0029's "Not yet in code" line to the fix paths Phase 9 adds
- [x] Run `just bazel-repin-crate tokio` on macOS with `GH_TOKEN` unset and `GH_CONFIG_DIR` at an empty directory; only `Cargo.Bazel.lock` and `MODULE.bazel.lock` change and the lock records `CARGO_BAZEL_REPIN` as unset; revert it
- [x] Run `just bazel-build` (and `just bazel-test` in place of coverage) on macOS; both pass
- [ ] Run `just bazel-coverage` on macOS; it passes (unenforceable locally: instrumented binaries segfault on macOS 27.0.1 at the phase base too, see the journal's owner decisions; CI's linux-amd64 coverage leg is the gate)
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 9: Fix PRs for BCR modules, the distroless base and Debian packages

- [x] Add `just bazel-lock-update` (`bazel mod deps` after a `bazel_dep` change)
- [x] Extend `fix.py` for BCR findings: lowest non-yanked version at or above the fix from the registry's `metadata.json`, compared with Bazel's module version ordering, edit the `bazel_dep` and any `single_version_override` holding the module (alert-only when that override carries `patches`), refresh the lock
- [x] Extend `fix.py` for base-image findings: current index digest of the tag `advisories.toml` names (`crane digest`; `base-debian12:latest` still moves: on 2026-11-06 it resolved to index `sha256:fabbf1c0…`, not the pinned `9dce90e6…`, so the first daily run proposes a base bump; with `crane` added to `advisories-scanners`), replaced in `oci.pull` with the capture date in its comment
- [x] Extend `fix.py` for deb findings: snapshot date in the deb manifest `advisories.toml` names moved to the scan date, lock regenerated
- [x] Unit-test BCR version ordering and selection (including `.bcr.N` and yanked versions) and the digest and snapshot edits in `test_fix.py`; run `just advisories-test`; it passes
- [x] Run `just bazel-lock-update` and `just bazel-distroless-lock` on macOS with `GH_TOKEN` unset and `GH_CONFIG_DIR` at an empty directory; both complete; revert any change
- [x] Replace "Quarterly native-pin review" in `docs/src/development/ci.md` with the pin table and alert-only workflow for `http_archive` libraries, FFmpeg and Kakadu
- [x] Drop ADR-0029's "Not yet in code" line once every fix class is in code
- [x] Name `tools/advisories/components.toml` in `CLAUDE.md`'s dependency section as the classification of record
- [x] Run `just bazel-build` on macOS; it passes (with `just bazel-test`, 78/78)
- [ ] Run `just bazel-coverage` on macOS; it passes (unenforceable locally: macOS 27.0.1 instrumented binaries segfault at exit, also at the phase base; CI's linux-amd64 coverage leg is the gate)
- [x] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

## Human Actions

| Id | Action | Who | When | Why not the agent |
|----|--------|-----|------|-------------------|
| H1 | Confirm `DASCHBOT_PAT` may push `advisories/*` branches and open PRs under the current rulesets | Maintainer | before start | Token scopes and rulesets are not readable by the agent |
| H2 | After merge, run `advisories.yml` once with `dry_run`, then once without; confirm the issue appears, fix PRs open, and a fix PR's CI (including `advisories`) runs and passes; on the next release, confirm `advisories-gate` ran before `release-gate` | Maintainer | after ship | The workflow runs only from the default branch and mutates shared state |
| H3 | Triage the baseline findings in the rolling issue: fix PRs merged or ignores with expiry | Maintainer | after ship | Risk acceptance is the owner's decision |
| H4 | Make the `advisories` PR check required in the `main` ruleset | Maintainer | after ship | Repository ruleset is maintainer territory |
| H5 | Remove the `daschswiss` Docker Hub organisation's Scout integration and its `production` environment | Maintainer | after ship | Docker Hub organisation settings |
| H6 | Decide whether Dependabot security updates stay enabled: the repository setting is on, while `ci.md` says Dependabot is off | Maintainer | after ship | Repository security settings |

## Acceptance Criteria

- [ ] `bookworm.lock.json` (both arches) contains no `curl`, `libcurl4`, `libssl3` or `libgnutls30`; image comments name `sipi health` as the healthcheck
- [ ] Every BCR module in `MODULE.bazel.lock` and every `http_archive` / `git_override` / custom repository rule is classified once per source in `components.toml`; each drift case fails `just advisories-inventory` (covered by tests)
- [ ] The inventory contains every crate in `Cargo.Bazel.lock` except the synthetic `direct-cargo-bazel-deps`, every package of both arches in `bookworm.lock.json`, and every `shipped` map entry
- [ ] The chosen scanner(s) report the fixture's vulnerable component in each class, or the ADR records which class rests on the pin table
- [ ] A scanner error exits 3 and never reads as a finding or a clean scan (covered by tests)
- [ ] A component past its `eol` is a finding, and one within 90 days is listed in the issue (covered by tests)
- [ ] Every non-local `uses:` ref is in the inventory with its resolved tag, or listed as unresolved
- [ ] No tool hard-codes a repository-specific path, image reference, tag or label: lockfile, deb, Kakadu extension and workflow paths, the base image and its tag, and the issue label come from `advisories.toml`; only format parsers (Cargo lock, MODULE.bazel, rules_distroless lock) are built in
- [ ] `just advisories-scan-diff` passes when base and head share a finding and fails on a head-only finding, including a pin bumped to a vulnerable version (covered by tests)
- [ ] An expired ignore entry stops suppressing its finding (covered by tests)
- [ ] `just audit`, `osv-scanner.toml` and the cargo-audit flags are gone; `ignores.toml` carries the four entries, each with reason and expiry
- [ ] The `advisories` PR job replaces `dependency-audit`
- [ ] The daily workflow keeps one rolling issue, capped at 60 000 characters; `dry_run` writes the body to the job summary
- [ ] The baseline per-class finding counts are in the Phase 4 PR description
- [ ] The release gate blocks every push and the docs deploy, runs once per release, and publishes the CycloneDX SBOM
- [ ] `grep -rni scout .github justfile docs/src` returns nothing
- [ ] Fix-PR target choice, naming, titles, commit bodies and lifecycle decisions are covered by `test_fix.py`
- [ ] Each `bazel-*` fix recipe runs with `GH_TOKEN` unset and `GH_CONFIG_DIR` at an empty directory
- [ ] ADR-0029 names the reversed decisions with their original reasons
- [ ] `just advisories-test`, `just bazel-build` and `just bazel-coverage` pass

## Dependencies & Risks

- **CPE false positives** may make the issue noisy at first; triage with expiring ignores (H3).
- **A release diff misses a backlog finding** by design; the daily issue owns the backlog, and H3 drains it.
- **BCR fix PRs can hit MVS conflicts** (curl and OpenSSL needed a `single_version_override`) or break the build (libexpat's config header); CI on the PR catches it and the PR waits for a person.
- **Base-image and Debian bumps** change runtime libraries that the smoke test only partly exercises (startup, `/health`, IIIF, `sipi health`). Every fix PR runs the full CI, and CI is the gate: a check a base or Debian bump needs belongs in the smoke test, never in a manual step on the PR. Closing that gap for what the base supplies is part of DEV-7785.
- **A CPE miss is silent** for `http_archive` libraries and FFmpeg; the pin table in the issue is the remaining human signal.
- **Action pins lose freshness** with Dependabot gone (S2-41); accepted as a routine-currency non-goal. The pin table shows each action's pin date.
- **The base image is already unsupported.** `base-debian12` gets no new digests, so its findings and its `EOL` finding stay open until the move to `base-debian13`, tracked in DEV-7785 and outside this plan.
- **A persistently failing daily scan** leaves the issue untouched and skips the fix job. The signal is the red run, which GitHub mails to whoever last edited the cron; nothing else alerts. Accepted for now.
- **The fix job runs only on Linux;** local verification is macOS. H2 covers the first real Linux run.

## Success Metrics

- Debian packages in the lock: from 33 per arch to `tini` and its glibc dependencies, 4 expected (counts recorded in the Phase 1 PR).
- Classified components: from Rust crates only (448 in `Cargo.Bazel.lock`, test crates included) to every shipped component class, zero unclassified.
- Baseline per-class finding counts recorded in the Phase 4 PR; the rolling issue reaches zero unignored findings after H3.
- An advisory against `main` no longer turns unrelated PRs red (today every open PR failed `dependency-audit` on RUSTSEC-2026-0387).
- A fixable crate advisory has a fix PR within one daily run of reaching the advisory database.

## References

- Current scan: `justfile` `audit` recipe; `.github/workflows/ci.yml` `dependency-audit` job; `osv-scanner.toml`
- Scout steps: `.github/workflows/ci.yml` (`test` job, linux-amd64 PR leg); `.github/workflows/publish.yml` (`publish-docker` job)
- Dependency surfaces: `MODULE.bazel` (`bazel_dep`s, `http_archive` pins, `crate.spec` / `crate.from_specs`, `oci.pull`); `bazel/kakadu_extension.bzl`; `bazel/distroless/bookworm.yaml`, `bookworm.lock.json`
- Stdlib tool precedent: `tools/loadtest/loadgen.py`
- [ADR-0015 native cc_library over foreign_cc](../adr/0015-native-cc_library-over-foreign_cc.md)
- crate_universe `CARGO_BAZEL_REPIN` values: https://raw.githubusercontent.com/bazelbuild/rules_rust/0.74.0/crate_universe/private/crates_repository.bzl
- Bazel lockfile modes: https://bazel.build/external/lockfile
- Renovate `bazel-module` manager: https://docs.renovatebot.com/modules/manager/bazel-module/
- Renovate `osvVulnerabilityAlerts`: https://docs.renovatebot.com/configuration-options/#osvvulnerabilityalerts
- OSV-Scanner configuration, output and exit codes: https://google.github.io/osv-scanner/configuration/, https://google.github.io/osv-scanner/output/
- OSV-Scanner image scanning: https://google.github.io/osv-scanner/usage/scan-image
- Grype configuration (CPE matching, ignore rules): https://oss.anchore.com/docs/reference/grype/configuration/
- Grype scan targets: https://oss.anchore.com/docs/guides/vulnerability/scan-targets/
- GitHub workflow triggers and `GITHUB_TOKEN`: https://docs.github.com/en/actions/using-workflows/triggering-a-workflow
- GitHub dependency submission API: https://docs.github.com/en/code-security/supply-chain-security/understanding-your-software-supply-chain/using-the-dependency-submission-api
- Dependabot Bazel support: https://blog.bazel.build/2026/02/20/dependabot.html
- Learning: `dasch-specs/learnings/best-practices/sipi-nix-to-bazel-migration-lessons.md` (distroless nsswitch, CI cache behaviour)
