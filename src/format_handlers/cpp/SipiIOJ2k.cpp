/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Byte-exact output invariant (approval gate).
 *
 * The JPEG 2000 encode path here is pinned byte-for-byte by the approval tests
 * (//test/approval:approvaltests), which CI runs on every supported platform —
 * so the emitted bytes must be identical across darwin-aarch64, linux-x86_64,
 * and linux-aarch64. Keep any pixel math that feeds the encoder deterministic
 * and architecture-independent (prefer fixed-point to float, which can differ
 * bit-for-bit across arches). Embedded ICC profiles are serialized through
 * Sipi::Icc::iccBytes(), whose SOURCE_DATE_EPOCH normalization keeps the
 * wall-clock-stamped creation date out of the goldens — see
 * docs/adr/0002-icc-profile-determinism-test-only.md.
 *
 * Hot path: any change to this decode/encode code needs a Google Benchmark
 * microbench and a before/after `just bench {decode,encode}` run on the same
 * -c opt binary (ADR-0003; docs/src/development/benchmarking.md).
 */

#include <algorithm>
#include <assert.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <cstring>
#include <fcntl.h>

// Kakadu core includes
#include "kdu_compressed.h"
#include "kdu_elementary.h"
#include "kdu_messaging.h"
#include "kdu_params.h"
#include "kdu_sample_processing.h"

// Application level includes

#include "jp2.h"
#include "jpx.h"
#include "kdu_file_io.h"
#include "kdu_stripe_compressor.h"
#include "kdu_stripe_decompressor.h"

#include "util/Global.h"
#include "util/checked_arith.h"
#include "observability/metrics.h"

#include "error/SipiError.h"
#include "error/SipiValueError.h"
#include "image/SipiImageError.h"
#include "image_processing/processing.h"
#include "SipiIOJ2k.h"
#include "logging/logger.h"
#include "observability/profiling.h"

using namespace kdu_core;
using namespace kdu_supp;

namespace {
constexpr std::int64_t kMaxMetadataBytes = 64 * 1024 * 1024;// 64 MiB cap on any single embedded metadata box

// Kakadu's thread group holds at most KDU_MAX_THREADS threads; add_thread()
// indexes past its per-thread arrays for any count beyond that.
[[nodiscard]] int kakadu_thread_count()
{
  const int processors = kdu_get_num_processors();
  if (processors < 2) return 0;
  return std::min(processors, KDU_MAX_THREADS);
}

// Pixel channel of each of the nc codestream components: colour k of the
// layer's colour space (Y, Cb, Cr for YCbCr) goes to channel k, taken from the
// component the JP2 channel mapping (cdef box) assigns to it; components that
// carry no colour (alpha) follow in codestream order. Empty when the mapping
// is the identity or not a plain component permutation (a palette, another
// codestream): component c then stays in channel c.
[[nodiscard]] std::vector<int> jp2_channel_of(const jp2_channels &channels, int numcol, int nc)
{
  std::vector<int> channel_of(nc, -1);
  for (int k = 0; k < numcol; ++k) {
    int comp = 0;
    int lut = 0;
    int stream = 0;
    int format = 0;
    if (!channels.get_colour_mapping(k, comp, lut, stream, format) || lut >= 0 || stream != 0 || comp < 0
        || comp >= nc || channel_of[comp] >= 0) {
      return {};
    }
    channel_of[comp] = k;
  }
  int next = numcol;
  bool identity = true;
  for (int c = 0; c < nc; ++c) {
    if (channel_of[c] < 0) channel_of[c] = next++;
    identity = identity && channel_of[c] == c;
  }
  if (identity) return {};
  return channel_of;
}

// Decodes a JP2 whose components are subsampled relative to component 0
// (factors[c] is 1 or 2 per axis): pulls each component into its own buffer at
// its own size, then replicates each sample onto component 0's grid and
// interleaves it into `dst` (nc samples per pixel) at channel channel_of[c]
// (component c when channel_of is empty). JPEG 2000 sites component
// samples co-sited: sample k of a component with factor f sits at component-0
// position k*f and covers positions k*f .. k*f+f-1. Positions come from
// get_dims after the input restrictions, so region origins and reduce levels
// stay aligned.
template<typename T>
void pull_upsampled(kdu_supp::kdu_stripe_decompressor &decompressor,
  kdu_core::kdu_codestream &codestream,
  const std::vector<kdu_core::kdu_coords> &factors,
  const std::vector<int> &channel_of,
  T *dst)
{
  const int nc = static_cast<int>(factors.size());
  std::vector<kdu_core::kdu_dims> comp_dims(nc);
  std::vector<std::vector<T>> planes(nc);
  std::vector<T *> bufs(nc);
  std::vector<int> heights(nc);
  for (int c = 0; c < nc; ++c) {
    codestream.get_dims(c, comp_dims[c], true);
    planes[c].resize(static_cast<std::size_t>(comp_dims[c].size.x) * static_cast<std::size_t>(comp_dims[c].size.y));
    bufs[c] = planes[c].data();
    heights[c] = comp_dims[c].size.y;
  }
  if constexpr (std::is_same_v<T, kdu_core::kdu_byte>) {
    decompressor.pull_stripe(bufs.data(), heights.data());
  } else {
    const auto is_signed = std::make_unique<bool[]>(nc);// all false: unsigned samples
    decompressor.pull_stripe(bufs.data(), heights.data(), nullptr, nullptr, nullptr, is_signed.get());
  }

  const kdu_core::kdu_dims &ref = comp_dims[0];
  const std::size_t nx = ref.size.x;
  const std::size_t ny = ref.size.y;
  // Component sample index for each reference coordinate, clamped to the
  // component's extent.
  const auto source_index = [](int ref_pos, std::size_t n, int f, int comp_pos, int comp_len) {
    std::vector<std::size_t> idx(n);
    for (std::size_t i = 0; i < n; ++i) {
      idx[i] = std::clamp((ref_pos + static_cast<int>(i)) / f - comp_pos, 0, comp_len - 1);
    }
    return idx;
  };
  for (int c = 0; c < nc; ++c) {
    const kdu_core::kdu_dims &d = comp_dims[c];
    const auto xs = source_index(ref.pos.x, nx, factors[c].x, d.pos.x, d.size.x);
    const auto ys = source_index(ref.pos.y, ny, factors[c].y, d.pos.y, d.size.y);
    for (std::size_t j = 0; j < ny; ++j) {
      const T *row = planes[c].data() + ys[j] * static_cast<std::size_t>(d.size.x);
      T *out = dst + j * nx * nc + (channel_of.empty() ? c : channel_of[c]);
      for (std::size_t i = 0; i < nx; ++i) { out[i * nc] = row[xs[i]]; }
    }
  }
}
}// namespace

namespace Sipi {

Result<void>
  validate_j2k_palette_mapping(std::size_t bps, std::size_t nc, int numcol, int nentries, const std::string &filepath)
{
  if (bps != 8) {
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Cannot read JPEG2000 file \"" + filepath + "\": palette color images with " + std::to_string(bps)
        + "-bit indices are not supported" });
  }
  if (nc != 1) {
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Cannot read JPEG2000 file \"" + filepath
        + "\": palette color images with more than one index component are not supported" });
  }
  if (numcol != 3) {
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Cannot read JPEG2000 file \"" + filepath
        + "\": palette color images must map to exactly 3 output colors, got " + std::to_string(numcol) });
  }
  // Every possible bps-bit index value must resolve to a valid palette
  // entry; checking this once here means the expansion loop needs no
  // per-pixel bounds check.
  if (nentries < (1 << bps)) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "Cannot read JPEG2000 file \"" + filepath + "\": palette has only " + std::to_string(nentries)
        + " entries, too few for " + std::to_string(bps) + "-bit indices" });
  }
  return {};
}

//=========================================================================
// Here we are implementing a subclass of kdu_core::kdu_compressed_target
// in order to write directly to the HTTP server connection
//
class J2kHttpStream : public kdu_core::kdu_compressed_target
{
private:
  SinkStream *sink;

public:
  //! Set when write()/close() fails because the body sink (the socket) failed.
  //! Read from the top-level SipiIOJ2k::write error handler to distinguish
  //! client aborts from genuine Kakadu failures.
  bool client_aborted{ false };

  J2kHttpStream(SinkStream *sink_p);

  ~J2kHttpStream() override;

  inline int get_capabilities() override { return KDU_TARGET_CAP_SEQUENTIAL; };

  inline bool start_rewrite(kdu_long backtrack) override { return false; };

  inline bool end_rewrite() override { return false; };

  bool write(const kdu_byte *buf, int num_bytes) override;

  bool close() override;

  inline void set_target_size(kdu_long num_bytes) override {};// we just ignore it
};
//-------------------------------------------------------------------------

//-------------------------------------------------------------------------
// Constructor which takes the HTTP server connection as parameter
//........................................................................
J2kHttpStream::J2kHttpStream(SinkStream *sink_p) : kdu_core::kdu_compressed_target() { sink = sink_p; };
//-------------------------------------------------------------------------


//-------------------------------------------------------------------------
// Distructor which cleans up !!!!!!!!!! We still have to determine what has to be cleaned up!!!!!!!!!!
//........................................................................
J2kHttpStream::~J2kHttpStream(){
  // cleanup everything...
};
//-------------------------------------------------------------------------

//-------------------------------------------------------------------------
// Write the data to the HTTP server connection
//........................................................................
bool J2kHttpStream::write(const kdu_byte *buf, int num_bytes)
{
  // A non-zero sink return is a body-write failure (the socket is gone) — the
  // equivalent of the old OUTPUT_WRITE_FAIL abort signal. No C++ exception
  // crosses Kakadu's frames; the sink callback returns a status code.
  if (sink->write(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(num_bytes)) != 0) {
    client_aborted = true;
    return false;
  }
  return true;
};
//-------------------------------------------------------------------------

