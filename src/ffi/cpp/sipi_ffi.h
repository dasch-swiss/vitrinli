/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * The narrow C FFI seam between the Rust HTTP shell and the C++ image engine
 * (strangler-fig rewrite; ADR-0013).
 *
 * This header is the **durable interface** of the rewrite: the Rust shell
 * drives the C++ engine through exactly these functions, and the engine reaches
 * the response only through the `SipiResponse` callbacks — never a `shttps`
 * type. It is plain C (no C++ in the surface) so it is consumable both by the
 * C++ implementation in `sipi_ffi.cpp` and by the Rust shell, which mirrors it
 * with hand-written bindings in `src/server/rust/src/ffi.rs`.
 *
 * **Streamed, not a result struct.** A real SIPI response carries a dynamic
 * status, a variable header set (Content-Range/206, Content-Disposition,
 * Retry-After, Cache-Control, Last-Modified, Link, repeated Set-Cookie) and a
 * streamed body. A fixed result struct cannot express that, so the response is
 * emitted through the `SipiResponse` callbacks: set status, add each header,
 * write each body chunk, poll for client cancellation.
 *
 * **No C++ exception crosses this boundary.** Every `sipi_*` entry wraps its
 * body in a catch-all and returns a status code; a throw unwinding through
 * `extern "C"` into Rust is UB.
 *
 * The full entry-point set is declared here as one locked contract; the
 * definitions in `sipi_ffi.cpp` grow as the engine is carved behind the seam.
 */
#ifndef SIPI_FFI_SIPI_FFI_H
#define SIPI_FFI_SIPI_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define SIPI_FFI_NODISCARD [[nodiscard]]
extern "C" {
#else
#define SIPI_FFI_NODISCARD
#endif

/* ── Response sink callbacks (Rust-owned) ──────────────────────────────────
 * The engine emits the whole response through these. `ctx` is the opaque
 * Rust-owned cookie. */

/*! Body bytes, forward-only, for an **unknown-length** body (e.g. the image
 *  encoder, whose output size isn't known until encoding finishes — the
 *  transport frames it chunked over HTTP/1.1). Returns 0 on success, non-zero
 *  on a write failure (peer gone, socket error) so the engine aborts without
 *  throwing across C frames. Structurally identical to `Sipi::SipiWriteFn` in
 *  `format_handlers/output_sink.h` — the two are kept in lock-step by design across the
 *  format_handlers/ffi layer boundary; each header stays self-contained. */
typedef int (*SipiWriteFn)(void *ctx, const uint8_t *data, size_t len);

/*! Deliver a **known-length** file region `[offset, offset+length)` to the
 *  body. The size is known, so the transport frames it with Content-Length
 *  (and may use zero-copy `sendfile(2)`) — the right shape for raw file
 *  downloads (images via `/file`, and any media). Status + headers are set
 *  first via set_status/add_header. Returns 0 on success, non-zero on a write
 *  failure. Framing (Content-Length vs HTTP/2 DATA frames) is the transport's
 *  call, per the negotiated protocol version — never hard-coded here. */
typedef int (*SipiSendFileFn)(void *ctx, const char *path, uint64_t offset, uint64_t length);

/*! One call per response header line (Set-Cookie may repeat). */
typedef void (*SipiHeaderFn)(void *ctx, const char *name, const char *value);

/*! HTTP status code for the response. */
typedef void (*SipiStatusFn)(void *ctx, int status);

/*! Polled between pipeline stages; 1 = client gone / timed out → abort and
 *  unlink any partial cache file. */
typedef int (*SipiCancelledFn)(void *ctx);

/*! The response sink the engine drives. A body is delivered either as a
 *  known-length file region (`send_file`, Content-Length framing) or as an
 *  unknown-length byte stream (`write`, chunked framing) — never both. */
typedef struct
{
  void *ctx;
  SipiStatusFn set_status;
  SipiHeaderFn add_header;
  SipiWriteFn write;
  SipiSendFileFn send_file;
  SipiCancelledFn cancelled;
} SipiResponse;

/* ── IIIF serve request (consumed by sipi_serve_image) ──────────────────────
 * The IIIF URL components, typed as the `iiifparser` state machine produces
 * them. Region/size/rotation/quality/format are flattened so the seam carries
 * no `iiifparser` class layout (those types move to Rust in D+ without changing
 * this ABI). Enum values mirror the C++ `SipiRegion::CoordType`,
 * `SipiSize::SizeType`, `SipiQualityFormat::QualityType`/`FormatType`. */

typedef enum {
  SIPI_REGION_FULL = 0,
  SIPI_REGION_SQUARE = 1,
  SIPI_REGION_COORDS = 2,
  SIPI_REGION_PERCENTS = 3
} SipiRegionType;

typedef enum {
  SIPI_SIZE_UNDEFINED = 0,
  SIPI_SIZE_FULL = 1,
  SIPI_SIZE_PIXELS_XY = 2,
  SIPI_SIZE_PIXELS_X = 3,
  SIPI_SIZE_PIXELS_Y = 4,
  SIPI_SIZE_MAXDIM = 5,
  SIPI_SIZE_PERCENTS = 6,
  SIPI_SIZE_REDUCE = 7
} SipiSizeType;

typedef enum {
  SIPI_QUALITY_DEFAULT = 0,
  SIPI_QUALITY_COLOR = 1,
  SIPI_QUALITY_GRAY = 2,
  SIPI_QUALITY_BITONAL = 3
} SipiQualityType;

typedef enum {
  SIPI_FORMAT_UNSUPPORTED = 0,
  SIPI_FORMAT_JPG = 1,
  SIPI_FORMAT_TIF = 2,
  SIPI_FORMAT_PNG = 3,
  SIPI_FORMAT_GIF = 4,
  SIPI_FORMAT_JP2 = 5,
  SIPI_FORMAT_PDF = 6,
  SIPI_FORMAT_WEBP = 7
} SipiFormatType;

typedef struct
{
  SipiRegionType region_type;
  float region[4]; /* x,y,w,h (coords or percents); unused for FULL/SQUARE */

  SipiSizeType size_type;
  int size_upscaling; /* `^` prefix */
  float size_percent; /* PERCENTS */
  int size_reduce; /* REDUCE */
  size_t size_nx; /* requested width  (PIXELS or MAXDIM modes) */
  size_t size_ny; /* requested height (PIXELS or MAXDIM modes) */

  float rotation; /* degrees */
  int rotation_mirror;

  SipiQualityType quality_type;
  SipiFormatType format_type;
} SipiIiifParams;

/*! Flat, engine-populated context for a handled (non-fatal) image error —
 *  a decode/convert/write failure the engine catches internally. Reported as
 *  a side-channel via `SipiReportErrorFn`; never affects the `SipiStatus`
 *  returned to the caller (every entry point still returns a clean status
 *  code — this is purely additional context for observability). Every
 *  field is only valid for the duration of the `report_error` call; the
 *  callback must copy anything it needs to keep. Any string field may be
 *  NULL, and any numeric field may be 0, when not known for the phase that
 *  failed (e.g. width/height are unset if the image was never successfully
 *  read). All fields are naturally 8-byte aligned (pointers and `uint64_t`
 *  only), so there is no interior padding to reason about. */
typedef struct
{
  const char *phase; /* "read" / "convert" / "write" */
  const char *message; /* the caught exception's message */
  const char *input_file;
  const char *output_format;
  const char *colorspace;
  const char *icc_profile_type;
  const char *orientation;
  uint64_t width;
  uint64_t height;
  uint64_t channels;
  uint64_t bps;
  uint64_t file_size_bytes;
} SipiImageErrorReport;

/*! Reports a handled image error's context as a side-channel (Sentry, via
 *  the Rust shell) — never a response. `err` is only valid for the call's
 *  duration. `ctx` is the caller-supplied opaque data from `SipiServeRequest`
 *  (`report_ctx`) — the Rust edge uses it to carry the request URI, since
 *  that already lives on the request and isn't part of the flat struct. */
typedef void (*SipiReportErrorFn)(void *ctx, const SipiImageErrorReport *err);

/* Called at most once per serve, after the cache check and before the decode:
 *  never for HEAD, a passthrough or a cache hit. Returns 0 to go on; non-zero
 *  refuses the decode, and the engine answers 503 with `Retry-After: 1`. */
typedef int (*SipiBeforeDecodeFn)(void *ctx);

#ifdef __cplusplus
/* Lock-step layout guard — paired with the Rust offset/size_of test in
 * src/server/rust/src/ffi.rs. All fields are 8-byte-wide (pointers / uint64_t),
 * so there is no packing subtlety, but the guard still catches an accidental
 * field reorder or insertion on either side. LP64 on every supported target. */
static_assert(sizeof(SipiImageErrorReport) == 96, "SipiImageErrorReport size drifted from src/server/rust/src/ffi.rs");
static_assert(offsetof(SipiImageErrorReport, phase) == 0, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, message) == 8, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, input_file) == 16, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, output_format) == 24, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, colorspace) == 32, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, icc_profile_type) == 40, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, orientation) == 48, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, width) == 56, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, height) == 64, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, channels) == 72, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, bps) == 80, "SipiImageErrorReport layout drift");
static_assert(offsetof(SipiImageErrorReport, file_size_bytes) == 88, "SipiImageErrorReport layout drift");
#endif

