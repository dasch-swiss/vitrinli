/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * SipiImage is the core object of dealing with images within the Sipi package
 * The SipiImage object holds all the information about an image and offers the methods
 * to read, write and modify images. Reading and writing is supported in several standard formats
 * such as TIFF, J2k, PNG etc.
 */
#ifndef _sipi_image_h
#define _sipi_image_h

#include <functional>
#include <span>
#include <string>
// #include <unordered_map>

#include "SipiIO.h"
#include "SipiImageError.h"
// #include "iiifparser/SipiRegion.h"
#include "metadata/essentials.h"
#include "metadata/exif.h"
#include "metadata/icc.h"
#include "metadata/iptc.h"
#include "metadata/photometric_interpretation.h"
#include "metadata/xmp.h"


/*!
 * \namespace Sipi Is used for all Sipi things.
 */
namespace Sipi {

// Used for 8 bits per sample (color channel) images
using byte = unsigned char;

// Used for 16 bits per sample (color channel) images
using word = unsigned short;

/*! The meaning of extra channels as used in the TIF format */
enum class ExtraSamples : std::uint8_t {
  UNSPECIFIED = 0,//!< Unknown meaning
  ASSOCALPHA = 1,//!< Associated alpha channel
  UNASSALPHA = 2//!< Unassociated alpha channel
};

enum SkipMetadata : std::uint8_t {
  SKIP_NONE = 0x00,
  SKIP_ICC = 0x01,
  SKIP_XMP = 0x02,
  SKIP_IPTC = 0x04,
  SKIP_EXIF = 0x08,
  SKIP_ALL = 0xFF
};

/*!
 * \struct PixelDelta
 *
 * Per-channel absolute pixel-difference statistics between two images,
 * as reported by the `sipi compare` command. `max_abs` is the largest
 * |sample₁ − sample₂| across all pixels and channels, located at
 * (`max_x`, `max_y`); `mean_abs` is the mean |Δ| over every sample.
 *
 * `max_x`/`max_y` are row-major (`y * nx + x`) pixel coordinates, matching
 * the layout used by `processing::subtract`, the format handlers, and
 * `getPixel`/`setPixel` — so they may be fed back into those accessors.
 */
struct PixelDelta
{
  double mean_abs;//!< mean absolute per-channel difference
  int max_abs;//!< maximum absolute per-channel difference
  size_t max_x;//!< x coordinate of the maximum absolute difference (codec-store order)
  size_t max_y;//!< y coordinate of the maximum absolute difference (codec-store order)
};

/*!
 * \class SipiImage
 *
 * Base class for all images in the Sipi package.
 * This class implements all the data and handling (methods) associated with
 * images in Sipi. Please note that the map of io-classes (see \ref SipiIO) is
 * defined in `src/format_handlers/format_registry.cpp` (in //src/format_handlers, which depends
 * one-way on the engine); adding a new file format edits that registry plus the
 * read_dispatch/read_shape dispatch below, and its handler implements
 * `SipiIO::read_metadata` — see `tools/format-handlers-fanout.sh` for the
 * full list of shared edit sites.
 */
class SipiImage
{
  static std::unordered_map<std::string, std::shared_ptr<SipiIO>>
    io;//!< member variable holding a map of I/O class instances for the different file formats
  void ensure_exif();
  [[nodiscard]] Result<void> read_dispatch(const std::string &filepath,
    const std::function<Result<bool>(SipiIO &handler)> &handler_read);

protected:
  size_t nx;//!< Number of horizontal pixels (width)
  size_t ny;//!< Number of vertical pixels (height)
  size_t nc;//!< Total number of samples per pixel
  size_t bps;//!< bits per sample. Currently only 8 and 16 are supported
  std::vector<ExtraSamples> es;//!< meaning of the extra samples (channels)
  Orientation orientation;//!< Orientation of the image
  PhotometricInterpretation photo;//!< Image type, that is the meaning of the channels
  std::vector<byte> pixels;//!< Pixel buffer (allways in big-endian format if interpreted as 16 bit/sample)
  std::shared_ptr<Xmp> xmp;//!< Pointer to instance Xmp class (\ref Xmp), or NULL
  std::shared_ptr<Icc> icc;//!< Pointer to instance of Icc class (\ref Icc), or NULL
  std::shared_ptr<Iptc> iptc;//!< Pointer to instance of Iptc class (\ref Iptc), or NULL
  std::shared_ptr<Exif> exif;//!< Pointer to instance of Exif class (\ref Exif), or NULL
  Essentials emdata;//!< Metadata to be stored in file header
  SkipMetadata skip_metadata;//!< If true, all metadata is stripped off

public:
  //
  /*!
   * Default constructor. Creates an empty image
   */
  SipiImage();

