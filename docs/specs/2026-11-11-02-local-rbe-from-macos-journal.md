---
title: "Journal: run local macOS builds on the NativeLink RBE backend"
date: 2026-11-11
author: "Ivan Subotic"
status: implemented
repositories: []
plan: docs/specs/2026-11-11-02-local-rbe-from-macos-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: 484e94e
branch: feat/local-rbe-from-macos
started: 2026-11-11
---

# Journal: run local macOS builds on the NativeLink RBE backend

**Problem.** A local build compiles everything on the laptop and cannot use the shared
NativeLink worker that CI uses. The docs said local development never contacts the
backend, and they gave the internal service ports (`:50051`, `:50052`) as if a client
connected there. The only externally reachable endpoint is the proxy on
`rbe-01.ops.dasch.swiss:443`.

## Chunk queue

| id | chunk | files |
|----|-------|-------|
| 1 | Config and CI action: `rbe-cross`, `rbe-darwin`, action emits `--config=rbe-cross` | `.bazelrc`, `.github/actions/bazel-rbe/action.yml`, `justfile` |
| 2 | Docs: local use, port correction, stale claims | `docs/src/development/{rbe,ci,bazel}.md`, `docs/mkdocs.yml`, `CLAUDE.md`, `CONVENTIONS.md` |
| 3 | Mac verification through Remote Control | none |
| 4 | Review, curation, ready | none |

## Chunks

| id | status | commits | summary | blocker |
|----|--------|---------|---------|---------|
| 1 | done | (see branch) | One writer for the cross-leg flags; opt-in darwin config with `--remote_download_toplevel` | none |
| 2 | done | (see branch) | `rbe.md` § Local use from macOS, `:443` proxy recorded, bazel-remote downloader claim reconciled | none |
| 3 | done | none | Mac at dbc0695: 78/78 tests under rbe-darwin, 1448 remote cache hits, 5322 remote executions; `sipi --version` runs (Mach-O arm64) | none |
| 4a | done | (see branch) | Review fixes: upload guard in rbe-darwin, stale action/ci.yml/rbe.md text, CLAUDE.md single-writer clause | none |

## What went well

- The plan review before implementation caught six real gaps, among them a verification
  step that could not pass (`sipi --version` under `--remote_download_minimal`) and a
  second writer of the cross-leg flags.
- The edits were mechanical once the plan was settled; `just docs-build` and the sweeps
  ran in the cloud container.

## What went wrong

- I started implementing before the plan was reviewed. Ivan stopped the work; the draft
  edits were stashed and discarded.
- The plan-revision commit carries a wrong `Claude-Session` trailer. Curation rewrites it.
- Bazel cannot fetch dependencies in the cloud container, so every Bazel check runs on
  Ivan's Mac.

## Deviations from the plan

- `rbe.md` Backend section: the plan asked to "reconcile" the bazel-remote row. The action
  no longer reads `REMOTEBUILD_CACHE_ENDPOINT` and no client sets
  `--experimental_remote_downloader`, so the row and the GitHub Actions wiring list now
  say that no client configures bazel-remote. The "Why a separate bazel-remote" section
  stays as the rationale; deleting it would be a larger docs decision than this plan.
- `.bazelrc` remote section: the example endpoint changed from `grpcs://<vm>:50051` to
  `grpcs://<host>:443`, in line with Ivan's statement that 443 is the only external port.
- Post-implementation review: `rbe-darwin` itself sets `--noremote_upload_local_results`
  after `--config=dasch-rbe`, and the flag left the `dasch-rbe` contract. The plan had it
  in the developer's file only; a developer who copied CI's flag string would then write
  the action cache CI reads. A config guard beats a docs rule.
- Post-implementation review: the darwin platform pair stays written twice (`rbe-darwin`
  and the action's shell). Moving it into per-target configs that the action emits would
  change every CI leg for a two-line pair; both sites now carry a "change both together"
  comment instead.
- Also fixed beyond the plan's list: the action's `description:`, the `ci.yml` comment that
  called local fallback the default on every leg, and `rbe.md`'s "no proxy".
- `ci.md`: the backend summary drops the `bazel-remote` `:50052` mention for the same
  reason.

## Results

Measured on Ivan's Mac, 2026-11-11, both runs after a warm local analysis cache, so
indicative only:

| run | tests | wall time | processes |
|-----|-------|-----------|-----------|
| `just bazel-test --config=rbe-darwin` | 78/78 | 416 s | 1448 remote cache hit, 5322 remote, 129 local |
| `just bazel-test` | 78/78 | 580 s | 6762 darwin-sandbox, 90 local |

The Mac run verified commit `dbc0695`, before the review fixes. Those fixes change
comments and docs, plus `build:rbe-darwin --noremote_upload_local_results`, which the
tested `~/.bazelrc` already set. CI verified the final head on all three legs.

The verification ran in a git worktree; `rbe-darwin` works there unchanged.

## Deferrals

none

## Side findings

- Under `rbe-darwin`, 43 Rust compiles on the Linux worker warn that `xcrun` cannot find
  MacOSX.sdk. Harmless; documented in `rbe.md` as expected noise.

- `rbe.md` "Why a separate bazel-remote" describes a component that no client uses. Worth
  a follow-up decision: remove bazel-remote from the VM, or wire it back in.

## Closeout

- **Root cause.** Local builds could not reach the backend because the only client
  identity was CI's, and the docs described the internal ports as the endpoint.
- **Solution.** Personal certificates plus a per-developer `build:dasch-rbe`; a repo
  `build:rbe-darwin` that adds the shared `build:rbe-cross` (now also used by CI), the
  darwin platforms, `--remote_download_toplevel` and the upload guard.
- **Prevention.** One writer for the cross-leg flags; CONVENTIONS.md and CLAUDE.md name
  it. The accepted cache-write risk is documented, with an infra follow-up (H2).
