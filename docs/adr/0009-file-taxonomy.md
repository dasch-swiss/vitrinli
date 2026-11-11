---
status: proposed
---

# File taxonomy: Preservation File, Service File, Access File

SIPI's domain has three intentional stages of the preservation pipeline, distinguished by *purpose* — not by format, not by location, not by source provenance. The stages are: **Preservation File** (long-term bit-level preservation), **Service File** (mezzanine baseline for IIIF processing), **Access File** (end-user delivery). This taxonomy replaces the previous two-tier "Service master / Archival master" vocabulary and promotes the implicit IIIF-derivative concept to a first-class term.

The stages are orthogonal to file format. A pyramidal TIFF can be a Service File (with Essentials packet) or an Access File (plain TIFF output of a `convert` invocation, no packet). A JPEG is almost always an Access File but the format alone does not assert the stage — a file's stage is the outcome of the operator's intentional CLI invocation at creation time, recorded in the file's metadata, and verified per-stage at read time.

The three stages, with their distinctive properties:

| Stage | Purpose | Format set | Identity packet | Preservation metadata | Created by |
|------|---------|------------|-----------------|-----------------------|------------|
| **Preservation File** | Long-term bit-level preservation. The ultimate authoritative copy from which all downstream files derive. Stored in OAIS-compliant external archive. SIPI server does not read these. | Plain (non-pyramidal) lossless TIFF per archival policy. | None (in this ADR's scope; future ADR-0012 defines preservation-metadata schema). | Rich: rights statement, complete provenance chain, full PREMIS event ledger, custodial history — propagated through XMP per ADR-0011. | Future `sipi convert preservation-file <in> <out>` subcommand (out of scope for DEV-6537; awaits future work). |
| **Service File** | High-quality mezzanine baseline. Read by SIPI server to fulfill IIIF requests. Derived from a Preservation File or directly from a source. | Pyramidal TIFF or JP2 (the formats optimized for random-access IIIF serving). | Essentials packet (SIPI-specific: identity + image shape + future file-structure offsets per ADR-0004, ADR-0005). | Subset of preservation metadata that survives Preservation → Service conversion (rights statement, top-level provenance, IIIF-relevant identifiers) — propagated through XMP. | `sipi convert service-file <in> <out>` subcommand. |
| **Access File** | End-user delivery: web display, streaming, download. Highly compressed; not preservation-grade. | Any format the operator or IIIF client requests (JPEG, PNG, plain TIFF, JP2, etc.). | None — Access Files do not carry an Essentials packet. | Same subset as the Service File they derive from, plus an IIIF-server-emitted provenance event for the transformation that produced this Access File — propagated through XMP per ADR-0011. | (a) `sipi convert access-file <in> <out>` subcommand (offline batch; input must be a Service File). (b) The IIIF server, on every IIIF Image API request — server-mode is conceptually `convert access-file` over HTTP. |

We accept this because:

**1. Three stages, not two, match the operational reality.** The previous two-tier "Service master / Archival master" vocabulary left the IIIF-derivative concept implicit ("the thing the server returns"). Operators referred to it variously as "derivative," "representation," "rendered image," "IIIF output." Promoting it to first-class **Access File** makes the pipeline explicit at every layer — code, CLI, docs, conversations. The same term names the file in every context where it appears.

**2. "Master" carries inclusive-language baggage.** The digital-preservation community (LoC, BL, BnF, Wellcome, Archivematica) has been moving toward "preservation copy / access copy" terminology specifically to avoid the master/slave association, regardless of the current usage's intent. SIPI joins that move once, deliberately, instead of incrementally renaming later.

**3. The stage is established by intent, not inferred from format.** A pyramidal TIFF or JP2 is a Service File *if and only if* it was created by `sipi convert service-file` and carries an Essentials packet. The same format, produced by plain `sipi convert`, is an Access File without an Essentials packet. The discriminator is the packet, not the bytes around it. This makes stage assignment fully owned by the SIPI codebase — the file's stage can never be ambiguous; either it has the packet or it doesn't.

**4. The pipeline has a defined direction.** Preservation File → Service File → Access File is the canonical workflow. Each step is intentional, lossy or lossless per stage, and discoverable from the file itself (via XMP provenance events plus the Essentials packet's presence). Reverse-direction operations (deriving a Service File from an Access File) are non-canonical and should fail or warn.

## Considered Options

- **Keep "Service master / Archival master" two-tier vocabulary** — rejected. The IIIF-derivative concept (which we now call Access File) is the most-discussed pipeline stage in SIPI conversations and the least named. The vocabulary gap leaked into code as "the output," "the response," "the served image" — never with a stable term. Promoting it to first-class is a one-time docs-and-code cost that pays back at every documentation request and onboarding session.

- **Adopt the library-community two-tier "preservation copy / access copy"** — rejected. Two-tier doesn't capture the mezzanine character of the Service File: an intermediate baseline that IIIF processing operates from. In two-tier vocabulary, the Service File is either a "preservation copy" (wrong — pyramids and JP2 lossy compression aren't preservation-grade) or an "access copy" (wrong — it's not what the end user receives; it's what the server reads). Three-tier names what we actually have.

- **Drop the "master" terminology entirely without proposing a replacement** — rejected. Operators need vocabulary; ad-hoc naming ("file," "image," "the JP2") is overloaded. Specifying the replacement is half the cost of the rename.

- **Use OAIS package terminology (SIP, AIP, DIP)** — rejected. OAIS packages are containers of metadata + content for ingest, archival, and dissemination. SIPI deals with individual files, not packages. Borrowing SIP/AIP/DIP for our pipeline stages invites confusion with OAIS implementations (Archivematica, RODA) that use those terms with their original meaning.

- **Use broadcast-industry "tape master / mezzanine / proxy" vocabulary** — partially adopted. "Mezzanine" is exactly the right semantic for our Service File and informs the definition. But "tape master" and "proxy" are industry-specific and would obscure the preservation-domain intent (Preservation File) and the user-facing intent (Access File). We borrow the *concept* (a high-quality intermediate baseline) without the *vocabulary*.

## Consequences

- **`CONTEXT.md` is the source of truth for the taxonomy.** Glossary entries `Preservation File`, `Service File`, `Access File`, and `Preservation pipeline` (the architectural umbrella). The existing entries `Service master` and `Archival master` are removed with a brief redirect note in the "Flagged ambiguities" section noting the rename.

- **CLI subcommand surface uses stage names as verb objects:** `sipi convert service-file <in> <out>` produces a Service File; `sipi convert preservation-file <in> <out>` (future) produces a Preservation File; `sipi convert access-file <in> <out>` produces an Access File from a Service File; bare `sipi convert <in> <out>` is a generic ImageMagick-style conversion producing an Access File. The verb-noun pattern makes the operator's intent explicit. See ADR-0010 for the intentional-creation principle this CLI shape enforces.

- **The IIIF server is conceptually `convert access-file` over HTTP.** Server mode reads a Service File, applies IIIF Image API parameters (region, size, rotation, quality, format), and emits an Access File response. The same command logic and the same XMP-propagation rules apply on both surfaces (offline CLI and HTTP server). One operation, two invocation surfaces.

- **Existing files retain their meaning under the rename.** A file that today is called a "service master" is now called a Service File; same bytes, same Essentials packet, same on-disk position. The taxonomy change is purely terminological; no file conversion required. ADR-0005's legacy reader continues to handle pipe-delimited Essentials packets indefinitely.

- **ADR-0004 and ADR-0005 are amended to use the new terminology.** Both ADRs use "service master / archival master" extensively. The terminology sweep happens as part of the DEV-6542 docs-only PR ahead of the substantive ADR rewrites in DEV-6537's implementation phase. No semantic changes to either ADR.

- **The archive document `docs/archive/2026-05-08-modularization-analysis.md` is not rewritten.** That document is a snapshot of the analysis on the date it was written. A one-line forward-reference at the top points readers to this ADR for current terminology.

- **DSP-platform-wide alignment.** Other DaSCH platform components (dsp-ingest, dsp-api, dsp-app) currently use "master" terminology in their own codebases. Migrating those is out of scope for SIPI's DEV-6537 work but flagged as a future cross-repo coordination item. The dsp-ingest migration (tracked under DEV-6541) coincides with the SIPI subcommand rename, providing the natural moment to update dsp-ingest's terminology too.

- **Glossary distinguishes pipeline stage from file format.** Operationally, stage is the *intent* and format is the *encoding*. The glossary's *Preservation pipeline* entry makes this distinction explicit and points to the three stages' individual entries.
