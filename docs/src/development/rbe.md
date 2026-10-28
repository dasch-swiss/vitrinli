# Remote Build Execution

This page documents Vitrinli's Remote Build Execution (RBE) setup: its purpose, how the
cross-compilation pattern works, the key correctness fix in the hermetic-llvm toolchain,
measured performance characteristics, and how CI wires everything together.

The backend (`dasch-remotebuild-prod-01`) is DaSCH-hosted and defined outside
this repo: the libvirt VM in [`ops-tf`](https://github.com/dasch-swiss/ops-tf)
(`live/virt-03/prod/vms/`), the NativeLink/bazel-remote service configuration
in [`infra`](https://github.com/dasch-swiss/infra)
(`OS/ansible/roles/dasch.nativelink`, including its README and store-topology
notes).

## Why Vitrinli uses RBE

The goal is **cross-compilation**: building all three supported targets — `linux-amd64`,
`linux-aarch64`, and `darwin-aarch64` — on a single x86_64 worker, so each GitHub Actions
runner does not need a native matching toolchain.

Without RBE each platform leg runs its full compile on the runner that matches its arch.
The hermetic-llvm toolchain (see [Building with Bazel](bazel.md)) can cross-compile
end-to-end (bundled per-target glibc + libc++; no sysroot), so it can build all three
targets on a single x86_64 machine. RBE is the mechanism that routes those compiles to
a shared x86_64 worker regardless of which runner triggered the build.

Speed is not the justification. On a warm cache, RBE and the previous cache-only setup
have essentially identical per-PR wall-clock latency (see [Performance](#performance)).

## Architecture

### Backend

A self-hosted NativeLink VM acts as the single Bazel backend. It runs two services:

| Service | Port | Protocol | Purpose |
|---|---|---|---|
| NativeLink | `:50051` | gRPC + mTLS | Remote Cache (AC + CAS) + Remote Executor |
| bazel-remote | `:50052` | gRPC + mTLS | `http_archive` download cache (`--experimental_remote_downloader`) |

Both services present a certificate signed by the same NativeLink CA, so a single
`--tls_certificate` flag validates both — no CA bundle, no proxy.

`bazel-remote` handles `--experimental_remote_downloader` calls, which cache the source
tarballs for `http_archive` rules (jbigkit, the Apple macOS SDK, etc.). On a `FetchBlob`
miss bazel-remote fetches the URL itself, verifies the `sha256` qualifier, stores the blob,
and every later request for that hash is a permanent hit. The first successful build after
an upstream is reachable from the VM warms it for good; no manual step is required. See
[Bazel issue #14646](https://github.com/bazelbuild/bazel/issues/14646) for the known
limitation: when the VM cannot reach an upstream the runner CAN reach, a green
local-fallback build does not write back to the download cache.

### Why a separate bazel-remote, not just NativeLink

NativeLink *does* implement the Remote Asset API (`FetchServer` / `PushServer`,
`build.bazel.remote.asset.v1`), so the obvious question is why we don't point
`--experimental_remote_downloader` at `:50051` and drop bazel-remote. The reason is
*how* each implements `FetchBlob`:

- **NativeLink's Fetch is push-then-serve.** `fetch_blob` (nativelink-service
  `fetch_server.rs`) hashes the URI + qualifiers to a digest, does a single store
  lookup, and on a miss returns `NotFound` — it never contacts the upstream. It only
  serves assets that were *explicitly pushed* into its `fetch_store` first. (Its only
  `reqwest` HTTP client backs the GCS store, not URL fetching.)
- **bazel-remote's Fetch is a transparent proxy.** On a miss it fetches the URL
  itself, verifies the `sha256` qualifier, stores the blob, and serves it thereafter.

`--experimental_remote_downloader` relies on the proxy behavior — it expects the
endpoint to transparently mirror `http_archive` tarballs on first use. Replacing
bazel-remote with NativeLink's FetchServer would mean building a separate warming
pipeline that fetches every external dependency and pushes it (blob + asset mapping)
into NativeLink ahead of the build, i.e. reimplementing bazel-remote's proxy logic.
So the two services are not redundant: NativeLink caches and runs *build actions*
(AC/CAS + executor); bazel-remote transparently mirrors *source downloads*. (Verified
against NativeLink v1.5.2 — revisit if a later release adds proxy-fetch-on-miss.)

### GitHub Actions wiring

The `.github/actions/bazel-rbe` composite action encapsulates all RBE wiring:

1. It reads the org-level secrets `REMOTEBUILD_CA_CERT`, `REMOTEBUILD_CLIENT_CERT`,
   `REMOTEBUILD_CLIENT_KEY` and the variables `REMOTEBUILD_RUNNER_ENDPOINT` (the
   NativeLink RE-API, `:50051`) and `REMOTEBUILD_CACHE_ENDPOINT` (the bazel-remote
   download cache, `:50052`).
2. It writes the mTLS material outside the workspace checkout (`$RUNNER_TEMP/.nl/`)
   so cert files can never enter a Bazel action's input set.
3. It reads the two endpoints independently — they need not share a host.
4. It emits a `flags` output — a single Bazel flag string — that every subsequent
   `just bazel-*` invocation appends verbatim.

Fork PRs do not have access to org secrets; the action emits an empty `flags` string and
the build falls through to a cold local build.

`ci.yml`'s test matrix passes `matrix.bazel-platform` (`//platforms:linux_x86_64`,
`//platforms:linux_aarch64`, or `//platforms:darwin_aarch64`) as the `target-platform`
input to the composite action, so each leg cross-compiles for its own target on the worker.

## The cross-compile + test-local pattern

### The problem

`rules_rust` exec-config tools — `process_wrapper`, build scripts, proc-macros — must
run on the exec platform (the x86_64 worker). When a cross leg runs on an arm64 or
darwin runner, these tools would otherwise resolve their exec platform to the LOCAL host
(per [rules_rust #327](https://github.com/bazelbuild/rules_rust/issues/327)), shipping
an arm64 or darwin binary to the x86_64 worker where it cannot execute.

C++ was unaffected because hermetic-llvm registers cross toolchains for both
`exec=x86_64 → target=aarch64` directions out of the box. Rust did not.

### The fix

Five changes work together:

**1. Cross Rust toolchains** (`MODULE.bazel`):

```
rust.toolchain(
    ...
    extra_target_triples = ["aarch64-unknown-linux-gnu", "aarch64-apple-darwin"],
)
```

This registers the exec=x86_64 → aarch64 cross Rust toolchains so `rules_rust`
exec-config tool resolution no longer falls through to the host.

**2. Exec platform declaration** (`platforms/BUILD.bazel`):

`//platforms:linux_x86_64` carries `exec_properties = {cpu_arch: x86_64}`, mirroring
`--remote_default_exec_properties`. NativeLink routes actions to the worker on that
property.

**3. `--extra_execution_platforms`** (composite action, all legs):

```
--extra_execution_platforms=//platforms:linux_x86_64[,<target-platform>]
```

`//platforms:linux_x86_64` is listed first so compiles and exec-config tools resolve
to the worker. The target platform is appended on cross legs so Bazel's default test
toolchain (`default_test_toolchain_type`, new in Bazel 9) can resolve an exec platform
matching the target arch, which the test runner needs.

**4. `--host_platform`** (composite action, cross legs only):

```
--host_platform=//platforms:linux_x86_64
```

This pins `rules_rust`'s exec transition to x86_64 per the rules_rust #327 pattern.
Not applied on the amd64 leg (host and worker already match).

**5. `--noremote_local_fallback`** (composite action, cross legs only):

If the worker is unreachable and the build falls back to the local arm64/darwin runner,
the x86_64 `process_wrapper` binary would be dispatched there and crash. Cross legs
therefore disable local fallback. The amd64 leg retains the `.bazelrc` graceful fallback.

### Where tests execute

```
--strategy=TestRunner=local   # cross legs only (arm64/darwin)
```

**Cross legs (arm64/darwin).** Test binaries are cross-compiled on the x86_64 worker but
executed locally on the native runner — the cross-built binary can't run on the x86_64
worker, and the runner has the right kernel ABI and filesystem. `bazel-rbe` emits
`--strategy=TestRunner=local` only on these legs.

**Native x86_64 legs (amd64 `test` + `sanitizer`).** Tests EXECUTE on the worker
(exec == target == x86_64): results cache remotely and the runner skips the multi-GB
output download under `--remote_download_minimal`. The e2e tests are tagged
`exclusive-if-local` (see below), so on isolated workers they run in parallel rather than
serially. Under `--config=asan` the e2e macro attaches `//bazel:llvm-symbolizer` as a
runfile and sets `ASAN_SYMBOLIZER_PATH` to its runfiles path, so the sanitizer e2e tests
run remotely too (a runner-side absolute symbolizer path would not exist on the worker).
`docker_smoke` is the sole local exception, kept on the runner by its plain `exclusive`
tag and its Docker-daemon dependency.

### The `exclusive-if-local` and `no-sandbox` tags on e2e tests

E2E tests are tagged `exclusive-if-local` and `no-sandbox` in
`test/e2e/sipi_e2e_test.bzl` (`docker_smoke` keeps plain `exclusive` in
`test/e2e/BUILD.bazel`). Neither is `local`, and that is permanent and correct.

- `exclusive-if-local` runs the tests serially only when they execute locally (dev, cross
  legs), preventing port collisions between binaries that all bind `11024 + PID % 16384`
  on the shared runner. On the native x86_64 RBE leg each test runs on its own isolated
  worker, so there is no shared port range and Bazel runs them in parallel. Plain
  `exclusive` would disable remote execution entirely — Bazel can't guarantee exclusivity
  on a machine it doesn't own — which is why `docker_smoke` (which must stay on the runner
  for its Docker daemon) keeps `exclusive`.
- `local` propagates to the test's COMPILE action and pins the compile to the runner.
  On a cross leg this routes an arm64/darwin process_wrapper to the x86_64 worker —
  it crashes. `local` breaks cross-compilation.
- `no-sandbox` disables the Bazel sandbox for the TEST RUN only (the compile is
  unaffected). It does not prevent cross-compilation.

The reason e2e tests need `no-sandbox` at all: Vitrinli's `validate_resolved_path` guard
in `src/server/rust/src/path.rs` canonicalises the request path via `realpath(3)`
(Rust's `std::fs::canonicalize`). Under the Bazel sandbox, path resolution returns the
writable runfiles path rather than the `TEST_TMPDIR` copy, so sipi rejects legitimate
test files with HTTP 400. Disabling the
sandbox sidesteps this runfiles-symlink interaction. This is also the open question when
e2e tests execute remotely: the NativeLink executor must honor `no-sandbox` (materialise
real files under a stable root) for the guard to pass. The guard itself is correct and
must not be weakened for testing.

## The hermetic-llvm headers glob patch

### The problem

Remote execution requires every action input to be declared as a discrete file so Bazel
can construct the action's Merkle input tree. If an input is a bare source-directory
artifact (one opaque directory node), Bazel references the directory but its subtree may
not be enumerated — the include subdirectory is dropped from the Merkle tree, and the
remote executor receives an incomplete input set.

Local sandboxed builds follow the directory symlink and succeed. Remote builds fail with
`fatal error: 'linux/limits.h' file not found`. Warm action-cache hits skip the compile
entirely and mask the gap, so the failure is deterministic only on a cold cache and
appears intermittent on a warm one.

hermetic-llvm injects the Linux kernel + glibc system headers through two independent
paths, both using bare source-directory artifacts:

- **Path A** — `cc_args(data = [..._directory])` in `toolchain/args/linux`. Feeds the
  ordinary target-config compiles.
- **Path B** — `cc_library(name = "kernel_headers" / "gnu_libc_headers", hdrs = ["include"])`,
  consumed via `@llvm-project//libcxx,libcxxabi,libunwind implementation_deps`. The
  ONLY header source for exec-config "stage0" runtime compiles, which receive no
  `cc_args` kernel headers at all.

### The fix

`bazel/patches/hermetic_llvm_headers_glob.patch` applies two coupled changes to
hermetic-llvm via a `single_version_override` in `MODULE.bazel`:

1. `directory.bzl` gains an `expand_files` opt-in. When `True`, `DefaultInfo` carries the
   individual header files (a recursive glob minus `make headers_install` byproducts) rather
   than one source-directory artifact. This fixes Path A. The macOS SDK sysroot is left
   unexpanded — globbing it would bloat every darwin compile with unnecessary inputs.

2. `kernel/extension/kernel-headers.BUILD.bazel` and
   `runtimes/glibc/extension/glibc-headers.BUILD.bazel` change
   `hdrs = ["include"]` to `hdrs = glob(["include/**"])`. This makes every individual
   header a declared action input for the stage0 runtime compiles. This fixes Path B —
   the path the `expand_files` change does not reach.

The patch targets `directory.bzl`, `kernel/extension/kernel-headers.BUILD.bazel`, and
`runtimes/glibc/extension/glibc-headers.BUILD.bazel`. Re-check it on every `llvm` bump
with the path A and path B `aquery` commands below, run with this patch taken out of the
override (the override's other patch, `hermetic_llvm_darwin_rtlib.patch`, stays): while
the kernel and glibc header inputs are the two bare `include` directories, the patch is
still needed; once both paths list individual header files, drop this patch.

Local verification (no RBE required):

```bash
# Sanity-check the patch + MODULE changes on the host toolchain
nix develop -c just bazel-build

# Cross-compile a Linux target from macOS
nix develop -c bazel build --platforms=//platforms:linux_x86_64 //src/logging:logging

# Path A: confirm no bare source-directory artifact in the action inputs
nix develop -c bazel aquery \
  --platforms=//platforms:linux_x86_64 \
  'mnemonic("CppCompile", //src/logging:logging)' \
  --include_artifacts
# Path B: the stage0 runtime compiles
nix develop -c bazel aquery \
  --platforms=//platforms:linux_x86_64 \
  'mnemonic("CppCompile", deps(@llvm-project//libcxx:libcxx.static.stage0_non_asan) union deps(@llvm-project//libunwind:libunwind.static))' \
  --include_artifacts
# All inputs should be named files, not bare directory nodes.
```

## Performance

### What the numbers mean

The relevant metric is wall-clock time for a build triggered by a **new commit** — every
PR push is a new commit, and `--stamp` changes the binary, which causes all tests to
re-run regardless of cache state. A same-commit re-run (`gh run rerun`) hits a
cache-hit floor of ~3–5 minutes (compiles AND test results cached) and is not
representative. Do not use re-run measurements to compare build backends.

### Measured latency (warm vs cold)

Measured by parsing `--profile` JSON artifacts. (`bazel analyze-profile` was removed in
Bazel 9; parse the JSON directly.)

| scenario | cache-only (Cloud Run + GCS) | RBE (single worker) |
|---|---|---|
| Build + run all tests / leg (warm, new commit) | ~8.6–9.8 min | ~8.8–9.5 min |
| Total leg wall-clock (warm) | ~11–13 min | ~11–13 min |
| Cold (empty CAS / scale-from-zero) | ~13 min | 23–29 min |

On a warm cache the two approaches are essentially equal. RBE is slower cold because all
three legs share one worker and each has to rebuild the shared exec-config toolchain from
scratch concurrently (only ~220–430 cross-leg cache hits on a cold run). The cache-only
setup has no shared bottleneck. A ~6–9 minute fixed per-leg overhead (`ci-setup`
provisioning, LFS pull, Docker Scout) is identical either way and cannot be accelerated
by RBE.

**RBE buys no per-PR speed on a single worker. Its value is cross-compilation and the
ability to scale out workers.**

### Build-graph parallelism analysis

The available parallelism of a Bazel build is:

```
available_parallelism = total_action_work_seconds / critical_path_seconds
```

Measured from the `--profile` JSON for the regular build (fastbuild):

- **I/O-bound, not CPU-bound.** Output download (CAS fetch) is 6.4–9.3× the remote
  compute time per leg. The worker executes only ~2.3–2.5× actions concurrently despite
  having 16 vCPUs — it idles. `max_inflight_tasks=0` (unlimited) so the worker is not
  slot-limited.
- **Available parallelism ≈ 5.8×–17×** (6× counting pure CPU, up to 17× counting I/O
  overlap). A single regular build keeps roughly 6 cores busy.
- **amd64 critical path** is dominated (~87%) by `Tar src/ffmpeg_layer_amd64.tar`
  (≈21.6 min cold under `--remote_download_toplevel`; mitigated by
  `--remote_download_minimal`). The slowest translation units are
  `util/Parsing.cpp` (65–71s) and `protobuf/descriptor.cc` (48–59s).
- **Toolchain bootstrap** is ~18% of total action-seconds. The exec-config toolchain
  (exec=linux-x86_64) is shared across all three legs. On a warm CAS all legs reuse it;
  on a cold run they race.

The asan/ubsan build is CPU-bound with ~37× available parallelism (683 seconds of compile
work over an ~18-second critical path). That is the configuration where adding more worker
cores has the most impact.

**Ranked levers for build latency:**

1. `--remote_download_minimal` (build-without-the-bytes, already the `.bazelrc` default):
   stops fetching intermediate action outputs. The biggest single lever for an I/O-bound
   build — amd64 warm latency drops ~19%.
2. Persistent warm SSD CAS (the everyday PR case): 7–8.6 min amd64, ~2.7–3 min cross.
3. More workers. Do not raise `--jobs` on a single worker — `--jobs=64` was tested and
   caused `UNAVAILABLE` across all legs on a cold cache (the build is I/O-bound; more
   client fan-out piles up CAS pressure without adding CPU). The documented scaling path
   is a worker pool, not a higher `--jobs` value.

   **Update:** the backend moved from the 16-vCPU GCP demonstrator to a 250-core / 512 GB
   box on our own infra. `--jobs` was raised to 128 (and `--remote_max_connections` to 200
   in `.bazelrc`) as a first-cut bump, not yet re-measured — the CAS-I/O ceiling above was
   never actually a function of core count, so this assumes (unverified) that the new
   box's disk/network throughput scaled along with its CPU count. Re-run the
   [Performance](#performance) measurements above on the new backend and revise this
   section once real numbers exist.

### Warm-cache action distribution

| leg | cache-hit (warm) | remote-exec (warm) |
|---|---|---|
| amd64 | ~3,948 | ~751 |
| arm64 (cross) | ~3,226 | ~77 |
| darwin (cross) | ~3,102 | ~50 |

Cross legs execute very few actions remotely on a warm cache (almost everything hits the
shared AC), which is why their warm wall-clock drops to ~2.4–2.9 min.

## Docker Scout warm-cache fix

Docker Scout inspects `daschswiss/sipi:latest` from the local Docker daemon. The image
is loaded as a side effect of the `:docker_smoke` test's own `docker load` step. On a
warm cache the smoke test is a cache hit and never runs — the image is absent and Docker
Scout fails with `No such image`.

The fix: `bazel-test-smoke` in `ci.yml` runs `bazel run //src:image_load` explicitly
before the test. This both loads the image into the daemon and materialises the OCI
tarball on disk under `--remote_download_minimal` (where it would otherwise not be
fetched). The smoke test then finds the image regardless of its cache state.

## CI topology

`ci.yml`'s `test` job runs the matrix on all three platforms. Each leg:

1. Calls `.github/actions/ci-setup` with its `matrix.bazel-platform`, which provisions
   bazelisk + `just` on the plain GitHub-hosted runner and internally calls
   `.github/actions/bazel-rbe` to obtain the `$RBE_FLAGS` string — no Nix involved.
2. Passes `$RBE_FLAGS` to every `just bazel-*` invocation.
3. Runs `just bazel-test` (unit + approval + e2e).
4. On Linux: runs `just bazel-test-smoke` (which first calls `bazel run //src:image_load`).
5. On linux-amd64 PRs: runs Docker Scout.

`ci.yml`'s path-gated `sanitizer` job, plus `coverage.yml`,
`fuzz.yml`, and `publish.yml`, all go
through the same `ci-setup` action so every Bazel invocation benefits from the shared
remote cache.

## Reference

- `.github/actions/bazel-rbe/action.yml` — the composite action; source of truth for the
  emitted flag string
- `bazel/patches/hermetic_llvm_headers_glob.patch` — the remote-execution safety patch
- `bazel/patches/hermetic_llvm_darwin_rtlib.patch` — drops the unused `-rtlib` from macOS
  Rust links; not RBE-related, applied by the same override
- `MODULE.bazel` — `single_version_override` for the `llvm` module (patch application)
  and `rust.toolchain(extra_target_triples = ...)` (cross toolchain registration)
- `platforms/BUILD.bazel` — `//platforms:linux_x86_64` exec properties
- `test/e2e/sipi_e2e_test.bzl` and `test/e2e/BUILD.bazel` — `no-sandbox` tag on e2e tests
- [`ops-tf`](https://github.com/dasch-swiss/ops-tf) (`live/virt-03/prod/vms/`) and
  [`infra`](https://github.com/dasch-swiss/infra)
  (`OS/ansible/roles/dasch.nativelink`) — VM provisioning and NativeLink store
  configuration / operations (private repos)
- [RBE Write Pressure](rbe-write-pressure.md) — why the store disk writes
  terabytes/day, and the client-side levers that reduce it (worker
  materialization, not client uploads)
