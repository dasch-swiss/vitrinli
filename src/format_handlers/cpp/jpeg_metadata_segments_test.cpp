/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

// EXIF, XMP and IPTC larger than one JPEG marker segment (65533 bytes) are
// written: Extended XMP, IPTC across APP13 segments, EXIF without its
// previews. A block that cannot be fitted is left out with a warning; the
// JPEG and every other block are still written. Extended XMP is read back
// into one packet.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <exiv2/exiv2.hpp>

#include "image/SipiImage.h"
#include "metadata/exif.h"
#include "metadata/iptc.h"
#include "metadata/xmp.h"
#include "test_paths.h"

namespace {

// Exiv2's XMP decode (readMetadata included) registers each unknown namespace
// in a process-global registry that only XmpParser::terminate() frees; without
// it LeakSanitizer reports the registrations as leaks.
struct Exiv2XmpTerminate : ::testing::Environment
{
  void TearDown() override { Exiv2::XmpParser::terminate(); }
};
[[maybe_unused]] auto *kExiv2XmpTerminate = ::testing::AddGlobalTestEnvironment(new Exiv2XmpTerminate);

const std::string kSource = sipi::test::data_dir() + "/images/unit/lena512.tif";

std::string scratch(const std::string &name) { return sipi::test::tmp_dir() + "/" + name; }

std::vector<unsigned char> file_bytes(const std::string &path)
{
  std::ifstream in(path, std::ios::binary);
  return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct Segment
{
  int marker;
  std::vector<unsigned char> payload;
};

// The marker segments before the first SOS.
std::vector<Segment> segments(const std::vector<unsigned char> &jpeg)
{
  std::vector<Segment> out;
  std::size_t pos = 2;
  while (pos + 4 <= jpeg.size() && jpeg[pos] == 0xff && jpeg[pos + 1] != 0xda) {
    const std::size_t len = (std::size_t{ jpeg[pos + 2] } << 8) | jpeg[pos + 3];
    out.push_back({ jpeg[pos + 1], { jpeg.begin() + pos + 4, jpeg.begin() + pos + 2 + len } });
    pos += 2 + len;
  }
  return out;
}

bool starts_with(const std::vector<unsigned char> &bytes, const std::string &prefix)
{
  return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin());
}

Exiv2::Image::UniquePtr exiv2_read(const std::string &path)
{
  auto image = Exiv2::ImageFactory::open(path);
  image->readMetadata();
  return image;
}

bool has_app1(const std::string &path, const std::string &signature)
{
  const auto all = segments(file_bytes(path));
  return std::any_of(
    all.begin(), all.end(), [&](const Segment &s) { return s.marker == 0xe1 && starts_with(s.payload, signature); });
}

std::shared_ptr<Sipi::Iptc> iptc_copyright()
{
  Exiv2::IptcData data;
  data["Iptc.Application2.Copyright"] = "DaSCH";
  const auto raw = Exiv2::IptcParser::encode(data);
  auto iptc = Sipi::Iptc::parse(raw.c_data(), static_cast<unsigned int>(raw.size()));
  EXPECT_TRUE(iptc.has_value());
  return iptc.value_or(nullptr);
}

// Writes `img` as JPEG to `path`; the returned log output is what the write logged.
std::string write_jpeg_logged(Sipi::SipiImage &img, const std::string &path)
{
  ::testing::internal::CaptureStdout();
  const auto written = img.write("jpg", path);
  std::string log = ::testing::internal::GetCapturedStdout();
  EXPECT_TRUE(written.has_value()) << written.error().diagnostic_message();
  return log;
}

TEST(JpegMetadataSegments, LargeXmpIsSplitIntoExtendedXmpKeepingRightsInTheStandardPacket)
{
  const std::string rights = R"(<dc:rights><rdf:Alt><rdf:li xml:lang="x-default">DaSCH</rdf:li></rdf:Alt></dc:rights>)";
  const std::string history(100000, 'h');
  const std::string packet = R"(<?xpacket begin="" id="W5M0MpCehiHzreSzNTczkc9d"?><x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                             R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                             R"(<rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/")"
                             R"( xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/" photoshop:Credit="DaSCH Credit">)"
                             + rights + "<photoshop:History>" + history + "</photoshop:History>"
                             + R"(</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end="w"?>)";
  ASSERT_GT(packet.size(), 65533U);

  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(kSource).has_value());
  img.set_xmp(std::make_shared<Sipi::Xmp>(packet));
  const std::string path = scratch("large_xmp.jpg");
  const auto written = img.write("jpg", path);
  ASSERT_TRUE(written.has_value()) << written.error().diagnostic_message();

