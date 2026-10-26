---
plan: docs/specs/2026-10-26-01-rename-to-vitrinli-plan.md
target_repo: /Users/subotic/_github.com/dasch-swiss/vitrinli
base_commit: 658bb6c5
branch: docs/rename-to-vitrinli
started: 2026-10-28
problem: >
  The GitHub repository dasch-swiss/sipi was renamed to dasch-swiss/vitrinli and the
  product is now called Vitrinli, but the README, docs site, agent-context files and OCI
  labels still call it SIPI and link to the old repo name. This work renames the product
  in prose and links only; public contracts and code identifiers stay `sipi`.
status: complete
---

# Execution Journal: 2026-10-26-01-rename-to-vitrinli-plan

## Chunk queue

| id | goal | files |
|----|------|-------|
| 1 | Setup: repoint origin, fetch, docs-build baseline | — |
| 2 | Rename in README, mkdocs.yml, docs home and guide | README.md, docs/mkdocs.yml, docs/src/index.md, docs/src/guide/** |
| 3 | Rename in Lua/operation docs and root operator docs | docs/src/lua/**, docs/src/operation/**, docs/src/release-notes/**, docs/preservation-pipeline.md, RELEASING.md, SECURITY.md, DEPRECATIONS.md |
| 4 | Rename in development docs | docs/src/development/** |
| 5 | Rename in agent-context files | CLAUDE.md, CONTEXT.md, UBIQUITOUS_LANGUAGE.md, ARCH-MAP.md, CONVENTIONS.md |
| 6 | Verify + commit 1 (docs) | — |
| 7 | OCI labels + commit 2 (docker) | src/BUILD.bazel |
| 8 | Codecov slug + commit 3 (ci) | .github/workflows/coverage.yml |

## Chunks

| id | status | commit(s) | summary | blocker |
|----|--------|-----------|---------|---------|
| 1 | complete | — | origin repointed to dasch-swiss/vitrinli and fetched; docs-build baseline 24 pre-existing warnings | none |
| 2 | complete | docs(docs) | README, mkdocs.yml, docs home and guide renamed; "formerly SIPI" in README, index.md, introduction.md | none |
| 3 | complete | docs(docs) | Lua, operation, release-notes, preservation-pipeline, RELEASING/SECURITY/DEPRECATIONS renamed; lua/index.md anchor now #setup-of-vitrinli-directories | none |
| 4 | complete | docs(docs) | development docs renamed; gh api / gh run commands point at dasch-swiss/vitrinli | none |
| 5 | complete | docs(docs) | agent-context files renamed; CLAUDE.md naming paragraph rewritten; Vitrinli glossary row added; CLAUDE.md:26 miss fixed by session | none |
| 6 | complete | docs(docs) | checks 2-4 as expected; only URL rewrites inside fences; docs build no new warnings | none |
| 7 | complete | build(docker) | OCI image.title=Vitrinli, image.source=dasch-swiss/vitrinli; verified via bazel build //src:image_labels | none |
| 8 | complete | chore(ci) | Codecov slug dasch-swiss/vitrinli; check 1 empty; just commit-lint OK | none |

## Deferrals

- none

## Deviations

- none

## Side findings

- `mkdocs` is not in the nix dev shell, so `just docs-build` fails with exit 127 inside `nix develop`. CI installs it from `docs/requirements.txt`. Locally it builds with `uv run --python 3.12 --no-project --with-requirements requirements.txt -- mkdocs build` from `docs/`; the default uv Python fails to build `watchdog`.
- Review fan-out (consistency, code-simplicity, dune; sonnet) found no Critical. Applied: CLAUDE.md stated "formerly SIPI" twice and duplicated the keep-old-name list now owned by the UBIQUITOUS_LANGUAGE.md Vitrinli row; the row now also says how to name new code (follow surrounding `Sipi`/`sipi` names, never introduce `Vitrinli*`/`VITRINLI_*`) and limits the SIPI/Sipi avoid alias to prose. Folded into docs(docs).
- Commit signing failed transiently mid-rebase (surfaced as a lock/"staged changes" error); retrying `git commit --amend` succeeded.

## Closeout

- root_cause: The GitHub repo and product were renamed (sipi → vitrinli) outside the codebase; every prose mention and repo link still carried the old name, and agents kept propagating it.
- investigation: The rename splits into prose/links (safe) and public contracts (image, binary, SIPI_* env, metrics, service.name, code identifiers) that downstream repos depend on. Only the first was in scope. A blind regex would have broken `Sipi::` identifiers (`:` is a non-word char) and code spans, so workers used fence- and span-aware scripts plus hand edits; one worker's script mis-parsed a single-line triple-backtick span in guide/sipi.md and reverted/re-ran.
- solution: docs(docs) renames the product in README, the mkdocs site, agent-context and operator docs and rewrites dasch-swiss/sipi links (also inside gh commands); build(docker) sets OCI image.title/source; chore(ci) moves the Codecov slug. A Vitrinli glossary row owns the keep-old-name list and the rule for new identifiers.
- prevention: Leftover checks (no dasch-swiss/sipi outside history; no `#…sipi` anchors; no added code span containing vitrinli) and the glossary rule "never introduce Vitrinli*/VITRINLI_* identifiers". Verify OCI labels with `bazel build //src:image_labels`; `just bazel-build` does not build them. `mkdocs` is not in the nix dev shell.
