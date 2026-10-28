# Fuzz Testing

Coverage-guided libFuzzer fuzzing across two families of attacker-controlled
input: the production IIIF URL parser, and the C++ codec decode handlers
(TIFF, JPEG, PNG, JPEG 2000; DEV-7066). Both compose the same way — a
`cc_fuzz_test` from `rules_fuzzing`, `--config=fuzz` for the mutation loop,
`--config=fuzz --config=asan` for the sanitizer-paired pass — but they fuzz
different code and different bug classes, and the sections below call out
where the two diverge.

## The production IIIF parser

Fuzzing of `iiif_parser::parse_request` (`src/iiifparser/rust/request.rs`).
That function is the first parser every HTTP request hits: it classifies the
URI (`Iiif | InfoJson | KnoraJson | Redirect | FileDownload`) and parses
`{region}/{size}/{rotation}/{quality}.{format}` out of attacker-controlled
input, so a panic there is a remote-triggerable request failure.

The harness lives entirely inside Bazel. The target is
`//src/iiifparser/fuzz:parse_request_fuzz`, a `cc_fuzz_test` from
`rules_fuzzing`, seeded from the shared regression corpus
`//src/iiifparser/corpus:seed_corpus`. It replaces the C++ libFuzzer harness that
fuzzed the oracle-only classifier and was retired with the oracle
([ADR-0020](../../adr/0020-oracle-removal.md)).

### The C++→Rust shim seam

libFuzzer's entry point is `LLVMFuzzerTestOneInput`, a C symbol, and
`rules_fuzzing` is a C++/Java rule set — so the Rust parser is entered through a
small `extern "C"` shim:

```
src/iiifparser/fuzz/
├── BUILD.bazel       # rust_static_library shim + cc_fuzz_test
├── shim.rs           # extern "C" sipi_fuzz_parse_request → parse_request
└── fuzz_target.cc    # LLVMFuzzerTestOneInput → shim
```

This is the repo's only **C++-calls-Rust** link — every other FFI edge
(`src/ffi/`, `src/server/rust/src/ffi.rs`) is Rust→C++. The declaration in
`fuzz_target.cc` is hand-mirrored from the Rust signature, following the
`src/ffi/cpp/sipi_ffi.h` convention (no cbindgen).

The seam never leaves the package, and Bazel enforces that rather than merely
documenting it: both targets are `testonly` (`cc_fuzz_test` forwards `testonly`
to its raw `cc_binary`, and marks every target it generates `testonly` itself),
and the package declares no default visibility. Nothing that ships can even name
the shim, let alone depend on it. Naming a target on the command line needs no
visibility, so `just fuzz` and the nightly still build `:parse_request_fuzz_bin`
directly.

The shim rejects invalid UTF-8 rather than lossy-converting it: every production
caller (axum path extraction) hands the parser a valid `&str`, so a U+FFFD input
would only yield findings unreachable from a real request. The parse result is
discarded — `Err` is a normal outcome for a malformed URI; the contract under
test is that no input panics. This is also what a corpus file *means* here: a
seed is fed to the parser verbatim, and a non-UTF-8 seed is a no-op. Note that
`corpus_regression_test` sweeps the same files with a **lossy** decode, so it can
exercise a byte sequence this harness skips.

## The C++ codec decode handlers

Fuzzing of the `SipiIO` decode entry points — `read_shape` and `read` — for
each of the four format handlers: `SipiIOTiff`, `SipiIOJpeg`, `SipiIOPng`,
`SipiIOJ2k` (DEV-7066). Eight targets live in `src/format_handlers/fuzz/`:
a decode-only target per handler (`//src/format_handlers/fuzz:tiff_decode_fuzz`,
`:jpeg_decode_fuzz`, `:png_decode_fuzz`, `:j2k_decode_fuzz`) and a matching
decode-then-encode round-trip target (`:tiff_roundtrip_fuzz`, `:jpeg_roundtrip_fuzz`,
`:png_roundtrip_fuzz`, `:j2k_roundtrip_fuzz`). This reopens the codec-level gap
[ADR-0020](../../adr/0020-oracle-removal.md) left when the oracle-only C++
fuzz harness was retired — no other test layer feeds these handlers arbitrary
byte streams. The motivating bug class is crafted-input memory corruption in
header/marker parsing (TIFF IFD entries, JPEG APPn segments, PNG chunks,
JP2 boxes) that no unit test catches and that hand review finds only by luck.