  const std::string standard_sig("http://ns.adobe.com/xap/1.0/\0", 29);
  const std::string extension_sig("http://ns.adobe.com/xmp/extension/\0", 35);
  std::string standard;
  std::string guid;
  std::string extended;
  std::size_t declared = 0;
  for (const auto &s : segments(file_bytes(path))) {
    if (s.marker != 0xe1) { continue; }
    if (starts_with(s.payload, standard_sig)) { standard.assign(s.payload.begin() + 29, s.payload.end()); }
    if (starts_with(s.payload, extension_sig)) {
      guid.assign(s.payload.begin() + 35, s.payload.begin() + 67);
      const auto *p = s.payload.data() + 67;
      declared = (std::size_t{ p[0] } << 24) | (std::size_t{ p[1] } << 16) | (std::size_t{ p[2] } << 8) | p[3];
      const std::size_t offset =
        (std::size_t{ p[4] } << 24) | (std::size_t{ p[5] } << 16) | (std::size_t{ p[6] } << 8) | p[7];
      EXPECT_EQ(offset, extended.size());
      extended.append(s.payload.begin() + 75, s.payload.end());
    }
  }
  ASSERT_FALSE(standard.empty());
  EXPECT_LE(standard.size() + 29, 65533U);
  EXPECT_NE(standard.find(rights), std::string::npos);
  EXPECT_NE(standard.find(R"(xmpNote:HasExtendedXMP=")" + guid + '"'), std::string::npos);
  EXPECT_EQ(standard.find(history), std::string::npos);
  ASSERT_EQ(guid.size(), 32U);
  EXPECT_EQ(extended.size(), declared);
  EXPECT_NE(extended.find(history), std::string::npos);

  // Exiv2 reads the StandardXMP only; the ExtendedXMP parses on its own.
  const auto image = exiv2_read(path);
  EXPECT_EQ(image->xmpData()["Xmp.dc.rights"].toString(), "lang=\"x-default\" DaSCH");
  EXPECT_EQ(image->xmpData()["Xmp.photoshop.Credit"].toString(), "DaSCH Credit");
  Exiv2::XmpData moved;
  ASSERT_EQ(Exiv2::XmpParser::decode(moved, extended), 0);
  EXPECT_EQ(moved["Xmp.photoshop.History"].toString(), history);

  // Read back, the ExtendedXMP is merged into the packet.
  Sipi::SipiImage reread;
  ASSERT_TRUE(reread.read(path).has_value());
  ASSERT_NE(reread.getXmp(), nullptr);
  const std::string merged = reread.getXmp()->xmpBytes();
  EXPECT_EQ(merged.find("HasExtendedXMP"), std::string::npos);
  Exiv2::XmpData all;
  ASSERT_EQ(Exiv2::XmpParser::decode(all, merged), 0);
  EXPECT_EQ(all["Xmp.dc.rights"].toString(), "lang=\"x-default\" DaSCH");
  EXPECT_EQ(all["Xmp.photoshop.Credit"].toString(), "DaSCH Credit");
  EXPECT_EQ(all["Xmp.photoshop.History"].toString(), history);
}

// A packet split as the JPEG writer splits it, and its ExtendedXMP parts.
struct SplitPacket
{
  std::string standard;
  std::string guid;
  std::string extended;
};

SplitPacket split_packet()
{
  const std::string packet = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                             R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                             R"(<rdf:Description rdf:about="" xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/">)"
                             R"(<photoshop:Credit>DaSCH Credit</photoshop:Credit><photoshop:History>)"
                             + std::string(100000, 'h')
                             + "</photoshop:History></rdf:Description></rdf:RDF></x:xmpmeta>";
  const auto split = Sipi::Xmp(packet).split_extended(65533 - 29).value();
  return { split.standard, split.guid, split.extended };
}