typedef struct
{
  const char *resolved_path; /* image-root-validated absolute path (validation owned by the Rust edge) */
  const char *prefix; /* IIIF prefix — canonical-URL + cache key */
  const char *identifier; /* IIIF identifier — canonical-URL `id` + cache key */
  SipiIiifParams params; /* region/size/rotation/quality/format */
  uint32_t restricted_width; /* preflight `restrict` size cap (RW, RH), decided by routes::restriction; */
  uint32_t restricted_height; /* 0, 0 = no size cap; exactly one of them 0 is a 500 */
  const char *watermark_path; /* preflight `restrict` watermark, or NULL */
  const char *forwarded_proto; /* X-Forwarded-Proto → canonical-URL / redirect scheme */
  const char *forwarded_host; /* X-Forwarded-Host  → canonical-URL `id` (host for the canonical URL) */
  const char *request_uri; /* raw request URI — error/log context only (Sentry), or NULL */
  int is_head; /* 1 = HEAD: emit status + headers, no body, no cache write */
  SipiReportErrorFn report_error; /* handled-error side-channel report, or NULL = not wanted */
  void *report_ctx; /* opaque data passed to report_error (the Rust edge: the request URI) */
  SipiBeforeDecodeFn before_decode; /* decode-admission hook, or NULL = not wanted */
  void *before_decode_ctx; /* opaque data passed to before_decode */
} SipiServeRequest;

/* ── Config override channel ─────────────────────────────────────────────────
 * A fixed permission TYPE plus an open key/value channel (infile, watermark,
 * size, cookie_url, token_url, logout_url, service pass-through) — the real
 * preflight returns an unordered_map, not a flat struct. Two hooks, same
 * shape: the IIIF image preflight (prefix + identifier) and the `/file`
 * media-serving preflight (a resolved filepath; audio / video / PDF / any
 * non-IIIF file). */
