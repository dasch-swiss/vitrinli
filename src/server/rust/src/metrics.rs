//! The OTLP metrics bridge: engine counters + the shell's own concurrency
//! signals, exported as OTel observable instruments.
//!
//! The C++ engine keeps its own metrics singleton (cache /
//! decode-memory / admission counters + gauges), read across the seam as a flat
//! [`SipiMetricsSnapshot`]. This module registers an OTel **observable** (async)
//! instrument per field: on each collection the SDK invokes the callback, which
//! snapshots the singleton and reports the current value — a pull, not a push,
//! matching the engine side's pure `sipi_guard`-only read. The meter provider
//! itself is built in [`crate::telemetry`] (fail-open on
//! `OTEL_EXPORTER_OTLP_ENDPOINT`); with no provider installed the global meter is
//! a no-op and every instrument here silently does nothing.
//!
//! Instrument names are OTel-idiomatic (`sipi.cache.hits`), which the standard
//! OTLP→Prometheus normalization at the collector renders as the existing
//! dashboard names (`sipi_cache_hits_total`). Two snapshot fields are **not**
//! bridged: `rejected_connections_total` and `waiting_connections` are never
//! written on the FFI serve path, so under this shell they stay zero, and the
//! two-lane pool publishes its own `sipi.admission.*` analogues instead.
//!
//! `every_snapshot_field_is_accounted_for` in the tests below is the tripwire: a
//! field added to the snapshot must land in [`COUNTERS`], [`GAUGES`], or that
//! two-name exclusion list, or the test fails. Nothing else detects a metric that
//! stops at the seam — it just never appears in Grafana.
//!
//! opentelemetry 0.31 has no batch-observer API (`register_callback` was
//! removed), so each instrument carries its own callback and each snapshots the
//! singleton. The read is a cheap singleton copy and collection runs at the
//! reader interval, so the per-cycle cost is immaterial.
//!
//! Two instruments are **synchronous** rather than observable, because they
//! record a distribution over individual requests that no end-of-interval poll
//! can reconstruct: [`record_http_duration`] (request latency) and
//! [`record_decode_estimate`] (per-serve decode-memory estimate). Their handles
//! are kept in `OnceLock`s, and a request that arrives before [`register`] ran
//! records nothing.

use std::sync::{Arc, OnceLock};
use std::time::Instant;

use axum::extract::{MatchedPath, Request};
use axum::middleware::Next;
use axum::response::Response;
use opentelemetry::metrics::{Histogram, Meter};
use opentelemetry::{global, KeyValue};

use admission::{Admission, AdmissionMode, AdmissionSnapshot};

use crate::ffi::{self, SipiMetricsSnapshot};
use crate::malloc_stats::{self, MallocStats};
use crate::preflight_cache;
use crate::routes;

/// Explicit bucket boundaries (seconds) for `http.server.request.duration`, as
/// recommended by the HTTP semantic conventions.
const HTTP_DURATION_BOUNDARIES: &[f64] = &[
    0.005, 0.01, 0.025, 0.05, 0.075, 0.1, 0.25, 0.5, 0.75, 1.0, 2.5, 5.0, 7.5, 10.0,
];

/// Bucket boundaries (bytes) for the decode-memory estimate: 1 KiB → 2 GiB,
/// matching the buckets the engine's own histogram has always used, so the
/// operator PromQL in `docs/src/operation/memory-budget.md` keeps its shape.
const DECODE_ESTIMATE_BOUNDARIES: &[f64] = &[
    1024.0,
    10_240.0,
    102_400.0,
    1_048_576.0,
    10_485_760.0,
    104_857_600.0,
    524_288_000.0,
    1_073_741_824.0,
    2_147_483_648.0,
];

