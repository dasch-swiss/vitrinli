/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Shared libFuzzer harness for the `SipiIO` codec handlers (TIFF/JPEG/PNG/JP2).
//
// `SipiIO` has no in-memory decode overload anywhere — every handler's
// `read_shape`/`read` take a filesystem path — so the harness's only option is
// to materialize each fuzzer-supplied buffer as a temp file and drive the
// handler over that path.
//
// `run_decode` drives two decode passes per input:
//   1. The whole `data`/`size` buffer is written to a temp file and decoded
//      with the region/size-less 2-arg `read`. This is unchanged from before
//      and keeps every existing corpus seed decodable exactly as it was.
//   2. If the buffer is longer than a small fixed header, the header is
//      parsed as an explicit region/size (consumed from the front, not part
//      of the decoded file) and the remainder is decoded with the
//      region/size-aware `read` overload, reaching the ROI-dependent decode
//      branches the first pass can never exercise. Because the header is
//      stripped before the file is written, pass 1 still sees the exact
//      bytes a seed corpus was captured with.

#pragma once

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "iiifparser/SipiRegion.h"
#include "iiifparser/SipiSize.h"
#include "image/SipiImage.h"

namespace sipi::fuzz {

// One fixed path per process: libFuzzer runs each process single-threaded, so
// a per-iteration temp file with a stable name (truncated and rewritten every
// call) avoids filesystem churn. The pid keeps parallel `-jobs=N` workers from
// colliding on the same path. `TEST_TMPDIR` is honoured when set (Bazel's
// `cc_fuzz_test` replay engine sets it), falling back to the platform temp dir
// otherwise (mirrors `sipi::test::tmp_dir()` in `test/test_paths.h`, which the
// fuzz package intentionally does not depend on).
// `ForRoi` exists solely to give each call site (the whole-buffer path vs. the
// region/size-aware remainder path below) its own function-local static: a
// single non-template function would cache only the first suffix it was ever
// called with for the lifetime of the process, silently reusing that path for
// every later call regardless of the suffix argument.
template<bool ForRoi> inline const std::string &fuzz_temp_path_for(const char *suffix)
{
  static const std::string path = [suffix] {
    const char *base = std::getenv("TEST_TMPDIR");
    std::filesystem::path dir = (base != nullptr) ? std::filesystem::path{ base } : std::filesystem::temp_directory_path();
    return (dir / ("sipi_codec_fuzz_" + std::to_string(getpid()) + suffix)).string();
  }();
  return path;
}

inline const std::string &fuzz_temp_path(const char *suffix) { return fuzz_temp_path_for<false>(suffix); }

// Second per-process temp file, used for the region/size-aware remainder pass
// in `run_decode`.
inline const std::string &fuzz_roi_temp_path(const char *suffix) { return fuzz_temp_path_for<true>(suffix); }

// Third per-process temp file, holding the round-trip pass's decode input
// (distinct from `run_decode`'s two paths above so a target that links both
// harnesses in the same process never collides). A separate template from
// `fuzz_temp_path_for` above rather than a third `bool` value: `bool` only
// has the two states that function already uses.
template<int Slot> inline const std::string &fuzz_extra_temp_path_for(const char *suffix)
{
  static const std::string path = [suffix] {
    const char *base = std::getenv("TEST_TMPDIR");
    std::filesystem::path dir = (base != nullptr) ? std::filesystem::path{ base } : std::filesystem::temp_directory_path();
    return (dir / ("sipi_codec_fuzz_" + std::to_string(getpid()) + "_extra" + std::to_string(Slot) + suffix)).string();
  }();
  return path;
}

inline const std::string &fuzz_roundtrip_temp_path(const char *suffix) { return fuzz_extra_temp_path_for<0>(suffix); }

// One temp output path per encoder `ftype` string. Unlike the paths above,
// this is computed fresh on every call rather than cached in a function-local
// static: the encode targets loop over several `ftype` strings per fuzz
// iteration, so a single cached path (stable-per-process, keyed only by the
// first argument it ever saw) would silently make every encoder after the
// first overwrite the same file instead of getting its own.
inline std::string fuzz_roundtrip_out_path(const char *ftype)
{
  const char *base = std::getenv("TEST_TMPDIR");
  std::filesystem::path dir = (base != nullptr) ? std::filesystem::path{ base } : std::filesystem::temp_directory_path();
  return (dir / ("sipi_codec_fuzz_roundtrip_out_" + std::to_string(getpid()) + "_" + std::string(ftype))).string();
}

// Fixed header consumed from the front of the fuzzer-supplied buffer to drive
// the region/size-aware decode pass: four little-endian `uint16_t` region
// coordinates (x, y, w, h), two little-endian `uint16_t` size dimensions (w,
// h), a little-endian `uint16_t` rotation, a `uint8_t` reduce factor, and one
// reserved byte. Rotation/reduce are parsed for a future pass — the base
// `read` overload driven here takes neither parameter.
inline constexpr std::size_t kHeaderBytes = 16;

// Reads a little-endian `uint16_t` from `data[offset]`/`data[offset + 1]`.
// Explicit byte shifts, not a `reinterpret_cast`, because `data` has no
// guaranteed alignment for a 2-byte read.
inline std::uint16_t read_u16le(const uint8_t *data, std::size_t offset)
{
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset])
                                     | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

