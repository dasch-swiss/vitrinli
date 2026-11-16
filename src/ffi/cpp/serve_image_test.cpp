/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Co-located unit tests (ADR-0003) for the transport-pure IIIF response builder,
// build_image_response. Because it takes only (request, engine context, cancel
// predicate) it runs without a socket: the body shape (FileBody / StreamBody /
// EmptyBody) and the pre-commit status codes are checked directly.

#include "gtest/gtest.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <exiv2/exiv2.hpp>

#include "cache/SipiCache.h"
#include "ffi/engine_context.h"
#include "ffi/serve_image.h"
#include "ffi/serve_response.h"
#include "ffi/sipi_ffi.h"
#include "image/SipiImage.h"
#include "metadata/exif.h"
#include "metadata/xmp.h"
#include "observability/metrics.h"
#include "test_paths.h"

namespace {

using namespace Sipi::ffi;

const std::string kImagesDir = sipi::test::data_dir() + "/images";

// Resolve the fixture through realpath(), matching what the real serve path
// hands the FFI (R2 path validation realpath()s every request). It also sidesteps
// a Bazel-runfiles artifact: the fixture is a symlink, and libmagic would sniff
// the *symlink* (inode/symlink) rather than the TIFF behind it.
std::string fixture(const std::string &rel)
{
  const std::string p = kImagesDir + rel;
  char buf[PATH_MAX];
  return realpath(p.c_str(), buf) != nullptr ? std::string(buf) : p;
}

const auto kNeverCancelled = std::function<bool()>([] { return false; });

// A FULL-region / FULL-size / no-rotation request for the given output format.
SipiIiifParams full_params(SipiFormatType format, SipiQualityType quality = SIPI_QUALITY_DEFAULT)
{
  SipiIiifParams p{};
  p.region_type = SIPI_REGION_FULL;
  p.size_type = SIPI_SIZE_FULL;
  p.rotation = 0.F;
  p.rotation_mirror = 0;
  p.quality_type = quality;
  p.format_type = format;
  return p;
}

EngineContext bare_engine()
{
  EngineContext eng;// all services null (cache/budget disabled)
  eng.imgroot = kImagesDir;
  eng.resolved_imgroot = kImagesDir;
  eng.jpeg_quality = 60;
  return eng;
}

SipiServeRequest make_request(const std::string &path, const SipiIiifParams &params, int is_head = 0)
{
  SipiServeRequest req{};
  req.resolved_path = path.c_str();
  req.prefix = "unit";
  req.identifier = "lena512.tif";
  req.params = params;
  req.restricted_width = 0;
  req.restricted_height = 0;
  req.watermark_path = nullptr;
  req.forwarded_proto = nullptr;
  req.forwarded_host = "localhost";
  req.request_uri = "/unit/lena512.tif";
  req.is_head = is_head;
  return req;
}

bool has_header(const ServeResponse &r, const std::string &name, const std::string &value)
{
  for (const auto &[n, v] : r.headers) {
    if (n == name && v == value) { return true; }
  }
  return false;
}

// Collects a reported SipiImageErrorReport's fields, copying the strings —
// the struct's fields are only valid for the report_error call's duration.
struct ReportedError
{
  bool fired = false;
  std::string phase;
  std::string message;
  std::string input_file;
};

void collect_report(void *ctx, const SipiImageErrorReport *err)
{
  auto *out = static_cast<ReportedError *>(ctx);
  out->fired = true;
  out->phase = err->phase != nullptr ? err->phase : "";
  out->message = err->message != nullptr ? err->message : "";
  out->input_file = err->input_file != nullptr ? err->input_file : "";
}


// Region / size setters for the flat seam params.
void set_coords(SipiIiifParams &p, float x, float y, float w, float h)
{
  p.region_type = SIPI_REGION_COORDS;
  p.region[0] = x;
  p.region[1] = y;
  p.region[2] = w;
  p.region[3] = h;
}

void set_pct_region(SipiIiifParams &p, float x, float y, float w, float h)
{
  set_coords(p, x, y, w, h);
  p.region_type = SIPI_REGION_PERCENTS;
}

void set_size(SipiIiifParams &p, SipiSizeType type, size_t nx, size_t ny, bool upscaling = false)
{
  p.size_type = type;
  p.size_nx = nx;
  p.size_ny = ny;
  p.size_upscaling = upscaling ? 1 : 0;
}

// A unique scratch directory under the Bazel per-test tmp dir.
std::string scratch_dir(const std::string &name)
{
  const std::string dir = sipi::test::tmp_dir() + "/serve_image_test_" + name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

// Runs the StreamBody producer into memory, then the response's on_complete.
std::vector<std::uint8_t> drain(ServeResponse &resp)
{
  std::vector<std::uint8_t> bytes;
  SipiResponse sink_resp{};
  sink_resp.ctx = &bytes;
  sink_resp.write = [](void *ctx, const std::uint8_t *data, size_t len) -> int {
    auto *out = static_cast<std::vector<std::uint8_t> *>(ctx);
    out->insert(out->end(), data, data + len);
    return 0;
  };
  const StreamSink sink(sink_resp);
  const int rc = std::get<StreamBody>(resp.body).producer->produce(sink);
  EXPECT_EQ(rc, 0);
  if (resp.on_complete) { resp.on_complete(); }
  return bytes;
}

// Drains a StreamBody response and decodes the encoded image.
std::unique_ptr<Sipi::SipiImage> decode_body(ServeResponse &resp, const std::string &ext)
{
  const auto bytes = drain(resp);
  const std::string path = scratch_dir("decode") + "/body." + ext;
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  auto img = std::make_unique<Sipi::SipiImage>();
  const auto r = img->read(path);
  EXPECT_TRUE(r.has_value()) << path;
  return img;
}

// Serves `params` for `path` under the restriction (rw, rh) and decodes the
// PNG body; nullptr when the request is refused.
std::unique_ptr<Sipi::SipiImage> serve_png(const std::string &path,
  SipiIiifParams params,
  uint32_t rw,
  uint32_t rh,
  const EngineContext &eng)
{
  params.format_type = SIPI_FORMAT_PNG;
  auto req = make_request(path, params);
  req.restricted_width = rw;
  req.restricted_height = rh;
  auto result = build_image_response(req, eng, kNeverCancelled);
  if (!result.has_value()) { return nullptr; }
  EXPECT_EQ(result->http_status, 200);
  return decode_body(*result, "png");
}

// The pixels of `img` inside the rectangle (x, y, w, h).
std::vector<Sipi::byte> crop_pixels(const Sipi::SipiImage &img, size_t x, size_t y, size_t w, size_t h)
{
  const size_t nc = img.getNc();
  const size_t elem = img.getBps() == 16 ? 2 : 1;
  const auto px = img.pixels_view();
  std::vector<Sipi::byte> out;
  for (size_t j = y; j < y + h; ++j) {
    const auto row = px.begin() + static_cast<std::ptrdiff_t>((j * img.getNx() + x) * nc * elem);
    out.insert(out.end(), row, row + static_cast<std::ptrdiff_t>(w * nc * elem));
  }
  return out;
}

std::vector<Sipi::byte> all_pixels(const Sipi::SipiImage &img)
{
  return { img.pixels_view().begin(), img.pixels_view().end() };
}
}// namespace

TEST(BuildImageResponse, MissingFileIsNotFound)
{
  const std::string path = kImagesDir + "/unit/does_not_exist.tif";
  const auto params = full_params(SIPI_FORMAT_TIF);
  const auto req = make_request(path, params);

  const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SipiStatus::NotFound);
}

