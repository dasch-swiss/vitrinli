/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Byte-exact output invariant (approval gate).
 *
 * The TIFF encode path here is pinned byte-for-byte by the approval tests
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
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <cerrno>

#include "logging/logger.h"
#include "error/SipiError.h"
#include "error/SipiValueError.h"
#include "image/SipiIO.h"
#include "image/SipiImage.h"
#include "image/SipiImageError.h"
#include "image_processing/processing.h"
#include "SipiIOTiff.h"
#include "observability/metrics.h"
#include "observability/profiling.h"


#include "util/checked_arith.h"
#include "util/Global.h"

#define TIFF_GET_FIELD(file, tag, var, default)                      \
  {                                                                  \
    if (0 == TIFFGetField((file), (tag), (var))) *(var) = (default); \
  }

extern "C" {

typedef struct _memtiff
{
  unsigned char *data;
  tsize_t size;
  tsize_t incsiz;
  tsize_t flen;
  toff_t fptr;
} MEMTIFF;

static tsize_t memTiffReadProc(thandle_t handle, tdata_t buf, tsize_t size)
{
  auto *memtif = (MEMTIFF *)handle;

  tsize_t n;

  if (((tsize_t)memtif->fptr + size) <= memtif->flen) {
    n = size;
  } else {
    n = memtif->flen - static_cast<tsize_t>(memtif->fptr);
  }

  memcpy(buf, memtif->data + memtif->fptr, n);
  memtif->fptr += n;

  return n;
}
/*===========================================================================*/

static tsize_t memTiffWriteProc(thandle_t handle, tdata_t buf, tsize_t size)
{
  auto *memtif = (MEMTIFF *)handle;

  if (((tsize_t)memtif->fptr + size) > memtif->size) {
    // Use temp variable to avoid losing original pointer on realloc failure
    auto *newdata = (unsigned char *)realloc(memtif->data, memtif->fptr + memtif->incsiz + size);
    if (newdata == nullptr) {
      // Return 0 to signal write failure — libtiff treats short writes as errors.
      // Cannot throw: this is called from libtiff's C code.
      return 0;
    }
    memtif->data = newdata;
    memtif->size = memtif->fptr + memtif->incsiz + size;
  }

  memcpy(memtif->data + memtif->fptr, buf, size);
  memtif->fptr += size;

  if (memtif->fptr > memtif->flen) memtif->flen = memtif->fptr;

  return size;
}
/*===========================================================================*/

static toff_t memTiffSeekProc(thandle_t handle, toff_t off, int whence)
{
  auto *memtif = (MEMTIFF *)handle;

  toff_t target;
  switch (whence) {
  case SEEK_SET:
    target = off;
    break;
  case SEEK_CUR:
    target = memtif->fptr + off;
    break;
  case SEEK_END:
    target = static_cast<toff_t>(memtif->flen) + off;
    break;
  default:
    return (toff_t)-1;
  }

  if ((tsize_t)target > memtif->size) {
    auto *newdata = (unsigned char *)realloc(memtif->data, target + memtif->incsiz);
    if (newdata == nullptr) {
      return (toff_t)-1;  // signal seek failure to libtiff
    }
    memtif->data = newdata;
    memtif->size = target + memtif->incsiz;
  }

  // A seek past the end leaves a hole that ships in the response body: it must
  // read as zeros, never as whatever the allocator left in the buffer.
  if ((tsize_t)target > memtif->flen) {
    memset(memtif->data + memtif->flen, 0, target - memtif->flen);
    memtif->flen = target;
  }
  memtif->fptr = target;
  return memtif->fptr;
}
/*===========================================================================*/

static int memTiffCloseProc(thandle_t handle)
{
  auto *memtif = (MEMTIFF *)handle;
  memtif->fptr = 0;
  return 0;
}
/*===========================================================================*/


static toff_t memTiffSizeProc(thandle_t handle)
{
  auto *memtif = (MEMTIFF *)handle;
  return memtif->flen;
}
/*===========================================================================*/


static int memTiffMapProc(thandle_t handle, tdata_t *base, toff_t *psize)
{
  auto *memtif = (MEMTIFF *)handle;
  *base = memtif->data;
  *psize = memtif->flen;
  return (1);
}
/*===========================================================================*/

static void memTiffUnmapProc(thandle_t handle, tdata_t base, toff_t size) {}
/*===========================================================================*/

static void memTiffFree(MEMTIFF *memtif)
{
  if (memtif == nullptr) { return; }
  free(memtif->data);
  free(memtif);
}
/*===========================================================================*/
}

static MEMTIFF *memTiffOpen(tsize_t incsiz = 10240, tsize_t initsiz = 10240)
{
  MEMTIFF *memtif;
  if ((memtif = (MEMTIFF *)malloc(sizeof(MEMTIFF))) == nullptr) { throw Sipi::SipiImageError("malloc failed", errno); }

  memtif->incsiz = incsiz;

  if (initsiz == 0) initsiz = incsiz;

  if ((memtif->data = (unsigned char *)malloc(initsiz * sizeof(unsigned char))) == nullptr) {
    free(memtif);
    throw Sipi::SipiImageError("malloc failed", errno);
  }

  memtif->size = initsiz;
  memtif->flen = 0;
  memtif->fptr = 0;
  return memtif;
}
/*===========================================================================*/

//
// the 2 typedefs below are used to extract the EXIF-tags from a TIFF file. This is done
// using the normal libtiff functions...
//
typedef enum {
  EXIF_DT_UINT8 = 1,
  EXIF_DT_STRING = 2,
  EXIF_DT_UINT16 = 3,
  EXIF_DT_UINT32 = 4,
  EXIF_DT_RATIONAL = 5,
  EXIF_DT_2ST = 7,

  EXIF_DT_RATIONAL_PTR = 101,
  EXIF_DT_UINT8_PTR = 102,
  EXIF_DT_UINT16_PTR = 103,
  EXIF_DT_UINT32_PTR = 104,
  EXIF_DT_PTR = 105,
  EXIF_DT_UNDEFINED = 999

} ExifDataType_type;

typedef struct _exif_tag
{
  uint16_t tag_id;
  ExifDataType_type datatype;
  int len;
  union {
    float f_val;
    uint8_t c_val;
    uint16_t s_val;
    uint32_t i_val;
    char *str_val;
    float *f_ptr;
    uint8_t *c_ptr;
    uint16_t *s_ptr;
    uint32_t *i_ptr;
    void *ptr;
    unsigned char _4cc[4];
    unsigned short _2st[2];
  };
} ExifTag_type;

static ExifTag_type exiftag_list[] = {
  { EXIFTAG_EXPOSURETIME, EXIF_DT_RATIONAL, 0L, { 0L } },
  { EXIFTAG_FNUMBER, EXIF_DT_RATIONAL, 0L, { 0L } },
  { EXIFTAG_EXPOSUREPROGRAM, EXIF_DT_UINT16, 0L, { 0L } },
  { EXIFTAG_SPECTRALSENSITIVITY, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_ISOSPEEDRATINGS, EXIF_DT_UINT16_PTR, 0L, { 0L } },
  { EXIFTAG_OECF, EXIF_DT_PTR, 0L, { 0L } },
  { EXIFTAG_EXIFVERSION, EXIF_DT_UNDEFINED, 0L, { 0L } },
  { EXIFTAG_DATETIMEORIGINAL, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_DATETIMEDIGITIZED, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_COMPONENTSCONFIGURATION, EXIF_DT_UNDEFINED, 0L, { 1L } },// !!!! would be 4cc
  { EXIFTAG_COMPRESSEDBITSPERPIXEL, EXIF_DT_RATIONAL, 0L, { 0L } },
  { EXIFTAG_SHUTTERSPEEDVALUE, EXIF_DT_RATIONAL, 0L, { 0L } },
  { EXIFTAG_APERTUREVALUE, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_BRIGHTNESSVALUE, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_EXPOSUREBIASVALUE, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_MAXAPERTUREVALUE, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_SUBJECTDISTANCE, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_METERINGMODE, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_LIGHTSOURCE, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_FLASH, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_FOCALLENGTH, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_SUBJECTAREA,
    EXIF_DT_UINT16_PTR,
    0L,
    { 0L } },//!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! ARRAY OF SHORTS
  { EXIFTAG_MAKERNOTE, EXIF_DT_UNDEFINED, 0L, { 0L } },
  { EXIFTAG_USERCOMMENT, EXIF_DT_PTR, 0L, { 0L } },
  { EXIFTAG_SUBSECTIME, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_SUBSECTIMEORIGINAL, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_SUBSECTIMEDIGITIZED, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_FLASHPIXVERSION, EXIF_DT_UNDEFINED, 0L, { 01L } },// 2 SHORTS
  { EXIFTAG_COLORSPACE, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_PIXELXDIMENSION, EXIF_DT_UINT32, 0L, { 0l } },// CAN ALSO BE UINT16 !!!!!!!!!!!!!!
  { EXIFTAG_PIXELYDIMENSION, EXIF_DT_UINT32, 0L, { 0l } },// CAN ALSO BE UINT16 !!!!!!!!!!!!!!
  { EXIFTAG_RELATEDSOUNDFILE, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_FLASHENERGY, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_SPATIALFREQUENCYRESPONSE, EXIF_DT_PTR, 0L, { 0L } },
  { EXIFTAG_FOCALPLANEXRESOLUTION, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_FOCALPLANEYRESOLUTION, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_FOCALPLANERESOLUTIONUNIT, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_SUBJECTLOCATION, EXIF_DT_UINT32, 0L, { 0l } },// 2 SHORTS !!!!!!!!!!!!!!!!!!!!!!!!!!!
  { EXIFTAG_EXPOSUREINDEX, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_SENSINGMETHOD, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_FILESOURCE, EXIF_DT_UINT8, 0L, { 0L } },
  { EXIFTAG_SCENETYPE, EXIF_DT_UINT8, 0L, { 0L } },
  { EXIFTAG_CFAPATTERN, EXIF_DT_PTR, 0L, { 0L } },
  { EXIFTAG_CUSTOMRENDERED, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_EXPOSUREMODE, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_WHITEBALANCE, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_DIGITALZOOMRATIO, EXIF_DT_RATIONAL, 0L, { 0l } },
  { EXIFTAG_FOCALLENGTHIN35MMFILM, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_SCENECAPTURETYPE, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_GAINCONTROL, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_CONTRAST, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_SATURATION, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_SHARPNESS, EXIF_DT_UINT16, 0L, { 0l } },
  { EXIFTAG_DEVICESETTINGDESCRIPTION, EXIF_DT_PTR, 0L, { 0L } },
  { EXIFTAG_SUBJECTDISTANCERANGE, EXIF_DT_UINT16, 0L, { 0L } },
  { EXIFTAG_IMAGEUNIQUEID, EXIF_DT_STRING, 0L, { 0L } },
  // EXIF 2.3 / 2.31 lens, timezone, sensitivity, and gamma tags.
  // LensSpecification is RATIONAL[4] (min/max focal length, min F at min FL,
  // min F at max FL) and is the only entry in this list with EXIF_DT_RATIONAL_PTR
  // — it exercises the rational-array read path at SipiIOTiff.cpp readExif().
  // The `4` is the fixed count: libtiff calls this `TIFF_SETGET_C0_FLOAT`
  // (fixed array, no variadic count argument).
  { EXIFTAG_LENSSPECIFICATION, EXIF_DT_RATIONAL_PTR, 4, { 0L } },
  { EXIFTAG_LENSMAKE, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_LENSMODEL, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_LENSSERIALNUMBER, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_GAMMA, EXIF_DT_RATIONAL, 0L, { 0L } },
  { EXIFTAG_OFFSETTIME, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_OFFSETTIMEORIGINAL, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_OFFSETTIMEDIGITIZED, EXIF_DT_STRING, 0L, { 0L } },
  { EXIFTAG_SENSITIVITYTYPE, EXIF_DT_UINT16, 0L, { 0L } },
};

static int exiftag_list_len = sizeof(exiftag_list) / sizeof(ExifTag_type);


namespace Sipi {

namespace {

// Every decode-side buffer allocation in this file funnels through here:
// reject (throw) the moment `nx * ny * nc * elem` overflows `size_t`, rather
// than letting a wrapped size feed a too-small `std::vector` allocation that
// a later copy then overruns.
size_t checked_buf_size_or_throw(size_t nx_, size_t ny_, size_t nc_, size_t elem_)
{
  if (const auto sz = checked_buf_size(nx_, ny_, nc_, elem_)) { return *sz; }
  throw SipiImageError("Pixel buffer size overflow (dimensions=" + std::to_string(nx_) + "x" + std::to_string(ny_)
                        + ", channels=" + std::to_string(nc_) + ", elem=" + std::to_string(elem_) + ")");
}

// Accumulates the libtiff diagnostic text emitted while a single TIFF handle
// (opened via TIFFOpenExt/TIFFClientOpenExt) is alive, in call order, so a
// file that trips several libtiff errors contributes all of them to one
// Result rather than only the first or the last.
struct TiffDiagnosticSink
{
  std::string text;
};

// Formats one libtiff error/warning call into the sink, mirroring tiffError's
// 1024-byte buffer below. Invoked from libtiff's own C stack frames via
// TIFFOpenOptionsSet{Error,Warning}HandlerExtR — no exception may escape.
void append_tiff_diagnostic(void *user_data, const char *module, const char *fmt, va_list args)
{
  try {
    auto *sink = static_cast<TiffDiagnosticSink *>(user_data);
    char buf[1024];
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) { return; }
    if (!sink->text.empty()) { sink->text += "; "; }
    if (module != nullptr) {
      sink->text += module;
      sink->text += ": ";
    }
    sink->text += buf;
  } catch (...) {
  }
}

// Returns 0 (not handled) so libtiff also invokes the classic global handler
// (tiffError/tiffWarning below): log_err/log_warn keeps seeing every libtiff
// diagnostic exactly as before. This handler only ADDS capture into the
// per-call sink; it does not take over or suppress logging.
int tiff_error_ext_r(TIFF *, void *user_data, const char *module, const char *fmt, va_list args)
{
  append_tiff_diagnostic(user_data, module, fmt, args);
  return 0;
}

int tiff_warning_ext_r(TIFF *, void *user_data, const char *module, const char *fmt, va_list args)
{
  append_tiff_diagnostic(user_data, module, fmt, args);
  return 0;
}

using TiffOpenOptionsPtr = std::unique_ptr<TIFFOpenOptions, decltype(&TIFFOpenOptionsFree)>;

// RAII alloc: TIFFOpenOptionsFree runs on every return path, including early
// ones. Wires both handlers to the same sink so one open call's errors and
// warnings land in one accumulated string.
TiffOpenOptionsPtr make_tiff_open_options(TiffDiagnosticSink &sink)
{
  TiffOpenOptionsPtr opts(TIFFOpenOptionsAlloc(), TIFFOpenOptionsFree);
  TIFFOpenOptionsSetErrorHandlerExtR(opts.get(), tiff_error_ext_r, &sink);
  TIFFOpenOptionsSetWarningHandlerExtR(opts.get(), tiff_warning_ext_r, &sink);
  return opts;
}

// Appends the sink's accumulated libtiff diagnostic text to a failure's
// message when non-empty, preserving the original error's code/errnum/source
// location; a no-op when the sink captured nothing.
[[nodiscard]] SipiValueError with_tiff_diagnostic(SipiValueError err, const TiffDiagnosticSink &sink)
{
  if (sink.text.empty()) { return err; }
  return SipiValueError{ err.code(), err.raw_message() + ": " + sink.text, err.errnum(), err.location() };
}

}// namespace