// Fuzzing budget for the second (full `read`) decode step, not a codec limit.
// `validate_decode_dims` (`SipiIO.h`) caps each dimension and the channel
// count individually, but never their product, so a header can claim a
// multi-gigabyte decode buffer and still pass the guard. In production such a
// claim is bounded by the full-lane decode memory budget acquired at the FFI
// seam (`MemoryBudgetGuard` in `src/ffi/cpp/serve_image.cpp`); this harness
// has no such budget, so a header-claimed allocation this large would just
// burn the fuzz leg on an OOM that says nothing about codec correctness. The
// shape probe (`read_shape`) still runs unconditionally on every input — it
// is where the container/header parsing coverage lives — only the second,
// full `read` step is skipped once the probe's geometry implies a buffer
// above this budget.
//
// Sized for the slowest configuration that runs this harness, not for RSS.
// The ASan-paired fuzz pass builds at `-O0` (`build:asan` forces
// `--compilation_mode=dbg`), where libc++ destroys a byte vector with one
// non-inlined call per element and the TIFF handler allocates a full-size
// pixel buffer before the first scanline — so an admitted decode costs up to
// roughly 60 ms per MB of geometry even when libtiff rejects that scanline. libFuzzer's
// per-input timeout is 25 s (`fuzz.yml`); 64 MiB keeps the worst case near
// 4 s. Larger geometries add no coverage: more scanlines run the same code.
inline constexpr std::uint64_t kMaxHarnessDecodeBytes = 64ULL * 1024 * 1024;

// Encode budget for `run_roundtrip`'s four re-encodes (rationale at the use
// site): a decoded image above this many bytes is not re-encoded.
inline constexpr std::uint64_t kMaxRoundtripEncodeBytes = 16ULL * 1024 * 1024;

// Estimates the decode buffer size implied by a shape probe's geometry.
// `nc`/`bps` are 0 when only `DIMS` (not `ALL`) is known, so a zero channel
// count is treated as 1 and a zero bit depth as 8. Any negative field (the
// members are `int`) is treated as "unknown", returning 0 so the caller lets
// the decode run rather than folding it into a huge unsigned value.
inline std::uint64_t estimated_decode_bytes(const Sipi::SipiImgInfo &info)
{
  if (info.width < 0 || info.height < 0 || info.nc < 0 || info.bps < 0) { return 0; }
  const std::uint64_t width = static_cast<std::uint64_t>(info.width);
  const std::uint64_t height = static_cast<std::uint64_t>(info.height);
  const std::uint64_t nc = static_cast<std::uint64_t>(info.nc == 0 ? 1 : info.nc);
  const std::uint64_t bps = static_cast<std::uint64_t>(info.bps == 0 ? 8 : info.bps);
  return width * height * nc * ((bps + 7) / 8);
}

