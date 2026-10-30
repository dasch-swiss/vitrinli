# Sipi — IIIF-compatible media server
# Run `just` to list all available recipes.
#
# Build reproducibility invariant: every build-related recipe goes through
# `bazel build` / `bazel test` / `bazel coverage`. CI invokes only
# `just <recipe>` — no inline bazel calls. Bazel's own incremental rebuild
# IS the inner-loop edit/rebuild cycle (`just bazel-build` after a single-
# file edit completes in seconds via the action cache).

# Recipe arguments are also exposed as shell positionals ($1, $2, "$@") in
# shebang recipes. Needed wherever an argument can contain shell
# metacharacters — `{{VAR}}` interpolation pastes raw text into the script,
# so e.g. a `|` inside `just bench parse --benchmark_filter='A|B'` would
# otherwise become a shell pipe.
set positional-arguments

# Auto-detect CPU cores
nproc := `nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4`

# List all recipes
default:
    @just --list

#####################################
# Bazel — what CI runs
#
# Every build/test step in CI invokes one of these recipes. `bazelisk`
# resolves the pinned Bazel version from `.bazelversion` and is on PATH
# inside the Nix dev shell (`flake.nix`).
#
# All non-trivial recipes accept `*FLAGS` so callers can pass extra Bazel
# flags positionally — e.g. `--config=asan` for sanitiser runs,
# `--disk_cache=$HOME/.cache/bazel-disk` in CI, `--runs_per_test=3` for
# flakiness gates on the high-load e2e targets.
#
# `--stamp` is on for recipes whose output must carry the real version
# (`STABLE_SIPI_VERSION` from `tools/workspace_status.sh`): `bazel-build`,
# `bazel-build-server`, `bazel-test-smoke`, and the docker/release recipes.
# The `bazel-test` and `bazel-coverage` recipes are deliberately UNSTAMPED:
# the stamp changes the `:sipi_version_h` cache key every commit, which forces
# a fresh `sipi` re-link (and, on Linux, a fresh OCI image) whose closure the
# RBE worker re-materialises to disk on every run — the tests don't need the
# version string (they accept the `0.0.0-unstamped` fallback), so unstamped
# outputs cache across commits instead. See
# `docs/src/development/rbe-write-pressure.md`.
#####################################

# Build sipi. Bazel's fastbuild default — fast incremental rebuilds
# for the inner-loop edit/rebuild cycle. Pass `-c opt`, `-c dbg`,
# `--config=asan`, etc. as positional flags when needed.
bazel-build *FLAGS='':
    bazel build --stamp //src/cli:sipi {{FLAGS}}

# Build the Rust HTTP shell (strangler-fig rewrite). Additive — the C++ server
# still owns the production socket until the cutover; this is for local testing.
bazel-build-server *FLAGS='':
    bazel build --stamp //src/cli/rust:sipi {{FLAGS}}

# Build + run the full test pyramid (unit + approval + e2e) under
# fastbuild — no coverage instrumentation. CI's linux-arm64 and
# darwin-arm64 matrix entries use this path; only linux-amd64 (which
# uploads to Codecov) runs `bazel-coverage`. Splitting the two avoids
# paying instrumentation overhead (1.5-2x compile, slower runtime,
# flakiness risk on the high-load e2e targets) for arches that don't
# upload anyway.
#
# `--build_tests_only` keeps the wildcard from building the non-test OCI
# image + layer tars (built only by `bazel-test-smoke` / the docker recipes).
# `--test_tag_filters=-requires-docker` drops `//test/e2e:docker_smoke` (run
# separately by `bazel-test-smoke`), so its `//src:sipi_image` data dep is not built
# here either. Both cut RBE worker materialisation; see
# `docs/src/development/rbe-write-pressure.md`.
bazel-test *FLAGS='':
    bazel test --build_tests_only --test_tag_filters=-requires-docker //src/... //test/unit/... //test/approval/... //test/e2e/... {{FLAGS}}

# Build + run all tests (unit + approval + e2e) under coverage
# instrumentation; emit combined lcov at
# `bazel-out/_coverage/_coverage_report.dat` for `codecov-action@v6`
# to consume directly — no cobertura conversion.
#
# `instrumentation_filter` keeps measurement scoped to first-party
# code (`//src`), excluding test bodies and ext/* deps. Coverage spans unit + approval +
# e2e, so the reported % includes HTTP and Lua paths the unit suite cannot
# reach.
#
# `COVERAGE_GCOV_PATH` (= llvm-profdata) and `LLVM_COV` (= llvm-cov) are
# propagated into each test action. Bazel's `tools/test/collect_cc_coverage.sh`
# reads these to decide the LLVM coverage path (`llvm-profdata merge` +
# `llvm-cov export --format=lcov`) for the `.profraw` profiles. They MUST be
# the hermetic toolchain's binaries (LLVM 23), version-matched to the
# instrumented profile — a host/Nix `llvm-profdata` of a different major
# version cannot read the profile and yields an empty lcov. Resolved from the
# `//bazel:llvm-{profdata,cov}` aliases (exec-platform-selected bundle) rather
# than PATH. Required since the Bazel 7.2+ env-clobber bug — fix landed in
# Bazel 9.1.0 (PR #25879, closes #23247); `.bazelversion` must stay at or above it.
bazel-coverage *FLAGS='':
    #!/usr/bin/env bash
    set -euo pipefail
    bazel build //bazel:llvm-profdata //bazel:llvm-cov
    EXEC_ROOT="$(bazel info execution_root)"
    export COVERAGE_GCOV_PATH="$EXEC_ROOT/$(bazel cquery --output=files //bazel:llvm-profdata 2>/dev/null)"
    export LLVM_COV="$EXEC_ROOT/$(bazel cquery --output=files //bazel:llvm-cov 2>/dev/null)"
    bazel coverage \
        --combined_report=lcov \
        --instrumentation_filter='//src' \
        --features=llvm_coverage_map_format \
        --features=-gcc_coverage_map_format \
        --test_env=COVERAGE_GCOV_PATH \
        --test_env=LLVM_COV \
        --build_tests_only \
        --test_tag_filters=-requires-docker \
        //src/... //test/unit/... //test/approval/... //test/e2e/... {{FLAGS}}