bool J2kHttpStream::close()
{
  // Nothing to flush: each write() already pushed its chunk to the sink, and
  // the HTTP framing/terminator is flushed by the request handler after write().
  return true;
}

static kdu_core::kdu_byte
  xmp_uuid[] = { 0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8, 0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC };
static kdu_core::kdu_byte
  iptc_uuid[] = { 0x33, 0xc7, 0xa4, 0xd2, 0xb8, 0x1d, 0x47, 0x23, 0xa0, 0xba, 0xf1, 0xa3, 0xe0, 0x97, 0xad, 0x38 };
static kdu_core::kdu_byte
  exif_uuid[] = { 'J', 'p', 'g', 'T', 'i', 'f', 'f', 'E', 'x', 'i', 'f', '-', '>', 'J', 'P', '2' };

// SIPI Essentials UUID (ADR-0005 / DEV-6410). Generated via `uuidgen` on
// 2026-05-15: 7B28A646-B9C3-4FB2-900B-B6855DF23882. Identifies the JP2 UUID
// box that carries the protobuf-serialized Essentials packet for Service
// Files. Position: slot 4 after Signature → FTYP → `jp2h`, before `jp2c`.
// Layout inside the UUID box: 16-byte UUID + protobuf payload (no separate
// length prefix — the box's own length field encodes it).
//
// Documented in `docs/adr/0005-essentials-packet-versioned-binary-serialization.md`
// and `UBIQUITOUS_LANGUAGE.md`. jpylyzer reports unknown UUIDs as
// informational, so this carrier is preservation-validator-safe.
static kdu_core::kdu_byte sipi_essentials_uuid[] = { 0x7B, 0x28, 0xA6, 0x46, 0xB9, 0xC3, 0x4F, 0xB2, 0x90, 0x0B, 0xB6,
  0x85, 0x5D, 0xF2, 0x38, 0x82 };
// static kdu_core::kdu_byte geojp2_uuid[] = {0xB1, 0x4B, 0xF8, 0xBD, 0x08, 0x3D, 0x4B, 0x43, 0xA5, 0xAE, 0x8C, 0xD7,
// 0xD5, 0xA6, 0xCE, 0x03}; static kdu_core::kdu_byte world_uuid[] = {0x96, 0xa9, 0xf1, 0xf1, 0xdc, 0x98, 0x40, 0x2d,
// 0xa7, 0xae, 0xd6, 0x8e, 0x34, 0x45, 0x18, 0x09};


/*!
 * Local class for handling kakadu warnings
 */
class KduSipiWarning : public kdu_core::kdu_message
{
private:
  std::string msg;

public:
  KduSipiWarning() : kdu_message() { msg = "KAKADU-WARNING: "; }

  KduSipiWarning(const char *lead_in) : kdu_message(), msg(lead_in) {}

  void put_text(const char *str) override { msg += str; }

  void flush(bool end_of_message = false) override
  {
    if (end_of_message) { log_warn("%s", msg.c_str()); }
  }
};
//=============================================================================

/*!
 * Local class for handling kakadu errors. It overrides the "exit()" call and
 * throws a kdu_exception...
 */
class KduSipiError : public kdu_core::kdu_message
{
private:
  std::string msg;

public:
  KduSipiError() : kdu_message() { msg = "KAKADU-ERROR: "; }

  KduSipiError(const char *lead_in) : kdu_message(), msg(lead_in) {}

  void put_text(const char *str) override { msg += str; }

  void flush(bool end_of_message = false) override
  {
    if (end_of_message) {
      log_err("%s", msg.c_str());
      throw KDU_ERROR_EXCEPTION;
    }
  }

  void setMsg(const std::string &msg);
};
//=============================================================================

static KduSipiWarning kdu_sipi_warn("Kakadu-library: ");
static KduSipiError kdu_sipi_error("Kakadu-library: ");

static bool is_jpx(const char *fname)
{
  int retval = 0;
  int inf = ::open(fname, O_RDONLY);
  if (inf != -1) {
    char testbuf[48];
    char sig0[] = { '\xff', '\x52' };
    char sig1[] = { '\xff', '\x4f', '\xff', '\x51' };
    char sig2[] = { '\x00', '\x00', '\x00', '\x0C', '\x6A', '\x50', '\x20', '\x20', '\x0D', '\x0A', '\x87', '\x0A' };
    auto n = read(inf, testbuf, 48);
    if ((n >= 47) && (memcmp(sig0, testbuf + 45, 2) == 0)) {
      retval = 1;
    } else if ((n >= 4) && (memcmp(sig1, testbuf, 4) == 0)) {
      retval = 1;
    } else if ((n >= 12) && (memcmp(sig2, testbuf, 12) == 0))
      retval = 1;
    close(inf);
  }
  return retval == 1;
}
//=============================================================================