  /*!
   * Copy constructor. Makes a deep copy of the image
   *
   * Assumes `img_p` is a well-formed image whose geometry was already
   * validated at its own construction (the dimensioned constructor, or
   * `set_pixels()`'s size check). Recomputing the buffer size here is a
   * defensive re-check of that already-established invariant, not a parse
   * of new input: it throws only if the source object's internal state is
   * corrupt, the same class of failure as `std::bad_alloc`.
   *
   * \param[in] img_p An existing instance if SipiImage
   */
  SipiImage(const SipiImage &img_p);

  /*!
   * Move constructor. Transfers ownership of pixel buffer and metadata.
   */
  SipiImage(SipiImage &&other) noexcept;

  /*!
   * Move assignment operator. Transfers ownership of pixel buffer and metadata.
   */
  SipiImage &operator=(SipiImage &&other) noexcept;

  /*!
   * Create an empty image with the pixel buffer available, but all pixels set to 0
   *
   * The caller states the geometry directly, not a file's decoded shape:
   * every argument must already be a self-consistent combination. Passing
   * one that isn't is a programming error, not a fallible parse of external
   * input, so it throws rather than returning a value.
   *
   * \param[in] nx_p Dimension in x direction
   * \param[in] ny_p Dimension in y direction
   * \param[in] nc_p Number of channels
   * \param[in] bps_p Bits per sample, either 8 or 16 are allowed
   * \param[in] photo_p The photometric interpretation
   * \throws SipiImageError if `photo_p` and `nc_p` are an unsupported combination,
   *   `bps_p` is neither 8 nor 16, or the resulting buffer would be empty
   *   (`nx_p`, `ny_p`, or `nc_p` is zero)
   */
  SipiImage(size_t nx_p, size_t ny_p, size_t nc_p, size_t bps_p, PhotometricInterpretation photo_p);

  /*!
   * Getter for nx
   */
  [[nodiscard]] size_t getNx() const { return nx; };

  /*!
   * Getter for ny
   */
  [[nodiscard]] size_t getNy() const { return ny; };

  /*!
   * Getter for nc (includes alpha channels!)
   */
  [[nodiscard]] size_t getNc() const { return nc; };

  /*!
   * Getter for number of alpha channels
   */
  [[nodiscard]] size_t getNalpha() const { return es.size(); }

  /*!
   * Get bits per sample of image
   * @return bis per sample (bps)
   */
  [[nodiscard]] size_t getBps() const { return bps; }

  /**
   * Get the exif metadata of the image.
   * \return exif metadata
   */
  [[nodiscard]] std::shared_ptr<Exif> getExif() const { return exif; };

  /**
   * Get the ICC color profile of the image.
   * \return ICC profile, or nullptr if not set
   */
  [[nodiscard]] std::shared_ptr<Icc> getIcc() const { return icc; }

  /**
   * Get the XMP metadata of the image.
   * \return XMP metadata, or nullptr if not set
   */
  [[nodiscard]] std::shared_ptr<Xmp> getXmp() const { return xmp; }

  /**
   * Get the IPTC metadata of the image.
   * \return IPTC metadata, or nullptr if not set
   */
  [[nodiscard]] std::shared_ptr<Iptc> getIptc() const { return iptc; }

  /*!
   * Get orientation
   * @return Returns orientation tag
   */
  [[nodiscard]] Orientation getOrientation() const { return orientation; };