The decode-only targets never call `SipiIO::write`, so every encoder was
unreachable from the fuzzer — the blind spot a JP2-encode bug and a
JPEG-marker-write bug both shipped through undetected. The round-trip targets
close it: `run_roundtrip` (`codec_fuzz_harness.h`) decodes exactly like the
decode-only pass, then, if the decode produced an image, re-encodes it through
every format `SipiImage::write` supports ("tif", "jpx", "png", "jpg"). A write
failure or thrown exception is a valid fuzz outcome, same as a decode
rejection; only a crash or sanitizer report is a finding.

### The temp-file mechanism

`SipiIO` has no in-memory decode overload anywhere — every handler's
`read_shape`/`read` take a filesystem path — so the shared harness
(`src/format_handlers/fuzz/codec_fuzz_harness.h`) writes each fuzzer-supplied buffer to
one fixed per-process temp path before driving the handler over it. The path
name embeds `getpid()`: libFuzzer runs each process single-threaded, so a
per-iteration temp file with a stable, truncated-and-rewritten name avoids
filesystem churn, and the pid keeps parallel `-jobs=N` workers from colliding
on the same path. `TEST_TMPDIR` is honoured when set (Bazel's `cc_fuzz_test`
replay engine sets it), falling back to the platform temp directory otherwise.

### Expected-reject vs finding

Each call to `read_shape` and `read` returns a `Result`, and the harness
checks it only for presence: a codec rejecting malformed input reports that
through the `Result` holding no value, not through an exception — so a clean
rejection from `read_shape` still lets `read` run. Each call is additionally
wrapped in its own try/catch, because a decoder can still throw en route to
producing that `Result`: `std::bad_alloc`, the allocation-guard throws that
stay exception-based by design (`checked_buf_size_or_throw`, `memTiffOpen`'s
raw `malloc` failures), and Kakadu's `kdu_exception`. Findings are what
escapes both catch blocks entirely: SIGSEGV, SIGABRT, sanitizer reports. The
harness also has a bare `catch (...)` alongside `catch (const std::exception
&)`, because `kdu_exception` is an `int`-like type, not a `std::exception` —
without it, a Kakadu-thrown rejection on a malformed JP2 would itself register
as an uncaught-exception finding.

### The decode-size budget

The shape probe (`read_shape`) always runs, on every input — the full second
step, `read`, does not. The harness estimates the decode buffer implied by the
probe's geometry and skips `read` entirely once that estimate exceeds 512 MiB
(`kMaxHarnessDecodeBytes` in `codec_fuzz_harness.h`); container/header parsing
coverage is untouched by the skip, since `read_shape` is where that coverage
lives.

The budget exists because `validate_decode_dims` (`src/image/cpp/SipiIO.h`)
caps each dimension and the channel count individually, but never their
product — so a small, cheaply crafted header can legally claim a
multi-gigabyte decode buffer and still pass the guard. In production, that
claim is bounded at the FFI seam by the full-lane decode memory budget
(`MemoryBudgetGuard`, `src/ffi/cpp/serve_image.cpp`); the harness drives the
handler directly and has no equivalent budget, so without the skip a
275-byte file can end a fuzzing leg on an out-of-memory that says nothing
about codec correctness.

Raising `-rss_limit_mb` is not a substitute: a crafted header can claim
dimensions up to the per-dimension cap on both axes at once, so no fixed RSS
limit survives that argument — only skipping the oversized decode does.

### Per-target knobs

Each codec target passes `-max_len` and (except J2K) a `-dict=` flag; all nine
targets (including the parser) pass `-timeout=25`.

- **`-max_len` caps**: 16 KB for TIFF and JPEG (decode and round-trip alike),
  8 KB for PNG (decode and round-trip), 32 KB for the J2K decode target, 16 KB
  for the J2K round-trip target. The bug class these harnesses hunt is
  header/marker parsing at the front of the file, so a small cap concentrates
  the mutation budget there instead of on bulk pixel data a full-size image
  would carry.
