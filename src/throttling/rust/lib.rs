//! The two-lane admission pool (shell-side, pre-dispatch).
//!
//! `Admission` bounds concurrent engine work with two semaphores derived from a
//! (threads, ratios) envelope: a global pool sized to `nthreads`, and a full
//! sub-pool sized to `full_max = nthreads − tile_min` that hard-caps concurrent
//! full-image decodes. Tiles take a global permit only and burst into any idle
//! capacity; a full takes its sub-pool permit **first, then** the global permit,
//! which bounds the number of fulls contending for global permits to `full_max`
//! and so always leaves `≥ tile_min` global permits reachable by tiles.
//!
//! The crate is FFI-free: it depends on `tokio` and the domain types from
//! `iiif_parser` only — no `//src/ffi`, no C++ engine — so its concurrency tests
//! run without linking the image engine (DUNE-002). It owns the per-partition
//! wait/shed counters (a single writer) and exposes them as an [`AdmissionSnapshot`]
//! for the OTLP bridge.
//!
//! Vocabulary is **admission** throughout: the pool is `Admission`, a request's
//! partition is an [`AdmissionKind`] (`Tile` or `Full`), the mode is an
//! [`AdmissionMode`]. See `CONTEXT.md` (Throttling) and
//! `docs/adr/0022-two-lane-admission-control.md`.

use std::fmt;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use iiif_parser::{IiifParams, ParsedRequest, RequestKind, SizeKind};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};

/// Bytes-per-pixel proxy the shell uses to turn `large_decode_threshold_bytes`
/// into a pixel-count cutoff. The engine classifies precisely from
/// `estimate_peak_memory` (actual channels/depth + pipeline stages); the shell
/// has only the IIIF params, so it estimates an output-pixel upper bound and
/// compares `pixels × PROXY` against the byte threshold. 4 = 8-bit RGBA, the
/// common decoded-buffer footprint; residual disagreement is observable via the
/// classifier-disagreement counter.
const BYTES_PER_PIXEL_PROXY: u64 = 4;

/// Which admission tier is enforced. `Basic` enforces only the basic tier — the
/// global CPU/thread concurrency cap — while the advanced tier (the memory-aware
/// full-lane cap) only shadow-counts what it *would* shed. `Advanced` also
/// enforces that advanced tier and sheds over the memory/full budget. There is
/// no "off" — the basic tier always applies and the advanced tier always
/// accounts, so its shadow counters are available to size the full partition.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AdmissionMode {
    Basic,
    Advanced,
}

impl AdmissionMode {
    /// Parse the mode string (case-insensitive). `None` for an unrecognized
    /// value so the caller can fall back to the `Basic` default.
    #[must_use]
    pub fn parse(s: &str) -> Option<Self> {
        match s.to_ascii_lowercase().as_str() {
            "basic" => Some(Self::Basic),
            "advanced" => Some(Self::Advanced),
            _ => None,
        }
    }

    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Basic => "basic",
            Self::Advanced => "advanced",
        }
    }
}

/// Which admission partition a request belongs to. `Tile` takes a global permit
/// only; `Full` takes a full sub-pool permit first, then the global permit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AdmissionKind {
    Tile,
    Full,
}

/// Plain construction inputs (no FFI types). The caller resolves these from its
/// config; `nthreads == 0` means auto-detect from host parallelism.
#[derive(Clone, Copy, Debug)]
pub struct AdmissionConfig {
    pub nthreads: usize,
    pub tiles_thread_ratio: f64,
    pub tiles_memory_ratio: f64,
    pub mode: AdmissionMode,
    pub max_waiting: usize,
    pub queue_timeout: Duration,
    pub large_decode_threshold_bytes: u64,
    /// The RAM envelope in bytes (0 = auto). Carried only for the fingerprint
    /// metric; the byte cap itself is enforced engine-side.
    pub memory_limit_bytes: u64,
}

/// A construction-time configuration error — the pool fails loud rather than
/// silently clamping a nonsensical ratio.
#[derive(Clone, Debug, PartialEq)]
pub enum AdmissionError {
    /// A ratio was not strictly inside (0, 1).
    Ratio { name: &'static str, value: f64 },
}

impl fmt::Display for AdmissionError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Ratio { name, value } => {
                write!(f, "{name} {value} out of range (0, 1)")
            }
        }
    }
}

