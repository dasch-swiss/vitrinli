//! Concurrency flags (the "Concurrency" `--help` heading).
//!
//! All are Rust-owned serve knobs handed to `sipi::run`; the Lua config does not
//! set them. See `server/rust/routes.rs`'s `AppState::load` and the `Admission`
//! classify/acquire call sites (`//src/throttling/rust:admission`), and
//! `server/rust/preflight_cache.rs`.

use clap::Args;

#[derive(Args, Debug)]
#[command(next_help_heading = "Concurrency")]
pub struct ConcurrencyArgs {
    /// Worker thread count that sizes the engine-work pool (0 = auto-detect from
    /// CPU cores). Also accepts the `-t` short form.
    #[arg(long, short = 't', env = "SIPI_NTHREADS", value_name = "N")]
    pub nthreads: Option<u32>,
    /// Fraction of the worker threads guaranteed to tiles (0..1); the full
    /// partition is hard-capped at the rest. Defaults to 0.5.
    #[arg(long, env = "SIPI_TILES_THREAD_RATIO", value_name = "RATIO")]
    pub tiles_thread_ratio: Option<f64>,
    /// Max requests queued for a worker before 503 (0 = no queue, shed
    /// immediately; default 2×nthreads).
    #[arg(long, env = "SIPI_MAX_WAITING", value_name = "N")]
    pub max_waiting: Option<u64>,
    /// Max seconds a queued request waits for a worker before 503 (default 5).
    #[arg(long, env = "SIPI_QUEUE_TIMEOUT", value_name = "SECS")]
    pub queue_timeout: Option<u32>,
    /// Seconds a `pre_flight` access decision is cached per (image, credential),
    /// coalescing repeat auth calls for one image (default 0 = disabled; set >0,
    /// e.g. 2, only if the hook decides purely on prefix/identifier/Cookie/Authorization).
    #[arg(long, env = "SIPI_PREFLIGHT_CACHE_TTL", value_name = "SECS")]
    pub preflight_cache_ttl: Option<u32>,
    /// Slot count for the preflight access-cache (default 4096).
    #[arg(long, env = "SIPI_PREFLIGHT_CACHE_SLOTS", value_name = "N")]
    pub preflight_cache_slots: Option<u64>,
    /// Wedged decode threads (decodes abandoned at the decode deadline) at
    /// which every request is shed with 503 and /health fails until restart
    /// (0 = default: nthreads - 1, at least 1).
    #[arg(long, env = "SIPI_MAX_WEDGED_THREADS", value_name = "N")]
    pub max_wedged_threads: Option<u32>,
}
