# Preservation pipeline

Vitrinli's `convert`-family subcommands form an explicit pipeline that moves
files through preservation stages. Each subcommand has documented
prerequisites it checks at entry and produces a specific output. A
file's stage in the pipeline is the *outcome* of going through the
corresponding subcommand and meeting its prerequisites — not a label
the file carries or a flag the writer is told.

This page is the single reference for what each `convert` / `verify`
subcommand requires and produces. The architectural rationale lives
in [`ADR-0009`](adr/0009-file-taxonomy.md) (preservation-pipeline
taxonomy) and [`ADR-0010`](adr/0010-file-creation-is-intentional.md)
(creation is intentional, gated by CLI subcommand).

## The chain

```
                                          arbitrary source
                                                │
                                                ▼
                                  ┌───────────────────────────┐
                                  │ sipi convert preservation- │  (future, ADR-0012)
                                  │ file <src> <out>           │
                                  └───────────────────────────┘
                                                │
                                                ▼
                                       Preservation File
                                                │
                                                ▼
                                  ┌───────────────────────────┐
                                  │ sipi convert service-file  │
                                  │ <preservation> <out>       │
                                  └───────────────────────────┘
                                                │
                                                ▼
                                          Service File
                                                │
                                                ▼
                                  ┌───────────────────────────┐
                                  │ sipi convert access-file   │
                                  │ <service> <out>            │
                                  └───────────────────────────┘
                                                │
                                                ▼
                                           Access File
```

`sipi convert <src> <out>` (plain, no subnoun) is a generic
ImageMagick-style conversion that always produces an Access File. It
is not part of the preservation pipeline; it's the escape hatch for
operators who want a one-shot format conversion.

## Subcommand reference

### `sipi convert preservation-file <src> <out>` — future

**Status:** Stub. Errors with "awaits ADR-0012". Tracked in `DEV-6537`
follow-up `DEV-6552` (preservation file format ADR).

**Prerequisites (when implemented):** TBD by ADR-0012. Likely a
combination of: source is decodable, source carries some minimum
embedded-metadata baseline (rights / provenance), output extension is
the chosen preservation format.

**Produces:** Preservation File — plain (non-pyramidal) lossless TIFF
per archival policy; carries the preservation-metadata schema in the
embedded-metadata channel (XMP) per ADR-0011.

### `sipi convert service-file <src> <out>`

**Prerequisites:**

* Source is decodable via the format-handler reader.
* Output extension is `.jp2` / `.jpx` (→ JP2) or `.tif` / `.tiff`
  (→ pyramidal TIFF). Other extensions are rejected per ADR-0009:
  Service Files only live in those two carriers.

**Prerequisites pending ADR-0012:**

* Source is a Preservation File (currently not enforceable — the
  Preservation File discriminator awaits ADR-0012). Until then, any
  decodable source is accepted; the maintainer-discipline contract is
  to invoke this command on Preservation Files.

**Effects:**

1. Reads source via `SipiImage::readSource` (no ambient stamping;
   source file untouched on disk).
2. Drops any Essentials packet the source happened to carry — re-encoding
   always produces a fresh packet (maintainer decision 2026-05-14).
3. Applies `--topleft` orientation normalization if requested. Other
   transforms are intentionally NOT exposed on this subcommand per the
   D5 option-availability matrix.
4. Builds `EssentialsFields` from observed source + post-transformation
   `SipiImage` state: identity (`origname`, `mimetype`), shape (`img_w`,
   `img_h`, `nc`, `bps`), and the *pixel checksum* (SHA-256 of the
   post-transformation pixel buffer).
5. Stamps the packet on the image via `essential_metadata(...)`.
6. Calls the format-handler writer. The writer emits the Essentials
   carrier (JP2 UUID box / TIFF tag `65112`) because the in-memory
   packet is set — no separate marker is passed.

**Produces:** Service File — pyramidal TIFF or JP2 with an Essentials
packet at the carrier slot defined by ADR-0005.

### `sipi convert access-file <src> <out>`

**Prerequisites:**

* Input is a Service File: the format-handler reader populates the
  Essentials packet on `SipiImage` if the carrier is present and
  parses. If `essential_metadata().is_set()` is false after read, the
  command exits non-zero with the documented "use `sipi convert` for
  generic format conversion" message.
* Output extension is one of `.jpg` / `.jpeg` / `.jpx` / `.jp2` /
  `.tif` / `.tiff` / `.png` (or the corresponding `--format` value).

**Effects:**

1. Reads source via `SipiImage::readSource` with optional region /
   size.
2. Validates Essentials presence (prerequisite #1).
3. Drops the in-memory Essentials packet — Access Files do not carry
   Essentials per ADR-0009.
4. Applies transforms (orientation, ICC, rotate, mirror, watermark)
   per IIIF semantics.
5. Calls the format-handler writer. The writer does NOT emit an
   Essentials carrier because the in-memory packet is unset.

**Produces:** Access File — any supported format; no Essentials packet;
XMP / IPTC / EXIF / ICC propagated from the Service File source via
the format-handler write paths (ADR-0011).

### `sipi convert <src> <out>` (plain, generic)

**Prerequisites:** Source is decodable.

**Effects:** Reads, optionally transforms per CLI flags, writes. Does
not touch the in-memory Essentials packet (which is typically unset —
the source is usually a non-Service-File).

**Produces:** Access File — output of a generic ImageMagick-style
conversion. The writer's Essentials-emission gate stays closed because
no in-memory packet was set.

### `sipi verify <file>` and variants

| Variant | Prerequisites | Effect |
|---|---|---|
| `sipi verify <file>` | Source is decodable. | Decoder-coverage check (RDU sanity). |
| `sipi verify access-file <file>` | Source is decodable. | Generic check + assert NO Essentials packet (misclassification tripwire). |
| `sipi verify service-file <file>` | Source is decodable; extension is JP2 / pyramidal TIFF. | Generic check + assert Essentials packet present + shape consistency + pixel-hash matches `data_chksum`. |
| `sipi verify preservation-file <file>` | (future, ADR-0012). | Stub. |

## Why packet presence (not a parameter)

Earlier designs in the ADR carried a `*_FileRole` SipiCompressionParams
key on the writer's parameter surface. That was a redundant signal:
the in-memory Essentials packet already encoded the same intent. Two
parallel signals invite drift — a command could set the packet but
forget the marker, or vice versa.

The resolved design: the writer's emit gate is
`essential_metadata().is_set()` alone (TIFF also requires pyramidal
layout, since Service Files only live in pyramidal TIFF per ADR-0009).
Each `convert *` command sets up the in-memory state to match its
intended output, and the writer's behaviour follows directly. The
writer API stays generic; the pipeline discipline lives at the command
layer.

## See also

* [`UBIQUITOUS_LANGUAGE.md`](../UBIQUITOUS_LANGUAGE.md) — canonical
  glossary, including the `Preservation pipeline` entry.
* [`docs/adr/0009-file-taxonomy.md`](adr/0009-file-taxonomy.md) — preservation-pipeline taxonomy.
* [`docs/adr/0010-file-creation-is-intentional.md`](adr/0010-file-creation-is-intentional.md) — intentional-creation principle.
* [`docs/adr/0011-preservation-metadata-via-xmp.md`](adr/0011-preservation-metadata-via-xmp.md) — preservation metadata channel.