Result<bool> SipiIOJ2k::read(SipiImage *img,
  const std::string &filepath,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size,
  bool force_bps_8,
  ScalingQuality scaling_quality)
{
  SIPI_ZONE_N("SipiIOJ2k::read");
  if (!is_jpx(filepath.c_str())) return false;// It's not a JPGE2000....

  int num_threads = kakadu_thread_count();

  // Custom messaging services
  kdu_customize_warnings(&kdu_sipi_warn);
  kdu_customize_errors(&kdu_sipi_error);

  kdu_core::kdu_compressed_source *input = nullptr;
  kdu_supp::kdu_simple_file_source file_in;

  kdu_supp::jp2_family_src jp2_ultimate_src;
  kdu_supp::jpx_source jpx_in;
  kdu_supp::jpx_codestream_source jpx_stream;
  kdu_supp::jpx_layer_source jpx_layer;

  kdu_supp::jp2_channels channels;
  kdu_supp::jp2_palette palette;
  kdu_supp::jp2_resolution resolution;
  kdu_supp::jp2_colour colour;

  kdu_core::kdu_codestream codestream;
  // Multi-threaded decode environment, created below (before decompressor.start)
  // when num_threads > 0. Owned by KduReadTeardown so its worker threads are
  // joined ahead of the codestream they read from.
  kdu_core::kdu_thread_env env;
  kdu_core::kdu_thread_env *env_ref = nullptr;

  // Tears down the Kakadu decode machinery exactly once on every exit path
  // (normal, throw, and error-return), in the required order: the decode worker
  // threads first (env), then the codestream, then the compressed source, then
  // the JPX container. env-before-codestream mirrors Kakadu's own
  // kdu_buffered_expand demo (env.destroy ahead of codestream.destroy), so no
  // worker thread outlives the codestream it reads from.
  struct KduReadTeardown
  {
    kdu_core::kdu_thread_env &env;
    kdu_core::kdu_codestream &codestream;
    kdu_core::kdu_compressed_source *&input;
    kdu_supp::jpx_source &jpx_in;
    bool done = false;

    void teardown()
    {
      if (done) { return; }
      done = true;
      if (env.exists()) { env.destroy(); }
      if (codestream.exists()) { codestream.destroy(); }
      if (input != nullptr) { input->close(); }
      jpx_in.close();
    }

    ~KduReadTeardown() { teardown(); }
  } kdu_teardown{ env, codestream, input, jpx_in };

  jp2_ultimate_src.open(filepath.c_str());

  bool essentials_from_uuid_box = false;
  // A corrupt JP2/JPX box structure makes jpx_in.open raise a Kakadu error
  // (thrown as a kdu_exception by KduSipiError::flush) rather than returning
  // < 0; convert it to a SipiImageError instead of letting a bare int unwind
  // past the FFI seam.
  int jpx_open_result;
  try {
    jpx_open_result = jpx_in.open(&jp2_ultimate_src, true);
  } catch (kdu_exception&) {
    jp2_ultimate_src.close();
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "Cannot read JPEG2000 file \"" + filepath + "\": corrupt JP2/JPX structure" });
  }
  if (jpx_open_result < 0) {// if < 0, not compatible with JP2 or JPX. Try opening as a raw code-stream.
    jp2_ultimate_src.close();
    file_in.open(filepath.c_str());
    input = &file_in;
  } else {
    jp2_input_box box;
    if (box.open(&jp2_ultimate_src)) {
      do {
        if (box.get_box_type() == jp2_uuid_4cc) {
          kdu_byte buf[16];
          if (box.read(buf, 16) != 16) {
            return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
              "Cannot read JPEG2000 file \"" + filepath + "\": truncated UUID box header" });
          }
          if (memcmp(buf, xmp_uuid, 16) == 0) {
            const std::int64_t xmp_len = box.get_remaining_bytes();
            if (xmp_len < 0 || xmp_len > kMaxMetadataBytes) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": metadata box length out of range" });
            }
            auto xmp_buf = std::make_unique<char[]>(xmp_len);
            if (box.read((kdu_byte *)xmp_buf.get(), static_cast<int>(xmp_len)) != xmp_len) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": truncated XMP box" });
            }
            img->set_xmp(std::make_shared<Xmp>(xmp_buf.get(),
              xmp_len));// ToDo: Problem with thread safety!!!!!!!!!!!!!!
          } else if (memcmp(buf, iptc_uuid, 16) == 0) {
            const std::int64_t iptc_len = box.get_remaining_bytes();
            if (iptc_len < 0 || iptc_len > kMaxMetadataBytes) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": metadata box length out of range" });
            }
            auto iptc_buf = std::make_unique<unsigned char[]>(iptc_len);
            if (box.read(iptc_buf.get(), static_cast<int>(iptc_len)) != iptc_len) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": truncated IPTC box" });
            }
            // A malformed IPTC blob is fatal: SIPI is a repository and must
            // not admit corrupt embedded metadata. kdu_teardown releases the
            // live Kakadu resources on this early return.
            if (auto iptc = Iptc::parse(iptc_buf.get(), iptc_len)) {
              img->set_iptc(*iptc);
            } else {
              return std::unexpected(iptc.error());
            }
          } else if (memcmp(buf, exif_uuid, 16) == 0) {
            const std::int64_t exif_len = box.get_remaining_bytes();
            if (exif_len < 0 || exif_len > kMaxMetadataBytes) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": metadata box length out of range" });
            }
            auto exif_buf = std::make_unique<unsigned char[]>(exif_len);
            if (box.read(exif_buf.get(), static_cast<int>(exif_len)) != exif_len) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": truncated EXIF box" });
            }
            // A malformed EXIF blob is fatal: SIPI is a repository and must
            // not admit corrupt embedded metadata. kdu_teardown releases the
            // live Kakadu resources on this early return.
            if (auto exif = Exif::parse(exif_buf.get(), exif_len)) {
              img->set_exif(*exif);
            } else {
              return std::unexpected(exif.error());
            }
          } else if (memcmp(buf, sipi_essentials_uuid, 16) == 0) {
            // SIPI Essentials carrier (ADR-0005 / DEV-6410). New on-disk
            // location for the packet; the codestream-comment branch below
            // remains as the legacy fallback for pre-rollout JP2 files.
            const std::int64_t ess_len = box.get_remaining_bytes();
            if (ess_len < 0 || ess_len > kMaxMetadataBytes) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": metadata box length out of range" });
            }
            auto ess_buf = std::make_unique<std::byte[]>(ess_len);
            if (box.read(reinterpret_cast<kdu_byte *>(ess_buf.get()), static_cast<int>(ess_len)) != ess_len) {
              return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
                "Cannot read JPEG2000 file \"" + filepath + "\": truncated Essentials box" });
            }
            std::span<const std::byte> bytes(ess_buf.get(), ess_len);
            if (auto parsed = Essentials::parse(bytes)) {
              img->essential_metadata(*parsed);
              essentials_from_uuid_box = true;
            } else {
              log_warn("Essentials: protobuf parse failed for %s (variant=%d); falling back to codestream comment",
                filepath.c_str(),
                static_cast<int>(parsed.error()));
            }
          }
        }
        box.close();
      } while (box.open_next());
    }

    int stream_id = 0;
    // A truncated/corrupt JP2 container reaches here with no usable codestream;
    // Kakadu raises an error (thrown as a kdu_exception) from access_codestream /
    // open_stream. Convert it to a SipiImageError after closing the sources, so
    // the failure is an enriched, leak-free error rather than a bare catch-all.
    try {
      jpx_stream = jpx_in.access_codestream(stream_id);
      input = jpx_stream.open_stream();
      palette = jpx_stream.access_palette();
    } catch (kdu_exception&) {
      // KduReadTeardown closes whatever is live (input is set only if
      // open_stream() returned before the throw; the codestream is not
      // created yet). jp2_ultimate_src is released by its destructor on
      // unwind, as on the normal path.
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": no usable codestream in JP2/JPX container" });
    }
  }

  // Corrupt or truncated input makes create()/header parsing raise a Kakadu
  // error, which KduSipiError::flush throws as a kdu_exception. Convert it to a
  // SipiImageError (as the JPEG/PNG handlers do); KduReadTeardown tears down
  // the codestream + source, so the engine reports an enriched error rather
  // than a bare catch-all, and no source is leaked on the failure path.
  int maximal_reduce;
  try {
    codestream.create(input);
    // codestream.set_fussy(); // Set the parsing error tolerance.
    codestream.set_fast();// No errors expected in input
    maximal_reduce = codestream.get_min_dwt_levels();
  } catch (kdu_exception&) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "Cannot read JPEG2000 file \"" + filepath + "\": corrupt or truncated codestream" });
  }

  //
  // Legacy codestream-comment fallback. Only consulted when the SIPI UUID
  // box was absent — pre-rollout JP2 files use this carrier. No in-tree
  // writer emits both carriers; if a file somehow carries both, the
  // UUID-box parse above has already populated `essential_metadata` and
  // this loop is skipped.
  //
  if (!essentials_from_uuid_box) {
    kdu_codestream_comment comment = codestream.get_comment();
    while (comment.exists()) {
      const char *cstr = comment.get_text();
      if (cstr != nullptr && strncmp(cstr, "SIPI:", 5) == 0) {
        Essentials se = Essentials::parse_legacy(cstr + 5);
        img->essential_metadata(se);
        break;
      }
      comment = codestream.get_comment(comment);
    }
  }

  //
  // get the size of the full image (without reduce!)
  //
  siz_params *siz = codestream.access_siz();
  int __nx, __ny;
  siz->get(Ssize, 0, 0, __ny);
  siz->get(Ssize, 0, 1, __nx);

  //
  // is there a region of interest defined ? If yes, get the cropping parameters...
  //
  kdu_core::kdu_dims roi;
  bool do_roi = false;
  if ((region != nullptr) && (region->getType()) != SipiRegion::FULL) {
    size_t sx, sy;
    region->crop_coords(__nx, __ny, roi.pos.x, roi.pos.y, sx, sy);
    roi.size.x = sx;
    roi.size.y = sy;
    do_roi = true;
  }

  //
  // here we prepare tha scaling/reduce stuff...
  //
  int reduce = maximal_reduce;
  size_t nnx, nny;
  bool redonly = true;// we assume that only a reduce is necessary
  if ((size != nullptr) && (size->getType() != SipiSize::FULL)) {
    if (do_roi) {
      size->get_size(roi.size.x, roi.size.y, nnx, nny, reduce, redonly);
    } else {
      size->get_size(__nx, __ny, nnx, nny, reduce, redonly);
    }
  } else {
    reduce = 0;
  }

  if (reduce < 0) reduce = 0;

  codestream.apply_input_restrictions(0, 0, reduce, 0, do_roi ? &roi : nullptr);


  // Output components, as kdu_stripe_decompressor counts them: every buffer,
  // stripe height and loop bound below uses this one count.
  const int nc = codestream.get_num_components(true);
  kdu_core::kdu_dims dims;
  codestream.get_dims(0, dims, true);

  // A region at the image edge can reduce to no samples: at reduction 32,
  // ceil(512/32) - ceil(508/32) = 0. Kakadu's decode must not see an empty
  // window; x86-64 dies there with SIGFPE.
  if (dims.size.x <= 0 || dims.size.y <= 0) {
    return std::unexpected(SipiValueError{ ErrorCode::kInvalidRequestParameter,
      "Cannot read JPEG2000 file \"" + filepath + "\": the requested region and size leave no samples to decode" });
  }

  // bitdepth of zeroth component, assumed valid for all; nc is not the same as
  // the number of colors. No pixel buffer exists yet, so geometry alone moves.
  img->set_geometry(dims.size.x, dims.size.y, nc, codestream.get_bit_depth(0));

  if (auto r = validate_decode_dims(img->getNx(), img->getNy(), img->getNc(), static_cast<int>(img->getBps()), filepath);
      !r) {
    return std::unexpected(r.error());
  }

  // Subsampling of each component relative to component 0, from the
  // fundamental factors (a dims ratio is off by the ceil() of odd extents).
  // Only factors 1 and 2 are decoded; anything else fails here, before
  // decompressor.start.
  std::vector<kdu_core::kdu_coords> factors(nc);
  bool subsampled = false;
  const kdu_core::kdu_dims requested = dims;
  // A corrupt codestream can make these Kakadu queries raise an error, thrown
  // as a kdu_exception; it must leave as a value error like the reads above.
  try {
    {
      kdu_core::kdu_coords s0;
      codestream.get_min_subsampling(0, s0, true);
      const auto supported = [](int s, int ref) { return s % ref == 0 && (s / ref == 1 || s / ref == 2); };
      for (int c = 0; c < nc; ++c) {
        kdu_core::kdu_coords sc;
        codestream.get_min_subsampling(c, sc, true);
        if (!supported(sc.x, s0.x) || !supported(sc.y, s0.y)) {
          return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
            "Cannot read JPEG2000 file \"" + filepath + "\": component " + std::to_string(c) + " subsampling "
              + std::to_string(sc.x) + "x" + std::to_string(sc.y) + " relative to component 0's " + std::to_string(s0.x)
              + "x" + std::to_string(s0.y) + " is not supported" });
        }
        factors[c] = kdu_core::kdu_coords(sc.x / s0.x, sc.y / s0.y);
        subsampled = subsampled || factors[c].x != 1 || factors[c].y != 1;
      }
    }

    // A region read of a subsampled image decodes a window expanded outward to
    // whole subsampled samples and crops back to `requested` after the decode.
    // Without it a small region at an odd origin gets a zero-sample component
    // buffer, or the sample covering its first column lies outside the window.
    if (subsampled && do_roi) {
      kdu_core::kdu_coords s0;
      codestream.get_subsampling(0, s0, true);// includes the reduce levels
      int fx = 1;
      int fy = 1;
      for (const auto &f : factors) {
        fx = std::max(fx, f.x);
        fy = std::max(fy, f.y);
      }
      const int ax = fx * s0.x;
      const int ay = fy * s0.y;
      kdu_core::kdu_dims expanded;
      expanded.pos.x = roi.pos.x - roi.pos.x % ax;
      expanded.pos.y = roi.pos.y - roi.pos.y % ay;
      const int end_x = std::min(__nx, (roi.pos.x + roi.size.x + ax - 1) / ax * ax);
      const int end_y = std::min(__ny, (roi.pos.y + roi.size.y + ay - 1) / ay * ay);
      expanded.size.x = end_x - expanded.pos.x;
      expanded.size.y = end_y - expanded.pos.y;
      codestream.apply_input_restrictions(0, 0, reduce, 0, &expanded);
      codestream.get_dims(0, dims, true);
      img->set_geometry(dims.size.x, dims.size.y, nc, codestream.get_bit_depth(0));
      if (auto r = validate_decode_dims(img->getNx(), img->getNy(), img->getNc(), static_cast<int>(img->getBps()), filepath);
          !r) {
        return std::unexpected(r.error());
      }
    }
    // pull_upsampled indexes every component's buffer, and the crop-back reads
    // `requested`: neither may be empty.
    if (subsampled) {
      bool empty = requested.size.x <= 0 || requested.size.y <= 0;
      for (int c = 0; c < nc && !empty; ++c) {
        kdu_core::kdu_dims d;
        codestream.get_dims(c, d, true);
        empty = d.size.x <= 0 || d.size.y <= 0;
      }
      if (empty) {
        return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
          "Cannot read JPEG2000 file \"" + filepath
            + "\": the requested region is empty or leaves a subsampled component without samples" });
      }
    }
  } catch (kdu_exception &) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "Cannot read JPEG2000 file \"" + filepath + "\": corrupt component subsampling data" });
  }

  //
  // The following definitions we need in case we get a palette color image!
  //
  std::vector<byte> rlut;
  std::vector<byte> glut;
  std::vector<byte> blut;
  //
  // get ICC-Profile if available
  //
  jpx_layer = jpx_in.access_layer(0);
  img->setPhoto(PhotometricInterpretation::INVALID);// we initialize to an invalid value in order to test later if
                                                     // photo has been set
  int numcol = 0;
  std::vector<int> channel_of;
  if (jpx_layer.exists()) {
    kdu_supp::jp2_colour colinfo = jpx_layer.access_colour(0);
    kdu_supp::jp2_channels chaninfo = jpx_layer.access_channels();
    numcol = chaninfo.get_num_colours();// I assume these are the color channels (1, 3 or 4 in case of CMYK)
    channel_of = jp2_channel_of(chaninfo, numcol, nc);
    int nluts = palette.get_num_luts();
    if (nluts == 3) {
      int nentries = palette.get_num_entries();
      // The palette-expansion loop below only handles a single 8-bit index
      // component mapped to a 3-channel (RGB) output; reject anything else
      // instead of silently mis-decoding or overrunning the expansion buffer.
      if (auto validated =
            validate_j2k_palette_mapping(img->getBps(), img->getNc(), numcol, nentries, filepath);
          !validated) {
        return std::unexpected(validated.error());
      }
      rlut.resize(nentries);
      glut.resize(nentries);
      blut.resize(nentries);
      std::vector<float> tmplut(nentries);

      palette.get_lut(0, tmplut.data());
      for (int i = 0; i < nentries; i++) { rlut[i] = roundf((tmplut[i] + 0.5) * 255.0); }

      palette.get_lut(1, tmplut.data());
      for (int i = 0; i < nentries; i++) { glut[i] = roundf((tmplut[i] + 0.5) * 255.0); }

      palette.get_lut(2, tmplut.data());
      for (int i = 0; i < nentries; i++) { blut[i] = roundf((tmplut[i] + 0.5) * 255.0); }
    }
    img->setOrientation(TOPLEFT);
    if (img->getNc() > numcol) {// we have more components than colors -> alpha channel!
      for (size_t i = 0; i < img->getNc() - numcol; i++) {// nc - numcol: number of alpha channels (?)
        img->addEs(ExtraSamples::ASSOCALPHA);
      }
    }
    if (colinfo.exists()) {
      int space = colinfo.get_space();
      switch (space) {
      case kdu_supp::JP2_sRGB_SPACE: {
        img->setPhoto(PhotometricInterpretation::RGB);
        img->set_icc(std::make_shared<Icc>(icc_sRGB));
        break;
      }
      case kdu_supp::JP2_CMYK_SPACE: {
        img->setPhoto(PhotometricInterpretation::SEPARATED);
        img->set_icc(std::make_shared<Icc>(icc_CMYK_standard));
        break;
      }
      case kdu_supp::JP2_YCbCr1_SPACE: {
        img->setPhoto(PhotometricInterpretation::YCBCR);
        img->set_icc(std::make_shared<Icc>(icc_sRGB));
        break;
      }
      case kdu_supp::JP2_YCbCr2_SPACE:
      case kdu_supp::JP2_YCbCr3_SPACE: {
        float whitepoint[] = { 0.3127, 0.3290 };
        float primaries[] = { 0.630, 0.340, 0.310, 0.595, 0.155, 0.070 };
        img->setPhoto(PhotometricInterpretation::YCBCR);
        if (auto icc = Icc::createRGB(whitepoint, primaries)) {
          img->set_icc(*icc);
        } else {
          return std::unexpected(icc.error());
        }
        break;
      }
      case kdu_supp::JP2_iccRGB_SPACE: {
        img->setPhoto(PhotometricInterpretation::RGB);
        int icc_len;
        const unsigned char *icc_buf = colinfo.get_icc_profile(&icc_len);
        if (auto icc = Icc::parse(icc_buf, icc_len)) {
          img->set_icc(*icc);
        } else {
          return std::unexpected(icc.error());
        }
        break;
      }
      case kdu_supp::JP2_iccANY_SPACE: {
        if (numcol == 1) {
          img->setPhoto(PhotometricInterpretation::MINISBLACK);
        } else if (numcol == 3) {
          img->setPhoto(PhotometricInterpretation::RGB);
        } else if (numcol == 4) {
          img->setPhoto(PhotometricInterpretation::SEPARATED);
        } else {
          log_err("Unsupported number of colors: %d", numcol);
          return std::unexpected(
            SipiValueError{ ErrorCode::kUnsupportedFormat, "Unsupported number of colors: " + std::to_string(numcol) });
        }
        int icc_len;
        const unsigned char *icc_buf = colinfo.get_icc_profile(&icc_len);
        if (auto icc = Icc::parse(icc_buf, icc_len)) {
          img->set_icc(*icc);
        } else {
          return std::unexpected(icc.error());
        }
        break;
      }
      case kdu_supp::JP2_sLUM_SPACE: {
        img->setPhoto(PhotometricInterpretation::MINISBLACK);
        img->set_icc(std::make_shared<Icc>(icc_LUM_D65));
        break;
      }
      case kdu_supp::JP2_sYCC_SPACE: {
        img->setPhoto(PhotometricInterpretation::YCBCR);
        img->set_icc(std::make_shared<Icc>(icc_sRGB));
        break;
      }
      case kdu_supp::JP2_CIELab_SPACE: {
        img->setPhoto(PhotometricInterpretation::CIELAB);
        img->set_icc(std::make_shared<Icc>(icc_LAB));
        break;
      }
      case 100: {
        img->setPhoto(PhotometricInterpretation::MINISBLACK);
        img->set_icc(std::make_shared<Icc>(icc_ROMM_GRAY));
        break;
      }

      default: {
        log_err("Unsupported ICC profile: %s", std::to_string(space).c_str());
        return std::unexpected(
          SipiValueError{ ErrorCode::kUnsupportedFormat, "Unsupported ICC profile: " + std::to_string(space) });
      }
      }
    }
  } else {
    numcol = img->getNc();
  }

  if (img->getPhoto() == PhotometricInterpretation::INVALID) {
    switch (numcol) {
    case 1: {
      img->setPhoto(PhotometricInterpretation::MINISBLACK);
      break;
    }
    case 3: {
      img->setPhoto(PhotometricInterpretation::RGB);
      break;
    }
    case 4: {
      img->setPhoto(PhotometricInterpretation::SEPARATED);
      break;
    }
    default: {
      return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
        "Cannot determine photometric interpretation for JPEG2000 file \"" + filepath
          + "\": unsupported number of color channels (" + std::to_string(numcol) + ")" });
    }
    }// switch(numcol)
  }

  //
  // the following code directly converts a 16-Bit jpx into an 8-bit image.
  // In order to retrieve a 16-Bit image, use kdu_uin16 *buffer and the apropriate signature of the pull_stripe method
  //
  kdu_supp::kdu_stripe_decompressor decompressor;
  try {
    // Multi-threaded decode: one worker per core (mirroring the encode path in
    // write()), so the inverse DWT and sample processing run across all cores.
    // num_threads is 0 on a single-core host, in which case env_ref stays
    // NULL and decode falls back to the single-threaded path.
    if (num_threads > 0) {
      env.create();
      for (int nt = 1; nt < num_threads; nt++) {
        if (!env.add_thread()) {
          num_threads = nt;// Unable to create all the threads requested
          break;
        }
      }
      env_ref = &env;
    }
    decompressor.start(codestream, false, false, env_ref);
  } catch (kdu_exception&) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "Cannot read JPEG2000 file \"" + filepath + "\": corrupt codestream (decompressor start failed)" });
  }
  std::vector<int> stripe_heights(nc, dims.size.y);
  // Sample-interleaved output with each component at its mapped channel. A
  // non-NULL sample_offsets makes Kakadu default NULL sample_gaps to 1
  // (component-interleaved), so gaps and row gaps are passed alongside.
  std::vector<int> sample_gaps;
  std::vector<int> row_gaps;
  if (!channel_of.empty()) {
    sample_gaps.assign(channel_of.size(), static_cast<int>(img->getNc()));
    row_gaps.assign(channel_of.size(), dims.size.x * static_cast<int>(img->getNc()));
  }
  const int *offsets_p = channel_of.empty() ? nullptr : channel_of.data();
  const int *sample_gaps_p = channel_of.empty() ? nullptr : sample_gaps.data();
  const int *row_gaps_p = channel_of.empty() ? nullptr : row_gaps.data();

  if (force_bps_8) img->set_geometry(img->getNx(), img->getNy(), img->getNc(), 8);// forces kakadu to convert to 8 bit!
  switch (img->getBps()) {
  case 8: {
    const auto buf8_size = checked_buf_size(
      static_cast<std::size_t>(dims.size.x), static_cast<std::size_t>(dims.size.y), static_cast<std::size_t>(img->getNc()), 1);
    if (!buf8_size) {
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": image dimensions overflow buffer size" });
    }
    // The pixel buffer is owned by img (not a local) for the whole risk
    // window below: pull_stripe can throw a kdu_exception, and matching the
    // other handlers' ownership shape keeps that failure leak-free.
    img->set_pixels(std::vector<byte>(*buf8_size), img->getNx(), img->getNy(), img->getNc(), img->getBps());
    byte *dst = img->pixels_writable().data();
    try {
      if (subsampled) {
        pull_upsampled(decompressor, codestream, factors, channel_of, dst);
      } else {
        decompressor.pull_stripe(dst, stripe_heights.data(), offsets_p, sample_gaps_p, row_gaps_p);
      }
    } catch (kdu_exception&) {
      log_err("Error while decompressing image: %s.", filepath.c_str());
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": corrupt codestream (stripe decode failed)" });
    }
    break;
  }
  case 12: {
    std::vector<char> get_signed(nc, 0);// vector<bool> does not work -> special treatment in C++
    const auto buf16_size = checked_buf_size(
      static_cast<std::size_t>(dims.size.x), static_cast<std::size_t>(dims.size.y), static_cast<std::size_t>(img->getNc()), 2);
    if (!buf16_size) {
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": image dimensions overflow buffer size" });
    }
    img->set_pixels(std::vector<byte>(*buf16_size), img->getNx(), img->getNy(), img->getNc(), 16);
    byte *dst = img->pixels_writable().data();
    try {
      if (subsampled) {
        pull_upsampled(decompressor, codestream, factors, channel_of, reinterpret_cast<kdu_core::kdu_int16 *>(dst));
      } else {
        decompressor.pull_stripe(reinterpret_cast<kdu_core::kdu_int16 *>(dst),
          stripe_heights.data(),
          offsets_p,
          sample_gaps_p,
          row_gaps_p,
          nullptr,
          reinterpret_cast<bool *>(get_signed.data()));
      }
    } catch (kdu_exception&) {
      log_err("Error while decompressing image: %s.", filepath.c_str());
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": corrupt codestream (stripe decode failed)" });
    }
    break;
  }
  case 16: {
    std::vector<char> get_signed(nc, 0);// vector<bool> does not work -> special treatment in C++
    const auto buf16_size = checked_buf_size(
      static_cast<std::size_t>(dims.size.x), static_cast<std::size_t>(dims.size.y), static_cast<std::size_t>(img->getNc()), 2);
    if (!buf16_size) {
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": image dimensions overflow buffer size" });
    }
    img->set_pixels(std::vector<byte>(*buf16_size), img->getNx(), img->getNy(), img->getNc(), img->getBps());
    byte *dst = img->pixels_writable().data();
    try {
      if (subsampled) {
        pull_upsampled(decompressor, codestream, factors, channel_of, reinterpret_cast<kdu_core::kdu_int16 *>(dst));
      } else {
        decompressor.pull_stripe(reinterpret_cast<kdu_core::kdu_int16 *>(dst),
          stripe_heights.data(),
          offsets_p,
          sample_gaps_p,
          row_gaps_p,
          nullptr,
          reinterpret_cast<bool *>(get_signed.data()));
      }
    } catch (kdu_exception&) {
      log_err("Error while decompressing image: %s.", filepath.c_str());
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": corrupt codestream (stripe decode failed)" });
    }
    break;
  }
  default: {
    decompressor.finish();
    log_err("Unsupported number of bits/sample: %ld in file %s", img->getBps(), filepath.c_str());
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Cannot read JPEG2000: unsupported bits/sample (" + std::to_string(img->getBps()) + ") in file \"" + filepath
        + "\" (dimensions: " + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy())
        + ", channels: " + std::to_string(img->getNc()) + ")" });
  }
  }
  decompressor.finish();
  kdu_teardown.teardown();

  if (subsampled && do_roi) {
    if (auto r = processing::crop(*img,
          requested.pos.x - dims.pos.x,
          requested.pos.y - dims.pos.y,
          static_cast<std::size_t>(requested.size.x),
          static_cast<std::size_t>(requested.size.y));
      !r) {
      return std::unexpected(r.error());
    }
  }

  if (!rlut.empty()) {
    //
    // we have a palette color image...
    //
    // numcol == 3 and img->getBps() == 8 are enforced above where rlut/glut/blut
    // are built, so the buffer's stride-3 layout matches the writes below
    // and every index into rlut/glut/blut is in range.
    const auto tmpbuf_size = checked_buf_size(
      static_cast<std::size_t>(img->getNx()), static_cast<std::size_t>(img->getNy()), static_cast<std::size_t>(numcol), 1);
    if (!tmpbuf_size) {
      return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
        "Cannot read JPEG2000 file \"" + filepath + "\": palette-expanded buffer size overflow" });
    }
    std::vector<byte> tmpbuf(*tmpbuf_size);
    const byte *src = img->pixels_view().data();
    const size_t nx_val = img->getNx();
    const size_t ny_val = img->getNy();
    for (int y = 0; y < ny_val; ++y) {
      for (int x = 0; x < nx_val; ++x) {
        const byte idx = src[y * nx_val + x];
        tmpbuf[3 * (y * nx_val + x) + 0] = rlut[idx];
        tmpbuf[3 * (y * nx_val + x) + 1] = glut[idx];
        tmpbuf[3 * (y * nx_val + x) + 2] = blut[idx];
      }
    }
    img->set_pixels(std::move(tmpbuf), nx_val, ny_val, numcol, img->getBps());
  }
  if (img->getPhoto() == PhotometricInterpretation::YCBCR) {
    if (auto r = processing::convertYCC2RGB(*img); !r) { return std::unexpected(r.error()); }
    img->setPhoto(PhotometricInterpretation::RGB);
  }

  if ((size != nullptr) && (!redonly)) {
    if (auto r = Sipi::processing::scale_with(
          *img, nnx, nny, scaling_method_for(SipiQualityFormat::JP2, scaling_quality));
        !r) {
      return std::unexpected(r.error());
    }
  }
  return true;
}
//=============================================================================


