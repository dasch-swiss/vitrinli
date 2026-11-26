/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <string>

#include <exiv2/exiv2.hpp>

#include "metadata/exif.h"

namespace {

// Exif.Photo.UserComment: an 8-byte charset id, then the text in that charset.
const std::string kUnicodePrefix("UNICODE\0", 8);
const std::string kJisPrefix("JIS\0\0\0\0\0", 8);

std::shared_ptr<Sipi::Exif> exif_with_user_comment(const std::string &comment)
{
  Exiv2::CommentValue value;
  value.read(reinterpret_cast<const Exiv2::byte *>(comment.data()), comment.size(), Exiv2::littleEndian);
  Exiv2::ExifData data;
  data.add(Exiv2::ExifKey("Exif.Photo.UserComment"), &value);
  Exiv2::Blob blob;
  Exiv2::ExifParser::encode(blob, Exiv2::littleEndian, data);
  auto exif = Sipi::Exif::parse(blob.data(), static_cast<unsigned int>(blob.size()));
  EXPECT_TRUE(exif.has_value());
  return exif.value_or(nullptr);
}

// The UserComment line of the printed Exif, without the trailing newline.
std::string user_comment_line(Sipi::Exif &exif)
{
  std::ostringstream out;
  EXPECT_NO_THROW(out << exif);
  std::istringstream lines(out.str());
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with("Exif.Photo.UserComment")) { return line; }
  }
  ADD_FAILURE() << "no UserComment line in:\n" << out.str();
  return {};
}

}// namespace

TEST(ExifCharset, Ucs2UserCommentPrints)
{
  auto exif = exif_with_user_comment(kUnicodePrefix + std::string("H\0i\0", 4));
  ASSERT_NE(exif, nullptr);
  const std::string line = user_comment_line(*exif);
  EXPECT_TRUE(line.ends_with("  charset=Unicode Hi")) << line;
}

TEST(ExifCharset, CharsetPrefixWithoutTextPrints)
{
  auto exif = exif_with_user_comment(kUnicodePrefix);
  ASSERT_NE(exif, nullptr);
  const std::string line = user_comment_line(*exif);
  EXPECT_TRUE(line.ends_with("  charset=Unicode ")) << line;
}

// An odd byte count is not UCS-2: the conversion fails with or without iconv.
TEST(ExifCharset, UnconvertibleUserCommentPrintsPlaceholder)
{
  auto exif = exif_with_user_comment(kUnicodePrefix + std::string("H\0i", 3));
  ASSERT_NE(exif, nullptr);
  const std::string line = user_comment_line(*exif);
  EXPECT_TRUE(line.ends_with("  <unconvertible>")) << line;
}

// Exiv2 does not convert JIS text, so the bytes are printed as they are; the
// test pins only that printing does not throw.
TEST(ExifCharset, InvalidJisUserCommentPrints)
{
  auto exif = exif_with_user_comment(kJisPrefix + std::string("\x1b$B\xff\xfe", 5));
  ASSERT_NE(exif, nullptr);
  const std::string line = user_comment_line(*exif);
  EXPECT_NE(line.find("charset=Jis "), std::string::npos) << line;
}