Result<std::vector<unsigned char>> read_watermark(const std::string &wmfile, int &nx, int &ny, int &nc)
{
  int sll;
  unsigned short spp, bps, pmi, pc;
  nx = 0;
  ny = 0;

  TiffDiagnosticSink tiff_diag;
  TiffOpenOptionsPtr tiff_opts = make_tiff_open_options(tiff_diag);
  std::unique_ptr<TIFF, decltype(&TIFFClose)> tif(TIFFOpenExt(wmfile.c_str(), "r", tiff_opts.get()), TIFFClose);
  if (tif == nullptr) { return {}; }

  // add EXIF tags to the set of tags that libtiff knows about
  // necessary if we want to set EXIFTAG_DATETIMEORIGINAL, for example
  // const TIFFFieldArray *exif_fields = _TIFFGetExifFields();
  //_TIFFMergeFields(tif, exif_fields->fields, exif_fields->count);


  if (TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &nx) == 0) {
    return std::unexpected(with_tiff_diagnostic(SipiValueError{ ErrorCode::kMalformedInput,
                              "ERROR in read_watermark: TIFFGetField of TIFFTAG_IMAGEWIDTH failed: " + wmfile },
      tiff_diag));
  }

  if (TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &ny) == 0) {
    return std::unexpected(with_tiff_diagnostic(SipiValueError{ ErrorCode::kMalformedInput,
                              "ERROR in read_watermark: TIFFGetField of TIFFTAG_IMAGELENGTH failed: " + wmfile },
      tiff_diag));
  }

  TIFF_GET_FIELD(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp, 1);

  TIFF_GET_FIELD(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps, 1);

  if (bps != 8) {
    return std::unexpected(with_tiff_diagnostic(
      SipiValueError{ ErrorCode::kUnsupportedFormat, "ERROR in read_watermark: bps ≠ 8: " + wmfile }, tiff_diag));
  }

  TIFF_GET_FIELD(tif.get(), TIFFTAG_PHOTOMETRIC, &pmi, PHOTOMETRIC_MINISBLACK);
  TIFF_GET_FIELD(tif.get(), TIFFTAG_PLANARCONFIG, &pc, PLANARCONFIG_CONTIG);

  if (pc != PLANARCONFIG_CONTIG) {
    return std::unexpected(with_tiff_diagnostic(SipiValueError{ ErrorCode::kUnsupportedFormat,
                              "ERROR in read_watermark: Tag TIFFTAG_PLANARCONFIG is not PLANARCONFIG_CONTIG: "
                                + wmfile },
      tiff_diag));
  }

  if (auto r = validate_decode_dims(static_cast<std::size_t>(nx),
        static_cast<std::size_t>(ny),
        static_cast<std::size_t>(spp),
        static_cast<int>(bps),
        wmfile);
      !r) {
    return std::unexpected(std::move(r).error());
  }

  sll = nx * spp * bps / 8;

  std::vector<unsigned char> wmbuf;
  try {
    wmbuf.resize(static_cast<size_t>(ny) * sll);
  } catch (std::bad_alloc &ba) {
    throw Sipi::SipiImageError("ERROR in read_watermark: Could not allocate memory: ");// + ba.what());
  }

  for (int i = 0; i < ny; i++) {
    if (TIFFReadScanline(tif.get(), wmbuf.data() + i * sll, i) == -1) {
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kDecodeFailed,
          "ERROR in read_watermark: TIFFReadScanline failed on scanline " + std::to_string(i) + " in file " + wmfile },
        tiff_diag));
    }
  }

  nc = spp;

  return wmbuf;
}
//============================================================================


// libtiff's fmt/args pair is text to log, never a format string to re-run: the
// va_list is formatted locally with vsnprintf and the resulting text crosses
// into log_err/log_warn as a %s argument. These handlers are process-global
// and libtiff invokes them, from any decoding thread, out of its own C stack
// frames — the body uses locals only (no shared mutable state) and must never
// let an exception escape into those C frames, which is why the whole body is
// wrapped in a catch-all here at this C-library boundary.
static void tiffError(const char *module, const char *fmt, va_list args)
{
  try {
    char buf[1024];
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    log_err("libtiff error (module %s): %s", module != nullptr ? module : "unknown", n >= 0 ? buf : "");
  } catch (...) {
  }
}
//============================================================================


static void tiffWarning(const char *module, const char *fmt, va_list args)
{
  try {
    char buf[1024];
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    log_warn("libtiff warning (module %s): %s", module != nullptr ? module : "unknown", n >= 0 ? buf : "");
  } catch (...) {
  }
}
//============================================================================

#define N(a) (sizeof(a) / sizeof(a[0]))
#define TIFFTAG_SIPIMETA 65111
// New protobuf-binary carrier for the Essentials packet (ADR-0005 / DEV-6410).
// Field type TIFF_UNDEFINED with variable count so TIFFGetField returns the
// byte length alongside the data pointer (the legacy ASCII tag does not). The
// pyramidal TIFF writer emits this gated on `pyramid && es.is_set()` (per
// ADR-0010); plain TIFF never carries it (ADR-0009).
#define TIFFTAG_SIPIMETA_PB 65112

static const TIFFFieldInfo xtiffFieldInfo[] = {
  { TIFFTAG_SIPIMETA, 1, 1, TIFF_ASCII, FIELD_CUSTOM, 1, 0, const_cast<char *>("SipiEssentialMetadata") },
  // TIFF_VARIABLE2 = -3 means uint32 count + buffer (vs -1 for uint16). Service
  // File Essentials packets are sub-kilobyte today (~200 bytes for shape +
  // identity, +ICC payload), so even uint16 would suffice — uint32 is the
  // forward-compatible default for binary blobs and matches the carrier-size
  // budget reserved by the JP2 UUID box's `total_size: u32` field.
  { TIFFTAG_SIPIMETA_PB, -3, -3, TIFF_UNDEFINED, FIELD_CUSTOM, 1, 1, const_cast<char *>("SipiEssentialMetadataPB") },
};
//============================================================================

static TIFFExtendProc parent_extender = nullptr;

static void registerCustomTIFFTags(TIFF *tif)
{
  /* Install the extended Tag field info */
  TIFFMergeFieldInfo(tif, xtiffFieldInfo, N(xtiffFieldInfo));
  if (parent_extender != nullptr) (*parent_extender)(tif);
}
//============================================================================

void SipiIOTiff::initLibrary()
{
  static bool done = false;
  if (!done) {
    TIFFSetErrorHandler(tiffError);
    TIFFSetWarningHandler(tiffWarning);

    parent_extender = TIFFSetTagExtender(registerCustomTIFFTags);
    done = true;
  }
}
//============================================================================

template<typename T> void one2eight(const uint8_t *in, T *out, uint32_t len, uint8_t black, uint8_t white)
{
  static uint8_t mask[8] = {
    0b10000000, 0b01000000, 0b00100000, 0b00010000, 0b00001000, 0b00000100, 0b00000010, 0b00000001
  };
  uint32_t ii = 0;
  for (uint32_t i = 0; i < len; i += 8) {
    for (uint32_t k = 0; (k < 8) && ((k + i) < len); ++k) { out[i + k] = mask[k] & in[ii] ? white : black; }
    ++ii;
  }
}

template<typename T> void four2eight(const uint8_t *in, T *out, uint32_t len, bool is_palette = false)
{
  static uint8_t mask[2] = { 0b11110000, 0b00001111 };

  if (is_palette) {
    uint32_t ii = 0;
    for (uint32_t i = 0; i < len; i += 2, ++ii) {
      out[i] = (mask[0] & in[ii]) >> 4;
      if ((i + 1) < len) { out[i + 1] = mask[1] & in[ii]; }
    }
  } else {
    uint32_t ii = 0;
    for (uint32_t i = 0; i < len; i += 2, ++ii) {
      out[i] = mask[0] & in[ii];
      if ((i + 1) < len) { out[i + 1] = (mask[1] & in[ii]) << 4; }
    }
  }
}

template<typename T> void twelve2sixteen(const uint8_t *in, T *out, uint32_t len, bool is_palette = false)
{
  static uint8_t mask[2] = { 0b11110000, 0b00001111 };
  if (is_palette) {
    uint32_t ii = 0;
    for (uint32_t i = 0; i < len; i += 2, ii += 3) {
      out[i] = (in[ii] << 4) | ((in[ii + 1] & mask[0]) >> 4);
      if ((i + 1) < len) { out[i + 1] = ((in[ii + 1] & mask[1]) << 8) | in[ii + 2]; }
    }
  } else {
    uint32_t ii = 0;
    for (uint32_t i = 0; i < len; i += 2, ii += 3) {
      out[i] = (in[ii] << 8) | (in[ii + 1] & mask[0]);
      if ((i + 1) < len) { out[i + 1] = ((in[ii + 1] & mask[1]) << 12) | (in[ii + 2] << 4); }
    }
  }
}

template<typename T>
std::unique_ptr<T> separateToContig(std::unique_ptr<T> &&inbuf, uint32_t nx, uint32_t ny, uint32_t nc, uint32_t sll)
{
  // `T` is deduced from the call site as an array type (e.g. `uint8_t[]`), so
  // `std::unique_ptr<T>` is really `std::unique_ptr<uint8_t[]>` and
  // `std::make_unique<T>(n)` allocates `n` elements, not `n` bytes of `T`.
  static_assert(std::is_array_v<T>, "separateToContig(unique_ptr) expects an array type, e.g. uint8_t[]");
  auto tmpptr = std::make_unique<T>(checked_buf_size_or_throw(nc, ny, nx, 1));
  for (uint32_t c = 0; c < nc; ++c) {
    for (uint32_t y = 0; y < ny; ++y) {
      for (uint32_t x = 0; x < nx; ++x) { tmpptr[nc * (y * nx + x) + c] = inbuf.get()[c * ny * sll + y * nx + x]; }
    }
  }
  return tmpptr;
}

template<typename T>
std::vector<T> separateToContig(std::vector<T> &&inbuf, uint32_t nx, uint32_t ny, uint32_t nc, uint32_t sll)
{
  auto tmpptr = std::vector<T>(checked_buf_size_or_throw(nc, ny, nx, 1));
  for (uint32_t c = 0; c < nc; ++c) {
    for (uint32_t y = 0; y < ny; ++y) {
      for (uint32_t x = 0; x < nx; ++x) { tmpptr[nc * (y * nx + x) + c] = inbuf[c * ny * sll + y * nx + x]; }
    }
  }
  return tmpptr;
}

