---
dune_map: true
schema_version: 1
last_verified_commit: none   # not SHA-tracked: rebase-merge rewrites branch SHAs, so a pre-merge SHA is unknowable. Use `date` for freshness.
date: 2026-09-12
---

# ARCH-MAP.md — Vitrinli

Pull-on-demand architecture map. Not auto-loaded; open it for blast-radius and
boundary questions. Vocabulary lives in [`UBIQUITOUS_LANGUAGE.md`](UBIQUITOUS_LANGUAGE.md);
the bounded-context framing in [`CONTEXT.md`](CONTEXT.md); the module-scope table
this map lifts in [`CONVENTIONS.md`](CONVENTIONS.md).

## Overview

Vitrinli is a IIIF Image API 3.0 media server. Production is a **Rust axum shell**
(`server` + `cli`) that drives a **C++ image engine** (`libsipi`) across a
hand-mirrored `extern "C"` FFI seam (`src/ffi`); the former C++ HTTP server was
removed (ADR-0020), so the C++ `sipi` binary now provides only offline verbs
(convert/verify/query/compare/health). The engine is a set of Bazel `cc_library`
packages carved by concern — the image hub (`SipiImage` + cache + memory budget),
the image-processing free functions (`image_processing`), the codec handlers
(`format_handlers`), the IIIF parsers (`iiifparser`), metadata, and
the util support leaf extracted from the deleted shttps transport. Lua scripting
is Rust-hosted (`src/scripting/rust`, ADR-0023) and drives the engine through
the seam's `sipi_image_*` handle family. The
top-level dependency arrow is one-way: **shell → seam → engine → leaves**; the
engine never links the shell or the seam. New work is added by dropping a file in
a package or a route in the axum router, not by editing a central switch — except
image formats, whose registry fan-out is documented, not mechanized.

## Components

### image