  /*!
   * Set orientation parameter
   * @param value orientation value to be set
   */
  void setOrientation(Orientation value) { orientation = value; };


  /*!
   * Get photometric interpretation
   * @return Returns photometric interpretation tag
   */
  [[nodiscard]] PhotometricInterpretation getPhoto() const { return photo; };


  ~SipiImage() = default;

  /*!
   * Gets the value of a pixel sample.
   *
   * Indexes the pixel buffer row-major (`nc * (y * nx + x) + c`), matching the
   * codec store written by the format handlers and read by `maxPixelDelta`.
   *
   * The coordinates are the caller's to get right: `x`, `y`, `c` are not
   * validated against any decoded file or request input, only against the
   * dimensions of this already-decoded image. An out-of-range coordinate or
   * an unsupported `bps` is a programming error, signalled by throwing
   * `SipiImageError` (not returned as a `Result`, since no input path can
   * reach it).
   *
   * \param[in] x X position
   * \param[in] y Y position
   * \param[in] c Color channel
   * \return The sample value
   */
  [[nodiscard]] int getPixel(size_t x, size_t y, size_t c)
  {
    if (x >= nx)
      throw SipiImageError("getPixel: x out of range (x=" + std::to_string(x) + ", nx=" + std::to_string(nx) + ")");
    if (y >= ny)
      throw SipiImageError("getPixel: y out of range (y=" + std::to_string(y) + ", ny=" + std::to_string(ny) + ")");
    if (c >= nc)
      throw SipiImageError("getPixel: c out of range (c=" + std::to_string(c) + ", nc=" + std::to_string(nc) + ")");
    switch (bps) {
    case 8: {
      const unsigned char *tmp = pixels.data();
      return static_cast<int>(tmp[nc * (y * nx + x) + c]);
    }
    case 16: {
      const unsigned short *tmp = (const unsigned short *)pixels.data();
      return static_cast<int>(tmp[nc * (y * nx + x) + c]);
    }
    default: {
      throw SipiImageError("getPixel: unsupported bits-per-sample (bps=" + std::to_string(bps) + ", supported: 8, 16)");
    }
    }
  }

  /*!
   * Sets a pixel to a given value
   *
   * The coordinates and value are the caller's to get right: `x`, `y`, `c`
   * and `val` are not validated against any decoded file or request input,
   * only against the dimensions and bit depth of this already-decoded image.
   * An out-of-range coordinate, an out-of-range value or an unsupported
   * `bps` is a programming error, signalled by throwing `SipiImageError`
   * (not returned as a `Result`, since no input path can reach it).
   *
   * \param[in] x X position
   * \param[in] y Y position
   * \param[in] c Color channels
   * \param[in] val Pixel value
   */
  void setPixel(size_t x, size_t y, size_t c, int val)
  {
    if (x >= nx)
      throw SipiImageError("setPixel: x out of range (x=" + std::to_string(x) + ", nx=" + std::to_string(nx) + ")");
    if (y >= ny)
      throw SipiImageError("setPixel: y out of range (y=" + std::to_string(y) + ", ny=" + std::to_string(ny) + ")");
    if (c >= nc)
      throw SipiImageError("setPixel: c out of range (c=" + std::to_string(c) + ", nc=" + std::to_string(nc) + ")");

    switch (bps) {
    case 8: {
      if (val > 0xff)
        throw SipiImageError(
          "setPixel: val out of range for 8 bps (val=" + std::to_string(val) + ", max=" + std::to_string(0xff) + ")");
      unsigned char *tmp = pixels.data();
      tmp[nc * (y * nx + x) + c] = (unsigned char)val;
      break;
    }
    case 16: {
      if (val > 0xffff)
        throw SipiImageError("setPixel: val out of range for 16 bps (val=" + std::to_string(val)
                             + ", max=" + std::to_string(0xffff) + ")");
      unsigned short *tmp = (unsigned short *)pixels.data();
      tmp[nc * (y * nx + x) + c] = (unsigned short)val;
      break;
    }
    default: {
      if (val > 0xffff)
        throw SipiImageError(
          "setPixel: unsupported bits-per-sample (bps=" + std::to_string(bps) + ", supported: 8, 16)");
    }
    }
  }

