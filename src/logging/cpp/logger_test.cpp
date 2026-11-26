#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logging/logger.h"
#include "gtest/gtest.h"

// ---- helpers to capture stdout / stderr ----

static std::string capture_fd(int target_fd, std::function<void()> action)
{
  // Flush before redirecting
  fflush(stdout);
  fflush(stderr);

  // Create a temporary file to capture output
  char tmpname[] = "/tmp/logger_test_XXXXXX";
  int tmpfd = mkstemp(tmpname);
  EXPECT_GE(tmpfd, 0);

  // Save original fd
  int saved_fd = dup(target_fd);
  EXPECT_GE(saved_fd, 0);

  // Redirect target fd to temp file
  dup2(tmpfd, target_fd);

  // Execute the action
  action();

  // Flush again to make sure everything is written
  fflush(stdout);
  fflush(stderr);

  // Restore original fd
  dup2(saved_fd, target_fd);
  close(saved_fd);

  // Read back the captured output
  lseek(tmpfd, 0, SEEK_SET);
  char buf[4096] = {};
  ssize_t n = read(tmpfd, buf, sizeof(buf) - 1);
  close(tmpfd);
  unlink(tmpname);

  return (n > 0) ? std::string(buf, n) : std::string();
}

static std::string capture_stdout(std::function<void()> action) { return capture_fd(STDOUT_FILENO, action); }
static std::string capture_stderr(std::function<void()> action) { return capture_fd(STDERR_FILENO, action); }

// ---- JSON validation helper ----

static bool valid_json(const char *text)
{
  json_error_t error;
  json_t *root = json_loads(text, 0, &error);
  if (root) {
    json_decref(root);
    return true;
  } else {
    fprintf(stderr, "json error on line %d: %s\n", error.line, error.text);
    return false;
  }
}

// ---- Test fixture that saves/restores global Logger state ----

class LoggerTest : public ::testing::Test {
protected:
  void SetUp() override
  {
    saved_cli_mode_ = is_cli_mode();
    saved_log_level_ = get_log_level();
  }

  void TearDown() override
  {
    set_cli_mode(saved_cli_mode_);
    set_log_level(saved_log_level_);
  }

private:
  bool saved_cli_mode_;
  LogLevel saved_log_level_;
};

// ================================================================
// log_sformat tests (string formatting, no I/O)
// ================================================================

TEST_F(LoggerTest, SformatProducesValidJson)
{
  auto out = log_sformat(LL_DEBUG, "The \"%i\" cranks are turning!", 7);
  auto should = "{\"level\": \"DEBUG\", \"message\": \"The \\\"7\\\" cranks are turning!\"}\n";
  EXPECT_EQ(out, should);
  EXPECT_TRUE(valid_json(out.c_str()));
}

TEST_F(LoggerTest, SformatAllLevels)
{
  // Verify that log_sformat produces the correct level string for each level
  struct {
    LogLevel level;
    const char *expected_label;
  } cases[] = {
    {LL_DEBUG, "DEBUG"},
    {LL_INFO, "INFO"},
    {LL_NOTICE, "NOTICE"},
    {LL_WARNING, "WARN"},
    {LL_ERR, "ERROR"},
    {LL_CRIT, "CRIT"},
    {LL_ALERT, "ALERT"},
    {LL_EMERG, "EMERG"},
  };

  for (auto &tc : cases) {
    auto out = log_sformat(tc.level, "test message");
    EXPECT_TRUE(valid_json(out.c_str())) << "Invalid JSON for level " << tc.level;

    // Parse and verify level field
    json_error_t error;
    json_t *root = json_loads(out.c_str(), 0, &error);
    ASSERT_NE(root, nullptr) << "Failed to parse JSON for level " << tc.level;
    json_t *level_val = json_object_get(root, "level");
    ASSERT_NE(level_val, nullptr);
    EXPECT_STREQ(json_string_value(level_val), tc.expected_label) << "Wrong label for level " << tc.level;
    json_decref(root);
  }
}

TEST_F(LoggerTest, SformatEscapesSpecialCharacters)
{
  // Test backslash escaping
  auto out = log_sformat(LL_INFO, "path: C:\\foo\\bar");
  EXPECT_TRUE(valid_json(out.c_str()));

  // Test newline escaping
  out = log_sformat(LL_INFO, "line1\nline2");
  EXPECT_TRUE(valid_json(out.c_str()));

  // Test tab escaping
  out = log_sformat(LL_INFO, "col1\tcol2");
  EXPECT_TRUE(valid_json(out.c_str()));
}

// ================================================================
// CLI mode getter/setter tests
// ================================================================

