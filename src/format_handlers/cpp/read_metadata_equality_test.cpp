/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

// `sipi query` prints `operator<<` of the image that `SipiImage::read_metadata`
// leaves. These tests hold that output equal to the one of a full
// `SipiImage::read`, for every fixture kind the four handlers read.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "SipiIOTiff.h"
#include "image/SipiImage.h"
#include "test_paths.h"

namespace {

const std::string kImages = sipi::test::data_dir() + "/images";

bool is_image(const std::filesystem::path &p)
{
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext == ".tif" || ext == ".tiff" || ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".jp2"
         || ext == ".jpx";
}

// Every image in images/unit/, plus the kinds that directory lacks: a 1-bit
// TIFF and a CMYK JPEG (with and without the Adobe APP14 marker).
std::vector<std::string> fixtures()
{
  std::vector<std::string> paths;
  for (const auto &entry : std::filesystem::directory_iterator(kImages + "/unit")) {
    if (entry.is_regular_file() && is_image(entry.path())) { paths.push_back(entry.path().string()); }
  }
  std::sort(paths.begin(), paths.end());
  paths.push_back(kImages + "/bilevel/bilevel_lzw_miniswhite.tif");
  paths.push_back(kImages + "/bilevel/bilevel_none_miniswhite.tif");
  paths.push_back(kImages + "/bilevel/bilevel_lzw_minisblack.tif");
  paths.push_back(kImages + "/jpeg/cmyk/cmyk_photoshop_app14.jpg");
  paths.push_back(kImages + "/jpeg/cmyk/cmyk_raw_no_app14.jpg");
  return paths;
}

// Without the "ICC-Date" line: a profile that lcms creates during the read
// (CIELab) carries the wall-clock time, so two reads can print different
// seconds.
std::string printed(const Sipi::SipiImage &img)
{
  std::ostringstream out;
  out << img;
  std::istringstream lines(out.str());
  std::string kept;
  for (std::string line; std::getline(lines, line);) {
    if (!line.starts_with("ICC-Date")) { kept += line + '\n'; }
  }
  return kept;
}

class ReadMetadataEquality : public ::testing::TestWithParam<std::string>
{
protected:
  static void SetUpTestSuite() { Sipi::SipiIOTiff::initLibrary(); }
};

TEST_P(ReadMetadataEquality, PrintsWhatAFullReadPrints)
{
  const std::string &path = GetParam();

  Sipi::SipiImage full;
  auto full_status = full.read(path);
  ASSERT_TRUE(full_status) << path << ": " << full_status.error().diagnostic_message();

  Sipi::SipiImage meta;
  auto meta_status = meta.read_metadata(path);
  ASSERT_TRUE(meta_status) << path << ": " << meta_status.error().diagnostic_message();

  EXPECT_EQ(printed(meta), printed(full)) << path;
  EXPECT_EQ(meta.getOrientation(), full.getOrientation()) << path;
  EXPECT_TRUE(meta.pixels_view().empty()) << path;
}

INSTANTIATE_TEST_SUITE_P(Fixtures,
  ReadMetadataEquality,
  ::testing::ValuesIn(fixtures()),
  [](const ::testing::TestParamInfo<std::string> &info) {
    std::string name = std::filesystem::path(info.param).parent_path().filename().string() + "_"
                       + std::filesystem::path(info.param).filename().string();
    std::replace_if(name.begin(), name.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return name;
  });

TEST(ReadMetadata, UnknownFormatFailsAsAFullReadFails)
{
  Sipi::SipiIOTiff::initLibrary();
  const std::string path = kImages + "/unit/test.csv";

  Sipi::SipiImage full;
  auto full_status = full.read(path);
  ASSERT_FALSE(full_status);

  Sipi::SipiImage meta;
  auto meta_status = meta.read_metadata(path);
  ASSERT_FALSE(meta_status);

  EXPECT_EQ(meta_status.error().code(), full_status.error().code());
  EXPECT_EQ(meta_status.error().code(), Sipi::ErrorCode::kUnsupportedFormat);
  EXPECT_EQ(meta_status.error().diagnostic_message(), full_status.error().diagnostic_message());
}

}// namespace
