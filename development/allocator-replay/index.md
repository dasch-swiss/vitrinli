# Allocator replay harness

`tools/allocator-replay/` measures the **container RSS behavior of the production image under a fixed JP2 decode load** — the harness that root-caused the 2026-07-29 vre-prod-01 OOM and gated the mimalloc switch ([ADR-0019](https://sipi.io/adr/0019-mimalloc-production-allocator.md)).

Run it whenever a change touches the JP2 decode path's threading or allocation behavior (`src/format_handlers/cpp/SipiIOJ2k.cpp`, Kakadu thread handling, decode buffering), the allocator itself (`bazel/mimalloc.BUILD.bazel`, the `_ALLOCATOR` dep in `src/cli/rust/BUILD.bazel`), or allocator-relevant image env. Microbenchmarks (`just bench decode`) measure speed; this harness measures what those decodes do to resident memory over time — a regression here reaches production as an OOM, not a slow chart.

## What it measures, and how to read it

The replay drives a containerized sipi with a deterministic JP2-only request mix (large decodes weighted, per-request cache-key variety) and samples container RSS every ~2 s. The signal is the **per-minute RSS floor** — the lowest sample each minute:

- **Peaks** are legitimate in-flight decodes; ignore them (but they size the container limit: transient 2–3 GiB at this load is normal).
- **A flat floor** means freed memory actually leaves the process. Flat is within ±0.5 GiB/h of zero over 15 minutes.
- **A ratcheting floor** means memory that the engine freed is not coming back. Vary *only the allocator* (env or image) on the identical load to split the two possible causes: if another allocator flat-lines, it is allocator retention; if every allocator ratchets, it is a leak.

Fifteen minutes is enough to show or rule out a ratchet at this request rate; it says nothing about multi-day creep.

## Usage

```
just bazel-build                 # the CLI generates the corpus on first run
just bazel-docker-build-arm64    # or -amd64: the image under test

cd $(mktemp -d)                  # runs write <name>.csv/<name>.errors to $PWD
REPO=<repo-root>

# the image under test (daschswiss/sipi:latest by default)
$REPO/tools/allocator-replay/replay.sh candidate 900

# allocator variants on the same load, e.g. reproducing the glibc baseline
# against a release image:
IMAGE=daschswiss/sipi:v6.2.2 $REPO/tools/allocator-replay/replay.sh glibc 900
IMAGE=daschswiss/sipi:v6.2.2 $REPO/tools/allocator-replay/replay.sh arena2 900 -e MALLOC_ARENA_MAX=2
```

Each run prints the per-minute floors and a least-squares floor slope at the end. `<name>.errors` should contain only the corpus's known non-200s (the `!3000,3000` upscale 400s on small images); anything else is a finding.

## Baselines (2026-07-29, ~100 req/min, 6 workers, 6 GiB limit)

| configuration               | floor slope      | note                                         |
| --------------------------- | ---------------- | -------------------------------------------- |
| glibc default (v6.2.2)      | **+2.5 GiB/h**   | reproduces the prod OOM ratchet              |
| glibc, `MALLOC_ARENA_MAX=2` | flat             | the 6.2.3 stopgap                            |
| jemalloc `LD_PRELOAD`       | flat             | validated fallback                           |
| mimalloc 2.2.4 (BCR)        | **OOM at min 6** | abandoned-segment retention; never ship      |
| mimalloc v3.4.3 (shipped)   | flat             | floors 0.5–1 GiB                             |
| pre-6.2.1 build, glibc      | flat             | single-threaded decode: no churn, no ratchet |

The last row is the provenance pin: the glibc ratchet began with the 6.2.1 multithreaded-decode change (`7bdb6349`) — per-request Kakadu worker-thread creation is the churn that drives every allocator's worst case, which is why this harness must run again if that threading model changes.

In production, the same split is visible without a replay via the `sipi.malloc.*` gauges (`in_use` vs `retained`; see `src/server/rust/src/malloc_stats.rs`).