TEST_F(LoggerTest, CliModeDefaultIsFalse)
{
  // Fixture restores state, but the default compiled-in value is false
  set_cli_mode(false);
  EXPECT_FALSE(is_cli_mode());
}

TEST_F(LoggerTest, SetCliModeTrue)
{
  set_cli_mode(true);
  EXPECT_TRUE(is_cli_mode());
}

TEST_F(LoggerTest, SetCliModeToggle)
{
  set_cli_mode(true);
  EXPECT_TRUE(is_cli_mode());
  set_cli_mode(false);
  EXPECT_FALSE(is_cli_mode());
}

// ================================================================
// Log level getter/setter tests
// ================================================================

TEST_F(LoggerTest, LogLevelDefaultIsInfo)
{
  set_log_level(LL_INFO);
  EXPECT_EQ(get_log_level(), LL_INFO);
}

TEST_F(LoggerTest, SetLogLevelDebug)
{
  set_log_level(LL_DEBUG);
  EXPECT_EQ(get_log_level(), LL_DEBUG);
}

TEST_F(LoggerTest, SetLogLevelAllValues)
{
  LogLevel levels[] = {LL_DEBUG, LL_INFO, LL_NOTICE, LL_WARNING, LL_ERR, LL_CRIT, LL_ALERT, LL_EMERG};
  for (auto ll : levels) {
    set_log_level(ll);
    EXPECT_EQ(get_log_level(), ll);
  }
}

TEST_F(LoggerTest, ParseLogLevelCanonicalNames)
{
  EXPECT_EQ(parse_log_level("DEBUG", LL_INFO), LL_DEBUG);
  EXPECT_EQ(parse_log_level("INFO", LL_ERR), LL_INFO);
  EXPECT_EQ(parse_log_level("NOTICE", LL_ERR), LL_NOTICE);
  EXPECT_EQ(parse_log_level("WARNING", LL_ERR), LL_WARNING);
  EXPECT_EQ(parse_log_level("ERR", LL_INFO), LL_ERR);
  EXPECT_EQ(parse_log_level("CRIT", LL_INFO), LL_CRIT);
  EXPECT_EQ(parse_log_level("ALERT", LL_INFO), LL_ALERT);
  EXPECT_EQ(parse_log_level("EMERG", LL_INFO), LL_EMERG);
}

TEST_F(LoggerTest, ParseLogLevelIsCaseInsensitive)
{
  EXPECT_EQ(parse_log_level("debug", LL_INFO), LL_DEBUG);
  EXPECT_EQ(parse_log_level("Warning", LL_INFO), LL_WARNING);
}

TEST_F(LoggerTest, ParseLogLevelUnknownReturnsFallback)
{
  EXPECT_EQ(parse_log_level("", LL_INFO), LL_INFO);
  EXPECT_EQ(parse_log_level("WARN", LL_INFO), LL_INFO);// not a canonical name
  EXPECT_EQ(parse_log_level("nonsense", LL_ERR), LL_ERR);
}

// ================================================================
// Log level filtering tests
// ================================================================

TEST_F(LoggerTest, FilteringSuppressesDebugWhenLevelIsInfo)
{
  set_log_level(LL_INFO);
  set_cli_mode(false);

  auto out = capture_stdout([]() { log_debug("this should be suppressed"); });
  EXPECT_TRUE(out.empty()) << "DEBUG should be suppressed when level is INFO, got: " << out;
}

TEST_F(LoggerTest, FilteringAllowsDebugWhenLevelIsDebug)
{
  set_log_level(LL_DEBUG);
  set_cli_mode(false);

  auto out = capture_stdout([]() { log_debug("debug message visible"); });
  EXPECT_FALSE(out.empty()) << "DEBUG should be visible when level is DEBUG";
  EXPECT_TRUE(valid_json(out.c_str()));
}

TEST_F(LoggerTest, FilteringAllowsInfoWhenLevelIsInfo)
{
  set_log_level(LL_INFO);
  set_cli_mode(false);

  auto out = capture_stdout([]() { log_info("info message"); });
  EXPECT_FALSE(out.empty()) << "INFO should be visible when level is INFO";
  EXPECT_TRUE(valid_json(out.c_str()));
}

TEST_F(LoggerTest, FilteringSuppressesInfoWhenLevelIsWarning)
{
  set_log_level(LL_WARNING);
  set_cli_mode(false);

  auto out = capture_stdout([]() { log_info("should be suppressed"); });
  EXPECT_TRUE(out.empty()) << "INFO should be suppressed when level is WARNING, got: " << out;
}

