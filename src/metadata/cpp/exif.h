/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * This file implements the virtual abstract class which implements the image file I/O.
 */
#ifndef SIPI_METADATA_EXIF_H
#define SIPI_METADATA_EXIF_H


#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <exiv2/error.hpp>
#include <exiv2/exif.hpp>
#include <exiv2/tags.hpp>
#include <exiv2/types.hpp>
#include <exiv2/value.hpp>

#include "error/SipiError.h"
#include "error/SipiValueError.h"

namespace Sipi {

/**
 * @class Exif
 * @author Lukas Rosenthaler
 * @version 0.1
 *
 * This class handles Exif metadata. It uses the Exiv2 library
 *
 * There is a problem that the libtiff libraray accesses the EXIF data tag by tag and is not able
 * to pass or get the the EXIF data as a blob. All other libraries pass EXIF data as a blob that
 * can be handled by exiv2. Therefore there are methods to get/add EXIF-data tagwise.
 * A list of all valid EXIF tags can be found at http://www.exiv2.org/tags.html .
 */
class Exif
{
private:
  std::vector<unsigned char> binaryExif;//!< Cached binary EXIF blob (kept for re-encoding)
  Exiv2::ExifData exifData;//!< Private member variable holding the exiv2 EXIF data
  Exiv2::ByteOrder byteorder;//!< Private member holding the byteorder of the EXIF data

  // Type-dispatched assignment helpers backing the inline `getValByKey<T>`
  // templates below. Definitions live in Exif.cpp so the 22 inline bodies
  // don't have to be parsed by every TU that includes this header (DEV-6407).
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::string &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<std::string> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, char &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<char> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, unsigned char &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<unsigned char> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, short &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<short> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, unsigned short &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<unsigned short> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, int &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<int> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, unsigned int &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<unsigned int> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, float &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<float> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, double &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<double> &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, Exiv2::Rational &val);
  static bool assign_val(Exiv2::Value::UniquePtr &v, std::vector<Exiv2::Rational> &val);

  explicit Exif(Exiv2::ExifData data, Exiv2::ByteOrder order, std::vector<unsigned char> binary);

public:
  /*!
   * Constructor (default)
   */
  Exif();

  /*!
   * Parses a buffer of EXIF data in native format.
   *
   * \param[in] exif Buffer containing the EXIF data
   * \param[in] len Length of the EXIF buffer
   * \return The parsed Exif instance, or a SipiValueError if the buffer does not hold valid EXIF data.
   */
  [[nodiscard]] static Result<std::shared_ptr<Exif>> parse(const unsigned char *exif, unsigned int len);

  /*!
   * Returns the bytes of the EXIF data as a vector.
   *
   * @return Vector with EXIF data
   */
  std::vector<unsigned char> exifBytes();

  /*!
   * The EXIF data encoded in at most `limit` bytes. When it does not fit, it is
   * encoded again without the embedded previews (logged); this EXIF keeps
   * them. kWriteFailed if it still does not fit, if Exiv2 would drop any other
   * field to make it fit, or if Exiv2 fails to encode it; the caller decides
   * what that means (the JPEG writer leaves the EXIF out with a warning).
   */
  [[nodiscard]] Result<std::vector<unsigned char>> exifBytes(std::size_t limit) const;

  /*!
   * Helper function to convert a signed float to a signed rational as used by EXIF
   *
   * \param[in] f Input signed float
   * \returns Exiv2::Rational
   */
  static Exiv2::Rational toRational(float f);

  /*!
   * Helper function to convert a unsigned float to a unsigned rational as used by EXIF
   *
   * \param[in] f Input unsigned float
   * \returns Exiv2::URational
   */
  static Exiv2::URational toURational(float f);

  /// Assigns through ExifData::operator[]: replaces the first entry for the key only.
  template<class T> void addKeyVal(const std::string &key_p, const T &val) { exifData[key_p] = val; }

  /// Replaces every existing entry for the key; an unsupported type or a read that throws leaves them untouched.
  template<class T> void addKeyVal(uint16_t tag, const std::string &groupName, const T &val)
  {
    Exiv2::ExifKey key = Exiv2::ExifKey(tag, groupName);
    Exiv2::Value::UniquePtr v;
    if constexpr (std::is_same_v<T, std::string>) {
      // A string is not a POD to be read through `&val, sizeof(T)`: that
      // stores the 24-byte std::string object itself as the tag value.
      v = Exiv2::Value::create(Exiv2::asciiString);
      v->read(val);
      for (auto pos = exifData.findKey(key); pos != exifData.end(); pos = exifData.findKey(key)) { exifData.erase(pos); }
      exifData.add(key, v.get());
      return;
    }
    if (typeid(T) == typeid(int8_t)) {
      v = Exiv2::Value::create(Exiv2::signedByte);
    } else if (typeid(T) == typeid(uint8_t)) {
      v = Exiv2::Value::create(Exiv2::unsignedByte);
    } else if (typeid(T) == typeid(int16_t)) {
      v = Exiv2::Value::create(Exiv2::signedShort);
    } else if (typeid(T) == typeid(uint16_t)) {
      v = Exiv2::Value::create(Exiv2::unsignedShort);
    } else if (typeid(T) == typeid(int32_t)) {
      v = Exiv2::Value::create(Exiv2::signedLong);
    } else if (typeid(T) == typeid(uint32_t)) {
      v = Exiv2::Value::create(Exiv2::unsignedLong);
    } else if (typeid(T) == typeid(float)) {
      v = Exiv2::Value::create(Exiv2::tiffFloat);
    } else if (typeid(T) == typeid(double)) {
      v = Exiv2::Value::create(Exiv2::tiffDouble);
    } else if (typeid(T) == typeid(Exiv2::Rational)) {
      v = Exiv2::Value::create(Exiv2::signedRational);
    } else if (typeid(T) == typeid(Exiv2::URational)) {
      v = Exiv2::Value::create(Exiv2::unsignedRational);
    } else {
      throw SipiError("Unsupported type of addKeyVal(2)");
    }

    v->read((unsigned char *)&val, sizeof(T), Exiv2::littleEndian);
    for (auto pos = exifData.findKey(key); pos != exifData.end(); pos = exifData.findKey(key)) { exifData.erase(pos); }
    exifData.add(key, v.get());
  }

