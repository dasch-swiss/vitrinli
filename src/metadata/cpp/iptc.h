/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * This file implements the virtual abstract class which implements the image file I/O.
 */
#ifndef SIPI_METADATA_IPTC_H
#define SIPI_METADATA_IPTC_H

#include <exiv2/iptc.hpp>
#include <memory>
#include <string>
#include <vector>

#include "error/SipiValueError.h"

namespace Sipi {

/*!
 * Handles IPTC data based on the exiv2 library
 */
class Iptc
{
private:
  Exiv2::IptcData iptcData;//!< Private member variable holding the exiv2 IPTC object

  explicit Iptc(Exiv2::IptcData data);

public:
  /*!
   * Parses a buffer of IPTC data in native format.
   *
   * \param[in] iptc Buffer containing the IPTC data in native format
   * \param[in] len Length of the buffer
   * \return The parsed Iptc instance, or a SipiValueError if the buffer does not hold valid IPTC data.
   */
  [[nodiscard]] static Result<std::shared_ptr<Iptc>> parse(const unsigned char *iptc, unsigned int len);

  /*!
   * Like parse(), but kMetadataParseFailed unless the buffer is a sequence
   * of at least one whole IIM dataset, trailing zero padding aside, and Exiv2
   * decodes every one. parse() accepts a buffer Exiv2 reads only in part:
   * Exiv2 skips bytes that are no dataset and drops one that runs past the end.
   */
  [[nodiscard]] static Result<std::shared_ptr<Iptc>> parse_complete(const unsigned char *iptc, unsigned int len);

  /*!
   * Destructor
   */
  ~Iptc();

  /*!
   * Returns the bytes of the IPTC data as a std::vector.
   * @return IPTC bytes as std::vector
   */
  std::vector<unsigned char> iptcBytes();

  /*!
   * Removes the embedded preview image (Iptc.Application2.Preview with its
   * PreviewFormat and PreviewVersion).
   */
  void erase_preview();

  /*!
   * The overloaded << operator which is used to write the IPTC data formatted to the outstream
   *
   * \param[in] lhs The output stream
   * \param[in] rhs Reference to an instance of a Iptc
   * \returns Returns ostream object
   */
  friend std::ostream &operator<<(std::ostream &lhs, Iptc &rhs);
};

}// namespace Sipi

#endif
