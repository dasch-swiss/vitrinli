/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

// Co-located unit tests (ADR-0003) for the edge-probe seam entries the Rust
// shell calls at the request boundary: sipi_imgroot / sipi_prefix_as_path (read
// the installed engine context) and sipi_image_dims / sipi_mimetype (read a
// validated file — sipi_image_dims also optionally emits the Essentials
// identity from the same read). All are sipi_guard-only — no response sink —
// so they run without a transport.

#include "gtest/gtest.h"

#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "ffi/SipiConf.h"
#include "ffi/engine_context.h"
#include "ffi/sipi_ffi.h"
#include "test_paths.h"
#include "throttling/SipiMemoryBudget.h"

namespace {

const std::string kImagesDir = sipi::test::data_dir() + "/images";

// realpath() the fixture, matching what the real serve path hands the FFI and
// sidestepping the Bazel-runfiles symlink (libmagic would otherwise sniff the
// symlink, not the image behind it) — same helper as serve_image_test.
std::string fixture(const std::string &rel)
{
  const std::string p = kImagesDir + rel;
  char buf[PATH_MAX];
  return realpath(p.c_str(), buf) != nullptr ? std::string(buf) : p;
}

// Install a known engine context so the config getters have something to read.
void install_engine(bool prefix_as_path)
{
  Sipi::ffi::EngineContext eng;// services null (disabled) — the getters don't read them
  eng.imgroot = "/srv/images";
  eng.resolved_imgroot = "/srv/images/resolved";
  eng.prefix_as_path = prefix_as_path;
  Sipi::ffi::set_engine_context(eng);
}

// SipiStrFn that copies the emitted value into a std::string (the ctx).
void collect_str(void *ctx, const char *value) { *static_cast<std::string *>(ctx) = value; }

// The two-string result sipi_image_dims' optional essentials callback may
// fire into.
struct Essentials
{
  bool fired = false;
  std::string mimetype;
  std::string filename;
};

// SipiEssentialsFn that records whether it fired, plus both strings (the ctx).
void collect_essentials(void *ctx, const char *orig_mimetype, const char *orig_filename)
{
  auto *out = static_cast<Essentials *>(ctx);
  out->fired = true;
  out->mimetype = orig_mimetype;
  out->filename = orig_filename;
}

}// namespace

TEST(SeamProbe, ImgrootReturnsRawThenResolved)
{
  install_engine(true);
  const char *raw = nullptr;
  const char *res = nullptr;
  ASSERT_EQ(sipi_imgroot(0, &raw), 0);
  ASSERT_EQ(sipi_imgroot(1, &res), 0);
  EXPECT_STREQ(raw, "/srv/images");
  EXPECT_STREQ(res, "/srv/images/resolved");
}

TEST(SeamProbe, ImgrootPointerIsProcessStatic)
{
  install_engine(true);
  const char *first = nullptr;
  const char *second = nullptr;
  ASSERT_EQ(sipi_imgroot(0, &first), 0);
  ASSERT_EQ(sipi_imgroot(0, &second), 0);
  // The pointer is into the process-static EngineContext copy — stable, never
  // freed by the caller (the seam contract the Rust shell relies on).
  EXPECT_EQ(first, second);
}

TEST(SeamProbe, PrefixAsPathReflectsConfig)
{
  install_engine(false);
  int v = -1;
  ASSERT_EQ(sipi_prefix_as_path(&v), 0);
  EXPECT_EQ(v, 0);

  install_engine(true);
  ASSERT_EQ(sipi_prefix_as_path(&v), 0);
  EXPECT_EQ(v, 1);
}

