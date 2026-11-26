/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A JPEG 2000 error carries Kakadu's own message text, and the JPX writer
 * refuses a channel layout it has no colour description for (DEV-8091).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "error/SipiValueError.h"
#include "image/SipiImage.h"
#include "logging/logger.h"
#include "test_paths.h"

namespace {

using Sipi::ErrorCode;
using Sipi::PhotometricInterpretation;
using Sipi::SipiImage;

const std::string kLena = sipi::test::data_dir() + "/images/unit/lena512.jp2";
constexpr const char *kLeadIn = "Kakadu-library: ";

// Joins the whitespace-separated words of `text` with one space.
std::string words_of(const std::string &text)
{
  std::istringstream in(text);
  std::string word;
  std::string joined;
  while (in >> word) { joined += (joined.empty() ? "" : " ") + word; }
  return joined;
}

std::size_t count_of(const std::string &haystack, const std::string &needle)
{
  std::size_t n = 0;
  for (auto pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1)) { ++n; }
  return n;
}

// The first `nbytes` of lena512.jp2 from `marker` on, as `name`: from the
// SOC+SIZ marker, a raw codestream cut inside its SIZ marker segment, so
// codestream creation raises a Kakadu error; from the start, a JP2 file cut
// inside its boxes.
std::string write_cut(const std::string &marker, std::size_t nbytes, const std::string &name)
{
  std::ifstream in(kLena, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto from =
    marker.empty() ? bytes.begin() : std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
  if (bytes.end() - from < static_cast<std::ptrdiff_t>(nbytes)) { return {}; }
  const std::string path = sipi::test::tmp_dir() + "/" + name;
  std::ofstream out(path, std::ios::binary);
  out.write(&*from, static_cast<std::streamsize>(nbytes));
  return path;
}

std::string write_truncated_codestream() { return write_cut("\xFF\x4F\xFF\x51", 20, "_truncated_main_header.j2k"); }

class J2kErrorText : public ::testing::Test
{
protected:
  void SetUp() override
  {
    was_cli_mode = is_cli_mode();
    set_cli_mode(true);
  }

  void TearDown() override { set_cli_mode(was_cli_mode); }

  bool was_cli_mode = false;
};

TEST_F(J2kErrorText, ReadErrorCarriesKakaduText)
{
  const std::string path = write_truncated_codestream();
  ASSERT_FALSE(path.empty());

  SipiImage img;
  testing::internal::CaptureStderr();
  const auto r = img.read(path);
  const std::string logged = testing::internal::GetCapturedStderr();
  ASSERT_FALSE(r.has_value());

  const std::string message = r.error().raw_message();
  const auto lead = message.find(std::string("(") + kLeadIn);
  ASSERT_NE(lead, std::string::npos) << message;
  ASSERT_EQ(message.back(), ')') << message;
  const std::string kakadu_text = message.substr(lead + 1, message.size() - lead - 2);
  EXPECT_GT(kakadu_text.size(), std::string(kLeadIn).size()) << message;
  EXPECT_NE(words_of(logged).find(kakadu_text), std::string::npos) << logged;
  std::remove(path.c_str());
}

TEST_F(J2kErrorText, SecondReadErrorCarriesOnlyItsOwnText)
{
  const std::string path = write_truncated_codestream();
  ASSERT_FALSE(path.empty());

  SipiImage first;
  testing::internal::CaptureStderr();
  const auto r1 = first.read(path);
  SipiImage second;
  const auto r2 = second.read(path);
  testing::internal::GetCapturedStderr();
  ASSERT_FALSE(r1.has_value());
  ASSERT_FALSE(r2.has_value());

  EXPECT_EQ(count_of(r2.error().raw_message(), kLeadIn), 1u) << r2.error().raw_message();
  EXPECT_EQ(r2.error().raw_message(), r1.error().raw_message());
  std::remove(path.c_str());
}

// Two threads read two different broken files at once; each error carries
// only the text of its own read, as when read alone.
TEST_F(J2kErrorText, ConcurrentReadErrorsKeepTheirOwnText)
{
  const std::vector<std::string> paths{ write_truncated_codestream(), write_cut("", 100, "_truncated_boxes.jp2") };
  ASSERT_FALSE(paths[0].empty());
  ASSERT_FALSE(paths[1].empty());

  testing::internal::CaptureStderr();
  std::vector<std::string> alone;
  for (const auto &path : paths) {
    SipiImage img;
    const auto r = img.read(path);
    alone.push_back(r ? std::string("read succeeded") : r.error().raw_message());
  }

  constexpr int kRounds = 50;
  std::vector<std::vector<std::string>> seen(paths.size());
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < paths.size(); ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kRounds; ++i) {
        SipiImage img;
        const auto r = img.read(paths[t]);
        seen[t].push_back(r ? std::string("read succeeded") : r.error().raw_message());
      }
    });
  }
  for (auto &thread : threads) { thread.join(); }
  testing::internal::GetCapturedStderr();

  for (std::size_t t = 0; t < paths.size(); ++t) {
    for (const auto &message : seen[t]) { EXPECT_EQ(message, alone[t]); }
    std::remove(paths[t].c_str());
  }
}

TEST(J2kWriteColour, TwoColourChannelsWithoutExtraSamplesFailBeforeKakadu)
{
  constexpr std::size_t nx = 228, ny = 320, nc = 2, bps = 8;
  SipiImage img;
  img.set_pixels(std::vector<Sipi::byte>(nx * ny * nc, 128), nx, ny, nc, bps);
  img.setPhoto(PhotometricInterpretation::MINISBLACK);
  ASSERT_EQ(img.getIcc(), nullptr);
  ASSERT_TRUE(img.getEs().empty());

  const auto r = img.write("jpx", sipi::test::tmp_dir() + "/_two_colour_channels.jpx");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::kWriteFailed);
  EXPECT_NE(r.error().raw_message().find("nc=2"), std::string::npos) << r.error().raw_message();
  EXPECT_NE(r.error().raw_message().find("extra samples=0"), std::string::npos) << r.error().raw_message();
  EXPECT_EQ(r.error().raw_message().find(kLeadIn), std::string::npos) << r.error().raw_message();
  std::remove((sipi::test::tmp_dir() + "/_two_colour_channels.jpx").c_str());
}

}// namespace
