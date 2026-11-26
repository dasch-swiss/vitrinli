/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * This file implements the virtual abstract class which implements the image file I/O.
 */
#ifndef _sipi_io_h
#define _sipi_io_h

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "iiifparser/SipiQualityFormat.h"
#include "iiifparser/SipiRegion.h"
#include "iiifparser/SipiSize.h"
#include "format_handlers/output_sink.h"
#include "error/SipiValueError.h"

#include <memory>

/**
 * @namespace Sipi Is used for all Sipi things.
 */
namespace Sipi {

/// Named caps for post-header decode dimensions, enforced before any decode
/// buffer is sized/allocated from them.
inline constexpr std::size_t kMaxDecodeDim = 1u << 17;
inline constexpr std::size_t kMaxDecodeChannels = 32;

/*!
 * Validate the dimensions a codec reads from a file's header before any
 * decode buffer is sized/allocated from them (C-library boundary input
 * validation). A crafted header can claim dimensions or a channel/bit-depth
 * combination that overflows or exhausts memory long before pixel data is
 * read; reject such headers here instead of trusting them into an
 * allocation.
 *
 * \param nx Image width in pixels, as read from the file header
 * \param ny Image height in pixels, as read from the file header
 * \param nc Number of channels/samples-per-pixel, as read from the file header
 * \param bps Bits per sample, as read from the file header
 * \param filepath Path of the file being decoded, for the error message
 *
 * \return A `Result` holding no value if nx or ny exceeds kMaxDecodeDim or nc
 * exceeds kMaxDecodeChannels (`ErrorCode::kMalformedInput`), or if bps is not
 * one of the bit depths SIPI's codecs support (1, 4, 8, 12, 16)
 * (`ErrorCode::kUnsupportedFormat`); a value-holding `Result` otherwise.
 */
[[nodiscard]] inline Result<void>
  validate_decode_dims(std::size_t nx, std::size_t ny, std::size_t nc, int bps, const std::string &filepath)
{
  if (nx > kMaxDecodeDim || ny > kMaxDecodeDim) {
    return std::unexpected(SipiValueError{
      ErrorCode::kMalformedInput, "Image dimensions exceed the supported decode limit: " + filepath });
  }
  if (nc > kMaxDecodeChannels) {
    return std::unexpected(SipiValueError{
      ErrorCode::kMalformedInput, "Image channel count exceeds the supported decode limit: " + filepath });
  }
  if (bps != 1 && bps != 4 && bps != 8 && bps != 12 && bps != 16) {
    return std::unexpected(
      SipiValueError{ ErrorCode::kUnsupportedFormat, "Unsupported bits-per-sample value: " + filepath });
  }
  return {};
}

enum class ScalingMethod : std::uint8_t { HIGH = 0, MEDIUM = 1, LOW = 2 };

struct ScalingQuality
{
  ScalingMethod jk2;
  ScalingMethod jpeg;
  ScalingMethod tiff;
  ScalingMethod png;
};

/// The scaling method used to scale an image decoded from `source`. Every
/// codec read and the size-restricted crop take it from here, so `full/max`
/// and a restricted region scale alike. TIFF reads the `jpeg` setting.
[[nodiscard]] inline ScalingMethod scaling_method_for(SipiQualityFormat::FormatType source,
  const ScalingQuality &quality)
{
  switch (source) {
  case SipiQualityFormat::TIF:
  case SipiQualityFormat::JPG:
    return quality.jpeg;
  case SipiQualityFormat::PNG:
    return quality.png;
  default:
    return quality.jk2;
  }
}

enum Orientation : std::uint8_t {
  // from the TIFF specification...
  TOPLEFT = 1,
  //!< The 0th row represents the visual top of the image, and the 0th column represents the visual left-hand side.
  TOPRIGHT = 2,
  //!< The 0th row represents the visual top of the image, and the 0th column represents the visual right-hand side.
  BOTRIGHT = 3,
  //!< The 0th row represents the visual bottom of the image, and the 0th column represents the visual right-hand side.
  BOTLEFT = 4,
  //!< The 0th row represents the visual bottom of the image, and the 0th column represents the visual left-hand side.
  LEFTTOP = 5,
  //!< The 0th row represents the visual left-hand side of the image, and the 0th column represents the visual top.
  RIGHTTOP = 6,
  //!< The 0th row represents the visual right-hand side of the image, and the 0th column represents the visual top.
  RIGHTBOT = 7,
  //!< The 0th row represents the visual right-hand side of the image, and the 0th column represents the visual bottom.
  LEFTBOT = 8
  //!< The 0th row represents the visual left-hand side of the image, and the 0th column represents the visual bottom.
};

typedef struct SubImageInfo_
{
  uint32_t reduce; // equal to image.nx / SubImageInfo.width
  uint32_t width;
  uint32_t height;
  uint32_t tile_width;
  uint32_t tile_height;
} SubImageInfo;

class SipiImgInfo
{
public:
  enum : std::uint8_t { FAILURE = 0, DIMS = 1, ALL = 2 } success{ FAILURE };

