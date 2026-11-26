/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The PNG reader's extra samples match the decoded channel layout after
 * libpng's tRNS and bKGD transforms, so the image writes as JPX (DEV-8091).
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include <png.h>

#include "image/SipiImage.h"
#include "test_paths.h"

namespace {

using Sipi::SipiImage;

constexpr png_uint_32 kWidth = 228;
constexpr png_uint_32 kHeight = 320;

enum class Chunk { kNone, kBkgd, kTrns };

int png_channels(int color_type)
{
  switch (color_type) {
  case PNG_COLOR_TYPE_RGB_ALPHA:
    return 4;
  case PNG_COLOR_TYPE_RGB:
    return 3;
  case PNG_COLOR_TYPE_GRAY_ALPHA:
    return 2;
  default:
    return 1;
  }
}

// An 8-bit PNG with no sRGB, gAMA or iCCP chunk, as in DEV-8091. A palette
// image gets a 256-entry palette; its tRNS makes entry 0 transparent.
bool write_png(const std::string &path, int color_type, Chunk chunk)
{
  const int channels = png_channels(color_type);
  FILE *fp = std::fopen(path.c_str(), "wb");
  if (fp == nullptr) { return false; }
  png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info_ptr = png_ptr != nullptr ? png_create_info_struct(png_ptr) : nullptr;
  if (info_ptr == nullptr || setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_write_struct(&png_ptr, &info_ptr);
    std::fclose(fp);
    return false;
  }
  png_init_io(png_ptr, fp);
  png_set_IHDR(png_ptr,
    info_ptr,
    kWidth,
    kHeight,
    8,
    color_type,
    PNG_INTERLACE_NONE,
    PNG_COMPRESSION_TYPE_DEFAULT,
    PNG_FILTER_TYPE_DEFAULT);
  png_color_16 colour{};
  colour.red = colour.green = colour.blue = colour.gray = 255;
  if (chunk == Chunk::kBkgd) { png_set_bKGD(png_ptr, info_ptr, &colour); }
  std::vector<png_color> palette(256);
  for (std::size_t i = 0; i < palette.size(); ++i) {
    palette[i].red = palette[i].green = palette[i].blue = static_cast<png_byte>(i);
  }
  png_byte palette_alpha = 0;
  if (color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_PLTE(png_ptr, info_ptr, palette.data(), static_cast<int>(palette.size()));
  }
  if (chunk == Chunk::kTrns) {
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
      png_set_tRNS(png_ptr, info_ptr, &palette_alpha, 1, nullptr);
    } else {
      png_set_tRNS(png_ptr, info_ptr, nullptr, 0, &colour);
    }
  }
  png_write_info(png_ptr, info_ptr);
  std::vector<png_byte> row(static_cast<std::size_t>(kWidth) * channels);
  for (std::size_t i = 0; i < row.size(); ++i) { row[i] = static_cast<png_byte>(i % 251); }
  for (png_uint_32 y = 0; y < kHeight; ++y) { png_write_row(png_ptr, row.data()); }
  png_write_end(png_ptr, nullptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  std::fclose(fp);
  return true;
}

struct Case
{
  const char *name;
  int color_type;
  Chunk chunk;
  std::size_t nc;
  std::size_t es;
};

class PngAlphaLayout : public ::testing::TestWithParam<Case>
{
};

TEST_P(PngAlphaLayout, ExtraSamplesFollowDecodedChannelsAndJpxWrites)
{
  const Case &c = GetParam();
  const std::string png = sipi::test::tmp_dir() + "/_alpha_layout_" + c.name + ".png";
  ASSERT_TRUE(write_png(png, c.color_type, c.chunk));

  SipiImage img;
  const auto read = img.read(png);
  ASSERT_TRUE(read.has_value()) << read.error().raw_message();
  EXPECT_EQ(img.getNc(), c.nc);
  EXPECT_EQ(img.getEs().size(), c.es);
  EXPECT_EQ(img.getIcc(), nullptr);

  const std::string jpx = sipi::test::tmp_dir() + "/_alpha_layout_" + c.name + ".jpx";
  const auto written = img.write("jpx", jpx);
  ASSERT_TRUE(written.has_value()) << written.error().raw_message();
  SipiImage back;
  const bool read_back = back.read(jpx).has_value();
  std::remove(png.c_str());
  std::remove(jpx.c_str());
  ASSERT_TRUE(read_back);
  EXPECT_EQ(back.getNc(), c.nc);
}

INSTANTIATE_TEST_SUITE_P(Layouts,
  PngAlphaLayout,
  ::testing::Values(Case{ "rgb", PNG_COLOR_TYPE_RGB, Chunk::kNone, 3, 0 },
    Case{ "rgba_bkgd", PNG_COLOR_TYPE_RGB_ALPHA, Chunk::kBkgd, 3, 0 },
    Case{ "rgb_trns", PNG_COLOR_TYPE_RGB, Chunk::kTrns, 4, 1 },
    Case{ "gray_trns", PNG_COLOR_TYPE_GRAY, Chunk::kTrns, 2, 1 },
    Case{ "rgba", PNG_COLOR_TYPE_RGB_ALPHA, Chunk::kNone, 4, 1 },
    Case{ "gray_alpha_bkgd", PNG_COLOR_TYPE_GRAY_ALPHA, Chunk::kBkgd, 1, 0 },
    Case{ "palette_trns", PNG_COLOR_TYPE_PALETTE, Chunk::kTrns, 4, 1 }),
  [](const ::testing::TestParamInfo<Case> &info) { return std::string(info.param.name); });

}// namespace
