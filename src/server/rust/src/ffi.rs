//! Rust bindings for the narrow C FFI seam (`src/ffi/sipi_ffi.h`).
//!
//! Hand-written rather than bindgen-generated: the seam is a small, locked
//! `extern "C"` contract (ADR-0013), so mirroring it by
//! hand keeps the Bazel graph free of a bindgen build step and keeps the Rust
//! types readable. Each declaration here tracks `sipi_ffi.h` 1:1 — when the
//! header changes, this file changes in lock-step.
//!
//! Safety contract (from the header): no C++ exception crosses the boundary
//! (every entry is `sipi_guard`-wrapped → status code); all `*const c_char` /
//! struct inputs are caller-owned and must outlive the (blocking) call; the
//! response is emitted only through the [`SipiResponse`] callbacks.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;

use crate::config::{OverridesHolder, ServerOverrides, SipiServerConfig};

// ── Response sink callbacks (Rust-owned) ────────────────────────────────────
// `Option<extern "C" fn ...>` is the FFI-safe nullable function pointer: `None`
// is a null pointer on the C side, matching a sink that does not implement a
// given delivery mode.

/// Body bytes, forward-only, unknown length (image encoder → chunked framing).
/// Returns 0 on success, non-zero on a write failure.
pub type SipiWriteFn = extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize) -> c_int;

/// Known-length file region `[offset, offset+length)` (→ Content-Length framing,
/// zero-copy where possible). Returns 0 on success, non-zero on a write failure.
pub type SipiSendFileFn =
    extern "C" fn(ctx: *mut c_void, path: *const c_char, offset: u64, length: u64) -> c_int;

/// One call per response header line (`Set-Cookie` may repeat).
pub type SipiHeaderFn = extern "C" fn(ctx: *mut c_void, name: *const c_char, value: *const c_char);

/// HTTP status code for the response.
pub type SipiStatusFn = extern "C" fn(ctx: *mut c_void, status: c_int);

/// Polled between pipeline stages; 1 = client gone / timed out → abort.
pub type SipiCancelledFn = extern "C" fn(ctx: *mut c_void) -> c_int;

/// The response sink the engine drives. A body is delivered either as a
/// known-length file region (`send_file`) or an unknown-length byte stream
/// (`write`) — never both.
#[repr(C)]
pub struct SipiResponse {
    pub ctx: *mut c_void,
    pub set_status: Option<SipiStatusFn>,
    pub add_header: Option<SipiHeaderFn>,
    pub write: Option<SipiWriteFn>,
    pub send_file: Option<SipiSendFileFn>,
    pub cancelled: Option<SipiCancelledFn>,
}

