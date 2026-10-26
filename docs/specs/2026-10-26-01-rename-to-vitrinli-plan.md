---
title: "Rename the product from SIPI to Vitrinli in prose and repo URLs"
type: refactor
date: 2026-10-26
author: Ivan Subotic
status: implemented
repository: dasch-swiss/vitrinli
repositories: []
---

# Rename the product from SIPI to Vitrinli in prose and repo URLs

## Overview

The GitHub repository `dasch-swiss/sipi` is now `dasch-swiss/vitrinli`, and the product is
called Vitrinli. This plan updates the parts that are pure naming: links to the repository,
references to the repository by name, and the product name in the documentation, the
agent-context files and the OCI image labels. Nothing a deployment, a downstream repository
or a dashboard depends on changes.

## Problem Statement / Motivation

The repo still introduces itself as "SIPI (Simple Image Presentation Interface)" in the
README, the docs site and the agent-context files. Agents and readers keep using the old
name in commits, PRs and Linear issues, and the links point at a repository name that only
works through GitHub's redirect.

## Proposed Solution

Three kinds of edit, nothing else:

1. **Repo URLs.** Rewrite `github.com/dasch-swiss/sipi` and `dasch-swiss/sipi` to
   `dasch-swiss/vitrinli` in live files: README, `docs/mkdocs.yml`, `docs/src/**`, the
   Codecov slug in `.github/workflows/coverage.yml:75`, and the OCI `image.source` label
   in `src/BUILD.bazel:385`.
2. **Repo name in prose.** Where lowercase `sipi` names the repository (not the binary, crate
   or image), write `vitrinli`: `CLAUDE.md:37` ("never list `sipi` itself") and
   `CONVENTIONS.md:3,5` ("the sipi repository").
3. **Product name in prose.** Replace the product name `SIPI` / `Sipi` with `Vitrinli` in
   the target files:
   - root: `README.md`, `CLAUDE.md`, `CONTEXT.md`, `UBIQUITOUS_LANGUAGE.md`, `ARCH-MAP.md`,
     `CONVENTIONS.md`, `RELEASING.md`, `SECURITY.md`, `DEPRECATIONS.md`
   - docs: `docs/mkdocs.yml` (`site_name`, home description, nav descriptions),
     `docs/src/**/*.md`, `docs/preservation-pipeline.md`
   - the OCI `image.title` label (`src/BUILD.bazel:389`)

The former name is kept exactly where someone searching for it lands: one "formerly SIPI"
mention in the README, the docs home page, `docs/src/guide/introduction.md`, `CLAUDE.md`, and
an *avoid* alias in the glossary.

## Technical Considerations

### Out of scope (decided)

These stay `sipi` because other systems depend on them. Renaming them is a separate,
cross-repo migration.

- Docker image `daschswiss/sipi` (`src/BUILD.bazel` `oci_push`/`oci_load`, `publish.yml`, `ci.yml`).
- Binary `sipi`, entrypoint `/sbin/sipi`, config path `/sipi/config/sipi.config.lua`, the
  `config/sipi.*.lua` file names.
- `SIPI_*` environment variables, `sipi.*` metric names, OTel `service.name="sipi"`, Sentry tags.
- Code identifiers: `Sipi::` namespace, `SipiImage`, `SipiIO*`, `sipi_*` FFI symbols, the
  `sipi` crate, Bazel `module(name = "sipi")`, targets such as `//src/cli:sipi`, and the 95
  file paths containing `sipi`. The `CONVENTIONS.md` naming rule that new types take the
  `Sipi` prefix (`CONVENTIONS.md:84`, `:87`) stays as it is.
- Docs page file names (`docs/src/guide/sipi.md`): renaming changes published URLs.
- The `sipi.io` domain (`site_url`, `docs/src/CNAME`, `CUSTOM_DOMAIN` in `publish.yml`) and
  the JSON-LD context URL `http://sipi.io/api/file/3/context.json`.
- Historical records: `CHANGELOG.md`, `docs/specs/**`, `docs/archive/**`, `docs/adr/**`,
  `docs/learnings/**`. Their old links redirect.
- Every comment in code and build configuration: `src/`, `test/`, `tools/`, `bazel/`,
  `.github/`, `MODULE.bazel`, `flake.nix`, `osv-scanner.toml`, `rustfmt.toml`, including
  the provenance string in `tools/benchmark/generate_fixtures.sh:120`. The only non-Markdown
  edits are `docs/mkdocs.yml` and the three named lines in `src/BUILD.bazel` and `coverage.yml`.