# Run every GoogleTest unit-test target — the per-module ADR-0003
# co-located `*_test.cpp` under `//src/<mod>/`; `//test/unit/...` is
# kept in the target list because it still matches the manual fixture
# generator package. Useful for inner-loop development;
# CI runs unit tests via `bazel-coverage`. Accepts `*FLAGS` (e.g.
# `--config=asan --config=ubsan` for the sanitizer gate).
bazel-test-unit *FLAGS='':
    bazel test //src/... //test/unit/... {{FLAGS}}

# Run the approval-test target. SOURCE_DATE_EPOCH=946684800 and
# SIPI_WORKSPACE_ROOT="." are injected by `test/approval/BUILD.bazel`;
# any `.received.<ext>` file from a maintainer-driven re-approval
# lands under `bazel-testlogs/test/approval/approvaltests/`. Useful for
# inner-loop development; CI runs approval tests via `bazel-coverage`.
bazel-test-approval:
    bazel test //test/approval:approvaltests

# Format the first-party Rust crates in place per the repo rustfmt.toml
# (the rules_rust rustfmt runner formats every `rust_*` target in the workspace).
bazel-rustfmt *FLAGS='':
    bazel run @rules_rust//:rustfmt {{FLAGS}}

# Check Rust formatting — the CI gate. The rules_rust rustfmt aspect runs
# `rustfmt --check` over every first-party Rust target and fails on any file
# that is not rustfmt-clean; run `just bazel-rustfmt` to fix.
bazel-rustfmt-check *FLAGS='':
    bazel build //src/iiifparser/... //src/scripting/rust/... //src/throttling/rust/... //src/server/rust/... //src/cli/rust/... //test/e2e/... --aspects=@rules_rust//rust:defs.bzl%rustfmt_aspect --output_groups=rustfmt_checks {{FLAGS}}

# Run clippy — the CI lint gate. The rules_rust clippy aspect runs
# `clippy-driver` over every first-party Rust target; `-Dwarnings` promotes
# every clippy/rustc warning to an error so the gate fails on any finding. The
# `clippy::undocumented_unsafe_blocks` lint (enabled at each crate root) is the
# fast unsafe check: it requires a `// SAFETY:` comment on every `unsafe {}`
# block. Runs in CI's `lint` job alongside `bazel-rustfmt-check`.
bazel-clippy-check *FLAGS='':
    bazel build //src/iiifparser/... //src/scripting/rust/... //src/throttling/rust/... //src/server/rust/... //src/cli/rust/... //test/e2e/... --aspects=@rules_rust//rust:defs.bzl%rust_clippy_aspect --output_groups=clippy_checks --@rules_rust//rust/settings:clippy_flags=-Dwarnings {{FLAGS}}

# Advisory scan over the checked-in Cargo-format lockfile (`Cargo.Bazel.lock`,
# materialized from `crate.from_specs(cargo_lockfile = ...)` in MODULE.bazel —
# cargo-audit/OSV-Scanner read Cargo.lock syntax, not MODULE.bazel.lock's
# JSON). Covers the production `@crates` hub (server/cli/scripting); the
# `/test/e2e` cargo manifest is covered separately by Dependabot.
#
# Neither tool exposes a CLI severity threshold (RustSec advisories and OSV
# entries don't uniformly carry a Critical/High label), so both fail on any
# matched advisory — a conservative superset of "Critical/High", never a
# silent under-fail. `cargo-audit` and `osv-scanner` are not in the Nix dev
# shell — CI installs them (see `.github/workflows/ci.yml`); run locally after
# `cargo install cargo-audit` / installing osv-scanner from its release page.
# RUSTSEC-2023-0071 (`rsa` Marvin timing side-channel) has no fixed upstream
# release. `rsa` is transitive via `jsonwebtoken` (pulled for RS256/PS256); SIPI
# pins JWT to HS256, so no RSA private-key operation runs and the side-channel is
# unreachable. Ignored in both tools until an upstream fix ships (rationale also
# in `osv-scanner.toml`). Revisit when `rsa` cuts a fixed release.
# RUSTSEC-2026-0387 (`crypto-bigint` 0.5 timing variability) is fixed only in
# 0.7.6+, which needs `elliptic-curve` 0.14; `jsonwebtoken` (10.x and 11.1)
# still requires `p256`/`p384` 0.13 on 0.5. Those curves serve ES256/ES384 only;
# with HS256 pinned no elliptic-curve operation runs, so the timing leak is
# unreachable. Revisit when `jsonwebtoken` moves to `p256` 0.14.
audit:
    cargo audit --file Cargo.Bazel.lock --ignore RUSTSEC-2023-0071 --ignore RUSTSEC-2026-0387
    osv-scanner scan source --lockfile=Cargo.lock:Cargo.Bazel.lock --config=osv-scanner.toml

# Lint commit messages with commitlint-rs — the CI `commit-lint` gate. Enforces
# the type allowlist + mandatory scope from `.commitlintrc.yml` on every commit in
# `<from>..HEAD` (`from` defaults to origin/main). `commitlint` is in the Nix dev
# shell; CI installs it via `cargo install`.
#
# We feed one message at a time on stdin instead of commitlint's `--from/--to`
# range mode: commitlint-rs reads stdin whenever stdin is not a TTY (always true
# in CI and under `just`), so range mode is unreachable in automation. The loop
# also attributes each failure to its commit and is a no-op on an empty range.
# `--no-merges` skips merge commits, so neither the synthetic `refs/pull/N/merge`
# commit GitHub checks out for a PR nor a merge a developer created locally is
# message-checked here. That is deliberate: message rules do not apply to merges,
# and the `require linear history` branch-protection rule is what actually blocks a
# merge commit from landing on `main`. See `docs/src/development/commit-conventions.md`.
commit-lint from="origin/main":
    #!/usr/bin/env bash
    set -euo pipefail
    base="$(git merge-base "{{from}}" HEAD)"
    fail=0
    while IFS= read -r sha; do
        [ -n "$sha" ] || continue
        if ! git log -1 --pretty=%B "$sha" | commitlint; then
            echo "  ↳ offending commit: $(git log -1 --pretty='%h %s' "$sha")" >&2
            fail=1
        fi
    done < <(git rev-list --no-merges --reverse "$base..HEAD")
    if [ "$fail" -ne 0 ]; then
        echo "commit-lint: one or more commits violate the convention (see above)" >&2
        exit 1
    fi
    echo "commit-lint: all commits in $base..HEAD OK"

