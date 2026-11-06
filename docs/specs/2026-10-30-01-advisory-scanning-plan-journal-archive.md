# Execution Journal Archive: 2026-10-30-01-advisory-scanning-plan

## Phase 1

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 1.1 | vitrinli | justfile | — | "- [ ] Add `just bazel-distroless-lock`" | `just bazel-distroless-lock` runs `bazel run @bookworm//:lock` | justfile § Bazel Docker (rules_oci) | — |
| 1.2 | vitrinli | bazel/distroless/bookworm.yaml, bazel/distroless/bookworm.lock.json, MODULE.bazel, src/BUILD.bazel, docs/src/development/downstream-dependencies.md | 1.1 | "- [ ] Remove `curl` from `packages:`…", "- [ ] Rewrite the comments…", "- [ ] Drop `curl` from downstream-dependencies.md", "- [ ] Run `just bazel-build` and `just bazel-coverage`" | lock regenerated with GH_TOKEN unset + empty GH_CONFIG_DIR; no curl/libcurl4/libssl3/libgnutls30 per arch; comments say healthcheck is `sipi health`; build + coverage green | MODULE.bazel § apt.install; src/BUILD.bazel § runtime extras (~527); downstream-dependencies.md § Runtime Image Packages | — |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 1.1 | vitrinli | done | f00ddfb1 | `just bazel-distroless-lock` wraps `bazel run @bookworm//:lock` | — |
| 1.2 | vitrinli | done | a5e857fb | Debian curl dropped from bookworm.yaml; lock 33 to 4 packages per arch; comments and downstream-dependencies.md say healthcheck is `sipi health` | — |
| 1-review-1 | vitrinli | done | 9bc8b5e9 | review fix: runtime-extras comment points at the lock; `sipi health` named the compose-level healthcheck (image has no HEALTHCHECK) | — |