/// The HTTP methods the semantic conventions treat as known; anything else is
/// reported as `_OTHER` so a client cannot mint unbounded label values by
/// sending arbitrary method tokens (a method router answers 405 *after* this
/// layer has already seen the request).
const KNOWN_METHODS: &[&str] = &[
    "CONNECT", "DELETE", "GET", "HEAD", "OPTIONS", "PATCH", "POST", "PUT", "TRACE",
];

static HTTP_DURATION: OnceLock<Histogram<f64>> = OnceLock::new();
static DECODE_ESTIMATE: OnceLock<Histogram<u64>> = OnceLock::new();

/// Register the engine + admission observable instruments against the global
/// meter. Safe to call unconditionally: with no meter provider installed (no OTLP
/// endpoint) the global meter is a no-op and this registers nothing observable.
/// Call once, after [`crate::telemetry::init`] has set the global provider and
/// after the [`crate::routes::AppState`] admission pool exists (its snapshot feeds
/// the concurrency + config-fingerprint gauges).
///
/// The instrument handles are intentionally dropped: `build()` registers the
/// callback with the SDK meter pipeline, which owns it for the meter provider's
/// lifetime; the returned handle carries none of that state.
pub(crate) fn register(admission: Arc<Admission>) {
    let meter = global::meter("sipi");

    // ── Synchronous histograms ──────────────────────────────────────────────
    // Recorded per request rather than polled: a latency or decode-size
    // distribution cannot be reconstructed from an end-of-interval sample.
    let _ = HTTP_DURATION.set(
        meter
            .f64_histogram("http.server.request.duration")
            .with_description("Duration of HTTP server requests")
            .with_unit("s")
            .with_boundaries(HTTP_DURATION_BOUNDARIES.to_vec())
            .build(),
    );
    let _ = DECODE_ESTIMATE.set(
        meter
            .u64_histogram("sipi.decode_memory.estimate_bytes")
            .with_description("Estimated peak decode memory for one served image")
            .with_unit("By")
            .with_boundaries(DECODE_ESTIMATE_BOUNDARIES.to_vec())
            .build(),
    );

    // ── Engine counters (monotonic) ─────────────────────────────────────────
    for (name, description, extract) in COUNTERS {
        counter(&meter, name, description, *extract);
    }

    // ── Engine gauges ───────────────────────────────────────────────────────
    for (name, description, unit, extract) in GAUGES {
        gauge(&meter, name, description, unit, *extract);
    }

    // ── Two-lane admission metrics ──────────────────────────────────────────
    // Every instrument snapshots the shared `Admission` pool on collection (a
    // cheap atomic read). All live occupancy + fixed-sizing + config-fingerprint
    // signals share the one `sipi.admission.*` namespace.
    //
    // Global permits in flight: total − currently-available — the real saturation
    // signal.
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.permits_in_use",
        "Admission permits currently held (blocking engine work in flight)",
        |s| s.permits_in_use as i64,
    );
    // Total permits (the configured worker count); fixed after startup.
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.permits_total",
        "Admission total permit count (the configured worker count)",
        |s| s.permits_total as i64,
    );
    // Full sub-pool permits in flight — full-partition saturation against the cap.
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.full_in_use",
        "Full sub-pool permits currently held (full decodes in flight)",
        |s| s.full_in_use as i64,
    );
    // Requests currently parked waiting for a permit, per partition. Tiles wait
    // only behind other tiles (exempt from the full queue-depth shed).
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.tile_waiting",
        "Tile requests currently waiting for a global permit",
        |s| s.tile_waiting as i64,
    );
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.full_waiting",
        "Full requests currently waiting for a permit",
        |s| s.full_waiting as i64,
    );
    // 503 sheds per partition (immediate queue-full + queue-timeout).
    admission_counter(
        &meter,
        &admission,
        "sipi.admission.tile_shed",
        "Tile requests shed with 503 because the global pool was saturated",
        |s| s.tile_shed_total,
    );
    admission_counter(
        &meter,
        &admission,
        "sipi.admission.full_shed",
        "Full requests shed with 503 because admission was saturated",
        |s| s.full_shed_total,
    );
    // Basic-mode-only: fulls the advanced cap *would* have rejected — sizes
    // `full_max` before the flip to advanced. Always zero under advanced (the cap
    // rejects for real, counted in `full_shed`).
    admission_counter(
        &meter,
        &admission,
        "sipi.admission.full_shadow_rejected",
        "Basic-mode fulls the advanced cap would have rejected",
        |s| s.full_shadow_rejected_total,
    );
    // Residual heuristic drift: the shell's pre-dispatch partition disagreed with
    // the engine's precise post-decode verdict.
    admission_counter(
        &meter,
        &admission,
        "sipi.admission.classifier_disagreement",
        "Serves where the shell tile/full verdict differed from the engine's",
        |s| s.classifier_disagreement_total,
    );

    // ── Lua runtime kills ───────────────────────────────────────────────────
    // The scripting crate's process-wide KillStats, split by reason — the OTLP
    // export of the runtime's kill accounting (a request VM killed for
    // exceeding its wall-clock or Lua-heap budget). Renders as
    // `sipi_lua_kills_total{reason}` after Prometheus normalization.
    let kills = scripting::kill_stats();
    let _ = meter
        .u64_observable_counter("sipi.lua.kills")
        .with_description("Request VMs killed by the Lua runtime limits, by reason")
        .with_callback(move |observer| {
            observer.observe(kills.timeout(), &[KeyValue::new("reason", "timeout")]);
            observer.observe(kills.memory(), &[KeyValue::new("reason", "memory")]);
        })
        .build();

    // ── Lua runtime durations ───────────────────────────────────────────────
    // Per-sample VM-build and script-run histograms, recorded through the
    // scripting crate's host recorder (the crate stays OTel-free); the
    // `entry_point` attribute is probe / pre_flight / file_pre_flight /
    // route / elua.
    let vm_build = meter
        .f64_histogram("sipi.lua.vm_build.duration")
        .with_description("Request-VM build time (hardened VM + bindings + init script)")
        .with_unit("s")
        .build();
    let script = meter
        .f64_histogram("sipi.lua.script.duration")
        .with_description("Lua hook/route script run time (after the VM stands)")
        .with_unit("s")
        .build();
    scripting::set_duration_recorder(scripting::DurationRecorder {
        vm_build: Box::new(move |entry, secs| {
            vm_build.record(secs, &[KeyValue::new("entry_point", entry)]);
        }),
        script: Box::new(move |entry, secs| {
            script.record(secs, &[KeyValue::new("entry_point", entry)]);
        }),
    });

    // ── Config fingerprint ──────────────────────────────────────────────────
    // The derived (threads, ratios, mode, thresholds) the pool was built from —
    // observable on Grafana with no ops-deploy change after shipping the binary.
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.mode",
        "Admission mode (0 = basic, 1 = advanced)",
        |s| i64::from(s.mode == AdmissionMode::Advanced),
    );
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.tile_min_threads",
        "Guaranteed tile thread floor",
        |s| s.tile_min as i64,
    );
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.full_max_threads",
        "Full-partition thread hard cap",
        |s| s.full_max as i64,
    );
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.memory_limit_bytes",
        "Resolved RAM envelope in bytes (0 = auto-detect at startup)",
        |s| s.memory_limit_bytes as i64,
    );
    admission_gauge(
        &meter,
        &admission,
        "sipi.admission.large_decode_threshold_bytes",
        "Tile/full classifier threshold in bytes",
        |s| s.large_decode_threshold_bytes as i64,
    );
    admission_float_gauge(
        &meter,
        &admission,
        "sipi.admission.tiles_thread_ratio",
        "Fraction of threads reserved as the tile floor",
        |s| s.tiles_thread_ratio,
    );
    admission_float_gauge(
        &meter,
        &admission,
        "sipi.admission.tiles_memory_ratio",
        "Fraction of the RAM envelope reserved for tiles + the non-decode floor",
        |s| s.tiles_memory_ratio,
    );

    // ── Preflight access-cache metrics ──────────────────────────────────────
    // The shell's opt-in cache in front of the `pre_flight` hook (see
    // `crate::preflight_cache`). Zero unless the cache is enabled
    // (`--preflight-cache-ttl > 0`); disabled by default, so no request touches it.
    meter
        .u64_observable_counter("sipi.preflight_cache.hits")
        .with_description("Preflight access-cache hits (hook skipped)")
        .with_callback(|observer| observer.observe(preflight_cache::hits(), &[]))
        .build();
    meter
        .u64_observable_counter("sipi.preflight_cache.misses")
        .with_description("Preflight access-cache misses (hook ran)")
        .with_callback(|observer| observer.observe(preflight_cache::misses(), &[]))
        .build();
    // ── Preflight decision metrics ──────────────────────────────────────────
    // The shell's resolved access decisions (`crate::routes`), split by
    // permission. Renders as `sipi_preflight_decisions_total{permission}` after
    // Prometheus normalization, so a share is a ratio over the same family.
    meter
        .u64_observable_counter("sipi.preflight.decisions")
        .with_description("Resolved preflight decisions, by permission")
        .with_callback(|observer| {
            for permission in routes::PERMISSIONS {
                observer.observe(
                    routes::decisions(permission),
                    &[KeyValue::new(
                        "permission",
                        routes::permission_str(permission),
                    )],
                );
            }
        })
        .build();
    // ── Process allocator metrics ───────────────────────────────────────────
    // Gauges splitting container RSS into "in use" vs "freed but retained by
    // the allocator" — the series that tells a leak apart from allocator
    // retention when RSS climbs. The binary registers a reader for its linked
    // allocator (mimalloc in production); the fallback reads glibc `mallinfo2`
    // (see `crate::malloc_stats`). When neither applies `stats()` is `None`
    // and the gauges observe nothing.
    for (name, description, extract) in MALLOC_GAUGES {
        let extract = *extract;
        meter
            .i64_observable_gauge(*name)
            .with_description(*description)
            .with_unit("By")
            .with_callback(move |observer| {
                if let Some(stats) = malloc_stats::stats() {
                    observer.observe(extract(&stats), &[]);
                }
            })
            .build();
    }

    meter
        .i64_observable_gauge("sipi.preflight_cache.entries")
        .with_description(
            "Filled slots in the preflight access-cache (incl. expired-but-not-yet-reclaimed; \
             trends toward the slot ceiling on a busy server)",
        )
        .with_callback(|observer| observer.observe(preflight_cache::entries(), &[]))
        .build();
}

