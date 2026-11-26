/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "SipiReport.h"

#include <jansson.h>

#include "logging/logger.h"

#include <cstdlib>
#include <memory>
#include <string>

namespace Sipi {

namespace {

/*! RAII wrapper for the reference-counted jansson value root. */
using JsonPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

/*! RAII wrapper for malloc'd strings returned by `json_dumps`. */
using JsonStr = std::unique_ptr<char, decltype(&std::free)>;

/*! Build the nested `image` object from an ImageContext. Ownership is
 * transferred to the caller via a new reference; the caller is expected
 * to pass the returned pointer to `json_pack` with the `o` format
 * (which steals the reference) or wrap it in a JsonPtr. */
json_t *build_image_object(const observability::ImageContext &ctx)
{
  return json_pack("{s:I, s:I, s:I, s:I, s:s, s:s, s:s}",
    "width",
    static_cast<json_int_t>(ctx.width),
    "height",
    static_cast<json_int_t>(ctx.height),
    "channels",
    static_cast<json_int_t>(ctx.channels),
    "bps",
    static_cast<json_int_t>(ctx.bps),
    "colorspace",
    ctx.colorspace.c_str(),
    "icc_profile_type",
    ctx.icc_profile_type.c_str(),
    "orientation",
    ctx.orientation.c_str());
}

/*! The document for a jansson failure, built without jansson. The
 * correlation ID is spliced raw: its only setter passes a value of
 * `[A-Za-z0-9._:-]` (the Rust CLI validates `SIPI_CORRELATION_ID`). `phase`
 * and `error_code` are names from the code, never user text. An error
 * document keeps its phase and code, so a message jansson rejects (one
 * that is not valid UTF-8, such as a Latin-1 path) does not turn an input
 * error into an internal one. */
void emit_fallback_error(std::ostream &out,
  const std::string &phase,
  const std::string &error_code,
  const char *message)
{
  out << "{\"status\": \"error\", \"mode\": \"cli\", \"phase\": \"" << phase << "\", " << "\"error_code\": \""
      << error_code << "\", \"correlation_id\": \"" << get_correlation_id() << "\", " << "\"error_message\": \""
      << message << "\"}\n";
}

}// namespace

const char *error_code_name(ErrorCode code)
{
  switch (code) {
  case ErrorCode::kDecodeFailed: return "decode_failed";
  case ErrorCode::kUnsupportedFormat: return "unsupported_format";
  case ErrorCode::kMalformedInput: return "malformed_input";
  case ErrorCode::kWriteFailed: return "write_failed";
  case ErrorCode::kClientAbort: return "client_abort";
  case ErrorCode::kShapeProbeFailed: return "shape_probe_failed";
  case ErrorCode::kMetadataParseFailed: return "metadata_parse_failed";
  case ErrorCode::kInvalidRequestParameter: return "invalid_request_parameter";
  }
  return "internal";
}

void emit_json_report(std::ostream &out,
  const observability::ImageContext &ctx,
  std::optional<std::string> error_message,
  std::optional<std::string> phase,
  std::string error_code)
{
  const std::string correlation_id = get_correlation_id();
  const bool is_error = error_message.has_value();
  const bool omit_image = phase.has_value() && *phase == "cli_args";

  JsonPtr root{ nullptr, &json_decref };
  if (!is_error) {
    // Success payload. Always has an `image` object. json_pack with `o`
    // steals the reference to `image`, but only on success — guard against
    // jansson allocation failure by wrapping `image` in a JsonPtr until
    // ownership transfer is confirmed.
    JsonPtr image{ build_image_object(ctx), &json_decref };
    if (image == nullptr) {
      emit_fallback_error(out, "internal", "internal", "SipiReport: failed to build image object");
      return;
    }
    root.reset(json_pack("{s:s, s:s, s:s, s:s, s:s, s:s, s:I, s:o}",
      "status",
      "ok",
      "mode",
      "cli",
      "correlation_id",
      correlation_id.c_str(),
      "input_file",
      ctx.input_file.c_str(),
      "output_file",
      ctx.output_file.c_str(),
      "output_format",
      ctx.output_format.c_str(),
      "file_size_bytes",
      static_cast<json_int_t>(ctx.file_size_bytes),
      "image",
      image.get()));
    if (root != nullptr) {
      // json_pack's `o` format stole the reference; release our guard.
      (void)image.release();
    }
  } else {
    // Error payload.
    root.reset(json_pack("{s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:I}",
      "status",
      "error",
      "mode",
      "cli",
      "correlation_id",
      correlation_id.c_str(),
      "phase",
      phase.value_or("").c_str(),
      "error_code",
      error_code.c_str(),
      "error_message",
      error_message->c_str(),
      "input_file",
      ctx.input_file.c_str(),
      "output_file",
      ctx.output_file.c_str(),
      "output_format",
      ctx.output_format.c_str(),
      "file_size_bytes",
      static_cast<json_int_t>(ctx.file_size_bytes)));
    if (!omit_image && root != nullptr) {
      JsonPtr image{ build_image_object(ctx), &json_decref };
      if (image != nullptr && json_object_set_new(root.get(), "image", image.get()) == 0) {
        // json_object_set_new steals the reference on success.
        (void)image.release();
      }
    }
  }

  if (root == nullptr) {
    // Defensive: json_pack failed (should not happen with the shapes above).
    emit_fallback_error(out,
      is_error ? phase.value_or("") : std::string{ "internal" },
      is_error ? error_code : std::string{ "internal" },
      "SipiReport: json_pack failed");
    return;
  }

  // Wrap the malloc'd dump in a JsonStr so it is freed even if `out <<` throws.
  JsonStr dump{ json_dumps(root.get(), JSON_COMPACT), &std::free };
  if (dump != nullptr) { out << dump.get() << '\n'; }
  // `root` and `dump` are cleaned up automatically when the JsonPtr/JsonStr
  // destructors run, regardless of whether the stream insertion threw.
}

void emit_json_cli_arg_error(std::ostream &out, const std::string &err)
{
  observability::ImageContext empty;
  emit_json_report(out, empty, err, std::string{ "cli_args" }, "cli_args");
}

}// namespace Sipi
