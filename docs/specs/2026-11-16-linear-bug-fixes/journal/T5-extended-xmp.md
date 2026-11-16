---
plan: docs/specs/2026-11-16-linear-bug-fixes/01-fix-linear-bugs-plan.md
unit: extended-xmp
unit_state: done
curated_from: 9eb656c37d7b24dcba2e30ea3d8ef3371017b718
started_from: 90404d3b3db61b620d3511af018fb4227ab6440a
branch: claude/project-thread-fet8jg
started: 2026-11-16
problem: >
  DEV-7606. The JPEG writer splits an XMP packet over one APP1 segment into
  StandardXMP and ExtendedXMP, but the JPEG reader reads only the StandardXMP,
  so Vitrinli loses the moved properties of a packet it wrote itself.
symptoms:
  - A JPEG with a large XMP packet loses the ExtendedXMP properties when Vitrinli reads it again.
status: complete
---

# Thread journal: T5 extended-xmp (Phase 6, DEV-7606)

## Repos

| repo | branch | pr |
|------|--------|----|
| vitrinli | claude/project-thread-fet8jg | #849 |

## Phases

| phase | state | review_fix_rounds |
|-------|-------|-------------------|
| 6 | reviewed | 1 |

## Chunk queue

- 6.1 `Xmp::merge_extended` in `src/metadata/cpp/xmp.{h,cpp}`
- 6.2 `SipiIOJpeg::read()` collects ExtendedXMP segments and merges them; tests in `jpeg_metadata_segments_test.cpp`

## Chunks

| id | status | commit(s) | summary | blocker |
|----|--------|-----------|---------|---------|
| 6.1 | complete | 59f9f9a | `Xmp::merge_extended`: join parts by GUID and offset, merge descriptions, drop HasExtendedXMP, fail closed | none |
| 6.2 | complete | 8b55c13 | `xmp_packet()` in both JPEG readers; round-trip and malformed-part tests | none |
| 6.R1 | complete | c9eacfa, b5688d0 | review fixes: see Review round 1 | none |

## What went well

- The existing expat `Scan` of `split_extended` carried the merge with four new fields; no second parser.
- `merge_extended` and the malformed cases were checked locally with g++ 13, ASan and UBSan against system expat, outside Bazel.

## What went wrong

- The first `Scan` built several `std::string`s per element; a hostile 16 MiB extension took 1.3 s. Measured, then fixed (0.35 s; expat itself dominates).

- Bazel cannot fetch dependencies in the cloud container, and libjpeg and libtiff headers are absent, so `SipiIOJpeg.cpp` and the gtest file were not compiled here. CI is their first build.

## Review round 1

Reviewers (sonnet): cpp, security, performance, consistency, simplicity, dune, reference. Each claim was measured or traced before a fix.

- Fixed: per-element allocations in `Scan` (path is now an enum stack, names are built once); the extension scan no longer collects properties.
- Fixed: nesting over 256 levels and a second rdf:RDF are refused (the second rdf:RDF let the note sit after the rdf:RDF end and corrupt the splice).
- Fixed: packets are parsed as UTF-8 only; a UTF-16 packet could not be spliced by byte offset.
- Fixed: an empty-element note no longer reads the rest of the packet as its GUID; the attribute GUID is trimmed like the parsed one; `xmlns` matching is exact.
- Fixed: `split_extended` now refuses an ExtendedXMP over 16 MiB, so the writer cannot write what the reader ignores.
- Fixed: the in-scope bindings rule is one helper shared by split and merge; overlap and gap are one check.
- Fixed: `xmp.h` class comment, ARCH-MAP metadata and format_handlers entries, testing-strategy row.
- Added tests: an ExtendedXMP of another writer (note as attribute on the main description and as a property); a split over 16 MiB fails.
- Refuted: "Scan::path costs depth x URI length" (security, Critical). Measured with a 100 KB URI and 5000 levels: 3 MB peak RSS, before the enum change.
- Not taken: verify the extension MD5 against the GUID (the plan names only offsets and length); refuse an extension that repeats a rights property of the StandardXMP (the file's author writes both packets); `starts_with` for the StandardXMP signature (the reader always matched it anywhere in the segment).

## Deviations

- Branch: the session assigns `claude/project-thread-fet8jg`, not `impl/bugs-extended-xmp`. The coordinator's brief allows this.
- Journal path: the project instructions put the journal at `journal/T<n>-<slug>.md`; the eng skill's default is `<plan-name>-<unit>-journal.md`. The project instructions win.

## Deferrals

- ARCH-MAP content fingerprints for metadata and format_handlers were not recomputed; run `/dune:dune-map update`.
- `read_shape()` constructs its `SipiImage` after `setjmp`, so a libjpeg error leaks its EXIF and IPTC. Pre-existing; out of this unit.

## Side findings

- Phase 6: the plan calls the 16 MiB cap "the JPEG reader's ceiling"; no such ceiling exists in the reader. The cap is a new constant in `xmp.cpp`.
- Phase 6: `read_shape()` read XMP into a local image and discarded it. See Owner decisions.

## Owner decisions

- 2026-11-16, answered: Skip XMP in read_shape (card cmsg_01XC4b1mTKe7ziDJYWR1rUNVV8FY8Q3TkxbMzcKTtuBimx): the plan says `read_shape()` calls the merge. `read_shape()` discards its XMP, and a merge there costs up to 0.35 s and can leak up to 16 MiB on a libjpeg error. Taken: `read_shape()` reads no StandardXMP or ExtendedXMP from APP1. Alternative: merge there too.

## Final review

- 2026-11-16, final unit (whole stack against `main`, 8 reviewers, skeptic triage): Critical, fixed: the scan copies the namespace bindings in scope into every description, so an untrusted ExtendedXMP with long namespace URIs could make one read allocate gigabytes; a URI over 512 bytes now stops the scan, and the merge checks that the parts add up before it reserves the declared length (fixups into the `metadata` and `format_handlers` commits, with a test). Learning: `docs/learnings/architecture/input-caps-do-not-bound-copied-parse-state.md`. Deferred: H6 benchmark of the JPEG read with ExtendedXMP.

## Closeout

- root_cause: the JPEG reader kept only the StandardXMP APP1 segment and dropped the ExtendedXMP segments, so a large packet lost every property moved into the extension.
- investigation: `read_shape` discards its XMP, so it reads none (owner answered: skip XMP in read_shape); other writers put `xmpNote:HasExtendedXMP` as an attribute or a property, and both are handled.
- solution: `Xmp::merge_extended` joins the parts by GUID and offset and merges the extension's descriptions with their bindings; any malformed input keeps the StandardXMP unchanged.
- prevention: `jpeg_metadata_segments_test` round trip, order, other-writer, malformed and oversized cases.