/// axum middleware recording `http.server.request.duration` for one request.
///
/// Registered as the outermost router layer, so the observed duration covers the
/// whole in-router path — including the tracing layers — and is the closest thing
/// to what the client waited for. `/health` and `/favicon.ico` are registered
/// after the layers and so stay out of the histogram, matching their exclusion
/// from the trace pipeline.
///
/// Attributes are the route *template* (never the raw path), the normalised
/// method, and the status code, so the label set stays bounded by the routing
/// table rather than by the request stream.
pub(crate) async fn record_http_duration(req: Request, next: Next) -> Response {
    let method = normalise_method(req.method().as_str());
    let route = req
        .extensions()
        .get::<MatchedPath>()
        .map(|matched| matched.as_str().to_owned());
    let start = Instant::now();
    let response = next.run(req).await;
    let elapsed = start.elapsed().as_secs_f64();

    if let Some(histogram) = HTTP_DURATION.get() {
        let mut attributes = vec![
            KeyValue::new("http.request.method", method),
            KeyValue::new(
                "http.response.status_code",
                i64::from(response.status().as_u16()),
            ),
        ];
        // Conditionally required by the semantic conventions: present only when a
        // route actually matched.
        if let Some(route) = route {
            attributes.push(KeyValue::new("http.route", route));
        }
        histogram.record(elapsed, &attributes);
    }
    response
}

