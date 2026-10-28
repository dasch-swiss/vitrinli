# Memory Budget

The full-lane decode memory budget prevents OOM from concurrent large image
decodes by capping the aggregate memory in-flight full-lane decodes may hold. It
is the memory half of Vitrinli's two-lane admission control: expensive full-image
decodes are hard-capped in decode bytes, while cheap tile decodes bypass the
budget entirely and are never charged.

## Why

The thread pool controls CPU concurrency but has no awareness of memory. Multiple
concurrent full-resolution JP2 decodes (e.g., 20000x30000 pixels) can each consume
hundreds of MB to GBs, exhausting the RAM envelope. The per-request pixel limit
caps individual requests, but it does not prevent aggregate memory exhaustion from
multiple concurrent large decodes.

## How It Works

1. **Precise estimation from IIIF parameters:** Before each decode, the actual
   decode buffer size is computed from IIIF region/size parameters. For JP2, this
   accounts for DWT reduce levels and ROI restrictions — a tile request on a
   20000x30000 source estimates ~4MB, not 4.8GB. A JP2 with subsampled
   components also holds one buffer per component while it decodes, together at
   most the size of the decode buffer (half of it for 4:2:0). The estimate does
   not charge them: the shape probe it is computed from does not report
   subsampling. A size-restricted request is estimated as the decode of the
   whole image at the restricted dimensions (RW, RH), whatever its region, since
   every restricted output is cut from that one rendering.

2. **Pipeline-aware peak estimation:** Walks the processing stages (decode → scale
   → rotate → ICC convert) and returns the maximum concurrent allocation at any
   point, accounting for 2-stage downscale intermediates and rotation expansion.

3. **Lane classification:** A decode whose estimated peak memory reaches
   `large_decode_threshold_bytes` (default 32 MiB) is a **full-lane** decode and is
   charged against the budget; below it is a **tile** decode that bypasses the
   budget. The threshold is defined once in the shell config and passed to the
   engine over the FFI seam, so the shell (thread lanes) and the engine (memory
   budget) classify against the same value.

4. **Lock-free accounting:** Uses `std::atomic<size_t>` with compare-exchange for
   zero-contention acquire/release. The budget check adds nanoseconds vs.
   millisecond decode times.

5. **RAII release:** `MemoryBudgetGuard` releases the budget on all exit paths
   including exceptions and mid-decode client disconnect. No manual cleanup needed.

## The full-lane budget

The budget is derived from two knobs — the RAM envelope and the tile reserve:

```
full_mem = memory_limit × (1 − tiles_memory_ratio)
```

`memory_limit` is the total RAM envelope; `0` auto-detects available RAM. The
reserve `memory_limit × tiles_memory_ratio` (25% by default) is never charged to
the full lane — it houses tile decode usage plus the non-decode floor (base heap,
allocator retention, HTTP/encode buffers, cache). The invariant to verify while
observing in `basic` before returning to the enforcing `advanced` default is
`reserve ≥ observed floor`.

## Configuration

| Parameter | Default (binary) | Description |
|-----------|-----------------|-------------|
| `memory_limit` | `"0"` (auto) | Total RAM envelope. `0` = auto-detect available RAM. Accepts `M`/`G` suffixes: `"8G"`, `"500M"` |
| `tiles_memory_ratio` | `0.25` | Fraction of the envelope reserved for tiles + non-decode floor (range 0..1); the full lane gets the rest |
| `admission_mode` | `"advanced"` | `"basic"` (advanced tier shadow-counts only) or `"advanced"` (reject over budget). There is no `"off"` — the budget is always accounted |
| `large_decode_threshold_bytes` | `33554432` (32 MiB) | Estimated peak-memory at/above which a decode is charged to the full lane; below it bypasses as a tile |

Available via (see also [Running Vitrinli](../guide/running.md)):

- CLI flags: `--memory-limit`, `--tiles-memory-ratio`, `--admission-mode`, `--large-decode-threshold-bytes`
- Environment: `SIPI_MEMORY_LIMIT`, `SIPI_TILES_MEMORY_RATIO`, `SIPI_ADMISSION_MODE`, `SIPI_LARGE_DECODE_THRESHOLD_BYTES`
- Rust TOML config file (shell-owned)

These knobs are **not** read from the Lua config — they are set via the CLI/env/TOML
surface above and applied over the seam at init.

The DEFAULT (an unset `admission_mode`) is `"advanced"`. An UNRECOGNIZED value
(e.g. a stale `"off"` from an old template, or a legacy `"monitor"`/`"enforce"`)
is a different case — it is not a startup error, Vitrinli silently falls back to
`"basic"`.

### Auto-Detection

When `memory_limit = "0"` (default), the envelope is the detected available RAM:

1. **cgroups v2:** `/sys/fs/cgroup/memory.max`
2. **cgroups v1:** `/sys/fs/cgroup/memory/memory.limit_in_bytes`
3. **Linux fallback:** `/proc/meminfo` MemTotal
4. **macOS:** `sysctl hw.memsize`
5. **Fallback:** 1 GB if detection fails

The full lane then gets `envelope × (1 − tiles_memory_ratio)`; the reserve is the
headroom for tiles and the non-decode floor.

> On `vre-prod-01` Vitrinli is not cgroup-capped, so `memory_limit = "0"` would size
> the envelope to the whole VM RAM. Set `memory_limit` explicitly (ops-deploy
> renders `DSP_IIIF_MEMORY_LIMIT` into it) to hold Vitrinli to the intended cap.