TEST(BuildImageResponse, DirectPassthroughIsFileBody)
{
  // Same format as the source (TIFF), full region + size, no transform → the
  // engine serves the file directly (sendFile), not a re-encode.
  const std::string path = fixture("/unit/lena512.tif");
  const auto params = full_params(SIPI_FORMAT_TIF);
  const auto req = make_request(path, params);

  const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->http_status, 200);
  EXPECT_TRUE(std::holds_alternative<FileBody>(result->body));
  EXPECT_EQ(std::get<FileBody>(result->body).path, path);
  EXPECT_TRUE(has_header(*result, "Content-Type", "image/tiff"));
}

TEST(BuildImageResponse, FormatConversionIsStreamBody)
{
  // A different output format (PNG) forces a decode + re-encode: the body is a
  // streamed producer, not a file passthrough.
  const std::string path = fixture("/unit/lena512.tif");
  const auto params = full_params(SIPI_FORMAT_PNG);
  const auto req = make_request(path, params);

  const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->http_status, 200);
  EXPECT_TRUE(std::holds_alternative<StreamBody>(result->body));
  EXPECT_TRUE(has_header(*result, "Content-Type", "image/png"));
}

TEST(BuildImageResponse, HeadRequestIsEmptyBody)
{
  const std::string path = fixture("/unit/lena512.tif");
  const auto params = full_params(SIPI_FORMAT_PNG);
  const auto req = make_request(path, params, /*is_head=*/1);

  const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->http_status, 200);
  EXPECT_TRUE(std::holds_alternative<EmptyBody>(result->body));
  EXPECT_TRUE(has_header(*result, "Content-Type", "image/png"));
}

TEST(BuildImageResponse, ClientGoneBeforeDecode)
{
  const std::string path = fixture("/unit/lena512.tif");
  const auto params = full_params(SIPI_FORMAT_PNG);// decode path (polls cancel)
  const auto req = make_request(path, params);

  const auto always_cancelled = std::function<bool()>([] { return true; });
  const auto result = build_image_response(req, bare_engine(), always_cancelled);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SipiStatus::ClientGone);
}

// The restricted dims (RW, RH) arrive decided by the shell. lena512.tif is
// 512x512; the in-bounds case is the canary that the two refusals below come
// from the restricted pair, not from the request.
TEST(BuildImageResponse, RestrictedDimsWithinImageServe)
{
  const std::string path = fixture("/unit/lena512.tif");
  auto req = make_request(path, full_params(SIPI_FORMAT_PNG));
  req.restricted_width = 128;
  req.restricted_height = 128;

  const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->http_status, 200);
}

TEST(BuildImageResponse, HalfSetRestrictedPairIsInternalError)
{
  const std::string path = fixture("/unit/lena512.tif");
  for (const auto &[rw, rh] : { std::pair<uint32_t, uint32_t>{ 128, 0 }, std::pair<uint32_t, uint32_t>{ 0, 128 } }) {
    auto req = make_request(path, full_params(SIPI_FORMAT_PNG));
    req.restricted_width = rw;
    req.restricted_height = rh;

    const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
    ASSERT_FALSE(result.has_value()) << rw << "x" << rh;
    EXPECT_EQ(result.error(), SipiStatus::InternalError) << rw << "x" << rh;
  }
}

// Checked before the HEAD return, so HEAD and GET agree.
TEST(BuildImageResponse, RestrictedAxisBelowTwoIsInternalError)
{
  const std::string path = fixture("/unit/lena512.tif");
  for (const auto &[rw, rh] : { std::pair<uint32_t, uint32_t>{ 1, 128 }, std::pair<uint32_t, uint32_t>{ 128, 1 } }) {
    for (const int head : { 0, 1 }) {
      auto req = make_request(path, full_params(SIPI_FORMAT_PNG));
      req.restricted_width = rw;
      req.restricted_height = rh;
      req.is_head = head;

      const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
      ASSERT_FALSE(result.has_value()) << rw << "x" << rh << " head=" << head;
      EXPECT_EQ(result.error(), SipiStatus::InternalError) << rw << "x" << rh << " head=" << head;
    }
  }
}

TEST(BuildImageResponse, RestrictedDimsLargerThanImageIsInternalError)
{
  const std::string path = fixture("/unit/lena512.tif");
  for (const auto &[rw, rh] : { std::pair<uint32_t, uint32_t>{ 513, 512 }, std::pair<uint32_t, uint32_t>{ 512, 513 } }) {
    auto req = make_request(path, full_params(SIPI_FORMAT_PNG));
    req.restricted_width = rw;
    req.restricted_height = rh;

    const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
    ASSERT_FALSE(result.has_value()) << rw << "x" << rh;
    EXPECT_EQ(result.error(), SipiStatus::InternalError) << rw << "x" << rh;
  }
}