  /*!
   * Assignment operator
   *
   * Makes a deep copy of the instance
   *
   * \param[in] img_p Instance of a SipiImage
   */
  SipiImage &operator=(const SipiImage &img_p);

  /*!
   * Set the metadata that should be skipped in writing a file
   *
   * \param[in] smd Logical "or" of bitmasks for metadata to be skipped
   */
  void setSkipMetadata(SkipMetadata smd) { skip_metadata = smd; };

  /*!
   * Getter for the metadata-skip bitmask set by `setSkipMetadata()`.
   */
  [[nodiscard]] SkipMetadata getSkipMetadata() const { return skip_metadata; }

  void essential_metadata(const Essentials &emdata_p) { emdata = emdata_p; }

  [[nodiscard]] Essentials essential_metadata() const { return emdata; }

  /*!
   * Replace the pixel buffer and its geometry atomically. This is the only
   * path that changes the buffer's size: it validates
   * `buf.size() == nx_p * ny_p * nc_p * (bps_p / 8)` before accepting the
   * buffer, instead of trusting every call site to keep buffer and geometry
   * in sync.
   *
   * \param[in] buf New pixel buffer, moved in on success
   * \param[in] nx_p New width
   * \param[in] ny_p New height
   * \param[in] nc_p New channel count
   * \param[in] bps_p New bits per sample (8 or 16)
   * \throws SipiImageError if buf's size does not match the given geometry
   */
  void set_pixels(std::vector<byte> &&buf, size_t nx_p, size_t ny_p, size_t nc_p, size_t bps_p);

  /*!
   * Set the image geometry alone, without touching the pixel buffer. Serves
   * the decode phase, where a container's geometry is parsed before the
   * pixel buffer exists, and the rare encode-side geometry change that
   * doesn't move a buffer. `set_pixels()` remains the only size-checked path
   * that keeps buffer and geometry in lockstep — use it whenever a buffer is
   * available.
   *
   * \param[in] nx_p New width
   * \param[in] ny_p New height
   * \param[in] nc_p New channel count
   * \param[in] bps_p New bits per sample (8 or 16)
   */
  void set_geometry(size_t nx_p, size_t ny_p, size_t nc_p, size_t bps_p)
  {
    nx = nx_p;
    ny = ny_p;
    nc = nc_p;
    bps = bps_p;
  }

  /*!
   * Mutable view over the pixel buffer at its current size. A `span`
   * (unlike a `std::vector<byte>&`) cannot resize, clear, or reassign the
   * underlying buffer, so it cannot desynchronize it from
   * `nx`/`ny`/`nc`/`bps` — only `set_pixels()` changes size.
   */
  [[nodiscard]] std::span<byte> pixels_writable() { return std::span<byte>(pixels); }

  /*!
   * Read-only view over the pixel buffer at its current size.
   */
  [[nodiscard]] std::span<const byte> pixels_view() const { return std::span<const byte>(pixels); }

  /*!
   * Getter for the extra-samples (channel-meaning) vector.
   */
  [[nodiscard]] const std::vector<ExtraSamples> &getEs() const { return es; }

  /*!
   * Setter for the extra-samples (channel-meaning) vector. Kept independent
   * of `set_pixels()` since a channel add/remove changes `es` alongside,
   * but not in lockstep with, the buffer geometry `set_pixels()` tracks.
   */
  void setEs(std::vector<ExtraSamples> es_p) { es = std::move(es_p); }

  /*!
   * Appends a single entry to the extra-samples (channel-meaning) vector,
   * for decode paths that discover one extra channel's meaning at a time.
   */
  void addEs(ExtraSamples es_p) { es.push_back(es_p); }

  /*!
   * Setter for the photometric interpretation. Kept independent of
   * `set_pixels()` for the same reason as `setEs()`.
   */
  void setPhoto(PhotometricInterpretation photo_p) { photo = photo_p; }