// ── IIIF serve request (consumed by sipi_serve_image) ───────────────────────
// repr(C) mirrors of the seam structs/enums in `sipi_ffi.h`. The integer enum
// values match the C++ `iiifparser` enums 1:1 (verified against the header), so
// the Rust IIIF parser produces them directly.

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SipiRegionType {
    Full = 0,
    Square = 1,
    Coords = 2,
    Percents = 3,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SipiSizeType {
    Undefined = 0,
    Full = 1,
    PixelsXy = 2,
    PixelsX = 3,
    PixelsY = 4,
    Maxdim = 5,
    Percents = 6,
    Reduce = 7,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SipiQualityType {
    Default = 0,
    Color = 1,
    Gray = 2,
    Bitonal = 3,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SipiFormatType {
    Unsupported = 0,
    Jpg = 1,
    Tif = 2,
    Png = 3,
    Gif = 4,
    Jp2 = 5,
    Pdf = 6,
    Webp = 7,
}

// Compile-time value guard for the IIIF request enums — paired with the C++
// `static_assert`s in `sipi_ffi.h`. Each discriminant is the seam contract; a
// one-sided renumber (here or in the header) breaks this side's assert before it
// can silently mis-tag a region/size/quality/format across the boundary.
const _: () = {
    assert!(SipiRegionType::Full as isize == 0);
    assert!(SipiRegionType::Square as isize == 1);
    assert!(SipiRegionType::Coords as isize == 2);
    assert!(SipiRegionType::Percents as isize == 3);

    assert!(SipiSizeType::Undefined as isize == 0);
    assert!(SipiSizeType::Full as isize == 1);
    assert!(SipiSizeType::PixelsXy as isize == 2);
    assert!(SipiSizeType::PixelsX as isize == 3);
    assert!(SipiSizeType::PixelsY as isize == 4);
    assert!(SipiSizeType::Maxdim as isize == 5);
    assert!(SipiSizeType::Percents as isize == 6);
    assert!(SipiSizeType::Reduce as isize == 7);

    assert!(SipiQualityType::Default as isize == 0);
    assert!(SipiQualityType::Color as isize == 1);
    assert!(SipiQualityType::Gray as isize == 2);
    assert!(SipiQualityType::Bitonal as isize == 3);

    assert!(SipiFormatType::Unsupported as isize == 0);
    assert!(SipiFormatType::Jpg as isize == 1);
    assert!(SipiFormatType::Tif as isize == 2);
    assert!(SipiFormatType::Png as isize == 3);
    assert!(SipiFormatType::Gif as isize == 4);
    assert!(SipiFormatType::Jp2 as isize == 5);
    assert!(SipiFormatType::Pdf as isize == 6);
    assert!(SipiFormatType::Webp as isize == 7);
};

/// Flattened IIIF region/size/rotation/quality.format — mirrors `SipiIiifParams`
/// in `sipi_ffi.h`. `c_int` ⇔ C `int`, `usize` ⇔ C `size_t`, `f32` ⇔ C `float`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SipiIiifParams {
    pub region_type: SipiRegionType,
    pub region: [f32; 4],
    pub size_type: SipiSizeType,
    pub size_upscaling: c_int,
    pub size_percent: f32,
    pub size_reduce: c_int,
    pub size_nx: usize,
    pub size_ny: usize,
    pub rotation: f32,
    pub rotation_mirror: c_int,
    pub quality_type: SipiQualityType,
    pub format_type: SipiFormatType,
}

// ── Domain → seam mapping ────────────────────────────────────────────────────
// The `iiif_parser` crate emits idiomatic domain types; these `From` impls
// flatten them into the `#[repr(C)]` seam struct/enums above. They are the sole
// bridge, applied once at the `serve_image` seam site. Each is an **exhaustive
// match** (never an `x as c_int` cast) so a new domain variant fails to compile
// until it is mapped here — the compile-time complement to the discriminant
// asserts below, which guard the wire values against the C++ header.

impl From<iiif_parser::RegionKind> for SipiRegionType {
    fn from(k: iiif_parser::RegionKind) -> Self {
        match k {
            iiif_parser::RegionKind::Full => SipiRegionType::Full,
            iiif_parser::RegionKind::Square => SipiRegionType::Square,
            iiif_parser::RegionKind::Coords => SipiRegionType::Coords,
            iiif_parser::RegionKind::Percents => SipiRegionType::Percents,
        }
    }
}

impl From<iiif_parser::SizeKind> for SipiSizeType {
    fn from(k: iiif_parser::SizeKind) -> Self {
        match k {
            iiif_parser::SizeKind::Undefined => SipiSizeType::Undefined,
            iiif_parser::SizeKind::Full => SipiSizeType::Full,
            iiif_parser::SizeKind::PixelsXy => SipiSizeType::PixelsXy,
            iiif_parser::SizeKind::PixelsX => SipiSizeType::PixelsX,
            iiif_parser::SizeKind::PixelsY => SipiSizeType::PixelsY,
            iiif_parser::SizeKind::Maxdim => SipiSizeType::Maxdim,
            iiif_parser::SizeKind::Percents => SipiSizeType::Percents,
            iiif_parser::SizeKind::Reduce => SipiSizeType::Reduce,
        }
    }
}

impl From<iiif_parser::QualityKind> for SipiQualityType {
    fn from(k: iiif_parser::QualityKind) -> Self {
        match k {
            iiif_parser::QualityKind::Default => SipiQualityType::Default,
            iiif_parser::QualityKind::Color => SipiQualityType::Color,
            iiif_parser::QualityKind::Gray => SipiQualityType::Gray,
            iiif_parser::QualityKind::Bitonal => SipiQualityType::Bitonal,
        }
    }
}

impl From<iiif_parser::FormatKind> for SipiFormatType {
    fn from(k: iiif_parser::FormatKind) -> Self {
        match k {
            iiif_parser::FormatKind::Unsupported => SipiFormatType::Unsupported,
            iiif_parser::FormatKind::Jpg => SipiFormatType::Jpg,
            iiif_parser::FormatKind::Tif => SipiFormatType::Tif,
            iiif_parser::FormatKind::Png => SipiFormatType::Png,
            iiif_parser::FormatKind::Gif => SipiFormatType::Gif,
            iiif_parser::FormatKind::Jp2 => SipiFormatType::Jp2,
            iiif_parser::FormatKind::Pdf => SipiFormatType::Pdf,
            iiif_parser::FormatKind::Webp => SipiFormatType::Webp,
        }
    }
}

impl From<iiif_parser::IiifParams> for SipiIiifParams {
    fn from(p: iiif_parser::IiifParams) -> Self {
        SipiIiifParams {
            region_type: p.region_kind.into(),
            region: p.region,
            size_type: p.size_kind.into(),
            size_upscaling: c_int::from(p.size_upscaling),
            size_percent: p.size_percent,
            size_reduce: p.size_reduce,
            size_nx: p.size_nx,
            size_ny: p.size_ny,
            rotation: p.rotation,
            rotation_mirror: c_int::from(p.rotation_mirror),
            quality_type: p.quality_kind.into(),
            format_type: p.format_kind.into(),
        }
    }
}

/// Flat, engine-populated context for a handled (non-fatal) image error —
/// mirrors `SipiImageErrorReport` in `sipi_ffi.h`. Every field is only valid for the
/// duration of the `report_error` call — copy anything that needs to outlive
/// it. All fields are 8-byte-wide (pointers / `u64`), so there is no interior
/// padding to reason about; the ABI is still guarded below against an
/// accidental field reorder or insertion.
#[repr(C)]
pub struct SipiImageErrorReport {
    pub phase: *const c_char,
    pub message: *const c_char,
    pub input_file: *const c_char,
    pub output_format: *const c_char,
    pub colorspace: *const c_char,
    pub icc_profile_type: *const c_char,
    pub orientation: *const c_char,
    pub width: u64,
    pub height: u64,
    pub channels: u64,
    pub bps: u64,
    pub file_size_bytes: u64,
}

/// Reports a handled image error's context as a side-channel (mirrors
/// `SipiReportErrorFn`) — never a response. `ctx` is the opaque data from
/// `SipiServeRequest::report_ctx`.
pub type SipiReportErrorFn = extern "C" fn(ctx: *mut c_void, err: *const SipiImageErrorReport);

/// Decode-admission hook (mirrors `SipiBeforeDecodeFn`): called at most once,
/// after the cache check and before the decode. Non-zero refuses the decode,
/// and the engine answers 503. `ctx` is `SipiServeRequest::before_decode_ctx`.
pub type SipiBeforeDecodeFn = extern "C" fn(ctx: *mut c_void) -> c_int;

/// Flat snapshot of the engine metrics singleton — mirrors `SipiMetricsSnapshot`
/// in `src/ffi/metrics_snapshot.h`. Monotonic counters are `u64`; gauges are
/// `i64` (signed: `cache_size_limit_bytes` is `-1` when unlimited). Every field
/// is 8 bytes wide, so there is no interior padding; the ABI is guarded below
/// against an accidental field reorder or insertion. The OTel meter bridge
/// (`crate::metrics`) reads this each collection.
///
/// Two counters are never written on the FFI serve path, so they stay zero and
/// the bridge does not expose them: `rejected_connections_total` and
/// `waiting_connections`. The shell tracks its own equivalents Rust-side instead
/// (the two-lane pool's per-partition `sipi.admission.{tile,full}_waiting`/`_shed`).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct SipiMetricsSnapshot {
    pub cache_hits_total: u64,
    pub cache_misses_total: u64,
    pub cache_evictions_total: u64,
    pub cache_skips_total: u64,
    pub client_disconnected_total: u64,
    pub memory_alloc_failures_total: u64,
    pub rejected_connections_total: u64,
    pub decode_memory_acquired_total: u64,
    pub decode_memory_rejected_total: u64,
    pub decode_memory_shadow_rejected_total: u64,
    pub decode_memory_near_limit_total: u64,
    pub tiff_pyramid_reduced_decodes_total: u64,
    pub decode_memory_too_large_total: u64,
    pub decode_memory_shadow_too_large_total: u64,
    pub waiting_connections: i64,
    pub cache_size_bytes: i64,
    pub cache_files: i64,
    pub cache_size_limit_bytes: i64,
    pub cache_files_limit: i64,
    pub decode_memory_budget_bytes: i64,
    pub decode_memory_used_bytes: i64,
    pub wedged_threads: i64,
}

/// The IIIF serve request — mirrors `SipiServeRequest` in `sipi_ffi.h`. All
/// `*const c_char` fields are caller-owned and must outlive the (synchronous)
/// `sipi_serve_image` call; null is allowed where the header documents it.
#[repr(C)]
pub struct SipiServeRequest {
    pub resolved_path: *const c_char,
    pub prefix: *const c_char,
    pub identifier: *const c_char,
    pub params: SipiIiifParams,
    pub restricted_width: u32,
    pub restricted_height: u32,
    pub watermark_path: *const c_char,
    pub forwarded_proto: *const c_char,
    pub forwarded_host: *const c_char,
    pub request_uri: *const c_char,
    pub is_head: c_int,
    pub report_error: Option<SipiReportErrorFn>,
    pub report_ctx: *mut c_void,
    pub before_decode: Option<SipiBeforeDecodeFn>,
    pub before_decode_ctx: *mut c_void,
}

/// Native image shape from a header read — mirrors `SipiImageDims` in
/// `sipi_ffi.h`. `numpages` is 0 for a single-page image; `tile_width`/
/// `tile_height` are 0 when untiled. `width`/`height`/`tile_*` are enough to
/// assemble info.json's `sizes[]` / `tiles[]` from one probe — the pyramid is
/// derived from the tile grid.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SipiImageDims {
    pub width: u32,
    pub height: u32,
    pub numpages: u32,
    pub tile_width: u32,
    pub tile_height: u32,
}

/// Emits a single string value (mirrors `SipiStrFn`) — the seam returns no owned
/// C string, so `sipi_mimetype` hands its result back through this callback.
pub type SipiStrFn = extern "C" fn(ctx: *mut c_void, value: *const c_char);

/// Emits an image's Essentials-packet identity — original mimetype + original
/// filename — together (mirrors `SipiEssentialsFn`). Called at most once: both
/// strings are known together or not at all.
pub type SipiEssentialsFn =
    extern "C" fn(ctx: *mut c_void, orig_mimetype: *const c_char, orig_filename: *const c_char);

// ── Preflight (auth) ────────────────────────────────────────────────────────

/// Permission type a Lua preflight hook returns (mirrors `SipiPermType`). The
/// discriminants match the C enum 1:1.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SipiPermType {
    Allow = 0,
    Login = 1,
    Clickthrough = 2,
    Kiosk = 3,
    External = 4,
    Restrict = 5,
    Deny = 6,
    Stream = 7,
}

// Compile-time value guard for the preflight permission enum — paired with the
// C++ `static_assert`s in `sipi_ffi.h`. A one-sided renumber breaks this assert
// before a permission can be silently misread across the boundary.
const _: () = {
    assert!(SipiPermType::Allow as isize == 0);
    assert!(SipiPermType::Login as isize == 1);
    assert!(SipiPermType::Clickthrough as isize == 2);
    assert!(SipiPermType::Kiosk as isize == 3);
    assert!(SipiPermType::External as isize == 4);
    assert!(SipiPermType::Restrict as isize == 5);
    assert!(SipiPermType::Deny as isize == 6);
    assert!(SipiPermType::Stream as isize == 7);
};

/// The preflight key/value emit callback (mirrors `SipiKVFn`).
pub type SipiKVFn = extern "C" fn(ctx: *mut c_void, key: *const c_char, value: *const c_char);

