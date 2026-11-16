/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

// XMP and IPTC held only as the raw copies in EXIF IFD0 (Exif.Image.XMLPacket,
// Exif.Image.IPTCNAA) are written as the primary blocks, because the EXIF
// encoder never writes the copies. A primary block wins; a copy that does not
// parse whole is left out.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <exiv2/exiv2.hpp>

#include "image/SipiImage.h"
#include "metadata/exif.h"
#include "metadata/iptc.h"
#include "metadata/xmp.h"
#include "test_paths.h"

namespace {

// See jpeg_metadata_segments_test.cpp: Exiv2's XMP registry leaks without it.
struct Exiv2XmpTerminate : ::testing::Environment
{
  void TearDown() override { Exiv2::XmpParser::terminate(); }
};
[[maybe_unused]] auto *kExiv2XmpTerminate = ::testing::AddGlobalTestEnvironment(new Exiv2XmpTerminate);

const std::string kSource = sipi::test::data_dir() + "/images/unit/lena512.tif";
const std::string kXmpRights =
  R"(<dc:rights><rdf:Alt><rdf:li xml:lang="x-default">DaSCH IFD0 rights</rdf:li></rdf:Alt></dc:rights>)";
const std::string kIptcRights = "DaSCH IFD0 IPTC";

std::string scratch(const std::string &name) { return sipi::test::tmp_dir() + "/" + name; }

std::vector<unsigned char> file_bytes(const std::string &path)
{
  std::ifstream in(path, std::ios::binary);
  return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::string xmp_packet(const std::string &rights, const std::string &extra = "")
{
  return R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/">)"
         R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
         R"(<rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/")"
         R"( xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/">)"
         + rights + extra + R"(</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>)";
}

Exiv2::IptcData iptc_copyright(const std::string &copyright)
{
  Exiv2::IptcData iptc;
  iptc["Iptc.Application2.Copyright"] = copyright;
  return iptc;
}

// An EXIF block as a file carries it, with the given XMP packet and IPTC data
// only as the IFD0 copies.
std::shared_ptr<Sipi::Exif> exif_with_copies(const std::string &packet, Exiv2::IptcData iptc)
{
  Exiv2::ExifData data;
  data["Exif.Image.Copyright"] = "DaSCH";
  data["Exif.Image.IPTCNAA"] = static_cast<std::uint32_t>(0);
  Exiv2::XmpData xmp;
  xmp.setPacket(packet);
  xmp.usePacket(true);
  Exiv2::MemIo io;
  Exiv2::TiffParser::encode(io, nullptr, 0, Exiv2::littleEndian, data, iptc, xmp);
  auto exif = Sipi::Exif::parse(io.mmap(), static_cast<unsigned int>(io.size()));
  EXPECT_TRUE(exif.has_value());
  return exif.value_or(nullptr);
}

// lena512.tif with `exif` as its only metadata block.
std::unique_ptr<Sipi::SipiImage> source_with(std::shared_ptr<Sipi::Exif> exif)
{
  auto img = std::make_unique<Sipi::SipiImage>();
  EXPECT_TRUE(img->read(kSource).has_value());
  img->set_xmp(nullptr);
  img->set_iptc(nullptr);
  img->set_exif(std::move(exif));
  return img;
}

std::string iptc_copyright_of(const Sipi::SipiImage &img)
{
  if (img.getIptc() == nullptr) { return "<absent>"; }
  const auto bytes = img.getIptc()->iptcBytes();
  Exiv2::IptcData data;
  if (Exiv2::IptcParser::decode(data, bytes.data(), bytes.size()) != 0) { return "<unparsable>"; }
  return data["Iptc.Application2.Copyright"].toString();
}

std::string xmp_of(const Sipi::SipiImage &img) { return img.getXmp() == nullptr ? "" : img.getXmp()->xmpBytes(); }

TEST(Ifd0CopyPromotion, RightsOnlyInTheCopiesSurviveEveryFormat)
{
  const auto exif = exif_with_copies(xmp_packet(kXmpRights), iptc_copyright(kIptcRights));
  ASSERT_NE(exif, nullptr);
  ASSERT_TRUE(exif->value_bytes("Exif.Image.XMLPacket").has_value());
  ASSERT_TRUE(exif->value_bytes("Exif.Image.IPTCNAA").has_value());

  for (const auto &[ftype, ext] :
    { std::pair<std::string, std::string>{ "jpg", "jpg" }, { "png", "png" }, { "tif", "tif" }, { "jpx", "jp2" } }) {
    auto img = source_with(exif_with_copies(xmp_packet(kXmpRights), iptc_copyright(kIptcRights)));
    const std::string path = scratch("ifd0_copies." + ext);
    const auto written = img->write(ftype, path);
    ASSERT_TRUE(written.has_value()) << ftype << ": " << written.error().diagnostic_message();

    Sipi::SipiImage reread;
    ASSERT_TRUE(reread.read(path).has_value()) << ftype;
    EXPECT_NE(xmp_of(reread).find(kXmpRights), std::string::npos) << ftype;
    EXPECT_EQ(iptc_copyright_of(reread), kIptcRights) << ftype;
  }
}

