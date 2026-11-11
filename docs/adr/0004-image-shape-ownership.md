---
status: accepted
---

# Image shape and S3-access index lookup is owned by format handlers via a fixed-offset Essentials packet

> **Joint-implementation pointer (DEV-6537, 2026-05-15):** this ADR's fast path landed together with [ADR-0005](./0005-essentials-packet-versioned-binary-serialization.md)'s protobuf wire format and the preservation-pipeline ADRs ([ADR-0009](./0009-file-taxonomy.md), [ADR-0010](./0010-file-creation-is-intentional.md), [ADR-0011](./0011-preservation-metadata-via-xmp.md)) in a single PR. See the [DEV-6537](https://linear.app/dasch/issue/DEV-6537) parent for the cross-ADR coordination. The wire format reconciled from the original "CBOR" framing to **Protocol Buffers** at implementation time — see ADR-0005's "Why protobuf (CBOR → protobuf reconciliation)" section. Any "CBOR" reference in the prose below is shorthand for "the binary wire format specified by ADR-0005" and should be read as the protobuf form.


A SIPI module that needs the intrinsic shape of a Service File — `(img_w, img_h, tile_w, tile_h, clevels, numpages, nc, bps)` — calls `SipiIO::read_shape(origpath) → SipiImgInfo` on the appropriate format handler. The cache holds no shape data; `SipiCache::SizeRecord`, `SipiCache::sizetable`, and `SipiCache::getSize()` are removed. The two consumers today are canonical-URL computation (always — it needs `img_w`/`img_h` to resolve pixel-coord cache keys) and the decode-memory-budget peak estimator (cache-miss only — the full struct).

`read_shape` is the existing `SipiIO::getDim(filepath) → SipiImgInfo` virtual, renamed for self-documentation: the return type carries full image shape (dimensions + tile + clevels + numpages + nc + bps + orientation + mimetype + sub-image resolutions), not just dimensions. No new virtual method is added; existing overrides are renamed.

The **Service File format handlers** — `SipiIOJ2k` (JP2) today, `SipiIOTiff` (pyramidal mode) after the planned migration — implement `read_shape` via a fast path: they read shape *and file-structure offsets* from a dedicated **Essentials packet** at a known fixed prefix offset within the Service File. The packet's contents broaden from "shape only" to:

- **Image shape** (8 fields): `img_w, img_h, tile_w, tile_h, clevels, numpages, nc, bps`.
- **File-structure offsets** (per-format):
  - Pyramidal TIFF: per-IFD byte offset + compressed size for each pyramid level.
  - JP2: codestream-box offset + per-resolution-level offsets within the codestream.
- **ICC profile bytes** (existing).
- **Identity** (existing): original filename, mimetype, hash type, pixel checksum.

Other format handlers (`SipiIOPng`, `SipiIOJpeg`, plain `SipiIOTiff`) implement `read_shape` via standard format-native header parsing only. They are not in the server hot path: they are used for IIIF output writes in server mode (post-decode encoding to JPEG / PNG / TIFF; latency dominated by the encode itself), and for reading input files of arbitrary format in CLI mode (the read side of conversion; offline, not S3-bound). Neither use case warrants the Essentials-packet fast path.

CLI-mode conversion writes the Essentials packet with shape + structure offsets. Server-mode reads benefit from the fast path. The Essentials-packet schema is format-agnostic — same wire format ([ADR-0005](./0005-essentials-packet-versioned-binary-serialization.md)) for JP2 and pyramidal TIFF — but the *embedding mechanism* is handler-specific (a JP2 UUID box positioned after the JP2 signature + FTYP boxes; a tag in the first IFD of a TIFF, reachable via the file-header offset at bytes 4-7).

We accept this for four coupled reasons.

**1. Shape is intrinsic to the image, not derivable from cache state.** The cache's earlier shape memoization (`SipiCache::sizetable`) was parasitic — populated only as a side effect of `add()`, never independently persisted, vestigial after eviction, absent for un-cached origpaths. The right-shaped optimisation lives at the format-handler layer, not the cache layer. See [Probe 1](../archive/2026-05-08-modularization-analysis.md#probe-1--sipicache).

**2. The S3 transition is a forcing function (3-6 months out).** Service Files are accessed remotely *today* (NFS-mounted ZFS spinning disk; each read is a network round trip with seek penalties on spinning disk). The S3 transition makes every read an HTTP **range GET** (~1-10ms per round trip, no seek but with TLS + auth overhead). The packet-at-fixed-offset design allows SIPI's pre-decode logic to fetch the packet with **one** range GET of a known prefix (e.g. the first 64KB of the file), then **one** targeted range GET for the data SIPI actually needs to decode. Without the packet, SIPI must walk format-native structures (TIFF IFD chains, JP2 box hierarchies) — each parse step is a separate range GET — racking up 5+ round trips per request, multiplied by pyramid depth. The latency difference is roughly 5ms vs. 50ms on the server hot path. NFS already pays a fraction of this cost today; S3 makes it the dominant load-bearing factor.

**3. The packet's file-position must be fixed-prefix-readable.** For JP2: a UUID box positioned after the JP2 signature box and FTYP box. For pyramidal TIFF: a tag in the first IFD (which is reachable in the first 64KB by virtue of TIFF's header pointing to it at bytes 4-7). Both formats accommodate this without breaking spec compliance. If 64KB isn't enough for outliers (large embedded ICC, many pyramid levels), either bump the prefix size globally or include a `packet_size` field at a known fixed offset for a worst-case-bounded second range GET.