impl std::error::Error for AdmissionError {}

/// The outcome of an [`Admission::acquire`] call.
pub enum Acquired {
    /// A permit was taken (immediately or after waiting); hold it for the dispatch.
    Admitted(Permit),
    /// The full-partition wait queue is at capacity (or disabled) — shed now.
    Shed,
    /// Parked for a permit but `queue_timeout` elapsed first.
    TimedOut,
}

/// RAII permit held for the duration of one engine dispatch. Dropping it releases
/// the global permit and (for a full) the full sub-pool permit.
pub struct Permit {
    _global: OwnedSemaphorePermit,
    _full: Option<OwnedSemaphorePermit>,
}

/// A point-in-time read of the pool's fixed sizing, live occupancy, per-partition
/// wait/shed counters, and the config fingerprint — the single source the OTLP
/// bridge (`server/rust/src/metrics.rs`) reads.
#[derive(Clone, Copy, Debug)]
pub struct AdmissionSnapshot {
    // Fixed sizing.
    pub permits_total: usize,
    pub tile_min: usize,
    pub full_max: usize,
    // Live occupancy.
    pub permits_in_use: usize,
    pub full_in_use: usize,
    pub tile_waiting: usize,
    pub full_waiting: usize,
    // Cumulative sheds (immediate queue-full + timeout), per partition.
    pub tile_shed_total: u64,
    pub full_shed_total: u64,
    // Basic-mode-only: fulls that the full cap *would* have rejected under
    // advanced but were admitted (the signal that sizes `full_max` before the
    // flip to advanced).
    pub full_shadow_rejected_total: u64,
    // Times the shell's pre-dispatch partition disagreed with the engine's precise
    // post-decode verdict — residual heuristic drift, observable but not fatal.
    pub classifier_disagreement_total: u64,
    // Config fingerprint.
    pub tiles_thread_ratio: f64,
    pub tiles_memory_ratio: f64,
    pub mode: AdmissionMode,
    pub large_decode_threshold_bytes: u64,
    pub memory_limit_bytes: u64,
}

/// Pool size when the configured `nthreads` is 0 (auto): the host parallelism,
/// falling back to 4 when unavailable.
#[must_use]
pub fn default_pool_size() -> usize {
    std::thread::available_parallelism().map_or(4, std::num::NonZeroUsize::get)
}

/// The two-lane admission pool. Built once at startup; shared (behind an `Arc`)
/// across all requests.
pub struct Admission {
    global: Arc<Semaphore>,
    full: Arc<Semaphore>,
    permits_total: usize,
    tile_min: usize,
    full_max: usize,
    max_waiting: usize,
    queue_timeout: Duration,
    large_decode_threshold_bytes: u64,
    // Config fingerprint (carried for the snapshot).
    tiles_thread_ratio: f64,
    tiles_memory_ratio: f64,
    mode: AdmissionMode,
    memory_limit_bytes: u64,
    // Per-partition wait/shed accounting — single writer, this type.
    tile_waiting: AtomicUsize,
    full_waiting: AtomicUsize,
    tile_shed_total: AtomicU64,
    full_shed_total: AtomicU64,
    full_shadow_rejected_total: AtomicU64,
    // Times the shell's pre-dispatch partition disagreed with the engine's precise
    // post-decode verdict (see `record_classification`) — the residual-drift signal.
    classifier_disagreement_total: AtomicU64,
}

