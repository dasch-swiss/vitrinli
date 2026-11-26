/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * Byte-exact output invariant (approval gate).
 *
 * The PNG encode path here is pinned byte-for-byte by the approval tests
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
#include <arpa/inet.h>
#include <cassert>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <cstring>

#include <png.h>
#include <tiff.h>
#include <zlib.h>

#include "logging/logger.h"
#include "error/SipiValueError.h"
#include "image/SipiIO.h"
#include "image/SipiImageError.h"
#include "image_processing/processing.h"
#include "SipiIOPng.h"
#include "observability/profiling.h"

// bad hack in order to include definitions in png.h on debian systems
#if !defined(PNG_TEXT_SUPPORTED)
#define PNG_TEXT_SUPPORTED 1
#endif
#if !defined(PNG_iTXt_SUPPORTED)
#define PNG_iTXt_SUPPORTED 1
#endif
#if !defined(PNG_eXIf_SUPPORTED)
#define PNG_eXIf_SUPPORTED 1
#endif

#define PNG_BYTES_TO_CHECK 4

namespace Sipi {

static char lang_en[] = "en";
static char iptc_tag[] = "Raw profile type iptc";
static char xmp_tag[] = "XML:com.adobe.xmp";
static char sipi_tag[] = "SIPI:io.sipi.essentials";

// ImageMagick's "raw profile" text-chunk convention: a name line, an 8-wide
// decimal length, then the profile bytes as lowercase hex wrapped at 72
// columns. Used here to carry IPTC (which has no dedicated PNG chunk) inside
// a text chunk without the NUL-truncation that binary data would suffer.
static std::string encode_raw_profile(const char *name, const std::vector<unsigned char> &data)
{
  std::string out;
  out += '\n';
  out += name;
  out += '\n';
  char len_buf[16];
  std::snprintf(len_buf, sizeof(len_buf), "%8lu", static_cast<unsigned long>(data.size()));
  out += len_buf;
  out += '\n';
  static const char hex_digits[] = "0123456789abcdef";
  for (size_t i = 0; i < data.size(); i++) {
    if (i > 0 && i % 36 == 0) { out += '\n'; }
    out += hex_digits[(data[i] >> 4) & 0x0f];
    out += hex_digits[data[i] & 0x0f];
  }
  out += '\n';
  return out;
}

static Result<std::vector<unsigned char>> decode_raw_profile(const char *text, size_t text_len)
{
  const char *p = text;
  const char *end = text + text_len;

  if (p >= end || *p != '\n') {
    return std::unexpected(
      SipiValueError{ ErrorCode::kMetadataParseFailed, "malformed raw profile in PNG: missing leading newline" });
  }
  ++p;// skip leading newline

  // skip the profile-name line
  while (p < end && *p != '\n') { ++p; }
  if (p >= end) {
    return std::unexpected(
      SipiValueError{ ErrorCode::kMetadataParseFailed, "malformed raw profile in PNG: missing name line" });
  }
  ++p;// skip newline after name

  // read the length line: leading spaces tolerated, terminated by '\n'
  const char *len_start = p;
  while (p < end && *p != '\n') { ++p; }
  if (p >= end) {
    return std::unexpected(
      SipiValueError{ ErrorCode::kMetadataParseFailed, "malformed raw profile in PNG: missing length line" });
  }
  std::string len_str(len_start, p);
  char *len_end = nullptr;
  const unsigned long length = std::strtoul(len_str.c_str(), &len_end, 10);
  if (len_end == len_str.c_str()) {
    return std::unexpected(
      SipiValueError{ ErrorCode::kMetadataParseFailed, "malformed raw profile in PNG: unparseable length" });
  }
  ++p;// skip newline after length

  std::vector<unsigned char> result;
  result.reserve(std::min(length, static_cast<size_t>((end - p) / 2 + 1)));
  int hi_nibble = -1;
  while (p < end && result.size() < length) {
    const char c = *p++;
    int nibble;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      nibble = c - 'A' + 10;
    } else {
      continue;// ignore embedded newlines/whitespace
    }
    if (hi_nibble < 0) {
      hi_nibble = nibble;
    } else {
      result.push_back(static_cast<unsigned char>((hi_nibble << 4) | nibble));
      hi_nibble = -1;
    }
  }
  return result;
}