// A size-restricted image is served as its own image of (RW, RH): region and
// size resolve against (RW, RH), and every output is cut from the one
// rendering at (RW, RH). odd_373x496.jp2 is 373x496; (96, 128) divides
// neither axis.
namespace {
const std::string kOddJp2 = "/unit/odd_373x496.jp2";
constexpr uint32_t kRw = 96;
constexpr uint32_t kRh = 128;

SipiIiifParams png_params() { return full_params(SIPI_FORMAT_PNG); }
}// namespace

TEST(RestrictedImage, FullMaxIsExactlyTheRestrictedDims)
{
  const auto jp2 = serve_png(fixture(kOddJp2), png_params(), kRw, kRh, bare_engine());
  ASSERT_NE(jp2, nullptr);
  EXPECT_EQ(jp2->getNx(), kRw);
  EXPECT_EQ(jp2->getNy(), kRh);

  const auto tif = serve_png(fixture("/unit/lena512.tif"), png_params(), 100, 77, bare_engine());
  ASSERT_NE(tif, nullptr);
  EXPECT_EQ(tif->getNx(), 100U);
  EXPECT_EQ(tif->getNy(), 77U);
}

TEST(RestrictedImage, SourceFormatFullMaxIsStreamedNeverTheFile)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  auto req = make_request(odd_jp2, full_params(SIPI_FORMAT_JP2));
  req.restricted_width = kRw;
  req.restricted_height = kRh;

  auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(std::holds_alternative<StreamBody>(result->body));
  const auto img = decode_body(*result, "jp2");
  EXPECT_EQ(img->getNx(), kRw);
  EXPECT_EQ(img->getNy(), kRh);
}

TEST(RestrictedImage, RegionIsTheMatchingCropOfFullMax)
{
  const auto eng = bare_engine();
  const auto full = serve_png(fixture(kOddJp2), png_params(), kRw, kRh, eng);
  ASSERT_NE(full, nullptr);

  auto coords = png_params();
  set_coords(coords, 10, 20, 40, 50);
  const auto region = serve_png(fixture(kOddJp2), coords, kRw, kRh, eng);
  ASSERT_NE(region, nullptr);
  ASSERT_EQ(region->getNx(), 40U);
  ASSERT_EQ(region->getNy(), 50U);
  EXPECT_EQ(all_pixels(*region), crop_pixels(*full, 10, 20, 40, 50));

  auto quarter = png_params();
  set_pct_region(quarter, 0, 0, 50, 50);
  const auto top_left = serve_png(fixture(kOddJp2), quarter, kRw, kRh, eng);
  ASSERT_NE(top_left, nullptr);
  ASSERT_EQ(top_left->getNx(), kRw / 2);
  ASSERT_EQ(top_left->getNy(), kRh / 2);
  EXPECT_EQ(all_pixels(*top_left), crop_pixels(*full, 0, 0, kRw / 2, kRh / 2));

  auto outside = png_params();
  set_coords(outside, 80, 100, 40, 40);
  const auto clamped = serve_png(fixture(kOddJp2), outside, kRw, kRh, eng);
  ASSERT_NE(clamped, nullptr);
  ASSERT_EQ(clamped->getNx(), 16U);
  ASSERT_EQ(clamped->getNy(), 28U);
  EXPECT_EQ(all_pixels(*clamped), crop_pixels(*full, 80, 100, 16, 28));
}

TEST(RestrictedImage, RegionStartingBeyondTheRestrictedDimsIsBadRequest)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  for (const auto &[x, y] : { std::pair<float, float>{ kRw, 0 }, std::pair<float, float>{ 0, kRh } }) {
    auto params = png_params();
    set_coords(params, x, y, 10, 10);
    auto req = make_request(odd_jp2, params);
    req.restricted_width = kRw;
    req.restricted_height = kRh;
    const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
    ASSERT_FALSE(result.has_value()) << x << "," << y;
    EXPECT_EQ(result.error(), SipiStatus::BadRequest) << x << "," << y;
  }
}

namespace {
struct RestrictedSizeCase
{
  const char *name;
  bool quarter_region;
  SipiSizeType type;
  size_t nx;
  size_t ny;
  float percent;
  bool upscaling;
  size_t want_w;// 0 = refused with 400
  size_t want_h;
};

class RestrictedSize : public ::testing::TestWithParam<RestrictedSizeCase>
{};
}// namespace

TEST_P(RestrictedSize, ResolvesAgainstTheRegionAndNeverExceedsFullMax)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  const auto &c = GetParam();
  auto params = png_params();
  if (c.quarter_region) { set_pct_region(params, 0, 0, 50, 50); }
  set_size(params, c.type, c.nx, c.ny, c.upscaling);
  params.size_percent = c.percent;

  auto req = make_request(odd_jp2, params);
  req.restricted_width = kRw;
  req.restricted_height = kRh;
  auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  if (c.want_w == 0) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), SipiStatus::BadRequest);
    return;
  }
  ASSERT_TRUE(result.has_value());
  const auto img = decode_body(*result, "png");
  EXPECT_EQ(img->getNx(), c.want_w);
  EXPECT_EQ(img->getNy(), c.want_h);
}

