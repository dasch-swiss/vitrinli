# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> [!IMPORTANT]
> **No commit or push without the lint gates passing.** For any Rust change
> (`src/server/rust`, `src/cli/rust`, `test/e2e`), run **`just bazel-rustfmt-check`**
> and **`just bazel-clippy-check`** before committing (fix with `just bazel-rustfmt`).
> Both are CI gates (`-Dwarnings`, run as `rules_rust` lint aspects on the `test`
> job) that are **not** exercised by `just bazel-test`, `bazel-test-e2e`, or
> `bazel-coverage` — so a fully green local test run can
> still fail CI on a formatting or clippy finding. This is a hard rule, not a
> suggestion.

## Project Overview

Vitrinli is a multithreaded, high-performance, IIIF-compatible media server written in C++23. It implements IIIF Image API 3.0 and provides efficient image format conversions while preserving metadata. The server can be used both as a command-line tool and as a web server with Lua scripting support.

**Naming: the product is Vitrinli (formerly SIPI, the Simple Image Presentation Interface).** Prose says Vitrinli; code identifiers and artefact names keep `sipi`. The **Vitrinli** entry in [`CONTEXT.md`](CONTEXT.md) lists what keeps the old name and how to name new code. In Linear (team DEV), the product label is `components > vitrinli` (renamed from `sipi`, so it covers all history), set by the reporter. The `vitrinli module` label group holds one label per `CONVENTIONS.md` module scope, set by the maintainer at triage. To list Vitrinli issues, filter on the `vitrinli` label and also search title/description text for both "sipi" and "vitrinli", then merge: issues filed without the product label only show up in the text search.

## Domain Model

Read these before reasoning about names, boundaries, or architectural decisions:

