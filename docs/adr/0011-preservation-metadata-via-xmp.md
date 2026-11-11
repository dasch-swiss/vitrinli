---
status: proposed
---

# Preservation metadata propagates via the Embedded metadata (XMP) channel

Rights statements, provenance assertions, PREMIS event chains, and other long-term preservation metadata travel through SIPI's existing Embedded-metadata channel (specifically XMP, with IPTC and EXIF carrying their conventional subsets). The Essentials packet stays scoped to SIPI-internal concerns — technical identity, image shape, future file-structure offsets — and does not grow to carry preservation-metadata fields.

The Embedded-metadata channel is the propagation path SIPI already operates: on read, format handlers extract EXIF / IPTC / XMP / ICC into the `SipiImage` value type; on write, format handlers serialize those payloads via each format's native carrier (JPEG XMP marker, TIFF `XMLPACKET` tag, JP2 UUID box, PNG iTXt chunk). All four formats inherit propagation for free. Future preservation-metadata fields (rights, provenance, PREMIS events) ride this channel by being valid XMP — using established schemas (XMP-Rights, XMP-PLUS for licensing, XMP-PROV, C2PA-via-XMP, PREMIS-XMP) maintained by external standards bodies — and propagate end-to-end without any new SIPI code.

The chain across the three roles (per ADR-0009):

- **Preservation File**: rich XMP — full PREMIS event chain, complete custodial history, original rights statement, all provenance assertions back to acquisition. Schema details specified in future ADR-0012 (preservation file specification).
- **Service File**: subset of preservation XMP that survives Preservation → Service conversion — rights statement, top-level provenance, IIIF-relevant identifiers. The Service File's Essentials packet adds SIPI-internal fields on top; the two coexist in the file without overlap.
- **Access File**: same XMP subset as the Service File it derived from, plus a SIPI-emitted provenance event for the IIIF transformation that produced this Access File. No Essentials packet (per ADR-0009).

We accept this because:

**1. The propagation infrastructure already exists.** SIPI reads EXIF / IPTC / XMP / ICC from input today (`SipiImage` has dedicated member fields for each) and writes them on output via format-native carriers (JPEG marker, TIFF tag, JP2 UUID box for XMP, PNG iTXt). The end-to-end chain works for every input/output format combination SIPI supports. Adding rights/provenance fields to the Essentials packet would require parallel infrastructure: a translation layer that reads SIPI-specific Essentials fields and re-emits them as JPEG XMP / PNG XMP / etc. for Access Files — duplicating the existing XMP path.

**2. Tooling support is universal for XMP.** ExifTool, Adobe Bridge, jpylyzer, Apache Tika, Archivematica, every digital-preservation tool reads XMP. The Essentials packet is SIPI-private; only SIPI can decode it. For rights/provenance — fields that operators, archivists, lawyers, and downstream systems all want to read — encoding them in a SIPI-private format defeats the purpose of having them in the file.

**3. Schema authority lives outside SIPI.** XMP-Rights is maintained by Adobe + ISO. XMP-PROV is maintained by W3C. C2PA is maintained by an industry consortium. PREMIS-XMP is maintained by the Library of Congress. SIPI inherits the schema versioning, validation, and interoperability work done by those bodies without owning any of it. The Essentials packet's schema discipline (ADR-0005's field-number-as-contract) is right for SIPI-internal fields but wrong for cross-organizational rights/provenance — those need authority that outlasts SIPI's release cadence.

