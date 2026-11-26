/*
 * Copyright © 2026 Swiss National Data and Service Center for the Humanities
 * and/or DaSCH Service Platform contributors. SPDX-License-Identifier:
 * AGPL-3.0-or-later
 */

// SipiIOTiff::read decodes into the one pixel buffer it hands to set_pixels().
// The pixel cases pin the samples and their byte order for each layout the
// reader writes straight into that buffer (strips and tiles, 8 and 16 bit,
// compressed or not, with a region) and for the planar-separate path that
// still converts through its own buffer. The peak-heap case fails when a
// second full-size buffer comes back.
//
// This binary replaces the global operator new/delete to count live heap
// bytes, so it is its own cc_test and never joins formats_test. Under ASan
// that also turns off the new/delete mismatch check for this binary only.

#include "gtest/gtest.h"

#include "SipiIOTiff.h"
#include "iiifparser/SipiRegion.h"
#include "image/SipiImage.h"

#include "tiff.h"
#include "tiffio.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

std::atomic<size_t> g_live_bytes{ 0 };
std::atomic<size_t> g_peak_bytes{ 0 };

// Each block carries its size just in front of the returned pointer, in a
// header as large as the block's alignment, so the pointer keeps it.
void *counted_alloc(size_t size, size_t align)
{
  const size_t header = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
  void *raw = std::aligned_alloc(header, ((header + size + header - 1) / header) * header);
  if (raw == nullptr) { return nullptr; }
  auto *base = static_cast<unsigned char *>(raw);
  *reinterpret_cast<size_t *>(base + header - sizeof(size_t)) = size;
  const size_t live = g_live_bytes.fetch_add(size) + size;
  size_t peak = g_peak_bytes.load();
  while (live > peak && !g_peak_bytes.compare_exchange_weak(peak, live)) {}
  return base + header;
}

void counted_free(void *ptr, size_t align)
{
  if (ptr == nullptr) { return; }
  const size_t header = align < alignof(std::max_align_t) ? alignof(std::max_align_t) : align;
  auto *base = static_cast<unsigned char *>(ptr) - header;
  g_live_bytes.fetch_sub(*reinterpret_cast<size_t *>(base + header - sizeof(size_t)));
  std::free(base);
}

void *counted_alloc_or_throw(size_t size, size_t align)
{
  void *p = counted_alloc(size, align);
  if (p == nullptr) { throw std::bad_alloc(); }
  return p;
}

}// namespace

void *operator new(size_t size) { return counted_alloc_or_throw(size, alignof(std::max_align_t)); }
void *operator new[](size_t size) { return counted_alloc_or_throw(size, alignof(std::max_align_t)); }
void *operator new(size_t size, const std::nothrow_t &) noexcept
{
  return counted_alloc(size, alignof(std::max_align_t));
}
void *operator new[](size_t size, const std::nothrow_t &) noexcept
{
  return counted_alloc(size, alignof(std::max_align_t));
}
void *operator new(size_t size, std::align_val_t al) { return counted_alloc_or_throw(size, static_cast<size_t>(al)); }
void *operator new[](size_t size, std::align_val_t al) { return counted_alloc_or_throw(size, static_cast<size_t>(al)); }
void operator delete(void *p) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete[](void *p) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete(void *p, size_t) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete[](void *p, size_t) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete(void *p, const std::nothrow_t &) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { counted_free(p, alignof(std::max_align_t)); }
void operator delete(void *p, std::align_val_t al) noexcept { counted_free(p, static_cast<size_t>(al)); }
void operator delete[](void *p, std::align_val_t al) noexcept { counted_free(p, static_cast<size_t>(al)); }
void operator delete(void *p, size_t, std::align_val_t al) noexcept { counted_free(p, static_cast<size_t>(al)); }
void operator delete[](void *p, size_t, std::align_val_t al) noexcept { counted_free(p, static_cast<size_t>(al)); }