/// Number of serve phases — matches the C `SIPI_PHASE_COUNT`. The
/// `serve_timings_layout` test asserts this equals the engine's own
/// `sipi_phase_count()` so a one-sided change fails loudly.
pub const PHASE_COUNT: usize = 6;

/// Serve phase, mirroring the C `SipiPhase` enum (`ffi/sipi_ffi.h`) index-for-
/// index. Used to index [`PHASE_SPAN_NAMES`] and the `SipiServeTimings` arrays;
/// the order is guarded against the span names by the `serve_timings_layout`
/// test.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SipiPhase {
    Shape = 0,
    Decode = 1,
    Rotate = 2,
    Quality = 3,
    Watermark = 4,
    Encode = 5,
}

/// Low-cardinality child-span name per phase index, matching the [`SipiPhase`]
/// order (shape, decode, rotate, quality, watermark, encode).
pub const PHASE_SPAN_NAMES: [&str; PHASE_COUNT] = [
    "sipi.engine.shape",
    "sipi.engine.decode",
    "sipi.engine.rotate",
    "sipi.engine.quality",
    "sipi.engine.watermark",
    "sipi.engine.encode",
];

/// What one `sipi_serve_image` call observed about itself — the hand-mirrored
/// layout of the C `SipiServeTimings` (`ffi/sipi_ffi.h`). `start_ns[i]`/`dur_ns[i]`
/// are nanosecond offset-from-call-start + duration for phase `i`; `present[i]`
/// is 0 when the phase did not run, and `failed[i]` is 1 when the phase exited
/// via an exception (the shell then marks that span errored).
/// `decode_estimate_bytes` is the estimated peak decode memory the budget
/// admitted against, 0 when no decode ran. Kept in lock-step with the C struct by
/// the `serve_timings_layout` test below.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct SipiServeTimings {
    pub start_ns: [u64; PHASE_COUNT],
    pub dur_ns: [u64; PHASE_COUNT],
    pub present: [u8; PHASE_COUNT],
    pub failed: [u8; PHASE_COUNT],
    pub decode_estimate_bytes: u64,
}

/// Take the calling thread's per-serve observations. Call right after
/// [`sipi_serve_image`] on the same thread; every `present` is 0 when the engine
/// recorded nothing (e.g. a cache hit or HEAD, which skip decode/encode).
#[must_use]
pub fn serve_timings_take() -> SipiServeTimings {
    let mut out = SipiServeTimings {
        start_ns: [0; PHASE_COUNT],
        dur_ns: [0; PHASE_COUNT],
        present: [0; PHASE_COUNT],
        failed: [0; PHASE_COUNT],
        decode_estimate_bytes: 0,
    };
    // SAFETY: `out` is a valid, fully-initialised SipiServeTimings; the FFI only
    // writes its fields for the duration of the call and never retains the pointer.
    unsafe { sipi_serve_timings_take(&mut out) };
    out
}

/// The engine's baked version — `version.txt`, via Bazel's build stamp. `None`
/// only if the stamp somehow held non-UTF-8 bytes.
#[must_use]
pub fn build_version() -> Option<&'static str> {
    // SAFETY: the seam returns a non-null string literal with static storage
    // duration, so the borrow is sound for the process lifetime.
    unsafe { CStr::from_ptr(sipi_build_version()) }
        .to_str()
        .ok()
}

/// The engine's baked commit SHA (see [`build_version`]).
#[must_use]
pub fn build_commit() -> Option<&'static str> {
    // SAFETY: as `build_version` — a static string literal, never null.
    unsafe { CStr::from_ptr(sipi_build_commit()) }.to_str().ok()
}

extern "C" {
    /// IIIF decode→transform→encode→stream; honours the restrict size/watermark.
    /// Returns 0 when the response was emitted via the sink, or an HTTP status
    /// code (400/403/404/500/…) on a pre-commit failure (403 = a restrict
    /// decision that does not actually restrict resolution or add a
    /// watermark; 499 = client gone → no response). The engine reads
    /// `engine_context()`, so `sipi_init` must have run first.
    pub fn sipi_serve_image(req: *const SipiServeRequest, resp: *const SipiResponse) -> c_int;

    /// Copy the current thread's per-serve observations into `*out`. Call on the
    /// same thread right after [`sipi_serve_image`] returns; a null `out` is a
    /// no-op. Never fails, emits nothing.
    pub fn sipi_serve_timings_take(out: *mut SipiServeTimings);

    /// The engine's serve-phase count (`SIPI_PHASE_COUNT`), asserted equal to
    /// [`PHASE_COUNT`] by the layout test so a one-sided phase-count change fails
    /// loudly instead of overflowing the Rust-sized [`SipiServeTimings`].
    pub fn sipi_phase_count() -> c_int;

    /// The engine's baked build stamp — `version.txt` and the commit SHA. Both
    /// return a string literal with static storage duration, never null, so the
    /// borrow is good for the process lifetime.
    pub fn sipi_build_version() -> *const c_char;
    pub fn sipi_build_commit() -> *const c_char;

    /// Raw `/file` passthrough incl. HTTP Range / 206 — no decode.
    /// `resolved_path` is an already-validated absolute path; `range` is the raw
    /// `Range` header value or null. Returns 0 when the response was emitted, or
    /// an HTTP status code (404/400/500) on a pre-commit failure.
    pub fn sipi_serve_file(
        resolved_path: *const c_char,
        range: *const c_char,
        resp: *const SipiResponse,
    ) -> c_int;

    /// Parse the Lua config and install the engine + Lua config from scratch.
    /// Must run once before any serve call (`engine_context()` hard-fails
    /// otherwise). `overrides` is the `SipiServerConfig*` CLI/env override
    /// channel the engine layers over the Lua config (null = no overrides).
    /// Returns 0 on success, non-zero on failure.
    ///
    /// `pub(crate)` (unlike the sibling bindings): it names the crate-private
    /// `SipiServerConfig`, so wider visibility would trip `private_interfaces`.
    pub(crate) fn sipi_init(overrides: *const SipiServerConfig) -> c_int;

    /// The configured image root (`resolved` = 0 → raw config value for the path
    /// build; 1 → realpath()-resolved root for the containment check). `*out` is
    /// set to process-static memory owned by the engine (never freed). Returns 0,
    /// or 500 if `sipi_init` has not run.
    pub fn sipi_imgroot(resolved: c_int, out: *mut *const c_char) -> c_int;

    /// The `/server` fileserver docroot (raw config value; the edge canonicalises
    /// per request). `*out` is empty when no fileserver is configured. `*out` is
    /// process-static engine memory. Returns 0, or 500 if `sipi_init` has not run.
    pub fn sipi_docroot(out: *mut *const c_char) -> c_int;

    /// The URL prefix the docroot fileserver is mounted at (e.g. "/server").
    /// `*out` is empty when no fileserver is configured. Returns 0, or 500 if
    /// `sipi_init` has not run.
    pub fn sipi_wwwroute(out: *mut *const c_char) -> c_int;

    /// The `prefix_as_path` config knob (`*out` = 1/0). Returns 0, or 500 if
    /// `sipi_init` has not run.
    pub fn sipi_prefix_as_path(out: *mut c_int) -> c_int;

    /// The configured max POST body size in bytes (`*out`); 0 = unlimited.
    /// Returns 0, or 500 if `sipi_init` has not run.
    pub fn sipi_max_post_size(out: *mut usize) -> c_int;

    /// The resolved admission mode ("basic"|"advanced"). `*out` is process-static
    /// engine memory. Returns 0, or 500 if `sipi_init` has not run.
    pub fn sipi_admission_mode(out: *mut *const c_char) -> c_int;

    /// The tile reserve fraction (`tiles_memory_ratio`). Returns 0, or 500 if
    /// `sipi_init` has not run.
    pub fn sipi_tiles_memory_ratio(out: *mut f64) -> c_int;

    /// The tile/full classifier threshold in bytes. Returns 0, or 500 if
    /// `sipi_init` has not run.
    pub fn sipi_large_decode_threshold_bytes(out: *mut usize) -> c_int;

    /// The resolved RAM envelope in bytes (`memory_limit`, post auto-detect).
    /// Returns 0, or 500 if `sipi_init` has not run.
    pub fn sipi_memory_limit_bytes(out: *mut usize) -> c_int;

    /// Snapshot the engine metrics singleton (cache /
    /// decode-memory / admission counters + gauges) into `out`, a caller-owned
    /// buffer. A pure `sipi_guard`-only read — no response sink, no fallible
    /// pre-commit step. Returns 0, or non-zero on an internal error.
    pub fn sipi_metrics_snapshot(out: *mut SipiMetricsSnapshot) -> c_int;

    /// Header-only image-shape probe (no full decode) — also optionally emits
    /// the Essentials identity from the SAME read via `emit`/`ctx` (`None` =
    /// caller doesn't want it, e.g. info.json). When `emit` is present, it
    /// fires exactly once, with both strings, iff the file carries a
    /// parseable Essentials packet; zero times otherwise (not an error).
    /// `resolved_path` is an already-validated absolute path. Returns 0 (and
    /// fills `*out`), or 500.
    pub fn sipi_image_dims(
        resolved_path: *const c_char,
        out: *mut SipiImageDims,
        emit: Option<SipiEssentialsFn>,
        ctx: *mut c_void,
    ) -> c_int;

    /// The engine's libmagic MIME type for a file, emitted once via `emit`.
    /// `resolved_path` is an already-validated absolute path. Returns 0, or 500.
    pub fn sipi_mimetype(resolved_path: *const c_char, emit: SipiStrFn, ctx: *mut c_void) -> c_int;

    /// Stamp the C++ engine's server-mode JSON logs on the calling thread with
    /// the active trace context (lowercase-hex `trace_id`/`span_id`); both NULL
    /// clears it. See `sipi_ffi.h`.
    pub fn sipi_set_log_trace_context(trace_id: *const c_char, span_id: *const c_char);

    /// Stamp the calling thread's outbound `traceparent` — the W3C header the
    /// engine's Lua `server.http` client injects on outbound requests so a
    /// downstream service continues this trace. NULL clears it. See `sipi_ffi.h`.
    pub fn sipi_set_outbound_traceparent(traceparent: *const c_char);
}