typedef enum {
  SIPI_ALLOW = 0,
  SIPI_LOGIN = 1,
  SIPI_CLICKTHROUGH = 2,
  SIPI_KIOSK = 3,
  SIPI_EXTERNAL = 4,
  SIPI_RESTRICT = 5,
  SIPI_DENY = 6,
  SIPI_STREAM = 7
} SipiPermType;

typedef void (*SipiKVFn)(void *ctx, const char *key, const char *value);


/* ── CLI/env override channel (concrete) ──────────────────────────────────────
 * The resolved config `sipi_init` consumes (the shell parses both config
 * flavors and merges the CLI/env layer before the call).
 * Hand-mirrored by the Rust `#[repr(C)] SipiServerConfig` in
 * src/server/rust/src/config.rs — field order, widths, and the `has_` presence
 * flags must match byte-for-byte or it is silent UB on drift. NOT bindgen: the
 * seam is small and mirrored on purpose. The lock-step `static_assert` guard
 * below + the Rust `size_of`/`offset_of!` test pin the layout against drift.
 *
 * Presence convention (mirrors the seam's "NULL = absent" idiom):
 *   - strings / the string array: a NULL pointer  ⇒ the override is absent.
 *   - scalars: a paired `has_<field>` flag (non-zero ⇒ present), because 0 is a
 *     valid value (e.g. cache_nfiles 0 = unlimited).
 * Sized strings (cache_size / maxpost / memory_limit) carry the raw "300M"
 * text; the engine parses the suffix (parseSizeString) — never pre-parsed here.
 * Fields are grouped by alignment (8-byte first, then the 4-byte values, then
 * their `has_` flags) so the layout has no interior padding and the guard
 * offsets form a clean sequence. */
typedef struct SipiServerConfig
{
  /* 8-byte: path / identity strings (NULL = absent) */
  const char *imgroot;
  const char *scriptdir;
  const char *initscript;
  const char *tmpdir;
  const char *jwtkey;
  const char *cache_dir;
  const char *cache_size;         /* raw "200M" — engine parses the suffix */
  const char *maxpost;            /* raw "300M" — engine parses the suffix */
  const char *memory_limit;       /* raw "8G" RAM envelope — engine parses the suffix; "0"/absent = auto-detect */
  const char *admission_mode;     /* "basic" | "advanced" */
  const char *thumbsize;
  const char *knorapath;
  const char *knoraport;
  const char *docroot;
  const char *wwwroute;
  const char *loglevel;
  /* 8-byte: image scaling-quality per codec ("high"|"medium"|"low"; NULL =
   * engine default). TOML-config-only — there is no CLI flag, so these never
   * arrive from the clap path. */
  const char *scaling_quality_jpeg;
  const char *scaling_quality_tiff;
  const char *scaling_quality_png;
  const char *scaling_quality_j2k;
  /* 8-byte: 64-bit scalar values (presence via the has_ flags below) */
  double tiles_memory_ratio;            /* fraction of the envelope reserved for tiles; full lane = envelope × (1 − ratio) */
  uint64_t large_decode_threshold_bytes;/* estimated peak >= this => full lane (charged); below => tile (bypass) */
  /* 4-byte scalar values (presence via the has_ flags below) */
  int32_t serverport;
  int32_t maxtmpage;
  uint32_t cache_nfiles;          /* 0 = unlimited; a negative is rejected at the CLI (no wrap) */
  int32_t pathprefix;             /* prefix_as_path, bool carried as 0/1 */
  int32_t jpeg_quality;           /* JPEG output quality (1-100); TOML-config-only */
  /* 4-byte presence flags for the scalars above (non-zero = present) */
  int has_serverport;
  int has_maxtmpage;
  int has_cache_nfiles;
  int has_pathprefix;
  int has_jpeg_quality;
  int has_tiles_memory_ratio;
  int has_large_decode_threshold_bytes;
} SipiServerConfig;

#ifdef __cplusplus
/* Lock-step layout guard — paired with the Rust offset/size_of test in
 * src/server/rust/src/config.rs. Any field reorder / width change on either side
 * breaks one of the two. LP64 on every supported target (darwin-aarch64,
 * linux-x86_64, linux-aarch64). */
static_assert(sizeof(void *) == 8, "SipiServerConfig layout assumes an LP64 target");
static_assert(sizeof(SipiServerConfig) == 224, "SipiServerConfig size drifted from src/server/rust/src/config.rs");
static_assert(offsetof(SipiServerConfig, imgroot) == 0, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, scriptdir) == 8, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, initscript) == 16, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, tmpdir) == 24, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, jwtkey) == 32, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, cache_dir) == 40, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, cache_size) == 48, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, maxpost) == 56, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, memory_limit) == 64, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, admission_mode) == 72, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, thumbsize) == 80, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, knorapath) == 88, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, knoraport) == 96, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, docroot) == 104, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, wwwroute) == 112, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, loglevel) == 120, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, scaling_quality_jpeg) == 128, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, scaling_quality_tiff) == 136, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, scaling_quality_png) == 144, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, scaling_quality_j2k) == 152, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, tiles_memory_ratio) == 160, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, large_decode_threshold_bytes) == 168, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, serverport) == 176, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, maxtmpage) == 180, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, cache_nfiles) == 184, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, pathprefix) == 188, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, jpeg_quality) == 192, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, has_serverport) == 196, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, has_maxtmpage) == 200, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, has_cache_nfiles) == 204, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, has_pathprefix) == 208, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, has_jpeg_quality) == 212, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, has_tiles_memory_ratio) == 216, "SipiServerConfig layout drift");
static_assert(offsetof(SipiServerConfig, has_large_decode_threshold_bytes) == 220, "SipiServerConfig layout drift");
#endif