Result<SipiImgInfo> SipiIOJ2k::read_shape(const std::string &filepath)
{
  SIPI_ZONE_N("SipiIOJ2k::read_shape");
  SipiImgInfo info;
  if (!is_jpx(filepath.c_str())) {
    info.success = SipiImgInfo::FAILURE;
    return info;
  }

  kdu_customize_warnings(&kdu_sipi_warn);
  kdu_customize_errors(&kdu_sipi_error);

  kdu_supp::jp2_family_src jp2_ultimate_src;
  kdu_supp::jpx_source jpx_in;
  kdu_supp::jpx_codestream_source jpx_stream;
  kdu_core::kdu_compressed_source *input = nullptr;
  kdu_supp::kdu_simple_file_source file_in;

  kdu_core::kdu_codestream codestream;

  // Same exactly-once teardown as in read(): a corrupt file makes
  // codestream.create() (or set_fussy header parsing) throw a kdu_exception,
  // and the sources must not leak on that path.
  struct KduReadTeardown
  {
    kdu_core::kdu_codestream &codestream;
    kdu_core::kdu_compressed_source *&input;
    kdu_supp::jpx_source &jpx_in;
    bool done = false;

    void teardown()
    {
      if (done) { return; }
      done = true;
      if (codestream.exists()) { codestream.destroy(); }
      if (input != nullptr) { input->close(); }
      jpx_in.close();
    }

    ~KduReadTeardown() { teardown(); }
  } kdu_teardown{ codestream, input, jpx_in };

  jp2_ultimate_src.open(filepath.c_str());

  bool essentials_from_uuid_box = false;
  bool take_fast_path = false;
  bool essentials_parse_failed = false;
  bool essentials_partial = false;
  // A corrupt JP2/JPX box structure makes jpx_in.open raise a Kakadu error
  // (thrown as a kdu_exception by KduSipiError::flush) rather than returning
  // < 0; convert it to a SipiValueError instead of letting a bare int unwind
  // past the FFI seam.
  int jpx_open_result;
  try {
    jpx_open_result = jpx_in.open(&jp2_ultimate_src, true);
  } catch (kdu_exception &) {
    return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
      "Cannot read JPEG2000 file \"" + filepath + "\": corrupt JP2/JPX structure" });
  }
  if (jpx_open_result < 0) {// if < 0, not compatible with JP2 or JPX.  Try opening as a raw code-stream.
    jp2_ultimate_src.close();
    file_in.open(filepath.c_str());
    input = &file_in;
  } else {
    // Walk top-level UUID boxes for the SIPI Essentials carrier (ADR-0005 /
    // DEV-6410). Same scan loop as the main reader at the top of this file;
    // the codestream-comment fallback below covers pre-rollout files. When the
    // packet's image-shape fields are populated, the fast path returns
    // directly from here without calling codestream.create().
    jp2_input_box box;
    if (box.open(&jp2_ultimate_src)) {
      do {
        if (box.get_box_type() == jp2_uuid_4cc) {
          kdu_byte buf[16];
          if (box.read(buf, 16) != 16) {
            return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
              "Cannot read JPEG2000 file \"" + filepath + "\": truncated UUID box header" });
          }
          if (memcmp(buf, sipi_essentials_uuid, 16) == 0) {
            const std::int64_t ess_len = box.get_remaining_bytes();
            if (ess_len < 0 || ess_len > kMaxMetadataBytes) {
              return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
                "Cannot read JPEG2000 file \"" + filepath + "\": metadata box length out of range" });
            }
            auto ess_buf = std::make_unique<std::byte[]>(ess_len);
            if (box.read(reinterpret_cast<kdu_byte *>(ess_buf.get()), static_cast<int>(ess_len)) != ess_len) {
              return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
                "Cannot read JPEG2000 file \"" + filepath + "\": truncated Essentials box" });
            }
            std::span<const std::byte> bytes(ess_buf.get(), ess_len);
            if (auto parsed = Essentials::parse(bytes)) {
              const auto &f = parsed->fields();
              info.origmimetype = f.mimetype;
              info.origname = f.origname;
              info.success = SipiImgInfo::ALL;
              essentials_from_uuid_box = true;
              // Fast path (ADR-0004): if BOTH img_w and img_h are
              // non-zero, populate every shape field from the packet and skip
              // codestream creation entirely. Partial population (one but not
              // the other) → fall through to slow path.
              if (f.img_w != 0 && f.img_h != 0) {
                // Reject an Essentials packet that claims malformed shape before trusting
                // its fields for the fast path (malformed metadata is fatal, not logged).
                if (auto r = validate_decode_dims(static_cast<std::size_t>(f.img_w),
                      static_cast<std::size_t>(f.img_h),
                      static_cast<std::size_t>(f.nc),
                      static_cast<int>(f.bps),
                      filepath);
                    !r) {
                  return std::unexpected(std::move(r).error());
                }
                info.width = static_cast<int>(f.img_w);
                info.height = static_cast<int>(f.img_h);
                info.tile_width = static_cast<int>(f.tile_w);
                info.tile_height = static_cast<int>(f.tile_h);
                info.clevels = static_cast<int>(f.clevels);
                info.numpages = static_cast<int>(f.numpages);
                info.nc = static_cast<int>(f.nc);
                info.bps = static_cast<int>(f.bps);
                take_fast_path = true;
              } else if (f.img_w != 0 || f.img_h != 0) {
                essentials_partial = true;
              }
            } else {
              essentials_parse_failed = true;
            }
          }
        }
        box.close();
      } while (box.open_next());
    }
    if (take_fast_path) {
      Sipi::observability::read_shape_fast_path_counter(
        Sipi::observability::EssentialsFormat::Jp2,
        Sipi::observability::ReadShapeFastPathOutcome::Hit)
        .Increment();
      return info;
    }
    // Pre-record fast-path outcome for the slow path; the actual
    // increment happens in the unified post-shape block below.
    int stream_id = 0;
    // A truncated/corrupt JP2 container reaches here with no usable
    // codestream; Kakadu raises an error (thrown as a kdu_exception) from
    // access_codestream/open_stream. Convert it to a SipiValueError so the
    // failure is an enriched, leak-free error rather than a bare kdu_exception
    // escaping past the FFI seam.
    try {
      jpx_stream = jpx_in.access_codestream(stream_id);
      input = jpx_stream.open_stream();
    } catch (kdu_exception &) {
      return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
        "Cannot read JPEG2000 file \"" + filepath + "\": no usable codestream in JP2/JPX container" });
    }
  }

  // Corrupt or truncated input makes create()/header parsing raise a Kakadu
  // error, which KduSipiError::flush throws as a kdu_exception. Convert it to
  // a SipiValueError; KduReadTeardown tears down the codestream + source, so
  // the engine reports an enriched error rather than a bare kdu_exception, and
  // no source is leaked on the failure path. The subsequent siz->get(...)
  // shape reads parse the same header, so they stay inside the same guarded
  // region.
  int tmp_height = 0, tmp_width = 0, __tnx = 0, __tny = 0;
  try {
    codestream.create(input);
    codestream.set_fussy();// Set the parsing error tolerance.

    //
    // get the size of the full image (without reduce!)
    //
    siz_params *siz = codestream.access_siz();
    siz->get(Ssize, 0, 0, tmp_height);
    siz->get(Ssize, 0, 1, tmp_width);
    siz->get(Stiles, 0, 0, __tny);
    siz->get(Stiles, 0, 1, __tnx);
    info.clevels = codestream.get_min_dwt_levels();
    info.nc = codestream.get_num_components();
    info.bps = codestream.get_bit_depth(0);
  } catch (kdu_exception &) {
    return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
      "Cannot read JPEG2000 file \"" + filepath + "\": corrupt or truncated codestream" });
  }
  info.height = tmp_height;
  info.width = tmp_width;
  if (info.success == SipiImgInfo::FAILURE) { info.success = SipiImgInfo::DIMS; }
  info.tile_width = __tnx;
  info.tile_height = __tny;

  bool essentials_from_codestream_comment = false;
  if (!essentials_from_uuid_box) {
    kdu_codestream_comment comment = codestream.get_comment();
    while (comment.exists()) {
      const char *cstr = comment.get_text();
      if (cstr != nullptr && strncmp(cstr, "SIPI:", 5) == 0) {
        Essentials se = Essentials::parse_legacy(cstr + 5);
        info.origmimetype = se.fields().mimetype;
        info.origname = se.fields().origname;
        info.success = SipiImgInfo::ALL;
        essentials_from_codestream_comment = true;
        break;
      }
      comment = codestream.get_comment(comment);
    }
  }

  // Slow-path outcome classification (ADR-0004). Reached when
  // either no SIPI UUID box was found, or one was found but did not
  // populate both img_w and img_h. Precedence: parse failure > partial >
  // fallback (legacy carrier) > miss.
  using Outcome = Sipi::observability::ReadShapeFastPathOutcome;
  Outcome outcome = Outcome::Miss;
  if (essentials_parse_failed) {
    outcome = Outcome::Fallback;
  } else if (essentials_partial) {
    outcome = Outcome::Partial;
  } else if (essentials_from_codestream_comment) {
    outcome = Outcome::Fallback;
  }
  Sipi::observability::read_shape_fast_path_counter(
    Sipi::observability::EssentialsFormat::Jp2, outcome).Increment();

  return info;
}
//=============================================================================


