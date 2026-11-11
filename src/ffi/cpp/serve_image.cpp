/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ffi/decode_guard.h"
#include "ffi/serve_image.h"
#include "ffi/serve_timings.h"// PhaseTimer + decode-estimate capture, read back by the shell

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include "image/SipiImage.h"
#include "image/SipiImageError.h"
#include "image_processing/processing.h"
#include "cache/SipiCache.h"
#include "throttling/SipiMemoryBudget.h"
#include "throttling/SipiPeakMemory.h"
#include "format_handlers/output_sink.h"
#include "iiifparser/SipiDecodeDims.h"
#include "iiifparser/SipiIdentifier.h"
#include "iiifparser/SipiQualityFormat.h"
#include "iiifparser/SipiRegion.h"
#include "iiifparser/SipiRotation.h"
#include "iiifparser/SipiSize.h"
#include "logging/logger.h"
#include "metadata/exif.h"
#include "metadata/icc.h"
#include "metadata/iptc.h"
#include "metadata/xmp.h"
#include "observability/metrics.h"
#include "image/populate_from_image.h"
#include "util/Parsing.h"
#include "util/UrlDecode.h"

namespace Sipi::ffi {
namespace {

  using observability::get_file_size;
  using observability::ImageContext;
  using observability::Metrics;
  using observability::populate_from_image;

  constexpr const char *kCacheControl = "must-revalidate, post-check=0, pre-check=0";

  // Flattens a handled image error into the seam's SipiImageErrorReport and
  // reports it through `report_error` iff non-null — the seam's "NULL =
  // absent" idiom: a caller that passes no callback stays on a log-only path.
  // Never affects the caller's SipiStatus: this is purely a side channel for
  // error reporting.
  // Named *Report*, not *Error*, to avoid colliding with the
  // `Sipi::SipiImageError` exception type this same file catches.
  void report_image_error(SipiReportErrorFn report_error,
    void *report_ctx,
    const std::string &message,
    const std::string &phase,
    const ImageContext &ctx)
  {
    if (report_error == nullptr) { return; }
    SipiImageErrorReport err{};
    err.phase = phase.c_str();
    err.message = message.c_str();
    err.input_file = ctx.input_file.c_str();
    err.output_format = ctx.output_format.c_str();
    err.colorspace = ctx.colorspace.c_str();
    err.icc_profile_type = ctx.icc_profile_type.c_str();
    err.orientation = ctx.orientation.c_str();
    err.width = ctx.width;
    err.height = ctx.height;
    err.channels = ctx.channels;
    err.bps = ctx.bps;
    err.file_size_bytes = ctx.file_size_bytes;
    report_error(report_ctx, &err);
  }

  // The single FormatType ↔ Content-Type table for the served image formats.
  // `content_type_for` and `detect_in_format` are exact inverses and both source
  // from this one table, so the format/mime pairing lives in one place
  // (DUNE-005). The engine's own read-time mime classifier
  // (`SipiImage::getFileType`, SipiImage.cpp) keeps its separate sniff-alias
  // list — unifying the two is format-descriptor-table work, deferred until a
  // fifth format arrives (decision 2; see ARCH-MAP format_handlers entry).
  struct FormatMime
  {
    SipiQualityFormat::FormatType fmt;
    const char *mime;
  };
  constexpr std::array<FormatMime, 4> kFormatMimes = { {
    { SipiQualityFormat::TIF, "image/tiff" },
    { SipiQualityFormat::JPG, "image/jpeg" },
    { SipiQualityFormat::PNG, "image/png" },
    { SipiQualityFormat::JP2, "image/jp2" },
  } };

  // The Content-Type for an emitted IIIF format.
  const char *content_type_for(SipiQualityFormat::FormatType fmt)
  {
    for (const auto &e : kFormatMimes) {
      if (e.fmt == fmt) { return e.mime; }
    }
    return nullptr;
  }

  // The image-root mimetype → input format (matches the legacy sniff). `image/jpx`
  // is the JP2 alias `getFileMimetype` can return; fold it onto the canonical mime
  // before the table lookup.
  SipiQualityFormat::FormatType detect_in_format(const std::string &infile)
  {
    const std::string mime = shttps::Parsing::getFileMimetype(infile).first;
    const std::string canonical = (mime == "image/jpx") ? "image/jp2" : mime;
    for (const auto &e : kFormatMimes) {
      if (canonical == e.mime) { return e.fmt; }
    }
    return SipiQualityFormat::UNSUPPORTED;
  }

  // Reconstruct the typed iiifparser objects from the flat seam struct (the
  // params are typed so the seam carries no iiifparser class layout). The values
  // were produced by the C++ string parsers caller-side, so reconstruction cannot
  // fail — it only re-seats the parse-time fields.
  std::shared_ptr<SipiRegion> rebuild_region(const SipiIiifParams &p)
  {
    return std::make_shared<SipiRegion>(static_cast<SipiRegion::CoordType>(p.region_type),
      p.region[0],
      p.region[1],
      p.region[2],
      p.region[3]);
  }

  std::shared_ptr<SipiSize> rebuild_size(const SipiIiifParams &p)
  {
    return std::make_shared<SipiSize>(static_cast<SipiSize::SizeType>(p.size_type),
      p.size_upscaling != 0,
      p.size_percent,
      p.size_reduce,
      p.size_nx,
      p.size_ny);
  }

