/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * \brief Implements an IIIF server with many features.
 *
 */
#include <climits>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include <fstream>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <jansson.h>

#include <CLI/CLI.hpp>
#include "logging/logger.h"
#include "util/Error.h"
#include "cli/commands/convert_access_file.h"
#include "cli/commands/convert_service_file.h"
#include "cli/commands/decode_deadline.h"
#include "cli/commands/health.h"
#include "cli/commands/verify.h"
#include "image/SipiIO.h"
#include "image/SipiImage.h"
#include "image/SipiImageError.h"
#include "image_processing/processing.h"
#include "ffi/engine_context.h"
#include "ffi/sipi_ffi.h"
#include "ffi/startup.h"
#include "cli/SipiReport.h"
#include "image/populate_from_image.h"
#include "format_handlers/SipiIOTiff.h"

#include "generated/SipiVersion.h"

#ifdef __linux__
#include <sched.h>
#endif

// A macro for silencing incorrect compiler warnings about unused variables.
#define _unused(x) ((void)(x))



/*!
 * \mainpage
 *
 * # Sipi – Simple Image Presentation Interface #
 *
 * Sipi is a package that can be used to convert images from/to different formats while
 * preserving as much metadata thats embeded in the file headers a possible. Sipi is also
 * able to do some conversions, especially some common color space transformation using
 * ICC profiles. Currently Sipi supports the following file formats
 *
 * - TIFF
 * - JPEG2000
 * - PNG
 * - JPEG
 *
 * The following metadata "standards" are beeing preserved
 * - EXIF
 * - IPTC
 * - XMP
 *
 * ## Commandline Use ##
 *
 * For simple conversions, Sipi is being used from the command line (in a terminal window). The
 * format is usually
 *
 *     sipi [options] <infile> <outfile>
 *
 */


// small function to check if file exist
inline bool exists_file(const std::string &name)
{
  struct stat buffer;
  return (stat(name.c_str(), &buffer) == 0);
}


extern "C" void sipi_cli_set_correlation_id(const char *id) { set_correlation_id(id != nullptr ? id : ""); }

/*!
 * The CLI entry point, behind the FFI seam (`ffi/sipi_ffi.h`).
 *
 * Owns the CLI11 app and every offline subcommand (`convert`, `verify`,
 * `query`, `compare`, `health`). Lives in `//src/cli:cli_app` so the Rust
 * shell can link it and call `sipi_cli_main` without colliding with
 * the binary's own `main`. Returns the exit code rather than calling `exit()`
 * — the caller (the C++ `main`, or the Rust shell) owns process teardown.
 *
 * @param argc the number of command line arguments.
 * @param argv the command line arguments.
 * @return the exit code.
 */