# (Re)generate rust-project.json so rust-analyzer understands the Bazel crate
# graph (cargo can't see the rules_rust targets). The file is git-ignored — it
# holds machine-absolute paths. Re-run after adding/moving a Rust target or dep.
bazel-rust-project:
    bazel run @rules_rust//tools/rust_analyzer:gen_rust_project

# (Re)generate the git-ignored compile_commands.json for clangd and clang-tidy.
# Re-run after adding/moving a C++ file or changing a target's deps/copts.
# --bcce-compiler pins the driver to the hermetic clang++'s absolute path (the
# tool would emit it relative). Prints the version-matched hermetic clangd path.
compile-commands:
    #!/usr/bin/env bash
    set -euo pipefail
    EXEC_ROOT="$(bazel info execution_root)"
    CLANGXX="$EXEC_ROOT/$(bazel cquery --output=files '"//bazel:clang++"' 2>/dev/null)"
    test -x "$CLANGXX" || { echo "//bazel:clang++ did not resolve to the hermetic clang++: $CLANGXX" >&2; exit 1; }
    bazel run //tools/compile_commands:refresh_compile_commands -- --bcce-compiler="$CLANGXX"
    echo "hermetic clangd: $EXEC_ROOT/$(bazel cquery --output=files //bazel:clangd 2>/dev/null || true)"

# Run all Rust e2e `rust_test` targets against the Bazel-built `:sipi`
# binary. Pass extra Bazel flags positionally: e.g. `--config=asan` for
# the sanitiser run, `--test_output=streamed` for live stdout,
# `--runs_per_test=3` for a flakiness gate on `iiif_compliance` /
# `resource_limits` / `latency`. Useful for inner-loop development; CI
# runs e2e tests via `bazel-coverage`.
#
# Unstamped, like `bazel-test`: `cli_version_flag` accepts the
# `0.0.0-unstamped` fallback (see `test/e2e/tests/cli.rs`), so the e2e
# test actions cache across commits instead of forcing a
# `:sipi_version_h` re-link (and its closure re-materialisation on the
# RBE worker) every commit. See `docs/src/development/rbe-write-pressure.md`.
bazel-test-e2e *FLAGS='':
    bazel test --verbose_failures {{FLAGS}} //test/e2e:all_e2e

# Build + run the unit and e2e suites in ONE invocation under the
# sanitizer configs — CI's `sanitizer` job passes `--config=asan
# --config=ubsan --build_tag_filters=-no-sanitizer
# --test_tag_filters=-no-sanitizer`. Running both suites through a single
# `bazel test` compiles the instrumented `sipi` tree exactly once — both
# suites share that one ~5500-file `CppCompile` set within a single action
# graph (two separate jobs would each compile the full tree). Combining
# them here is the reason for this recipe over `bazel-test-unit` +
# `bazel-test-e2e`. `--build_tests_only` keeps the `//src/...` wildcard
# from building the non-test OCI image + CLI binary under instrumentation.
# Unstamped for the same caching reason as `bazel-test-e2e`. See
# `docs/src/development/rbe-write-pressure.md`.
bazel-test-sanitized *FLAGS='':
    bazel test --build_tests_only --verbose_failures //src/... //test/unit/... //test/e2e:all_e2e {{FLAGS}}

# Run the Docker smoke test against the Bazel-built OCI image.
#
# `bazel run //src:image_load` loads the image into the local daemon as
# `daschswiss/sipi:latest` (its `oci_load` repo_tag) explicitly, BEFORE the
# test — not as a side-effect of it. On a warm cache `:docker_smoke` is a cache
# hit and never executes its own `docker load`, yet downstream CI steps (Docker
# Scout) still need the image present in the daemon. `bazel run` also
# materialises the tarball under the `.bazelrc` default `--remote_download_minimal`,
# which a cached test would leave absent.
#
# `--config=asan` does not apply: the test client is uninstrumented and the sipi
# binary inside the container is already covered by `bazel-test-e2e --config=asan`.
bazel-test-smoke *FLAGS='':
    bazel run --stamp --verbose_failures {{FLAGS}} //src:image_load
    bazel test --stamp --verbose_failures {{FLAGS}} //test/e2e:docker_smoke

# Sanitized build: Debug + ASan + UBSan via Bazel
# `--config=asan --config=ubsan`. The resulting binary at
# `bazel-bin/src/cli/sipi` is what ci.yml's sanitizer job's e2e tests consume
# via `SIPI_BIN`. DWARF stays inline (`--strip=never` in
# `.bazelrc`) so the symbol-name suppressions in `.lsan_suppressions.txt`
# match. `--verbose_failures` surfaces the full failing compile/link
# command line — without it, Bazel reports only the higher-level summary,
# which makes triage harder. `--stamp` runs
# `tools/workspace_status.sh` so `STABLE_SIPI_VERSION` (from `version.txt`)
# is baked into `SipiVersion.h` via `src/BUILD.bazel`'s
# `expand_template(stamp_substitutions = {…})`. Without it, the binary
# reports `sipi 0.0.0-unstamped` and the `cli_version_flag` e2e test
# fails.
bazel-build-sanitized *FLAGS='':
    bazel build --config=asan --config=ubsan --verbose_failures --stamp {{FLAGS}} //src/cli:sipi
    @echo "Binary at: $(pwd)/bazel-bin/src/cli/sipi"