/// Stamps the C++ engine's logs (on the current thread) with a trace context for
/// the guard's lifetime, clearing on drop. Held across a blocking FFI serve call
/// so the engine's log lines carry the active `trace_id`/`span_id` and correlate
/// with the Rust trace; cleared on drop so a reused `spawn_blocking` thread never
/// leaks a stale id onto the next request.
pub struct LogTraceScope {
    active: bool,
}

impl LogTraceScope {
    /// Set the current thread's engine-log trace context (hex ids from the active
    /// OTel span). Returns a guard that clears it on drop.
    #[must_use]
    pub fn set(trace_id: &str, span_id: &str) -> Self {
        match (CString::new(trace_id), CString::new(span_id)) {
            (Ok(t), Ok(s)) => {
                // SAFETY: both are valid NUL-terminated strings for the call; the
                // engine copies them into a thread-local; the seam guards exceptions.
                unsafe { sipi_set_log_trace_context(t.as_ptr(), s.as_ptr()) };
                Self { active: true }
            }
            _ => Self { active: false },
        }
    }
}

impl Drop for LogTraceScope {
    fn drop(&mut self) {
        if self.active {
            // SAFETY: NULL clears the thread-local; the seam guards exceptions.
            unsafe { sipi_set_log_trace_context(std::ptr::null(), std::ptr::null()) };
        }
    }
}

/// Stamps the C++ engine's outbound Lua HTTP calls (on the current thread) with a
/// W3C `traceparent` for the guard's lifetime, clearing on drop. Held around a
/// preflight/route FFI call so the engine's `server.http` request to dsp-api
/// carries the active trace context and dsp-api continues the trace; cleared on
/// drop so a reused `spawn_blocking` thread never leaks a stale traceparent onto
/// the next request.
///
/// **Not nestable:** `Drop` clears the thread-local, it does not restore a prior
/// value. Only one scope may be live per thread at a time — the preflight,
/// file-preflight, and lua-route call sites are mutually exclusive, so this holds
/// by construction. A future nested use would need a save/restore stack instead.
pub struct OutboundTraceScope {
    active: bool,
}

impl OutboundTraceScope {
    /// Set the current thread's outbound `traceparent` (from the active OTel
    /// span, via [`crate::telemetry::current_traceparent`]). Returns a guard that
    /// clears it on drop.
    #[must_use]
    pub fn set(traceparent: &str) -> Self {
        match CString::new(traceparent) {
            Ok(tp) => {
                // SAFETY: valid NUL-terminated string for the call; the engine
                // validates + copies it into a thread-local; the seam guards
                // exceptions.
                unsafe { sipi_set_outbound_traceparent(tp.as_ptr()) };
                Self { active: true }
            }
            Err(_) => Self { active: false },
        }
    }
}

impl Drop for OutboundTraceScope {
    fn drop(&mut self) {
        if self.active {
            // SAFETY: NULL clears the thread-local; the seam guards exceptions.
            unsafe { sipi_set_outbound_traceparent(std::ptr::null()) };
        }
    }
}

/// Startup link self-check: forces the C++ engine `cc_library` to link into this
/// `rust_binary` and proves a round-trip across the seam. Calls `sipi_serve_file`
/// on a path that cannot exist, so the engine returns 404 (`access(2)` fails
/// before any I/O or sink callback) without side effects. Returns the status
/// code (expected 404).
pub fn link_self_check() -> i32 {
    let resp = SipiResponse {
        ctx: std::ptr::null_mut(),
        set_status: None,
        add_header: None,
        write: None,
        send_file: None,
        cancelled: None,
    };
    let bogus = c"/sipi-rust-shell-link-self-check/does-not-exist";
    // SAFETY: `bogus` is a valid NUL-terminated C string that outlives the call;
    // `range` is null (no Range header); `resp` outlives the synchronous call.
    // The seam guarantees no exception unwinds across the boundary.
    unsafe { sipi_serve_file(bogus.as_ptr(), std::ptr::null(), &resp) as i32 }
}

/// Parse the Lua config and install the engine + Lua config (`sipi_init`). Must
/// run once before serving images. `overrides` carries the parsed CLI/env flags
/// the engine layers over the loaded config. Returns the FFI status code on
/// failure (non-zero).
pub fn init(overrides: &ServerOverrides) -> Result<(), i32> {
    let holder = match OverridesHolder::new(overrides) {
        Ok(h) => h,
        Err(_) => return Err(-1), // interior NUL in a config string value
    };
    // SAFETY: `holder` outlives this synchronous call; the engine deep-copies
    // every present override during sipi_init, so none of the holder's pointers
    // escape; the seam guards exceptions. `holder` is a local consumed inline,
    // so it is not moved between `as_ptr()` and the call.
    let code = unsafe { sipi_init(holder.as_ptr()) };
    if code == 0 {
        Ok(())
    } else {
        Err(code)
    }
}

