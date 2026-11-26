/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "cli/commands/convert_access_file.h"

#include "cli/commands/decode_deadline.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "logging/logger.h"
#include "image/SipiImage.h"
#include "image/SipiImageError.h"
#include "image_processing/processing.h"
#include "iiifparser/SipiRegion.h"
#include "cli/SipiReport.h"
#include "iiifparser/SipiSize.h"
#include "metadata/essentials.h"
#include "metadata/icc.h"
#include "image/populate_from_image.h"

namespace Sipi::cli {

namespace {

std::string lower_extension(const std::string &path)
{
  const auto dot = path.rfind('.');
  if (dot == std::string::npos || dot + 1 >= path.size()) { return {}; }
  std::string ext = path.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext;
}

/// Map the output extension or `--format` choice to the codec name accepted
/// by `SipiImage::write` ("jpg" / "jpx" / "tif" / "png"). Returns the format
/// string on success or an empty string + error message on failure.
struct FormatResolution
{
  std::string format;
  std::string error;
};

FormatResolution resolve_output_format(const ConvertAccessFileArgs &req)
{
  if (!req.format.empty()) {
    if (req.format == "jpg" || req.format == "jpx" || req.format == "tif" || req.format == "png") {
      return { req.format, {} };
    }
    return { {}, "Unsupported --format value: '" + req.format + "'" };
  }

  const std::string ext = lower_extension(req.output_path);
  if (ext == "jpx" || ext == "jp2") { return { "jpx", {} }; }
  if (ext == "tif" || ext == "tiff") { return { "tif", {} }; }
  if (ext == "jpg" || ext == "jpeg") { return { "jpg", {} }; }
  if (ext == "png") { return { "png", {} }; }
  return { {}, "Not a supported filename extension: '" + ext + "'" };
}

std::shared_ptr<SipiRegion> build_region(const std::vector<int> &region)
{
  if (region.size() < 4) { return nullptr; }
  return std::make_shared<SipiRegion>(region.at(0), region.at(1), region.at(2), region.at(3));
}

struct SizeOrError
{
  std::shared_ptr<SipiSize> size;
  std::string error;
};

SizeOrError build_size(const ConvertAccessFileArgs &req)
{
  if (req.reduce > 0) {
    return { std::make_shared<SipiSize>(req.reduce), {} };
  }
  if (!req.size.empty()) {
    try {
      return { std::make_shared<SipiSize>(req.size), {} };
    } catch (const std::exception &e) {
      return { nullptr, std::string{ "Error in size parameter: " } + e.what() };
    }
  }
  if (req.scale > 0) {
    try {
      return { std::make_shared<SipiSize>(req.scale), {} };
    } catch (const std::exception &e) {
      return { nullptr, std::string{ "Error in scale parameter: " } + e.what() };
    }
  }
  return { nullptr, {} };
}

void report_error(const observability::ImageContext &ctx,
  const std::string &phase,
  const std::string &message,
  const std::string &error_code,
  bool json_output)
{
  log_err("Error %s image: %s", phase.c_str(), message.c_str());
  if (json_output) { emit_json_report(std::cout, ctx, message, phase, error_code); }
}

[[nodiscard]] Result<void> apply_orientation_topleft(SipiImage &img)
{
  Orientation orientation = img.getOrientation();
  auto exif = img.getExif();
  if (exif != nullptr) {
    unsigned short ori;
    if (exif->getValByKey("Exif.Image.Orientation", ori)) {
      orientation = static_cast<Orientation>(ori);
    }
  }
  Result<void> r;
  switch (orientation) {
  case TOPLEFT: break;
  case TOPRIGHT: r = Sipi::processing::rotate(img, 0., true); break;
  case BOTRIGHT: r = Sipi::processing::rotate(img, 180., false); break;
  case BOTLEFT: r = Sipi::processing::rotate(img, 180., true); break;
  case LEFTTOP: r = Sipi::processing::rotate(img, 270., true); break;
  case RIGHTTOP: r = Sipi::processing::rotate(img, 90., false); break;
  case RIGHTBOT: r = Sipi::processing::rotate(img, 90., true); break;
  case LEFTBOT: r = Sipi::processing::rotate(img, 270., false); break;
  default: break;
  }
  if (!r) { return r; }
  if (exif != nullptr) {
    exif->addKeyVal("Exif.Image.Orientation", static_cast<unsigned short>(TOPLEFT));
  }
  img.setOrientation(TOPLEFT);
  return {};
}

[[nodiscard]] Result<void> apply_icc(SipiImage &img, const std::string &icc)
{
  if (icc == "sRGB") { return Sipi::processing::convertToIcc(img, Icc(PredefinedProfiles::icc_sRGB), img.getBps()); }
  if (icc == "AdobeRGB") {
    return Sipi::processing::convertToIcc(img, Icc(PredefinedProfiles::icc_AdobeRGB), img.getBps());
  }
  if (icc == "GRAY") {
    return Sipi::processing::convertToIcc(img, Icc(PredefinedProfiles::icc_GRAY_D50), img.getBps());
  }
  return {};
}

[[nodiscard]] Result<void> apply_rotate_mirror(SipiImage &img, float rotate, const std::string &mirror)
{
  if (mirror == "vertical") { return Sipi::processing::rotate(img, rotate + 180.0F, true); }
  if (mirror == "horizontal") { return Sipi::processing::rotate(img, rotate, true); }
  if (rotate != 0.0F) { return Sipi::processing::rotate(img, rotate, false); }
  return {};
}

}// namespace

int cmd_convert_access_file(const ConvertAccessFileArgs &args)
{
  set_cli_mode(true);
  if (args.json_output) { set_json_mode(true); }

  const FormatResolution format_res = resolve_output_format(args);
  if (!format_res.error.empty()) {
    log_err("%s", format_res.error.c_str());
    if (args.json_output) { emit_json_cli_arg_error(std::cout, format_res.error); }
    return EXIT_FAILURE;
  }
  const std::string &format = format_res.format;

  std::shared_ptr<SipiRegion> region = build_region(args.region);
  const SizeOrError size_res = build_size(args);
  if (!size_res.error.empty()) {
    log_err("%s", size_res.error.c_str());
    if (args.json_output) { emit_json_cli_arg_error(std::cout, size_res.error); }
    return EXIT_FAILURE;
  }
  const std::shared_ptr<SipiSize> size = size_res.size;

  observability::ImageContext sentry_ctx;
  sentry_ctx.input_file = args.input_path;
  sentry_ctx.output_file = args.output_path;
  sentry_ctx.output_format = format;
  sentry_ctx.file_size_bytes = observability::get_file_size(args.input_path);

  //
  // Read input. The format-handler reader populates the Essentials packet
  // if the source carries one — that's what we gate the Access File
  // contract on (input MUST be a Service File per ADR-0009).
  //
  auto decoded = read_source_with_deadline(args.decode_timeout_ms, args.input_path, region, size);
  if (!decoded) {
    report_error(sentry_ctx, "read", decode_deadline_message(args.decode_timeout_ms), "internal", args.json_output);
    return EXIT_FAILURE;
  }
  SipiImage img = std::move(decoded->img);
  if (auto &r = decoded->status; !r) {
    observability::populate_from_image(sentry_ctx, img);
    report_error(
      sentry_ctx, "read", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
    return EXIT_FAILURE;
  }
  try {
    if (format == "jpg") {
      if (auto r = Sipi::processing::to8bps(img); !r) {
        observability::populate_from_image(sentry_ctx, img);
        report_error(
          sentry_ctx, "convert", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
        return EXIT_FAILURE;
      }
      if (auto r = Sipi::processing::convertToIcc(img, Icc(PredefinedProfiles::icc_sRGB), 8); !r) {
        observability::populate_from_image(sentry_ctx, img);
        report_error(
          sentry_ctx, "convert", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
        return EXIT_FAILURE;
      }
    }
  } catch (const std::exception &err) {
    observability::populate_from_image(sentry_ctx, img);
    report_error(sentry_ctx, "convert", err.what(), "internal", args.json_output);
    return EXIT_FAILURE;
  }

  //
  // Validate: input MUST be a Service File (Essentials packet present).
  // Otherwise route the operator at the generic `sipi convert` verb.
  //
  if (!img.essential_metadata().is_set()) {
    const std::string msg = "convert access-file requires a Service File input; " + args.input_path
      + " has no Essentials packet. Use 'sipi convert' for generic format conversion.";
    log_err("%s", msg.c_str());
    if (args.json_output) { emit_json_cli_arg_error(std::cout, msg); }
    return EXIT_FAILURE;
  }

  //
  // Access Files do not carry Essentials. Drop the in-memory packet so
  // the writer has nothing to emit even if the gate were misconfigured.
  //
  img.essential_metadata(Essentials{});

  //
  // Apply transformations.
  //
  try {
    if (args.set_topleft) {
      if (auto r = apply_orientation_topleft(img); !r) {
        observability::populate_from_image(sentry_ctx, img);
        report_error(
          sentry_ctx, "convert", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
        return EXIT_FAILURE;
      }
    }
    if (!args.icc.empty() && args.icc != "none") {
      if (auto r = apply_icc(img, args.icc); !r) {
        observability::populate_from_image(sentry_ctx, img);
        report_error(
          sentry_ctx, "convert", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
        return EXIT_FAILURE;
      }
    }
    if (!args.mirror.empty() || args.rotate != 0.0F) {
      if (auto r = apply_rotate_mirror(img, args.rotate, args.mirror); !r) {
        observability::populate_from_image(sentry_ctx, img);
        report_error(
          sentry_ctx, "convert", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
        return EXIT_FAILURE;
      }
    }
    if (!args.watermark.empty()) {
      if (auto r = Sipi::processing::add_watermark(img, args.watermark); !r) {
        observability::populate_from_image(sentry_ctx, img);
        report_error(
          sentry_ctx, "convert", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
        return EXIT_FAILURE;
      }
    }
  } catch (const SipiImageError &err) {
    observability::populate_from_image(sentry_ctx, img);
    report_error(sentry_ctx, "convert", err.what(), "internal", args.json_output);
    return EXIT_FAILURE;
  } catch (const std::exception &err) {
    observability::populate_from_image(sentry_ctx, img);
    report_error(sentry_ctx, "convert", err.what(), "internal", args.json_output);
    return EXIT_FAILURE;
  }

  //
  // Write output. The in-memory Essentials packet was dropped above, so
  // the writer's emit gate (essential_metadata().is_set()) stays closed
  // and no Essentials carrier is emitted (ADR-0010). XMP /
  // IPTC / EXIF / ICC propagate via the standard write paths (ADR-0011).
  //
  SipiCompressionParams params;
  if (args.jpeg_quality > 0) { params[JPEG_QUALITY] = std::to_string(args.jpeg_quality); }

  try {
    if (auto r = img.write(format, args.output_path, &params); !r) {
      observability::populate_from_image(sentry_ctx, img);
      report_error(
        sentry_ctx, "write", r.error().diagnostic_message(), error_code_name(r.error().code()), args.json_output);
      return EXIT_FAILURE;
    }
  } catch (const SipiImageError &err) {
    observability::populate_from_image(sentry_ctx, img);
    report_error(sentry_ctx, "write", err.what(), "internal", args.json_output);
    return EXIT_FAILURE;
  } catch (const std::exception &err) {
    observability::populate_from_image(sentry_ctx, img);
    report_error(sentry_ctx, "write", err.what(), "internal", args.json_output);
    return EXIT_FAILURE;
  }

  if (args.json_output) {
    observability::populate_from_image(sentry_ctx, img);
    emit_json_report(std::cout, sentry_ctx);
  }
  return EXIT_SUCCESS;
}

}// namespace Sipi::cli
