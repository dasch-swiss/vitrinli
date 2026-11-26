/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef SIPIREPORT_H
#define SIPIREPORT_H

#include <optional>
#include <ostream>
#include <string>

#include "error/SipiValueError.h"
#include "image/populate_from_image.h"

namespace Sipi {

/*!
 * The `error_code` name of an `ErrorCode` in the `--json` error document.
 * Total over `ErrorCode`: the `switch` has no `default`, so a new variant
 * fails to compile until it has a name. The names are a contract with
 * callers (dsp-api maps them to HTTP status classes, json-output.md).
 */
[[nodiscard]] const char *error_code_name(ErrorCode code);

/*!
 * Emit a structured JSON report mirroring an ImageContext to the given
 * output stream. Used by the `--json` CLI flag to give a full diagnostic
 * payload independent of any telemetry backend.
 *
 * The produced document is a single JSON object terminated by a trailing
 * newline. The schema matches the ImageContext struct in populate_from_image.h.
 * Every document carries `correlation_id` (`get_correlation_id()`, empty when
 * no caller set it).
 *
 * On success (no error_message / no phase): `status == "ok"` and the `image`
 * object is populated from the ImageContext. There is no `error_code` key.
 *
 * On error (`error_message` set): `status == "error"`, `phase` (if supplied),
 * `error_code` and `error_message` are emitted. If `phase == "cli_args"` the
 * `image` object is omitted entirely (no image was loaded); otherwise the
 * `image` object is still emitted and contains whatever the ImageContext
 * captured before the failure point.
 *
 * \param[out] out             Output stream (typically std::cout)
 * \param[in]  ctx             Image context with metadata about the image
 * \param[in]  error_message   If present, renders the document as an error
 * \param[in]  phase           "cli_args" | "read" | "convert" | "write" | "unknown"
 * \param[in]  error_code      An `error_code_name`, or "cli_args" / "internal"
 */
void emit_json_report(std::ostream &out,
  const observability::ImageContext &ctx,
  std::optional<std::string> error_message = std::nullopt,
  std::optional<std::string> phase = std::nullopt,
  std::string error_code = "internal");

/*!
 * Emit a minimal `phase: "cli_args"`, `error_code: "cli_args"` error JSON
 * for CLI argument validation failures that fire before any image is loaded.
 * The `image` object is omitted entirely because no image processing was
 * attempted.
 *
 * \param[out] out  Output stream (typically std::cout)
 * \param[in]  err  Error message describing the argument failure
 */
void emit_json_cli_arg_error(std::ostream &out, const std::string &err);

}// namespace Sipi

#endif// SIPIREPORT_H