/* Engine-counter snapshot for `sipi_metrics_snapshot`. Incomplete here on
 * purpose: the implementing translation unit owns the layout, so the seam
 * commits no field set it does not need (made concrete at its own slice). */
typedef struct SipiMetricsSnapshot SipiMetricsSnapshot;


/* ── Edge-probe types (Rust-edge path validation + info.json/knora.json) ─────
 * Read-only helpers the Rust shell needs to build a request and to assemble the
 * JSON responses the seam has no serve entry for. */

/*! Native image shape from a header read (NOT a full decode). `numpages` is 0
 *  for a single-page image; `tile_width`/`tile_height` are 0 when the image is
 *  untiled. Carries the tiling fields so the Rust shell assembles info.json's
 *  `sizes[]` / `tiles[]` from one probe rather than a second call — the pyramid
 *  is derived from the tile grid. */
typedef struct
{
  uint32_t width;
  uint32_t height;
  uint32_t numpages;
  uint32_t tile_width;
  uint32_t tile_height;
} SipiImageDims;

/*! Emits a single string value through a caller callback, so the seam returns no
 *  owned C string (no malloc/free contract across the boundary). */
typedef void (*SipiStrFn)(void *ctx, const char *value);

/*! Emits an image's Essentials-packet identity — its original mimetype and
 *  original filename — through a caller callback, so the seam returns no
 *  owned strings. Called at most once, from sipi_image_dims: both strings are
 *  known together or not at all. */
typedef void (*SipiEssentialsFn)(void *ctx, const char *orig_mimetype, const char *orig_filename);

/* ── Per-serve observations ──────────────────────────────────────────────────
 * What one `sipi_serve_image` call observed about itself: nanosecond timings per
 * phase (relative to the call's start) plus the decode-memory estimate. The
 * engine accumulates them in a thread-local during the
 * call; `sipi_serve_timings_take` reads that accumulator, so the caller must
 * call it on the SAME thread immediately after `sipi_serve_image` returns. A
 * `present[i]` of 0 means the phase did not run (a cache hit or passthrough
 * skips decode/encode; a rotate-free request skips ROTATE, etc.); a `failed[i]`
 * of 1 means the phase started but exited via a C++ exception (a decode/encode
 * error), so the shell marks that span `Status=Error`. Purely observational: the
 * Rust shell mints one child span per present phase under its request span — the
 * engine emits no spans of its own.
 *
 * Caveat: SIPI_PHASE_ENCODE spans the streamed encode *and* the write to the
 * response sink, so a slow/back-pressured HTTP client inflates its `dur_ns` with
 * client-side wait, not just codec CPU. */
typedef enum {
  SIPI_PHASE_SHAPE = 0, /* header/shape probe (source open, no full decode) */
  SIPI_PHASE_DECODE = 1, /* SipiImage::read — decode + region + scale */
  SIPI_PHASE_ROTATE = 2, /* rotate / mirror */
  SIPI_PHASE_QUALITY = 3, /* colour/gray ICC or bitonal conversion */
  SIPI_PHASE_WATERMARK = 4, /* watermark overlay */
  SIPI_PHASE_ENCODE = 5, /* encode + write (streamed tail) */
  SIPI_PHASE_COUNT = 6 /* sentinel: number of phases, not a phase index */
} SipiPhase;

typedef struct
{
  uint64_t start_ns[SIPI_PHASE_COUNT]; /* offset from the serve call's start */
  uint64_t dur_ns[SIPI_PHASE_COUNT];
  uint8_t present[SIPI_PHASE_COUNT];
  uint8_t failed[SIPI_PHASE_COUNT]; /* 1 = phase exited via an exception */
  /* Estimated peak decode memory for this serve, in bytes — the value the
   * decode-memory budget admitted against. 0 when no decode ran (cache hit,
   * HEAD, passthrough), mirroring `present`, so the caller can tell "no sample"
   * from "a real measurement". Not a timing, but it rides the same thread-local
   * accumulator because it is observed at the same point and taken by the same
   * post-call read; the caller records it into a histogram the flat
   * `SipiMetricsSnapshot` cannot carry. */
  uint64_t decode_estimate_bytes;
} SipiServeTimings;

#ifdef __cplusplus
/* Lock-step ABI guard for the hand-mirrored seam types — paired with the Rust
 * `offset_of!`/`size_of` tests and `const _` enum asserts in
 * `src/server/rust/src/ffi.rs`. A field reorder / width change / insertion, or a
 * one-sided enum renumber, breaks one side of the pair before it becomes silent
 * UB across the boundary. (`SipiServeTimings`, `SipiImageErrorReport`,
 * `SipiServerConfig`, and `SipiMetricsSnapshot` are guarded next to their own
 * definitions.) LP64 on every supported target. */
static_assert(sizeof(void *) == 8, "seam ABI assumes an LP64 target");