- **Paths:** `:(glob)src/image/**`, `:(glob)src/BUILD.bazel`, `:(glob)src/nsswitch.conf`
- **Purpose:** The image engine hub — `SipiImage` is the pixel-buffer + metadata value type that orchestrates decode → encode through the format-handler dispatch, and owns the `//src` package's Bazel wiring. Processing operators (crop/scale/rotate/ICC conversion/watermarking/comparison/arithmetic) are not methods on the class — they are free functions in `image_processing`.
- **Key entities:** `Sipi::SipiImage`, `SipiImage::io` (static handler registry, *defined* in `format_handlers`), `SipiImage::read`/`readSource`/`write`/`getDim` (decode/encode orchestration), the pixel/metadata mutator surface (`setPixel`/`set_pixels`/`setEs`/`setPhoto`/`set_icc`/`setOrientation`/`essential_metadata`), `Sipi::SipiIO` (abstract), `SipiImgInfo`, `Sipi::SipiImageError`
- **Public interface:** `SipiImage` (via `//src/image`), `SipiIO`; consumed by `format_handlers`, `image_processing`, `ffi` (including the `sipi_image_*` handles behind the Lua `SipiImage` bindings), and `cli`.
- **Local-context kit:** `src/image/cpp/SipiImage.h`, `src/image/cpp/SipiImage.cpp`, `src/image/cpp/SipiIO.h`, `src/image/BUILD.bazel`, `src/BUILD.bazel` (the `:sipi_lib` target), `src/format_handlers/cpp/format_registry.cpp` (where `io` is defined), `docs/adr/0007-sipiimage-decomposition.md`, `CONVENTIONS.md`
- **Depends on:** error, metadata, iiifparser, format_handlers (`:output_sink` only), util, logging, observability, throttling
- **Used by:** format_handlers (one-way back-edge, see rule), image_processing (one-way, see the `image_processing` entry's boundary rules), ffi, cli (transitively)
- **Boundary rules:**
  - The engine references but does **not** define `SipiImage::io` / `Sipi::read_watermark`; both are defined in `format_handlers`, so `//src/image` does not depend on `//src/format_handlers:format_handlers`. This inverts the `SipiImage`↔handler cycle. *Enforcement: `structure`* (a Bazel package cannot depend on itself; the cycle is unrepresentable).
  - The four codec handlers mutate `SipiImage` only through its public mutator surface (`pixels_writable()` plus the metadata/geometry setters) — no `friend` declarations remain; the sole surviving friend is `operator<<`. *Enforcement: `structure`* (the mutator surface is a normal public API, not a language reach-in).
  - `image_processing` depends one-way on `//src/image`; `//src/image` never depends back — a facade method forwarding to a free function in `image_processing` would reintroduce the cycle the extraction removes. *Enforcement: `structure`* (`bazel query 'somepath(//src/image, //src/image_processing)'` is empty; see the `image_processing` entry for the CI-only caveat on running this locally).
- **Durable state:** `SipiImage::io` (static registry, single writer = `format_registry.cpp`).

### image_processing

- **Paths:** `:(glob)src/image_processing/**`
- **Purpose:** Free-function pixel operators over `SipiImage &`, extracted off the `SipiImage` god-object per ADR-0007: crop, scale (fast/medium), rotate, colour conversion (YCC→RGB, ICC), channel removal, bit-depth reduction (8bps/bitonal), watermarking, comparison, plus `processing::subtract` and the surviving free equality operator over `SipiImage`, and the separable resampler the geometry operators call.
- **Key entities:** `Sipi::processing::crop`/`scaleFast`/`scaleMedium`/`scale`/`scale_with` (the one method→operator dispatch; codecs and the restricted crop pick the method with `image`'s `scaling_method_for`)/`rotate`/`set_topleft`/`convertYCC2RGB`/`convertToIcc`/`removeChannel`/`removeExtraSamples`/`to8bps`/`toBitonal`/`add_watermark`/`compare`/`subtract`/`bilinn`, `Sipi::read_watermark` (declared here, *called* from `add_watermark`, *defined* in `format_handlers`), `Sipi::operator==` (free equality over `SipiImage`, namespace `Sipi`), `Sipi::resample_separable_u8`/`resample_separable_u16`
- **Public interface:** the free functions (via `//src/image_processing`; default visibility is `//src:__subpackages__`, with the `:image_processing` library additionally granted `//test/approval:__pkg__` because `processing.h` is included directly from `test/approval/`, which sits outside `//src/...`). Visibility narrows who may depend on this package from outside a grant; it cannot express the one-way rule against `//src/image` in the boundary rules below (both packages sit inside `//src:__subpackages__`) — that constraint stays enforced by dep-set review, not by visibility.
- **Local-context kit:** `src/image_processing/BUILD.bazel`, `src/image_processing/cpp/processing.h`, `src/image_processing/cpp/geometry.cpp`, `src/image_processing/cpp/color.cpp`, `src/image_processing/cpp/compose.cpp`, `docs/adr/0007-sipiimage-decomposition.md`
- **Depends on:** image (one-way), logging, metadata, observability, util; `@highway` (SIMD), `@lcms2`
- **Used by:** format_handlers' test target (real codec decode fixtures the operators run against — the `image_processing` *library* itself is not a `format_handlers` dependency), `test/approval` (`processing.h` included directly)
- **Boundary rules:**
  - `//src/image_processing` depends one-way on `//src/image`; `//src/image` never depends back. *Enforcement: `structure`* — `bazel query 'somepath(//src/image, //src/image_processing)'` is empty.
  - `deps(//src/image_processing/...)` resolves only to `{image, error, util, logging, observability, metadata}` (plus the vendored `@highway`/`@lcms2` externals) — no `format_handlers`, no `ffi`, no Kakadu/libtiff. *Enforcement: `structure`* — `bazel query 'deps(//src/image_processing/...)'`. Both queries above are CI-verifiable, not locally runnable in every environment: a `bazel query` whose universe reaches `@google_benchmark` needs to fetch `@libpfm`, which fails without DNS — treat a local failure-to-fetch as inconclusive, not as a passing or failing check.
  - `Sipi::read_watermark` is *declared* here and *called* from `add_watermark`, but *defined* in `format_handlers` (`SipiIOTiff.cpp`) — resolved at link time, the same declare-here/define-there asymmetry `//src/image` documents for `SipiImage::io`. This package must never gain a dependency on `format_handlers` (the reverse edge — since `format_handlers` already depends on `image_processing` for the pixel operators — would cycle). *Enforcement: `docs-only`* (a Bazel dep would fail analysis, but the link-time resolution itself isn't mechanically checked).
- **Durable state:** none.

### error

- **Paths:** `:(glob)src/error/**`
- **Purpose:** The shared `SipiError` exception base and `SipiValueError` (ADR-0024), the image/codec layer's value-returning failure type. Both are a standalone leaf so `metadata`, `format_handlers`, and `iiifparser` can throw/catch `SipiError` without depending on the image engine — folding it into `image` would close an `image -> metadata -> image` cycle Bazel cannot express. `SipiValueError` lives here for the same reason: every consumer of the image/codec layer already depends on this package without a cycle.
- **Key entities:** `Sipi::SipiError`, `Sipi::SipiValueError`, `Sipi::ErrorCode`, `Sipi::Result<T>`
- **Public interface:** `SipiError`, `SipiValueError` (via `//src/error`).
- **Local-context kit:** `src/error/BUILD.bazel`, `src/error/cpp/SipiError.h`, `src/error/cpp/SipiError.cpp`, `src/error/cpp/SipiValueError.h`, `src/error/cpp/sipi_value_error_test.cpp`
- **Depends on:** util
- **Used by:** image, metadata, format_handlers, iiifparser, cache
- **Boundary rules:** a leaf — no internal deps beyond `util`; `deps(//src/error/...)` matches this minimal set — nothing pulls `image` back in through `error`. *Enforcement: `structure`* — `bazel query 'deps(//src/error/...)'` (CI-verifiable; see the `image_processing` entry's caveat on running `bazel query` locally in this sandbox).
- **Durable state:** none.

### cache

- **Paths:** `:(glob)src/cache/**`
- **Purpose:** File-based LRU of generated representations, keyed by a SHA-256 digest of the *Cache key*, with dual-limit eviction (total size **and** file count) and crash recovery. The on-disk index is fronted by a versioned header; a magic/version/record-size mismatch means "start empty" (ADR-0025).
- **Key entities:** `Sipi::SipiCache`, `SipiCache::check`/`add`/`purge`/`deblock`, `FileCacheRecord` (on-disk fixed-width; `canonical` is a SHA-256 hex digest, `source_size` is the freshness gate), `CacheRecord`, `CacheIndexHeader` (magic / `kCacheIndexVersion` / record_size), `sha256_hex`, `subtractClamped` (underflow-guarded byte accounting)
- **Public interface:** `SipiCache` (via `//src/cache`); the FFI runtime owns one instance.
- **Local-context kit:** `src/cache/BUILD.bazel`, `src/cache/cpp/SipiCache.h`, `src/cache/cpp/SipiCache.cpp`, `src/ffi/cpp/init.cpp` (constructs it), `src/ffi/cpp/serve_image.cpp` (check/add + the `O_NOFOLLOW` cache-read), `docs/adr/0025-cache-index-header.md`, `UBIQUITOUS_LANGUAGE.md` (Cache / Cache key / Cache pin)
- **Depends on:** error, logging, observability; openssl (SHA-256 keying)
- **Used by:** ffi (`init.cpp` owns it, `serve_image.cpp` uses it)
- **Boundary rules:**
  - Cache state is exposed **exclusively** through *Metrics*, never Lua bindings; cache-hit responses bypass both Throttling policies (ADR-0008). *Enforcement: `docs-only`* (glossary rule; no mechanical check).
  - The on-disk index is fronted by a fixed `CacheIndexHeader` (magic + version + `sizeof(FileCacheRecord)`); any mismatch → start empty and log once, so a layout change to `FileCacheRecord` MUST bump `kCacheIndexVersion` (ADR-0025). *Enforcement: `structure`* (the load-time header check refuses a mismatched index) + `docs-only` (the version-bump obligation).
  - Cache-hit file reads open with `O_NOFOLLOW` and reject non-regular files (`serve_image.cpp` `full_file_body`), so a symlink planted in the cache dir plus a forged record cannot read arbitrary files (ADR-0025). *Enforcement: `structure`* (the open flag + `S_ISREG` check).
- **Durable state:** in-memory `cachetable` (keyed by SHA-256 hex) + `blocked_files` + `cache_used_bytes`/`nfiles` (mutex-guarded; byte accounting underflow-clamped via `subtractClamped`); on-disk `.sipicache` index behind the versioned header (rewritten on destruction; version/magic/record-size mismatch → start empty + log once). Single owner = the FFI runtime (`std::unique_ptr`, constructed in `init.cpp`).

### throttling

Colocated polyglot (ADR-0021/0022): a shell-side admission pool and an
engine-side memory budget under one component. Supersedes the former
`memory-budget` entry.

- **Paths:** `:(glob)src/throttling/rust/**` (admission, shell-side), `:(glob)src/throttling/cpp/**` (memory_budget, engine-side).
- **Purpose:** Vitrinli's load-driven request-rejection (Throttling). Three sub-policies: **Admission** (the two-partition thread pool — tile floor + full hard cap, pre-dispatch), the **Decode memory budget** (the full partition's decode-RAM cap, post-cache, 503/413 under advanced mode), and the **Decode watchdog** (a per-decode wall-clock deadline, `EngineContext::decode_timeout_ms` default 120000ms; on timeout the wedged worker is detached and counted in the `wedged_threads` gauge — the DEV-7080 unpatchable-Kakadu-hang availability guard, enacted at the `decode_guard.cpp` acquire site via `run_with_deadline`/`run_guarded_decode`).
- **Key entities:** Rust `admission::{Admission, AdmissionKind, AdmissionMode, AdmissionConfig, AdmissionSnapshot, Permit, default_pool_size}` (`//src/throttling/rust:admission`); C++ `Sipi::SipiMemoryBudget` + `MemoryBudgetGuard` + `enum class AdmissionMode { BASIC, ADVANCED }` + `Sipi::estimate_peak_memory` (`//src/throttling/cpp:memory_budget`).
- **Public interface:** `Admission` (owned by `server` `AppState`; `classify`/`acquire`/`snapshot`); `SipiMemoryBudget` + `MemoryBudgetGuard` (via `//src/throttling/cpp:memory_budget`).
- **Local-context kit:** `src/throttling/rust/lib.rs`, `src/throttling/cpp/SipiMemoryBudget.{h,cpp}`, `src/throttling/cpp/SipiPeakMemory.h`, `src/ffi/cpp/decode_guard.{h,cpp}` (the single acquire site: `acquire_full_lane_budget` + `run_guarded_decode`, shared by both decode entry points), `src/ffi/cpp/serve_image.cpp` and `src/ffi/cpp/image_handle.cpp` (the two callers), `src/ffi/cpp/init.cpp` (resolves the config), `src/server/rust/src/routes.rs` (`AppState`, classify/acquire call sites), `UBIQUITOUS_LANGUAGE.md` (Throttling), `docs/adr/0022-two-lane-admission-control.md`.
- **Depends on:** admission (Rust) → `tokio` + `//src/iiifparser/rust:iiif_parser` only (FFI-free; no `//src/ffi`, no C++ engine — DUNE-002). memory_budget (C++) → nothing internal (decoupled from observability by design; the acquire site re-publishes gauges).
- **Used by:** `server` (`AppState` owns the pool; `routes.rs` classify/acquire; `metrics.rs` reads the snapshot); ffi (`init.cpp` constructs the budget; `decode_guard.cpp` acquires for both `serve_image.cpp` and `image_handle.cpp`'s `sipi_image_new`).
- **Boundary rules:**
  - The admission crate is engine-free: `bazel query 'deps(//src/throttling/rust:admission)'` shows no `//src/ffi`, no engine, no Kakadu. *Enforcement: `structure`* (Bazel dep graph).
  - The memory-budget module writes no metrics itself; the `decode_guard.cpp` acquire site takes an optional `on_release` callback so callers re-publish gauges without coupling the shared helper to observability (DUNE-005). *Enforcement: `docs-only`*.
  - The shell Semaphore pool (pre-dispatch) and the engine memory budget (post-cache) are two distinct admission layers. *Enforcement: `docs-only`*.
  - `admission_mode`/`tiles_memory_ratio`/`large_decode_threshold_bytes`/`memory_limit` are resolved engine-side and read back over the seam (single authority, DUNE-003). *Enforcement: `docs-only`*.
- **Durable state:** admission — `Arc<Semaphore>` global + full sub-pool, per-partition `AtomicUsize`/`AtomicU64` wait/shed counters (single writer). memory_budget — `std::atomic<size_t> _used` (CAS-updated). No persistence.

### format_handlers

- **Paths:** `:(glob)src/format_handlers/**`
- **Purpose:** The four `SipiIO` codec handlers (TIFF, JPEG2000/Kakadu, JPEG, PNG), the codec-agnostic `output_sink`, the `SipiImage::io` registry definition, and the codec fuzz harnesses under `fuzz/` (per-format `*_decode_fuzz` + `*_roundtrip_fuzz` `cc_fuzz_test` targets, corpus-replayed on every platform).
- **Key entities:** `SipiIOTiff`/`SipiIOJ2k`/`SipiIOJpeg`/`SipiIOPng`, `OutputSink`/`SinkStream`, `read_watermark` (declared/called in `image_processing`, *defined* here), `SipiImage::io` (defined in `format_registry.cpp`)
- **Public interface:** the `SipiIO` overrides (reached only through `SipiImage`'s dispatch); `output_sink` is a separate leaf target `//src/format_handlers:output_sink`.
- **Local-context kit:** `src/format_handlers/BUILD.bazel`, `src/format_handlers/cpp/format_registry.cpp`, `src/format_handlers/cpp/SipiIOTiff.{h,cpp}`, `src/format_handlers/cpp/output_sink.h`, `src/image/cpp/SipiImage.h` (`io` decl + the public mutator surface), `tools/format-handlers-fanout.sh`, `src/image_processing/cpp/processing.h` (the `read_watermark` declaration) + `src/image_processing/cpp/compose.cpp` (its sole caller — the declare-there/define-here reciprocal for a `read_watermark` signature change) (the new-format edit-site list)
- **Depends on:** error, image (`//src/image`, one-way), image_processing (the `read_watermark` declaration + pixel-operator fixtures its tests drive), metadata, observability, logging, util, output_sink; codecs `@kakadu` `@tiff` `@libpng` `@libjpeg_turbo`
- **Used by:** image (link-time, for `io`/`read_watermark`), ffi, cli, tests
- **Boundary rules:**
  - `//src/format_handlers:format_handlers` depends one-way on `//src/image`; `output_sink` is a dependency-free leaf so the engine can reach it without depending on the handler package. *Enforcement: `structure`* (Bazel target-granularity dep direction).
  - Byte-exact cross-arch output invariant — the encode paths are pinned byte-for-byte by `//test/approval:approvaltests` across all platforms; math feeding the encoder must be architecture-independent (fixed-point over float). *Enforcement: `static-analysis`* (CI approval gate; file-head banners in SipiIOTiff/J2k/Png.cpp).
  - The JPEG writer owns the one-segment fit policy: a metadata block that `Exif::exifBytes(limit)` or `Xmp::split_extended` cannot fit is left out with a `log_warn` naming block, size and reason, and the write succeeds; those functions only report `kWriteFailed`. *Enforcement: `static-analysis`* (`formats_test` `JpegMetadataSegments.*LeftOutWithAWarning`).
  - Adding a format touches a shared registry + dispatch fan-out (~4 sites across 3 files, listed by `tools/format-handlers-fanout.sh`). *Enforcement: `docs-only`* (DUNE-005 deferred a descriptor table until a real 5th format; ADR-0006 is the prior art to check first).
- **Durable state:** `SipiImage::io` (single writer here); no runtime state.

### iiifparser

- **Paths:** `:(glob)src/iiifparser/**`
- **Purpose:** The IIIF URL parser, colocated polyglot (component-first, then language; ADR-0021). `cpp/value_objects/` is the live engine value objects (region/size/rotation/quality/format/identifier + `compute_decode_dims`); `cpp/classifier/` is the `testonly` `parse_iiif_uri` reference oracle; `rust/` is the production Rust parser (`//src/iiifparser/rust:iiif_parser`) the shell drives, emitting domain types. `corpus/` is the language-neutral regression corpus both languages sweep. `fuzz/` is the libFuzzer harness over `parse_request` (`rules_fuzzing` `cc_fuzz_test` + an `extern "C"` shim), seeded from `corpus/`.
- **Key entities:** C++: `SipiRegion`/`SipiSize`/`SipiRotation`/`SipiQualityFormat`/`SipiIdentifier` (each with a string-parse ctor and a flattened-FFI-seam ctor + `canonical()`), `SipiDecodeDims`/`compute_decode_dims`, `handlers::iiif_handler::parse_iiif_uri` (testonly reference). Rust: `parse_request`, `ParsedRequest`/`RequestKind`, `IiifParams`, `RegionKind`/`SizeKind`/`QualityKind`/`FormatKind` (domain enums, total supersets of the FFI enums), `clamp_dims_to_size`/`size_never_reduces` (the size math `server`'s `routes::restriction` decides (RW, RH) with)
- **Public interface:** the value-object classes (via `//src/iiifparser/cpp/value_objects:iiifparser`); `parse_iiif_uri` via the testonly `//src/iiifparser/cpp/classifier:iiif_handler`; the Rust parser via `//src/iiifparser/rust:iiif_parser` (`parse_request` → domain `IiifParams`, `server` owns the `From` flattening).
- **Local-context kit:** `src/iiifparser/cpp/value_objects/BUILD.bazel`, `src/iiifparser/cpp/value_objects/SipiSize.h`, `src/iiifparser/cpp/classifier/iiif_handler.h`, `src/iiifparser/rust/BUILD.bazel`, `src/iiifparser/rust/parse.rs`, `src/ffi/cpp/serve_image.cpp` (the C++ FFI-seam reconstruction), `src/server/rust/src/ffi.rs` (the domain → seam `From<IiifParams>` mapping a domain-enum change must be kept exhaustive against). ADR-0021 is one hop away via every subpackage's BUILD docstring.
- **Depends on:** error (`SipiError`), util. The Rust crate deps only `@crates//:percent-encoding` — no FFI, no C++ engine.
- **Used by:** image (`SipiIO.h`), ffi (`serve_image.cpp` rebuilds the value objects), format_handlers (`SipiIOJ2k`), cli; server (drives the Rust parser)
- **Boundary rules:**
  - `cpp/value_objects` is a leaf — deps only `//src/error` + `//src/util`, never `SipiImage`/codecs. *Enforcement: `structure`* (Bazel visibility + dep set). Each subpackage pins the virtual `iiifparser/` include prefix to its own physical depth (`strip_include_prefix` + `include_prefix = "iiifparser"`) so consumers keep `#include "iiifparser/*.h"`. *Enforcement: `structure`* (a wrong prefix fails the engine compile).
  - `cpp/classifier` `iiif_handler` is the `testonly` reference oracle for the Rust `parse_request`, deps `//src/util` only, and is `rm -rf`-deletable as a whole folder once the Rust port is trusted (only the `//test/approval` edge + corpus consumer need unwiring). *Enforcement: `structure`* (`testonly` keeps it out of `//src:sipi_lib`; DUNE-015).
  - `rust/iiif_parser` is FFI-free: `bazel query 'deps(...)'` shows no `//src/ffi:sipi_ffi` and no C++ engine, so it needs no `_CPP_STDLIB_LINK` and is sanitizer-eligible (untagged). *Enforcement: `structure`* (Bazel dep set). The domain→FFI `From` impls in `server/rust/ffi.rs` are exhaustive matches, never `as` casts. *Enforcement: `static-analysis`* (a new variant fails to compile; per-variant mapping test).
  - `fuzz/` holds the repo's only **C++→Rust** link (`shim.rs` → `fuzz_target.cc`); every other FFI edge is Rust→C++. It exists because libFuzzer's entry point is C and `rules_fuzzing` is a C++/Java rule set. Nothing outside the package can depend on the shim, so the one-way shell→seam→engine direction still holds for everything that ships. *Enforcement: `structure`* (both targets are `testonly` and the package grants no default visibility, so a dep from outside fails analysis). `--config=fuzz` is Linux-only; the default `replay` engine keeps the target building and corpus-replaying on every platform. *Enforcement: `structure`* (the `//src/...` test sweeps pick up the replay test).
- **Durable state:** `SipiSize::limitdim` (static compile-time constant, no writer).

### metadata

- **Paths:** `:(glob)src/metadata/**`
- **Purpose:** EXIF / IPTC / XMP / ICC wrappers over exiv2 + lcms2, and the Essentials preservation packet. Sits **below** the image engine (no `SipiImage` dep). The canonical model package (docstring README, Test-seam visibility, colocated tests).
- **Key entities:** `Exif`/`Iptc`/`Xmp`/`Icc`/`Essentials`, `Icc::iccBytes()` (the single ICC-materialization chokepoint, ADR-0002), `EssentialsFields`, `PhotometricInterpretation`, `Exif::set_pixel_dimensions`/`erase_previews` + `Xmp::set_pixel_dimensions`/`erase_thumbnails` + `Exif::restrict_image_resources` (called only by `ffi`'s `rewrite_restricted_metadata`: a size-restricted output states its delivered size in every size field and drops every embedded preview (EXIF IFD1/SubIFD/maker-note previews, XMP `xmp:Thumbnails` and `xmpGImg:image`, IPTC `Preview`, via `Iptc::erase_preview`; the same inside the EXIF's Photoshop image resources), every other field is kept, and a size field that cannot take the new value, an XMP preview that cannot be removed or image resources that cannot be rewritten fail the request; unrestricted outputs keep the source's tags), `Exif::exifBytes(limit)` + `Xmp::split_extended` (the JPEG writer's one-segment fit: EXIF without its previews, XMP split into ExtendedXMP; otherwise `kWriteFailed`, and the JPEG writer decides; see format_handlers).
- **Public interface:** the wrapper classes (via `//src/metadata:metadata`); the byte-mutation helper is in `//src/metadata/cpp/internal` (restricted).
- **Local-context kit:** `src/metadata/BUILD.bazel`, `src/metadata/cpp/icc.h`, `src/metadata/cpp/icc.cpp`, `src/metadata/cpp/internal/BUILD.bazel`, `src/metadata/cpp/internal/icc_normalization.{h,cpp}`, `src/metadata/cpp/internal/icc_lcms2.h`, `src/metadata/cpp/essentials.h`, `docs/adr/0002-icc-profile-determinism-test-only.md`
- **Depends on:** error (`SipiError`), util, logging; exiv2, lcms2, openssl, protobuf (internal codec), libexpat (`Xmp::split_extended`)
- **Used by:** image (owns the wrappers as members), format_handlers (read/write ICC + Essentials), ffi, cli
- **Boundary rules:**
  - `//src/metadata/cpp/internal` (`icc_normalization`, `protobuf_codec`) is visibility-restricted to `//src/metadata:__pkg__` — the canonical **Test seam** pattern (DEV-6406); `:icc_lcms2` is the one target in that package carrying an additional per-target grant to `//src/image_processing:__pkg__` (for `color.cpp`'s `cmsCreateTransform` handles) — every other target in the package stays parent-only. *Enforcement: `structure`* (Bazel visibility fails analysis on an external include).
  - `Icc::iccBytes()` is the single chokepoint every codec-bound ICC profile funnels through; new format handlers must route through it (bypassing it breaks the approval gate). *Enforcement: `static-analysis`* (approval determinism gate) + `docs-only` (the banner).
  - No `SipiImage` dependency — the one former coupling was inverted via `photometric_interpretation.h`. *Enforcement: `structure`* (dep set).
- **Durable state:** none in-process; the durable artifact is the Essentials packet embedded in image headers (written by format encoders via `Essentials::serialize`).

### scripting

- **Paths:** `:(glob)src/scripting/**`
- **Purpose:** The Rust-hosted mlua Lua runtime (ADR-0023, in `rust/` per the colocated-polyglot layout ADR-0021): hardened per-request VM profile (stdlib whitelist, `os` shim, restricted `require`, memory cap, deadline hook + binding chokepoint), the bytecode cache, the `LuaEnv` entry points (preflight / file preflight / routes / elua), the full binding surface (`server.*`, `server.fs`, `helper`, `SipiImage`, sqlite `server.db`), and the Lua-flavor config-file parse.
- **Key entities:** `LuaEnv` (preflight/route/config entry), `ScriptRuntime`/`RequestVm` (VM factory + binding chokepoint), `BytecodeCache`, `LimitConfig`/`Deadline`/`KillStats`, `RequestData`/`ResponseWriter`, `LuaImage`, `valid_permission` (the runtime-side *Permission* gate, exported for `server`'s lockstep test), `parse_config_file`
- **Public interface:** `//src/scripting/rust:scripting` (crate `scripting`), re-exports in `lib.rs`.
- **Local-context kit:** `src/scripting/rust/BUILD.bazel`, `src/scripting/rust/entry.rs`, `src/scripting/rust/runtime.rs`, `src/scripting/rust/limits.rs`, `src/scripting/rust/bindings/mod.rs`, `src/scripting/rust/engine_ffi.rs` (the Rust mirror of the `sipi_image_*` handle seam the Lua bindings call — disjoint from `server`'s `ffi.rs`; see the `ffi` entry), `docs/adr/0023-rust-hosted-mlua-lua-runtime.md`
- **Depends on:** mlua (`external` link mode against `@lua`), ffi (the `sipi_image_*` handle family + edge probes), sqlite3 (hand-written FFI over `@sqlite3`), jsonwebtoken, reqwest, libc, tracing
- **Used by:** server (the Rust shell builds one `LuaEnv` and drives every Lua entry through it)
- **Boundary rules:**
  - Rust side: every script-visible binding registers through the `RequestVm::register_binding` chokepoint (deadline check first); `verify_bindings_checked` enumerates binding tables against the registration record. *Enforcement: `static-analysis`* (the enumeration test).
- **Durable state:** none intrinsic (a runtime/VM; fresh VM per request). The bytecode cache is in-memory, keyed by path, invalidated by mtime+size. The `server.db` sqlite binding opens caller-controlled sqlite files at script direction.

### util

- **Paths:** `:(glob)src/util/**`
- **Purpose:** Generic Vitrinli-domain helpers (MIME/string parsing, file hashing, filename-to-subdirectory hashing, error/global types, URL decode) extracted from shttps; a leaf. Namespace stays `shttps::`.
- **Key entities:** `shttps::Error`, `shttps::Hash` + `enum HashType` (on-disk contract, ADR-0005), `shttps::Parsing::{getFileMimetype,getBestFileMimetype,parseMimetype}`, `shttps::urldecode`, `Global::as_integer`, `SipiFilenameHash`, `SipiFilenameHash::setLevels`, `SipiFilenameHash::migrateToLevels`
- **Public interface:** the free functions + value types (via `//src/util`, `include_prefix="util"` + `strip_include_prefix="/src/util/cpp"` → `#include "util/…"`).
- **Local-context kit:** `src/util/BUILD.bazel`, `src/util/cpp/Parsing.h`, `src/util/cpp/Hash.h`, `src/util/cpp/Error.h`, `src/util/cpp/UrlDecode.h`, `src/util/cpp/SipiFilenameHash.h`
- **Depends on:** openssl (Hash), libmagic (Parsing MIME sniff), logging (`SipiFilenameHash`'s `log_debug` calls) — the one Vitrinli-internal dep.
- **Used by:** image, format_handlers, iiifparser, metadata, cli, ffi (broadly used leaf)
- **Boundary rules:** a leaf — deps only `logging` internally; colocated `util_test` links only `:util`, so a forbidden cross-module include is a build error, not a `sipi_lib`-wide slip. *Enforcement: `structure`*.
- **Durable state:** `Hash::HashType` values are an on-disk contract (mirrored in `essentials.proto`); `Parsing` ships a compiled-in `magic.mgc` blob (read-only); `SipiFilenameHash::__levels` (static, set via `setLevels`/`migrateToLevels`).

### observability

- **Paths:** `:(glob)src/observability/**`
- **Purpose:** In-process metrics (plain atomic counters/gauges — prometheus-cpp removed in Phase 7) and a Tracy profiling shim. Metrics reach production OTLP **only** via the scalar `SipiMetricsSnapshot` FFI struct.
- **Key entities:** `Sipi::observability::Metrics` (Meyers singleton), `Counter`/`Gauge`, `read_shape_fast_path_counter`, `essentials_hash_mismatch_counter`, `SIPI_ZONE()` macros
- **Public interface:** `Metrics::instance()` (via `//src/observability:observability`).
- **Local-context kit:** `src/observability/cpp/metrics.h`, `src/observability/cpp/metrics.cpp`, `src/observability/cpp/metrics_registry_test.cpp` (the seam tripwire), `src/ffi/cpp/metrics_snapshot.h` (the Inclusion rule), `src/server/rust/src/metrics.rs` (the OTLP bridge)
- **Depends on:** tracy (inert unless `--config=tracy`)
- **Used by:** image, cache (writers), ffi (reader/bridge), format_handlers
- **Boundary rules:** the singleton is engine-internal; a scalar counter reaches production only if it is also read into `SipiMetricsSnapshot` and mapped in `metrics.rs`. `metrics_registry_test` pins the full field inventory (22 bridged / 13 engine-internal). *Enforcement: `static-analysis`* (the seam-tripwire test + the `SipiMetricsSnapshot` 176-byte layout lock) + `docs-only` (the banner).
- **Durable state:** the `Metrics` singleton atomics (process-lifetime); writers across image/cache/format_handlers/ffi, reader = `sipi_metrics_snapshot`.

### logging

- **Paths:** `:(glob)src/logging/**`
- **Purpose:** A generic, non-`Sipi::` logging primitive (free functions + set-once flags + per-request thread-local trace context) any module may depend on. Pure stdlib.
- **Key entities:** `log_debug`/`log_info`/`log_warn`/`log_err`, `enum LogLevel`, `set_log_trace_context`/`set_outbound_traceparent`, `set_json_mode`
- **Public interface:** `logger.h` (via `//src/logging:logging`, `include_prefix="logging"`).
- **Local-context kit:** `src/logging/BUILD.bazel`, `src/logging/cpp/logger.h`, `src/logging/cpp/logger.cpp`
- **Depends on:** (stdlib only)
- **Used by:** nearly every C++ component
- **Boundary rules:** the one module whose per-target `layering_check` passes today (no vendored includes). *Enforcement: `static-analysis`* (layering_check enabled on this target only; deferred elsewhere — DEV-6353).
- **Durable state:** module-level statics (`g_log_level`, `g_json_mode`) + thread-locals (`g_trace_id`/`g_span_id`/`g_outbound_traceparent`); writers are the `set_*` functions.

### ffi

- **Paths:** `:(glob)src/ffi/**`
- **Purpose:** The hand-mirrored `extern "C"` seam the Rust shell drives the C++ engine through — serve entries, the engine-context install (`sipi_init`), the metrics snapshot, edge probes, the `sipi_image_*` opaque image handle family (the Rust Lua `SipiImage` bindings' callee), and the shared `LibraryInitialiser`. Config parsing is NOT here: the shell parses both config flavors (TOML via `config_file.rs`, Lua via `//src/scripting/rust`) and `sipi_init` consumes only the resolved `SipiServerConfig` override channel.
- **Key entities:** `sipi_serve_image`/`sipi_serve_file`/`sipi_init`/`sipi_metrics_snapshot`/`sipi_cli_main` (defined in `cli`), the `sipi_image_*` handle family, `SipiResponse` (streamed sink), `SipiIiifParams`/`SipiServeRequest`/`SipiMetricsSnapshot` (`#[repr(C)]` mirrors; `SipiServeRequest.restricted_width`/`restricted_height` carry the shell-decided (RW, RH), both 0 when unrestricted, and the engine refuses a half-set pair, an axis below 2 or dims larger than the image), `SipiPermType` (the eight-value *Permission* enum, hand-mirrored with paired value asserts; appended to, never renumbered), `EngineContext` + `set_engine_context`, `LibraryInitialiser`, `Sipi::SipiConf`, `Sipi::parseSizeString` (the Lua-flavoured config object `sipi_init` constructs and populates)
- **Public interface:** `sipi_ffi.h` (`include_prefix="ffi"` + `strip_include_prefix="/src/ffi/cpp"` → `#include "ffi/sipi_ffi.h"`), mirrored by hand in `src/server/rust/src/ffi.rs`.
- **Local-context kit:** `src/ffi/cpp/sipi_ffi.h`, `src/ffi/cpp/serve_image.cpp`, `src/ffi/cpp/engine_context.{h,cpp}`, `src/ffi/cpp/init.cpp`, `src/ffi/cpp/metrics_snapshot.h`, `src/ffi/cpp/SipiConf.{h,cpp}`, `src/ffi/cpp/image_handle.cpp` (defines the `sipi_image_*` family), `src/ffi/BUILD.bazel`, `src/server/rust/src/ffi.rs` (the primary Rust mirror)
- **Depends on:** image, format_handlers, iiifparser (transitive), metadata, util, observability, logging; curl, exiv2
- **Used by:** server (drives it), cli (links it and shares `startup`/`LibraryInitialiser`), scripting (`//src/scripting/rust` links the `sipi_image_*` handle family through its own `engine_ffi.rs` mirror)
- **Boundary rules:**
  - `//src/image` does **not** depend on this package (the seam drives the engine, never the reverse) — no cycle. *Enforcement: `structure`* (Bazel dep direction).
  - Every `#[repr(C)]` struct/enum crossing the seam is guarded on both sides: C++ `static_assert(sizeof/offsetof)` + Rust `offset_of!`/`size_of` layout tests (DUNE-002). *Enforcement: `structure`* on the C++ side (a drifted struct fails to compile) + `static-analysis` on the Rust side (test-time layout asserts run on the `//src/...` wildcard).
  - No C++ exception may cross the boundary — every `sipi_*` wraps in a catch-all (`sipi_guard`). *Enforcement: `review`*.
  - The C ABI is hand-mirrored on the Rust side in **two disjoint** files, partitioned by consumer: `src/server/rust/src/ffi.rs` (serve entries + config getters) and `src/scripting/rust/engine_ffi.rs` (the `sipi_image_*` handle family the Lua bindings call). No `sipi_*` function is declared in both. As with every hand-mirrored `extern "C"` fn decl here, the function *signatures* carry no mechanical guard against `sipi_ffi.h` (only the `#[repr(C)]` struct *layouts* are guarded, above), so a C++ signature change must be mirrored by hand in whichever file owns that function. *Enforcement: `docs-only`*.
- **Durable state:** `EngineContext` (file-static `g_engine`, single sink `set_engine_context`, sole installer `sipi_init`).

### cli

Colocated polyglot (ADR-0021 pattern): the C++ offline-verb library and the
Rust binary entry point under one component.

- **Paths:** `:(glob)src/cli/cpp/**` (offline verbs, engine-side), `:(glob)src/cli/rust/**` (the `sipi` binary entry point, shell-side), `:(glob)src/cli/BUILD.bazel`.
- **Purpose:** C++: the offline verbs (convert/verify/query/compare/health) behind the `sipi_cli_main` FFI entry, and the thin `main` in `sipi.cpp`. Server mode was deleted with the oracle (Phase 7). Rust: the default `sipi` binary (`//src/cli/rust:sipi`) — owns `main`, the clap `server` verb, Sentry init + the out-of-process minidump reporter (not in ASan builds), and the mimalloc allocator; dispatches offline verbs to the C++ `sipi_cli_main`.
- **Key entities:** C++: `sipi_cli_main` (extern "C"), `Sipi::cli::cmd_convert_access_file`/`cmd_convert_service_file`/`cmd_verify`/`cmd_health`, `LibraryInitialiser::instance()`, `Sipi::emit_json_report`, `Sipi::emit_json_cli_arg_error` (the `--json` report emitter). Rust: `main`, `commands::server::run`, `ServerArgs` (clap flatten groups), `impl From<&ServerArgs> for ServerOverrides` (exhaustive destructure, DUNE-006), `mod allocator` (mimalloc `extern "C"` block), `init_sentry`.
- **Public interface:** `sipi_cli_main` (the sole export `cli/rust` links); `//src/cli:sipi` binary (offline verbs); `//src/cli/rust:sipi` binary (the deployed `sipi` binary); clap `ServerArgs`.
- **Local-context kit:** `src/cli/cpp/cli_app.cpp`, `src/cli/cpp/sipi.cpp`, `src/cli/cpp/SipiReport.{h,cpp}`, `src/cli/BUILD.bazel`, `src/cli/cpp/commands/BUILD.bazel`, `src/cli/cpp/commands/convert_service_file.h`, `src/cli/rust/src/main.rs`, `src/cli/rust/src/commands/server/mod.rs`, `src/cli/rust/src/commands/server/args/mod.rs`, `src/cli/rust/BUILD.bazel`, `docs/adr/0009-file-taxonomy.md`, `docs/adr/0019-mimalloc-production-allocator.md`, `docs/adr/0018-minidump-crash-memory-accepted-risk.md`
- **Depends on:** C++: image (`sipi_lib`), ffi (`sipi_cli_main` contract, shared `startup`), cli11, jansson. Rust: server (`sipi::run`), the C++ `cli_app` (`sipi_cli_main`), mimalloc (vendored static, Linux-non-ASan), sentry(+minidump, non-ASan).
- **Used by:** (top of the binary graph) — `//src/cli/rust:sipi` is the deployed entry point.
- **Boundary rules:**
  - C++: one `.cpp`/`.h` per `sipi <verb> <noun>` in `commands/`; CLI11 stays in `cli_app.cpp` and never leaks into `commands/` (which take plain `*Args` structs). `//src/cli/cpp/commands` visibility scoped to `//src/cli:__pkg__`. *Enforcement: `structure`* (visibility) + `docs-only` (the one-file-per-verb convention).
  - Rust: the config seam fails on omission: `From<&ServerArgs>` destructures every clap group exhaustively (no `..`), so a new `server` flag fails to compile until forwarded or explicitly `field: _`. *Enforcement: `structure`* (exhaustive-match compile error; DUNE-006).
  - The mimalloc stats `extern "C"` block is deliberately colocated in `main.rs` (drops out with the feature; a version drift is a SIGSEGV, not a build error — read via the `mi_stats_shim.c` C shim). *Enforcement: `docs-only`* (SAFETY comment; decision 4).
  - Same oracle-vocabulary avoidance as `server`. *Enforcement: `docs-only`* (the `CONVENTIONS.md` § Production surface rule).
- **Durable state:** `LibraryInitialiser` singleton (process-global init, idempotent); the Rust binary owns the process lifecycle, the Sentry client guard, and the minidump reporter guard (none persistent).

### server

- **Paths:** `:(glob)src/server/rust/**`
- **Purpose:** The production Rust axum HTTP shell — routing, IIIF/info assembly, the streaming response sink, edge path validation, config (Lua or TOML), the Throttling pool, the preflight cache, and OTel telemetry. Shipped as the `sipi` library so a downstream crate can embed it.
- **Key entities:** `run`/`serve`/`app`, `routes::iiif`/`cors_preflight`/`serve_docroot`, `AppState`, `IMAGE_MIMES`, `iiif_parser::parse_request` (the carved parser crate), the `ffi.rs` `From<iiif_parser::IiifParams> for SipiIiifParams` seam mapping, `info::{image_info_json,bitstream_info_json}`, `preflight_cache::PreflightCache`, the *Permission* dispatch (`ffi::SipiPermType` + `routes::PERMISSIONS`/`permission_from_str`/`permission_str`, dispatched on in `dispatch_engine`'s auth gate, `file_access` and `restricted_dims`), `routes::restriction` (the one restricted-view `(RW, RH)` decision, refusals included, for info.json, knora.json and the image route), `ServerOverrides`, the `ffi.rs` `#[repr(C)]` mirrors + layout-lock tests, the edge-config parsers (`config.rs`: `SIPI_PUBLIC_HOSTS`, `SIPI_REQUEST_TIMEOUT`, `SIPI_BODY_READ_TIMEOUT`), `jwtkey_is_unsafe` (fail-closed startup gate)
- **Public interface:** crate `sipi` (`//src/server/rust:lib`) — `pub fn run` and `pub fn app`; `ServerOverrides`.
- **Local-context kit:** `src/server/rust/src/lib.rs`, `src/server/rust/src/routes.rs`, `src/server/rust/src/ffi.rs`, `src/server/rust/src/config.rs`, `src/server/rust/BUILD.bazel`, `src/ffi/cpp/sipi_ffi.h` (the C++ side of the seam)
- **Depends on:** ffi (`//src/ffi:sipi_ffi`, the first Rust→C++ link; carries the whole engine); iiifparser (`//src/iiifparser/rust:iiif_parser`, the domain-typed URL parser); axum/tokio/opentelemetry/sentry (via the single `@crates` hub)
- **Used by:** cli (`//src/cli/rust:sipi` calls `sipi::run`)
- **Boundary rules:**
  - Production Rust comments describe current behaviour on their own terms — the oracle vocabulary (`oracle|shttps|cutover|parity|strangler|C++ server`) is avoided in `src/server/rust/src` + `src/cli/rust/src` `.rs` files. *Enforcement: `docs-only`* (the `CONVENTIONS.md` § Production surface rule; DUNE-012).
  - The listen-port precedence chain has a single authority in `lib.rs::serve()` (`SIPI_RS_PORT` > `--serverport`/`SIPI_SERVERPORT` > Lua `sipi.port` > `DEFAULT_PORT=1024`). *Enforcement: `docs-only`* (one code site; doc copies are pointers).
  - `IMAGE_MIMES` must list the same image mimes as the C++ `detect_in_format`. *Enforcement: `docs-only`* (cross-reference comment; no mechanical check).
  - The *Permission* vocabulary is gated twice over one string set — `scripting`'s `valid_permission` (inside the Lua runtime; an unlisted string fails the hook, so the request is a 500, never a permission) and this crate's `permission_from_str` — and the two must stay in lockstep. The `permission_vocabulary_is_identical_on_both_gates` unit test pins them; `permission_str`'s exhaustive match makes a new variant a compile error. *Enforcement: `static-analysis`* (the round-trip test) + `structure` (the exhaustive match).
  - Startup fails closed on an unsafe `jwt_secret` (empty, `<32` bytes, or the shipped default) via `jwtkey_is_unsafe` (`lib.rs`); a docroot overlapping a writable/image root and an init-script failure likewise refuse startup. *Enforcement: `static-analysis`* (the `jwtkey_is_unsafe` unit tests + startup boot probes).
  - Request lifetime is edge-bounded by `SIPI_REQUEST_TIMEOUT` (default 60s, to the response head) and `SIPI_BODY_READ_TIMEOUT` (default 10s, request-body read) via tower-http layers; `SIPI_PUBLIC_HOSTS` allowlists `X-Forwarded-Host`/`Host` (empty = reflect verbatim). The header-read timeout and connection cap stay Traefik-side (S2-18). *Enforcement: `docs-only`*.
- **Durable state:** `AppState` (built once per `serve()`), which owns the two-lane `Admission` pool (`Arc<admission::Admission>`, holding the semaphores + per-partition counters — see the `throttling` component); the opt-in Preflight cache.

## Cross-cutting concerns

Files and rules that span components rather than living in one:

- **The FFI seam** (`src/ffi/cpp/sipi_ffi.h` ↔ `src/server/rust/src/ffi.rs`) is the single contract between the shell and the engine; its layout is locked on both sides (see the `ffi` and `server` entries). A change to any `#[repr(C)]` struct is a two-file edit by construction.
- **The metrics bridge** flows `observability::Metrics` (engine) → `SipiMetricsSnapshot` (`src/ffi/cpp/metrics_snapshot.h`) → `server/rust/src/metrics.rs` → OTLP. The `metrics_registry_test` seam tripwire is the mechanical guard that a new counter is a conscious bridge-or-not decision.
- **The Throttling gate** (`src/ffi/cpp/serve_image.cpp`, via `decode_guard.cpp`'s `acquire_full_lane_budget`) is the one post-cache point where the engine-side memory budget fires (ADR-0008); the shell's two-lane `Admission` pool is a separate, earlier (pre-dispatch) admission layer (`server/rust/routes.rs`, `//src/throttling/rust:admission`). See the `throttling` component and ADR-0022.
- **Ubiquitous language** — identifiers/comments follow `UBIQUITOUS_LANGUAGE.md`; the reviewer checklist (`docs/src/development/reviewer-guidelines.md` § Ubiquitous Language) and the `CONVENTIONS.md` § Production surface rule are the guards (convention-only — no mechanical gate).

## Support areas (completeness coverage)

These carry no component boundary rules but exist so every tracked file maps somewhere:

- **Tests** — `:(glob)test/**` (unit under `test/unit/**` — shared fixture generators only (`test/unit/fixtures/`); every per-module unit-test suite is co-located under its own package (ADR-0003) — snapshot regression under `test/approval/**`, Rust reqwest e2e under `test/e2e/**`, fixtures under `test/_test_data/**`). Every unit test links its own module's narrow Bazel target; `//src:sipi_lib` has exactly one test consumer, `//test/approval`'s snapshot-regression binary.
- **Build & tooling** — `:(glob)MODULE.bazel`, `:(glob)MODULE.bazel.lock`, `:(glob)Cargo.Bazel.lock`, `:(glob).bazelrc` (absent = tracked via workflow), `:(glob)justfile`, `:(glob)bazel/**`, `:(glob)tools/**`, `:(glob)platforms/**`, `:(glob).github/**`, `:(glob)flake.nix`, `:(glob)flake.lock`, `:(glob)rustfmt.toml`, `:(glob)codecov.yml`, `:(glob)version.txt`, `:(glob)osv-scanner.toml`, `:(glob)BUILD.bazel` (root), `:(glob).bazelignore`, `:(glob).bazelversion`, `:(glob).clang-format`, `:(glob).clang-tidy`, `:(glob).clangd` (reads the `compile_commands.json` that `just compile-commands` writes), `:(glob).clang_complete`, `:(glob).cmake-format.yaml`, `:(glob).commitlintrc.yml`, `:(glob).envrc`, `:(glob).gitattributes`, `:(glob).gitignore`, `:(glob).grenrc`, `:(glob).lsan_suppressions.txt`. CI gates: `just bazel-rustfmt-check`, `just bazel-clippy-check`, `just commit-lint`, the approval + e2e + unit suites.
- **Docs & agent-context** — `:(glob)docs/**`, `:(glob)*.md` (CLAUDE.md, CONTEXT.md, UBIQUITOUS_LANGUAGE.md, CONVENTIONS.md, REVIEW.md, RELEASING.md, README.md, DEPRECATIONS.md, ARCH-MAP.md), ADRs under `docs/adr/**`; `:(glob)LICENSE`, `:(glob)diagrams/**` (the excalidraw/pdf/png architecture diagrams).
- **Runtime assets** — `:(glob)include/**` (generated headers, ICC profiles, favicon), `:(glob)server/**`, `:(glob)config/**`, `:(glob)scripts/**`, `:(glob)certificate/**`, `:(glob)db/**`, `:(glob)openseadragon.min.js.map`, `:(glob)test_tifs.sh`, `:(glob).claude/**`.

## Conventions

- **Module granularity** — one Bazel `cc_library` per concern under `src/<mod>/`, source + header + `*_test.cpp` colocated (ADR-0003). Local-context-kit budget **≤7 files**.
- **Top-level dependency direction (one-way):** `cli → server → //src/ffi:sipi_ffi → //src/image → {metadata, iiifparser, format_handlers:output_sink, util, logging, observability}`; `format_handlers` and `image_processing` sit beside/below the engine and depend on `//src/image`, never the reverse; `//src/scripting/rust` is a shell-side Rust crate over the seam. **The engine never links the shell or the seam.** *Enforcement: `structure`* (Bazel dep graph; a back-edge fails analysis).
- **New work is added by dropping a file / adding a route, not editing a central switch** — a new offline verb is one file in `src/cli/cpp/commands/`; a new axum route is a registration in `server/rust/lib.rs::app()`; a new engine module is a new `cc_library` package. *Enforcement: `docs-only`.*
  - **Exception (banned-construct):** new image format → editing the `SipiImage::io` registry + `read`/`read_shape` switch (~4 shared sites, `tools/format-handlers-fanout.sh`) → *why it couples:* the dispatch is centralized, so a 5th format is a multi-file shared edit → *alternative:* a descriptor-registration table (deferred until a real 5th format; ADR-0006) → *enforcement:* `docs-only`.
  - **Exception (banned-construct):** new *Permission* type → a hand-mirrored vocabulary across three components: the `sipi_ffi.h` enum + its `static_assert`s, `ffi.rs`'s enum + `const _` value guard, `scripting`'s `entry.rs::valid_permission`, `server`'s `routes.rs::{PERMISSIONS, permission_from_str, permission_str}` and its three dispatch sites (`dispatch_engine`'s auth gate, `file_access`, `restricted_dims`), plus the prose that enumerates the vocabulary (`UBIQUITOUS_LANGUAGE.md`, `CONTEXT.md`, `docs/src/lua/index.md`) → *why it couples:* the enum crosses the seam and is gated on both sides of it, so a locally-obvious edit to one gate is globally wrong → *alternative:* none; the vocabulary is a published contract → *enforcement:* `structure` (`permission_str`'s exhaustive match + the seam value asserts) + `static-analysis` (the both-gates round-trip test) + `docs-only` (the prose).
- **Test seam** — a helper that must be unit-tested but not publicly callable goes in an `internal/` subpackage with visibility restricted to its parent (`//src/metadata/cpp/internal` is the model); a target may carry a documented per-target grant beyond the parent (`:icc_lcms2` is the instance). *Enforcement: `structure`.*
- **Colocated docs** — every `cc_library`/`rust_library` package carries a `BUILD.bazel` docstring; invariant banners sit at the file head next to the code they govern (`src/metadata/cpp/icc.h`, the format encoders, `src/observability/cpp/metrics.h`). *Enforcement: `docs-only`.*
- **C++ libraries reach a Rust target through `link_deps`, never `deps`** — `deps` is for crates; rules_rust (0.74+) deprecates a `cc_library` in `deps` and will drop support. *Enforcement: `docs-only`* (rules_rust prints an analysis-time warning, not an error).
- **`layering_check`** is deferred repo-wide (vendored native deps emit no module maps — DEV-6353) except `//src/logging`, where it passes today. *Enforcement: `static-analysis`* (where enabled).
- **Banned constructs:**
  - oracle-era framing (`oracle`/`shttps`/`cutover`/`parity`/`strangler`/`C++ server`) in production Rust comments → couples the code to a removed transport → describe current behaviour on its own terms → *enforcement:* `docs-only` (the `CONVENTIONS.md` § Production surface rule).
  - a `server` clap flag that is silently dropped → a config option that never reaches the engine → destructure exhaustively in `From<&ServerArgs>` (bind `field: _` to drop deliberately) → *enforcement:* `structure`.
  - "file" as a domain noun for the served byte stream, "backpressure" for the load-shed policies, "canonical URL" for the cache key → drift from the ubiquitous language → use Bitstream / Throttling / Cache key (`UBIQUITOUS_LANGUAGE.md`) → *enforcement:* `docs-only` (reviewer checklist) + `static-analysis` for the oracle subset.
