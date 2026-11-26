/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * This file handles the reading and writing of PNG files using libpng.
 */
#ifndef __sipi_io_png_h
#define __sipi_io_png_h

#include <string>

#include "error/SipiValueError.h"
#include "image/SipiImage.h"
#include "image/SipiIO.h"

namespace Sipi {

class SipiIOPng : public SipiIO
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
  ~SipiIOPng() override = default;
  ;

  /*!
   * Method used to read an image file
   *
   * \param *img Pointer to SipiImage instance
   * \param filepath Image file path
   * \param reduce Reducing factor. Not used reading TIFF files
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
   * \param[out] width Width of the image in pixels
   * \param[out] height Height of the image in pixels
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
   * Write a PNG image to a file, stdout or to a memory buffer
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