TEST_F(LoggerTest, FilteringAllowsWarnWhenLevelIsWarning)
{
  set_log_level(LL_WARNING);
  set_cli_mode(false);

  auto out = capture_stdout([]() { log_warn("warning message"); });
  EXPECT_FALSE(out.empty()) << "WARNING should be visible when level is WARNING";
}

TEST_F(LoggerTest, FilteringAllowsErrWhenLevelIsWarning)
{
  set_log_level(LL_WARNING);
  set_cli_mode(false);

  auto out = capture_stdout([]() { log_err("error message"); });
  EXPECT_FALSE(out.empty()) << "ERR should be visible when level is WARNING";
}

TEST_F(LoggerTest, FilteringSuppressesAllBelowEmerg)
{
  set_log_level(LL_EMERG);
  set_cli_mode(false);

  auto out = capture_stdout([]() {
    log_debug("d");
    log_info("i");
    log_warn("w");
    log_err("e");
  });
  EXPECT_TRUE(out.empty()) << "All levels below EMERG should be suppressed, got: " << out;
}

TEST_F(LoggerTest, FilteringPassesAllWhenLevelIsDebug)
{
  set_log_level(LL_DEBUG);
  set_cli_mode(false);

  auto out = capture_stdout([]() {
    log_debug("d");
    log_info("i");
    log_warn("w");
  });
  // Should contain 3 JSON lines
  int newlines = 0;
  for (char c : out) {
    if (c == '\n') newlines++;
  }
  EXPECT_EQ(newlines, 3) << "Expected 3 log lines, got output: " << out;
}

// ================================================================
// Server mode output tests (JSON to stdout)
// ================================================================

TEST_F(LoggerTest, ServerModeOutputsJsonToStdout)
{
  set_cli_mode(false);
  set_log_level(LL_DEBUG);

  auto out = capture_stdout([]() { log_info("server json test"); });
  EXPECT_FALSE(out.empty());
  EXPECT_TRUE(valid_json(out.c_str()));
  EXPECT_NE(out.find("\"level\": \"INFO\""), std::string::npos);
  EXPECT_NE(out.find("server json test"), std::string::npos);
}

TEST_F(LoggerTest, ServerModeErrorsAlsoGoToStdout)
{
  set_cli_mode(false);
  set_log_level(LL_DEBUG);

  auto out = capture_stdout([]() { log_err("server error"); });
  EXPECT_FALSE(out.empty());
  EXPECT_TRUE(valid_json(out.c_str()));
  EXPECT_NE(out.find("\"level\": \"ERROR\""), std::string::npos);

  // In server mode, nothing should go to stderr
  auto err = capture_stderr([]() { log_err("server error 2"); });
  EXPECT_TRUE(err.empty()) << "Server mode should not write to stderr, got: " << err;
}

// ================================================================
// CLI mode output tests (plain text, errors to stderr)
// ================================================================

TEST_F(LoggerTest, CliModeOutputsPlainTextToStdout)
{
  set_cli_mode(true);
  set_log_level(LL_DEBUG);

  auto out = capture_stdout([]() { log_info("cli info message"); });
  EXPECT_NE(out.find("cli info message"), std::string::npos);
  // CLI mode should NOT produce JSON
  EXPECT_EQ(out.find("{\"level\""), std::string::npos) << "CLI mode should output plain text, not JSON";
}

TEST_F(LoggerTest, CliModeErrorsGoToStderr)
{
  set_cli_mode(true);
  set_log_level(LL_DEBUG);

  auto err = capture_stderr([]() { log_err("cli error message"); });
  EXPECT_NE(err.find("cli error message"), std::string::npos);

  // Errors should NOT go to stdout in CLI mode
  auto out = capture_stdout([]() { log_err("cli error message 2"); });
  EXPECT_TRUE(out.empty()) << "CLI mode errors should go to stderr, not stdout, got: " << out;
}

TEST_F(LoggerTest, CliModeDebugGoesToStdout)
{
  set_cli_mode(true);
  set_log_level(LL_DEBUG);

  auto out = capture_stdout([]() { log_debug("cli debug msg"); });
  EXPECT_NE(out.find("cli debug msg"), std::string::npos);
}

TEST_F(LoggerTest, CliModeWarnGoesToStdout)
{
  set_cli_mode(true);
  set_log_level(LL_DEBUG);

  auto out = capture_stdout([]() { log_warn("cli warn msg"); });
  EXPECT_NE(out.find("cli warn msg"), std::string::npos);
}

// ================================================================
// Combined CLI + filtering tests
// ================================================================

