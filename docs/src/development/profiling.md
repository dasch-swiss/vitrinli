# Profiling

Vitrinli integrates [Tracy](https://github.com/wolfpld/tracy), a real-time,
nanosecond-resolution instrumentation profiler, to answer one question that the
production metrics cannot: **for a given workload, where inside the request does
the time actually go?** Tracy is opt-in, local-dev only, and adds zero overhead
to normal builds.

## When to use what

Vitrinli has several observability and measurement tools. They do not overlap — each
answers a different question, in a different place. Reaching for the wrong one
wastes effort.

| Tool | Question it answers | Scope | Where it runs | Overhead |
|------|--------------------|-------|---------------|----------|
| **Prometheus + Grafana** | *How does the service behave for real users — latency/cache/throughput distribution, in aggregate, over time? Is it trending worse?* | All requests, aggregated | **Production**, always-on | Negligible |
| **Sentry** | *What broke, with what stack and context?* | Individual errors | Production | Negligible |
| **OpenTelemetry tracing** | *For this one slow request, which stage ate the time?* | Per-request span tree | Production / staging | Low |
| **Tracy** *(this doc)* | *Where in the code does the time go for this workload — per function, lock, allocation, thread, at ns resolution?* | One workload, deep | **Local dev**, opt-in | High while the GUI is connected |
| **`just bench`** ([Benchmarking](benchmarking.md)) | *Did my specific change make this operation measurably faster?* | One isolated op | Local dev | Measurement harness |
| **`just valgrind`** | *Is there a memory error or leak?* | Correctness, not speed | Local dev | Very high |

A note on the production stack, because the three pieces are often conflated:
**OpenTelemetry** is the instrumentation *standard* (how telemetry is emitted),
**Prometheus** is the aggregate *metrics store* (the counters/gauges/histograms,
which the collector derives from the OTLP stream), and **Grafana** is the
*visualization* layer on top. Vitrinli's Rust server emits **OpenTelemetry traces +
OTLP metrics** (with Sentry for errors): per-request spans via `OtelAxumLayer`,
W3C `traceparent` continuation in and out (incl. propagation to dsp-api on the
Lua preflight's outbound call), the engine counters bridged to OTLP, and the
`http.server.request.duration` latency histogram. None
of them tell you which C++ function consumed the milliseconds — that is Tracy's job.

## The loop

The tools are stages of one workflow, not alternatives:

1. **Grafana** surfaces *what matters to users* and *whether there is a problem*
   (e.g. p99 latency on JP2 region requests is bad; cache hit-rate dropped).
2. Reproduce that workload **locally**.
3. **Tracy** shows you *where the time goes* in the code for that workload
   (e.g. it is Kakadu decode, not encode; or an unexpected lock).
4. **`just bench` + `just bench-compare`** *prove the fix* with the
   [statistical-significance rule](benchmarking.md), on the same `-c opt` binary
   and machine.
5. Ship; **Grafana** *confirms* the production win.

Tracy is step 3. Prometheus/Grafana are steps 1 and 5. Optimize from data, not
from a hunch about which function "feels slow."

## Building and running

```bash
just bazel-build-tracy        # -c opt --config=tracy //src/cli/rust:sipi
./bazel-bin/src/cli/rust/sipi server --config config/sipi.localdev-config.lua
```

`just bazel-build-tracy` builds at `-c opt` (so the timeline reflects production
codegen) and adds `--config=tracy`, which defines `TRACY_ENABLE` and
`TRACY_ON_DEMAND` (see `.bazelrc`). Because the config touches every compile, the
first Tracy build is a cold `-c opt` rebuild — subsequent ones are incremental.

**On-demand.** With `TRACY_ON_DEMAND` the server collects nothing until the Tracy
GUI connects, so it is safe to leave a Tracy build running. Connection is over TCP
port **8086**.

## Viewing the timeline

The instrumented process listens on TCP port **8086** (`127.0.0.1`). The Tracy
profiler can either connect to a live client or open a saved `.tracy` capture, and
comes in two forms.

### Browser — no install (easiest)

<https://tracy.nereid.pl> is a WASM build that runs entirely in the page —
nothing to install. It does both: click **Connect** and enter the client address
(`127.0.0.1:8086` for a local run, or `host:8086` elsewhere), or load a saved
`.tracy` capture file from disk.

### Native desktop GUI

The installable equivalent (same live-connect and capture-file loading).

- **Linux:** `nix develop .#profiling` puts `tracy` (GUI) and `tracy-capture` on
  your `PATH`.
- **macOS:** `brew install tracy` (the nixpkgs GUI build is unreliable on
  aarch64-darwin, so it is not in the `profiling` shell on macOS).

Launch the profiler and click **Connect** (it auto-discovers a local client, or
enter `host:8086`).

To record a `.tracy` file headlessly — for CI, sharing, or feeding either viewer's
file loader — `tracy-capture -o sipi.tracy` connects to the running client and
captures until it disconnects.

Either way, drive the server with IIIF requests; each worker thread
(`sipi-worker`) shows a per-request timeline.

## What is instrumented

Zones mirror the four [benchmark tiers](benchmarking.md), so Tracy is the *live,
end-to-end* counterpart to the *isolated, microbenchmark* view. A typical IIIF
request decomposes as:

```
iiif_handler
├─ parse_iiif_uri            (parse tier)
└─ serve_iiif                (request handler)
   ├─ SipiCache::check       (cache tier)
   ├─ SipiImage::read        (decode tier)
   │  └─ SipiIOJpeg::read    (or SipiIO{J2k,Tiff,Png}::read)
   ├─ SipiImage::scale       (process tier)
   ├─ SipiImage::rotate
   ├─ SipiImage::convertToIcc
   ├─ SipiIOJpeg::write      (encode tier)
   └─ SipiCache::add
```

Instrumentation lives behind one first-party shim,
[`src/observability/cpp/profiling.h`](../../../src/observability/cpp/profiling.h): code
includes it and uses `SIPI_ZONE()` / `SIPI_ZONE_N("name")` rather than the Tracy
macros directly, so the profiler dependency sits at a single site.

## Adding a zone

```cpp
#include "observability/profiling.h"

void SomeClass::hotMethod()
{
  SIPI_ZONE_N("SomeClass::hotMethod");   // or SIPI_ZONE() to use the function name
  // ...
}
```

`SIPI_ZONE*` expand to Tracy zones only under `--config=tracy`; in every other
build they compile to nothing. The macros are RAII-scoped — the zone closes when
the enclosing scope exits, including on early `return`.

For a deeper dive (sampling, locks, memory, GPU), see the upstream
[Tracy manual](https://github.com/wolfpld/tracy/releases) (`tracy.pdf`).

## Why it is free when off

Tracy is an unconditional Bazel dependency, but inert without `--config=tracy`:
the entire profiler — sockets, sampling, the allocation arena — is gated behind
`TRACY_ENABLE`, so a normal build links no listening socket and collects nothing.
See [`docs/adr/0016-tracy-opt-in-dev-profiler.md`](../../adr/0016-tracy-opt-in-dev-profiler.md)
for the rationale behind the always-present-but-inert wiring.