std::string part(const std::string &guid, std::uint32_t total, std::uint32_t offset, const std::string &bytes)
{
  std::string out = guid;
  for (const std::uint32_t v : { total, offset }) {
    out += static_cast<char>(v >> 24);
    out += static_cast<char>(v >> 16);
    out += static_cast<char>(v >> 8);
    out += static_cast<char>(v);
  }
  return out + bytes;
}

std::string merge(const std::string &standard, const std::vector<std::string> &parts)
{
  const std::vector<std::string_view> views(parts.begin(), parts.end());
  return Sipi::Xmp::merge_extended(standard, views);
}

TEST(JpegMetadataSegments, ExtendedXmpPartsAreJoinedInAnyOrder)
{
  const auto s = split_packet();
  const auto total = static_cast<std::uint32_t>(s.extended.size());
  const std::size_t half = s.extended.size() / 2;
  const std::string merged = merge(s.standard,
    { part(s.guid, total, static_cast<std::uint32_t>(half), s.extended.substr(half)),
      part(s.guid, total, 0, s.extended.substr(0, half)) });
  Exiv2::XmpData all;
  ASSERT_EQ(Exiv2::XmpParser::decode(all, merged), 0);
  EXPECT_EQ(all["Xmp.photoshop.Credit"].toString(), "DaSCH Credit");
  EXPECT_EQ(all["Xmp.photoshop.History"].toString(), std::string(100000, 'h'));
}

// Other writers put xmpNote:HasExtendedXMP on the main description, as an
// attribute or a property, and bind namespaces on the extension's rdf:RDF.
TEST(JpegMetadataSegments, ExtendedXmpOfOtherWritersIsMerged)
{
  const std::string guid(32, 'C');
  const std::string extension = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                                R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#")"
                                R"( xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/">)"
                                R"(<rdf:Description rdf:about=""><photoshop:History>moved</photoshop:History>)"
                                R"(</rdf:Description></rdf:RDF></x:xmpmeta>)";
  const std::vector<std::string> parts{ part(guid, static_cast<std::uint32_t>(extension.size()), 0, extension) };
  const std::string head = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                           R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                           R"(<rdf:Description rdf:about="" xmlns:xmpNote="http://ns.adobe.com/xmp/note/")"
                           R"( xmlns:dc="http://purl.org/dc/elements/1.1/" dc:format="image/jpeg")";
  const std::string tail = "</rdf:Description></rdf:RDF></x:xmpmeta>";
  const std::string as_attribute =
    head + R"( xmpNote:HasExtendedXMP=")" + guid + R"("><dc:source>DaSCH</dc:source>)" + tail;
  const std::string as_property =
    head + R"(><xmpNote:HasExtendedXMP> )" + guid + R"( </xmpNote:HasExtendedXMP><dc:source>DaSCH</dc:source>)" + tail;
  for (const std::string &standard : { as_attribute, as_property }) {
    const std::string merged = merge(standard, parts);
    EXPECT_EQ(merged.find("HasExtendedXMP"), std::string::npos) << merged;
    Exiv2::XmpData all;
    ASSERT_EQ(Exiv2::XmpParser::decode(all, merged), 0) << merged;
    EXPECT_EQ(all["Xmp.dc.format"].toString(), "image/jpeg");
    EXPECT_EQ(all["Xmp.dc.source"].toString(), "DaSCH");
    EXPECT_EQ(all["Xmp.photoshop.History"].toString(), "moved");
  }
}

