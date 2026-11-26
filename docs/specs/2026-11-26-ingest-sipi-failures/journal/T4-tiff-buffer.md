---
plan: docs/specs/2026-11-26-ingest-sipi-failures/01-fix-ingest-sipi-failures-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: a7b9dd8
branch: claude/project-thread-5pkqyn
started: 2026-11-26
unit: tiff-buffer
unit_state: done
curated_from: d87f754
started_from: 491e9a9
problem: >
  DEV-7236 / DEV-8092: `sipi convert` is OOM-killed on 2-4 GB TIFFs in the
  rdu-13 ingest. Part of the cause: SipiIOTiff::read holds two full-size
  pixel buffers at once (the read_*_data result vector and the buffer
  handed to set_pixels, joined by a memcpy).
symptoms:
  - kernel OOM kills of `sipi convert --format jpx` in dsp-ingest (2048 MiB cap) on rdu-13
status: complete
---

# Execution Journal: 01-fix-ingest-sipi-failures, unit tiff-buffer

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | 491e9a9 | claude/project-thread-5pkqyn | rebase | in-progress | https://github.com/dasch-swiss/vitrinli/pull/861 |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 4 | reviewed | vitrinli@a7b9dd8 | 1 |

## Chunk queue

- 4.1 TIFF reader decodes into the set_pixels buffer; pixel and peak-heap tests; SipiPeakMemory.h comment check

## Chunks

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 4.1 | vitrinli | complete | 1e98d97 (curated into 210b9db) | TIFF reader decodes into the set_pixels buffer; pixel and peak-heap tests | none |
| 4.2 | vitrinli | complete | dc48729 (curated into 210b9db) | review fixes: stale `inbuf` comments, fuzz-harness budget comment, docs scope, test cleanup, DEV-8115 note | none |

## What went well

- A standalone harness (g++ 13 + system libtiff + ASan/UBSan, outside Bazel) ran the old and new `read_standard_data`/`read_tiled_data` side by side on the test's synthetic TIFFs: output equal for every layout, and the caller's peak heap fell from 2.00x to 1.00x of the pixel buffer for a 64 MiB 16-bit RGB TIFF.

## What went wrong

- Bazel cannot build in the cloud container, so the new `tiff_read_buffer_test` itself runs first in CI.

## Deferrals

- Phase 4: the hot-path before/after `just bench` + `just bench-compare` (`decode_benchmark`, `-c opt`) is the plan's human action H3; all reviewers flagged its absence. Bazel cannot build here. Whether the `decode_benchmark` TIFF fixtures (`pyr-*.tif`, `flat.tif`, from `@sipi_bench_fixtures`) include a 16-bit image is not known here; H3 should check.
- Phase 4: `ARCH-MAP.md` format_handlers fingerprint refresh (dune reviewer) is the final unit's ship step (`/dune:dune-map update`).
- Phase 4: REVIEW.md "Bazel test parity" still says new tests go in `test/unit/<mod>/`; tests are co-located per ADR-0003. Not this unit's file; for the final review.

## Deviations

- Phase 4: stacked on `claude/project-thread-nrl1bz` (exif-charset) at 491e9a9 by plain `git rebase --onto`; no conflicts, the two units touch disjoint files, so the phase review was not re-run on the rebased commits (their diff is unchanged).
- Phase 4: branch name. The plan's table names `impl/ingest-tiff-buffer`; this session is assigned `claude/project-thread-5pkqyn`, which the plan allows (Cross-cutting, Stacking without `gh stack`).
- Phase 4: journal path. The skill puts the journal at `<plan-dir>/<plan-name>-<unit>-journal.md`; the project instructions put it at `<plan-dir>/journal/T<n>-<slug>.md`. The project instructions win: `journal/T4-tiff-buffer.md` (T4 = Phase 4).
- Phase 4: no `gh stack init`. `gh stack` is not in the cloud container; owner decision 2026-11-16 (plan, Cross-cutting).
- Phase 4: the peak-heap test calls `SipiIOTiff::read` directly, not `SipiImage::read`, so it bounds the TIFF reader that REQ-5 names and not the format dispatch around it.

## Side findings

- Phase 4: pre-existing bug, not fixed (out of scope). `read_tiled_data` places tile pixels at `(tx - starttile_x) * tile_width + tile_x` and never subtracts `roi_x % tile_width` (same for y). A region on a tiled TIFF whose origin is not on a tile boundary returns pixels shifted by that remainder. The harness shows it on main and on this branch (1530 of 1530 samples wrong for region 3,5,30,17 with 16x16 tiles). The pixel test uses a tile-aligned region. Filed as DEV-8115 (Triage), cited at the test.
- Phase 4: `SipiPeakMemory.h` makes no claim about the TIFF-internal copy; no change needed.

- Phase 4: the planar-separate strip paths still peak at three full-size buffers (`out`, the local planar vector, the `separateToContig` result). The base peaked at three too (caller buffer, callee buffer, `separateToContig` temp), so no regression; out of scope per the plan. REVIEW: a consistency reviewer called it a regression; refuted by tracing the base (the caller allocated its buffer before the call).
- Phase 4: the `out.size()` guard in both readers is reachable from file input: the caller sizes the buffer with `img->getNc()` from IFD0, while the reader takes SamplesPerPixel from the current directory, which for a pyramid level is another IFD. So `kMalformedInput` is right; two reviewers' "defense in depth" finding was dropped (the plan also asks for the check).

## Review (phase 4)

- Reviewers (sonnet): cpp, performance, consistency, code-simplicity, dune, reference, devops. No Critical.
- Fixed: stale `inbuf` names in comments; `codec_fuzz_harness.h` comment claimed two TIFF buffers; developing.md claim scoped to contiguous images; peak test deletes its 64 MiB file; ASan note in the test header; DEV-8115 cited at the aligned tiled region test.
- Dropped after checking: planar regression (see Side findings), size guard as defense in depth (reachable), allocator scaffolding size (the plan names this method), `reinterpret_cast` aliasing (same access as `SipiImage::getPixel`; ASan+UBSan harness clean).

## Owner decisions

- none

## Closeout

- root_cause: `SipiIOTiff::read` held two full-size pixel buffers at once: the `read_*_data` result and the buffer handed to `set_pixels`, joined by a `memcpy`.
- investigation: a standalone g++ 13 harness compared old and new readers on the test's TIFFs (equal output) and measured the caller's peak heap (2.00x to 1.00x of the pixel buffer). It also found DEV-8115 (tiled region shift, on main).
- solution: the contiguous 8- and 16-bit strip and tile paths decode into a `std::span<T>` over the one `std::vector<byte>` that `set_pixels` receives; the planar-separate paths keep their copy.
- prevention: `tiff_read_buffer_test` (pixels per layout, peak heap below 1.5x the buffer).
- final review (2026-11-27): one fixup, the two ROI-offset comments state the invariant instead of the earlier bug. Still open: H3 benchmark on Ivan's Mac.
