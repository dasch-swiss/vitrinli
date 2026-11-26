/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "cli/commands/decode_deadline.h"

#include <chrono>
#include <utility>

#include "ffi/decode_guard.h"

namespace Sipi::cli {

std::optional<DeadlineDecode> read_source_with_deadline(std::size_t timeout_ms,
  const std::string &path,
  std::shared_ptr<SipiRegion> region,
  std::shared_ptr<SipiSize> size)
{
  // Captures by value and returns by value, as run_with_deadline requires: on
  // timeout this frame is gone while the detached worker is still decoding.
  return ffi::run_with_deadline(
    std::chrono::milliseconds(timeout_ms), [path, region = std::move(region), size = std::move(size)] {
      SipiImage img;
      auto status = img.readSource(path, region, size);
      return DeadlineDecode{ std::move(img), std::move(status) };
    });
}

std::optional<DeadlineDecode> read_with_deadline(std::size_t timeout_ms, const std::string &path)
{
  return ffi::run_with_deadline(std::chrono::milliseconds(timeout_ms), [path] {
    SipiImage img;
    auto status = img.read(path);
    return DeadlineDecode{ std::move(img), std::move(status) };
  });
}

std::optional<DeadlineDecode> read_metadata_with_deadline(std::size_t timeout_ms, const std::string &path)
{
  return ffi::run_with_deadline(std::chrono::milliseconds(timeout_ms), [path] {
    SipiImage img;
    auto status = img.read_metadata(path);
    return DeadlineDecode{ std::move(img), std::move(status) };
  });
}

std::string decode_deadline_message(std::size_t timeout_ms)
{
  return "decode exceeded the " + std::to_string(timeout_ms) + " ms deadline; the wedged decode thread is abandoned";
}

}// namespace Sipi::cli
