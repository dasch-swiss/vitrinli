/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <gtest/gtest.h>

#include <jansson.h>

#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "cli/SipiReport.h"
#include "logging/logger.h"

namespace {

using JsonPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

JsonPtr parse(const std::string &text)
{
  json_error_t error;
  JsonPtr root{ json_loads(text.c_str(), 0, &error), &json_decref };
  EXPECT_NE(root, nullptr) << error.text << ": " << text;
  return root;
}

std::string field(const JsonPtr &root, const char *key)
{
  const json_t *value = json_object_get(root.get(), key);
  return json_is_string(value) ? json_string_value(value) : std::string{ "<missing>" };
}

std::string report(std::optional<std::string> message = std::nullopt,
  std::optional<std::string> phase = std::nullopt,
  std::string error_code = "internal")
{
  Sipi::observability::ImageContext ctx;
  ctx.input_file = "in.tif";
  std::ostringstream out;
  Sipi::emit_json_report(out, ctx, std::move(message), std::move(phase), std::move(error_code));
  return out.str();
}

class SipiReportTest : public ::testing::Test
{
protected:
  void TearDown() override { set_correlation_id(""); }
};

TEST_F(SipiReportTest, EveryErrorCodeHasItsName)
{
  using Sipi::ErrorCode;
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kDecodeFailed), "decode_failed");
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kUnsupportedFormat), "unsupported_format");
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kMalformedInput), "malformed_input");
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kWriteFailed), "write_failed");
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kClientAbort), "client_abort");
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kShapeProbeFailed), "shape_probe_failed");
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kMetadataParseFailed), "metadata_parse_failed");
  EXPECT_STREQ(Sipi::error_code_name(ErrorCode::kInvalidRequestParameter), "invalid_request_parameter");
}

TEST_F(SipiReportTest, ErrorDocumentCarriesCodeAndCorrelationId)
{
  set_correlation_id("e2e-123");
  const auto root = parse(report("bad block", "read", "metadata_parse_failed"));
  EXPECT_EQ(field(root, "status"), "error");
  EXPECT_EQ(field(root, "phase"), "read");
  EXPECT_EQ(field(root, "error_code"), "metadata_parse_failed");
  EXPECT_EQ(field(root, "correlation_id"), "e2e-123");
  EXPECT_EQ(field(root, "error_message"), "bad block");
}

TEST_F(SipiReportTest, ErrorCodeDefaultsToInternal)
{
  const auto root = parse(report("boom", "convert"));
  EXPECT_EQ(field(root, "error_code"), "internal");
}

TEST_F(SipiReportTest, CorrelationIdIsEmptyWhenNotSet)
{
  const auto error = parse(report("boom", "write", "write_failed"));
  EXPECT_EQ(field(error, "correlation_id"), "");
  const auto ok = parse(report());
  EXPECT_EQ(field(ok, "correlation_id"), "");
}

TEST_F(SipiReportTest, SuccessDocumentHasCorrelationIdAndNoErrorCode)
{
  set_correlation_id("abc");
  const auto root = parse(report());
  EXPECT_EQ(field(root, "status"), "ok");
  EXPECT_EQ(field(root, "correlation_id"), "abc");
  EXPECT_EQ(json_object_get(root.get(), "error_code"), nullptr);
}

TEST_F(SipiReportTest, CliArgErrorUsesTheCliArgsCode)
{
  set_correlation_id("abc");
  std::ostringstream out;
  Sipi::emit_json_cli_arg_error(out, "bad flag");
  const auto root = parse(out.str());
  EXPECT_EQ(field(root, "phase"), "cli_args");
  EXPECT_EQ(field(root, "error_code"), "cli_args");
  EXPECT_EQ(field(root, "correlation_id"), "abc");
  EXPECT_EQ(json_object_get(root.get(), "image"), nullptr);
}

TEST_F(SipiReportTest, FallbackDocumentCarriesCodeAndCorrelationId)
{
  // jansson's json_pack rejects a string that is not valid UTF-8, so the
  // report falls back to its hand-built document, keeping phase and code.
  set_correlation_id("abc");
  const auto root = parse(report(std::string{ "\xff" }, "read", "decode_failed"));
  EXPECT_EQ(field(root, "status"), "error");
  EXPECT_EQ(field(root, "phase"), "read");
  EXPECT_EQ(field(root, "error_code"), "decode_failed");
  EXPECT_EQ(field(root, "correlation_id"), "abc");
  EXPECT_EQ(field(root, "error_message"), "SipiReport: json_pack failed");
}

}// namespace
