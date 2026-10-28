# Building with Bazel

[Bazel](https://bazel.build/) is the build system for Vitrinli. It owns
the entire build graph — every C/C++ target (sipi binary, the native
third-party `cc_library` deps, unit + approval tests), the OCI Docker
image, and the Rust e2e + smoke test binaries.

[Nix](nix.md) is no longer the build orchestrator — it provisions the
*dev shell* (bazelisk, `gh` for the Kakadu fetch, `crane`,
`just`, etc.) and nothing else.

## Mental model

A few concepts that make the rest of this page click:

**MODULE.bazel** is the project's manifest. It declares the Bazel
modules sipi depends on (`rules_oci`, the BCR `llvm` (hermetic-llvm)
module, `rules_rust`, the BCR codec/util `bazel_dep`s, …) and pins the
native-`cc_library` deps' source archives via `http_archive`. Version
bumps live here.

**BUILD.bazel** files describe the *target graph*. Each first-party
package — `//src`, `//src/server/rust`, `//src/cli/rust`,
`//test/unit/<mod>`, `//bazel/...` — has its own
BUILD.bazel that declares its `cc_library`/`cc_binary`/`cc_test`/
`oci_image`/`rust_test` targets and visibility rules.

**Hermetic toolchain.** The BCR `llvm` (hermetic-llvm) module, pinned
at 0.8.25, registers a single constraint-based LLVM 23.1.2 toolchain
that every cc action runs under — it serves all platforms.
libc++ is the default stdlib.
The bundle ships per-target glibc (~2.28) + libc++ + compiler-rt on
Linux (no external Chromium debian sysroots) and fetches the macOS
SDK hermetically from Apple's CDN (no host Xcode CLT). The host
compiler version is irrelevant — `bazel build //src/cli:sipi` produces
the same binary on macOS, linux-x86_64, and linux-aarch64 (modulo
platform-specific codegen). See
[ADR-0014](../../adr/0014-toolchain-provider-swap.md).

**Stamping.** `tools/workspace_status.sh` reads `version.txt` and
emits `STABLE_*` keys (`STABLE_SIPI_VERSION`, `STABLE_GIT_COMMIT`,
…). `expand_template` substitutes them into `include/SipiVersion.h.in`
so `sipi --version` reports the right string. Keys are also consumed
by the `oci_image` rule to stamp Docker labels and tags.

**Caching.** Bazel keeps three caches:

| Cache | What it holds | Where |
|---|---|---|
| Action cache | Hashed action outputs (compile, link, test) | `bazel info output_base` |
| Repository cache | Downloaded `http_archive` source tarballs | `~/.cache/bazel/_bazel_<user>/cache/repos/v1` |
| Disk cache | Action cache mirrored to a stable path (CI) | `~/.cache/bazel-disk` (CI only) |

`bazel build //src/cli:sipi` after a single-file edit re-runs only the
affected compile + link via the action cache — typically sub-second
through link.

## Quick start

```bash
nix develop                                    # bazelisk + host tools on PATH
just bazel-build-server                        # bazel build --stamp //src/cli/rust:sipi (the Rust server shell)
./bazel-bin/src/cli/rust/sipi server --config config/sipi.localdev-config.lua
# Subsequent edits:
just bazel-build-server                        # incremental, sub-second through link
```

`just run` chains the two: it depends on `bazel-build` and starts
sipi with the localdev config in one step.

## Common commands

Every CI step invokes one of these recipes — there are no inline
`bazel ...` invocations in workflows.

```bash
# Build sipi (fastbuild — fast incremental for inner-loop edits)
just bazel-build                 # bazel build --stamp //src/cli:sipi
just bazel-build --config=release  # production build: -c opt + hardening (matches Docker image)
just bazel-build --config=asan   # ASan+UBSan; same flag form for ad-hoc variants

# Tests
just bazel-test-unit             # bazel test //test/unit/...  (12 components)
just bazel-test-approval         # bazel test //test/approval:approvaltests
just bazel-test-e2e              # Rust e2e tests via rules_rust
just bazel-test-smoke            # Docker smoke test (OCI tarball loaded by the test)

# Coverage (canonical CI build — what ci.yml invokes on every PR)
just bazel-coverage              # unit + approval + e2e under instrumentation;
                                 # lcov at bazel-out/_coverage/_coverage_report.dat

# Sanitizer
just bazel-build-sanitized       # bazel build --config=asan --config=ubsan //src/cli:sipi

# Docker (rules_oci)
just bazel-docker-build-amd64    # build + load amd64 image as daschswiss/sipi:latest
just bazel-docker-build-arm64    # arm64 equivalent
just bazel-docker-publish-manifest  # crane index append → daschswiss/sipi:v<version>
just bazel-docker-extract-debug <arch>  # produce sipi-<arch>.debug for sentry-cli
```

## `--config=` flags

Defined in `.bazelrc`. Each flag composes with `bazel build` /
`bazel test` to switch the build configuration.

| Flag | Effect |
|---|---|
| `-c opt` | `-O3 -DNDEBUG`. Optimized codegen (the Docker image adds hardening — use `--config=release`). |
| `--config=release` | `-c opt` + `_FORTIFY_SOURCE=2` hardening. The production build; what the Docker image ships. |
| `-c dbg` | `-O0 -g`. Full debug symbols; what `bazel-build-sanitized` consumes. |
| `--config=asan` | AddressSanitizer + DWARF inline; consults `.lsan_suppressions.txt` at e2e time. |
| `--config=ubsan` | UndefinedBehaviorSanitizer. Composes with `--config=asan`. |

## Querying the build graph

`bazel query` answers structural questions about the target graph
without running an action:

```bash
# What does //src/cli:sipi depend on?
bazel query 'deps(//src/cli:sipi)' --output=label_kind | head -20

# Which targets transitively depend on //src/util:util?
bazel query 'rdeps(//..., //src/util:util)' --output=label

# Which BUILD files declare cc_test targets?
bazel query 'kind("cc_test", //test/unit/...)' --output=label

# What sources does //src/cache:cache_test compile?
bazel query 'attr("srcs", "", //src/cache:cache_test)' --output=build
```

`bazel cquery` is the configured-graph variant: it accounts for
`select()` and platform-specific deps, useful when querying targets
that vary by config (`//src:sipi_image` is `target_compatible_with`-gated
to Linux, etc.).

## Cache hygiene

CI cache strategy is documented in `.github/workflows/ci.yml` (see
the long "CACHE STRATEGY" comment block):

- Disk cache managed by `actions/cache@v5` (not `setup-bazel`'s
  built-in disk-cache wiring) so an analysis-phase failure cannot
  0-byte-poison the cache.
- Targeted key formula on inputs that actually affect the native-dep
  action keys: `MODULE.bazel{,.lock}`, `bazel/**`, `.bazelrc`,
  `.bazelversion`, `flake.lock`. App/test sources are deliberately
  excluded.
- Repository cache off in CI (would persist ~4 GB per arch and
  evict the real disk-cache speedup).

Locally, `bazel clean` flushes per-action outputs while preserving
downloaded archives; `bazel clean --expunge` flushes everything
including the repository cache.

## Kakadu

The proprietary Kakadu SDK is fetched at build time by a custom
`gh_release_archive` repository_rule (`bazel/gh_release.bzl`) that shells out
to `gh release download` against `dasch-swiss/dsp-ci-assets`. Auth
flows through `GH_TOKEN`; locally `gh auth login` once is enough, in
CI the workflow injects `GH_TOKEN: ${{ secrets.DASCHBOT_PAT }}` on
every Bazel-invoking step.

See [Kakadu setup](kakadu.md) for the full version-bump procedure.

## Cross-platform

Linux-only targets (`//src:sipi_image`, `//src:image_load`,
`//src:image_push_*`, `//src:sipi_debug_layout`) are gated by
`target_compatible_with = ["@platforms//os:linux"]` — on the **target**
OS, not the host. A macOS host builds them by setting the target
platform explicitly (`--platforms=//bazel/platforms:linux_{amd64,arm64}`),
which the hermetic-llvm toolchain cross-compiles end to end (bundled
per-target glibc + libc++; no sysroot). `just bazel-cross-build-image
{amd64,arm64}` wraps this. Cross-compilation is **not currently
CI-gated** (a gate was prototyped and removed for being too slow; the
approach is being reconsidered) — verify it locally. Without a
`--platforms` override the default host platform on macOS is darwin,
which fails the linux gate and skips the target.

Fuzzing has its own opt-in config. `//src/iiifparser/fuzz:parse_request_fuzz`
builds and corpus-replays everywhere by default; `--config=fuzz` arms the real
libFuzzer engine and is Linux-only. See [Fuzzing](fuzzing.md).

For Linux-target builds from a macOS host, see
[Building from source](building.md#cross-platform-builds).

## Reference

- [`MODULE.bazel`](https://github.com/dasch-swiss/vitrinli/blob/main/MODULE.bazel)
  — module manifest + every third-party `http_archive` pin
- [`.bazelrc`](https://github.com/dasch-swiss/vitrinli/blob/main/.bazelrc)
  — flag defaults, sanitizer/fuzz configs, production hardening
  (stack-protector-strong, _FORTIFY_SOURCE=2, stack-clash-protection,
  BindNow) with per-config exemptions for asan/ubsan (which `--config=fuzz`
  inherits when paired with them)
- [`tools/workspace_status.sh`](https://github.com/dasch-swiss/vitrinli/blob/main/tools/workspace_status.sh)
  — emits `STABLE_*` keys consumed by `expand_template` and
  `oci_image`'s stamping
- [Bazel: BUILD files](https://bazel.build/concepts/build-files)
- [Bazelmod (`MODULE.bazel`)](https://bazel.build/external/module)
- [`rules_oci`](https://github.com/bazel-contrib/rules_oci)
- [`cmake_configure_file`](https://github.com/wep21/cmake_configure_file)
- [hermetic-llvm (`llvm` BCR module)](https://github.com/hermeticbuild/hermetic-llvm)
- [ADR-0014: toolchain provider swap](../../adr/0014-toolchain-provider-swap.md)
