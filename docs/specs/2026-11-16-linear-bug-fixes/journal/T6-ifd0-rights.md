---
plan: docs/specs/2026-11-16-linear-bug-fixes/01-fix-linear-bugs-plan.md
unit: ifd0-rights
unit_state: done
curated_from: df851e1980d9db07152e6225a5d3008ccd7679e2
started_from: e19060ca65ace8291238d0d9ab7bd82e952e7f99
final_state: closeout
branch: claude/project-thread-3jo7cz
linear: DEV-7607
---

# T6 ifd0-rights journal

## Repos

| repo | branch | pr |
|---|---|---|
| dasch-swiss/vitrinli | claude/project-thread-3jo7cz | https://github.com/dasch-swiss/vitrinli/pull/850 |

## Phases

| phase | state | review_fix_rounds |
|---|---|---|
| 7 Keep rights metadata held only in IFD0 copies | reviewed | 1 |

## Chunk queue

1. `metadata`: `Exif::value_bytes`, `Xmp::parse_complete`, `Iptc::parse_complete` (the "parses whole" checks).
2. `image`: `SipiImage::promote_ifd0_copies()`, called by `SipiImage::write`; `formats_test` cases `Ifd0CopyPromotion.*`.
3. `ffi`: `rewrite_restricted_metadata` promotes first; `serve_image_test` restricted and watermarked case.

## Chunks

| chunk | commit | tests |
|---|---|---|
| 1 metadata checks | bc3423a | none of its own; covered by chunk 2. The IIM walk and the expat check were run in a scratch harness against system Exiv2 0.27 and expat (valid, truncated, garbage, prefixed, all-zero IPTC; valid with trailing NULs, broken, DOCTYPE, RDF-less XMP) |
| 2 promotion in write | 326d329 | `formats_test` `Ifd0CopyPromotion.*` added; not run (Bazel cannot fetch in the cloud container), CI runs it |
| 3 restricted rewrite | c692ec4 | `serve_image_test` case added; not run, CI runs it |

## Owner decisions

- XMP "parses whole" check: expat (well-formed, rdf:RDF present, no DOCTYPE) instead of the plan's "Exiv2 rejects the packet", because Exiv2's XmpParser is not thread-safe at the pinned version and nothing in production calls it. answered by Ivan 2026-11-16: expat, card cmsg_01XC4b1mTKe7ziDJYWR1rUNVFhTS8rhx8MbePumGosF2jW.

## Deviations

- Branch name: the session assigned `claude/project-thread-3jo7cz`, not the plan's `impl/bugs-ifd0-rights` (allowed by the plan, Cross-cutting, Stacking without `gh stack`).
- Journal path: the project instructions name `journal/T<n>-<slug>.md` in the plan folder, which wins over the skill's `<plan-name>-<unit>-journal.md`.
- Stacked on `claude/project-thread-48t4wn` (deadline), the top at stack time; the rebase was clean.

- IPTC "parses whole" check: Exiv2's IptcParser::decode returns success on garbage and on a truncated block, so it cannot reject one. `Iptc::parse_complete` walks the IIM datasets itself and also requires Exiv2 to decode as many.
- Promotion also runs at the start of `rewrite_restricted_metadata` (`serve_image.cpp`), not only in `SipiImage::write`: the restricted rewrite runs before `write`, so a block promoted only in `write` would escape the preview drop and size rewrite the plan requires. Idempotent, so `write` then does nothing.
- Commit scopes: `metadata`, `image` and `ffi`, one commit per module touched; the plan's table names only `metadata`.
- Docs: one sentence in `CONTEXT.md` (Embedded metadata) and one testing-strategy row; the plan names neither.
- The "fails with the change reverted" check for the new tests was not run: Bazel cannot fetch in the container.

## Review

Round 1 (cpp-reviewer, consistency-reviewer, sonnet; cut from the full set for a ~300-line metadata diff: cpp covered thread safety and Exiv2 API use, consistency covered comments, docs and plan coverage). Neither could compile or run anything.
- Fixed: `exif.h` `restrict_image_resources` comment now says the copies reach an output as the promoted XMP and IPTC (fixup into the metadata commit).
- Fixed: `ACopyThatDoesNotParseWholeIsNotPromoted` built an EXIF holding only the two copies, which `exifBytes()` encodes to nothing; the JPEG reader could then fail on an empty APP1 EXIF. The EXIF now also holds a Copyright.
- Fixed: the `serve_image_test` fixture splices APP1 after the JFIF APP0, not before it.
- Not taken: the PNG and JP2 re-read checking the rights by substring; `server.rs::restricted_output_keeps_rights_metadata_verbatim` already shows `dc:rights` survives byte for byte in all four formats.
- Not taken: tests for only one copy present or one copy parsing; the two promotions are independent branches with the same shape.

## Deferrals

- XMP or IPTC held only inside the EXIF IFD0 Photoshop image resources (`Exif.Image.ImageResources`, resources 0x0424 and 0x0404) are still not promoted; `exifBytes()` drops the block when only IPTC is left in it. Outside DEV-7607's two keys; needs its own issue (cpp-reviewer suggestion).

## What went well

## What went wrong

## Final review

- 2026-11-16, final unit (whole stack against `main`, 8 reviewers, skeptic triage): `Xmp::parse_complete` now parses as UTF-8 like `split_extended`, so a copy the JPEG writer cannot split is not promoted, and it compares the cached `rdf:RDF` name instead of allocating per element (fixup into the `metadata` commit). Deferred at first, then fixed on the owner's request: a rejected copy logs one warning per kind per process, later ones at debug. Promotion still runs on every write (twice for a restricted request); a per-image flag was rejected because `SipiImage` copies and moves field by field in four hand-written members and `set_exif` would have to reset it, for a saving of one parse of a bad copy.
- 2026-11-16, final unit, CI: the first CI run of `ifd0_copy_promotion_test` failed for PNG. The PNG reader took an iTXt chunk's length from `text_length`, which libpng leaves 0 for iTXt, so every PNG read had an empty XMP packet (a bug on `main`). Fixed in its own `format_handlers` commit in this layer.

## Closeout

- root_cause: some writers keep XMP and IPTC only as raw copies in EXIF IFD0 (`XMLPacket`, `IPTCNAA`); Vitrinli wrote neither as the primary block, so the rights in them never reached the output, and the restricted rewrite never saw them.
- investigation: Exiv2's XmpParser is not thread-safe at the pinned version, so the completeness check uses expat (owner answered: expat).
- solution: `Exif::value_bytes`, `Xmp::parse_complete`, `Iptc::parse_complete`, and `SipiImage::promote_ifd0_copies`, called by `write()` and before `rewrite_restricted_metadata`.
- prevention: `ifd0_copy_promotion_test` and a restricted-output `serve_image_test` case.
