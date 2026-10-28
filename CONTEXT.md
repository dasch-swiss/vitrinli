# Vitrinli image server

Vitrinli is the **IIIF subdomain implementation** of the **Access Area** bounded context in the wider [`dsp-repository`](https://github.com/dasch-swiss/dsp-repository) system. It serves IIIF Image API 3.0 and IIIF Presentation API requests over the OCFL-backed Preservation Files held by the `dsp-repository` Archive context — producing Access Files (image tiles, IIIF Manifests) for IIIF clients.

For the system-level view of where Vitrinli fits — the Archive context, the Access Area subdomain shapes (IIIF, HTML/DPE, Custom Presentation, Asset/Download, SPARQL), the Producer-side flow from VRE through RDU-Tooling, and the Access Area's other subdomain implementations — see the upstream [`dsp-repository` Context Map](https://github.com/dasch-swiss/dsp-repository/blob/main/CONTEXT-MAP.md).

## Published Language (from Access Area)

These three terms are **shared Published Language across contexts** at the system level. Vitrinli consumes them on input and produces them on output:

- **Preservation File** — owned by the Archive context. The canonical, long-term-stable byte stream stored in OCFL. Vitrinli reads these via the Archive's Binary retrieval API during Service File derivation.
- **Service File** — owned by the Access Area context. The derivative form optimised for fast access (e.g. pyramidal TIFF or JP2 with an `Essentials` carrier for Vitrinli). Materialised by an Access Area subdomain (this one) on subscription to Archive events.
- **Access File** — owned by the Access Area subdomain that serves the request. The on-the-wire form a Consumer actually receives — for Vitrinli, an IIIF tile (JPEG/PNG/TIFF/WebP) carved out of a Service File by the IIIF pipeline.

Use these terms in code, commits, ADRs, and PR descriptions when crossing the seam to the Archive or to another Access Area subdomain. Do not synonymize them with Vitrinli-internal vocabulary.

## Upstream language (from the VRE)

Vitrinli also runs inside the **VRE** — `dsp-api` and `dsp-ingest` — where researchers
upload material and work on it before anything is archived. The VRE has its own
vocabulary, and Vitrinli meets it at one seam: the access decision the *Preflight
script* fetches from `dsp-api` (`GET /admin/files/{shortcode}/{filename}`). That
payload is authored by the VRE and speaks VRE terms; Vitrinli translates on receipt.

- **Original** — the byte stream a researcher uploaded. `dsp-ingest` serves it at
  `/projects/{shortcode}/assets/{assetId}/original`. Vitrinli never serves an Original
  under that name.
- **Derivative** — what `dsp-ingest` produces from an Original at upload time and
  what Vitrinli serves, whether through the IIIF pipeline as an *Image* or as-is
  through `/file` as a *Bitstream*.

**Only still images are transcoded.** `IngestService` dispatches on media type,
and only `StillImage` produces new bytes (a JP2). `MovingImage`, `Audio`,
`SvgImage` and `OtherFiles` (PDF, archive, text) all reach
`storage.copyFile(original.file, derivative)` — the Derivative is byte-identical
to the Original and differs only in filename (`{assetId}.{ext}`). So for every
media kind except raster still images, **Original and Derivative name the same
bytes reached by two different routes**, not two artifacts. Any reasoning that
treats withholding one as withholding the content is wrong for those kinds.

**These are not synonyms for the Access Area's terms**, and the mapping is a
lifecycle rather than a translation table. When material is archived, the
Original is archived *and* a *Preservation File* is derived from it for long-term
bit preservation; the Access Area separately derives a *Service File*, from which
Vitrinli carves *Access Files*. So an Original and a Preservation File are different
artifacts that coexist, as are a Derivative and a Service File.

Use VRE terms only when describing that seam — the preflight payload and the
callers on either side of it. Everywhere else in this repo, the Access Area terms
above and the Vitrinli glossary remain canonical.

## Subdomain language (Vitrinli-local)

The canonical Vitrinli glossary is in [UBIQUITOUS_LANGUAGE.md](./UBIQUITOUS_LANGUAGE.md). It defines: Image vs Bitstream, Identifier (with embedded Page) + Prefix, Image root vs Document root, the IIIF pipeline terms (Region / Size / Rotation / Quality / Format / Decode level / Canonical URL / Cache key), Format handler vs Codec, Preservation metadata (umbrella) over Embedded metadata + Essentials packet, Image / Bitstream Information document, the three Lua entry points (Init script / Preflight script / Route handler), the eight Permission types, and the Throttling umbrella over Admission (the shell-side two-lane pool) + Decode memory budget.

Prefer the glossary's canonical terms over the variant spellings in older code.

## The HTTP server: Rust shell over the C++ engine

The HTTP server is the Rust shell (`//src/cli/rust:sipi` over the `//src/server/rust:lib`
library, axum-based), which drives the C++ image engine through the FFI seam. There
is no C++ HTTP server: the retained `shttps` transport and `SipiHttpServer` — kept
in-tree through the strangler migration as the differential-parity oracle — have
been removed ([ADR-0020](docs/adr/0020-oracle-removal.md), which completes the
migration [ADR-0013](docs/adr/0013-shttps-as-internal-module.md) prepared). The
Rust shell owns the two-lane Admission pool and its knobs (`nthreads`, `max_waiting`,
`queue_timeout`, the tile/full ratios), route registration (axum for built-in
endpoints, Lua for scripted routes per [ADR-0017](docs/adr/0017-extensibility-lua-and-rust.md)),
and IIIF URI parsing (the standalone `//src/iiifparser/rust:iiif_parser` crate).

## Extracted domain modules (namespace `shttps`)

The C++ domain modules that used to live under `src/shttps/` survive the oracle
removal as ordinary top-level packages:

- `src/util/` — generic utilities (`shttps::Hash` / `HashType`, `shttps::Parsing`,
  `shttps::Error`, `shttps::Global`, `shttps::urldecode`).
- `src/scripting/rust/` — the Rust-hosted mlua Lua runtime (ADR-0023): the
  hardened per-request VM, all `server.*`/`SipiImage`/sqlite bindings (JWT
  sign/verify included, via `jsonwebtoken`), and Vitrinli's three Lua entry points.

Their C++ symbols keep `namespace shttps` (only the file/package location moved),
so a `shttps::` qualifier in code refers to one of these surviving modules, not to
a deleted HTTP transport. `src/shttps/` no longer exists.

### Naming clarification

Vitrinli's **Route handler** (in `UBIQUITOUS_LANGUAGE.md`) is a *Lua script* bound to a
URL pattern, run inside a request-scoped hardened VM of the Rust mlua runtime. Vitrinli's IIIF `/file`
endpoint (the **Bitstream** path-through) is the `FILE_DOWNLOAD` case of the Rust
IIIF classifier (`//src/iiifparser/rust:iiif_parser`), which reads from the **image root**.