namespace {

struct TiffSpec
{
  uint32_t nx;
  uint32_t ny;
  uint16_t nc;
  uint16_t bps;
  bool tiled;
  uint16_t planar;
  uint16_t compression;
};

// 16-bit values use both bytes, so a byte-order slip changes the sample.
uint16_t sample_at(const TiffSpec &spec, uint32_t x, uint32_t y, uint32_t c)
{
  if (spec.bps == 8) { return static_cast<uint16_t>((x * 7U + y * 13U + c * 101U) & 0xffU); }
  return static_cast<uint16_t>((x * 257U + y * 131U + c * 4099U) & 0xffffU);
}

std::string scratch_path(const std::string &name)
{
  const char *dir = std::getenv("TEST_TMPDIR");
  return std::string(dir != nullptr ? dir : "/tmp") + "/" + name;
}

using TiffPtr = std::unique_ptr<TIFF, void (*)(TIFF *)>;

// Fills one buffer of `count` samples starting at (x0, y0), `width` samples
// per row, `nc_in_buf` interleaved channels starting at channel `c0`.
std::vector<unsigned char> make_samples(const TiffSpec &spec,
  uint32_t x0,
  uint32_t y0,
  uint32_t width,
  uint32_t height,
  uint32_t c0,
  uint32_t nc_in_buf)
{
  const size_t bytes = spec.bps / 8;
  std::vector<unsigned char> buf(static_cast<size_t>(width) * height * nc_in_buf * bytes, 0);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      if (x0 + x >= spec.nx || y0 + y >= spec.ny) { continue; }
      for (uint32_t c = 0; c < nc_in_buf; ++c) {
        const uint16_t v = sample_at(spec, x0 + x, y0 + y, c0 + c);
        const size_t idx = (static_cast<size_t>(y) * width + x) * nc_in_buf + c;
        if (bytes == 1) {
          buf[idx] = static_cast<unsigned char>(v);
        } else {
          reinterpret_cast<uint16_t *>(buf.data())[idx] = v;
        }
      }
    }
  }
  return buf;
}

void write_tiff(const std::string &path, const TiffSpec &spec)
{
  TiffPtr tif(TIFFOpen(path.c_str(), "w"), &TIFFClose);
  ASSERT_NE(tif, nullptr);
  TIFF *t = tif.get();
  TIFFSetField(t, TIFFTAG_IMAGEWIDTH, spec.nx);
  TIFFSetField(t, TIFFTAG_IMAGELENGTH, spec.ny);
  TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, spec.nc);
  TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, spec.bps);
  TIFFSetField(t, TIFFTAG_PHOTOMETRIC, spec.nc >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK);
  TIFFSetField(t, TIFFTAG_PLANARCONFIG, spec.planar);
  TIFFSetField(t, TIFFTAG_COMPRESSION, spec.compression);
  TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);

  if (spec.tiled) {
    constexpr uint32_t kTile = 16;
    TIFFSetField(t, TIFFTAG_TILEWIDTH, kTile);
    TIFFSetField(t, TIFFTAG_TILELENGTH, kTile);
    for (uint32_t ty = 0; ty < spec.ny; ty += kTile) {
      for (uint32_t tx = 0; tx < spec.nx; tx += kTile) {
        auto tile = make_samples(spec, tx, ty, kTile, kTile, 0, spec.nc);
        ASSERT_GE(TIFFWriteTile(t, tile.data(), tx, ty, 0, 0), 0);
      }
    }
  } else {
    TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, 8U);
    if (spec.planar == PLANARCONFIG_CONTIG) {
      for (uint32_t y = 0; y < spec.ny; ++y) {
        auto row = make_samples(spec, 0, y, spec.nx, 1, 0, spec.nc);
        ASSERT_EQ(TIFFWriteScanline(t, row.data(), y, 0), 1);
      }
    } else {
      for (uint16_t c = 0; c < spec.nc; ++c) {
        for (uint32_t y = 0; y < spec.ny; ++y) {
          auto row = make_samples(spec, 0, y, spec.nx, 1, c, 1);
          ASSERT_EQ(TIFFWriteScanline(t, row.data(), y, c), 1);
        }
      }
    }
  }
}

Sipi::ScalingQuality default_quality()
{
  return { Sipi::ScalingMethod::HIGH, Sipi::ScalingMethod::HIGH, Sipi::ScalingMethod::HIGH, Sipi::ScalingMethod::HIGH };
}

void expect_pixels(const TiffSpec &spec, const std::string &name, std::shared_ptr<Sipi::SipiRegion> region = nullptr)
{
  const std::string path = scratch_path(name);
  write_tiff(path, spec);
  if (::testing::Test::HasFatalFailure()) { return; }

  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  Sipi::SipiIOTiff io;
  auto read = io.read(&img, path, region, nullptr, false, default_quality());
  ASSERT_TRUE(read.has_value()) << read.error().diagnostic_message();
  ASSERT_TRUE(*read);

  uint32_t x0 = 0;
  uint32_t y0 = 0;
  size_t w = spec.nx;
  size_t h = spec.ny;
  if (region != nullptr) {
    int32_t rx = 0;
    int32_t ry = 0;
    region->crop_coords(spec.nx, spec.ny, rx, ry, w, h);
    x0 = static_cast<uint32_t>(rx);
    y0 = static_cast<uint32_t>(ry);
  }
  ASSERT_EQ(img.getNx(), w);
  ASSERT_EQ(img.getNy(), h);
  ASSERT_EQ(img.getNc(), spec.nc);
  ASSERT_EQ(img.getBps(), spec.bps);
  for (size_t y = 0; y < h; ++y) {
    for (size_t x = 0; x < w; ++x) {
      for (size_t c = 0; c < spec.nc; ++c) {
        const int expected = sample_at(spec, x0 + static_cast<uint32_t>(x), y0 + static_cast<uint32_t>(y), c);
        ASSERT_EQ(img.getPixel(x, y, c), expected) << name << " at (" << x << ", " << y << ", " << c << ")";
      }
    }
  }
}

}// namespace

