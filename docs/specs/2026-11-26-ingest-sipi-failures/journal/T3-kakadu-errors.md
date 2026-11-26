---
plan: docs/specs/2026-11-26-ingest-sipi-failures/01-fix-ingest-sipi-failures-plan.md
unit: kakadu-errors
unit_state: done
curated_from: 17e7837
started_from: 356ca69
target_repo: dasch-swiss/vitrinli
branch: claude/project-thread-5mvhen
started: 2026-11-26
problem: >
  DEV-8091: a small 8-bit RGB PNG without an ICC profile failed to write as
  JPX with "Kakadu exception 1801745733" (kduE). Kakadu's text ("No colour
  description found ...") went only to the log, never into the returned
  error, and the shared KduSipiError buffer accumulated text across errors
  from concurrent decodes (a data race).
symptoms:
  - "Failed writing JPEG2000 image (Kakadu exception 1801745733), file=..., dimensions=228x320, channels=3, bps=8, colorspace=RGB"
status: complete
---

# Execution Journal: kakadu-errors (Phase 3)

## Repos

| repo | branch | pr |
|------|--------|----|
| dasch-swiss/vitrinli | claude/project-thread-5mvhen | #864 |

## Phases

| phase | state | review_fix_rounds |
|-------|-------|-------------------|
| 3 | reviewed | 1 |

## Chunks

| id | status | commit(s) | summary | blocker |
|----|--------|-----------|---------|---------|
| 3.1 | complete | 46a241f | Kakadu text per thread and in every returning catch; JPX colour-layout check; PNG extra samples from decoded nc; tests j2k_error_text_test, png_alpha_layout_test (not run locally: Kakadu and gtest absent; CI is the first build) | none |

## Deferrals

- Phase 3 review round 1 (Warning, cpp reviewer): a Kakadu error raised on a `kdu_thread_env` worker (stripe decode, `push_stripe`/`finish`) keeps its text on the worker thread, so the returned error lacks it; only the log has it. Handing the text across threads needs Kakadu's thread-entity API, not checkable without the headers. Documented in the code comment and `error-model.md`. Follow-up: verify with a corrupt-tile file in CI and decide.
- Phase 3 review round 1 (Warning, consistency reviewer): `SipiIOPng::read_shape` reports `png_get_channels` before the tRNS/bKGD/palette transforms, so its nc can differ from `read` (pre-existing for palette images). Not changed: out of this unit.
- Phase 3 review round 1 (Suggestion): `kdu_customize_*` are process-global setters called on every read and write (benign repeat, pre-existing); not changed.
- Hot-path benchmark rule: the change adds no per-pixel work (one channel-count check per write and per PNG read, string work on error paths), so no `just bench` run; to be stated in the PR.

## Deviations

- Phase 3: the session assigned branch `claude/project-thread-5mvhen`, not the plan's `impl/ingest-kakadu-errors` (allowed by the plan's Cross-cutting rule).
- Phase 3: the journal is at `journal/T3-kakadu-errors.md`, per the project instructions, not `<plan-name>-kakadu-errors-journal.md` as the eng reference says.
- Phase 3: the JPX colour check runs before any Kakadu object (nc - extra samples not 1, 3 or 4), not as `jp2_family_colour.exists()` after the colour block as the plan says: `jpx_layer.add_colour()` returns a live interface, so `exists()` is true without `init()`. Same behaviour as REQ-9.
- Phase 3: `KduSipiWarning` got the same per-thread fix as `KduSipiError` (not in the plan): it shared the race, and each warning line repeated every earlier warning.
- Phase 3: a kakadu error raised on a decode worker thread stays in that worker's thread_local text, so the returned error lacks it (still logged). Accepted limit of the plan's thread_local design.
- Phase 3: DEV-8091 could not be reproduced by running sipi: Kakadu is not fetchable in the cloud container, so no J2K code builds here. The branch was found by reading the code (see Side findings); the test that proves it runs first in CI.

## Side findings

- PR #864 CI run 45911579520 (head 7e76379) green, commit-lint included; this is the first green run of the `kdu_error` helper form.