# Tracy-instrumented profiling build: `-c opt --config=tracy`. Local dev only,
# never CI-gated (like `bench` / `valgrind`). `-c opt` so the timeline reflects
# production codegen; `--config=tracy` (see `.bazelrc`) defines TRACY_ENABLE +
# TRACY_ON_DEMAND so the binary profiles only while the Tracy GUI is connected.
# Run the binary, then open the Tracy profiler and Connect (TCP 8086). The GUI
# comes from `nix develop .#profiling` on Linux or `brew install tracy` on macOS;
# it connects over the network too, so a macOS GUI can profile a Linux server.
# See docs/src/development/profiling.md.
bazel-build-tracy *FLAGS='':
    bazel build -c opt --config=tracy --verbose_failures --stamp {{FLAGS}} //src/cli/rust:sipi
    @echo "Tracy-instrumented server binary at: $(pwd)/bazel-bin/src/cli/rust/sipi"
    @echo "Run it (e.g. `sipi server --config …`), then open the Tracy profiler and Connect (localhost:8086)."

#####################################
# Fuzzing (libFuzzer via rules_fuzzing)
#
# Coverage-guided fuzzing of the production Rust IIIF parser
# (`//src/iiifparser/fuzz`) and the C++ codec decode entry points
# (`//src/format_handlers/fuzz`; DEV-7066). Runs on Linux and macOS: hermetic-llvm
# stages `libclang_rt.fuzzer_osx.a` since 0.8.18, so `-fsanitize=fuzzer` links on
# darwin (see `.bazelrc` §Fuzzing; the macOS repo-rule DEVELOPER_DIR that lets
# rules_fuzzing's Python deps resolve in the Nix shell is set globally there).
#
# The mutation loop executes the built binary DIRECTLY rather than via
# `bazel run` — the sandbox would discard corpus growth.
#####################################

# Build the instrumented fuzz binaries (CI-invoked; RBE-eligible). Builds the
# `_bin` targets rather than the `*_fuzz` test targets: `_bin` is what the loop
# executes, and only a top-level target is materialised locally under the
# `.bazelrc` BwoB default (`--remote_download_minimal`). The `_corpus` targets
# are built alongside them so `just fuzz`/`fuzz-corpus-merge` can re-seed from
# `bazel-bin` without a separate build step.
bazel-build-fuzz *FLAGS='':
    #!/usr/bin/env bash
    set -euo pipefail
    bazel build --config=fuzz --verbose_failures --remote_download_toplevel {{FLAGS}} \
        //src/iiifparser/fuzz:parse_request_fuzz_bin //src/iiifparser/fuzz:parse_request_fuzz_corpus \
        //src/format_handlers/fuzz:tiff_decode_fuzz_bin //src/format_handlers/fuzz:tiff_decode_fuzz_corpus \
        //src/format_handlers/fuzz:jpeg_decode_fuzz_bin //src/format_handlers/fuzz:jpeg_decode_fuzz_corpus \
        //src/format_handlers/fuzz:png_decode_fuzz_bin //src/format_handlers/fuzz:png_decode_fuzz_corpus \
        //src/format_handlers/fuzz:j2k_decode_fuzz_bin //src/format_handlers/fuzz:j2k_decode_fuzz_corpus \
        //src/format_handlers/fuzz:tiff_roundtrip_fuzz_bin //src/format_handlers/fuzz:tiff_roundtrip_fuzz_corpus \
        //src/format_handlers/fuzz:jpeg_roundtrip_fuzz_bin //src/format_handlers/fuzz:jpeg_roundtrip_fuzz_corpus \
        //src/format_handlers/fuzz:png_roundtrip_fuzz_bin //src/format_handlers/fuzz:png_roundtrip_fuzz_corpus \
        //src/format_handlers/fuzz:j2k_roundtrip_fuzz_bin //src/format_handlers/fuzz:j2k_roundtrip_fuzz_corpus \
        //bazel:llvm-symbolizer
    # The sanitizer runtime symbolizes crash frames by shelling out to
    # `llvm-symbolizer`, which is a source file inside the hermetic toolchain
    # repo — its execroot path embeds the pinned LLVM version, so resolve it
    # rather than hardcoding it, and expose it at a stable workspace path the
    # loop (`just fuzz`, the nightly) can point the runtime at. Without this,
    # first-party frames print as bare `binary+0xOFFSET`.
    mkdir -p .fuzz
    ln -sfn "$(bazel info execution_root)/$(bazel cquery --config=fuzz //bazel:llvm-symbolizer --output=files 2>/dev/null | head -1)" .fuzz/llvm-symbolizer
    echo "Instrumented fuzz binaries at: $(pwd)/bazel-bin/src/iiifparser/fuzz/parse_request_fuzz_bin, $(pwd)/bazel-bin/src/format_handlers/fuzz/{tiff,jpeg,png,j2k}_decode_fuzz_bin, $(pwd)/bazel-bin/src/format_handlers/fuzz/{tiff,jpeg,png,j2k}_roundtrip_fuzz_bin"
    echo "llvm-symbolizer at:            $(pwd)/.fuzz/llvm-symbolizer"