**4. Existing Service Files lacking the packet (or lacking the structure-offset additions) fall back to format-native structure walking.** Backward compatibility is preserved; the fast path activates incrementally as files are re-processed. No mass re-conversion required.

## Considered Options

- **Keep the parasitic shape memoization in the cache** — rejected. The two structs (`SizeRecord` and `CacheRecord`) overlap by construction (same fields, different keys); `sizetable` is populated only by `add()`, never independently persisted, vestigial after eviction, absent for un-cached origpaths. Over S3 the cache-as-shape-source design has no architectural advantage that wouldn't also exist for the format handler — and the memoization couples shape lookup to cache state, which is wrong.

- **Memoize shape inside the peak-memory estimator** — rejected. The estimator is not the only consumer; canonical-URL computation also needs `img_w`/`img_h` on every request. Putting the memoization in the estimator would either duplicate the lookup at the canonical-URL site or force the canonical-URL site to depend on the estimator. Both worse than the format-handler-as-source design.

- **JP2-specific Essentials-packet fast path only, no pyramidal TIFF** — rejected. Pyramidal TIFF is the planned successor to JP2 as the Service File format. Designing the packet carrier as JP2-specific would require duplicating it for TIFF immediately. Defining the schema once at the Essentials-packet layer and letting both `SipiIOJ2k` and `SipiIOTiff` consume it is uniform.

- **Add the shape-read fast path to every format handler** — rejected. PNG, JPEG, and non-pyramidal TIFF handlers are used either for IIIF output writes (server mode, post-decode) or for reading arbitrary-format input files in CLI mode (offline). Neither path is server-hot. Format-native header parsing is sufficient.

- **Limit the packet to image shape only (no file-structure offsets)** — rejected. Without offsets, SIPI must walk format-native structures over S3 — the optimization is half-measure. The marginal cost of including offsets in the packet is small (a few hundred bytes for typical pyramids); the marginal benefit is large (5+ round trips → 1).

