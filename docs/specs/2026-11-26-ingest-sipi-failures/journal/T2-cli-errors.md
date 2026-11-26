---
plan: docs/specs/2026-11-26-ingest-sipi-failures/01-fix-ingest-sipi-failures-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: a7b9dd800b8837c3d5ae7b3af3ea88f3da686c69
branch: claude/project-thread-l8k9ki
started: 2026-11-26
unit: cli-errors
unit_state: done
curated_from: 8482befdb496473373bf6d6d2e7fde7d67e3b9b4
started_from: fd2a8f06cb234b1f79284350832669a76199b60c
problem: >
  CLI errors carry no code that tells the caller "the input file is invalid"
  (4xx) apart from "sipi failed" (5xx), no ID that finds the run's log lines,
  and an invalid IPTC block fails with "No valid IPTC data!" that names
  neither the file nor the reason (DEV-8090).
status: complete
---

# Execution Journal: 01-fix-ingest-sipi-failures-plan, unit cli-errors

The session assigned the branch `claude/project-thread-l8k9ki` instead of the
plan's `impl/ingest-cli-errors`. The journal lives at
`journal/T2-cli-errors.md` per the project instructions, not at the skill's
default `<plan-name>-<unit>-journal.md`.

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | a7b9dd8 | claude/project-thread-l8k9ki | rebase | stacked | https://github.com/dasch-swiss/vitrinli/pull/862 |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 2 | reviewed | vitrinli@a7b9dd8 | 1 |

## Chunk queue

- 2.1 IPTC error text and the file path at the four reader sites, with a JPEG handler test
- 2.2 Logger correlation ID and CLI prefix, with logger tests
- 2.3 error_code_name, emit_json_report codes and IDs, phase moves, query/compare/catch-all documents, Rust ID resolution and FFI setter, SipiReport test
- 2.4 e2e tests
- 2.5 json-output.md

## Chunks

The SHAs below are history of `curated_from`. Curation folded every chunk, fixup and journal commit into one commit per module: metadata, format_handlers, logging, cli (code, tests, docs), and this journal.

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 2.1 | vitrinli | complete | 5ed7c1e, 20da3f5 | `Iptc::parse` names the decode code; TIFF/PNG/J2K/JPEG read sites prefix `<path>: `; `jpeg_metadata_segments_test` builds a garbage APP13 | none |
| 2.2 | vitrinli | complete | 06a24a7 | `set_correlation_id`/`get_correlation_id`, `[<id>] ` on CLI log calls; two logger tests (checked with g++ 13 outside Bazel) | none |
| 2.3 | vitrinli | complete | 92d1c36 | codes and IDs in every document; `sipi_report_test`; Rust `correlation_id` with unit tests (checked with cargo test and clippy in a scratch crate) | none |
| 2.4 | vitrinli | complete | 79e5684 | e2e in `cli_json.rs` with `write_jpeg_with_garbage_iptc` in the e2e lib (helper checked in a scratch crate) | none |
| 2.5 | vitrinli | complete | 2e8308e | json-output.md, sipi.md, running.md, error-model.md, testing-strategy matrix | none |

## Deferrals

- Phase 2 review: not taken. A `report_error`/`json_error` overload that takes a `SipiValueError` to cut the repeated `error_code_name(r.error().code())` calls (Warning from the simplicity reviewer; the explicit calls match the plan and read plainly). Making `error_code` a required parameter (the plan sets the default `internal`). Escaping the correlation ID in the fallback document and validating it in the C++ setter (validated at the Rust boundary; repository rule: no defense-in-depth). `try`/`catch` in `sipi_cli_set_correlation_id` (its sibling setters have the same shape). Skip guards for a missing `MaoriFigure.jpg` in the new e2e cases (CI has the LFS file).

## Deviations

- Stack on `claude/project-thread-5pkqyn` (tiff-buffer): the one conflict was `test/e2e/src/lib.rs`, where exif-charset added `jpeg_with_app_segment`. `write_jpeg_with_garbage_iptc` now uses it, so the APP13 segment goes after the existing APPn segments instead of right after SOI (MaoriFigure.jpg has no APP13, so the garbage block is still the one parsed).

- JPEG reader site: the `<path>: ` prefix sits on the `parse_photoshop_resources` call in `SipiIOJpeg::read`, not at `Iptc::parse` inside `parse_photoshop` (that function has no path). It also prefixes the other Photoshop-resource errors (ICC).
- Random ID: `libc::getentropy`, not `libc::getrandom`. libc 0.2.189 has no `getrandom` on Apple targets, so the plan's call would break the macOS build (Build Completeness Invariant). A `getentropy` failure panics.
- An empty `SIPI_CORRELATION_ID` counts as unset (a generated ID, no warning), not as an invalid value.
- `SipiReport` fallback documents are tested through a message that is not valid UTF-8, which makes `json_pack` fail.
- `verify --json` emits no JSON document today (it only routes logs to stderr), so it carries no `correlation_id`; not in the plan, not changed.
- The e2e cases are in `test/e2e/tests/cli_json.rs` (the `--json` contract file), not `cli.rs` as the plan says. This also keeps them out of the file the `exif-charset` and `query` units edit.
- `ARCH-MAP.md` still calls `sipi_cli_main` the sole export `cli/rust` links. `final` runs `/dune:dune-map update`.

## Owner decisions

None asked. The plan's decisions (IPTC stays fatal; the 4xx/5xx rule; the ID passed in by the caller) were implemented as written.

## What went well

- Standalone checks outside Bazel caught nothing new but proved the logger prefix (g++ 13) and the Rust ID functions (cargo test + clippy `-Dwarnings`, libc 0.2.189 source read to find that `getrandom` is missing on macOS).
- The review found one real defect: a message that is not valid UTF-8 (a Latin-1 path, more likely now that read errors carry the path) made the fallback document drop `phase` and `error_code`, turning a 4xx into a 5xx. Fixed: the fallback keeps both.

## What went wrong

- The review subagents have no Bash, so they could not run `git diff`; one returned nothing until given a diff file. Reviewer roles were merged into five sonnet agents (consistency, cpp, rust, simplicity+dune+reference, security+devops) instead of one per role.
- A whole-file `clang-format -i` reformatted `cli_app.cpp` beyond the change; reverted and redone with `git clang-format` on changed lines only.
- `commands.test` (`just bazel-test`) cannot run in the cloud container; CI on the draft PR is the first build.

## Closeout

- root_cause: a CLI `--json` error had no code that tells "the input file is invalid" (4xx) from "sipi failed" (5xx), no ID that finds the run's log lines, and the IPTC rejection said "No valid IPTC data!" without file or reason.
- investigation: libc 0.2.189 has no `getrandom` on Apple targets, so the ID uses `getentropy`. A message that is not valid UTF-8 made the fallback document drop `phase` and `error_code`; fixed in review.
- solution: `error_code_name` over `ErrorCode`, `error_code` and `correlation_id` in every document, `query`/`compare`/catch-all error documents, post-decode steps in phase `convert`, the ID resolved once in Rust and passed over `sipi_cli_set_correlation_id`, `[<id>] ` on CLI log calls, the IPTC decode code and the file path in the error.
- prevention: `sipi_report_test`, the logger tests, `jpeg_metadata_segments_test` (garbage APP13), e2e in `cli_json.rs`.
- final review (2026-11-27): no finding in this layer. Not taken (Suggestions): make `error_code` a required parameter, one helper for the repeated `emit_json_report(..., error_code_name(...))` calls.