//============== HELPER CLASS ==================
// NOTE: pointers returned by next() are invalidated by the following next()
// call; write the entry through them immediately (as add_zTXt/add_iTXt do).
class PngTextPtr
{
private:
  std::vector<png_text> text;

public:
  inline PngTextPtr(unsigned int len = 16) { text.reserve(len); };

  inline unsigned int num() const { return static_cast<unsigned int>(text.size()); };

  inline png_text *ptr() { return text.data(); };

  png_text *next();

  void add_zTXt(char *key, char *data, unsigned int len);

  void add_iTXt(char *key, char *data, unsigned int len);
};

png_text *PngTextPtr::next() { return &text.emplace_back(); }
//=============================================

void PngTextPtr::add_zTXt(char *key, char *data, unsigned int len)
{
  png_text *tmp = this->next();
  tmp->compression = PNG_TEXT_COMPRESSION_zTXt;
  tmp->key = key;
  tmp->text = (char *)data;
  tmp->text_length = len;
  tmp->itxt_length = 0;
  tmp->lang = (char *)"";
  tmp->lang_key = (char *)"";
}
//=============================================

void PngTextPtr::add_iTXt(char *key, char *data, unsigned int len)
{
  png_text *tmp = this->next();
  tmp->compression = PNG_ITXT_COMPRESSION_zTXt;
  tmp->key = key;
  tmp->text = data;
  tmp->text_length = 0;
  tmp->itxt_length = len;
  tmp->lang = (char *)"";
  tmp->lang_key = (char *)"";
}
//=============================================

void sipi_error_fn(png_structp png_ptr, png_const_charp error_msg)
{
  log_err("PNG error: %s", error_msg);
  // Use longjmp via png_jmpbuf — the canonical libpng error handling pattern.
  // Throwing C++ exceptions through libpng's C stack frames is undefined behavior.
  longjmp(png_jmpbuf(png_ptr), 1);
}

void sipi_warning_fn(png_structp png_ptr, png_const_charp warning_msg)
{
  log_warn("PNG warning: %s", warning_msg);
}

Result<bool> SipiIOPng::read(SipiImage *img,
  const std::string &filepath,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size,
  bool force_bps_8,
  ScalingQuality scaling_quality)
{
  return read_image(
    img, filepath, std::move(region), std::move(size), force_bps_8, scaling_quality, /*decode_pixels=*/true);
}

Result<bool> SipiIOPng::read_metadata(SipiImage *img, const std::string &filepath)
{
  return read_image(img,
    filepath,
    nullptr,
    nullptr,
    false,
    { ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH },
    /*decode_pixels=*/false);
}