  /// Replaces every existing entry for the key; an unsupported type or a read that throws leaves them untouched.
  template<class T> void addKeyVal(uint16_t tag, const std::string &groupName, const T *valptr, size_t len)
  {
    Exiv2::ExifKey key = Exiv2::ExifKey(tag, groupName);
    Exiv2::Value::UniquePtr v;
    if (typeid(T) == typeid(int8_t)) {
      v = Exiv2::Value::create(Exiv2::signedByte);
    } else if (typeid(T) == typeid(uint8_t)) {
      v = Exiv2::Value::create(Exiv2::unsignedByte);
    } else if (typeid(T) == typeid(int16_t)) {
      v = Exiv2::Value::create(Exiv2::signedShort);
    } else if (typeid(T) == typeid(uint16_t)) {
      v = Exiv2::Value::create(Exiv2::unsignedShort);
    } else if (typeid(T) == typeid(int32_t)) {
      v = Exiv2::Value::create(Exiv2::signedLong);
    } else if (typeid(T) == typeid(uint32_t)) {
      v = Exiv2::Value::create(Exiv2::unsignedLong);
    } else if (typeid(T) == typeid(float)) {
      v = Exiv2::Value::create(Exiv2::tiffFloat);
    } else if (typeid(T) == typeid(double)) {
      v = Exiv2::Value::create(Exiv2::tiffDouble);
    } else if (typeid(T) == typeid(Exiv2::Rational)) {
      v = Exiv2::Value::create(Exiv2::signedRational);
    } else if (typeid(T) == typeid(Exiv2::URational)) {
      v = Exiv2::Value::create(Exiv2::unsignedRational);
    } else {
      throw SipiError("Unsupported type of addKeyVal(2)");
    }
    v->read((unsigned char *)valptr, static_cast<long>(len * sizeof(T)), Exiv2::littleEndian);
    for (auto pos = exifData.findKey(key); pos != exifData.end(); pos = exifData.findKey(key)) { exifData.erase(pos); }
    exifData.add(key, v.get());
  }

  //............................................................................
  // Getting values from the EXIF object
  //

  //____________________________________________________________________________
  // string values
  //
  template<class T> bool getValByKey(const std::string &key_p, T &val)
  {
    try {
      Exiv2::ExifKey key = Exiv2::ExifKey(key_p);
      auto pos = exifData.findKey(key);
      if (pos == exifData.end()) { return false; }
      auto v = pos->getValue();
      return assign_val(v, val);
    } catch (const Exiv2::Error &err) {
      return false;
    }
  }

  template<class T> bool getValByKey(uint16_t tag, const std::string &groupName, T &val)
  {
    try {
      Exiv2::ExifKey key = Exiv2::ExifKey(tag, groupName);
      auto pos = exifData.findKey(key);
      if (pos == exifData.end()) { return false; }
      auto v = pos->getValue();
      return assign_val(v, val);

    } catch (const Exiv2::Error &err) {
      return false;
    }
  }

  /// Sets the pixel-size tags that are present to w x h, keeping each tag's type; adds none:
  /// PixelX/YDimension, ImageWidth/ImageLength/ImageHeight in any IFD (maker notes
  /// included), RelatedImageWidth/Length, DefaultCropSize and the DNG Original*Size
  /// tags (w h), ActiveArea (0 0 h w). False if a tag's type cannot hold its value,
  /// which then keeps the old one.
  [[nodiscard]] bool set_pixel_dimensions(std::size_t w, std::size_t h);

  /// Removes every embedded preview image Exiv2 exposes, with its offset and size
  /// tags: the IFD1 thumbnail, the SubIFD and IFD2/IFD3 images, and the maker-note
  /// previews Exiv2's PreviewManager reads.
  void erase_previews();

  /// Gives the Photoshop image resources IFD0 can carry (Exif.Image.ImageResources)
  /// the same treatment: drops the thumbnail resources, and the nested XMP and EXIF
  /// go through erase_thumbnails/erase_previews and set_pixel_dimensions. Every
  /// other resource stays byte for byte. False if the block cannot be parsed or
  /// holds a native size this does not rewrite (slices, the legacy header).
  /// The IPTC resource and the raw XMP and IPTC copies (XMLPacket, IPTCNAA)
  /// need nothing: exifBytes() never writes them. The copies reach an output
  /// only as the XMP and IPTC SipiImage::promote_ifd0_copies() makes of them.
  [[nodiscard]] bool restrict_image_resources(std::size_t w, std::size_t h);

  /// The value bytes of the field `key` in this EXIF's byte order (the bytes
  /// the file holds), or nullopt if the field is absent.
  [[nodiscard]] std::optional<std::vector<unsigned char>> value_bytes(const std::string &key) const;

  friend std::ostream &operator<<(std::ostream &lhs, Exif &rhs);
};

}// namespace Sipi

#endif