- **Defer the S3 design to a separate future ADR** — rejected. The decisions are coupled (where the packet lives, what's in it, how SIPI reads it). Splitting risks designing the shape-only packet now, then rediscovering S3 constraints later and reopening the file format. The 3-6 month S3 horizon is too close to defer the design.

## Consequences

- **`SipiCache` shrinks**. `SizeRecord`, `sizetable`, `getSize()`, and the bug-prone non-cleanup of `sizetable` on eviction (`purge()` and `remove()` don't touch it — vestigial growth) all go away. The cache becomes a pure representation cache. See [Probe 1](../archive/2026-05-08-modularization-analysis.md#probe-1--sipicache).

- **`SipiIO::getDim` is renamed to `read_shape`**. Existing virtual already returns full shape via `SipiImgInfo`; the rename is for self-documentation. Each subclass's override updates name. No new virtual method added. See [Probe 3](../archive/2026-05-08-modularization-analysis.md#probe-3--format_handlers-renamed-from-formats).

- **Service File format handlers (`SipiIOJ2k` + pyramidal `SipiIOTiff`) get the Essentials-packet fast path** in `read_shape`. Implementation: range-read a fixed prefix (e.g. first 64KB), parse the packet from its known location, return shape from the packet's `shape` section. Fallback to format-native parsing if the packet is absent or lacks the requested fields.

- **The Essentials packet schema gains image-shape AND file-structure-offset fields**. Wire format per [ADR-0005](./0005-essentials-packet-versioned-binary-serialization.md). The packet's role broadens from "preserve cross-conversion identity" to "preserve identity *and* serve as the S3-access index for the file." Old Service Files without the new fields fall back to format-native parsing.

- **`SipiHttpServer.cpp` request flow simplifies**. The call site at line 1571 becomes `format_handler->read_shape(infile)`, returning a full `SipiImgInfo` with `nc`/`bps` populated (the "remain 0" overestimate comment at line 1563 disappears). The decode-memory-budget admission check gets accurate inputs as a happy side effect.

- **A `SourceReader` abstraction is needed** to wrap local FS / NFS / S3 access uniformly. Today format handlers call libtiff / Kakadu / libjpeg / libpng with file paths; under S3 they need a stream-or-range abstraction. Out of scope for this ADR; flagged as the natural follow-on (likely a separate ADR + module under `src/source_reader/` or similar). The Essentials-packet design in this ADR is independent of which `SourceReader` implementation runs underneath — same packet location, same parsing.

- **`SipiLua.cpp` is not directly affected**. The Lua admin surface does not touch `getSize` or `read_shape`.

- **Approval-test surface is unchanged**. Shape and structure-offset fields are read-only metadata; rendered image bytes are unaffected.

- **Migration path for existing Service Files**: existing JP2s and TIFFs in production lacking the packet (or lacking the structure-offset additions) fall back to format-native parsing — slower over S3 but functionally correct. New conversions populate the packet. No mass re-conversion required; fast path activates incrementally as files are re-processed. This is the load-bearing operational property — given the 100K Service File install base ([ADR-0005](./0005-essentials-packet-versioned-binary-serialization.md)'s longevity invariant), mass re-encoding is not feasible.

- **The decision is coupled with [ADR-0005](./0005-essentials-packet-versioned-binary-serialization.md)** (wire format). The wire-format choice matters more under S3 — every byte of header sits inside a range-GET span, and forward-compat allows future schema additions (e.g. per-tile offset tables for ultra-tile-heavy access patterns) without re-encoding.

- **Glossary deltas land in [`CONTEXT.md`](../../CONTEXT.md)**: `Image shape`, `Operating mode` / `Server mode` / `CLI mode`, the three-tier *Preservation pipeline* taxonomy per [ADR-0009](./0009-file-taxonomy.md) (**Preservation File** / **Service File** / **Access File** plus `Preservation pipeline` umbrella and format entries), `Pyramidal TIFF`, `Object storage`, `Range GET`, `Codec` (sharpened), `read_shape` (rename of `getDim`), and a sharpened `Essentials packet` definition. Tracked in the [glossary delta register](../archive/2026-05-08-modularization-analysis.md#glossary-delta-register).
