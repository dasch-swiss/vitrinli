---
plan: docs/specs/2026-11-16-linear-bug-fixes/01-fix-linear-bugs-plan.md
unit: deadline
unit_state: done
curated_from: 2b16cd2dd05b2145f536f3f1e08a9d9c0c492b6f
started_from: fa5b804ec9ef1f5ac073b5ef021ee7dff5419d6e
target_repo: dasch-swiss/vitrinli
base_commit: 6d48d679575aa9b3b0303f871b925e7f6c1fc7de
branch: claude/project-thread-48t4wn
started: 2026-11-16
problem: >
  The decode deadline (120 s) exists twice and is longer than the 60 s request
  timeout, so the engine's own timeout path never runs (DEV-7857, DEV-7598);
  the Lua SipiImage.dims probe has no deadline (DEV-7604); and wedged decode
  threads accumulate until every core spins (DEV-7597).
status: complete
---

# Execution Journal: 01-fix-linear-bugs-plan, unit deadline (Phases 3, 4)

Branch note: the session assigns `claude/project-thread-48t4wn` instead of the
plan's `impl/bugs-deadline`; the coordinator note allows this.

## Repos

| repo | base_commit | branch | merge_strategy | status | pr |
|------|-------------|--------|----------------|--------|----|
| vitrinli | 6d48d67 | claude/project-thread-48t4wn | rebase | in-progress | https://github.com/dasch-swiss/vitrinli/pull/851 |

## Phases

| phase | status | phase_base | review_fix_rounds |
|-------|--------|------------|-------------------|
| 3 | reviewed | vitrinli@6d48d67 | 1 |
| 4 | reviewed | vitrinli@c76eb82 | 1 |

## Chunks

| id | repo | status | commit(s) | summary | blocker |
|----|------|--------|-----------|---------|---------|
| 3.1 | vitrinli | complete | 9fc509a, f2eb249 | DEV-7857: kDefaultDecodeTimeoutMs in engine_context.h; cli kDecodeTimeoutMs deleted | none |
| 3.2 | vitrinli | complete | 1aedc5d | DEV-7598: decode_timeout_ms knob, resolve_decode_timeout (80 % default, startup refusal), seam fields at 224/232 | none |
| 3.3 | vitrinli | complete | c63c0a9 | DEV-7604: sipi_image_file_dims under run_with_deadline; seam_probe_test hang case | none |
| 3.4 | vitrinli | complete | c76eb82 | Phase 3 docs: running.md, admission-control.md, ARCH-MAP throttling entry | none |
| 4.1 | vitrinli | complete | 174ab9a | DEV-7597: --max-wedged-threads cap, shed in iiif() before acquire, unit + e2e tests, health/running docs | none |
| 3.5 | vitrinli | complete | 99141b1 | Phase 3 review fixup: drop the racy env half of the CLI parse test | none |
| 4.2 | vitrinli | complete | 8c1f8d8 | Phase 4 review fixup: busy_response doc reattached, cap helpers documented | none |

## What went well

- Phase 3: a scratch Cargo workspace (outside the repo) plus a C stub library gave a local `cargo clippy -D warnings`, `rustfmt --check` and Rust unit-test loop despite Bazel being blocked in the cloud container.
- Phase 3 review: seven reviewers, no Critical or Warning finding.

## What went wrong

- Phase 4: a `git commit -a` for a journal row swept the uncommitted Phase 4 code into the journal commit; undone with a soft reset before the code commit, and the branch re-pushed with `--force-with-lease`. Journal commits now stage the journal file by name.

- C++ changes (`init.cpp`, `image_handle.cpp`, `seam_probe_test.cpp`, the cli headers) could not be compiled here; only `sipi_ffi.h`'s static_asserts were checked with `clang++ -fsyntax-only`. CI is the first build.

## Deferrals

## Deviations

- Phase 4: `AppState` has no `nthreads` field (the plan cited one); the cap is resolved in `AppState::load` from the local `resolved_nthreads`, which is the same value.
- Phase 4: `--max-wedged-threads 0` means the default (`nthreads - 1`), like `--nthreads 0`; the plan did not say. A literal cap of 0 would shed every request from startup.