Result<bool> SipiIOPng::read_image(SipiImage *img,
  const std::string &filepath,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size,
  bool force_bps_8,
  ScalingQuality scaling_quality,
  bool decode_pixels)
{
  SIPI_ZONE_N("SipiIOPng::read");
  unsigned char header[PNG_BYTES_TO_CHECK];
  png_structp png_ptr;
  png_infop info_ptr;

  //
  // open the input file. Objects that own a resource are declared before the
  // setjmp so that their destructors run on the normal C++ path out of the
  // landing block below.
  //
  auto infile = std::unique_ptr<FILE, decltype(&fclose)>(fopen(filepath.c_str(), "rb"), fclose);
  if (infile == nullptr) { return false; }

  //
  // check header if we really have a PNG file...
  //
  fread(header, 1, PNG_BYTES_TO_CHECK, infile.get());
  if (png_sig_cmp(header, 0, PNG_BYTES_TO_CHECK) != 0) {
    return false;// it's not a PNG file
  }

  if ((png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, (png_voidp) nullptr, sipi_error_fn, sipi_warning_fn))
      == nullptr) {
    return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
      "Error reading PNG file \"" + filepath + "\": Could not allocate memory for png_structp !" });
  }
  if ((info_ptr = png_create_info_struct(png_ptr)) == nullptr) {
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
      "Error reading PNG file \"" + filepath + "\": Could not allocate memory for png_infop !" });
  }

  // Bound per-chunk decompression and the number of cached ancillary chunks
  // (zTXt/iTXt/etc.) — without these, a hostile file can decompression-bomb
  // the chunk cache before any pixel data is read.
  png_set_chunk_malloc_max(png_ptr, png_alloc_size_t(16) << 20);
  png_set_chunk_cache_max(png_ptr, 1000);

  // Declared before the setjmp so that their destructors run on the normal
  // C++ path out of the landing block — a longjmp skips the destructors of
  // anything constructed inside the risk window (they are resized inside the
  // window, which is safe: the vector object's storage is stable memory, not
  // a register).
  std::vector<uint8_t> buffer;
  std::vector<png_bytep> row_pointers;

  // setjmp error recovery — sipi_error_fn calls longjmp(png_jmpbuf(png_ptr), 1)
  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed, "PNG read failed for \"" + filepath + "\"" });
  }

  png_init_io(png_ptr, infile.get());
  png_set_sig_bytes(png_ptr, PNG_BYTES_TO_CHECK);
  png_read_info(png_ptr, info_ptr);

  png_uint_32 width, height;
  int color_type, bit_depth, interlace_type;
  png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, NULL, NULL);
  img->setOrientation(TOPLEFT);

  png_set_packing(png_ptr);

  png_uint_32 res_x, res_y;
  int unit_type;
  if (png_get_pHYs(png_ptr, info_ptr, &res_x, &res_y, &unit_type)) {
    auto exif = img->exif_writable();
    float fres_x, fres_y;
    if (unit_type == PNG_RESOLUTION_METER) {
      fres_x = res_x / 39.37007874015748;
      fres_y = res_y / 39.37007874015748;
    } else {
      fres_x = res_x;
      fres_y = res_y;
    }
    exif->addKeyVal("Exif.Image.XResolution", Exif::toRational(fres_x));
    exif->addKeyVal("Exif.Image.YResolution", Exif::toRational(fres_y));
    exif->addKeyVal("Exif.Image.ResolutionUnit", 2);// DPI
  }

  switch (color_type) {
  case PNG_COLOR_TYPE_GRAY: {// implies nc = 1, (bit depths 1, 2, 4, 8, 16)
    png_set_expand_gray_1_2_4_to_8(png_ptr);
    img->setPhoto(PhotometricInterpretation::MINISBLACK);
    break;
  }
  case PNG_COLOR_TYPE_GRAY_ALPHA: {// implies nc = 2, (bit depths 8, 16)
    png_set_expand_gray_1_2_4_to_8(png_ptr);
    img->setPhoto(PhotometricInterpretation::MINISBLACK);
    break;
  }
  case PNG_COLOR_TYPE_PALETTE: {// tRNS may add alpha: see the extra sample after png_read_update_info
    png_set_palette_to_rgb(png_ptr);
    img->setPhoto(PhotometricInterpretation::RGB);
    break;
  }
  case PNG_COLOR_TYPE_RGB: {// implies nc = 3 (standard case :-), (bit_depths 8, 16)
    img->setPhoto(PhotometricInterpretation::RGB);
    break;
  }
  case PNG_COLOR_TYPE_RGBA: {// implies nc = 4, (bit_depths 8, 16)
    img->setPhoto(PhotometricInterpretation::RGB);
    break;
  }
  }
  if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS) != 0) png_set_tRNS_to_alpha(png_ptr);

  png_color_16 *image_background;
  if (png_get_bKGD(png_ptr, info_ptr, &image_background) != 0) {
    png_set_background(png_ptr, image_background, PNG_BACKGROUND_GAMMA_FILE, 1, 1.0);
  }

  png_read_update_info(png_ptr, info_ptr);

  const size_t decoded_bps = png_get_bit_depth(png_ptr, info_ptr);
  const size_t decoded_nc = png_get_channels(png_ptr, info_ptr);
  if (auto r = validate_decode_dims(width, height, decoded_nc, static_cast<int>(decoded_bps), filepath); !r) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    return std::unexpected(r.error());
  }
  img->set_geometry(width, height, decoded_nc, decoded_bps);
  // The alpha channel follows the decoded layout, not the colour type: tRNS
  // adds one, and bKGD composites the image and removes it.
  if (decoded_nc == 2 || decoded_nc == 4) { img->addEs(ExtraSamples::ASSOCALPHA); }

  //
  // check for ICC profiles...
  //
  int srgb_intent;
  if (png_get_sRGB(png_ptr, info_ptr, &srgb_intent) != 0) {
    img->set_icc(std::make_shared<Icc>(icc_sRGB));
  } else {
    png_charp name;
    int compression_type = PNG_COMPRESSION_TYPE_BASE;
    png_bytep profile;
    png_uint_32 proflen;
    if (png_get_iCCP(png_ptr, info_ptr, &name, &compression_type, &profile, &proflen) != 0) {
      auto icc = Icc::parse((unsigned char *)profile, (int)proflen);
      if (!icc) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        return std::unexpected(icc.error());
      }
      img->set_icc(*icc);
    }
  }

  // EXIF travels in the binary-safe eXIf chunk (libpng >= 1.6.32), carrying
  // the raw TIFF-header-relative byte stream exifBytes() emits directly.
  png_uint_32 num_exif = 0;
  png_bytep exif_data = nullptr;
  if (png_get_eXIf_1(png_ptr, info_ptr, &num_exif, &exif_data) != 0 && exif_data != nullptr && num_exif > 0) {
    // A PNG whose EXIF block does not parse is not decoded.
    if (auto exif = Exif::parse(exif_data, num_exif)) {
      img->set_exif(*exif);
    } else {
      png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
      return std::unexpected(exif.error());
    }
  }

  png_text *png_texts;
  int num_comments = png_get_text(png_ptr, info_ptr, &png_texts, nullptr);

  for (int i = 0; i < num_comments; i++) {
    if (strcmp(png_texts[i].key, xmp_tag) == 0) {
      // libpng reports an iTXt chunk's length in itxt_length and leaves text_length 0.
      const size_t xmp_len = png_texts[i].itxt_length > 0 ? png_texts[i].itxt_length : png_texts[i].text_length;
      img->set_xmp(std::make_shared<Xmp>(png_texts[i].text, static_cast<int>(xmp_len)));
    } else if (strcmp(png_texts[i].key, iptc_tag) == 0) {
      const size_t iptc_text_len =
        png_texts[i].text_length > 0 ? png_texts[i].text_length : strlen(png_texts[i].text);
      auto raw_profile = decode_raw_profile(png_texts[i].text, iptc_text_len);
      if (!raw_profile) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        return std::unexpected(raw_profile.error());
      }
      // A PNG whose IPTC block does not parse is not decoded.
      if (auto iptc = Iptc::parse(raw_profile->data(), (unsigned int)raw_profile->size())) {
        img->set_iptc(*iptc);
      } else {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        return std::unexpected(SipiValueError{ iptc.error().code(), filepath + ": " + iptc.error().raw_message() });
      }
    } else if (strcmp(png_texts[i].key, sipi_tag) == 0) {
      Essentials se = Essentials::parse_legacy(png_texts[i].text);
      img->essential_metadata(se);
    } else {
      fprintf(stderr, "PNG-COMMENT: key=\"%s\" text=\"%s\"\n", png_texts[i].key, png_texts[i].text);
    }
  }

  if (!decode_pixels) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    return true;
  }

  size_t sll = png_get_rowbytes(png_ptr, info_ptr);
  buffer.resize(height * sll);

  row_pointers.resize(height);
  for (size_t i = 0; i < img->getNy(); i++) { row_pointers[i] = (buffer.data() + i * sll); }

  png_read_image(png_ptr, row_pointers.data());
  png_read_end(png_ptr, info_ptr);

  png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

  if (img->getBps() == 16) {
    auto *tmp = (unsigned short *)buffer.data();
    for (size_t i = 0; i < img->getNx() * img->getNy() * img->getNc(); i++) { tmp[i] = ntohs(tmp[i]); }
  }
  img->set_pixels(std::move(buffer), img->getNx(), img->getNy(), img->getNc(), img->getBps());

  infile.reset();

  if (region != nullptr) {// we just use the image.crop method
    if (const auto cropped = Sipi::processing::crop(*img, region); !cropped) {
      return std::unexpected(cropped.error());
    }
  }

  //
  // resize/Scale the image if necessary
  //
  if (size != nullptr) {
    size_t nnx, nny;
    int reduce = -1;
    bool redonly;
    SipiSize::SizeType rtype = size->get_size(img->getNx(), img->getNy(), nnx, nny, reduce, redonly);
    if (rtype != SipiSize::FULL) {
      if (auto r = Sipi::processing::scale_with(
            *img, nnx, nny, scaling_method_for(SipiQualityFormat::PNG, scaling_quality));
          !r) {
        return std::unexpected(r.error());
      }
    }
  }

  if (force_bps_8) {
    if (auto r = processing::to8bps(*img); !r) { return std::unexpected(r.error()); }
  }
  return true;
};