  // Builds the Canonical URL (IIIF spec form). Returns it twice-shaped: the
  // `<...>;rel="canonical"` Link-header value (`.first`) and the bare URL
  // (`.second`), the base of the Cache key (`build_image_response` appends the
  // size-restriction suffix, the one other part of the key). Engine-facing,
  // so it lives with the seam. The size resolves against the region extent, as
  // the served output does: two requests whose outputs differ must never get
  // the same Cache key.
  std::pair<std::string, std::string> build_canonical_url(size_t tmp_w,
    size_t tmp_h,
    const std::string &scheme,
    const std::string &host,
    const std::string &prefix,
    const std::string &identifier,
    const std::shared_ptr<SipiRegion> &region,
    const std::shared_ptr<SipiSize> &size,
    SipiRotation &rotation,
    SipiQualityFormat &quality_format,
    int pagenum,
    const std::string &cannonical_watermark)
  {
    static constexpr int canonical_len = 127;

    char canonical_region[canonical_len + 1];
    char canonical_size[canonical_len + 1];

    int tmp_r_x = 0, tmp_r_y = 0, tmp_red = 0;
    size_t tmp_r_w = 0, tmp_r_h = 0;
    bool tmp_ro = false;

    size_t extent_w = tmp_w, extent_h = tmp_h;
    if (region->getType() != SipiRegion::FULL) {
      region->crop_coords(tmp_w, tmp_h, tmp_r_x, tmp_r_y, extent_w, extent_h);
    }

    region->canonical(canonical_region, canonical_len);

    if (size->getType() != SipiSize::FULL) {
      try {
        size->get_size(extent_w, extent_h, tmp_r_w, tmp_r_h, tmp_red, tmp_ro);
      } catch (Sipi::SipiSizeError &err) {
        throw SipiError("SipiSize error!");
      }
    }

    size->canonical(canonical_size, canonical_len);
    float angle;
    const bool mirror = rotation.get_rotation(angle);
    char canonical_rotation[canonical_len + 1];

    if (mirror || (angle != 0.0)) {
      if ((angle - floorf(angle)) < 1.0e-6) {
        if (mirror) {
          (void)snprintf(canonical_rotation, canonical_len, "!%ld", std::lround(angle));
        } else {
          (void)snprintf(canonical_rotation, canonical_len, "%ld", std::lround(angle));
        }
      } else {
        if (mirror) {
          (void)snprintf(canonical_rotation, canonical_len, "!%1.1f", angle);
        } else {
          (void)snprintf(canonical_rotation, canonical_len, "%1.1f", angle);
        }
      }
    } else {
      (void)snprintf(canonical_rotation, canonical_len, "0");
    }

    constexpr unsigned canonical_header_len = 511;
    char canonical_header[canonical_header_len + 1];
    char ext[5];

    switch (quality_format.format()) {
    case SipiQualityFormat::JPG:
      ext[0] = 'j', ext[1] = 'p', ext[2] = 'g', ext[3] = '\0';
      break;
    case SipiQualityFormat::JP2:
      ext[0] = 'j', ext[1] = 'p', ext[2] = '2', ext[3] = '\0';
      break;
    case SipiQualityFormat::TIF:
      ext[0] = 't', ext[1] = 'i', ext[2] = 'f', ext[3] = '\0';
      break;
    case SipiQualityFormat::PNG:
      ext[0] = 'p', ext[1] = 'n', ext[2] = 'g', ext[3] = '\0';
      break;
    default:
      throw SipiError("Unsupported file format requested! Supported are .jpg, .jp2, .tif, .png");
    }

    std::string format;
    if (quality_format.quality() != SipiQualityFormat::DEFAULT) {
      switch (quality_format.quality()) {
      case SipiQualityFormat::COLOR:
        format = "/color.";
        break;
      case SipiQualityFormat::GRAY:
        format = "/gray.";
        break;
      case SipiQualityFormat::BITONAL:
        format = "/bitonal.";
        break;
      default:
        format = "/default.";
      }
    } else {
      format = "/default.";
    }

    std::string fullid = identifier;
    if (pagenum > 0) { fullid += "@" + std::to_string(pagenum); }
    (void)snprintf(canonical_header,
      canonical_header_len,
      "<%s://%s/%s/%s/%s/%s/%s/default.%s/%s>;rel=\"canonical\"",
      scheme.c_str(),
      host.c_str(),
      prefix.c_str(),
      fullid.c_str(),
      canonical_region,
      canonical_size,
      canonical_rotation,
      ext,
      cannonical_watermark.c_str());

    std::string canonical = host + "/" + prefix + "/" + fullid + "/" + std::string(canonical_region) + "/"
                            + std::string(canonical_size) + "/" + std::string(canonical_rotation) + format
                            + std::string{ ext } + "/" + std::string{ cannonical_watermark };

    return std::make_pair(std::string(canonical_header), canonical);
  }

  // The decoded image + the encode job, captured for the streamed-body tail.
  // produce() runs ONLY the encode (the rarely-failing step): the decode +
  // transforms already ran in build_image_response, before the response committed.
  class ImageEncodeProducer : public StreamProducer
  {
  public:
    ImageEncodeProducer(SipiImage &&img,
      SipiQualityFormat::FormatType format,
      int jpeg_quality,
      SipiCache *cache,
      std::string cachefile,
      std::string infile,
      std::string cache_key,
      std::string request_uri,
      SipiImgInfo info,
      std::optional<MemoryBudgetGuard> budget_guard,
      SipiReportErrorFn report_error,
      void *report_ctx)
      : budget_guard_(std::move(budget_guard)), img_(std::move(img)), format_(format), jpeg_quality_(jpeg_quality),
        cache_(cache), cachefile_(std::move(cachefile)), infile_(std::move(infile)), cache_key_(std::move(cache_key)),
        request_uri_(std::move(request_uri)), info_(info), report_error_(report_error), report_ctx_(report_ctx)
    {}