template<typename T>
static Result<void>
  read_standard_data(TIFF *tif, int32_t roi_x, int32_t roi_y, uint32_t roi_w, uint32_t roi_h, std::span<T> out)
{
  uint16_t planar;
  TIFF_GET_FIELD(tif, TIFFTAG_PLANARCONFIG, &planar, PLANARCONFIG_CONTIG)
  uint16_t compression;
  TIFF_GET_FIELD(tif, TIFFTAG_COMPRESSION, &compression, COMPRESSION_NONE)
  auto sll = static_cast<uint32_t>(TIFFScanlineSize(tif));

  uint32_t nx, ny, nc, bps;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &nx);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &ny);
  uint16_t stmp;
  TIFF_GET_FIELD(tif, TIFFTAG_SAMPLESPERPIXEL, &stmp, 1)
  nc = static_cast<uint32_t>(stmp);

  TIFF_GET_FIELD(tif, TIFFTAG_BITSPERSAMPLE, &stmp, 8)
  bps = static_cast<uint32_t>(stmp);

  PhotometricInterpretation photo;
  if (1 != TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &stmp)) {
    photo = PhotometricInterpretation::MINISBLACK;
  } else {
    photo = static_cast<PhotometricInterpretation>(stmp);
  }

  uint8_t black, white;
  if (photo == PhotometricInterpretation::MINISBLACK) {
    black = 0x00;// 0b0 -> 0x00
    white = 0xff;// 0b1 -> 0xff
  } else if (photo == PhotometricInterpretation::MINISWHITE) {
    black = 0xff;// 0b0 -> 0xff
    white = 0x00;// 0b1 -> 0x00
  }

  uint32_t psiz;
  if (bps <= 8) {// 1, 4, 8 bit -> 8 bit
    psiz = sizeof(uint8_t);
  } else {// 12, 16 bit -> 16 bit
    psiz = sizeof(uint16_t);
  }

  const uint64_t samples_per_line =
    (planar == PLANARCONFIG_CONTIG) ? static_cast<uint64_t>(nx) * nc : static_cast<uint64_t>(nx);
  const uint64_t expected_sll = (samples_per_line * bps + 7) / 8;
  if (sll < expected_sll) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "TIFF scanline size " + std::to_string(sll) + " smaller than expected " + std::to_string(expected_sll)
        + " for " + std::to_string(nx) + "x" + std::to_string(ny) + ", channels=" + std::to_string(nc)
        + ", bps=" + std::to_string(bps) });
  }

  if (out.size() != checked_buf_size_or_throw(roi_h, roi_w, nc, 1)) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "TIFF pixel buffer holds " + std::to_string(out.size()) + " samples, region needs " + std::to_string(roi_w) + "x"
        + std::to_string(roi_h) + ", channels=" + std::to_string(nc) });
  }
  auto scanline = std::make_unique<uint8_t[]>(sll);
  std::unique_ptr<T[]> line;
  if (compression == COMPRESSION_NONE) {
    if (planar == PLANARCONFIG_CONTIG) {// RGBRGBRGBRGB...
      line = std::make_unique<T[]>(nx * nc);
      for (uint32_t i = roi_y; i < roi_y + roi_h; ++i) {
        if (TIFFReadScanline(tif, scanline.get(), i, 0) != 1) {
          return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
            "TIFFReadScanline failed on scanline " + std::to_string(i)
              + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
              + ", channels=" + std::to_string(nc) + ", bps=" + std::to_string(bps) });
        }
        // Every memcpy destination offset uses `(i - roi_y)`, so the first
        // ROI row lands in row 0 of `out`; `i * roi_w` overflows `out` for
        // any roi_y > 0.
        switch (bps) {
        case 1:
          one2eight<T>(scanline.get(), line.get(), nc * nx, black, white);
          std::memcpy(out.data() + nc * (i - roi_y) * roi_w, line.get() + nc * roi_x, nc * roi_w);
          break;
        case 4:
          four2eight<T>(scanline.get(), line.get(), nc * nx, photo == PhotometricInterpretation::PALETTE);
          std::memcpy(out.data() + nc * (i - roi_y) * roi_w, line.get() + nc * roi_x, nc * roi_w);
          break;
        case 8:
          std::memcpy(out.data() + nc * (i - roi_y) * roi_w, scanline.get() + nc * roi_x, nc * roi_w);
          break;
        case 12:
          twelve2sixteen<T>(scanline.get(), line.get(), nc * nx, photo == PhotometricInterpretation::PALETTE);
          std::memcpy(out.data() + nc * (i - roi_y) * roi_w, line.get() + nc * roi_x, nc * roi_w * psiz);
          break;
        case 16:
          std::memcpy(out.data() + nc * (i - roi_y) * roi_w, scanline.get() + nc * roi_x * psiz, nc * roi_w * psiz);
          break;
        default:;
        }
      }
    } else if (planar == PLANARCONFIG_SEPARATE) {// RRRRR…RRR GGGGG…GGGG BBBBB…BBB
      std::vector<T> inbuf(out.size());
      line = std::make_unique<T[]>(nx);
      for (uint32_t c = 0; c < nc; ++c) {
        for (uint32_t i = roi_y; i < roi_y + roi_h; ++i) {
          if (TIFFReadScanline(tif, scanline.get(), i, c) == -1) {
            return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
              "TIFFReadScanline failed on scanline " + std::to_string(i)
                + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
                + ", channels=" + std::to_string(nc) + ", bps=" + std::to_string(bps) });
          }
          // Destination offsets are `(c * roi_h + (i - roi_y)) * roi_w` so
          // channel c writes into its own [c * roi_h, (c + 1) * roi_h) row
          // band and the first destination row is always row 0 of that band.
          switch (bps) {
          case 1:
            one2eight<T>(scanline.get(), line.get(), nx, black, white);
            std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, line.get() + roi_x, roi_w);
            break;
          case 4:
            four2eight<T>(scanline.get(), line.get(), nx, photo == PhotometricInterpretation::PALETTE);
            std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, line.get() + roi_x, roi_w);
            break;
          case 8:
            std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, scanline.get() + roi_x, roi_w);
            break;
          case 12:
            twelve2sixteen<T>(scanline.get(), line.get(), nx, photo == PhotometricInterpretation::PALETTE);
            std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, line.get() + roi_x, roi_w * psiz);
            break;
          case 16:
            std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, scanline.get() + roi_x * psiz, roi_w * psiz);
            break;
          default:;
          }
        }
      }
      const auto contig = separateToContig<T>(std::move(inbuf), roi_w, roi_h, nc, roi_w);
      std::copy(contig.begin(), contig.end(), out.begin());
    }
  } else {// we do have compression....
    if (planar == PLANARCONFIG_CONTIG) {// RGBRGBRGBRGB...
      line = std::make_unique<T[]>(nx * nc);
      for (uint32_t i = 0; i < ny; ++i) {
        if (TIFFReadScanline(tif, scanline.get(), i, 0) != 1) {
          return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
            "TIFFReadScanline failed on scanline " + std::to_string(i)
              + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
              + ", channels=" + std::to_string(nc) + ", bps=" + std::to_string(bps) });
        }
        if ((i >= roi_y) && (i < (roi_y + roi_h))) {
          // Every memcpy destination offset uses `(i - roi_y)`, so the first
          // ROI row lands in row 0 of `out` wherever the ROI starts;
          // `i * roi_w` overflows `out` for any roi_y > 0.
          switch (bps) {
          case 1:
            one2eight<T>(scanline.get(), line.get(), nc * nx, black, white);
            std::memcpy(out.data() + nc * (i - roi_y) * roi_w, line.get() + nc * roi_x, nc * roi_w);
            break;
          case 4:
            four2eight<T>(scanline.get(), line.get(), nc * nx, photo == PhotometricInterpretation::PALETTE);
            std::memcpy(out.data() + nc * (i - roi_y) * roi_w, line.get() + nc * roi_x, nc * roi_w);
            break;
          case 8:
            std::memcpy(out.data() + nc * (i - roi_y) * roi_w, scanline.get() + nc * roi_x, nc * roi_w);
            break;
          case 12:
            twelve2sixteen<T>(scanline.get(), line.get(), nc * nx, photo == PhotometricInterpretation::PALETTE);
            std::memcpy(out.data() + nc * (i - roi_y) * roi_w, line.get() + nc * roi_x, nc * roi_w * psiz);
            break;
          case 16:
            std::memcpy(out.data() + nc * (i - roi_y) * roi_w, scanline.get() + nc * roi_x * psiz, nc * roi_w * psiz);
            break;
          default:;
          }
        }
      }
    } else if (planar == PLANARCONFIG_SEPARATE) {// RRRRR…RRR GGGGG…GGGG BBBBB…BBB
      std::vector<T> inbuf(out.size());
      line = std::make_unique<T[]>(nx);
      for (uint32_t c = 0; c < nc; ++c) {
        for (uint32_t i = 0; i < ny; ++i) {
          if (TIFFReadScanline(tif, scanline.get(), i, c) == -1) {
            return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
              "TIFFReadScanline failed on scanline " + std::to_string(i)
                + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
                + ", channels=" + std::to_string(nc) + ", bps=" + std::to_string(bps) });
          }
          if ((i >= roi_y) && (i < (roi_y + roi_h))) {
            // Destination offsets are `(c * roi_h + (i - roi_y)) * roi_w` so
            // channel c writes into its own [c * roi_h, (c + 1) * roi_h) row
            // band and the first destination row is always row 0 of that band.
            switch (bps) {
            case 1:
              one2eight<T>(scanline.get(), line.get(), nx, black, white);
              std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, line.get() + roi_x, roi_w);
              break;
            case 4:
              four2eight<T>(scanline.get(), line.get(), nx, photo == PhotometricInterpretation::PALETTE);
              std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, line.get() + roi_x, roi_w);
              break;
            case 8:
              std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, scanline.get() + roi_x, roi_w);
              break;
            case 12:
              twelve2sixteen<T>(scanline.get(), line.get(), nx, photo == PhotometricInterpretation::PALETTE);
              std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w, line.get() + roi_x, roi_w * psiz);
              break;
            case 16:
              std::memcpy(inbuf.data() + (c * roi_h + (i - roi_y)) * roi_w,
                scanline.get() + roi_x * psiz,
                roi_w * psiz);
              break;
            default:;
            }
          }
        }
      }
      const auto contig = separateToContig<T>(std::move(inbuf), roi_w, roi_h, nc, roi_w);
      std::copy(contig.begin(), contig.end(), out.begin());
    }
  }
  return {};
}

static const float epsilon = 1.0e-4;

size_t epsilon_ceil(float a)
{
  if (fabs(floor(a) - a) < epsilon) { return static_cast<size_t>(floorf(a)); }
  return static_cast<size_t>(ceilf(a));
}

size_t epsilon_ceil_division(float a, float b)
{
  //
  // epsilontic:
  // if a/b is x.00002, the result will be floorf(x), otherwise ceilf(x)
  //
  if (fabs(floorf(a / b) - (a / b)) < epsilon) { return static_cast<size_t>(floorf(a / b)); }
  return static_cast<size_t>(ceilf(a / b));
}

size_t epsilon_floor(float a)
{
  if (fabs(ceilf(a) - a) < epsilon) { return static_cast<size_t>(ceilf(a)); }
  return static_cast<size_t>(floorf(a));
}

size_t epsilon_floor_division(float a, float b)
{
  //
  // epsilontic:
  // if a/b is x.9998, the result will be ceilf(x), otherwise floor(x)
  //
  if (fabs(ceilf(a / b) - (a / b)) < epsilon) { return static_cast<size_t>(ceilf(a / b)); }
  return static_cast<size_t>(floorf(a / b));
}