TEST_F(LoggerTest, CliModeRespectsLogLevelFiltering)
{
  set_cli_mode(true);
  set_log_level(LL_WARNING);

  auto out = capture_stdout([]() { log_info("filtered info"); });
  EXPECT_TRUE(out.empty()) << "INFO should be filtered in CLI mode when level=WARNING, got: " << out;

  auto err = capture_stderr([]() { log_debug("filtered debug"); });
  EXPECT_TRUE(err.empty()) << "DEBUG should be filtered in CLI mode when level=WARNING";

  // Warning should still pass
  out = capture_stdout([]() { log_warn("visible warning"); });
  EXPECT_NE(out.find("visible warning"), std::string::npos);

  // Error should still pass (to stderr)
  err = capture_stderr([]() { log_err("visible error"); });
  EXPECT_NE(err.find("visible error"), std::string::npos);
}

// ================================================================
// log_format tests (generic level + message)
// ================================================================

TEST_F(LoggerTest, LogFormatWithExplicitLevel)
{
  set_cli_mode(false);
  set_log_level(LL_DEBUG);

  auto out = capture_stdout([]() { log_format(LL_NOTICE, "notice via log_format %d", 42); });
  EXPECT_FALSE(out.empty());
  EXPECT_TRUE(valid_json(out.c_str()));
  EXPECT_NE(out.find("notice via log_format 42"), std::string::npos);
  EXPECT_NE(out.find("\"level\": \"NOTICE\""), std::string::npos);
}

TEST_F(LoggerTest, LogFormatRespectsFiltering)
{
  set_cli_mode(false);
  set_log_level(LL_ERR);

  auto out = capture_stdout([]() { log_format(LL_INFO, "should be filtered"); });
  EXPECT_TRUE(out.empty()) << "log_format should respect level filtering";
}

// ================================================================
// Outbound traceparent (distributed-trace propagation) tests
// ================================================================
//
// The Rust shell stamps the outbound `traceparent` here; the Lua HTTP client
// reads it back and injects it on outbound requests. The setter validates the
// W3C shape at this FFI boundary, so a malformed or injected value can never
// reach a header. These run on the test thread; each clears the thread-local
// when done.

static const char *const kValidTraceparent = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";

TEST_F(LoggerTest, OutboundTraceparentValidRoundTrips)
{
  set_outbound_traceparent(kValidTraceparent);
  EXPECT_EQ(get_outbound_traceparent(), std::string(kValidTraceparent));
  set_outbound_traceparent(nullptr);
}

TEST_F(LoggerTest, OutboundTraceparentNullClears)
{
  set_outbound_traceparent(kValidTraceparent);
  set_outbound_traceparent(nullptr);
  EXPECT_TRUE(get_outbound_traceparent().empty());
}

TEST_F(LoggerTest, OutboundTraceparentRejectsMalformed)
{
  const char *bad[] = {
    "",// empty
    "not-a-traceparent",
    "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331",// missing flags
    "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-1",// 1-char flags
    "00_0af7651916cd43dd8448eb211c80319c_b7ad6b7169203331_01",// wrong separators
    "00-0AF7651916CD43DD8448EB211C80319C-b7ad6b7169203331-01",// uppercase hex
    "00-0af7651916cd43dd8448eb211c80319g-b7ad6b7169203331-01",// non-hex 'g'
    "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01\r\nEvil: 1",// CRLF injection
  };
  for (const char *v : bad) {
    // Start from a known-good value; a rejected set must CLEAR it, never keep it.
    set_outbound_traceparent(kValidTraceparent);
    set_outbound_traceparent(v);
    EXPECT_TRUE(get_outbound_traceparent().empty()) << "should reject: " << v;
  }
  set_outbound_traceparent(nullptr);
}

// ================================================================
// Correlation ID
// ================================================================

TEST_F(LoggerTest, CliModeLineStartsWithTheCorrelationId)
{
  set_cli_mode(true);
  set_log_level(LL_DEBUG);
  set_correlation_id("e2e-123");

  auto err = capture_stderr([]() { log_err("first line\nsecond line"); });
  EXPECT_EQ(err, "[e2e-123] first line\nsecond line\n");
  auto out = capture_stdout([]() { log_info("info line"); });
  EXPECT_EQ(out, "[e2e-123] info line\n");

  set_correlation_id("");
  auto plain = capture_stderr([]() { log_err("no id"); });
  EXPECT_EQ(plain, "no id\n");
}

TEST_F(LoggerTest, ServerModeLineHasNoCorrelationId)
{
  set_cli_mode(false);
  set_log_level(LL_DEBUG);
  set_correlation_id("e2e-123");

  auto out = capture_stdout([]() { log_info("server line"); });
  set_correlation_id("");
  EXPECT_TRUE(valid_json(out.c_str()));
  EXPECT_EQ(out.find("e2e-123"), std::string::npos) << out;
}