**4. The Essentials packet stays scoped.** Today's Essentials packet carries technical identity (origname, mimetype, hash, ICC profile bytes) and image shape (8 fields). DEV-6442 will add file-structure offsets in the reserved 16-31 field range. Adding rights / provenance / PREMIS would either explode the packet's scope (and the wire format's compile-time cost) or require a sub-message that becomes its own ad-hoc preservation-metadata format. Keeping the packet narrow keeps its role discoverable and its schema discipline workable.

**5. Access Files behave consistently across formats.** Today's rule "Access Files carry standard EXIF / XMP / IPTC but no Essentials packet" is clean. If preservation metadata went into Essentials, Access Files would need to either (a) acquire an Essentials packet (contradicts ADR-0009) or (b) get the data translated to XMP at emission time per output format (new translation code, four output formats). Routing rights/provenance through XMP from the start sidesteps the choice.

## Considered Options

- **Extend the Essentials packet with rights / provenance / PREMIS fields** — rejected for the five reasons above. Specifically rejected for the four-format-translation-layer cost: every Access File emission path (JPEG marker, TIFF XMLPACKET, JP2 UUID box, PNG iTXt) would need new code to translate Essentials → format-native XMP. SIPI already emits XMP through each of those carriers; we'd be reinventing the wheel one pipeline stage at a time.

- **Hybrid: write to XMP and to Essentials packet** — rejected. No clear benefit (XMP propagation doesn't fail for typical reads); doubles the maintenance surface (schema changes touch both); risks the two channels going out of sync on schema evolution; and adds verification complexity (which channel wins if they disagree?).

- **Use a SIPI-specific preservation-metadata packet alongside the Essentials packet** — rejected. Same translation-layer cost as extending Essentials; plus a second SIPI-private format that downstream tooling can't read. The argument "XMP is the wrong schema authority for our specific needs" is not borne out — XMP-Rights and PREMIS-XMP cover all the use cases ADR-0012 will need to address.

- **Defer the decision until ADR-0012 (preservation file specification)** — rejected. The choice of channel affects code SIPI is shipping right now: whether the format-handler writers need a translation layer, whether the Essentials packet's reserved field range needs to accommodate rights/provenance fields, whether the `convert service-file` command constructs the rights/provenance data structure or just passes XMP through. Committing to XMP at ADR-0011 frees ADR-0012 to focus purely on schema selection (which XMP namespaces and which PREMIS subset).

## Consequences

- **The Essentials packet's schema (per ADR-0005) does not change for preservation metadata.** Reserved field range `16-31` remains earmarked for DEV-6442's file-structure offsets. No rights / provenance / PREMIS fields are added to `essentials.proto` now or in ADR-0012.

- **Future ADR-0012 (preservation file specification) specifies which XMP namespaces SIPI requires.** Candidate namespaces include XMP-Rights (`http://ns.adobe.com/xmp/rights/`), XMP-PLUS (`http://ns.useplus.org/ldf/xmp/1.0/`), XMP-PROV / C2PA-via-XMP, PREMIS-XMP. ADR-0012 enumerates the required fields per stage (Preservation File: full set; Service File: documented subset that survives the conversion). DEV-6537's implementation needs only to preserve whatever XMP is in the input, not to validate or enrich it.

- **DEV-6537's implementation phases do not add new metadata-propagation code.** The existing format-handler writers' XMP emission paths are sufficient. The `convert service-file` command explicitly verifies that `SipiImage`'s XMP is not stripped between read and write; nothing more.

- **`CONTEXT.md`'s existing entries remain accurate.** `Preservation metadata = Embedded metadata ∪ Essentials packet` continues to hold, with this ADR clarifying that *rights / provenance / PREMIS* fields are in the **Embedded metadata** side of the union, not the Essentials packet side. The relationships diagram (line 167) is unchanged.

- **The IIIF server emits its own provenance event into Access File XMP.** Conceptually: "this Access File was produced by SIPI version X from Service File Y on date Z by applying region R, size S, rotation θ, quality Q." This is a SIPI-side enrichment of the propagated XMP, written into XMP-PROV before the format-handler writer serializes XMP for the output format. Implementation is a DEV-6537 follow-up (under the `convert access-file` / IIIF-server shared-command work).

- **Verification of preservation metadata is per-stage and explicit.** `sipi verify service-file <file>` confirms the Essentials packet plus presence of the documented XMP subset. `sipi verify preservation-file <file>` (future) confirms the richer required XMP set per ADR-0012. Bare `sipi verify <file>` is the stage-agnostic "can SIPI read it?" check (decoder coverage, no metadata assertions).

- **Cross-language readiness inherited from XMP.** When dsp-repository's Rust IIIF service ships (the cross-language consumer scenario that drove the protobuf choice in ADR-0005), it reads XMP via standard Rust crates (e.g. `xmp_toolkit`, `kamadak-exif`). No SIPI-specific decoder, no protobuf-via-Rust schema drift. The cross-language case is *better* served by XMP than by Essentials-packet extension.

- **Future preservation-metadata fields can be added without touching the Essentials packet's wire format.** Adding a new field to the XMP subset Service Files preserve (e.g., a new PREMIS event type) is an XMP namespace change, not a SIPI schema migration. No `format_version` bump in `essentials.proto`; no per-version reader.
