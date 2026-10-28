# Developing Vitrinli

## Using an IDE

### CLion

If you are using [CLion](https://www.jetbrains.com/clion/), open the
project root and let CLion's Bazel support index the workspace via
the [Bazel for IntelliJ](https://plugins.jetbrains.com/plugin/8609-bazel)
plugin (project from `MODULE.bazel`). Launch CLion from inside the
Nix dev shell so it inherits the build environment:

```bash
nix develop
clion .
```

A direnv setup (see [Nix dev shell](nix.md#direnv)) gives the IDE
the same PATH/env without launching it from a terminal.

### clangd

clangd and clang-tidy read `compile_commands.json` at the workspace
root. Generate it (it is git-ignored) with:

```bash
just compile-commands
```

Re-run it after adding or moving a C++ file or changing a target's
deps or copts. Build once first (`just bazel-build`) so generated
headers exist under `bazel-out/`.

The compile commands name the hermetic clang as driver, so start
clangd with `--query-driver='**'` to let it query that driver for its
system include paths. In Zed, set it in `settings.json`:

```json
{
  "lsp": {
    "clangd": {
      "binary": { "arguments": ["--query-driver=**"] }
    }
  }
}
```

The repo-root `.clangd` strips flags clangd cannot parse. A clangd
older than the hermetic LLVM (22) may report errors inside the
toolchain's own builtin headers (e.g. `unknown type name '__mfp8'`
on aarch64). The toolchain ships a matching clangd;
`just compile-commands` prints its path. Point the editor's clangd
binary at it to avoid the version skew.

## Running locally

A dedicated local development config is provided at
`config/sipi.localdev-config.lua`. It points `imgroot` at the
bundled test images and uses small cache limits (1 MB, 10 files)
so IIIF requests work out of the box and cache eviction is easy
to observe.

### Start the server

```bash
nix develop
just run                                  # bazel build + run sipi server --config localdev
```

The server starts on `http://localhost:1024`.

`just run` depends on `just bazel-build` so the binary at
`bazel-bin/src/cli/sipi` is always rebuilt before the run. After the
first cold build (the native `cc_library` deps kakadu/libtiff/exiv2/etc
compile from source), incremental rebuilds re-run only the affected compile + link
through Bazel's per-action cache — typically sub-second through
link.

### Try some requests

```bash
# Fetch an IIIF image with a transformation (creates a cache entry)
# Note: requests that need no processing (same format, full size, no rotation)
# are served directly from the original file and bypass the cache.
curl http://localhost:1024/unit/gradient-stars.tif/full/max/0/default.jpg -o /tmp/test.jpg
```

The server exports metrics over OTLP, not a scrape endpoint. Cache
behaviour is observable directly from the on-disk cache directory
(`./cache` under the localdev config).

Make several different image requests to fill the cache past its
1 MB / 10 file limits and watch entries evict from `./cache`:

```bash
# Format conversions (TIF → JPG) trigger caching — all well under 2 MB
curl http://localhost:1024/unit/gradient-stars.tif/full/max/0/default.jpg -o /dev/null
curl http://localhost:1024/unit/lena512.tif/full/max/0/default.jpg -o /dev/null
curl http://localhost:1024/unit/cielab.tif/full/max/0/default.jpg -o /dev/null

# Resized requests also trigger caching
curl http://localhost:1024/unit/MaoriFigure.jpg/full/200,/0/default.jpg -o /dev/null
curl http://localhost:1024/unit/MaoriFigureWatermark.jpg/full/200,/0/default.jpg -o /dev/null

ls ./cache
```

### Available configs

| Config file | Purpose |
|-------------|---------|
| `config/sipi.config.lua` | Production-like defaults (`./images` imgroot, 20 MB cache) |
| `config/sipi.localdev-config.lua` | Local development (test images, tiny cache, DEBUG logging) |
| `config/sipi.test-config.lua` | Automated test suite |

## Writing tests

For the authoritative testing strategy (pyramid, layer definitions,
decision tree, IIIF coverage matrix, feature inventory), see
[Testing strategy](testing-strategy.md).

Unit + approval tests use [GoogleTest](https://github.com/google/googletest)
(approval tests additionally use [ApprovalTests.cpp](https://github.com/approvals/ApprovalTests.cpp)).
End-to-end and HTTP-contract tests are written in Rust and live under
`test/e2e/`.

All tests build under Bazel:

```bash
just bazel-test-unit             # bazel test //src/... //test/unit/...
just bazel-test-approval         # bazel test //test/approval:approvaltests
just bazel-test-e2e              # all rust_test e2e targets
just bazel-test-smoke            # Docker smoke test

bazel test //src/iiifparser/fuzz:parse_request_fuzz   # fuzz-corpus replay (also inside the //src/... sweeps)
```

CI runs the full pyramid through `just bazel-coverage` (unit +
approval + e2e under instrumentation, lcov for Codecov).

### Unit tests

Unit tests use GoogleTest with ApprovalTests. `test/unit/fixtures/` holds
shared fixture generators (test images, EXIF payloads) consumed by tests
across packages, not a test suite itself.

Per-module Bazel packages co-locate their unit tests alongside the sources
(per ADR-0003). Co-located tests today:

- `//src/observability:metrics_registry_test` — metrics-registry seam tripwire: pins every metric field against how it reaches production OTLP via the `SipiMetricsSnapshot` bridge
- `//src/metadata:icc_normalize_test`, `//src/metadata:essentials_test`, `//src/metadata:exif_rational_test` — ICC normalization, the Essentials packet, and EXIF rational-array decode
- `//src/util:util_test` — util: Hash, Parsing, PathRedact, filename hashing, urldecode
- `//src/iiifparser/cpp/value_objects:iiifparser_test` — IIIF value-object parsers + seam round-trip + decode dims
- `//src/iiifparser/cpp/classifier:iiif_handler_test` — the `parse_iiif_uri` classifier + its regression corpus
- `//src/iiifparser/rust:iiif_parser_test` — the production Rust IIIF URL parser (domain, parse, request modules)
- `//src/iiifparser/rust:corpus_regression_test` — the Rust parser swept over the shared corpus
- `//src/image:image_test` — `SipiImage` construction, mutation, pixel accessors, and the Essentials-packet service-file prefix invariant
- `//src/image_processing:image_processing_test` — scale/rotate/crop, channel-count and equality/pixel-delta regressions, and the engine-direct IIIF transform matrix
- `//src/format_handlers:formats_test`, `//src/format_handlers:output_sink_test` — per-format read/write/read_shape regression suites (TIFF, JPEG, PNG, J2K) + the write sink
- `//src/format_handlers:tiff_codecs_test` — libtiff ingest-codec coverage proof (JPEG/LZMA/ZSTD/WebP/JBIG compiled in)
- `//src/cache:cache_test` — LRU cache: indexing, crash recovery, persistence, add/check/remove, dual-limit eviction, blocked-file handling
- `//src/logging:logger_test` — logger
- `//src/throttling/cpp:memory_budget_test` — decode memory budget: CAS accounting, RAII guard, peak-memory estimator

Run one component:

```bash
bazel test //src/iiifparser/cpp/value_objects:iiifparser_test --test_output=streamed
```

When adding a new unit test, declare a `cc_test` target in the module's own
`BUILD.bazel`, co-located with the sources it exercises (per ADR-0003) —
follow the pattern of the co-located tests listed above rather than adding a
`test/unit/<mod>/` directory. CI runs `just bazel-coverage` — which exercises
every `cc_test` under `//src/...` (co-located unit tests) plus
`//test/unit/...`, `//test/approval/...`, and `//test/e2e/...` in a single
pass — so a missing `cc_test` target, or one not reachable from `//src/...`,
means no CI coverage.

### Rust end-to-end tests

Rust e2e tests live in `test/e2e/` and use `reqwest` for HTTP,
`serde_json` for JSON validation, and `insta` for golden snapshots.
They cover IIIF compliance, server behaviour, and upload
functionality.

Run via Bazel — `rules_rust` produces one `rust_test` target per
`tests/<name>.rs`:

```bash
just bazel-test-e2e                                  # full suite (CI canonical)
bazel test //test/e2e:server                    # single target, inner-loop
bazel test //test/e2e:server --test_output=streamed   # see live output
```

The full suite resolves the sipi binary via `$SIPI_BIN`, defaulting
to `bazel-bin/src/cli/rust/sipi`. Override `SIPI_BIN` to point at a
sanitized build (`bazel build --config=asan`) when investigating
ASan findings.

!!! note "Sequential execution"
    Each test starts its own Vitrinli server on a unique port. The
    `sipi_e2e_test` Bazel macro sets `--test-threads=1` so this
    works out of the box.

### Smoke tests

Smoke tests live in `test/e2e/tests/docker_smoke.rs` and run
against a built Docker image. They verify basic server functionality
after a Docker build:

```bash
just bazel-test-smoke            # builds //src:sipi_image, loads tarball, probes
```

### Approval tests

Approval tests live in `test/approval/` and use snapshot-based
testing for regression detection. They run as part of every
`just bazel-test`, `just bazel-coverage`, or focused
`just bazel-test-approval` invocation.

`SOURCE_DATE_EPOCH=946684800` and `SIPI_WORKSPACE_ROOT="."` are
injected by `test/approval/BUILD.bazel`'s `env = {}` block, so the
wall-clock-stamped ICC creation date that lcms2 stamps into JPEG /
PNG / JP2 (and ICC-carrying TIFF) outputs is overwritten with a
fixed value. Without these env vars, the seconds field drifts by
one byte across consecutive runs.

When running the binary directly outside of `bazel test`, export
the same value first:

```bash
SOURCE_DATE_EPOCH=946684800 SIPI_WORKSPACE_ROOT="." \
  ./bazel-bin/test/approval/approvaltests \
  --gtest_filter='ImageEncodeBaseline.*'
```

Without it, expect `.received.*` files for every ICC-touching
test — that's a deliberate test-infrastructure side-effect, not a
regression. See
[`test/approval/CHANGELOG.approval.md`](../../../test/approval/CHANGELOG.approval.md)
for the full list and the re-approval procedure, and `docs/adr/0002-icc-profile-determinism-test-only.md`
in the project root for the design rationale.

### Fuzzing

Coverage-guided libFuzzer fuzzing of the production Rust IIIF parser lives in
`src/iiifparser/fuzz/`. `bazel test //src/iiifparser/fuzz:parse_request_fuzz` is
a corpus replay that runs on every platform; `just fuzz` and the nightly
`fuzz.yml` run the mutation loop on Linux. The canonical how-to — the C++→Rust
shim seam, the two modes, the corpus policy, and crash triage — is
[Fuzzing](fuzzing.md).

### Microbenchmarks

Performance work uses the Google Benchmark suite: `just bench <tier>` and
`just bench-compare <before> <after>`. The canonical how-to — tiers,
fixtures, the before/after workflow, and the regression decision rule
("no benchmark, no hot-path change") — is [Benchmarking](benchmarking.md).

## Managing dependencies

Every C/C++ dependency is one of two shapes (there is no
`rules_foreign_cc` — see
[ADR-0015](../../adr/0015-native-cc_library-over-foreign_cc.md)):

- **BCR `bazel_dep`** — a one-line pin in `MODULE.bazel` for libs whose
  Bazel Central Registry module is a true drop-in (zlib, libpng,
  libjpeg_turbo, libwebp, curl, openssl, …).
- **Native `cc_library`** — for libs with no BCR module or where stock
  BCR would compromise capability (libtiff, exiv2, lcms2, jansson,
  sentry, jbigkit, kakadu). The source is fetched by an `http_archive`
  in `MODULE.bazel`; the build rule is a hand-written `cc_library` in
  `bazel/<lib>.BUILD.bazel` (referenced as the archive's `build_file`),
  compiled by the hermetic LLVM toolchain like first-party code.
  Config headers are reproduced via the `cmake_configure_file` module.

To bump a **BCR `bazel_dep`**: change the `version` string and run
`bazel build //src/cli:sipi` + `just bazel-test`; commit `MODULE.bazel`
and `MODULE.bazel.lock`.

To bump a **native `cc_library`** dep:

1. Edit the relevant `http_archive(...)` in `MODULE.bazel` — update
   `url` and clear `sha256`.
2. Run `bazel build //src/cli:sipi` once; Bazel reports the actual
   sha256 in the failure output. Paste it into the block.
3. Re-diff any `*.cmake.in` config templates the `bazel/<lib>.BUILD.bazel`
   reproduces — upstream may have added a `#cmakedefine`/`@VAR@` token
   that `cmake_configure_file` now needs declared.
4. Run `bazel build //src/cli:sipi` and `just bazel-test` to confirm.
5. Commit `MODULE.bazel`, `MODULE.bazel.lock`, and any `bazel/<lib>.BUILD.bazel`
   change.

Adding a brand-new dependency: prefer a BCR `bazel_dep` if the module is
a true drop-in. Otherwise add an `http_archive(...)` + a native
`cc_library` in `bazel/<lib>.BUILD.bazel` (use a sibling lib as the
template) and wire it into the consumers (`//src:sipi_lib`,
`//src/util:util`, …) via `@<lib>//:<target>`.

Kakadu is special: it is fetched via a custom `gh_release_archive`
repository_rule (`bazel/gh_release.bzl`) that shells out to
`gh release download`. See [Kakadu setup](kakadu.md).

## Commit and PR conventions

The commit message schema (Conventional Commit types, the scope
vocabulary, and what `fix:` means), the rebase/one-commit-per-PR git
workflow, and the PR description format all live in a single source:
[`commit-conventions.md`](commit-conventions.md).