TEST(SeamProbe, AdmissionConfigReflectsEngineContext)
{
  // The shell reads the resolved admission config back over these getters so its
  // two-lane pool runs the same mode/threshold/ratio/envelope the engine budget
  // does (single authority = the engine's resolved EngineContext).
  Sipi::ffi::EngineContext eng;// services null — the getters don't read them
  eng.admission_mode = "advanced";
  eng.tiles_memory_ratio = 0.3;
  eng.large_decode_threshold_bytes = 64UL * 1024 * 1024;
  eng.memory_limit_bytes = 8UL * 1024 * 1024 * 1024;
  Sipi::ffi::set_engine_context(eng);

  const char *mode = nullptr;
  ASSERT_EQ(sipi_admission_mode(&mode), 0);
  EXPECT_STREQ(mode, "advanced");

  double ratio = -1.0;
  ASSERT_EQ(sipi_tiles_memory_ratio(&ratio), 0);
  EXPECT_DOUBLE_EQ(ratio, 0.3);

  size_t threshold = 0;
  ASSERT_EQ(sipi_large_decode_threshold_bytes(&threshold), 0);
  EXPECT_EQ(threshold, 64UL * 1024 * 1024);

  size_t limit = 0;
  ASSERT_EQ(sipi_memory_limit_bytes(&limit), 0);
  EXPECT_EQ(limit, 8UL * 1024 * 1024 * 1024);
}

TEST(SeamProbe, ImageDimsReadsNativeShape)
{
  const std::string path = fixture("/unit/lena512.tif");
  SipiImageDims dims{};
  ASSERT_EQ(sipi_image_dims(path.c_str(), &dims, nullptr, nullptr), 0);
  EXPECT_EQ(dims.width, 512u);
  EXPECT_EQ(dims.height, 512u);
}

TEST(SeamProbe, ImageDimsOnNonImageIsError)
{
  // test.csv exists but is not a recognised image format → read_shape throws →
  // a non-zero (500) status, never a crash or a bogus 0×0 success.
  const std::string path = fixture("/unit/test.csv");
  SipiImageDims dims{};
  EXPECT_NE(sipi_image_dims(path.c_str(), &dims, nullptr, nullptr), 0);
}

TEST(SeamProbe, ImageDimsOnMissingFileIsErrorAndLeavesOutUntouched)
{
  // A failed probe must never read as a shape: the shell fails closed on the
  // status, and `out` keeps what the caller put there.
  const std::string path = kImagesDir + "/unit/does_not_exist.tif";
  SipiImageDims dims{};
  dims.width = 7;
  dims.height = 7;
  EXPECT_NE(sipi_image_dims(path.c_str(), &dims, nullptr, nullptr), 0);
  EXPECT_EQ(dims.width, 7u);
  EXPECT_EQ(dims.height, 7u);
}

TEST(SeamProbe, MimetypeMatchesEngineLibmagic)
{
  const std::string tif = fixture("/unit/lena512.tif");
  std::string mime;
  ASSERT_EQ(sipi_mimetype(tif.c_str(), collect_str, &mime), 0);
  EXPECT_NE(mime.find("image/"), std::string::npos) << "got: " << mime;

  const std::string csv = fixture("/unit/test.csv");
  std::string csv_mime;
  ASSERT_EQ(sipi_mimetype(csv.c_str(), collect_str, &csv_mime), 0);
  EXPECT_FALSE(csv_mime.empty());
}

TEST(SeamProbe, ImageDimsEssentialsFiresOnFileWithPacket)
{
  // lena512.jp2 carries a legacy pipe-delimited Essentials packet:
  // SIPI:lena512.tif|image/tiff|sha256|...
  const std::string path = fixture("/unit/lena512.jp2");
  SipiImageDims dims{};
  Essentials out;
  ASSERT_EQ(sipi_image_dims(path.c_str(), &dims, collect_essentials, &out), 0);
  EXPECT_TRUE(out.fired);
  EXPECT_EQ(out.mimetype, "image/tiff");
  EXPECT_EQ(out.filename, "lena512.tif");
}

TEST(SeamProbe, ImageDimsEssentialsDoesNotFireWithoutPacket)
{
  // lena512.tif is a plain source fixture — no embedded Essentials packet, so
  // the callback must not fire (not an error: 0 either way).
  const std::string path = fixture("/unit/lena512.tif");
  SipiImageDims dims{};
  Essentials out;
  ASSERT_EQ(sipi_image_dims(path.c_str(), &dims, collect_essentials, &out), 0);
  EXPECT_FALSE(out.fired);
}

TEST(SeamProbe, ImageDimsEssentialsOnNonImageIsError)
{
  const std::string path = fixture("/unit/test.csv");
  SipiImageDims dims{};
  Essentials out;
  EXPECT_NE(sipi_image_dims(path.c_str(), &dims, collect_essentials, &out), 0);
  EXPECT_FALSE(out.fired);
}