INSTANTIATE_TEST_SUITE_P(Sizes,
  RestrictedSize,
  ::testing::Values(RestrictedSizeCase{ "max_quarter", true, SIPI_SIZE_FULL, 0, 0, 0.F, false, 48, 64 },
    RestrictedSizeCase{ "upmax_quarter", true, SIPI_SIZE_FULL, 0, 0, 0.F, true, 48, 64 },
    RestrictedSizeCase{ "upmax_full", false, SIPI_SIZE_FULL, 0, 0, 0.F, true, kRw, kRh },
    RestrictedSizeCase{ "box_40", false, SIPI_SIZE_MAXDIM, 40, 40, 0.F, false, 30, 40 },
    RestrictedSizeCase{ "uppct200_quarter", true, SIPI_SIZE_PERCENTS, 0, 0, 200.F, true, kRw, kRh },
    RestrictedSizeCase{ "uppct200_full", false, SIPI_SIZE_PERCENTS, 0, 0, 200.F, true, 0, 0 },
    RestrictedSizeCase{ "pct200_quarter", true, SIPI_SIZE_PERCENTS, 0, 0, 200.F, false, 0, 0 },
    RestrictedSizeCase{ "upw_wider", false, SIPI_SIZE_PIXELS_X, kRw + 1, 0, 0.F, true, 0, 0 },
    RestrictedSizeCase{ "uph_taller", false, SIPI_SIZE_PIXELS_Y, 0, kRh + 1, 0.F, true, 0, 0 },
    RestrictedSizeCase{ "upwh_wider", false, SIPI_SIZE_PIXELS_XY, kRw + 1, 10, 0.F, true, 0, 0 },
    RestrictedSizeCase{ "upwh_taller", false, SIPI_SIZE_PIXELS_XY, 10, kRh + 1, 0.F, true, 0, 0 },
    RestrictedSizeCase{ "upbox_beyond", false, SIPI_SIZE_MAXDIM, 200, 200, 0.F, true, 0, 0 }),
  [](const ::testing::TestParamInfo<RestrictedSizeCase> &info) { return std::string(info.param.name); });

TEST(RestrictedImage, EdgeTileAtSizeMaxIsAPureCrop)
{
  auto params = png_params();
  set_coords(params, 64, kRh - 1, 32, 1);
  const auto tile = serve_png(fixture(kOddJp2), params, kRw, kRh, bare_engine());
  ASSERT_NE(tile, nullptr);
  ASSERT_EQ(tile->getNx(), 32U);
  ASSERT_EQ(tile->getNy(), 1U);
  const auto full = serve_png(fixture(kOddJp2), png_params(), kRw, kRh, bare_engine());
  ASSERT_NE(full, nullptr);
  EXPECT_EQ(all_pixels(*tile), crop_pixels(*full, 64, kRh - 1, 32, 1));
}

// A 1-sample axis cannot be scaled, so a request that would need it is a 400
// on GET and HEAD alike, never a 500 from the scale operators.
TEST(RestrictedImage, UnscalableOneSampleAxisIsBadRequestOnGetAndHead)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  auto to_one_row = png_params();
  set_coords(to_one_row, 0, 0, 40, 60);
  set_size(to_one_row, SIPI_SIZE_PIXELS_Y, 0, 1);
  auto from_one_column = png_params();
  set_coords(from_one_column, kRw - 1, 0, 40, kRh);// clamps to 1 x RH
  set_size(from_one_column, SIPI_SIZE_PIXELS_XY, 2, 100, true);
  for (const auto &params : { to_one_row, from_one_column }) {
    for (const int is_head : { 0, 1 }) {
      auto req = make_request(odd_jp2, params, is_head);
      req.restricted_width = kRw;
      req.restricted_height = kRh;
      const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
      ASSERT_FALSE(result.has_value()) << "head=" << is_head;
      EXPECT_EQ(result.error(), SipiStatus::BadRequest) << "head=" << is_head;
    }
  }
}

TEST(RestrictedImage, BoundIsCheckedBeforeRotation)
{
  auto full = png_params();
  full.rotation = 90.F;
  const auto rotated = serve_png(fixture(kOddJp2), full, kRw, kRh, bare_engine());
  ASSERT_NE(rotated, nullptr);
  EXPECT_EQ(rotated->getNx(), kRh);
  EXPECT_EQ(rotated->getNy(), kRw);

  auto region = png_params();
  set_coords(region, 0, 0, 40, 60);
  region.rotation = 90.F;
  const auto rotated_region = serve_png(fixture(kOddJp2), region, kRw, kRh, bare_engine());
  ASSERT_NE(rotated_region, nullptr);
  EXPECT_EQ(rotated_region->getNx(), 60U);
  EXPECT_EQ(rotated_region->getNy(), 40U);
}

TEST(RestrictedImage, SizeAndWatermarkServeTheRestrictedDimsWatermarked)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  const std::string watermark = fixture("/unit/watermark_correct.tif");
  const auto plain = serve_png(fixture(kOddJp2), png_params(), kRw, kRh, bare_engine());
  ASSERT_NE(plain, nullptr);

  auto params = png_params();
  auto req = make_request(odd_jp2, params);
  req.restricted_width = kRw;
  req.restricted_height = kRh;
  req.watermark_path = watermark.c_str();
  auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_TRUE(result.has_value());
  const auto marked = decode_body(*result, "png");
  ASSERT_EQ(marked->getNx(), kRw);
  ASSERT_EQ(marked->getNy(), kRh);
  EXPECT_NE(all_pixels(*marked), all_pixels(*plain));
}

TEST(RestrictedImage, WatermarkOnlyKeepsNativeDimsAndCoordinates)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  const std::string watermark = fixture("/unit/watermark_correct.tif");
  auto params = png_params();
  set_coords(params, 0, 0, 200, 300);
  auto req = make_request(odd_jp2, params);
  req.watermark_path = watermark.c_str();
  auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_TRUE(result.has_value());
  const auto img = decode_body(*result, "png");
  EXPECT_EQ(img->getNx(), 200U);
  EXPECT_EQ(img->getNy(), 300U);
}

TEST(RestrictedImage, SizeAndWatermarkNeverReachThePassthrough)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  const std::string watermark = fixture("/unit/watermark_correct.tif");
  // (373, 496) is the native size: an image already inside the box.
  for (const auto &[rw, rh] : { std::pair<uint32_t, uint32_t>{ kRw, kRh }, std::pair<uint32_t, uint32_t>{ 373, 496 } }) {
    for (const char *wm : { static_cast<const char *>(nullptr), watermark.c_str() }) {
      auto req = make_request(odd_jp2, full_params(SIPI_FORMAT_JP2));
      req.restricted_width = rw;
      req.restricted_height = rh;
      req.watermark_path = wm;
      const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
      ASSERT_TRUE(result.has_value()) << rw << "x" << rh;
      EXPECT_TRUE(std::holds_alternative<StreamBody>(result->body)) << rw << "x" << rh << " wm=" << (wm != nullptr);
    }
  }
}

