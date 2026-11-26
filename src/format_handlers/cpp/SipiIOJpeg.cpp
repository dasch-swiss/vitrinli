/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <algorithm>
#include <array>
#include <climits>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <cstdio>
#include <fcntl.h>

#include <tiff.h>


#include "logging/logger.h"
#include "error/SipiError.h"
#include "error/SipiValueError.h"
#include "image/SipiIO.h"
#include "image/SipiImage.h"
#include "image/SipiImageError.h"
#include "image_processing/processing.h"
#include "SipiIOJpeg.h"
#include "observability/profiling.h"

#include "jerror.h"
#include "jpeglib.h"

#define ICC_MARKER (JPEG_APP0 + 2) /* JPEG marker code for ICC */
// #define ICC_OVERHEAD_LEN  14        /* size of non-profile data in APP2 */
// #define MAX_BYTES_IN_MARKER  65533    /* maximum data len of a JPEG marker */


namespace Sipi {

/// RAII guard for POSIX file descriptors (int fd).
struct FdGuard {
  int fd{-1};
  explicit FdGuard(int fd) : fd(fd) {}
  ~FdGuard() { if (fd >= 0) ::close(fd); }
  FdGuard(const FdGuard &) = delete;
  FdGuard &operator=(const FdGuard &) = delete;
  void release() { fd = -1; }
};

inline bool getbyte(int &c, FILE *f)
{
  if ((c = getc(f)) == EOF) {
    return false;
  } else {
    return true;
  }
}

inline bool getword(int &c, FILE *f)
{
  int cc_;
  int dd_;
  if (((cc_ = getc(f)) == EOF) || ((dd_ = getc(f)) == EOF)) {
    return false;
  } else {
    c = (cc_ << 8) + dd_;
    return true;
  }
}

/*!
 * Special exception within the JPEG routines which can be caught separately
 */
/// Extended jpeg_error_mgr with setjmp buffer for safe error handling through
/// libjpeg's C stack frames. This is the canonical IJG libjpeg error handling
/// pattern — the struct's first field must be jpeg_error_mgr so libjpeg can
/// cast cinfo->err to it, and we cast back to access error_jmp.
struct JpegErrorMgr {
  jpeg_error_mgr pub;  // must be first — libjpeg casts to this
  jmp_buf error_jmp;
  char error_message[JMSG_LENGTH_MAX]{};  // char[], not std::string — safe across longjmp
};

/// Error exit for all JPEG paths (read, read_shape, write): longjmp back to the
/// setjmp point. This avoids throwing C++ exceptions through libjpeg's C frames.
static void jpegErrorExit(j_common_ptr cinfo)
{
  auto *myerr = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
  (*(cinfo->err->format_message))(cinfo, myerr->error_message);
  longjmp(myerr->error_jmp, 1);
}

//------------------------------------------------------------------


/*!
 * Buffer for libjpeg's file source/destination managers. Owned by the caller
 * of jpeg_file_src/jpeg_file_dest and declared *before* the setjmp() so its
 * destructor runs on the C++ unwind path; the term_* callbacks only flush and
 * unhook — they do not free (longjmp would otherwise leak what they skip).
 */
struct FileBuffer
{
  explicit FileBuffer(int file_id_p, size_t buflen_p = 65536)
    : buffer(std::make_unique<JOCTET[]>(buflen_p)), buflen(buflen_p), file_id(file_id_p)
  {}

  std::unique_ptr<JOCTET[]> buffer;
  size_t buflen;
  int file_id;
};

/*!
 * Function which initializes the structures for managing the IO
 */
static void init_file_destination(j_compress_ptr cinfo)
{
  auto *file_buffer = (FileBuffer *)cinfo->client_data;
  cinfo->dest->free_in_buffer = file_buffer->buflen;
  cinfo->dest->next_output_byte = file_buffer->buffer.get();
}

//=============================================================================

/*!
 * Function empty the libjeg buffer and write the data to the socket
 */
static boolean empty_file_buffer(j_compress_ptr cinfo)
{
  auto *file_buffer = (FileBuffer *)cinfo->client_data;
  size_t n = file_buffer->buflen;
  size_t nn = 0;
  do {
    ssize_t tmp_n = write(file_buffer->file_id, file_buffer->buffer.get() + nn, n);
    if (tmp_n < 0) {
      ERREXIT(cinfo, JERR_FILE_WRITE);  // triggers jpegErrorExit → longjmp
      return FALSE;  // unreachable, but satisfies return type
    } else {
      n -= tmp_n;
      nn += tmp_n;
    }
  } while (n > 0);

  cinfo->dest->free_in_buffer = file_buffer->buflen;
  cinfo->dest->next_output_byte = file_buffer->buffer.get();

  return static_cast<boolean>(1);
}

//=============================================================================

/*!
 * Finish writing data
 */
static void term_file_destination(j_compress_ptr cinfo)
{
  FileBuffer *file_buffer = (FileBuffer *)cinfo->client_data;
  size_t n = cinfo->dest->next_output_byte - file_buffer->buffer.get();
  size_t nn = 0;
  do {
    auto tmp_n = write(file_buffer->file_id, file_buffer->buffer.get() + nn, n);
    if (tmp_n < 0) {
      ERREXIT(cinfo, JERR_FILE_WRITE);  // triggers jpegErrorExit → longjmp
      return;  // unreachable
    } else {
      n -= tmp_n;
      nn += tmp_n;
    }
  } while (n > 0);

  // The FileBuffer and the destination manager are owned by the caller of
  // jpeg_file_dest; only unhook them here.
  cinfo->client_data = nullptr;
  cinfo->dest = nullptr;
}

//=============================================================================

/*!
 * This function is used to setup the I/O destination to a file descriptor.
 * Both the FileBuffer and the destination manager are owned by the caller and
 * must outlive the compress struct (declare them before the setjmp).
 */
static void
  jpeg_file_dest(struct jpeg_compress_struct *cinfo, FileBuffer *file_buffer, struct jpeg_destination_mgr *destmgr)
{
  cinfo->client_data = file_buffer;

  destmgr->init_destination = init_file_destination;
  destmgr->empty_output_buffer = empty_file_buffer;
  destmgr->term_destination = term_file_destination;

  cinfo->dest = destmgr;
}

//=============================================================================


static void init_file_source(struct jpeg_decompress_struct *cinfo)
{
  auto *file_buffer = (FileBuffer *)cinfo->client_data;
  cinfo->src->next_input_byte = file_buffer->buffer.get();
  cinfo->src->bytes_in_buffer = 0;
}

//=============================================================================

static boolean file_source_fill_input_buffer(struct jpeg_decompress_struct *cinfo)
{
  auto *file_buffer = (FileBuffer *)cinfo->client_data;
  size_t nbytes = 0;
  do {
    auto n = read(file_buffer->file_id, file_buffer->buffer.get() + nbytes, file_buffer->buflen - nbytes);
    if (n < 0) {
      break;// error
    }
    if (n == 0) break;// EOF reached...
    nbytes += n;
  } while (nbytes < file_buffer->buflen);
  if (nbytes <= 0) {
    ERREXIT(cinfo, 999);
    /*
    WARNMS(cinfo, JWRN_JPEG_EOF);
    infile_buffer->buffer[0] = (JOCTET) 0xFF;
    infile_buffer->buffer[1] = (JOCTET) JPEG_EOI;
    nbytes = 2;
    */
  }
  cinfo->src->next_input_byte = file_buffer->buffer.get();
  cinfo->src->bytes_in_buffer = nbytes;
  return static_cast<boolean>(true);
}

//=============================================================================

static void file_source_skip_input_data(struct jpeg_decompress_struct *cinfo, long num_bytes)
{
  if (num_bytes > 0) {
    while (num_bytes > (long)cinfo->src->bytes_in_buffer) {
      num_bytes -= (long)cinfo->src->bytes_in_buffer;
      (void)file_source_fill_input_buffer(cinfo);
    }
  }
  cinfo->src->next_input_byte += (size_t)num_bytes;
  cinfo->src->bytes_in_buffer -= (size_t)num_bytes;
}

//=============================================================================

static void term_file_source(struct jpeg_decompress_struct *cinfo)
{
  // The FileBuffer and the source manager are owned by the caller of
  // jpeg_file_src; only unhook them here.
  cinfo->client_data = nullptr;
  cinfo->src = nullptr;
}

//=============================================================================

/*!
 * Load the JPEG file. Both the FileBuffer and the source manager are owned by
 * the caller and must outlive the decompress struct (declare them before the
 * setjmp).
 */
static void jpeg_file_src(struct jpeg_decompress_struct *cinfo, FileBuffer *file_buffer, struct jpeg_source_mgr *srcmgr)
{
  cinfo->client_data = file_buffer;

  srcmgr->init_source = init_file_source;
  srcmgr->fill_input_buffer = file_source_fill_input_buffer;
  srcmgr->skip_input_data = file_source_skip_input_data;
  srcmgr->resync_to_restart = jpeg_resync_to_restart;// default!
  srcmgr->term_source = term_file_source;

  cinfo->src = srcmgr;
}

//=============================================================================


/*!
 * Struct that is used to hold the variables for defining the
 * private I/O routines which are used to write the the HTTP socket
 */
/*!
 * Context for libjpeg's HTTP destination manager. Owned by the caller of
 * SipiIOJpeg::write via std::unique_ptr declared *before* the setjmp(); that
 * placement keeps the destructor on the normal C++ unwind path when we throw
 * from the setjmp handler, avoiding the longjmp-skips-destructors UB that
 * forced the previous malloc-based design.
 */
struct HtmlBuffer
{
  explicit HtmlBuffer(SinkStream *sink_p, size_t buflen_p = 65536)
    : buffer(std::make_unique<JOCTET[]>(buflen_p)), buflen(buflen_p), sink(sink_p)
  {}

