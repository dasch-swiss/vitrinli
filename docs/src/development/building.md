# Building Vitrinli from Source Code

Vitrinli's build system is **Bazel** (orchestrating every C/C++ target,
the OCI Docker image, and the Rust e2e + smoke tests). Nix's role
is reduced to **provisioning a dev shell** with bazelisk + the host
tools the `gh_release_archive` repository_rule needs on PATH (`gh`, for
the Kakadu fetch).

All build commands are wrapped by recipes in the root `justfile`.
Run `just` to see the full list. Every CI step invokes one of these
recipes — there are no inline `bazel ...` calls in any workflow.

## Quick start

```bash
nix develop                                    # bazelisk + host tools on PATH
just bazel-build-server                        # bazel build --stamp //src/cli/rust:sipi (the Rust server shell)
./bazel-bin/src/cli/rust/sipi server --config config/sipi.localdev-config.lua
```

`just run` chains the two: it depends on `bazel-build` and starts
sipi with the localdev config in one step.

For Bazel concepts, common commands, `--config=` flags, and
querying the build graph, see **[Building with Bazel](bazel.md)**.

For dev-shell setup (Determinate Nix install, what the shell
provides), see **[Nix dev shell](nix.md)**.

## Prerequisites

### Kakadu (JPEG 2000)

Vitrinli uses [Kakadu](http://kakadusoftware.com/), a proprietary JPEG
2000 toolkit licensed separately. The archive is published as a
release asset on the private
[`dasch-swiss/dsp-ci-assets`](https://github.com/dasch-swiss/dsp-ci-assets)
repo and is fetched at build time by a custom Bazel
`gh_release_archive` repository_rule (`bazel/gh_release.bzl`) that shells out
to `gh release download`.

```bash
gh auth login    # one-time; needs dasch-swiss org membership
just bazel-build # the repository_rule fetches Kakadu on first build
```

CI passes `GH_TOKEN: ${{ secrets.DASCHBOT_PAT }}` on every Bazel
step so the repository_rule resolves there too. See
[Kakadu setup](kakadu.md) for details and the version-bump
procedure.

### Adobe ICC Color Profiles

Vitrinli uses Adobe ICC color profiles, which are downloaded
automatically by the Bazel build. The user is responsible for
reading and agreeing with Adobe's license conditions, which are
specified in `Color Profile EULA.pdf` at the repo root.

## Building a Docker image

The Docker image is built by Bazel `rules_oci` (`//src:sipi_image`).
There is no `Dockerfile` — `src/BUILD.bazel` is the
single source of truth. A running Docker daemon is still required
for `docker load` / `docker push`, but `docker buildx` is not used
(multi-arch manifest assembly happens via `crane index append` on a
coordinator job).

```bash
just bazel-docker-build-arm64  # or -amd64; loads daschswiss/sipi:latest
just bazel-test-smoke          # builds //src:sipi_image and probes the loaded image
```

### Per-arch builds (used by CI)

```bash
just bazel-docker-build-amd64           # build + load amd64 as daschswiss/sipi:latest
just bazel-docker-build-arm64           # build + load arm64
just bazel-docker-extract-debug amd64   # surface sipi-amd64.debug for sentry-cli
just bazel-docker-push-amd64            # push as :v<version>-amd64 + :latest-amd64
just bazel-docker-push-arm64            # push as :v<version>-arm64 + :latest-arm64
just bazel-docker-publish-manifest      # crane index append → daschswiss/sipi:v<version>
```

Each per-arch CI runner builds + pushes only its matching
architecture (`target_compatible_with` rejects cross-arch
invocations). A coordinator job runs `crane index append` to stitch
the two pushed digests into a multi-arch manifest at
`daschswiss/sipi:v<version>`.

## Cross-platform builds

The full test matrix runs on macOS-aarch64, linux-x86_64, and
linux-aarch64. CI exercises every variant on every platform; a
green CI run verifies macOS as well as Linux.

A macOS host cross-compiles the Linux OCI image directly — the
hermetic-llvm toolchain declares every exec×target pair and bundles a
per-target glibc + libc++ + compiler-rt, so no sysroot, VM, or host
Linux toolchain is needed. `rules_foreign_cc` was the only thing that
host-bound the build; with it gone (ADR-0015), every native
`cc_library` (kakadu/libtiff/exiv2/…) compiles through the relocatable
clang for the target platform:

```bash
just bazel-cross-build-image amd64   # build //src:sipi_image for linux-x86_64
just bazel-cross-build-image arm64   # build //src:sipi_image for linux-aarch64
```

These are build-only (no `docker load`, since a macOS host has no Linux
Docker daemon). Cross-compilation is a supported capability but is **not
currently CI-gated** — verify it locally with the recipes above (a
CI gate was prototyped and removed for being too slow; the approach is
being reconsidered). To actually load/run the image, either use a Linux
host's `bazel-docker-build-{amd64,arm64}` or a lightweight Linux VM such
as [OrbStack](https://orbstack.dev/) with the dev shell available inside
it (`nix develop` from a shared workdir).

## All `just` targets

Run `just` with no arguments to see the live list. Key target
groups:

| Target | Description |
|--------|-------------|
| `bazel-build [*FLAGS]` | `bazel build --stamp //src/cli:sipi` (fastbuild; pass `-c opt`/`--config=asan` etc.) |
| `bazel-test [*FLAGS]` | `bazel test //test/unit/... //test/approval/... //test/e2e/...` (no coverage) |
| `bazel-coverage [*FLAGS]` | Same target set, instrumented; emits combined lcov for Codecov |
| `bazel-test-unit` | `bazel test //test/unit/...` |
| `bazel-test-approval` | `bazel test //test/approval:approvaltests` |
| `bazel-test-e2e [*FLAGS]` | All Rust e2e `rust_test` targets |
| `bazel-test-smoke [*FLAGS]` | Docker smoke test (consumes Bazel-built image tarball) |
| `bazel-build-sanitized [*FLAGS]` | `bazel build --config=asan --config=ubsan //src/cli:sipi` |
| `bazel-cross-build-image {amd64,arm64}` | Cross-build `//src:sipi_image` for the Linux target arch (build-only; darwin→linux gate) |
| `bazel-docker-build-{amd64,arm64}` | Build + load per-arch image as `daschswiss/sipi:latest` |
| `bazel-docker-push-{amd64,arm64}` | Push to `daschswiss/sipi:{latest,v<version>}-${arch}` |
| `bazel-docker-publish-manifest` | `crane index append` → multi-arch manifest at `daschswiss/sipi:v<version>` |
| `bazel-docker-extract-debug arch` | Build `:sipi_debug_layout`, surface `sipi-<arch>.debug` |
| `run` | `just bazel-build` then run sipi with the dev config |
| `valgrind` | `just bazel-build` then run sipi under Valgrind |
| `docs-build` | Build documentation site (`mkdocs build`) |
| `docs-serve` | Serve documentation locally for preview |

## Documentation

```bash
just docs-build                # build documentation site
just docs-serve                # serve documentation locally for preview
```