// sipi_image_new's full-lane memory-budget charge (S2-20): mirrors the IIIF
// serve path's estimate_peak_memory / try_acquire gate so a Lua-driven
// SipiImage.new() decode cannot bypass the memory envelope the serve path
// enforces.

TEST(SeamProbe, ImageNewRefusedWhenDecodeExceedsAdvancedBudget)
{
  // large_decode_threshold_bytes = 1 makes any real decode full-lane; a
  // 200-byte ADVANCED budget is far smaller than any real fixture's estimate,
  // so try_acquire refuses it.
  Sipi::SipiMemoryBudget budget(200, Sipi::AdmissionMode::ADVANCED);
  Sipi::ffi::EngineContext eng;
  eng.memory_budget = &budget;
  eng.large_decode_threshold_bytes = 1;
  Sipi::ffi::set_engine_context(eng);

  const std::string path = fixture("/unit/lena512.tif");
  std::string err_msg;
  SipiImageHandle *handle = sipi_image_new(path.c_str(), nullptr, nullptr, 0, 0, nullptr, collect_str, &err_msg);
  EXPECT_EQ(handle, nullptr);
  EXPECT_FALSE(err_msg.empty());
}

// Shipped-defaults regression (S2-33): the SipiConf default must be the
// enforcing "advanced" mode, not the shadow-counting "basic" mode, so a
// generic deployment that never sets admission_mode still enforces the
// memory budget.
TEST(SeamProbe, ShippedAdmissionModeDefaultIsAdvanced)
{
  Sipi::SipiConf conf;
  EXPECT_EQ(conf.getAdmissionMode(), "advanced");
}

TEST(SeamProbe, ImageNewSucceedsWhenBudgetDisabled)
{
  // The current default (memory_budget = nullptr): the charge only gates when
  // a budget is installed — a positive control for the refused case above.
  Sipi::ffi::EngineContext eng;
  Sipi::ffi::set_engine_context(eng);

  const std::string path = fixture("/unit/lena512.tif");
  std::string err_msg;
  SipiImageHandle *handle = sipi_image_new(path.c_str(), nullptr, nullptr, 0, 0, nullptr, collect_str, &err_msg);
  ASSERT_NE(handle, nullptr);
  sipi_image_free(handle);
}

TEST(SeamProbe, FileDimsReadsNativeShape)
{
  const std::string path = fixture("/unit/lena512.tif");
  std::uint64_t nx = 0;
  std::uint64_t ny = 0;
  int orientation = -1;
  std::string err_msg;
  ASSERT_EQ(sipi_image_file_dims(path.c_str(), &nx, &ny, &orientation, collect_str, &err_msg), 0) << err_msg;
  EXPECT_EQ(nx, 512u);
  EXPECT_EQ(ny, 512u);
}

// DEV-7604: the Lua `SipiImage.dims` probe runs under the decode deadline. The
// fixture wedges inside read_shape (DEV-7080), so the call must return the
// deadline error within the deadline plus a margin instead of hanging.
TEST(SeamProbe, FileDimsOnHangingFileHitsTheDeadline)
{
  // set_engine_context is process-global: restore the previous context even
  // when an assertion below fails.
  struct RestoreEngine
  {
    Sipi::ffi::EngineContext saved = Sipi::ffi::engine_context_or_default();
    ~RestoreEngine() { Sipi::ffi::set_engine_context(saved); }
  } restore;

  Sipi::ffi::EngineContext eng;
  eng.decode_timeout_ms = 2000;
  Sipi::ffi::set_engine_context(eng);

  const std::string path = fixture("/hang/dev7080_read_shape_hang.jp2");
  std::uint64_t nx = 0;
  std::uint64_t ny = 0;
  int orientation = -1;
  std::string err_msg;
  const auto start = std::chrono::steady_clock::now();
  const int rc = sipi_image_file_dims(path.c_str(), &nx, &ny, &orientation, collect_str, &err_msg);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_NE(rc, 0);
  EXPECT_EQ(err_msg, "image decode deadline exceeded");
  EXPECT_LT(elapsed, std::chrono::milliseconds(2000) + std::chrono::seconds(10));
}