  std::unique_ptr<JOCTET[]> buffer;
  size_t buflen;
  SinkStream *sink;
  bool client_aborted{ false };
};

/*!
 * Function which initializes the structures for managing the IO
 */
static void init_html_destination(j_compress_ptr cinfo)
{
  auto *html_buffer = static_cast<HtmlBuffer *>(cinfo->client_data);
  cinfo->dest->free_in_buffer = html_buffer->buflen;
  cinfo->dest->next_output_byte = html_buffer->buffer.get();
}

//=============================================================================

/*!
 * Function empty the libjeg buffer and write the data to the socket
 */
static boolean empty_html_buffer(j_compress_ptr cinfo)
{
  auto *html_buffer = static_cast<HtmlBuffer *>(cinfo->client_data);
  // A non-zero return means the body sink (the HTTP socket) failed — the
  // equivalent of the old OUTPUT_WRITE_FAIL: the client is gone. No C++
  // exception crosses libjpeg's C frames (the sink callback returns a code).
  if (html_buffer->sink->write(html_buffer->buffer.get(), html_buffer->buflen) != 0) {
    html_buffer->client_aborted = true;
    log_err("JPEG HTTP write failed: sink write error");
    ERREXIT(cinfo, JERR_FILE_WRITE);// triggers jpegErrorExit → longjmp
    return FALSE;// unreachable
  }
  cinfo->dest->free_in_buffer = html_buffer->buflen;
  cinfo->dest->next_output_byte = html_buffer->buffer.get();
  return static_cast<boolean>(true);
}

//=============================================================================

/*!
 * Finish writing data
 */
static void term_html_destination(j_compress_ptr cinfo)
{
  auto *html_buffer = static_cast<HtmlBuffer *>(cinfo->client_data);
  const size_t nbytes = cinfo->dest->next_output_byte - html_buffer->buffer.get();
  if (html_buffer->sink->write(html_buffer->buffer.get(), nbytes) != 0) {
    // See empty_html_buffer: a non-zero sink return is the abort signal.
    html_buffer->client_aborted = true;
    log_err("JPEG HTTP write (term) failed: sink write error");
    ERREXIT(cinfo, JERR_FILE_WRITE);
    // unreachable
  }
}

//=============================================================================

/*!
 * Wire libjpeg's destination callbacks to the caller-owned HtmlBuffer and
 * destination-manager objects. Ownership of both stays with the caller's
 * std::unique_ptrs — this function only installs pointers into `cinfo`.
 */
static void jpeg_html_dest(struct jpeg_compress_struct *cinfo,
                           HtmlBuffer *html_buffer,
                           jpeg_destination_mgr *destmgr)
{
  cinfo->client_data = html_buffer;
  destmgr->init_destination = init_html_destination;
  destmgr->empty_output_buffer = empty_html_buffer;
  destmgr->term_destination = term_html_destination;
  cinfo->dest = destmgr;
}

//=============================================================================

// The XMP packet of the APP1 segments: the StandardXMP, with its ExtendedXMP
// merged in (XMP Specification Part 3, 1.1.3.1). The StandardXMP payload is
// everything after the 29-byte namespace header: older Photoshop exports
// omit the optional <?xpacket> wrappers, so scanning for them is not reliable.
static std::optional<std::string> xmp_packet(jpeg_saved_marker_ptr marker)
{
  constexpr std::string_view standard_sig("http://ns.adobe.com/xap/1.0/\0", 29);
  constexpr std::string_view extension_sig("http://ns.adobe.com/xmp/extension/\0", 35);
  std::optional<std::string_view> standard;
  std::vector<std::string_view> parts;
  for (; marker != nullptr; marker = marker->next) {
    if (marker->marker != JPEG_APP0 + 1) { continue; }
    const std::string_view data(reinterpret_cast<const char *>(marker->data), marker->data_length);
    if (data.starts_with(extension_sig)) {
      parts.push_back(data.substr(extension_sig.size()));
      continue;
    }
    const auto pos = data.find(standard_sig);
    if (pos != std::string_view::npos && pos + standard_sig.size() < data.size()) {
      standard = data.substr(pos + standard_sig.size());
    }
  }
  if (!standard) { return std::nullopt; }
  return Xmp::merge_extended(*standard, parts);
}

// The image resources of every APP13 "Photoshop 3.0" segment, in order: a
// resource can continue in the next segment.
static std::string photoshop_resources(jpeg_saved_marker_ptr marker)
{
  std::string resources;
  for (; marker != nullptr; marker = marker->next) {
    if (marker->marker == JPEG_APP0 + 13 && marker->data_length >= 14
        && memcmp("Photoshop 3.0", marker->data, 14) == 0) {
      resources.append(reinterpret_cast<const char *>(marker->data) + 14, marker->data_length - 14);
    }
  }
  return resources;
}

Result<bool> SipiIOJpeg::parse_photoshop_resources(SipiImage *img, std::string resources)
{
  if (resources.size() > INT_MAX) {
    return std::unexpected(SipiValueError{ ErrorCode::kMalformedInput, "JPEG Photoshop resources over 2 GB" });
  }
  if (resources.empty()) { return true; }
  return parse_photoshop(img, resources.data(), static_cast<int>(resources.size()));
}

Result<bool> SipiIOJpeg::parse_photoshop(SipiImage *img, char *data, int length)
{
  int slen;
  unsigned int datalen = 0;
  char *ptr = data;
  char *end = data + length;
  unsigned short id;
  char name[256];

  while ((ptr - data) < length) {
    // Bounds check: need at least 4 bytes for signature. `end - ptr` (rather
    // than `ptr + N > end`) avoids forming a pointer past `end` — pointer
    // arithmetic that overflows the pointed-to object is undefined behavior.
    if (4 > (size_t)(end - ptr)) break;

    if (memcmp(ptr, "8BIM", 4) != 0) break;
    ptr += 4;

    // Bounds check: need at least 2 bytes for tag ID
    if (2 > (size_t)(end - ptr)) break;
    id = ((unsigned char)*(ptr + 0) << 8) | (unsigned char)*(ptr + 1);
    ptr += 2;

    // Name processing (Pascal string) — bounds check
    if (ptr >= end) break;
    slen = (unsigned char)*ptr;
    if (static_cast<size_t>(1 + slen) > (size_t)(end - ptr)) break;
    int name_len = (slen < 255) ? slen : 255;
    for (int i = 0; i < name_len; i++) name[i] = *(ptr + i + 1);
    name[name_len] = '\0';
    slen++;// add length byte
    if ((slen % 2) == 1) slen++;
    // The even-padding bump above can push slen one byte past the bound
    // already checked against `end - ptr`; re-check before advancing.
    if (slen > static_cast<size_t>(end - ptr)) break;
    ptr += slen;

    // Bounds check: need 4 bytes for data length
    if (4 > (size_t)(end - ptr)) break;
    datalen = ((unsigned char)*ptr << 24) | ((unsigned char)*(ptr + 1) << 16) | ((unsigned char)*(ptr + 2) << 8)
              | (unsigned char)*(ptr + 3);
    ptr += 4;

    // Bounds check: validate datalen against remaining buffer
    if (datalen > (size_t)(end - ptr)) break;

    // A malformed resource block is fatal: SIPI is a repository and must
    // not admit corrupt embedded metadata, so the failure propagates to
    // the caller instead of leaving the image with partial metadata.
    switch (id) {
    case 0x0404: {
      if (img->getIptc() == nullptr) {
        if (auto iptc = Iptc::parse((unsigned char *)ptr, datalen)) {
          img->set_iptc(*iptc);
        } else {
          return std::unexpected(iptc.error());
        }
      }
      break;
    }
    case 0x040f: {
      if (img->getIcc() == nullptr) {
        if (auto icc = Icc::parse((unsigned char *)ptr, datalen)) {
          img->set_icc(*icc);
        } else {
          return std::unexpected(icc.error());
        }
      }
      break;
    }
    case 0x0422: {
      if (img->getExif() == nullptr) {
        if (auto exif = Exif::parse((unsigned char *)ptr, datalen)) {
          img->set_exif(*exif);
        } else {
          return std::unexpected(exif.error());
        }
      }
      uint16_t ori;
      if (img->getExif()->getValByKey("Exif.Image.Orientation", ori)) { img->setOrientation(Orientation(ori)); }
      break;
    }
    case 0x0424: {
      if (img->getXmp() == nullptr) img->set_xmp(std::make_shared<Xmp>(ptr, datalen));
      break;  // Fix: was missing, causing fall-through to default
    }
    default: {
      // Unknown resource — skip (removed leaking calloc/memcpy)
      break;
    }
    }

    if ((datalen % 2) == 1) datalen++;
    // The even-padding bump above can push datalen one byte past the bound
    // already checked against `end - ptr`; re-check before advancing.
    if (datalen > static_cast<size_t>(end - ptr)) break;
    ptr += datalen;
  }
  return true;
}

//=============================================================================


//=============================================================================

//=============================================================================


Result<bool> SipiIOJpeg::read(SipiImage *img,
  const std::string &filepath,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size,
  bool force_bps_8,
  ScalingQuality scaling_quality)
{
  SIPI_ZONE_N("SipiIOJpeg::read");
  int infile;
  //
  // open the input file
  //
  if ((infile = ::open(filepath.c_str(), O_RDONLY)) == -1) { return false; }
  FdGuard infile_guard(infile);
  // workaround for bug #0011: jpeglib crashes the app when the file is not a jpeg file
  // we check the magic number before calling any jpeglib routines
  unsigned char magic[2];
  if (::read(infile, magic, 2) != 2) { return false; }
  if ((magic[0] != 0xff) || (magic[1] != 0xd8)) {
    return false;// it's not a JPEG file!
  }
  // move infile position back to the beginning of the file
  ::lseek(infile, 0, SEEK_SET);

  struct jpeg_decompress_struct cinfo {};
  JpegErrorMgr jerr;

  JSAMPARRAY linbuf = nullptr;
  jpeg_saved_marker_ptr marker = nullptr;
  // volatile: assigned inside the setjmp risk window during ICC marker
  // parsing and read by the landing block on longjmp.
  unsigned char *volatile icc_buffer_guard = nullptr;
  // Adobe APP14 transform flag (0=Unknown/CMYK, 1=YCbCr, 2=YCCK); 255 means
  // no APP14 marker was present. Set while parsing markers below and
  // consulted once, after decompression, by the CMYK/YCCK polarity check.
  // Not read by the setjmp landing block, so no volatile qualification is
  // needed here.
  uint8_t app14_transform = 255;

  // Source manager + buffer, owned here and declared before the setjmp so
  // their destructors run on the C++ unwind path.
  FileBuffer file_buffer(infile);
  struct jpeg_source_mgr srcmgr{};

  //
  // let's create the decompressor
  //
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;

  jpeg_create_decompress(&cinfo);
  // JDCT_ISLOW (integer), not JDCT_FLOAT: libjpeg documents the float IDCT as
  // giving different results across machines/compilers (roundoff), while the
  // integer method is bit-exact on all platforms — verified by libjpeg-turbo's
  // own single-MD5 integer tests across x86 SSE2/AVX2 and ARM NEON. This keeps
  // decoded pixels (and the downstream approval goldens) architecture-invariant,
  // matches the encoder's default, and is faster. The <0.1 dB accuracy
  // difference is imperceptible and irrelevant for an archive.
  cinfo.dct_method = JDCT_ISLOW;

  //
  // setjmp error handler — ALL libjpeg errors from this point longjmp to this
  // landing site; C++ exceptions must never cross libjpeg's C frames.
  //
  if (setjmp(jerr.error_jmp)) {
    // longjmp landed here. Resource-owning objects (file_buffer, srcmgr,
    // infile_guard) are declared before the setjmp so their destructors run
    // on the normal C++ path out of this landing block; a longjmp skips the
    // destructors of anything constructed inside the risk window.
    free(icc_buffer_guard);  // may have been allocated during marker parsing
    jpeg_destroy_decompress(&cinfo);
    return std::unexpected(SipiValueError{ ErrorCode::kDecodeFailed,
      "JPEG read failed for \"" + filepath + "\": " + std::string(jerr.error_message) });
  }

  jpeg_file_src(&cinfo, &file_buffer, &srcmgr);
  jpeg_save_markers(&cinfo, JPEG_COM, 0xffff);
  for (int i = 0; i < 16; i++) { jpeg_save_markers(&cinfo, JPEG_APP0 + i, 0xffff); }

  //
  // now we read the header
  //
  int res = jpeg_read_header(&cinfo, static_cast<boolean>(true));
  if (res != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return std::unexpected(
      SipiValueError{ ErrorCode::kMalformedInput, "Error reading JPEG file: \"" + filepath + "\"" });
  }

  auto no_cropping = static_cast<boolean>(false);
  if (region == nullptr) { no_cropping = static_cast<boolean>(true); }
  if ((region != nullptr) && (region->getType()) == SipiRegion::FULL) { no_cropping = static_cast<boolean>(true); }

  size_t nnx, nny;
  SipiSize::SizeType rtype = SipiSize::FULL;
  if (size != nullptr) { rtype = size->getType(); }

  if (no_cropping == 1) {
    //
    // here we prepare tha scaling/reduce stuff...
    //
    int reduce = 3;// maximal reduce factor is 3: 1/1, 1/2, 1/4 and 1/8
    if ((size != nullptr) && (rtype != SipiSize::FULL)) {
      bool redonly = true;// we assume that only a reduce is necessary
      size->get_size(cinfo.image_width, cinfo.image_height, nnx, nny, reduce, redonly);
    } else {
      reduce = 0;
    }

    reduce = std::max(reduce, 0);
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    for (int i = 0; i < reduce; i++) { cinfo.scale_denom *= 2; }
  }
  cinfo.do_fancy_upsampling = static_cast<boolean>(false);

  //
  // set default orientation
  //
  img->setOrientation(TOPLEFT);

  //
  // getting Metadata
  //
  marker = cinfo.marker_list;
  // Use icc_buffer_guard (declared before setjmp) so longjmp cleanup can free it
  unsigned char *volatile &icc_buffer = icc_buffer_guard;
  int icc_buffer_len = 0;
  while (marker != nullptr) {
    if (marker->marker == JPEG_COM) {
      std::string emdatastr(reinterpret_cast<char *>(marker->data), marker->data_length);
      if (emdatastr.compare(0, 5, "SIPI:", 5) == 0) {
        Essentials se = Essentials::parse_legacy(emdatastr);
        img->essential_metadata(se);
      }
    } else if (marker->marker == JPEG_APP0 + 1) {
      // EXIF, XMP MARKER....
      //
      // first we try to find the exif part
      //
      auto *pos = static_cast<unsigned char *>(memmem(marker->data, marker->data_length, "Exif\000\000", 6));
      if (pos != nullptr) {
        // A malformed EXIF blob is fatal: SIPI is a repository and must not
        // admit corrupt embedded metadata.
        if (auto exif = Exif::parse(pos + 6, marker->data_length - (pos - marker->data) - 6)) {
          img->set_exif(*exif);
          uint16_t ori;
          if (img->getExif()->getValByKey("Exif.Image.Orientation", ori)) {
            img->setOrientation(static_cast<Orientation>(ori));
          }
        } else {
          free(icc_buffer);
          icc_buffer = nullptr;
          jpeg_destroy_decompress(&cinfo);
          return std::unexpected(exif.error());
        }
      }

    } else if (marker->marker == JPEG_APP0 + 2) {
      // ICC MARKER.... may span multiple marker segments
      auto *pos = static_cast<unsigned char *>(memmem(marker->data, marker->data_length, "ICC_PROFILE\0", 12));
      if (pos != nullptr) {
        const auto offset = static_cast<unsigned int>(pos - (unsigned char *)marker->data);
        // "ICC_PROFILE\0" (12 bytes) + sequence-number byte + count byte = 14
        // bytes of non-profile overhead precede the profile bytes in this
        // segment. memmem only guarantees the 12-byte identifier fits, not
        // the trailing 2-byte header, so reject a segment too short to hold
        // it before computing `len` — done in unsigned arithmetic so a
        // malformed marker can never underflow into a huge copy length.
        if (marker->data_length < offset + 14) {
          log_err("ICC APP2 marker too short for ICC_PROFILE header (%u bytes)", marker->data_length);
        } else {
          const unsigned int len = marker->data_length - offset - 14;
          auto *newbuf = static_cast<unsigned char *>(realloc(icc_buffer, icc_buffer_len + len));
          if (newbuf == nullptr) {
            // realloc failed — skip this ICC segment, keep what we have
            log_err("realloc failed for ICC buffer (%d bytes)", icc_buffer_len + len);
            break;
          }
          icc_buffer = newbuf;
          memcpy(icc_buffer + icc_buffer_len, pos + 14, (size_t)len);
          icc_buffer_len += len;
        }
      }
    } else if (marker->marker == JPEG_APP0 + 14) {
      // Adobe APP14 marker — 12-byte segment payload (data_length excludes
      // the 2-byte marker and 2-byte length field that precede it):
      //   bytes 0-4  : "Adobe" identifier (NO trailing NUL)
      //   bytes 5-6  : version
      //   bytes 7-8  : flags0
      //   bytes 9-10 : flags1
      //   byte  11   : transform flag (0=Unknown/CMYK, 1=YCbCr, 2=YCCK)
      if (marker->data_length >= 12 && memcmp(marker->data, "Adobe", 5) == 0) {
        app14_transform = static_cast<uint8_t>(marker->data[11]);
      }
    }
    marker = marker->next;
  }
  if (icc_buffer != nullptr) {
    auto icc = Icc::parse(icc_buffer, icc_buffer_len);
    free(icc_buffer);// Icc::parse copies the data
    icc_buffer = nullptr;  // prevent double-free if longjmp fires later
    if (!icc) {
      jpeg_destroy_decompress(&cinfo);
      return std::unexpected(icc.error());
    }
    img->set_icc(*icc);
  }
  if (auto xmp = xmp_packet(cinfo.marker_list)) { img->set_xmp(std::make_shared<Xmp>(*std::move(xmp))); }
  // A malformed resource block is fatal: SIPI is a repository and must not
  // admit corrupt embedded metadata.
  if (auto ps = parse_photoshop_resources(img, photoshop_resources(cinfo.marker_list)); !ps) {
    jpeg_destroy_decompress(&cinfo);
    return std::unexpected(SipiValueError{ ps.error().code(), filepath + ": " + ps.error().raw_message() });
  }

  // icc_buffer is freed and nulled above; errors → longjmp → setjmp handler
  jpeg_start_decompress(&cinfo);

  img->set_geometry(cinfo.output_width, cinfo.output_height, cinfo.output_components, 8);
  if (auto r = validate_decode_dims(img->getNx(), img->getNy(), img->getNc(), static_cast<int>(img->getBps()), filepath);
      !r) {
    jpeg_destroy_decompress(&cinfo);
    return std::unexpected(r.error());
  }
  int colspace = cinfo.out_color_space;
  // JCS_UNKNOWN, JCS_GRAYSCALE, JCS_RGB, JCS_YCbCr, JCS_CMYK, JCS_YCCK
  switch (colspace) {
  case JCS_RGB: {
    img->setPhoto(PhotometricInterpretation::RGB);
    break;
  }
  case JCS_GRAYSCALE: {
    img->setPhoto(PhotometricInterpretation::MINISBLACK);
    break;
  }
  case JCS_CMYK: {
    img->setPhoto(PhotometricInterpretation::SEPARATED);
    break;
  }
  case JCS_YCbCr: {
    img->setPhoto(PhotometricInterpretation::YCBCR);
    break;
  }
  case JCS_YCCK: {
    // libjpeg-turbo decodes YCCK internally to CMYK; the post-read
    // inversion handling is shared with the CMYK path.
    img->setPhoto(PhotometricInterpretation::SEPARATED);
    break;
  }
  case JCS_UNKNOWN: {
    jpeg_destroy_decompress(&cinfo);
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Unsupported JPEG colorspace JCS_UNKNOWN in file \"" + filepath
        + "\" (dimensions: " + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy())
        + ", components: " + std::to_string(cinfo.output_components) + ")" });
  }
  default: {
    jpeg_destroy_decompress(&cinfo);
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Unsupported JPEG colorspace (code: " + std::to_string(colspace) + ") in file \"" + filepath
        + "\" (dimensions: " + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy())
        + ", components: " + std::to_string(cinfo.output_components) + ")" });
  }
  }
  int sll = cinfo.output_components * cinfo.output_width * sizeof(uint8_t);

  // The pixel buffer is owned by img (not a local) for the whole risk
  // window below: jpeg_read_scanlines can longjmp, which skips destructors,
  // so a local std::vector held across that call would leak on error.
  img->set_pixels(std::vector<byte>(static_cast<size_t>(img->getNy()) * sll, 0),
    img->getNx(), img->getNy(), img->getNc(), img->getBps());
  byte *dst = img->pixels_writable().data();

  // All libjpeg calls below — errors → longjmp → setjmp handler above
  linbuf = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE, sll, 1);
  for (size_t i = 0; i < img->getNy(); i++) {
    jpeg_read_scanlines(&cinfo, linbuf, 1);
    memcpy(dst + i * sll, linbuf[0], (size_t)sll);
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  //
  // CMYK / YCCK polarity handling.
  //
  // libjpeg-turbo's CMYK output is inverted when the source declares an
  // Adobe APP14 marker with transform=0 (Photoshop-style "Unknown / CMYK")
  // or transform=2 (YCCK, which libjpeg-turbo converts to inverted CMYK
  // internally). In those cases we re-invert (`v = 255 - v`) so the
  // subsequent ICC conversion sees CMYK in the expected polarity.
  //
  // We deliberately do NOT invert when:
  //   - app14_transform == 1 (YCbCr → RGB, already correct polarity)
  //   - app14_transform == 255 (no APP14 marker — raw CMYK as on disk;
  //     inverting here would corrupt files that do not need it; the R10
  //     `JpegCmykRawNoApp14NotInverted` test pins this branch).
  //
  const bool is_cmyk_path = img->getPhoto() == PhotometricInterpretation::SEPARATED && img->getNc() == 4;
  const bool needs_inversion = is_cmyk_path && (app14_transform == 0 || app14_transform == 2);
  if (needs_inversion) {
    const size_t total_bytes = static_cast<size_t>(img->getNy()) * static_cast<size_t>(sll);
    for (size_t b = 0; b < total_bytes; ++b) { dst[b] = static_cast<byte>(255 - dst[b]); }
  }

  //
  // do some cropping...
  //
  if (no_cropping == 0) {
    // not no cropping (!!) means "do crop"!
    //
    // let's first crop the region (we read the full size image in this case)
    //
    if (const auto cropped = Sipi::processing::crop(*img, region); !cropped) {
      return std::unexpected(cropped.error());
    }

    //
    // no we scale the region to the desired size
    //
    int reduce = -1;
    bool redonly = false;
    (void)size->get_size(img->getNx(), img->getNy(), nnx, nny, reduce, redonly);
  }

  //
  // resize/Scale the image if necessary
  //
  if ((size != nullptr) && (rtype != SipiSize::FULL)) {
    if (auto r = Sipi::processing::scale_with(
          *img, nnx, nny, scaling_method_for(SipiQualityFormat::JPG, scaling_quality));
        !r) {
      return std::unexpected(r.error());
    }
  }

  return true;
}