static void write_xmp_box(kdu_supp::jp2_family_tgt *tgt, const char *xmpstr)
{
  kdu_supp::jp2_output_box out;
  out.open(tgt, jp2_uuid_4cc);
  out.set_target_size(strlen(xmpstr) + sizeof(xmp_uuid));
  out.write(xmp_uuid, 16);
  out.write((kdu_core::kdu_byte *)xmpstr, strlen(xmpstr));
  out.close();
}
//=============================================================================

static void write_iptc_box(kdu_supp::jp2_family_tgt *tgt, kdu_core::kdu_byte *iptc, int iptc_len)
{
  kdu_supp::jp2_output_box out;
  out.open(tgt, jp2_uuid_4cc);
  out.set_target_size(iptc_len + sizeof(iptc_uuid));
  out.write(iptc_uuid, 16);
  out.write((kdu_core::kdu_byte *)iptc, iptc_len);
  out.close();
}
//=============================================================================

static void write_exif_box(kdu_supp::jp2_family_tgt *tgt, kdu_core::kdu_byte *exif, int exif_len)
{
  kdu_supp::jp2_output_box out;
  out.open(tgt, jp2_uuid_4cc);
  out.set_target_size(exif_len + sizeof(exif_uuid));
  out.write(exif_uuid, sizeof(exif_uuid));
  out.write((kdu_byte *)exif, exif_len);// NOT::: skip JPEG marker header 'E', 'x', 'i', 'f', '\0', '\0'..
  out.close();
}
//=============================================================================