- **Dictionaries**: `src/format_handlers/fuzz/dicts/{tiff,jpeg,png}.dict`, vendored
  verbatim from AFL++ (see the package README for provenance and license).
  Each round-trip target reuses its decode-side counterpart's dictionary.
  There is no canonical J2K dictionary upstream, so neither J2K target runs
  with one. They are passed as explicit `-dict=` flags from the `justfile` recipes
  rather than through `rules_fuzzing`'s `dictionary` `cc_fuzz_test` attribute:
  that attribute only reaches libFuzzer through the rule's Python launcher via
  `FUZZER_DICTIONARY_PATH`, and this repo executes the built `..._bin` binary
  directly rather than through the launcher — running via `bazel run` (which
  the launcher needs) would sandbox away corpus growth (see Recipes below).
- **`-timeout=25`**: applied uniformly, including to the parser target and to
  `fuzz-corpus-merge`'s `-merge=1` pass — libFuzzer's 1200s per-input default
  exceeds any of these budgets, so a hang would silently consume a run instead
  of being reported as a finding.

### `detect_leaks`

The ASan-paired pass (below) sets `ASAN_OPTIONS=detect_leaks=0` for the JPEG
and PNG legs only. libjpeg-turbo's and libpng's error paths raise via
`setjmp`/`longjmp`, which skips destructors past the jump — a documented LSan
false-positive source, not a real leak. TIFF and J2K unwind via real C++
exceptions and keep leak detection on, and the parser target carries no ASan
special-casing at all. This is never disabled repo-wide, and it is an
outcome adopted up front rather than one found by running the pass: LSan is a
Linux-only ASan feature, and the local macOS ASan link is broken (see the
sanitizer gate notes), so it cannot be verified on a developer Mac — the first
nightly run against real crafted-JPEG/PNG input is what confirms the
false-positive theory rather than an unrelated leak.

## Two modes, one target family

`rules_fuzzing`'s default engine is `//fuzzing/engines:replay` with
instrumentation `none`, so **without** `--config=fuzz` every target — the
parser and all eight codec handlers (decode and round-trip) — needs no
libFuzzer runtime and, e.g.,

```bash
bazel test //src/iiifparser/fuzz:parse_request_fuzz
bazel test //src/format_handlers/fuzz:tiff_decode_fuzz
bazel test //src/format_handlers/fuzz:tiff_roundtrip_fuzz
```

is a corpus-replay regression run: every seed through the harness, asserting no
crash. All nine build and run on every platform — macOS included — and ride
along in the `//src/...` sweeps (`just bazel-test`, `bazel-test-unit`,
`bazel-test-sanitized`, `bazel-coverage`), so every PR replays every corpus, and
the sanitizer leg replays them under ASan/UBSan.

`--config=fuzz` (`.bazelrc` §Fuzzing) switches to the real libFuzzer engine and
arms SanitizerCoverage. That is the mutation loop. For the parser target this
also arms coverage on the Rust crate graph (below); for the codec targets it
arms coverage across the linked C++ handler code directly, since those targets
have no Rust in their dependency graph.

**Coverage instrumentation of the Rust side** (parser target only) uses only
stable rustc flags — `-Cpasses=sancov-module` plus
`-Cllvm-args=-sanitizer-coverage-*`, applied config-wide through `rules_rust`'s
singular `extra_rustc_flag` build setting (target config only; exec-config
proc-macros and build scripts are untouched). No nightly toolchain, no
`-Zsanitizer`, no Cargo. libFuzzer observes the Rust edges: 894 inline 8-bit
counters versus 8 for a shim-only build, and a recommended dictionary of
substrings of Rust-only string literals (`ault` from `default`) that exist
nowhere in the C++ shim.

The config also sets `--compilation_mode=opt`. Fuzzing throughput *is* coverage —
a nightly has a fixed wall-clock window — and there is nothing to step through in
a debugger, since a finding is a saved reproducer replayed separately. Two flags
come back on top of it, for the Rust parser target:

- `-Cdebug-assertions=on` and `-Coverflow-checks=on`, which `-c opt` turns off.
  An integer overflow in a coordinate or dimension parser is exactly the bug
  class this target hunts, and it is a panic in debug and a silent wraparound in
  release — turning the checks back on is what makes the fuzzer see it.
- `std::hint::black_box` around the discarded parse result in `shim.rs`, so LLVM
  cannot prove the call dead and delete the code under test.