template<typename T>
static Result<void>
  read_tiled_data(TIFF *tif, int32_t roi_x, int32_t roi_y, uint32_t roi_w, uint32_t roi_h, std::span<T> out)
{
  uint16_t planar;
  TIFF_GET_FIELD(tif, TIFFTAG_PLANARCONFIG, &planar, PLANARCONFIG_CONTIG)
  uint32_t tile_width;
  uint32_t tile_length;
  TIFF_GET_FIELD(tif, TIFFTAG_TILEWIDTH, &tile_width, 0)
  TIFF_GET_FIELD(tif, TIFFTAG_TILELENGTH, &tile_length, 0)
  if ((tile_width == 0) || (tile_length == 0)) {
    return std::unexpected(
      SipiValueError{ ErrorCode::kMalformedInput, "Expected tiled image, but no tile dimension given!" });
  }

  uint32_t nx, ny, nc, bps;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &nx);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &ny);

  uint16_t stmp;
  TIFF_GET_FIELD(tif, TIFFTAG_SAMPLESPERPIXEL, &stmp, 1)
  nc = static_cast<uint32_t>(stmp);

  uint32_t ntiles_x = epsilon_ceil_division(static_cast<float>(nx), static_cast<float>(tile_width));
  uint32_t ntiles_y = epsilon_ceil_division(static_cast<float>(ny), static_cast<float>(tile_length));
  uint32_t ntiles = TIFFNumberOfTiles(tif);
  // For PLANARCONFIG_SEPARATE, each of the nc samples is stored as its own
  // set of spatial tiles, so the on-disk tile count is nc times the spatial
  // tile grid; for PLANARCONFIG_CONTIG it is just the spatial grid.
  const uint32_t expected_ntiles = ntiles_x * ntiles_y * (planar == PLANARCONFIG_SEPARATE ? nc : 1);
  if (ntiles != expected_ntiles) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "Number of tiles not consistent: expected " + std::to_string(expected_ntiles)
        + " (" + std::to_string(ntiles_x) + "x" + std::to_string(ntiles_y) + ", channels=" + std::to_string(nc) + ")"
        + ", got " + std::to_string(ntiles)
        + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
        + ", tile=" + std::to_string(tile_width) + "x" + std::to_string(tile_length) });
  }
  uint32_t starttile_x = epsilon_floor_division(static_cast<float>(roi_x), static_cast<float>(tile_width));
  uint32_t starttile_y = epsilon_floor_division(static_cast<float>(roi_y), static_cast<float>(tile_length));
  uint32_t endtile_x = epsilon_ceil_division(static_cast<float>(roi_x + roi_w), static_cast<float>(tile_width));
  uint32_t endtile_y = epsilon_ceil_division(static_cast<float>(roi_y + roi_h), static_cast<float>(tile_length));

  TIFF_GET_FIELD(tif, TIFFTAG_BITSPERSAMPLE, &stmp, 8)
  bps = static_cast<uint32_t>(stmp);

  if ((bps != 8) && (bps != 16)) {
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      std::to_string(bps) + " bits/sample not supported for tiled TIFFs (only 8 and 16 supported)"
        + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
        + ", channels=" + std::to_string(nc)
        + ", tile=" + std::to_string(tile_width) + "x" + std::to_string(tile_length) });
  }

  const tmsize_t tile_size_signed = TIFFTileSize(tif);
  if (tile_size_signed <= 0) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "TIFFTileSize returned a non-positive size for tile " + std::to_string(tile_width) + "x"
        + std::to_string(tile_length) + ", channels=" + std::to_string(nc) + ", bps=" + std::to_string(bps) });
  }
  const uint32_t tile_size = static_cast<uint32_t>(tile_size_signed);
  // For PLANARCONFIG_CONTIG, TIFFTileSize() already accounts for all nc
  // samples per tile, so plane_elems covers the whole interleaved tile. For
  // PLANARCONFIG_SEPARATE, TIFFTileSize() reports the size of a single-sample
  // plane, so nc of these are needed to hold one tile.
  const uint32_t plane_elems = (bps == 8) ? tile_size : (tile_size >> 1);
  // TIFFTileSize() is libtiff's view of the tile (from its own copy of the
  // directory); the copy loop below indexes the tile by SIPI's tile geometry
  // and channel count. A directory the two read differently (e.g. an
  // inconsistent BitsPerSample array) makes the libtiff buffer smaller than
  // the loop's extent and the copy reads past it.
  const uint64_t expected_plane_elems =
    static_cast<uint64_t>(tile_width) * tile_length * (planar == PLANARCONFIG_SEPARATE ? 1U : nc);
  if (plane_elems < expected_plane_elems) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "TIFFTileSize (" + std::to_string(tile_size) + " bytes) smaller than the tile geometry "
        + std::to_string(tile_width) + "x" + std::to_string(tile_length) + ", channels=" + std::to_string(nc)
        + ", bps=" + std::to_string(bps) });
  }

  if (out.size() != checked_buf_size_or_throw(roi_w, roi_h, nc, 1)) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
      "TIFF pixel buffer holds " + std::to_string(out.size()) + " samples, region needs " + std::to_string(roi_w) + "x"
        + std::to_string(roi_h) + ", channels=" + std::to_string(nc) });
  }

  auto tilebuf = std::make_unique<T[]>(plane_elems);
  for (uint32_t ty = starttile_y; ty < endtile_y; ++ty) {
    for (uint32_t tx = starttile_x; tx < endtile_x; ++tx) {
      const T *pixels = nullptr;
      std::unique_ptr<T[]> contigbuf;

      if (planar == PLANARCONFIG_SEPARATE) {
        // Each of the nc samples lives in its own plane on disk, so it needs
        // its own TIFFReadTile() call (sample index as the 6th argument) into
        // its own plane_elems-sized slot. Reading only sample 0 into a
        // one-plane buffer and then treating it as nc interleaved planes (the
        // previous behavior) discloses (nc - 1) * plane_elems bytes of
        // adjacent heap.
        auto planebuf = std::make_unique<T[]>(static_cast<size_t>(nc) * plane_elems);
        for (uint32_t c = 0; c < nc; ++c) {
          if (TIFFReadTile(tif, planebuf.get() + static_cast<size_t>(c) * plane_elems, tx * tile_width,
                ty * tile_length, 0, static_cast<uint16_t>(c))
              < 0) {
            return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
              "TIFFReadTile failed on tile (" + std::to_string(tx) + ", " + std::to_string(ty) + "), sample "
                + std::to_string(c) + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
                + ", channels=" + std::to_string(nc) + ", bps=" + std::to_string(bps) });
          }
        }
        contigbuf = separateToContig(std::move(planebuf), tile_width, tile_length, nc, tile_width);
        pixels = contigbuf.get();
      } else {
        if (TIFFReadTile(tif, tilebuf.get(), tx * tile_width, ty * tile_length, 0, 0) < 0) {
          return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
            "TIFFReadTile failed on tile (" + std::to_string(tx) + ", " + std::to_string(ty) + ")"
              + ", dimensions=" + std::to_string(nx) + "x" + std::to_string(ny)
              + ", channels=" + std::to_string(nc) + ", bps=" + std::to_string(bps) });
        }
        pixels = tilebuf.get();
      }

      for (uint32_t tile_x = 0; tile_x < tile_width; tile_x++) {
        for (uint32_t tile_y = 0; tile_y < tile_length; tile_y++) {
          uint32_t final_x = (tx - starttile_x) * tile_width + tile_x;
          uint32_t final_y = (ty - starttile_y) * tile_length + tile_y;

          if (final_x < roi_w && final_y < roi_h) {
            uint32_t pixel_offset = nc * (final_y * roi_w + final_x);

            for (uint32_t c = 0; c < nc; ++c) {
              out[pixel_offset + c] = pixels[(tile_x + tile_y * tile_width) * nc + c];
            }
          }
        }
      }
    }
  }
  return {};
}
// TIFFTAG_JPEGCOLORMODE is a pseudo-tag owned by libtiff's JPEG codec and exists
// only while that codec is bound to the directory. TIFFSetDirectory re-binds the
// codec for the target directory and libtiff resets the pseudo-tag to
// JPEGCOLORMODE_RAW, so it must be re-requested after every directory switch on a
// JPEG-compressed TIFF, or TIFFReadScanline rejects downsampled JPEG data.
static void set_jpeg_colormode_if_jpeg(TIFF *tif)
{
  uint16_t compression = COMPRESSION_NONE;
  TIFF_GET_FIELD(tif, TIFFTAG_COMPRESSION, &compression, COMPRESSION_NONE);
  if (compression == COMPRESSION_JPEG) { TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB); }
}

// get the resolutions of pyramid if available
std::vector<SubImageInfo> read_resolutions(uint64_t image_width, TIFF *tif)
{
  std::vector<SubImageInfo> resolutions;
  do {
    uint32_t tmp_width;
    uint32_t tmp_height;
    uint32_t tile_width;
    uint32_t tile_length;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &tmp_width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &tmp_height);
    if (TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_width) != 1) { tile_width = 0; }
    if (TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_length) != 1) { tile_length = 0; }
    uint32_t reduce_w = std::lroundf(static_cast<float>(image_width) / static_cast<float>(tmp_width));
    uint32_t reduce = reduce_w;
    resolutions.push_back({ reduce, tmp_width, tmp_height, tile_width, tile_length });
  } while (TIFFReadDirectory(tif));

  TIFFSetDirectory(tif, 0);
  set_jpeg_colormode_if_jpeg(tif);

  return resolutions;
}

uint32_t select_pyramid_level(const std::vector<SubImageInfo> &resolutions, int reduce_exp)
{
  if (resolutions.empty() || reduce_exp <= 0) return 0;
  // Clamp the exponent so `1u << reduce_exp` can never overflow; a divisor larger
  // than the pyramid depth just selects the smallest level via the loop below.
  if (reduce_exp > 31) reduce_exp = 31;
  const uint32_t divisor = 1u << static_cast<uint32_t>(reduce_exp);

  uint32_t level = 0;
  for (uint32_t i = 0; i < resolutions.size(); ++i) {
    if (resolutions[i].reduce <= divisor) {
      level = i;
    } else {
      break;
    }
  }
  return level;
}

#include <iostream>
std::ostream &operator<<(std::ostream &os, const SubImageInfo &s)
{
  return os << "Reduce: " << s.reduce << ", "
            << "Width: " << s.width << ", "
            << "Height: " << s.height << ", "
            << "TW: " << s.tile_width << ", "
            << "TH:" << s.tile_height;
}

/**
 * TODO: SipiImage always assumes the image data to be in big endian format.
 * TIFF files can be in little endian format. Every TIFF file begins with a two-byte indicator of byte order:
 * "II" for little-endian (a.k.a. "Intel byte ordering" or "MM" for big-endian (a.k.a. "Motorola byte ordering" byte
 * ordering. I don't see where this is handled in the code.
 */
Result<bool> SipiIOTiff::read(SipiImage *img,
  const std::string &filepath,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size,
  bool force_bps_8,
  ScalingQuality scaling_quality)
{
  return read_image(
    img, filepath, std::move(region), std::move(size), force_bps_8, scaling_quality, /*decode_pixels=*/true);
}

Result<bool> SipiIOTiff::read_metadata(SipiImage *img, const std::string &filepath)
{
  return read_image(img,
    filepath,
    nullptr,
    nullptr,
    false,
    { ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH },
    /*decode_pixels=*/false);
}