    int produce(const StreamSink &sink) override
    {
      // Bridge the StreamSink to the format handlers' C-ABI write callback. The
      // free thunk + struct ctx carry both the sink (the socket) and a running
      // byte count for the DEV-6660 cache-integrity check.
      ThunkCtx tctx{ &sink, 0 };
      const CallbackSink socket{ &ImageEncodeProducer::sink_thunk, &tctx };

      const bool caching = cache_ != nullptr && !cachefile_.empty();
      const OutputSink out = caching
                               ? OutputSink{ TeeSink{ { OutputSink{ socket }, OutputSink{ FilePath{ cachefile_ } } } } }
                               : OutputSink{ socket };

      Result<void> r;
      try {
        // Spans the encode AND the streamed write to the sink, so a slow client's
        // back-pressure counts toward this phase's duration (see SipiServeTimings).
        PhaseTimer phase_timer(SIPI_PHASE_ENCODE);
        switch (format_) {
        case SipiQualityFormat::JPG: {
          SipiCompressionParams qp = { { JPEG_QUALITY, std::to_string(jpeg_quality_) } };
          r = img_.write("jpg", out, &qp);
          break;
        }
        case SipiQualityFormat::JP2:
          r = img_.write("jpx", out);
          break;
        case SipiQualityFormat::TIF:
          r = img_.write("tif", out);
          break;
        case SipiQualityFormat::PNG:
          r = img_.write("png", out);
          break;
        default:
          break;
        }
      } catch (SipiError &err) {
        if (caching) { ::unlink(cachefile_.c_str()); }
        capture_write_error(err.to_string());
        log_err("GET %s: error writing image: %s", request_uri_.c_str(), err.to_string().c_str());
        return 1;
      } catch (SipiImageError &err) {
        if (caching) { ::unlink(cachefile_.c_str()); }
        capture_write_error(err.what());
        log_err("GET %s: error writing image: %s", request_uri_.c_str(), err.what());
        return 1;
      }

      if (!r) {
        const auto &err = r.error();
        if (caching) { ::unlink(cachefile_.c_str()); }
        if (err.code() == ErrorCode::kClientAbort) {
          // Client closed the socket mid-response (Traefik 499). Not a server
          // error: no Sentry.
          log_info("Client aborted HTTP response for %s", request_uri_.c_str());
          Metrics::instance().client_disconnected_total.Increment();
          return 1;
        }
        if (policy_for(err.code()).sentry_policy != SentryPolicy::kSkip) { capture_write_error(err.diagnostic_message()); }
        log_err("GET %s: error writing image: %s", request_uri_.c_str(), err.diagnostic_message().c_str());
        return 1;
      }

      if (caching) { finalize_cache(tctx.bytes); }
      return 0;
    }

  private:
    struct ThunkCtx
    {
      const StreamSink *sink;
      std::uint64_t bytes;
    };

    static int sink_thunk(void *ctx, const std::uint8_t *data, std::size_t len)
    {
      auto *t = static_cast<ThunkCtx *>(ctx);
      t->bytes += len;
      return t->sink->write(data, len);
    }

    void capture_write_error(const std::string &message) const
    {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile_;
      sentry_ctx.file_size_bytes = get_file_size(infile_);
      sentry_ctx.output_format = format_type_to_string(format_);
      populate_from_image(sentry_ctx, img_);
      report_image_error(report_error_, report_ctx_, message, "write", sentry_ctx);
    }

    // Commit the cache file iff it is intact (DEV-6660): the FilePath leaf is
    // best-effort, so a short/failed cache write must not register a truncated
    // file. Also drops over-size cache files (the legacy post-write check).
    void finalize_cache(std::uint64_t streamed_bytes)
    {
      struct stat st{};
      if (stat(cachefile_.c_str(), &st) != 0) {
        Metrics::instance().cache_skips_total.Increment();
        return;
      }
      const auto written = static_cast<std::uint64_t>(st.st_size);
      if (written == 0 || written != streamed_bytes) {
        // Truncated or empty: the cache write did not keep up with the socket.
        log_warn("Cache file %s incomplete (%llu of %llu bytes), discarding",
          cachefile_.c_str(),
          static_cast<unsigned long long>(written),
          static_cast<unsigned long long>(streamed_bytes));
        ::unlink(cachefile_.c_str());
        Metrics::instance().cache_skips_total.Increment();
        return;
      }
      const long long max_cs = cache_->getMaxCacheSize();
      if (max_cs > 0 && st.st_size > max_cs) {
        log_warn("Converted file %s (%lld bytes) exceeds cache_size (%lld bytes), removing",
          cachefile_.c_str(),
          static_cast<long long>(st.st_size),
          max_cs);
        ::unlink(cachefile_.c_str());
        Metrics::instance().cache_skips_total.Increment();
        return;
      }
      cache_->add(infile_,
        cache_key_,
        cachefile_,
        info_.width,
        info_.height,
        info_.tile_width,
        info_.tile_height,
        info_.clevels,
        info_.numpages);
    }