  /*!
   * Setter for the ICC color profile, mirroring `essential_metadata()`'s
   * getter/setter shape.
   */
  void set_icc(std::shared_ptr<Icc> icc_p) { icc = std::move(icc_p); }

  /*!
   * Setter for the XMP metadata, mirroring `set_icc()`'s shape.
   */
  void set_xmp(std::shared_ptr<Xmp> xmp_p) { xmp = std::move(xmp_p); }

  /*!
   * Setter for the IPTC metadata, mirroring `set_icc()`'s shape.
   */
  void set_iptc(std::shared_ptr<Iptc> iptc_p) { iptc = std::move(iptc_p); }

  /*!
   * Setter for the Exif metadata, mirroring `set_icc()`'s shape.
   */
  void set_exif(std::shared_ptr<Exif> exif_p) { exif = std::move(exif_p); }

  /**
   * Sets the XMP and IPTC from the raw copies EXIF IFD0 can hold
   * (Exif.Image.XMLPacket, Exif.Image.IPTCNAA), each only while the image has
   * no such block and only when the copy parses whole (else logged). The
   * EXIF encoder never writes the copies, so without this a file that keeps
   * its rights only there loses them in every output. write() calls it;
   * a caller that edits metadata before write() calls it first.
   */
  void promote_ifd0_copies();

  /*!
   * Returns the image's Exif metadata, lazily allocating an empty `Exif`
   * instance first if none exists yet. Absorbs `ensure_exif()`'s
   * lazy-initialisation so callers never need the private method.
   */
  [[nodiscard]] std::shared_ptr<Exif> exif_writable()
  {
    ensure_exif();
    return exif;
  }

  /*!
   * Hash the current pixel buffer with the requested digest algorithm and
   * return the raw digest bytes (NOT hex). Used by the `convert service-file`
   * command (DEV-6540) to
   * populate `EssentialsFields::data_chksum` over the **post-transformation**
   * pixel buffer, and by the corruption-tripwire branch in `readSource`
   * (which compares against the on-disk `data_chksum` from an existing
   * Essentials packet — same digest, hex-vs-raw bridged via
   * `Essentials::to_hex`).
   *
   * Buffer layout matches `readSource`'s tripwire: `nx * ny * nc * bps / 8`
   * raw bytes, big-endian when `bps == 16`.
   */
  [[nodiscard]] std::vector<std::byte> compute_pixel_hash(shttps::HashType type) const;

  /*!
   * Read an image from the given path
   *
   * \param[in] filepath A string containing the path to the image file
   * \param[in] region Pointer to a SipiRegion which indicates that we
   *            are only interested in this region. The image will be cropped.
   * \param[in] size Pointer to a size object. The image will be scaled accordingly
   * \param[in] force_bps_8 We want in any case a 8 Bit/sample image. Reduce if necessary. Default is false.
   * \param[in] scaling_quality Quality of the scaling algorithm. Default is HIGH.
   *
   * \return `Result<void>`, or a `SipiValueError` if no registered format
   *   handler recognises the file, or a handler recognised it and failed to decode it.
   */
  [[nodiscard]] Result<void> read(const std::string &filepath,
    const std::shared_ptr<SipiRegion> &region = nullptr,
    const std::shared_ptr<SipiSize> &size = nullptr,
    bool force_bps_8 = false,
    ScalingQuality scaling_quality = { ScalingMethod::HIGH,
      ScalingMethod::HIGH,
      ScalingMethod::HIGH,
      ScalingMethod::HIGH });

  /*!
   * Read the header and the embedded metadata of a file, without decoding
   * pixels: the image then holds what `read(filepath)` leaves, except the
   * pixel buffer, which stays empty. Picks the handler and fails as `read`
   * does. Serves `sipi query`; `read_shape` stays the shape probe (ADR-0004).
   *
   * \param[in] filepath A string containing the path to the image file
   * \return `Result<void>`, or a `SipiValueError` if no registered format
   *   handler recognises the file, or a handler recognised it and failed.
   */
  [[nodiscard]] Result<void> read_metadata(const std::string &filepath);