/// Map a request method onto the semantic conventions' known-method set,
/// collapsing anything else to `_OTHER` (see [`KNOWN_METHODS`]).
fn normalise_method(method: &str) -> &'static str {
    KNOWN_METHODS
        .iter()
        .find(|known| **known == method)
        .copied()
        .unwrap_or("_OTHER")
}

/// Record one serve's estimated peak decode memory, as carried back from the
/// engine by [`crate::ffi::serve_timings_take`]. A zero estimate means no decode
/// ran (cache hit, HEAD, or passthrough) and is not a sample.
pub(crate) fn record_decode_estimate(estimate_bytes: u64) {
    if estimate_bytes == 0 {
        return;
    }
    if let Some(histogram) = DECODE_ESTIMATE.get() {
        histogram.record(estimate_bytes, &[]);
    }
}

/// The 13 live monotonic counters: OTel name, description, and the field to read
/// from a snapshot. (`rejected_connections_total` is omitted — transport-dead.)
type CounterRow = (&'static str, &'static str, fn(&SipiMetricsSnapshot) -> u64);
const COUNTERS: &[CounterRow] = &[
    ("sipi.cache.hits", "Cache hits", |s| s.cache_hits_total),
    ("sipi.cache.misses", "Cache misses", |s| {
        s.cache_misses_total
    }),
    ("sipi.cache.evictions", "Cache entries evicted", |s| {
        s.cache_evictions_total
    }),
    (
        "sipi.cache.skips",
        "Files too large to cache (skipped)",
        |s| s.cache_skips_total,
    ),
    (
        "sipi.client_disconnected",
        "Requests aborted by client disconnect",
        |s| s.client_disconnected_total,
    ),
    (
        "sipi.memory_alloc_failures",
        "Allocation failures during image processing",
        |s| s.memory_alloc_failures_total,
    ),
    (
        "sipi.decode_memory.acquired",
        "Decode-memory budget: acquisitions",
        |s| s.decode_memory_acquired_total,
    ),
    (
        "sipi.decode_memory.rejected",
        "Decode-memory budget: rejections",
        |s| s.decode_memory_rejected_total,
    ),
    (
        "sipi.decode_memory.shadow_rejected",
        "Decode-memory budget: shadow-mode rejections",
        |s| s.decode_memory_shadow_rejected_total,
    ),
    (
        "sipi.decode_memory.near_limit",
        "Decode-memory budget: times usage exceeded 80% of budget",
        |s| s.decode_memory_near_limit_total,
    ),
    (
        "sipi.decode_memory.too_large",
        "Full-lane budget: requests whose estimate alone exceeds the budget (413)",
        |s| s.decode_memory_too_large_total,
    ),
    (
        "sipi.decode_memory.shadow_too_large",
        "Full-lane budget: basic-mode would-be 413 (estimate alone exceeds budget)",
        |s| s.decode_memory_shadow_too_large_total,
    ),
    (
        "sipi.tiff_pyramid.reduced_decodes",
        "TIFF decodes served from a reduced pyramid level",
        |s| s.tiff_pyramid_reduced_decodes_total,
    ),
];

