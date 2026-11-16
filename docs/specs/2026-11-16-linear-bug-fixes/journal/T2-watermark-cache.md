---
plan: docs/specs/2026-11-16-linear-bug-fixes/01-fix-linear-bugs-plan.md
unit: watermark-cache
unit_state: done
curated_from: a41194e014cc115e66c13e9acde926fdae0501e7
started_from: 6d48d679575aa9b3b0303f871b925e7f6c1fc7de
branch: claude/project-thread-8xyvc7
linear: DEV-7602
---

# T2 watermark-cache journal

## Repos

| repo | branch | pr |
|---|---|---|
| dasch-swiss/vitrinli | claude/project-thread-8xyvc7 | https://github.com/dasch-swiss/vitrinli/pull/845 |

## Phases

| phase | state | review_fix_rounds |
|---|---|---|
| 2 Never cache watermarked output | reviewed | 1 |

## Chunk queue

1. `serve_image.cpp`: skip cache read and write when a watermark is applied, with the `serve_image_test` case.

## Chunks

| chunk | commit | tests |
|---|---|---|
| 1 skip cache for watermarked output | 2abfa84 | `serve_image_test` case added; not run locally (Bazel cannot fetch in the cloud container), CI runs it |

## Owner decisions

## Deviations

- Branch name: the session assigned `claude/project-thread-8xyvc7`, not the plan's `impl/bugs-watermark-cache`. The plan allows this (Cross-cutting, Stacking without `gh stack`).
- Docs: added one sentence to `docs/src/guide/sipi.md` (Cache Configuration) and one row to the testing-strategy matrix. The plan names neither; both would be stale without it.
- Test fixtures: the second watermark is `unit/gradient-stars.tif`, which `sipiimage_test` already uses as a watermark; the plan named no fixture.
- Journal path: the project instructions name `journal/T<n>-<slug>.md` in the plan folder, which wins over the skill's `<plan-name>-<unit>-journal.md`.

## Review

Round 1 (cpp-reviewer, consistency-reviewer, sonnet; the reviewer set was cut from six to two for a 20-line diff: cpp covered security and simplicity of the C++ path, consistency covered references and docs):
- Fixed: `CONTEXT.md` Cache key entry and relationship bullet now say the cache holds only unwatermarked output (fixup into the fix commit).
- Fixed: the test now seeds a `/1` entry under the watermarked request's own key and asserts it is not served, so a revert of the read guard alone fails the test.
- Not taken: rewording the `serve_image.cpp` key comment; the watermark marker is still part of the key string, so it is accurate.
- Not taken: renaming `cache.rs::watermark_cache_separation`; it still asserts that watermarked and plain bytes differ.

## Deferrals

## What went well

## What went wrong

## Final review

- 2026-11-16, final unit (whole stack against `main`, 8 reviewers, skeptic triage): no finding in this layer.

## Closeout

- root_cause: the cache key names the IIIF params and the restriction box but not the watermark, so a watermarked response could be written to, and read back from, the same key as an unwatermarked one.
- investigation: the watermark is applied by the Lua hook per request, so no stable value exists to put in the key; skipping the cache is the only safe choice.
- solution: `serve_image` passes a null cache when a non-empty watermark applies, so watermarked output neither reads nor writes the cache.
- prevention: a `serve_image_test` case pins it; ARCH-MAP's cache entry records the rule (review-enforced). Operators should flush caches written before this fix, because an old unwatermarked entry under a canonical key stays until eviction.