  /*!
   * Read an image from disk into memory. The tool makes no claim that the file
   * is "the original" — it is the source for the current operation (ADR-0009,
   * ADR-0010, DEV-6539).
   *
   * If the source happens to be a Service File carrying an Essentials packet,
   * the embedded pixel checksum is recomputed and compared against the
   * `data_chksum` field as a corruption tripwire (ADR-0010): on mismatch, an
   * ERROR is logged and reading continues. No Essentials packet is *created*
   * during read; packet creation is intentional output gated by the
   * `convert service-file` subcommand (DEV-6540).
   *
   * \param[in] filepath A string containing the path to the source image file
   * \param[in] region Optional region of interest — the image will be cropped
   * \param[in] size Optional size — the image will be scaled accordingly
   * \return `Result<void>`, propagating `read`'s failure.
   */
  [[nodiscard]] Result<void> readSource(const std::string &filepath,
    const std::shared_ptr<SipiRegion> &region = nullptr,
    const std::shared_ptr<SipiSize> &size = nullptr);

  /*!
   * Overload accepting an `origname` hint. Until the `convert service-file`
   * command lands (DEV-6540), `origname` is consumed by the
   * command — not by readSource itself — so this overload behaves
   * identically to the 3-arg form. Kept for the existing Lua-side call site.
   */
  [[nodiscard]] Result<void> readSource(const std::string &filepath,
    const std::shared_ptr<SipiRegion> &region,
    const std::shared_ptr<SipiSize> &size,
    const std::string &origname);


  /*!
   * Read the image shape (dimensions, tiling, levels, channels, bit depth) from a file
   * without performing a full decode. Dispatches to the format handler's read_shape;
   * service-file handlers may take a fast path via the Essentials packet (ADR-0004).
   *
   * \param[in] filepath Pathname of the image file
   * \return Info about image (see SipiImgInfo), or a `SipiValueError` if the
   *   mimetype is unrecognised or no registered format handler recognises the file.
   */
  [[nodiscard]] Result<SipiImgInfo> read_shape(const std::string &filepath) const;

  /*!
   * Get the dimensions of an in-memory SipiImage (already loaded; no file I/O).
   *
   * @param[out] width Width of the image in pixels
   * @param[out] height Height of the image in pixels
   */
  void getDim(size_t &width, size_t &height) const;

  /*!
   * Write an image to the given OutputSink (ADR-0006).
   *
   * \param[in] ftype The file format that should be used to write the file. Supported are
   * the keys of the static SipiIO handler map (SipiImage.cpp):
   * - "tif" for TIFF files
   * - "jpx" for JPEG2000 files
   * - "png" for PNG files
   * - "jpg" for JPEG files
   * Any other value throws std::out_of_range.
   * \param[in] sink Where the encoded bytes go: a FilePath (file, or stdout via
   * "-"/"stdout:"), a CallbackSink, or a TeeSink.
   * \return `Result<void>`, or a `SipiValueError` if the handler failed to encode
   *   or write the image.
   */
  [[nodiscard]] Result<void> write(const std::string &ftype,
    const OutputSink &sink,
    const SipiCompressionParams *params = nullptr);

  /*!
   * Convenience overload: write to a filesystem path (or stdout via "-" /
   * "stdout:"). Equivalent to write(ftype, FilePath{filepath}, params).
   *
   * \return `Result<void>`, propagating `write`'s failure.
   */
  [[nodiscard]] Result<void> write(const std::string &ftype,
    const std::string &filepath,
    const SipiCompressionParams *params = nullptr);


  /*!
   * The overloaded << operator which is used to write the error message to the output
   *
   * \param[in] lhs The output stream
   * \param[in] rhs Reference to an instance of a SipiImage
   * \returns Returns ostream object
   */
  friend std::ostream &operator<<(std::ostream &lhs, const SipiImage &rhs);
};
}// namespace Sipi

#endif