TEST(JpegMetadataSegments, XmpNeedingAnExtendedXmpOver16MiBIsNotSplit)
{
  const std::string packet = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                             R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                             R"(<rdf:Description rdf:about="" xmlns:photoshop="http://ns.adobe.com/photoshop/1.0/">)"
                             R"(<photoshop:History>)"
                             + std::string(16U << 20U, 'h')
                             + "</photoshop:History></rdf:Description></rdf:RDF></x:xmpmeta>";
  EXPECT_FALSE(Sipi::Xmp(packet).split_extended(65533 - 29).has_value());
}

TEST(JpegMetadataSegments, MalformedExtendedXmpKeepsTheStandardXmpUnchanged)
{
  const auto s = split_packet();
  const auto total = static_cast<std::uint32_t>(s.extended.size());
  const std::size_t half = s.extended.size() / 2;
  const auto at_half = static_cast<std::uint32_t>(half);
  const std::string first = part(s.guid, total, 0, s.extended.substr(0, half));
  const std::string second = part(s.guid, total, at_half, s.extended.substr(half));
  const std::string other_guid(32, s.guid.front() == 'A' ? 'B' : 'A');

  EXPECT_EQ(merge(s.standard, { first }), s.standard) << "a missing part";
  EXPECT_EQ(merge(s.standard,
              { part(other_guid, total, 0, s.extended.substr(0, half)),
                part(other_guid, total, at_half, s.extended.substr(half)) }),
    s.standard)
    << "a wrong GUID";
  EXPECT_EQ(merge(s.standard, { first, part(s.guid, total, at_half - 10, s.extended.substr(half - 10)) }), s.standard)
    << "overlapping offsets";
  EXPECT_EQ(merge(s.standard, { part(s.guid, total + 1, 0, s.extended.substr(0, half)), second }), s.standard)
    << "disagreeing full lengths";
  // Trailing white space keeps the extension well-formed.
  const std::string over_cap = s.extended + std::string((16U << 20U) + 1 - s.extended.size(), ' ');
  EXPECT_EQ(merge(s.standard, { part(s.guid, static_cast<std::uint32_t>(over_cap.size()), 0, over_cap) }), s.standard)
    << "a full length over 16 MiB";
  EXPECT_EQ(merge(s.standard, { part(s.guid, 10, 0, "<x:xmpmeta") }), s.standard) << "an extension that does not parse";
}

// Every description of the extension carries the bindings in scope, so long
// namespace URIs times many descriptions would multiply the 16 MiB input.
TEST(JpegMetadataSegments, ExtendedXmpWithAnOversizedNamespaceUriIsIgnored)
{
  const std::string guid(32, 'D');
  const std::string extension = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                                R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#")"
                                R"( xmlns:big="http://example.org/)"
                                + std::string(600, 'u')
                                + R"(/"><rdf:Description rdf:about=""><big:Note>x</big:Note>)"
                                  R"(</rdf:Description></rdf:RDF></x:xmpmeta>)";
  const std::string standard = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">)"
                               R"(<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">)"
                               R"(<rdf:Description rdf:about="" xmlns:xmpNote="http://ns.adobe.com/xmp/note/")"
                               R"( xmpNote:HasExtendedXMP=")"
                               + guid + R"("/></rdf:RDF></x:xmpmeta>)";
  EXPECT_EQ(merge(standard, { part(guid, static_cast<std::uint32_t>(extension.size()), 0, extension) }), standard);
}

TEST(JpegMetadataSegments, XmpThatCannotBeSplitIsLeftOutWithAWarning)
{
  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(kSource).has_value());
  img.set_xmp(std::make_shared<Sipi::Xmp>("<x:xmpmeta>" + std::string(70000, 'x')));
  img.set_iptc(iptc_copyright());
  const std::string path = scratch("unsplittable_xmp.jpg");
  const std::string log = write_jpeg_logged(img, path);
  EXPECT_NE(log.find("JPEG written without its XMP packet (70011 bytes)"), std::string::npos) << log;

  EXPECT_FALSE(has_app1(path, std::string("http://ns.adobe.com/xap/1.0/\0", 29)));
  EXPECT_FALSE(has_app1(path, std::string("http://ns.adobe.com/xmp/extension/\0", 35)));
  const auto image = exiv2_read(path);
  EXPECT_EQ(image->iptcData()["Iptc.Application2.Copyright"].toString(), "DaSCH");
  Sipi::SipiImage reread;
  ASSERT_TRUE(reread.read(path).has_value());
  EXPECT_EQ(reread.getNx(), img.getNx());
}