impl Admission {
    /// Build the pool from plain config values. Fails loud on a ratio outside
    /// (0, 1). For `nthreads < 2`, lane separation is disabled: a single global
    /// permit, `tile_min = 0`, `full_max = nthreads` (the full sub-pool never
    /// binds); the engine's memory budget still applies.
    pub fn new(cfg: AdmissionConfig) -> Result<Self, AdmissionError> {
        if !(cfg.tiles_thread_ratio > 0.0 && cfg.tiles_thread_ratio < 1.0) {
            return Err(AdmissionError::Ratio {
                name: "tiles_thread_ratio",
                value: cfg.tiles_thread_ratio,
            });
        }
        if !(cfg.tiles_memory_ratio > 0.0 && cfg.tiles_memory_ratio < 1.0) {
            return Err(AdmissionError::Ratio {
                name: "tiles_memory_ratio",
                value: cfg.tiles_memory_ratio,
            });
        }

        let permits_total = if cfg.nthreads > 0 {
            cfg.nthreads
        } else {
            default_pool_size()
        };

        let (tile_min, full_max) = if permits_total < 2 {
            // Degenerate: one worker → no lane separation (full sub-pool never binds).
            (0, permits_total.max(1))
        } else {
            let raw = ((permits_total as f64) * cfg.tiles_thread_ratio).round() as usize;
            // Clamp so both partitions keep at least one thread.
            let tile_min = raw.clamp(1, permits_total - 1);
            let full_max = (permits_total - tile_min).max(1);
            (tile_min, full_max)
        };

        Ok(Self {
            global: Arc::new(Semaphore::new(permits_total)),
            full: Arc::new(Semaphore::new(full_max)),
            permits_total,
            tile_min,
            full_max,
            max_waiting: cfg.max_waiting,
            queue_timeout: cfg.queue_timeout,
            large_decode_threshold_bytes: cfg.large_decode_threshold_bytes,
            tiles_thread_ratio: cfg.tiles_thread_ratio,
            tiles_memory_ratio: cfg.tiles_memory_ratio,
            mode: cfg.mode,
            memory_limit_bytes: cfg.memory_limit_bytes,
            tile_waiting: AtomicUsize::new(0),
            full_waiting: AtomicUsize::new(0),
            tile_shed_total: AtomicU64::new(0),
            full_shed_total: AtomicU64::new(0),
            full_shadow_rejected_total: AtomicU64::new(0),
            classifier_disagreement_total: AtomicU64::new(0),
        })
    }

    #[must_use]
    pub fn mode(&self) -> AdmissionMode {
        self.mode
    }

    /// Classify a parsed request into its admission partition.
    ///
    /// Only a `RequestKind::Iiif` with a large enough estimated output is `Full`;
    /// metadata reads (`info.json`/`knora.json`), the raw `/file` byte stream, and
    /// redirects carry no decode and take the tile partition. Lua routes are not
    /// routed through here — their call site acquires `AdmissionKind::Tile`
    /// directly (a global permit only).
    #[must_use]
    pub fn classify(&self, parsed: &ParsedRequest) -> AdmissionKind {
        match parsed.kind {
            RequestKind::Iiif => match &parsed.params {
                Some(p) if self.is_full_image(p) => AdmissionKind::Full,
                _ => AdmissionKind::Tile,
            },
            RequestKind::InfoJson
            | RequestKind::KnoraJson
            | RequestKind::FileDownload
            | RequestKind::Redirect => AdmissionKind::Tile,
        }
    }

    /// The shell's coarse tile/full test over the IIIF params, tracking the
    /// engine's precise `estimate_peak_memory ≥ large_decode_threshold_bytes` via
    /// a pixel-count upper bound and the bytes-per-pixel proxy. When the output
    /// size cannot be bounded without the source dimensions (`full`/`max`, a
    /// percentage, or a single-dimension request with an extreme aspect), it errs
    /// toward `Full` — safe for RAM (the engine budget is the precise gate), at
    /// worst spending a full-partition thread on a cheap request.
    fn is_full_image(&self, p: &IiifParams) -> bool {
        match Self::output_pixel_upper_bound(p) {
            Some(pixels) => {
                pixels.saturating_mul(BYTES_PER_PIXEL_PROXY) >= self.large_decode_threshold_bytes
            }
            None => true,
        }
    }

