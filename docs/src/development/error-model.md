# Error Model

The image/codec layer's failure contract: what type carries an error, what
policy each failure maps to at the three seams that consume it, and which
exception mechanisms remain in permanent, deliberate use. The decision this
document reflects is [ADR-0024](../../adr/0024-value-based-image-errors.md);
this page is the living catalog of the contract as it stands, not the
decision record itself.

## The value type: `Sipi::SipiValueError`

`src/error/cpp/SipiValueError.h` (`//src/error`) defines:

- `ErrorCode` — an `enum class`, one variant per distinct failure category a
  caller can act on (decode rejection, unsupported format, malformed/oversized
  input, encode/write failure, client-abort, shape-probe failure,
  metadata-parse failure, invalid request parameter). New failure categories
  get a new variant — never folded into an existing one.
- `SipiValueError` — the value itself: an `ErrorCode`, a diagnostic message, and
  a `std::source_location` captured at construction. Cheap to move; the
  diagnostic string is not eagerly formatted.
- `client_message()` — path-redacted, no source location. Safe to surface in an
  HTTP response body or a Lua-visible error string.
- `diagnostic_message()` — full detail (source file + line), for server-side
  logs and Sentry.
- `Result<T>` — `template <typename T> using Result = std::expected<T,
  SipiValueError>;`, defined in the same header.

## Policy as data: `policy_for(ErrorCode)`

A `constexpr ErrorPolicy policy_for(ErrorCode)` returns
`{ HttpStatusClass, SentryPolicy, MetricHint }` for a given code. This is the
single source of truth for a `SipiValueError`'s failure policy — no seam
dispatches on `ErrorCode` with its own scattered `catch`-equivalent `switch`.
Each seam maps the returned `HttpStatusClass` to its own vocabulary — a
concrete `SipiStatus` on the HTTP seam, an emitted error string on the Lua
seam, an exit code on the CLI seam.

| `ErrorCode` | `HttpStatusClass` | `SentryPolicy` | `MetricHint` |
|---|---|---|---|
| `kDecodeFailed` | `kInternalError` | `kReport` (+ `ImageContext`) | none |
| `kUnsupportedFormat` | `kInternalError` | `kReport` | none |
| `kMalformedInput` | `kInternalError` | `kReport` | none |
| `kWriteFailed` | `kInternalError` | `kReport` (+ `ImageContext`) | none |
| `kClientAbort` | `kInternalError` | `kSkip` | `kClientDisconnected` |
| `kShapeProbeFailed` | `kInternalError` | `kReport` | none |
| `kMetadataParseFailed` | `kInternalError` | `kReport` | none |
| `kInvalidRequestParameter` | `kClientError` | `kSkip` | none |

`kInvalidRequestParameter` is the one `ErrorCode` whose `HttpStatusClass` is
`kClientError` rather than `kInternalError`: it marks a client-supplied IIIF
parameter the engine cannot honour (for example, a requested scale target of
one sample or fewer on an otherwise well-formed source image), as distinct
from `kMalformedInput`, which stays `kInternalError` because it flags a
problem with the repository's own content (a corrupt or degenerate source
file) rather than with the request.

`std::bad_alloc` is deliberately not an `ErrorCode` and never appears in this
table — it stays exception-based permanently (ADR-0024 Decision 7). It has no
`MetricHint` to key on, since it never carries an `ErrorCode`: every seam maps
it to `HttpStatusClass::kInternalError` and increments the
`memory_alloc_failures_total` metric (`src/observability/cpp/metrics.h`)
directly at the `catch` site, outside `policy_for`.

## Embedded metadata parse failures are fatal on read

Vitrinli is a repository: it must not admit corrupt material. Every embedded
metadata blob a decode path hands to `Exif::parse`, `Iptc::parse`, or
`Icc::parse` (JPEG's APP1/APP2/APP13 segments, J2K's UUID boxes and
`colr` box, PNG's `iCCP`/`zTXt` chunks, TIFF's IPTC/ICC tags) must parse
cleanly or the read fails with `kMetadataParseFailed` — there is no
partial-metadata success path. A handler that decoded past a malformed
blob with the metadata simply dropped was a pre-repository-era shortcut,
not a supported outcome. `Icc::createRGB` is a different operation — it
synthesizes an ICC profile from a file's colour tags rather than parsing
an embedded blob — but the same ground applies: colour tags that cannot
be built into a profile mean the file is corrupt, so synthesis failure
is fatal too. `Xmp` is stored verbatim (see `src/metadata/cpp/xmp.h`)
and has no parse step to fail.

There is no reader-level exception to this contract: malformed ICC,
IPTC, and EXIF — whether parsed from an embedded blob or synthesized by
`Icc::createRGB` from colour tags — are all fatal in every codec handler
(JPEG, J2K, PNG, TIFF), including PNG's "Raw profile type exif" text
chunk (`src/format_handlers/cpp/SipiIOPng.cpp`).

### EXIF rationals on TIFF write

