---
plan: docs/specs/2026-11-16-linear-bug-fixes/01-fix-linear-bugs-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: 6d48d679575aa9b3b0303f871b925e7f6c1fc7de
branch: claude/project-thread-3p8wse
started: 2026-11-16
unit: admission
unit_state: done
curated_from: 6a2d0d278e0d8ed4cc0701c2e389a4d0797e9b2f
started_from: 77a4dbe64d3600dca4045d49e2457e2dc5d929f0
problem: >
  DEV-7603: restricted region requests are classified as Tile-lane work before
  the preflight runs, but a size restriction makes the decode a whole-image
  decode (ADR-0028), which is Full-lane work by ADR-0022. A crawler on
  restricted non-pyramidal TIFFs can fill the Tile lane's partition.
status: complete
---

# Execution Journal: 01-fix-linear-bugs-plan, unit admission

The session assigned the branch `claude/project-thread-3p8wse` instead of the
plan's `impl/bugs-admission`; the plan allows this (Cross-cutting, Stacking).

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | 6d48d67 | claude/project-thread-3p8wse | rebase | shipped | https://github.com/dasch-swiss/vitrinli/pull/846 |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 5 | reviewed | vitrinli@6d48d67 | 1 |

## Chunk queue

- 5.1 throttling: `Permit::kind()` and its tests.
- 5.2 server: thread the permit into `serve_image`, `reclassify_to_full`, record the held lane; unit tests.
- 5.3 e2e: a restricted region request is served (`security.rs`).
- 5.4 docs: ADR-0028 residual bullet, ADR-0022 amendment.

## Chunks

SHAs below are the history of `curated_from`; curation folded them into one commit per module.

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 5.1 | vitrinli | complete | ade7371 | `Permit::kind()`, kind stored in the permit; 3 crate tests | none |
| 5.2 | vitrinli | complete | cc6e955 | permit threaded into `serve_image`, `reclassify_to_full`, held kind recorded; 4 unit tests | none |
| 5.3 | vitrinli | complete | baea257 | e2e `restricted_region_request_is_served` | none |
| 5.4 | vitrinli | complete | 1691f73 | ADR-0022 amendment, ADR-0028 residual, operator classification table | none |

## What went well

- `reclassify_to_full` and its tests were run in a scratch cargo crate against the real `admission` crate; reversing the drop/acquire order makes the ordering test fail, so the test proves the invariant.

- The throttling crate builds and tests with plain cargo in the cloud container (scratch workspace with path deps), so its tests, clippy and rustfmt ran locally.

## What went wrong

- CI runs only on pull requests, and the unit opens no PR before `stack on`, so the Rust lint gates, the server unit tests and the e2e get their first run on the draft PR.

- `routes.rs` links the C++ engine, so the server crate cannot be built in the cloud container; its unit tests, clippy and the e2e first run in CI. Only the copied function and its tests ran locally.

## Deferrals

## Deviations

- Phase 5: stack on `claude/project-thread-8xyvc7` (watermark-cache) rebased cleanly; that layer touches only `serve_image.cpp` cache use and its journal, so the review was not re-run on the unchanged commits.

- Phase 5: provisional: `docs/src/operation/admission-control.md` gains a classification-table row for size-restricted requests. The plan named only the two ADRs; without the row the operator page would still imply a restricted tile is Tile work. Commit 1691f73.

- Phase 5: answered by Ivan 2026-11-16: "Store the kind". The plan says `Permit::kind()` is "Full when the private `_full` permit is present, else Tile". In `Basic` mode a `Full` admitted over the cap holds no `_full` permit, so that rule reports it as `Tile`, and passing the held kind to `record_classification` would add false disagreements for ordinary requests. The permit stores its kind instead. Card cmsg_01XC4b1mTKe7ziDJYWR1rUNV4w9xcQF7KEMWGG14ok8amd, recommended and chosen "store the kind". Commit ade7371.

## Side findings

- Phase 5: review (consistency, simplicity, dune, reference, rust, security; sonnet). Verified and fixed in round 1: the `if let Some(kind)` guard before `record_classification` was unreachable defensive code (now an `expect`); the ADR-0022 amendment and the operator table did not say that a swapped request can wait up to two queue timeouts, or that restricted cache hits and HEAD requests also move (the swap runs before the engine's cache check). Refuted: "a restriction at native size needs no whole-image decode" (`serve_image.cpp` treats any non-zero (RW, RH) as size-restricted and decodes the whole image); "the e2e does not exercise the swap" (the plan assigns the lane to the unit tests); `too_many_arguments` allow on `dispatch_engine` (same pattern as `serve_image`). Not acted on: the Full-lane wait does not observe a client disconnect; the engine's `cancelled()` poll aborts the work once it starts, and the wait is bounded by the queue timeout.

## Owner decisions

- 2026-11-16, Ivan, card cmsg_01XC4b1mTKe7ziDJYWR1rUNV4w9xcQF7KEMWGG14ok8amd: the permit stores its kind ("Store the kind").

## Final review

- 2026-11-16, final unit (whole stack against `main`, 8 reviewers, skeptic triage): ADR-0028's added bullet said "earlier than before" and "no longer"; it now states the standing rule (fixup into `docs(throttling)`). `admission-control.md` now says a size-restricted request can be shed by full-partition pressure and that the classifier compares against the lane held after the swap. Fixed on the owner's request: the swap runs from a new seam hook, `before_decode`, which the engine calls after its cache check, so a restricted cache hit or HEAD keeps its lane; a refused swap is the engine's 503.

## Closeout

- root_cause: the shell picks the lane from the IIIF params before the preflight runs, so a size-restricted region request, which decodes the whole image (ADR-0028), ran in the Tile lane.
- investigation: deriving the lane from the private Full sub-permit fails in Basic mode, where a Full admitted over the cap holds none; the permit stores its kind instead (owner answered: store the kind).
- solution: `Permit::kind`; the permit is threaded as `&mut Option<Permit>` to `serve_image`, and `reclassify_to_full` drops the Tile permit and blocks on a Full acquire once the restriction is known; a shed or timeout answers 503.
- prevention: throttling and routes unit tests of the swap; ADR-0022 amendment and ADR-0028 bullet.