# short name -> "_bin target|_corpus target|dict path|max_len|checked-in corpus dir"
# used by both `fuzz` and `fuzz-corpus-merge`. `-max_len` caps are deliberate:
# the bug class these harnesses hunt is header/marker parsing at the front of
# the file, and a small cap concentrates the mutation budget there rather than
# on bulk pixel data. `parse_request` has no dict/max_len (it fuzzes URL text,
# not a binary container format). The checked-in corpus dir is the destination
# `fuzz-corpus-merge` merges coverage-adding inputs into.
_fuzz_target_table := '
parse_request //src/iiifparser/fuzz:parse_request_fuzz_bin //src/iiifparser/fuzz:parse_request_fuzz_corpus - - src/iiifparser/corpus
tiff //src/format_handlers/fuzz:tiff_decode_fuzz_bin //src/format_handlers/fuzz:tiff_decode_fuzz_corpus src/format_handlers/fuzz/dicts/tiff.dict 16384 src/format_handlers/corpus/tiff
jpeg //src/format_handlers/fuzz:jpeg_decode_fuzz_bin //src/format_handlers/fuzz:jpeg_decode_fuzz_corpus src/format_handlers/fuzz/dicts/jpeg.dict 16384 src/format_handlers/corpus/jpeg
png //src/format_handlers/fuzz:png_decode_fuzz_bin //src/format_handlers/fuzz:png_decode_fuzz_corpus src/format_handlers/fuzz/dicts/png.dict 8192 src/format_handlers/corpus/png
j2k //src/format_handlers/fuzz:j2k_decode_fuzz_bin //src/format_handlers/fuzz:j2k_decode_fuzz_corpus - 8192 src/format_handlers/corpus/j2k
tiff_roundtrip //src/format_handlers/fuzz:tiff_roundtrip_fuzz_bin //src/format_handlers/fuzz:tiff_roundtrip_fuzz_corpus src/format_handlers/fuzz/dicts/tiff.dict 16384 src/format_handlers/corpus/tiff
jpeg_roundtrip //src/format_handlers/fuzz:jpeg_roundtrip_fuzz_bin //src/format_handlers/fuzz:jpeg_roundtrip_fuzz_corpus src/format_handlers/fuzz/dicts/jpeg.dict 16384 src/format_handlers/corpus/jpeg
png_roundtrip //src/format_handlers/fuzz:png_roundtrip_fuzz_bin //src/format_handlers/fuzz:png_roundtrip_fuzz_corpus src/format_handlers/fuzz/dicts/png.dict 8192 src/format_handlers/corpus/png
j2k_roundtrip //src/format_handlers/fuzz:j2k_roundtrip_fuzz_bin //src/format_handlers/fuzz:j2k_roundtrip_fuzz_corpus - 16384 src/format_handlers/corpus/j2k
'

# Run the mutation loop locally, e.g. `just fuzz tiff -max_total_time=60`.
# TARGET selects the harness (see the table above); FLAGS pass straight
# through to libFuzzer.
#
# The working corpus lives at `.fuzz/corpus/<TARGET>/` — strictly per-format,
# never shared — and is writable, persisting between runs. It is re-seeded on
# every run by copying from the built `_corpus` target under `bazel-bin`,
# which already merges both tiers (the hand-picked `//test/_test_data`/
# `//src/iiifparser/corpus` fixture seeds and whatever coverage growth is
# checked into the source-tree corpus directory), so there is no seed list to
# duplicate here. Crash reproducers land in `.fuzz/artifacts/`; promoting
# anything into the checked-in corpus is deliberate and manual
# (`fuzz-corpus-merge`, or a reproducer committed with its fix).
fuzz TARGET='parse_request' *FLAGS='': bazel-build-fuzz
    #!/usr/bin/env bash
    set -euo pipefail
    bin='' corpus_target='' dict='' max_len=''
    while read -r name bin_t corpus_t dict_t max_len_t checked_in_corpus_t; do
        [ -z "$name" ] && continue
        if [ "$name" = "{{TARGET}}" ]; then
            bin="$bin_t"; corpus_target="$corpus_t"; dict="$dict_t"; max_len="$max_len_t"
        fi
    done <<< "{{_fuzz_target_table}}"
    if [ -z "$bin" ]; then
        echo "ERROR: unknown fuzz TARGET '{{TARGET}}' — valid names: parse_request, tiff, jpeg, png, j2k, tiff_roundtrip, jpeg_roundtrip, png_roundtrip, j2k_roundtrip" >&2
        exit 1
    fi
    bin_path="bazel-bin/${bin#//}"
    bin_path="${bin_path/://}"
    corpus_path="bazel-bin/${corpus_target#//}"
    corpus_path="${corpus_path/://}"
    work_dir=".fuzz/corpus/{{TARGET}}"
    mkdir -p "$work_dir" .fuzz/artifacts
    find "$corpus_path" -maxdepth 1 -type f -exec cp {} "$work_dir/" \;
    args=(-timeout=25)
    [ "$dict" != "-" ] && args+=("-dict=$dict")
    [ "$max_len" != "-" ] && args+=("-max_len=$max_len")
    args+=(-artifact_prefix=.fuzz/artifacts/ "$work_dir")
    # `set positional-arguments` + the `*FLAGS=''` default expand to a single
    # empty positional when no flags are given, which libFuzzer would read as an
    # (empty) corpus path and abort with "No such file or directory: ;". Keep
    # only the non-empty extra flags.
    flags=()
    shift
    for f in "$@"; do [ -n "$f" ] && flags+=("$f"); done
    "./$bin_path" "${args[@]}" ${flags[@]+"${flags[@]}"}

