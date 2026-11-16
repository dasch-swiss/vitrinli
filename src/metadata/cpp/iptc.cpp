/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "metadata/iptc.h"

#include <cstddef>
#include <string>

#include <exiv2/error.hpp>

namespace Sipi {

Iptc::Iptc(Exiv2::IptcData data) : iptcData(std::move(data)) {}
//============================================================================

Result<std::shared_ptr<Iptc>> Iptc::parse(const unsigned char *iptc, unsigned int len)
{
  Exiv2::IptcData data;
  if (Exiv2::IptcParser::decode(data, iptc, (uint32_t)len) != 0) {
    return std::unexpected(SipiValueError{ ErrorCode::kMetadataParseFailed, "No valid IPTC data!" });
  }
  return std::shared_ptr<Iptc>(new Iptc(std::move(data)));
}
//============================================================================

Result<std::shared_ptr<Iptc>> Iptc::parse_complete(const unsigned char *iptc, unsigned int len)
{
  const auto fail = [](const std::string &why) {
    return std::unexpected(SipiValueError{ ErrorCode::kMetadataParseFailed, "IPTC block does not parse: " + why });
  };
  // Each dataset: marker 0x1c, record, number, a 2-byte length, the data. A
  // length with its top bit set gives the count of length bytes that follow.
  std::size_t end = len;
  while (end > 0 && iptc[end - 1] == 0) { --end; }
  std::size_t datasets = 0;
  for (std::size_t pos = 0; pos < end; ++datasets) {
    if (len - pos < 5 || iptc[pos] != 0x1c) { return fail("bytes outside its datasets"); }
    std::size_t size = (std::size_t{ iptc[pos + 3] } << 8) | iptc[pos + 4];
    std::size_t header = 5;
    if ((size & 0x8000) != 0) {
      const std::size_t n = size & 0x7fff;
      if (n > 4 || len - pos - header < n) { return fail("a dataset length past the end"); }
      size = 0;
      for (std::size_t i = 0; i < n; ++i) { size = (size << 8) | iptc[pos + header + i]; }
      header += n;
    }
    if (len - pos - header < size) { return fail("a dataset past the end"); }
    pos += header + size;
  }
  if (datasets == 0) { return fail("no dataset"); }
  try {
    auto parsed = parse(iptc, len);
    if (parsed && (*parsed)->iptcData.count() != datasets) { return fail("a dataset Exiv2 does not read"); }
    return parsed;
  } catch (const Exiv2::Error &err) {
    return fail(err.what());
  }
}
//============================================================================

Iptc::~Iptc() {}


std::vector<unsigned char> Iptc::iptcBytes()
{
  Exiv2::DataBuf databuf = Exiv2::IptcParser::encode(iptcData);
  if (databuf.size() == 0) return {};
  return std::vector<unsigned char>(databuf.data(), databuf.data() + databuf.size());
}
//============================================================================

void Iptc::erase_preview()
{
  for (auto it = iptcData.begin(); it != iptcData.end();) {
    const std::string key = it->key();
    if (key == "Iptc.Application2.Preview" || key == "Iptc.Application2.PreviewFormat"
        || key == "Iptc.Application2.PreviewVersion") {
      it = iptcData.erase(it);
    } else {
      ++it;
    }
  }
}
//============================================================================

std::ostream &operator<<(std::ostream &outstr, Iptc &rhs)
{
  Exiv2::IptcData::iterator end = rhs.iptcData.end();
  for (Exiv2::IptcData::iterator md = rhs.iptcData.begin(); md != end; ++md) {
    outstr << std::setw(44) << std::setfill(' ') << std::left << md->key() << " "
           << "0x" << std::setw(4) << std::setfill('0') << std::right << std::hex << md->tag() << " " << std::setw(9)
           << std::setfill(' ') << std::left << md->typeName() << " " << std::dec << std::setw(3) << std::setfill(' ')
           << std::right << md->count() << "  " << std::dec << md->value() << std::endl;
  }
  return outstr;
}
//============================================================================

}// namespace Sipi
