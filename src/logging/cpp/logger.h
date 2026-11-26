#ifndef LOGGER_H
#define LOGGER_H

#include <cstdarg>
#include <string>

enum LogLevel { LL_DEBUG, LL_INFO, LL_NOTICE, LL_WARNING, LL_ERR, LL_CRIT, LL_ALERT, LL_EMERG };

std::string log_vsformat(LogLevel ll, const char *message, va_list args);
std::string log_sformat(LogLevel ll, const char *message, ...);
void log_vformat(LogLevel ll, const char *message, va_list args);
void log_format(LogLevel ll, const char *message, ...);
void log_debug(const char *message, ...);
void log_info(const char *message, ...);
void log_warn(const char *message, ...);
void log_err(const char *message, ...);
void set_cli_mode(bool cli);
bool is_cli_mode();
void set_log_level(LogLevel level);
LogLevel get_log_level();

/*!
 * Map a log-level name (case-insensitive: DEBUG, INFO, NOTICE, WARNING, ERR,
 * CRIT, ALERT, EMERG) to its `LogLevel`. Returns `fallback` for an empty or
 * unrecognized value.
 */
[[nodiscard]] LogLevel parse_log_level(const std::string &name, LogLevel fallback);

/*!
 * Enable JSON-output mode. When true, every log level (info, warn, err) is
 * routed to stderr so that stdout stays reserved for the single JSON document
 * produced by `--json`. Backed by `std::atomic<bool>`: writers use
 * `memory_order_relaxed` since the flag is set once at CLI startup
 * (immediately after CLI11 parses `--json`) and never re-set, and readers
 * only need to observe the value published by that single store.
 */
void set_json_mode(bool enabled);
bool is_json_mode();

/*!
 * The correlation ID of a CLI run: the ID a caller finds the run's log lines
 * and its `--json` document by. Process-wide, set once by the Rust CLI before
 * any offline verb runs (and before any thread starts), never re-set. In CLI
 * mode, the first line of each log call starts with `[<id>] `; server mode is
 * unchanged. Empty (the default) means no prefix.
 */
void set_correlation_id(const std::string &id);
[[nodiscard]] const std::string &get_correlation_id();

/*!
 * Stamp this thread's server-mode JSON log lines with the active distributed
 * trace context (W3C lowercase-hex `trace_id` / `span_id`), so the C++ engine's
 * logs correlate to the Rust shell's OpenTelemetry trace. The Rust shell sets it
 * before each blocking FFI call and clears it after (both pointers NULL) — engine
 * work runs on the same thread, so a thread-local is the right scope. NULL or
 * empty clears it; while clear, the `trace_id`/`span_id` keys are omitted.
 */
void set_log_trace_context(const char *trace_id, const char *span_id);

/*!
 * Stamp this thread's outbound-HTTP trace-propagation context: the W3C
 * `traceparent` the Lua `server.http` client (`CurlConnection`) injects on
 * outbound requests so a downstream service (dsp-api) continues this trace. The
 * Rust shell sets it — from the active OpenTelemetry span — before a preflight /
 * route FFI call and clears it after; engine work runs on the same thread, so a
 * thread-local is the right scope. NULL / empty / malformed clears it (nothing
 * is injected). Kept separate from `set_log_trace_context` because propagation
 * needs the full formatted header incl. the sampling flag, while log lines carry
 * only the raw trace/span ids.
 */
void set_outbound_traceparent(const char *traceparent);

/*!
 * The W3C `traceparent` set for this thread, or empty when none is active. Read
 * by the Lua HTTP client to inject the header on outbound requests.
 */
std::string get_outbound_traceparent();

#endif