- Stack-on review (after rebase onto release-image 356ca69; consistency and dune reviewers): no interaction with the lower units. Fixed DUNE-002: the 12 returning catches build their error through one `kdu_error(code, msg)` helper (keeps the catch's source location) and recovering code calls `discard_kdu_error_text()`, so the next copied catch has one obvious call. Refuted for a unit: DUNE-001 (ARCH-MAP refresh is the final unit's ship step). Not taken: DUNE-003/004/005 suggestions (es/nc invariant in SipiImage, a pointer in the precheck message, one canonical statement of the Kakadu text rule).

- Phase 3: CI run 45907945026 (workflow_dispatch on 3b74c3c) green on linux-amd64, linux-arm64, darwin-arm64 and asan-ubsan; commit-lint runs on PRs only. The new tests were not run with the change reverted (no local build); each asserts text or counts that the old code cannot produce.

- Phase 3 review round 1 (0f26cb0, fixup into 46a241f): comments state the worker-thread limit and why the recovering ICC catch clears; the redundant client-abort clear is gone; the write-check comment says why 1, 3 or 4; tests compare words without reimplementing the fold, remove their temp files, add a two-thread read test and gray+alpha+bKGD and palette+tRNS cases; `formats_test` depends on `@libpng//:png` directly; `error-model.md` describes the Kakadu text. Refuted: stale text from a swallowed kdu_exception (the only recovering catch clears; `jpx_in.open(.., true)` returns < 0 without an error); the trailer model name (the session's attribution says Opus 5.5); the ICC regression from the early check (Kakadu rejects a colour count that differs from the colour channels, so no 2- or 5+-colour layout wrote before).

- Phase 3: DEV-8091 cause, from the code: `SipiIOPng::read` adds `ASSOCALPHA` for colour type RGBA and GRAY_ALPHA before libpng's transforms. A `bKGD` chunk makes `png_set_background` composite and drop the alpha channel, so the image has nc=3 but one extra sample. `SipiIOJ2k::write` then switches on nc - es = 2, which has no case, and Kakadu gets no colour box. Plain RGB without ICC takes `case 3` and writes sRGB. The same mismatch, the other way round, hits RGB and GRAY with `tRNS`: libpng adds alpha, sipi adds no extra sample (nc=4 written as CMYK; nc=2 gets no colour box). Checked with the container's libpng 1.6.43 and a scratch C program using the reader's transforms: RGBA+bKGD decodes to 3 channels, RGB+tRNS to 4, gray+tRNS to 2.

## Owner decisions

- 2026-11-27, Ivan (card cmsg_01XC4b1mTKe7ziDJYWR1rUNV3XpT4URwiKhvPxcZYkKZrM): also fix the PNG reader cause (extra samples from the decoded channel count) next to the planned JPX colour check? Answer: "Fix PNG reader and add JPX check" (as implemented in #864).

## Closeout

- root_cause: `SipiIOPng::read` set the alpha extra sample from the colour type before libpng's bKGD/tRNS transforms changed the channel count, so the JPX writer saw an `nc - es` with no colour case and gave Kakadu no colour box. Kakadu's text went only to the log, and the shared `KduSipiError` buffer was a data race.
- investigation: no Kakadu in the cloud container; the branch was found by reading the code and confirmed with a scratch libpng program. See `docs/learnings/debugging/png-transforms-change-the-channel-count.md`.
- solution: extra samples from the decoded channel count; the JPX writer refuses `nc - es` outside {1, 3, 4} before any Kakadu object; Kakadu text per thread, appended by `kdu_failure` in every returning catch.
- prevention: `j2k_error_text_test`, `png_alpha_layout_test`.
- final review (2026-11-27): one fixup, the helper is renamed `kdu_error` to `kdu_failure` (it shadowed Kakadu's `kdu_core::kdu_error` class under `using namespace kdu_core`), and stray blank lines in its call sites are gone. Ivan kept the PNG reader fix (card answered 2026-11-27).