/// The configured image root. `resolved = false` → the raw config value (used to
/// build the request path, as the engine's `imgroot()` returns it); `resolved = true`
/// → the realpath()-resolved root (used for the R2 containment check). Returns
/// an owned copy — the underlying C string is process-static, but the edge holds
/// these in its own config so a copy keeps lifetimes simple. `Err` carries the
/// FFI status (500 if `sipi_init` has not run).
pub fn imgroot(resolved: bool) -> Result<String, i32> {
    let mut ptr: *const c_char = std::ptr::null();
    // SAFETY: `out` is a valid pointer; on success the engine writes a
    // process-static, NUL-terminated pointer; the seam guards exceptions.
    let code = unsafe { sipi_imgroot(resolved as c_int, &mut ptr) };
    if code != 0 {
        return Err(code);
    }
    if ptr.is_null() {
        return Err(-1);
    }
    // SAFETY: `ptr` is a NUL-terminated C string owned by the engine, valid for
    // the process lifetime; we copy it before returning.
    Ok(unsafe { CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned())
}

/// The `/server` fileserver docroot (raw config value). Empty when no fileserver
/// is configured; the shell then registers no static route. The edge canonicalises
/// it per request for the containment check. `Err` carries the FFI status (500 if
/// `sipi_init` has not run, -1 on a null pointer).
pub fn docroot() -> Result<String, i32> {
    cstr_getter(|out| {
        // SAFETY: `out` is the local out-pointer `cstr_getter` supplies; the seam guards exceptions.
        unsafe { sipi_docroot(out) }
    })
}

/// The URL prefix the docroot fileserver is mounted at (e.g. "/server"). Empty
/// when no fileserver is configured. `Err` carries the FFI status (500 if
/// `sipi_init` has not run, -1 on a null pointer).
pub fn wwwroute() -> Result<String, i32> {
    cstr_getter(|out| {
        // SAFETY: `out` is the local out-pointer `cstr_getter` supplies; the seam guards exceptions.
        unsafe { sipi_wwwroute(out) }
    })
}

/// Shared body of the `*const c_char`-out getters (docroot/wwwroute): run the FFI
/// call into a local out-pointer, then copy the process-static C string to an
/// owned `String`. `Err` carries the FFI status, or -1 on a null pointer.
fn cstr_getter(call: impl FnOnce(*mut *const c_char) -> c_int) -> Result<String, i32> {
    let mut ptr: *const c_char = std::ptr::null();
    let code = call(&mut ptr);
    if code != 0 {
        return Err(code);
    }
    if ptr.is_null() {
        return Err(-1);
    }
    // SAFETY: `ptr` is a NUL-terminated C string owned by the engine, valid for
    // the process lifetime; we copy it before returning.
    Ok(unsafe { CStr::from_ptr(ptr) }
        .to_string_lossy()
        .into_owned())
}

/// The `prefix_as_path` config knob: `true` → the IIIF prefix is a path
/// component under imgroot. `Err` carries the FFI status (500 if uninitialised).
pub fn prefix_as_path() -> Result<bool, i32> {
    let mut v: c_int = 0;
    // SAFETY: `out` is a valid pointer; the seam guards exceptions.
    let code = unsafe { sipi_prefix_as_path(&mut v) };
    if code != 0 {
        return Err(code);
    }
    Ok(v != 0)
}

/// The configured max POST body size in bytes (`max_post_size`), as the engine
/// holds it; `0` means the config left it unset. The shell maps that to a finite
/// default rather than to "unlimited" (see `routes::AppState::load`). `Err`
/// carries the FFI status (500 if `sipi_init` has not run).
pub fn max_post_size() -> Result<usize, i32> {
    let mut v: usize = 0;
    // SAFETY: `out` is a valid pointer; the seam guards exceptions.
    let code = unsafe { sipi_max_post_size(&mut v) };
    if code != 0 {
        return Err(code);
    }
    Ok(v)
}

/// The resolved admission mode ("basic"|"advanced"), read back so the shell's
/// two-lane pool runs the same mode the engine's memory budget does. `Err`
/// carries the FFI status (500 if `sipi_init` has not run, -1 on a null pointer).
pub fn admission_mode() -> Result<String, i32> {
    cstr_getter(|out| {
        // SAFETY: `out` is the local out-pointer `cstr_getter` supplies; the seam guards exceptions.
        unsafe { sipi_admission_mode(out) }
    })
}

/// The tile reserve fraction (`tiles_memory_ratio`) for the config-fingerprint
/// metric. `Err` carries the FFI status (500 if `sipi_init` has not run).
pub fn tiles_memory_ratio() -> Result<f64, i32> {
    let mut v: f64 = 0.0;
    // SAFETY: `out` is a valid pointer; the seam guards exceptions.
    let code = unsafe { sipi_tiles_memory_ratio(&mut v) };
    if code != 0 {
        return Err(code);
    }
    Ok(v)
}

/// The tile/full classifier threshold in bytes, read back so the shell classifies
/// against the same value the engine charges the budget by. `Err` carries the FFI
/// status (500 if `sipi_init` has not run).
pub fn large_decode_threshold_bytes() -> Result<u64, i32> {
    let mut v: usize = 0;
    // SAFETY: `out` is a valid pointer; the seam guards exceptions.
    let code = unsafe { sipi_large_decode_threshold_bytes(&mut v) };
    if code != 0 {
        return Err(code);
    }
    Ok(v as u64)
}

/// The resolved RAM envelope in bytes (`memory_limit`, post auto-detect) for the
/// config-fingerprint metric. `Err` carries the FFI status (500 if `sipi_init` has
/// not run).
pub fn memory_limit_bytes() -> Result<u64, i32> {
    let mut v: usize = 0;
    // SAFETY: `out` is a valid pointer; the seam guards exceptions.
    let code = unsafe { sipi_memory_limit_bytes(&mut v) };
    if code != 0 {
        return Err(code);
    }
    Ok(v as u64)
}

/// Read the engine metrics singleton into a flat snapshot for the OTel meter.
/// Returns `None` on an internal FFI error — the caller then observes nothing
/// this collection cycle (fail-safe, never a panic on the collection thread).
#[must_use]
pub fn metrics_snapshot() -> Option<SipiMetricsSnapshot> {
    let mut snap = SipiMetricsSnapshot::default();
    // SAFETY: `out` is a valid, writable pointer; the seam guards exceptions.
    let code = unsafe { sipi_metrics_snapshot(&mut snap) };
    if code == 0 {
        Some(snap)
    } else {
        None
    }
}

/// One configured Lua route: HTTP method, the route prefix, and the script
/// path (composed against the config's script dir by the config loader).
#[derive(Clone)]
pub struct RouteEntry {
    pub method: String,
    pub route: String,
    pub script: String,
}

/// Header-only image shape for a validated path (no Essentials identity —
/// the info.json path doesn't need it, so this passes no callback and costs
/// exactly one `read_shape()`; see [`image_dims_and_essentials`] for the
/// knora.json path, which wants both from a single call). `Err` carries the
/// FFI status (500 if the shape cannot be read, or -1 on an interior NUL in
/// the path).
pub fn image_dims(resolved_path: &str) -> Result<SipiImageDims, i32> {
    let c_path = CString::new(resolved_path).map_err(|_| -1)?;
    let mut dims = SipiImageDims::default();
    // SAFETY: `c_path` outlives the synchronous call; `out` is a valid pointer;
    // the seam guards exceptions.
    let code = unsafe { sipi_image_dims(c_path.as_ptr(), &mut dims, None, std::ptr::null_mut()) };
    if code != 0 {
        return Err(code);
    }
    Ok(dims)
}

/// Collects the single emitted MIME string into the `Option<String>` at `ctx`.
extern "C" fn collect_mime(ctx: *mut c_void, value: *const c_char) {
    // Mirror the sink callbacks: a Rust panic must not unwind into C++.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // SAFETY: `ctx` is the `&mut Option<String>` passed to sipi_mimetype.
        let out = unsafe { &mut *(ctx as *mut Option<String>) };
        if !value.is_null() {
            // SAFETY: the engine passes a NUL-terminated C string valid for the call.
            *out = Some(
                unsafe { CStr::from_ptr(value) }
                    .to_string_lossy()
                    .into_owned(),
            );
        }
    }));
}

/// The engine's libmagic MIME type for a validated path (one source of truth
/// with the serve paths). `Err` carries the FFI status (500), or -1 on an
/// interior NUL or a missing emit.
pub fn mimetype(resolved_path: &str) -> Result<String, i32> {
    let c_path = CString::new(resolved_path).map_err(|_| -1)?;
    let mut out: Option<String> = None;
    // SAFETY: `c_path` outlives the synchronous call; `collect_mime` writes into
    // `out` via the ctx pointer; the seam guards exceptions.
    let code = unsafe {
        sipi_mimetype(
            c_path.as_ptr(),
            collect_mime,
            &mut out as *mut Option<String> as *mut c_void,
        )
    };
    if code != 0 {
        return Err(code);
    }
    out.ok_or(-1)
}

/// An image's original-file identity, read from its embedded Essentials
/// packet: the client-declared mimetype and filename at upload time. Both
/// fields are known together or not at all — see
/// [`image_dims_and_essentials`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ImageEssentials {
    pub original_mimetype: String,
    pub original_filename: String,
}