    /// Upper-bound the output pixel count from the size params alone. `None` when
    /// it needs the source dimensions (`Full`/`Percents`/`Reduce`/`Undefined`).
    fn output_pixel_upper_bound(p: &IiifParams) -> Option<u64> {
        let nx = p.size_nx as u64;
        let ny = p.size_ny as u64;
        match p.size_kind {
            // Both dimensions given (or a bounding box): exact / upper bound.
            SizeKind::PixelsXy | SizeKind::Maxdim => Some(nx.saturating_mul(ny)),
            // One dimension given: square it as an aspect-agnostic proxy.
            SizeKind::PixelsX => Some(nx.saturating_mul(nx)),
            SizeKind::PixelsY => Some(ny.saturating_mul(ny)),
            SizeKind::Full | SizeKind::Percents | SizeKind::Reduce | SizeKind::Undefined => None,
        }
    }

    /// Record whether the shell's pre-dispatch partition matched the engine's
    /// precise post-decode verdict, bumping the disagreement counter when they
    /// differ. `engine_estimate_bytes` is the engine's decode-memory estimate for
    /// this serve (`SipiServeTimings::decode_estimate_bytes`); `0` means no decode
    /// ran (cache hit, HEAD, passthrough) — no engine verdict, nothing recorded.
    /// The engine verdict mirrors its own gate: `estimate ≥ threshold` → `Full`,
    /// so both sides read the single seam-sourced `large_decode_threshold_bytes`.
    pub fn record_classification(&self, shell_kind: AdmissionKind, engine_estimate_bytes: u64) {
        if engine_estimate_bytes == 0 {
            return;
        }
        let engine_kind = if engine_estimate_bytes >= self.large_decode_threshold_bytes {
            AdmissionKind::Full
        } else {
            AdmissionKind::Tile
        };
        if shell_kind != engine_kind {
            self.classifier_disagreement_total
                .fetch_add(1, Ordering::Relaxed);
        }
    }

    /// Acquire a permit for a request of the given partition, or shed it.
    pub async fn acquire(&self, kind: AdmissionKind) -> Acquired {
        match kind {
            AdmissionKind::Tile => self.acquire_tile().await,
            AdmissionKind::Full => self.acquire_full().await,
        }
    }

    async fn acquire_tile(&self) -> Acquired {
        // Fast path: a free global permit.
        if let Ok(g) = Arc::clone(&self.global).try_acquire_owned() {
            return Acquired::Admitted(Permit {
                _global: g,
                _full: None,
            });
        }
        // Tiles are exempt from the full-partition queue-depth shed: a tile only
        // ever waits for a global permit (never blocked by full-queue depth), up
        // to `queue_timeout`. Global permits held by fulls are bounded by
        // `full_max`, so a tile blocks only behind other tiles.
        let _w = WaitGuard::new(&self.tile_waiting);
        match tokio::time::timeout(self.queue_timeout, Arc::clone(&self.global).acquire_owned())
            .await
        {
            Ok(Ok(g)) => Acquired::Admitted(Permit {
                _global: g,
                _full: None,
            }),
            // The semaphore is never closed in normal operation; a closed pool
            // (only on teardown) sheds rather than panics.
            Ok(Err(_)) => Acquired::Shed,
            Err(_) => {
                self.tile_shed_total.fetch_add(1, Ordering::Relaxed);
                Acquired::TimedOut
            }
        }
    }

    async fn acquire_full(&self) -> Acquired {
        match self.mode {
            AdmissionMode::Advanced => self.acquire_full_advanced().await,
            AdmissionMode::Basic => self.acquire_full_basic().await,
        }
    }