- Phase 3: the session assigned branch `claude/project-thread-48t4wn` instead of `impl/bugs-deadline` (allowed by the coordinator note).
- Phase 3: `SipiServerConfig.decode_timeout_ms` is `uint64_t`/`u64` rather than the plan's `size_t`, matching the neighbouring `large_decode_threshold_bytes`; same width on LP64, same offsets.
- Phase 3: `resolve_decode_timeout` also refuses `0` (not named in the plan), since a zero deadline fails every decode.
- Phase 3: `has_decode_timeout_ms` is `is_some()` in `OverridesHolder`; `server_main` always fills the value before init, so the server always sends 1 as the plan requires, while a holder built without resolution sends 0 (engine default).
- Phase 3: the env half of the CLI parse test was dropped after review (process-global env in parallel tests); the env and TOML layers are covered by the merged-value test in `config.rs` and clap's `env =` attribute.
- Phase 3: no test that a legitimate e2e server with `SIPI_REQUEST_TIMEOUT=1`/`5` still starts was added; the derived default (800 ms / 4 s) never refuses, and those existing e2e tests exercise it.

## Side findings

- Phase 4 review: a failed `ffi::metrics_snapshot()` counts as not wedged (the plan's decision), so the shed fails open silently while the snapshot keeps failing. Left as decided; the snapshot only fails on an FFI exception.
- Phase 4 review: the cap check is one `sipi_metrics_snapshot` FFI call (plain atomic loads) per IIIF request. Judged cheap; a single-field seam getter is the follow-up if profiling shows a cost.

- Phase 3: `sipi_image_new` (Lua `SipiImage.new`) still runs its memory-estimate shape probe (`shape_probe.read_shape`, `image_handle.cpp`) without a deadline, before the deadline-bounded decode. A file in the DEV-7080 read_shape hang class wedges that Lua request forever. Outside DEV-7604's scope (which names only `SipiImage.dims`); not fixed here. Candidate follow-up issue.

## Review

- The `/eng:workflows:review` gate ran as the unit's two phase reviews (the eng.yaml reviewer set: consistency, simplicity, dune, reference, cpp, rust, security, observability), on Sonnet. Phase 3: no Critical or Warning; one suggestion fixed. Phase 4: one Warning (a misplaced doc comment) fixed; two notes kept under Side findings.

## Owner decisions

- none

## Final review

- 2026-11-16, final unit (whole stack against `main`, 8 reviewers, skeptic triage): Critical, fixed: the `sipi server --help` snapshot lacked the two new flags (fixups into the two `fix(server...)` commits). Also: the env-var table now lists `SIPI_MAX_WEDGED_THREADS`; the docs and `resolve_decode_timeout` comment say queue wait and a probe come out of the deadline margin; the e2e wedged-thread bound is 20 s. Deferred at first, then fixed on the owner's request ("Fix all six"): the wedged shed moved into `Admission::acquire` (a `fn() -> u64` probe and `max_wedged_threads` in `AdmissionConfig`), so Lua routes and the docroot are shed too and a request that queued while the cap was reached is shed on admission; `/health` answers 503 `wedged` at the cap, so Swarm restarts the container (this reverses the plan's scope cut on `/health`); the serve knobs travel as one `ServeKnobs` struct (new `refactor(server,cli)` commit). The residual overshoot is the decodes already running: at most `permits_total - 1`.

## Closeout

- root_cause: the server used the offline verbs' 120 s decode deadline, longer than the 60 s request timeout, so a hung decode answered 408 without releasing its permit or counting as wedged; the Lua dims probe had no deadline; nothing bounded the number of wedged threads.
- investigation: the default became 80 % of the request timeout; a literal cap of 0 would shed every request, so 0 means the default.
- solution: `kDefaultDecodeTimeoutMs` named once; `resolve_decode_timeout` with a startup refusal; the value crosses the seam in `SipiServerConfig` (240 bytes); `sipi_image_file_dims` runs under `run_with_deadline`; `--max-wedged-threads` sheds IIIF requests with 503.
- prevention: config, seam-probe and serve_image unit tests and the `resource_limits.rs` e2e case; H5 Grafana alert on `sipi.wedged_threads`.