`-Cpanic=abort` is deliberately **not** set. It would apply config-wide and so
break `bazel test --config=fuzz` on every Rust test target (the prebuilt
sysroot's `test` crate is unwind-compiled), and it buys nothing: a Rust panic
unwinding out of an `extern "C"` fn has aborted since Rust 1.71, so the
panic→SIGABRT→libFuzzer-crash path through `sipi_fuzz_parse_request` is a
language guarantee. Verified with an injected panic: the abort arrives via
`core::panicking::panic_cannot_unwind`, exit code 77, reproducer saved.

### ASan pairing

`--config=fuzz --config=asan` composes — the repo's own sanitizer config, not
`rules_fuzzing`'s `cc_engine_sanitizer` (which injects `-fsanitize=…` without
provisioning a runtime and must stay at its `none` default). For the codec
targets, ASan instruments the actual decoder C++ code directly (TIFF/JPEG/
PNG/J2K are C++ all the way down from the harness to the handler), so this is
the primary sanitizer coverage for that bug class, not a boundary check. For
the parser target, mind the narrower scope: ASan instruments the C++ shim,
libFuzzer itself, and the linked native runtime — **not** the Rust crate graph
(that would need nightly `-Zsanitizer`, which this design avoids). The value
of the paired pass there is the shim boundary (`*const u8, usize` → `&[u8]` →
`&str`), libFuzzer's own buffer handling, and the runtime; the Rust parser is
safe code that already runs under the ASan/UBSan CI leg via its unit,
corpus-regression, and replay tests.

### Linux and macOS

`--config=fuzz` links on both platforms. Since **0.8.18**, hermetic-llvm stages
the compiler-rt libFuzzer runtime for darwin (`libclang_rt.fuzzer_osx.a`, upstream
[PR #700](https://github.com/hermeticbuild/hermetic-llvm/pull/700)), closing the
gap where `@llvm//toolchain:resource_dir` used to select `[]` for
`@platforms//os:macos`. The clang driver also links `libclang_rt.ubsan_osx_dynamic`
on darwin, so `--@llvm//config:ubsan=true` is required there too (already set by
`--config=fuzz`). This applies uniformly to all nine targets, including the
Kakadu-linked J2K harnesses (decode and round-trip) — Kakadu itself is a
native `cc_library` dependency with no darwin-specific gap here.

One macOS-only wrinkle, unrelated to the LLVM runtime: `rules_fuzzing`'s Python
launcher deps (`absl-py`) resolve as an sdist, and `rules_python`'s macOS sdist
path shells out to `xcrun xcodebuild`, which the Nix dev shell's SDK stub lacks.
`.bazelrc` handles this globally by pointing the **repo-rule** `DEVELOPER_DIR` at
the Command Line Tools (`/Library/Developer/CommandLineTools`), which makes
`rules_python` take its non-Xcode branch and skip the `xcodebuild` call — for the
replay build in the `//src/...` sweep as well as `--config=fuzz`. Build actions
are unaffected; they compile against the hermetic-llvm SDK. A macOS dev therefore
needs the Command Line Tools installed (`xcode-select --install`); no full Xcode.

The broader `--config=fuzz --config=asan` pairing stays Linux-only for the
nightly mutation loop — the macOS ASan header path is still gapped (see the
sanitizer gate) — but the plain replay-mode `bazel test` of every target,
codec targets included, runs on macOS same as Linux.

## Recipes

```bash
just bazel-build-fuzz                        # build all five instrumented binaries (Linux + macOS; CI-invoked, RBE-eligible)
just fuzz tiff -max_total_time=60             # run the mutation loop locally; TARGET selects the harness, FLAGS pass through to libFuzzer
just fuzz-corpus-merge tiff                   # import coverage-adding inputs from the latest nightly artifact for TARGET
```

`TARGET` is a short name, not a Bazel label: `parse_request` (the default),
`tiff`, `jpeg`, `png`, `j2k`.

`bazel-build-fuzz` builds the `_bin` and `_corpus` targets for all five
harnesses in one invocation — e.g.
`//src/iiifparser/fuzz:parse_request_fuzz_bin` and
`//src/format_handlers/fuzz:tiff_decode_fuzz_bin` — the `_bin` target, not the test
target: `_bin` is what the loop executes, and only a top-level target
materialises locally under the `--remote_download_minimal` default. Each
target's path (e.g.
`bazel-bin/src/iiifparser/fuzz/parse_request_fuzz_bin`) is a stable symlink
`fuzzing_binary` declares into the transitioned config's config-hashed
`…_raw_` path; never hardcode the latter. Every generated target except the
test itself is `manual`-tagged upstream, so the `_bin`/`_corpus` targets are
built by naming them explicitly, not by a `//src/...` wildcard.

`just fuzz` and the nightly both execute the built binary **directly**, not via
`bazel run` — the sandbox would silently discard corpus growth. The working
corpus for a given `TARGET` lives under the gitignored `.fuzz/corpus/<TARGET>/`
— strictly per-format, never shared across targets — re-seeded on every run
from the target's built `_corpus` target under `bazel-bin`, which already
merges both corpus tiers (below), so there is no separate seed list to
duplicate. Crash reproducers land in `.fuzz/artifacts/`.

Each `cc_fuzz_test` also generates a `<name>_fuzz_run` launcher for
`bazel run`; it is not what CI or the recipes use, for the sandbox reason above.

## Corpus policy — two tiers plus the merge path between them

The parser and the codec targets share the same two-tier shape, but source
the hand-picked tier differently.

1. **Checked-in seed corpus.**
   - Parser: `src/iiifparser/corpus/` (241 files, exposed as
     `//src/iiifparser/corpus:seed_corpus`), shared with the C++ classifier
     test and `//src/iiifparser/rust:corpus_regression_test`.
   - Codec targets: two separate locations merged by the `seed_corpus`
     filegroup in each `src/format_handlers/corpus/<fmt>/BUILD.bazel` — the
     hand-picked fixtures reached through
     `//test/_test_data:fuzz_seeds_{tiff,jpeg,png,j2k}` (crafted-malformed
     images already used by that codec's unit tests, plus a smallest-valid
     fixture; these are Git LFS), and the checked-in `src/format_handlers/corpus/<fmt>/`
     directory itself as the growth tier (empty today).

   Both cases grow only deliberately: a human-committed merge (below), or a
   crash reproducer committed alongside the fix for the bug it reproduces. CI
   never writes either.

2. **Live working corpus** — chained between nightly runs as a GitHub Actions
   **artifact** per target, `fuzz-corpus-<target>`, not a cache. Artifacts
   chain explicitly across runs, are downloadable for the manual merge step,
   and are not subject to cache eviction. Each nightly leg seeds
   `.fuzz/corpus` from that target's generated two-tier corpus (the built
   `_corpus` Bazel target — for the codec targets, that generated directory is
   what makes the `test/_test_data` fixture seeds reachable at all by a binary
   executed outside Bazel) plus the artifact of the last successful run
   (falling back to the generated seeds alone on the first run or after the
   90-day retention expires), fuzzes, minimizes with libFuzzer `-merge=1`, and
   uploads the result as this run's artifact.