/* Request/size/quality/format + permission enum discriminants. */
static_assert(SIPI_REGION_FULL == 0, "SipiRegionType drift");
static_assert(SIPI_REGION_SQUARE == 1, "SipiRegionType drift");
static_assert(SIPI_REGION_COORDS == 2, "SipiRegionType drift");
static_assert(SIPI_REGION_PERCENTS == 3, "SipiRegionType drift");
static_assert(SIPI_SIZE_UNDEFINED == 0, "SipiSizeType drift");
static_assert(SIPI_SIZE_FULL == 1, "SipiSizeType drift");
static_assert(SIPI_SIZE_PIXELS_XY == 2, "SipiSizeType drift");
static_assert(SIPI_SIZE_PIXELS_X == 3, "SipiSizeType drift");
static_assert(SIPI_SIZE_PIXELS_Y == 4, "SipiSizeType drift");
static_assert(SIPI_SIZE_MAXDIM == 5, "SipiSizeType drift");
static_assert(SIPI_SIZE_PERCENTS == 6, "SipiSizeType drift");
static_assert(SIPI_SIZE_REDUCE == 7, "SipiSizeType drift");
static_assert(SIPI_QUALITY_DEFAULT == 0, "SipiQualityType drift");
static_assert(SIPI_QUALITY_COLOR == 1, "SipiQualityType drift");
static_assert(SIPI_QUALITY_GRAY == 2, "SipiQualityType drift");
static_assert(SIPI_QUALITY_BITONAL == 3, "SipiQualityType drift");
static_assert(SIPI_FORMAT_UNSUPPORTED == 0, "SipiFormatType drift");
static_assert(SIPI_FORMAT_JPG == 1, "SipiFormatType drift");
static_assert(SIPI_FORMAT_TIF == 2, "SipiFormatType drift");
static_assert(SIPI_FORMAT_PNG == 3, "SipiFormatType drift");
static_assert(SIPI_FORMAT_GIF == 4, "SipiFormatType drift");
static_assert(SIPI_FORMAT_JP2 == 5, "SipiFormatType drift");
static_assert(SIPI_FORMAT_PDF == 6, "SipiFormatType drift");
static_assert(SIPI_FORMAT_WEBP == 7, "SipiFormatType drift");
static_assert(SIPI_ALLOW == 0, "SipiPermType drift");
static_assert(SIPI_LOGIN == 1, "SipiPermType drift");
static_assert(SIPI_CLICKTHROUGH == 2, "SipiPermType drift");
static_assert(SIPI_KIOSK == 3, "SipiPermType drift");
static_assert(SIPI_EXTERNAL == 4, "SipiPermType drift");
static_assert(SIPI_RESTRICT == 5, "SipiPermType drift");
static_assert(SIPI_DENY == 6, "SipiPermType drift");
static_assert(SIPI_STREAM == 7, "SipiPermType drift");

/* SipiResponse — void* + five callback pointers. */
static_assert(sizeof(SipiResponse) == 48, "SipiResponse size drifted from src/server/rust/src/ffi.rs");
static_assert(offsetof(SipiResponse, ctx) == 0, "SipiResponse layout drift");
static_assert(offsetof(SipiResponse, set_status) == 8, "SipiResponse layout drift");
static_assert(offsetof(SipiResponse, add_header) == 16, "SipiResponse layout drift");
static_assert(offsetof(SipiResponse, write) == 24, "SipiResponse layout drift");
static_assert(offsetof(SipiResponse, send_file) == 32, "SipiResponse layout drift");
static_assert(offsetof(SipiResponse, cancelled) == 40, "SipiResponse layout drift");

/* SipiIiifParams — the flattened IIIF params (also nested in SipiServeRequest). */
static_assert(sizeof(SipiIiifParams) == 72, "SipiIiifParams size drifted from src/server/rust/src/ffi.rs");
static_assert(offsetof(SipiIiifParams, region_type) == 0, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, region) == 4, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, size_type) == 20, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, size_upscaling) == 24, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, size_percent) == 28, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, size_reduce) == 32, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, size_nx) == 40, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, size_ny) == 48, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, rotation) == 56, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, rotation_mirror) == 60, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, quality_type) == 64, "SipiIiifParams layout drift");
static_assert(offsetof(SipiIiifParams, format_type) == 68, "SipiIiifParams layout drift");

/* SipiServeRequest — pointers + the nested SipiIiifParams + the report and before-decode channels. */
static_assert(sizeof(SipiServeRequest) == 176, "SipiServeRequest size drifted from src/server/rust/src/ffi.rs");
static_assert(offsetof(SipiServeRequest, resolved_path) == 0, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, prefix) == 8, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, identifier) == 16, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, params) == 24, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, restricted_width) == 96, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, restricted_height) == 100, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, watermark_path) == 104, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, forwarded_proto) == 112, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, forwarded_host) == 120, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, request_uri) == 128, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, is_head) == 136, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, report_error) == 144, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, report_ctx) == 152, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, before_decode) == 160, "SipiServeRequest layout drift");
static_assert(offsetof(SipiServeRequest, before_decode_ctx) == 168, "SipiServeRequest layout drift");


/* SipiImageDims — five uint32_t; 4-aligned, unlike the pointer-bearing structs. */
static_assert(sizeof(SipiImageDims) == 20, "SipiImageDims size drifted from src/server/rust/src/ffi.rs");
static_assert(offsetof(SipiImageDims, width) == 0, "SipiImageDims layout drift");
static_assert(offsetof(SipiImageDims, height) == 4, "SipiImageDims layout drift");
static_assert(offsetof(SipiImageDims, numpages) == 8, "SipiImageDims layout drift");
static_assert(offsetof(SipiImageDims, tile_width) == 12, "SipiImageDims layout drift");
static_assert(offsetof(SipiImageDims, tile_height) == 16, "SipiImageDims layout drift");
#endif

/* ── Entry points ───────────────────────────────────────────────────────────
 * All return 0 on success / an error code on failure; none let a C++ exception
 * cross the boundary. */

/*! IIIF decode→transform→encode→stream; honours the restrict size/watermark. */
SIPI_FFI_NODISCARD int sipi_serve_image(const SipiServeRequest *req, const SipiResponse *resp);

/*! Copy the current thread's per-serve observations (see `SipiServeTimings`)
 *  into `*out`. Call on the SAME thread right after `sipi_serve_image` returns;
 *  a NULL `out` is a no-op. Never fails and emits nothing; the accumulator is
 *  reset at the next `sipi_serve_image` entry on the thread. */
