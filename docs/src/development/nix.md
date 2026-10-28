# Nix dev shell

Nix's role in Vitrinli is **dev-shell provisioning only**. It assembles
a reproducible bash environment with bazelisk, `gh` (consumed by the
`gh_release_archive` repository_rule for the Kakadu fetch), `crane`
(used by `bazel-docker-publish-manifest`), and `jpylyzer` (JP2
conformance validator). No host C/C++ build tools are needed — every
dep is a BCR `bazel_dep` or a native `cc_library` compiled by the
hermetic LLVM toolchain. Rust + LLVM toolchains are provisioned
hermetically by Bazel (`rules_rust` + the BCR `llvm` (hermetic-llvm) module in
`MODULE.bazel`) — the Bazel cc actions never use the dev-shell `clang`.

It is **not** the build system. Vitrinli's binaries, tests, and Docker
image are all produced by Bazel. See [Building with Bazel](bazel.md).

## One-time setup

**Install [Determinate Nix](https://docs.determinate.systems/).**
The Determinate installer enables flakes and `nix develop` by
default. macOS and Linux are both supported.

```bash
curl -fsSL https://install.determinate.systems/nix | sh -s -- install
```

After the installer finishes, restart your shell and verify:

```bash
nix --version
```

That's the entire setup. There is no Cachix dependency, no Kakadu
FOD pre-fetch step, and no GH_TOKEN export needed at the shell layer
(the `gh_release_archive` Bazel repository_rule reads `GH_TOKEN` itself
from the build environment; locally `gh auth login` is enough).

## Entering the dev shell

```bash
nix develop                    # default shell — Clang + libc++ + host tools
just bazel-build               # subsequent commands work as usual
```

Three shells are exposed:

| Shell | Stdenv | Purpose |
|---|---|---|
| `default` | `llvmPackages_19.libcxxStdenv` | Clang + libc++ for non-Bazel tooling (clang-tidy, ad-hoc shell compiles). The Bazel cc actions run under the hermetic LLVM 23.1.2 toolchain, not this stdenv. 99% of work happens here. |
| `gcc` | `pkgs.gcc14Stdenv` | Diagnostic escape hatch when the LLVM toolchain produces a confusing error. Not used by CI. |
| `llvm-tools` | `llvmPackages_19.libcxxStdenv` | `default` + `llvmPackages_19.llvm` (host `llvm-cov`, `llvm-profdata`). For LOCAL coverage runs only — local devs running `just bazel-coverage` should enter this shell. CI resolves the same tools hermetically from the Bazel toolchain (`//bazel:llvm-*` aliases), not from this shell. Sanitizer runs no longer need it: the e2e macro attaches the hermetic `//bazel:llvm-symbolizer` as a test runfile under `--config=asan`. |

```bash
nix develop .#gcc              # GCC + libstdc++ environment
nix develop .#llvm-tools       # default + LLVM host binaries on PATH
```

`bazel build` invokes its own hermetic LLVM toolchain regardless of
which shell is active — the dev-shell stdenv only matters for
ad-hoc shell-level compilation.

## What's on the PATH

`flake.nix` is the source of truth (60 lines, dev-shells only).
Highlights:

- **bazelisk** + a `bazel` shim — reads `.bazelversion` and downloads
  the matching Bazel.
- **gh, cacert** — the `gh_release_archive` Bazel repository_rule shells
  out to `gh release download`; `cacert` provides a TLS bundle on
  headless Linux dev shells.
- **go-containerregistry** — provides `crane`, used by
  `just bazel-docker-publish-manifest` to assemble the multi-arch
  manifest from per-arch digests.
- **just** — recipe runner.
- **python3Packages.jpylyzer** — JP2 conformance validator used to
  verify regenerated JP2 goldens (see ADR-0010).
- **llvmPackages_19.llvm** *(coverage shell only)* — `llvm-cov` /
  `llvm-profdata` for `just bazel-coverage`. Bazel's
  `collect_cc_coverage.sh` hard-requires `COVERAGE_GCOV_PATH` and
  `LLVM_COV` env vars on every test action; the justfile recipe
  resolves them via `$(command -v llvm-{cov,profdata})`.

## Shell hooks

The default shell hook does two things:

- Exports `SSL_CERT_FILE` from the `cacert` package so `gh`'s
  Go-based TLS works on headless Linux dev shells.
- On macOS, prepends `/usr/bin` to `PATH` so Apple's
  `/usr/bin/xcrun` resolves ahead of nixpkgs' xcbuild stub (which
  returns the apple-sdk-14.4 stub — private frameworks only, no
  `libc++.tbd`). Note: the hermetic-llvm toolchain now fetches the
  macOS SDK from Apple's CDN, so the Bazel cc actions no longer
  depend on this `xcrun` SDK probe; the hook is retained for host
  tools that still shell out to `xcrun`.

## direnv

If you use [direnv](https://direnv.net/), drop a one-line `.envrc`:

```bash
# .envrc
use flake
```

`direnv allow` once and the dev shell auto-activates whenever you
`cd` into the repo. Combined with editor integrations (e.g. JetBrains
[direnv plugin](https://plugins.jetbrains.com/plugin/15285-direnv-integration),
[VS Code direnv extension](https://marketplace.visualstudio.com/items?itemName=Rubymaniac.vscode-direnv))
this gives the IDE the same PATH/env as the terminal.
