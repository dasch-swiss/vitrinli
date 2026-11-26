/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef SIPI_CLI_COMMANDS_DECODE_DEADLINE_H
#define SIPI_CLI_COMMANDS_DECODE_DEADLINE_H

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "iiifparser/SipiRegion.h"
#include "iiifparser/SipiSize.h"
#include "image/SipiImage.h"

namespace Sipi::cli {

//! A read that finished before its deadline: the image and the read's result.
struct DeadlineDecode
{
  SipiImage img;
  Result<void> status;
};

/*!
 * `SipiImage::readSource(path, region, size)` on a fresh image under a
 * deadline of `timeout_ms` (callers pass `Sipi::ffi::kDefaultDecodeTimeoutMs`).
 * The offline verbs decode through the same Kakadu path the server does, so
 * the unpatchable JP2 hang class (DEV-7080) reaches them too; without a
 * deadline `sipi convert` on a hostile file wedges the operator's shell (or
 * the ingest worker driving it) forever. `std::nullopt` on timeout — the decode thread is
 * abandoned and keeps running until the process exits, so the caller reports
 * and returns non-zero rather than retrying. Exceptions thrown by the decode
 * propagate to the caller as before.
 */
[[nodiscard]] std::optional<DeadlineDecode> read_source_with_deadline(std::size_t timeout_ms,
  const std::string &path,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size);

//! `SipiImage::read(path)` under a deadline; same contract as above.
[[nodiscard]] std::optional<DeadlineDecode> read_with_deadline(std::size_t timeout_ms, const std::string &path);

//! `SipiImage::read_metadata(path)` under a deadline; same contract as above.
[[nodiscard]] std::optional<DeadlineDecode> read_metadata_with_deadline(std::size_t timeout_ms,
  const std::string &path);

//! The operator-facing message for a decode that hit `timeout_ms`.
[[nodiscard]] std::string decode_deadline_message(std::size_t timeout_ms);

}// namespace Sipi::cli

#endif// SIPI_CLI_COMMANDS_DECODE_DEADLINE_H