/// Collects the jointly-emitted essentials strings into the
/// `Option<ImageEssentials>` at `ctx`. Fires at most once; a null pointer on
/// either string (should not happen — the probe emits both or neither) skips
/// the write rather than panicking.
extern "C" fn collect_essentials(
    ctx: *mut c_void,
    orig_mimetype: *const c_char,
    orig_filename: *const c_char,
) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // SAFETY: `ctx` is the `&mut Option<ImageEssentials>` passed to
        // sipi_image_dims.
        let out = unsafe { &mut *(ctx as *mut Option<ImageEssentials>) };
        if orig_mimetype.is_null() || orig_filename.is_null() {
            return;
        }
        // SAFETY: the engine passes NUL-terminated C strings valid for the call.
        let (original_mimetype, original_filename) = unsafe {
            (
                CStr::from_ptr(orig_mimetype).to_string_lossy().into_owned(),
                CStr::from_ptr(orig_filename).to_string_lossy().into_owned(),
            )
        };
        *out = Some(ImageEssentials {
            original_mimetype,
            original_filename,
        });
    }));
}

/// Header-only image shape AND original-file identity, for a validated path,
/// from a SINGLE `read_shape()` call (the knora.json path wants both; see
/// [`image_dims`] for the info.json path, which wants only the shape and so
/// pays for no essentials work). The identity is `None` when the file carries
/// no Essentials packet (a plain JPEG/PNG, or a packet-less TIFF/JP2 — not an
/// error). `Err` carries the FFI status (500 if the shape cannot be read), or
/// -1 on an interior NUL in the path.
pub fn image_dims_and_essentials(
    resolved_path: &str,
) -> Result<(SipiImageDims, Option<ImageEssentials>), i32> {
    let c_path = CString::new(resolved_path).map_err(|_| -1)?;
    let mut dims = SipiImageDims::default();
    let mut essentials: Option<ImageEssentials> = None;
    // SAFETY: `c_path` outlives the synchronous call; `out` is a valid
    // pointer; `collect_essentials` writes into `essentials` via the ctx
    // pointer; the seam guards exceptions.
    let code = unsafe {
        sipi_image_dims(
            c_path.as_ptr(),
            &mut dims,
            Some(collect_essentials),
            &mut essentials as *mut Option<ImageEssentials> as *mut c_void,
        )
    };
    if code != 0 {
        return Err(code);
    }
    Ok((dims, essentials))
}

/// Plain-data snapshot of a reported handled-error context, decoupled from
/// the FFI seam's raw pointers so the Sentry event shape it drives
/// ([`build_image_error_event`]) is unit-testable without an engine call.
#[derive(Default)]
struct ImageErrorEventData {
    phase: String,
    message: Option<String>,
    input_file: Option<String>,
    output_format: Option<String>,
    colorspace: Option<String>,
    icc_profile_type: Option<String>,
    orientation: Option<String>,
    request_uri: Option<String>,
    width: u64,
    height: u64,
    channels: u64,
    bps: u64,
    file_size_bytes: u64,
}

/// Builds the `sentry::Event` for a reported handled-error context. Kept
/// free of FFI pointers so its event shape — in particular, that
/// `input_file` is reduced to a basename and `request_uri` lands only in the
/// non-indexed `Image` context, never as a tag — is directly unit-testable.
/// `input_file` and `request_uri` can carry restricted repository paths and
/// full request paths respectively; tags are indexed/searchable, so only the
/// basename (never the directory) and never the request URI are tagged.
fn build_image_error_event(data: ImageErrorEventData) -> sentry::protocol::Event<'static> {
    let ImageErrorEventData {
        phase,
        message,
        input_file,
        output_format,
        colorspace,
        icc_profile_type,
        orientation,
        request_uri,
        width,
        height,
        channels,
        bps,
        file_size_bytes,
    } = data;

    let input_file_basename = input_file.map(|v| {
        Path::new(&v)
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or(v)
    });

    let mut tags = sentry::protocol::Map::new();
    tags.insert("sipi.mode".to_owned(), "server".to_owned());
    tags.insert("sipi.phase".to_owned(), phase);
    if let Some(v) = &output_format {
        tags.insert("sipi.output_format".to_owned(), v.clone());
    }
    if let Some(v) = &colorspace {
        tags.insert("sipi.colorspace".to_owned(), v.clone());
    }
    if bps > 0 {
        tags.insert("sipi.bps".to_owned(), bps.to_string());
    }

    let mut image_data = sentry::protocol::Map::new();
    if let Some(v) = input_file_basename {
        image_data.insert("input_file".to_owned(), v.into());
    }
    if let Some(v) = &output_format {
        image_data.insert("output_format".to_owned(), v.clone().into());
    }
    if width > 0 || height > 0 {
        image_data.insert("width".to_owned(), width.into());
        image_data.insert("height".to_owned(), height.into());
    }
    if channels > 0 {
        image_data.insert("channels".to_owned(), channels.into());
    }
    if bps > 0 {
        image_data.insert("bps".to_owned(), bps.into());
    }
    if let Some(v) = colorspace {
        image_data.insert("colorspace".to_owned(), v.into());
    }
    if let Some(v) = icc_profile_type {
        image_data.insert("icc_profile_type".to_owned(), v.into());
    }
    if let Some(v) = orientation {
        image_data.insert("orientation".to_owned(), v.into());
    }
    if file_size_bytes > 0 {
        image_data.insert("file_size_bytes".to_owned(), file_size_bytes.into());
    }
    if let Some(v) = request_uri {
        image_data.insert("request_uri".to_owned(), v.into());
    }

    let mut contexts = sentry::protocol::Map::new();
    contexts.insert(
        "Image".to_owned(),
        sentry::protocol::Context::Other(image_data),
    );

    sentry::protocol::Event {
        level: sentry::Level::Error,
        message,
        tags,
        contexts,
        ..Default::default()
    }
}

/// Builds a `sentry::Event` from a reported handled-error context and
/// captures it — the engine links no Sentry SDK and reports this
/// handled-error context across the seam instead (a report is
/// always a side-channel, never a body). `ctx` is the request-URI C string
/// the Rust edge passes as `report_ctx` — not part of the flat struct, since
/// the edge already holds it for `SipiServeRequest::request_uri`.
pub(crate) extern "C" fn report_image_error(ctx: *mut c_void, err: *const SipiImageErrorReport) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if err.is_null() {
            return;
        }
        // SAFETY: the engine passes a valid pointer, valid for this call.
        let err = unsafe { &*err };

        let field = |p: *const c_char| -> Option<String> {
            // SAFETY: every string field is either null or a NUL-terminated
            // C string valid for this call, per the seam contract.
            (!p.is_null()).then(|| unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned())
        };
        let request_uri = (!ctx.is_null()).then(|| {
            // SAFETY: `ctx` is either null or the request-URI C string the
            // Rust edge passed as `report_ctx`, valid for this call.
            unsafe { CStr::from_ptr(ctx as *const c_char) }
                .to_string_lossy()
                .into_owned()
        });

        let event = build_image_error_event(ImageErrorEventData {
            phase: field(err.phase).unwrap_or_default(),
            message: field(err.message),
            input_file: field(err.input_file),
            output_format: field(err.output_format),
            colorspace: field(err.colorspace),
            icc_profile_type: field(err.icc_profile_type),
            orientation: field(err.orientation),
            request_uri,
            width: err.width,
            height: err.height,
            channels: err.channels,
            bps: err.bps,
            file_size_bytes: err.file_size_bytes,
        });

        sentry::capture_event(event);
    }));
}