## Advanced behaviour: 503 vs 413

In `advanced` mode a full-lane decode that cannot be admitted is rejected two ways:

- **503 Service Unavailable + `Retry-After`** — the budget is *currently*
  exhausted by concurrent decodes. The request fits on its own and may succeed on
  retry.
- **413 Payload Too Large** (no `Retry-After`) — the request's estimate *alone*
  exceeds the whole full-lane budget. It can never succeed, so retrying is
  pointless.

Tile decodes are never rejected for full-lane memory pressure — they bypass the
budget.

## Observing Before Enforcing

Generic deployments run `advanced` (enforcing) by default. To observe the
shadow counters before enforcing — e.g. before tuning `memory_limit`/
`tiles_memory_ratio` on a new deployment — override to `basic` explicitly:

1. **Set `SIPI_ADMISSION_MODE=basic`** (or `DSP_IIIF_ADMISSION_MODE=basic` in
   ops-deploy) and redeploy:
   - The budget is tracked and logged but requests are never rejected.
   - `sipi_decode_memory_shadow_rejected_total` shows what *would* be 503'd;
     `sipi_decode_memory_shadow_too_large_total` what *would* be 413'd.

2. **Observe metrics** (1-2 weeks):
   - Budget utilization: `sipi_decode_memory_used_bytes / sipi_decode_memory_budget_bytes` — should be < 0.8 normally.
   - Shadow rejection rate: `rate(sipi_decode_memory_shadow_rejected_total[5m])`.
   - Request size distribution: `histogram_quantile(0.99, rate(sipi_decode_memory_estimate_bytes_bucket[5m]))`.
   - Verify the reserve holds the floor: `(sipi_memory_limit_bytes − sipi_decode_memory_budget_bytes) ≥` the observed non-decode working set.

3. **Tune** if needed:
   - If shadow rejections fire on normal full traffic, raise `memory_limit` or lower `tiles_memory_ratio`.
   - Use the histogram to understand the size distribution being served.

4. **Remove the `basic` override** (unset `SIPI_ADMISSION_MODE` /
   `DSP_IIIF_ADMISSION_MODE`) to return to the enforcing `advanced` default.

## Prometheus Metrics

Vitrinli exports over OTLP, so these are the names the collector renders after
normalization, not the output of a scrape endpoint.

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `sipi_decode_memory_budget_bytes` | Gauge | — | Full-lane byte cap (`memory_limit × (1 − tiles_memory_ratio)`; set at startup) |
| `sipi_decode_memory_used_bytes` | Gauge | — | Currently allocated to in-flight full-lane decodes |
| `sipi_decode_memory_acquired_total` | Counter | — | Admitted full-lane decodes |
| `sipi_decode_memory_rejected_total` | Counter | — | Full-lane decodes refused with 503 in `advanced` mode (transient) |
| `sipi_decode_memory_too_large_total` | Counter | — | Requests refused with 413 in `advanced` mode (estimate alone exceeds the budget — permanently unservable) |
| `sipi_decode_memory_shadow_rejected_total` | Counter | — | Decodes that *would* be 503'd in `basic` mode |
| `sipi_decode_memory_shadow_too_large_total` | Counter | — | Requests that *would* be 413'd in `basic` mode |
| `sipi_decode_memory_near_limit_total` | Counter | — | Acquisitions where usage > 80% of budget |
| `sipi_decode_memory_estimate_bytes` | Histogram | — | Per-request peak memory estimates |

## Traffic Patterns

| Request Type | Typical Estimate | Lane |
|-------------|-----------------|------|
| Tile (256x256) | < 1 MB | Tile — bypasses the budget |
| Thumbnail (/full/,128/) | < 100 KB | Tile — bypasses the budget |
| Medium (/full/,2000/) | 50-120 MB | Full — charged against the budget |
| Full resolution (/full/max/) | 1-5 GB | Full — budget limits concurrency |
| Full + rotation (/full/max/90/) | 2-10 GB | Full — often exceeds the budget alone (413) |

## Troubleshooting

**Full-lane 503s on legitimate traffic:**
- Check `histogram_quantile(0.5, rate(sipi_decode_memory_estimate_bytes_bucket[5m]))` — the median for viewer traffic should be well below the threshold.
- If large requests dominate, raise `memory_limit` or lower `tiles_memory_ratio`.

**413s appearing:**
- A single request's estimate exceeds the full-lane budget. Either the source is enormous and the client requested `/full/max/`, or `memory_limit`/`tiles_memory_ratio` leave the full lane too small.

**OOM despite the budget enabled:**
- Check mode is `advanced`, not `basic`.
- Check `sipi_decode_memory_budget_bytes` matches the expected `memory_limit × (1 − tiles_memory_ratio)`.
- Memory outside the full-lane decode pipeline (tiles, cache, Lua, HTTP buffers) lives in the reserve, not the budget — size the reserve with `tiles_memory_ratio`.
- `sipi_malloc_arena_bytes` reports the process resident set (true RSS, from `mi_process_info`) — compare it against the envelope and `container_memory_working_set_bytes` to see how close the whole process runs to the cap. `sipi_malloc_retained_bytes` (RSS not currently handed out) rising with a flat `in_use` is allocator retention, not a leak.