# Import coverage-adding inputs from the latest nightly `fuzz-corpus-<TARGET>`
# artifact into the checked-in corpus for TARGET. The only path from the live corpus to
# the repo — and it stops at a diff: the corpus filegroup also feeds the C++
# and Rust regression sweeps, so growing it is a maintainer decision, never
# automatic.
#
# `-merge=1 <dst> <src>` is libFuzzer's own minimisation: it copies an input
# from `<src>` into `<dst>` only if it adds coverage the destination lacks,
# which is why this needs the instrumented binary. Known imprecision: for the
# codec targets the checked-in tier does not contain the fixture seeds (those
# live in `test/_test_data` and reach the fuzzer through the Bazel filegroup),
# so `-merge=1` cannot see their coverage and may propose inputs the fixtures
# already cover — which is why this stops at a diff for a human.
fuzz-corpus-merge TARGET='parse_request': bazel-build-fuzz
    #!/usr/bin/env bash
    set -euo pipefail
    bin='' checked_in_corpus=''
    while read -r name bin_t corpus_t dict_t max_len_t checked_in_corpus_t; do
        [ -z "$name" ] && continue
        if [ "$name" = "{{TARGET}}" ]; then
            bin="$bin_t"; checked_in_corpus="$checked_in_corpus_t"
        fi
    done <<< "{{_fuzz_target_table}}"
    if [ -z "$bin" ]; then
        echo "ERROR: unknown fuzz TARGET '{{TARGET}}' — valid names: parse_request, tiff, jpeg, png, j2k, tiff_roundtrip, jpeg_roundtrip, png_roundtrip, j2k_roundtrip" >&2
        exit 1
    fi
    bin_path="bazel-bin/${bin#//}"
    bin_path="${bin_path/://}"
    # `--branch main`: only the nightly chain on the default branch is a real
    # corpus. `// empty` (not bare `.[0].databaseId`) so an empty result is the
    # empty string rather than the literal "null", and `if !` so a gh failure
    # (auth, network) reaches the message instead of aborting under `set -e`.
    if ! RUN_ID="$(gh run list --workflow=fuzz.yml --branch main --status=success --limit=1 --json databaseId --jq '.[0].databaseId // empty')"; then
        echo "ERROR: 'gh run list' failed — check 'gh auth status' and network" >&2
        exit 1
    fi
    if [ -z "$RUN_ID" ]; then
        echo "ERROR: no successful fuzz.yml run found on main — nothing to merge" >&2
        exit 1
    fi
    artifact_dir="$(mktemp -d)"
    trap 'rm -rf "$artifact_dir"' EXIT
    gh run download "$RUN_ID" --name "fuzz-corpus-{{TARGET}}" --dir "$artifact_dir"
    "./$bin_path" -timeout=25 -merge=1 "$checked_in_corpus" "$artifact_dir"
    echo "==> Review and commit deliberately:"
    git status --short "$checked_in_corpus"/

#####################################
# Microbenchmarks (Google Benchmark)
#
# Local dev-loop tooling only — never a CI gate. The discipline:
# *no benchmark, no hot-path change* — before/after numbers from the SAME
# machine and the SAME `-c opt` configuration accompany every codec/parser
# hot-path PR. See docs/src/development/benchmarking.md for the full
# workflow and the regression decision rule.
#####################################