TEST(TiffReadBuffer, Strip8BitRgb)
{
  expect_pixels({ 37, 29, 3, 8, false, PLANARCONFIG_CONTIG, COMPRESSION_NONE }, "strip8.tif");
}

TEST(TiffReadBuffer, Strip16BitRgb)
{
  expect_pixels({ 37, 29, 3, 16, false, PLANARCONFIG_CONTIG, COMPRESSION_NONE }, "strip16.tif");
}

TEST(TiffReadBuffer, StripLzw8BitRgb)
{
  expect_pixels({ 37, 29, 3, 8, false, PLANARCONFIG_CONTIG, COMPRESSION_LZW }, "strip8_lzw.tif");
}

TEST(TiffReadBuffer, StripLzw16BitGray)
{
  expect_pixels({ 37, 29, 1, 16, false, PLANARCONFIG_CONTIG, COMPRESSION_LZW }, "strip16_lzw.tif");
}

TEST(TiffReadBuffer, Tiled8BitRgb)
{
  expect_pixels({ 40, 27, 3, 8, true, PLANARCONFIG_CONTIG, COMPRESSION_NONE }, "tiled8.tif");
}

TEST(TiffReadBuffer, Tiled16BitRgb)
{
  expect_pixels({ 40, 27, 3, 16, true, PLANARCONFIG_CONTIG, COMPRESSION_NONE }, "tiled16.tif");
}

TEST(TiffReadBuffer, Strip16BitRgbRegion)
{
  expect_pixels({ 37, 29, 3, 16, false, PLANARCONFIG_CONTIG, COMPRESSION_NONE },
    "strip16_region.tif",
    std::make_shared<Sipi::SipiRegion>(5, 7, 20, 11));
}

// Tile-aligned origin: an unaligned one returns shifted pixels (DEV-8115).
TEST(TiffReadBuffer, Tiled8BitRgbRegion)
{
  expect_pixels({ 40, 27, 3, 8, true, PLANARCONFIG_CONTIG, COMPRESSION_NONE },
    "tiled8_region.tif",
    std::make_shared<Sipi::SipiRegion>(16, 16, 20, 11));
}

TEST(TiffReadBuffer, PlanarSeparate8BitRgb)
{
  expect_pixels({ 37, 29, 3, 8, false, PLANARCONFIG_SEPARATE, COMPRESSION_NONE }, "planar8.tif");
}

TEST(TiffReadBuffer, PlanarSeparateLzw16BitRgb)
{
  expect_pixels({ 37, 29, 3, 16, false, PLANARCONFIG_SEPARATE, COMPRESSION_LZW }, "planar16_lzw.tif");
}

// A 64 MiB 16-bit RGB image: the reader's own allocations at peak stay below
// 1.5 times the pixel buffer, so it never holds a second full-size copy.
// libtiff allocates with malloc, not operator new, so its strip buffers are
// not counted; the bound is on SIPI's buffers only.
TEST(TiffReadBuffer, PeakHeapStaysBelowOneAndAHalfBuffers)
{
  const TiffSpec spec{ 4096, 2731, 3, 16, false, PLANARCONFIG_CONTIG, COMPRESSION_NONE };
  const size_t buffer_bytes = static_cast<size_t>(spec.nx) * spec.ny * spec.nc * (spec.bps / 8);
  ASSERT_GE(buffer_bytes, size_t{ 64 } * 1024 * 1024);

  const std::string path = scratch_path("peak16.tif");
  write_tiff(path, spec);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());

  Sipi::SipiIOTiff::initLibrary();
  Sipi::SipiImage img;
  Sipi::SipiIOTiff io;
  const size_t live_before = g_live_bytes.load();
  g_peak_bytes.store(live_before);
  auto read = io.read(&img, path, nullptr, nullptr, false, default_quality());
  const size_t peak = g_peak_bytes.load() - live_before;

  std::remove(path.c_str());
  ASSERT_TRUE(read.has_value()) << read.error().diagnostic_message();
  ASSERT_EQ(img.pixels_view().size(), buffer_bytes);
  EXPECT_LT(peak, buffer_bytes + buffer_bytes / 2)
    << "peak heap " << peak << " bytes for a " << buffer_bytes << "-byte pixel buffer";
}