void sipi_serve_timings_take(SipiServeTimings *out);

/*! The build stamp Bazel baked into the engine, so telemetry can name the build
 *  it is reporting from. Both return a pointer to a string literal with static
 *  storage duration — never allocated, never freed, valid for the process
 *  lifetime, so unlike `SipiStrFn` these need no callback to avoid an ownership
 *  contract. Never NULL.
 *
 *  `sipi_build_version` is `version.txt` (release-please's single source of
 *  truth), NOT the `git describe` tag: describe degrades to a stale nearest-tag
 *  in a build context without full tag history, which is how a 6.2.1 build came
 *  to report itself as `v5.0.1-dirty`. `sipi_build_commit` is the commit SHA. */
const char *sipi_build_version(void);
const char *sipi_build_commit(void);

/*! The engine's serve-phase count (`SIPI_PHASE_COUNT`). The Rust shell mirrors
 *  `SipiServeTimings` by hand, so it asserts its own `PHASE_COUNT` against this
 *  at test time — a one-sided phase-count change then fails loudly instead of
 *  writing past a stale-sized `SipiServeTimings`. */
int sipi_phase_count(void);

/*! Raw `/file` passthrough incl. HTTP Range / 206 — no decode. Owns the serve
 *  *policy* (stat, MIME → Content-Type, Range parse → status + Content-Range)
 *  and delegates the byte delivery to `resp->send_file`, so the transport
 *  streams the file with Content-Length framing (and zero-copy where it can).
 *  `resolved_path` is an already-validated absolute path; `range` is the raw
 *  `Range` header value or NULL. Returns 0 when the response was emitted, or an
 *  HTTP status code (e.g. 404, 400, 500) when it fails before any byte is sent,
 *  so the caller can render its own error response. */
SIPI_FFI_NODISCARD int sipi_serve_file(const char *resolved_path, const char *range, const SipiResponse *resp);




/*! Engine counters → Rust OTel meter (NOT Prometheus). */
SIPI_FFI_NODISCARD int sipi_metrics_snapshot(SipiMetricsSnapshot *out);

/*! Install the engine from the resolved config the shell assembled (both
 *  config flavors are parsed Rust-side; `overrides` is the one channel). */
SIPI_FFI_NODISCARD int sipi_init(const SipiServerConfig *overrides);

/* ── Edge probes ─────────────────────────────────────────────────────────────
 * Read-only helpers the Rust shell calls at the request edge. Like
 * sipi_metrics_snapshot they drive no response sink, so they are sipi_guard-only
 * (no build/apply split). All require `sipi_init` to have installed the engine. */

/*! The configured image root, for the Rust edge to build + containment-check a
 *  `resolved_path`. `resolved` = 0 → the raw config value (for the path build);
 *  `resolved` = 1 → the realpath()-resolved root (the
 *  R2 containment check). `*out` points at process-static memory owned by the
 *  installed engine context — valid for the process lifetime after `sipi_init`,
 *  never freed by the caller. Returns 0, or 500 if `sipi_init` has not run. */
SIPI_FFI_NODISCARD int sipi_imgroot(int resolved, const char **out);

/*! The `/server` fileserver docroot (the config `fileserver.docroot`), the
 *  raw config value — the Rust edge canonicalises it per request for the
 *  containment check (the docroot dir may be created after startup, unlike the
 *  image root). `*out` is empty when no fileserver is configured. Points at
 *  process-static engine memory; never freed by the caller. Returns 0, or 500 if
 *  `sipi_init` has not run. */
SIPI_FFI_NODISCARD int sipi_docroot(const char **out);

/*! The URL prefix the docroot fileserver is mounted at (the config
 *  `fileserver.wwwroute`, e.g. "/server"). `*out` is empty when no fileserver is
 *  configured; the Rust shell registers the static route only when both docroot
 *  and wwwroute are non-empty. Returns 0, or 500 if `sipi_init` has not run. */
SIPI_FFI_NODISCARD int sipi_wwwroute(const char **out);

/*! The `prefix_as_path` config knob: `*out` = 1 → the IIIF prefix is a path
 *  component under imgroot (`imgroot/prefix/identifier`); 0 → `imgroot/identifier`.
 *  Returns 0, or 500 if `sipi_init` has not run. */
SIPI_FFI_NODISCARD int sipi_prefix_as_path(int *out);

/*! The configured max POST body size in bytes (the config `max_post_size`).
 *  The Rust shell caps Lua-route request bodies at this size (oversized → 413).
 *  `*out` = 0 means unlimited. Returns 0, or 500 if `sipi_init` has not run. */
SIPI_FFI_NODISCARD int sipi_max_post_size(size_t *out);

/*! The resolved admission mode, "basic" or "advanced". Read back so the shell's
 *  two-lane pool runs the same mode as the engine's memory budget. The pointer is
 *  into the process-static EngineContext. Returns 0, or 500 if `sipi_init` has not
 *  run. */
SIPI_FFI_NODISCARD int sipi_admission_mode(const char **out);

/*! The tile reserve fraction (`tiles_memory_ratio`) — for the shell's
 *  config-fingerprint metric. Returns 0, or 500 if `sipi_init` has not run. */
SIPI_FFI_NODISCARD int sipi_tiles_memory_ratio(double *out);

/*! The tile/full classifier threshold in bytes (`large_decode_threshold_bytes`).
 *  Read back so the shell classifies against the same value the engine charges
 *  the budget by. Returns 0, or 500 if `sipi_init` has not run. */
SIPI_FFI_NODISCARD int sipi_large_decode_threshold_bytes(size_t *out);