extern "C" int sipi_cli_main(int argc, char **argv)
{
  // Fast path: print version and exit before any library initialisation.
  // Doing this after curl/exiv2/TIFF init leaks their static registries
  // at exit and trips the LSan gate in the sanitizer e2e suite.
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--version") {
      std::cout << "sipi " << VERSION << std::endl;
      return 0;
    }
  }

  //
  // first we initialize the libraries that sipi uses
  //
  try {
    Sipi::ffi::LibraryInitialiser &lib_init = Sipi::ffi::LibraryInitialiser::instance();
    _unused(lib_init);// Silence compiler warning about unused variable.
  } catch (shttps::Error &e) {
    log_err("Library initialization failed: %s", e.to_string().c_str());
    return EXIT_FAILURE;
  }

  CLI::App sipiopt("SIPI is an IIIF image server and image format converter.");
  sipiopt.require_subcommand(1);

  // Exit code recorded by the matched subcommand callback, returned after
  // dispatch. Replaces the legacy `exit(run_X())` so no exit()/abort() crosses
  // the FFI boundary when the Rust shell calls sipi_cli_main. require_subcommand(1)
  // guarantees exactly one leaf callback assigns it (the `convert`/`verify`
  // parent callbacks self-skip when a nested subcommand matched).
  int sipi_exit_code = EXIT_SUCCESS;

  //
  // Option storage variables.
  //
  // Storage declarations live at the top of main so the helper lambdas
  // (attach_*_opts) and the body lambdas (run_query, run_compare,
  // run_convert) can all capture them by reference. Each option's CLI11
  // registration happens on the appropriate subcommand below.
  //
  std::string optConfigfile;
  std::string optInFile;
  std::string optOutFile;
  std::vector<std::string> optCompare;

  enum class OptFormat : int { jpx, jpg, tif, png };
  OptFormat optFormat = OptFormat::jpx;
  const std::vector<std::pair<std::string, OptFormat>> optFormatMap{ { "jpx", OptFormat::jpx },
    { "jp2", OptFormat::jpx },
    { "jpg", OptFormat::jpg },
    { "tif", OptFormat::tif },
    { "png", OptFormat::png } };

  enum class OptIcc : int { none, sRGB, AdobeRGB, GRAY };
  OptIcc optIcc = OptIcc::none;
  const std::vector<std::pair<std::string, OptIcc>> optIccMap{
    { "none", OptIcc::none }, { "sRGB", OptIcc::sRGB }, { "AdobeRGB", OptIcc::AdobeRGB }, { "GRAY", OptIcc::GRAY }
  };

  enum class OptMirror { none, horizontal, vertical };
  OptMirror optMirror = OptMirror::none;
  const std::vector<std::pair<std::string, OptMirror>> optMirrorMap{
    { "none", OptMirror::none }, { "horizontal", OptMirror::horizontal }, { "vertical", OptMirror::vertical }
  };

  int optJpegQuality = 60;
  std::vector<int> optRegion;
  int optReduce = 0;
  std::string optSize;
  int optScale = 0;
  bool optSkipMeta = false;
  float optRotate = 0.0;
  bool optSetTopleft = false;
  std::string optWatermark;
  bool optJsonOutput = false;
  int optPagenum = 0;

  // JPEG2000 / pyramidal-TIFF tuning knobs (only valid on `convert`).
  std::string j2k_Sprofile;
  std::vector<std::string> j2k_rates;
  int j2k_Clayers = 0;
  int j2k_Clevels = 0;
  std::string j2k_Corder;
  std::string j2k_Stiles;
  std::string j2k_Cprecincts;
  std::string j2k_Cblk;
  bool j2k_Cuse_sop = false;
  bool tiff_Pyramid = false;


  //
  // Body lambdas, each capturing the option storage.
  //
  // Each CLI mode's body is captured in a named lambda over the option
  // storage. The subcommand callbacks below invoke them and record the result
  // into `sipi_exit_code`.
  //
  // `query` and `compare` print text on success; under --json only an error
  // is a JSON document.
  auto json_error =
    [&](const std::string &input, const std::string &message, const std::string &phase, const std::string &error_code) {
      if (!optJsonOutput) { return; }
      Sipi::observability::ImageContext ctx;
      ctx.input_file = input;
      Sipi::emit_json_report(std::cout, ctx, message, phase, error_code);
    };

  auto run_query = [&]() -> int {
    set_cli_mode(true);
    if (optJsonOutput) { set_json_mode(true); }
    auto decoded = Sipi::cli::read_metadata_with_deadline(Sipi::ffi::kDefaultDecodeTimeoutMs, optInFile);
    if (!decoded) {
      const std::string msg = Sipi::cli::decode_deadline_message(Sipi::ffi::kDefaultDecodeTimeoutMs);
      log_err("sipi: %s", msg.c_str());
      json_error(optInFile, msg, "read", "internal");
      return EXIT_FAILURE;
    }
    if (auto &r = decoded->status; !r) {
      log_err("sipi: unhandled exception: %s", r.error().diagnostic_message().c_str());
      json_error(optInFile, r.error().diagnostic_message(), "read", Sipi::error_code_name(r.error().code()));
      return EXIT_FAILURE;
    }
    std::cout << decoded->img << std::endl;
    return 0;
  };

  //
  // Convert body. The `src` parameter points at the invoking subcommand
  // (cmd_convert today; a future access-file command will call this
  // body too). `user_set` queries
  // the subcommand's own option group to detect whether each flag was
  // explicitly set by the operator.
  //
  auto run_convert = [&](CLI::App *src) -> int {
    auto user_set = [&](const std::string &name) -> bool {
      auto *s = src->get_option_no_throw(name);
      return s != nullptr && !s->empty();
    };

    set_cli_mode(true);
    // Under --json, route all log output (info, warn, err) to stderr so stdout
    // stays reserved for the single JSON document emitted at the end of the
    // CLI run.
    if (optJsonOutput) { set_json_mode(true); }

    //
    // get the output format
    //
    std::string format("jpg");
    if (user_set("--format")) {
      switch (optFormat) {
      case OptFormat::jpx: format = "jpx"; break;
      case OptFormat::jpg: format = "jpg"; break;
      case OptFormat::tif: format = "tif"; break;
      case OptFormat::png: format = "png"; break;
      }
    } else {
      //
      // there is no format option given – we try to determine the format
      // from the output name extension
      //
      size_t pos = optOutFile.rfind('.');
      if (pos != std::string::npos) {
        std::string ext = optOutFile.substr(pos + 1);
        if ((ext == "jpx") || (ext == "jp2")) {
          format = "jpx";
        } else if ((ext == "tif") || (ext == "tiff")) {
          format = "tif";
        } else if ((ext == "jpg") || (ext == "jpeg")) {
          format = "jpg";
        } else if (ext == "png") {
          format = "png";
        } else {
          const std::string msg = "Not a supported filename extension: '" + ext + "'";
          log_err("%s", msg.c_str());
          if (optJsonOutput) { Sipi::emit_json_cli_arg_error(std::cout, msg); }
          return EXIT_FAILURE;
        }
      }
    }

    //
    // getting information about a region of interest
    //
    std::shared_ptr<Sipi::SipiRegion> region = nullptr;
    if (user_set("--region")) {
      region = std::make_shared<Sipi::SipiRegion>(optRegion.at(0), optRegion.at(1), optRegion.at(2), optRegion.at(3));
    }

    //
    // get the reduce parameter
    //
    std::shared_ptr<Sipi::SipiSize> size = nullptr;
    if (optReduce > 0) {
      size = std::make_shared<Sipi::SipiSize>(optReduce);
    } else if (user_set("--size")) {
      try {
        size = std::make_shared<Sipi::SipiSize>(optSize);
      } catch (std::exception &e) {
        const std::string msg = std::string{ "Error in size parameter: " } + e.what();
        log_err("%s", msg.c_str());
        if (optJsonOutput) { Sipi::emit_json_cli_arg_error(std::cout, msg); }
        return EXIT_FAILURE;
      }
    } else if (user_set("--scale")) {
      try {
        size = std::make_shared<Sipi::SipiSize>(optScale);
      } catch (std::exception &e) {
        const std::string msg = std::string{ "Error in scale parameter: " } + e.what();
        log_err("%s", msg.c_str());
        if (optJsonOutput) { Sipi::emit_json_cli_arg_error(std::cout, msg); }
        return EXIT_FAILURE;
      }
    }

    //
    // Prepare the image context for the --json report / log messages
    //
    Sipi::observability::ImageContext sentry_ctx;
    sentry_ctx.input_file = optInFile;
    sentry_ctx.output_file = optOutFile;
    sentry_ctx.output_format = format;
    sentry_ctx.file_size_bytes = Sipi::observability::get_file_size(optInFile);

    //
    // read the input image
    //
    auto decoded = Sipi::cli::read_source_with_deadline(Sipi::ffi::kDefaultDecodeTimeoutMs, optInFile, region, size);
    if (!decoded) {
      const std::string msg = Sipi::cli::decode_deadline_message(Sipi::ffi::kDefaultDecodeTimeoutMs);
      log_err("Error reading image: %s", msg.c_str());
      if (optJsonOutput) { Sipi::emit_json_report(std::cout, sentry_ctx, msg, std::string{ "read" }); }
      return EXIT_FAILURE;
    }
    Sipi::SipiImage img = std::move(decoded->img);
    if (auto &r = decoded->status; !r) {
      Sipi::observability::populate_from_image(sentry_ctx, img);
      log_err("Error reading image: %s", r.error().diagnostic_message().c_str());
      if (optJsonOutput) {
        Sipi::emit_json_report(std::cout,
          sentry_ctx,
          r.error().diagnostic_message(),
          std::string{ "read" },
          Sipi::error_code_name(r.error().code()));
      }
      return EXIT_FAILURE;
    }
    try {
      if (format == "jpg") {
        if (auto r = Sipi::processing::to8bps(img); !r) {
          Sipi::observability::populate_from_image(sentry_ctx, img);
          log_err("Error converting image: %s", r.error().diagnostic_message().c_str());
          if (optJsonOutput) {
            Sipi::emit_json_report(std::cout,
              sentry_ctx,
              r.error().diagnostic_message(),
              std::string{ "convert" },
              Sipi::error_code_name(r.error().code()));
          }
          return EXIT_FAILURE;
        }
        if (auto r = Sipi::processing::convertToIcc(img, Sipi::Icc(Sipi::PredefinedProfiles::icc_sRGB), 8); !r) {
          Sipi::observability::populate_from_image(sentry_ctx, img);
          log_err("Error converting image: %s", r.error().diagnostic_message().c_str());
          if (optJsonOutput) {
            Sipi::emit_json_report(std::cout,
              sentry_ctx,
              r.error().diagnostic_message(),
              std::string{ "convert" },
              Sipi::error_code_name(r.error().code()));
          }
          return EXIT_FAILURE;
        }
      }
    } catch (const std::exception &err) {
      Sipi::observability::populate_from_image(sentry_ctx, img);
      log_err("Error converting image: %s", err.what());
      if (optJsonOutput) { Sipi::emit_json_report(std::cout, sentry_ctx, err.what(), std::string{ "convert" }); }
      return EXIT_FAILURE;
    }

    //
    // image processing: orientation, metadata, ICC, rotation, watermark
    //
    try {
      if (user_set("--topleft")) {
        Sipi::Orientation orientation = img.getOrientation();
        std::shared_ptr<Sipi::Exif> exif = img.getExif();
        if (exif != nullptr) {
          unsigned short ori;
          if (exif->getValByKey("Exif.Image.Orientation", ori)) { orientation = static_cast<Sipi::Orientation>(ori); }
        }
        Sipi::Result<void> r;
        switch (orientation) {
        case Sipi::TOPLEFT: break;
        case Sipi::TOPRIGHT: r = Sipi::processing::rotate(img, 0., true); break;
        case Sipi::BOTRIGHT: r = Sipi::processing::rotate(img, 180., false); break;
        case Sipi::BOTLEFT: r = Sipi::processing::rotate(img, 180., true); break;
        case Sipi::LEFTTOP: r = Sipi::processing::rotate(img, 270., true); break;
        case Sipi::RIGHTTOP: r = Sipi::processing::rotate(img, 90., false); break;
        case Sipi::RIGHTBOT: r = Sipi::processing::rotate(img, 90., true); break;
        case Sipi::LEFTBOT: r = Sipi::processing::rotate(img, 270., false); break;
        default:;
        }
        if (!r) {
          Sipi::observability::populate_from_image(sentry_ctx, img);
          log_err("Error processing image: %s", r.error().diagnostic_message().c_str());
          if (optJsonOutput) {
            Sipi::emit_json_report(std::cout,
              sentry_ctx,
              r.error().diagnostic_message(),
              std::string{ "convert" },
              Sipi::error_code_name(r.error().code()));
          }
          return EXIT_FAILURE;
        }
        exif->addKeyVal("Exif.Image.Orientation", static_cast<unsigned short>(Sipi::TOPLEFT));
        img.setOrientation(Sipi::TOPLEFT);
      }

      if (user_set("--skipmeta")) { img.setSkipMetadata(Sipi::SkipMetadata::SKIP_ALL); }

      if (user_set("--icc")) {
        Sipi::Icc icc;
        switch (optIcc) {
        case OptIcc::sRGB: icc = Sipi::Icc(Sipi::PredefinedProfiles::icc_sRGB); break;
        case OptIcc::AdobeRGB: icc = Sipi::Icc(Sipi::PredefinedProfiles::icc_AdobeRGB); break;
        case OptIcc::GRAY: icc = Sipi::Icc(Sipi::PredefinedProfiles::icc_GRAY_D50); break;
        case OptIcc::none: break;
        }
        if (auto r = Sipi::processing::convertToIcc(img, icc, img.getBps()); !r) {
          Sipi::observability::populate_from_image(sentry_ctx, img);
          log_err("Error processing image: %s", r.error().diagnostic_message().c_str());
          if (optJsonOutput) {
            Sipi::emit_json_report(std::cout,
              sentry_ctx,
              r.error().diagnostic_message(),
              std::string{ "convert" },
              Sipi::error_code_name(r.error().code()));
          }
          return EXIT_FAILURE;
        }
      }

      if (user_set("--mirror") || user_set("--rotate")) {
        Sipi::Result<void> r;
        switch (optMirror) {
        case OptMirror::vertical: r = Sipi::processing::rotate(img, optRotate + 180.0F, true); break;
        case OptMirror::horizontal: r = Sipi::processing::rotate(img, optRotate, true); break;
        case OptMirror::none:
          if (optRotate != 0.0F) { r = Sipi::processing::rotate(img, optRotate, false); }
          break;
        }
        if (!r) {
          Sipi::observability::populate_from_image(sentry_ctx, img);
          log_err("Error processing image: %s", r.error().diagnostic_message().c_str());
          if (optJsonOutput) {
            Sipi::emit_json_report(std::cout,
              sentry_ctx,
              r.error().diagnostic_message(),
              std::string{ "convert" },
              Sipi::error_code_name(r.error().code()));
          }
          return EXIT_FAILURE;
        }
      }

      if (user_set("--watermark")) {
        if (auto r = Sipi::processing::add_watermark(img, optWatermark); !r) {
          Sipi::observability::populate_from_image(sentry_ctx, img);
          log_err("Error processing image: %s", r.error().diagnostic_message().c_str());
          if (optJsonOutput) {
            Sipi::emit_json_report(std::cout,
              sentry_ctx,
              r.error().diagnostic_message(),
              std::string{ "convert" },
              Sipi::error_code_name(r.error().code()));
          }
          return EXIT_FAILURE;
        }
      }
    } catch (const Sipi::SipiImageError &err) {
      Sipi::observability::populate_from_image(sentry_ctx, img);
      log_err("Error processing image: %s", err.what());
      if (optJsonOutput) { Sipi::emit_json_report(std::cout, sentry_ctx, err.what(), std::string{ "convert" }); }
      return EXIT_FAILURE;
    } catch (const std::exception &err) {
      Sipi::observability::populate_from_image(sentry_ctx, img);
      log_err("Error processing image: %s", err.what());
      if (optJsonOutput) { Sipi::emit_json_report(std::cout, sentry_ctx, err.what(), std::string{ "convert" }); }
      return EXIT_FAILURE;
    }

    //
    // write the output file
    //
    Sipi::SipiCompressionParams comp_params;
    // SipiCompressionParams maps to std::string; optJpegQuality is an int, so it
    // MUST be stringified (like the J2K int params below). Assigning the int
    // directly bound to std::string::operator=(char), storing the byte 0x50
    // ('P') and making the JPEG writer's stoi() throw.
    if (user_set("--quality")) comp_params[Sipi::JPEG_QUALITY] = std::to_string(optJpegQuality);
    if (user_set("--Sprofile")) comp_params[Sipi::J2K_Sprofile] = j2k_Sprofile;
    if (user_set("--Clayers")) comp_params[Sipi::J2K_Clayers] = std::to_string(j2k_Clayers);
    if (user_set("--Clevels")) comp_params[Sipi::J2K_Clevels] = std::to_string(j2k_Clevels);
    if (user_set("--Corder")) comp_params[Sipi::J2K_Corder] = j2k_Corder;
    if (user_set("--Cprecincts")) comp_params[Sipi::J2K_Cprecincts] = j2k_Cprecincts;
    if (user_set("--Cblk")) comp_params[Sipi::J2K_Cblk] = j2k_Cblk;
    if (user_set("--Cuse_sop")) comp_params[Sipi::J2K_Cuse_sop] = j2k_Cuse_sop ? "yes" : "no";
    if (user_set("--Stiles")) comp_params[Sipi::J2K_Stiles] = j2k_Stiles;
    if (user_set("--Ctiff_pyramid")) comp_params[Sipi::TIFF_Pyramid] = tiff_Pyramid ? "yes" : "no";

    if (user_set("--rates")) {
      std::stringstream ss;
      for (auto &rate : j2k_rates) {
        if (rate == "X") {
          ss << "-1.0 ";
        } else {
          ss << rate << " ";
        }
      }
      comp_params[Sipi::J2K_rates] = ss.str();
    }

    try {
      if (auto r = img.write(format, optOutFile, &comp_params); !r) {
        Sipi::observability::populate_from_image(sentry_ctx, img);
        log_err("Error writing image: %s", r.error().diagnostic_message().c_str());
        if (optJsonOutput) {
          Sipi::emit_json_report(std::cout,
            sentry_ctx,
            r.error().diagnostic_message(),
            std::string{ "write" },
            Sipi::error_code_name(r.error().code()));
        }
        return EXIT_FAILURE;
      }
    } catch (const Sipi::SipiImageError &err) {
      Sipi::observability::populate_from_image(sentry_ctx, img);
      log_err("Error writing image: %s", err.what());
      if (optJsonOutput) { Sipi::emit_json_report(std::cout, sentry_ctx, err.what(), std::string{ "write" }); }
      return EXIT_FAILURE;
    } catch (const std::exception &err) {
      Sipi::observability::populate_from_image(sentry_ctx, img);
      log_err("Error writing image: %s", err.what());
      if (optJsonOutput) { Sipi::emit_json_report(std::cout, sentry_ctx, err.what(), std::string{ "write" }); }
      return EXIT_FAILURE;
    }

    // Successful CLI completion — emit the structured JSON report if --json was set.
    if (optJsonOutput) {
      Sipi::observability::populate_from_image(sentry_ctx, img);
      Sipi::emit_json_report(std::cout, sentry_ctx);
    }
    return EXIT_SUCCESS;
  };

  auto run_compare = [&]() -> int {
    set_cli_mode(true);
    if (optJsonOutput) { set_json_mode(true); }
    for (const auto &file : optCompare) {
      if (!exists_file(file)) {
        const std::string msg = "File not found: " + file;
        log_err("%s", msg.c_str());
        json_error(file, msg, "cli_args", "cli_args");
        return EXIT_FAILURE;
      }
    }

    auto decoded1 = Sipi::cli::read_with_deadline(Sipi::ffi::kDefaultDecodeTimeoutMs, optCompare[0]);
    if (!decoded1) {
      const std::string msg = Sipi::cli::decode_deadline_message(Sipi::ffi::kDefaultDecodeTimeoutMs);
      log_err("sipi: %s", msg.c_str());
      json_error(optCompare[0], msg, "read", "internal");
      return EXIT_FAILURE;
    }
    if (auto &r = decoded1->status; !r) {
      log_err("sipi: unhandled exception: %s", r.error().diagnostic_message().c_str());
      json_error(optCompare[0], r.error().diagnostic_message(), "read", Sipi::error_code_name(r.error().code()));
      return EXIT_FAILURE;
    }
    auto decoded2 = Sipi::cli::read_with_deadline(Sipi::ffi::kDefaultDecodeTimeoutMs, optCompare[1]);
    if (!decoded2) {
      const std::string msg = Sipi::cli::decode_deadline_message(Sipi::ffi::kDefaultDecodeTimeoutMs);
      log_err("sipi: %s", msg.c_str());
      json_error(optCompare[1], msg, "read", "internal");
      return EXIT_FAILURE;
    }
    if (auto &r = decoded2->status; !r) {
      log_err("sipi: unhandled exception: %s", r.error().diagnostic_message().c_str());
      json_error(optCompare[1], r.error().diagnostic_message(), "read", Sipi::error_code_name(r.error().code()));
      return EXIT_FAILURE;
    }
    Sipi::SipiImage &img1 = decoded1->img;
    Sipi::SipiImage &img2 = decoded2->img;

    if (img1 == img2) {
      log_info("Files identical!");
      return 0;
    }

    // Capture the per-channel delta from the original pixels before the
    // `Sipi::processing::subtract` visualization step below rewrites img1
    // into the normalized diff (which would otherwise corrupt the reported
    // avg/max).
    const std::optional<Sipi::PixelDelta> delta = Sipi::processing::maxPixelDelta(img1, img2);

    if (!delta.has_value()) {
      // Differing channel count / bit depth / photometric interpretation:
      // no meaningful per-channel delta, and `Sipi::processing::subtract`
      // would return an error.
      log_info("Files differ: dimensions or format not comparable.");
      return -1;
    }

    if (auto r = Sipi::processing::subtract(img1, img2); !r) {
      log_err("sipi: unhandled exception: %s", r.error().diagnostic_message().c_str());
      json_error(optCompare[0], r.error().diagnostic_message(), "convert", Sipi::error_code_name(r.error().code()));
      return EXIT_FAILURE;
    }
    if (auto r = img1.write("tif", "diff.tif"); !r) {
      log_err("sipi: unhandled exception: %s", r.error().diagnostic_message().c_str());
      json_error(optCompare[0], r.error().diagnostic_message(), "write", Sipi::error_code_name(r.error().code()));
      return EXIT_FAILURE;
    }
    log_info("Files differ: avg: %f max: %d (%zu, %zu) See diff.tif",
      delta->mean_abs,
      delta->max_abs,
      delta->max_x,
      delta->max_y);

    return -1;
  };


  //
  // Subcommand surface.
  //
  // Two tiers per ADR-0010:
  //   - generic verbs: `convert <in> <out>`, `verify <file>`, `query`,
  //     `compare` (anyone-use, ImageMagick-style).
  //   - pipeline-stage verbs under `convert` / `verify`: `access-file`,
  //     `service-file`, `preservation-file` (DSP preservation-chain
  //     semantics).
  //
  // `sipiopt.require_subcommand(1)` (set near the top of main) gates
  // every invocation through one of these subcommands; a bare `sipi`
  // exits with a usage error. The `preservation-file` callbacks under
  // `convert` and `verify` remain stubs pending ADR-0012.
  //
  auto stub_preservation_file = [](const std::string &name) {
    log_err("`sipi %s preservation-file` awaits ADR-0012; not yet implemented.", name.c_str());
    return EXIT_FAILURE;
  };

  // ----- Option-group helpers (D5 matrix) ------------------------------
  // Each helper attaches a logical group of options to the given
  // subcommand. CLI11 rejects options at parse time on subcommands the
  // group isn't attached to — the option-availability matrix is
  // enforced by which subcommands each helper is invoked on, below.
  // All groups bind to the legacy storage variables so the
  // if-else body chain keeps reading them transparently.
  auto attach_generic_transform_opts = [&](CLI::App *cmd) {
    cmd->add_option("-r,--region", optRegion, "Select region of interest, where x y w h are integer values.")
      ->expected(4);
    cmd->add_option("-R,--reduce", optReduce, "Reduce image size by factor (cannot be used together with --size and --scale).");
    cmd->add_option("-s,--size", optSize, "Resize image to given size (cannot be used together with --reduce and --scale).");
    cmd->add_option("-S,--scale", optScale,
      "Resize image by the given percentage Value (cannot be used together with --size and --reduce).");
    cmd->add_option("-o,--rotate", optRotate, "Rotate the image by degree Value, angle between (0.0 - 360.0).");
    cmd->add_option("-m,--mirror", optMirror, "Mirror the image: 'none', 'horizontal', 'vertical'.")
      ->transform(CLI::CheckedTransformer(optMirrorMap, CLI::ignore_case));
    cmd->add_option("-w,--watermark", optWatermark, "Add a watermark to the image.");
    cmd->add_option("-q,--quality", optJpegQuality, "Quality (compression).")->check(CLI::Range(1, 100));
    cmd->add_option("-F,--format", optFormat, "Output format.")
      ->transform(CLI::CheckedTransformer(optFormatMap, CLI::ignore_case));
  };
  auto attach_color_space_opts = [&](CLI::App *cmd) {
    cmd->add_option("-I,--icc", optIcc, "Convert to ICC profile.")
      ->transform(CLI::CheckedTransformer(optIccMap, CLI::ignore_case));
  };
  auto attach_normalize_opts = [&](CLI::App *cmd) {
    cmd->add_flag("--topleft", optSetTopleft, "Enforce orientation TOPLEFT.");
  };
  auto attach_strip_opts = [&](CLI::App *cmd) {
    cmd->add_flag("-k,--skipmeta", optSkipMeta, "Skip metadata of original file if flag is present.");
  };
  auto attach_output_opts = [&](CLI::App *cmd) {
    cmd->add_flag("--json",
      optJsonOutput,
      "Emit a structured JSON report to stdout (convert: success or error; query, compare: error only).");
  };

  // ----- Format-specific options ---------------------------------------
  // J2K + pyramidal-TIFF tuning knobs from `kdu_compress`. Attached only
  // to `convert` since that is the verb that can produce JP2 or pyramidal
  // TIFF outputs. `convert service-file` deliberately omits these — that
  // command bakes in good baseline defaults, not operator-controlled.
  auto attach_j2k_opts = [&](CLI::App *cmd) {
    cmd->add_option("--Sprofile", j2k_Sprofile,
      "Restricted profile to which the code-stream conforms [Default: PART2].")
      ->check(CLI::IsMember({ "PROFILE0", "PROFILE1", "PROFILE2", "PART2", "CINEMA2K", "CINEMA4K",
                               "BROADCAST", "CINEMA2S", "CINEMA4S", "CINEMASS", "IMF" },
        CLI::ignore_case));
    cmd->add_option("--rates", j2k_rates,
      "One or more bit-rates (see kdu_compress help!). A value \"-1\" may be used in place of the "
      "first bit-rate in the list to indicate that the final quality layer should include all "
      "compressed bits.");
    cmd->add_option("--Clayers", j2k_Clayers, "J2K: Number of quality layers [Default: 8].");
    cmd->add_option("--Clevels", j2k_Clevels,
      "J2K: Number of wavelet decomposition levels, or stages [default: 8].");
    cmd->add_option("--Corder", j2k_Corder,
      "J2K: Progression order: LRCP, RLCP, RPCL (default), PCRL, CPRL.")
      ->check(CLI::IsMember({ "LRCP", "RLCP", "RPCL", "PCRL", "CPRL" }, CLI::ignore_case));
    cmd->add_option("--Stiles", j2k_Stiles, "J2K: Tiles dimensions \"{tx,ty}\" [Default: {256,256}].");
    cmd->add_option("--Cprecincts", j2k_Cprecincts,
      "J2K: Precinct dimensions \"{px,py}\" (powers of 2) [Default: {256,256}].");
    cmd->add_option("--Cblk", j2k_Cblk,
      "J2K: Nominal code-block dimensions (powers of 2, 4..1024, product <= 4096) [Default: {64,64}].");
    cmd->add_option("--Cuse_sop", j2k_Cuse_sop,
      "J2K Cuse_sop: Include SOP markers (resync markers) [Default: yes].");
    cmd->add_option("--Ctiff_pyramid", tiff_Pyramid,
      "TIFF: store in Pyramidal TIFF format [Default: no].");
  };



  // ----- convert (generic, ImageMagick-style) ----------------------------
  CLI::App *cmd_convert =
    sipiopt.add_subcommand("convert", "Generic format conversion (Access File output, no Essentials).");
  cmd_convert->add_option("input", optInFile, "Input file to be converted.")->check(CLI::ExistingFile);
  cmd_convert->add_option("output", optOutFile, "Output file.");
  attach_generic_transform_opts(cmd_convert);
  attach_color_space_opts(cmd_convert);
  attach_normalize_opts(cmd_convert);
  attach_strip_opts(cmd_convert);
  attach_output_opts(cmd_convert);
  attach_j2k_opts(cmd_convert);
  cmd_convert->add_option("-n,--pagenum", optPagenum, "Page number for PDF documents or multipage TIFFs.");
  cmd_convert->callback([&]() {
    // Bare `convert <in> <out>` only fires if no nested subcommand matched.
    if (cmd_convert->get_subcommands().empty()) { sipi_exit_code = run_convert(cmd_convert); }
  });

  // ----- convert access-file (DSP-opinionated; Access File output) ------
  CLI::App *cmd_convert_access = cmd_convert->add_subcommand(
    "access-file", "Produce an Access File from a Service File input (validates input has Essentials).");
  cmd_convert_access->add_option("input", optInFile, "Input Service File.")->check(CLI::ExistingFile);
  cmd_convert_access->add_option("output", optOutFile, "Output Access File.");
  attach_generic_transform_opts(cmd_convert_access);
  attach_color_space_opts(cmd_convert_access);
  attach_normalize_opts(cmd_convert_access);
  attach_output_opts(cmd_convert_access);
  cmd_convert_access->callback([&]() {
    auto user_set = [&](const std::string &name) -> bool {
      auto *s = cmd_convert_access->get_option_no_throw(name);
      return s != nullptr && !s->empty();
    };

    Sipi::cli::ConvertAccessFileArgs req;
    req.input_path = optInFile;
    req.output_path = optOutFile;
    if (user_set("--format")) {
      switch (optFormat) {
      case OptFormat::jpx: req.format = "jpx"; break;
      case OptFormat::jpg: req.format = "jpg"; break;
      case OptFormat::tif: req.format = "tif"; break;
      case OptFormat::png: req.format = "png"; break;
      }
    }
    if (user_set("--region")) { req.region = optRegion; }
    if (user_set("--size")) { req.size = optSize; }
    if (user_set("--scale")) { req.scale = optScale; }
    if (optReduce > 0) { req.reduce = optReduce; }
    if (user_set("--rotate")) { req.rotate = optRotate; }
    if (user_set("--mirror")) {
      switch (optMirror) {
      case OptMirror::horizontal: req.mirror = "horizontal"; break;
      case OptMirror::vertical: req.mirror = "vertical"; break;
      case OptMirror::none: break;
      }
    }
    if (user_set("--watermark")) { req.watermark = optWatermark; }
    if (user_set("--quality")) { req.jpeg_quality = optJpegQuality; }
    if (user_set("--icc")) {
      switch (optIcc) {
      case OptIcc::sRGB: req.icc = "sRGB"; break;
      case OptIcc::AdobeRGB: req.icc = "AdobeRGB"; break;
      case OptIcc::GRAY: req.icc = "GRAY"; break;
      case OptIcc::none: break;
      }
    }
    req.set_topleft = optSetTopleft;
    req.json_output = optJsonOutput;
    sipi_exit_code = Sipi::cli::cmd_convert_access_file(req);
  });

  // ----- convert service-file (Service File creation) -------------------------
  CLI::App *cmd_convert_service = cmd_convert->add_subcommand(
    "service-file", "Create a Service File (writes Essentials packet); restricted option set.");
  cmd_convert_service->add_option("input", optInFile, "Input source file.")->check(CLI::ExistingFile);
  cmd_convert_service->add_option("output", optOutFile, "Output Service File.");
  attach_normalize_opts(cmd_convert_service);
  cmd_convert_service->callback([&]() {
    Sipi::cli::ConvertServiceFileArgs req;
    req.input_path = optInFile;
    req.output_path = optOutFile;
    req.set_topleft = optSetTopleft;
    sipi_exit_code = Sipi::cli::cmd_convert_service_file(req);
  });

  // ----- convert preservation-file (stub) -------------------------------
  CLI::App *cmd_convert_preservation = cmd_convert->add_subcommand(
    "preservation-file", "(stub) Awaits ADR-0012; not yet implemented.");
  cmd_convert_preservation->callback([&]() { sipi_exit_code = stub_preservation_file("convert"); });

  // ----- verify (generic decoder check) --------------------------------
  auto run_verify_with_mode = [&](Sipi::cli::VerifyMode mode) {
    Sipi::cli::VerifyArgs req;
    req.mode = mode;
    req.input_path = optInFile;
    req.json_output = optJsonOutput;
    return Sipi::cli::cmd_verify(req);
  };
  CLI::App *cmd_verify = sipiopt.add_subcommand("verify", "Generic decoder-coverage check (no stage assertions).");
  cmd_verify->add_option("file", optInFile, "File to verify.")->check(CLI::ExistingFile);
  attach_output_opts(cmd_verify);
  cmd_verify->callback([&]() {
    // Bare `verify <file>` only fires if no nested subcommand matched.
    if (cmd_verify->get_subcommands().empty()) {
      sipi_exit_code = run_verify_with_mode(Sipi::cli::VerifyMode::Generic);
    }
  });

  // ----- verify access-file / service-file / preservation-file ---------
  CLI::App *cmd_verify_access = cmd_verify->add_subcommand(
    "access-file", "Assert file is a valid Access File (no Essentials; well-formed XMP).");
  cmd_verify_access->add_option("file", optInFile, "Access File to verify.")->check(CLI::ExistingFile);
  attach_output_opts(cmd_verify_access);
  cmd_verify_access->callback([&]() { sipi_exit_code = run_verify_with_mode(Sipi::cli::VerifyMode::AccessFile); });

  CLI::App *cmd_verify_service = cmd_verify->add_subcommand(
    "service-file", "Assert Essentials parses, hash matches, shape consistent.");
  cmd_verify_service->add_option("file", optInFile, "Service File to verify.")->check(CLI::ExistingFile);
  attach_output_opts(cmd_verify_service);
  cmd_verify_service->callback([&]() { sipi_exit_code = run_verify_with_mode(Sipi::cli::VerifyMode::ServiceFile); });

  CLI::App *cmd_verify_preservation = cmd_verify->add_subcommand(
    "preservation-file", "(stub) Awaits ADR-0012; not yet implemented.");
  cmd_verify_preservation->callback([&]() { sipi_exit_code = stub_preservation_file("verify"); });

  // ----- query -----------------------------------------------------------
  CLI::App *cmd_query = sipiopt.add_subcommand("query", "Print image header and metadata (no pixel decode).");
  cmd_query->add_option("file", optInFile, "File to query.")->check(CLI::ExistingFile);
  attach_output_opts(cmd_query);
  cmd_query->callback([&]() { sipi_exit_code = run_query(); });

  // ----- compare ---------------------------------------------------------
  CLI::App *cmd_compare = sipiopt.add_subcommand("compare", "Byte/pixel comparison of two files.");
  cmd_compare->add_option("files", optCompare, "Two files to compare.")->expected(2);
  attach_output_opts(cmd_compare);
  cmd_compare->callback([&]() { sipi_exit_code = run_compare(); });

  // ----- health ----------------------------------------------------------
  // Self-contained liveness probe for container/orchestrator healthchecks:
  // GET http://127.0.0.1:<port>/health, exit 0 if healthy, 1 otherwise. The
  // caller passes the port the server was configured with (`--port`); a
  // separate process can't discover it from config/env/flags.
  int optHealthPort = 1024;
  CLI::App *cmd_health = sipiopt.add_subcommand(
    "health", "Probe the local /health endpoint; exit 0 if healthy, 1 otherwise.");
  cmd_health->add_option("--port", optHealthPort, "Port the sipi server listens on.")
    ->check(CLI::Range(1, 65535));
  cmd_health->callback([&]() { sipi_exit_code = Sipi::cli::cmd_health({ optHealthPort }); });

  // Catch-all around dispatch: a subcommand body can still throw SipiImageError
  // for an allocation-guard failure (e.g. checked_buf_size_or_throw, memTiffOpen),
  // which is NOT a CLI::Error, so CLI11_PARSE would let it unwind out of this
  // `extern "C"` entry into the Rust caller — UB across the FFI (sipi_ffi.h's
  // no-exception contract). Map any escaped exception to EXIT_FAILURE here,
  // mirroring the engine's sipi_guard.
  // (CLI11_PARSE's own try/catch still handles CLI::ParseError + returns its code.)
  try {
    CLI11_PARSE(sipiopt, argc, argv);
  } catch (const std::exception &e) {
    log_err("sipi: unhandled exception: %s", e.what());
    json_error(optInFile, std::string{ "unhandled exception: " } + e.what(), "unknown", "internal");
    return EXIT_FAILURE;
  } catch (...) {
    log_err("sipi: unhandled non-standard exception");
    json_error(optInFile, "unhandled non-standard exception", "unknown", "internal");
    return EXIT_FAILURE;
  }

  // `require_subcommand(1)` means exactly one leaf subcommand callback fired
  // and recorded its result into sipi_exit_code.
  return sipi_exit_code;
}