Result<bool> SipiIOTiff::read_image(SipiImage *img,
  const std::string &filepath,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size,
  bool force_bps_8,
  ScalingQuality scaling_quality,
  bool decode_pixels)
{
  SIPI_ZONE_N("SipiIOTiff::read");
  TiffDiagnosticSink tiff_diag;
  TiffOpenOptionsPtr tiff_opts = make_tiff_open_options(tiff_diag);
  std::unique_ptr<TIFF, decltype(&TIFFClose)> tif_guard(TIFFOpenExt(filepath.c_str(), "r", tiff_opts.get()), TIFFClose);

  if (tif_guard != nullptr) {
    TIFF *tif = tif_guard.get();

    set_jpeg_colormode_if_jpeg(tif);

    //
    // OK, it's a TIFF file
    //
    uint16_t safo, ori, planar, stmp;

    // TIFFGetField writes a uint32_t through these two pointers; the image's
    // width/height are size_t, so passing their addresses directly leaves the
    // upper 32 bits (on a 64-bit size_t) uninitialized. Read into local
    // uint32_t out-params first.
    uint32_t tiff_width = 0, tiff_height = 0;
    if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &tiff_width) == 0) {
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kMalformedInput, "TIFFGetField of TIFFTAG_IMAGEWIDTH failed: " + filepath },
        tiff_diag));
    }

    if (TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &tiff_height) == 0) {
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kMalformedInput, "TIFFGetField of TIFFTAG_IMAGELENGTH failed: " + filepath },
        tiff_diag));
    }

    TIFF_GET_FIELD(tif, TIFFTAG_SAMPLESPERPIXEL, &stmp, 1);
    const size_t tiff_nc = static_cast<size_t>(stmp);

    TIFF_GET_FIELD(tif, TIFFTAG_BITSPERSAMPLE, &stmp, 1);
    const size_t tiff_bps = static_cast<size_t>(stmp);

    img->set_geometry(tiff_width, tiff_height, tiff_nc, tiff_bps);

    if (auto r = validate_decode_dims(img->getNx(), img->getNy(), img->getNc(), static_cast<int>(img->getBps()), filepath);
        !r) {
      return std::unexpected(with_tiff_diagnostic(std::move(r).error(), tiff_diag));
    }

    TIFF_GET_FIELD(tif, TIFFTAG_ORIENTATION, &ori, ORIENTATION_TOPLEFT);
    img->setOrientation(static_cast<Orientation>(ori));

    if (1 != TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &stmp)) {
      img->setPhoto(PhotometricInterpretation::MINISBLACK);
    } else {
      img->setPhoto(static_cast<PhotometricInterpretation>(stmp));
    }

    // With TIFFTAG_JPEGCOLORMODE set to JPEGCOLORMODE_RGB, libtiff's JPEG codec
    // converts YCbCr scanlines to RGB before handing them back; TIFFTAG_PHOTOMETRIC
    // keeps reporting the file's on-disk YCbCr encoding regardless. The image's
    // photometric interpretation drives downstream colorspace/ICC handling, so
    // it must track the decoded pixel layout.
    uint16_t decode_compression = COMPRESSION_NONE;
    TIFF_GET_FIELD(tif, TIFFTAG_COMPRESSION, &decode_compression, COMPRESSION_NONE);
    if (decode_compression == COMPRESSION_JPEG && img->getPhoto() == PhotometricInterpretation::YCBCR) {
      img->setPhoto(PhotometricInterpretation::RGB);
    }

    //
    // if we have a palette TIFF with a colormap, it gets complicated. We will have to
    // read the colormap and later convert the image to RGB, since we do internally
    // not support palette images.
    //


    std::vector<uint16_t> rcm;
    std::vector<uint16_t> gcm;
    std::vector<uint16_t> bcm;

    size_t colmap_len = 0;
    if (img->getPhoto() == PhotometricInterpretation::PALETTE) {
      uint16_t *_rcm = nullptr, *_gcm = nullptr, *_bcm = nullptr;
      if (TIFFGetField(tif, TIFFTAG_COLORMAP, &_rcm, &_gcm, &_bcm) == 0) {
        return std::unexpected(with_tiff_diagnostic(
          SipiValueError{ ErrorCode::kMalformedInput, "TIFFGetField of TIFFTAG_COLORMAP failed: " + filepath },
          tiff_diag));
      }
      // The image's bps was already validated above (validate_decode_dims) to
      // be one of {1, 4, 8, 12, 16}, so 1 << bps cannot overflow size_t.
      colmap_len = static_cast<size_t>(1) << img->getBps();
      rcm.resize(colmap_len);
      gcm.resize(colmap_len);
      bcm.resize(colmap_len);
      for (size_t ii = 0; ii < colmap_len; ii++) {
        rcm[ii] = _rcm[ii];
        gcm[ii] = _gcm[ii];
        bcm[ii] = _bcm[ii];
      }
    }

    TIFF_GET_FIELD(tif, TIFFTAG_PLANARCONFIG, &planar, PLANARCONFIG_CONTIG);
    TIFF_GET_FIELD(tif, TIFFTAG_SAMPLEFORMAT, &safo, SAMPLEFORMAT_UINT);

    uint16_t *es;
    uint16_t eslen = 0;
    if (TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &eslen, &es) == 1) {
      for (uint16_t i = 0; i < eslen; i++) {
        ExtraSamples extra;
        switch (es[i]) {
        case 0:
          extra = ExtraSamples::UNSPECIFIED;
          break;
        case 1:
          extra = ExtraSamples::ASSOCALPHA;
          break;
        case 2:
          extra = ExtraSamples::UNASSALPHA;
          break;
        default:
          extra = ExtraSamples::UNSPECIFIED;
        }
        img->addEs(extra);
      }
    }

    //
    // reading TIFF Meatdata and adding the fields to the exif header.
    // We store the TIFF metadata in the private exifData member variable using addKeyVal.
    //

    char *str;

    if (1 == TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.ImageDescription"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_MAKE, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.Make"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_MODEL, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.Model"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_SOFTWARE, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.Software"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_DATETIME, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.DateTime"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_ARTIST, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.Artist"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_HOSTCOMPUTER, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.HostComputer"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_COPYRIGHT, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.Copyright"), std::string(str));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_DOCUMENTNAME, &str)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.DocumentName"), std::string(str));
    }
    float f;
    if (1 == TIFFGetField(tif, TIFFTAG_XRESOLUTION, &f)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.XResolution"), Exif::toRational(f));
    }
    if (1 == TIFFGetField(tif, TIFFTAG_YRESOLUTION, &f)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.YResolution"), Exif::toRational(f));
    }

    // libtiff writes a uint16_t here; `short` is already the same width
    // (16 bits) so there is no memory-corruption risk from the signedness
    // mismatch — unlike `int`/`size_t` used for wider fields elsewhere in
    // this function. Keep `short` (not `uint16_t`): it selects the same
    // Exiv2::addKeyVal overload the approval-test goldens were pinned
    // against; switching to `uint16_t` changes the written EXIF value's
    // Exiv2 type tag (signed vs unsigned SHORT) and shifts encoded output
    // bytes for a well-formed decode.
    short s;
    if (1 == TIFFGetField(tif, TIFFTAG_RESOLUTIONUNIT, &s)) {
      img->exif_writable()->addKeyVal(std::string("Exif.Image.ResolutionUnit"), s);
    }

    //
    // read iptc header
    //
    unsigned int iptc_length = 0;
    unsigned char *iptc_content = nullptr;

    if (TIFFGetField(tif, TIFFTAG_RICHTIFFIPTC, &iptc_length, &iptc_content) != 0) {
      // A malformed IPTC block is fatal: SIPI is a repository and must not
      // admit corrupt embedded metadata. tif_guard closes the TIFF handle on
      // this early return.
      if (auto iptc = Iptc::parse(iptc_content, iptc_length)) {
        img->set_iptc(*iptc);
      } else {
        return std::unexpected(SipiValueError{ iptc.error().code(), filepath + ": " + iptc.error().raw_message() });
      }
    }

    //
    // read exif here....
    //
    toff_t exif_ifd_offs;
    if (1 == TIFFGetField(tif, TIFFTAG_EXIFIFD, &exif_ifd_offs)) { readExif(img, tif, exif_ifd_offs); }

    //
    // read xmp header
    //
    unsigned int xmp_length;
    char *xmp_content = nullptr;

    if (1 == TIFFGetField(tif, TIFFTAG_XMLPACKET, &xmp_length, &xmp_content)) {
      if (xmp_length > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
        log_warn("TIFF XMLPACKET (XMP) packet of %u bytes exceeds INT_MAX; skipping", xmp_length);
      } else {
        img->set_xmp(std::make_shared<Xmp>(xmp_content, static_cast<int>(xmp_length)));
      }
    }

    //
    // Read ICC-profile
    //
    unsigned int icc_len;
    unsigned char *icc_buf;
    float *whitepoint_ti = nullptr;
    float whitepoint[2];

    if (1 == TIFFGetField(tif, TIFFTAG_ICCPROFILE, &icc_len, &icc_buf)) {
      // A malformed ICC profile is fatal: SIPI is a repository and must not
      // admit corrupt embedded metadata. tif_guard closes the TIFF handle on
      // this early return.
      if (auto icc = Icc::parse(icc_buf, icc_len)) {
        img->set_icc(*icc);
      } else {
        return std::unexpected(icc.error());
      }
    } else if (1 == TIFFGetField(tif, TIFFTAG_WHITEPOINT, &whitepoint_ti)) {
      whitepoint[0] = whitepoint_ti[0];
      whitepoint[1] = whitepoint_ti[1];
      //
      // Wow, we have TIFF colormetry..... Who is still using this???
      //
      float *primaries_ti = nullptr;
      float primaries[6];

      if (1 == TIFFGetField(tif, TIFFTAG_PRIMARYCHROMATICITIES, &primaries_ti)) {
        primaries[0] = primaries_ti[0];
        primaries[1] = primaries_ti[1];
        primaries[2] = primaries_ti[2];
        primaries[3] = primaries_ti[3];
        primaries[4] = primaries_ti[4];
        primaries[5] = primaries_ti[5];
      } else {
        //
        // not defined, let's take the sRGB primaries
        //
        primaries[0] = 0.6400;
        primaries[1] = 0.3300;
        primaries[2] = 0.3000;
        primaries[3] = 0.6000;
        primaries[4] = 0.1500;
        primaries[5] = 0.0600;
      }

      // Transfer functions are only meaningful for 8-bit and 16-bit images.
      // For 1-bit / 4-bit images the buffer `3 * (1 << bps)` is only 6 / 48
      // shorts respectively; a malformed TRANSFERFUNCTION tag with a larger
      // payload would overflow the heap allocation below. We deliberately
      // allowlist 8 and 16 (rather than using `bps >= 8`) so the intent is
      // documented in the code.
      if (img->getBps() == 8 || img->getBps() == 16) {
        // RAII-wrap the transfer-function buffer so that an exception from
        // the Icc constructor cannot leak it. The `tfunc_ti_*` pointers are
        // owned by libtiff and must not be freed; only our copy (`tfunc`) is
        // owned here.
        //
        // libtiff's TIFFTAG_TRANSFERFUNCTION has NO count argument: it
        // always fills 3 `uint16_t*` out-pointers, each pointing to a
        // `1 << bps`-entry table (libtiff tif_dir.c TIFFVGetField); for a
        // grayscale image (SamplesPerPixel - ExtraSamples == 1) the 2nd and
        // 3rd pointers alias the 1st. The previous code passed
        // `&tfunc_len_ti` (an `unsigned int*`) as the first vararg, so
        // libtiff wrote an 8-byte pointer into that 4-byte slot and the
        // following memcpy read through whatever garbage pointer resulted.
        // Passing fewer than 3 out-pointer slots here under-reads the
        // varargs and crashes — verified empirically against the vendored
        // libtiff, not merely inferred from the tag's field-info table.
        const size_t table_len = static_cast<size_t>(1) << img->getBps();
        auto tfunc = std::make_unique<unsigned short[]>(3 * table_len);
        unsigned int tfunc_len = 0;
        bool has_tfunc = false;

        const int color_channels = static_cast<int>(img->getNc()) - static_cast<int>(eslen);
        uint16_t *tfunc_ti_0 = nullptr;
        uint16_t *tfunc_ti_1 = nullptr;
        uint16_t *tfunc_ti_2 = nullptr;
        const int got = TIFFGetField(tif, TIFFTAG_TRANSFERFUNCTION, &tfunc_ti_0, &tfunc_ti_1, &tfunc_ti_2);

        if (1 == got && tfunc_ti_0 != nullptr) {
          has_tfunc = true;
          memcpy(tfunc.get(), tfunc_ti_0, table_len * sizeof(unsigned short));
          if (color_channels == 1 || tfunc_ti_1 == nullptr || tfunc_ti_2 == nullptr) {
            // Grayscale table: replicate across all three ICC channels.
            memcpy(tfunc.get() + table_len, tfunc_ti_0, table_len * sizeof(unsigned short));
            memcpy(tfunc.get() + 2 * table_len, tfunc_ti_0, table_len * sizeof(unsigned short));
          } else {
            memcpy(tfunc.get() + table_len, tfunc_ti_1, table_len * sizeof(unsigned short));
            memcpy(tfunc.get() + 2 * table_len, tfunc_ti_2, table_len * sizeof(unsigned short));
          }
          tfunc_len = static_cast<unsigned int>(table_len);
        }

        // Colour tags that cannot be synthesized into an ICC profile are
        // fatal: SIPI is a repository and must not admit corrupt embedded
        // metadata. tif_guard closes the TIFF handle on this early return;
        // tfunc is a unique_ptr and needs no manual release.
        if (auto icc =
              Icc::createRGB(whitepoint, primaries, has_tfunc ? tfunc.get() : nullptr, has_tfunc ? tfunc_len : 0)) {
          img->set_icc(*icc);
        } else {
          return std::unexpected(icc.error());
        }
      } else {
        // bilevel / 4-bit / non-standard — no transfer function. See the
        // comment above: a synthesis failure is fatal here too.
        if (auto icc = Icc::createRGB(whitepoint, primaries, nullptr, 0)) {
          img->set_icc(*icc);
        } else {
          return std::unexpected(icc.error());
        }
      }
    }

    //
    // Read SipiEssential metadata. Prefer the new TIFFTAG_SIPIMETA_PB
    // (protobuf); fall back to the legacy TIFFTAG_SIPIMETA (pipe-delimited
    // ASCII) when the new tag is absent or fails to parse. No in-tree writer
    // ever emits both — the simultaneous presence of both tags would imply
    // external tooling, and the prefer-new behaviour is the correct
    // resolution either way.
    //
    {
      uint32_t pb_count = 0;
      const void *pb_data = nullptr;
      char *emdatastr = nullptr;
      const bool has_pb = 1 == TIFFGetField(tif, TIFFTAG_SIPIMETA_PB, &pb_count, &pb_data) && pb_count > 0 && pb_data;
      const bool has_legacy = 1 == TIFFGetField(tif, TIFFTAG_SIPIMETA, &emdatastr) && emdatastr && strlen(emdatastr) > 0;

      if (has_pb) {
        std::span<const std::byte> bytes(static_cast<const std::byte *>(pb_data), pb_count);
        auto parsed = Essentials::parse(bytes);
        if (parsed) {
          img->essential_metadata(*parsed);
        } else {
          log_warn("Essentials: protobuf parse failed for %s (variant=%d); falling back to legacy carrier",
            filepath.c_str(),
            static_cast<int>(parsed.error()));
          if (has_legacy) { img->essential_metadata(Essentials::parse_legacy(emdatastr)); }
        }
      } else if (has_legacy) {
        img->essential_metadata(Essentials::parse_legacy(emdatastr));
      }
    }

    auto resolutions = read_resolutions(img->getNx(), tif);
    int reduce = -1;

    size_t w = img->getNx(), h = img->getNy();
    size_t out_w, out_h;
    bool redonly;
    bool is_tiled;
    uint32_t level = 0;

    if (size) {
      // get_size hands back `reduce` as a log2 exponent (0, 1, 2, 3 …). Pick the
      // pyramid IFD whose reduction ratio is the largest available not exceeding
      // 2^reduce, then decode from that directory. Any residual between the level
      // ratio and the requested size is handled by the downstream size stage.
      size->get_size(w, h, out_w, out_h, reduce, redonly);

      level = select_pyramid_level(resolutions, reduce);
      TIFFSetDirectory(tif, level);
      set_jpeg_colormode_if_jpeg(tif);

      img->set_geometry(resolutions[level].width, resolutions[level].height, img->getNc(), img->getBps());

      // crop_coords maps a full-resolution region into this level's coordinate
      // space by dividing by the level's ratio (1, 2, 4, 8 …), NOT by the log2
      // exponent. Passing the exponent here is the historical "region + pct:50"
      // bug; the ratio is always ≥ 1 so there is no division by zero.
      if (region != nullptr) { region->set_reduce(static_cast<float>(resolutions[level].reduce)); }
    }
    is_tiled = (resolutions[level].tile_width != 0) && (resolutions[level].tile_height != 0);

    if (level > 0) { observability::Metrics::instance().tiff_pyramid_reduced_decodes_total.Increment(); }

    int32_t roi_x;
    int32_t roi_y;
    size_t roi_w;
    size_t roi_h;
    if (region == nullptr) {
      roi_x = 0;
      roi_y = 0;
      roi_w = img->getNx();
      roi_h = img->getNy();
    } else {
      region->crop_coords(img->getNx(), img->getNy(), roi_x, roi_y, roi_w, roi_h);
    }

    int ps;// pixel size in bytes
    switch (img->getBps()) {
    case 1:// 1-bit is converted to 8-bit on-the-fly by read_standard_data (one2eight<T>())
    case 8:
      ps = 1;
      break;
    case 16:
      ps = 2;
      break;
    default:
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kUnsupportedFormat,
          "Unsupported bits/sample (" + std::to_string(img->getBps()) + ") in file " + filepath },
        tiff_diag));
    }

    // The decoded bps can be promoted from the file's on-disk value (1-bit ->
    // 8-bit via one2eight<T>()); applied to the image together with the pixel
    // buffer via set_pixels(), which is the only path that keeps buffer and
    // geometry in lockstep, or alone when no pixels are decoded.
    const size_t decoded_bps = 8 * static_cast<size_t>(ps);
    if (decode_pixels) {
      std::vector<uint8_t> inbuf(checked_buf_size_or_throw(roi_w, roi_h, img->getNc(), static_cast<size_t>(ps)));
      Result<void> decoded{};
      if (img->getBps() <= 8) {
        const std::span<uint8_t> samples(inbuf);
        decoded = is_tiled ? read_tiled_data<uint8_t>(tif, roi_x, roi_y, roi_w, roi_h, samples)
                           : read_standard_data<uint8_t>(tif, roi_x, roi_y, roi_w, roi_h, samples);
      } else if (img->getBps() <= 16) {
        // The decoder writes into the buffer that set_pixels() takes: a second
        // full-size buffer doubles the peak heap of a multi-GB TIFF read.
        // operator new aligns that buffer for any fundamental type.
        static_assert(alignof(uint16_t) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
        const std::span<uint16_t> samples(reinterpret_cast<uint16_t *>(inbuf.data()), inbuf.size() / sizeof(uint16_t));
        decoded = is_tiled ? read_tiled_data<uint16_t>(tif, roi_x, roi_y, roi_w, roi_h, samples)
                           : read_standard_data<uint16_t>(tif, roi_x, roi_y, roi_w, roi_h, samples);
      }
      if (!decoded) { return std::unexpected(with_tiff_diagnostic(std::move(decoded).error(), tiff_diag)); }

      img->set_pixels(std::move(inbuf), roi_w, roi_h, img->getNc(), decoded_bps);
    } else {
      img->set_geometry(roi_w, roi_h, img->getNc(), decoded_bps);
    }
    tif_guard.reset();

    if (img->getPhoto() == PhotometricInterpretation::PALETTE) {
      //
      // ok, we have a palette color image we have to convert to RGB...
      //
      if (decode_pixels) {
        // Validate every decoded pixel value once, up front, against the
        // colormap size before the RGB-expansion loop below indexes into
        // rcm/gcm/bcm with it unchecked (DEV-6065).
        std::span<const byte> palette_pixels = img->pixels_view();
        for (size_t i = 0; i < img->getNx() * img->getNy(); i++) {
          if (static_cast<size_t>(palette_pixels[i]) >= colmap_len) {
            return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput,
              "Palette index " + std::to_string(palette_pixels[i]) + " out of range for colormap size "
                + std::to_string(colmap_len) + ": " + filepath });
          }
        }

        uint16_t cm_max = 0;
        for (size_t i = 0; i < colmap_len; i++) {
          if (rcm[i] > cm_max) cm_max = rcm[i];
          if (gcm[i] > cm_max) cm_max = gcm[i];
          if (bcm[i] > cm_max) cm_max = bcm[i];
        }
        std::vector<uint8_t> dataptr(checked_buf_size_or_throw(img->getNx(), img->getNy(), 3, 1));
        if (cm_max <= 256) {// we have a colomap with entries form 0 - 255
          for (size_t i = 0; i < img->getNx() * img->getNy(); i++) {
            dataptr[3 * i] = (uint8_t)rcm[palette_pixels[i]];
            dataptr[3 * i + 1] = (uint8_t)gcm[palette_pixels[i]];
            dataptr[3 * i + 2] = (uint8_t)bcm[palette_pixels[i]];
          }
        } else {// we have a colormap with entries > 255, assuming 16 bit
          for (size_t i = 0; i < img->getNx() * img->getNy(); i++) {
            dataptr[3 * i] = (uint8_t)(rcm[palette_pixels[i]] >> 8);
            dataptr[3 * i + 1] = (uint8_t)(gcm[palette_pixels[i]] >> 8);
            dataptr[3 * i + 2] = (uint8_t)(bcm[palette_pixels[i]] >> 8);
          }
        }
        img->set_pixels(std::move(dataptr), img->getNx(), img->getNy(), 3, img->getBps());
      } else {
        img->set_geometry(img->getNx(), img->getNy(), 3, img->getBps());
      }
      img->setPhoto(PhotometricInterpretation::RGB);
    }

    if (img->getIcc() == nullptr) {
      switch (img->getPhoto()) {
      case PhotometricInterpretation::MINISBLACK: {
        // read_standard_data<uint8_t>() already converts 1-bit to 8-bit via
        // one2eight<T>(); by the time we reach this block the image already
        // reports 8 bits per sample. The previous `cvrt1BitTo8Bit` call was
        // dead code.
        img->set_icc(std::make_shared<Icc>(icc_GRAY_D50));
        break;
      }

      case PhotometricInterpretation::MINISWHITE: {
        // Same as MINISBLACK above — 1-bit → 8-bit conversion already happened.
        img->set_icc(std::make_shared<Icc>(icc_GRAY_D50));
        break;
      }

      case PhotometricInterpretation::SEPARATED: {
        img->set_icc(std::make_shared<Icc>(icc_CMYK_standard));
        break;
      }

      case PhotometricInterpretation::YCBCR:// fall through!

      case PhotometricInterpretation::RGB: {
        img->set_icc(std::make_shared<Icc>(icc_sRGB));
        break;
      }

      case PhotometricInterpretation::CIELAB: {
        //
        // we have to convert to JPEG2000/littleCMS standard
        //
        const size_t lab_nc = img->getNc();
        const size_t lab_nx = img->getNx();
        const size_t lab_ny = img->getNy();
        if (img->getBps() == 8) {
          if (decode_pixels) {
            byte *pix = img->pixels_writable().data();
            for (size_t y = 0; y < lab_ny; y++) {
              for (size_t x = 0; x < lab_nx; x++) {
                union {
                  unsigned char u;
                  signed char s;
                } v{};
                v.u = pix[lab_nc * (y * lab_nx + x) + 1];
                pix[lab_nc * (y * lab_nx + x) + 1] = 128 + v.s;
                v.u = pix[lab_nc * (y * lab_nx + x) + 2];
                pix[lab_nc * (y * lab_nx + x) + 2] = 128 + v.s;
              }
            }
          }
          img->set_icc(std::make_shared<Icc>(icc_LAB));
        } else if (img->getBps() == 16) {
          if (decode_pixels) {
            auto *data = (unsigned short *)img->pixels_writable().data();
            for (size_t y = 0; y < lab_ny; y++) {
              for (size_t x = 0; x < lab_nx; x++) {
                union {
                  unsigned short u;
                  signed short s;
                } v{};
                v.u = data[lab_nc * (y * lab_nx + x) + 1];
                data[lab_nc * (y * lab_nx + x) + 1] = 32768 + v.s;
                v.u = data[lab_nc * (y * lab_nx + x) + 2];
                data[lab_nc * (y * lab_nx + x) + 2] = 32768 + v.s;
              }
            }
          }
          img->set_icc(std::make_shared<Icc>(icc_LAB));
        } else {
          return std::unexpected(SipiValueError{
            ErrorCode::kUnsupportedFormat, "Unsupported bits per sample (" + std::to_string(img->getBps()) + ")" });
        }
        break;
      }

      default: {
        return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
          "Unsupported photometric interpretation (" + to_string(img->getPhoto()) + ")" });
      }
      }
    }
    //
    // resize/Scale the image if necessary
    //
    if (size != NULL) {
      size_t nnx, nny;
      int reduce = -1;
      bool redonly;
      SipiSize::SizeType rtype = size->get_size(w, h, nnx, nny, reduce, redonly);
      if (rtype != SipiSize::FULL) {
        if (auto r = Sipi::processing::scale_with(
              *img, nnx, nny, scaling_method_for(SipiQualityFormat::TIF, scaling_quality));
            !r) {
          return std::unexpected(r.error());
        }
      }
    }
    if (force_bps_8) {
      if (auto r = processing::to8bps(*img); !r) { return std::unexpected(r.error()); }
    }
    return true;
  }
  return false;
}
//============================================================================