# Build (`-c opt`, matching production codegen — never fastbuild, never
# sanitized/instrumented) and exec the named microbenchmark binary
# directly, forwarding Google Benchmark flags. `name` is the tier:
# parse | decode | encode | process → `//src:<name>_benchmark`.
#
# Typical before/after loop:
#   just bench parse --benchmark_repetitions=20 \
#       --benchmark_out=before.json --benchmark_out_format=json
#   ... make the hot-path change ...
#   just bench parse --benchmark_repetitions=20 \
#       --benchmark_out=after.json --benchmark_out_format=json
#   just bench-compare before.json after.json
#
# SOURCE_DATE_EPOCH + SIPI_WORKSPACE_ROOT mirror what
# `test/approval/BUILD.bazel` injects for tests: the encode/process tiers
# touch the ICC machinery whose creation date is wall-clock-stamped
# (ADR-0002), and `//test:test_paths` resolves fixtures relative to the
# workspace root. They are exported here (not via the cc_binary `env`
# attr) because the recipe runs the binary directly — `env` only applies
# under `bazel run`/`bazel test`. The decode/encode tiers additionally
# read the @sipi_bench_fixtures external repo (fetched lazily on first
# build) out of the binary's runfiles tree via SIPI_BENCH_FIXTURES_DIR,
# and the encode tier writes its outputs to a throwaway TEST_TMPDIR.
bench name *FLAGS='':
    #!/usr/bin/env bash
    set -euo pipefail
    # The parse tier lives in the carved //src/iiifparser/cpp/value_objects
    # package, the decode/encode tiers in //src/format_handlers, and the
    # process tier in //src/image_processing (ADR-0003).
    case "{{name}}" in
        parse)         pkg="src/iiifparser/cpp/value_objects" ;;
        decode|encode) pkg="src/format_handlers" ;;
        process)       pkg="src/image_processing" ;;
        *)             pkg="src" ;;
    esac
    bazel build -c opt //${pkg}:{{name}}_benchmark
    shopt -s nullglob
    fixtures=(./bazel-bin/${pkg}/{{name}}_benchmark.runfiles/+*sipi_bench_fixtures)
    env_args=()
    [ ${#fixtures[@]} -gt 0 ] && env_args+=("SIPI_BENCH_FIXTURES_DIR=${fixtures[0]}")
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    env SOURCE_DATE_EPOCH=946684800 SIPI_WORKSPACE_ROOT="." TEST_TMPDIR="$TMP" \
        ${env_args[@]+"${env_args[@]}"} \
        ./bazel-bin/${pkg}/{{name}}_benchmark "${@:2}"

# Compare two `just bench` JSON outputs: per-benchmark Mann-Whitney
# U-test deltas + OVERALL_GEOMEAN via the vendored google_benchmark
# compare.py (`//tools/benchmark:compare`, hermetic python + numpy/scipy).
# The regression decision rule lives in
# docs/src/development/benchmarking.md. Extra FLAGS (e.g.
# `--alpha=0.01`, `--no-utest`) are global compare.py options and must
# precede the subcommand. Paths are resolved to absolute because
# `bazel run` does not execute in the invocation directory.
bench-compare before after *FLAGS='':
    #!/usr/bin/env bash
    set -euo pipefail
    BEFORE="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    AFTER="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
    # `set positional-arguments` + the `*FLAGS=''` default expand to a single
    # empty positional when no flags are given, which compare.py would read as
    # the (invalid) `mode`. Keep only the non-empty extra flags.
    flags=()
    for f in "${@:3}"; do [ -n "$f" ] && flags+=("$f"); done
    bazel run //tools/benchmark:compare -- ${flags[@]+"${flags[@]}"} benchmarks "$BEFORE" "$AFTER"

# Concurrent-load decode measurement (local dev loop, never CI-gated). Unlike
# `just bench` (single-request microbench), this drives the production Rust
# shell under N concurrent clients requesting distinct native-resolution JP2
# tiles — every request decodes — and reports throughput + latency percentiles
# per concurrency level. Answers "how does a decode-path change behave at the
# request pool's saturation point", which the isolated microbench cannot.
# Compare two runs by eye (run on each branch); see benchmarking.md.
# Usage: just loadtest-decode "10,20,40"
loadtest-decode concs='10,20,40' dur='20' warm='5' id='knora/load_test.jpx':
    #!/usr/bin/env bash
    set -euo pipefail
    bazel build -c opt //src/cli/rust:sipi
    port=2048
    rm -rf ./loadtest_cache && mkdir -p ./loadtest_cache
    ./bazel-bin/src/cli/rust/sipi server --config config/sipi.loadtest-config.lua \
        --serverport "$port" --drain-timeout 2 >/tmp/sipi-loadtest.log 2>&1 &
    srv=$!
    trap 'kill $srv 2>/dev/null || true; wait $srv 2>/dev/null || true' EXIT
    for _ in $(seq 1 60); do
        curl -fsS "http://localhost:$port/health" >/dev/null 2>&1 && break
        kill -0 $srv 2>/dev/null || { echo "server died; see /tmp/sipi-loadtest.log"; exit 1; }
        sleep 0.5
    done
    python3 tools/loadtest/loadgen.py "http://localhost:$port" "{{id}}" 512 max "{{concs}}" "{{dur}}" "{{warm}}"

#####################################
# Bazel Docker (rules_oci)
#
# Each per-arch CI runner builds + loads the matching architecture's
# image into its local Docker daemon (`bazel-docker-build-${arch}`) and
# pushes per-arch digests to the registry (`bazel-docker-push-${arch}`).
# A coordinator job runs `bazel-docker-publish-manifest` to stitch the
# two pushed digests into a multi-arch manifest at
# `daschswiss/sipi:v<version>` via `crane index append`.
#
# `oci_image_index` is intentionally not used — its `images=[…]` attr
# accepts only same-build-graph labels, not external digests pushed by
# other runners (see dasch-specs research-findings §1).
#####################################

# Build the per-arch image and load it into the local Docker daemon as
# `daschswiss/sipi:latest`. Smoke-test consumes `latest`; CI re-tags
# with `:v<version>-${arch}` via `bazel-docker-push-${arch}` below.
# `--stamp` runs `tools/workspace_status.sh` so STABLE_SIPI_VERSION
# (from version.txt) is baked into image labels and tag suffixes;
# without it, tags resolve to `0.0.0-unstamped-${arch}` and `publish.yml`'s
# version-based downstream steps (Sentry release naming, Scout
# environment recording) silently break.
#
# `--config=release` is the production build: `-c opt` codegen (-O3 -DNDEBUG) +
# `_FORTIFY_SOURCE=2` hardening (both inert at the default fastbuild `-O0`; see
# .bazelrc). The push and debug-extract recipes below carry `--config=release`
# too — they must agree, or the pushed image / Sentry debug symbols won't match
# the smoke-tested binary.
bazel-docker-build-amd64 *FLAGS='':
    bazel run --config=release --stamp --platforms=//platforms:linux_x86_64 --verbose_failures {{FLAGS}} //src:image_load

bazel-docker-build-arm64 *FLAGS='':
    bazel run --config=release --stamp --platforms=//platforms:linux_aarch64 --verbose_failures {{FLAGS}} //src:image_load

# Cross-build the Linux OCI image for `arch` (amd64|arm64) WITHOUT loading it
# into a Docker daemon. Builds `//src:sipi_image` under
# `--platforms=//platforms:linux_x86_64` / `:linux_aarch64` so a host of any OS (e.g. a
# macOS dev box, where there is no Linux Docker daemon so `bazel-docker-build-*`
# cannot `docker load`) can prove every native dep cross-compiles through the
# relocatable hermetic clang toolchain. Cross-compilation is not currently
# CI-gated (a gate was prototyped and removed for being too slow; being
# reconsidered) — this recipe is the local check. No `--stamp`: cross-compile
# validity is independent of the workspace-status version embed.
bazel-cross-build-image arch *FLAGS='':
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{arch}}" in
        amd64) PLATFORM=//platforms:linux_x86_64 ;;
        arm64) PLATFORM=//platforms:linux_aarch64 ;;
        *)
            echo "ERROR: unknown arch '{{arch}}' (expected: amd64, arm64)" >&2
            exit 1
            ;;
    esac
    bazel build --platforms="$PLATFORM" --verbose_failures {{FLAGS}} //src:sipi_image

# Push the per-arch image to docker.io/daschswiss/sipi with two tags:
# `latest-${arch}` and `v<version>-${arch}`. Driven by `oci_push`'s
# `remote_tags` attribute consuming `:remote_tags_${arch}`. The
# coordinator job (`bazel-docker-publish-manifest` below) reads these
# digests off the registry to assemble the multi-arch manifest.
bazel-docker-push-amd64 *FLAGS='':
    bazel run --config=release --stamp --platforms=//platforms:linux_x86_64 --verbose_failures {{FLAGS}} //src:image_push_amd64

bazel-docker-push-arm64 *FLAGS='':
    bazel run --config=release --stamp --platforms=//platforms:linux_aarch64 --verbose_failures {{FLAGS}} //src:image_push_arm64

# Assemble the multi-arch manifest at `daschswiss/sipi:v<version>` from
# the two per-arch digests already pushed by `bazel-docker-push-*`.
# Also applies the `latest` tag to the manifest so downstream consumers
# can pull `daschswiss/sipi:latest` and get the right arch automatically.
#
# Pre-conditions (held by publish.yml's job graph):
#   - `daschswiss/sipi:v<version>-amd64` and `-arm64` exist in the
#     registry (pushed by the per-arch publish-docker matrix jobs)
#   - `crane` on PATH (declared in flake.nix devShells)
#   - Docker Hub credentials available (handled by docker/login-action
#     in publish.yml; `crane` reads ~/.docker/config.json)
bazel-docker-publish-manifest:
    #!/usr/bin/env bash
    set -euo pipefail
    VERSION="$(tr -d '[:space:]' < version.txt)"
    BASE="docker.io/daschswiss/sipi"
    echo "==> Assembling ${BASE}:v${VERSION} from per-arch digests"
    crane index append \
        -m "${BASE}:v${VERSION}-amd64" \
        -m "${BASE}:v${VERSION}-arm64" \
        -t "${BASE}:v${VERSION}"
    echo "==> Tagging manifest as :latest"
    crane tag "${BASE}:v${VERSION}" latest
    crane manifest "${BASE}:v${VERSION}" >/dev/null
    echo "Multi-arch manifest published: ${BASE}:v${VERSION}"