    // The decode-memory reservation, held (not read) so the budget stays
    // accounted for across the streamed encode and is released on destruction.
    // Declared first so it outlives img_: the image buffer frees, *then* the
    // budget is released, keeping the in-flight accounting honest at teardown.
    std::optional<MemoryBudgetGuard> budget_guard_;
    SipiImage img_;
    SipiQualityFormat::FormatType format_;
    int jpeg_quality_;
    SipiCache *cache_;
    std::string cachefile_;
    std::string infile_;
    std::string cache_key_;
    std::string request_uri_;
    SipiImgInfo info_;
    // Safe to hold past construction only because `produce()` runs
    // synchronously within the same `sipi_serve_image` call that constructed
    // this producer (the Rust caller is blocked on that call the whole time,
    // per the seam's synchronous contract) — so `report_ctx_` (the Rust-owned
    // request-URI C string, per routes.rs) is still alive whenever `produce()`
    // reads it. Never retain either past this object's lifetime.
    SipiReportErrorFn report_error_;
    void *report_ctx_;
  };

  std::string str_or_empty(const char *s) { return s != nullptr ? std::string(s) : std::string(); }

  // Cuts `region` out of the restricted rendering and scales the crop to
  // (out_w, out_h). A crop already at the target size is not scaled: the scale
  // operators refuse a 1-sample axis, and a tile at size `max` must be a pure
  // crop so that tiles stitch to `full/max` exactly. `build_image_response`
  // refuses, before the HEAD return, a crop this would have to scale from or
  // to a 1-sample axis.
  [[nodiscard]] Result<void> crop_restricted(SipiImage &img,
    const std::shared_ptr<SipiRegion> &region,
    size_t out_w,
    size_t out_h,
    ScalingMethod method)
  {
    if (auto r = processing::crop(img, region); !r) { return r; }
    if (img.getNx() == out_w && img.getNy() == out_h) { return {}; }
    return processing::scale_with(img, out_w, out_h, method);
  }

  // A size-restricted output is its own image of the delivered size: the
  // embedded size fields must state that size, not the original's, and the
  // embedded previews (pixels that can exceed the restriction) go. Nothing
  // else is removed: rights and other descriptive metadata are always
  // returned (CONTEXT.md, restrict). False if a size field kept
  // its native value, or an XMP preview or the EXIF's Photoshop resources
  // could not be rewritten.
  [[nodiscard]] bool rewrite_restricted_metadata(SipiImage &img)
  {
    bool ok = true;
    if (auto exif = img.getExif(); exif != nullptr) {
      exif->erase_previews();
      ok = exif->set_pixel_dimensions(img.getNx(), img.getNy());
      ok = exif->restrict_image_resources(img.getNx(), img.getNy()) && ok;
    }
    if (auto xmp = img.getXmp(); xmp != nullptr) {
      ok = xmp->erase_thumbnails() && ok;
      xmp->set_pixel_dimensions(img.getNx(), img.getNy());
    }
    if (auto iptc = img.getIptc(); iptc != nullptr) { iptc->erase_preview(); }
    return ok;
  }

