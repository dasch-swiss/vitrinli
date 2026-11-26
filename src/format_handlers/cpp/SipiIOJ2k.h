/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * This file handles the reading and writing of JPEG 2000 files using libtiff.
 */
#ifndef __sipi_io_j2k_h
#define __sipi_io_j2k_h

#include <string>

#include "tiff.h"
#include "tiffio.h"

#include "error/SipiValueError.h"
#include "image/SipiImage.h"
// #include "metadata/exif.h"
#include "image/SipiIO.h"

namespace Sipi {

/*!
 * Validate a JPEG2000 palette (index) color mapping before the index-to-RGB
 * expansion allocates or writes its output buffer. The expansion loop only
 * handles a single 8-bit index component mapped to a 3-channel (RGB) output,
 * and every possible index value must resolve to a palette entry.
 *
 * \param bps Bits per sample of the index component
 * \param nc Number of components in the index codestream (before expansion)
 * \param numcol Number of output colors the palette LUTs map to
 * \param nentries Number of entries in each palette LUT
 * \param filepath Path of the file being decoded, for the error message
 *
 * \returns A `Result<void>` holding no value if bps != 8, nc != 1, numcol !=
 * 3, or nentries is too small to cover every possible bps-bit index value.
 */
[[nodiscard]] Result<void>
  validate_j2k_palette_mapping(std::size_t bps, std::size_t nc, int numcol, int nentries, const std::string &filepath);

/*! Class which implements the JPEG2000-reader/writer */
class SipiIOJ2k : public SipiIO
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

public:
  ~SipiIOJ2k() override = default;
  ;
  /*!
   * Method used to read an image file
   *
   * \param img Pointer to SipiImage instance
   * \param filepath Image file path
   * \param region Region of the image to be read
   * \param size Size of the image to be read
   * \param force_bps_8 Force the image to be read as 8 bits per sample
   * \param scaling_quality Scaling quality for the different formats
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
   * \param filepath Pathname of the image file
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
   * Write a JPEG2000 image to the given OutputSink (file/stdout, or a streamed
   * callback/tee per ADR-0006).
   *
   * \param *img Pointer to SipiImage instance
   * \param sink Where the encoded bytes go.
   */
  [[nodiscard]] Result<void> write(SipiImage *img,
    const OutputSink &sink,
    const SipiCompressionParams *params) override;
};
}// namespace Sipi

#endif
