# Admission Control

Vitrinli admits requests through a **cost-based two-partition pool** that protects
viewer tile traffic and the RAM envelope from expensive full-image decodes
(overwhelmingly distributed crawler bots). Each request is classified into a
**Tile** or **Full** partition; tiles get a guaranteed thread floor and burst
into idle capacity, full-image downloads are hard-capped in both threads and
decode memory and shed when their share is exhausted.

This page covers the thread partitions and the overall model. The memory half —
the full partition's decode-RAM cap — is detailed in
[Memory Budget](memory-budget.md).

## The model

Everything derives from two hard caps (RAM + CPU threads), two ratios, and a mode:

- **Threads.** `tile_min = round(nthreads × tiles_thread_ratio)` is guaranteed to
  tiles; `full_max = nthreads − tile_min` hard-caps concurrent full decodes. A
  tile takes a global permit only and may burst up to `nthreads` when the full
  partition is idle. A full takes its full-partition permit **first, then** the
  global permit, so at most `full_max` fulls ever contend for global permits and
  `≥ tile_min` are always free for tiles.
- **Memory.** `full_mem = memory_limit × (1 − tiles_memory_ratio)` caps the full
  partition's decode bytes; tiles bypass the budget. See
  [Memory Budget](memory-budget.md).
- **Mode.** Two tiers. The **basic tier** (the global CPU/thread concurrency cap)
  is always enforced. `advanced` (default) enforces both tiers and sheds with
  503/413; `basic` enforces only the basic tier and shadow-counts what the
  **advanced tier** (the full thread cap plus the memory budget) *would* shed.

Tile priority is **bounded, not absolute**: `tokio`'s semaphore is FIFO, so a
full already parked in the global queue does not yield to a later tile. The
tile floor (`≥ tile_min` global permits kept free of fulls) is the mechanism that
preserves tile headroom — an explicit design choice, not automatic preemption.

## What is a tile vs a full request

| Request | Partition |
|---------|-----------|
| Viewer tiles, small explicit `{w},{h}` / `!{w},{h}` sizes | Tile |
| `info.json`, `knora.json` (metadata, no decode) | Tile |
| `/{id}/file` (raw byte stream, no decode) | Tile |
| Large explicit sizes, `/full/max/`, percentages (estimated peak ≥ `large_decode_threshold_bytes`) | Full |
| Lua routes and docroot `.lua`/`.elua` scripts (script cost is unknowable up front; decodes they trigger are memory-budgeted in the same lane) | Full |

Classification is coarse in the shell (from the IIIF URL params, before any
decode) and precise in the engine (from `estimate_peak_memory`). The two are
tracked and their disagreement is observable, so residual drift can be tuned.

## Configuration

| Env var | CLI flag | Default | Description |
|---------|----------|---------|-------------|
| `SIPI_NTHREADS` | `--nthreads` | `0` (auto) | Worker threads (0 = auto-detect) |
| `SIPI_TILES_THREAD_RATIO` | `--tiles-thread-ratio` | `0.5` | Fraction of workers guaranteed to tiles (0..1) |
| `SIPI_MEMORY_LIMIT` | `--memory-limit` | `0` (auto) | Total RAM envelope (0 = auto-detect) |
| `SIPI_TILES_MEMORY_RATIO` | `--tiles-memory-ratio` | `0.25` | Fraction of the envelope reserved for tiles + the non-decode floor |
| `SIPI_ADMISSION_MODE` | `--admission-mode` | `advanced` | `basic` (enforce basic tier only) or `advanced` (also enforce the advanced tier) |
| `SIPI_LARGE_DECODE_THRESHOLD_BYTES` | `--large-decode-threshold-bytes` | `33554432` (32 MiB) | Estimated peak at/above which a decode is a full-partition decode |
| `SIPI_REQUEST_TIMEOUT` | *(none)* | `60` (seconds) | Handler wall-clock timeout; a request whose handler exceeds it answers 408. Must exceed `SIPI_QUEUE_TIMEOUT` plus the largest expected full decode, or legitimate slow requests get 408 instead of completing |
| `SIPI_BODY_READ_TIMEOUT` | *(none)* | `10` (seconds) | Lua-route/docroot request-body-read timeout; a slow/trickling client is cut off before it ever reaches `admission.acquire`, so it cannot hold a Full permit while still streaming its body |