3. **Periodic pull-into-repo** — `just fuzz-corpus-merge <target>` downloads
   the latest `fuzz-corpus-<target>` artifact and `-merge=1`s it into that
   target's checked-in corpus directory (`src/iiifparser/corpus/` or
   `src/format_handlers/corpus/<fmt>/`), so only coverage-adding inputs are imported,
   then prints the diff. Reviewing and committing is manual. This is the only
   path from the live corpus to the checked-in one.

   **Known imprecision for the codec targets**: the checked-in tier
   (`src/format_handlers/corpus/<fmt>/`) does not contain the `test/_test_data`
   fixture seeds — those live in a separate package. `-merge=1` only sees
   coverage relative to its destination directory, so it can propose importing
   an input whose coverage the fixtures already reach but the checked-in
   directory does not yet contain. The recipe does not attempt to correct for
   this; it stops at a diff precisely so a maintainer reviews what is actually
   being added rather than trusting the merge blindly.

`-merge=1 <dst> <src>` is libFuzzer's own minimization — it copies an input into
the destination only if it adds coverage the destination lacks, which is why it
needs the instrumented binary.

## Nightly CI

`.github/workflows/fuzz.yml`, scheduled `17 3 * * *` (03:17 UTC — off-peak for
the team and off the congested top of the hour), plus `workflow_dispatch` for
manual runs. One job, a **9-leg matrix** (`parse_request`, `tiff`, `jpeg`,
`png`, `j2k`, `tiff_roundtrip`, `jpeg_roundtrip`, `png_roundtrip`,
`j2k_roundtrip`) on `ubuntu-24.04` with `fail-fast: false` — a crash in one leg
must never discard another target's corpus growth for the night. Each leg runs
the same five phases: build, restore the working corpus, fuzz for 600s,
minimize and upload the `fuzz-corpus-<target>` artifact, then a 300s
ASan-paired pass over the minimized corpus.