// Writes `data`/`size` to the per-process temp file, then drives `Handler`'s
// `read_shape` and `read` entry points over it. Each call is wrapped in its
// own try/catch so a clean rejection from `read_shape` still lets `read` run.
// Both entry points return a `Result`, which the harness intentionally checks
// only for presence: a codec rejecting malformed input reports that through
// the `Result` holding no value, not through an exception, so the harness's
// finding surface is what escapes the catch blocks entirely (SIGSEGV/SIGABRT/
// sanitizer reports). The try/catch blocks stay because a decoder can still
// throw en route to producing that `Result`: Kakadu's `kdu_exception` (an
// `int`-like type, not a `std::exception`, which is why the bare `catch (...)`
// matters — SipiIOJ2k.cpp's decode path calls several Kakadu accessors, e.g.
// `codestream.access_siz()`/`jpx_layer.access_colour()`, that are not
// individually try/caught) and `std::bad_alloc`, including the allocation-guard
// throws that stay exception-based by design (`checked_buf_size_or_throw`,
// `memTiffOpen`'s raw `malloc` failures). The `Iptc`/`Exif`/`Icc` metadata
// parsers are `Result`-returning factories and `Xmp`'s constructor is
// infallible (it stores the given bytes verbatim, never parsing them), so
// none of the four metadata types can still throw here.
//
// When `size` exceeds `kHeaderBytes`, a second pass follows the same
// read_shape-then-read shape over the buffer's remainder (after `data` is
// interpreted as a fixed header), this time through the region/size-aware
// `read` overload — the only way to reach the ROI-dependent decode branches
// (e.g. planar-separate region handling) from this harness. The header is
// consumed from the front rather than appended, so the first pass keeps
// decoding every existing corpus seed byte-for-byte as before.
template<typename Handler> int run_decode(const uint8_t *data, size_t size, const char *suffix)
{
  const std::string &path = fuzz_temp_path(suffix);
  {
    std::ofstream out{ path, std::ios::binary | std::ios::trunc };
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
  }

  Handler handler;
  bool skip_full_read = false;

  try {
    if (const auto r = handler.read_shape(path); r) {
      skip_full_read = estimated_decode_bytes(*r) > kMaxHarnessDecodeBytes;
    }
  } catch (const std::exception &) {
  } catch (...) {
  }

  if (!skip_full_read) {
    try {
      Sipi::SipiImage img;
      // Qualified to reach the base class's 2-arg convenience overload
      // (`SipiIO.h:172-175`): the concrete handlers only override the 6-arg
      // `read`, which hides all base-class `read` overloads from unqualified
      // lookup on the derived type. The base overload still dispatches
      // virtually into the derived handler's 6-arg override.
      if (const auto r = handler.Sipi::SipiIO::read(&img, path); !r) { /* a rejection is a valid fuzz outcome */ }
    } catch (const std::exception &) {
    } catch (...) {
    }
  }

  // Region/size-aware pass: the leading `kHeaderBytes` of `data` are consumed
  // as an explicit region/size rather than decoded, so this only runs once a
  // buffer is long enough to hold both the header and some remainder to
  // decode. This is independent of `skip_full_read` above — it has its own
  // shape probe and its own budget check against the remainder's geometry.
  if (size > kHeaderBytes) {
    const std::uint16_t region_x = read_u16le(data, 0);
    const std::uint16_t region_y = read_u16le(data, 2);
    const std::uint16_t region_w = read_u16le(data, 4);
    const std::uint16_t region_h = read_u16le(data, 6);
    const std::uint16_t size_w = read_u16le(data, 8);
    const std::uint16_t size_h = read_u16le(data, 10);
    // Reserved for a future rotation/reduce-aware pass; the `read` overload
    // driven below takes neither parameter.
    [[maybe_unused]] const std::uint16_t rotation = read_u16le(data, 12);
    [[maybe_unused]] const std::uint8_t reduce = data[14];
    // byte 15 is reserved/ignored.

    const std::string &roi_path = fuzz_roi_temp_path(suffix);
    {
      std::ofstream out{ roi_path, std::ios::binary | std::ios::trunc };
      out.write(reinterpret_cast<const char *>(data + kHeaderBytes),
        static_cast<std::streamsize>(size - kHeaderBytes));
    }

    bool skip_roi_read = false;
    try {
      if (const auto r = handler.read_shape(roi_path); r) {
        skip_roi_read = estimated_decode_bytes(*r) > kMaxHarnessDecodeBytes;
      }
    } catch (const std::exception &) {
    } catch (...) {
    }

    if (!skip_roi_read) {
      try {
        Sipi::SipiImage roi_img;
        const auto region = std::make_shared<Sipi::SipiRegion>(region_x, region_y, region_w, region_h);
        const auto roi_size =
          std::make_shared<Sipi::SipiSize>(Sipi::SipiSize::PIXELS_XY, false, 0.0F, 0, size_w, size_h);
        // Qualified for the same reason as the whole-buffer pass above.
        if (const auto r = handler.Sipi::SipiIO::read(&roi_img, roi_path, region, roi_size); !r) {
          /* a rejection is a valid fuzz outcome */
        }
      } catch (const std::exception &) {
      } catch (...) {
      }
    }
  }

  return 0;
}