/*! The resolved RAM envelope in bytes (`memory_limit`, post 0→auto-detect) — for
 *  the shell's config-fingerprint metric. Returns 0, or 500 if `sipi_init` has not
 *  run. */
SIPI_FFI_NODISCARD int sipi_memory_limit_bytes(size_t *out);

/*! Header-only image-shape probe (`SipiImage::read_shape` — no full decode;
 *  one read_shape() call serves both the native shape AND the Essentials
 *  identity, since both come from the same underlying engine read).
 *  `resolved_path` is an already-validated absolute path (the Rust edge owns
 *  existence + containment). Fills `*out` on success. `emit`/`ctx` are
 *  OPTIONAL (NULL `emit` = caller doesn't want the identity, matching the
 *  seam's "NULL = absent" idiom) — when non-NULL, `emit` fires exactly once,
 *  with BOTH the original mimetype and filename, iff the file carries a
 *  parseable Essentials packet; it fires zero times otherwise (a plain
 *  JPEG/PNG, or a TIFF/JP2 without a packet, has no original-file identity to
 *  report — this is not an error). Returns 0, or 500 if the shape cannot be
 *  read within the decode deadline (the edge has already confirmed the file
 *  exists, so an unreadable image here is an engine-level failure). */
SIPI_FFI_NODISCARD int sipi_image_dims(const char *resolved_path,
  SipiImageDims *out,
  SipiEssentialsFn emit,
  void *ctx);

/*! The engine's libmagic MIME type for a file (the same `getBestFileMimetype`
 *  the `/file` and info.json paths use — one source of truth for MIME mapping),
 *  emitted once via `emit`. `resolved_path` is an already-validated absolute
 *  path. Returns 0 (and calls `emit` once) on success, or 500 on error. */
SIPI_FFI_NODISCARD int sipi_mimetype(const char *resolved_path, SipiStrFn emit, void *ctx);

/*! Hands argv verbatim to the existing C++ CLI11 parser; returns the process
 *  exit code (no `exit()`/`abort()` from inside the FFI). */
SIPI_FFI_NODISCARD int sipi_cli_main(int argc, char **argv);

/*! Stamp the C++ engine's server-mode JSON logs on the CALLING thread with the
 *  active trace context (W3C lowercase-hex `trace_id` / `span_id`), so engine
 *  logs correlate to the Rust shell's OpenTelemetry trace. The shell sets it
 *  before a blocking serve call and clears it after (both NULL). NULL/empty
 *  clears it; while clear, the keys are omitted. Engine work runs on the calling
 *  thread, so the thread-local scope is correct. */
void sipi_set_log_trace_context(const char *trace_id, const char *span_id);

/*! Stamp the CALLING thread's outbound-HTTP trace-propagation context: the W3C
 *  `traceparent` the Lua `server.http` client injects on outbound requests so a
 *  downstream service (dsp-api) continues the Rust shell's trace. The shell sets
 *  it from the active span before a preflight/route call and clears it after
 *  (NULL). NULL/empty/malformed clears it (nothing is injected). Thread-local,
 *  like `sipi_set_log_trace_context`; kept separate because propagation needs
 *  the full formatted header (incl. the sampling flag). */
void sipi_set_outbound_traceparent(const char *traceparent);

/* ── SipiImage handle (script-facing image work) ─────────────────────────────
 *
 * The engine surface the Lua runtime's `SipiImage` userdata drives — THE CONTRACT:
 *
 * Ownership. `sipi_image_new` returns a heap-allocated opaque handle owned by
 * the caller; `sipi_image_free` is the only release path (NULL-safe). The
 * Rust userdata frees it in `Drop`, so a killed/unwound VM releases every
 * handle it created. Handles carry no engine-global state; freeing in any
 * order is safe.
 *
 * Inputs. Every `const char*` is borrowed for the synchronous call only and
 * deep-copied on receipt (NULL is normalized to ""). The seam returns no
 * owned strings: values and error text are emitted through caller callbacks
 * (`SipiStrFn`/`SipiKVFn`) with pointers valid only during the emit call.
 *
 * Error channel. Pointer-returning `sipi_image_new` returns NULL on failure
 * and emits one message through its `err` callback — the message is the
 * script-visible `(false, msg)` text, shape-preserved from the historical
 * bindings. `int`-returning entries return 0 on success or a non-zero status
 * with the message emitted through `err`; every entry wraps `sipi_guard`
 * (or the pointer-returning try/catch), so no C++ exception crosses.
 *
 * Reentrancy / threading. A handle is confined to one thread at a time (the
 * request VM's blocking thread); no entry is reentrant on the same handle.
 * Different handles are independent.
 *
 * Callbacks during kill/unwind. `sipi_image_send`'s write callback is Rust
 * code invoked from inside C++ codec frames; the Rust side wraps it in
 * `catch_unwind`, and a failed/blocked sink returns non-zero so the codec
 * aborts — no Rust panic ever unwinds through the C++ frames, and engine
 * calls are uninterruptible by the VM deadline (the deadline bounds Lua and
 * bindings, not decode time).
 *
 * Geometry validation. `region`/`size` arrive as IIIF strings and `reduce`
 * as an integer from Lua, bypassing the IIIF URL parser — the parse
 * constructors validate the grammar here at the seam, `reduce` must be
 * >= 0, and range clamping against the actual dims happens in the engine's
 * crop/scale paths as on the serve path. */

typedef struct SipiImageHandle SipiImageHandle;

/*! Read an image into a new handle. `region`/`size` are IIIF strings (NULL =
 *  absent); `has_reduce` gates `reduce` (>= 0). A non-empty `original`
 *  selects the preservation-aware `readSource` read and records the original
 *  filename. NULL on failure (one message emitted via `err`). */