    /// Advanced: the full cap binds. Acquire the full sub-pool permit FIRST, then
    /// the global permit.
    async fn acquire_full_advanced(&self) -> Acquired {
        // Fast path: full sub-pool permit then global, both immediately free.
        if let Ok(f) = Arc::clone(&self.full).try_acquire_owned() {
            if let Ok(g) = Arc::clone(&self.global).try_acquire_owned() {
                return Acquired::Admitted(Permit {
                    _global: g,
                    _full: Some(f),
                });
            }
            // Global busy: drop `f` (end of this block) and fall through to the
            // waiting path rather than pin a scarce full permit while blocked.
        }
        // Queue-depth shed applies to the full partition only.
        if self.max_waiting == 0 || self.full_waiting.load(Ordering::Relaxed) >= self.max_waiting {
            self.full_shed_total.fetch_add(1, Ordering::Relaxed);
            return Acquired::Shed;
        }
        let _w = WaitGuard::new(&self.full_waiting);
        // Full-lane-first ordering (correctness-critical): the full sub-pool
        // permit FIRST, then the global permit. Global-first would let queued
        // fulls hold global permits while blocked on the full sub-pool, starving
        // tiles. Full-first bounds fulls in the global FIFO to `≤ full_max`.
        let acquire = async {
            let f = Arc::clone(&self.full).acquire_owned().await.ok()?;
            let g = Arc::clone(&self.global).acquire_owned().await.ok()?;
            Some(Permit {
                _global: g,
                _full: Some(f),
            })
        };
        match tokio::time::timeout(self.queue_timeout, acquire).await {
            Ok(Some(p)) => Acquired::Admitted(p),
            Ok(None) => {
                self.full_shed_total.fetch_add(1, Ordering::Relaxed);
                Acquired::Shed
            }
            Err(_) => {
                self.full_shed_total.fetch_add(1, Ordering::Relaxed);
                Acquired::TimedOut
            }
        }
    }

    /// Basic: the advanced (memory/full-lane) cap is observe-only. The full cap
    /// never rejects — the default `basic` mode must not change behavior beyond
    /// the basic thread cap. A full takes a full sub-pool permit when one is free
    /// (so `full_in_use` tracks up to `full_max`); otherwise it shadow-counts the
    /// would-be full-cap rejection and proceeds with a global permit only. The
    /// basic tier — the global concurrency bound — still applies, as it did
    /// before two-lane admission existed.
    async fn acquire_full_basic(&self) -> Acquired {
        let full_permit = Arc::clone(&self.full).try_acquire_owned().ok();
        if full_permit.is_none() {
            self.full_shadow_rejected_total
                .fetch_add(1, Ordering::Relaxed);
        }
        if let Ok(g) = Arc::clone(&self.global).try_acquire_owned() {
            return Acquired::Admitted(Permit {
                _global: g,
                _full: full_permit,
            });
        }
        // Global saturated: the pre-existing queue-depth shed still applies.
        if self.max_waiting == 0 || self.full_waiting.load(Ordering::Relaxed) >= self.max_waiting {
            self.full_shed_total.fetch_add(1, Ordering::Relaxed);
            return Acquired::Shed;
        }
        let _w = WaitGuard::new(&self.full_waiting);
        match tokio::time::timeout(self.queue_timeout, Arc::clone(&self.global).acquire_owned())
            .await
        {
            Ok(Ok(g)) => Acquired::Admitted(Permit {
                _global: g,
                _full: full_permit,
            }),
            Ok(Err(_)) => {
                self.full_shed_total.fetch_add(1, Ordering::Relaxed);
                Acquired::Shed
            }
            Err(_) => {
                self.full_shed_total.fetch_add(1, Ordering::Relaxed);
                Acquired::TimedOut
            }
        }
    }

    /// Snapshot the pool's sizing, live occupancy, counters, and fingerprint.
    #[must_use]
    pub fn snapshot(&self) -> AdmissionSnapshot {
        AdmissionSnapshot {
            permits_total: self.permits_total,
            tile_min: self.tile_min,
            full_max: self.full_max,
            permits_in_use: self
                .permits_total
                .saturating_sub(self.global.available_permits()),
            full_in_use: self.full_max.saturating_sub(self.full.available_permits()),
            tile_waiting: self.tile_waiting.load(Ordering::Relaxed),
            full_waiting: self.full_waiting.load(Ordering::Relaxed),
            tile_shed_total: self.tile_shed_total.load(Ordering::Relaxed),
            full_shed_total: self.full_shed_total.load(Ordering::Relaxed),
            full_shadow_rejected_total: self.full_shadow_rejected_total.load(Ordering::Relaxed),
            classifier_disagreement_total: self
                .classifier_disagreement_total
                .load(Ordering::Relaxed),
            tiles_thread_ratio: self.tiles_thread_ratio,
            tiles_memory_ratio: self.tiles_memory_ratio,
            mode: self.mode,
            large_decode_threshold_bytes: self.large_decode_threshold_bytes,
            memory_limit_bytes: self.memory_limit_bytes,
        }
    }
}