// The shape probe proper: reports a missing width/height tag as a
// SipiValueError value.
Result<SipiImgInfo> SipiIOTiff::read_shape(const std::string &filepath)
{
  SIPI_ZONE_N("SipiIOTiff::read_shape");
  SipiImgInfo info;
  TiffDiagnosticSink tiff_diag;
  TiffOpenOptionsPtr tiff_opts = make_tiff_open_options(tiff_diag);
  auto tif =
    std::unique_ptr<TIFF, decltype(&TIFFClose)>(TIFFOpenExt(filepath.c_str(), "r", tiff_opts.get()), TIFFClose);
  if (tif) {
    //
    // OK, it's a TIFF file
    //
    unsigned int tmp_width;

    if (TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &tmp_width) == 0) {
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kShapeProbeFailed, "TIFFGetField of TIFFTAG_IMAGEWIDTH failed: " + filepath },
        tiff_diag));
    }

    info.width = static_cast<size_t>(tmp_width);
    unsigned int tmp_height;

    if (TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &tmp_height) == 0) {
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kShapeProbeFailed, "TIFFGetField of TIFFTAG_IMAGELENGTH failed: " + filepath },
        tiff_diag));
    }
    info.height = tmp_height;
    info.success = SipiImgInfo::DIMS;

    unsigned short spp = 1, bps_val = 8;
    TIFFGetField(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetField(tif.get(), TIFFTAG_BITSPERSAMPLE, &bps_val);
    info.nc = spp;
    info.bps = bps_val;

    unsigned short ori;
    TIFF_GET_FIELD(tif.get(), TIFFTAG_ORIENTATION, &ori, ORIENTATION_TOPLEFT);
    info.orientation = static_cast<Orientation>(ori);

    // Essentials read for read_shape header probe. Prefer the new
    // TIFFTAG_SIPIMETA_PB protobuf tag and fall back to the legacy
    // TIFFTAG_SIPIMETA pipe-delimited form. When the packet's image-shape
    // fields are populated (later commits), the fast path returns from
    // here without calling read_resolutions().
    bool take_fast_path = false;
    bool essentials_parse_failed = false;
    bool essentials_partial = false;
    bool essentials_from_legacy = false;
    {
      uint32_t pb_count = 0;
      const void *pb_data = nullptr;
      char *emdatastr = nullptr;
      const bool has_pb =
        1 == TIFFGetField(tif.get(), TIFFTAG_SIPIMETA_PB, &pb_count, &pb_data) && pb_count > 0 && pb_data;
      const bool has_legacy =
        1 == TIFFGetField(tif.get(), TIFFTAG_SIPIMETA, &emdatastr) && emdatastr && strlen(emdatastr) > 0;

      if (has_pb) {
        std::span<const std::byte> bytes(static_cast<const std::byte *>(pb_data), pb_count);
        if (auto parsed = Essentials::parse(bytes)) {
          const auto &f = parsed->fields();
          info.origmimetype = f.mimetype;
          info.origname = f.origname;
          info.success = SipiImgInfo::ALL;
          // Fast path (ADR-0004): if BOTH img_w and img_h are
          // non-zero, hoist every shape field from the packet over what
          // TIFFGetField returned and skip read_resolutions(). Partial
          // population falls through to the slow path.
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
        } else if (has_legacy) {
          Essentials se = Essentials::parse_legacy(emdatastr);
          info.origmimetype = se.fields().mimetype;
          info.origname = se.fields().origname;
          info.success = SipiImgInfo::ALL;
          essentials_parse_failed = true;
          essentials_from_legacy = true;
        } else {
          essentials_parse_failed = true;
        }
      } else if (has_legacy) {
        Essentials se = Essentials::parse_legacy(emdatastr);
        info.origmimetype = se.fields().mimetype;
        info.origname = se.fields().origname;
        info.success = SipiImgInfo::ALL;
        essentials_from_legacy = true;
      }
    }

    if (take_fast_path) {
      Sipi::observability::read_shape_fast_path_counter(
        Sipi::observability::EssentialsFormat::Tiff,
        Sipi::observability::ReadShapeFastPathOutcome::Hit)
        .Increment();
      return info;
    }

    info.resolutions = read_resolutions(tmp_width, tif.get());

    // Slow-path outcome classification (ADR-0004). Precedence:
    // parse failure > partial > fallback (legacy carrier) > miss.
    using Outcome = Sipi::observability::ReadShapeFastPathOutcome;
    Outcome outcome = Outcome::Miss;
    if (essentials_parse_failed && !essentials_from_legacy) {
      outcome = Outcome::Fallback;
    } else if (essentials_partial) {
      outcome = Outcome::Partial;
    } else if (essentials_from_legacy) {
      outcome = Outcome::Fallback;
    }
    Sipi::observability::read_shape_fast_path_counter(
      Sipi::observability::EssentialsFormat::Tiff, outcome).Increment();
  }
  return info;
}
//============================================================================

void SipiIOTiff::write_basic_tags(const SipiImage &img,
  TIFF *tif,
  uint32_t nx,
  uint32_t ny,
  bool its_1_bit,
  const std::string &compression)
{
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(nx));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(ny));
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  if (its_1_bit) {
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)1);
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4);// that's out default....
  } else {
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, static_cast<uint16_t>(img.getBps()));
    if (compression == "COMPRESSION_LZW") {
      TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    } else if (compression == "COMPRESSION_DEFLATE") {
      TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE);
    }
  }
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(img.getNc()));
  const std::vector<ExtraSamples> &basic_tags_es = img.getEs();
  if (!basic_tags_es.empty()) {
    // libtiff expects uint16_t* for TIFFTAG_EXTRASAMPLES, not uint8_t*
    std::vector<uint16_t> es_uint16(basic_tags_es.size());
    for (size_t i = 0; i < basic_tags_es.size(); i++) { es_uint16[i] = static_cast<uint16_t>(basic_tags_es[i]); }
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(es_uint16.size()), es_uint16.data());
  }
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, img.getPhoto());
}