SipiImageHandle *sipi_image_new(const char *path,
  const char *region,
  const char *size,
  int reduce,
  int has_reduce,
  const char *original,
  SipiStrFn err,
  void *err_ctx);

/*! Free a handle (NULL-safe). */
void sipi_image_free(SipiImageHandle *img);

/*! Dims of an already-open handle (never fails on a live handle). */
SIPI_FFI_NODISCARD int sipi_image_handle_dims(const SipiImageHandle *img,
  uint64_t *nx,
  uint64_t *ny,
  int *orientation);

/*! Header-only shape probe for a path (`read_shape`, no full decode) — the
 *  path form of `SipiImage.dims`. */
SIPI_FFI_NODISCARD int
  sipi_image_file_dims(const char *path, uint64_t *nx, uint64_t *ny, int *orientation, SipiStrFn err, void *err_ctx);

/*! In-place mutations. 0 on success; non-zero with the message emitted. */
SIPI_FFI_NODISCARD int sipi_image_crop(SipiImageHandle *img, const char *iiif_region, SipiStrFn err, void *err_ctx);
SIPI_FFI_NODISCARD int sipi_image_scale(SipiImageHandle *img, const char *iiif_size, SipiStrFn err, void *err_ctx);
SIPI_FFI_NODISCARD int sipi_image_rotate(SipiImageHandle *img, float angle, int mirror, SipiStrFn err, void *err_ctx);
SIPI_FFI_NODISCARD int sipi_image_topleft(SipiImageHandle *img);
SIPI_FFI_NODISCARD int sipi_image_watermark(SipiImageHandle *img, const char *wmfile, SipiStrFn err, void *err_ctx);

/*! EXIF tag read. The tag's typed value is emitted as one JSON document
 *  (string / integer / number / [num,den] rational / arrays thereof) the Lua
 *  runtime converts to the historical Lua shapes. Returns 0 (emitted),
 *  1 (unrecognized tag), 2 (tag not present in this image), 3 (no exif
 *  data), or 500. */
SIPI_FFI_NODISCARD int sipi_image_exif_get(const SipiImageHandle *img, const char *tag, SipiStrFn emit, void *ctx);

/*! The fixed GPS block as one JSON object ({"GPSLatitudeRef": "N",
 *  "GPSLatitude": [d,m,s], …}; absent fields default to 0 / ""). Returns 0,
 *  3 (no exif data), or 500. */
SIPI_FFI_NODISCARD int sipi_image_gps(const SipiImageHandle *img, SipiStrFn emit, void *ctx);

/*! Consistency of the handle's recorded source path against the caller-given
 *  mimetype + filename (libmagic + extension logic). */
SIPI_FFI_NODISCARD int sipi_image_mimetype_consistency(const SipiImageHandle *img,
  const char *mimetype,
  const char *filename,
  int *consistent,
  SipiStrFn err,
  void *err_ctx);

/*! Encode + write to `path`. `ftype` is the resolved handler name
 *  ("tif"/"jpg"/"png"/"jpx" — the Lua runtime maps extensions and validates
 *  the compression-parameter values). `param_keys/values` are the validated
 *  compression parameters (J2K/JPEG knobs by their Lua key names). A
 *  non-empty `origname` + `mimetype` pair requests Service-File stamping:
 *  the Essentials packet (SHA-256 pixel hash, ICC bytes, dims) is built
 *  engine-side and TIFF output is forced pyramidal. */
SIPI_FFI_NODISCARD int sipi_image_write(SipiImageHandle *img,
  const char *ftype,
  const char *path,
  const char *const *param_keys,
  const char *const *param_values,
  size_t n_params,
  const char *origname,
  const char *mimetype,
  SipiStrFn err,
  void *err_ctx);

/*! Encode + stream through `write` (the response sink) — `SipiImage.send`
 *  and the `write("http.<ext>")` streaming form. No Rust panic crosses the
 *  codec frames: the callback is `catch_unwind`-wrapped caller-side and
 *  reports failure via its non-zero return. */
SIPI_FFI_NODISCARD int sipi_image_send(SipiImageHandle *img,
  const char *ftype,
  const char *const *param_keys,
  const char *const *param_values,
  size_t n_params,
  SipiWriteFn write,
  void *write_ctx,
  SipiStrFn err,
  void *err_ctx);

/*! The handle's `__tostring` rendering ("File: <path>" + the image summary). */
SIPI_FFI_NODISCARD int sipi_image_tostring(const SipiImageHandle *img, SipiStrFn emit, void *ctx);

/*! `helper.filename_hash`: the storage-path derivation over
 *  `SipiFilenameHash` (byte-identical — it derives on-disk layout). Returns
 *  0 with the path emitted, or non-zero with the error text emitted. */
SIPI_FFI_NODISCARD int sipi_filename_hash(const char *filename, SipiStrFn emit, void *ctx);

/*! `server.file_mimetype`: libmagic sniff of `path`; emits ("mimetype", v)
 *  and, when non-empty, ("charset", v). Non-zero → error text via `err`. */
SIPI_FFI_NODISCARD int sipi_file_mimetype(const char *path, SipiKVFn emit, void *ctx, SipiStrFn err, void *err_ctx);

/*! `server.file_mimeconsistency`: does the file's actual content match the
 *  expected mimetype/extension for `filename`? */
SIPI_FFI_NODISCARD int sipi_file_mimeconsistency(const char *path,
  const char *filename,
  const char *expected_mimetype,
  int *consistent,
  SipiStrFn err,
  void *err_ctx);

#ifdef __cplusplus
}
#endif

#endif /* SIPI_FFI_SIPI_FFI_H */