// A size-restricted output carries no embedded preview and states the
// delivered size in its size fields; every other field, rights included, is
// returned. No test image carries a maker-note preview, an Iop size or an XMP
// thumbnail, so the source is crafted.
TEST(RestrictedImage, MetadataDropsPreviewsAndStatesTheDeliveredSize)
{
  const std::string pixels = "SIPI-PREVIEW-PIXELS";
  const std::string xmp_pixels = "U0lQSS1QUkVWSUVX";
  std::vector<Exiv2::byte> jpeg = { 0xff, 0xd8 };
  jpeg.insert(jpeg.end(), pixels.begin(), pixels.end());
  jpeg.insert(jpeg.end(), { 0xff, 0xd9 });

  Exiv2::ExifData data;
  data["Exif.Image.Make"] = "OLYMPUS IMAGING CORP.";
  data["Exif.Image.Copyright"] = "DaSCH";
  data["Exif.Photo.PixelXDimension"] = static_cast<std::uint32_t>(512);
  data["Exif.Iop.RelatedImageWidth"] = static_cast<std::uint32_t>(512);
  Exiv2::DataValue preview(Exiv2::undefined);
  preview.read(jpeg.data(), jpeg.size());
  data.add(Exiv2::ExifKey("Exif.Olympus2.ThumbnailImage"), &preview);
  Exiv2::ExifThumb(data).setJpegThumbnail(jpeg.data(), jpeg.size());
  Exiv2::Blob blob;
  Exiv2::ExifParser::encode(blob, Exiv2::littleEndian, data);
  auto exif = Sipi::Exif::parse(blob.data(), static_cast<unsigned int>(blob.size()));
  ASSERT_TRUE(exif.has_value());

  const std::string rights = R"(<dc:rights><rdf:Alt><rdf:li xml:lang="x-default">DaSCH</rdf:li></rdf:Alt></dc:rights>)";
  const std::string xmp =
    R"(<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
    R"(<rdf:Description rdf:about="" xmlns:xmp="http://ns.adobe.com/xap/1.0/" xmlns:dc="http://purl.org/dc/elements/1.1/")"
    R"( xmlns:xmpGImg="http://ns.adobe.com/xap/1.0/g/img/" xmlns:tiff="http://ns.adobe.com/tiff/1.0/" tiff:ImageWidth="512">)"
    R"(<xmp:Thumbnails><rdf:Alt><rdf:li rdf:parseType="Resource"><xmpGImg:width>512</xmpGImg:width><xmpGImg:image>)"
    + xmp_pixels + R"(</xmpGImg:image></rdf:li></rdf:Alt></xmp:Thumbnails>)" + rights
    + R"(</rdf:Description></rdf:RDF></x:xmpmeta>)";

  Sipi::SipiImage src;
  ASSERT_TRUE(src.read(fixture("/unit/lena512.tif")).has_value());
  src.set_exif(*exif);
  src.set_xmp(std::make_shared<Sipi::Xmp>(xmp));
  const std::string path = scratch_dir("crafted") + "/crafted.jpg";
  ASSERT_TRUE(src.write("jpg", path).has_value());

  const auto serve = [&](uint32_t rw, uint32_t rh) {
    auto params = full_params(SIPI_FORMAT_JPG);
    set_size(params, SIPI_SIZE_PIXELS_XY, 100, 80);
    auto req = make_request(path, params);
    req.restricted_width = rw;
    req.restricted_height = rh;
    auto result = build_image_response(req, bare_engine(), kNeverCancelled);
    EXPECT_TRUE(result.has_value());
    return result.has_value() ? drain(*result) : std::vector<std::uint8_t>{};
  };
  const auto has = [](const std::vector<std::uint8_t> &bytes, const std::string &needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
  };

  // The same size served unrestricted keeps the previews.
  const auto unrestricted = serve(0, 0);
  ASSERT_TRUE(has(unrestricted, pixels));
  ASSERT_TRUE(has(unrestricted, xmp_pixels));

  const auto body = serve(100, 80);
  EXPECT_FALSE(has(body, pixels));
  EXPECT_FALSE(has(body, xmp_pixels));
  const std::string out_path = scratch_dir("crafted_out") + "/out.jpg";
  {
    std::ofstream out(out_path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(body.data()), static_cast<std::streamsize>(body.size()));
  }
  Sipi::SipiImage out;
  ASSERT_TRUE(out.read(out_path).has_value());
  ASSERT_EQ(out.getNx(), 100U);
  ASSERT_EQ(out.getNy(), 80U);

  ASSERT_NE(out.getExif(), nullptr);
  std::uint32_t v = 0;
  EXPECT_TRUE(out.getExif()->getValByKey("Exif.Photo.PixelXDimension", v));
  EXPECT_EQ(v, 100U);
  EXPECT_TRUE(out.getExif()->getValByKey("Exif.Iop.RelatedImageWidth", v));
  EXPECT_EQ(v, 100U);
  EXPECT_FALSE(out.getExif()->getValByKey("Exif.Thumbnail.JPEGInterchangeFormat", v));
  std::string text;
  EXPECT_TRUE(out.getExif()->getValByKey("Exif.Image.Copyright", text));
  EXPECT_EQ(text, "DaSCH");
  EXPECT_TRUE(out.getExif()->getValByKey("Exif.Image.Make", text));
  EXPECT_EQ(text, "OLYMPUS IMAGING CORP.");

  ASSERT_NE(out.getXmp(), nullptr);
  const std::string out_xmp = out.getXmp()->xmpBytes();
  EXPECT_EQ(out_xmp.find("Thumbnails"), std::string::npos);
  EXPECT_NE(out_xmp.find(R"(tiff:ImageWidth="100")"), std::string::npos);
  EXPECT_NE(out_xmp.find(rights), std::string::npos);
}

