# Commit and PR Conventions

## Git Workflow

We use a **rebase workflow**. All changes are made on a branch, then
rebased onto `main` before being merged. This keeps a clean, linear
history.

The goal is a **meaningful history on `main`**: every commit on main
should be a deliberate, self-contained unit of change. Working commits
("WIP", "fix typo", "address review feedback") do not belong on main.

- **Rebase-merge**: PRs are integrated using rebase-merge (not squash or
  merge commits). Every commit on the branch lands on main verbatim — so
  the branch history _is_ the main history. There is no squash-on-merge
  safety net; whatever you leave on the branch is what ships.
- **The history's reader is a coding agent.** This is the leading
  principle for cleanup. An agent reaches a commit through `git blame`,
  `git log -- <path>` and `git log -S`, and the commit message is where it
  learns why the code is the way it is. Curate the branch for that reader:
  **one commit per module per slice**, with a body that explains the end
  state. See [Commit Organization](#commit-organization).
- **Clean up before merging (mandatory)**: before a PR is merged, rewrite
  the branch (interactive rebase) to that shape. Squash aggressively, in
  service of that principle: fold working commits, fixups and
  trial-and-error into the commit they belong to, reword messages, and
  reorder as needed.

## Commit message schema

We use [Conventional Commits](https://www.conventionalcommits.org/).
These prefixes drive [release-please](ci.md#release-automation-release-please)
to determine SemVer bumps and generate the changelog —
**using the correct prefix is required, not optional**.

    type(scope): subject
    body

**Every commit has a type and a scope** — both are mandatory (see
[Scopes](#scopes)). This is the whole table; there are exactly eight types:

| Type | Meaning | Changelog section | Version bump |
|--------|---------|-------------------|--------------|
| `feat` | New user-visible functionality | Features | minor |
| `fix` | Bug fix (see [What `fix:` means](#what-fix-means)) | Bug Fixes | patch |
| `perf` | Performance improvement | Performance Improvements | patch |
| `refactor` | Code restructuring, no behavior change | Code Refactoring | patch |
| `docs` | Documentation | Documentation | patch |
| `test` | Adding or refactoring tests | Tests | patch |
| `build` | Build system or dependencies | Build System | patch |
| `chore` | Miscellaneous maintenance | Miscellaneous Chores | patch |

Every type is visible in the release notes, in its own section, rendered in the
order above. Because the scope is mandatory, every changelog line reads
`concern: subject`. This table is the single human source for the type
vocabulary; the machine source is
[`.github/release-please/config.json`](../../../.github/release-please/config.json).

Breaking changes take a `!` suffix (or a `BREAKING CHANGE:` footer) and
bump the **major** version:

    feat(cache)!: remove the deprecated cache-purge endpoint

Example:

    feat(jwt): support more authentication methods

The release-please config sentence-cases subjects in the changelog, so
write the subject in normal prose case (`feat(cache): add dual-limit
eviction`, not `feat(cache): Add ...`).

### Removed types

`revert`, `style`, and `ci` are **not** valid types — the commit-lint gate
rejects them, and release-please hides them if one ever slips onto `main`.

- **CI/build automation** → `chore(ci): ...` (lands in Miscellaneous Chores).
  Dividing CI into concerns is not worthwhile, and a dedicated Continuous
  Integration changelog section is noise; it belongs under maintenance.
- **Formatting-only changes** → fold into the functional commit, or
  `chore(<scope>): ...`. `rustfmt`/`clang-format` are enforced gates, so a
  standalone formatting commit should be rare.
- **Reverts** → `fix(<scope>): revert ...` or `chore(<scope>): revert ...`
  depending on whether it corrects released behavior. (release-please's built-in
  revert auto-negation no longer applies; reverts are rare and handled by hand.)

### What `fix:` means

A `fix:` corrects behavior that exists on `main` — a bug a deployer could
hit today, or that a released version shipped. It earns a "Bug Fixes"
changelog line precisely because deployers need to know. The same change
filed as `chore:` or `refactor:` still appears in the release notes, but
under a section nobody scanning for a regression will read.

A bug you introduced earlier in the **same branch** is not a `fix:`. Fold
it into the commit that introduced it (`git commit --fixup=<sha>`, then
`git rebase --autosquash`) so it never lands on `main` and never generates
a changelog entry for a bug nobody ever saw.

Corollary: if you discover a genuine pre-existing `main` bug while doing
unrelated work, give it its **own** `fix:` commit — don't bury it inside
the `feat:`/`refactor:` you happened to be writing.

## Scopes

**A scope is mandatory.** Every commit is `type(scope): subject`, so every
release-notes line reads `concern: subject`. There is no scopeless form.

The scope names the **concern the change serves** — the module whose
responsibility it belongs to, not the directory the edited files happen to
sit in. Telemetry code under `src/server/rust/` is `observability`; an `ffi`
seam field whose only purpose is to carry a metric across is `observability`
too. Scope by what the change is *about*, not where it landed. The canonical
module list — which is also the scope vocabulary — lives in
[`CONVENTIONS.md` § Module Layout](../../../CONVENTIONS.md). Use one of
these names, lowercase:

- **Module scopes:** `image`, `format_handlers`, `metadata`, `iiifparser`,
  `scripting`, `util`, `jwt`, `cache`, `memory-budget`,
  `memory`, `observability`, `logging`, `cli`, `ffi`, `lua`, `server`
  (`scripting` = the C++ Lua runtime in `src/scripting/`; `lua` stays
  the Lua *scripts*/config in `scripts/` + `config/*.lua`; the retired `handlers`
  scope folded into `iiifparser` per DUNE-014)
- **Test-layer scopes** (`e2e`, `approval`) — for changes to a test
  layer's own harness or fixtures. A test *about* a specific concern takes
  that concern's scope (`test(observability): ...`), not the layer: the
  `test` type already says it is a test, so the scope is free to name the
  concern. A unit-test change takes the scope of the module under test.
- **Cross-cutting scopes** (changes not tied to one module): `deps`,
  `bazel`, `ci`, `nix`, `docker`, `docs`. Use `docs` for the agent-context /
  domain documentation that spans no single module — the root `CLAUDE.md` /
  `CONVENTIONS.md` / `CONTEXT.md` / `UBIQUITOUS_LANGUAGE.md` and the ADRs. A doc
  change about one module still takes that module's scope.

Rules:

- Lowercase, kebab-case. `ci` not `CI`; `bazel` not `bazel-build`.
- A commit that spans several modules may list them comma-separated:
  `refactor(cli,server): ...`.
- **Concern over location.** When code for one module's responsibility lives
  under another module's directory, scope by the responsibility, not the
  enclosing directory. `server/rust/src/metrics.rs` → `observability`.
- **No catch-all.** There is no `repo`/`all` scope. If none of the enumerated
  scopes genuinely fits, ask the maintainer before inventing one — new scopes
  are added to the canonical list in `CONVENTIONS.md` deliberately, not ad hoc.

The commit-lint gate enforces only that a scope is *present* (it does not yet
restrict *which* scope — there is no closed vocabulary). The list above is the
advisory vocabulary; keep to it.

## Enforcement

Commit messages are gated in CI by
[`commitlint-rs`](https://github.com/KeisukeYamashita/commitlint-rs) via the
`commit-lint` job in [`ci.yml`](../../../.github/workflows/ci.yml) (a composite
action at `.github/actions/commit-lint`). The rules live in
[`.commitlintrc.yml`](../../../.commitlintrc.yml): the `type` allowlist above is
mandatory, `scope-empty` makes the scope mandatory, and the subject must be
non-empty.

Run the same check locally before pushing:

    just commit-lint            # lints origin/main..HEAD
    just commit-lint <base-ref> # lints <base-ref>..HEAD

`commitlint-rs` is in the Nix dev shell (`nix develop`); CI installs it with
`cargo install`.

Merge commits are skipped by the linter (message rules do not apply to them). The
rebase workflow is enforced separately by the `require linear history`
branch-protection rule, which blocks any merge commit from landing on `main`.

## Commit Organization

### Principle

Shape the history for the agent that will read it. An agent asks two
questions of it: "why is this line the way it is?" (`git blame`, then the
commit body) and "how did this module get here?" (`git log -- <path>`).
Both are answered best by **one commit per module per slice**:

- **Too fine** (a commit per fix, per review round, per working step) makes
  `git log -- <path>` a list of steps the agent must replay to learn the
  current state, and each body explains one step rather than the design.
- **Too coarse** (the whole PR as one commit when it spans several slices)
  makes `git blame` land every line on one message that cannot explain any
  of them.

A slice is the unit the plan delivered: a phase, or a feature within it.
Inside a slice, split by module (the scopes in
[`CONVENTIONS.md` § Module Layout](../../../CONVENTIONS.md)), so each commit
stays within one module's paths and its scope names that module. A shared
piece a slice adds goes in its own commit ahead of the modules that use it,
so no commit uses something that lands after it. A small PR that is one
slice in one module is one commit.

### The body is written for an agent

The subject says what changed. The body carries what the diff cannot:

- the decisions taken and the alternatives rejected, with the reason;
- the invariants the code relies on and where they are enforced;
- the gotchas a later change would trip over;
- the stable sources behind them (an ADR, a `DEV-` id, a doc page).

It describes the end state of the slice, never the journey to it: no
"first tried", no review-round history, no test counts or benchmark runs,
no behaviour that existed only on the branch. A long body is fine; an agent
reads all of it.

### Rules

1. Each `feat:` or `fix:` commit = one changelog entry in the sections
   developers deploying Vitrinli read first.
2. Internal work (`build:`, `ci:`, `refactor:`, `docs:`, `chore:`,
   `test:`) lands in its own changelog section further down. Squash
   aggressively, in service of the agent reader: fold every working,
   fixup and trial-and-error commit until each commit is one module in
   one slice, and stop there. Squashing past that point makes the history
   too coarse for `git blame`.
3. Ask: "would a developer deploying Vitrinli care about this change?"
   If yes → `feat:` or `fix:`. If no → an internal-work prefix.
4. Debugging journeys (trial-and-error, reverts of in-branch mistakes,
   iterative fixes) belong in the PR description, not the commit history.
   See [What `fix:` means](#what-fix-means).

### Where context lives

| Layer | Audience | Content |
|-------|----------|---------|
| Commit messages | Release notes readers | Every change; `feat:`/`fix:` carry the user-visible ones |
| PR description | Reviewers + future developers | Full context including challenges |
| Learnings docs | Future Claude + engineers | Structured, searchable knowledge |
| Code comments | Code readers | "Why not the obvious approach" |

## PR Description Format

The repository ships a [`.github/PULL_REQUEST_TEMPLATE.md`](../../../.github/PULL_REQUEST_TEMPLATE.md)
that pre-populates this structure when you open a PR.

### Template

```
Fixes LINEAR-ID, LINEAR-ID, ...

## Motivation
Why this work was needed. What problem it solves for users.

## Summary
1-3 bullet points of user-visible changes.

## Key Changes
### [Topic]
- change details

## Challenges and Decisions
What was tried, what failed, and key architecture decisions.
Structure as sub-sections when multiple challenges exist:

### [Challenge title]
**Problem:** description of the issue encountered
**Tried:** approaches that didn't work and why
**Solution:** what worked and why it's the right approach

## Gotchas
Things future developers should know. Each gotcha should be
actionable — not just "this is hard" but "do X instead of Y".

## Test Plan
- [ ] verification steps
```

Use `Part of LINEAR-ID` instead of `Fixes LINEAR-ID` when the PR advances
an umbrella issue it does not close.

### Why this format matters

The "Challenges and Decisions" section captures the debugging journey
that the curated commit history deliberately omits. The
`/eng:workflows:compound` skill reads PR descriptions to generate
structured learnings — well-structured challenges become high-quality
learnings automatically.

### What goes where

| Information | Put it in... |
|-------------|-------------|
| New feature / breaking change | Commit message (`feat:` / `feat!:`) |
| Bug fix | Commit message (`fix:`) |
| Build/CI/refactor details | Commit message (`build:` / `ci:` / `refactor:`) |
| Why the work was needed | PR Motivation section |
| What was tried and failed | PR Challenges section |
| Architecture decisions + rationale | PR Challenges section |
| Things to watch out for | PR Gotchas section |
| Structured, searchable knowledge | Learnings doc (dasch-specs) |