/*==========================================================================*/


// The shape probe proper: reports a libpng struct-allocation failure as a
// SipiValueError value.
Result<SipiImgInfo> SipiIOPng::read_shape(const std::string &filepath)
{
  SIPI_ZONE_N("SipiIOPng::read_shape");
  SipiImgInfo info;
  unsigned char header[8];

  //
  // open the input file
  //
  auto infile = std::unique_ptr<FILE, decltype(&fclose)>(fopen(filepath.c_str(), "rb"), fclose);
  if (!infile) {
    info.success = SipiImgInfo::FAILURE;
    return info;
  }
  fread(header, 1, 8, infile.get());
  if (png_sig_cmp(header, 0, 8) != 0) {
    info.success = SipiImgInfo::FAILURE;
    return info;
  }

  png_structp png_ptr;
  png_infop info_ptr;

  if ((png_ptr = png_create_read_struct(
         PNG_LIBPNG_VER_STRING, (png_voidp) nullptr, (png_error_ptr)sipi_error_fn, (png_error_ptr)sipi_warning_fn))
      == nullptr) {
    return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
      "Error reading PNG file \"" + filepath + "\": Could not allocate memory for png_structp !" });
  }
  if ((info_ptr = png_create_info_struct(png_ptr)) == nullptr) {
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    return std::unexpected(SipiValueError{ ErrorCode::kShapeProbeFailed,
      "Error reading PNG file \"" + filepath + "\": Could not allocate memory for png_infop !" });
  }

  // Bound per-chunk decompression and the number of cached ancillary chunks
  // (zTXt/iTXt/etc.) — without these, a hostile file can decompression-bomb
  // the chunk cache before any pixel data is read.
  png_set_chunk_malloc_max(png_ptr, png_alloc_size_t(16) << 20);
  png_set_chunk_cache_max(png_ptr, 1000);

  // setjmp error recovery for read_shape
  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    info.success = SipiImgInfo::FAILURE;
    return info;
  }

  png_init_io(png_ptr, infile.get());
  png_set_sig_bytes(png_ptr, 8);
  png_read_info(png_ptr, info_ptr);

  info.width = png_get_image_width(png_ptr, info_ptr);
  info.height = png_get_image_height(png_ptr, info_ptr);
  info.nc = png_get_channels(png_ptr, info_ptr);
  info.bps = png_get_bit_depth(png_ptr, info_ptr);
  info.orientation = TOPLEFT;
  info.success = SipiImgInfo::DIMS;

  png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);

  return info;
}
/*==========================================================================*/