TEST(JpegMetadataSegments, LargeIptcContinuesInFurtherApp13Segments)
{
  Exiv2::IptcData data;
  data["Iptc.Application2.Copyright"] = "DaSCH";
  for (int i = 0; i < 1200; ++i) {
    Exiv2::StringValue keyword("keyword-" + std::to_string(i) + std::string(48, 'k'));
    data.add(Exiv2::IptcKey("Iptc.Application2.Keywords"), &keyword);
  }
  const auto raw = Exiv2::IptcParser::encode(data);
  ASSERT_GT(raw.size(), 65533U);
  auto iptc = Sipi::Iptc::parse(raw.c_data(), static_cast<unsigned int>(raw.size()));
  ASSERT_TRUE(iptc.has_value());

  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(kSource).has_value());
  img.set_iptc(*iptc);
  const std::string path = scratch("large_iptc.jpg");
  ASSERT_TRUE(img.write("jpg", path).has_value());

  int app13 = 0;
  for (const auto &s : segments(file_bytes(path))) {
    if (s.marker == 0xed) {
      ++app13;
      EXPECT_TRUE(starts_with(s.payload, std::string("Photoshop 3.0\0", 14)));
    }
  }
  EXPECT_GE(app13, 2);

  const auto image = exiv2_read(path);
  EXPECT_EQ(image->iptcData()["Iptc.Application2.Copyright"].toString(), "DaSCH");
  EXPECT_EQ(image->iptcData().size(), data.size());

  Sipi::SipiImage reread;
  ASSERT_TRUE(reread.read(path).has_value());
  ASSERT_NE(reread.getIptc(), nullptr);
  const auto bytes = reread.getIptc()->iptcBytes();
  Exiv2::IptcData back;
  ASSERT_EQ(Exiv2::IptcParser::decode(back, bytes.data(), bytes.size()), 0);
  EXPECT_EQ(back["Iptc.Application2.Copyright"].toString(), "DaSCH");
  EXPECT_EQ(back.size(), data.size());
}

// EXIF as a file carries it, without the 64 KB fitting Exiv2's JPEG encoder applies.
std::shared_ptr<Sipi::Exif> exif_of(Exiv2::ExifData data)
{
  Exiv2::IptcData iptc;
  Exiv2::XmpData xmp;
  Exiv2::MemIo io;
  Exiv2::TiffParser::encode(io, nullptr, 0, Exiv2::littleEndian, data, iptc, xmp);
  auto exif = Sipi::Exif::parse(io.mmap(), static_cast<unsigned int>(io.size()));
  EXPECT_TRUE(exif.has_value());
  return exif.value_or(nullptr);
}

TEST(JpegMetadataSegments, OversizedExifDropsItsPreviewsToFit)
{
  std::vector<Exiv2::byte> thumbnail = { 0xff, 0xd8 };
  thumbnail.insert(thumbnail.end(), 70000, 0x55);
  thumbnail.insert(thumbnail.end(), { 0xff, 0xd9 });
  Exiv2::ExifData data;
  data["Exif.Image.Copyright"] = "DaSCH";
  Exiv2::ExifThumb(data).setJpegThumbnail(thumbnail.data(), thumbnail.size());
  auto exif = exif_of(data);
  ASSERT_NE(exif, nullptr);

  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(kSource).has_value());
  img.set_exif(exif);
  const std::string path = scratch("oversized_exif.jpg");
  ASSERT_TRUE(img.write("jpg", path).has_value());

  // The image's own EXIF keeps the thumbnail for other outputs.
  std::uint32_t offset = 0;
  EXPECT_TRUE(img.getExif()->getValByKey("Exif.Thumbnail.JPEGInterchangeFormat", offset));

  const auto image = exiv2_read(path);
  EXPECT_EQ(image->exifData()["Exif.Image.Copyright"].toString(), "DaSCH");
  EXPECT_EQ(image->exifData().findKey(Exiv2::ExifKey("Exif.Thumbnail.JPEGInterchangeFormat")), image->exifData().end());
}