// Lock-step layout guard — paired with the C++ static_assert/offsetof block in
// src/ffi/serve_timings.cpp. The offset asserts catch a field reorder; the
// `size_of` literal and the `sipi_phase_count()` cross-check catch a one-sided
// phase-count change (which the self-consistent offset asserts on either side
// would miss) before it overflows the Rust-sized struct. LP64 on every target.
#[cfg(test)]
mod serve_timings_layout {
    use super::{sipi_phase_count, SipiServeTimings, PHASE_COUNT, PHASE_SPAN_NAMES};
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_sipi_ffi_h() {
        assert_eq!(size_of::<usize>(), 8, "layout assumes an LP64 target");
        assert_eq!(PHASE_COUNT, 6);
        // Cross-check against the engine's own count, not just our literal, so a
        // C-side SIPI_PHASE_COUNT bump without a Rust bump fails here.
        // SAFETY: a pure `return SIPI_PHASE_COUNT` accessor; no state, never fails.
        assert_eq!(PHASE_COUNT, unsafe { sipi_phase_count() } as usize);
        assert_eq!(align_of::<SipiServeTimings>(), 8);
        assert_eq!(size_of::<SipiServeTimings>(), 120);
        assert_eq!(offset_of!(SipiServeTimings, start_ns), 0);
        assert_eq!(offset_of!(SipiServeTimings, dur_ns), PHASE_COUNT * 8);
        assert_eq!(offset_of!(SipiServeTimings, present), 2 * PHASE_COUNT * 8);
        assert_eq!(
            offset_of!(SipiServeTimings, failed),
            2 * PHASE_COUNT * 8 + PHASE_COUNT
        );
        // `failed` ends at 108; the u64 that follows is 8-aligned, so four bytes
        // of padding precede it.
        assert_eq!(offset_of!(SipiServeTimings, decode_estimate_bytes), 112);
    }

    #[test]
    fn span_names_match_phase_order() {
        // Bind the span-name array to the SipiPhase order so a future enum reorder
        // (which the offset/count asserts cannot see) can't silently mislabel spans.
        use super::SipiPhase::*;
        assert_eq!(PHASE_SPAN_NAMES.len(), PHASE_COUNT);
        assert_eq!(PHASE_SPAN_NAMES[Shape as usize], "sipi.engine.shape");
        assert_eq!(PHASE_SPAN_NAMES[Decode as usize], "sipi.engine.decode");
        assert_eq!(PHASE_SPAN_NAMES[Rotate as usize], "sipi.engine.rotate");
        assert_eq!(PHASE_SPAN_NAMES[Quality as usize], "sipi.engine.quality");
        assert_eq!(
            PHASE_SPAN_NAMES[Watermark as usize],
            "sipi.engine.watermark"
        );
        assert_eq!(PHASE_SPAN_NAMES[Encode as usize], "sipi.engine.encode");
    }

    #[test]
    fn take_without_a_serve_is_all_absent() {
        // On a thread that has not run `sipi_serve_image`, the accumulator reads
        // as all-zero / absent — validates the FFI symbol links and the struct
        // marshals across the seam.
        let t = super::serve_timings_take();
        assert!(t.present.iter().all(|&p| p == 0));
        assert!(t.dur_ns.iter().all(|&d| d == 0));
        assert!(t.failed.iter().all(|&f| f == 0));
        assert_eq!(t.decode_estimate_bytes, 0);
    }
}

#[cfg(test)]
mod image_error_layout {
    use super::SipiImageErrorReport;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_sipi_ffi_h() {
        assert_eq!(size_of::<usize>(), 8, "layout assumes an LP64 target");
        assert_eq!(align_of::<SipiImageErrorReport>(), 8);
        assert_eq!(size_of::<SipiImageErrorReport>(), 96);

        assert_eq!(offset_of!(SipiImageErrorReport, phase), 0);
        assert_eq!(offset_of!(SipiImageErrorReport, message), 8);
        assert_eq!(offset_of!(SipiImageErrorReport, input_file), 16);
        assert_eq!(offset_of!(SipiImageErrorReport, output_format), 24);
        assert_eq!(offset_of!(SipiImageErrorReport, colorspace), 32);
        assert_eq!(offset_of!(SipiImageErrorReport, icc_profile_type), 40);
        assert_eq!(offset_of!(SipiImageErrorReport, orientation), 48);
        assert_eq!(offset_of!(SipiImageErrorReport, width), 56);
        assert_eq!(offset_of!(SipiImageErrorReport, height), 64);
        assert_eq!(offset_of!(SipiImageErrorReport, channels), 72);
        assert_eq!(offset_of!(SipiImageErrorReport, bps), 80);
        assert_eq!(offset_of!(SipiImageErrorReport, file_size_bytes), 88);
    }
}

#[cfg(test)]
mod image_error_event_pii {
    use super::{build_image_error_event, ImageErrorEventData};

    #[test]
    fn input_file_is_reduced_to_basename() {
        let event = build_image_error_event(ImageErrorEventData {
            input_file: Some("/sipi/images/0112/abcd.jpx".to_owned()),
            ..Default::default()
        });
        let image = event.contexts.get("Image").expect("Image context present");
        let sentry::protocol::Context::Other(map) = image else {
            panic!("Image context is not a map");
        };
        let input_file = map.get("input_file").and_then(|v| v.as_str());
        assert_eq!(input_file, Some("abcd.jpx"));
    }

    #[test]
    fn request_uri_is_not_an_indexed_tag_but_stays_in_context() {
        let event = build_image_error_event(ImageErrorEventData {
            request_uri: Some("/0112/abcd.jpx/full/max/0/default.jpg".to_owned()),
            ..Default::default()
        });
        assert!(
            !event.tags.contains_key("sipi.request_uri"),
            "request_uri must not be an indexed tag"
        );
        let image = event.contexts.get("Image").expect("Image context present");
        let sentry::protocol::Context::Other(map) = image else {
            panic!("Image context is not a map");
        };
        assert_eq!(
            map.get("request_uri").and_then(|v| v.as_str()),
            Some("/0112/abcd.jpx/full/max/0/default.jpg")
        );
    }
}

// Lock-step layout guard — paired with the C++ static_assert/offsetof block in
// src/ffi/metrics_snapshot.h. Every field is 8 bytes wide (u64 / i64), so there
// is no packing subtlety, but the guard still catches an accidental field
// reorder or insertion on either side. LP64 on every supported target.
#[cfg(test)]
mod metrics_snapshot_layout {
    use super::SipiMetricsSnapshot;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_metrics_snapshot_h() {
        assert_eq!(size_of::<usize>(), 8, "layout assumes an LP64 target");
        assert_eq!(align_of::<SipiMetricsSnapshot>(), 8);
        assert_eq!(size_of::<SipiMetricsSnapshot>(), 176);

        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_hits_total), 0);
        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_misses_total), 8);
        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_evictions_total), 16);
        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_skips_total), 24);
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, client_disconnected_total),
            32
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, memory_alloc_failures_total),
            40
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, rejected_connections_total),
            48
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_acquired_total),
            56
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_rejected_total),
            64
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_shadow_rejected_total),
            72
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_near_limit_total),
            80
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, tiff_pyramid_reduced_decodes_total),
            88
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_too_large_total),
            96
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_shadow_too_large_total),
            104
        );
        assert_eq!(offset_of!(SipiMetricsSnapshot, waiting_connections), 112);
        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_size_bytes), 120);
        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_files), 128);
        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_size_limit_bytes), 136);
        assert_eq!(offset_of!(SipiMetricsSnapshot, cache_files_limit), 144);
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_budget_bytes),
            152
        );
        assert_eq!(
            offset_of!(SipiMetricsSnapshot, decode_memory_used_bytes),
            160
        );
        assert_eq!(offset_of!(SipiMetricsSnapshot, wedged_threads), 168);
    }
}

// Lock-step layout guards for the remaining hand-mirrored seam structs — paired
// with the C++ `static_assert` block in `src/ffi/sipi_ffi.h`. The `size_of`
// literal + `offset_of!` asserts catch a field reorder, width change, or
// insertion on either side before it becomes silent UB across the boundary. LP64
// on every supported target.
#[cfg(test)]
mod response_layout {
    use super::SipiResponse;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_sipi_ffi_h() {
        assert_eq!(size_of::<usize>(), 8, "layout assumes an LP64 target");
        assert_eq!(align_of::<SipiResponse>(), 8);
        assert_eq!(size_of::<SipiResponse>(), 48);
        assert_eq!(offset_of!(SipiResponse, ctx), 0);
        assert_eq!(offset_of!(SipiResponse, set_status), 8);
        assert_eq!(offset_of!(SipiResponse, add_header), 16);
        assert_eq!(offset_of!(SipiResponse, write), 24);
        assert_eq!(offset_of!(SipiResponse, send_file), 32);
        assert_eq!(offset_of!(SipiResponse, cancelled), 40);
    }
}