// The Photoshop image resources an EXIF block can carry get the same
// treatment; resources it cannot rewrite fail the restricted request only.
TEST(RestrictedImage, ExifImageResourcesDropPreviewsAndStateTheDeliveredSize)
{
  const auto resource = [](std::uint16_t id, const std::string &data) {
    const std::size_t n = data.size();
    std::vector<Exiv2::byte> r = { '8',
      'B',
      'I',
      'M',
      static_cast<Exiv2::byte>(id >> 8),
      static_cast<Exiv2::byte>(id),
      0,
      0,
      static_cast<Exiv2::byte>(n >> 24),
      static_cast<Exiv2::byte>(n >> 16),
      static_cast<Exiv2::byte>(n >> 8),
      static_cast<Exiv2::byte>(n) };
    r.insert(r.end(), data.begin(), data.end());
    if (n % 2 == 1) { r.push_back(0); }
    return r;
  };
  const auto crafted_jpeg = [](const std::vector<Exiv2::byte> &irb, const std::string &name) {
    Exiv2::ExifData data;
    data["Exif.Image.Copyright"] = "DaSCH";
    Exiv2::DataValue resources(Exiv2::unsignedByte);
    resources.read(irb.data(), irb.size());
    data.add(Exiv2::ExifKey("Exif.Image.ImageResources"), &resources);
    Exiv2::Blob blob;
    Exiv2::ExifParser::encode(blob, Exiv2::littleEndian, data);
    auto exif = Sipi::Exif::parse(blob.data(), static_cast<unsigned int>(blob.size()));
    EXPECT_TRUE(exif.has_value());
    Sipi::SipiImage src;
    EXPECT_TRUE(src.read(fixture("/unit/lena512.tif")).has_value());
    src.set_exif(*exif);
    const std::string path = scratch_dir(name) + "/crafted.jpg";
    EXPECT_TRUE(src.write("jpg", path).has_value());
    return path;
  };
  const auto serve = [](const std::string &path, uint32_t rw, uint32_t rh) {
    auto params = full_params(SIPI_FORMAT_JPG);
    set_size(params, SIPI_SIZE_PIXELS_XY, 100, 80);
    auto req = make_request(path, params);
    req.restricted_width = rw;
    req.restricted_height = rh;
    return build_image_response(req, bare_engine(), kNeverCancelled);
  };
  const auto has = [](const std::vector<std::uint8_t> &bytes, const std::string &needle) {
    return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
  };

  const std::string pixels = "SIPI-PREVIEW-PIXELS";
  const std::string rights = R"(<dc:rights><rdf:Alt><rdf:li xml:lang="x-default">DaSCH</rdf:li></rdf:Alt></dc:rights>)";
  const std::string xmp = R"(<rdf:Description rdf:about="" tiff:ImageWidth="512">)" + rights + "</rdf:Description>";
  std::vector<Exiv2::byte> irb = resource(0x040c, pixels);
  const auto nested_xmp = resource(0x0424, xmp);
  irb.insert(irb.end(), nested_xmp.begin(), nested_xmp.end());
  const std::string path = crafted_jpeg(irb, "crafted_irb");

  auto unrestricted = serve(path, 0, 0);
  ASSERT_TRUE(unrestricted.has_value());
  ASSERT_TRUE(has(drain(*unrestricted), pixels));
  auto restricted = serve(path, 100, 80);
  ASSERT_TRUE(restricted.has_value());
  const auto body = drain(*restricted);
  EXPECT_FALSE(has(body, pixels));
  EXPECT_TRUE(has(body, R"(tiff:ImageWidth="100")"));
  EXPECT_TRUE(has(body, rights));

  const std::string slices = crafted_jpeg(resource(0x041a, "SLICES"), "crafted_slices");
  EXPECT_TRUE(serve(slices, 0, 0).has_value());
  const auto refused = serve(slices, 100, 80);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error(), SipiStatus::InternalError);
}

// A restricted and an unrestricted request with the same canonical
// coordinates, and two restrictions of the same file, must never share a
// cache entry: a cache hit would hand one view's pixels to the other.
TEST(RestrictedImage, CacheKeySeparatesRestrictions)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  const std::string dir = scratch_dir("cache");
  Sipi::SipiCache cache(dir + "/cache");
  auto eng = bare_engine();
  eng.cache = &cache;

  auto params = png_params();
  set_coords(params, 0, 0, 40, 40);
  set_size(params, SIPI_SIZE_PIXELS_XY, 10, 10);

  const auto serve = [&](uint32_t rw, uint32_t rh) {
    auto req = make_request(odd_jp2, params);
    req.restricted_width = rw;
    req.restricted_height = rh;
    auto result = build_image_response(req, eng, kNeverCancelled);
    ASSERT_TRUE(result.has_value()) << rw << "x" << rh;
    ASSERT_TRUE(std::holds_alternative<StreamBody>(result->body)) << rw << "x" << rh << " was a cache hit";
    drain(*result);
  };

  serve(0, 0);
  serve(kRw, kRh);
  EXPECT_EQ(cache.getNfiles(), 2U);
  serve(kRh, kRw);
  EXPECT_EQ(cache.getNfiles(), 3U);
}

// The before-decode hook runs only when a decode starts, and a refusal is a
// 503 that decodes and caches nothing.
struct BeforeDecodeCalls
{
  int calls = 0;
  int answer = 0;
};

int count_before_decode(void *ctx)
{
  auto *c = static_cast<BeforeDecodeCalls *>(ctx);
  ++c->calls;
  return c->answer;
}

