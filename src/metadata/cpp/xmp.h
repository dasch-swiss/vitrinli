/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef SIPI_METADATA_XMP_H
#define SIPI_METADATA_XMP_H

#include <cstddef>
#include <iosfwd>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "error/SipiValueError.h"

namespace Sipi {

typedef struct
{
  std::mutex lock;
} XmpMutex;

extern XmpMutex xmp_mutex;

extern void xmplock_func(void *pLockData, bool lockUnlock);

/*!
 * This class holds an XMP metadata packet. SIPI stores the RDF/XML payload
 * verbatim, neither parsing nor validating it, because Exiv2's XMP parser
 * is not thread-safe at the Exiv2 version SIPI depends on. The only edits are
 * set_pixel_dimensions() and erase_thumbnails(), text substitutions a
 * size-restricted output applies. split_extended() and merge_extended() read
 * packets with expat (one parser per call, so thread-safe): the first cuts a
 * packet into StandardXMP and ExtendedXMP for the JPEG writer, the second
 * joins them again for the JPEG reader.
 */
class Xmp
{
private:
  std::string __xmpstr;

public:
  /*!
   * Constructor
   *
   * \param[in] xmp A std::string containing RDF/XML with XMP data
   */
  Xmp(const std::string &xmp);

  /*!
   * Constructor
   *
   * \param[in] xmp A C-string (char *)containing RDF/XML with XMP data
   */
  Xmp(const char *xmp);

  /*!
   * Constructor
   *
   * \param[in] xmp A string containing RDF/XML with XMP data
   * \param[in] len Length of the string
   */
  Xmp(const char *xmp, int len);


  /*!
   * Destructor
   */
  ~Xmp();


  /*!
   * Returns the bytes of the RDF/XML data as a std::string.
   *
   * @return String holding the xmp data
   */
  std::string xmpBytes();

  /*!
   * Sets exif:PixelXDimension/PixelYDimension, tiff:ImageWidth/ImageLength and
   * Iptc4xmpExt:MaxAvailWidth/MaxAvailHeight, in attribute or element form and
   * under any prefix the packet binds to their namespace, to w x h where the
   * value is an integer. Every other byte of the packet is left as it is.
   */
  void set_pixel_dimensions(std::size_t w, std::size_t h);

  /*!
   * Removes every xmp:Thumbnails property (the embedded preview images, with
   * their xmpGImg size fields) and every other xmpGImg:image, under any prefix
   * the packet binds to their namespace. False if an element has no end or a
   * Thumbnails element or GImg image is left that the scan cannot attribute.
   */
  [[nodiscard]] bool erase_thumbnails();

  /// A packet split into StandardXMP and ExtendedXMP (XMP Specification Part 3,
  /// 1.1.3.1), for a container that holds at most `limit` bytes of it in one piece.
  struct Extended
  {
    std::string standard;//!< The packet without the moved properties, plus xmpNote:HasExtendedXMP
    std::string extended;//!< The moved properties as one serialization
    std::string guid;//!< MD5 of `extended`, 32 upper-case hex digits
  };

  /*!
   * Moves whole top-level properties, largest first and rights properties
   * last, out of the packet until the rest fits in `limit` bytes. Every
   * property keeps its bytes. kWriteFailed if the packet is not well-formed
   * UTF-8 XML, has a DOCTYPE, binds more than 64 namespaces in scope, nests
   * elements more than 256 deep, has more than one rdf:RDF or more than 256
   * top-level rdf:Description elements, is over 2 GB, does not fit even with
   * every property moved, or would need an ExtendedXMP over 16 MiB; the caller decides what that means
   * (the JPEG writer leaves the packet out with a warning).
   */
  [[nodiscard]] Result<Extended> split_extended(std::size_t limit) const;

  /*!
   * The packet a JPEG holds as `standard`, a StandardXMP, and `parts`, the
   * payloads of its ExtendedXMP segments after their
   * "http://ns.adobe.com/xmp/extension/\0" signature (GUID, full length,
   * offset, bytes). The parts whose GUID is the xmpNote:HasExtendedXMP of
   * `standard` are joined, their rdf:Description elements are added to
   * `standard`, and xmpNote:HasExtendedXMP is removed. `standard` comes back
   * unchanged, with a debug log, if a part is missing or overlaps another,
   * the parts disagree on the full length or declare more than 16 MiB, or
   * either packet does not parse as split_extended() requires.
   */
  [[nodiscard]] static std::string merge_extended(std::string_view standard, std::span<const std::string_view> parts);
};

/*!
 * Lets an Xmp appear in SipiImage's stream output. It emits nothing: SIPI
 * holds the XMP packet as an opaque RDF/XML byte string with no parsed
 * structure to format.
 *
 * \param[in] lhs The output stream
 * \param[in] rhs Reference to an instance of a Xmp
 * \returns Returns ostream object
 */
std::ostream &operator<<(std::ostream &lhs, const Xmp &rhs);

}// namespace Sipi

#endif