//============================================================================


#define readbyte(a, b)                      \
  do                                        \
    if (((a) = getc((b))) == EOF) return 0; \
  while (0)
#define readword(a, b)                                                  \
  do {                                                                  \
    int cc_ = 0, dd_ = 0;                                               \
    if ((cc_ = getc((b))) == EOF || (dd_ = getc((b))) == EOF) return 0; \
    (a) = (cc_ << 8) + (dd_);                                           \
  } while (0)


Result<SipiImgInfo> SipiIOJpeg::read_shape(const std::string &filepath)
{
  SIPI_ZONE_N("SipiIOJpeg::read_shape");
  SipiImgInfo info;
  //
  // open the input file
  //
  int raw_fd = ::open(filepath.c_str(), O_RDONLY);
  if (raw_fd == -1) {
    info.success = SipiImgInfo::FAILURE;
    return info;
  }
  FdGuard fd_guard(raw_fd);

  // workaround for bug #0011: jpeglib crashes the app when the file is not a jpeg file
  // we check the magic number before calling any jpeglib routines
  unsigned char magic[2];
  if (::read(raw_fd, magic, 2) != 2) {
    info.success = SipiImgInfo::FAILURE;
    return info;
  }
  if ((magic[0] != 0xff) || (magic[1] != 0xd8)) {
    info.success = SipiImgInfo::FAILURE;
    return info;
  }

  // move infile position back to the beginning of the file
  ::lseek(raw_fd, 0, SEEK_SET);

  struct jpeg_decompress_struct cinfo {};
  JpegErrorMgr jerr;

  jpeg_saved_marker_ptr marker;

  // Source manager + buffer, owned here and declared before the setjmp so
  // their destructors run on the C++ unwind path.
  FileBuffer file_buffer(raw_fd);
  struct jpeg_source_mgr srcmgr{};

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;

  jpeg_create_decompress(&cinfo);
  cinfo.dct_method = JDCT_ISLOW;// integer IDCT — cross-arch bit-exact (see the main read path)

  // setjmp error handler for read_shape — libjpeg errors longjmp here
  if (setjmp(jerr.error_jmp)) {
    jpeg_destroy_decompress(&cinfo);
    info.success = SipiImgInfo::FAILURE;
    return info;
  }

  jpeg_file_src(&cinfo, &file_buffer, &srcmgr);
  jpeg_save_markers(&cinfo, JPEG_COM, 0xffff);
  for (int i = 0; i < 16; i++) { jpeg_save_markers(&cinfo, JPEG_APP0 + i, 0xffff); }

  //
  // now we read the header
  //
  int res = jpeg_read_header(&cinfo, static_cast<boolean>(true));
  if (res != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    info.success = SipiImgInfo::FAILURE;
    return info;
  }

  SipiImage img{};
  //
  // getting Metadata
  //
  marker = cinfo.marker_list;
  while (marker) {
    if (marker->marker == JPEG_COM) {
      std::string emdatastr((char *)marker->data, marker->data_length);
      if (emdatastr.compare(0, 5, "SIPI:", 5) == 0) {
        Essentials se = Essentials::parse_legacy(emdatastr);
        img.essential_metadata(se);
      }
    } else if (marker->marker == JPEG_APP0 + 1) {
      // EXIF, XMP MARKER....
      //
      // first we try to find the exif part
      //
      auto *pos = (unsigned char *)memmem(marker->data, marker->data_length, "Exif\000\000", 6);
      if (pos != nullptr) {
        auto exif = Exif::parse(pos + 6, marker->data_length - (pos - marker->data) - 6);
        if (!exif) {
          jpeg_destroy_decompress(&cinfo);
          return std::unexpected(exif.error());
        }
        img.set_exif(*exif);
      }
    }
    marker = marker->next;
  }
  // A malformed resource block is fatal: SIPI is a repository and must not
  // admit corrupt embedded metadata.
  if (auto ps = parse_photoshop_resources(&img, photoshop_resources(cinfo.marker_list)); !ps) {
    jpeg_destroy_decompress(&cinfo);
    return std::unexpected(SipiValueError{ ps.error().code(), filepath + ": " + ps.error().raw_message() });
  }

  // errors → longjmp → setjmp handler above
  jpeg_start_decompress(&cinfo);

  info.width = cinfo.output_width;
  info.height = cinfo.output_height;
  info.nc = cinfo.num_components;
  info.bps = cinfo.data_precision;
  info.orientation = TOPLEFT;
  if (img.getExif() != nullptr) {
    uint16_t ori;
    if (img.getExif()->getValByKey("Exif.Image.Orientation", ori)) { info.orientation = Orientation(ori); }
  }
  info.success = SipiImgInfo::DIMS;
  jpeg_destroy_decompress(&cinfo);
  // fd_guard closes the file descriptor automatically
  return info;// portions derived from IJG code */
}

