---
title: Close the AI-readiness baseline gaps (Tier A)
date: 2026-11-11
author: Claude Code, for Ivan Subotic
status: implemented
repositories: []
---

# Journal: close the AI-readiness baseline gaps (Tier A)

Linear: DEV-7645. Source: the 2026-10-20 baseline assessment in `dasch-specs`
(`assessments/2026-10-20-baseline/sipi.md`, 40 of 57, Level 3). There is no plan file:
the scope was agreed in the project thread. Ivan chose "Tier A + issues for Tier B and C".

## Scope

- Tier A (this PR): documentation and mechanical fixes with no behavior change.
- Tier B (Linear issues): one source for the decode timeout, a pre-commit hook,
  `cargo machete`, a test for `tools/workspace_status.sh`.
- Tier C (Linear issues): C++ naming, splitting `routes.rs`, `-Werror` and clang-tidy
  `WarningsAsErrors`.

## What went well

- Every Tier A gap was still open on `main` at `2a1c681`, so the assessment mapped to
  concrete edits without re-scoring.
- The eng plugin's `## Required Shape` sections for `CONVENTIONS.md` and `REVIEW.md` gave
  an exact checklist, so the new sections fill named gaps rather than guesses.
- `eng.yaml` validated on the first run with the plugin's own validator.
- ARCH-MAP completeness validation passes: every tracked file maps to exactly one
  component or support area.

## What went wrong

- The cloud container cannot fetch Bazel's `http_archive` sources (GitHub archive
  downloads return 403 through the proxy), so `bazel test //src/cache:cache_test`, the
  rustfmt and clippy gates, and every other Bazel recipe could not run here. The
  `SipiCache` rename is mechanical (identifiers and comments only, no logic), but it is
  verified only by CI.

## Deviations from the assessment, and why

- **Oracle-vocabulary grep gate.** The assessment (migration-completeness F2) says to
  "widen the oracle-vocabulary grep gate to cover BUILD.bazel and justfile". No such gate
  exists; the rule is `docs-only`. ARCH-MAP's banned-constructs row claimed a
  `static-analysis` subset for it, which was false, so that claim is removed. Adding a
  real gate is new tooling and was left out of Tier A.
- **`SipiCache` rename went past comments.** The parameter names (`canonical_p`), the
  on-disk record field (`FileCacheRecord::canonical`) and the in-memory list field were
  also renamed to `cache_key`. A field name does not change the on-disk layout, and the
  glossary bans "canonical URL" in cache code, so leaving the identifiers would keep
  teaching the banned term. Two test names changed with them.
- **ARCH-MAP schema 2 migration accepted the current entries.** `last_verified_commit`
  was `none`, so by the migration rule every component counted as stale. The entries were
  accepted as verified and fingerprinted instead of re-exploring all 15 components. Risk:
  an entry that drifted before today is now marked fresh. The assessment sampled the
  content as mostly accurate.
- **Kit budget: overrides, not trims.** Seven components exceed the ≤7-file kit budget
  (throttling 14, cli 14, ffi 11, format_handlers 9, metadata 9, image 8, scripting 8).
  The assessment only named ffi. Trimming needs per-component judgment, so each override
  is recorded with its reason in `## Conventions` instead.
- **ICC invariant moved, not cut.** The detailed ICC determinism text (lcms2 visibility,
  `icc_lcms2.h`, the `layering_check` limit) existed only in `CLAUDE.md`. It moved to
  `CONVENTIONS.md` § ICC Determinism Invariant; `CLAUDE.md` keeps a three-line summary.
- **Naming.** The rating (2/10) is about mixed C++ styles. Renaming the API is Tier C;
  this PR only states one decision rule for new code in `CONVENTIONS.md` and fixes the
  table (the removed `shttps::` namespace, examples that no longer exist).
- **`.env.example`.** No variable is required for `just run`, so the file lists only
  what a developer sets by hand (`SIPI_JWTKEY`, the Sentry variables) and points to
  `docs/src/guide/running.md` for the full table. `GH_TOKEN` and the OTel variables are
  already set by `.envrc` and `just run-otel`. `.envrc` now loads a git-ignored `.env`
  with `dotenv_if_exists`.
- **`REVIEW.md` Skip line.** It said formatting is "enforced by clang-format"; no CI gate
  runs clang-format. The line now names the formatters as the reference and says only
  rustfmt is gated.
- **Review.** Four reviewers ran (consistency, dune, C++, code simplicity). Their
  findings were checked against the repo by hand instead of by one skeptic subagent per
  finding, to keep the cost in proportion to a docs change. Fixed: the field rename
  carried into ADR-0025 (a note), the glossary's Cache pin row and two docs pages; a
  leftover "strangler-fig" docstring in `src/ffi/BUILD.bazel`; the C++ key rules restored
  in `CLAUDE.md` (they must stay in the always-loaded file); the ICC guard marked partial.
  Not taken: removing the commit and lint-gate rules from either `CLAUDE.md` or
  `CONVENTIONS.md`. `CLAUDE.md` is always loaded and `CONVENTIONS.md`'s required shape
  needs both sections, so the overlap is deliberate. Not taken: splitting the `cli` and
  `throttling` kits per language (DUNE-002); it is listed with the Tier C issues.
- **`context-doc`: glossary folded into `CONTEXT.md`.** Ivan decided after the PR was
  opened: merge `UBIQUITOUS_LANGUAGE.md` into `CONTEXT.md` (DUNE-010, first option) rather
  than record the split in an ADR. The 92 table rows became dune's canonical entry format
  (`**Term**:`, definition, `_Avoid_:`); Relationships, Example dialogue and Flagged
  ambiguities moved over verbatim. Every reference outside `docs/specs/` now points at
  `CONTEXT.md`. `docs/archive/` keeps its old links: it is a frozen record, and rewriting
  it would make it claim two files were one.
