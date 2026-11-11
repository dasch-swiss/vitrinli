---
status: accepted
---

# Essentials packet adopts a versioned, self-describing binary wire format (Protocol Buffers)

The Essentials packet's wire format migrates from pipe-delimited text to **Protocol Buffers (`proto3`)** built with the `LITE_RUNTIME` option, consumed via Bazel Central Registry. A top-level `format_version` field discriminates reader dispatch. New SIPI versions read every prior `format_version` they support; legacy pipe-delimited packets continue to parse via `Essentials::parse_legacy(...)` indefinitely (longevity invariant). New packets are always written in the protobuf form, and only by the intentional `sipi convert service-file` subcommand (per [ADR-0010](./0010-file-creation-is-intentional.md)) into the two Service File carriers (JP2 + pyramidal TIFF) per [ADR-0009](./0009-file-taxonomy.md).

We accept this because SIPI's preservation guarantee is that conversions don't lose metadata, and SIPI's installed base is *hundreds of thousands* of files with legacy pipe-delimited Essentials packets. Mass re-encoding to flip serialization formats is not operationally feasible at that scale; the wire format must therefore be forward-evolvable forever — new fields, new optimisations, new types — without requiring coordination across the existing file population. The current pipe-delimited format has zero versioning, no schema, no escaping, and no field discovery; [ADR-0004](./0004-image-shape-ownership.md) alone (adding eight image-shape fields) already pushes it past its design limits, and any subsequent schema change would compound the fragility. Biting this bullet now, while the install base is smaller than it will be at any future point, is strictly cheaper than deferring.

## Why protobuf (CBOR → protobuf reconciliation)

The original drafts of this ADR (v1, v2) selected **CBOR (RFC 8949)** for compactness + IETF standardisation. The implementation-time decision flipped to protobuf because:

