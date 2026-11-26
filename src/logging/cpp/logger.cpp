#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "logger.h"

static bool g_cli_mode = false;
// `g_json_mode` is read from every request worker thread in server mode and
// from the main thread in CLI mode. It is set once at startup (before any
// threads exist) and never re-set, so `memory_order_relaxed` is sufficient
// — but using `std::atomic` rather than a plain bool makes the code
// formally data-race-free per the C++ memory model.
static std::atomic<bool> g_json_mode{ false };
static LogLevel g_log_level = LL_INFO;
// The active distributed-trace context for the current thread, set by the Rust
// shell across the FFI before each blocking serve call and cleared after. Each
// engine worker runs on one thread for the whole call, so thread-local exactly
// scopes the context; empty means "no active trace" (the keys are then omitted).
static thread_local std::string g_trace_id;
static thread_local std::string g_span_id;
// The W3C `traceparent` this thread injects on outbound Lua HTTP calls, so a
// downstream service continues the trace. Set by the Rust shell from the active
// span before a preflight/route FFI call and cleared after; empty means "do not
// inject". Distinct from the log ids above: propagation needs the full header
// (incl. the sampling flag), log lines need only the raw ids.
static thread_local std::string g_outbound_traceparent;
static std::string g_correlation_id;

void set_cli_mode(bool cli) { g_cli_mode = cli; }
bool is_cli_mode() { return g_cli_mode; }
void set_log_level(LogLevel level) { g_log_level = level; }
LogLevel get_log_level() { return g_log_level; }

LogLevel parse_log_level(const std::string &name, LogLevel fallback)
{
  std::string upper(name);
  std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  if (upper == "DEBUG") return LL_DEBUG;
  if (upper == "INFO") return LL_INFO;
  if (upper == "NOTICE") return LL_NOTICE;
  if (upper == "WARNING") return LL_WARNING;
  if (upper == "ERR") return LL_ERR;
  if (upper == "CRIT") return LL_CRIT;
  if (upper == "ALERT") return LL_ALERT;
  if (upper == "EMERG") return LL_EMERG;
  return fallback;
}
void set_json_mode(bool enabled) { g_json_mode.store(enabled, std::memory_order_relaxed); }
bool is_json_mode() { return g_json_mode.load(std::memory_order_relaxed); }

