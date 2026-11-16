/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <tiffio.h>

#include "error/SipiValueError.h"
#include "format_handlers/SipiIOTiff.h"
#include "image/SipiImage.h"
#include "metadata/exif.h"
#include "test_paths.h"

namespace {

std::string unit_path(const char *name) { return sipi::test::data_dir() + "/images/unit/" + name; }

}// namespace

// A camera-authored EXIF rational whose value is unmeasured is encoded as
// 0/0 (e.g. BrightnessValue on `image_orientation.jpg`). Writing that
// straight through as `num/den` produces NaN, which libtiff's
// DoubleToSrational then truncates with `(int32_t)NaN` — undefined
// behaviour. `SipiIOTiff::writeExif` must skip a 0/0 tag rather than write
// it, while unaffected rationals still round-trip.
TEST(TiffExifRational, ZeroOverZeroBrightnessValueIsSkippedOnWrite)
{
  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  const std::string src = unit_path("image_orientation.jpg");
  ASSERT_TRUE(img.read(src).has_value());
  auto exif = img.getExif();
  ASSERT_NE(exif, nullptr);

  // Confirm what the fixture actually carries before relying on it.
  Exiv2::Rational brightness_src;
  ASSERT_TRUE(exif->getValByKey(EXIFTAG_BRIGHTNESSVALUE, "Photo", brightness_src));
  EXPECT_EQ(brightness_src.first, 0);
  EXPECT_EQ(brightness_src.second, 0);

  Exiv2::Rational fnumber_src;
  ASSERT_TRUE(exif->getValByKey(EXIFTAG_FNUMBER, "Photo", fnumber_src));
  Exiv2::Rational exposure_src;
  ASSERT_TRUE(exif->getValByKey(EXIFTAG_EXPOSURETIME, "Photo", exposure_src));

  const std::string dst = sipi::test::tmp_dir() + "/_tiff_exif_rational_zero_over_zero.tif";
  const auto write_result = img.write("tif", dst);
  ASSERT_TRUE(write_result.has_value());

  Sipi::SipiImage rt;
  ASSERT_TRUE(rt.read(dst).has_value());
  auto rt_exif = rt.getExif();
  ASSERT_NE(rt_exif, nullptr);

  Exiv2::Rational brightness_rt;
  EXPECT_FALSE(rt_exif->getValByKey(EXIFTAG_BRIGHTNESSVALUE, "Photo", brightness_rt))
    << "0/0 BrightnessValue must not survive the write (would have gone through TIFFSetField as NaN)";

  Exiv2::Rational fnumber_rt;
  ASSERT_TRUE(rt_exif->getValByKey(EXIFTAG_FNUMBER, "Photo", fnumber_rt));
  EXPECT_NEAR(static_cast<double>(fnumber_rt.first) / fnumber_rt.second,
    static_cast<double>(fnumber_src.first) / fnumber_src.second,
    1e-4);

  Exiv2::Rational exposure_rt;
  ASSERT_TRUE(rt_exif->getValByKey(EXIFTAG_EXPOSURETIME, "Photo", exposure_rt));
  EXPECT_NEAR(static_cast<double>(exposure_rt.first) / exposure_rt.second,
    static_cast<double>(exposure_src.first) / exposure_src.second,
    1e-4);

  std::remove(dst.c_str());
}

// A rational with a non-zero numerator and a zero denominator has no valid
// float representation and is malformed, not "no value". SIPI is a
// repository: malformed metadata is fatal.
TEST(TiffExifRational, NonZeroOverZeroRationalFailsWrite)
{
  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(unit_path("image_orientation.jpg")).has_value());

  auto exif = img.getExif();
  ASSERT_NE(exif, nullptr);
  exif->addKeyVal(EXIFTAG_BRIGHTNESSVALUE, "Photo", Exiv2::Rational{ 1, 0 });

  const std::string dst = sipi::test::tmp_dir() + "/_tiff_exif_rational_n_over_zero.tif";
  const auto write_result = img.write("tif", dst);
  ASSERT_FALSE(write_result.has_value());
  EXPECT_EQ(write_result.error().code(), Sipi::ErrorCode::kMetadataParseFailed);
  std::remove(dst.c_str());
}

// libtiff declares EXIFTAG_FLASHENERGY as a scalar (TIFF_SETGET_FLOAT),
// matching its EXIF_DT_RATIONAL entry in exiftag_list.
TEST(TiffExifRational, FlashEnergyRoundTrips)
{
  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(unit_path("image_orientation.jpg")).has_value());

  auto exif = std::make_shared<Sipi::Exif>();
  exif->addKeyVal(EXIFTAG_FLASHENERGY, "Photo", Exiv2::Rational{ 5, 2 });
  img.set_exif(exif);

  const std::string dst = sipi::test::tmp_dir() + "/_tiff_exif_rational_flash_energy.tif";
  const auto write_result = img.write("tif", dst);
  ASSERT_TRUE(write_result.has_value());

  Sipi::SipiImage rt;
  ASSERT_TRUE(rt.read(dst).has_value());
  auto rt_exif = rt.getExif();
  ASSERT_NE(rt_exif, nullptr);

  Exiv2::Rational flash_energy_rt;
  ASSERT_TRUE(rt_exif->getValByKey(EXIFTAG_FLASHENERGY, "Photo", flash_energy_rt));
  EXPECT_NEAR(static_cast<double>(flash_energy_rt.first) / flash_energy_rt.second, 2.5, 1e-4);

  std::remove(dst.c_str());
}