`SipiIOTiff::writeExif` applies the same fatal-on-corruption stance to the
write path. libtiff converts an EXIF rational tag to a `float` before
storing it, so a scalar rational `n/0` with `n != 0` has no valid float
representation; Vitrinli treats it as corrupt metadata and rejects the write
with `kMetadataParseFailed` rather than silently dropping it. A scalar
`0/0`, by contrast, is EXIF's own "no value" convention (e.g. an
unmeasured `BrightnessValue`) and carries no value to reject — it is
skipped, and the tag is simply omitted from the output. The
`LensSpecification` tag (`EXIF_DT_RATIONAL_PTR`, a fixed four-rational
array) is written all-or-nothing: any `n/0` element rejects the whole
write, and otherwise any `0/0` element skips the whole tag, since
libtiff's fixed-count `TIFFSetField` call has no per-element "unknown"
encoding.

## Legacy mechanisms and their disposition

| Mechanism | Package | Status |
|---|---|---|
| `Sipi::SipiError` | `iiifparser` | Unchanged. Exception-based, out of scope for this migration — a different package with its own lifetime (ADR-0021). The seams keep catching it by type for HTTP-400 dispatch. |
| `Sipi::SipiSizeError` | `iiifparser` | Unchanged, same reasoning as `SipiError`. |
| `Sipi::SipiImageError` | `image` | No fallible operation throws it. Narrowed to the unrecoverable/invariant class: allocation-size overflow (`checked_buf_size_or_throw` in `SipiImage.cpp`, `SipiIOTiff.cpp`, `compose.cpp`), `memTiffOpen`'s raw `malloc` failures, `SipiImage`'s construction/geometry invariants, and the `getPixel`/`setPixel` accessors — all programming errors with no input path. |
| `Sipi::InfoError` (bare `enum`) | `image` | Deleted; superseded by the dedicated `ErrorCode` variant `kShapeProbeFailed`. |
| `std::bad_alloc` | n/a | Unchanged, permanent. See above. |

## The three seams

Each seam converts a fallible operation's `Result`/`SipiValueError` into its
own failure vocabulary. What each seam catches by exception type differs:

- **`src/ffi/cpp/serve_image.cpp`** (HTTP) — `HttpStatusClass` → `SipiStatus`;
  `SentryPolicy::kReport` populates an `ImageContext` and calls
  `report_image_error`; `MetricHint` increments the matching
  `Sipi::observability::Metrics` counter. Its own `try`/`catch` blocks vary by
  step: the image-read step catches `std::bad_alloc`, `SipiImageError`, and
  `Sipi::SipiSizeError`; the later rotate/quality-conversion steps catch
  `std::bad_alloc` and `Sipi::SipiError`; the watermark step catches
  `Sipi::SipiError` and `std::exception`. It also catches `iiifparser`'s
  `SipiError`/`SipiSizeError` elsewhere in the file (out of scope for this
  migration, ADR-0021) for its HTTP 400. None of these name `kdu_exception`.
- **`src/ffi/cpp/image_handle.cpp`** (Lua userdata surface, behind
  `src/scripting/rust/bindings/image.rs`) — `client_message()` is emitted as the
  Lua-visible error string; there is no HTTP status or Sentry report on this
  surface. `sipi_image_new` (the entry that calls `SipiImage::read`) hand-rolls
  its own three-stage catch, in order: `Sipi::SipiError`, then `std::exception`,
  then a bare `catch (...)`.
- **CLI offline verbs** (`src/cli/cpp/cli_app.cpp`,
  `src/cli/cpp/commands/convert_access_file.cpp`, `convert_service_file.cpp`,
  `verify.cpp`, `health.cpp`) — `diagnostic_message()` reaches the operator
  through the verb's own reporting path: `log_err` to stderr, plus the
  optional `--json` report on `sipi convert` (`emit_json_report`) or
  `report_error` on `convert access-file`. The exit code is a plain binary
  `EXIT_SUCCESS`/`EXIT_FAILURE` — a richer `HttpStatusClass`-derived exit-code
  mapping is not implemented today. Each verb body catches `SipiImageError`
  and/or `std::exception` around its own calls; none name `kdu_exception`.

None of the three seams' own `try`/`catch` blocks name Kakadu's
`kdu_exception` (an `int`-like type, not a `std::exception`) — and none need
to, because it is stopped further out by a bare `catch (...)` wall. Inside
`SipiIOJ2k::read`, only the source-open/`access_codestream`/`open_stream` call
and the `codestream.create` call are wrapped in `try`/`catch (kdu_exception&)`,
converting those two failures to a `Result`. Several later calls on the same
decode path are not inside any `try`: `codestream.access_siz()`,
`codestream.apply_input_restrictions(...)`, `codestream.get_dims(...)`, and
`jpx_layer.access_colour(0)`. A `kdu_exception` raised from any of those
escapes `SipiIOJ2k::read` uncaught by any type-specific handler in the call
chain above it, and is stopped only by the outer catch-all at each seam's
boundary: `sipi_guard`'s bare `catch (...)` at the `extern "C"` boundary
(`src/ffi/cpp/serve_response.h`) for the HTTP entries that go through
`sipi_serve_image`, `sipi_image_new`'s own bare `catch (...)` for the Lua
userdata surface, and `sipi_cli_main`'s bare `catch (...)` around
`CLI11_PARSE` for the CLI. This is why
`src/format_handlers/fuzz/codec_fuzz_harness.h` keeps a bare `catch (...)`
around its own `read` calls — it exercises the same decode path directly,
outside any seam.

## Error-variant principle

Enumerate failure variants per operation; never collapse distinct failures into
one generic code to avoid extending the `policy_for` table. A catch-all that
silently genericizes a new failure is the failure mode this model exists to
prevent.
