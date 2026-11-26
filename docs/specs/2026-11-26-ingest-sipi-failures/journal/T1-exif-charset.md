---
plan: docs/specs/2026-11-26-ingest-sipi-failures/01-fix-ingest-sipi-failures-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: a7b9dd800b8837c3d5ae7b3af3ea88f3da686c69
branch: claude/project-thread-nrl1bz
started: 2026-11-26
unit: exif-charset
unit_state: done
curated_from: 2ef5c440e5b683d9bc8d94d5adf79fc6e69c0ca6
started_from: a7b9dd800b8837c3d5ae7b3af3ea88f3da686c69
problem: >
  `sipi query` on a JPEG whose Exif.Photo.UserComment is UCS-2LE exits with
  "unhandled exception: Cannot convert text encoding from 'UCS-2LE' to 'UTF-8'"
  (DEV-8089). The Linux Exiv2 build has no iconv, and the Exif printer lets the
  Exiv2 conversion error escape to the CLI catch-all.
symptoms:
  - "sipi query: unhandled exception: Cannot convert text encoding from 'UCS-2LE' to 'UTF-8'"
status: complete
---

# Execution Journal: T1 exif-charset

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | a7b9dd8 | claude/project-thread-nrl1bz | rebase | in-progress | https://github.com/dasch-swiss/vitrinli/pull/860 |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 1 | reviewed | vitrinli@a7b9dd8 | 1 |

## Chunk queue

- 1.1 `bazel/exiv2.BUILD.bazel`: `EXV_HAVE_ICONV=1` on Linux, docstring updated
- 1.2 `src/metadata/cpp/exif.cpp`: the Exif printer renders each value inside a `try`; unit test `exif_charset_test`
- 1.3 `test/e2e`: byte-level helper that inserts a UCS-2LE UserComment APP1 into a JPEG; `sipi query` e2e case

## Chunks

The SHAs below are history of `curated_from`. Curated layer: `build(bazel)` (1.1 and its review fixup), `fix(metadata)` (1.2 and its review fixup), `test(metadata)` (1.3), and this journal.

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 1.1 | vitrinli | complete | 6e70c97 | Exiv2 `EXV_HAVE_ICONV=1` on Linux, docstring updated | none |
| 1.2 | vitrinli | complete | 479775b | Exif printer prints `<unconvertible>` and warns; `exif_charset_test` | none |
| 1.3 | vitrinli | complete | cd65b89 | e2e `cli_query_prints_ucs2_user_comment`; `jpeg_with_app_segment` and `exif_app1_with_user_comment` helpers | none |

## What went well

- Phase 1: conda-forge's exiv2 0.28.9, libiconv and gtest packages are reachable from the cloud container. With them, `exif.cpp` and `exif_charset_test.cpp` compile with g++ 13 and run under ASan/UBSan outside Bazel; the test passes with the fix and fails without it.

## What went wrong

- Phase 1: `commands.test` (`just bazel-test`) and the lint gates cannot run in the cloud container (Bazel fetches are blocked). CI on the draft PR is the first Bazel build; the unit test ran only in a standalone g++ 13 build against conda-forge's Exiv2 0.28.9, which is built with GNU libiconv, not glibc iconv.
- Phase 1: the reviewer subagents had no git access and reviewed the working tree, not the diff.

## Deferrals

## Deviations

- Phase 1: the unit branch is `claude/project-thread-nrl1bz`, not `impl/ingest-exif-charset`: the session may push only to its assigned branch. The plan's Cross-cutting section allows this; the name goes into the `done` line.
- Phase 1: the journal is at `journal/T1-exif-charset.md`, as the project instructions say, not at the skill's default `<plan-name>-exif-charset-journal.md`.

## Side findings

- Phase 1: review round 1 (consistency, simplicity, dune, reference, cpp, rust, devops; sonnet). Fixed: `CLAUDE.md` said iconv is a macOS-only system dependency; the Exiv2 docstring said a Unicode UserComment "fails to print"; the charset tests accepted either outcome, so a Linux build without iconv passed them (now pinned to the converted text, since both platforms build with iconv). Refuted by measurement: `EXV_HAVE_ICONV` not visible through `<exiv2/exiv2.hpp>` (the standalone build saw it). Not taken: hoist `EXV_HAVE_ICONV` out of the `select` (the plan names the Linux branch), drop the `marker` parameter of `jpeg_with_app_segment` (cli-errors reuses it for APP13), ARCH-MAP update (the `final` unit runs `dune-map`), pre-existing history wording at `exif.cpp:60`.

- Phase 1: stacked on the plan branch with no rebase (its head was still `a7b9dd8`). The pre-PR review the project instructions ask for is the phase review: the same reviewers `eng:eng-review` selects, and the diff has not changed since its fixes. The PR was opened as a draft with the GitHub MCP; a review request to `subotic` was refused because he is the PR author on GitHub.

## Owner decisions

- none

## Closeout

- root_cause: the Linux Exiv2 build had no iconv (`EXV_HAVE_ICONV` set only on macOS), so printing a UCS-2LE `Exif.Photo.UserComment` threw, and the Exif printer let the Exiv2 error reach the CLI catch-all.
- investigation: the read path already succeeded; only `operator<<` threw. conda-forge's Exiv2 0.28.9 with libiconv let the test run outside Bazel.
- solution: `EXV_HAVE_ICONV=1` on Linux (glibc has iconv in libc, no linkopt); the Exif printer renders each value in a `try` and prints `<unconvertible>` with a `log_warn` naming the key.
- prevention: `exif_charset_test` (pinned to the converted text, so a Linux build without iconv fails it) and the e2e `cli_query_prints_ucs2_user_comment`.
- final review (2026-11-27): no finding in this layer.
