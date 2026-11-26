/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * This file handles the reading and writing of TIFF files using libtiff.
 */
#ifndef __sipi_io_tiff_h
#define __sipi_io_tiff_h

#include <string>
#include <vector>

#include "tiff.h"
#include "tiffio.h"

#include "error/SipiValueError.h"
#include "image/SipiImage.h"
#include "image/SipiIO.h"

namespace Sipi {

// `read_watermark` is declared in //src/image_processing's processing.h and
// defined in SipiIOTiff.cpp below — it is the one codec helper
// `Sipi::processing::add_watermark` calls back into.

/*!
 * Select the pyramid resolution level (IFD index) for a requested reduce.
 *
 * `resolutions` are ordered full-first (level 0 = full, ratio 1, then 2, 4, 8
 * …); `SubImageInfo::reduce` is the ratio image_width / level_width. `reduce_exp`
 * is the IIIF size stage's log2 exponent (0, 1, 2, 3 → divisor 1, 2, 4, 8).
 * Returns the largest level whose ratio does not exceed the requested divisor;
 * the caller applies any residual scale in that level's coordinate space.
 * Returns 0 for an empty pyramid or a full-size (exp ≤ 0) request. Never
 * divides by zero: the divisor is always ≥ 1.
 */
[[nodiscard]] uint32_t select_pyramid_level(const std::vector<SubImageInfo> &resolutions, int reduce_exp);

/*! Class which implements the TIFF-reader/writer */
class SipiIOTiff : public SipiIO
{
private:
  // The one read path behind `read` and `read_metadata`. With `decode_pixels`
  // false it leaves the header effects that the pixel steps of a full read
  // have, and stops before the first pixel is decoded.
  [[nodiscard]] Result<bool> read_image(SipiImage *img,
    const std::string &filepath,
    std::shared_ptr<SipiRegion> region,
    std::shared_ptr<SipiSize> size,
    bool force_bps_8,
    ScalingQuality scaling_quality,
    bool decode_pixels);

  /*!
   * Read the EXIF data from the TIFF file and create an Exiv2::Exif object
   * \param img Pointer to SipiImage instance
   * \param[in] tif Pointer to TIFF file handle
   * \param[in] exif_offset Offset of EXIF directory in TIFF file
   */
  static void readExif(SipiImage *img, TIFF *tif, toff_t exif_offset);

  /*!
   * Write the EXIF data to the TIFF file
   * \param img Pointer to SipiImage instance
   * \param[in] tif Pointer to TIFF file handle
   * \returns Success, or a value error if a rational tag has a non-zero
   *          numerator and a zero denominator (malformed, not "no value")
   */
  [[nodiscard]] static Result<void> writeExif(SipiImage *img, TIFF *tif);

  static void write_basic_tags(const SipiImage &img,
    TIFF *tif,
    uint32_t nx,
    uint32_t ny,
    bool its_1_bit,
    const std::string &compression);

  static void write_subfile(const SipiImage &img,
    TIFF *tif,
    int level,
    uint32_t &tile_width,
    uint32_t &tile_height,
    const std::string &compression = "");

  /*!
   * Converts a 8 bps bitonal image to 1 bps bitonal image
   *
   * \param[in] img Reference to SipiImage instance
   * \param[out] sll Scan line lengt
   * \returns Buffer of 1-bit data (padded to bytes) on success, or a value error
   *          if the image's photometric interpretation or bits per sample is
   *          not supported
   */
  [[nodiscard]] static Result<std::vector<unsigned char>> cvrt8BitTo1bit(const SipiImage &img, unsigned int &sll);

public:
  virtual ~SipiIOTiff(){};

  static void initLibrary(void);

  /*!
   * Method used to read an image file
   *
   * \param img Pointer to SipiImage instance
   * \param filepath Image file path
   * \param region Region of the image to read
   * \param size Size of the image to read
   * \param force_bps_8 Convert the file to 8 bits/sample on reading thus enforcing an 8 bit image
   * \param scaling_quality Quality of the scaling algorithm
   */
  [[nodiscard]] Result<bool> read(SipiImage *img,
    const std::string &filepath,
    std::shared_ptr<SipiRegion> region,
    std::shared_ptr<SipiSize> size,
    bool force_bps_8,
    ScalingQuality scaling_quality) override;

  /*!
   * Get the dimension of the image
   *
   * \param[in] filepath Pathname of the image file
   * \return Image information
   */
  [[nodiscard]] Result<SipiImgInfo> read_shape(const std::string &filepath) override;

  /*!
   * Read the header and embedded metadata only (see `SipiIO::read_metadata`).
   *
   * \param img Pointer to SipiImage instance
   * \param filepath Image file path
   */
  [[nodiscard]] Result<bool> read_metadata(SipiImage *img, const std::string &filepath) override;

  /*!
   * Write a TIFF image to a file, stdout or to a memory buffer
   *
   * For a streamed sink (callback/tee) or stdout the TIFF is built in an
   * internal memory buffer first, because libtiff makes extensive use of
   * "lseek" which is not available on a non-seekable destination.
   *
   * \param *img Pointer to SipiImage instance
   * \param sink Where the encoded bytes go (ADR-0006): a FilePath (file or
   * stdout via "-"/"stdout:"), or a streamed CallbackSink / TeeSink.
   */
  [[nodiscard]] Result<void> write(SipiImage *img,
    const OutputSink &sink,
    const SipiCompressionParams *params) override;
};
}// namespace Sipi

#endif