Result<void> SipiIOTiff::write(SipiImage *img, const OutputSink &sink, const SipiCompressionParams *params)
{
  SIPI_ZONE_N("SipiIOTiff::write");
  // A streamed sink (callback/tee) and stdout both need an in-memory TIFF
  // (libtiff requires a seekable target); a real FilePath is written directly.
  // `filepath` is derived only for the file branch and the error messages.
  const bool streaming = is_streaming_sink(sink);
  const std::string filepath = streaming ? std::string("<http response>") : std::get<FilePath>(sink).path;

  // Declared before tif_guard: TIFFClose flushes through the memTiff*Proc
  // callbacks, so the MEMTIFF must outlive the TIFF handle.
  std::unique_ptr<MEMTIFF, decltype(&memTiffFree)> memtif_guard(nullptr, &memTiffFree);
  std::unique_ptr<TIFF, decltype(&TIFFClose)> tif_guard(nullptr, &TIFFClose);
  TiffDiagnosticSink tiff_diag;
  TiffOpenOptionsPtr tiff_opts = make_tiff_open_options(tiff_diag);
  auto rowsperstrip = (uint32_t)-1;
  if (streaming || (filepath == "stdout:")) {
    memtif_guard.reset(memTiffOpen());
    tif_guard.reset(TIFFClientOpenExt("MEMTIFF",
      "w",
      (thandle_t)memtif_guard.get(),
      memTiffReadProc,
      memTiffWriteProc,
      memTiffSeekProc,
      memTiffCloseProc,
      memTiffSizeProc,
      memTiffMapProc,
      memTiffUnmapProc,
      tiff_opts.get()));
    if (tif_guard == nullptr) {
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kWriteFailed, "TIFFClientOpen for in-memory TIFF failed!" }, tiff_diag));
    }
  } else {
    tif_guard.reset(TIFFOpenExt(filepath.c_str(), "w", tiff_opts.get()));
    if (tif_guard == nullptr) {
      std::string msg = "TIFFopen of \"" + filepath + "\" failed!";
      return std::unexpected(with_tiff_diagnostic(SipiValueError{ ErrorCode::kWriteFailed, msg }, tiff_diag));
    }
  }
  TIFF *tif = tif_guard.get();
  MEMTIFF *memtif = memtif_guard.get();
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(img->getNx()));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(img->getNy()));
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, rowsperstrip));
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  bool its_1_bit = false;
  if ((img->getPhoto() == PhotometricInterpretation::MINISWHITE)
      || (img->getPhoto() == PhotometricInterpretation::MINISBLACK)) {
    its_1_bit = true;

    if (img->getBps() == 8) {
      const byte *scan = img->pixels_view().data();
      for (size_t i = 0; i < img->getNx() * img->getNy(); i++) {
        if ((scan[i] != 0) && (scan[i] != 255)) { its_1_bit = false; }
      }
    } else if (img->getBps() == 16) {
      const word *scan = (const word *)img->pixels_view().data();
      for (size_t i = 0; i < img->getNx() * img->getNy(); i++) {
        if ((scan[i] != 0) && (scan[i] != 65535)) { its_1_bit = false; }
      }
    }

    if (its_1_bit) {
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)1);
      TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_CCITTFAX4);// that's out default....
    } else {
      TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)img->getBps());
    }
  } else {
    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)img->getBps());
  }
  if (img->getPhoto() == PhotometricInterpretation::CIELAB) {
    const size_t cielab_nc = img->getNc();
    const size_t cielab_nx = img->getNx();
    const size_t cielab_ny = img->getNy();
    if (img->getBps() == 8) {
      byte *pix = img->pixels_writable().data();
      for (size_t y = 0; y < cielab_ny; y++) {
        for (size_t x = 0; x < cielab_nx; x++) {
          union {
            unsigned char u;
            signed char s;
          } v{};
          v.s = pix[cielab_nc * (y * cielab_nx + x) + 1] - 128;
          pix[cielab_nc * (y * cielab_nx + x) + 1] = v.u;
          v.s = pix[cielab_nc * (y * cielab_nx + x) + 2] - 128;
          pix[cielab_nc * (y * cielab_nx + x) + 2] = v.u;
        }
      }
    } else if (img->getBps() == 16) {
      auto *data = (unsigned short *)img->pixels_writable().data();
      for (size_t y = 0; y < cielab_ny; y++) {
        for (size_t x = 0; x < cielab_nx; x++) {
          union {
            unsigned short u;
            signed short s;
          } v{};
          v.s = data[cielab_nc * (y * cielab_nx + x) + 1] - 32768;
          data[cielab_nc * (y * cielab_nx + x) + 1] = v.u;
          v.s = data[cielab_nc * (y * cielab_nx + x) + 2] - 32768;
          data[cielab_nc * (y * cielab_nx + x) + 2] = v.u;
        }
      }
    } else {
      return std::unexpected(with_tiff_diagnostic(
        SipiValueError{ ErrorCode::kUnsupportedFormat,
          "Unsupported bits per sample (" + std::to_string(img->getBps()) + ")" },
        tiff_diag));
    }

    // We don't want to add the ICC profile in this case (doesn't make sense!)
    img->set_icc(nullptr);
  }
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<uint16_t>(img->getNc()));

  const std::vector<ExtraSamples> &write_es = img->getEs();
  if (write_es.size() > 0) {
    // libtiff expects uint16_t* for TIFFTAG_EXTRASAMPLES, not uint8_t*
    std::vector<uint16_t> es_uint16(write_es.size());
    for (size_t i = 0; i < write_es.size(); i++) { es_uint16[i] = static_cast<uint16_t>(write_es[i]); }
    TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<uint16_t>(es_uint16.size()), es_uint16.data());
  }

  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, img->getPhoto());

  //
  // let's get the TIFF metadata if there is some. We stored the TIFF metadata in the exifData member variable!
  //
  std::shared_ptr<Exif> exif = img->getExif();
  if ((exif != nullptr) & static_cast<int>(!(img->getSkipMetadata() & SkipMetadata::SKIP_EXIF))) {
    std::string value;

    if (exif->getValByKey("Exif.Image.ImageDescription", value)) {
      TIFFSetField(tif, TIFFTAG_IMAGEDESCRIPTION, value.c_str());
    }

    if (exif->getValByKey("Exif.Image.Make", value)) { TIFFSetField(tif, TIFFTAG_MAKE, value.c_str()); }

    if (exif->getValByKey("Exif.Image.Model", value)) { TIFFSetField(tif, TIFFTAG_MODEL, value.c_str()); }

    if (exif->getValByKey("Exif.Image.Software", value)) { TIFFSetField(tif, TIFFTAG_SOFTWARE, value.c_str()); }

    if (exif->getValByKey("Exif.Image.DateTime", value)) { TIFFSetField(tif, TIFFTAG_DATETIME, value.c_str()); }

    if (exif->getValByKey("Exif.Image.Artist", value)) { TIFFSetField(tif, TIFFTAG_ARTIST, value.c_str()); }

    if (exif->getValByKey("Exif.Image.HostComputer", value)) {
      TIFFSetField(tif, TIFFTAG_HOSTCOMPUTER, value.c_str());
    }

    if (exif->getValByKey("Exif.Image.Copyright", value)) { TIFFSetField(tif, TIFFTAG_COPYRIGHT, value.c_str()); }

    if (exif->getValByKey("Exif.Image.DocumentName", value)) {
      TIFFSetField(tif, TIFFTAG_DOCUMENTNAME, value.c_str());
    }

    float f;

    if (exif->getValByKey("Exif.Image.XResolution", f)) { TIFFSetField(tif, TIFFTAG_XRESOLUTION, f); }

    if (exif->getValByKey("Exif.Image.YResolution", f)) { TIFFSetField(tif, TIFFTAG_YRESOLUTION, f); }

    short s;

    if (exif->getValByKey("Exif.Image.ResolutionUnit", s)) { TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, s); }
  }
  Essentials es = img->essential_metadata();
  std::shared_ptr<Icc> icc = img->getIcc();
  if (((icc != nullptr) || es.fields().use_icc) & (!(img->getSkipMetadata() & SKIP_ICC))) {
    std::vector<unsigned char> buf;
    try {
      if (es.fields().use_icc) {
        buf = es.fields().icc_profile;
      } else {
        buf = icc->iccBytes();
      }

      if (buf.size() > 0) { TIFFSetField(tif, TIFFTAG_ICCPROFILE, static_cast<uint32_t>(buf.size()), buf.data()); }
    } catch (SipiError &err) {
      log_err("%s", err.to_string().c_str());
    }
  }
  //
  // write IPTC data, if available
  //
  std::shared_ptr<Iptc> iptc = img->getIptc();
  if ((iptc != nullptr) & (!(img->getSkipMetadata() & SKIP_IPTC))) {
    try {
      std::vector<unsigned char> buf = iptc->iptcBytes();
      if (buf.size() > 0) { TIFFSetField(tif, TIFFTAG_RICHTIFFIPTC, static_cast<uint32_t>(buf.size()), buf.data()); }
    } catch (SipiError &err) {
      log_err("%s", err.to_string().c_str());
    }
  }

  //
  // write XMP data
  //
  std::shared_ptr<Xmp> xmp = img->getXmp();
  if ((xmp != nullptr) & (!(img->getSkipMetadata() & SKIP_XMP))) {
    try {
      std::string buf = xmp->xmpBytes();
      if (!buf.empty()) { TIFFSetField(tif, TIFFTAG_XMLPACKET, static_cast<uint32_t>(buf.size()), buf.c_str()); }
    } catch (SipiError &err) {
      log_err("%s", err.to_string().c_str());
    }
  }
  //
  // Essentials packet emission per ADR-0009 / ADR-0010: plain TIFF is an
  // Access File and MUST NOT carry the packet. The pyramidal branch emits
  // the new TIFFTAG_SIPIMETA_PB carrier (ADR-0005 / DEV-6410) gated on
  // `pyramid && es.is_set()` — the `convert service-file` command stamps
  // the packet on the SipiImage before write; other write paths leave it
  // unset and don't carry it. The legacy TIFFTAG_SIPIMETA tag is never
  // emitted from this writer anymore; readers retain it via the
  // dual-carrier path. The `Essentials es` declaration above
  // (line 1652) still feeds the ICC fallback at line 1653+.
  //
  const bool pyramid =
    params && params->contains(TIFF_Pyramid) && params->at(TIFF_Pyramid).compare("yes") == 0;
  // Essentials emit gate: the packet's presence in memory is the signal
  // (caller's responsibility, per ADR-0010). The pyramid check stays —
  // a plain TIFF carrying Essentials in memory would be a caller bug,
  // and Service Files only live in pyramidal TIFF per ADR-0009.
  const bool emit_essentials_pb = pyramid && es.is_set();
  if (emit_essentials_pb) {
    const std::vector<std::byte> bytes = es.serialize();
    TIFFSetField(tif, TIFFTAG_SIPIMETA_PB, static_cast<uint32_t>(bytes.size()), bytes.data());
  }
  // TIFFCheckpointDirectory(tif);
  if (its_1_bit) {
    unsigned int sll;
    Result<std::vector<unsigned char>> cvrt_result = cvrt8BitTo1bit(*img, sll);
    if (!cvrt_result.has_value()) {
      return std::unexpected(with_tiff_diagnostic(std::move(cvrt_result).error(), tiff_diag));
    }
    std::vector<unsigned char> buf = std::move(cvrt_result).value();

    for (size_t i = 0; i < img->getNy(); i++) { TIFFWriteScanline(tif, buf.data() + i * sll, (int)i, 0); }
  } else {
    if (!pyramid) {
      byte *pixel_data = img->pixels_writable().data();
      const size_t row_stride = img->getNc() * img->getNx() * (img->getBps() / 8);
      for (size_t i = 0; i < img->getNy(); i++) { TIFFWriteScanline(tif, pixel_data + i * row_stride, (int)i, 0); }
    } else {
      for (int reduce = 0; reduce <= 5; reduce += 1) {
        SipiSize size(reduce);
        size_t nnx;
        size_t nny;
        bool redonly;
        (void)size.get_size(img->getNx(), img->getNy(), nnx, nny, reduce, redonly);

        if (std::min(nnx, nny) <= 32) break;

        uint32_t tw = 0, th = tw;
        write_subfile(*img, tif, reduce, tw, th, "");
      }
    }
  }

  //
  // write exif data
  //
  if (exif != nullptr) {
    TIFFWriteDirectory(tif);
    Result<void> exif_result = writeExif(img, tif);
    if (!exif_result.has_value()) { return std::unexpected(std::move(exif_result).error()); }
  }
  // Close (and thereby flush) the TIFF before consuming the MEMTIFF buffer.
  tif_guard.reset();

  if (memtif != nullptr) {
    if (!streaming && filepath == "stdout:") {
      size_t n = 0;

      while (n < memtif->flen) {
        n += fwrite(&(memtif->data[n]), 1, memtif->flen - n > 10240 ? 10240 : memtif->flen - n, stdout);
      }

      fflush(stdout);
    } else if (streaming) {
      // The whole in-memory TIFF is broadcast to the sink in one write. A
      // non-zero return means the peer is gone: the body write failed.
      SinkStream stream{ sink };
      if (stream.write(memtif->data, memtif->flen) != 0) {
        return std::unexpected(
          SipiValueError{ ErrorCode::kClientAbort, "Client aborted HTTP response during TIFF write" });
      }
    } else {
      return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed, "Unknown output method: " + filepath + " !" });
    }
  }
  return {};
}
//============================================================================

template<typename T>
std::vector<T> doReduce(std::vector<T> &&inbuf, int reduce, size_t nx, size_t ny, size_t nc, size_t &nnx, size_t nny)
{
  SipiSize size(reduce);
  int r = 0;
  bool redonly;
  size.get_size(nx, ny, nnx, nny, r, redonly);
  auto outbuf = std::vector<T>(nnx * nny * nc);

  auto inbuf_raw = inbuf.data();
  auto outbuf_raw = outbuf.data();
  reduce = 1 << reduce;
  if (reduce <= 1) {
    memcpy(outbuf_raw, inbuf_raw, nnx * nny * nc * sizeof(T));
  } else {
    for (uint32_t y = 0; y < nny; ++y) {
      for (uint32_t x = 0; x < nnx; ++x) {
        for (uint32_t c = 0; c < nc; ++c) {
          uint32_t cnt = 0;
          uint32_t tmp = 0;
          for (uint32_t xx = 0; xx < reduce; ++xx) {
            for (uint32_t yy = 0; yy < reduce; ++yy) {
              if (((reduce * x + xx) < nx) && ((reduce * y + yy) < ny)) {
                tmp += static_cast<uint32_t>(inbuf[nc * ((reduce * y + yy) * nx + (reduce * x + xx)) + c]);
                ++cnt;
              }
            }
          }
          outbuf[nc * (y * nnx + x) + c] = (cnt > 0) ? tmp / cnt : 0;
        }
      }
    }
  }
  return outbuf;
}

