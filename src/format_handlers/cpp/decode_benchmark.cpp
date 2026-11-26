/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Decode-tier microbenchmarks — `SipiImage::read()` across the input-format
// matrix of the @sipi_bench_fixtures archive (one 7216×5412 photographic
// master, see tools/benchmark/generate_fixtures.sh):
//
//   pyr-none.tif   256×256 tiled pyramid, uncompressed — the fast baseline
//   pyr-zstd.tif   tiled pyramid, ZStd level 9
//   pyr-webp.tif   tiled pyramid, WebP Q90
//   pyr.jp2        Kakadu JPEG2000 (Pillay slide-14 params)
//   baseline.jpg   plain JPEG Q90      — deliberate slow baseline
//   flat.tif       untiled flat TIFF   — deliberate slow baseline
//
// Two access shapes per format: a full-resolution tile (region dim×dim,
// 1:1 size — the deep-zoom viewer hot path; the slow baselines pay a full
// decode for it, the Pillay slide-23 ~100× penalty) and a `!256,256`
// thumbnail (full region, best-fit — exercises pyramid level selection).
//
// `read()` opens the file each call, so OS-level file I/O is part of the
// measured path by design (decision recorded in the plan: no in-memory
// decode seam exists, and adding one would be new public API).
//
// Plus full decodes of two source-tree files: decode_full_subsampled_jp2 (the
// ISO/IEC 15444-4 file3.jp2, chroma subsampled 2x2) and decode_full_png.
//
// HTJ2K is absent from the matrix: Kakadu's HT block coder is
// license-gated (FBC_ENABLED) and the production build cannot decode it.
//
// Built only via `just bench decode` (-c opt, manual-tagged cc_binary),
// which exports SIPI_BENCH_FIXTURES_DIR from the binary's runfiles tree.
// See docs/src/development/benchmarking.md.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "image/SipiImage.h"
#include "iiifparser/SipiRegion.h"
#include "iiifparser/SipiSize.h"

namespace {

// Fixtures live in the @sipi_bench_fixtures external repo, not the source
// tree, so workspace-relative resolution (test_paths.h) cannot find them
// under direct exec. The `just bench` recipe exports the runfiles location.
std::string fixture(const std::string &name)
{
  const char *dir = std::getenv("SIPI_BENCH_FIXTURES_DIR");
  if (dir == nullptr) {
    std::fprintf(stderr, "SIPI_BENCH_FIXTURES_DIR not set — run via `just bench decode`\n");
    std::exit(1);
  }
  return std::string{ dir } + "/big_building/" + name;
}

// Full-resolution dim×dim tile at (1024,1024) — what a deep-zoom viewer
// requests. Tiled formats touch a handful of tiles; the slow baselines
// (plain JPEG, flat TIFF) must decode the whole 39 Mpx image first.
void decode_tile(benchmark::State &state, const char *file)
{
  const std::string path = fixture(file);
  const auto dim = state.range(0);
  const std::string size_spec = std::to_string(dim) + "," + std::to_string(dim);
  for (auto _ : state) {
    Sipi::SipiImage img;
    auto region = std::make_shared<Sipi::SipiRegion>(1024, 1024, dim, dim);
    auto size = std::make_shared<Sipi::SipiSize>(size_spec);
    if (!img.read(path, region, size)) { std::abort(); }
    benchmark::DoNotOptimize(img.getNx());
    benchmark::ClobberMemory();
  }
  state.SetBytesProcessed(state.iterations() * dim * dim * 3);
}

// `!256,256` best-fit thumbnail of the full image — the info.json-adjacent
// "give me a preview" shape. Pyramid formats read a small reduced level;
// the slow baselines decode everything and downscale.
void decode_thumb(benchmark::State &state, const char *file)
{
  const std::string path = fixture(file);
  // !256,256 best-fits the 4:3 master to 256×192, not 256×256 — report
  // throughput from the actual decoded dimensions.
  int64_t thumb_bytes = 0;
  for (auto _ : state) {
    Sipi::SipiImage img;
    auto size = std::make_shared<Sipi::SipiSize>("!256,256");
    if (!img.read(path, nullptr, size)) { std::abort(); }
    benchmark::DoNotOptimize(img.getNx());
    benchmark::ClobberMemory();
    thumb_bytes = static_cast<int64_t>(img.getNx() * img.getNy() * img.getNc());
  }
  state.SetBytesProcessed(state.iterations() * thumb_bytes);
}

#define SIPI_DECODE_BENCH(name, file)                                                             \
  BENCHMARK_CAPTURE(decode_tile, name, file)->Arg(256)->Arg(1024)->Unit(benchmark::kMillisecond); \
  BENCHMARK_CAPTURE(decode_thumb, name, file)->Unit(benchmark::kMillisecond)

SIPI_DECODE_BENCH(pyr_none, "pyr-none.tif");
SIPI_DECODE_BENCH(pyr_zstd, "pyr-zstd.tif");
SIPI_DECODE_BENCH(pyr_webp, "pyr-webp.tif");
SIPI_DECODE_BENCH(jp2, "pyr.jp2");
SIPI_DECODE_BENCH(jpeg_baseline, "baseline.jpg");
SIPI_DECODE_BENCH(flat_tiff, "flat.tif");

#undef SIPI_DECODE_BENCH

// Full decode of a source-tree file, resolved from SIPI_WORKSPACE_ROOT, which
// `just bench` exports.
void decode_full(benchmark::State &state, const std::string &workspace_relative_path)
{
  const char *root = std::getenv("SIPI_WORKSPACE_ROOT");
  if (root == nullptr) {
    std::fprintf(stderr, "SIPI_WORKSPACE_ROOT not set — run via `just bench decode`\n");
    std::exit(1);
  }
  const std::string path = std::string{ root } + "/" + workspace_relative_path;
  int64_t bytes = 0;
  for (auto _ : state) {
    Sipi::SipiImage img;
    if (!img.read(path)) { std::abort(); }
    benchmark::DoNotOptimize(img.getNx());
    benchmark::ClobberMemory();
    bytes = static_cast<int64_t>(img.getNx() * img.getNy() * img.getNc());
  }
  state.SetBytesProcessed(state.iterations() * bytes);
}

// The ISO/IEC 15444-4 conformance file file3.jp2 (480x640, sYCC, chroma
// subsampled 2x2): the planar pull + upsampling path in SipiIOJ2k::read.
void decode_full_subsampled_jp2(benchmark::State &state)
{
  decode_full(state, "test/_test_data/images/iso-15444-4/testfiles_jp2/file3.jp2");
}
BENCHMARK(decode_full_subsampled_jp2)->Unit(benchmark::kMillisecond);

// An 8-bit RGB PNG with an embedded ICC profile (256x489): the PNG read path,
// which @sipi_bench_fixtures has no file for.
void decode_full_png(benchmark::State &state) { decode_full(state, "test/_test_data/images/unit/sample_with_icc.png"); }
BENCHMARK(decode_full_png)->Unit(benchmark::kMillisecond);

}// namespace