TEST(JpegMetadataSegments, ExifThatDoesNotFitWithoutItsPreviewsIsLeftOutWithAWarning)
{
  Exiv2::ExifData data;
  data["Exif.Image.Copyright"] = "DaSCH";
  data["Exif.Image.ImageDescription"] = std::string(70000, 'd');
  auto exif = exif_of(data);
  ASSERT_NE(exif, nullptr);

  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(kSource).has_value());
  img.set_exif(exif);
  img.set_iptc(iptc_copyright());
  const std::string path = scratch("unfitting_exif.jpg");
  const std::string log = write_jpeg_logged(img, path);
  EXPECT_NE(log.find("JPEG written without its EXIF block: EXIF does not fit"), std::string::npos) << log;

  EXPECT_FALSE(has_app1(path, std::string("Exif\0\0", 6)));
  const auto image = exiv2_read(path);
  EXPECT_EQ(image->iptcData()["Iptc.Application2.Copyright"].toString(), "DaSCH");
  Sipi::SipiImage reread;
  ASSERT_TRUE(reread.read(path).has_value());
  EXPECT_EQ(reread.getNx(), img.getNx());

  // Formats without the segment limit keep it.
  const std::string png = scratch("unfitting_exif.png");
  ASSERT_TRUE(img.write("png", png).has_value());
  EXPECT_EQ(exiv2_read(png)->exifData()["Exif.Image.Copyright"].toString(), "DaSCH");
}

TEST(JpegMetadataSegments, IptcBlockThatDoesNotParseFailsTheReadNamingTheFile)
{
  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(kSource).has_value());
  const std::string plain = scratch("garbage_iptc_source.jpg");
  ASSERT_TRUE(img.write("jpg", plain).has_value());

  // An APP13 Photoshop 3.0 segment with one 8BIM IPTC resource (0x0404) whose
  // dataset claims an extended length of 0x7fff length bytes, which
  // IptcParser::decode rejects with a non-zero code.
  const std::vector<unsigned char> iptc{ 0x1c, 0x02, 0x05, 0xff, 0xff, 0x00, 0x00, 0x00 };
  std::vector<unsigned char> payload{ 'P', 'h', 'o', 't', 'o', 's', 'h', 'o', 'p', ' ', '3', '.', '0', 0 };
  payload.insert(payload.end(), { '8', 'B', 'I', 'M', 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 });
  payload.push_back(static_cast<unsigned char>(iptc.size()));
  payload.insert(payload.end(), iptc.begin(), iptc.end());
  const std::size_t seglen = payload.size() + 2;
  std::vector<unsigned char> segment{
    0xff, 0xed, static_cast<unsigned char>(seglen >> 8), static_cast<unsigned char>(seglen & 0xff)
  };
  segment.insert(segment.end(), payload.begin(), payload.end());

  auto jpeg = file_bytes(plain);
  ASSERT_GE(jpeg.size(), 2U);
  jpeg.insert(jpeg.begin() + 2, segment.begin(), segment.end());
  const std::string path = scratch("garbage_iptc.jpg");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  }

  Sipi::SipiImage reread;
  const auto r = reread.read(path);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), Sipi::ErrorCode::kMetadataParseFailed);
  const std::string &msg = r.error().raw_message();
  const std::string prefix = path + ": IPTC block does not parse: Exiv2 rejected the block (decode code ";
  EXPECT_EQ(msg.rfind(prefix, 0), 0U) << msg;
  EXPECT_TRUE(msg.ends_with(")")) << msg;
}

}// namespace
