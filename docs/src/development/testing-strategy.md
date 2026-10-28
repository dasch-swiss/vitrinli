# Testing Strategy

<!-- last_reviewed: 2026-07-10 -->

This document defines the authoritative testing strategy for sipi. It maps the IIIF Image API 3.0 specification, sipi's extensions (Lua scripting, cache, CLI, knora integration), and Rust migration readiness onto a concrete testing pyramid. Use this document to determine **where** a new test should live, **what** layer it belongs to, and **how** to assess coverage.

## Vitrinli Feature Inventory

Vitrinli is a multithreaded, high-performance, IIIF-compatible media server written in C++23. The following is an exhaustive inventory of every feature area. Each feature needs test coverage — the [coverage matrices](#iiif-image-api-30-coverage-matrix) later in this document track the current state.

### IIIF Image API 3.0

Vitrinli implements the full [IIIF Image API 3.0](https://iiif.io/api/image/3.0/) at Level 2 compliance:

| Feature Area | Parameters | Source |
|---|---|---|
| **Region** (Section 4.1) | `full`, `square`, `x,y,w,h`, `pct:x,y,w,h` | `src/iiifparser/cpp/value_objects/SipiRegion.cpp` |
| **Size** (Section 4.2) | `max`, `w,`, `,h`, `w,h`, `!w,h`, `pct:n`, `^` upscale variants | `src/iiifparser/cpp/value_objects/SipiSize.cpp` |
| **Rotation** (Section 4.3) | Arbitrary angles (float), `!` mirror prefix | `src/iiifparser/cpp/value_objects/SipiRotation.cpp` |
| **Quality** (Section 4.4) | `default`, `color`, `gray`, `bitonal` | `src/iiifparser/cpp/value_objects/SipiQualityFormat.cpp` |
| **Format** (Section 4.5) | `jpg`, `png`, `tif`, `jp2`, `webp` | `src/iiifparser/cpp/value_objects/SipiQualityFormat.cpp` |
| **Identifiers** (Section 3) | URL-encoded, `%2F` slash, prefix-based resolution | `src/iiifparser/cpp/value_objects/SipiIdentifier.cpp` |
| **Info.json** (Section 5) | Full response: `@context`, `id`, `type`, `protocol`, `profile`, `width`, `height`, `sizes`, `tiles`, `extraFormats`, `extraFeatures`, `preferredFormats` | `src/server/rust/src/info.rs` |
| **Content negotiation** | `Accept: application/ld+json` → JSON-LD with `@context`; default → `application/json` | `src/server/rust/src/info.rs` |
| **HTTP behavior** (Section 7) | Base URI redirect, HEAD, CORS, Link headers, canonical URI, 400/401/403/404/500/501 errors | `src/server/rust/src/routes.rs` |
| **IIIF Extension: `red:n`** | Reduce factor for JP2 (faster subsampling on read) — sipi-specific, not in IIIF spec | `src/iiifparser/cpp/value_objects/SipiSize.cpp` |

!!! note "IIIF Auth exclusion"
    Vitrinli does **not** implement the IIIF Authentication API. Access control is handled via custom Lua preflight scripts (`pre_flight`, `file_pre_flight`) that return allow/deny/restrict permissions. This is intentional — the Knora/DSP integration requires custom auth flows (cookie-based sessions) that don't map to the IIIF Auth spec.

### Image Format Support

**Input/Output Formats:**

| Format | Handler | Read | Write | Notes |
|---|---|---|---|---|
| TIFF | `SipiIOTiff` | Yes | Yes | Multi-page, tiled, pyramid; LZW/ZIP/CCITT compression; 1/8/16-bit; RGB/YCbCr/CMYK |
| JPEG | `SipiIOJpeg` | Yes | Yes | 8-bit, progressive and baseline; configurable quality |
| PNG | `SipiIOPng` | Yes | Yes | Palette/gray/RGB/RGBA; 1/2/4/8/16-bit |
| JPEG2000 | `SipiIOJ2k` | Yes | Yes | Via Kakadu (commercial license); reduce factor; quality layers; progression orders |
| WebP | `SipiIOWebp` | Yes | Yes | Via libwebp |

**Metadata Systems:**

| System | Library | Read | Write | Source |
|---|---|---|---|---|
| EXIF | Exiv2 | Yes | Yes | `src/metadata/cpp/exif.cpp` |
| XMP | Exiv2 | Yes | Yes | `src/metadata/cpp/xmp.cpp` |
| IPTC | Exiv2 | Yes | Yes | `src/metadata/cpp/iptc.cpp` |
| ICC Profiles | littleCMS2 | Yes | Yes | `src/metadata/cpp/icc.cpp` |
| Essentials | Custom | Yes | Yes | `src/metadata/cpp/essentials.cpp` |

**Predefined ICC Profiles:** sRGB, AdobeRGB, GRAY_D50, LUM_D65, CMYK_standard, LAB, ROMM_GRAY.

**Essentials** is a custom metadata packet embedded in image headers. It stores: original filename, MIME type, pixel data checksum (MD5/SHA1/SHA256/SHA384/SHA512), and a backup of the ICC profile. This survives format conversions and enables provenance tracking.

**Color Space Support:** RGB, Grayscale, Bitonal, YCbCr, CMYK (with conversion to sRGB), CIELab (with conversion). 8 TIFF orientations handled; `topleft()` normalization for tiling compatibility. 16-bit big-endian internal representation with automatic 8-bit conversion where needed.

### Image Processing Pipeline

The IIIF processing pipeline applies transformations in spec-mandated order:

1. **Region** — crop the source image
2. **Size** — scale to requested dimensions
3. **Rotation** — rotate and/or mirror
4. **Quality** — color space conversion (color, gray, bitonal)
5. **Format** — encode to output format

Each step allocates an intermediate buffer. Peak memory is ~2x image size per transform step.

**Watermarking** is applied as an additional step when the preflight script returns a `restrict` permission with a watermark path. Watermark files must be single-channel 8-bit gray TIFF (SAMPLESPERPIXEL=1, BITSPERSAMPLE=8, PHOTOMETRIC=MINISBLACK).

### HTTP Server (Rust axum shell)

The production HTTP server is the Rust axum shell (`src/server/rust` +
`src/cli/rust`), which drives the C++ image engine over the FFI seam:

| Feature | Details | Source |
|---|---|---|
| SSL/TLS | Terminated at Traefik in front of the shell; `sslport`/`sslcert`/`sslkey` keys parse but are inert | `src/server/rust/src/config.rs` |
| Threading | Bounded worker pool with a request queue (env-tunable: `SIPI_NTHREADS`, `SIPI_MAX_WAITING`, `SIPI_QUEUE_TIMEOUT`) | `src/server/rust/src/routes.rs` |
| Keep-alive | `keepalive`/`SIPI_KEEPALIVE` CLI/env flag; parse-only, unread | `src/server/rust/src/config.rs` |
| Chunked transfer | axum/hyper streaming responses | `src/server/rust/src/sink.rs` |
| Range requests | HTTP 206 Partial Content | Handler-level |
| CORS | Via Lua preflight scripts | `scripts/` |
| Methods | GET, POST, PUT, DELETE | `src/server/rust/src/lib.rs` |
| Authentication | JWT (HS256), HTTP Basic Auth, cookie support | `src/scripting/rust/bindings/server.rs` |
| Max POST size | Configurable (`max_post_size`, default 300M) | `src/server/rust/src/config_file.rs` |
| Multipart upload | Form-data file upload retained as a Lua route (ADR-0017) | `scripts/` |

### Caching System

File-based LRU cache with dual-limit eviction (`src/cache/cpp/SipiCache.h`, `src/cache/cpp/SipiCache.cpp`):

| Feature | Details |
|---|---|
| **Eviction policy** | LRU by access time; evicts down to 80% low-water mark |
| **Size limit** | `cache_size`: `'-1'`=unlimited, `'0'`=disabled, or `'200M'`, `'1G'` |
| **File count limit** | `cache_nfiles`: 0=no limit |
| **Crash recovery** | Serialized index on disk; rebuild from directory scan if index missing |
| **Concurrent access** | Mutex-protected; `blocked_files` map prevents reads during writes |
| **Canonical key** | Full IIIF URL (with watermark flag) as cache key |
| **Metrics** | hits, misses, evictions, skips, size, file count — the scalar counters cross the FFI seam via `SipiMetricsSnapshot` and export over OTLP (see below) |
| **API endpoints** | None. There is no `/api/cache` route on the Rust shell; observe cache behaviour via the on-disk cache-dir file count |
| **Cache metadata** | Image dimensions, tile info, pyramid levels, MIME type, checksum per entry |

### Lua Scripting System

Per-request isolated Lua 5.3.5 interpreter with full server access:

**SipiImage Lua Class:**

| Method | Description |
|---|---|
| `SipiImage.new(filepath)` | Load image from file |
| `SipiImage.new(upload_index)` | Load from uploaded file |
| `SipiImage.new(filepath, {region=, size=, reduce=, ...})` | Load with IIIF options |
| `img:dims()` | Get dimensions (nx, ny, orientation) |
| `img:exif(tag)` | Read EXIF data |
| `img:crop(region)` | Crop to IIIF region |
| `img:scale(size)` | Resize to IIIF size |
| `img:rotate(rotation)` | Rotate/mirror |
| `img:topleft()` | Normalize orientation |
| `img:watermark(path)` | Apply watermark |
| `img:write(target)` | Write to file or `'HTTP.jpg'` for HTTP response |

**Server Object:**

| Method/Property | Description |
|---|---|
| `server.method`, `.path`, `.get`, `.post`, `.content` | Request data |
| `server.sendStatus(code)` | Set HTTP status |
| `server.sendHeader(name, value)` | Set response header |
| `server.print(...)` | Write response body |
| `server.setBuffer()` | Enable buffered output |
| `server.requireAuth()` | Require HTTP basic auth |
| `server.generate_jwt(table)` | Create HS256 JWT |
| `server.decode_jwt(token)` | Decode/verify JWT |
| `server.http(method, url, headers, timeout)` | Outbound HTTP request |
| `server.json_to_table(json)` / `server.table_to_json(t)` | JSON conversion |
| `server.getMimeType(filename)` | MIME detection |
| `server.log(msg, level)` | Structured logging |
| `server.uuid_to_base62(uuid)` / `server.base62_to_uuid(b62)` | UUID encoding |

**Cache Object:**

| Method | Description |
|---|---|
| `cache.filelist(sort)` | List entries (sort: AT_ASC, AT_DESC, FS_ASC, FS_DESC) |
| `cache.delete(canonical)` | Remove entry by canonical URL |
| `cache.purge()` | Trigger LRU eviction |
| `cache.nfiles()` / `cache.size()` | Current counts |

**SQLite Integration:** `server.db` provides Lua access to SQLite databases for custom data storage.

**Preflight Scripts:**

- `pre_flight(prefix, identifier, cookie)` → returns permission (`allow`, `deny`, `restrict`) + filepath for IIIF requests
- `file_pre_flight(prefix, identifier, cookie)` → same for raw file downloads
- Restriction types: `watermark` (overlay image), `size` (reduce dimensions)

**Custom Routes:** Lua scripts mapped to HTTP method + URL pattern via config `routes` table.

### CLI Mode

Vitrinli operates in three CLI modes (`src/cli/cpp/cli_app.cpp`):

**File Conversion:** `sipi infile outfile [options]`

| Option | Description |
|---|---|
| `-F, --format` | Output format (jpg, tif, png, jpx, j2k, webp) |
| `-I, --icc` | ICC profile conversion (sRGB, AdobeRGB, GRAY) |
| `-q, --quality` | JPEG quality (1-100) |
| `-n, --pagenum` | Multi-page file page selection |
| `-r, --region` | Crop region (pixels) |
| `-s, --size` | IIIF-format size |
| `--scale` | Percentage scaling |
| `-R, --reduce` | JP2 reduce factor |
| `-m, --mirror` | Mirror: horizontal, vertical |
| `-o, --rotate` | Rotation angle (0-360) |
| `-k, --skipmeta` | Strip all metadata |
| `-w, --watermark` | Apply watermark (TIFF) |

**JP2-specific options:** `--Sprofile`, `--Clayers`, `--Clevels`, `--Corder`, `--Cprecincts`, `--Cblk`, `--Stiles`, `--Cuse_sop`, `--rates`, `--Ctiff_pyramid`

**Query Mode:** `sipi query infile` — print image metadata.

**Compare Mode:** `sipi compare file1 file2` — compare two images.

**Server Mode:** the Rust axum shell (`//src/cli/rust:sipi server --config config.lua`) with CLI overrides for port, hostname, imgroot, cache settings, JWT, admin credentials, logging, etc. (TLS terminates at Traefik; `sslcert`/`sslkey` flags parse but are inert.)

### Configuration System

Lua-based configuration (`src/ffi/cpp/SipiConf.h`, `src/ffi/cpp/SipiConf.cpp`):

| Category | Keys |
|---|---|
| **Server** | `port`, `nthreads` |
| **Image repository** | `imgroot`, `prefix_as_path` |
| **Image processing** | `jpeg_quality`, `scaling_quality.{jpeg,tiff,png,j2k}` (high/medium/low) |
| **Cache** | `cache_dir`, `cache_size`, `cache_nfiles` |
| **Request handling** | `max_post_size`, `tmpdir`, `max_temp_file_age` |
| **Lua** | `initscript`, `scriptdir`, `thumb_size` |
| **Authentication** | `jwt_secret` (≥32 bytes) |
| **Static files** | `fileserver.docroot`, `fileserver.wwwroute` |
| **Knora/DSP** | `knora_path`, `knora_port` |
| **Routes** | `routes` table: `{method, route, script}` |

**Deprecated keys:** `cachedir` → `cache_dir`, `cachesize` → `cache_size`, `cache_hysteresis` → removed (80% low-water mark is now hardcoded).

### Metrics (OTLP)

The cache counters and gauges are exported over OTLP, not a scrape endpoint. The C++ engine maintains them (`src/observability/cpp/metrics.h`, `src/observability/cpp/metrics.cpp`); the scalar fields cross the FFI seam via `SipiMetricsSnapshot` and the Rust shell (`src/server/rust/src/metrics.rs`) emits them over OTLP. There is no `GET /metrics` route. Test accordingly: cache observability goes through on-disk cache-dir file counts (`cache.rs`), not a scrape endpoint.

| Metric | Type | Description |
|---|---|---|
| `cache_hits_total` | Counter | Total cache hits |
| `cache_misses_total` | Counter | Total cache misses |
| `cache_evictions_total` | Counter | Total files evicted |
| `cache_skips_total` | Counter | Cache checks skipped (cache disabled) |
| `cache_size_bytes` | Gauge | Current cache size |
| `cache_files` | Gauge | Current cached file count |
| `cache_size_limit_bytes` | Gauge | Configured size limit |
| `cache_files_limit` | Gauge | Configured file count limit |

### Upload & Ingest

- Multipart form-data file upload via POST
- Format conversion on upload (e.g., TIFF → JP2) via Lua routes
- knora.json sidecar generation with checksums, MIME type, original filename, dimensions
- Essentials metadata embedded during ingest

### Security Features

| Feature | Details |
|---|---|
| JWT validation | HS256 with configurable secret; decode + verify in Lua |
| HTTP Basic Auth | Via `server.requireAuth()` in Lua routes |
| Preflight access control | `pre_flight` / `file_pre_flight` scripts return allow/deny/restrict |
| Path traversal prevention | URL-decoded identifier validation |
| Max POST size | Configurable (`max_post_size`, default 300M) |
| Admin auth | Separate admin user/password for management endpoints |

### Integration Features

| Feature | Details |
|---|---|
| **Knora/DSP** | Session cookie validation, knora.json sidecar, configurable API path/port |
| **File access** | Raw file download via `/prefix/identifier/file` with `file_pre_flight` auth |
| **Sentry** | Error reporting via `SIPI_SENTRY_DSN`, `SIPI_SENTRY_ENVIRONMENT`, `SIPI_SENTRY_RELEASE` |

### Build & Deployment

| Feature | Details |
|---|---|
| **Build systems** | Bazel (Nix provisions the dev shell only) |
| **CI** | GitHub Actions: unit tests, e2e tests, fuzz (nightly), Docker builds |
| **Documentation** | MkDocs Material site, LLM-optimized `llms.txt` output |
| **Dependency management** | Bazel `bazel_dep` (BCR) + `http_archive` source pins with SHA-256 checksums in `MODULE.bazel` |

---

## Testing Pyramid

Four layers, from fastest/narrowest to slowest/broadest:

```
                    ┌─────────┐
                    │  Fuzz   │  Continuous (nightly CI)
                    │ Testing │  Finds crashes & edge cases
                ┌───┴─────────┴───┐
                │   E2E Contract   │  Rust harness (reqwest)
                │   Tests (HTTP)   │  Tests the API contract
            ┌───┴─────────────────┴───┐
            │   Integration / Snapshot │  insta golden baselines
            │   Tests                  │  Regression detection
        ┌───┴─────────────────────────┴───┐
        │         Unit Tests               │  GoogleTest (C++) where it's the only reach
        │         + Rust unit tests        │  New: Rust #[test] + proptest (preferred)
        └──────────────────────────────────┘
```

**Distribution target (post-Rust-migration steady state):** ~50% unit, ~30% e2e contract, ~15% snapshot/integration, ~5% fuzz. Current distribution (~47% unit, ~52% e2e) is inverted because the C++ codebase lacks Rust unit tests; as migration progresses and Rust `#[test]` modules grow, the ratio will shift toward the target.

## Layer Definitions

### Layer 1: Unit Tests (fastest, most numerous)

**Purpose:** Test individual functions and parsers in isolation.

| Sublayer | Framework | Location | When to use |
|---|---|---|---|
| C++ unit | GoogleTest | `test/unit/` | When the behavior cannot be reached from Rust e2e — internal C++ state, FFI boundaries, RAII / exception paths, concurrency primitives |
| Rust unit (preferred) | `#[test]` + `proptest` | Future `src/` modules | During Rust migration: inline `#[cfg(test)]` modules |
| Rust property-based | `proptest` | Future `src/` modules | Parsers, serializers, roundtrip invariants |

**What belongs here:**

- IIIF URL parsing (region, size, rotation, quality, format)
- Filename hashing
- Configuration parsing
- HTTP header parsing, URL encoding/decoding
- Image metadata extraction
- Any pure function with well-defined inputs/outputs

**C++ unit-test policy:** Default to Rust unit tests or Rust e2e for new coverage — they survive the migration and exercise the contract clients see. Add C++ unit tests where they are the only practical reach, and only there. In practice this means:

- **Internal C++ state** that has no HTTP-observable surface (cache eviction order, memory-budget acquire/release, parser intermediate AST).
- **FFI / C-library boundaries** where the bug class lives in the C++↔C interop (libtiff variadic-arg type widths, kakadu callback lifetimes, lcms2 profile ownership) and Rust e2e would observe only the downstream symptom.
- **Move/copy/RAII semantics** that pure-function tests can pin but server-level tests cannot (assignment-operator buffer lifecycle, exception-safety guarantees, `noexcept` move correctness).
- **Concurrency primitives** with lock-free or atomic semantics (`SipiMemoryBudget`) where deterministic e2e reproduction is impractical.
- **Engine-internal coverage:** C++ engine components the Rust shell drives (cache management, decode pipeline) are covered with C++ unit tests that travel with the C++ code. This documents existing engine behavior and avoids false failures when internal C++ behavior changes while the HTTP contract the shell exposes stays fixed. (Lua scripting is Rust-hosted — its coverage is the Rust `#[test]` files in `src/scripting/rust/`.)

Behavior that *is* HTTP-observable belongs in Rust e2e — not in C++ unit tests.

### Layer 2: Snapshot / Golden Baseline Tests

**Purpose:** Detect unintended output changes via approved golden baselines.

| Framework | Location | When to use |
|---|---|---|
| `insta` (Rust) | `test/e2e/tests/snapshots/` | info.json structure, HTTP headers, response metadata |
| ApprovalTests (C++) | `test/approval/` | Image-conversion metadata fingerprints; byte-exact encoder output baselines |

**ApprovalTests scope:** New C++ approval suites are admissible only for byte-exact encoder-output gating against dependency migrations (e.g. libtiff, libjpeg, kakadu, lcms2 version bumps) and for metadata fingerprints that have no Rust-side equivalent. Functional behaviour belongs in Layer 3 (Rust e2e), not here.

**What belongs here:**

- Full info.json structure (field names, types, values)
- HTTP response header sets (content-type, CORS, Link)
- knora.json response structure
- Image metadata fingerprints (EXIF tags, XMP fields, ICC profile name) — golden baselines prevent silent metadata drift during code changes or format handler updates
- Byte-exact encoder output baselines for dep-migration regression gating (TIFF/JP2/PNG goldens captured under deterministic encoder settings)
- Any complex output where field-by-field assertion is fragile

**Pattern:** Use `insta::assert_json_snapshot!` with redactions for dynamic fields (`id`, timestamps).

**ICC determinism gate.** Approval tests under `test/approval/image_encode_baseline_test.cpp` are byte-stable only when `SOURCE_DATE_EPOCH` is set. lcms2 stamps wall-clock UTC into bytes 24-35 of every freshly-created ICC profile (and several emit paths in SipiImage round-trip through that), so without the env var the seconds field drifts across consecutive runs of the same binary. `Icc::iccBytes()` overwrites those bytes (and zeros the Profile ID at bytes 84-99) when the env var is set; CMake injects `SOURCE_DATE_EPOCH=946684800` (2000-01-01T00:00:00Z UTC) into the `sipi.approvaltests` invocation via `set_tests_properties(... ENVIRONMENT ...)`. Production never sets the var and retains lcms2's wall-clock behaviour. See [`docs/adr/0002-icc-profile-determinism-test-only.md`](../../adr/0002-icc-profile-determinism-test-only.md) for the architectural rationale and [`test/approval/CHANGELOG.approval.md`](../../../test/approval/CHANGELOG.approval.md) for the maintainer's how-to.

### Layer 3: E2E Contract Tests (HTTP-level)

**Purpose:** Test sipi's HTTP API contract — the behavior visible to clients. These tests survive the Rust migration because they test the contract, not the implementation.

| Sublayer | Framework | Location | When to use |
|---|---|---|---|
| All HTTP contracts | Rust (`reqwest`) | `test/e2e/tests/` | Status/header smokes, multi-step workflows, response body inspection, uploads, golden snapshots |

**What belongs here:**

- IIIF Image API 3.0 compliance (ALL testable requirements)
- Content negotiation (Accept header → Content-Type)
- CORS (preflight, origin echo, wildcard)
- Authentication/authorization (401, 403)
- Error handling (400, 404, 500, 501)
- File upload and retrieval
- Lua endpoint contracts
- Cache behavior (hit/miss via headers or metrics)
- Video/non-image file handling
- CLI mode testing (via process spawn + file output verification)
- Range requests
- Concurrent request handling

### Layer 4: Property & Fuzz Testing

**Purpose:** Find crashes, memory safety issues, and edge cases in parsers and input handlers.

| Framework | Location | When to use |
|---|---|---|
| `proptest` (Rust) | `test/e2e/tests/proptest_iiif_uri.rs` | Property-based coverage of the Rust IIIF URI parser (`parse_request`) |
| Corpus regression (Rust) | `src/iiifparser/rust/corpus_regression_test.rs` | Fixed sweep of the 241-file shared corpus through `parse_request` — the per-PR net |
| libFuzzer (`rules_fuzzing` + FFI shim) | `src/iiifparser/fuzz/` → `//src/iiifparser/fuzz:parse_request_fuzz` | Coverage-guided fuzzing of `//src/iiifparser/rust:iiif_parser` (`parse_request`) |
| libFuzzer (codec decode harnesses) | `src/format_handlers/fuzz/` → `//src/format_handlers/fuzz:{tiff,jpeg,png,j2k}_decode_fuzz` | Coverage-guided fuzzing of the four `SipiIO` codec decode entry points (DEV-7066) |

**Status: shipped.** Every fuzz target has two modes from one definition. By
default (`rules_fuzzing`'s `replay` engine, no libFuzzer runtime) it is a
corpus-replay regression test that builds and runs on every platform and rides
along in the `//src/...` sweeps — including the sanitizer leg. `--config=fuzz`
arms the real libFuzzer engine plus SanitizerCoverage on the Rust crate graph
(for `parse_request_fuzz`) or the C++ codec handlers (for the four
`format_handlers` decode harnesses); that mode is Linux-only and runs nightly.

```bash
bazel test //src/iiifparser/fuzz:parse_request_fuzz          # corpus replay, any platform
bazel test //src/format_handlers/fuzz:tiff_decode_fuzz       # corpus replay, any platform
just fuzz -max_total_time=60                                 # mutation loop, Linux
```

The retired C++ libFuzzer harness (`//fuzz/handlers`) fuzzed the oracle-only C++
classifier and went away with the oracle. See [Fuzzing](fuzzing.md) for the shim
seam, the corpus policy, and the nightly workflow.

**What belongs here:**

- IIIF URI parser (`//src/iiifparser/rust:iiif_parser` (`parse_request`))
- Image format header parsing (`//src/format_handlers/fuzz:{tiff,jpeg,png,j2k}_decode_fuzz`, the `SipiIO` decode entry points)
- Any function that processes untrusted input

## Test Decision Tree

```
New test needed?
├── Is it testing a pure function/parser?
│   ├── C++ component not yet migrated → maintain existing GoogleTest (no new suites)
│   └── Rust component → #[test] + proptest for property-based
├── Is it testing HTTP API behavior?
│   └── Rust e2e (`reqwest` + `serde_json` / `insta` for snapshots)
├── Is it regression detection for complex output?
│   └── insta snapshot (JSON structure, headers) — this is a Rust e2e test with insta
├── Is it testing untrusted input handling?
│   └── proptest (`proptest_iiif_uri.rs`) + the corpus regression sweep; the nightly
│       libFuzzer target (`//src/iiifparser/fuzz`) covers mutation
├── Is it testing image output correctness?
│   └── Rust e2e with `image` crate decode + dimension/checksum verification
└── Does it need filesystem setup or custom server config?
    └── Rust e2e with tempfile + custom SipiServer::start config
```

**Clarifications:**

- "Image output correctness" tests are a specialization of e2e contract tests, not a separate layer
- Snapshot tests (`insta`) live inside Rust e2e test files — they are e2e tests that use snapshot assertions
- Tests needing cache verification require a cache-enabled server config (use `sipi.cache-test-config.lua` which has cache configured)
- Python e2e tests have been retired and replaced by Rust e2e tests. See [Python Test Deprecation](#python-test-deprecation--parity-checklist) for the parity checklist
- Flaky tests (e.g., races against file flush) should use the `retry_flaky()` helper — see [Flaky Test Handling](#flaky-test-handling)

## IIIF Image API 3.0 Coverage Matrix

The following matrix maps every testable IIIF spec requirement to its test status. This is the authoritative coverage reference.

### Info.json (Section 5)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| `@context` field present and correct | :white_check_mark: | `info_json_context` | |
| `id` matches base URI | :white_check_mark: | `info_json_id_contains_base_uri` | |
| `type` = `ImageService3` | :white_check_mark: | `info_json_type_imageservice3` | |
| `protocol` = `http://iiif.io/api/image` | :white_check_mark: | `info_json_protocol` | |
| `profile` = `level2` | :white_check_mark: | `info_json_profile_level2` | |
| `width` and `height` integers | :white_check_mark: | `info_json_dimensions_match_lena512` | |
| `sizes` array with valid dimensions | :white_check_mark: | `info_json_sizes_have_valid_dimensions` | |
| `tiles` with scaleFactors | :white_check_mark: | `info_json_tiles_have_scale_factors` | |
| `extraFormats` | :white_check_mark: | `info_json_extra_formats` | |
| `preferredFormats` | :white_check_mark: | `info_json_preferred_formats` | |
| `extraFeatures` (17 features) | :white_check_mark: | `info_json_all_17_extra_features` | |
| Golden baseline snapshot | :white_check_mark: | `info_json_golden_snapshot` | insta |
| Header snapshot (CT, CORS, Link) | :white_check_mark: | `info_json_headers_snapshot` | insta |
| Content-Type without Accept | :white_check_mark: | `info_json_content_type_default` | `application/json` |
| Content-Type with Accept: ld+json | :white_check_mark: | `jsonld_media_type_with_accept` | |
| Link header on default request | :white_check_mark: | `jsonld_default_has_link_header` | |
| Canonical Link header | :white_check_mark: | `canonical_link_header` | |
| Profile Link header | :white_check_mark: | `profile_link_header` | emitted on image responses (DEV-6733) |
| X-Forwarded-Proto HTTPS rewrite | :white_check_mark: | `info_json_x_forwarded_proto_https` | |
| Required fields structural check | :white_check_mark: | `info_json_has_required_fields` | |
| Structural: sizes array exists | :white_check_mark: | `info_json_has_sizes_array` | |
| Structural: tiles array exists | :white_check_mark: | `info_json_has_tiles_array` | |
| Structural: extraFeatures exists | :white_check_mark: | `info_json_has_extra_features` | |

### Region (Section 4.1)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| `full` | :white_check_mark: | `full_iiif_url_returns_image` | |
| `square` | :white_check_mark: | `region_square` | |
| `pct:x,y,w,h` | :white_check_mark: | `region_percent` | |
| `x,y,w,h` (pixel) | :white_check_mark: | `region_pixel`, `region_pixel_offset` | |
| Overflow → crop at edge | :white_check_mark: | `region_beyond_bounds_is_cropped` | |
| Start beyond image → error | :white_check_mark: | `region_start_beyond_image` | |
| Zero width → 400 | :white_check_mark: | `region_zero_width` | |
| Invalid syntax → error | :white_check_mark: | `region_invalid_syntax` | |
| Region + size combination | :white_check_mark: | `size_after_region` | |
| Region + rotation combination | :white_check_mark: | `rotation_after_region` | |
| Region crop (specific) | :white_check_mark: | `iiif_region_crop` | |
| Region dimension verification | :white_check_mark: | `region_dimension_verification` | decodes output, asserts pixel dims match the requested region |

### Size (Section 4.2)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| `max` | :white_check_mark: | `full_iiif_url_returns_image` | implicit |
| `w,` (width) | :white_check_mark: | `size_by_width` | |
| `,h` (height) | :white_check_mark: | `size_by_height` | |
| `w,h` (exact) | :white_check_mark: | `size_exact` | |
| `!w,h` (best fit) | :white_check_mark: | `size_best_fit` | |
| `pct:n` | :white_check_mark: | `size_percent` | |
| `^` upscaling | :white_check_mark: | `size_upscaling` | |
| No upscale beyond original | :white_check_mark: | `size_no_upscale_beyond_original` | |
| Invalid syntax → error | :white_check_mark: | `size_invalid_syntax` | |
| Output dimension verification | :white_check_mark: | `size_dimension_verification` | decodes output, asserts actual pixel dimensions |
| `^max` upscale to limits | :white_check_mark: | `size_upscale_max` | |
| `^,h` (height-only upscale) | :white_check_mark: | `size_upscale_height` | |
| `^w,h` (exact with upscale) | :white_check_mark: | `size_upscale_exact` | |
| `^!w,h` (confined upscale) | :white_check_mark: | `size_upscale_confined` | |
| `^pct:n` (upscale percent) | :white_check_mark: | `size_upscale_percent` | |

### Rotation (Section 4.3)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| `0` (no rotation) | :white_check_mark: | `full_iiif_url_returns_image` | implicit |
| `90` | :white_check_mark: | `iiif_rotation_90` | |
| `180` | :white_check_mark: | `rotation_180` | |
| `270` | :white_check_mark: | `rotation_270` | |
| Arbitrary (e.g. 22.5) | :white_check_mark: | `rotation_arbitrary` | |
| `!0` (mirror only) | :white_check_mark: | `mirror_rotation` | |
| `!180` (mirror + rotate) | :white_check_mark: | `mirror_plus_180` | |
| Invalid → error | :white_check_mark: | `rotation_invalid` | |
| Rotation output verification | :white_check_mark: | `rotation_dimension_verification` | decodes output, asserts dimensions swap for 90° |

### Quality (Section 4.4)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| `default` | :white_check_mark: | `full_iiif_url_returns_image` | implicit |
| `color` | :white_check_mark: | `quality_color` | |
| `gray` | :white_check_mark: | `quality_gray` | |
| `bitonal` | :white_check_mark: | `quality_bitonal` | |
| Invalid → error | :white_check_mark: | `quality_invalid` | |
| `extraQualities` in info.json | :white_check_mark: | `extra_qualities_in_info_json` | asserts array shape when present; absence documented, not failed |

### Format (Section 4.5)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| `jpg` + Content-Type | :white_check_mark: | `format_jpg_content_type` | |
| `png` + Content-Type | :white_check_mark: | `format_png_content_type` | |
| `tif` + Content-Type | :white_check_mark: | `format_tiff_content_type` | |
| `jp2` + Content-Type | :white_check_mark: | `format_jp2_content_type` | |
| Unsupported (gif, pdf, webp, bmp) | :white_check_mark: | `unsupported_formats_rejected` | |

### CORS (Section 7.1)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| Info.json ACAO without Origin | :white_check_mark: | `cors_info_json_without_origin` | `*`, no credentials |
| Info.json ACAO with Origin | :white_check_mark: | `cors_info_json_with_origin` | Echoed Origin + credentials, `Vary: Origin` |
| Info.json ACAO with Origin and Cookie | :white_check_mark: | `cors_info_json_with_origin_and_cookie` | Echoed Origin + credentials |
| Info.json 401 auth challenge with Origin | :white_check_mark: | `cors_info_json_auth_challenge_with_origin` | Echoed Origin + credentials |
| Info.json OPTIONS preflight | :white_check_mark: | `cors_preflight_info_json` | Echoed Origin + credentials, explicit methods and headers |
| Info.json under `SIPI_ALLOWED_ORIGINS` | :white_check_mark: | `allowed_origins_allowlist_gates_info_json_cors` | `security.rs`; denied Origin gets no ACAO |
| Knora.json `Vary: Origin` with and without Origin | :white_check_mark: | `cors_knora_json_varies_on_origin` | `*` without Origin, echoed Origin + credentials with one |
| Image ACAO with Origin | :white_check_mark: | `cors_image_with_origin` | |
| Image ACAO without Origin | :white_check_mark: | `cors_image_without_origin` | |
| Error ACAO with Origin (400/401/403/404) | :white_check_mark: | `cors_error_responses_with_origin` | Image, info.json, knora.json and `/file` (no `/file` 403: the test hook never restricts); echoed Origin + credentials |
| Error ACAO on a 503 shed | :white_check_mark: | `lua_route_occupies_the_full_lane` | `admission_control.rs`; echoed Origin + credentials |
| Error ACAO under `SIPI_ALLOWED_ORIGINS` | :white_check_mark: | `allowed_origins_allowlist_gates_info_json_cors` | `security.rs`; 404 echoes a listed Origin, an unlisted one gets only `Vary: Origin` |
| OPTIONS preflight | :white_check_mark: | `cors_preflight` | |

### HTTP Behavior (Section 7)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| Base URI → redirect to info.json | :white_check_mark: | `base_uri_redirect` | |
| HEAD request | :white_check_mark: | `head_iiif_image_empty_body` | |
| 401 unauthorized | :white_check_mark: | `deny_unauthorized_image` | |
| 404 not found | :white_check_mark: | `id_random_gives_404` | |
| Path traversal rejected | :white_check_mark: | `path_traversal_rejected` | |
| Incomplete URL → error | :white_check_mark: | `id_incomplete_iiif_url` | |
| Malformed URL → error | :white_check_mark: | `id_malformed_iiif_url` | |
| Empty identifier → error | :white_check_mark: | `invalid_iiif_url_empty_identifier` | |
| HEAD returns headers | :white_check_mark: | `head_request_returns_headers` | |
| Missing file → 404 | :white_check_mark: | `returns_404_for_missing_file` | |
| HTTP 304 conditional requests | :white_check_mark: | `conditional_request_304` | If-Modified-Since round-trip |
| Operation ordering (Region→Size→Rot→Qual→Fmt) | :white_check_mark: | `operation_ordering` | |
| Fractional percent regions (e.g. pct:0.5,...) | :white_check_mark: | `fractional_percent_region` | |

### Identifier (Section 3)

| Requirement | Status | Test | Notes |
|---|---|---|---|
| Encoded slash `%2F` | :white_check_mark: | `id_escaped_slash_decoded` | |
| Encoded `#` (`%23`) | :white_check_mark: | `id_escaped` | resolves to the on-disk `#` file (DEV-6732) |
| Subdirectory identifier | :white_check_mark: | via server tests | |
| Non-ASCII identifiers | :white_check_mark: | `id_non_ascii` | |
| ARK/URN identifiers | :x: GAP | — | Not tested (may not apply — sipi identifiers are opaque path segments, not a namespaced scheme) |

## Vitrinli Extension Coverage Matrix

Two Rust-shell features the C++-era gap list assumed are permanently **removed**, not gaps: the Prometheus `/metrics` scrape (OTLP replaces it) and the Lua `/api/cache` admin endpoint (the route is unregistered and the Lua `cache` table bindings were never carried over). SSL/TLS is likewise **N/A** — TLS terminates at Traefik in front of the shell; `sslcert`/`sslkey` parse but are inert. `admin_upload.lua` / `/admin/upload` never existed in this repo.

| Feature | Status | Test Location | Notes |
|---|---|---|---|
| File upload (TIFF→JP2) | :white_check_mark: | `upload.rs` | |
| Upload knora.json | :white_check_mark: | `upload.rs` | |
| Upload JPEG with comment block | :white_check_mark: | `upload.rs` | |
| Video knora.json metadata | :white_check_mark: | `server.rs` | |
| Lua test_functions endpoint | :white_check_mark: | `server.rs` | |
| Lua mediatype endpoint | :white_check_mark: | `server.rs` | |
| Lua mimetype_func endpoint | :white_check_mark: | `server.rs` | |
| Lua knora_session_cookie | :white_check_mark: | `server.rs` | |
| Lua orientation endpoint | :white_check_mark: | `server.rs` | |
| Lua exif_gps endpoint | :white_check_mark: | `server.rs` | |
| Lua read_write endpoint | :white_check_mark: | `server.rs` | |
| SQLite API | :white_check_mark: | `server.rs::sqlite_api` | |
| Missing sidecar handling | :white_check_mark: | `server.rs::missing_sidecar_handled_gracefully` | |
| Concurrent request handling | :white_check_mark: (ignored) | `server.rs::concurrent_requests` | `#[ignore]`'d — the engine-pool semaphore sheds load (503) under concurrent bursts by design; the test itself is correct, revisit when the cluster-A permit-release work lands |
| File access allowed/denied | :white_check_mark: | `server.rs` | |
| Knora.json validation | :white_check_mark: | `server.rs::knora_json_image_required_fields`, `knora_json_image_dimensions` | |
| Upload edge cases | :white_check_mark: | `upload.rs` | |
| Video metadata extensions | :white_check_mark: | `server.rs::video_knora_json_checksums` | |
| Small-file range requests | :white_check_mark: | `range_requests.rs` | 7 tests |
| Large-file range requests (10MB+) | :white_check_mark: | `range_requests.rs` | 8 tests |
| Cache hit/miss verification | :white_check_mark: | `cache.rs::cache_hit_avoids_decode`, `head_does_not_warm_cache` | observed via on-disk cache-dir file count, not `/metrics` (removed) |
| CLI mode (file conversion) | :white_check_mark: | `test/e2e/tests/cli.rs` | `sipi convert <in> <out>` covered |
| Prometheus metrics endpoint | N/A — removed | — | OTLP replaces it on the Rust shell; there is no scrape endpoint |
| SSL/TLS endpoints | N/A — removed | — | TLS terminates at Traefik; the Rust shell serves plain HTTP only, flags parse-only |
| Image dimension verification | :white_check_mark: | `iiif_compliance.rs::region_dimension_verification`, `size_dimension_verification`, `rotation_dimension_verification` | decodes output and asserts actual pixel dimensions |
| EXIF preservation through IIIF pipeline | :white_check_mark: | `iiif_compliance.rs::metadata_iiif_pipeline`; `server.rs::lua_exif_gps` | HTTP-level; `server.rs::restricted_output_rewrites_dimension_metadata` diffs every `Exif.*` line (via `sipi query`) against the unrestricted output |
| XMP preservation through IIIF pipeline | :white_check_mark: | `server.rs::restricted_output_keeps_rights_metadata_verbatim`; unit `formats_test` `JpegMetadataSegments.*` | XMP `dc:rights` and EXIF `Copyright` of `CIELab16.tif` survive byte for byte in jpg/png/tif/jp2 outputs; `cli_json.rs` covers malformed XMP; a jpg XMP packet over one segment is split into Extended XMP with the rights in the standard packet, an EXIF over one segment loses only its previews; a block that still does not fit (EXIF, unsplittable XMP) is left out with a warning and the JPEG is delivered |
| ICC profile preservation/conversion | :x: GAP | — | C++ unit tests exist but no HTTP-level round-trip test |
| IPTC metadata preservation | :white_check_mark: | `heritage_jpeg.rs`; `server.rs::every_output_format_keeps_the_iptc_metadata` | resilient parse of heritage APP13/IPTC; `CIELab16.tif`'s `Iptc.*` lines (Copyright included) equal the source's in jpg/png/tif/jp2 outputs, restricted and unrestricted; IPTC over one segment continues in further APP13 segments (unit `JpegMetadataSegments.LargeIptcContinuesInFurtherApp13Segments`) |
| Essentials round-trip | :white_check_mark: | `upload.rs::metadata_essentials_roundtrip` | |
| CLI conversion metadata fidelity | :white_check_mark: | `cli.rs::cli_metadata_fidelity` | |
| MIME consistency check (`/api/mimetest`) | :white_check_mark: | `server.rs::mime_consistency` | all 6 cases ported |
| Thumbnail generation (`/make_thumbnail`) | :white_check_mark: | `server.rs::thumbnail_generation`, `thumbnail_convert_from_file` | |
| Convert from binaries (`/convert_from_binaries`) | :white_check_mark: | `server.rs::image_conversion_from_binaries` | |
| Temp directory cleanup | :white_check_mark: | `server.rs::temp_directory_cleanup` | |
| Restricted image size reduction | :white_check_mark: | `server.rs::restricted_image_reduction` | |
| Restricted image as its own image (RW, RH) | :white_check_mark: | `dsp_api_closure.rs::restricted_info_json_states_the_restricted_dims_as_its_maximum`, `restricted_info_json_dims_equal_full_max`, `restricted_tiles_at_size_max_stitch_to_full_max`, `restricted_left_half_region_is_the_left_half_of_full_max`, `restricted_canonical_link_is_in_restricted_coordinates`, `permission_code_1_restricts_the_size_of_a_region_request`; unit `//src/ffi:serve_image_test` (`RestrictedImage.*`, `Sizes/RestrictedSize.*`) | ADR-0028: region/size resolve against (RW, RH), exact `full/max` on non-dividing sizes, tiles stitch, pre-rotation (RW, RH) bound, passthrough guard, `/r<RW>x<RH>` cache-key marker; `info.rs::size_restricted_image_states_its_max_size` |
| `full/max` on unrestricted images is native (DEV-6118) | :white_check_mark: | `iiif_compliance.rs::full_max_on_unrestricted_images_is_the_native_size` | JP2 and TIFF, `max` and `^max` |
| Restriction refusals and the box size | :white_check_mark: | `security.rs::no_op_restrictions_are_refused_on_every_route`, `restricted_knora_json_dims_match_info_json`, `image_inside_restriction_box_is_served_at_native_size` | A size that never reduces (`max`, `pct:100`, empty watermark) is refused on the image, info.json and knora.json; knora.json dims equal info.json's; an image inside a box size is served at native size |
| JP2 component subsampling | :white_check_mark: | `server.rs::subsampled_jp2_serves_full_image`; `cli_conversions.rs` `jp2_decode_file3`; unit `//src/format_handlers:formats_test` (`J2kSubsampling.*`, `J2kSubsamplingRegion.*`) | ISO 15444-4 `file3.jp2` decodes full, reduced and by region against the reference; unsupported factors fail fast |
| JP2 channel order from the `cdef` mapping | :white_check_mark: | `cli_conversions.rs` `jp2_decode_file2`; unit `//src/format_handlers:formats_test` (`J2kChannelOrder.*`) | YCbCr converted in Y/Cb/Cr order, with and without a reversing `cdef` |
| Writing over a larger existing file | :white_check_mark: | unit `//src/format_handlers:formats_test` (`FormatOverwrite.*`, `JpegWrite.OverwritingLargerFileTruncatesIt`) | No stale trailing bytes in any format |
| EXIF `addKeyVal` replaces an existing value | :white_check_mark: | unit `//src/metadata:exif_rational_test` (`ExifAddKeyVal.*`) | Tag/group overloads; an unsupported type throws and keeps the value |
| Size-restricted output dimension metadata | :white_check_mark: | `server.rs::restricted_output_rewrites_dimension_metadata`; `serve_image_test` `RestrictedImage.MetadataDropsPreviewsAndStatesTheDeliveredSize`, `RestrictedImage.ExifImageResourcesDropPreviewsAndStateTheDeliveredSize`; unit `//src/metadata:pixel_dimensions_test` | EXIF and XMP size fields state the delivered size, no EXIF thumbnail, maker-note preview, XMP thumbnail or Photoshop thumbnail resource (crafted source), every other EXIF/IPTC line equal to the unrestricted output |
| 4-bit palette PNG upload | :white_check_mark: | `upload.rs` | `upload_4bit_palette_png` |
| Cache API routes (`/api/cache`) | N/A — removed | — | route unregistered, Lua `cache` table bindings absent from `src/` |
| Favicon endpoint | :x: GAP | — | favicon route exists (`src/server/rust/src/lib.rs`); its only e2e test was removed with the differential suite |
| Memory safety (ASan/LSan) | :white_check_mark: | `ci.yml`'s `sanitizer` job | unit + e2e suites against an ASan+UBSan-instrumented binary (see Cross-Cutting section below) |
| Thread safety (TSan) | :x: GAP | — | Untested for data races; future optional nightly variant |
| Performance regression detection | :white_check_mark: | `latency.rs` | smoke thresholds only (info.json / cache-miss / cache-hit); load-baseline tier intentionally deferred to staging (see Cross-Cutting section) |
| Corrupt/truncated image handling | :white_check_mark: | `iiif_compliance.rs::corrupt_jpeg_handling`, `corrupt_png_handling`, `corrupt_image_handling` (truncated JP2) | JP2 decode converts Kakadu's thrown error into a `SipiImageError` (like JPEG/PNG); the server returns a clean error and stays up (DEV-6731) |
| Lua route handler errors | :white_check_mark: | `server.rs::lua_route_error_handling` | |
| Zero-byte / empty file upload | :white_check_mark: | `upload.rs::empty_file_upload` | |
| Invalid server config startup | :white_check_mark: | `config.rs::invalid_config_startup` | |
| Double-encoded URL handling | :white_check_mark: | `iiif_compliance.rs::double_encoded_url` | `%252F`: the Rust shell single-decodes (DEV-6700) |
| Extremely long URL / header | :x: GAP | `//src/iiifparser/fuzz:parse_request_fuzz` (URL half only) | URL half closed: libFuzzer mutates URI length without bound (nightly, Linux) and the corpus replay runs on every PR. The header half is still untested — the fuzz target takes the URI, not HTTP headers |
| JWT validation edge cases | :white_check_mark: | `security.rs::jwt_expired_token`, `jwt_alg_none_bypass`, `jwt_tampered_payload` | the preflight response-sink crash (DEV-6670) is fixed — a Bearer token to an `/auth`-prefixed image no longer null-derefs |
| Image decompression bomb | :white_check_mark: | `security.rs::decompression_bomb_rejection` | |
| Upload size enforcement | :white_check_mark: | `upload.rs::upload_size_enforcement` | |
| CRLF header injection | :white_check_mark: | `security.rs::crlf_header_injection`; `input_validation.rs::crlf_in_identifier_no_header_injection` | |
| Cache key collision | :white_check_mark: | `cache.rs::cache_key_isolation` | |
| Error message information disclosure | :white_check_mark: | `security.rs::error_no_path_disclosure`; `input_validation.rs::path_traversal_no_content_leaked` | |
| Slowloris / connection exhaustion | :white_check_mark: (ignored) | `security.rs::slowloris_resilience` | `#[ignore]`'d for CI timing flakiness, not a coverage gap |
| `parseSizeString` edge cases | :white_check_mark: | `config.rs::parse_size_string_edge_cases` | |
| Deprecated config key migration | :white_check_mark: | `config.rs::config_deprecated_key_migration` | |
| CLI argument overrides | :white_check_mark: | `config.rs::config_cli_overrides` | |
| Empty jwt_secret behavior | :white_check_mark: | `security.rs::config_empty_jwt_secret` | |
| Invalid Lua config syntax | :white_check_mark: | `config.rs::invalid_config_startup` | |
| Config with nonexistent paths | :white_check_mark: | `config.rs::config_nonexistent_paths` | |
| SImage Lua API coverage | :x: GAP | — | Methods tested only via black-box HTTP, not a direct Lua-API unit harness |
| Lua JWT round-trip | :white_check_mark: | `server.rs::lua_jwt_round_trip` | |
| Lua UUID round-trip | :white_check_mark: | `server.rs::lua_uuid_round_trip` | non-deterministic body |
| Lua `server.http` outbound | :white_check_mark: | `server.rs::lua_http_client_error_handling` | |
| Lua error propagation to HTTP | :white_check_mark: | `server.rs::lua_route_error_handling` | |
| HTTP keep-alive | :white_check_mark: | `connection.rs::http_keep_alive` | |
| Chunked transfer encoding | :white_check_mark: | `connection.rs::chunked_transfer_upload` | |
| Connection: close header | :white_check_mark: | `connection.rs::connection_close_header` | |
| Thread pool exhaustion | :x: GAP | — | 503 load-shedding exists (semaphore-backed pool) but has no dedicated saturation test; masked today by `flaky_test_attempts` on suite-load bursts |
| Graceful shutdown | :white_check_mark: | `connection.rs::graceful_shutdown`; `shutdown.rs` (×3) | |
| Multi-page TIFF `@page` e2e | :x: GAP (documented) | `iiif_compliance.rs::multipage_tiff_page_selection` | `#[ignore]`'d — page selection may not be implemented; test documents current behavior |
| CMYK→sRGB through IIIF pipeline | :white_check_mark: | `iiif_compliance.rs::cmyk_through_iiif_pipeline` | |
| CIELab through IIIF pipeline | :white_check_mark: | `iiif_compliance.rs::cielab_through_iiif_pipeline` | |
| 16-bit depth through IIIF pipeline | :white_check_mark: | `iiif_compliance.rs::bit16_through_iiif_pipeline` | |
| Progressive JPEG handling | :white_check_mark: | `iiif_compliance.rs::progressive_jpeg_input` | |
| TIFF with JPEG compression | :white_check_mark: | `iiif_compliance.rs::tiff_jpeg_compression_input` | documents current (non-crashing) behavior for the known scanline-order edge case |
| 1-bit TIFF (bi-level) | :white_check_mark: | unit + rust-e2e (`bilevel_tiff.rs`) | MINISWHITE and MINISBLACK, LZW and uncompressed |
| JPEG YCCK colorspace | :white_check_mark: | unit | Was throw; now decoded via CMYK path |
| JPEG CMYK with APP14 (Photoshop) — inverted before ICC | :white_check_mark: | unit | regression test |
| JPEG CMYK without APP14 (raw) — not inverted | :white_check_mark: | unit | regression test (negative case) |
| JPEG with APP13 before APP1 + non-ASCII IPTC | :white_check_mark: | unit + `heritage_jpeg.rs` | heritage collection regression |
| CLI `--json` output contract | :white_check_mark: | unit + rust-e2e (`cli_json.rs`) | success + error payloads, single-document stdout |
| CLI `query` / `compare` / `verify` subcommands | :white_check_mark: | `cli.rs::cli_query_dumps_image_info`, `cli_compare_identical_files_reports_match`, `cli_compare_differing_files_reports_mismatch`, `cli_verify_pipeline_service_and_access_files` | the verify pipeline test exercises `convert service-file` → `verify service-file` → `convert access-file` → `verify access-file` end to end |
| Watermark application via HTTP | :white_check_mark: | `server.rs::watermark_applied_via_http` | |
| Restrict + watermark combined | :white_check_mark: | `server.rs::restrict_plus_watermark` | |
| Watermark cache key separation | :white_check_mark: | `cache.rs::watermark_cache_separation` | |
| CLI watermark mode | :white_check_mark: | `cli.rs::cli_convert_watermark_changes_output_bytes` | asserts a valid JPEG whose bytes differ from a plain convert of the same input |
| Concurrent cache writes (same key) | :white_check_mark: | `cache.rs::cache_returns_consistent_results` | |
| Cache eviction during active reads | :white_check_mark: | `cache.rs::cache_eviction_during_read` | |
| Concurrent file uploads | :white_check_mark: | `upload.rs::concurrent_file_uploads` | |
| Lua state thread isolation | :white_check_mark: (ignored) | `server.rs::lua_state_thread_isolation` | `#[ignore]`'d — same engine-pool 503-shedding reason as `concurrent_requests`, not a coverage gap; the test itself asserts per-thread Lua global isolation via `/test_thread_isolation` |
| Cache disabled mode (`cache_size=0`) | :white_check_mark: | `cache.rs::cache_disabled_mode` | |
| Cache LRU purge under size limit | :white_check_mark: | `cache.rs::cache_lru_purge_correctness` | |
| Cache nfiles limit enforcement | :white_check_mark: | `cache.rs::cache_nfiles_limit` | |
| Keep-alive timeout enforcement | :white_check_mark: | `connection.rs::keepalive_timeout_enforcement` | |
| Sustained load memory growth | :white_check_mark: | `resource_limits.rs::sustained_load_memory_growth` | |
| Concurrent large image decode memory | :white_check_mark: | `resource_limits.rs::concurrent_large_image_decode` | |
| Full-lane budget rejection (advanced 413, tile bypass, in-budget, basic) | :white_check_mark: | `admission_control.rs` (×4, each on its own cache dir) | Transient 503 is engine-unit-covered (`MemoryBudgetTest`), not e2e — a live-server 503 is a decode-timing race |
| Two-lane admission (thread cap, tile floor/burst, per-partition shed, classifier) | :white_check_mark: | `//src/throttling/rust:admission` crate tests (engine-free) | |
| Intermediate buffer accumulation | :white_check_mark: | `resource_limits.rs::transform_pipeline_memory` | |
| Cache as memory pressure relief | :white_check_mark: | `cache.rs::cache_hit_avoids_decode` | |

## Gap Summary

| Category | Covered | Gaps | Coverage |
|---|---|---|---|
| Info.json fields | 23 | 0 | 100% |
| Region parameters | 12 | 0 | 100% |
| Size parameters | 15 | 0 | 100% |
| Rotation parameters | 9 | 0 | 100% |
| Quality parameters | 6 | 0 | 100% |
| Format parameters | 5 | 0 | 100% |
| CORS | 13 | 0 | 100% |
| HTTP behavior | 13 | 0 | 100% |
| Identifiers | 4 | 1 (ARK/URN) | 80% |
| Vitrinli extensions | 100 | 7 | 93% |
| **Total** | **200** | **8** | **96%** |

*(Three additional Vitrinli-extension rows — Prometheus `/metrics`, `/api/cache`, SSL/TLS — are excluded from this count entirely: they are removed-on-the-Rust-shell features, not gaps. See the N/A note above the extension matrix.)*

**Key remaining gap categories** (7 rows, all in Vitrinli extensions):

- **Deep metadata survival** (1 gap): ICC profile HTTP-level round-trip — EXIF/IPTC/XMP now covered
- **Concurrency edge cases** (1 gap): thread-pool-exhaustion saturation test (`concurrent_requests`/`lua_state_thread_isolation` are written and correct, just `#[ignore]`'d for the unrelated 503-shedding reason above — not gaps)
- **Hardening depth** (2 gaps): TSan (nightly-future), extremely long URL/header (URL half now covered by the nightly libFuzzer target; the header half is untested)
- **Test-harness depth** (1 gap): a direct SImage Lua-API unit harness (vs. black-box HTTP coverage)
- **Test removed with the differential suite** (1 gap): favicon endpoint — the route exists but its only e2e test lived in the removed differential suite
- **Documented, not missing** (1 row): multi-page TIFF `@page` — `#[ignore]`'d pending a real page-selection implementation, not an untested behavior

## Cross-Cutting: Memory Safety (Sanitizer Builds)

Memory leaks and undefined behavior are not a separate pyramid layer but a **build variant** that runs existing tests with compiler instrumentation. This is critical for sipi's long-running C++ image engine, where leaks accumulate over the process lifetime.

**Current state:** ASan+UBSan infrastructure is in place — Bazel `--config=asan` and `--config=ubsan` blocks in `.bazelrc`, `just bazel-build-sanitized` (`bazel build --config=asan --config=ubsan //src/cli:sipi`), and one path-gated `sanitizer` job in `ci.yml` that exercises the unit and e2e suites against the instrumented binary in a single `just bazel-test-sanitized` invocation. Known findings to triage on first run:

- **`SipiFilenameHash::operator=` memory leak** — `operator=` allocates `new vector<char>` without deleting the old `hash` pointer. Confirmed by code inspection. Fix: add `delete hash;` before the new allocation, or switch to `std::unique_ptr`.
- **Potential: `SipiFilenameHash` copy constructor** — also `new`s without freeing, but only leaks if the destination object was previously constructed with a different hash (doesn't happen via typical usage).
- **Expected: false positives from external libraries** — exiv2, lcms2, and other vendored libraries may trigger ASan warnings that aren't sipi bugs. These should be suppressed via an ASan suppression file if needed.

**Sanitizer stack:**

| Sanitizer | Catches | Flag | Overhead |
|---|---|---|---|
| ASan (AddressSanitizer) | Buffer overflow, use-after-free, double-free, leaks | `-fsanitize=address` | ~2x |
| UBSan (UndefinedBehaviorSanitizer) | Integer overflow, null deref, misaligned access | `-fsanitize=undefined` | ~1.2x |
| TSan (ThreadSanitizer) | Data races, deadlocks | `-fsanitize=thread` | ~5-15x |

**Infrastructure:**

| Component | Status | Details |
|-----------|--------|---------|
| `--config=asan` / `--config=ubsan` in `.bazelrc` | Done | `-fsanitize=address` / `-fsanitize=undefined` plus `-fno-omit-frame-pointer`, `-fno-optimize-sibling-calls`, `--strip=never`, `--compilation_mode=dbg` (DWARF inline so `.lsan_suppressions.txt` symbol-name suppressions match) |
| `just bazel-build-sanitized` | Done | Wraps `bazel build --config=asan --config=ubsan //src/cli:sipi` |
| PR CI (`ci.yml`'s `sanitizer` job) | Done | Bazel-built binary at `bazel-bin/src/cli/sipi`; runs `just bazel-test-sanitized` (unit + e2e in one invocation) `--config=asan --config=ubsan`; `.lsan_suppressions.txt` consumed by LSan via `LSAN_OPTIONS` |
| TSan variant | Future | Optional nightly, separate from ASan (can't combine) |

**Strategy:** PR CI runs both the unit and e2e suites against an ASan+UBSan-instrumented binary, as two parallel jobs gated on native/build-relevant path changes. TSan remains a future option.

## Cross-Cutting: Performance Regression Detection

**Current state:** OTLP-exported metrics include cache counters/gauges and the semantic-convention `http.server.request.duration` histogram (5ms–10s buckets), which normalizes to `http_server_request_duration_seconds` at the collector. CI infrastructure includes smoke latency assertions in PR CI. Throughput-style load testing is intentionally **not** run in CI — synthetic `wrk` against three IIIF endpoints on a shared GitHub runner does not mirror dasch-prod-01's actual workload (cold-cache reads of medium-sized JP2s under spiky concurrency) and the prior nightly `loadtest.yml` artifacts were not consulted. Meaningful load testing belongs in a staging environment with realistic fixtures and traffic shape.

**Strategy (three tiers):**

| Tier | What | Tool | When |
|---|---|---|---|
| Smoke latency | Assert response time < threshold in e2e tests | Rust `Instant::now()` | PR CI |
| Load baseline | Throughput against realistic workload | `wrk` / `k6` | **Future** — staging environment, not CI |
| Component microbenchmarks | Parse / decode / process / encode tiers, before/after comparisons for hot-path PRs | Google Benchmark (`just bench`) | **Done** — local dev loop, never a CI gate; see [Benchmarking](benchmarking.md) |

**Smoke latency thresholds (proposed):**

- Info.json request: < 50ms
- 512x512 JPEG delivery (cache miss): < 500ms
- 512x512 JPEG delivery (cache hit): < 100ms
- These catch gross regressions (10x slowdown), not subtle changes

## Snapshot Review Workflow

Vitrinli uses [insta](https://insta.rs/) for golden baseline snapshots. When a snapshot changes:

1. Run tests: `just bazel-test-e2e` (pending snapshots are written as
   `*.snap.new` next to the existing `*.snap`)
2. Review the diff between each `.snap` and `.snap.new` (a regular
   `git diff` works, or open both in your editor)
3. For intentional changes, replace: `mv path/to/foo.snap.new path/to/foo.snap`
4. For regressions, delete the `.snap.new` and fix the code
5. Commit updated `.snap` files

For interactive review, install `cargo-insta` ad-hoc — it operates on
`.snap.new` files and doesn't need the dev-shell rust toolchain:

```bash
nix shell nixpkgs#cargo-insta --command cargo insta review
```

**When to use insta:**

- info.json response structure
- HTTP response header sets
- knora.json response structure
- Image metadata fingerprints

**Pattern:** Use `insta::assert_json_snapshot!` with `redact` for dynamic values:

```rust
insta::assert_json_snapshot!(info_json, {
    ".id" => "[base_uri]",
});
```

## CI Integration

| Target | Just Recipe | When | Notes |
|---|---|---|---|
| C++ unit tests (CI) | `just nix-build` (`.#dev` checkPhase) | PR CI | GoogleTest via ctest, runs in the Nix sandbox |
| C++ unit tests (Bazel) | `just bazel-test-unit` | local + CI | `bazel test //test/unit/...`. CI runs them via `just bazel-coverage`. |
| C++ approval tests (CI) | `just nix-build` (`.#dev` checkPhase) | PR CI | ApprovalTests via ctest; `SOURCE_DATE_EPOCH=946684800` injected by CMake |
| C++ approval tests (Bazel) | `just bazel-test-approval` | local + CI | `bazel test //test/approval:approvaltests`; env injection via `BUILD.bazel`. CI runs them via `just bazel-coverage`. |
| Rust e2e tests (CI) | `just nix-test-e2e` | PR CI | Pre-built binaries from `.#e2e-tests` (crane); reads `$SIPI_BIN` |
| Rust e2e tests (inner-loop) | `bazel test //test/e2e:<name>` | local | Same hermetic toolchain as CI |
| Docker smoke (CI) | `just bazel-test-smoke` | PR + tag CI | Bazel-built OCI tarball, `docker load`ed by the test |
| Differential parity | *(retired)* | — | The strangler parity gate and the retained C++ oracle server it compared against have been removed; the Rust shell is the sole production surface |
| Hurl contract tests | *(retired)* | — | Folded into Rust e2e (`tests/http_contracts.rs` + `iiif_compliance.rs`) |
| Python e2e tests | *(retired)* | — | Replaced by Rust e2e tests |
| Fuzz corpus replay | `just bazel-test` / `bazel-test-unit` / `bazel-test-sanitized` / `bazel-coverage` | PR CI, all platforms | `//src/iiifparser/fuzz:parse_request_fuzz` and `//src/format_handlers/fuzz:{tiff,jpeg,png,j2k}_decode_fuzz` under `rules_fuzzing`'s default `replay` engine; picked up by the `//src/...` wildcard |
| Fuzz mutation loop | `just bazel-build-fuzz` + the runner-local loop (`just fuzz` locally) | nightly (`fuzz.yml`), Linux only | libFuzzer + SanitizerCoverage on the Rust parser via the `//src/iiifparser/fuzz` shim and on the C++ codec handlers via `//src/format_handlers/fuzz`; see [Fuzzing](fuzzing.md) |
| Sanitizer builds | `just bazel-build-sanitized` (`bazel build --config=asan --config=ubsan //src/cli:sipi`) | PR | ASan+UBSan; e2e suite against `bazel-bin/src/cli/sipi` with `.lsan_suppressions.txt` |

## Python Test Deprecation — Parity Checklist

Python e2e tests (`test/e2e/`) have been retired. The following per-function parity checklist confirmed Rust coverage before removal.

### test_01_conversions.py (2 tests) — RETIRED

| Python Test | Rust Equivalent | Notes |
|---|---|---|
| `test_iso_15444_4_decode_jp2` | `cli_file_conversion` in `cli.rs` | JP2→TIFF decode with PAE comparison |
| `test_iso_15444_4_round_trip` | `cli_file_conversion` in `cli.rs` | TIFF→JP2→TIFF round-trip |

### test_02_server.py (32 tests) — RETIRED

| Python Test | Rust Equivalent | Notes |
|---|---|---|
| `test_sipi_starts` | `server_starts_and_responds` in `smoke.rs` | |
| `test_sipi_log_output` | — | Dropped: infrastructure check ("Added route" in stdout), not behavioral |
| `test_lua_functions` | `lua_test_functions` in `server.rs` | |
| `test_clean_temp_dir` | `temp_directory_cleanup` in `server.rs` | |
| `test_lua_scripts` | `lua_mediatype` in `server.rs` | |
| `test_lua_mimetype` | `lua_mimetype_func` in `server.rs` | |
| `test_knora_session_parsing` | `lua_knora_session_cookie` in `server.rs` | |
| `test_file_bytes` | `full_iiif_url_returns_image` in `iiif_compliance.rs` | Status+content-type check; byte-exact comparison covered by dimension verification and snapshot tests |
| `test_restrict` | `restricted_image_reduction` in `server.rs` | Verifies 128x128 via image decode |
| `test_deny` | `deny_unauthorized_image` in `iiif_compliance.rs` | |
| `test_not_found` | `returns_404_for_missing_file` in `smoke.rs` | |
| `test_iiif_url_parsing` | `invalid_iiif_url_empty_identifier`, `id_incomplete_iiif_url`, `id_malformed_iiif_url` in `iiif_compliance.rs` | Multiple tests cover all 5 invalid URL patterns |
| `test_read_write` | `lua_read_write` in `server.rs` | |
| `test_jpg_with_comment` | `upload_jpeg_with_comment_block` in `upload.rs` | |
| `test_odd_file` | `upload_odd_file` in `upload.rs` | |
| `test_head_response_should_be_empty` | `head_iiif_image_empty_body` in `iiif_compliance.rs` + `head_request_returns_headers` in `smoke.rs` | |
| `test_mimeconsistency` | `mime_consistency` in `server.rs` | All 6 test cases ported |
| `test_thumbnail` | `thumbnail_generation` + `thumbnail_convert_from_file` in `server.rs` | |
| `test_image_conversion` | `image_conversion_from_binaries` in `server.rs` | `/convert_from_binaries` endpoint |
| `test_knora_info_validation` | `knora_json_image_required_fields` + `upload_tiff_knora_json` in `upload.rs` | Image and CSV sidecar flows |
| `test_json_info_validation` | Info.json tests in `iiif_compliance.rs` + `info_json_x_forwarded_proto_https` | Full structure + X-Forwarded-Proto |
| `test_knora_json_for_video` | `video_knora_json` in `server.rs` | |
| `test_handling_of_missing_sidecar_file_for_video` | `missing_sidecar_handled_gracefully` in `server.rs` | |
| `test_sqlite_api` | `sqlite_api` in `server.rs` | |
| `test_iiif_auth_api` | `iiif_auth_api` in `server.rs` | 401 + IIIF Auth service block in info.json |
| `test_orientation_topleft` | `lua_orientation` in `server.rs` | Lua endpoint tests same code path |
| `test_4bit_palette_png` | `upload_4bit_palette_png` in `upload.rs` | |
| `test_upscaling_server` | `size_upscaling` + `size_upscale_*` in `iiif_compliance.rs` | Status + dimension verification |
| `test_file_access` | `file_access_allowed` + `file_access_denied` in `server.rs` | |
| `test_concurrency` | `concurrent_requests` in `server.rs` | |
| `test_orientation` | `lua_orientation` in `server.rs` | |
| `test_exif_gps` | `lua_exif_gps` in `server.rs` | |

### test_03_iiif.py (1 test) — RETIRED

| Python Test | Rust Equivalent | Notes |
|---|---|---|
| `test_iiif_validation` | — | Explicitly excluded: calls external `iiif-validate.py` binary, effectively a no-op when validator unavailable. IIIF compliance covered by 80+ tests in `iiif_compliance.rs` |

### test_04_range_requests.py (12 active tests) — RETIRED

| Python Test | Rust Equivalent | Notes |
|---|---|---|
| `test_small_file_no_range` | `small_file_no_range` in `range_requests.rs` | |
| `test_small_file_range_first_100_bytes` | `small_file_range_first_100_bytes` in `range_requests.rs` | |
| `test_small_file_range_middle_bytes` | `small_file_range_middle_bytes` in `range_requests.rs` | |
| `test_small_file_range_last_byte` | `small_file_range_last_byte` in `range_requests.rs` | |
| `test_small_file_open_ended_from_start` | `small_file_open_ended_from_start` in `range_requests.rs` | |
| `test_small_file_open_ended_from_middle` | `small_file_open_ended_from_middle` in `range_requests.rs` | |
| `test_large_file_no_range` | `large_file_full_download` in `range_requests.rs` | |
| `test_large_file_range_first_megabyte` | `large_file_range_first_1mb` in `range_requests.rs` | |
| `test_large_file_range_middle_chunk` | `large_file_range_middle_chunk` in `range_requests.rs` | |
| `test_large_file_range_last_chunk` | `large_file_range_last_bytes` in `range_requests.rs` | Last 1000 bytes |
| `test_large_file_range_single_last_byte` | `large_file_range_single_last_byte` in `range_requests.rs` | |
| `test_large_file_open_ended_from_start` | `large_file_open_ended_from_start` in `range_requests.rs` | |
| `test_large_file_open_ended_from_middle` | `large_file_open_ended_from_middle` in `range_requests.rs` | |
| `test_large_file_multiple_ranges_simulation` | `large_file_sequential_range_reassembly` in `range_requests.rs` | |

### Infrastructure files — RETIRED

| File | Notes |
|---|---|
| `conftest.py` | Test manager, fixtures, nginx control — replaced by `test/e2e/tests/common/` |
| `config.ini` | Python test config — replaced by Rust test harness |
| `nginx/` | Nginx reverse proxy for SSL testing — Rust tests use direct HTTPS |
| `requirements.txt` | Not present (deps managed by Nix/pip) |

## Rust Migration Testing Path

When a C++ component is migrated to Rust:

1. **Before migration:** Ensure e2e contract tests cover the component's behavior
2. **During migration:** Write Rust unit tests (`#[test]`, `proptest`) for the new implementation
3. **After migration:** Existing e2e tests validate the Rust implementation matches C++ behavior
4. **Cleanup:** Remove corresponding C++ unit tests (they tested the old implementation)

The `insta` golden baselines are critical — they capture the Rust shell's exact response output and detect any implementation drift.

## Flaky Test Handling

Some e2e tests are inherently racy — for example, a test that uploads a file and immediately GETs the converted result may fail if the server hasn't flushed to disk yet. Rather than retrying at the CI job level, handle flakiness at the **test level** using the `retry_flaky()` helper from the test harness (`test/e2e/src/lib.rs`):

```rust
use sipi_e2e::retry_flaky;

#[test]
fn my_flaky_test() {
    let srv = server();
    // ... setup ...

    retry_flaky(3, || {
        match client().get(&url).send() {
            Ok(resp) if resp.status().as_u16() == 200 => Ok(()),
            Ok(resp) => Err(format!("HTTP {}", resp.status())),
            Err(e) => Err(format!("{}", e)),
        }
    });
}
```

**Guidelines:**

- `retry_flaky(max_attempts, closure)` retries the closure up to `max_attempts` times with a 2-second sleep between attempts
- The closure returns `Ok(())` on success or `Err(message)` on failure
- Failed attempts emit `[retry_flaky]` log lines for CI visibility
- Only use for tests with a known race condition — do not mask real bugs with retries
- If a test needs more than 3 retries, the underlying issue should be fixed instead

## Future Additions

- **Doc tests:** Once sipi has Rust library code (post-migration), `///` example doc tests become valuable

(Component microbenchmarks — formerly listed here as a post-Rust-migration
`criterion` aspiration — exist today as the C++ Google Benchmark suite; see
[Benchmarking](benchmarking.md). If/when the codec migrates to Rust, the
discipline carries over to `criterion` unchanged.)