TEST(BuildImageResponse, BeforeDecodeRunsOnlyForADecode)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  const std::string dir = scratch_dir("cache_before_decode");
  Sipi::SipiCache cache(dir + "/cache");
  auto eng = bare_engine();
  eng.cache = &cache;

  auto params = png_params();
  set_coords(params, 0, 0, 40, 40);
  BeforeDecodeCalls hook;
  const auto request = [&](int is_head) {
    auto req = make_request(odd_jp2, params, is_head);
    req.restricted_width = kRw;
    req.restricted_height = kRh;
    req.before_decode = count_before_decode;
    req.before_decode_ctx = &hook;
    return req;
  };

  hook.answer = 1;
  auto refused = build_image_response(request(0), eng, kNeverCancelled);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->http_status, 503);
  EXPECT_TRUE(has_header(*refused, "Retry-After", "1"));
  EXPECT_TRUE(std::holds_alternative<EmptyBody>(refused->body));
  EXPECT_EQ(hook.calls, 1);
  EXPECT_EQ(cache.getNfiles(), 0U);

  auto head = build_image_response(request(1), eng, kNeverCancelled);
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(head->http_status, 200);
  EXPECT_EQ(hook.calls, 1) << "HEAD called the hook";

  hook.answer = 0;
  auto miss = build_image_response(request(0), eng, kNeverCancelled);
  ASSERT_TRUE(miss.has_value());
  ASSERT_TRUE(std::holds_alternative<StreamBody>(miss->body));
  drain(*miss);
  EXPECT_EQ(hook.calls, 2);
  ASSERT_EQ(cache.getNfiles(), 1U);

  auto hit = build_image_response(request(0), eng, kNeverCancelled);
  ASSERT_TRUE(hit.has_value());
  EXPECT_TRUE(std::holds_alternative<FileBody>(hit->body)) << "the second request missed the cache";
  EXPECT_EQ(hook.calls, 2) << "a cache hit called the hook";
  if (hit->on_complete) { hit->on_complete(); }
}

// The Cache key records only whether a watermark was applied, so watermarked
// output must never use the cache: a hit would hand one caller's watermark to
// another. An empty watermark is no watermark and still uses the cache.
TEST(BuildImageResponse, WatermarkedOutputNeverUsesTheCache)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  const std::string first_watermark = fixture("/unit/watermark_correct.tif");
  const std::string second_watermark = fixture("/unit/gradient-stars.tif");
  const std::string dir = scratch_dir("cache_watermark");
  Sipi::SipiCache cache(dir + "/cache");
  auto eng = bare_engine();
  eng.cache = &cache;

  auto params = png_params();
  set_coords(params, 0, 0, 40, 40);

  const auto serve = [&](const char *watermark) {
    auto req = make_request(odd_jp2, params);
    req.watermark_path = watermark;
    return build_image_response(req, eng, kNeverCancelled);
  };

  auto first = serve(first_watermark.c_str());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(std::holds_alternative<StreamBody>(first->body));
  std::string link;
  for (const auto &[name, value] : first->headers) {
    if (name == "Link") { link = value; }
  }
  const auto first_bytes = drain(*first);
  EXPECT_EQ(cache.getNfiles(), 0U);

  // A `/1` entry left on disk, under the key the watermarked request computes
  // (its canonical Link without the scheme), must never be served.
  const std::string scheme = "<http://";
  ASSERT_EQ(link.rfind(scheme, 0), 0U) << link;
  const std::string wm_key = link.substr(scheme.size(), link.find('>') - scheme.size());
  ASSERT_TRUE(wm_key.ends_with("/1")) << wm_key;
  const std::string stale = cache.getNewCacheFileName();
  {
    std::ofstream out(stale, std::ios::binary);
    out.write(reinterpret_cast<const char *>(first_bytes.data()), static_cast<std::streamsize>(first_bytes.size()));
  }
  cache.add(odd_jp2, wm_key, stale, 373, 496);
  ASSERT_EQ(cache.getNfiles(), 1U);

  auto second = serve(second_watermark.c_str());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(std::holds_alternative<StreamBody>(second->body)) << "the second watermark was a cache hit";
  EXPECT_NE(drain(*second), first_bytes);
  EXPECT_EQ(cache.getNfiles(), 1U);

  auto plain = serve(nullptr);
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(std::holds_alternative<StreamBody>(plain->body));
  drain(*plain);
  EXPECT_EQ(cache.getNfiles(), 2U);

  auto empty = serve("");
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(std::holds_alternative<FileBody>(empty->body)) << "an empty watermark missed the cache";
  if (empty->on_complete) { empty->on_complete(); }
  EXPECT_EQ(cache.getNfiles(), 2U);
}

// The size in the Cache key resolves against the region extent: `pct:50`,
// `50,` and `50,67` on an 80x100 region are 40x50, 50x63 and 50x67, three
// images, three entries, restricted or not.
TEST(BuildImageResponse, CacheKeyResolvesTheSizeAgainstTheRegion)
{
  const std::string odd_jp2 = fixture(kOddJp2);
  for (const auto &[rw, rh] : { std::pair<uint32_t, uint32_t>{ 0, 0 }, std::pair<uint32_t, uint32_t>{ kRw, kRh } }) {
    const std::string dir = scratch_dir("cache_region_size_" + std::to_string(rw));
    Sipi::SipiCache cache(dir + "/cache");
    auto eng = bare_engine();
    eng.cache = &cache;

    auto pct = png_params();
    set_coords(pct, 0, 0, 80, 100);
    pct.size_type = SIPI_SIZE_PERCENTS;
    pct.size_percent = 50.F;
    auto width = png_params();
    set_coords(width, 0, 0, 80, 100);
    set_size(width, SIPI_SIZE_PIXELS_X, 50, 0);
    auto both = png_params();
    set_coords(both, 0, 0, 80, 100);
    set_size(both, SIPI_SIZE_PIXELS_XY, 50, 67);

    for (const auto &[params, w, h] : { std::tuple{ pct, size_t{ 40 }, size_t{ 50 } },
           std::tuple{ width, size_t{ 50 }, size_t{ 63 } },
           std::tuple{ both, size_t{ 50 }, size_t{ 67 } } }) {
      auto req = make_request(odd_jp2, params);
      req.restricted_width = rw;
      req.restricted_height = rh;
      auto result = build_image_response(req, eng, kNeverCancelled);
      ASSERT_TRUE(result.has_value());
      ASSERT_TRUE(std::holds_alternative<StreamBody>(result->body)) << w << "x" << h << " was a cache hit, rw=" << rw;
      const auto img = decode_body(*result, "png");
      EXPECT_EQ(img->getNx(), w);
      EXPECT_EQ(img->getNy(), h);
    }
    EXPECT_EQ(cache.getNfiles(), 3U) << "rw=" << rw;
  }
}

