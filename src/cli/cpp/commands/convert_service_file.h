/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef SIPI_CLI_COMMANDS_CONVERT_SERVICE_FILE_H
#define SIPI_CLI_COMMANDS_CONVERT_SERVICE_FILE_H

#include <cstddef>
#include <string>

#include "ffi/engine_context.h"

namespace Sipi::cli {

/*!
 * Inputs to `sipi convert service-file <in> <out>` (DEV-6537 / DEV-6540).
 * Each field maps to a CLI option attached to
 * `cmd_convert_service` in `sipi.cpp`; this command is the only path that
 * ever produces an Essentials packet per ADR-0010.
 */
struct ConvertServiceFileArgs
{
  std::string input_path;  //!< source file path supplied by the operator
  std::string output_path; //!< destination Service File path
  bool set_topleft = false;//!< apply `--topleft` orientation normalization
  std::size_t decode_timeout_ms = Sipi::ffi::kDefaultDecodeTimeoutMs;//!< wall-clock decode deadline, see decode_deadline.h
};

/*!
 * Run `sipi convert service-file`. Steps:
 *
 *   1. Detect the output Service File format from the output extension
 *      (`.jp2` / `.jpx` → JP2; `.tif` / `.tiff` → pyramidal TIFF). Other
 *      extensions are rejected per ADR-0009 — Service Files only live in
 *      those two carriers.
 *   2. Read the source via `SipiImage::readSource` (no Essentials stamping
 *      per ADR-0010; the source file is untouched on disk).
 *   3. Drop any Essentials packet the source happened to carry — the
 *      output gets a fresh packet keyed to the new source path and the
 *      post-transformation pixel hash (maintainer decision 2026-05-14).
 *   4. Apply `--topleft` orientation normalization if requested. Other
 *      transforms are intentionally NOT exposed on `convert service-file`
 *      per the D5 option-availability matrix.
 *   5. Build `EssentialsFields`: identity + image-shape fields populated
 *      from current `SipiImage` state; `data_chksum` = SHA-256 of the
 *      post-transformation pixel buffer (raw bytes, NOT hex).
 *   6. Set the packet on the SipiImage and pass to the format-handler
 *      writer with `file_role = "service-file"` so the carrier gets
 *      emitted (JP2 UUID box / TIFF tag 65112). Pyramidal TIFF mode is
 *      forced for `.tif` outputs since Service Files are pyramidal.
 *
 * Returns the process exit code (EXIT_SUCCESS or EXIT_FAILURE).
 */
[[nodiscard]] int cmd_convert_service_file(const ConvertServiceFileArgs &args);

}// namespace Sipi::cli

#endif