//============================================================================


Result<void> SipiIOJpeg::write(SipiImage *img, const OutputSink &sink, const SipiCompressionParams *params)
{
  SIPI_ZONE_N("SipiIOJpeg::write");
  // A streamed sink (callback/tee) writes to the response stream via SinkStream;
  // a FilePath uses libjpeg's native file/stdout writer. `filepath` is derived
  // only for the file branch and the error messages below.
  const bool streaming = is_streaming_sink(sink);
  const std::string filepath = streaming ? std::string("<http response>") : std::get<FilePath>(sink).path;

  int quality = 80;
  if ((params != nullptr) && (!params->empty())) {
    try {
      quality = stoi(params->at(JPEG_QUALITY));
    } catch (const std::out_of_range &er) {
      return std::unexpected(
        SipiValueError{ ErrorCode::kWriteFailed, "JPEG quality argument must be integer between 0 and 100" });
    } catch (const std::invalid_argument &ia) {
      return std::unexpected(
        SipiValueError{ ErrorCode::kWriteFailed, "JPEG quality argument must be integer between 0 and 100" });
    }
    if ((quality < 0) || (quality > 100)) {
      return std::unexpected(
        SipiValueError{ ErrorCode::kWriteFailed, "JPEG quality argument must be integer between 0 and 100" });
    }
  }

  //
  // we have to check if the image has an alpha channel (not supported by JPEG). If
  // so, we remove it!
  //
  const int number_of_alpha_channels = img->getNalpha();
  const int number_of_channels = img->getNc();
  bool three_or_more_channels = number_of_channels > 3;
  bool more_than_zero_alpha_channel = number_of_alpha_channels > 0;

  bool range_valid = three_or_more_channels && more_than_zero_alpha_channel;
  if (range_valid) {
    // we can have an alpha channel and possibly a CMYK image
    if (auto r = processing::removeExtraSamples(*img, false); !r) { return std::unexpected(r.error()); }
  }

  auto icc = Sipi::Icc(Sipi::icc_sRGB);// force sRGB !!
  // only 8 bit JPEGs are supported by the spec
  if (auto r = processing::convertToIcc(*img, icc, 8); !r) { return std::unexpected(r.error()); }

  jpeg_compress_struct cinfo{};
  JpegErrorMgr jerr;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegErrorExit;

  // FdGuard for outfile — constructed outside the setjmp risk window. Its
  // destructor runs during the normal C++ unwind out of this function,
  // including the std::unexpected return path taken from the setjmp landing
  // block below.
  FdGuard outfile_guard(-1);
  // HTTP / file destination owned by the caller via unique_ptr. Constructed
  // outside the setjmp risk window so their destructors run on the normal
  // C++ unwind path on return from the setjmp landing block — a longjmp
  // skips the destructors of anything constructed *between* setjmp and
  // longjmp, and these live outside that window.
  std::unique_ptr<SinkStream> sink_stream;
  std::unique_ptr<HtmlBuffer> html_buffer;
  std::unique_ptr<FileBuffer> file_buffer;
  std::unique_ptr<jpeg_destination_mgr> destmgr;
  JSAMPROW row_pointer[1];
  int row_stride;

  bool use_stdout = false;

  if (streaming) {
    sink_stream = std::make_unique<SinkStream>(sink);
    html_buffer = std::make_unique<HtmlBuffer>(sink_stream.get());
    destmgr = std::make_unique<jpeg_destination_mgr>();
  } else if (filepath == "stdout:") {
    use_stdout = true;
  } else {
    int outfile = open(filepath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (outfile == -1) {
      return std::unexpected(SipiValueError{ ErrorCode::kWriteFailed, "Cannot open file \"" + filepath + "\"!" });
    }
    outfile_guard.fd = outfile;
    file_buffer = std::make_unique<FileBuffer>(outfile);
    destmgr = std::make_unique<jpeg_destination_mgr>();
  }

  jpeg_create_compress(&cinfo);  // errors → longjmp → setjmp handler below

  //
  // Photometric validation and color-space selection happen here, after
  // jpeg_create_compress() but before the setjmp below — see the note there.
  // The chosen color spaces land in locals; the setjmp landing block only
  // assigns them to cinfo.
  //
  J_COLOR_SPACE in_color_space;
  J_COLOR_SPACE jpeg_color_space;
  switch (img->getPhoto()) {
  case PhotometricInterpretation::MINISWHITE:
  case PhotometricInterpretation::MINISBLACK: {
    if (img->getNc() != 1) {
      jpeg_destroy_compress(&cinfo);
      return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
        "Cannot write JPEG: grayscale (MINISBLACK) requires 1 channel, got " + std::to_string(img->getNc())
          + " (dimensions: " + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy())
          + ", bps: " + std::to_string(img->getBps()) + ")" });
    }
    in_color_space = JCS_GRAYSCALE;
    jpeg_color_space = JCS_GRAYSCALE;
    break;
  }
  case PhotometricInterpretation::RGB: {
    if (img->getNc() != 3) {
      jpeg_destroy_compress(&cinfo);
      return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
        "Cannot write JPEG: RGB requires 3 channels, got " + std::to_string(img->getNc()) + " (dimensions: "
          + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy()) + ", bps: "
          + std::to_string(img->getBps()) + ")" });
    }
    in_color_space = JCS_RGB;
    jpeg_color_space = JCS_RGB;
    break;
  }
  case PhotometricInterpretation::SEPARATED: {
    if (img->getNc() != 4) {
      jpeg_destroy_compress(&cinfo);
      return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
        "Cannot write JPEG: CMYK (SEPARATED) requires 4 channels, got " + std::to_string(img->getNc())
          + " (dimensions: " + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy()) + ", bps: "
          + std::to_string(img->getBps()) + ")" });
    }
    in_color_space = JCS_CMYK;
    jpeg_color_space = JCS_CMYK;
    break;
  }
  case PhotometricInterpretation::YCBCR: {
    if (img->getNc() != 3) {
      jpeg_destroy_compress(&cinfo);
      return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
        "Cannot write JPEG: YCbCr requires 3 channels, got " + std::to_string(img->getNc()) + " (dimensions: "
          + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy()) + ", bps: "
          + std::to_string(img->getBps()) + ")" });
    }
    in_color_space = JCS_YCbCr;
    jpeg_color_space = JCS_YCbCr;
    break;
  }
  case PhotometricInterpretation::CIELAB: {
    if (auto r = processing::convertToIcc(*img, Icc(Sipi::PredefinedProfiles::icc_sRGB), 8); !r) {
      jpeg_destroy_compress(&cinfo);
      return std::unexpected(r.error());
    }
    in_color_space = JCS_RGB;
    jpeg_color_space = JCS_RGB;
    break;
  }
  default: {
    jpeg_destroy_compress(&cinfo);
    return std::unexpected(SipiValueError{ ErrorCode::kUnsupportedFormat,
      "Cannot write JPEG: unsupported colorspace " + to_string(img->getPhoto()) + " (dimensions: "
        + std::to_string(img->getNx()) + "x" + std::to_string(img->getNy())
        + ", channels: " + std::to_string(img->getNc()) + ", bps: " + std::to_string(img->getBps()) + ")" });
  }
  }

  //
  // Markers must be written in sequence: APP0, APP1, APP2, ..., APP15. Their
  // payloads are built here, before the setjmp below, into function-scope
  // buffers — see the note there.
  //

  // A marker segment holds at most 65533 bytes. EXIF has no multi-segment
  // form; XMP and IPTC larger than one segment continue in further segments.
  // A block that still cannot be fitted is left out with a warning; it never
  // fails the write, and every block that fits is still written.
  constexpr size_t kMaxSegment = 65533;
  const auto be32 = [](size_t n) {
    return std::array<unsigned char, 4>{ static_cast<unsigned char>(n >> 24),
      static_cast<unsigned char>(n >> 16),
      static_cast<unsigned char>(n >> 8),
      static_cast<unsigned char>(n) };
  };

  std::vector<unsigned char> exif_marker_payload;
  std::shared_ptr<Exif> exif = img->getExif();
  if (exif != nullptr) {
    constexpr std::string_view start("Exif\0\0", 6);
    auto buf = exif->exifBytes(kMaxSegment - start.size());
    if (buf) {
      exif_marker_payload.assign(start.begin(), start.end());
      exif_marker_payload.insert(exif_marker_payload.end(), buf->begin(), buf->end());
    } else {
      log_warn("JPEG written without its EXIF block: %s", buf.error().client_message().c_str());
    }
  }

  // StandardXMP, then the ExtendedXMP segments (XMP Specification Part 3,
  // 1.1.3.1): signature, GUID, full length, offset, part.
  std::vector<std::vector<unsigned char>> xmp_marker_payloads;
  std::shared_ptr<Xmp> xmp = img->getXmp();
  if (xmp != nullptr) {
    constexpr std::string_view start("http://ns.adobe.com/xap/1.0/\0", 29);
    constexpr std::string_view extension("http://ns.adobe.com/xmp/extension/\0", 35);
    std::string standard = xmp->xmpBytes();
    Xmp::Extended split;
    if (start.size() + standard.size() > kMaxSegment) {
      auto s = xmp->split_extended(kMaxSegment - start.size());
      if (s) {
        split = *std::move(s);
        standard = split.standard;
      } else {
        log_warn("JPEG written without its XMP packet (%zu bytes): %s",
          standard.size(),
          s.error().client_message().c_str());
        standard.clear();
      }
    }
    if (!standard.empty()) {
      auto &payload = xmp_marker_payloads.emplace_back(start.begin(), start.end());
      payload.insert(payload.end(), standard.begin(), standard.end());
    }
    const size_t part_size = kMaxSegment - extension.size() - split.guid.size() - 8;
    for (size_t offset = 0; offset < split.extended.size(); offset += part_size) {
      auto &payload = xmp_marker_payloads.emplace_back(extension.begin(), extension.end());
      payload.insert(payload.end(), split.guid.begin(), split.guid.end());
      const auto total = be32(split.extended.size());
      const auto at = be32(offset);
      payload.insert(payload.end(), total.begin(), total.end());
      payload.insert(payload.end(), at.begin(), at.end());
      const size_t n = std::min(part_size, split.extended.size() - offset);
      payload.insert(payload.end(),
        split.extended.begin() + static_cast<std::ptrdiff_t>(offset),
        split.extended.begin() + static_cast<std::ptrdiff_t>(offset + n));
    }
  }

  Essentials es = img->essential_metadata();
  std::shared_ptr<Icc> image_icc = img->getIcc();
  std::vector<std::vector<unsigned char>> icc_marker_chunks;
  if ((image_icc != nullptr) || es.fields().use_icc) {
    std::vector<unsigned char> buf;
    try {
      if (es.fields().use_icc) {
        buf = es.fields().icc_profile;
      } else {
        buf = image_icc->iccBytes();
      }
    } catch (SipiError &err) {
      log_err("Error writing ICC profile in JPEG: %s", err.what());
    }
    unsigned char start[14] = {
      0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x0
    };
    size_t start_l = 14;
    unsigned int n = buf.size() / (65533 - start_l + 1) + 1;

    unsigned int n_towrite = buf.size();
    unsigned int n_nextwrite = 65533 - start_l;
    unsigned int n_written = 0;
    for (unsigned int i = 0; i < n; i++) {
      start[12] = (unsigned char)(i + 1);
      start[13] = (unsigned char)n;
      if (n_nextwrite > n_towrite) n_nextwrite = n_towrite;
      std::vector<unsigned char> chunk(start_l + n_nextwrite);
      memcpy(chunk.data(), start, start_l);
      memcpy(chunk.data() + start_l, buf.data() + n_written, n_nextwrite);
      icc_marker_chunks.push_back(std::move(chunk));
      n_towrite -= n_nextwrite;
      n_written += n_nextwrite;
    }
    if (n_towrite != 0) { log_warn("Incomplete JPEG ICC write: %u bytes remaining", n_towrite); }
  }

  // The APP13 layout parse_photoshop() reads: "Photoshop 3.0\0", then one
  // 8BIM resource 0x0404 (empty name), its length, and the IPTC data padded
  // to an even length. A resource larger than one segment continues in the
  // next APP13 segment after its own "Photoshop 3.0\0"; readers join them.
  std::vector<std::vector<unsigned char>> iptc_marker_payloads;
  std::shared_ptr<Iptc> iptc = img->getIptc();
  if (iptc != nullptr) {
    std::vector<unsigned char> buf = iptc->iptcBytes();
    constexpr std::string_view start("Photoshop 3.0\0", 14);
    std::vector<unsigned char> resource = { '8', 'B', 'I', 'M', 0x04, 0x04, 0, 0 };
    const auto size = be32(buf.size());
    resource.insert(resource.end(), size.begin(), size.end());
    resource.insert(resource.end(), buf.begin(), buf.end());
    if (buf.size() % 2 == 1) { resource.push_back(0); }
    const size_t part_size = kMaxSegment - start.size();
    for (size_t offset = 0; offset < resource.size(); offset += part_size) {
      auto &payload = iptc_marker_payloads.emplace_back(start.begin(), start.end());
      const size_t n = std::min(part_size, resource.size() - offset);
      payload.insert(payload.end(),
        resource.begin() + static_cast<std::ptrdiff_t>(offset),
        resource.begin() + static_cast<std::ptrdiff_t>(offset + n));
    }
  }

  // JPEG is an Access File format per ADR-0009 — it MUST NOT carry the
  // Essentials packet. `es` above feeds only the ICC fallback branch; the
  // COM marker emission has been removed (DEV-6379).

  //
  // setjmp error handler — ALL libjpeg errors from this point longjmp here;
  // C++ exceptions must never cross libjpeg's C frames. A longjmp out of
  // libjpeg skips C++ destructors, so nothing with a destructor may be
  // constructed past this point.
  //
  if (setjmp(jerr.error_jmp)) {
    // longjmp landed here — clean up and return the error in C++ context
    const bool client_aborted = html_buffer && html_buffer->client_aborted;
    jpeg_destroy_compress(&cinfo);
    // html_buffer / destmgr / outfile_guard destructors run during the return unwind
    if (client_aborted) {
      return std::unexpected(
        SipiValueError{ ErrorCode::kClientAbort, "Client aborted HTTP response during JPEG write" });
    }
    return std::unexpected(
      SipiValueError{ ErrorCode::kWriteFailed, "JPEG write failed: " + std::string(jerr.error_message) });
  }

  if (streaming) {
    jpeg_html_dest(&cinfo, html_buffer.get(), destmgr.get());
  } else if (use_stdout) {
    jpeg_stdio_dest(&cinfo, stdout);
  } else {
    jpeg_file_dest(&cinfo, file_buffer.get(), destmgr.get());
  }

  cinfo.image_width = (int)img->getNx();
  cinfo.image_height = (int)img->getNy();
  cinfo.input_components = (int)img->getNc();
  cinfo.in_color_space = in_color_space;
  cinfo.jpeg_color_space = jpeg_color_space;
  cinfo.write_Adobe_marker = TRUE;
  cinfo.write_JFIF_header = TRUE;

  // All libjpeg calls below — errors trigger longjmp to the setjmp handler above
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  // Baseline sequential JPEG, not progressive. libjpeg-turbo's x86 SSE2
  // progressive Huffman encoder (jcphuff) emits "Missing Huffman code table
  // entry" for some scan scripts — its statistics-gather pass and its encode
  // pass disagree on the emitted symbol stream, so a symbol that is written was
  // never assigned a code. This is architecture-divergent (the Arm NEON path is
  // unaffected) and cannot be worked around via optimize_coding, because the
  // disagreement *is* between the two passes optimize_coding runs. Baseline uses
  // the jchuff encoder, which is bit-exact across SIMD implementations and the
  // canonical libjpeg-turbo path. optimize_coding builds compact Huffman tables
  // from the image data (smaller output, still fully deterministic).
  cinfo.optimize_coding = TRUE;
  jpeg_start_compress(&cinfo, TRUE);

  if (!exif_marker_payload.empty()) {
    jpeg_write_marker(
      &cinfo, JPEG_APP0 + 1, exif_marker_payload.data(), static_cast<unsigned int>(exif_marker_payload.size()));
  }
  for (const auto &payload : xmp_marker_payloads) {
    jpeg_write_marker(&cinfo, JPEG_APP0 + 1, payload.data(), static_cast<unsigned int>(payload.size()));
  }
  for (const auto &chunk : icc_marker_chunks) {
    jpeg_write_marker(&cinfo, ICC_MARKER, chunk.data(), static_cast<unsigned int>(chunk.size()));
  }
  for (const auto &payload : iptc_marker_payloads) {
    jpeg_write_marker(&cinfo, JPEG_APP0 + 13, payload.data(), static_cast<unsigned int>(payload.size()));
  }

  row_stride = img->getNx() * img->getNc();
  byte *pixel_data = img->pixels_writable().data();

  while (cinfo.next_scanline < cinfo.image_height) {
    row_pointer[0] = pixel_data + cinfo.next_scanline * row_stride;
    (void)jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  // outfile_guard destructor closes fd
  return {};
}
}// namespace Sipi
