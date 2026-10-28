# RBE Write Pressure

This page is the durable record of how Vitrinli's CI writes to the Remote Build
Execution (RBE) backend, so the "why is the RBE disk writing so much?" question
does not get re-investigated from scratch. It covers the mechanism, the numbers
as measured, the client-side levers that actually move the needle, and how to
reproduce the analysis.

See [Remote Build Execution](rbe.md) for the backend topology and cross-compile
setup. The store lives on the DaSCH-hosted VM `dasch-remotebuild-prod-01`
(NativeLink config in [`infra`](https://github.com/dasch-swiss/infra)
`OS/ansible/roles/dasch.nativelink`).

## Bottom line

**Write pressure on the RBE host is worker-side action materialization, not
client uploads to CAS.** The Bazel client uploads a few hundred MB per full CI
run; the store disk absorbs terabytes per busy hour. Do not reason about RBE
write pressure from the client's `bytesSent` — it is five orders of magnitude
too small to be the signal.

The CAS store is **bounded with multi-TB headroom**, so store *growth* /
eviction is out of scope. This page is only about the *rate of writes* to the
store disk and what the client can do to reduce it.

## The numbers (measured 2026-07-24)

Reconciliation over 24h on `dasch-remotebuild-prod-01`:

| Quantity | Value |
|---|---|
| Bytes written to `vdb` (the NativeLink store disk) | **~22.6 TB / day** |
| Bytes written to `vda` (OS disk) | 17 GB / day |
| CAS net growth (new persisted content) | **272 GB / day** |
| Bazel client → server upload, per full 5-leg CI run | ~200 MB (27+22+8+17+129 MB across the legs) |
| Peak store-disk write in a single busy hour | 1.6–3.2 TB/h (≈ 0.9 GB/s avg, bursts to ~2.5 GB/s) |

The store disk writes **~22.6 TB to persist 272 GB of new content — roughly 83×
write amplification.** The ~22.3 TB difference is transient: worker input/output
materialization and bounded worker-cache churn, written and overwritten, never
persisted.

The store disk (`vdb`) holds every NativeLink directory: `cas` (~567 GB),
`ac`, `bazel-remote`, `directory_cache-1..4` (~40 GB each), `worker-1..4`
(~45–51 GB each), and the ephemeral `work-1..8` exec sandboxes.

## Why the amplification happens

For every action that is **remotely executed** (not a cache hit), the worker:

1. Materializes the action's **entire input closure** onto its local filesystem
   (`work-N` sandbox + `directory_cache-N`) before the action runs. For
   large-fan-in actions this is huge:
   - `CppLink` of `sipi` pulls the whole object + archive closure (multi-GB).
   - The OCI image `Tar` / `CopyToDirectory` materializes the full rootfs.
   - `TestRunner` / `RunfilesTree` materialize the test's whole runfiles tree.
2. Writes the action's outputs to disk, then into CAS.

So store-disk write volume scales with **(number of remotely-executed actions) ×
(their materialized closure size)** — not with the count of unique blobs that
end up persisted. A cache *hit* writes nothing on the worker; a cache *miss* on a
big-fan-in action writes its entire closure.

Two amplifiers on top of that, both server-side (noted for completeness, not
client-tunable): the `directory_cache-N` worker caches are bounded (~40 GB each),
so a working set larger than the bound thrashes (evict + rewrite); and a
filesystem-backed CAS of millions of small blob files carries heavy metadata /
journal write overhead.

## Client-side levers (what CI can change)

Ranked by expected impact on worker materialization. The goal is fewer /
smaller remotely-executed actions.

### 1. Drop `--stamp` from the test path (highest impact, zero risk)

`just bazel-test` and `just bazel-coverage` pass `--stamp`. Stamping injects the
git commit into `SipiVersion.h`, which makes the final `CppLink` of `sipi` **and**
the OCI image (via `STABLE_IMAGE_CREATED`) unique **every commit**. Unique inputs
never cache-hit, so on every commit the worker re-executes the full link and
image build and materializes their entire multi-GB closures.

Unstamped, those outputs are deterministic and cache across commits whose engine
code is unchanged (the common PR case) — the worker stops materializing the link
and image closures on every run.

The test suite is already written to tolerate the unstamped
`0.0.0-unstamped` fallback: `test/e2e/tests/cli.rs::cli_version_flag` asserts
*either* the stamped or unstamped string, and `health.rs` / `docker_smoke.rs`
only assert the `version` field is a string.
Keep `--stamp` on `bazel-build`, the Docker build, and release recipes (they need
the real version); drop it only from `bazel-test` / `bazel-coverage`.

### 2. Do not build heavyweight non-test targets on the test legs

`bazel test //src/... //test/...` builds every non-test target under `//src`,
including `//src:sipi_image` (the OCI image — a full-rootfs `Tar`). The image is
already exercised by its own `bazel-test-smoke` job, so building it on all three
test legs makes the worker materialize the rootfs closure three extra times per
run for no test coverage. Scope the test pattern (or use
`--build_tag_filters`) so the image and other non-test artifacts are not pulled
into the test graph.

### 3. Deduplicate the concurrent sanitizer builds — DONE

Previously `sanitizer-unit` and `sanitizer-e2e` each remotely compiled the full
`--config=asan --config=ubsan` tree (~5500 `CppCompile` each) and ran
concurrently, so the worker materialized two near-identical instrumented
closures. They are now merged into a single `sanitizer` job that runs both test
sets in one `bazel test` invocation (`just bazel-test-sanitized`), so the shared
instrumented tree compiles exactly once.

### 4. Raise the cache-hit rate generally

Every avoided remote execution avoids one input-closure materialization. The
`linux-arm64` and `darwin-arm64` legs run with a **cold local action cache**
(zero local hits — every action round-trips to the remote cache), while
`linux-amd64` has a warm on-runner cache (~5600 local hits). Persisting the
per-runner action cache (or output base) on the arm64/darwin runners cuts
redundant remote work. This is primarily read relief, but fewer executions also
means less materialization.

### What NOT to do

Do **not** disable `--remote_upload_local_results`. It is what populates the
cache that gives the arm64/darwin legs their ~67% remote-cache-hit rate. Turning
it off trades a negligible one-time upload cut (uploads are ~200 MB/run — not the
problem) for permanently worse hit rates, i.e. *more* remote execution and
*more* materialization over time.

## How to reproduce the analysis

### 1. Pull the Build Event Protocol (BEP) JSON from CI

Every `ci.yml` test leg writes its Bazel invocation's BEP JSON via
`--build_event_json_file` and uploads it as an artifact (`bep-<platform>`,
`bep-sanitizer`; 30-day retention). See the upload
steps in `.github/workflows/ci.yml`.

```bash
# find a completed run that carries the artifacts
gh run list --workflow=ci.yml --limit 20 \
  --json databaseId,headBranch,conclusion,createdAt
gh api "repos/dasch-swiss/vitrinli/actions/runs/<run-id>/artifacts" \
  --jq '.artifacts[].name'
gh run download <run-id> -R dasch-swiss/vitrinli -D ./bep
```

### 2. Read the write-relevant BEP fields

The single `buildMetrics` event carries the signal:

- `actionSummary.runnerCount[]` — the disposition breakdown:
  `total` / `remote cache hit` / `remote` (executed remotely) / `local`
  (executed locally, uploaded) / `internal` (orchestration, not uploaded).
  **Writes to CAS come from `remote` + `local`; cache hits and `internal` do
  not write.**
- `actionSummary.actionData[]` — per-mnemonic `actionsExecuted`. Note: on a leg
  with a cold local cache this *includes remote cache hits* (reads), so a large
  `CppCompile` count is not proof of recompilation — cross-check against
  `runnerCount`.
- `actionSummary.actionCacheStatistics` — the *local* action cache (`hits` /
  `misses`). Zero hits ⇒ cold local cache ⇒ everything round-trips remotely.
- `artifactMetrics` — `sourceArtifactsRead`, `outputArtifactsSeen`,
  `topLevelArtifacts` sizes: the scale of data movement.
- `networkMetrics.systemNetworkStats.bytesSent` / `bytesRecv` — the client's
  upload / download. **`bytesSent` is NOT the host write signal.**

A useful tell: if `actionsExecuted` per mnemonic is *identical across different
branches*, the executing set is a fixed must-run tail (stamped links, tests,
non-cacheable actions), not churn driven by the code change.

### 3. Confirm against the host from Grafana

Datasource `grafanacloud-prom`, instance `dasch-remotebuild-prod-01`:

```promql
# Store-disk write total over 24h (GB) — the real write-pressure number
increase(node_disk_written_bytes_total{instance="dasch-remotebuild-prod-01",device="vdb"}[24h]) / 1e9

# CAS net growth over 24h (GB) — compare to the above to see amplification
(nativelink_store_dir_bytes{instance="dasch-remotebuild-prod-01",dir="cas"}
 - (nativelink_store_dir_bytes{instance="dasch-remotebuild-prod-01",dir="cas"} offset 24h)) / 1e9

# Per-directory store sizes (cas, ac, directory_cache-*, worker-*, work-*)
nativelink_store_dir_bytes{instance="dasch-remotebuild-prod-01"}
```

## Honest limitations

`node_exporter` reports writes **per block device**, not per directory, so the
~22.6 TB/day cannot be split precisely between worker input materialization,
`directory_cache` eviction churn, and CAS blob writes. The attribution to
worker-side materialization is by elimination: CAS net growth (272 GB) and client
uploads (~GB/day) together account for barely 1% of the disk writes, so the
remaining ~22 TB must be transient worker-side write/rewrite. NativeLink does not
currently export per-operation write counters that would let us confirm the split
directly.