  int width{ 0 };
  int height{ 0 };
  Orientation orientation{ TOPLEFT };
  int tile_width{ 0 };
  int tile_height{ 0 };
  int clevels{ 0 };
  int nc{ 0 };      ///< number of channels (samples per pixel)
  int bps{ 0 };     ///< bits per sample (8 or 16 typically)
  int numpages{ 0 };
  std::string internalmimetype;
  std::string origname;
  std::string origmimetype;
  std::vector<SubImageInfo> resolutions;

  SipiImgInfo() = default;
};

enum SipiCompressionParamName : std::uint8_t {
  JPEG_QUALITY,
  J2K_Sprofile,
  J2K_Creversible,
  J2K_Clayers,
  J2K_Clevels,
  J2K_Corder,
  J2K_Cprecincts,
  J2K_Cblk,
  J2K_Cuse_sop,
  J2K_Stiles,
  J2K_rates,
  TIFF_Pyramid,
};

using SipiCompressionParams = std::unordered_map<int, std::string>;

class SipiImage;//!< forward declaration of class SipiImage

/*!
 * This is the virtual base class for all classes implementing image I/O.
 */
class SipiIO
{
public:
  virtual ~SipiIO() = default;
  ;


  /*!
   * Read an image file into `img`. The returned `bool` (on success) reports
   * whether this handler recognised and decoded the file at `filepath` —
   * `false` lets the caller fall through to another registered handler rather
   * than treating a format mismatch as an error. A `Result` holding no value
   * reports a genuine decode failure (malformed input, an unsupported
   * variant, a codec error) for a file this handler did recognise.
   *
   * \param img Pointer to SipiImage instance
   * \param filepath Image file path
   * \param region Region of the image to read
   * \param size Size of the image to read
   * \param force_bps_8 Convert the file to 8 bits/sample on reading thus enforcing an 8 bit image
   * \param scaling_quality Quality of the scaling algorithm
   */
  [[nodiscard]] virtual Result<bool> read(SipiImage *img,
    const std::string &filepath,
    std::shared_ptr<SipiRegion> region,
    std::shared_ptr<SipiSize> size,
    bool force_bps_8,
    ScalingQuality scaling_quality) = 0;

  [[nodiscard]] Result<bool> read(SipiImage *img, const std::string &filepath)
  {
    return read(img, filepath, nullptr, nullptr, false, { ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH });
  }

  [[nodiscard]] Result<bool> read(SipiImage *img, const std::string &filepath, const std::shared_ptr<SipiRegion> region)
  {
    return read(img, filepath, region, nullptr, false, { ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH });
  }

  [[nodiscard]] Result<bool> read(SipiImage *img,
    const std::string &filepath,
    const std::shared_ptr<SipiRegion> region,
    const std::shared_ptr<SipiSize> size)
  {
    return read(img, filepath, region, size, false, { ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH });
  }

  [[nodiscard]] Result<bool> read(SipiImage *img,
    const std::string &filepath,
    const std::shared_ptr<SipiRegion> region,
    std::shared_ptr<SipiSize> size,
    bool force_bps_8)
  {
    return read(img, filepath, region, size, force_bps_8, { ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH, ScalingMethod::HIGH });
  }

  /*!
   * Read the image shape (dimensions, tiling, levels, channels, bit depth) from a file
   * without performing a full decode. Service-file overrides may take a fast path via
   * the Essentials packet when present (ADR-0004 / DEV-6537). A `Result` holding no
   * value reports a genuine shape-probe failure; a recognised file whose shape cannot
   * be determined yet (e.g. no matching handler) is reported through `SipiImgInfo::success`.
   *
   * \param filepath Pathname of the image file
   */
  [[nodiscard]] virtual Result<SipiImgInfo> read_shape(const std::string &filepath) = 0;

  /*!
   * Read the header and the embedded metadata of a file into `img`, without
   * decoding pixels. On success `img` holds the geometry, photometric
   * interpretation, extra samples, orientation and the EXIF, XMP, IPTC and
   * ICC objects that a full `read(img, filepath)` leaves, and no pixel buffer.
   * The `bool` and the error follow `read`: `false` for a file this handler
   * does not recognise, an error for a recognised file that fails, and an
   * embedded metadata parse failure is fatal, as in `read`.
   *
   * Each handler implements it on the code path of its `read`, so that the
   * two cannot drift apart.
   *
   * \param img Pointer to SipiImage instance
   * \param filepath Image file path
   */
  [[nodiscard]] virtual Result<bool> read_metadata(SipiImage *img, const std::string &filepath) = 0;


  /*!
   * Write an image using the given file format implemented by the subclass. A
   * `Result` holding no value reports a write failure; its `ErrorCode`
   * distinguishes a client-initiated abort of the destination stream from
   * every other write failure.
   *
   * \param img Pointer to SipiImage instance
   * \param sink Where the encoded bytes go (ADR-0006). A `FilePath` is written
   * via the codec's native file/stdout writer (`"-"`/`"stdout:"` mean stdout);
   * a `CallbackSink` or `TeeSink` is streamed through `SinkStream`.
   * \param params Compression parameters
   */
  [[nodiscard]] virtual Result<void>
    write(SipiImage *img, const OutputSink &sink, const SipiCompressionParams *params) = 0;
};
}// namespace Sipi

#endif
