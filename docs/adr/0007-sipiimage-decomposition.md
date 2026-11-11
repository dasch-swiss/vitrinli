---
status: accepted
---

# `SipiImage` decomposition into `image/` + `image_processing/`

The `SipiImage` god-object (~2,526 lines of `.cpp` + `.hpp`, six distinct responsibility groups in one class) is decomposed into:

- **`src/image/`** — a pure value type (geometry, photometric, RAII pixel buffer, metadata composite) with ~15 public methods. No image-processing behaviour, no I/O facade, no HTTP integration.
- **`src/image_processing/`** — free functions over `const Image&` for crop, scale, rotate, colour conversion, channel ops, bit-depth reduction, dithering, watermark application, comparison, arithmetic.

Three concerns leave the class entirely:

- The static `io` registry (extension → format handler) — already split so that [`src/format_handlers/`](../../src/format_handlers/) (`//src/format_handlers`) *defines* it (`format_registry.cpp`) while `SipiImage` only *declares* it — stays split this way; `image` never gains a Bazel edge to `format_handlers`.
- The `shttps::Connection*` field disappears (replaced by `OutputSink::HttpSink` per ADR-0006).
- The `app14_transform` JPEG-specific marker field moves into the JPEG decode pipeline (consumed at decode, inverted before `Image` is "complete"; downstream sees standard CMYK).

We accept this for five reasons.

**1. Six responsibility groups in one class is the textbook god-object shape.** Image data, metadata, format I/O facade, image processing, HTTP integration, and arithmetic are conceptually separable; co-locating them costs every consumer (heavy header transitively-includes, broken encapsulation via the four format-handler `friend class` declarations, leaky JPEG-specific field in the universal type). The pixel-ownership RAII swap is already done — `pixels` is `std::vector<byte>` ([`SipiImage.h`](../../src/SipiImage.h)), not a raw owning pointer — so that argument no longer applies; what remains is the decomposition of the six responsibility groups themselves. The accumulation is historical (the class is the SIPI core since 2016 and has absorbed every responsibility that touched images) rather than designed.

