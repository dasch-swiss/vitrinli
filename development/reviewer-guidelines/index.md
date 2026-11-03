# Reviewer Guidelines

Checklist for human and AI reviewers. Not every item applies to every PR — use judgment.

## Documentation & Discoverability

- [ ] New config keys: documented in `docs/src/guide/sipi.md`, `docs/src/guide/running.md`, and config file inline comments
- [ ] Deprecation warnings: include the new key name and an example of the corrected config line
- [ ] New CLI flags/env vars: `--help` text updated, documented in `running.md`
- [ ] New HTTP endpoints: documented with request/response format
- [ ] If a feature is only discoverable by reading source, it's not done

## Configuration & Defaults

- [ ] Lua config, CLI args, and env vars all accept the same semantics and produce the same defaults
- [ ] Defaults are consistent across all entry points (`SipiConf.cpp`, `cli_app.cpp` CLI, documentation)
- [ ] Invalid values produce clear startup errors with guidance on valid values
- [ ] Deprecated keys: old names accepted with warning, both old+new in same config is a hard error

## Commit & PR Hygiene

- [ ] Commits follow [commit-conventions.md](https://sipi.io/development/commit-conventions/index.md) — `feat:` / `fix:` for changes a deployer cares about, `build:` / `test:` / `refactor:` for internal work
- [ ] One topic per commit (rebase-merge = commits land as-is on `main`)
- [ ] PR description follows the template (Motivation, Summary, Key Changes, Test Plan)

## C++ Quality

- [ ] Builds clean under Clang 15+ and GCC 13+ with `-Wall -Werror`
- [ ] No new compiler warnings introduced
- [ ] Thread safety: shared data structures accessed under appropriate locks
- [ ] No raw `new`/`delete` — use smart pointers or RAII
- [ ] Error paths: resources cleaned up, partial state not left behind
- [ ] C library calls: argument types match exactly (see [REVIEW.md](https://sipi.io/REVIEW.md) "C library boundary safety" section)
- [ ] Multi-buffer operations: loop bounds match the buffer being indexed, not a different buffer's dimensions
- [ ] C resource handles (`DIR*`, `FILE*`, `TIFF*`) wrapped in RAII — no manual cleanup paths
- [ ] GoogleTest unit tests added for new logic; existing tests updated if behavior changes
- [ ] E2E tests added or updated for user-visible behavior changes
- [ ] Sanitizer CI passes with zero findings for PRs touching `src/`

## Logging

- [ ] Per-item operations at DEBUG level, summaries at INFO
- [ ] Warnings for recoverable issues (e.g., missing optional files, deprecated config)
- [ ] Errors for unrecoverable issues that prevent operation

## Metrics

- [ ] New metrics use correct metric types (counter for monotonic, gauge for current state, histogram for distributions)
- [ ] Metric names follow `sipi_` prefix convention with `_total` suffix for counters
- [ ] Instrumentation points are in the correct layer (not duplicated across call chain)

## Consistency

- [ ] Follow existing patterns (route registration in the Rust axum router, `app()` in `src/server/rust/src/lib.rs`; native third-party deps as `cc_library` in `bazel/<lib>.BUILD.bazel`; test layout in `test/unit/`)
- [ ] Config example files updated alongside code changes
- [ ] New fields mirror structure of similar existing fields

## Ubiquitous Language

Identifiers, comments, commit messages, and PR text use the canonical terms in [`UBIQUITOUS_LANGUAGE.md`](https://sipi.io/UBIQUITOUS_LANGUAGE.md), not the aliases that column flags as "avoid". Common slips to reject:

- [ ] **Bitstream**, not "file" as a domain noun for the served byte stream (*file* survives only in URL paths and filesystem discussion)
- [ ] **Region**, not "ROI" / "crop" / "crop coords"
- [ ] **Size**, not "scale" / "dimensions"; **Decode level**, not "reduce" (except as a codec-API parameter name)
- [ ] **Cache key**, not "canonical URL" when the string is used to key the cache (*Canonical URL* = the IIIF spec form only)
- [ ] **Throttling**, not "backpressure" for the load-driven 503/400 rejection policies
- [ ] **Bitstream Information document** / **Image Information document**, not "info.json" (the file name) for the response schemas

## Testing Strategy Compliance

- [ ] New tests placed in the correct pyramid layer — consult the [decision tree](https://sipi.io/development/testing-strategy/#test-decision-tree)
- [ ] New HTTP behavior tests are Rust e2e
- [ ] Tests verify behavior (dimensions, content, structure), not just status codes
- [ ] Snapshot tests use `insta` with appropriate redactions for dynamic fields
- [ ] No new `test/unit/` directories — C++ unit tests are frozen (maintain existing only)
- [ ] If a gap from the [coverage matrix](https://sipi.io/development/testing-strategy/#iiif-image-api-30-coverage-matrix) is closed, the matrix is updated

## Security

- [ ] No path traversal possible via user-supplied inputs (IIIF identifiers, config paths, cache file names)
- [ ] Internal-only endpoints documented as requiring reverse proxy protection
- [ ] No secrets or credentials in log output
