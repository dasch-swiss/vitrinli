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
3. **Docker Scout** (linux-amd64 PRs only, one leg — a single PR
   comment): `Docker Scout — compare to production`,
   `Docker Scout — CVE report (SARIF)`, and
   `Upload SARIF to GitHub Security`.

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

A `dependency-audit` job runs `just audit` against the checked-in
Cargo-format lockfile — see "Dependency advisory scanning" below for
what it covers. Self-contained (no Bazel/RBE setup); not currently a
required branch-protection check.

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
plus the `asan-ubsan / amd64` job. The `docs` job, the `dependency-audit`
job and the `changes` gate itself are not required checks. (The
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

Workflow: `.github/workflows/ci.yml`, job `dependency-audit`. Runs `just
audit` — `cargo-audit` and `osv-scanner` (installed in-job; neither is in the
Nix dev shell) scanning `Cargo.Bazel.lock`, the checked-in Cargo-format
lockfile materialized by `crate.from_specs(cargo_lockfile = ...)` in
`MODULE.bazel` (`crate_universe`'s own `MODULE.bazel.lock` is JSON, which
neither tool reads). Repin with
`CARGO_BAZEL_REPIN=1 bazel build //src/server/rust:lib` after any
`crate.spec()` change. Covers the single `@crates` hub: server, cli,
scripting and the e2e suite.

Dependabot is off: no version-update or security-update PRs. GitHub Actions,
the Nix flake inputs and the BCR `bazel_dep`s are bumped by hand.

This closes one gap, not the whole dependency surface: `dependency-audit`
does not see the native `http_archive` / `cc_library` pins below —
they have no lockfile-based advisory feed. Docker Scout (`ci.yml`'s PR-time
CVE scan and `publish.yml`'s release-time Critical gate) covers OS/runtime
packages in the *image layers*, which also does not reach these
statically-linked libraries.

### Quarterly native-pin review

Manually check each project's release page against the pinned version in
`MODULE.bazel` and CVE feeds (NVD, GitHub Advisory Database) for the pinned
version. Bump via a normal version-pin PR (update `sha256`/`strip_prefix`/`url`
or the BCR module version) when a fix is available.

**Native `cc_library` (`http_archive`, hand-pinned — see
[ADR-0015](../../adr/0015-native-cc_library-over-foreign_cc.md)):**

| Dependency | Pinned via | Releases |
|---|---|---|
| libtiff | `tiff` `http_archive` | <https://github.com/libsdl-org/libtiff/releases> |
| exiv2 | `exiv2` `http_archive` | <https://github.com/Exiv2/exiv2/releases> |
| lcms2 | `lcms2` `http_archive` | <https://github.com/mm2/Little-CMS/releases> |
| jansson | `jansson` `http_archive` | <https://github.com/akheron/jansson/releases> |
| jbigkit | `jbigkit` `http_archive` | no GitHub releases; upstream page: <https://www.cl.cam.ac.uk/~mgk25/jbigkit/> |
| mimalloc | `mimalloc` `http_archive` | <https://github.com/microsoft/mimalloc/releases> |
| tracy | `tracy` `http_archive` (dev-only, `--config=tracy`-gated) | <https://github.com/wolfpld/tracy/releases> |
| Kakadu | `kakadu_extension` (proprietary, license-gated) | <https://github.com/dasch-swiss/dsp-ci-assets/releases> (private, org-restricted) |

**BCR `bazel_dep` drop-ins** (image codec / parsing surface — see
CLAUDE.md's Dependencies table for the full BCR list; excludes pure
build-tooling deps like `rules_cc`/`rules_rust`/`rules_python`):

| Dependency | Releases |
|---|---|
| libpng | <https://github.com/pnggroup/libpng/releases> |
| libjpeg_turbo | <https://github.com/libjpeg-turbo/libjpeg-turbo/releases> |
| libwebp | <https://github.com/webmproject/libwebp/releases> |
| libdeflate | <https://github.com/ebiggers/libdeflate/releases> |
| zlib | <https://github.com/madler/zlib/releases> |
| bzip2 | no GitHub releases; upstream page: <https://sourceware.org/bzip2/> |
| xz | <https://github.com/tukaani-project/xz/releases> |
| zstd | <https://github.com/facebook/zstd/releases> |
| sqlite3 | no GitHub release notes; changelog: <https://www.sqlite.org/changes.html> |
| libexpat | <https://github.com/libexpat/libexpat/releases> |
| libmagic | <https://github.com/file/file/releases> |
| Lua | no GitHub releases; upstream page: <https://www.lua.org/versions.html> |
| curl | <https://github.com/curl/curl/releases> |
| OpenSSL | <https://github.com/openssl/openssl/releases> |
| protobuf | <https://github.com/protocolbuffers/protobuf/releases> |

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
2. `release-gate` — fires on `validate-docker` success.
3. Publish jobs run after the gate:
   - `publish-docker / {amd64, arm64}` — rebuilds the per-arch
     image, extracts the `.debug` file via
     `just bazel-docker-extract-debug ${arch}`, pushes via
     `just bazel-docker-push-${arch}`, records the image with Docker
     Scout, runs a Docker Scout CVE gate (`only-severities: critical`,
     `exit-code: true` — fails the job on any Critical finding in the
     image layers; see "Dependency advisory scanning" above for what
     this does and doesn't cover), uploads SBOM, pushes debug
     symbols to Sentry. It does not repeat the smoke test —
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
(03:17 UTC) plus manual `workflow_dispatch` — the repo's only scheduled
workflow. One job on `ubuntu-24.04`: it restores the previous nightly's
working corpus from the `fuzz-corpus` artifact, builds the instrumented
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
