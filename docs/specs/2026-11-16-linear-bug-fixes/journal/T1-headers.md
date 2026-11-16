---
plan: docs/specs/2026-11-16-linear-bug-fixes/01-fix-linear-bugs-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: 6d48d679575aa9b3b0303f871b925e7f6c1fc7de
branch: claude/project-thread-5ilb3c
started: 2026-11-16
unit: headers
unit_state: done
curated_from: afd2e716459b56565c382c694df2320d590a03e3
started_from: d893604773ce05b699cc9d9dbbcef3e8b8decdcf
problem: >
  Hook-gated image and /file responses can be stored by a shared cache
  although their bytes depend on the caller's credentials (DEV-7600); CORS
  does not vary on Origin in reflect mode, accepts the Origin `null`, and
  info.json does not vary on Accept (DEV-7599 items 2-4); the 401 info.json
  copies every unknown preflight key into its public auth-service block
  (DEV-7601).
status: complete
---

# Execution Journal: 01-fix-linear-bugs-plan, unit headers

The session assigned the branch `claude/project-thread-5ilb3c` instead of the
plan's `impl/bugs-headers`.

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | 6d48d67 | claude/project-thread-5ilb3c | rebase | shipped | https://github.com/dasch-swiss/vitrinli/pull/847 |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 1 | reviewed | vitrinli@6d48d67 | 1 |

## Chunk queue

- 1.1 CORS: `cors_allow` always varies, denies Origin `null`; info.json varies on Accept (DEV-7599)
- 1.2 Hook-gated image and /file responses are `private` and vary on Cookie and Authorization (DEV-7600)
- 1.3 auth-service block copies only the allowlisted preflight keys (DEV-7601)
- 1.4 e2e tests in `security.rs` and `iiif_compliance.rs`

## Chunks

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 1.1 | vitrinli | complete | 490bf07 | `CorsDecision` loses its `vary` field (every site varies on Origin); `null` Origin denied; `json_response` adds `Accept` with a link context | none |
| 1.2 | vitrinli | complete | 0ba747b | `mark_private` after the engine outcome for hook-gated image and /file responses; `append_vary` shared with `with_cors` | none |
| 1.3 | vitrinli | complete | b06e2d5 | `AUTH_SERVICE_KEYS` allowlist in `info::auth_service`, debug log for dropped keys; Lua docs | none |
| 1.5 | vitrinli | complete | 481e6d8, dd3b497 | review round 1 fixes: stale preflight test, `with_cors` varies before its early return, `expect` for the joined header values, docs (`running.md`, `config.rs`, testing-strategy matrix), literal keys in the auth-service test, `mark_private` on every hook-gated IIIF kind | none |
| 1.4 | vitrinli | complete | 07296f8, ab576c0 | `append_vary` merges into one `Vary` header (fixup); e2e in `security.rs` and `iiif_compliance.rs` | none |

## Deferrals

- Phase 1: review suggestions not taken (all Suggestion severity): add `Access-Control-Request-Headers` to the OPTIONS `Vary` (DEV-7599 item 5 is accepted, shared caches rarely store OPTIONS); one shared writer or const for every `Vary` site (`push_cors` and the preflight build a fresh header list, so there is no duplicate today); `Vary: *` handling in `append_vary` (nothing emits it); dropping the debug log of dropped auth-service keys (the plan asks for it).

## Deviations

- Phase 1: implementation choices inside the decided behaviour, no owner-decided behaviour changed:
  - `CorsDecision` drops its `vary` field instead of returning `vary: true` in every branch (the plan's wording). A field that is always true is dead state; the behaviour (every CORS site varies on Origin) is the plan's.
  - `Accept` is added to `Vary` in `json_response` when it has a link context (only info.json), not in `json_cors`. `json_response` is where `Accept` is read, so the dependency and its `Vary` entry sit together; knora.json, which does not read `Accept`, does not vary on it.
  - `append_vary` writes one merged `Vary` header instead of appending a separate one per token (the plan's "h.append a separate Vary header"). With separate headers, `allowed_origins_allowlist_gates_info_json_cors` (security.rs), which reads only the first `Vary`, would fail on a hook-gated 404 (`Cookie` first, `Origin` third), and a client or cache that reads only the first `Vary` would miss tokens.
  - `with_cors` adds `Vary: Origin` before its early return for a response that already carries ACAO (a hook direct response), so REQ-2 also holds there. Found by the dune and security reviewers.
  - Because `cors_allow` is the chokepoint, the docroot fileserver (`push_cors`) and OPTIONS (`cors_preflight_response`) also always vary on Origin and deny `null`. The plan says "on every route", so this is in scope.

## Side findings

none

## Owner decisions

- 2026-11-16, answered by Ivan 2026-11-16 21:00 UTC: "Yes, all hook-gated IIIF kinds", card cmsg_01XC4b1mTKe7ziDJYWR1rUNVQRYTwhXnBPsQHDihJ72KDu: the security review found that hook-gated info.json and knora.json responses outside the JSON 200 path (knora.json deny 404, restrict refusals, hook direct responses) reach the client without `Cache-Control`. The plan scopes DEV-7600 to image and /file. Taken: `mark_private` runs for every IIIF kind when `pre_flight` is configured (/file when `file_pre_flight` is), commit dd3b497; it is a no-op on the JSON 200s, which already send `private, no-store`. Alternative: keep image and /file only.

## What went well

- A scratch cargo crate with the pure CORS, `mark_private`, `json_response` and `auth_service` functions extracted from the sources ran the unit tests and clippy `-Dwarnings` in the cloud container, and each new test failed under a hand-made revert of its fix.
- The reviewers caught a stale unit test (`cors_preflight_without_origin_omits_origin_and_credentials`) the scratch crate had not included.

## What went wrong

- Bazel cannot build in the container, so the full unit suite and the e2e tests run first in CI.
- The first review round lost four of six reviewers to a missing diff: they have no shell, and the brief gave a git command instead of a diff file. Saving the diff to a file and re-sending fixed it.

## Final review

- 2026-11-16, final unit (whole stack against `main`, 8 reviewers, skeptic triage): a unit test name still said an unlisted Origin gets nothing; renamed to `push_cors_echoes_only_a_listed_origin`. Fixed on the owner's request: one writer for `Vary` and credential `Cache-Control`. `append_vary` is the only `Vary` writer (`push_cors` no longer pushes it; the preflight, the static handler and `json_response` call `append_vary` on the built response), and `json_response` takes `private` through `mark_private`.

## Closeout

- root_cause: responses that depend on the Lua hook's view of the credentials carried no `private` and no `Vary: Cookie, Authorization`, reflect-mode CORS answers had no `Vary: Origin`, a `null` Origin could be echoed, and the auth-service block copied every preflight key.
- investigation: the security review found hook-gated JSON responses outside the 200 path without `Cache-Control`, which widened `mark_private` to every IIIF kind with a hook (owner answered: all hook-gated IIIF kinds).
- solution: `mark_private` and `append_vary` in `iiif()`, `Vary: Origin` on every CORS site, `null` Origin denied, `Vary: Accept` on info.json, and an allowlist of six description keys in `info::auth_service`.
- prevention: e2e cases in `security.rs` and `iiif_compliance.rs` and unit tests in `routes.rs`/`info.rs`; operators confirm no shared cache sits in front (H5).