The minimize-and-upload steps run on `!cancelled()`, and the ASan pass comes
after them, so a finding anywhere still chains that leg's corpus growth
forward — losing a night of coverage to a crash would be a second injury. This
is safe because libFuzzer writes reproducers to `-artifact_prefix` and never
into the corpus directory, so a crashing input cannot enter the chain via the
merge. The ASan pass's own corpus additions are deliberately discarded — it
exists to exercise the codec/shim/runtime boundary, not to grow coverage.

Two requirements are specific to the codec legs:

- **Git LFS must be checked out** (`lfs: true` on the checkout, plus
  `lfs: "true"` into the CI-setup composite action). The codec seed corpora
  route through `test/_test_data`, which is Git LFS. Without it, those
  fixtures materialise as ~131-byte pointer files, and the codec harnesses
  would spend their whole budget fuzzing pointer text instead of image bytes
  — silently worthless coverage, not a loud failure, which is why this is
  called out rather than left implicit.
- **`GH_TOKEN` must be job-level**, not step-level: `kakadu_archive`'s
  `gh_release_archive` repository rule re-evaluates on every `bazel`
  invocation, and each leg invokes `bazel` (via `bazel-build-fuzz`) twice —
  the plain build and the ASan-paired build. A step-scoped token would miss
  whichever invocation didn't carry it. The parser-only leg builds Kakadu too
  (one `bazel-build-fuzz` invocation builds all five binaries) even though it
  doesn't read it, so the job-level token covers every leg uniformly.

`-timeout=25` is passed to both loops and to the merge in every leg: libFuzzer's
1200s per-input default exceeds the whole budget, so a hang would silently
consume the run instead of being reported. The merge also gets
`-rss_limit_mb=4096`, since `-merge=1` holds the whole feature set in memory as
the corpus grows.

The two J2K legs (`j2k`, `j2k_roundtrip`) additionally run both loops in
libFuzzer fork mode with `-fork=1 -ignore_timeouts=1 -ignore_ooms=0
-timeout_exitcode=0` (matrix column `libfuzzer_extra`). Kakadu's box parser has an unpatchable hang class
(DEV-7080) that the seam deadline in `serve_image.cpp` contains in production,
but the codec harness drives `SipiIOJ2k::read_shape` directly, below that
seam, and the mutator finds a fresh instance on most nights. In fork mode the
parent keeps fuzzing past a timed-out child; the child still writes its
`timeout-*` reproducer to `-artifact_prefix`, so the instance is recorded in
the `fuzz-crashes-<target>` artifact of a green leg. `-timeout_exitcode=0`
is needed on top of `-ignore_timeouts=1` because the fork parent exits with
its *last* child's exit code, so a tolerated timeout in the final child would
otherwise still fail the step with 70. Crashes and OOMs still fail the leg. The other seven legs keep the strict per-input timeout: a hang
in libtiff/libjpeg/libpng or in Vitrinli's own code is a fixable finding.

The ASan pass runs every leg with `ASAN_OPTIONS=detect_container_overflow=0`.
The fuzz binary links the libFuzzer runtime, which hermetic-llvm builds
unsanitized (`cc_unsanitized_library`) against the same libc++ the
instrumented code uses, so libc++'s container annotations are updated by some
objects in the link and checked by others — the documented false-positive
shape for that one detector. It fired three times with no defect behind it:
the Rust parser leg (DEV-7081), `Exiv2::append` on `tiff_roundtrip` (the
identical decode-then-JP2-write path is green under the `asan-ubsan` CI job,
which links no libFuzzer; the reproducer is pinned as
`tiff_exif_make_jp2_roundtrip.tif`), and libFuzzer's own fork driver on the
j2k legs before a single input ran. Heap, stack, and global overflow detection
are unaffected, and the container detector stays on for the `asan-ubsan`
job's unit and e2e tests, which cover the same code without libFuzzer.