void SipiIOTiff::write_subfile(const SipiImage &img,
  TIFF *tif,
  int level,
  uint32_t &tile_width,
  uint32_t &tile_height,
  const std::string &compression)
{
  SipiSize size(level);
  size_t nnx;
  size_t nny;
  bool redonly;
  /* try { */
  (void)size.get_size(img.getNx(), img.getNy(), nnx, nny, level, redonly);
  /* } catch (const IIIFSizeError &err) {} */

  write_basic_tags(img, tif, nnx, nny, false, compression);
  if (level > 0) { TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE); }

  if (tile_width == 0 || tile_height == 0) { TIFFDefaultTileSize(tif, &tile_width, &tile_height); }
  TIFFSetField(tif, TIFFTAG_TILEWIDTH, tile_width);
  TIFFSetField(tif, TIFFTAG_TILELENGTH, tile_height);

  tsize_t tilesize = TIFFTileSize(tif);

  // reduce resolution of image: A reduce factor can be given, 0=no scaling, 1=0.5, 2=0.25, 3=0.125,...
  const size_t subfile_nc = img.getNc();
  std::span<const byte> subfile_pixels = img.pixels_view();
  std::vector<uint8_t> nbuf(
    subfile_pixels.data(), subfile_pixels.data() + img.getNx() * img.getNy() * subfile_nc * img.getBps() / 8);

  if (level > 0) { nbuf = doReduce<uint8_t>(std::move(nbuf), level, img.getNx(), img.getNy(), subfile_nc, nnx, nny); }

  auto ntiles_x = static_cast<uint32_t>(ceilf(static_cast<float>(nnx) / static_cast<float>(tile_width)));
  auto ntiles_y = static_cast<uint32_t>(ceilf(static_cast<float>(nny) / static_cast<float>(tile_height)));

  auto tilebuf = std::vector<uint8_t>(tilesize);
  for (uint32_t ty = 0; ty < ntiles_y; ++ty) {
    for (uint32_t tx = 0; tx < ntiles_x; ++tx) {
      for (uint32_t y = 0; y < tile_height; ++y) {
        for (uint32_t x = 0; x < tile_width; ++x) {
          for (uint32_t c = 0; c < subfile_nc; ++c) {
            uint32_t xx = tx * tile_width + x;
            uint32_t yy = ty * tile_height + y;
            if ((xx < nnx) && (yy < nny)) {
              tilebuf[subfile_nc * (y * tile_width + x) + c] = nbuf[subfile_nc * (yy * nnx + xx) + c];
            } else {
              tilebuf[subfile_nc * (y * tile_width + x) + c] = 0;
            }
          }
        }
      }
      TIFFWriteTile(tif, static_cast<void *>(tilebuf.data()), tx * tile_width, ty * tile_height, 0, 0);
    }
  }
  TIFFWriteDirectory(tif);
}

void SipiIOTiff::readExif(SipiImage *img, TIFF *tif, toff_t exif_offset)
{
  uint16_t curdir = TIFFCurrentDirectory(tif);
  auto exif = img->exif_writable();

  if (TIFFReadEXIFDirectory(tif, exif_offset)) {
    for (int i = 0; i < exiftag_list_len; i++) {
      switch (exiftag_list[i].datatype) {
      case EXIF_DT_RATIONAL: {
        float f;
        if (TIFFGetField(tif, exiftag_list[i].tag_id, &f)) {
          Exiv2::Rational r = Exif::toRational(f);
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", r);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_UINT8: {
        unsigned char uc;
        if (TIFFGetField(tif, exiftag_list[i].tag_id, &uc)) {
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", uc);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_UINT16: {
        unsigned short us;
        if (TIFFGetField(tif, exiftag_list[i].tag_id, &us)) {
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", us);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_UINT32: {
        unsigned int ui;
        if (TIFFGetField(tif, exiftag_list[i].tag_id, &ui)) {
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", ui);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_STRING: {
        char *tmpstr = nullptr;
        if (TIFFGetField(tif, exiftag_list[i].tag_id, &tmpstr)) {
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", std::string(tmpstr));
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_RATIONAL_PTR: {
        // libtiff exposes RATIONAL[] EXIF tags two different ways: variable-count
        // tags use TIFFGetField(tif, tag, &count, &buf) and fixed-count tags use
        // TIFFGetField(tif, tag, &buf). The variadic API performs no type checking,
        // so passing the wrong arg shape causes UB. `exiftag_list[i].len == 0`
        // means variable-count; non-zero means a fixed count specified by EXIF.
        float *tmpbuf;
        uint16_t len;
        bool got = false;
        if (exiftag_list[i].len == 0) {
          got = TIFFGetField(tif, exiftag_list[i].tag_id, &len, &tmpbuf) != 0;
        } else {
          len = static_cast<uint16_t>(exiftag_list[i].len);
          got = TIFFGetField(tif, exiftag_list[i].tag_id, &tmpbuf) != 0;
        }
        if (got) {
          auto r = std::make_unique<Exiv2::Rational[]>(len);
          for (int j = 0; j < len; j++) { r[j] = Exif::toRational(tmpbuf[j]); }
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", r.get(), len);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_UINT8_PTR: {
        uint8_t *tmpbuf;
        uint16_t len;

        if (TIFFGetField(tif, exiftag_list[i].tag_id, &len, &tmpbuf)) {
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", tmpbuf, len);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_UINT16_PTR: {
        uint16_t *tmpbuf;
        uint16_t len;// in bytes !!
        if (TIFFGetField(tif, exiftag_list[i].tag_id, &len, &tmpbuf)) {
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", tmpbuf, len);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_UINT32_PTR: {
        uint32_t *tmpbuf;
        uint16_t len;
        if (TIFFGetField(tif, exiftag_list[i].tag_id, &len, &tmpbuf)) {
          try {
            exif->addKeyVal(exiftag_list[i].tag_id, "Photo", tmpbuf, len);
          } catch (const SipiError &err) {
            log_err("Error writing EXIF data: %s", err.to_string().c_str());
          }
        }
        break;
      }

      case EXIF_DT_PTR: {
        unsigned char *tmpbuf;
        uint16_t len;

        if (exiftag_list[i].len == 0) {
          if (TIFFGetField(tif, exiftag_list[i].tag_id, &len, &tmpbuf)) {
            try {
              exif->addKeyVal(exiftag_list[i].tag_id, "Photo", tmpbuf, len);
            } catch (const SipiError &err) {
              log_err("Error writing EXIF data: %s", err.to_string().c_str());
            }
          }
        } else {
          len = exiftag_list[i].len;
          if (TIFFGetField(tif, exiftag_list[i].tag_id, &tmpbuf)) {
            try {
              exif->addKeyVal(exiftag_list[i].tag_id, "Photo", tmpbuf, len);
            } catch (const SipiError &err) {
              log_err("Error writing EXIF data: %s", err.to_string().c_str());
            }
          }
        }
        break;
      }

      default: {
        // NO ACTION HERE At THE MOMENT...
      }
      }
    }
  }

  TIFFSetDirectory(tif, curdir);
}
//============================================================================


namespace {

// A 0-denominator EXIF rational with a non-zero numerator has no valid
// float representation and cannot be handed to TIFFSetField.
Result<void> zero_denominator_error(uint16_t tag_id)
{
  char hex[8];
  std::snprintf(hex, sizeof(hex), "0x%04x", tag_id);
  return std::unexpected(
    SipiValueError{ ErrorCode::kMetadataParseFailed, "EXIF tag " + std::string(hex) + ": zero denominator" });
}

}// namespace

Result<void> SipiIOTiff::writeExif(SipiImage *img, TIFF *tif)
{
  // add EXIF tags to the set of tags that libtiff knows about
  // necessary if we want to set EXIFTAG_DATETIMEORIGINAL, for example
  // const TIFFFieldArray *exif_fields = _TIFFGetExifFields();
  //_TIFFMergeFields(tif, exif_fields->fields, exif_fields->count);

  std::shared_ptr<Exif> exif = img->getExif();
  TIFFCreateEXIFDirectory(tif);
  int count = 0;
  for (int i = 0; i < exiftag_list_len; i++) {
    switch (exiftag_list[i].datatype) {
    case EXIF_DT_RATIONAL: {
      Exiv2::Rational r;

      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", r)) {
        // 0/0 is EXIF's "no value" marker (e.g. an unmeasured BrightnessValue);
        // n/0 with n != 0 has no valid float representation.
        if (r.second == 0) {
          if (r.first != 0) { return zero_denominator_error(exiftag_list[i].tag_id); }
        } else {
          float f = (float)r.first / (float)r.second;
          TIFFSetField(tif, exiftag_list[i].tag_id, f);
          count++;
        }
      }

      break;
    }

    case EXIF_DT_UINT8: {
      uint8_t uc;

      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", uc)) {
        TIFFSetField(tif, exiftag_list[i].tag_id, uc);
        count++;
      }

      break;
    }

    case EXIF_DT_UINT16: {
      uint16_t us;

      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", us)) {
        TIFFSetField(tif, exiftag_list[i].tag_id, us);
        count++;
      }

      break;
    }

    case EXIF_DT_UINT32: {
      uint32_t ui;

      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", ui)) {
        TIFFSetField(tif, exiftag_list[i].tag_id, ui);
        count++;
      }

      break;
    }

    case EXIF_DT_STRING: {
      std::string tmpstr;
      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", tmpstr)) {
        TIFFSetField(tif, exiftag_list[i].tag_id, tmpstr.c_str());
        count++;
      }

      break;
    }

    case EXIF_DT_RATIONAL_PTR: {
      // Symmetric with the read side. Fixed-count tags (LensSpecification etc.)
      // call TIFFSetField(tif, tag, buf) with no count argument; libtiff then
      // memcpys exactly `entry.len` floats from `buf` regardless of how many
      // we actually allocated. Skip with a warning when the source EXIF data
      // doesn't match the fixed count, otherwise libtiff reads past `f.get()`.
      std::vector<Exiv2::Rational> vr;
      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", vr)) {
        const auto len = vr.size();
        if (exiftag_list[i].len != 0 && len != static_cast<size_t>(exiftag_list[i].len)) {
          log_warn("EXIF tag 0x%04x: source has %zu rationals, spec requires %d — skipping write",
            exiftag_list[i].tag_id,
            len,
            exiftag_list[i].len);
          break;
        }
        // A fixed-count tag is written all-or-nothing: any n/0 element (n != 0)
        // is malformed for the whole tag; any 0/0 element ("no value") skips
        // the whole tag, since libtiff has no per-element "unknown" encoding.
        bool has_no_value = false;
        for (const auto &rat : vr) {
          if (rat.second == 0) {
            if (rat.first != 0) { return zero_denominator_error(exiftag_list[i].tag_id); }
            has_no_value = true;
          }
        }
        if (!has_no_value) {
          auto f = std::make_unique<float[]>(len);
          for (size_t j = 0; j < len; j++) {
            f[j] = static_cast<float>(vr[j].first) / static_cast<float>(vr[j].second);
          }
          if (exiftag_list[i].len == 0) {
            TIFFSetField(tif, exiftag_list[i].tag_id, static_cast<uint16_t>(len), f.get());
          } else {
            TIFFSetField(tif, exiftag_list[i].tag_id, f.get());
          }
          count++;
        }
      }
      break;
    }

    case EXIF_DT_UINT8_PTR: {
      std::vector<uint8_t> vuc;

      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", vuc)) {
        int len = vuc.size();
        TIFFSetField(tif, exiftag_list[i].tag_id, len, vuc.data());
        count++;
      }

      break;
    }

    case EXIF_DT_UINT16_PTR: {
      std::vector<uint16_t> vus;
      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", vus)) {
        int len = vus.size();
        TIFFSetField(tif, exiftag_list[i].tag_id, len, vus.data());
        count++;
      }
      break;
    }

    case EXIF_DT_UINT32_PTR: {
      std::vector<uint32_t> vui;

      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", vui)) {
        int len = vui.size();
        TIFFSetField(tif, exiftag_list[i].tag_id, len, vui.data());
        count++;
      }

      break;
    }

    case EXIF_DT_PTR: {
      std::vector<unsigned char> vuc;

      if (exif->getValByKey(exiftag_list[i].tag_id, "Photo", vuc)) {
        int len = vuc.size();
        TIFFSetField(tif, exiftag_list[i].tag_id, len, vuc.data());
        count++;
      }

      break;
    }

    default: {
      // NO ACTION HERE AT THE MOMENT...
    }
    }
  }

  if (count > 0) {
    uint64_t exif_dir_offset = 0L;
    TIFFWriteCustomDirectory(tif, &exif_dir_offset);
    TIFFSetDirectory(tif, 0);
    TIFFSetField(tif, TIFFTAG_EXIFIFD, exif_dir_offset);
  }
  // TIFFCheckpointDirectory(tif);
  return {};
}
//============================================================================

// cvrt1BitTo8Bit removed — read_standard_data<uint8_t>() converts bilevel to
// 8-bit on-the-fly via one2eight<T>(). The post-read conversion path was dead
// code (the image already reported 8 bits per sample by the time those call
// sites executed).
//============================================================================

Result<std::vector<unsigned char>> SipiIOTiff::cvrt8BitTo1bit(const SipiImage &img, unsigned int &sll)
{
  static unsigned char mask[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

  unsigned int x, y;

  if ((img.getPhoto() != PhotometricInterpretation::MINISWHITE)
      && (img.getPhoto() != PhotometricInterpretation::MINISBLACK)) {
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Photometric interpretation is not MINISWHITE or  MINISBLACK" });
  }

  if (img.getBps() != 8) {
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Bits per sample is not 8 but: " + std::to_string(img.getBps()) });
  }

  const size_t nx = img.getNx();
  const size_t ny = img.getNy();
  std::span<const byte> pix = img.pixels_view();
  sll = (nx + 7) / 8;

  if (img.getPhoto() == PhotometricInterpretation::MINISBLACK) {
    std::vector<unsigned char> outbuf(sll * ny, 0x00);
    for (y = 0; y < ny; y++) {
      for (x = 0; x < nx; x++) {
        outbuf[y * sll + (x / 8)] |= (pix[y * nx + x] > 128) ? mask[x % 8] : !mask[x % 8];
      }
    }
    return outbuf;
  }
  // must be MINISWHITE
  std::vector<unsigned char> outbuf(sll * ny, 0xff);
  for (y = 0; y < ny; y++) {
    for (x = 0; x < nx; x++) {
      outbuf[y * sll + (x / 8)] |= (pix[y * nx + x] > 128) ? !mask[x % 8] : mask[x % 8];
    }
  }
  return outbuf;
}
//============================================================================

}// namespace Sipi