TEST(BuildImageResponse, ReportErrorFiresOnReadFailure)
{
  // test.csv exists but is not a recognised image format — read_shape()
  // throws before any decode is attempted, exercising the seam's earliest
  // report_error call site.
  const std::string path = fixture("/unit/test.csv");
  const auto params = full_params(SIPI_FORMAT_TIF);
  auto req = make_request(path, params);
  ReportedError out;
  req.report_error = collect_report;
  req.report_ctx = &out;

  const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SipiStatus::InternalError);
  EXPECT_TRUE(out.fired);
  EXPECT_EQ(out.phase, "read");
  EXPECT_FALSE(out.message.empty());
  EXPECT_EQ(out.input_file, path);
}

TEST(BuildImageResponse, ReportErrorNullCallbackIsSafeNoOp)
{
  // Same failing request as above, but report_error is left null (make_request's
  // SipiServeRequest{} default) — the seam's "NULL = absent" idiom must not
  // crash or otherwise change the returned status.
  const std::string path = fixture("/unit/test.csv");
  const auto params = full_params(SIPI_FORMAT_TIF);
  const auto req = make_request(path, params);

  const auto result = build_image_response(req, bare_engine(), kNeverCancelled);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SipiStatus::InternalError);
}

TEST(StatusFor, ReportPolicyCodeIsInternalError)
{
  const Sipi::SipiValueError err(Sipi::ErrorCode::kDecodeFailed, "boom");
  EXPECT_EQ(status_for(err), SipiStatus::InternalError);
}

TEST(StatusFor, SkipPolicyCodeIsAlsoInternalError)
{
  // Every ErrorCode maps to kInternalError today (error-model.md); the
  // sentry_policy (kReport vs kSkip) is orthogonal to the HTTP status.
  const Sipi::SipiValueError err(Sipi::ErrorCode::kClientAbort, "client gone");
  EXPECT_EQ(status_for(err), SipiStatus::InternalError);
}

TEST(ReportValueError, CallsCallbackForReportPolicyCode)
{
  const Sipi::SipiValueError err(Sipi::ErrorCode::kDecodeFailed, "decode boom");
  Sipi::observability::ImageContext ctx;
  ctx.input_file = "/unit/lena512.tif";
  ctx.output_format = "png";
  ReportedError out;

  report_value_error(collect_report, &out, err, "convert", ctx);

  ASSERT_TRUE(out.fired);
  EXPECT_EQ(out.phase, "convert");
  EXPECT_EQ(out.message, err.diagnostic_message());
  EXPECT_EQ(out.input_file, ctx.input_file);
}

TEST(ReportValueError, SkipsCallbackForClientAbort)
{
  // The load-bearing case: kSkip must not call the callback at all,
  // not merely pass it an empty report.
  const Sipi::SipiValueError err(Sipi::ErrorCode::kClientAbort, "client gone");
  Sipi::observability::ImageContext ctx;
  ReportedError out;

  report_value_error(collect_report, &out, err, "write", ctx);

  EXPECT_FALSE(out.fired);
}

TEST(ReportValueError, NullCallbackIsSafeNoOp)
{
  const Sipi::SipiValueError err(Sipi::ErrorCode::kDecodeFailed, "decode boom");
  Sipi::observability::ImageContext ctx;

  report_value_error(nullptr, nullptr, err, "read", ctx);
}

// A wedged Kakadu decode (DEV-7080) must be bounded by the seam's wall-clock
// deadline instead of hanging the calling thread forever. Both committed
// reproducers wedge inside read_shape, so a short deadline must trip well
// under the 30s bound this test enforces (comfortably below the production
// default of 120s).
class DecodeHangWatchdog : public ::testing::TestWithParam<const char *>
{};

TEST_P(DecodeHangWatchdog, HitsDeadlineAndCountsWedgedThread)
{
  const std::string rel = GetParam();
  const std::string basename = rel.substr(rel.find_last_of('/') + 1);
  const std::string path = fixture(rel);
  const auto params = full_params(SIPI_FORMAT_TIF);
  auto req = make_request(path, params);
  req.identifier = basename.c_str();

  auto eng = bare_engine();
  eng.decode_timeout_ms = 2000;

  const auto before = Sipi::observability::Metrics::instance().wedged_threads.Value();
  const auto start = std::chrono::steady_clock::now();
  const auto result = build_image_response(req, eng, kNeverCancelled);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), SipiStatus::InternalError);
  EXPECT_LT(elapsed, std::chrono::seconds(30));
  EXPECT_EQ(Sipi::observability::Metrics::instance().wedged_threads.Value(), before + 1);
}

INSTANTIATE_TEST_SUITE_P(Jp2HangFixtures,
  DecodeHangWatchdog,
  ::testing::Values("/hang/dev7080_read_shape_hang.jp2", "/hang/nightly_j2k_read_shape_hang.jp2"));

TEST(BuildImageResponse, GoodJp2DecodesWithinDeadline)
{
  const std::string path = fixture("/unit/lena512.jp2");
  const auto params = full_params(SIPI_FORMAT_TIF);
  auto req = make_request(path, params);
  req.identifier = "lena512.jp2";

  auto eng = bare_engine();
  eng.decode_timeout_ms = Sipi::ffi::kDefaultDecodeTimeoutMs;

  const auto before = Sipi::observability::Metrics::instance().wedged_threads.Value();
  const auto result = build_image_response(req, eng, kNeverCancelled);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->http_status, 200);
  EXPECT_EQ(Sipi::observability::Metrics::instance().wedged_threads.Value(), before);
}