// SIPI Essentials carrier (ADR-0005 / DEV-6410). Emits the protobuf-serialized
// packet as the box payload, preceded by `sipi_essentials_uuid`. Mirrors the
// XMP/IPTC/EXIF helpers above so the four UUID-box carriers stay symmetric.
// The `convert service-file` command stamps the Essentials packet on the
// SipiImage before calling write — that's what opens the call site gate
// below (see line 1234).
static void write_essentials_box(kdu_supp::jp2_family_tgt *tgt, const std::vector<std::byte> &payload)
{
  kdu_supp::jp2_output_box out;
  out.open(tgt, jp2_uuid_4cc);
  out.set_target_size(payload.size() + sizeof(sipi_essentials_uuid));
  out.write(sipi_essentials_uuid, sizeof(sipi_essentials_uuid));
  if (!payload.empty()) { out.write(reinterpret_cast<const kdu_core::kdu_byte *>(payload.data()), payload.size()); }
  out.close();
}
//=============================================================================

Result<void> SipiIOJ2k::write(SipiImage *img, const OutputSink &sink, const SipiCompressionParams *params)
{
  SIPI_ZONE_N("SipiIOJ2k::write");
  // A streamed sink (callback/tee) writes via J2kHttpStream → SinkStream; a
  // FilePath opens the JP2 target on the path. `filepath` is derived only for
  // the file branch and the error messages below.
  const bool streaming = is_streaming_sink(sink);
  const std::string filepath = streaming ? std::string("<http response>") : std::get<FilePath>(sink).path;

  kdu_customize_warnings(&kdu_sipi_warn);
  kdu_customize_errors(&kdu_sipi_error);

  int num_threads = kakadu_thread_count();

  kdu_membroker membroker;

  // Declared outside the try so the catch below can read `http->client_aborted`
  // to distinguish client aborts from genuine Kakadu failures. sink_stream
  // outlives http (J2kHttpStream holds a raw SinkStream*).
  std::unique_ptr<SinkStream> sink_stream;
  std::unique_ptr<J2kHttpStream> http;

  // The JPX target and its boxes are deliberately NOT closed explicitly on
  // the error path — closing a jpx_target with incompletely written
  // codestreams raises another Kakadu error. Its destructor still runs when
  // the try block unwinds, and frees the codestream target this codestream
  // flushes into, so `codestream_guard` below destroys the codestream first.
  kdu_codestream codestream;

  try {
    // Construct code-stream object
    siz_params siz;
    siz.set(Scomponents, 0, 0, (int)img->getNc());
    siz.set(Sdims, 0, 0, (int)img->getNy());// Height of first image component
    siz.set(Sdims, 0, 1, (int)img->getNx());// Width of first image component
    siz.set(Sprecision, 0, 0, (int)img->getBps());// Bits per sample (usually 8 or 16)
    siz.set(Ssigned, 0, 0, false);// Image samples are originally unsigned

    //
    // tiling has to be done here. Tile size must be adapted to image dimesions!
    //
    int tw = 0, th = 0;
    const int mindim = img->getNy() < img->getNx() ? img->getNy() : img->getNx();
    if ((params != nullptr) && (!params->empty())) {
      if (params->find(J2K_Stiles) != params->end()) {
        int n = std::sscanf(params->at(J2K_Stiles).c_str(), "{%d,%d}", &tw, &th);
        if (n != 2) { return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed, "Tiling parameter invalid!" }); }
        if ((mindim > tw) && (mindim > th)) {
          std::stringstream ss;
          ss << "Stiles=" << params->at(J2K_Stiles);
          siz.parse_string(ss.str().c_str());
        }
      }
    } else {
      if (mindim >= 4096) {
        tw = th = 1024;
      } else if (mindim >= 2048) {
        tw = th = 1024;
      } else if (mindim >= 1024) {
        tw = th = 256;
      } else {
        tw = th = 0;
      }
      if (mindim > 1024) {
        std::stringstream ss;
        ss << "Stiles={" << tw << "," << th << "}";
        siz.parse_string(ss.str().c_str());
      }
    }

    kdu_params *siz_ref = &siz;
    siz_ref->finalize();

    kdu_compressed_target *output = nullptr;

    jp2_family_tgt jp2_ultimate_tgt;

    if (streaming) {
      sink_stream = std::make_unique<SinkStream>(sink);
      http = std::make_unique<J2kHttpStream>(sink_stream.get());
      jp2_ultimate_tgt.open(http.get(), &membroker);
    } else {
      jp2_ultimate_tgt.open(filepath.c_str(), &membroker);
    }

    jpx_target jpx_out;
    jpx_out.open(&jp2_ultimate_tgt, &membroker);
    jpx_codestream_target jpx_stream = jpx_out.add_codestream();
    jpx_layer_target jpx_layer = jpx_out.add_layer();

    // Declared after `jpx_out` so it is destroyed before it: on a Kakadu error
    // `~jpx_target` frees the codestream target, and a `codestream.destroy()`
    // that runs afterwards flushes into that freed memory (heap
    // use-after-free in kd_compressed_output::flush_buf). The success path
    // destroys the codestream itself below; the guard then finds none.
    struct CodestreamGuard
    {
      kdu_codestream &cs;
      ~CodestreamGuard()
      {
        if (cs.exists()) { cs.destroy(); }
      }
    } codestream_guard{ codestream };

    // jp2_palette jp2_family_palette = jpx_stream.access_palette();
    jp2_resolution jp2_family_resolution = jpx_layer.access_resolution();

    output = jpx_stream.access_stream();

    kdu_thread_env env;
    kdu_thread_env *env_ref;

    if (num_threads > 0) {
      env.create();
      for (int nt = 1; nt < num_threads; nt++) {
        if (!env.add_thread()) {
          num_threads = nt;// Unable to create all the threads requested
          break;
        }
      }
      env_ref = &env;
    } else {
      env_ref = nullptr;
    }

    codestream.create(&siz, output, nullptr, 0, 0, env_ref, &membroker);

    // Set up any specific coding parameters and finalize them.
    std::vector<double> rates;

    //
    // always
    //
    bool is_reversible = true;
    codestream.access_siz()->parse_string("Creversible=yes");

    if ((params != nullptr) && (!params->empty())) {

      if (params->find(J2K_Sprofile) != params->end()) {
        std::stringstream ss;
        ss << "Sprofile=" << params->at(J2K_Sprofile);
        codestream.access_siz()->parse_string(ss.str().c_str());
      } else {
        codestream.access_siz()->parse_string("Sprofile=PART2");
      }

      if (params->find(J2K_Clayers) != params->end()) {
        std::stringstream ss;
        ss << "Clayers=" << params->at(J2K_Clayers);
        codestream.access_siz()->parse_string(ss.str().c_str());
      } else {
        codestream.access_siz()->parse_string("Clayers=8");
      }

      if (params->find(J2K_Clevels) != params->end()) {
        std::stringstream ss;
        ss << "Clevels=" << params->at(J2K_Clevels);
        codestream.access_siz()->parse_string(ss.str().c_str());
      } else {
        codestream.access_siz()->parse_string("Clevels=8");// resolution levels
      }

      if (params->find(J2K_Corder) != params->end()) {
        std::stringstream ss;
        ss << "Corder=" << params->at(J2K_Corder);
        codestream.access_siz()->parse_string(ss.str().c_str());
      } else {
        codestream.access_siz()->parse_string("Corder=RPCL");
      }

      if (params->find(J2K_Cprecincts) != params->end()) {
        std::stringstream ss;
        ss << "Cprecincts=" << params->at(J2K_Cprecincts);
        codestream.access_siz()->parse_string(ss.str().c_str());
      } else {
        codestream.access_siz()->parse_string("Cprecincts={256,256},{256,256},{128,128}");
      }

      if (params->find(J2K_Cblk) != params->end()) {
        std::stringstream ss;
        ss << "Cblk=" << params->at(J2K_Cblk);
        codestream.access_siz()->parse_string(ss.str().c_str());
      } else {
        codestream.access_siz()->parse_string("Cblk={64,64}");
      }

      if (params->find(J2K_Cuse_sop) != params->end()) {
        std::stringstream ss;
        ss << "Cuse_sop=" << params->at(J2K_Cuse_sop);
        codestream.access_siz()->parse_string(ss.str().c_str());
      } else {
        codestream.access_siz()->parse_string("Cuse_sop=yes");
      }

      if (params->find(J2K_rates) != params->end()) {
        std::string ratestr = params->at(J2K_rates);
        std::stringstream ss(ratestr);
        double temp;
        while (ss >> temp) rates.push_back(temp);// done! now array={102,330,3133,76531,451,000,12,44412}
      }
    } else {
      codestream.access_siz()->parse_string("Sprofile=PART2");
      if (mindim > 4096) {
        codestream.access_siz()->parse_string("Clayers=8");
        codestream.access_siz()->parse_string("Clevels=8");// resolution levels ***
      } else if (mindim > 2048) {
        codestream.access_siz()->parse_string("Clayers=5");
        codestream.access_siz()->parse_string("Clevels=5");// resolution levels ***
      } else if (mindim > 1024) {
        codestream.access_siz()->parse_string("Clayers=3");
        codestream.access_siz()->parse_string("Clevels=3");// resolution levels ***
      }
      codestream.access_siz()->parse_string("Corder=RPCL");
      codestream.access_siz()->parse_string("Cprecincts={256,256}");
      codestream.access_siz()->parse_string("Cblk={64,64}");
      codestream.access_siz()->parse_string("Cuse_sop=yes");
      codestream.access_siz()->parse_string("Cuse_eph=yes");
    }


    codestream.access_siz()->finalize_all();// Set up coding defaults

    jp2_dimensions jp2_family_dimensions = jpx_stream.access_dimensions();
    jp2_family_dimensions.init(&siz);// initalize dimension box

    //
    // get resolution tag from exif if existent: From kakadu:
    // Sets the vertical resolution in high resolution canvas grid points per metre.
    // If for_display is true, this is the desired display resolution; oherwise, it
    // is the capture resolution. There is no need to explicitly specify either of these
    // resolutions. If no resolution is specified the relevant box will not be written
    // in the JP2/JPX file, except when a non-unity aspect ratio is selected.
    // In the latter case, a desired display resolution box will be created, having a
    // default vertical display resolution of one grid-point per metre.
    //
    std::shared_ptr<Exif> exif = img->getExif();
    if (exif != nullptr) {
      float res_x = 72., res_y = 72.;
      int unit = 2;// RESUNIT_INCH
      if (exif->getValByKey("Exif.Image.XResolution", res_x)
          && exif->getValByKey("Exif.Image.YResolution", res_y)) {
        (void)exif->getValByKey("Exif.Image.ResolutionUnit", unit);
        switch (unit) {
        case 1:
          break;// RESUNIT_NONE
        case 2: {// RESUNIT_INCH
          res_x = res_x * 100. / 2.51;
          res_y = res_y * 100. / 2.51;
          break;
        }
        case 3: {// RESUNIT_CENTIMETER
          res_x = res_x * 100.;
          res_y = res_y * 100.;
        }
        }
        jp2_family_resolution.init(res_y / res_x);
        jp2_family_resolution.set_resolution(res_x, false);
      }
    }

    //
    // we need the essential metadata in order to preserve unsupported ICC profiles
    //
    Essentials es = img->essential_metadata();

    jp2_colour jp2_family_colour = jpx_layer.add_colour();
    std::shared_ptr<Icc> image_icc = img->getIcc();
    if (image_icc != nullptr) {
      PredefinedProfiles icc_type = image_icc->getProfileType();
      // An empty or unparseable ICC profile can't seed jp2_colour::init(kdu_byte*)
      // (it reads a profile header from the pointer), so both the empty-buffer
      // sites below and the exception handlers fall back to a colorspace guess
      // derived from the channel count.
      auto guess_colour_by_nc = [&]() {
        switch (img->getNc() - img->getEs().size()) {
        case 1: {
          jp2_family_colour.init(JP2_sLUM_SPACE);
          break;
        }
        case 3: {
          jp2_family_colour.init(JP2_sRGB_SPACE);
          break;
        }
        case 4: {
          jp2_family_colour.init(JP2_CMYK_SPACE);
          break;
        }
        }
      };
      try {
        switch (icc_type) {
        case icc_undefined: {
          std::vector<unsigned char> icc_buf = image_icc->iccBytes();
          if (!icc_buf.empty()) {
            jp2_family_colour.init(reinterpret_cast<kdu_byte *>(icc_buf.data()));
          } else {
            guess_colour_by_nc();
          }
          break;
        }
        case icc_unknown: {
          std::vector<unsigned char> icc_buf = image_icc->iccBytes();
          if (!icc_buf.empty()) {
            jp2_family_colour.init(reinterpret_cast<kdu_byte *>(icc_buf.data()));
          } else {
            guess_colour_by_nc();
          }
          break;
        }
        case icc_sRGB: {
          // TODO: this fixes the problem with grayscale JPEGs, but is it breaking anything else?
          if (img->getNc() - img->getEs().size() == 1) {
            jp2_family_colour.init(JP2_sLUM_SPACE);
          } else {
            jp2_family_colour.init(JP2_sRGB_SPACE);
          }
          break;
        }
        case icc_AdobeRGB: {
          std::vector<unsigned char> icc_buf = image_icc->iccBytes();
          if (!icc_buf.empty()) {
            jp2_family_colour.init(reinterpret_cast<kdu_byte *>(icc_buf.data()));
          } else {
            guess_colour_by_nc();
          }
          break;
        }
        case icc_RGB: {// TODO: DOES NOT WORK AS EXPECTED!!!!! Fallback below
          std::vector<unsigned char> icc_buf = image_icc->iccBytes();
          if (!icc_buf.empty()) {
            jp2_family_colour.init(reinterpret_cast<kdu_byte *>(icc_buf.data()));
          } else {
            guess_colour_by_nc();
          }
          break;
        }
        case icc_CMYK_standard: {
          jp2_family_colour.init(JP2_CMYK_SPACE);
          break;
        }
        case icc_GRAY_D50: {
          std::vector<unsigned char> icc_buf = image_icc->iccBytes();
          if (!icc_buf.empty()) {
            jp2_family_colour.init(reinterpret_cast<kdu_byte *>(icc_buf.data()));// TODO: DOES NOT WORK AS EXPECTED!!!!! Fallback below
          } else {
            guess_colour_by_nc();
          }
          break;
        }
        case icc_LUM_D65: {
          if (es.is_set()) es.fields_mut().use_icc = true;
          jp2_family_colour.init(JP2_sLUM_SPACE);// TODO: just a fallback
          break;
        }
        case icc_ROMM_GRAY: {
          if (es.is_set()) es.fields_mut().use_icc = true;
          jp2_family_colour.init(JP2_sLUM_SPACE);// TODO: just a fallback
          break;
        }
        case icc_LAB: {
          if (img->getBps() == 8) {
            jp2_family_colour.init(JP2_CIELab_SPACE, 100, 0, 8, 255, 128, 8, 255, 128, 8);
          } else {
            //
            // very strange parameters – was experimentally found
            //
            jp2_family_colour.init(JP2_CIELab_SPACE, 100, 0, 16, 255, 32767, 16, 255, 32767, 16);
          }
          break;
        };
        default: {
          std::vector<unsigned char> icc_buf = image_icc->iccBytes();
          if (!icc_buf.empty()) {
            jp2_family_colour.init(reinterpret_cast<kdu_byte *>(icc_buf.data()));
          } else {
            guess_colour_by_nc();
          }
        }
        }
      } catch (kdu_exception e) {
        if (es.is_set()) es.fields_mut().use_icc = true;
        guess_colour_by_nc();
      } catch (const SipiError &e) {
        // iccBytes() is not noexcept and throws SipiError on a
        // cmsSaveProfileToMem failure; treat it the same as an unusable profile.
        if (es.is_set()) es.fields_mut().use_icc = true;
        guess_colour_by_nc();
      }
    } else {
      switch (img->getNc() - img->getEs().size()) {
      case 1: {
        jp2_family_colour.init(JP2_sLUM_SPACE);
        break;
      }
      case 3: {
        jp2_family_colour.init(JP2_sRGB_SPACE);
        break;
      }
      case 4: {
        jp2_family_colour.init(JP2_CMYK_SPACE);
        break;
      }
      }
    }

    // Essentials carrier migrated from codestream-comment to UUID box (slot 4
    // after Signature → FTYP → jp2h, before jp2c) per ADR-0005 / DEV-6410.
    // Emission is gated on `es.is_set()` per ADR-0010 — the `convert
    // service-file` command stamps the packet on the SipiImage before write;
    // Access File JP2s arrive here with the packet unset and don't carry it.
    // The actual UUID-box write happens after `jpx_out.write_headers()`
    // (below), alongside the IPTC /
    // EXIF / XMP UUID boxes, so all four carrier boxes land in the same
    // post-headers / pre-codestream region.

    jp2_channels jp2_family_channels = jpx_layer.access_channels();
    const size_t es_size = img->getEs().size();
    const size_t color_nc = img->getNc() - es_size;
    jp2_family_channels.init(color_nc);
    for (int c = 0; c < color_nc; c++) { jp2_family_channels.set_colour_mapping(c, c); }
    if (es_size > 0) {
      if (es_size == 1) {
        for (int c = 0; c < color_nc; c++) { jp2_family_channels.set_opacity_mapping(c, color_nc); }
      } else if (es_size == color_nc) {
        for (int c = 0; c < color_nc; c++) { jp2_family_channels.set_opacity_mapping(c, color_nc + c); }
      }
    }

    jpx_out.write_headers();

    //
    // SIPI Essentials carrier (slot 4 — first UUID box after jp2h). The
    // packet's presence in memory is the emit gate per ADR-0010 — the
    // caller's responsibility (the `convert service-file` command, or
    // the Lua `file_role = "service-file"` keyword path in SipiLua.cpp)
    // is to ensure essential_metadata() carries the packet only when
    // the output is intentionally a Service File.
    //
    if (es.is_set()) { write_essentials_box(&jp2_ultimate_tgt, es.serialize()); }

    std::shared_ptr<Iptc> iptc = img->getIptc();
    if (iptc != nullptr) {
      std::vector<unsigned char> iptc_buf = iptc->iptcBytes();
      write_iptc_box(&jp2_ultimate_tgt, iptc_buf.data(), iptc_buf.size());
    }

    //
    // write EXIF here
    //
    if (exif != nullptr) {
      std::vector<unsigned char> exif_buf = exif->exifBytes();
      write_exif_box(&jp2_ultimate_tgt, exif_buf.data(), exif_buf.size());
    }

    //
    // write XMP data here
    //
    std::shared_ptr<Xmp> xmp = img->getXmp();
    if (xmp != nullptr) {
      std::string xmp_buf = xmp->xmpBytes();
      if (!xmp_buf.empty()) { write_xmp_box(&jp2_ultimate_tgt, xmp_buf.c_str()); }
    }

    jpx_out.write_headers();
    jpx_stream.open_stream();// return value unused; called for codestream-box init side-effect

    codestream.access_siz()->finalize_all();

    // Now compress the image in one hit, using `kdu_stripe_compressor'
    kdu_stripe_compressor compressor;
    compressor.mem_configure(&membroker);
    compressor.start(codestream,
      0,// num_layer_specs
      nullptr,// layer_sizes
      nullptr,// layer_slopes
      0,// min_slope_threshold
      true,// no_prediction
      is_reversible,// force_precise [YES, if reversible=yes]
      true,// record_layer_info_in_comment
      0.0,// size_tolerance
      img->getNc(),// num_components
      false,// want_fastest [NO]
      env_ref);

    std::vector<int> stripe_heights(img->getNc());
    byte *pixel_data = img->pixels_writable().data();
    if (img->getBps() == 16) {
      kdu_int16 *buf = (kdu_int16 *)pixel_data;
      std::vector<int> precisions(img->getNc(), static_cast<int>(img->getBps()));
      std::vector<char> is_signed(img->getNc(), 0);// vector<bool> does not work -> special treatment in C++
      for (size_t i = 0; i < img->getNc(); i++) { stripe_heights[i] = img->getNy(); }
      compressor.push_stripe(buf,
        stripe_heights.data(),
        nullptr,
        nullptr,
        nullptr,
        precisions.data(),
        reinterpret_cast<bool *>(is_signed.data()));
    } else if (img->getBps() == 8) {
      if (th == 0) th = img->getNy();
      const size_t nc = img->getNc();
      const size_t nx = img->getNx();
      const size_t ny = img->getNy();
      size_t stripe_start = 0;
      do {
        kdu_byte *buf = pixel_data + stripe_start * nc * nx;
        for (size_t i = 0; i < nc; i++) { stripe_heights[i] = th; }
        compressor.push_stripe(buf, stripe_heights.data());
        stripe_start += th;
      } while ((ny - stripe_start) >= th);
      if ((ny - stripe_start) > 0) {
        kdu_byte *buf = pixel_data + stripe_start * nc * nx;
        for (size_t i = 0; i < nc; i++) { stripe_heights[i] = ny - stripe_start; }
        compressor.push_stripe(buf, stripe_heights.data());
        stripe_start += ny - stripe_start;
      }
    } else {
      return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
        "Unsupported number of bits/sample for JPEG2000 write: bps=" + std::to_string(img->getBps())
          + " (only 8 and 16 supported)" + ", file=" + filepath + ", dimensions=" + std::to_string(img->getNx()) + "x"
          + std::to_string(img->getNy()) + ", channels=" + std::to_string(img->getNc()) });
    }
    compressor.finish(0, NULL, NULL, env_ref);
    // Finally, cleanup
    codestream.destroy();// All done: simple as that.
    output->close();// Not really necessary here.
    jpx_out.close();
    if (jp2_ultimate_tgt.exists()) { jp2_ultimate_tgt.close(); }
  } catch (kdu_exception e) {
    if (http && http->client_aborted) {
      return std::unexpected(
        SipiValueError{ ErrorCode::kClientAbort, "Client aborted HTTP response during JPEG2000 write" });
    }
    return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed,
      "Failed writing JPEG2000 image (Kakadu exception " + std::to_string(e) + ")" + ", file=" + filepath
        + ", dimensions=" + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy()) + ", channels="
        + std::to_string(img->getNc()) + ", bps=" + std::to_string(img->getBps())
        + ", colorspace=" + to_string(img->getPhoto()) });
  }
  return {};
}
}// namespace Sipi