/// The 7 live gauges: OTel name, description, unit (`""` = none), and the field.
/// (`waiting_connections` is omitted — transport-dead.)
type GaugeRow = (
    &'static str,
    &'static str,
    &'static str,
    fn(&SipiMetricsSnapshot) -> i64,
);
const GAUGES: &[GaugeRow] = &[
    ("sipi.cache.size_bytes", "Current cache size", "By", |s| {
        s.cache_size_bytes
    }),
    ("sipi.cache.files", "Current cached file count", "", |s| {
        s.cache_files
    }),
    (
        "sipi.cache.size_limit_bytes",
        "Configured cache size limit (-1 = unlimited)",
        "By",
        |s| s.cache_size_limit_bytes,
    ),
    (
        "sipi.cache.files_limit",
        "Configured cache file-count limit (0 = none)",
        "",
        |s| s.cache_files_limit,
    ),
    (
        "sipi.decode_memory.budget_bytes",
        "Configured decode-memory budget",
        "By",
        |s| s.decode_memory_budget_bytes,
    ),
    (
        "sipi.decode_memory.used_bytes",
        "Decode memory currently in use",
        "By",
        |s| s.decode_memory_used_bytes,
    ),
    (
        "sipi.wedged_threads",
        "Leaked decode threads that blew the seam deadline; at --max-wedged-threads every request is shed and /health fails until restart",
        "",
        |s| s.wedged_threads,
    ),
];

