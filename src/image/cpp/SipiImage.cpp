/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <cassert>

#include <sys/stat.h>

#include "util/Parsing.h"

#include "logging/logger.h"
#include "SipiImage.h"
#include "SipiImageError.h"
#include "error/SipiValueError.h"
#include "observability/metrics.h"
#include "observability/profiling.h"
#include "util/checked_arith.h"
#include "util/Hash.h"

namespace Sipi {

namespace {

// Every pixel-buffer allocation in this file funnels through here: reject
// (throw) the moment `nx * ny * nc * elem` overflows `size_t`, rather than
// letting a wrapped size feed a too-small `std::vector`/`unique_ptr<[]>`
// allocation that a later copy then overruns.
size_t checked_buf_size_or_throw(size_t nx_, size_t ny_, size_t nc_, size_t elem_)
{
  if (const auto sz = checked_buf_size(nx_, ny_, nc_, elem_)) { return *sz; }
  throw SipiImageError("Pixel buffer size overflow (dimensions=" + std::to_string(nx_) + "x" + std::to_string(ny_)
                        + ", channels=" + std::to_string(nc_) + ", elem=" + std::to_string(elem_) + ")");
}

}// namespace

// SipiImage::io — the static format-handler registry — is defined in
// //src/format_handlers (format_registry.cpp), not here, so the engine does not
// include the concrete SipiIO* handlers and the SipiImage<->handler Bazel
// dependency cycle stays broken. The engine references `io` below; the linker
// resolves its definition from the format_handlers package.

SipiImage::SipiImage()
{
  nx = 0;
  ny = 0;
  nc = 0;
  bps = 0;
  photo = PhotometricInterpretation::INVALID;
  orientation = TOPLEFT;
  xmp = nullptr;
  icc = nullptr;
  iptc = nullptr;
  exif = nullptr;
  skip_metadata = SkipMetadata::SKIP_NONE;
  ensure_exif();
};
//============================================================================

SipiImage::SipiImage(const SipiImage &img_p)
{
  nx = img_p.nx;
  ny = img_p.ny;
  nc = img_p.nc;
  bps = img_p.bps;
  es = img_p.es;
  orientation = img_p.orientation;
  photo = img_p.photo;
  size_t bufsiz;

  switch (bps) {
  case 8: {
    bufsiz = checked_buf_size_or_throw(nx, ny, nc, sizeof(unsigned char));
    break;
  }

  case 16: {
    bufsiz = checked_buf_size_or_throw(nx, ny, nc, sizeof(unsigned short));
    break;
  }

  default: {
    bufsiz = 0;
  }
  }

  if (bufsiz > 0 && !img_p.pixels.empty()) { pixels = img_p.pixels; }

  // R11: Null-check metadata before deep copy
  if (img_p.xmp) xmp = std::make_shared<Xmp>(*img_p.xmp);
  if (img_p.icc) icc = std::make_shared<Icc>(*img_p.icc);
  if (img_p.iptc) iptc = std::make_shared<Iptc>(*img_p.iptc);
  if (img_p.exif) exif = std::make_shared<Exif>(*img_p.exif);
  emdata = img_p.emdata;
  skip_metadata = img_p.skip_metadata;
}

//============================================================================

SipiImage::SipiImage(SipiImage &&other) noexcept
  : nx(other.nx), ny(other.ny), nc(other.nc), bps(other.bps), es(std::move(other.es)), orientation(other.orientation),
    photo(other.photo), pixels(std::move(other.pixels)), xmp(std::move(other.xmp)), icc(std::move(other.icc)),
    iptc(std::move(other.iptc)), exif(std::move(other.exif)), emdata(std::move(other.emdata)),
    skip_metadata(other.skip_metadata)
{
  other.pixels.clear();
  other.nx = 0;
  other.ny = 0;
}

//============================================================================

SipiImage::SipiImage(size_t nx_p, size_t ny_p, size_t nc_p, size_t bps_p, PhotometricInterpretation photo_p)
  : nx(nx_p), ny(ny_p), nc(nc_p), bps(bps_p), photo(photo_p)
{
  orientation = TOPLEFT;// assuming default...
  if (((photo == PhotometricInterpretation::MINISWHITE) || (photo == PhotometricInterpretation::MINISBLACK))
      && !((nc == 1) || (nc == 2))) {
    throw SipiImageError("Mismatch in Photometric interpretation and number of channels");
  }

  if ((photo == PhotometricInterpretation::RGB) && !((nc == 3) || (nc == 4))) {
    throw SipiImageError("Mismatch: photometric interpretation is RGB but image has " + std::to_string(nc) + " channels (expected 3 or 4)");
  }

  if ((bps != 8) && (bps != 16)) { throw SipiImageError("Unsupported bits/sample (" + std::to_string(bps) + "), only 8 and 16 are supported"); }

  size_t bufsiz;

  switch (bps) {
  case 8: {
    bufsiz = checked_buf_size_or_throw(nx, ny, nc, sizeof(unsigned char));
    break;
  }

  case 16: {
    bufsiz = checked_buf_size_or_throw(nx, ny, nc, sizeof(unsigned short));
    break;
  }

  default: {
    bufsiz = 0;
  }
  }

  if (bufsiz > 0) {
    pixels.resize(bufsiz);
  } else {
    throw SipiImageError("Image has no pixel content (dimensions: " + std::to_string(nx) + "x" + std::to_string(ny) + ", bps: " + std::to_string(bps) + " — file may be corrupt or empty)");
  }

  xmp = nullptr;
  icc = nullptr;
  iptc = nullptr;
  ensure_exif();
  skip_metadata = SkipMetadata::SKIP_NONE;
}

//============================================================================

SipiImage &SipiImage::operator=(const SipiImage &img_p)
{
  if (this != &img_p) {
    pixels.clear();

    nx = img_p.nx;
    ny = img_p.ny;
    nc = img_p.nc;
    bps = img_p.bps;
    orientation = img_p.orientation;
    es = img_p.es;
    photo = img_p.photo;      // BUG FIX: missing in original operator=
    emdata = img_p.emdata;    // BUG FIX: missing in original operator=
    skip_metadata = img_p.skip_metadata;

    size_t bufsiz;

    switch (bps) {
    case 8: {
      bufsiz = checked_buf_size_or_throw(nx, ny, nc, sizeof(unsigned char));
      break;
    }

    case 16: {
      bufsiz = checked_buf_size_or_throw(nx, ny, nc, sizeof(unsigned short));
      break;
    }

    default: {
      bufsiz = 0;
    }
    }

    if (bufsiz > 0 && !img_p.pixels.empty()) { pixels = img_p.pixels; }

    // R11: Null-check metadata before deep copy, reset if source is null
    if (img_p.xmp)  xmp  = std::make_shared<Xmp>(*img_p.xmp);   else xmp.reset();
    if (img_p.icc)  icc  = std::make_shared<Icc>(*img_p.icc);   else icc.reset();
    if (img_p.iptc) iptc = std::make_shared<Iptc>(*img_p.iptc); else iptc.reset();
    if (img_p.exif) exif = std::make_shared<Exif>(*img_p.exif); else exif.reset();
  }

  return *this;
}

//============================================================================

SipiImage &SipiImage::operator=(SipiImage &&other) noexcept
{
  if (this != &other) {
    nx = other.nx;
    ny = other.ny;
    nc = other.nc;
    bps = other.bps;
    es = std::move(other.es);
    orientation = other.orientation;
    photo = other.photo;
    pixels = std::move(other.pixels);
    xmp = std::move(other.xmp);
    icc = std::move(other.icc);
    iptc = std::move(other.iptc);
    exif = std::move(other.exif);
    emdata = std::move(other.emdata);
    skip_metadata = other.skip_metadata;

    other.pixels.clear();
    other.nx = 0;
    other.ny = 0;
  }
  return *this;
}

//============================================================================

/*!
 * If this image has no Exif, creates an empty one.
 */
void SipiImage::ensure_exif()
{
  if (exif == nullptr) exif = std::make_shared<Exif>();
}
//============================================================================

void SipiImage::set_pixels(std::vector<byte> &&buf, size_t nx_p, size_t ny_p, size_t nc_p, size_t bps_p)
{
  const size_t expected = checked_buf_size_or_throw(nx_p, ny_p, nc_p, bps_p / 8);
  if (buf.size() != expected) {
    throw SipiImageError("Pixel buffer size mismatch: buffer has " + std::to_string(buf.size())
                          + " bytes, geometry expects " + std::to_string(expected) + " bytes (dimensions="
                          + std::to_string(nx_p) + "x" + std::to_string(ny_p) + ", channels=" + std::to_string(nc_p)
                          + ", bps=" + std::to_string(bps_p) + ")");
  }
  pixels = std::move(buf);
  nx = nx_p;
  ny = ny_p;
  nc = nc_p;
  bps = bps_p;
}
//============================================================================


/*!
 * Runs `handler_read` with the handler selected by the file's extension,
 * falling back to every other registered handler in turn on a mismatch.
 * Returns a `SipiValueError` if no handler recognises the file, or
 * immediately propagates a handler's failure without considering the
 * fallback handlers. `read` and `read_metadata` share it, so both pick the
 * same handler and fail with the same error.
 */
Result<void> SipiImage::read_dispatch(const std::string &filepath,
  const std::function<Result<bool>(SipiIO &handler)> &handler_read)
{
  size_t pos = filepath.find_last_of('.');
  std::string fext = filepath.substr(pos + 1);
  std::string _fext;

  bool got_file = false;
  _fext.resize(fext.size());
  std::transform(fext.begin(), fext.end(), _fext.begin(), ::tolower);

  std::string dispatched_key;
  if ((_fext == "tif") || (_fext == "tiff")) {
    dispatched_key = "tif";
  } else if ((_fext == "jpg") || (_fext == "jpeg")) {
    dispatched_key = "jpg";
  } else if (_fext == "png") {
    dispatched_key = "png";
  } else if ((_fext == "jp2") || (_fext == "jpx") || (_fext == "j2k")) {
    dispatched_key = "jpx";
  }
  if (!dispatched_key.empty()) {
    auto result = handler_read(*io[dispatched_key]);
    if (!result) { return std::unexpected(result.error()); }
    got_file = *result;
  }

  // A successful `false` means the handler does not recognise the file's
  // actual format, so the search continues across every remaining registered
  // handler (the one already tried by extension is skipped). An error value
  // means the handler recognised the file and failed to decode it; that
  // failure is the authoritative cause and is propagated immediately without
  // trying the remaining handlers.
  if (!got_file) {
    for (auto const &iterator : io) {
      if (iterator.first == dispatched_key) continue;
      auto result = handler_read(*iterator.second);
      if (!result) { return std::unexpected(result.error()); }
      if ((got_file = *result)) break;
    }
  }

  if (!got_file) {
    std::string tried_keys;
    for (auto const &iterator : io) {
      if (!tried_keys.empty()) { tried_keys += ", "; }
      tried_keys += iterator.first;
    }
    return std::unexpected(SipiValueError(ErrorCode::kUnsupportedFormat,
      "Error reading file " + filepath + ": no registered format handler recognised it (tried: " + tried_keys + ")"));
  }
  return {};
}

Result<void> SipiImage::read(const std::string &filepath,
  const std::shared_ptr<SipiRegion> &region,
  const std::shared_ptr<SipiSize> &size,
  bool force_bps_8,
  ScalingQuality scaling_quality)
{
  SIPI_ZONE_N("SipiImage::read");
  return read_dispatch(filepath,
    [&](SipiIO &handler) { return handler.read(this, filepath, region, size, force_bps_8, scaling_quality); });
}

Result<void> SipiImage::read_metadata(const std::string &filepath)
{
  SIPI_ZONE_N("SipiImage::read_metadata");
  return read_dispatch(filepath, [&](SipiIO &handler) { return handler.read_metadata(this, filepath); });
}

//============================================================================

Result<void> SipiImage::readSource(const std::string &filepath,
  const std::shared_ptr<SipiRegion> &region,
  const std::shared_ptr<SipiSize> &size)
{
  if (auto r = read(filepath, region, size, false); !r) { return std::unexpected(r.error()); }

  // Corruption tripwire (ADR-0010): if the source carries an Essentials packet,
  // recompute the pixel checksum and compare. On mismatch, log ERROR and continue —
  // serving operates from Service Files, and an operator wants the signal, not an
  // abort. Active deliberate validation lives in `sipi verify service-file`.
  if (emdata.is_set()) {
    shttps::Hash internal_hash(emdata.fields().hash_type);
    internal_hash.add_data(pixels.data(), checked_buf_size_or_throw(nx, ny, nc, bps / 8));
    std::string checksum = internal_hash.hash();
    if (checksum != Essentials::to_hex(emdata.fields().data_chksum)) {
      log_err("Essentials data_chksum mismatch in %s; possible corruption", filepath.c_str());
      Sipi::observability::essentials_hash_mismatch_counter(
        Sipi::observability::format_from_path(filepath)).Increment();
    }
  }
  return {};
}

//============================================================================


Result<void> SipiImage::readSource(const std::string &filepath,
  const std::shared_ptr<SipiRegion> &region,
  const std::shared_ptr<SipiSize> &size,
  const std::string &origname)
{
  (void)origname;  // consumed by the `convert service-file` command (DEV-6540), not here
  return readSource(filepath, region, size);
}

//============================================================================

std::vector<std::byte> SipiImage::compute_pixel_hash(shttps::HashType type) const
{
  shttps::Hash digest(type);
  if (!pixels.empty()) { digest.add_data(pixels.data(), checked_buf_size_or_throw(nx, ny, nc, bps / 8)); }
  return Essentials::from_hex(digest.hash());
}

//============================================================================

Result<SipiImgInfo> SipiImage::read_shape(const std::string &filepath) const
{
  SIPI_ZONE_N("SipiImage::read_shape");
  size_t pos = filepath.find_last_of('.');
  std::string fext = filepath.substr(pos + 1);
  std::string _fext;

  _fext.resize(fext.size());
  std::transform(fext.begin(), fext.end(), _fext.begin(), ::tolower);

  SipiImgInfo info;
  std::string mimetype = shttps::Parsing::getFileMimetype(filepath).first;
  info.internalmimetype = mimetype;

  std::string dispatched_key;
  if ((mimetype == "image/tiff") || (mimetype == "image/x-tiff")) {
    dispatched_key = "tif";
  } else if ((mimetype == "image/jpeg") || (mimetype == "image/pjpeg")) {
    dispatched_key = "jpg";
  } else if (mimetype == "image/png") {
    dispatched_key = "png";
  } else if ((mimetype == "image/jp2") || (mimetype == "image/jpx")) {
    dispatched_key = "jpx";
  } else {
    return std::unexpected(SipiValueError(ErrorCode::kUnsupportedFormat, "unknown mimetype: \"" + mimetype + "\"!"));
  }
  auto result = io[dispatched_key]->read_shape(filepath);
  if (!result) { return std::unexpected(result.error()); }
  info = *result;

  // A `FAILURE`-marked `SipiImgInfo` means the handler does not recognise the
  // file's actual format, so the search continues across every remaining
  // registered handler (the one already tried by mimetype is skipped). An
  // error value means the handler recognised the file and failed; that
  // failure is the authoritative cause and is propagated immediately without
  // trying the remaining handlers.
  if (info.success == SipiImgInfo::FAILURE) {
    for (auto const &iterator : io) {
      if (iterator.first == dispatched_key) continue;
      auto result2 = iterator.second->read_shape(filepath);
      if (!result2) { return std::unexpected(result2.error()); }
      info = *result2;
      if (info.success != SipiImgInfo::FAILURE) break;
    }
  }

  if (info.success == SipiImgInfo::FAILURE) {
    std::string tried_keys;
    for (auto const &iterator : io) {
      if (!tried_keys.empty()) { tried_keys += ", "; }
      tried_keys += iterator.first;
    }
    return std::unexpected(SipiValueError(ErrorCode::kShapeProbeFailed,
      "Could not read file " + filepath + ": no registered format handler recognised it (tried: " + tried_keys + ")"));
  }
  return info;
}

//============================================================================


void SipiImage::getDim(size_t &width, size_t &height) const
{
  width = getNx();
  height = getNy();
}

//============================================================================

namespace {
// One warning per kind per process; later rejections log at debug. The same
// file rejects its copy on every request, and a restricted request promotes
// twice (before the rewrite and in write()).
void log_rejected_copy(std::atomic<bool> &warned, const char *what, const std::string &reason)
{
  if (!warned.exchange(true, std::memory_order_relaxed)) {
    log_warn("%s (further rejections log at debug): %s", what, reason.c_str());
  } else {
    log_debug("%s: %s", what, reason.c_str());
  }
}

std::atomic<bool> xmp_copy_warned{ false };
std::atomic<bool> iptc_copy_warned{ false };
}// namespace

void SipiImage::promote_ifd0_copies()
{
  if (exif == nullptr) { return; }
  if (xmp == nullptr) {
    if (const auto bytes = exif->value_bytes("Exif.Image.XMLPacket")) {
      auto promoted = Xmp::parse_complete(std::string(bytes->begin(), bytes->end()));
      if (promoted) {
        xmp = *std::move(promoted);
      } else {
        log_rejected_copy(
          xmp_copy_warned, "EXIF XMLPacket copy not used as the XMP", promoted.error().diagnostic_message());
      }
    }
  }
  if (iptc == nullptr) {
    if (const auto bytes = exif->value_bytes("Exif.Image.IPTCNAA")) {
      auto promoted = Iptc::parse_complete(bytes->data(), static_cast<unsigned int>(bytes->size()));
      if (promoted) {
        iptc = *std::move(promoted);
      } else {
        log_rejected_copy(
          iptc_copy_warned, "EXIF IPTCNAA copy not used as the IPTC", promoted.error().diagnostic_message());
      }
    }
  }
}

//============================================================================

Result<void> SipiImage::write(const std::string &ftype, const OutputSink &sink, const SipiCompressionParams *params)
{
  promote_ifd0_copies();
  // .at(): an unknown ftype must throw, not operator[]-insert a null
  // handler and segfault on the virtual call. Callers pass validated
  // format strings; the historical write() docstring advertised "j2k"
  // (the map key is "jpx"), which is exactly how this fired.
  auto result = io.at(ftype)->write(this, sink, params);
  if (!result) { return std::unexpected(result.error()); }
  return {};
}

Result<void> SipiImage::write(const std::string &ftype,
  const std::string &filepath,
  const SipiCompressionParams *params)
{
  return write(ftype, OutputSink{ FilePath{ filepath } }, params);
}

//============================================================================

std::ostream &operator<<(std::ostream &outstr, const SipiImage &rhs)
{
  outstr << '\n' << "SipiImage with the following parameters:" << '\n';
  outstr << "nx    = " << std::to_string(rhs.nx) << '\n';
  outstr << "ny    = " << std::to_string(rhs.ny) << '\n';
  outstr << "nc    = " << std::to_string(rhs.nc) << '\n';
  outstr << "es    = " << std::to_string(rhs.es.size()) << '\n';
  outstr << "bps   = " << std::to_string(rhs.bps) << '\n';
  outstr << "photo = " << to_string(rhs.photo) << '\n';

  if (rhs.xmp) { outstr << "XMP-Metadata: " << '\n' << *(rhs.xmp) << '\n'; }

  if (rhs.iptc) { outstr << "IPTC-Metadata: " << '\n' << *(rhs.iptc) << '\n'; }

  if (rhs.exif) { outstr << "EXIF-Metadata: " << '\n' << *(rhs.exif) << '\n'; }

  if (rhs.icc) { outstr << "ICC-Metadata: " << '\n' << *(rhs.icc) << '\n'; }

  return outstr;
}

//============================================================================

}// namespace Sipi
