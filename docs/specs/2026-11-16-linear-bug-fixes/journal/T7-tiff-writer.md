---
plan: docs/specs/2026-11-16-linear-bug-fixes/01-fix-linear-bugs-plan.md
unit: tiff-writer
unit_state: done
curated_from: c85c387ac89038863b767cc1ec51a735224cfa01
started_from: 435496747f40c32d73d01927d4249107bdf63801
branch: claude/project-thread-lgu0i4
linear: DEV-7621, DEV-6214
---

# T7 tiff-writer journal

## Repos

| repo | branch | pr |
|---|---|---|
| dasch-swiss/vitrinli | claude/project-thread-lgu0i4 | https://github.com/dasch-swiss/vitrinli/pull/848 |

## Phases

| phase | state | review_fix_rounds |
|---|---|---|
| 8 TIFF writer fixes | reviewed | 0 |

## Chunk queue

1. DEV-7621: `SipiIOTiff::write` sets `TIFFTAG_YRESOLUTION` from `Exif.Image.YResolution`, with a round-trip case in `tiff_exif_rational_test.cpp`.
2. DEV-6214: move `memTiffOpen` out of the `extern "C"` block, unchanged and `static`.

## Chunks

| chunk | commit | tests |
|---|---|---|
| 1 YResolution tag | 5e3aa2a | `TiffExifRational.DistinctXAndYResolutionRoundTrip` added; not run locally (Bazel cannot fetch in the cloud container), CI runs it. Without the fix it fails: XResolution reads 150, YResolution reads 0 |
| 2 memTiffOpen linkage | 256753b | no behaviour change; CI compiles it |

## Owner decisions

## Deviations

- Branch name: the session assigned `claude/project-thread-lgu0i4`, not the plan's `impl/bugs-tiff-writer`. The plan allows this (Cross-cutting, Stacking without `gh stack`).
- Journal path: the project instructions name `journal/T<n>-<slug>.md` in the plan folder, which wins over the skill's `<plan-name>-<unit>-journal.md`.

- Approval goldens: libtiff writes X and Y resolution as one field, so the old writer emitted YResolution 0/1, and the five TIFF-output `ImageEncodeBaseline` goldens change by those 8 bytes. Found by the extended-xmp thread on its CI run (#849), after this unit was stacked. Re-approved by byte-patching the YResolution value (all sources have X equal to Y), with a `CHANGELOG.approval.md` row, folded into the fix commit. A first `fixup!` commit on top failed `commit-lint`, so the branch was rewritten with `--force-with-lease` (tree equal to the fixup head) and the extended-xmp thread was told to rebase onto the new head.

## Review

Round 1 (cpp-reviewer, consistency-reviewer, sonnet; the reviewer set was cut to two for a 3-line code change and one test: cpp covered correctness, simplicity and the performance-irrelevant metadata path, consistency covered references and docs). No Critical, Warning or Suggestion needing a fix.
- Checked: no other writer sets TIFF resolution (`writeExif` does not), so the round-trip test cannot be masked; PNG and J2k resolution code is correct.
- Wrong: the review concluded no golden changes, from the metadata text goldens only. The five `ImageEncodeBaseline` TIFF goldens change (see Deviations).
- Not taken: adding the test to `testing-strategy.md`; that matrix lists no individual format-handler test files.
- Hot-path benchmark: the change is metadata-only; the before/after `just bench` stays with H6, as the plan says.

## Deferrals

## What went well

## What went wrong

- The phase review missed the approval-golden change: it checked only the metadata text goldens, not the binary TIFF goldens, and no Bazel run was possible to catch it. Lesson: for a TIFF writer change, parse the binary TIFF goldens' IFDs.

## Final review

- 2026-11-16, final unit (whole stack against `main`, 8 reviewers, skeptic triage): no finding in this layer.

## Closeout

- root_cause: the TIFF writer wrote `TIFFTAG_YRESOLUTION` from the X resolution, and `memTiffOpen` sat in an `extern "C"` block although nothing outside the file calls it.
- investigation: five approval goldens had YResolution 0/1 and were re-approved.
- solution: YResolution from the source YResolution; `memTiffOpen` keeps `static` with C++ linkage.
- prevention: `tiff_exif_rational_test` and the re-approved goldens.
