---
plan: docs/specs/2026-11-26-ingest-sipi-failures/01-fix-ingest-sipi-failures-plan.md
target_repo: dasch-swiss/vitrinli
base_commit: a7b9dd8
branch: claude/project-thread-ip96c5
started: 2026-11-26
unit: release-image
unit_state: done
curated_from: b367343
started_from: 17bbac2
problem: >
  DEV-8113: the released image is not the image the smoke tests ran on.
  `publish-docker` builds the image again and pushes it, and `validate-docker`
  smoke-tests a fastbuild image because `just bazel-test-smoke` runs without
  `--config=release`.
symptoms:
  - "validate-docker passes, but the pushed image is a separate build"
status: complete
---

# Execution Journal: 01-fix-ingest-sipi-failures-plan, unit release-image (T6)

The session assigns branch `claude/project-thread-ip96c5`; the plan's table names
`impl/ingest-release-image`. The plan allows the session name (Cross-cutting,
Stacking without `gh stack`).

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | a7b9dd8 | claude/project-thread-ip96c5 | rebase | in-review | #863 |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 6 | reviewed | vitrinli@a7b9dd8 | 1 |

## Chunk queue

- 6.1 `bazel-test-smoke-release <arch>` and `bazel-docker-image-tarball <arch>` recipes; `validate-docker` runs them and uploads `sipi-image-<arch>` and `sipi-debug-<arch>`
- 6.2 `bazel-docker-push-tarball` crane recipe with digest check; `publish-docker` downloads the artifacts and pushes, no Bazel
- 6.3 remove `image_push_*`, `remote_tags_*` and `bazel-docker-push-*`; update docs

## Chunks

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 6.1-6.3 | vitrinli | done | 1141634 | One `fix(docker)` commit: release smoke test, tarball upload, crane push with digest check, `oci_push` removal, docs | — |

## What went well

- `crane`, `just` and `actionlint` release binaries download in the cloud container, so the push recipe ran end to end against a local `crane registry serve` (equal digests; a faked mismatch fails with exit 1), and `actionlint` passes on `publish.yml`.

## What went wrong

- The plan's file list for the doc updates missed five stale places that the review found: `MODULE.bazel` (named `oci_push`), the `bazel-docker-build-*` comment in `justfile` (named the removed stamped tag suffixes), `CONVENTIONS.md` Docker section, and the `crane` lines in `flake.nix` and `docs/src/development/nix.md`. All fixed in the same commit.
- Gates in the cloud: no Bazel, so the new recipes' `bazel` lines are unrun until a `v*` tag. Checked here: `just --list` parses, `actionlint` and a YAML load pass on `publish.yml`, `just commit-lint` passes, and `docker-push-tarball` ran against a local `crane registry serve` (equal digests, and a faked mismatch exits 1).

## Deferrals

Phase review (devops, consistency, simplicity reviewers; round 1 fixed every verified stale reference). Kept as they are, for the owner to judge:

- `imjasonh/setup-crane` had no `version:`, so it installed the latest `crane`. Owner decision (Ivan, 2026-11-27): pin it in both jobs. Both steps now set `version: v0.22.1`.
- The `--config=release --stamp --platforms` flags and the arch `case` block are copied into each per-arch recipe, as in the existing `bazel-docker-extract-debug` and `bazel-cross-build-image`. A shared helper would touch those recipes too; the comment above `bazel-docker-build-*` states that the flags must agree.
- `validate-docker` still runs `bazel-docker-build-<arch>` (build and `docker load`) and `docker/login-action` before the smoke test. The plan keeps them; the build step now only warms the cache and feeds the BEP upload.
- `compression-level: 0` on the tarball upload would save CPU (layers are already gzip). Not done; it is a tuning choice.

## Deviations

- The three chunks landed as one commit: they are one slice of one concern (`docker`), and the commit rules ask for one commit per module per slice.
- `publish-docker` runs on `ubuntu-latest` for both arches instead of the per-arch runners. The job no longer builds or runs arch code; `crane push` takes the platform from the tarball. The plan does not name the runner.
- The plan lists `README.md`, `CLAUDE.md` and `allocator-replay.md` for updates; none of them names `image_push` or `bazel-docker-push`, so they are unchanged. `.gitignore` gains `/sipi-image-*.tar` next to `/sipi-*.debug`.
- Recipe names: `bazel-docker-image-tarball <arch>` (Bazel) and `docker-push-tarball <arch>` (no Bazel, so no `bazel-` prefix). The plan leaves both names open.
- `actions/download-artifact` is pinned at v7.0.0 (`37930b1c…`), the release that matches the repository's `upload-artifact` v7.

## Side findings

- `test/e2e/BUILD.bazel:230` names a `just test-smoke` recipe that does not exist (it is `bazel-test-smoke`). Not in this unit's scope.

## Closeout

- root_cause: `publish-docker` built the image again and pushed that build, and `validate-docker` smoke-tested a fastbuild image (`bazel-test-smoke` ran without `--config=release`).
- investigation: `crane`, `just` and `actionlint` run in the cloud container, so the push recipe ran end to end against a local `crane registry serve`.
- solution: `validate-docker` smoke-tests the release build (`bazel-test-smoke-release`), uploads that tarball and the debug file; `publish-docker` downloads them, pushes with `crane`, and fails when the pushed digest differs from the tarball's. The `oci_push` targets and `bazel-docker-push-*` recipes are gone.
- prevention: the digest check in `docker-push-tarball`; the smoke test and the pushed image share one Bazel action.
- final review (2026-11-27): no Critical or Warning. `crane` pinned to v0.22.1 in both jobs (Ivan, 2026-11-27). Not taken: `overwrite: true` and `compression-level: 0` on the uploads.
