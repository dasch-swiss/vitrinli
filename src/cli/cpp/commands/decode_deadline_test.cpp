/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gtest/gtest.h>

#include <string>

#include "cli/commands/decode_deadline.h"
#include "ffi/engine_context.h"
#include "format_handlers/SipiIOTiff.h"
#include "test_paths.h"

namespace {

const std::string kUnit = sipi::test::data_dir() + "/images/unit/";

TEST(ReadMetadataWithDeadline, ReadsTheHeaderWithoutPixels)
{
  Sipi::SipiIOTiff::initLibrary();
  auto decoded = Sipi::cli::read_metadata_with_deadline(Sipi::ffi::kDefaultDecodeTimeoutMs, kUnit + "cmyk.tif");
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(decoded->status) << decoded->status.error().diagnostic_message();
  EXPECT_EQ(decoded->img.getNx(), 4872u);
  EXPECT_EQ(decoded->img.getNy(), 3864u);
  EXPECT_EQ(decoded->img.getNc(), 4u);
  EXPECT_TRUE(decoded->img.pixels_view().empty());
}

TEST(ReadMetadataWithDeadline, ReportsAFileNoHandlerReads)
{
  auto decoded = Sipi::cli::read_metadata_with_deadline(Sipi::ffi::kDefaultDecodeTimeoutMs, kUnit + "test.csv");
  ASSERT_TRUE(decoded.has_value());
  ASSERT_FALSE(decoded->status);
  EXPECT_EQ(decoded->status.error().code(), Sipi::ErrorCode::kUnsupportedFormat);
}

}// namespace