Both loops run with `RUST_BACKTRACE=1` and the hermetic `llvm-symbolizer`
(resolved to `.fuzz/llvm-symbolizer` by `just bazel-build-fuzz`) wired into the
sanitizer runtime that prints the crash trace — `UBSAN_OPTIONS=external_symbolizer_path`
for the plain loop, `ASAN_SYMBOLIZER_PATH` for the ASan pass. Without it,
first-party frames print as bare `binary+0xOFFSET`.

**Crash semantics.** A Rust panic aborts at the `extern "C"` boundary → SIGABRT
(parser target); a memory-safety finding in a codec handler raises a signal
directly (SIGSEGV/SIGABRT) or trips a sanitizer report under the ASan pass.
Either way libFuzzer writes the input as `crash-<sha1>` under
`-artifact_prefix` and exits **77**. That exit code fails the step, so the
workflow needs no crash-detection logic of its own — only preservation: the
reproducers and the libFuzzer logs are uploaded as a `fuzz-crashes-<target>`
artifact (30-day retention) on failure. Triage is manual; no issue is
auto-filed.

**Why nightly is enough.** The per-PR regression net for every target is the
replay-mode `bazel test` (the target's corpus through the harness and the
engine path, on every platform, and under ASan/UBSan on the sanitizer leg),
plus — for the parser only — `//src/iiifparser/rust:corpus_regression_test`
(the crate API swept directly over the whole corpus). A newly introduced crash
that the existing corpus already reaches fails the PR. Only a crash that
requires *mutation* to reach waits for the next nightly, and up to 24h of
latency on that class of finding is accepted deliberately — the alternative is
a long-running loop in the PR path.

## Crash triage

A nightly failure is a manual triage, not an auto-filed issue:

1. **Reproduce** from the saved artifact — `.fuzz/artifacts/` locally, or the
   `fuzz-crashes-<target>` CI artifact — by running the leg's built binary
   directly against the single reproducer file, e.g.:

   ```bash
   just bazel-build-fuzz
   ./bazel-bin/src/format_handlers/fuzz/tiff_decode_fuzz_bin path/to/crash-<sha1>
   ```

   The artifact directory is not exclusively crashes: libFuzzer names a saved
   reproducer `crash-`, `oom-`, or `timeout-` depending on how the process
   died, and the CI artifact is uploaded as `fuzz-crashes-<target>` regardless
   of which prefix it holds.

2. **Classify the finding** before deciding what to do with it — a nightly
   failure is not automatically a codec bug:
   - **Sanitizer report inside Vitrinli code.** A real bug. Steps 3-5 below apply
     as written.
   - **Sanitizer report whose stack is entirely inside libFuzzer or the C++
     runtime.** Not a Vitrinli finding. One observed instance was an ASan report
     inside libFuzzer's own `Sha1ToString` → `std::basic_stringbuf::str()` →
     libc++'s internal-buffer initialisation, with the saved reproducer being
     the *empty input* — a tell that the finding sits outside the target
     entirely. This gets its own issue against the toolchain, not a `fix:` on
     a codec.
   - **Out-of-memory (`oom-` prefix).** Check the reproducer's claimed
     geometry first. An allocation the decode-size budget above is meant to
     prevent means the harness needs adjusting, not the codec — the codec
     never got the chance to reject it. An allocation the codec should have
     refused on its own is a real bug.
   - **Timeout (`timeout-` prefix).** Reproduce with the timeout raised before
     dismissing it as a slow decode. A genuine hang inside a third-party codec
     is still a finding — it is an availability bug on the production
     `read_shape`/`read` path, even though the fix cannot be a patch to that
     codec's source. **Never pin a hanging input into the seed corpus**: the
     corpus is replayed by `bazel test` on every PR, so a hanging seed would
     hang the test suite instead of failing it.
3. **Fix as its own commit**, typed `fix:` — the bug already exists on `main`,
   so `fix:` is the correct Conventional Commit type regardless of when the
   fuzz target that found it landed.
4. **Commit the reproducer** into the target's checked-in corpus directory
   (`src/iiifparser/corpus/` or `src/format_handlers/corpus/<fmt>/`) alongside the fix,
   so it replays forever in the `//src/...` sweeps rather than only living in a
   30-day CI artifact. Skip this for a finding classified outside Vitrinli code, and
   never do it for a timeout.
5. **Open a Linear issue** tracking the finding and its fix.

No step here is automated: the nightly preserves the reproducer and fails
loudly, and a person decides what — if anything — a given finding is worth.