**2. Free functions over a value type is the Rust-aligned shape.** `image.crop(...)` doesn't survive the Rust port directly (Rust doesn't have inheritance-based dispatch on values). `Sipi::processing::crop(image, ...)` maps cleanly to a Rust trait method or free function. Per the [Rust-aligned, transitional C++ method invariant](../archive/2026-05-08-modularization-analysis.md#method-invariants-dont-drift-on-these-across-sessions).

**3. The HTTP-output dual-write optimization is preserved through `OutputSink::TeeSink` composition.** Today's encoder writes its byte stream simultaneously to the HTTP socket *and* a cache file. The current implementation uses the magic-string filepath sentinel `"HTTP"` to encode the destination, with the cache-write happening outside the format handler. Under ADR-0006's typed `OutputSink` variant, a `TeeSink` alternative composes other sinks: the request handler hands the format handler one sink — `TeeSink{HttpSink, FilePath}` — and the format handler writes once, with each chunk broadcast to all sub-sinks. Same semantic; cleaner location of the tee (inside the format handler's write loop, not in the request handler orchestrating two passes). Generalises naturally — an `S3Sink` alternative joining the tee covers write-through to remote storage if that ever lands.

**4. Migration lands as small, independently reversible changes.** The raw-`pixels`-pointer audit and the `vector<byte>` swap (reason 5) already landed ahead of the decomposition itself — they did not depend on the Bazel package split and were the lowest-risk, highest-leverage first step. What remains is Bazel-package-dependent: extracting `image_processing/` as free functions, removing the four format-handler friend declarations in favour of a public mutator surface, and moving the supporting modules per the disposition below. Each of those is its own reversible change.

**5. Performance was preserved by the `vector<byte>` swap.** `std::vector<byte>::operator[]` and `byte_ptr[i]` compile to identical machine code in optimized builds; `vec.data()` returns a `byte*` indistinguishable from the raw pointer for any C-API use (libtiff, libpng, libjpeg-turbo, Kakadu). The pixel-touching algorithms (Floyd-Steinberg dither, in-place colour conversion, libtiff strip writes, etc.) run at identical speed — verified by benchmark before the swap landed, with no measurable regression.

## Considered Options

- **Keep `SipiImage` as a god-object** — rejected. Six responsibility groups is the textbook over-loaded class; every consumer pays for behaviours they don't use. The Rust port has nothing to map this onto.

- **Split into many tiny modules (one per behaviour)** — rejected. `image_processing/` as a single Bazel package with multiple sub-headers (`crop.h`, `scale.h`, etc.) is sufficient. Per-operation packages would multiply Bazel boilerplate without adding boundary value; nothing meaningful is enforced by separating `crop` from `scale`.

- **Subclass-based decomposition** (e.g., `Image` base, `ProcessableImage` derived) — rejected. Inheritance hierarchies don't survive the Rust port (Rust uses traits, not class hierarchies). Free-functions-over-value-type maps to Rust traits cleanly.

- **Keep `byte *pixels`; replace only ownership semantics** — rejected. Manual `new[]`/`delete[]` ownership requires explicit deep-copy ctor, move ctor, move-assignment, and dtor; ~100 lines of mechanical code that vanishes with `vector<byte>`. The maintenance cost is permanent.

- **Use `std::unique_ptr<byte[]>` instead of `std::vector<byte>`** — kept as fallback. `vector` is preferred for inherent size-tracking (consistency check vs. claimed dimensions); `unique_ptr<byte[]>` is the second-best if benchmarking shows any regression, decided empirically.

- **Defer the decomposition to the Rust rewrite itself** — rejected. The strangler-fig migration is incremental; each Rust-rewritten module needs to interoperate with still-C++ surrounding modules through stable shims. Decomposing now reduces the size of the eventual Rust translation unit and the shim surface.

## Target structure

The hub dissolves into named packages, not a single successor. Full disposition:

| Source today | Destination |
|---|---|
| [`SipiImage.{h,cpp}`](../../src/SipiImage.h), [`SipiImageError.h`](../../src/SipiImageError.h), [`populate_from_image.{h,cpp}`](../../src/populate_from_image.h), [`SipiIO.h`](../../src/SipiIO.h) | `src/image/cpp/` (`//src/image`) |
| the ~12 processing methods (crop/scale/rotate/colour/channel/bit-depth/watermark/arithmetic) | free functions in `src/image_processing/cpp/` (`//src/image_processing`) |
| [`resample.{cc,h}`](../../src/resample.h), [`process_benchmark.cpp`](../../src/process_benchmark.cpp) | `src/image_processing/cpp/` (benchmark co-located per ADR-0003) |
| [`SipiCache.{h,cpp}`](../../src/SipiCache.h) | `src/cache/cpp/` (`//src/cache`) |
| `SipiCommon.{h,cpp}` | deleted — empty translation units |
| [`SipiFilenameHash.{h,cpp}`](../../src/util/cpp/SipiFilenameHash.h) | `src/util/cpp/` |
| [`SipiConf.{h,cpp}`](../../src/SipiConf.cpp) | `src/ffi/cpp/` |
| [`SipiReport.cpp`](../../src/SipiReport.cpp) | `src/cli/cpp/` |
| [`SipiError.{h,cpp}`](../../src/SipiError.h) | `src/error/cpp/` (`//src/error`) |

`//src/format_handlers` (the four `SipiIO` codec handlers, `output_sink`, and `format_registry.cpp`) is out of scope for this ADR — it already exists as its own package and is unaffected except for the `read_watermark` boundary below.

Two points in this disposition are non-obvious and need their reasoning spelled out:

- **No `src/engine/` folder is created.** "Engine" stays umbrella vocabulary for the whole C++ side (as [`ARCH-MAP.md`](../../ARCH-MAP.md) uses it), not a directory. The hub dissolves into named packages; there is no grab-bag destination for anything that doesn't obviously belong elsewhere.
- **`//src/error` is its own foundational package**, not folded into `image`. `metadata`, `format_handlers`, and `iiifparser` all need `SipiError` *without* depending on `image`; folding it into `image` would create an `image → metadata → image` Bazel cycle that cannot be expressed. It is the shared-error-base role that used to live in the flat `//src:sipi_top` target (since dissolved), made an explicit, named package.
- **`SipiConf` goes to `ffi`, not `cli`.** Its production consumer is [`src/ffi/cpp/init.cpp`](../../src/ffi/cpp/init.cpp) (`sipi_init`), and `cli → ffi` is the documented one-way dependency direction ([`src/ffi/BUILD.bazel:39-42`](../../src/ffi/BUILD.bazel)). ARCH-MAP's "CLI's config object" line for `SipiConf` is stale and will be corrected when ARCH-MAP is next rewritten.

### The `read_watermark` link-time inversion

`format_registry.cpp` keeps sole ownership of `SipiImage::io`: `image` declares the static registry, `format_handlers` defines it, and there is no Bazel edge from `image` to `format_handlers` — the existing inversion is carried forward unchanged.

`Sipi::read_watermark` needs the same treatment, but the direction is the reverse of `io`: it is *declared* today in `SipiImage.h` (moving to `image`) and *called* from the watermark-application method (moving to `image_processing`), while its *definition* stays in `format_handlers` (`SipiIOTiff.cpp`). Once `image_processing` calls it, `format_handlers` must be able to see the same declaration it defines against. Two options, and only two:

- `//src/format_handlers` gains an explicit dependency on the `image_processing` header that declares `read_watermark` (the reverse edge — `image_processing` depending on `format_handlers` — would cycle, since `format_handlers` already depends on `image`/`image_processing` for the `Image` value type).
- The declaration is hoisted into a dependency-free leaf header that both packages depend on, following the [`output_sink`](../../src/format_handlers/cpp/output_sink.h) pattern (a leaf `format_handlers` already uses so `image` can reach it without depending on the handler package).

"No edge in either direction" is not one of the options — the definition and the declaration must agree, and that requires a dependency one way or the other (or a shared leaf both depend on).

### Boundary acceptance checks

The new topology must satisfy these `bazel query` invariants (same style as [ARCH-MAP's iiifparser FFI-freedom check](../../ARCH-MAP.md)):

- `bazel query 'deps(//src/image_processing/...)'` resolves only to `{image, error, util, logging, observability, metadata}` — no `format_handlers`, no `ffi`, no Kakadu/libtiff/etc.
- `bazel query 'somepath(//src/image, //src/image_processing)'` is empty — `image` never depends on `image_processing`, even transitively.
- `bazel query 'deps(//src/error/...)'` matches its minimal documented set (stdlib only) — nothing pulls `image` back in through `error`.

## Consequences

- **`SipiImage` shrinks dramatically**. Public API ~15 methods (geometry, pixel access, metadata accessors, move/copy semantics) instead of ~50. Heavy header transitively-includes (shttps, format-handler types, 5 metadata standards) reduce to ~3 forward declarations + the value-type basics. Bazel `--strict_deps` becomes practical at the `image/` boundary.

- **Free-function-style image processing**. `image.crop(...)` becomes `Sipi::processing::crop(image, ...)`. ~12 method-to-free-function rewrites at every call site. The Lua-facing surface — the C++ [`image_handle.cpp`](../../src/ffi/cpp/image_handle.cpp) FFI handle family and the Rust binding layer [`bindings/image.rs`](../../src/scripting/rust/bindings/image.rs) — is updated to call the free functions. No facade methods are added to `Image`: the FFI/Lua binding layer is the sole translator from `image:crop(...)`-style Lua calls to `Sipi::processing::crop(image, ...)`. A facade on `Image` would require `image` to depend on `image_processing`, which cycles (`image_processing` depends on `image`, never the reverse) — see the boundary acceptance checks above.

- **No more friend classes**. `SipiImage.h` today has four `friend class` declarations for the format handlers (`SipiIOTiff`, `SipiIOJ2k`, `SipiIOJpeg`, `SipiIOPng`) plus one `friend std::ostream &operator<<`; there is no `Icc` friend. The four format-handler friendships are what the decomposition removes — format handlers gain a public `pixels_writable()` API + metadata setters instead of reach-in access. The `operator<<` friendship is a different question (a stream-output operator, not a handler back-door) and is unaffected.

- **No more raw `byte *pixels`** (already true). `pixels` is `std::vector<byte>`; RAII eliminates the explicit deep-copy ctor / move ctor / move-assignment / dtor dance the manual ownership required. `cpp-style-guide.md`'s "no raw owning new/delete" rule honoured.

- **No more HTTP coupling in `SipiImage`** (already true). No `shttps::Connection*` field or `connection()` accessor remains on the class. The HTTP-output sink is a parameter to format-handler `write()` per ADR-0006, not a property of `Image`.

- **Static `io` registry stays split, definition-side in `format_handlers`.** `SipiImage::io` is declared where `SipiImage` moves (`image`) and defined in `format_registry.cpp` (`format_handlers`) — see the link-time inversion above. This is not a pending move; it already holds and the decomposition preserves it.

- **`app14_transform` field removed.** The JPEG handler inverts CMYK/YCCK at decode time so downstream code sees standard CMYK; no transient flag needed on the universal Image type. Still present on `SipiImage` today; lands with the decomposition.

- **TeeSink for dual-write preservation** (already true). `OutputSink`'s variant already includes a `TeeSink` alternative ([`output_sink.h`](../../src/format_handlers/cpp/output_sink.h)).

- **Migration is gated on the Bazel build-system migration reaching its module-co-located layout phase** for the Bazel-package-dependent steps: extracting `image_processing`, removing the friend declarations in favour of a public mutator surface, and moving the supporting modules per the disposition table. The `vector<byte>` swap and `app14_transform` removal are not Bazel-package-dependent and can land independently.

- **Approval-test surface unchanged**. Behaviour preservation is intended throughout the decomposition. Approval goldens stay valid.

- **No facade methods on `Image`.** See "Free-function-style image processing" above — the FFI/Lua binding layer (`image_handle.cpp` + `bindings/image.rs`) is the sole translator; this is a closed decision, not an open question.

- **Glossary deltas land in [`CONTEXT.md`](../../CONTEXT.md)** in the batched edit pass: add **Image processing** (umbrella for the free-function module). Sharpen **Image** (the code-level class becomes a narrow value type; domain term stays correct).
