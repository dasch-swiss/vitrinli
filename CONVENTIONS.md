# Vitrinli Architectural Conventions

Project-specific architectural context for the vitrinli repository. Used by planning, working, and compounding workflows to understand the codebase structure.

For review-specific rules, see `REVIEW.md` in the vitrinli repository root.
For the full C++23 style guide, see `docs/src/development/cpp-style-guide.md`.
For commit and PR conventions, see `docs/src/development/commit-conventions.md`.
For reviewer guidelines, see `docs/src/development/reviewer-guidelines.md`.

## Production surface

The **Rust axum shell** (`src/server/rust` + `src/cli/rust`) is the sole production server. It drives the C++ **image engine** (`libsipi`, the FFI callee) over the seam in `src/server/rust/src/ffi.rs`. There is no C++ server: the `shttps` transport and `SipiHttpServer` were removed with the oracle ([ADR-0020](docs/adr/0020-oracle-removal.md)). `src/cli/cpp` is production too: `sipi_cli_main` and the offline verbs (`convert`/`verify`/`query`/`compare`/`health`) the Rust CLI shell drives.

Consequences for production code:

- Do **not** frame the Rust shell relative to the removed C++ server / oracle / transport ("matches the oracle", "the transport's X", "at the cutover", "reconstructs shttps' Y"). Referencing the C++ **engine** (the production FFI callee) is fine: that is what the shell calls into. The general rule is [Comments](#comments).

## Stack

- C++23, Clang 15+ / GCC 13+
- Build orchestrator: Bazel (single source of truth for CI; reproducible action graph)
- Reproducible dev environment: Nix dev shells (`flake.nix` `devShells` only — no Nix-side build derivations)
- HTTP framework: Rust axum shell (`src/server/rust`, `src/cli/rust`) — the production server (see "Production surface")
- Image formats: libtiff, libpng, libjpeg, libwebp, Kakadu (JPEG 2000)
- Scripting: Lua (routes, preflight checks, image manipulation)

## Build Reproducibility Invariant

Every build/test/coverage step in CI invokes one of the `just bazel-*`
recipes — no inline `bazel` calls in workflow YAML, no `nix build` calls.
The consequences:

- Recipes are contracts: every `bazel-*` recipe is a promise that CI runs
  the same command. Adding ad-hoc bazel invocations in a workflow would
  violate that contract and create a drift surface.
- Tests are part of the action graph. `just bazel-coverage` builds sipi
  with coverage instrumentation and runs unit + approval + e2e in one
  pass; the lcov report at `bazel-out/_coverage/_coverage_report.dat` is
  what Codecov consumes.
- The inner-loop edit/rebuild cycle IS `just bazel-build`. Bazel's per-
  action cache rebuilds only the affected compile + link, not the full
  closure. No separate dev-shell-only path is needed.

If a new build configuration is genuinely needed across the team (non-
trivial compiler flags, specialized toolchain), that configuration
belongs in `.bazelrc` (a `--config=<name>` block) or a new Bazel target
in `MODULE.bazel` / `BUILD.bazel`, not an imperative shell recipe.

## Build Completeness Invariant

Every build target must succeed on every supported platform:
macOS (darwin-aarch64), linux-x86_64, and linux-aarch64. Linux-only
outputs (`//src:sipi_image` and the `bazel-docker-*` recipes built on top of
it) are gated by host-CPU `target_compatible_with`; everything else
must build on every platform. CI runs the test matrix on all three
platforms, so a green CI run verifies macOS as well as Linux. Before
shipping changes to `flake.nix`, `MODULE.bazel`, `BUILD.bazel`, or a
justfile build recipe, run `just bazel-build` and `just bazel-coverage`
locally on macOS at minimum.

## Scope Discipline

These rules govern *what* to build (mirrored from CLAUDE.md so
contributors and Claude share the same contract):

- **No backwards-compatibility shims.** Update every caller in the same
  change. Rebase-merge preserves history; deprecated aliases,
  re-exports, and "kept for now" pointers are not needed.
- **No defense-in-depth.** Validate at system boundaries only — HTTP
  request handlers, FFI boundaries, CLI parsers. See
  `REVIEW.md` § "Security (input validation)" for what qualifies.
- **No enterprise abstractions — KISS.** Three similar lines beat one
  parameterised helper. Introduce an abstraction only for a *second
  real* caller, not a hypothetical one.
- **Ask when in doubt.** Surface ambiguous decisions to the maintainer
  before acting. "Suggest, don't decide" is the default.

## Naming Conventions

The codebase has mixed naming styles. For new code, prefer the C++23 style guide (`camelCase` functions, `trailing_underscore_` members). When modifying existing code, match the surrounding style.

| Entity | Existing Convention | Example |
|---|---|---|
| Types / Classes | `PascalCase` with `Sipi` prefix | `SipiImage`, `SipiCache` |
| Functions / Methods | Mixed `camelCase` / `snake_case` | `imgroot()`, `send_error()`, `build_canonical_url()` |
| Private members | `_leading_underscore` | `_imgroot`, `_nthreads` |
| Namespaces | `PascalCase` | `Sipi::`, `shttps::` |

## Comments

- A comment states what the reader must not break, never what the session discovered. Test: would it still be true and useful for someone who never saw the change that added it?
- Keep: an invariant a reader would otherwise break, a non-obvious third-party contract (lifetime, ownership, error/return semantics), the public API contract. One or two sentences each.
- Move out: what a change fixed or a test caught → commit body or PR; probe tables, benchmarks, corpus counts → `docs/` or a learning; a rejected alternative → a docs page or ADR, leaving one line and a link in the source.
- Delete: restatements of the code below, history ("previously", "now uses", "was changed to"), and any REQ id, user story, or plan-phase reference. A `TODO` without an issue id belongs in the tracker. A doc comment on an item a caller reaches is not a restatement: it is API surface, measured against what a caller needs rather than against the line below it.
- A PR description describes the code and the diff, never the commit history. "Commit 1 did X, commit 2 fixed Y" describes the journey.
- A "why" longer than about five lines belongs in a file; the comment becomes a pointer to it. A block past ~12 lines is a routing signal.
- Delete by default, when adding and when trimming: per sentence, name what a reader breaks without it, cut the ones with no answer, unclear included, and hold each survivor to one or two sentences.
- Rust doc comments use `///` for items and `//!` for modules, `//` inline.
- C++ doc comments use `///` on public API functions and classes, per [`docs/src/development/cpp-style-guide.md`](docs/src/development/cpp-style-guide.md). No Doxygen build renders them, so they are a reading convention.
- Lua, Bazel/Starlark, YAML, shell and TOML have no doc-comment tool: `--` and `#` comments only, held to the bullets above.

## Module Layout

### Canonical modules (commit-scope vocabulary)

These are Vitrinli's modules. Each name is also the canonical [commit
scope](docs/src/development/commit-conventions.md#scopes). A module is a
unit of responsibility as well as a per-module co-located directory
([ADR-0003](docs/adr/0003-module-co-located-source-and-tests.md)); see
[Directory layouts](#directory-layouts) below for the residual test-only
holdouts.

Scope a commit by the responsibility it serves, not the directory the edited
files happen to sit in. A metrics or tracing change is `observability` even
when the code lives under `src/server/rust/` or rides the `ffi` seam; a change
is scoped `ffi` only when the seam mechanism itself is the point.

| Module (scope) | Path | Responsibility |
|---|---|---|
| `image` | `src/image/` | The `SipiImage` value type: pixel buffer, metadata composite, decode/encode orchestration and format dispatch. No processing operators — those are `image_processing` |
| `image_processing` | `src/image_processing/` | Free-function pixel operators over `SipiImage &` (crop/scale/rotate/colour/channel/watermark/comparison/arithmetic), extracted off `SipiImage` per ADR-0007; depends one-way on `image`, never the reverse |
| `format_handlers` | `src/format_handlers/` | Per-format codecs: TIFF, JP2 (Kakadu), PNG, JPEG; `fuzz/` (the libFuzzer harnesses over the `SipiIO` decode handlers), `fuzz/dicts/` (vendored AFL++ token dictionaries), and `corpus/` (the per-format two-tier seed corpora) |
| `metadata` | `src/metadata/` | EXIF, IPTC, XMP, ICC profile handling |
| `iiifparser` | `src/iiifparser/` | IIIF URL parsing, colocated polyglot (ADR-0021): `cpp/value_objects/` (live engine value objects), `cpp/classifier/` (testonly `parse_iiif_uri` reference oracle `:iiif_handler`), `rust/` (the production parser `//src/iiifparser/rust:iiif_parser` the shell drives), and `fuzz/` (the libFuzzer harness over the Rust parser) |
| `scripting` | `src/scripting/rust/` (ADR-0021/0023 layout) | The Rust-hosted mlua Lua runtime: hardened VM profile, limits, bytecode cache, the `server.*`/`SipiImage`/sqlite bindings, Lua-flavor config parse |
| `util` | `src/util/` | Generic Vitrinli-domain helpers: MIME/string parsing, file hashing, filename-to-subdirectory hashing (`SipiFilenameHash`), the `shttps::Error`/`Global` types |
| `error` | `src/error/` | The `SipiError` exception base shared by the codec, metadata, and IIIF-parser packages; also houses the image/codec layer's value-error types (`SipiValueError`, the `ErrorCode` enum, `Result<T>`, ADR-0024) |
| `cache` | `src/cache/` | File-based LRU cache with dual-limit eviction |
| `throttling` | `src/throttling/` (colocated polyglot, ADR-0021/0022): `cpp/` (engine-side `memory_budget` — the full-lane decode budget) + `rust/` (shell-side `admission` — the two-lane pool) | Load-driven request-rejection: two-lane admission (tile floor + full hard cap) and the full-lane memory budget |
| `memory` | `bazel/mimalloc.BUILD.bazel`, `_ALLOCATOR` in `src/cli/rust/BUILD.bazel`, `tools/allocator-replay/` | Process memory behavior: the production allocator, RSS/retention measurement |
| `observability` | `src/observability/` | Metrics (atomic counters/gauges), tracing |
| `logging` | `src/logging/` | Structured logging |
| `cli` | `src/cli/` (colocated polyglot, ADR-0021): `cpp/` (the offline verbs behind the `sipi_cli_main` FFI entry) + `rust/` (the default `sipi` binary, the production entry point) | C++ CLI app + the Rust CLI binary |
| `ffi` | `src/ffi/` | Rust↔C++ FFI seam (incl. the `sipi_image_*` handle family) |
| `lua` | `scripts/`, `config/*.lua` | Lua route/preflight scripts and config |
| `server` | `src/server/rust/` | Rust server shell — the production server |

All server work lands under `server`; there is no C++ server (the `shttps`
transport and `SipiHttpServer` were removed with the oracle,
[ADR-0020](docs/adr/0020-oracle-removal.md)).

**FFI direction.** The seam is Rust-calls-C++ everywhere that ships: `src/ffi/`
and `src/server/rust/src/ffi.rs`. The one **C++-calls-Rust** link is
`//src/iiifparser/fuzz` (`shim.rs` → `fuzz_target.cc`), which exists because
libFuzzer's entry point is a C symbol and `rules_fuzzing` is a C++ rule set.
Its confinement is analysis-enforced, not documented: both targets are `testonly`
and the package grants no default visibility, so nothing outside it can depend on
the shim and the reverse direction cannot reach a production binary. Adding a
second reverse-direction link is an
architectural decision, not a local one — ask the maintainer. See
[fuzzing.md](docs/src/development/fuzzing.md).

Beyond modules, commits use **test-layer scopes** (`e2e`, `approval`, for a
test layer's own harness or fixtures) and **cross-cutting scopes** (`deps`,
`bazel`, `ci`, `nix`, `docker`, `docs`). `docs` is for the agent-context /
domain documentation that is not tied to one module — the root `CLAUDE.md` /
`CONVENTIONS.md` / `CONTEXT.md` / `UBIQUITOUS_LANGUAGE.md` and the ADRs; a doc
change scoped to one module still takes that module's scope. A test *about* a concern takes that concern's
scope — `test(observability)`, not `test(e2e)` — since the `test` type already
says it is a test. If none of the enumerated scopes genuinely fits a change,
ask the maintainer before inventing a new one. The full scope rules live in
[commit-conventions.md § Scopes](docs/src/development/commit-conventions.md#scopes).

### Directory layouts

Every module's source now follows **module-co-located**
([ADR-0003](docs/adr/0003-module-co-located-source-and-tests.md), status
`proposed` but the layout is in force repo-wide): `src/<mod>/{Foo.cpp, Foo.h,
foo_test.cpp}` with flat-style includes (`#include "metadata/Foo.h"`
cross-module, `#include "Foo.h"` intra-module). The `include/<mod>/` shadow
directories are gone; `include/` retains only the generated headers
(`SipiVersion.h.in`, `ICC-Profiles/`). `test/unit/` retains only shared
fixture generators (`test/unit/fixtures/`); every per-module unit-test
suite is co-located under its own package (see ARCH-MAP's Support areas
note).

A module-co-located package additionally splits its sources by language
into `cpp/`/`rust/` subfolders whenever it holds (or will hold) more
than one — the colocated polyglot of
[ADR-0021](docs/adr/0021-iiifparser-polyglot-colocation.md). No package
folder carries a `-rs` suffix; the subfolder is named `rust/`, not
`<pkg>-rs/`. `fuzz/` and `corpus/` stay sibling subfolders of the
package, never nested under `cpp/` — they exercise the package as a
whole, not one language's sources. `src/iiifparser/{cpp,rust,fuzz,corpus}/`
is the reference shape; `src/scripting/rust/`, `src/throttling/cpp/`,
and `src/format_handlers/{fuzz,corpus}/` also conform today.

Splitting into `cpp/` moves sources one physical directory deeper, so
the package's `cc_library` re-pins the virtual include path to keep
cross-module `#include`s unchanged: `strip_include_prefix =
"/src/<pkg>/cpp"` (or `/src/<pkg>/cpp/<subpkg>` for a nested
subpackage, e.g.
`src/iiifparser/cpp/value_objects/BUILD.bazel`) paired with
`include_prefix = "<pkg>"`, so consumers keep writing `#include
"<pkg>/Foo.h"`; siblings inside the package still use the quote-form
(`#include "Foo.h"`). A package that hasn't split by language has no
extra physical depth to correct for and needs only the plain
`strip_include_prefix = "/src"` form, self-sufficient without any
`include_prefix` twinning.

### Visibility

A new package's Bazel visibility follows two defaults: engine modules under
`src/**` default to `//src:__subpackages__` (visible to any other package
under `src/`, private to everything outside it); every other package
(`test/**`, `bazel/**`, `tools/**`, `platforms/**`, `config/**`, root) defaults
to private (`//visibility:private`). Beyond the default, a grant is explicit
and narrow — name the consuming package (`//test/approval:__pkg__`,
`//test/e2e:__pkg__`), never a wildcard grant and never
`//visibility:public`. A vendored overlay (`bazel/*.BUILD.bazel`) grants
using the canonical `@@//pkg:__pkg__` form (the main repo's canonical
repository name is empty) because the overlay applies inside an external
repo, where a bare `//pkg:__pkg__` label would resolve relative to that
repo instead of this one.

A target that must stay public carries a comment on the `visibility`
attribute naming why it cannot be narrowed (e.g. a `config_setting()`
reverse-referenced from another `http_archive`'s overlay, or a target with
no BUILD-file consumer that is only ever named on the command line).
Command-line references — `bazel build/run/test <label>`, `--platforms=`,
`bazel cquery`, justfile recipes, CI steps — are never gated by visibility;
only a dependency edge declared in a `BUILD.bazel` file is.

Visibility restricts *who may depend on a target from outside its grant*; it
cannot express a one-way rule between two packages that both sit inside the
same grant (e.g. `//src/image_processing` depending one-way on `//src/image`
while `//src/image` never depends back — both live under
`//src:__subpackages__`). Rules like that stay enforced by dep-set review,
not by a BUILD file.

Bazel's own analysis is the enforcement for the defaults and grants above,
backed by this documented convention — deliberately no grep-based CI gate
re-checks visibility declarations.

## Route Registration

Built-in routes are registered on the axum `Router` in the Rust shell
(`src/server/rust/src/routes.rs`). Scripted routes are Lua scripts bound to URL
patterns in the config ([ADR-0017](docs/adr/0017-extensibility-lua-and-rust.md)):
a `Route handler` is a Lua script the shell dispatches to, run inside a
request-scoped hardened VM of the Rust mlua runtime (`src/scripting/rust/`). IIIF requests are classified by the
standalone `//src/iiifparser/rust:iiif_parser` crate (`parse_request`), which
owns region/size/rotation/quality/format parsing and emits domain types;
`server` flattens those into the FFI params that cross the seam to the C++
engine.

## HTTP Status Codes

The Rust shell returns axum `http::StatusCode`s. Common cases:

| Code | Enum | Use for |
|---|---|---|
| 400 | `BAD_REQUEST` | Invalid IIIF parameters, path traversal attempts |
| 403 | `FORBIDDEN` | Access denied by preflight Lua script |
| 404 | `NOT_FOUND` | Image file not found |
| 500 | `INTERNAL_SERVER_ERROR` | Unexpected failures, OOM recovery |
| 503 | `SERVICE_UNAVAILABLE` | Server overloaded / shutting down / memory budget exhausted |

## Configuration Pattern

Production config for an engine-behaviour knob flows **Rust → FFI → C++ engine**:
the Rust shell parses CLI/env (clap) and TOML, layers them into a
`ServerOverrides` bag, hands it across the FFI seam as the `#[repr(C)]`
`SipiServerConfig`, and `sipi_init` applies it onto the Lua-parsed `SipiConf`
before the engine reads it. Adding one option touches every link below; most
links **fail to compile** if you forget them (DUNE-006), except the last C++
apply block, which is the one hand-mirrored seam.

Rust production side:
1. **clap flag** — a field in the right `src/cli/rust/src/commands/server/args/<group>.rs`
   group (network/paths/cache/limits/tls_auth/knora/logging/concurrency), with a
   colocated `env = "SIPI_X"` (clap owns CLI-over-env precedence).
2. **`ServerOverrides` field** — `src/server/rust/src/config.rs` (the Rust-native bag).
3. **forward from clap** — `From<&ServerArgs> for ServerOverrides`
   (`src/cli/rust/src/commands/server/mod.rs`). *Exhaustively destructures every
   clap group → a new flag that is not forwarded (or explicitly `field: _`) fails
   to compile.*
4. **TOML base** — a `Config` field + its `Config::base()` mapping
   (`src/server/rust/src/config_file.rs`). *Exhaustive `ServerOverrides` literal →
   a missing map fails to compile.*
5. **merge** — `ServerOverrides::layered_over` (`config.rs`). *Exhaustive literal.*

FFI seam:
6. **`SipiServerConfig` struct** — a field (plus a `has_*` presence flag for a
   scalar) in `src/ffi/cpp/sipi_ffi.h`, mirrored by the Rust `#[repr(C)]
   SipiServerConfig` in `config.rs`. Both sides are layout-guarded by the paired
   `static_assert`/`offset_of!` blocks (a drift fails the build/tests).
7. **forward to the FFI struct** — `OverridesHolder::new` (`config.rs`).
   *Exhaustively destructures `ServerOverrides` → a field never forwarded to the C
   struct fails to compile (unused binding under `-D warnings`).*

C++ engine:
8. **`src/ffi/cpp/SipiConf.h` + `src/ffi/cpp/SipiConf.cpp`** — the getter/setter and the Lua
   `config.*` table read (the engine's own config surface).
9. **`config/sipi.config.lua`** — document the option.
10. **THE ONE UNMECHANIZED LINK — the `sipi_init` apply block**
    (`src/ffi/cpp/init.cpp`): a hand-written `if (o.newfield != nullptr)
    conf.setNewfield(...)` per override. **Nothing checks this for
    completeness** — a forgotten line compiles clean and silently drops the
    override before the engine sees it. Always add the apply line here when you
    add an FFI field (step 6).

## Error Handling Pattern

| Situation | Mechanism |
|---|---|
| Fallible operation in the image/codec layer (`image`, `image_processing`, `format_handlers`, `metadata`) | `Result<T>` (`std::expected<T, SipiValueError>`) |
| Fallible operation in `iiifparser` (URL/request parsing) | `throw SipiError`/`SipiSizeError` — a different package, unchanged (ADR-0021) |
| Truly unrecoverable / invariant errors in the image/codec layer | `throw SipiImageError(...)` |
| Engine errors to the client | The engine returns a `SipiStatus` over the FFI seam; the Rust shell maps it to an axum HTTP response |
| Resource exhaustion (OOM) | Catch `std::bad_alloc`, return an error status over the seam, log, continue serving |

A new fallible operation in the image layer (`src/image/`, `src/image_processing/`, `src/format_handlers/`, `src/metadata/`) returns `Result<T>` — it does not throw. Exceptions stay reserved for `std::bad_alloc` and genuine logic-bug invariants (a condition only a caller passing wrong arguments can reach). `[[nodiscard]]` is mandatory on every `Result`-returning function, no exceptions — enforced by clang-tidy's `bugprone-unused-return-value` check, which covers `std::expected` in its default watched-type list and, unlike `[[nodiscard]]` alone, also flags a `(void)`-cast used to silence the warning. See [ADR-0024](docs/adr/0024-value-based-image-errors.md) for the model and [`docs/src/development/error-model.md`](docs/src/development/error-model.md) for the per-seam accessors.

## Docker

- Build: `bazel run //src:image_load` (per-arch); `crane index append`
  assembles the multi-arch manifest (see `src/BUILD.bazel`).
- Base image: `gcr.io/distroless/base-debian12` (glibc, pinned by digest)
- Init: `tini` (PID 1 zombie reaping, signal forwarding)
- Runtime user: `root` (NFS uid/gid coordination is a known constraint;
  documented inline in `src/BUILD.bazel` near the image rule)
- Port: 1024 (non-privileged)
- Config mount: `/sipi/config/`
- Image root: `/sipi/images/`
- Cache: `/sipi/cache/`

## Metrics

Engine-internal singleton at `Sipi::observability::Metrics::instance()`
(`src/observability/cpp/metrics.h`) — plain lock-free atomics (`Counter` / `Gauge`).
The engine bumps them on the decode/cache/serve paths. Production exports over OTLP:
the scalar counters and gauges cross the seam as the flat `SipiMetricsSnapshot`
(`src/ffi/cpp/sipi_ffi.cpp`) and are re-registered as OTel observable instruments in
`src/server/rust/src/metrics.rs`. **A new counter or gauge here does not reach
production until it is added to that snapshot and that module.** Distributions
cannot cross the flat snapshot at all; record them as OTel histograms shell-side
(see `record_http_duration` and `record_decode_estimate`).

Two tests enforce this, because the seam is easy to forget and a metric that stops
at it fails silently — it simply never appears in Grafana:

- `//src/observability:metrics_registry_test` classifies every metric field as
  bridged-to-OTLP or engine-internal. Adding a field fails it until you say which.
- `every_snapshot_field_is_accounted_for` in `src/server/rust/src/metrics.rs` fails
  unless each snapshot field is exported or explicitly listed as unexported.

**Engine-internal (not bridged)** — incremented in production, observable by
nobody, recorded in the first test's `kEngineInternalNotBridged`:

| Metric | Why | Cost to fix |
|---|---|---|
| `read_shape_fast_path_*` (ADR-0004) | Label-fanned `format` × `outcome`, 8 counters; the flat snapshot holds scalars | 8 scalar fields |
| `essentials_hash_mismatch_*` (ADR-0010) | Same, 5 counters. This is the **corruption tripwire** — nothing in production can see a detected corruption; `sipi verify service-file` is the only read path | 5 scalar fields |

Both label sets are static, so bridging them is mechanical rather than a design
problem. Shrinking that list is a welcome change; growing it needs an argument in
review.

```cpp
// Declared as a member of Sipi::observability::Metrics (metrics.h):
Counter my_counter_total;
// Bumped on the hot path:
Sipi::observability::Metrics::instance().my_counter_total.Increment();
```

To reach production OTLP, also read the field into `SipiMetricsSnapshot`
(`src/ffi/cpp/sipi_ffi.cpp`) and map it in `src/server/rust/src/metrics.rs`.