/// The process-allocator gauges: OTel name, description, and the field to
/// read from a [`MallocStats`] reading. All byte-valued. Field semantics per
/// allocator are documented on [`MallocStats`].
type MallocGaugeRow = (&'static str, &'static str, fn(&MallocStats) -> i64);
const MALLOC_GAUGES: &[MallocGaugeRow] = &[
    (
        "sipi.malloc.in_use_bytes",
        "Bytes allocated and not yet freed",
        |s| s.in_use_bytes,
    ),
    (
        "sipi.malloc.retained_bytes",
        "Resident bytes the allocator holds that are not currently handed out",
        |s| s.retained_bytes,
    ),
    (
        "sipi.malloc.mmap_bytes",
        "Bytes in mmap-served allocations outside the regular heap",
        |s| s.mmap_bytes,
    ),
    (
        "sipi.malloc.arena_bytes",
        "Process resident set size (true RSS)",
        |s| s.arena_bytes,
    ),
];

/// Build one observable `u64` counter whose callback snapshots the engine
/// metrics and reports `extract`'s field. A failed snapshot observes nothing
/// (fail-safe on the collection thread). The handle is dropped (see
/// [`register`]).
fn counter(
    meter: &Meter,
    name: &'static str,
    description: &'static str,
    extract: fn(&SipiMetricsSnapshot) -> u64,
) {
    meter
        .u64_observable_counter(name)
        .with_description(description)
        .with_callback(move |observer| {
            if let Some(snap) = ffi::metrics_snapshot() {
                observer.observe(extract(&snap), &[]);
            }
        })
        .build();
}

/// Build one observable `i64` gauge (see [`counter`]); `unit` is applied only
/// when non-empty (byte gauges pass `"By"`).
fn gauge(
    meter: &Meter,
    name: &'static str,
    description: &'static str,
    unit: &'static str,
    extract: fn(&SipiMetricsSnapshot) -> i64,
) {
    let mut builder = meter
        .i64_observable_gauge(name)
        .with_description(description);
    if !unit.is_empty() {
        builder = builder.with_unit(unit);
    }
    builder
        .with_callback(move |observer| {
            if let Some(snap) = ffi::metrics_snapshot() {
                observer.observe(extract(&snap), &[]);
            }
        })
        .build();
}

/// Build one observable `u64` counter whose callback snapshots the admission pool
/// and reports `extract`'s field. The handle is dropped (see [`register`]).
fn admission_counter(
    meter: &Meter,
    pool: &Arc<Admission>,
    name: &'static str,
    description: &'static str,
    extract: fn(&AdmissionSnapshot) -> u64,
) {
    let pool = Arc::clone(pool);
    meter
        .u64_observable_counter(name)
        .with_description(description)
        .with_callback(move |observer| observer.observe(extract(&pool.snapshot()), &[]))
        .build();
}