`ops-deploy` renders `DSP_IIIF_MEMORY_LIMIT` → `SIPI_MEMORY_LIMIT` and
`DSP_IIIF_ADMISSION_MODE` → `SIPI_ADMISSION_MODE`. The DEFAULT (an unset
`admission_mode`) is `advanced`. An UNRECOGNIZED value (e.g. a stale `off` or a
legacy `monitor`/`enforce`) is a different case — it is **not** a startup
error; it silently falls back to `basic`.

## Observing before enforcing

Generic deployments run `advanced` (enforcing) by default. To observe the
advanced tier's shadow counters before it enforces — e.g. before tuning
`full_max`/`full_mem` on a new deployment — override to `basic` explicitly:

1. **Set `SIPI_ADMISSION_MODE=basic`** (or `DSP_IIIF_ADMISSION_MODE=basic` under
   `ops-deploy`) and redeploy. `basic` enforces only the basic tier — the
   advanced tier becomes observe-only (the full thread cap and memory budget do
   not reject; they shadow-count).
2. **Observe** (1–2 weeks):
   - `sipi_admission_permits_in_use` / `sipi_admission_permits_total` and
     `sipi_admission_full_in_use` — global and full-partition saturation.
   - `sipi_admission_tile_waiting` / `sipi_admission_full_waiting`,
     `sipi_admission_tile_shed_total` / `sipi_admission_full_shed_total` —
     per-partition queue pressure and 503s.
   - `sipi_admission_full_shadow_rejected_total` and the full-partition memory
     shadow counters (see Memory Budget) show what `advanced` would shed — enough
     to size `full_max`/`full_mem`.
   - `sipi_admission_mode`, `sipi_admission_tile_min_threads`,
     `sipi_admission_full_max_threads`, the ratios, `sipi_admission_memory_limit_bytes`,
     `sipi_admission_large_decode_threshold_bytes` — the config fingerprint.
3. **Tune** the ratios / `memory_limit` if the shadow counters fire on legitimate
   traffic.
4. **Remove the `basic` override** (unset `SIPI_ADMISSION_MODE` /
   `DSP_IIIF_ADMISSION_MODE`) to return to the enforcing `advanced` default.

## Rejections

- **503 Service Unavailable + `Retry-After`** — the pool (threads) or the full
  memory budget is currently saturated; retry may succeed.
- **413 Payload Too Large** (no `Retry-After`) — a single request's estimate
  alone exceeds the full-partition memory budget; it can never succeed.

Tiles are never shed for full-partition pressure — they only shed when no global
permit is genuinely free (a tile burst beyond `nthreads`).

## Metrics

All admission metrics share the `sipi_admission_*` namespace (rendered from the
OTLP `sipi.admission.*` instruments; Prometheus appends `_total` to counters).

**Gauges (point-in-time occupancy + sizing):**

- `permits_in_use` / `permits_total` — global pool saturation.
- `full_in_use` — full sub-pool permits held (against `full_max_threads`).
- `tile_waiting` / `full_waiting` — requests parked per partition. Tiles wait
  only behind other tiles (exempt from the full queue-depth shed).

**Counters (monotonic):**

- `tile_shed_total` / `full_shed_total` — 503 sheds per partition.
- `full_shadow_rejected_total` — basic-only: fulls the advanced-tier cap *would*
  have rejected (zero in `advanced`, where `full_shed_total` counts the real
  rejections). The signal that sizes `full_max` before the switch.
- `classifier_disagreement_total` — serves where the shell's pre-dispatch
  tile/full verdict differed from the engine's precise post-decode verdict. A
  low, flat value confirms the pixel-proxy heuristic tracks the engine; a rising
  value means the bytes-per-pixel proxy needs revisiting.

**Config fingerprint** (gauges, observable with no ops-deploy change): `mode`,
`tile_min_threads`, `full_max_threads`, `tiles_thread_ratio`,
`tiles_memory_ratio`, `memory_limit_bytes`, `large_decode_threshold_bytes`.

The full-partition memory metrics (`decode_memory_*`, including the 413/`too_large`
counters) are documented in [Memory Budget](memory-budget.md).

> **Temporality.** The `sipi_admission_*` counters are cumulative (monotonic)
> OTLP sums that live for the whole process and reset only on restart, so
> `rate()` / `increase()` read them correctly. (The `max_over_time()` idiom some
> Vitrinli dashboards use is for windowed *extremes* over gauges, a different query
> pattern — it does not apply to these counters.)