void create_text_chunk(PngTextPtr *png_textptr, char *key, char *str, unsigned int len)
{
  png_text *chunk = png_textptr->next();
  chunk->compression = PNG_TEXT_COMPRESSION_NONE;
  chunk->key = key;
  chunk->text = str;
  chunk->text_length = len;
  chunk->itxt_length = 0;
  chunk->lang = lang_en;
  chunk->lang_key = nullptr;
}

/*==========================================================================*/

/*!
 * Context passed as the libpng I/O pointer for HTTP writes.
 * The `client_aborted` flag is set when a send/flush fails because the
 * peer closed the socket; the top-level setjmp handler reads it to
 * distinguish client aborts from genuine write errors.
 */
struct PngHttpCtx
{
  SinkStream *sink;
  bool client_aborted;
};

static void conn_write_data(png_structp png_ptr, png_bytep data, png_size_t length)
{
  auto *ctx = static_cast<PngHttpCtx *>(png_get_io_ptr(png_ptr));

  // A non-zero sink return means the response stream (the HTTP socket) is no
  // longer usable. No C++ exception crosses libpng's C frames — the sink
  // callback returns a status code, which we route to libpng's error path.
  if (ctx->sink->write(data, length) != 0) {
    ctx->client_aborted = true;
    png_error(png_ptr, "HTTP write failed");// → sipi_error_fn → longjmp
  }
}