// LensSpecification is a fixed-count RATIONAL[4] tag. Cameras routinely emit
// the trailing two elements as 0/0 ("unknown") while the first two are real
// values; because libtiff writes the whole array in one TIFFSetField call,
// the tag must be skipped in its entirety rather than partially written.
TEST(TiffExifRational, LensSpecificationAllUnknownDenominatorsSkipsWholeTag)
{
  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(unit_path("image_orientation.jpg")).has_value());

  auto exif = std::make_shared<Sipi::Exif>();
  const Exiv2::Rational lens_spec[4] = {
    Exiv2::Rational{ 33, 10 }, Exiv2::Rational{ 33, 10 }, Exiv2::Rational{ 0, 0 }, Exiv2::Rational{ 0, 0 }
  };
  exif->addKeyVal(EXIFTAG_LENSSPECIFICATION, "Photo", lens_spec, 4);
  img.set_exif(exif);

  const std::string dst = sipi::test::tmp_dir() + "/_tiff_exif_rational_lensspec_unknown.tif";
  const auto write_result = img.write("tif", dst);
  ASSERT_TRUE(write_result.has_value());

  Sipi::SipiImage rt;
  ASSERT_TRUE(rt.read(dst).has_value());
  auto rt_exif = rt.getExif();
  ASSERT_NE(rt_exif, nullptr);

  std::vector<Exiv2::Rational> lens_spec_rt;
  EXPECT_FALSE(rt_exif->getValByKey(EXIFTAG_LENSSPECIFICATION, "Photo", lens_spec_rt))
    << "a LensSpecification with any 0/0 element must not survive the write";

  std::remove(dst.c_str());
}

// A malformed element (non-zero numerator, zero denominator) anywhere in a
// fixed-count rational array rejects the whole write, regardless of the
// other elements' validity.
TEST(TiffExifRational, LensSpecificationMalformedElementFailsWrite)
{
  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(unit_path("image_orientation.jpg")).has_value());

  auto exif = std::make_shared<Sipi::Exif>();
  const Exiv2::Rational lens_spec[4] = {
    Exiv2::Rational{ 33, 10 }, Exiv2::Rational{ 33, 10 }, Exiv2::Rational{ 22, 0 }, Exiv2::Rational{ 22, 10 }
  };
  exif->addKeyVal(EXIFTAG_LENSSPECIFICATION, "Photo", lens_spec, 4);
  img.set_exif(exif);

  const std::string dst = sipi::test::tmp_dir() + "/_tiff_exif_rational_lensspec_malformed.tif";
  const auto write_result = img.write("tif", dst);
  ASSERT_FALSE(write_result.has_value());
  EXPECT_EQ(write_result.error().code(), Sipi::ErrorCode::kMetadataParseFailed);
  std::remove(dst.c_str());
}

// XResolution and YResolution are written from their own source tags; distinct
// values make a swapped or duplicated tag visible.
TEST(TiffExifRational, DistinctXAndYResolutionRoundTrip)
{
  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  ASSERT_TRUE(img.read(unit_path("image_orientation.jpg")).has_value());

  auto exif = std::make_shared<Sipi::Exif>();
  exif->addKeyVal(std::string("Exif.Image.XResolution"), Exiv2::Rational{ 300, 1 });
  exif->addKeyVal(std::string("Exif.Image.YResolution"), Exiv2::Rational{ 150, 1 });
  img.set_exif(exif);

  const std::string dst = sipi::test::tmp_dir() + "/_tiff_exif_rational_xy_resolution.tif";
  const auto write_result = img.write("tif", dst);
  ASSERT_TRUE(write_result.has_value());

  Sipi::SipiImage rt;
  ASSERT_TRUE(rt.read(dst).has_value());
  auto rt_exif = rt.getExif();
  ASSERT_NE(rt_exif, nullptr);

  Exiv2::Rational x_rt;
  ASSERT_TRUE(rt_exif->getValByKey(std::string("Exif.Image.XResolution"), x_rt));
  EXPECT_NEAR(static_cast<double>(x_rt.first) / x_rt.second, 300.0, 1e-4);

  Exiv2::Rational y_rt;
  ASSERT_TRUE(rt_exif->getValByKey(std::string("Exif.Image.YResolution"), y_rt));
  EXPECT_NEAR(static_cast<double>(y_rt.first) / y_rt.second, 150.0, 1e-4);

  std::remove(dst.c_str());
}