- [`CONTEXT.md`](CONTEXT.md) — the bounded context and its vocabulary. Vitrinli is the IIIF subdomain implementation of the **Access Area** bounded context in the wider [`dsp-repository`](https://github.com/dasch-swiss/dsp-repository) system. Defines the Published Language inherited from Access Area (**Preservation File** / **Service File** / **Access File**) and the Vitrinli glossary: Image vs Bitstream, the IIIF pipeline terms (Region / Size / Rotation / Quality / Format / Decode level / Canonical URL / Cache key), Preservation metadata, the three Lua entry points, the eight Permission types, and more. Use these terms in code comments, commit messages, and PR descriptions; the aliases on its `_Avoid_` lines are *avoid*.
- [`docs/adr/`](docs/adr/) — architectural decision records. Start with [`0020-oracle-removal.md`](docs/adr/0020-oracle-removal.md) (the C++ oracle is removed; the Rust shell is the sole server — completing the strangler-fig migration that [`0013-shttps-as-internal-module.md`](docs/adr/0013-shttps-as-internal-module.md) prepared) and [`0017-extensibility-lua-and-rust.md`](docs/adr/0017-extensibility-lua-and-rust.md) (Lua and Rust extensions are both first-class, permanently).
- Architecture map: see [`ARCH-MAP.md`](ARCH-MAP.md); load on demand for blast-radius and boundary questions (never auto-loaded).

## Specs

Vitrinli specs (PRDs, implementation plans, design docs) live directly in [`docs/specs/`](docs/specs/) as flat, date-prefixed files. Platform-wide specs that span several repos stay in [`dasch-specs`](https://github.com/dasch-swiss/dasch-specs); a spec whose primary subject is Vitrinli belongs here even when it touches deploy config or another repo.

- **File:** each spec is a flat file directly in `docs/specs/`, named `YYYY-MM-DD-NN-{topic}-{type}.md` — `NN` a 2-digit daily sequence (`01`, `02`, … so more than one spec can share a date), `{topic}` lowercase alphanumeric + hyphens (≤60 chars), `{type}` ∈ `PRD` / `plan` / `design` / `journal`. The artifacts of one piece of work share the same `YYYY-MM-DD-NN-{topic}-` stem (e.g. `…-plan.md` alongside its `…-journal.md`).
- **Assets:** any images go in a sibling `docs/specs/YYYY-MM-DD-NN-{topic}-assets/` directory, referenced relatively (`![…](YYYY-MM-DD-NN-{topic}-assets/file.png)`).
- **Existing subfolders:** older `docs/specs/YYYY-MM-DD-{slug}/` folders predate this convention and stay as-is; only new specs are flat.
- **Frontmatter** (YAML, all spec files): `title`, `date`, `author`, `status: draft | reviewed | approved | implemented`, and `repositories:` listing the *other* code repos the feature modifies — never list `vitrinli` itself (the spec's home repo carries no signal).
- **Reference direction is one-way — specs are a sink.** A spec may reference *out* (code, docs, ADRs, `CONTEXT.md`), but **nothing outside `docs/specs/` may reference into a spec**, and a new spec should reference durable artifacts (an ADR, code, the glossary), not another spec. This is what lets any spec be renamed, flattened, or deleted without breaking the tree — the flatten test: a `grep -rn "docs/specs/"` over everything except `docs/specs/` itself returns nothing. *Enforcement: `review`* (promotable to a `static-analysis` grep gate). Pre-existing cross-spec links in historical plans are tolerated; do not add new ones.
- `docs/specs/` is outside the mkdocs site (`docs_dir: src`); specs are repo context, not published documentation.

## Build System and Common Commands

All targets are in a single `justfile`. Run `just` for a complete list.
For full build instructions, see [`docs/src/development/building.md`](docs/src/development/building.md) and [`docs/src/development/bazel.md`](docs/src/development/bazel.md).

**Build reproducibility invariant:** Bazel is the build system; Nix provisions only the dev shell. Every CI build/test/coverage step invokes a `just bazel-*` recipe, never an inline `bazel` or `nix build` call ([`CONVENTIONS.md` § Build Reproducibility Invariant](CONVENTIONS.md#build-reproducibility-invariant)). CI runs on a DaSCH-hosted NativeLink Remote Build Execution backend whose flags are injected per CI step; local development uses it only through the opt-in `--config=rbe-darwin`, and the cross-leg flags both share live only in `.bazelrc` `build:rbe-cross` ([`rbe.md`](docs/src/development/rbe.md)). Read [`rbe-write-pressure.md`](docs/src/development/rbe-write-pressure.md) before re-investigating RBE disk-write behavior.

**Build completeness invariant:** every target builds on macOS (darwin-aarch64), linux-x86_64 and linux-aarch64; only the Docker image is Linux-only. Before shipping a change to `flake.nix`, `MODULE.bazel`, `BUILD.bazel`, or a justfile build recipe, run `just bazel-build` and `just bazel-coverage` locally on macOS at minimum ([`CONVENTIONS.md` § Build Completeness Invariant](CONVENTIONS.md#build-completeness-invariant)).

**First-time setup:** Bazel builds (including `just bazel-docker-build-${arch}`) fetch Kakadu directly via Bazel's `gh_release_archive` repository_rule (no `vendor/` step). Requires `gh auth login` and `dasch-swiss` org membership. See [`docs/src/development/kakadu.md`](docs/src/development/kakadu.md). Once per clone, run `git config blame.ignoreRevsFile .git-blame-ignore-revs` so `git blame` skips the mass-reformat commits.

**ICC determinism invariant:** [`Icc::iccBytes()`](src/metadata/cpp/icc.cpp) is the single chokepoint that turns an ICC profile into bytes for every codec, and `@lcms2` visibility is narrowed so a format handler cannot reach lcms2 directly (a partial guard: `layering_check` is deferred, DEV-6353). Approval tests pin the ICC creation date via `SOURCE_DATE_EPOCH`; production never sets it. Detail and limits: [`CONVENTIONS.md` § ICC Determinism Invariant](CONVENTIONS.md#icc-determinism-invariant) and [ADR-0002](docs/adr/0002-icc-profile-determinism-test-only.md).

### Inner-loop development (incremental rebuilds)

`bazel build` IS the inner loop. The first build is slow (cold action cache,
the native `cc_library` deps kakadu/libtiff/exiv2/etc compile from source), but
subsequent edits to a single `.cpp` file rebuild in seconds — Bazel's per-action cache only
re-runs the affected compile + link.

```bash
nix develop                                   # dev shell with build deps + bazelisk
just bazel-build                              # first build: cold action cache
./bazel-bin/src/cli/sipi server --config config/sipi.localdev-config.lua
# subsequent edits:
just bazel-build                              # incremental, sub-second through link
```

`just run` wraps this: it depends on `bazel-build` and starts sipi with the
localdev config in one step.

## High-Level Architecture

**Production surface.** The **Rust axum shell** (`src/server/rust` + `src/cli/rust`, built by `just bazel-build-server`) is the production server. It drives the C++ **image engine** (`libsipi`) over the FFI seam in `src/server/rust/src/ffi.rs`. There is no C++ server: the retained shttps oracle was removed (ADR-0020); the C++ `//src/cli:sipi` binary now provides only the offline verbs (`convert`/`verify`/`query`/`compare`/`health`). In production Rust code, describe current behavior on its own terms; do not frame it relative to the removed C++ server / oracle / transport (referencing the C++ *engine*, the FFI callee, is fine). See [`CONVENTIONS.md` § Production surface](CONVENTIONS.md).

### Core Components

| Component | Path | Purpose |
|-----------|------|---------|
| Main Application | `src/cli/cpp/cli_app.cpp` | CLI11 arg parsing + offline-verb dispatch, behind the `sipi_cli_main` FFI entry; `src/cli/rust/src/main.rs` owns `main` and Sentry init |
| SipiImage | `src/image/cpp/SipiImage.h` | The image value type: pixel buffer, metadata composite, decode/encode orchestration and format dispatch |
| Image Processing | `src/image_processing/cpp/processing.h` | Free-function pixel operators over `SipiImage &` — crop/scale/rotate/ICC conversion/channel ops/watermark/comparison/arithmetic; depends one-way on `SipiImage` |
| Rust HTTP shell | `src/server/rust/` | The production server: axum routes, IIIF endpoints, caching, Lua request-shaping; drives the C++ engine over FFI |
| IIIF Parser | `src/iiifparser/cpp/` (C++ engine) / `src/iiifparser/rust/` (production, `//src/iiifparser/rust:iiif_parser`) | IIIF URL parsing: identifier, region, size, rotation, quality/format. Production parses in Rust and emits domain types; `server` flattens them into the seam struct the C++ engine consumes (ADR-0021) |
| Format Handlers | `src/format_handlers/` | SipiIO base class + SipiIOTiff, SipiIOJ2k, SipiIOJpeg, SipiIOPng |
| Caching | `src/cache/cpp/SipiCache.h` | File-based LRU cache with dual-limit eviction (size + file count), crash recovery |
| Metrics | `src/observability/cpp/metrics.h` | Metrics singleton (`Sipi::observability::Metrics`) — plain atomic counters/gauges; scalar fields cross the FFI seam as `SipiMetricsSnapshot` and export over OTLP via `src/server/rust/src/metrics.rs` |
| Memory Budget | `src/throttling/cpp/SipiMemoryBudget.h` | Lock-free decode memory budget with RAII guard — prevents OOM from concurrent large decodes |
| Lua Runtime | `src/scripting/rust/` | Rust-hosted mlua runtime (ADR-0023): hardened per-request VM (stdlib whitelist, memory cap, deadline), bytecode cache, all `server.*`/`SipiImage`/sqlite bindings, Lua-flavor config parse |

### Dependencies

**External Libraries.** Each is either a BCR `bazel_dep` (drop-in) or a
hand-written native `cc_library` over an `http_archive`/release fetch
(`bazel/<lib>.BUILD.bazel`) — never `rules_foreign_cc` (see
[`docs/adr/0015-native-cc_library-over-foreign_cc.md`](docs/adr/0015-native-cc_library-over-foreign_cc.md)).
BCR drop-ins: libpng, libjpeg_turbo, libwebp, libdeflate, zlib, bzip2, xz, zstd,
sqlite3, libexpat, libmagic, Lua, curl, OpenSSL,
protobuf. Native `cc_library`: libtiff (codecs re-enabled + JBIG via jbigkit),
exiv2, lcms2, jansson, jbigkit, and Kakadu (requires license).

**System Dependencies:** Threads (pthread), iconv (macOS only).

### Important Files

- `MODULE.bazel` — Bazel module + BCR `bazel_dep`s + `http_archive` pins for the native-`cc_library` deps
- `BUILD.bazel` (root + `src/`, `test/`) — target graph; `bazel/<lib>.BUILD.bazel` — native dep build files
- `justfile` — all build targets (run `just` to list)
- `flake.nix` — dev-shell only (`default` clang+libc++, `gcc` diagnostic)
- `version.txt` — version information; baked in via `tools/workspace_status.sh`
  + `expand_template` substitution into `include/SipiVersion.h.in`

## Testing

For the authoritative testing strategy (pyramid, layer definitions, decision tree, IIIF coverage matrix, feature inventory), see [`docs/src/development/testing-strategy.md`](docs/src/development/testing-strategy.md).

For test framework details (how to run tests, directory layout, adding tests), see [`docs/src/development/developing.md`](docs/src/development/developing.md).

- **Unit tests** (`test/unit/`): GoogleTest — `just bazel-test-unit` (or via `bazel-coverage` in CI)
- **Approval tests** (`test/approval/`): snapshot-based regression — `just bazel-test-approval` (or via `bazel-coverage` in CI). `SOURCE_DATE_EPOCH=946684800` and `SIPI_WORKSPACE_ROOT="."` are injected by `test/approval/BUILD.bazel`.
- **E2E tests** (`test/e2e/`): Rust (reqwest + `rules_rust`'s hermetic rustc). Run via `just bazel-test-e2e`, or a single target with `bazel test //test/e2e:<name> --test_output=streamed`.
- **Smoke tests** (`test/e2e/tests/docker_smoke.rs`): against Docker image. Run via `just bazel-test-smoke` — the `:docker_smoke` rust_test consumes the OCI tarball from `//src:image_load` and `docker load`s it before probing endpoints.
- **Fuzzing** (`src/iiifparser/fuzz/`): libFuzzer over the production Rust IIIF parser. `bazel test //src/iiifparser/fuzz:parse_request_fuzz` is a corpus replay that rides along in the `//src/...` sweeps on every platform; the mutation loop (`just fuzz`, nightly `fuzz.yml`) runs on Linux and macOS (macOS needs the Command Line Tools installed). See [`docs/src/development/fuzzing.md`](docs/src/development/fuzzing.md).

Run a single unit-test target with `bazel test //test/unit/<component>:<component>_test --test_output=streamed`.

**Hot-path changes require a benchmark.** Before changing any image decode/encode hot path (`src/format_handlers/*`, `SipiImage` read/write, `iiifparser`), a Google Benchmark microbench must exist, co-located with the module (per ADR-0003) as a manual-tagged `*_benchmark.cpp` `cc_binary`. Add one if it doesn't. Justify the change with a before/after `just bench` + `just bench-compare` run on the same `-c opt` binary and machine (trust a delta only if the U-test is green AND the median shift exceeds the baseline CV; sub-3% is noise). See [`docs/src/development/benchmarking.md`](docs/src/development/benchmarking.md).

## CI, Release, and Commit Messages

For CI pipeline details (Docker publishing, release automation), see [`docs/src/development/ci.md`](docs/src/development/ci.md).

**Releases are automated via release-please.** Correct [Conventional Commit](https://www.conventionalcommits.org/) prefixes are required — they drive SemVer bumps and changelog generation. See [`docs/src/development/ci.md`](docs/src/development/ci.md) for the full prefix-to-release mapping.

Valid types (exactly eight): `feat`, `fix`, `perf`, `refactor`, `docs`, `test`, `build`, `chore`. `revert`, `style`, and `ci` are **not** valid — use `chore(ci): ...`, fold formatting in, and `fix`/`chore` for reverts. Breaking changes use `!` suffix: `feat(scope)!: ...`. **A scope is mandatory** (`type(scope): subject`, no catch-all) — a module name from [`CONVENTIONS.md` § Module Layout](CONVENTIONS.md); if none fits, ask the maintainer before coining a new one. A CI gate (`commitlint-rs`, `just commit-lint`) enforces this. Full schema single-sourced in [`docs/src/development/commit-conventions.md`](docs/src/development/commit-conventions.md).

**The history's reader is a coding agent.** Rebase-merge puts every branch commit on `main` verbatim — there is no squash safety net. Before merge, curate the branch to **one commit per module per slice**, each with a body that explains the end state (decisions, rejected alternatives, invariants, gotchas), never the journey. A small one-slice, one-module PR is one commit. A `fix:` corrects behavior already on `main`; a bug introduced earlier in the same branch is folded into its introducing commit, never a standalone `fix:`.

For the git workflow, commit message schema, scope vocabulary, and PR description format (all single-sourced), see [`docs/src/development/commit-conventions.md`](docs/src/development/commit-conventions.md). PRs are scaffolded by [`.github/PULL_REQUEST_TEMPLATE.md`](.github/PULL_REQUEST_TEMPLATE.md).

**Code review:** Use [`docs/src/development/reviewer-guidelines.md`](docs/src/development/reviewer-guidelines.md) as the review checklist for all PRs.

## C++ Style Guide

Follow [`docs/src/development/cpp-style-guide.md`](docs/src/development/cpp-style-guide.md) for all new and modified C++ code. Key rules:

- **Ownership:** No raw owning `new`/`delete` — use `std::unique_ptr`, `std::make_unique`, or value semantics
- **Error handling:** `std::expected<T, E>` for fallible operations, exceptions for truly unrecoverable conditions
- **Input validation:** Validate all user input at HTTP handler boundaries before any file I/O or header construction
- **`[[nodiscard]]`:** Apply to all functions where ignoring the return value is a bug
- **const correctness:** Apply `const` everywhere it is valid
- **Naming:** new code follows the style guide; a new member in an existing class matches that class ([`CONVENTIONS.md` § Naming Conventions](CONVENTIONS.md#naming-conventions))
- **Legacy code:** When modifying existing code, apply modernization opportunistically (see style guide Section 4)

## Infrastructure boundary

**All infrastructure changes are operator-only. Claude Code has read-only access to infrastructure; the maintainer applies every change. No exceptions.**

- NEVER run `tofu apply` / `tofu destroy` / `tofu import` / state edits, Ansible plays, or any command that mutates deployed infrastructure or its state — the RBE backend VM (`dasch-remotebuild-prod-01`, defined in the `ops-tf` / `infra` repos), buckets, IAM, firewall, secrets. This includes SSH-and-mutate (an `ssh … --command` that installs/restarts/writes), cloud-CLI create/update/delete, `kubectl apply`, and remote `docker` mutations.
- Read-only analysis IS allowed and expected: `tofu plan`/`validate`, Grafana/Loki queries, describe/list-style CLI reads, and read-only SSH diagnostics (probing service status, reading logs/configs).
- Workflow: diagnose, read state, and propose the exact change (the file diff and/or the precise command/apply plan). The maintainer applies it. Verify afterwards read-only. Authoring commits/PRs in the ops repos is fine; *deploying* them is not.

## Scope discipline

Four rules decide *what* to build: **no backwards-compatibility shims**, **no defense-in-depth** (validate at system boundaries only), **no enterprise abstractions (KISS)**, and **ask when in doubt**. They are a contract with the maintainer, not style preferences; follow them unless the user explicitly overrides one. The full text is [`CONVENTIONS.md` § Scope Discipline](CONVENTIONS.md#scope-discipline).

## Development Notes

**Compiler Requirements:** C++23. Bazel selects a hermetic LLVM 23.1.2 toolchain via the BCR `llvm` (hermetic-llvm) module pinned at 0.8.25; the host compiler does not need to be Clang. libc++ is the default stdlib and a single toolchain serves all platforms and configs, `--config=fuzz` included. See [`docs/adr/0014-toolchain-provider-swap.md`](docs/adr/0014-toolchain-provider-swap.md).

**Build configurations:**
- `bazel build //src/cli:sipi` — fastbuild (`-O0 -g`, fast incremental)
- `bazel build -c opt //src/cli:sipi` — optimized (`-O3 -DNDEBUG`)
- `bazel build --config=release //src/cli:sipi` — production: `-c opt` + `_FORTIFY_SOURCE=2` hardening; what the Docker image ships
- `bazel build -c dbg //src/cli:sipi` — Debug (`-O0 -g`)
- `bazel build --config=asan --config=ubsan //src/cli:sipi` — sanitizers
- `bazel build --config=fuzz //src/iiifparser/fuzz:parse_request_fuzz_bin` — instrumented libFuzzer binary (Linux only; `-c opt` + SanitizerCoverage on the Rust crate graph)

**Error Reporting:** Optional Sentry integration (Rust `sentry` crate, `cli/rust/src/main.rs`) via `SIPI_SENTRY_DSN`, `SIPI_SENTRY_ENVIRONMENT`, `SIPI_SENTRY_RELEASE` environment variables — panics and handled image errors for every verb, plus an out-of-process minidump reporter for native crashes on `server` (see [`docs/adr/0018-minidump-crash-memory-accepted-risk.md`](docs/adr/0018-minidump-crash-memory-accepted-risk.md)).