  // A full-file body for the passthrough / cache-hit paths. Opened here
  // (O_NOFOLLOW + a regular-file check via fstat) rather than merely stat'd,
  // so a planted symlink at `path` — a forged cache index record pointing at
  // e.g. the server config with the JWT secret (S2-26) — is rejected before
  // any bytes are ever served, instead of followed. This is also why a file
  // that vanished after the earlier checks is a clean error rather than a
  // 200 with a wrong length (get_file_size returns 0 on a failed stat, which
  // would underflow send_file's inclusive byte range). A 0-byte file
  // becomes an EmptyBody for the same reason.
  std::expected<Body, SipiStatus> full_file_body(const std::string &path)
  {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { return std::unexpected(SipiStatus::InternalError); }
    struct stat st{};
    const bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    ::close(fd);
    if (!ok) { return std::unexpected(SipiStatus::InternalError); }
    const auto size = static_cast<std::uint64_t>(st.st_size);
    if (size == 0) { return Body{ EmptyBody{} }; }
    return Body{ FileBody{ path, 0, size } };
  }

}// namespace

SipiStatus status_for(const SipiValueError &err)
{
  switch (policy_for(err.code()).http_status_class) {
  case HttpStatusClass::kInternalError:
    return SipiStatus::InternalError;
  case HttpStatusClass::kClientError:
    return SipiStatus::BadRequest;
  }
}

void report_value_error(SipiReportErrorFn report_error,
  void *report_ctx,
  const SipiValueError &err,
  const std::string &phase,
  const observability::ImageContext &ctx)
{
  // kSkip marks a failure that is not a server-side fault (e.g. a client
  // abort mid-response): no callback, no Sentry.
  if (policy_for(err.code()).sentry_policy == SentryPolicy::kSkip) { return; }
  report_image_error(report_error, report_ctx, err.diagnostic_message(), phase, ctx);
}

std::expected<ServeResponse, SipiStatus>
  build_image_response(const SipiServeRequest &req, const EngineContext &eng, const std::function<bool()> &cancelled)
{
  const std::string infile = str_or_empty(req.resolved_path);
  const std::string uri = str_or_empty(req.request_uri);

  // Reconstruct the typed IIIF params from the flat seam (caller already
  // validated the source strings, so this cannot throw).
  auto region = rebuild_region(req.params);
  auto size = rebuild_size(req.params);
  SipiRotation rotation(req.params.rotation, req.params.rotation_mirror != 0);
  SipiQualityFormat quality_format(
    static_cast<SipiQualityFormat::QualityType>(req.params.quality_type),
    static_cast<SipiQualityFormat::FormatType>(req.params.format_type));

  const SipiIdentifier sid(shttps::urldecode(str_or_empty(req.identifier)));

  const std::string watermark = str_or_empty(req.watermark_path);
  const size_t rest_w = req.restricted_width;
  const size_t rest_h = req.restricted_height;
  if ((rest_w == 0) != (rest_h == 0)) {
    log_warn("GET %s: half-set restricted dims", uri.c_str());
    return std::unexpected(SipiStatus::InternalError);
  }
  // The shell refuses a restriction below 2 px per axis (routes.rs
  // `restriction`); the scale operators cannot produce one.
  if (rest_w != 0 && (rest_w < 2 || rest_h < 2)) {
    log_warn("GET %s: restricted dims below 2 px", uri.c_str());
    return std::unexpected(SipiStatus::InternalError);
  }

  const SipiQualityFormat::FormatType in_format = detect_in_format(infile);

  if (access(infile.c_str(), R_OK) != 0) { return std::unexpected(SipiStatus::NotFound); }

  float angle = 0.F;
  const bool mirror = rotation.get_rotation(angle);

  // Image shape (no full decode) — needed for size math, the canonical URL, the
  // memory estimate, and the cache entry.
  SipiImgInfo info;
  {
    PhaseTimer phase_timer(SIPI_PHASE_SHAPE);
    auto deadline_result = run_with_deadline(std::chrono::milliseconds(eng.decode_timeout_ms), [infile] {
      SipiImage probe;
      return probe.read_shape(infile);
    });
    if (!deadline_result) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      report_image_error(
        req.report_error, req.report_ctx, "JP2 decode deadline exceeded (read_shape)", "read", sentry_ctx);
      return std::unexpected(SipiStatus::InternalError);
    }
    auto &shape = *deadline_result;
    if (!shape) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      report_value_error(req.report_error, req.report_ctx, shape.error(), "read", sentry_ctx);
      return std::unexpected(status_for(shape.error()));
    }
    // read_shape() converts an all-handlers-refused probe into an error value
    // before returning, so a successfully unwrapped SipiImgInfo is always valid.
    info = *shape;
  }

  const size_t img_w = info.width;
  const size_t img_h = info.height;

  // The shell probed the shape before this read; a file replaced in between
  // must not widen the restricted view.
  if (rest_w > img_w || rest_h > img_h) {
    log_warn("GET %s: restricted dims exceed the image", uri.c_str());
    return std::unexpected(SipiStatus::InternalError);
  }

  // A size-restricted image is served as its own image of (RW, RH): region and
  // size resolve in that space, every output is cut from one rendering at
  // (RW, RH), and nothing larger than (RW, RH) before rotation is served, so
  // no request reveals more than `full/max` (ADR-0028). Checked before the
  // HEAD return and the cache lookup, so HEAD and GET agree.
  const bool size_restricted = rest_w != 0;
  size_t out_w{ 0 }, out_h{ 0 };
  if (size_restricted) {
    int crop_x{ 0 }, crop_y{ 0 };
    size_t crop_w{ 0 }, crop_h{ 0 };
    int out_reduce{ 0 };
    bool out_redonly{ false };
    try {
      region->crop_coords(rest_w, rest_h, crop_x, crop_y, crop_w, crop_h);
      size->get_size(crop_w, crop_h, out_w, out_h, out_reduce, out_redonly);
    } catch (Sipi::SipiSizeError &) {
      return std::unexpected(SipiStatus::BadRequest);
    } catch (Sipi::SipiError &) {
      return std::unexpected(SipiStatus::BadRequest);
    }
    if (out_w > rest_w || out_h > rest_h) { return std::unexpected(SipiStatus::BadRequest); }
    // A crop that needs scaling must have at least 2 samples per axis on both
    // sides: the scale operators refuse anything less, after the HEAD return.
    const bool scales = out_w != crop_w || out_h != crop_h;
    if (scales && (crop_w < 2 || crop_h < 2 || out_w < 2 || out_h < 2)) {
      return std::unexpected(SipiStatus::BadRequest);
    }
  } else {
    size_t tmp_r_w{ 0 }, tmp_r_h{ 0 };
    int tmp_red{ 0 };
    bool tmp_ro{ false };
    try {
      size->get_size(img_w, img_h, tmp_r_w, tmp_r_h, tmp_red, tmp_ro);
    } catch (Sipi::SipiSizeError &) {
      return std::unexpected(SipiStatus::BadRequest);
    } catch (Sipi::SipiError &) {
      return std::unexpected(SipiStatus::BadRequest);
    }
  }

  // Canonical URL (Link header + cache key). The Link header honours
  // X-Forwarded-Proto (SIPI serves plain HTTP behind Traefik); the cache key
  // stays scheme-free so an http and an https request share one cache entry.
  const std::string cannonical_watermark = watermark.empty() ? "0" : "1";
  const char *forwarded_proto = req.forwarded_proto;
  const std::string scheme =
    (forwarded_proto != nullptr && *forwarded_proto != '\0') ? std::string(forwarded_proto) : std::string("http");
  std::pair<std::string, std::string> canonical_info;
  try {
    canonical_info = build_canonical_url(size_restricted ? rest_w : img_w,
      size_restricted ? rest_h : img_h,
      scheme,
      str_or_empty(req.forwarded_host),
      str_or_empty(req.prefix),
      sid.getIdentifier(),
      region,
      size,
      rotation,
      quality_format,
      sid.getPage(),
      cannonical_watermark);
  } catch (Sipi::SipiError &) {
    return std::unexpected(SipiStatus::BadRequest);
  }
  const std::string canonical_header = canonical_info.first;
  // The bare Canonical URL plus its watermark marker is the Cache key;
  // `SipiCache` keys its table by this string. A size restriction appends
  // `/r<RW>x<RH>` to the key only, never to the Link: the same canonical
  // coordinates under two restrictions, or with and without one, are
  // different images and must never share a cache entry.
  std::string cache_key = canonical_info.second;
  if (size_restricted) { cache_key += "/r" + std::to_string(rest_w) + "x" + std::to_string(rest_h); }
  const char *content_type = content_type_for(quality_format.format());

  auto base_headers = [&] {
    std::vector<Header> h;
    h.emplace_back("Cache-Control", kCacheControl);
    // IIIF Image API 3.0 profileLinkHeader (advertised in info.json's extraFeatures):
    // fold the profile Link into the canonical Link's value (one header) — the Rust
    // response sink is last-write-wins per header name, so two separate "Link"
    // entries would drop one.
    h.emplace_back("Link", canonical_header + R"(, <http://iiif.io/api/image/3/level2.json>;rel="profile")");
    if (content_type != nullptr) { h.emplace_back("Content-Type", content_type); }
    return h;
  };

  // HEAD: headers only — no decode, no cache write (also closes the legacy
  // zero-byte-HEAD cache bug, DEV-6660).
  if (req.is_head != 0) {
    ServeResponse out;
    out.http_status = 200;
    out.headers = base_headers();
    out.body = EmptyBody{};
    return out;
  }

  // Direct passthrough: the request maps 1:1 onto the source file. Never for a
  // size-restricted image: the file is the unrestricted original.
  if (!size_restricted && region->getType() == SipiRegion::FULL && size->getType() == SipiSize::FULL
      && angle == 0.0 && !mirror && watermark.empty() && quality_format.format() == in_format
      && quality_format.quality() == SipiQualityFormat::DEFAULT) {
    auto body = full_file_body(infile);
    if (!body) { return std::unexpected(body.error()); }
    ServeResponse out;
    out.http_status = 200;
    out.headers = base_headers();
    out.body = std::move(*body);
    return out;
  }

  // Cache hit (never for watermarked output): pin the file, serve it, unpin when
  // the body has been delivered.
  if (eng.cache != nullptr) {
    const std::string cachefile = eng.cache->check(infile, cache_key, true);
    if (!cachefile.empty()) {
      log_debug("Using cachefile %s", cachefile.c_str());
      SipiCache *cache = eng.cache;
      auto body = full_file_body(cachefile);
      if (!body) {
        cache->deblock(cachefile);// pinned by check(); release it before bailing
        return std::unexpected(body.error());
      }
      ServeResponse out;
      out.http_status = 200;
      out.headers = base_headers();
      out.body = std::move(*body);
      out.on_complete = [cache, cachefile] { cache->deblock(cachefile); };
      return out;
    }
  }

  // Estimated peak decode memory for this serve. Recorded for every decode —
  // handed back over the seam accumulator into the shell's OTLP histogram —
  // independently of whether the budget is enforced: the estimate describes the
  // request, not the budget feature.
  //
  // A size-restricted request decodes the whole image at (RW, RH) whatever its
  // region, so a small region cannot hide a full decode from the budget.
  const auto decode_region = size_restricted ? std::make_shared<SipiRegion>() : region;
  const auto decode_size =
    size_restricted ? std::make_shared<SipiSize>(SipiSize::PIXELS_XY, false, 0.F, 0, rest_w, rest_h) : size;
  auto ddims = compute_decode_dims(img_w, img_h, info.clevels, decode_region, decode_size);
  if (size_restricted) {
    ddims.out_w = out_w;
    ddims.out_h = out_h;
  }
  const bool needs_icc = quality_format.quality() == SipiQualityFormat::COLOR
                         || quality_format.quality() == SipiQualityFormat::GRAY;
  const size_t estimated = estimate_peak_memory(
    ddims.width, ddims.height, ddims.out_w, ddims.out_h, info.nc, info.bps, static_cast<double>(angle), needs_icc);

  auto &metrics = Metrics::instance();
  serve_timings_set_decode_estimate(static_cast<std::uint64_t>(estimated));

  // The full-lane memory budget accounts only full-lane decodes: those whose
  // estimated peak memory reaches the large-decode threshold. Tile decodes
  // (below the threshold) bypass the budget entirely and are never charged, so a
  // tile is never rejected for full-lane memory pressure. `acquire_full_lane_budget`
  // (decode_guard.h) is the single site that also gates the Lua SipiImage.new() path.
  SipiMemoryBudget *mb = eng.memory_budget;
  auto acquisition = acquire_full_lane_budget(mb, eng.large_decode_threshold_bytes, estimated, [mb] {
    Metrics::instance().decode_memory_used_bytes.Set(static_cast<double>(mb->used()));
  });

  if (acquisition.consulted) {
    const auto &result = acquisition.result;
    metrics.decode_memory_used_bytes.Set(static_cast<double>(result.used));

    if (result.allowed && !result.over_budget) {
      metrics.decode_memory_acquired.Increment();
    } else if (result.allowed && result.over_budget) {
      // BASIC: admit, but shadow-count the would-be rejection — distinguishing
      // the permanently-unservable (would-be 413) case from the transient one.
      if (result.exceeds_budget_alone) {
        metrics.decode_memory_shadow_too_large_total.Increment();
      } else {
        metrics.decode_memory_shadow_rejected.Increment();
      }
      log_warn("Full-lane memory budget over limit (basic): %zu / %zu bytes for %s",
        result.used, result.budget, uri.c_str());
    } else if (result.exceeds_budget_alone) {
      // ADVANCED, permanently unservable: a single request whose estimate alone
      // exceeds the full-lane budget can never succeed — 413, no Retry-After.
      metrics.decode_memory_too_large_total.Increment();
      log_warn("Request estimate %zu exceeds full-lane budget %zu; rejecting (413): %s",
        estimated, result.budget, uri.c_str());
      ServeResponse out;
      out.http_status = 413;
      out.body = EmptyBody{};
      return out;
    } else {
      // ADVANCED, transient: the full lane is currently exhausted — 503 + Retry-After.
      metrics.decode_memory_rejected.Increment();
      log_warn("Full-lane memory budget exhausted (advanced): %zu / %zu bytes, rejecting %s",
        result.used, result.budget, uri.c_str());
      ServeResponse out;
      out.http_status = 503;
      out.headers.emplace_back("Retry-After", "5");
      out.body = EmptyBody{};
      return out;
    }

    if (result.used > result.budget - result.budget / 5) { metrics.decode_memory_near_limit_total.Increment(); }
  }

  std::optional<MemoryBudgetGuard> budget_guard = std::move(acquisition.guard);

  if (cancelled()) {
    Metrics::instance().client_disconnected_total.Increment();
    return std::unexpected(SipiStatus::ClientGone);
  }

  // The producer's own SipiImage / decode result, carried out by value so the
  // deadline helper's timeout path never touches this stack (see
  // run_with_deadline's lifetime-safety note).
  struct DecodeOutcome
  {
    SipiImage img;
    Result<void> status;
  };

  SipiImage img;
  try {
    PhaseTimer phase_timer(SIPI_PHASE_DECODE);
    const bool force_bps_8 = quality_format.format() == SipiQualityFormat::JPG;
    // `budget_guard` is moved into the decode's worker closure (decode_guard.h):
    // on success it comes back armed below, still charging the budget through
    // the subsequent encode; on timeout it stays with the detached worker.
    auto deadline_result = run_guarded_decode(std::chrono::milliseconds(eng.decode_timeout_ms),
      std::move(budget_guard),
      [infile,
        decode_region,
        decode_size,
        region,
        size_restricted,
        out_w,
        out_h,
        force_bps_8,
        scaling_quality = eng.scaling_quality,
        method = scaling_method_for(in_format, eng.scaling_quality)] {
        SipiImage decoded;
        auto status = decoded.read(infile, decode_region, decode_size, force_bps_8, scaling_quality);
        if (status && size_restricted) { status = crop_restricted(decoded, region, out_w, out_h, method); }
        return DecodeOutcome{ std::move(decoded), std::move(status) };
      });
    if (!deadline_result) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      report_image_error(req.report_error, req.report_ctx, "JP2 decode deadline exceeded (read)", "read", sentry_ctx);
      return std::unexpected(SipiStatus::InternalError);
    }
    budget_guard = std::move(deadline_result->guard);
    img = std::move(deadline_result->value.img);
    if (auto &r = deadline_result->value.status; !r) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      populate_from_image(sentry_ctx, img);
      report_value_error(req.report_error, req.report_ctx, r.error(), "read", sentry_ctx);
      return std::unexpected(status_for(r.error()));
    }
  } catch (const std::bad_alloc &) {
    Metrics::instance().memory_alloc_failures_total.Increment();
    ImageContext sentry_ctx;
    sentry_ctx.input_file = infile;
    sentry_ctx.file_size_bytes = get_file_size(infile);
    report_image_error(req.report_error, req.report_ctx, "std::bad_alloc during image read", "read", sentry_ctx);
    return std::unexpected(SipiStatus::InternalError);
  } catch (const SipiImageError &err) {
    // Covers the allocation-guard and codec-boundary throws that stay exception-based
    // (checked_buf_size_or_throw, memTiffOpen, Kakadu), not decode failures.
    ImageContext sentry_ctx;
    sentry_ctx.input_file = infile;
    sentry_ctx.file_size_bytes = get_file_size(infile);
    populate_from_image(sentry_ctx, img);
    report_image_error(req.report_error, req.report_ctx, err.to_string(), "read", sentry_ctx);
    return std::unexpected(SipiStatus::InternalError);
  } catch (const SipiSizeError &) {
    return std::unexpected(SipiStatus::BadRequest);
  }

  if (mirror || angle != 0.0) {
    if (cancelled()) {
      Metrics::instance().client_disconnected_total.Increment();
      return std::unexpected(SipiStatus::ClientGone);
    }
    try {
      PhaseTimer phase_timer(SIPI_PHASE_ROTATE);
      if (auto r = processing::rotate(img, angle, mirror); !r) {
        ImageContext sentry_ctx;
        sentry_ctx.input_file = infile;
        sentry_ctx.file_size_bytes = get_file_size(infile);
        populate_from_image(sentry_ctx, img);
        report_value_error(req.report_error, req.report_ctx, r.error(), "convert", sentry_ctx);
        return std::unexpected(status_for(r.error()));
      }
    } catch (const std::bad_alloc &) {
      Metrics::instance().memory_alloc_failures_total.Increment();
      return std::unexpected(SipiStatus::InternalError);
    } catch (Sipi::SipiError &err) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      populate_from_image(sentry_ctx, img);
      report_image_error(req.report_error, req.report_ctx, err.to_string(), "convert", sentry_ctx);
      return std::unexpected(SipiStatus::InternalError);
    }
  }

  if (quality_format.quality() != SipiQualityFormat::DEFAULT) {
    if (cancelled()) {
      Metrics::instance().client_disconnected_total.Increment();
      return std::unexpected(SipiStatus::ClientGone);
    }
    try {
      PhaseTimer phase_timer(SIPI_PHASE_QUALITY);
      Result<void> r;
      switch (quality_format.quality()) {
      case SipiQualityFormat::COLOR:
        r = processing::convertToIcc(img, Icc(icc_sRGB), 8);
        break;
      case SipiQualityFormat::GRAY:
        r = processing::convertToIcc(img, Icc(icc_GRAY_D50), 8);
        break;
      case SipiQualityFormat::BITONAL:
        r = processing::toBitonal(img);
        break;
      default:
        return std::unexpected(SipiStatus::BadRequest);
      }
      if (!r) {
        ImageContext sentry_ctx;
        sentry_ctx.input_file = infile;
        sentry_ctx.file_size_bytes = get_file_size(infile);
        populate_from_image(sentry_ctx, img);
        report_value_error(req.report_error, req.report_ctx, r.error(), "convert", sentry_ctx);
        return std::unexpected(status_for(r.error()));
      }
    } catch (const std::bad_alloc &) {
      Metrics::instance().memory_alloc_failures_total.Increment();
      return std::unexpected(SipiStatus::InternalError);
    } catch (Sipi::SipiError &err) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      populate_from_image(sentry_ctx, img);
      report_image_error(req.report_error, req.report_ctx, err.to_string(), "convert", sentry_ctx);
      return std::unexpected(SipiStatus::InternalError);
    }
  }

  if (!watermark.empty()) {
    if (cancelled()) {
      Metrics::instance().client_disconnected_total.Increment();
      return std::unexpected(SipiStatus::ClientGone);
    }
    try {
      PhaseTimer phase_timer(SIPI_PHASE_WATERMARK);
      if (auto r = processing::add_watermark(img, watermark); !r) {
        ImageContext sentry_ctx;
        sentry_ctx.input_file = infile;
        sentry_ctx.file_size_bytes = get_file_size(infile);
        populate_from_image(sentry_ctx, img);
        report_value_error(req.report_error, req.report_ctx, r.error(), "convert", sentry_ctx);
        return std::unexpected(status_for(r.error()));
      }
    } catch (Sipi::SipiError &err) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      populate_from_image(sentry_ctx, img);
      report_image_error(req.report_error, req.report_ctx, err.to_string(), "convert", sentry_ctx);
      return std::unexpected(SipiStatus::InternalError);
    } catch (std::exception &err) {
      ImageContext sentry_ctx;
      sentry_ctx.input_file = infile;
      sentry_ctx.file_size_bytes = get_file_size(infile);
      populate_from_image(sentry_ctx, img);
      report_image_error(req.report_error, req.report_ctx, err.what(), "convert", sentry_ctx);
      return std::unexpected(SipiStatus::InternalError);
    }
    log_info("GET %s: adding watermark", uri.c_str());
  }

  if (cancelled()) {
    Metrics::instance().client_disconnected_total.Increment();
    return std::unexpected(SipiStatus::ClientGone);
  }

  if (size_restricted) {
    bool rewritten = false;
    // Exiv2 can throw while editing the EXIF data.
    try {
      rewritten = rewrite_restricted_metadata(img);
    } catch (const std::exception &err) {
      log_warn("GET %s: restricted metadata rewrite failed: %s", uri.c_str(), err.what());
    }
    // A native size field or an embedded preview must not be served.
    if (!rewritten) {
      log_warn("GET %s: a native size field or preview could not be removed", uri.c_str());
      return std::unexpected(SipiStatus::InternalError);
    }
  }

  // Cache file: probe writability now (a 500 here is still pre-commit), then let
  // the producer's TeeSink fill it during the encode.
  std::string cachefile;
  if (eng.cache != nullptr) {
    cachefile = eng.cache->getNewCacheFileName();
    std::ofstream probe(cachefile, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);
    if (probe.fail()) { return std::unexpected(SipiStatus::InternalError); }
  }

  if (content_type == nullptr) { return std::unexpected(SipiStatus::BadRequest); }

  ServeResponse out;
  out.http_status = 200;
  out.headers = base_headers();
  out.body = StreamBody{ std::make_unique<ImageEncodeProducer>(std::move(img),
    quality_format.format(),
    eng.jpeg_quality,
    eng.cache,
    std::move(cachefile),
    infile,
    cache_key,
    uri,
    info,
    std::move(budget_guard),
    req.report_error,
    req.report_ctx) };
  return out;
}

}// namespace Sipi::ffi