// Decode-then-encode round trip: `run_decode` above only ever reaches
// `SipiIO::read`, so every encoder (`SipiIO::write`) is unreachable from the
// fuzzer — a blind spot that let a JP2-encode bug (S2-02) and a
// JPEG-marker-write bug (S2-14) ship undetected. This drives the same
// `read_shape`-then-`read` decode as `run_decode`'s whole-buffer pass, then,
// if the decode produced an image, re-encodes it through every format
// `SipiImage::write` supports ("tif", "jpx", "png", "jpg" — see
// `SipiImage.cpp`'s write dispatch), including the handler's own format when
// that differs from `Handler`'s native decode format. A write failure or
// thrown exception is a valid fuzz outcome, exactly like a decode rejection
// above; only a crash/sanitizer report is a finding.
template<typename Handler> int run_roundtrip(const uint8_t *data, size_t size, const char *in_suffix)
{
  const std::string &path = fuzz_roundtrip_temp_path(in_suffix);
  {
    std::ofstream out{ path, std::ios::binary | std::ios::trunc };
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
  }

  Handler handler;
  bool skip_read = false;

  try {
    if (const auto r = handler.read_shape(path); r) {
      skip_read = estimated_decode_bytes(*r) > kMaxHarnessDecodeBytes;
    }
  } catch (const std::exception &) {
  } catch (...) {
  }

  if (skip_read) { return 0; }

  std::unique_ptr<Sipi::SipiImage> img;
  try {
    img = std::make_unique<Sipi::SipiImage>();
    // Qualified for the same reason as `run_decode`'s whole-buffer pass.
    if (const auto r = handler.Sipi::SipiIO::read(img.get(), path); !r) { img.reset(); }
  } catch (const std::exception &) {
    img.reset();
  } catch (...) {
    img.reset();
  }

  if (!img) { return 0; }

  // A malformed input can decode "successfully" into a degenerate image — zero
  // width, height, or channels (e.g. a TIFF whose directory yields no strips,
  // leaving SamplesPerPixel/scanline size at 0). Round-tripping that is not a
  // meaningful encode test and drives the encoders through zero-size buffers
  // and divisors that trip ASan; a real decoded image always has non-zero
  // extent, so skip the encode rather than exercise a nonsense one.
  if (img->getNx() == 0 || img->getNy() == 0 || img->getNc() == 0) { return 0; }

  // Encode budget, not a codec limit. A few hundred bytes of JPEG can declare
  // a 65056x255 frame, which decodes in milliseconds into ~50 MB of pixels
  // that the four encodes below then chew through — past libFuzzer's 25s
  // per-input timeout under the ASan build. The bug class the round-trip legs
  // hunt is encoder header/marker/metadata handling, which does not need bulk
  // pixel volume, so skip the encodes for a decoded image above this size.
  const std::uint64_t decoded_bytes = static_cast<std::uint64_t>(img->getNx()) * img->getNy() * img->getNc()
                                      * ((static_cast<std::uint64_t>(img->getBps()) + 7) / 8);
  if (decoded_bytes > kMaxRoundtripEncodeBytes) { return 0; }

  static constexpr const char *kEncodeFtypes[] = { "tif", "jpx", "png", "jpg" };
  for (const char *ftype : kEncodeFtypes) {
    const std::string out_path = fuzz_roundtrip_out_path(ftype);
    try {
      if (const auto r = img->write(ftype, out_path); !r) { /* a write rejection is a valid fuzz outcome */ }
    } catch (const std::exception &) {
    } catch (...) {
    }
  }

  return 0;
}

}// namespace sipi::fuzz