/// Build one observable `i64` gauge over an admission-snapshot field.
fn admission_gauge(
    meter: &Meter,
    pool: &Arc<Admission>,
    name: &'static str,
    description: &'static str,
    extract: fn(&AdmissionSnapshot) -> i64,
) {
    let pool = Arc::clone(pool);
    meter
        .i64_observable_gauge(name)
        .with_description(description)
        .with_callback(move |observer| observer.observe(extract(&pool.snapshot()), &[]))
        .build();
}

/// Build one observable `f64` gauge over an admission-snapshot field (the ratios).
fn admission_float_gauge(
    meter: &Meter,
    pool: &Arc<Admission>,
    name: &'static str,
    description: &'static str,
    extract: fn(&AdmissionSnapshot) -> f64,
) {
    let pool = Arc::clone(pool);
    meter
        .f64_observable_gauge(name)
        .with_description(description)
        .with_callback(move |observer| observer.observe(extract(&pool.snapshot()), &[]))
        .build();
}

#[cfg(test)]
mod tests {
    use super::{COUNTERS, GAUGES, MALLOC_GAUGES};
    use crate::ffi::SipiMetricsSnapshot;
    use std::collections::HashSet;
    use std::mem::size_of;

    /// Snapshot fields deliberately not exported, by field name. Neither is
    /// written on the FFI serve path, so both stay permanently zero under this
    /// shell; the two-lane pool publishes its own per-partition series instead
    /// (`sipi.admission.{tile,full}_waiting` / `_shed`).
    ///
    /// Lives here rather than beside the tables because it exists to be counted,
    /// not read — the reasoning is in the module docs.
    const NOT_BRIDGED: &[&str] = &["rejected_connections_total", "waiting_connections"];

    /// Every field the engine hands across the seam must either be exported or be
    /// listed as deliberately unexported.
    ///
    /// This guards the gap the `SipiMetricsSnapshot` layout test leaves: that
    /// test pins the struct's *shape*, so a field added C++-side and never
    /// bridged would pass silently and simply never reach production.
    ///
    /// The field count comes from the struct size rather than a hand-maintained
    /// name list, which would just be one more place to forget: every snapshot
    /// field is an 8-byte `u64`/`i64`, so `size_of / 8` is exact, and it moves on
    /// its own when a field is added.
    #[test]
    fn every_snapshot_field_is_accounted_for() {
        let field_count = size_of::<SipiMetricsSnapshot>() / 8;
        let accounted = COUNTERS.len() + GAUGES.len() + NOT_BRIDGED.len();
        assert_eq!(
            accounted, field_count,
            "{field_count} snapshot fields but {accounted} accounted for — a new \
             field must be added to COUNTERS, GAUGES, or NOT_BRIDGED, or it will \
             never reach OTLP"
        );
    }

    #[test]
    fn instrument_names_are_unique() {
        // A duplicate name would have two callbacks reporting one series, which
        // the SDK reconciles silently rather than rejecting.
        let names = names();
        let unique: HashSet<&&str> = names.iter().collect();
        assert_eq!(unique.len(), names.len(), "duplicate instrument name");
    }

    #[test]
    fn instrument_names_are_sipi_namespaced() {
        // Engine names reach dashboards through the collector's OTLP→Prometheus
        // normalization, which maps `.` to `_`. Underscores *within* a segment are
        // fine and intended (`sipi.cache.size_bytes` → `sipi_cache_size_bytes`);
        // an empty segment is not, because it renders a double underscore.
        for name in names() {
            assert!(
                name.starts_with("sipi."),
                "{name} should be namespaced under `sipi.`"
            );
            assert!(
                !name.ends_with('.') && !name.contains(".."),
                "{name} has an empty name segment"
            );
        }
    }

    /// Every table-driven instrument name: engine counters and gauges, then
    /// the allocator gauges.
    fn names() -> Vec<&'static str> {
        COUNTERS
            .iter()
            .map(|(name, ..)| *name)
            .chain(GAUGES.iter().map(|(name, ..)| *name))
            .chain(MALLOC_GAUGES.iter().map(|(name, ..)| *name))
            .collect()
    }
}