TEST(Ifd0CopyPromotion, APrimaryBlockWinsOverTheCopy)
{
  const std::string primary_rights =
    R"(<dc:rights><rdf:Alt><rdf:li xml:lang="x-default">DaSCH primary rights</rdf:li></rdf:Alt></dc:rights>)";
  auto img = source_with(exif_with_copies(xmp_packet(kXmpRights), iptc_copyright(kIptcRights)));
  img->set_xmp(std::make_shared<Sipi::Xmp>(xmp_packet(primary_rights)));
  const auto raw = Exiv2::IptcParser::encode(iptc_copyright("DaSCH primary IPTC"));
  auto primary_iptc = Sipi::Iptc::parse(raw.c_data(), static_cast<unsigned int>(raw.size()));
  ASSERT_TRUE(primary_iptc.has_value());
  img->set_iptc(*primary_iptc);
  const std::string path = scratch("ifd0_primary_wins.jpg");
  ASSERT_TRUE(img->write("jpg", path).has_value());

  Sipi::SipiImage reread;
  ASSERT_TRUE(reread.read(path).has_value());
  EXPECT_NE(xmp_of(reread).find(primary_rights), std::string::npos);
  EXPECT_EQ(xmp_of(reread).find(kXmpRights), std::string::npos);
  EXPECT_EQ(iptc_copyright_of(reread), "DaSCH primary IPTC");
}

TEST(Ifd0CopyPromotion, ACopyThatDoesNotParseWholeIsNotPromoted)
{
  const auto valid = Exiv2::IptcParser::encode(iptc_copyright(kIptcRights));
  const std::vector<std::uint8_t> truncated(valid.c_data(), valid.c_data() + valid.size() - 3);
  const std::string garbage = "not an IIM block";
  const std::vector<std::uint8_t> garbage_bytes(garbage.begin(), garbage.end());
  const std::string broken_xmp = "<x:xmpmeta>" + kXmpRights;
  const std::string no_rdf = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"/>";

  for (const auto &[xmp, iptc] :
    { std::pair<std::string, std::vector<std::uint8_t>>{ broken_xmp, truncated }, { no_rdf, garbage_bytes } }) {
    auto exif = std::make_shared<Sipi::Exif>();
    exif->addKeyVal("Exif.Image.Copyright", std::string("DaSCH"));
    exif->addKeyVal(0x02bc, "Image", reinterpret_cast<const std::uint8_t *>(xmp.data()), xmp.size());
    exif->addKeyVal(0x83bb, "Image", iptc.data(), iptc.size());
    auto img = source_with(exif);
    img->promote_ifd0_copies();
    EXPECT_EQ(img->getXmp(), nullptr) << xmp;
    EXPECT_EQ(img->getIptc(), nullptr) << xmp;

    const std::string path = scratch("ifd0_unparsable.jpg");
    ASSERT_TRUE(img->write("jpg", path).has_value());
    Sipi::SipiImage reread;
    ASSERT_TRUE(reread.read(path).has_value());
    EXPECT_EQ(reread.getXmp(), nullptr);
    EXPECT_EQ(reread.getIptc(), nullptr);
  }
}

TEST(Ifd0CopyPromotion, APromotedXmpOverOneApp1SegmentIsWrittenAsExtendedXmp)
{
  const std::string history = "<photoshop:History>" + std::string(100000, 'h') + "</photoshop:History>";
  auto img = source_with(exif_with_copies(xmp_packet(kXmpRights, history), iptc_copyright(kIptcRights)));
  const std::string path = scratch("ifd0_large_xmp.jpg");
  ASSERT_TRUE(img->write("jpg", path).has_value());

  const auto bytes = file_bytes(path);
  const std::string extension_sig("http://ns.adobe.com/xmp/extension/\0", 35);
  EXPECT_NE(std::search(bytes.begin(), bytes.end(), extension_sig.begin(), extension_sig.end()), bytes.end());
  Sipi::SipiImage reread;
  ASSERT_TRUE(reread.read(path).has_value());
  EXPECT_NE(xmp_of(reread).find(kXmpRights), std::string::npos);
}

}// namespace