/*==========================================================================*/

static void conn_flush_data(png_structp /*png_ptr*/)
{
  // No-op: the per-chunk SinkStream write already pushed bytes downstream, and
  // the HTTP framing/terminator is flushed by the caller after write() returns.
  // libpng requires a non-null flush fn.
}

/*==========================================================================*/

namespace {
// Owns the libpng write-struct pair for the duration of SipiIOPng::write.
// Declared before the setjmp so that a longjmp out of a libpng error callback
// still runs this destructor, releasing png_ptr/info_ptr exactly once on
// every exit path (success, every error return, and the setjmp landing
// block). Armed as soon as png_ptr exists, before png_create_info_struct
// runs — png_destroy_write_struct tolerates info_ptr still being null.
class PngWriteStructGuard
{
public:
  PngWriteStructGuard(png_structp &png_ptr, png_infop &info_ptr) : png_ptr_(png_ptr), info_ptr_(info_ptr) {}

  ~PngWriteStructGuard()
  {
    if (png_ptr_ != nullptr) { png_destroy_write_struct(&png_ptr_, &info_ptr_); }
  }

  PngWriteStructGuard(const PngWriteStructGuard &) = delete;
  PngWriteStructGuard &operator=(const PngWriteStructGuard &) = delete;

private:
  png_structp &png_ptr_;
  png_infop &info_ptr_;
};
}// namespace