- Test fixtures and test-layer docs: `test/_test_data/**`, `test/approval/CHANGELOG.approval.md`.

### Replacement rules

A blind `sed` breaks code references. Apply these rules per occurrence:

- **Replace** standalone `SIPI` and `Sipi` used as the product name in prose, including
  hyphenated compounds: `SIPI-internal`, `SIPI-specific`, `SIPI-local`, `SIPI-only`,
  `SIPI-domain`, `SIPI-served` → `Vitrinli-…`. Possessives (`Sipi's`) too.
- **Keep** anything inside a fenced code block or an inline code span, with one exception:
  the URL rule (Proposed Solution 1) applies inside code too, because `gh api
  repos/dasch-swiss/sipi/…` commands in `docs/src/development/ci.md` and
  `rbe-write-pressure.md` should name the current repo. Examples that must
  survive: `password = 'Sipi-Admin'` (`docs/src/guide/running.md:66`, in a `lua` fence) and
  `<title>SIPI Configuration Info</title>` (`docs/src/lua/index.md`, in the `.elua` example).
- **Keep** `Sipi::` and every `Sipi`/`SIPI` that is part of a longer identifier. A
  word-boundary regex alone does not protect `Sipi::` (`:` is a non-word character), so
  handle those occurrences by hand. The heading ``## The value type: `Sipi::SipiValueError` ``
  (`docs/src/development/error-model.md:10`) stays.