/// RAII bump of a per-partition waiting gauge: incremented while a request is
/// parked for a permit, decremented on every exit (acquired, timeout, or panic).
struct WaitGuard<'a>(&'a AtomicUsize);

impl<'a> WaitGuard<'a> {
    fn new(counter: &'a AtomicUsize) -> Self {
        counter.fetch_add(1, Ordering::Relaxed);
        WaitGuard(counter)
    }
}

impl Drop for WaitGuard<'_> {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::Relaxed);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use iiif_parser::{FormatKind, QualityKind, RegionKind};

    fn cfg(
        nthreads: usize,
        tiles_thread_ratio: f64,
        max_waiting: usize,
        timeout_ms: u64,
    ) -> AdmissionConfig {
        AdmissionConfig {
            nthreads,
            tiles_thread_ratio,
            tiles_memory_ratio: 0.25,
            mode: AdmissionMode::Advanced,
            max_waiting,
            queue_timeout: Duration::from_millis(timeout_ms),
            large_decode_threshold_bytes: 32 * 1024 * 1024,
            memory_limit_bytes: 0,
        }
    }

    fn rt() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_current_thread()
            .enable_time()
            .build()
            .unwrap()
    }

    fn admitted(a: Acquired) -> Permit {
        match a {
            Acquired::Admitted(p) => p,
            Acquired::Shed => panic!("expected Admitted, got Shed"),
            Acquired::TimedOut => panic!("expected Admitted, got TimedOut"),
        }
    }

    // ── Construction / sizing ────────────────────────────────────────────────

    #[test]
    fn derives_tile_floor_and_full_cap_from_ratio() {
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        let s = a.snapshot();
        assert_eq!(s.permits_total, 16);
        assert_eq!(s.tile_min, 8);
        assert_eq!(s.full_max, 8);
    }

    #[test]
    fn degenerate_ratios_fail_loud() {
        for bad in [0.0, 1.0, -0.1, 1.5, f64::NAN] {
            let mut c = cfg(16, bad, 0, 50);
            assert!(
                matches!(Admission::new(c), Err(AdmissionError::Ratio { .. })),
                "thread ratio {bad}"
            );
            c = cfg(16, 0.5, 0, 50);
            c.tiles_memory_ratio = bad;
            assert!(
                matches!(Admission::new(c), Err(AdmissionError::Ratio { .. })),
                "memory ratio {bad}"
            );
        }
    }

    #[test]
    fn single_thread_disables_lane_separation() {
        let a = Admission::new(cfg(1, 0.5, 0, 50)).unwrap();
        let s = a.snapshot();
        assert_eq!(s.permits_total, 1);
        assert_eq!(s.tile_min, 0);
        assert_eq!(s.full_max, 1);
    }

    #[test]
    fn clamps_keep_both_partitions_nonempty() {
        // An extreme ratio must still leave tile_min >= 1 and full_max >= 1.
        let a = Admission::new(cfg(4, 0.99, 0, 50)).unwrap();
        let s = a.snapshot();
        assert!(
            s.tile_min >= 1 && s.full_max >= 1,
            "tile_min {} full_max {}",
            s.tile_min,
            s.full_max
        );
        assert_eq!(s.tile_min + s.full_max, 4);
    }

    // ── Burst / floor behaviour ──────────────────────────────────────────────

    #[test]
    fn ninth_tile_bursts_while_full_idle() {
        // tile_min = 8, but with the full partition idle a tile bursts into the
        // whole pool: 9 (indeed 16) concurrent tiles are admitted.
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        rt().block_on(async {
            let mut held = Vec::new();
            for i in 0..16 {
                held.push(admitted(a.acquire(AdmissionKind::Tile).await));
                assert!(held.len() == i + 1);
            }
            // The 9th (and every tile up to 16) was admitted while full sat idle.
            assert_eq!(held.len(), 16);
        });
    }

    #[test]
    fn full_partition_is_hard_capped_even_when_tiles_idle() {
        // full_max = 8: an 8-deep full burst saturates the sub-pool; the 9th full
        // sheds immediately (queue disabled) even though 8 global permits are free.
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        rt().block_on(async {
            let mut held = Vec::new();
            for _ in 0..8 {
                held.push(admitted(a.acquire(AdmissionKind::Full).await));
            }
            assert!(matches!(
                a.acquire(AdmissionKind::Full).await,
                Acquired::Shed
            ));
            // ...yet a tile still flows: >= tile_min global permits are free of fulls.
            let _t = admitted(a.acquire(AdmissionKind::Tile).await);
            let s = a.snapshot();
            assert_eq!(s.full_in_use, 8);
        });
    }

    #[test]
    fn basic_mode_never_caps_fulls_but_shadow_counts() {
        // Advanced tier observe-only: with the full cap at 8, a 12-deep full
        // burst is admitted (global has 16 permits), and the 4 over the cap are
        // shadow-counted.
        let mut c = cfg(16, 0.5, 0, 50);
        c.mode = AdmissionMode::Basic;
        let a = Admission::new(c).unwrap();
        rt().block_on(async {
            let mut held = Vec::new();
            for _ in 0..12 {
                held.push(admitted(a.acquire(AdmissionKind::Full).await));
            }
            let s = a.snapshot();
            assert_eq!(held.len(), 12, "basic mode must admit past the cap");
            assert_eq!(s.full_in_use, 8, "full_in_use tracks up to full_max");
            assert_eq!(
                s.full_shadow_rejected_total, 4,
                "over-cap fulls shadow-counted"
            );
        });
    }

    #[test]
    fn starvation_inversion_tiles_stay_flat_under_full_saturation() {
        // Fulls saturate their sub-pool AND hold their global permits; tiles must
        // still be admitted up to the tile floor.
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        rt().block_on(async {
            let mut fulls = Vec::new();
            for _ in 0..8 {
                fulls.push(admitted(a.acquire(AdmissionKind::Full).await));
            }
            // 8 globals held by fulls → tile_min = 8 still free for tiles.
            let mut tiles = Vec::new();
            for _ in 0..8 {
                tiles.push(admitted(a.acquire(AdmissionKind::Tile).await));
            }
            assert_eq!(tiles.len(), 8);
        });
    }

    #[test]
    fn tile_times_out_when_pool_full_of_tiles() {
        // A tile sheds (times out) only when no global permit is genuinely free.
        let a = Admission::new(cfg(2, 0.5, 0, 20)).unwrap();
        rt().block_on(async {
            let _h1 = admitted(a.acquire(AdmissionKind::Tile).await);
            let _h2 = admitted(a.acquire(AdmissionKind::Tile).await);
            assert!(matches!(
                a.acquire(AdmissionKind::Tile).await,
                Acquired::TimedOut
            ));
            assert_eq!(a.snapshot().tile_shed_total, 1);
        });
    }

    // ── Queue segregation: a parked full burst never sheds tiles ──────────────

    #[test]
    fn tiles_exempt_from_full_queue_depth_shed() {
        let a = Arc::new(Admission::new(cfg(4, 0.5, 8, 500)).unwrap()); // tile_min=2, full_max=2
        let mrt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .unwrap();
        mrt.block_on(async {
            // Occupy both full sub-pool permits (2 globals held, 2 free).
            let _f1 = admitted(a.acquire(AdmissionKind::Full).await);
            let _f2 = admitted(a.acquire(AdmissionKind::Full).await);
            // Park two more fulls: they queue on the full sub-pool (full_waiting → 2).
            for _ in 0..2 {
                let a2 = Arc::clone(&a);
                tokio::spawn(async move { a2.acquire(AdmissionKind::Full).await });
            }
            // Wait until both parked fulls have registered as waiting.
            for _ in 0..200 {
                if a.snapshot().full_waiting == 2 {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
            assert_eq!(
                a.snapshot().full_waiting,
                2,
                "parked fulls did not register"
            );
            // A tile with 2 free global permits is admitted immediately — never
            // shed because the full queue is full.
            let _t = admitted(a.acquire(AdmissionKind::Tile).await);
        });
    }

    // ── Classification ───────────────────────────────────────────────────────

    fn parsed(kind: RequestKind, params: Option<IiifParams>) -> ParsedRequest {
        ParsedRequest {
            kind,
            prefix: "prefix".to_string(),
            identifier: "id".to_string(),
            params,
        }
    }

    fn img(size_kind: SizeKind, nx: usize, ny: usize) -> IiifParams {
        IiifParams {
            region_kind: RegionKind::Full,
            region: [0.0; 4],
            size_kind,
            size_upscaling: false,
            size_percent: 0.0,
            size_reduce: 0,
            size_nx: nx,
            size_ny: ny,
            rotation: 0.0,
            rotation_mirror: false,
            quality_kind: QualityKind::Default,
            format_kind: FormatKind::Jpg,
        }
    }

    #[test]
    fn classifies_small_explicit_size_as_tile() {
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        // 256×256 × 4 B = 256 KiB < 32 MiB threshold → tile.
        let p = parsed(RequestKind::Iiif, Some(img(SizeKind::PixelsXy, 256, 256)));
        assert_eq!(a.classify(&p), AdmissionKind::Tile);
    }

    #[test]
    fn classifies_large_explicit_size_as_full() {
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        // 8000×8000 × 4 B ≈ 244 MiB > 32 MiB → full.
        let p = parsed(RequestKind::Iiif, Some(img(SizeKind::PixelsXy, 8000, 8000)));
        assert_eq!(a.classify(&p), AdmissionKind::Full);
    }

    #[test]
    fn classifies_full_size_conservatively_as_full() {
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        // `/full/max/` cannot be bounded without source dims → conservative full.
        let p = parsed(RequestKind::Iiif, Some(img(SizeKind::Full, 0, 0)));
        assert_eq!(a.classify(&p), AdmissionKind::Full);
    }

    #[test]
    fn classifies_metadata_and_file_as_tile() {
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        for kind in [
            RequestKind::InfoJson,
            RequestKind::KnoraJson,
            RequestKind::FileDownload,
            RequestKind::Redirect,
        ] {
            assert_eq!(
                a.classify(&parsed(kind, None)),
                AdmissionKind::Tile,
                "{kind:?}"
            );
        }
    }

    #[test]
    fn mode_parses_and_rejects_unknown() {
        assert_eq!(AdmissionMode::parse("basic"), Some(AdmissionMode::Basic));
        assert_eq!(
            AdmissionMode::parse("ADVANCED"),
            Some(AdmissionMode::Advanced)
        );
        // Legacy values and "off" are no longer recognized (caller defaults to basic).
        assert_eq!(AdmissionMode::parse("monitor"), None);
        assert_eq!(AdmissionMode::parse("enforce"), None);
        assert_eq!(AdmissionMode::parse("off"), None);
        assert_eq!(AdmissionMode::parse(""), None);
    }

    #[test]
    fn classifier_disagreement_counts_only_on_verdict_mismatch() {
        let a = Admission::new(cfg(16, 0.5, 0, 50)).unwrap();
        let threshold = a.snapshot().large_decode_threshold_bytes;

        // A zero estimate (cache hit / HEAD / passthrough) carries no engine
        // verdict — never recorded, whatever the shell said.
        a.record_classification(AdmissionKind::Full, 0);
        a.record_classification(AdmissionKind::Tile, 0);
        assert_eq!(a.snapshot().classifier_disagreement_total, 0);

        // Agreement: shell Full ↔ estimate ≥ threshold; shell Tile ↔ estimate <.
        a.record_classification(AdmissionKind::Full, threshold);
        a.record_classification(AdmissionKind::Tile, threshold - 1);
        assert_eq!(a.snapshot().classifier_disagreement_total, 0);

        // Disagreement in both directions.
        a.record_classification(AdmissionKind::Tile, threshold); // engine Full
        a.record_classification(AdmissionKind::Full, threshold - 1); // engine Tile
        assert_eq!(a.snapshot().classifier_disagreement_total, 2);
    }
}