// W3C trace/span ids are fixed-width lowercase hex. Validating the pair at this
// FFI boundary lets log_vsformat splice them into the JSON line without escaping
// and rejects (clears) any malformed value — boundary validation, so the safety
// never depends on a caller's discipline.
static bool is_lower_hex(const std::string &s, std::size_t len)
{
  if (s.size() != len) return false;
  for (const char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

void set_log_trace_context(const char *trace_id, const char *span_id)
{
  const std::string t = (trace_id != nullptr) ? trace_id : "";
  const std::string s = (span_id != nullptr) ? span_id : "";
  // Accept only a valid (trace_id, span_id) hex pair; anything else — including
  // the NULL/empty clear — clears both, so log_vsformat emits the keys only when
  // both ids are present and safe to append raw.
  if (is_lower_hex(t, 32) && is_lower_hex(s, 16)) {
    g_trace_id = t;
    g_span_id = s;
  } else {
    g_trace_id.clear();
    g_span_id.clear();
  }
}

// A W3C `traceparent` is `<version>-<trace-id>-<parent-id>-<flags>` with fixed
// widths — version 2 hex, trace-id 32 hex, parent-id 16 hex, flags 2 hex, dashes
// at positions 2/35/52 (55 chars total). Validating the whole shape at this FFI
// boundary lets CurlConnection splice it into an outbound header without escaping
// and makes header injection impossible: any CR/LF or non-hex byte fails.
static bool is_w3c_traceparent(const std::string &tp)
{
  if (tp.size() != 55) return false;
  if (tp[2] != '-' || tp[35] != '-' || tp[52] != '-') return false;
  return is_lower_hex(tp.substr(0, 2), 2)     // version
         && is_lower_hex(tp.substr(3, 32), 32)// trace-id
         && is_lower_hex(tp.substr(36, 16), 16)// parent-id
         && is_lower_hex(tp.substr(53, 2), 2);// flags
}

void set_outbound_traceparent(const char *traceparent)
{
  const std::string tp = (traceparent != nullptr) ? traceparent : "";
  if (is_w3c_traceparent(tp)) {
    g_outbound_traceparent = tp;
  } else {
    g_outbound_traceparent.clear();
  }
}

std::string get_outbound_traceparent() { return g_outbound_traceparent; }

void set_correlation_id(const std::string &id) { g_correlation_id = id; }
const std::string &get_correlation_id() { return g_correlation_id; }

std::string escape_json_str(const std::string &s)
{
  std::ostringstream o;
  for (auto c = s.cbegin(); c != s.cend(); c++) {
    switch (*c) {
    case '"':
      o << "\\\"";
      break;
    case '\\':
      o << "\\\\";
      break;
    case '\b':
      o << "\\b";
      break;
    case '\f':
      o << "\\f";
      break;
    case '\n':
      o << "\\n";
      break;
    case '\r':
      o << "\\r";
      break;
    case '\t':
      o << "\\t";
      break;
    default:
      if ('\x00' <= *c && *c <= '\x1f') {
        o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(*c);
      } else {
        o << *c;
      }
    }
  }
  return o.str();
}

inline const char *LogLevelToString(LogLevel ll)
{
  switch (ll) {
  case LL_DEBUG:
    return "DEBUG";
  case LL_INFO:
    return "INFO";
  case LL_NOTICE:
    return "NOTICE";
  case LL_WARNING:
    return "WARN";
  case LL_ERR:
    return "ERROR";
  case LL_CRIT:
    return "CRIT";
  case LL_ALERT:
    return "ALERT";
  case LL_EMERG:
    return "EMERG";
  default:
    return "Missing";
  }
}

std::string vformat(const char *format, va_list args)
{
  va_list copy;
  va_copy(copy, args);
  int len = std::vsnprintf(nullptr, 0, format, copy);
  va_end(copy);

  if (len >= 0) {
    std::string s(std::size_t(len) + 1, '\0');
    std::vsnprintf(&s[0], s.size(), format, args);
    s.resize(len);
    return s;
  }

  const auto err = errno;
  const auto ec = std::error_code(err, std::generic_category());
  throw std::system_error(ec);
}

std::string format(const char *message, ...)
{
  va_list args;
  va_start(args, message);
  auto r = vformat(message, args);
  va_end(args);
  return r;
}

//

std::string log_vsformat(LogLevel ll, const char *message, va_list args)
{
  std::string f = vformat(message, args);
  std::string line = format("{\"level\": \"%s\", \"message\": \"%s\"", LogLevelToString(ll), escape_json_str(f).c_str());
  // Correlate with the Rust shell's trace when one is active on this thread; the
  // ids are W3C lowercase hex, the same keys the Rust formatter emits.
  if (!g_trace_id.empty()) {
    line += format(", \"trace_id\": \"%s\", \"span_id\": \"%s\"", g_trace_id.c_str(), g_span_id.c_str());
  }
  line += "}\n";
  return line;
}

std::string log_sformat(LogLevel ll, const char *message, ...)
{
  va_list args;
  va_start(args, message);
  auto r = log_vsformat(ll, message, args);
  va_end(args);
  return r;
}

void log_vformat(LogLevel ll, const char *message, va_list args)
{
  if (ll < g_log_level) return;

  if (g_cli_mode) {
    // CLI mode: plain text. Under --json every level goes to stderr so stdout
    // stays reserved for the single JSON document. Outside --json,
    // errors→stderr, others→stdout (legacy behaviour).
    std::string msg = vformat(message, args);
    if (!g_correlation_id.empty()) { msg.insert(0, "[" + g_correlation_id + "] "); }
    if (g_json_mode.load(std::memory_order_relaxed) || ll >= LL_ERR) {
      fprintf(stderr, "%s\n", msg.c_str());
    } else {
      fprintf(stdout, "%s\n", msg.c_str());
      fflush(stdout);
    }
  } else {
    // Server mode: JSON format→stdout (container best practice)
    // fflush required because stdout is fully buffered when piped
    std::string outfmt = log_vsformat(ll, message, args);
    fprintf(stdout, "%s", outfmt.c_str());
    fflush(stdout);
  }
}

void log_format(LogLevel ll, const char *message, ...)
{
  va_list args;
  va_start(args, message);
  log_vformat(ll, message, args);
  va_end(args);
}

void log_debug(const char *message, ...)
{
  va_list args;
  va_start(args, message);
  log_vformat(LL_DEBUG, message, args);
  va_end(args);
}

void log_info(const char *message, ...)
{
  va_list args;
  va_start(args, message);
  log_vformat(LL_INFO, message, args);
  va_end(args);
}

void log_warn(const char *message, ...)
{
  va_list args;
  va_start(args, message);
  log_vformat(LL_WARNING, message, args);
  va_end(args);
}

void log_err(const char *message, ...)
{
  va_list args;
  va_start(args, message);
  log_vformat(LL_ERR, message, args);
  va_end(args);
}