- **Keep** `SIPI-2` (the name of the original author's successor repo).
- **Keep** the literal `SIPI:` prefix of the legacy JP2 comment carrier
  (`UBIQUITOUS_LANGUAGE.md`, *JP2 UUID-box carrier* row).
- **Keep** lowercase `sipi` when it names the binary, crate, image, path or command
  (`sipi server`, the `sipi` binary, `knora-sipi`, "`just bazel-coverage` builds sipi" at
  `CONVENTIONS.md:36`). Lowercase `sipi` naming the repository is
  rule 2 of Proposed Solution.
- Where prose about the executable would read wrongly after the rename, name the binary:
  "The SIPI Executable" (`docs/src/guide/sipi.md:47`) becomes "The Vitrinli executable
  (`sipi`)".
- **Drop** the expansion "Simple Image Presentation Interface" as the product's name. It
  survives only in the "formerly SIPI (Simple Image Presentation Interface)" mention, which
  is the README's first paragraph and the first paragraph of
  `docs/src/guide/introduction.md`. That page's H1 becomes `# Vitrinli – Introduction`.
- Grammar: re-read every changed sentence, since "a SIPI …" becomes "a Vitrinli …".

### Headings and anchors

39 Markdown headings in the target files contain the product name and change slug when
renamed (for example `docs/src/guide/sipi.md:200` `#### Setup of SIPI Directories`). Rename
them like any prose. Inbound links from outside the repo to these anchors land on the page
top afterwards; that is accepted. Internal links must be updated in the same edit. Today there
is exactly one: `docs/src/lua/index.md:91` → `../guide/sipi.md#setup-of-sipi-directories`.
`docs/mkdocs.yml` has no `strict`, so the docs build does not catch a broken anchor; the
leftover check below does.

### Specific edits

- `CLAUDE.md`: the opening sentence and the **Naming** paragraph become one statement:
  Vitrinli (formerly SIPI) is the media server; code identifiers, the binary, the Docker
  image and the `SIPI_*` variables keep the `sipi` name. Keep the Linear search guidance
  (search text for both "sipi" and "vitrinli").
- `UBIQUITOUS_LANGUAGE.md`: add a **Vitrinli** term row (the product; code-level
  identifiers keep the `Sipi`/`sipi` prefix) with `SIPI, Sipi` in the *avoid* column. Keep
  the existing `SIPI UUID` and `sipi metadata` entries in *avoid* columns as they are.
- `README.md`: title becomes `# Vitrinli`; the first paragraph names it "formerly SIPI
  (Simple Image Presentation Interface)". The badge URL changes per the URL rule; the Docker
  Hub links and `SIPI_SENTRY_*` variable names stay.
- `docs/src/index.md`: front-matter `title`/`description` and the H1 become Vitrinli; add
  the "formerly SIPI" mention.
- `src/BUILD.bazel:389`: `org.opencontainers.image.title=Vitrinli`. The labels are OCI
  metadata only; no test asserts on them (`image.title` appears only in `src/BUILD.bazel`).

### Commit structure

One commit per scope (rebase-merge puts each on `main`):

1. `docs(docs): rename the product from SIPI to Vitrinli`: every Markdown file and
   `docs/mkdocs.yml`. `docs` is the cross-cutting scope for documentation not tied to one
   module (`CONVENTIONS.md` § Canonical modules).
2. `build(docker): label the image Vitrinli from dasch-swiss/vitrinli`: `src/BUILD.bazel`
   `image.title` and `image.source`.
3. `chore(ci): point the Codecov upload at dasch-swiss/vitrinli`: `coverage.yml` slug.

## Implementation Approach

- [x] Point the local `origin` at the new repo: `git remote set-url origin git@github.com:dasch-swiss/vitrinli.git`
- [x] Run `git fetch origin`; `origin/main` is the base for every diff below
- [x] Inside `nix develop`, run `just docs-build` before any edit and note its warning lines as the baseline
- [x] Rewrite `dasch-swiss/sipi` repo URLs in `README.md`
- [x] Rewrite `repo_url` and `repo_name` in `docs/mkdocs.yml`
- [x] Rewrite `dasch-swiss/sipi` repo URLs in `docs/src/**` (bazel.md, ci.md, kakadu.md, rbe-write-pressure.md, introduction.md, running.md, release-notes/index.md)
- [x] Rewrite the lowercase repository-name references in `CLAUDE.md:37` and `CONVENTIONS.md:3,5`
- [x] Rename the product in `README.md`, with the "formerly SIPI" mention
- [x] Rename the product in `docs/mkdocs.yml` (`site_name`, home description, nav descriptions)
- [x] Rename the product in `docs/src/index.md`, with the "formerly SIPI" mention
- [x] Rename the product in `docs/src/guide/**`, with the "formerly SIPI" mention in `introduction.md`
- [x] Update the anchor link at `docs/src/lua/index.md:91` to the renamed heading's slug
- [x] Rename the product in `docs/src/lua/**`
- [x] Rename the product in `docs/src/operation/**`
- [x] Rename the product in `docs/src/development/**`
- [x] Rename the product in `docs/preservation-pipeline.md`
- [x] Rewrite the naming paragraph and the product mentions in `CLAUDE.md`
- [x] Rename the product in `CONTEXT.md`
- [x] Rename the product in `UBIQUITOUS_LANGUAGE.md`
- [x] Add the **Vitrinli** term row to `UBIQUITOUS_LANGUAGE.md`
- [x] Rename the product in `ARCH-MAP.md`
- [x] Rename the product in `CONVENTIONS.md` prose, leaving the `Sipi` prefix naming rule
- [x] Rename the product in `RELEASING.md`, `SECURITY.md` and `DEPRECATIONS.md`
- [x] Run `just docs-build` inside `nix develop`; no warning line appears that is not in the baseline
- [x] Run leftover checks 2, 3 and 4 from Success Metrics; each gives its expected result
- [x] Review `git diff origin/main -- '*.md' docs/mkdocs.yml ':!docs/specs'`: no changed line inside a fenced block except a `dasch-swiss/sipi` URL rewrite
- [x] Commit 1 (`docs(docs)`)
- [x] Set `image.title=Vitrinli` and `image.source=https://github.com/dasch-swiss/vitrinli` in `src/BUILD.bazel`
- [x] Run `bazel build //src:image_labels`; `bazel-bin/src/image_labels.txt` contains `image.title=Vitrinli` and `image.source=https://github.com/dasch-swiss/vitrinli` (`just bazel-build` builds only `//src/cli:sipi` and would not touch the labels)
- [x] Commit 2 (`build(docker)`)
- [x] Set `slug: dasch-swiss/vitrinli` in `.github/workflows/coverage.yml`
- [x] Commit 3 (`chore(ci)`)
- [x] Run leftover check 1 from Success Metrics; it gives no output
- [x] Run `just commit-lint`; it passes

## Human Actions

| Id | Action | Who | When | Why not the agent |
|----|--------|-----|------|-------------------|
| H1 | Push the branch and open the PR | Maintainer | after ship | Pushing is the maintainer's call |
| H2 | Confirm the first `coverage.yml` run on `main` uploads to Codecov under `dasch-swiss/vitrinli` | Maintainer | after ship | Needs the Codecov dashboard login |
| H3 | Update the GitHub repo description/homepage and the Docker Hub `daschswiss/sipi` description to say Vitrinli | Maintainer | after ship | Org/registry settings, outside the repo |

## Acceptance Criteria

- [x] No live file outside the out-of-scope list links to `dasch-swiss/sipi`.
- [x] Every target file calls the product Vitrinli, with "formerly SIPI" in the README, the docs home page, `introduction.md` and `CLAUDE.md`.
- [x] Every code span, code block, identifier, command, path, image name and environment variable is unchanged.
- [x] No internal anchor link targets a `…sipi…` slug, and `docs/src/lua/index.md:91` targets `#setup-of-vitrinli-directories`.
- [x] `just docs-build` passes with no new warnings.
- [x] `bazel build //src:image_labels` passes and its output carries the new title and source.
- [x] `just commit-lint` passes.

## Dependencies & Risks

- **Low risk.** No runtime, API or deploy contract changes. The OCI label change alters
  image metadata only.
- **Main risk: over-replacement** of `Sipi::…` or code spans. Mitigated by the replacement
  rules and leftover check 3, which inspects added lines rather than remaining ones.
- **External anchors** to renamed headings on `sipi.io` stop scrolling to the section. Accepted.
- **Mixed naming is intentional.** Readers see "Vitrinli" in prose next to `sipi server`
  and `daschswiss/sipi`. The "formerly SIPI" note and the `CLAUDE.md` naming paragraph
  explain why.
- `just commit-lint` checks types and that a scope is present; `.commitlintrc.yml` has no
  scope allowlist, so scope validity rests on `CONVENTIONS.md`.

## Success Metrics

Baseline before the change: check 1 reports 20 lines in 11 files; check 2 reports 355
lines; check 4 reports 1 line (`docs/src/lua/index.md:91`).

Leftover checks after the change:

```sh
# 1. repo URLs — expected: no output
git grep -n "dasch-swiss/sipi" -- . ':!CHANGELOG.md' ':!docs/specs' ':!docs/archive' ':!docs/adr' ':!docs/learnings' ':!tools'

# 2. remaining product names — expected: only code spans/blocks, `Sipi::` identifiers,
#    SIPI-2, the `SIPI:` carrier literal, *avoid* columns, the `Sipi` prefix rule in
#    CONVENTIONS.md, and the "formerly SIPI" mentions
git grep -nwE "SIPI|Sipi" -- README.md CLAUDE.md CONTEXT.md UBIQUITOUS_LANGUAGE.md ARCH-MAP.md \
  CONVENTIONS.md RELEASING.md SECURITY.md DEPRECATIONS.md docs/mkdocs.yml \
  docs/preservation-pipeline.md 'docs/src/*.md'

# 3. over-replacement — expected: only lines produced by the URL rule, the repo-name
#    rule (Proposed Solution 1 and 2), CLAUDE.md's naming paragraph and the new
#    Vitrinli glossary row
git diff origin/main -U0 -- '*.md' ':!docs/specs' \
  | perl -ne 'next unless /^\+/; while (/`([^`]*)`/g) { if ($1 =~ /vitrinli/i) { print; last } }'

# 4. anchors — expected: no output
git grep -nE '\]\([^)]*#[^)]*sipi' -- '*.md' ':!docs/specs' ':!docs/archive' ':!docs/adr' ':!CHANGELOG.md'
```

In a git pathspec without `:(glob)`, `*` crosses `/`, so `'docs/src/*.md'` covers every
Markdown file under `docs/src/`. Checks 2 to 4 run after the Markdown edits; check 1 runs after commit 3, since commits 2
and 3 carry its last two hits. Check 3 lists added lines only, so the fenced-block diff review
in Implementation Approach complements it.

## References

- Naming note and Linear labels: `CLAUDE.md` (Project Overview, **Naming**)
- Commit scopes: `CONVENTIONS.md` § Canonical modules (`docs`, `docker`, `ci` cross-cutting scopes)
- Commit schema: `docs/src/development/commit-conventions.md`; type allowlist `.commitlintrc.yml`
- OCI labels: `src/BUILD.bazel:385-390`
- Codecov slug: `.github/workflows/coverage.yml:75`
- Docs site: `docs/mkdocs.yml`, `just docs-build`