# Build `:sipi_debug_layout` and surface the .debug file at
# `sipi-${arch}.debug` for `sentry-cli debug-files upload` consumption.
# Filename matches today's `nix-docker-extract-debug` recipe so
# publish.yml's existing Sentry-upload step is untouched (only the
# upstream changes from Nix to Bazel; the contract — `sipi-${arch}.debug`
# at the workspace root — is preserved verbatim).
#
# `arch` is `amd64` or `arm64`; the recipe selects the Bazel platform
# accordingly. CI's per-arch runners pass their matching value.
bazel-docker-extract-debug arch *FLAGS='':
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{arch}}" in
        amd64) PLATFORM=//platforms:linux_x86_64 ;;
        arm64) PLATFORM=//platforms:linux_aarch64 ;;
        *)
            echo "ERROR: unknown arch '{{arch}}' (expected: amd64, arm64)" >&2
            exit 1
            ;;
    esac

    # Build the laid-out debug-tree tarball and extract its single
    # `.debug` file. The tarball layout is `lib/debug/.build-id/<xx>/<yy>.debug`;
    # the filename is the build-id with the leading two hex chars removed
    # and `.debug` appended.
    #
    # `--remote_download_toplevel` overrides the .bazelrc BwoB default
    # (`--remote_download_minimal`): the genrule runs on RBE, so without this
    # the tarball stays in the CAS and the `tar -xf` below can't open it. A
    # no-op in local dev (no remote configured).
    bazel build --config=release --stamp --platforms="$PLATFORM" --verbose_failures --remote_download_toplevel {{FLAGS}} //src:sipi_debug_layout

    # Find the single `.debug` file inside the tar. The tarball is
    # deterministic by construction, so this glob always resolves
    # to exactly one entry.
    workdir=$(mktemp -d)
    trap 'rm -rf "$workdir"' EXIT
    tar -xf bazel-bin/src/sipi_debug_layout.tar -C "$workdir"
    debug_file=$(find "$workdir/lib/debug/.build-id" -name '*.debug' | head -1)
    if [ -z "$debug_file" ]; then
        echo "ERROR: no .debug file found inside sipi_debug_layout.tar" >&2
        exit 1
    fi
    cp "$debug_file" "sipi-{{arch}}.debug"
    echo "Debug symbols copied to: sipi-{{arch}}.debug"

# Run sipi with the localdev config.
run: bazel-build-server
    #!/usr/bin/env bash
    set -euo pipefail
    SIPI_BIN="${SIPI_BIN:-{{justfile_directory()}}/bazel-bin/src/cli/rust/sipi}"
    "$SIPI_BIN" server --config={{justfile_directory()}}/config/sipi.localdev-config.lua

# Start a local LGTM observability stack (Grafana on :3000, OTLP gRPC :4317 /
# HTTP :4318) to test the Rust shell's traces + logs end to end. Open
# http://localhost:3000 (admin/admin) → Explore → Tempo/Loki. Run `just run-otel`
# in another terminal to point SIPI at it. Ctrl-C stops + removes the container.
lgtm-up:
    docker run --rm --name sipi-lgtm -p 3000:3000 -p 4317:4317 -p 4318:4318 grafana/otel-lgtm

# Run the Rust shell against the local LGTM stack (run `just lgtm-up` first).
# Exports traces over OTLP and — via SIPI_OTLP_LOGS — also ships logs to the
# local Loki (production scrapes stdout instead, so OTLP log export is dev-only).
# C++ engine logs still print to stdout here; their trace_id correlates in Tempo.
run-otel: bazel-build-server
    #!/usr/bin/env bash
    set -euo pipefail
    # service.version + deployment.environment.name land on the OTel resource via
    # OTEL_RESOURCE_ATTRIBUTES (Resource::builder() merges the env detector); a
    # production deploy sets the same vars (the ops-deploy iiif service block).
    version="$(cat "{{justfile_directory()}}/version.txt")"
    OTEL_EXPORTER_OTLP_ENDPOINT=http://localhost:4317 \
    OTEL_SERVICE_NAME=sipi \
    OTEL_RESOURCE_ATTRIBUTES="service.namespace=dsp,service.version=${version},deployment.environment.name=dev" \
    SIPI_OTLP_LOGS=1 \
    RUST_LOG="${RUST_LOG:-info}" \
    "{{justfile_directory()}}/bazel-bin/src/cli/rust/sipi" server \
        --config="{{justfile_directory()}}/config/sipi.localdev-config.lua" --serverport 1024

# Run sipi under Valgrind.
valgrind: bazel-build-server
    #!/usr/bin/env bash
    set -euo pipefail
    SIPI_BIN="${SIPI_BIN:-{{justfile_directory()}}/bazel-bin/src/cli/rust/sipi}"
    valgrind --leak-check=yes --track-origins=yes "$SIPI_BIN" server --config={{justfile_directory()}}/config/sipi.config.lua

#####################################
# Documentation
#####################################

# Build docs into the local 'site' folder
docs-build:
    cd docs && mkdocs build

# Serve docs for local viewing
docs-serve:
    cd docs && mkdocs serve

# Build and publish docs to Github Pages
docs-publish:
    cd docs && mkdocs gh-deploy

# Install requirements for documentation
docs-install-requirements:
    pip3 install -r docs/requirements.txt

#####################################
# Utilities
#####################################

# Clean local build artifacts (Bazel symlinks + docs site + IDE-emitted dirs).
# Bazel's own cache is at `bazel info output_base`; use `bazel clean
# --expunge` to drop it.
clean:
    rm -rf bazel-bin bazel-out bazel-testlogs bazel-* || true
    rm -rf site/