## Phase 2

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 2.1 | vitrinli | tools/advisories/{_model.py,inventory.py,advisories.toml,components.toml}, tools/advisories/tests/{__init__.py,test_inventory.py,fixtures/*} | — | "components.toml", "advisories.toml", "eol", "_model.py + inventory.py" (Bazel inputs part), "drift check" | crates, BCR (source.json, .bcr.N stripped), http_archive (version_re), deb purls with derived distro, base digest, eol carried; drift classes fail exit 1, --best-effort skips; unittest green | plan § Proposed Solution 1; § Technical Considerations "Tool conventions" | — |
| 2.2 | vitrinli | tools/advisories/inventory.py, tools/advisories/tests/test_inventory.py, tools/advisories/tests/fixtures/* | 2.1 | "_model.py + inventory.py" (uses: part), "tests" | uses: refs resolved from recorded ls-remote: SHA two tags, lightweight, tag ref, sub-path, job-level, branch, docker://, unresolved SHA, missing repo | plan § Technical Considerations "Actions pins" | — |
| 2.3 | vitrinli | justfile, .gitignore | 2.2 | "just advisories-inventory/test; gitignore", "Run just advisories-test ...", "Run just bazel-build and just bazel-coverage" (build part) | recipes run; real inventory covers every shipped entry, crate, deb package, base digest | justfile `audit` recipe area | — |
| 2.4 | vitrinli | CLAUDE.md | — | "Correct CLAUDE.md's dependency list" | libexpat listed as http_archive / native cc_library, not BCR | CLAUDE.md § Dependencies | — |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 2.1 | vitrinli | complete | fef5f0a7 | inventory.py + _model.py over crates, BCR source.json, http_archive URLs, deb lock, oci.pull digest; components.toml (67 BCR + 10 http_archive + 1 git_override + 3 custom rules); drift check + --best-effort; 23 tests | none |
| 2.2 | vitrinli | complete | 56f09650 | pkg:githubactions components from uses: refs via git ls-remote (most specific release tag; branch/untagged SHA/docker:///missing repo unresolved); recorded ls-remote fixtures; 37 tests | none |
| 2.3 | vitrinli | complete | 8ef22714 | just advisories-inventory / advisories-test, /advisories.cdx.json gitignored; real inventory: 17 BCR + 9 http_archive + Kakadu + 448 crates + 8 debs + debian OS + base digest + 15 Actions; bazel-build ok, bazel-test 78/78; coverage checkbox split, remainder unticked (macOS segfault) | none |
| 2.4 | vitrinli | complete | c54ed203 | CLAUDE.md: libexpat moved from BCR drop-ins to native cc_library | none |
| 2-review-1a | vitrinli | complete | 9f7c7670 | fixup! 2.1: shipped entry without version -> exit 3 (was CPE `:None:`); deb name percent-encoded (libstdc++6); non-crates.io crate source -> exit 3; output unlinked before run (no stale file on 1/3); drift message names components.toml and the statuses; duplicate eol check removed; map legend documents eol | none |
| 2-review-1b | vitrinli | complete | 5265d5ca | fixup! 2.2: git ls-remote with LC_ALL=C (missing-repo detection is English-only); Actions ref merge rebuilt without mutating a frozen Component; RepoMissing-not-retried test | none |
| 2-review-1c | vitrinli | complete | 72c7bc01 | fixup! 2.3: `just --list` help line states network need and exit 1 | none |
| 2-review-1d | vitrinli | complete | c32b25ac | CLAUDE.md: new Bazel dependencies are classified in tools/advisories/components.toml (DUNE-001 stopgap) | none |

### Settled side findings
- [settled] Phase 2: CPEs without known NVD history are best-guess vendor:product (cli11 `cliutils:cli11`, highway `google:highway`, libdeflate `libdeflate_project:libdeflate`, abseil-cpp `abseil:abseil-cpp`); a wrong prefix yields silent misses, which the pin table (Phase 7) covers. Worth checking against the NVD CPE dictionary in Phase 3.
  settled: 2026-11-05, checked against the NVD CPE API in Phase 3; abseil corrected to `abseil:common_libraries` in d28185d9, cli11/highway/libdeflate have no NVD product

## Phase 3

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 3.1 | vitrinli | tools/advisories/tests/fixtures/spike.cdx.json | - | Phase 3 #1 (fixture) | one known-vulnerable component per class (cargo, deb with derived distro, CPE libtiff + curl, githubactions tj-actions/changed-files 45.0.0, container with an older base-debian12 digest), shapes as in fixtures/tree/expected.cdx.json; each advisory confirmed against OSV; advisories-test green | plan Phase 3 + Technical Considerations "Scanner choice" | - |
| 3.3 | vitrinli | tools/advisories/inventory.py, components.toml, tests/test_inventory.py, tests/fixtures/tree/expected.cdx.json, tests/fixtures/spike.cdx.json | 3.1 | (none; spike evidence, Phase 3 #2) | deb purl carries `upstream=<source>` from the lock pool URL; resolved Actions carry `syft:package:type=github-action`; abseil CPE `abseil:common_libraries`; fixture in the same shapes, 55116b82 body corrected; advisories-test green | spike results in journal Side findings | - |
| 3.2 | vitrinli | docs/adr/0029-advisory-scanning-from-the-bazel-graph.md | 3.1, 3.3 | Phase 3 #2 (spike), #3 (ADR) | spike run with pinned OSV-Scanner + Grype (version, sha256), per-class results, deb qualifier, Grype `@<digest>`, exit codes, DB date field in journal and ADR; decision rule applied; ADR records inventory over image, chosen scanner(s), alert-only classes, three reversals, rejected alternatives; no reference into docs/specs/ | plan Phase 3, Prior decisions, Alternative Approaches | - |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 3.1 | vitrinli | done | 55116b82 | spike.cdx.json: smallvec 1.6.0, libc6 2.36-9+deb12u1 (distro=debian-12), curl 8.3.0 (bcr, CPE), tiff 4.5.0 (http_archive, CPE), tj-actions/changed-files 45.0.0, base-debian12@sha256:74ddbf52 (uploaded 2025-01-12); advisories-test 42/42 | - |
| 3.3 | vitrinli | done | d28185d9, 46469366, a9f53374 | `fixup!` 51fbcf30: deb purl `upstream=<source>` from the pool URL (exit 3 without one), abseil CPE -> `abseil:common_libraries`; `fixup!` 52019c49: resolved Actions carry `syft:package:type=github-action`; `amend!` 55116b82: fixture in the new shapes, body corrected (CVE-2023-0795 does not affect tiff 4.5.0 -> CVE-2023-40745; CVE-2023-38545 is filed under haxx:libcurl -> CVE-2023-46218). Canary: reverting inventory.py fails 5 tests. advisories-test 44/44 | - |
| 3.2 | vitrinli | done | abbd1572 | spike run (OSV-Scanner 2.7.0, Grype 0.121.1, DB v6.1.10 built 2026-11-05T06:21:06Z) + ADR-0029: OSV-Scanner for cargo, Grype for deb, CPE, base image, Actions; per-class table, exit codes, DB date field, alert-only classes, three reversals, rejected alternatives; ticks Phase 3 #2 and #3 | - |
| 3.4 | vitrinli | done | 6bee8d4d | `fixup!` 52019c49: composite-action fixture `ci/actions/local/` -> `ci/actions/composite/`; root `.gitignore` `local` ignored it, so a clean checkout failed 3 inventory tests (found running the tests at each autosquashed commit in a scratch worktree; all 8 commits now pass) | - |
| 3-review-1 | vitrinli | done | 9779cd52, 26d694f1, 1546223e | review round 1: pool regex accepts `pool/updates/` (debian-security; old form read `g` as source) and is anchored on `.deb`, tests for security layout + no/none/two sources (canary: old regex fails the security test); cli11/highway/libdeflate CPEs annotated as matching nothing; root `.gitignore` `lib`/`local` anchored (nothing tracked or untracked matched below root); ADR: `just advisories-scan` as sole scanner entry point, class count, S2-30 wording, Dependabot already off, anecdote and eol-provider note dropped. advisories-test 45/45 | - |
| 3.5 | vitrinli | done | ecc3be86 | `fixup!` ADR-0029: Grype DB size stated as ~200 MB download, 3.4 GB unpacked; scanners called only through the scan wrapper behind `just advisories-scan` (so Phase 5's `diff.py` -> `scan.py` stays within it). Own-record correction, not a review round | - |

## Phase 4

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 4.1 | vitrinli | tools/advisories/{_model,inventory}.py, components.toml, tests/test_inventory.py, tests/fixtures/tree/{components.toml,expected.cdx.json} | - | (from Phase 3) several CPEs | map `cpe` string or list; extras as repeated `syft:cpe23` properties; curl has haxx:curl + haxx:libcurl; advisories-test green | plan Phase 4; journal Deferrals Phase 3 | - |
| 4.2 | vitrinli | tools/advisories/scan.py, advisories.toml, tests/test_scan.py, tests/fixtures/scan/* | 4.1 | scan.py; test_scan.py; (from Phase 3) empty SBOM + --platform | wrapper per plan Scanner invocation; tests per checkbox; advisories-test green | plan Technical Considerations; ADR-0029 "How the wrapper must read each scanner" | - |
| 4.3 | vitrinli | tools/advisories/ignores.toml, tests/test_scan.py | 4.2 | ignores.toml | four entries with component, reason, bare expiry; real file passes lint | osv-scanner.toml | - |
| 4.4 | vitrinli | justfile, .gitignore, flake.nix | 4.3 | just advisories-scan; flake.nix scanners | recipe runs inventory then scan.py; grype in dev shell | justfile advisories-* recipes | - |
| 4.5 | vitrinli | docs/adr/0029-advisory-scanning-from-the-bazel-graph.md | 4.4 | (ADR Not-yet-in-code line) | wrapper and ignores no longer listed as not in code | ADR-0029 intro | - |
| 4.6 | vitrinli | (none: verification) | 4.5 | run advisories-test + advisories-scan; bazel-build + bazel-coverage | baseline per-class counts in Side findings; build + test green | brief notes | - |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 4.1 | vitrinli | done | ac43b9cc | map `cpe` string or list; extras emitted as repeated `syft:cpe23` properties (`Component.extra_cpes`); curl gets haxx:curl + haxx:libcurl; advisories-test 47/47 | - |
| 4.2 | vitrinli | done | 4dac437c | `scan.py`: OSV-Scanner for crates, Grype for the rest plus `registry:` base digest per `base_image.platforms`; explicit empty scanner configs (the repo osv-scanner.toml would hide all 3 crate findings); exit-code contract per scanner; empty Grype SBOM exits 3; `Finding` in `_model.py`; alias merge; EOL; ignore lint + apply; fixtures recorded from OSV-Scanner 2.7.0 / Grype 0.121.1; mutation canaries fail the tests. advisories-test 75/75 | - |
| 4.3 | vitrinli | done | 916f28a5 | `ignores.toml` with the four entries (component-scoped, expiry 2027-02-05); `advisories.toml` names it; test lints the real file. advisories-test 76/76 | - |
| 4.4 | vitrinli | done | ed82df1e | `just advisories-scan` (inventory then scan.py, gitignored advisories-findings.json); `grype` in the dev shell (nixpkgs 0.114.0; osv-scanner 2.3.3 already there); dev-shell and ADR-pinned versions give the same 268 findings | - |
| 4.5 | vitrinli | done | 59a156ab | ADR-0029: wrapper and ignores.toml no longer under Not yet in code | - |
| 4-review-1 | vitrinli | done | 2ee99435, ddb80c56, 7a3c9ad8 | `fixup!` round 1: osv-scanner.toml names ignores.toml as owner until it goes with `just audit` (consistency + dune DUNE-001); `merge` rewritten with named variables; `cpe` normalized to a list once in `load_map`; tests: PATH vs ADVISORIES_BIN precedence actually exercised, both image platform scans merge into one finding, bridging-alias merge with severity max and lowest fix, repo ignores checked for duplicate (component, id) instead of a count. Autosquash dry run onto 8fe0fcdf clean, tree equal, advisories-test green at every commit. advisories-test 77/77 | - |
| 4-review-2 | vitrinli | done | 776e5718, c040b998, 50d089a3 | `fixup!` round 2 from the re-review: parity test that osv-scanner.toml's ids equal ignores.toml's (dune DUNE-001 repeated; canary: changing one id fails it), comment says edit ignores.toml first; `load_map` binds `cpes` once (simplicity); one redundant `merge` comment line dropped. Cap reached, round 2 not re-reviewed. Autosquash dry run clean, tree equal, advisories-test green at every commit (78/78) | - |

## Phase 5

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 5.1 | vitrinli | tools/advisories/inventory.py | - | (prep for 5.2) | `build()` takes the Actions lookup; existing tests green | plan TC "Actions pins" | - |
| 5.2 | vitrinli | tools/advisories/diff.py, tools/advisories/scan.py, tools/advisories/tests/test_diff.py, justfile, .gitignore | 5.1 | Phase 5 #1, #2 | test_diff cases pass; real diff vs origin/main completes | plan Phase 5 #1-2, TC "Diffing against a base" | - |
| 5.3 | vitrinli | .github/actions/advisories-scanners/action.yml, .github/workflows/ci.yml | 5.2 | Phase 5 #3, #4 | actionlint clean; sha256 verified by download | plan TC "Pinned scanner installs", "GitHub mechanics" | - |
| 5.4 | vitrinli | justfile, osv-scanner.toml, tools/advisories/tests/test_scan.py, flake.nix, MODULE.bazel | 5.3 | Phase 5 #5, #6, #7, #9 | no `just audit`/cargo-audit left outside docs; advisories-test green | plan Phase 5 #5-9 | - |
| 5.5 | vitrinli | docs/src/development/ci.md, docs/adr/0029-advisory-scanning-from-the-bazel-graph.md, CLAUDE.md | 5.4 | Phase 5 #10 | no `dependency-audit`/`just audit`/cargo-audit mention in docs | plan Phase 5 #10 | - |
| 5.6 | vitrinli | ARCH-MAP.md | 5.4 | Phase 5 #8 | glob gone, via dune:dune-map | plan Phase 5 #8 | - |
| 5.7 | vitrinli | (gates) | 5.6 | Phase 5 #11 (build part) | just bazel-build + bazel-test green; coverage unenforceable | brief decided_behaviours | - |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 5.1 | vitrinli | done | 8ed3b464 | `inventory.build()` takes the `ls_remote` lookup (in-process form for diff.py: one memoised lookup shared by head and base) | - |
| 5.2 | vitrinli | done | 7be3b1f1 | diff.py + test_diff.py (19 tests, canaries: id-only key, no subtraction, second `today`, unshared lookup each fail) + `just advisories-scan-diff base`; recipe param named `base` | - |
| 5.3 | vitrinli | done | 03a54109 | `advisories-scanners` action (OSV-Scanner 2.7.0, Grype 0.121.1, sha256 re-verified by download 2026-11-05, Linux X64 guard) + `advisories` job (ubuntu-24.04, timeout 30 min, base ref via env); actionlint clean | - |
| 5.4 | vitrinli | done | 06d98d4c | `just audit` + cargo-audit flags, `osv-scanner.toml`, parity test, `cargo-audit` (dev shell) removed; osv-scanner + grype share one flake comment; MODULE.bazel `cargo_lockfile` comment names `just advisories-scan` | - |
| 5.5 | vitrinli | done | c9664490 | ci.md advisory section rewritten (all listed topics; quarterly review kept until Phase 7, Scout sentence left for Phase 6); ADR-0029 "Not yet in code" drops the PR job; CLAUDE.md names the PR job | - |
| 5.6 | vitrinli | blocked | - | ARCH-MAP.md `:(glob)osv-scanner.toml` (Build & tooling, line 275) not dropped | `dune:dune-map` requires AskUserQuestion confirmation before any overwrite of an existing map; the executor has none. Session: run `/dune:dune-map update` (or confirm the one-glob edit) |
| 5.7 | vitrinli | done | - | gates after 5.5: `just bazel-build` ok, `just bazel-test` 78/78 (MODULE.bazel.lock unchanged by the comment edit); `just bazel-coverage` remainder left unticked (unenforceable on macOS 27.0.1) | - |
| 5-review-1a | vitrinli | done | 33b5716f | fixup diff: drop unused `diff(ls_remote=)`; print base drift; base missing input exits 3 (doc+test); prune worktree on every path; 6 test gaps closed | - |
| 5-review-1b | vitrinli | done | 156e5a94 | fixup ci: `if: github.event_name == 'pull_request'` (dispatch has no base_ref); comment no longer implies required; `--retry-all-errors`; crane note dropped | - |
| 5-review-1c | vitrinli | done | c25a4eef | fixup docs: dispatch skip, not-yet-required, Dependabot alerts vs version PRs | - |
| 5-review-2 | vitrinli | done | cb1e221a | fixup docs: rewrap; ci.md says base drift makes that component's findings read as new, and a base missing an input file exits 3 | - |

## Phase 6

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 6.1 | vitrinli | tools/advisories/diff.py, tools/advisories/advisories.toml, tools/advisories/tests/test_diff.py, justfile | - | Phase 6 #1 (baseline resolution + SBOM tooling part) | `release_baseline` skips a tag absent from Docker Hub; repo from advisories.toml; `--sbom` drops `advisories:scope=ci`; advisories-test green | plan Technical Considerations "GitHub mechanics" | - |
| 6.2 | vitrinli | .github/workflows/publish.yml, justfile | 6.1 | Phase 6 #1 (job), #2 | `advisories-gate` in `release-gate` needs; no Scout step, SBOM upload or sbom_* keys; actionlint clean | plan Phase 6 | - |
| 6.3 | vitrinli | .github/workflows/ci.yml, justfile | - | Phase 6 #3, #4 | no Scout step, SARIF upload, pull-requests/security-events write; actionlint clean | plan Phase 6 | - |
| 6.4 | vitrinli | docs/src/development/rbe.md, docs/src/development/ci.md, RELEASING.md, docs/adr/0029-advisory-scanning-from-the-bazel-graph.md | 6.2, 6.3 | Phase 6 #5, #6, #7, #8 | `grep -rni scout .github justfile docs/src` empty | plan Phase 6 | - |
| 6.5 | vitrinli | (verification only) | 6.1-6.4 | Phase 6 #9, #10 | actionlint, `just advisories-scan-diff v10.0.1`, bazel-build + bazel-test | plan Phase 6 | - |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 6.1 | vitrinli | done | d9cc2ee8 | `diff.py --release-baseline` (+ `just advisories-release-baseline`), Docker Hub repo from `[release]` in advisories.toml; `--sbom` drops `advisories:scope=ci`; 15 tests; advisories-test 114/114 | - |
| 6.2 | vitrinli | done | 04d255a9 | `advisories-gate` job (baseline, scan-diff, `sbom-<tag>` artifact) in `release-gate` needs; Scout steps, SBOM upload, sbom_* keys gone; actionlint clean except the pre-existing SC2086 in the sentry job (also at d2056e06) | - |
| 6.3 | vitrinli | done | 31cda1b3 | ci.yml: Scout compare/SARIF steps, SARIF upload, pull-requests/security-events write, Scout comments gone; justfile smoke comment rewritten; actionlint clean | - |
| 6.4 | vitrinli | done | 0bc2c294 | rbe.md, ci.md, RELEASING.md Scout-free and name advisories-gate + release SBOM; ADR-0029 Not-yet-in-code line drops the release gate; `grep -rni scout .github justfile docs/src` empty | - |
| 6-review-1 | vitrinli | done | 9e540499, 1c73de40, 9f75c1ae | review round 1: `--release-baseline` rejects output flags; HTTPError closed, http.client errors retried; stale-SBOM removal pinned; upload `if-no-files-found: error`; publish.yml comment -> ci.md pointer; exit-3 re-run wording says transient | - |
| 6-review-2 | vitrinli | done | 1e365897, ccaae6a9 | review round 2: test pins http.client retry; publish.yml job comment keeps the re-run rule with the full ci.md path | - |
| 6.5 | vitrinli | done | - | verification: `just bazel-build` ok + `just bazel-test` 78/78 on macOS at 0bc2c294 (later commits touch only Python, YAML, docs); advisories-test 114/114 at HEAD; actionlint clean (publish.yml: pre-existing SC2086 only); scan-diff v10.0.1 exit 0; coverage checkbox split, remainder unticked (decided unenforceable gate) | - |

## Phase 7

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 7.1 | vitrinli | tools/advisories/report.py, tools/advisories/tests/test_report.py | - | Phase 7 #1, #2 | body: open findings, Grype DB date, ignores expiring within 14 days, approaching EOL within 90 days, pin table (http_archive URL, version_files match, Actions per (repo, ref), SHA refs dated); cap 60 000 with run-summary link; prints `open=`/`label=`; exit 3 on error; advisories-test green | plan Technical Considerations "Rolling issue", "CPE results are weak evidence" | - |
| 7.2 | vitrinli | (verification, no files) | 7.1 | Phase 7 #5 | `just advisories-scan` exits 0/1 with findings file; report.py renders the baseline findings | plan Phase 7 | - |
| 7.3 | vitrinli | .github/workflows/advisories.yml, .github/workflows/fuzz.yml | 7.1 | Phase 7 #3, #4 | cron 41 5, dry_run, concurrency advisories, scan/issue jobs with decided permissions; scan exit 1 green, 3 red; issue job skipped on scan failure; fuzz note names both schedules; actionlint clean | plan Phase 7; "Permissions", "Schedules", "GitHub mechanics" | - |
| 7.4 | vitrinli | docs/src/development/ci.md, docs/adr/0029-advisory-scanning-from-the-bazel-graph.md | 7.3 | Phase 7 #6 (added) | ci.md describes the daily workflow and rolling issue; ADR "Not yet in code" names only the fix PRs | ci.md § Dependency advisory scanning; ADR-0029 status | - |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 7.1 | vitrinli | done | 8dbf1266 | report.py renders the issue body (findings, Grype DB date, expiring ignores, approaching EOL, pin table via git log -S), caps at 60 000 with a run-summary link, prints open=/label=; test_report.py over the recorded scan fixtures and a temp git repo; advisories-test 131/131 | - |
| 7.2 | vitrinli | done | - | local `just advisories-scan` (nix dev shell, grype 0.114.0): exit 1, 265 open, 3 ignored (bcr 94, deb 43, http_archive 75, oci 53); report.py renders 19 755 chars, both EOL findings listed, pin table with FFmpeg, Kakadu (2026-06-02) and 19 Actions rows | - |
| 7.3 | vitrinli | done | 3de50a1b | advisories.yml: scan job (exit 1 + findings file green; drift/exit 3 red), issue job gated on scan success, full history, report.py to summary + capped body, gh find/create/update/close by label + fixed title, dry_run skips the gh step; fuzz.yml note names both schedules; actionlint clean | - |
| 7.4 | vitrinli | done | 5ca72860 | ci.md describes the daily workflow and rolling issue; ADR-0029 "Not yet in code" names only the fix PRs | - |
| 7-review-1a | vitrinli | done | 6183ad26 | inventory.http_archive_url shared with report.py (dune DUNE-001, partial) | - |
| 7-review-1b | vitrinli | done | a7d42445 | fixup report.py: issue_label read inside the try (python, Critical: a missing key exited 1 = findings); version_files entry without version_re raises; plain last_change + require_full_history; deterministic pin sort; docstring states the -S occurrence semantics; tests: EOL-alone open=0 via main, cap edges, expiry lower edge, missing label, missing version_re | - |
| 7-review-1c | vitrinli | done | 9c3fbb03 | fixup advisories.yml: issue step only for schedule or a default-branch dispatch (devops + consistency Warning); RUN_URL once at job level; one edit site, close after it | - |
| 7-review-1d | vitrinli | done | 69e81eb3 | fixup ci.md: report paragraph after the exit-codes text, report.py exits 0 or 3, pin-date occurrence wording, Fuzz testing no longer says fuzz.yml is the only scheduled workflow | - |

## Phase 8

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 8.1 | vitrinli | justfile, MODULE.bazel (repin comment) | - | P8 #1 | `just -n bazel-repin-crate tokio` prints both fetches | plan Phase 8, TC "Crate repin and the lockfile" | - |
| 8.2 | vitrinli | tools/advisories/fix.py, tools/advisories/tests/test_fix.py | 8.1 | P8 #2 | target choice, alert-only classes, reverse-dep walk tested; advisories-test green | plan Proposed Solution 4 | - |
| 8.3 | vitrinli | (merged into 8.2: same new file) | 8.2 | P8 #3 | lifecycle table + command sequence tested | plan Proposed Solution 4 "Lifecycle" | - |
| 8.4 | vitrinli | tools/advisories/report.py, tools/advisories/tests/test_report.py, tests/fixtures | 8.3 | P8 #5, P8 #6 | Fix column + fix-PR section tested; advisories-test green | plan TC "Rolling issue" | - |
| 8.5 | vitrinli | .github/workflows/advisories.yml | 8.4 | P8 #4, P8 #7 | actionlint green | plan TC "GitHub mechanics", "Permissions" | - |
| 8.6 | vitrinli | docs/src/development/ci.md, docs/adr/0029-advisory-scanning-from-the-bazel-graph.md | 8.5 | P8 added docs item | docs name crate fix PRs | ci.md "Dependency advisory scanning" | - |
| 8.7 | vitrinli | (verification only, no commit) | 8.6 | P8 #8, P8 #9 | tokio repin touches only the two locks, reverted; bazel-build + bazel-test green | plan Phase 8 | - |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 8.1 | vitrinli | done | 4ad79ef2 | `bazel-repin-crate <spec>`: repin fetch then plain fetch; uses `bazel` like every recipe (plan said `bazelisk`) | - |
| 8.2 | vitrinli | done | 25780c69 | fix.py crate path + lifecycle (8.3 folded in: one new file), test_fix.py (28 tests, mutation canaries fail as expected) | - |
| 8.4 | vitrinli | done | a0d5d4ae | report.py Fix column (component+source) and Fix PRs section from a gh pr list file; advisories-test 169 green | - |
| 8.5 | vitrinli | done | e5c4c20e | fix job (DASCHBOT_PAT, ci-setup, scanners, dry-run unless schedule/default-branch dispatch, outcomes artifact, repo-cache save); issue job needs fix, lists PRs, passes outcomes; actionlint green | - |
| 8.6 | vitrinli | done | 43c1f0a4 | ci.md Fix PRs paragraph + automated-bump note; ADR-0029 Not-yet-in-code narrowed to Phase 9 paths (checkbox added by the executor) | - |
| 8-review-1 | vitrinli | done | 06cc364e c2659027 833b0731 24d4f7a0 935b7ffa | fixups: older semver line still flagged -> major bump; reset at crate start, refuse dirty checkout; fork PRs (isCrossRepository) ignored in fix.py and report; state CLOSED not mergedAt; report declined = branch's latest PR; BRANCH_PREFIX in _model; one WRITE gate; always() upload/cache save; docs sentences; BUILD comments `bazel fetch`. Target commit bodies of 25780c69 (mergedAt, return-to-base wording) need refreshing at ship | - |
| 8-review-2 | vitrinli | done | 62d22883 8cf57492 c1dc887a | fixups: re-scan reads unmerged per-version findings, major bump only when a flagged version is on an older line; `branch_name` in _model (one definition for fix.py and report.py); dropped the defensive JSON wrapper in branch_state; report/ci.md wording; tests for the lock `(source)` suffix and same-line still flagged | - |
| 8.7 | vitrinli | done | - | tokio repin (GH_TOKEN unset, empty GH_CONFIG_DIR): only the two locks changed, REPIN recorded unset, reverted; bazel-build + bazel-test 78/78 green after both fix rounds; advisories-test 175 green; actionlint green; coverage unenforceable locally (owner decision) | - |

## Phase 9

### Chunk queue
| id | repo | files | depends_on | checkboxes | acceptance | context | replaces |
|----|------|-------|------------|------------|------------|---------|----------|
| 9.1 | vitrinli | justfile | - | P9 cb1 | `just --list` shows bazel-lock-update | plan Phase 9 | - |
| 9.2 | vitrinli | tools/advisories/fix.py | - | - (prep refactor) | lifecycle tail shared, advisories-test green, no behaviour change | fix.py fix_crate | - |
| 9.3 | vitrinli | tools/advisories/fix.py, tools/advisories/inventory.py, tools/advisories/tests/test_fix.py | 9.2 | P9 cb2, cb5 (BCR part) | Bazel ordering, yanked, override patches, grype re-scan tested | plan Proposed Solution 4 | - |
| 9.4 | vitrinli | tools/advisories/fix.py, tools/advisories/tests/test_fix.py, .github/actions/advisories-scanners/action.yml, MODULE.bazel (comment) | 9.2 | P9 cb3, cb5 (digest part) | crane digest edit + re-scan tested; actionlint | plan Pinned scanner installs | - |
| 9.5 | vitrinli | tools/advisories/fix.py, tools/advisories/_model.py, tools/advisories/report.py, tests | 9.2 | P9 cb4, cb5 (snapshot part) | snapshot edit + lock regen + re-scan tested | plan Proposed Solution 4 | - |
| 9.6 | vitrinli | none (local run, reverted) | 9.1 | P9 cb6 | both recipes complete with GH_TOKEN unset, GH_CONFIG_DIR empty | plan Kakadu stays out | - |
| 9.7 | vitrinli | .github/workflows/advisories.yml | 9.3-9.5 | - (comments follow the new classes) | actionlint clean | advisories.yml fix job | - |
| 9.8 | vitrinli | docs/src/development/ci.md, docs/adr/0029-advisory-scanning-from-the-bazel-graph.md | 9.3-9.5 | P9 cb7 | quarterly section replaced; ADR Not-yet line gone | ci.md Dependency advisory scanning | - |
| 9.9 | vitrinli | CLAUDE.md | - | P9 cb8 | dependency section names components.toml | CLAUDE.md Dependencies | - |
| 9.10 | vitrinli | none (verification) | all | P9 cb9 | bazel-build + bazel-test pass; coverage unenforceable locally | brief decided_behaviours | - |

### Chunks
| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 9.1 | vitrinli | done | abac25dc | `just bazel-lock-update` runs `bazel mod deps` | - |
| 9.2 | vitrinli | done | 3e694a8a | fix.py: `propose` holds the shared branch/PR lifecycle, `changed_within` the allowed-paths check; crate fix unchanged | - |
| 9.3 | vitrinli | done | cd886011 | BCR fix: Bazel version order, registry metadata (yanked skipped), bazel_dep + versioned override edit in the call span, `just bazel-lock-update`, Grype re-scan by the map's CPEs; patched override / transitive-only / no registry version are alert-only | - |
| 9.4 | vitrinli | done | 9599b82c, 46b15d4c | base-image fix: `crane digest` of `tracks`, digest + dated trailing comment in the `oci.pull` span, Grype re-scan per platform over the advisories with a released fix; crane 0.21.6 pinned in advisories-scanners; MODULE.bazel comment states the digest source | - |
| 9.5 | vitrinli | done | a7916854 | Debian fix: all deb findings share `advisories/debian` (one outcome per package), snapshot timestamp -> scan date, `just bazel-distroless-lock`, Grype re-scan of the new lock keyed (component, id); report maps deb findings to that branch; `just advisories-test` 199 OK (cb5 done across 9.3-9.5) | - |
| 9.7 | vitrinli | done | 03f15a61 | advisories.yml fix-job comments name every class and crane; actionlint clean | - |
| 9.8 | vitrinli | done | 837af8c0 | ci.md: every fix class, alert-only classes, pin table section replaces the quarterly review; ADR-0029 Not-yet line removed (checkbox added) | - |
| 9.9 | vitrinli | done | 8bf018cb | CLAUDE.md dependency section calls components.toml the classification of record | - |
| 9-review-1a | vitrinli | done | 0d612732 | review round 1 (code): Debian PR current while ids unchanged; uncovered deb package alert-only; patches presence incl. patch_cmds; no prerelease targets; http_get retries 429/5xx; set_digest line-scoped + real MODULE.bazel test; error tests assert messages | - |
| 9-review-1b | vitrinli | done | bc4a4c51 | review round 1 (docs): ci.md Debian rebuild rule; ADR-0029 crane in the tool pins | - |
| 9-review-2a | vitrinli | done | dec9ce8e | review round 2 (code): http_get backoff on retried HTTP errors; `outcomes_for` with per-component ids; explicit `track_target`; `is_prerelease` | - |
| 9-review-2b | vitrinli | done | cbec5f61 | review round 2 (docs): `patch_cmds` named in ci.md and ADR-0029 | - |
| 9.10 | vitrinli | done | none | `just bazel-build` and `just bazel-test` (78/78) pass on macOS after the last fix round; coverage checkbox split, remainder unticked (unenforceable locally) | - |
| 9.6 | vitrinli | done | none | `env -u GH_TOKEN -u GITHUB_TOKEN GH_CONFIG_DIR=<empty>`: `just bazel-lock-update` and `just bazel-distroless-lock` both exit 0 on macOS; neither changed a tracked file, nothing to revert | - |