Result<void> SipiIOPng::write(SipiImage *img, const OutputSink &sink, const SipiCompressionParams *params)
{
  SIPI_ZONE_N("SipiIOPng::write");
  // A streamed sink (callback/tee) is driven through SinkStream via libpng's
  // write callback; a FilePath uses libpng's native file/stdout writer.
  const bool streaming = is_streaming_sink(sink);
  const std::string filepath = streaming ? std::string("<http response>") : std::get<FilePath>(sink).path;

  FILE *outfile = nullptr;
  png_structp png_ptr;
  png_infop info_ptr = nullptr;

  // Owns the output FILE for the file branch (stdout is never owned).
  // Resource-owning objects are declared before the setjmp so their
  // destructors run on the normal C++ path out of the landing block below; a
  // longjmp skips the destructors of anything constructed inside the risk
  // window.
  auto outfile_guard = std::unique_ptr<FILE, decltype(&fclose)>(nullptr, fclose);

  if (!(png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, sipi_error_fn, sipi_warning_fn))) {
    return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed,
      "Error writing PNG file \"" + filepath + "\": png_create_write_struct failed !" });
  }
  PngWriteStructGuard png_guard(png_ptr, info_ptr);

  // Streamed-write context: SinkStream and http_ctx are kept alive for the
  // whole of SipiIOPng::write so their addresses stay valid across longjmp.
  // For a FilePath we leave them unused and let libpng's native file writer run.
  std::unique_ptr<SinkStream> sink_stream;
  PngHttpCtx http_ctx{ nullptr, false };

  if (streaming) {
    sink_stream = std::make_unique<SinkStream>(sink);
    http_ctx.sink = sink_stream.get();
    png_set_write_fn(png_ptr, &http_ctx, conn_write_data, conn_flush_data);
  } else if (filepath == "stdout:") {
    outfile = stdout;
  } else {
    if (!(outfile = fopen(filepath.c_str(), "wb"))) {
      return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed,
        "Error writing PNG file \"" + filepath + "\": Could not open output file!" });
    }
    outfile_guard.reset(outfile);
  }

  if (!(info_ptr = png_create_info_struct(png_ptr))) {
    return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed,
      "Error writing PNG file \"" + filepath + "\": png_create_info_struct !" });
  }

  // PNG does not support alpha channels, so we have to remove them if they are present
  if ((img->getNc() > 3) && (img->getNalpha() > 0)) {// we have an alpha channel and possibly a CMYK image
    if (auto r = processing::removeExtraSamples(*img); !r) { return std::unexpected(r.error()); }
  }

  int color_type;
  if (img->getNc() == 1) {// grey value
    color_type = PNG_COLOR_TYPE_GRAY;
  } else if ((img->getNc() == 2) && (img->getNalpha() == 1)) {// grey value with alpha
    color_type = PNG_COLOR_TYPE_GRAY_ALPHA;
  } else if (img->getNc() == 3) {// RGB
    color_type = PNG_COLOR_TYPE_RGB;
  } else if ((img->getNc() == 4) && (img->getNalpha() == 1)) {// RGB + ALPHA
    color_type = PNG_COLOR_TYPE_RGB_ALPHA;
  } else if (img->getNc() == 4) {
    if (auto r = processing::convertToIcc(*img, Icc(Sipi::PredefinedProfiles::icc_sRGB), 8); !r) {
      return std::unexpected(r.error());
    }
    // convertToIcc() moves in a freshly sized 3-channel/8-bit buffer via set_pixels(),
    // so the image geometry already matches; only the PNG colour type needs selecting.
    color_type = PNG_COLOR_TYPE_RGB;
  } else {
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Error writing PNG file \"" + filepath + "\": unsupported number of channels (" + std::to_string(img->getNc())
        + "), expected 1, 2, 3, or 4" });
  }

  //
  // ICC profile handfling is special...
  //
  Essentials es = img->essential_metadata();
  std::shared_ptr<Icc> icc = img->getIcc();
  std::vector<unsigned char> icc_buf;
  bool have_icc_buf = false;
  if ((icc != nullptr) || es.fields().use_icc) {
    if ((icc != nullptr) && (icc->getProfileType() == icc_LAB)) {
      if (auto r = processing::convertToIcc(*img, Icc(Sipi::PredefinedProfiles::icc_sRGB), img->getBps()); !r) {
        return std::unexpected(r.error());
      }
      icc = img->getIcc();
    }
    try {
      if (es.fields().use_icc) {
        icc_buf = es.fields().icc_profile;
      } else {
        icc_buf = icc->iccBytes();
      }
      have_icc_buf = true;
    } catch (SipiError &err) {
      log_err("Error writing ICC profile in PNG: %s", err.what());
    }
  }

  PngTextPtr chunk_ptr(4);

  //
  // other metadata comes here
  //

  // exif_buf must outlive png_write_info()/png_write_png() below — libpng
  // keeps the pointer handed to png_set_eXIf_1 rather than copying it.
  std::vector<unsigned char> exif_buf;
  std::shared_ptr<Exif> exif = img->getExif();
  if (exif) { exif_buf = exif->exifBytes(); }

  // iptc_profile must likewise outlive png_write_info()/png_write_png().
  std::vector<unsigned char> iptc_buf;
  std::shared_ptr<Iptc> iptc = img->getIptc();
  std::string iptc_profile;
  if (iptc) {
    iptc_buf = iptc->iptcBytes();
    if (!iptc_buf.empty()) {
      iptc_profile = encode_raw_profile("iptc", iptc_buf);
      chunk_ptr.add_zTXt(iptc_tag, iptc_profile.data(), (unsigned int)iptc_profile.size());
    }
  }

  std::string xmp_buf;
  std::shared_ptr<Xmp> xmp = img->getXmp();
  if (xmp != nullptr) {
    xmp_buf = xmp->xmpBytes();
    chunk_ptr.add_iTXt(xmp_tag, (char *)xmp_buf.data(), xmp_buf.size());
  }

  // PNG is an Access File format per ADR-0009 — it MUST NOT carry the
  // Essentials packet. `es` above feeds only the ICC fallback branch.

  // row_pointers must outlive png_write_png() below — libpng keeps the
  // pointer handed to png_set_rows rather than copying it.
  std::vector<png_bytep> row_pointers(img->getNy());
  png_bytep pixel_data = img->pixels_writable().data();
  if (img->getBps() == 8) {
    for (size_t i = 0; i < img->getNy(); i++) { row_pointers[i] = (pixel_data + i * img->getNx() * img->getNc()); }
  } else if (img->getBps() == 16) {
    for (size_t i = 0; i < img->getNy(); i++) { row_pointers[i] = (pixel_data + 2 * i * img->getNx() * img->getNc()); }
  }

  // setjmp error recovery for write — sipi_error_fn calls longjmp. A longjmp
  // out of libpng unwinds the C stack without running C++ destructors, so
  // nothing with a destructor may be constructed past this point.
  if (setjmp(png_jmpbuf(png_ptr))) {
    if (http_ctx.client_aborted) {
      return std::unexpected(
        SipiValueError{ ErrorCode::kClientAbort, "Client aborted HTTP response during PNG write" });
    }
    return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed, "PNG write failed for \"" + filepath + "\"" });
  }

  if (outfile != nullptr) png_init_io(png_ptr, outfile);

  png_set_filter(png_ptr, 0, PNG_FILTER_NONE);

  /* set the zlib compression level */
  png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);

  png_set_IHDR(png_ptr,
    info_ptr,
    img->getNx(),
    img->getNy(),
    img->getBps(),
    color_type,
    PNG_INTERLACE_NONE,
    PNG_COMPRESSION_TYPE_DEFAULT,
    PNG_FILTER_TYPE_DEFAULT);

  if (have_icc_buf) {
    png_set_iCCP(png_ptr, info_ptr, "ICC", PNG_COMPRESSION_TYPE_BASE, icc_buf.data(), icc_buf.size());
  }

  if (!exif_buf.empty()) { png_set_eXIf_1(png_ptr, info_ptr, (png_uint_32)exif_buf.size(), exif_buf.data()); }

  if (chunk_ptr.num() > 0) { png_set_text(png_ptr, info_ptr, chunk_ptr.ptr(), chunk_ptr.num()); }

  png_set_rows(png_ptr, info_ptr, row_pointers.data());

  png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_SWAP_ENDIAN,
    nullptr);// we expect the data to be little endian...

  return {};
}

}