- **Schema evolution is by field number, not field name.** Field numbers are stable; field renames cost nothing on the wire. CBOR-by-map-key works but treats the key string as the contract — exactly the kind of stringly-typed surface that bit us in the pipe-delimited era.
- **Cross-language readiness.** The dsp-repository roadmap calls for a Rust IIIF service that will need to read the same Essentials packet. Protobuf has a first-class, well-maintained Rust codegen story (`prost`, `protobuf-rs`); CBOR-in-Rust has competent libraries but no shared schema artifact across SIPI's C++ and the Rust consumer.
- **BCR makes integration one line.** `bazel_dep(name = "protobuf", version = "34.1", repo_name = "com_google_protobuf")` + `bazel_dep(name = "rules_proto", version = "7.1.0")` — no `rules_foreign_cc` integration, no manual codegen step in `justfile`, no host-environment fiddling. The "tooling overhead" objection from v1 is empirically smaller than the CBOR-library objection that motivated v1.
- **`LITE_RUNTIME` keeps the static-link footprint small.** `option optimize_for = LITE_RUNTIME;` drops descriptor metadata, reflection, and text-format I/O from the generated code. Measured at link time: the sipi binary gains ~300-500 KB of code from the protobuf runtime + generated `essentials.pb.{cc,h}`, well within the 1 MB binary-size budget set by [DEV-6537](https://linear.app/dasch/issue/DEV-6537)'s acceptance criteria.

CBOR-jsoncons, tinycbor, and a custom in-tree binary codec were all considered and rejected for variants of "reinvents schema-evolution semantics we then have to maintain forever." Protobuf is the conservative choice for a 10+ year preservation horizon **because** it has the largest ecosystem footprint of the candidates — a dropped dependency three years from now is the load-bearing risk, and protobuf is the option least likely to be dropped.

(Earlier drafts of this ADR are preserved in the repository's git history; see commits prior to the [DEV-6537](https://linear.app/dasch/issue/DEV-6537) joint-implementation merge for the v1/v2 CBOR-flavoured framing.)

## Schema (`src/metadata/cpp/essentials.proto`)

```protobuf
syntax = "proto3";

package sipi.metadata;

option optimize_for = LITE_RUNTIME;

enum HashType {
  HASH_TYPE_UNSPECIFIED = 0;
  HASH_TYPE_MD5 = 1;
  HASH_TYPE_SHA1 = 2;
  HASH_TYPE_SHA256 = 3;
  HASH_TYPE_SHA384 = 4;
  HASH_TYPE_SHA512 = 5;
}

message Essentials {
  uint32 format_version = 1;

  // Core preservation identity. Populated by the `convert service-file`
  // command from observed source + post-transformation hash.
  string origname = 2;
  string mimetype = 3;
  HashType hash_type = 4;
  bytes data_chksum = 5;
  bool use_icc = 6;
  bytes icc_profile = 7;

  // Image-shape fields (ADR-0004). Populated by the writer from
  // current (post-transformation) SipiImage state.
  uint32 img_w = 8;
  uint32 img_h = 9;
  uint32 tile_w = 10;
  uint32 tile_h = 11;
  uint32 clevels = 12;
  uint32 numpages = 13;
  uint32 nc = 14;
  uint32 bps = 15;

  // Reserved runway for file-structure offsets (post-DEV-6442 work).
  reserved 16 to 31;
}
```

### Schema discipline

These rules are documented inline in `essentials.proto` and enforced by reviewer convention (`docs/src/development/reviewer-guidelines.md` flags any `.proto` change for a two-reviewer rule):

- **Never repurpose a field number.** Removed fields are marked `reserved <n>;`.
- **Never repurpose an enum value.** `HashType` integer mapping is asserted in `essentials.cpp` via `static_assert(static_cast<int>(shttps::HashType::md5) == HASH_TYPE_MD5)` (and equivalents for every enumerator). The reverse-pointer comment in `shttps/Hash.h` declares the SIPI on-disk dependency so a future reorder of the upstream enum trips the assertion at compile time.
- **Adding a field** picks the lowest unreserved field number; describes intent; defaults to optional semantics (proto3 makes everything optional anyway, but the comment block matters for readers).
- **Breaking semantic change** bumps `format_version`. The reader switches dispatch on the new value and runs a per-version migration; the old version's reader is retained indefinitely.

## Dispatcher semantics

`Essentials::parse(std::span<const std::byte>) → std::expected<Essentials, ParseError>` is the new-format reader. It probes in the documented order:

1. **Empty** input → `ParseError::Empty`.
2. **`ParseFromArray`** fails → `ParseError::Malformed`.
3. **`format_version == 0`** (proto3 default — field never set on the wire) → `ParseError::MissingVersion`. Distinct from `UnknownVersion` so future writers emitting `format_version=1` are still readable by today's reader; only literal-zero is treated as "not a versioned packet."
4. **`format_version > 1`** → `ParseError::UnknownVersion`. A future writer emits `format_version=2`; today's reader logs once and falls through to the legacy reader if a dual-carrier file exposes one (otherwise returns the error to the caller).
5. **Missing core fields** (`origname` / `mimetype` / `hash_type` / `data_chksum`) → `ParseError::MissingCore`. Protobuf accepted the bytes but the packet is semantically incomplete.

`Essentials::parse_legacy(std::string_view)` is the **read-only** legacy reader. It retains the permissive pipe-split semantics of the legacy ctor; hex-decodes `data_chksum` into raw bytes; returns an unset `Essentials` (`is_set() == false`) on malformed input. It is **not** removed by the Contract step that drops the legacy writer — pre-rollout files must remain readable indefinitely (per the longevity invariant). What is removed is the legacy *writer* (the `serialize() → std::string` overload and the `Essentials(const std::string&)` ctor).

`Essentials::serialize() → std::vector<std::byte>` is the only emitter. Byte-deterministic — protobuf emits fields in field-number order, which is stable across protoc revisions for the same `.proto` schema.

A per-read tripwire log fires on every successful `parse()`: `"Essentials: read format_version=%u (max supported: 1)"`. The format-handler decision-boundary metrics ([DEV-6537](https://linear.app/dasch/issue/DEV-6537)) attribute the same event to a Prometheus counter (`sipi_read_shape_fast_path_total{format, outcome}`).

## JP2 carrier — UUID box at slot 4

Per [ADR-0004](./0004-image-shape-ownership.md), the Essentials packet must be readable from a known fixed prefix offset within the Service File so the S3 transition can prefetch shape + offsets with a single bounded range GET (target: <64 KB prefix). The JP2 implementation:

- **Carrier:** a JP2 UUID box positioned at slot 4 — i.e. after the JP2 Signature box → FTYP box → `jp2h` box, and before the `jp2c` codestream box. The UUID is committed in source: `kSipiEssentialsUuid = 7B28A646-B9C3-4FB2-900B-B6855DF23882` (the constant lives in `src/format_handlers/cpp/SipiIOJ2k.cpp` and is mirrored in `CONTEXT.md`).
- **Box layout:** `[16-byte UUID][N-byte protobuf payload]`. Kakadu's `jp2_output_box` writes the JP2 box header (4-byte length + 4-byte `uuid` type code) framing the payload; the box's own length field is the canonical packet size at read time. No separate `total_size: u32` is needed inside the box payload.
- **Reader:** walks top-level boxes via `jp2_family_src` + `jp2_input_box::open_next()`; on a UUID box matching `kSipiEssentialsUuid`, reads `box.get_remaining_bytes()` into a buffer and calls `Essentials::parse(span)`. Falls through to the legacy codestream-comment scan + `parse_legacy(cstr+5)` on miss.
- **64 KB prefix invariant:** the writer is structured so the UUID box always lands within the first 64 KB of the file. ICC profiles that would push past this bound are the only realistic spillover risk; the schema reserves `format_version`-and-up at fixed early-field positions specifically so a short header read can confirm "this is a protobuf packet and its `total_size` is N" before issuing a follow-up range GET for outliers. Range-source spillover handling is [DEV-6442](https://linear.app/dasch/issue/DEV-6442)'s scope.
- **Slot-4 rationale:** earlier than this would precede the `jp2h` header box that JP2 readers rely on for codestream-independent shape recovery; later than this risks slipping past the 64 KB prefix budget for files with extensive XMP / IPTC / EXIF boxes.

## Pyramidal TIFF carrier — private tag 65112

- **Carrier:** new private tag `TIFFTAG_SIPIMETA_PB = 65112`, registered as `TIFF_UNDEFINED`. The libtiff field type lets us pass raw bytes through `TIFFSetField(tif, TIFFTAG_SIPIMETA_PB, count, data)` without TIFF treating the payload as text.
- **Legacy carrier (read-only):** `TIFFTAG_SIPIMETA = 65111` (TIFF_ASCII) continues to parse via `parse_legacy(...)`. Re-encode through `convert service-file` strips the legacy tag from the output.
- **First-IFD invariant:** TIFF's file header points to the first IFD at bytes 4-7; the first IFD is reachable in the first 64 KB by construction. The Essentials tag lives in the first IFD so the 64 KB-prefix invariant holds.

## Carrier surface restriction

Per [ADR-0009](./0009-file-taxonomy.md) and the [DEV-6537](https://linear.app/dasch/issue/DEV-6537) implementation: JPEG, PNG, and plain TIFF outputs **do not** carry an Essentials packet. The writers in `SipiIOJpeg.cpp` / `SipiIOPng.cpp` and the non-pyramidal branch of `SipiIOTiff.cpp` have no Essentials emission path at all. Per [ADR-0010](./0010-file-creation-is-intentional.md), the Service File writers (pyramidal TIFF + JP2) emit the packet only when an in-memory *Essentials packet* is set on the `SipiImage`. The single point of control is the writer gate: `emit_essentials_box = es.is_set()` (JP2) or `pyramid && es.is_set()` (TIFF). The `convert service-file` command is the only path that sets the packet in production; ensuring the in-memory state matches the intended output is the command's responsibility, not a parallel writer flag.

## Considered Options

- **Stay pipe-delimited; add ADR-0004 fields by appending more pipe segments** — rejected. Pushes past the format's design limits when we already know the next schema change is coming. The pipe-delimited reader has no escape mechanism (an `origname` containing `|` corrupts parse), no version discriminator, and no defined rule for unknown fields.

- **Adopt CBOR (jsoncons or tinycbor)** — rejected. Functionally adequate, but no shared schema artifact for the Rust IIIF service in dsp-repository. Schema evolution by field name is a weaker contract than by field number when the consumer count grows.

- **Adopt MessagePack** — rejected. Same as CBOR plus no IETF standard. No technical advantage.

- **Custom versioned binary format** — rejected. Reinvents what protobuf's spec already nails down (length-prefixing, type tagging, default-value rules, canonical encoding for cross-implementation equality). The maintenance cost of an in-house format compounds across the lifetime of the Service File population.

- **JSON** — rejected. Text-based; defeats the compactness goal in image headers and reintroduces the same escaping fragility that motivated this ADR.

- **Adopt protobuf without `format_version`**, relying entirely on field-number evolution — rejected. Field-level evolution covers most cases but cannot handle semantic redefinition. A coarse-grained `format_version` field is cheap insurance against the cases field-level evolution can't handle, at the cost of one `uint32` per packet.

## Consequences

- **`Essentials::parse(span<const std::byte>)` is the dispatcher**: protobuf parse → ParseError enum on failure, otherwise an `Essentials` populated from the codec adapter. The legacy reader is reachable via the static `parse_legacy(string_view)` factory and is retained indefinitely.

- **`Essentials::serialize() → std::vector<std::byte>`** is the only emitter (the Contract step dropped the `std::string`-returning overload). The on-disk artefact is *bytes*; the existing API's typing was a mistake corrected by the rewrite.

- **New build dep**: protobuf + rules_proto, both via BCR. No `rules_foreign_cc` integration, no host-environment configuration. `cc_proto_library` consumed only by `src/metadata/cpp/internal/protobuf_codec.cpp` so the `.pb.h` header doesn't leak into client code.

- **`std::expected<Essentials, ParseError>`** is the parse-API shape, matching [docs/src/development/cpp-style-guide.md](../src/development/cpp-style-guide.md)'s preference and the planned Rust target's `Result` ergonomics.

- **Approval-test goldens for image-header bytes change** where the test round-trips a Service File through the encoder. The regeneration ran with `SOURCE_DATE_EPOCH=946684800` and `SIPI_WORKSPACE_ROOT="."` injected by `test/approval/BUILD.bazel` so the wall-clock-stamped ICC creation date is overwritten with a fixed value and goldens stay byte-deterministic ([ADR-0002](./0002-icc-profile-determinism-test-only.md)).

- **Existing files with legacy Essentials packets keep working unchanged.** Pipe-delimited packets continue to parse via `parse_legacy`. The new wire-format fast path activates incrementally as files are re-processed by an intentional `sipi convert service-file` invocation, without any mass re-encoding event. This is the load-bearing operational property of this ADR.

- **Pipe-delimited fragility is gone for new packets**: protobuf's well-defined encoding rules replace the escape-less, schema-less, version-less legacy format. Old packets remain readable but read-only.

- **The `metadata/` Bazel package gains protobuf + rules_proto deps but no visibility change.** Consumers (`SipiImage`, format handlers) see no API surface change at the call sites that already use `Essentials` getters/setters. Only `parse` / `serialize` change shape.

- **Future schema additions become low-friction**: extend the `.proto` schema (lowest unreserved field number), regenerate via Bazel, populate the new fields in the codec adapter. No `format_version` bump for additive changes; the field-number contract is the contract.

- **Cross-context coupling with `shttps::HashType` is documented and asserted**: `shttps/Hash.h` carries a comment declaring its enumeration is on-disk-stable; `essentials.cpp` carries `static_assert`s that trip at compile time if the upstream enum is reordered.
