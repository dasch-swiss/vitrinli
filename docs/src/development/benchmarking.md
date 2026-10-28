# Benchmarking

Vitrinli uses [Google Benchmark](https://github.com/google/benchmark) for in-process
microbenchmarks of the production codec and the IIIF parser front door. The
suite exists to enforce one convention:

> **No benchmark, no hot-path change.** Before changing any image
> decode/encode hot path (`src/format_handlers/cpp/*`, `SipiImage` read/write,
> `iiifparser`), a microbenchmark must exist — add one if it doesn't — and
> the change is justified with a before/after comparison in the PR.

The suite was motivated by Ruven Pillay's *Evaluating IIIF Server
Performance* (IIIF Annual Conference 2026,
`docs/benchmarks/01_Ruven_Pillay_Evaluating_IIIF_Server_Performance.pdf`),
which benchmarked Vitrinli as the slowest server tested for TIFF and JPEG. The
microbenchmarks measure *intra-Vitrinli* per-stage cost — what can be measured
rigorously and locally; the PDF's cross-server, end-to-end HTTP comparison
is deliberately out of scope (apples-to-oranges across machines and OSes).

## The tiers

One manual-tagged `cc_binary` per pipeline stage, named `<tier>_benchmark` and
co-located with the module it measures:

| Tier | Source | What it times |
|------|--------|---------------|
| `parse` | `src/iiifparser/cpp/value_objects/parse_benchmark.cpp` | The five IIIF URL component parsers + the full per-request parse. Pure CPU, no fixtures. |
| `decode` | `src/format_handlers/cpp/decode_benchmark.cpp` | `SipiImage::read()` across the input-format matrix (tiled-pyramid TIFF none/zstd/webp, JP2, plain-JPEG and flat-TIFF slow baselines) in two access shapes: full-resolution tile and `!256,256` thumbnail; plus a full decode of the ISO/IEC 15444-4 `file3.jp2` (chroma subsampled 2x2), read from the source tree. |
| `process` | `src/image_processing/cpp/process_benchmark.cpp` | The operators between decode and encode: `scaleFast`/`scaleMedium`/`scale`, `rotate` (90° fast path + 45° general), `crop`, `to8bps`, `convertToIcc`, `removeChannel`. |
| `encode` | `src/format_handlers/cpp/encode_benchmark.cpp` | `SipiImage::write()` for the four formats Vitrinli emits: JPEG (Q75/Q90), PNG, TIFF, JPEG2000. |

Benchmarks are co-located with the module they measure (ADR-0003 direction:
`*_benchmark.cpp` beside the source, the Abseil/Bloomberg-BDE/Chromium
convention). The `parse`/`decode`/`encode`/`process` targets have all been
promoted into their modules (`//src/iiifparser/cpp/value_objects`,
`//src/format_handlers`, `//src/image_processing`). A `**/*_benchmark.cpp` glob
exclude on `//src:sipi_lib` keeps the sources out of the production library
and the coverage build, and `tags = ["manual"]` keeps the targets out of
`bazel test //...`.

HTJ2K is absent from the decode matrix: Kakadu's HT block coder is
license-gated (`FBC_ENABLED`, evaluation-only) and the production Vitrinli
build can neither encode nor decode HT codestreams.

## Fixtures

- The `parse` tier needs none.
- The `process` tier reuses small checked-in repo fixtures
  (`test/_test_data/images/`) with the specific shapes its operators need
  (alpha channel, 16 bps, CMYK, known dimensions).
- The `decode`/`encode` tiers consume `@sipi_bench_fixtures` — a 321 MB
  variant matrix generated from one 7216×5412 photographic master by
  `tools/benchmark/generate_fixtures.sh` (the checked-in provenance: pinned
  source, pinned tool versions, exact commands). It is hosted as a release
  asset on `dasch-swiss/dsp-ci-assets` and fetched lazily — the first
  `just bench decode` downloads it; production and CI test builds never do.

## Running

```bash
just bench <tier>                # tier ∈ parse | decode | process | encode
just bench parse --benchmark_filter=ParseSize --benchmark_min_time=2s
```

`just bench` builds the binary with `-c opt` (matching production codegen —
never fastbuild, never sanitized or instrumented) and execs it directly,
exporting `SOURCE_DATE_EPOCH=946684800` + `SIPI_WORKSPACE_ROOT=.`
(ADR-0002 ICC determinism + fixture resolution), `SIPI_BENCH_FIXTURES_DIR`
(the `@sipi_bench_fixtures` runfiles location) and a throwaway
`TEST_TMPDIR` for encode outputs. All Google Benchmark
[flags](https://github.com/google/benchmark/blob/main/docs/user_guide.md)
pass through.

## The before/after workflow

```bash
just bench decode --benchmark_repetitions=20 \
    --benchmark_out=before.json --benchmark_out_format=json
# ... make the hot-path change, rebuild happens automatically ...
just bench decode --benchmark_repetitions=20 \
    --benchmark_out=after.json --benchmark_out_format=json
just bench-compare before.json after.json
```

`bench-compare` runs the vendored google_benchmark `compare.py`
(`//tools/benchmark:compare`, hermetic Python + numpy/scipy via the
`sipi_bench_pip` hub — nothing needed on the host). It prints per-benchmark
deltas with a Mann-Whitney U-test and an `OVERALL_GEOMEAN`:

- green/red numbers are the relative change (`+0.05` = 5% slower),
- the U-test p-value (with `--benchmark_repetitions`) tells you whether the
  two distributions actually differ.

## The regression decision rule

On Apple Silicon you cannot disable turbo or thermal throttling; rely on
statistics instead:

- Run both sides with `--benchmark_repetitions=20` on a quiesced machine
  and demand a baseline CV ≤ ~2% (`_cv` rows in the repetition output). A
  CV of 5–10% means the machine is too noisy — re-run cooler or raise
  `--benchmark_min_time`.
- Trust a delta only if it is **green (p < 0.05) AND the median shift
  exceeds the baseline CV**. Treat sub-3% deltas as noise; trust ≥5% green
  shifts.
- **Same machine, same `-c opt` binary, for both before and after.** Never
  compare a Mac "before" against a Linux "after".

This is never a CI gate — cross-machine comparisons are meaningless and
shared runners are too noisy. The numbers live in PR descriptions, produced
on the author's machine.

## Adding a benchmark

1. Put `<name>_benchmark.cpp` beside the code it measures (it moves with
   the module when ADR-0003 promotes it).
2. Declare a `cc_binary` in `src/BUILD.bazel` next to the existing ones:
   `tags = ["manual"]`, `testonly = True`, deps on `//src:sipi_lib` +
   `@google_benchmark//:benchmark_main` (+ `//test:test_paths` and
   `env`/`data` if it reads fixtures or emits ICC-stamped output).
3. Keep file I/O and source decoding out of the timed loop (decode once,
   deep-copy per iteration outside the timing if the operator mutates), and
   defeat the optimizer with `benchmark::DoNotOptimize` /
   `benchmark::ClobberMemory`.
4. `just bench <name>` — there is nothing to register anywhere else.

## Concurrent-load measurement

`just bench` measures a single decode/encode in isolation. It cannot see how
a change behaves when many requests run at once — the regime that matters for
decisions about worker-thread counts, pool sizing, or anything whose cost is
paid in contention rather than per-call work. A single-request speedup can
coincide with no aggregate gain (or a regression) once the request pool is
saturated.

`just loadtest-decode "10,20,40"` covers that gap. It builds the production
Rust shell (`//src/cli/rust:sipi`, `-c opt`), serves `load_test.jpx` via
`config/sipi.loadtest-config.lua`, and drives it with
`tools/loadtest/loadgen.py`: N concurrent clients each requesting a distinct
native-resolution tile (distinct region → cache miss → real decode), reporting
throughput and p50/p90/p99 per concurrency level after a warm-up window. The
pool is sized from the host core count (`nthreads = 0`), so the listed
concurrencies straddle its saturation point.

Same rules as the microbench: never CI-gated, run on a quiesced machine, and
compare a "before" branch against an "after" **on the same host** (build and
run each branch in turn — do not run a build during a measurement, it steals
cores). Interpretation caveats: the effect of thread-count changes scales with
core count, so a result on a 10-core laptop does not settle behavior on a
high-core production host; and the Rust FFI pool already bounds concurrent
decodes to its permit count, which caps the worst-case thread total.
