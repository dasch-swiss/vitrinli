---
plan: docs/specs/2026-11-26-ingest-sipi-failures/01-fix-ingest-sipi-failures-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: a7b9dd8
branch: claude/project-thread-ct0viq
started: 2026-11-26
unit: query
unit_state: done
final_state: closeout
curated_from: 3d70087
started_from: 7e76379
problem: >
  DEV-8093: `sipi query` decodes every pixel and then prints only header and
  metadata fields, so valid 1.75-2.3 GB JPX files hit the 120 s decode deadline
  during the rdu-13 bulk ingest. `query` must read header and metadata only,
  with the same output.
symptoms:
  - "sipi query <file>.jpx: decode exceeded the 120000 ms deadline"
status: complete
---

# Execution Journal: 01-fix-ingest-sipi-failures-plan, unit query (T5)

The session assigns branch `claude/project-thread-ct0viq`; the plan's table names
`impl/ingest-query`. The plan allows the session name (Cross-cutting, Stacking
without `gh stack`).

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | 7e76379 | claude/project-thread-ct0viq | rebase | in-review | [#865](https://github.com/dasch-swiss/vitrinli/pull/865) |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 5 | reviewed | vitrinli@a7b9dd8 | 1 |

## Chunk queue

- 5.1 `SipiIO::read_metadata` (pure virtual), `SipiImage::read_metadata` sharing `read`'s dispatch, the four handler overrides sharing their `read` code
- 5.2 metadata-only deadline read in `decode_deadline.{h,cpp}`, `run_query` uses it, unit test
- 5.3 equality test over `test/_test_data/images/unit/`, unknown-format error equality
- 5.4 e2e: `sipi query` on a JPX converted from `cmyk.tif`
- 5.5 `decode_benchmark.cpp` format coverage; docs

## Chunks

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 5.1 | vitrinli | complete | 060d9ab | `SipiIO::read_metadata`, `SipiImage::read_metadata` over a shared `read_dispatch`; the four handlers share `read_image(..., decode_pixels)`; equality test; PNG decode benchmark | none |
| 5.2 | vitrinli | complete | e7a9f43 | `read_metadata_with_deadline`, `run_query` uses it, unit test, help text and guide pages | none |
| 5.3 | vitrinli | complete | 060d9ab | equality test over images/unit + bilevel + CMYK JPEG; unknown-format error equality (in 5.1's commit) | none |
| 5.4 | vitrinli | complete | c8ebb00 | e2e `query_jpx_header` in `cli_conversions` | none |
| 5.5 | vitrinli | complete | 060d9ab, e7a9f43 | `decode_full_png` benchmark case; docs | none |

## What went well

- CI (workflow_dispatch of `ci.yml` on the branch, run 45908292608 at cf302f8)
  was green on all three legs, the ASan/UBSan job, the Docker smoke test,
  rustfmt and clippy; the J2K equality cases and the e2e JPX case ran there.

- Bazel cannot fetch here, but a standalone g++ 13 build of the TIFF, JPEG and
  PNG paths ran the equality and deadline tests locally: system libtiff,
  libjpeg-turbo, libpng, lcms2, libmagic, protobuf from apt, and Exiv2 0.28.9
  unpacked from the Ubuntu archive `.deb` (archive.ubuntu.com is reachable;
  GitHub archives are not). J2K needs Kakadu, so its cases run only in CI.
- A mutation check (dropping the palette header steps) failed the equality
  test for palette.tif and mario.png, so the test guards drift.
- A metadata read of the 4872x3864 cmyk.tif took about 1 ms.

## What went wrong

- The format handlers needed a reindent of the TIFF pixel block under
  `if (decode_pixels)`. The tiff-buffer unit rewrites the same block, so the
  later of the two units resolves a conflict there at stack-on.
- Stack-on onto #864 (7e76379) had three conflicts. TIFF pixel block: the
  span-buffer decode of tiff-buffer now runs inside `if (decode_pixels)`, and
  the decode buffer is allocated only there, so `query` allocates no pixel
  buffer. `run_query`: keeps the `set_json_mode` call of cli-errors.
  `formats_test` comment: both test lists joined. One silent overlap: the
  moved PNG palette `addEs` line duplicated the alpha rule kakadu-errors sets
  from the decoded layout, so the line is gone. The 32 local tests pass after
  the rebase. `png_alpha_layout_test` does not run locally (its JPX write needs
  the J2K handler, which the local registry lacks); CI runs it.
- CI on the curated head (99e70eb) failed `formats_test` once on
  `unit_CIELab16_tif`: the printed `ICC-Date` differed by one second. lcms
  creates the CIELab profile during the read with the wall-clock time, and
  `SOURCE_DATE_EPOCH` only normalizes `iccBytes()`, not `operator<<`. The
  equality test now compares the output without the `ICC-Date` line. Folded
  into the handler commit.
- The review request on #865 fails: GitHub refuses a review from the PR
  author (subotic). The PR is assigned to subotic.

## Deferrals

- Phase 5: the equality test has no palette JP2 and no 12-bit J2K fixture, so
  the J2K metadata branch's palette and 12-to-16-bit steps are checked by
  review only. `images/unit/` holds neither kind; building one needs Kakadu,
  which the cloud container lacks. Follow-up for a host with Bazel.

## Deviations

- Phase 5: the plan asks to build a missing fixture kind in the test. The 1-bit
  TIFF and the CMYK JPEG come from the existing fixtures in
  `images/bilevel/` and `images/jpeg/cmyk/` instead, which the formats test
  already ships as data. Commit 060d9ab.
- Phase 5: the plan names "a unit test next to the existing one" for
  `decode_deadline`; there was none. A new `decode_deadline_test.cpp` joins
  `commands_test`. Commit e7a9f43.
- Phase 5: the plan places the JPX e2e case next to `cli_query_dumps_image_info`
  in `cli.rs`. It runs in `cli_conversions.rs`, whose header reserves the
  Kakadu encodes, so `cli` stays light. Commit c8ebb00.
- Phase 5: `read_metadata` skips the checks that need pixels (TIFF palette
  index range, J2K YCbCr `nc < 3` in `convertYCC2RGB`, corrupt scan data). A
  file that fails a full read on its pixels passes `query`. Stated in
  `json-output.md`. Commits 060d9ab, e7a9f43. The phase review found two more
  cases of the same kind, accepted on the same ground (the ingest's `convert`
  still runs the full read and fails them): a 16-bit palette TIFF, which a full
  read rejects at `set_pixels`, and checks that libjpeg runs in
  `jpeg_start_decompress` but not in `jpeg_calc_output_dimensions`.
- Phase 5: the `query` help text changed from "Dump image information." to
  "Print image header and metadata (no pixel decode)." Commit e7a9f43.

## Side findings

- Phase 5 review: three reviewers ran (cpp, consistency, simplicity), not the
  full `eng.yaml` set of eight, to keep the cloud review cost bounded; the
  final review covers the whole stack. Round 1 fixed: the TIFF decoded bps is
  computed once for both branches, and the new-format edit-site prose
  (`SipiImage.h`, `tools/format-handlers-fanout.sh`, `ARCH-MAP.md`), the
  `formats_test` comment, `benchmarking.md` and the `DeadlineDecode` comment
  name `read_metadata`. Not taken: moving the wrapper trio into `SipiIO` as a
  non-virtual (the plan fixes a pure virtual). Pre-existing, outside the unit:
  `docs/learnings/testing/seam-mitigation-does-not-green-a-codec-fuzz-leg.md`
  links into `docs/specs/`.

- Phase 5: `decode_benchmark` had no PNG case and `@sipi_bench_fixtures` has no
  PNG; `decode_full_png` reads `images/unit/sample_with_icc.png` from the source
  tree, as `decode_full_subsampled_jp2` reads file3.jp2.

## Owner decisions

- 2026-11-27, final (card cmsg_01XC4b1mTKe7ziDJYWR1rUNVYTP9PoRVfum9dG1ZRCLEfr): keep the PNG reader fix for DEV-8091 in #864? Answer: keep (Ivan answered the same question on the kakadu unit's card, 07:30Z; this card closed to match).
- 2026-11-27, final, owner decision (Ivan): pin the crane version in publish.yml in both jobs. Done in the release-image layer: `version: v0.22.1` on both `setup-crane` steps.

## Closeout

- root_cause: `sipi query` decoded every pixel and then printed only header and metadata, so 2 GB JPX files hit the 120 s deadline.
- investigation: a standalone g++ 13 build ran the equality and deadline tests for TIFF, JPEG and PNG; a mutation (dropping the palette header steps) failed the equality test. CI found the CIELab `ICC-Date` wall clock (see `docs/learnings/testing/icc-date-is-wall-clock-in-printed-metadata.md`).
- solution: `SipiIO::read_metadata` in all four handlers, sharing `read_image(..., decode_pixels)` with `read`; `SipiImage::read_metadata` over the shared `read_dispatch`; `read_metadata_with_deadline` for `run_query`.
- prevention: `read_metadata_equality_test`, `decode_deadline_test`, e2e `query_jpx_header`.
- final review (2026-11-27): the stack review (cpp+performance, consistency, rust+security, devops+nix, dune+simplicity+reference) found no Critical; fixes went into tiff-buffer and kakadu-errors. This layer gained the ARCH-MAP/CONTEXT refresh and two learnings.