#[cfg(test)]
mod iiif_params_layout {
    use super::SipiIiifParams;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_sipi_ffi_h() {
        assert_eq!(size_of::<usize>(), 8, "layout assumes an LP64 target");
        assert_eq!(align_of::<SipiIiifParams>(), 8);
        assert_eq!(size_of::<SipiIiifParams>(), 72);
        assert_eq!(offset_of!(SipiIiifParams, region_type), 0);
        assert_eq!(offset_of!(SipiIiifParams, region), 4);
        assert_eq!(offset_of!(SipiIiifParams, size_type), 20);
        assert_eq!(offset_of!(SipiIiifParams, size_upscaling), 24);
        assert_eq!(offset_of!(SipiIiifParams, size_percent), 28);
        assert_eq!(offset_of!(SipiIiifParams, size_reduce), 32);
        assert_eq!(offset_of!(SipiIiifParams, size_nx), 40);
        assert_eq!(offset_of!(SipiIiifParams, size_ny), 48);
        assert_eq!(offset_of!(SipiIiifParams, rotation), 56);
        assert_eq!(offset_of!(SipiIiifParams, rotation_mirror), 60);
        assert_eq!(offset_of!(SipiIiifParams, quality_type), 64);
        assert_eq!(offset_of!(SipiIiifParams, format_type), 68);
    }
}

#[cfg(test)]
mod domain_seam_mapping {
    use super::{SipiFormatType, SipiQualityType, SipiRegionType, SipiSizeType};

    // Every domain enum variant must map to the seam enum with the matching
    // discriminant. An exhaustive per-variant check plus the `bool → c_int`
    // flag mapping below.

    #[test]
    fn region_kind_maps_every_variant() {
        use iiif_parser::RegionKind::*;
        for (k, want) in [
            (Full, SipiRegionType::Full),
            (Square, SipiRegionType::Square),
            (Coords, SipiRegionType::Coords),
            (Percents, SipiRegionType::Percents),
        ] {
            assert_eq!(SipiRegionType::from(k), want);
        }
    }

    #[test]
    fn size_kind_maps_every_variant() {
        use iiif_parser::SizeKind::*;
        for (k, want) in [
            (Undefined, SipiSizeType::Undefined),
            (Full, SipiSizeType::Full),
            (PixelsXy, SipiSizeType::PixelsXy),
            (PixelsX, SipiSizeType::PixelsX),
            (PixelsY, SipiSizeType::PixelsY),
            (Maxdim, SipiSizeType::Maxdim),
            (Percents, SipiSizeType::Percents),
            (Reduce, SipiSizeType::Reduce),
        ] {
            assert_eq!(SipiSizeType::from(k), want);
        }
    }

    #[test]
    fn quality_kind_maps_every_variant() {
        use iiif_parser::QualityKind::*;
        for (k, want) in [
            (Default, SipiQualityType::Default),
            (Color, SipiQualityType::Color),
            (Gray, SipiQualityType::Gray),
            (Bitonal, SipiQualityType::Bitonal),
        ] {
            assert_eq!(SipiQualityType::from(k), want);
        }
    }

    #[test]
    fn format_kind_maps_every_variant() {
        use iiif_parser::FormatKind::*;
        for (k, want) in [
            (Unsupported, SipiFormatType::Unsupported),
            (Jpg, SipiFormatType::Jpg),
            (Tif, SipiFormatType::Tif),
            (Png, SipiFormatType::Png),
            (Gif, SipiFormatType::Gif),
            (Jp2, SipiFormatType::Jp2),
            (Pdf, SipiFormatType::Pdf),
            (Webp, SipiFormatType::Webp),
        ] {
            assert_eq!(SipiFormatType::from(k), want);
        }
    }

    #[test]
    fn iiif_params_flatten_maps_bool_flags_and_fields() {
        let domain = iiif_parser::IiifParams {
            region_kind: iiif_parser::RegionKind::Square,
            region: [1.0, 2.0, 3.0, 4.0],
            size_kind: iiif_parser::SizeKind::Maxdim,
            size_upscaling: true,
            size_percent: 50.0,
            size_reduce: 2,
            size_nx: 640,
            size_ny: 480,
            rotation: 90.0,
            rotation_mirror: true,
            quality_kind: iiif_parser::QualityKind::Color,
            format_kind: iiif_parser::FormatKind::Png,
        };
        let ffi = super::SipiIiifParams::from(domain);
        assert_eq!(ffi.region_type, SipiRegionType::Square);
        assert_eq!(ffi.region, [1.0, 2.0, 3.0, 4.0]);
        assert_eq!(ffi.size_type, SipiSizeType::Maxdim);
        assert_eq!(ffi.size_upscaling, 1); // bool true → c_int 1
        assert_eq!(ffi.size_percent, 50.0);
        assert_eq!(ffi.size_reduce, 2);
        assert_eq!(ffi.size_nx, 640);
        assert_eq!(ffi.size_ny, 480);
        assert_eq!(ffi.rotation, 90.0);
        assert_eq!(ffi.rotation_mirror, 1); // bool true → c_int 1
        assert_eq!(ffi.quality_type, SipiQualityType::Color);
        assert_eq!(ffi.format_type, SipiFormatType::Png);

        // The false path of both bool flags → c_int 0.
        let mut d = domain;
        d.size_upscaling = false;
        d.rotation_mirror = false;
        let ffi = super::SipiIiifParams::from(d);
        assert_eq!(ffi.size_upscaling, 0);
        assert_eq!(ffi.rotation_mirror, 0);
    }
}

#[cfg(test)]
mod serve_request_layout {
    use super::SipiServeRequest;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_sipi_ffi_h() {
        assert_eq!(size_of::<usize>(), 8, "layout assumes an LP64 target");
        assert_eq!(align_of::<SipiServeRequest>(), 8);
        assert_eq!(size_of::<SipiServeRequest>(), 176);
        assert_eq!(offset_of!(SipiServeRequest, resolved_path), 0);
        assert_eq!(offset_of!(SipiServeRequest, prefix), 8);
        assert_eq!(offset_of!(SipiServeRequest, identifier), 16);
        // `params` (SipiIiifParams, size 72) is guarded field-by-field by
        // `iiif_params_layout`; here we pin only where it sits in the request.
        assert_eq!(offset_of!(SipiServeRequest, params), 24);
        assert_eq!(offset_of!(SipiServeRequest, restricted_width), 96);
        assert_eq!(offset_of!(SipiServeRequest, restricted_height), 100);
        assert_eq!(offset_of!(SipiServeRequest, watermark_path), 104);
        assert_eq!(offset_of!(SipiServeRequest, forwarded_proto), 112);
        assert_eq!(offset_of!(SipiServeRequest, forwarded_host), 120);
        assert_eq!(offset_of!(SipiServeRequest, request_uri), 128);
        assert_eq!(offset_of!(SipiServeRequest, is_head), 136);
        assert_eq!(offset_of!(SipiServeRequest, report_error), 144);
        assert_eq!(offset_of!(SipiServeRequest, report_ctx), 152);
        assert_eq!(offset_of!(SipiServeRequest, before_decode), 160);
        assert_eq!(offset_of!(SipiServeRequest, before_decode_ctx), 168);
    }
}

#[cfg(test)]
mod image_dims_layout {
    use super::SipiImageDims;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_sipi_ffi_h() {
        // All fields are u32, so this struct is 4-aligned (unlike the others).
        assert_eq!(align_of::<SipiImageDims>(), 4);
        assert_eq!(size_of::<SipiImageDims>(), 20);
        assert_eq!(offset_of!(SipiImageDims, width), 0);
        assert_eq!(offset_of!(SipiImageDims, height), 4);
        assert_eq!(offset_of!(SipiImageDims, numpages), 8);
        assert_eq!(offset_of!(SipiImageDims, tile_width), 12);
        assert_eq!(offset_of!(SipiImageDims, tile_height), 16);
    }
}
