/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef SIPI_CLI_COMMANDS_CONVERT_ACCESS_FILE_H
#define SIPI_CLI_COMMANDS_CONVERT_ACCESS_FILE_H

#include <cstddef>
#include <string>
#include <vector>

#include "ffi/engine_context.h"

namespace Sipi::cli {

/*!
 * Inputs to `sipi convert access-file <in> <out>` (DEV-6537 / DEV-6540).
 *
 * Per ADR-0009 / ADR-0010, an Access File is the end-user-delivery
 * format produced from a Service File. The command's contract:
 *
 *   - Input MUST be a Service File (Essentials packet present + parses).
 *     Any other input is rejected with a clear error pointing the
 *     operator at the generic `sipi convert` verb.
 *   - The output never carries an Essentials packet (Access Files do
 *     not carry SIPI-internal identity per ADR-0009).
 *   - XMP / IPTC / EXIF / ICC propagate via the standard format-handler
 *     write paths (existing infrastructure per ADR-0011).
 *   - `--skipmeta` is intentionally NOT exposed on this verb (D5
 *     option-availability matrix): DSP-opinionated flows always
 *     propagate metadata.
 */
struct ConvertAccessFileArgs
{
  std::string input_path;            //!< source Service File path
  std::string output_path;           //!< destination Access File path

  //!< Output format selector. Empty → derive from `output_path`
  //!< extension; otherwise one of "jpg", "jpx", "tif", "png".
  std::string format;

  //!< Region of interest as [x, y, w, h]. Empty → full image.
  std::vector<int> region;

  //!< Resize spec, see `SipiSize`. Empty → no resize.
  std::string size;

  //!< Reduce factor. 0 → no reduce.
  int reduce = 0;

  //!< Scale percentage. 0 → no scale.
  int scale = 0;

  //!< Rotation in degrees [0.0, 360.0). 0 → no rotation.
  float rotate = 0.0F;

  //!< Mirror axis: "none", "horizontal", "vertical".
  std::string mirror;

  //!< Watermark file path. Empty → no watermark.
  std::string watermark;

  //!< JPEG quality [1, 100]. 0 → format default.
  int jpeg_quality = 0;

  //!< ICC conversion target: "none" / "sRGB" / "AdobeRGB" / "GRAY".
  std::string icc;

  //!< Apply --topleft orientation normalization.
  bool set_topleft = false;

  //!< Wall-clock decode deadline in ms, see decode_deadline.h.
  std::size_t decode_timeout_ms = Sipi::ffi::kDefaultDecodeTimeoutMs;

  //!< Emit JSON report on stdout.
  bool json_output = false;
};

/*!
 * Run `sipi convert access-file`. Steps:
 *
 *   1. Read source via `SipiImage::readSource`. The format-handler
 *      reader populates `Essentials` if the source carries it.
 *   2. **Validate**: if no Essentials packet is present, fail with
 *      the documented error and exit non-zero — the operator should
 *      use bare `sipi convert` for generic format conversion.
 *   3. Drop the source Essentials packet — Access Files never carry
 *      Essentials.
 *   4. Apply transformations (orientation, ICC, rotate, mirror,
 *      watermark) per IIIF semantics.
 *   5. Write output with NO `file_role` param — the writer's
 *      Essentials-emission gate stays closed (ADR-0010).
 *
 * Returns the process exit code (EXIT_SUCCESS or EXIT_FAILURE).
 */
[[nodiscard]] int cmd_convert_access_file(const ConvertAccessFileArgs &args);

}// namespace Sipi::cli

#endif
