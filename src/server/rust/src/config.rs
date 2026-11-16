//! CLI/env override surface for the `server` verb.
//!
//! [`ServerOverrides`] is the Rust-native bag of `server`-verb flags that layer
//! over the loaded Lua config. `cli/rust` builds it from the parsed clap args and
//! hands it to [`crate::run`]; the library never parses argv itself
//! (the library is reusable, the binary owns the CLI).
//!
//! It carries one `Option` per forwarded `server` flag; `None` means the flag
//! was set by neither CLI nor env, so the loaded Lua config value wins (the
//! Rust analog of the C++ `user_set` gate; precedence `config < env < CLI`).
//! [`OverridesHolder`] converts it to the `#[repr(C)]` [`SipiServerConfig`] and
//! forwards it through `sipi_init`, which layers the present overrides onto the
//! parsed Lua config before the engine builds its services. The `#[repr(C)]`
//! layout is lock-step with `sipi_ffi.h`'s `SipiServerConfig` — guarded by the
//! `layout` test below paired with the header's `static_assert`s (not bindgen).
//!
//! `--drain-timeout` is deliberately *not* here: it is a Rust-owned serve knob,
//! not a config override, so it stays a direct [`crate::run`] argument.

use std::ffi::{CString, NulError};
use std::os::raw::{c_char, c_int};

/// The single, shell-side default for the large-decode classifier threshold
/// (DUNE-003): a decode whose estimated peak memory reaches this is a full-lane
/// decode (charged against the budget); below it is a tile decode that bypasses
/// the budget. 32 MiB. Tunable via `--large-decode-threshold-bytes` / the TOML
/// `[limits]` key / `SIPI_LARGE_DECODE_THRESHOLD_BYTES`. The engine only ever
/// reads it from the seam, so the shell and engine classifiers cannot drift.
pub(crate) const DEFAULT_LARGE_DECODE_THRESHOLD_BYTES: u64 = 32 * 1024 * 1024;

/// Parses `SIPI_ALLOWED_ORIGINS` — a comma-separated list of exact origins
/// (e.g. `https://a.example,https://b.example`) — into the CORS allowlist
/// `routes::cors_allow` reads. Entries are trimmed; empty entries are dropped.
///
/// Read directly via `std::env` (like `SIPI_RS_PORT` in `lib.rs`) rather than
/// through the `ServerOverrides`/FFI channel: CORS is a Rust-shell-only
/// concern and never crosses the FFI seam into the engine, unlike the
/// codec/limit overrides above.
///
/// Unset or empty is the "no restriction configured" sentinel: `cors_allow`
/// then reflects any origin except the opaque `null`. Setting the var switches an instance into
/// allowlist mode.
pub fn allowed_origins_from_env() -> Vec<String> {
    parse_csv_list(std::env::var("SIPI_ALLOWED_ORIGINS").ok().as_deref())
}

/// Parses `SIPI_PUBLIC_HOSTS` — a comma-separated list of exact hosts (e.g.
/// `iiif.example.org,iiif2.example.org`) — into the host allowlist
/// `routes::forwarded` reads.  Entries are trimmed; empty entries are dropped.
///
/// Read directly via `std::env`, like [`allowed_origins_from_env`]: the
/// `X-Forwarded-Host`/`Host` validation is a Rust-shell-only concern and never
/// crosses the FFI seam (`config.hostname` is Lua-config-only and is not the
/// validation source here).
///
/// Unset or empty is the "no restriction configured" sentinel: `forwarded`
/// then returns any host verbatim. Setting the var switches an instance into
/// allowlist mode: a header host on the list passes through unchanged, any
/// other host is replaced by the first allowlisted host.
pub fn public_hosts_from_env() -> Vec<String> {
    parse_csv_list(std::env::var("SIPI_PUBLIC_HOSTS").ok().as_deref())
}

/// The handler wall-clock timeout default.
pub const DEFAULT_REQUEST_TIMEOUT_SECS: u64 = 60;

/// Parses `SIPI_REQUEST_TIMEOUT` — an integer number of seconds — into the
/// [`std::time::Duration`] `lib.rs` hands to `tower_http::timeout::TimeoutLayer`.
///
/// Read directly via `std::env`, like [`allowed_origins_from_env`] /
/// [`public_hosts_from_env`]: the handler timeout is a Rust-shell-only serve
/// knob and never crosses the FFI seam.
///
/// Unset, unparseable, or `0` all fall back to [`DEFAULT_REQUEST_TIMEOUT_SECS`]:
/// a `0`-second timeout would answer every request with 408, which is never the
/// intent of an accidental empty or zero value.
pub fn request_timeout_from_env() -> std::time::Duration {
    let secs = std::env::var("SIPI_REQUEST_TIMEOUT")
        .ok()
        .and_then(|v| v.parse::<u64>().ok())
        .filter(|&secs| secs != 0)
        .unwrap_or(DEFAULT_REQUEST_TIMEOUT_SECS);
    std::time::Duration::from_secs(secs)
}

/// The server's decode deadline in ms: `set` (the merged CLI/env/TOML value)
/// when given, else 80 % of `request`. It must stay shorter than the request
/// timeout, so a decode that starts promptly ends with the engine's deadline
/// error and permit release before the shell answers 408 (queue wait and a
/// shape probe come out of the same margin); a set value at or above it, or
/// zero, refuses startup.
pub fn resolve_decode_timeout(
    set: Option<u64>,
    request: std::time::Duration,
) -> Result<u64, String> {
    let request_ms = request.as_millis();
    match set {
        None => Ok(u64::try_from(request_ms * 4 / 5).unwrap_or(u64::MAX)),
        Some(0) => Err("decode_timeout_ms must be positive".to_owned()),
        Some(ms) if u128::from(ms) >= request_ms => Err(format!(
            "decode_timeout_ms ({ms} ms) must be shorter than the request timeout \
             (SIPI_REQUEST_TIMEOUT, {request_ms} ms)"
        )),
        Some(ms) => Ok(ms),
    }
}

/// The Lua-route body-admission size default when the Lua config leaves
/// `max_post_size` unset (`0`). `0` does not mean unlimited: every Lua-route
/// body cap is finite, so no unbounded request body is read into RAM before
/// admission.
pub const DEFAULT_MAX_POST_SIZE: usize = 256 * 1024 * 1024;

/// The Lua-route body-read timeout default.
pub const DEFAULT_BODY_READ_TIMEOUT_SECS: u64 = 10;

/// Parses `SIPI_BODY_READ_TIMEOUT` — an integer number of seconds — into the
/// [`std::time::Duration`] `routes.rs` hands to
/// `tower_http::timeout::RequestBodyTimeoutLayer` on the Lua-route and docroot
/// method routers.
///
/// Read directly via `std::env`, like [`request_timeout_from_env`]: the
/// body-read timeout is a Rust-shell-only serve knob and never crosses the FFI
/// seam.
///
/// Unset, unparseable, or `0` all fall back to
/// [`DEFAULT_BODY_READ_TIMEOUT_SECS`]: a `0`-second timeout would fail every
/// request body read, which is never the intent of an accidental empty or zero
/// value. Bounding the body read by time (in addition to
/// [`DEFAULT_MAX_POST_SIZE`]'s bound by size) means a slow/trickling client
/// cannot hold a body-read connection open indefinitely before an admission
/// permit is even acquired.
pub fn body_read_timeout_from_env() -> std::time::Duration {
    let secs = std::env::var("SIPI_BODY_READ_TIMEOUT")
        .ok()
        .and_then(|v| v.parse::<u64>().ok())
        .filter(|&secs| secs != 0)
        .unwrap_or(DEFAULT_BODY_READ_TIMEOUT_SECS);
    std::time::Duration::from_secs(secs)
}

/// Pure parse, split out from [`allowed_origins_from_env`] / [`public_hosts_from_env`]
/// so it is testable without mutating process-global env state.
fn parse_csv_list(raw: Option<&str>) -> Vec<String> {
    raw.map(|s| {
        s.split(',')
            .map(str::trim)
            .filter(|o| !o.is_empty())
            .map(str::to_owned)
            .collect()
    })
    .unwrap_or_default()
}

/// The Rust-owned serve knobs, each `None` when unset;
/// [`crate::routes::AppState::load`] resolves the defaults. Pool sizing:
/// `--nthreads`, `--tiles-thread-ratio`, `--max-waiting`, `--queue-timeout`
/// (the worker count, the tiles' share, how many requests may queue for a
/// worker and how long each waits before the shell sheds with 503). The
/// preflight access-cache: `--preflight-cache-ttl` (seconds; default 0 =
/// disabled, the cache is opt-in) and `--preflight-cache-slots`. The shed cap
/// `--max-wedged-threads` (default `nthreads - 1`, at least 1).
#[derive(Clone, Copy, Debug, Default)]
pub struct ServeKnobs {
    pub nthreads: Option<u32>,
    pub tiles_thread_ratio: Option<f64>,
    pub max_waiting: Option<u64>,
    pub queue_timeout: Option<u32>,
    pub preflight_cache_ttl: Option<u32>,
    pub preflight_cache_slots: Option<u64>,
    pub max_wedged_threads: Option<u32>,
}

/// CLI/env flag overrides layered over the loaded Lua config — one `Option` per
/// forwarded `server` flag (`None` = neither CLI nor env set it → the Lua config
/// wins).
///
/// Only engine-behaviour flags are forwarded from the CLI. Transport flags the
/// Rust shell owns (`--sslport`/`--sslcert`/`--sslkey`, `--keepalive`,
/// `--hostname`) are accepted for CLI compatibility but never forwarded from
/// clap; the `hostname`/`sslport` fields below are populated only by the Lua
/// config parse, because the Lua-built `config` table exposes them to scripts.
/// `--max-waiting`/`--queue-timeout` are also absent here, but for the opposite
/// reason: the shell honors them as Rust-owned serve knobs ([`ServeKnobs`])
/// passed straight to [`crate::run`], not layered onto the engine config.
///
/// `Debug` is implemented manually: `jwtkey` is a secret and
/// renders as `[redacted]`, so no `{:?}` of this struct (logs, Sentry
/// breadcrumbs, panic messages) can leak it.
#[derive(Default, Clone)]
pub struct ServerOverrides {
    /// HTTP listen port (`--serverport` / `SIPI_SERVERPORT`). One input to the
    /// listen-port resolution in `serve()` (`lib.rs`), which is the single
    /// authority on precedence — note `SIPI_RS_PORT` is checked *ahead* of this
    /// field, not after it.
    pub serverport: Option<u16>,

    // Paths
    pub imgroot: Option<String>,
    pub scriptdir: Option<String>,
    pub initscript: Option<String>,
    pub tmpdir: Option<String>,
    pub maxtmpage: Option<i32>,
    pub docroot: Option<String>,
    pub wwwroute: Option<String>,
    pub pathprefix: Option<bool>,

    // Auth (TLS terminates at Traefik; only the auth knobs forward)
    pub jwtkey: Option<String>,

    // Cache
    pub cache_dir: Option<String>,
    /// Raw size string ("200M"); the engine parses the suffix.
    pub cache_size: Option<String>,
    /// 0 = unlimited; a negative is rejected at the CLI (clap `u32` + the C++
    /// `unsigned` var), so there is no signed→unsigned wrap.
    pub cache_nfiles: Option<u32>,

    // Limits / admission
    /// Total RAM envelope as a raw size string ("8G"); the engine parses the
    /// suffix. "0"/absent = auto-detect available RAM.
    pub memory_limit: Option<String>,
    /// Admission mode: "basic" | "advanced".
    pub admission_mode: Option<String>,
    /// Fraction of the envelope reserved for tiles; the full lane gets
    /// envelope × (1 − ratio). Engine-consumed (full-lane byte cap).
    pub tiles_memory_ratio: Option<f64>,
    /// Estimated peak-memory threshold (bytes) at/above which a decode is a
    /// full-lane decode charged against the budget; below it is a tile decode.
    pub large_decode_threshold_bytes: Option<u64>,
    /// Wall-clock decode deadline in ms. `server_main` replaces it with the
    /// resolved value ([`resolve_decode_timeout`]) before the engine init.
    pub decode_timeout_ms: Option<u64>,
    /// Raw size string ("300M"); the engine parses the suffix.
    pub maxpost: Option<String>,
    pub thumbsize: Option<String>,

    // Knora
    pub knorapath: Option<String>,
    pub knoraport: Option<String>,

    // Logging
    pub loglevel: Option<String>,

    // Image quality — TOML-config-only (no CLI flag).
    pub jpeg_quality: Option<i32>,
    pub scaling_quality: ScalingQuality,

    // Lua-config-only (never set from CLI/env): no engine behavior of their
    // own — they feed the SipiConf getters the Lua `config` table exposes to
    // scripts (`config.hostname` / `config.sslport`).
    pub hostname: Option<String>,
    pub sslport: Option<i32>,
}

impl std::fmt::Debug for ServerOverrides {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        fn redact(s: &Option<String>) -> &'static str {
            if s.is_some() {
                "Some([redacted])"
            } else {
                "None"
            }
        }
        f.debug_struct("ServerOverrides")
            .field("serverport", &self.serverport)
            .field("imgroot", &self.imgroot)
            .field("scriptdir", &self.scriptdir)
            .field("initscript", &self.initscript)
            .field("tmpdir", &self.tmpdir)
            .field("maxtmpage", &self.maxtmpage)
            .field("docroot", &self.docroot)
            .field("wwwroute", &self.wwwroute)
            .field("pathprefix", &self.pathprefix)
            .field("jwtkey", &redact(&self.jwtkey))
            .field("cache_dir", &self.cache_dir)
            .field("cache_size", &self.cache_size)
            .field("cache_nfiles", &self.cache_nfiles)
            .field("memory_limit", &self.memory_limit)
            .field("admission_mode", &self.admission_mode)
            .field("tiles_memory_ratio", &self.tiles_memory_ratio)
            .field(
                "large_decode_threshold_bytes",
                &self.large_decode_threshold_bytes,
            )
            .field("decode_timeout_ms", &self.decode_timeout_ms)
            .field("maxpost", &self.maxpost)
            .field("thumbsize", &self.thumbsize)
            .field("knorapath", &self.knorapath)
            .field("knoraport", &self.knoraport)
            .field("loglevel", &self.loglevel)
            .field("jpeg_quality", &self.jpeg_quality)
            .field("scaling_quality", &self.scaling_quality)
            .field("hostname", &self.hostname)
            .field("sslport", &self.sslport)
            .finish()
    }
}

impl ServerOverrides {
    /// The parsed Lua config as an overrides base (before the CLI/env layer) —
    /// every field the config schema resolves arrives `Some` (the parse already
    /// applied the schema defaults), except the shell-owned admission knobs and
    /// `loglevel`, which the Lua config never carried.
    pub fn from_lua_config(cfg: &scripting::LuaConfigFile) -> Result<Self, String> {
        fn narrow<T: TryFrom<i64>>(value: i64, key: &str) -> Result<T, String> {
            T::try_from(value).map_err(|_| format!("{key} value {value} out of range"))
        }
        Ok(ServerOverrides {
            serverport: Some(narrow(cfg.port, "sipi.port")?),
            imgroot: Some(cfg.img_root.clone()),
            scriptdir: Some(cfg.script_dir.clone()),
            initscript: Some(cfg.init_script.clone()),
            tmpdir: Some(cfg.tmp_dir.clone()),
            maxtmpage: Some(narrow(cfg.max_temp_file_age, "sipi.max_temp_file_age")?),
            docroot: Some(cfg.docroot.clone()),
            wwwroute: Some(cfg.wwwroute.clone()),
            pathprefix: Some(cfg.prefix_as_path),
            jwtkey: Some(cfg.jwt_secret.clone()),
            cache_dir: Some(cfg.cache_dir.clone()),
            cache_size: Some(cfg.cache_size.clone()),
            cache_nfiles: Some(narrow(cfg.cache_nfiles, "sipi.cache_nfiles")?),
            // Shell-owned admission knobs: never read from the Lua config
            // (CLI/env or TOML only).
            memory_limit: None,
            admission_mode: None,
            tiles_memory_ratio: None,
            large_decode_threshold_bytes: None,
            decode_timeout_ms: None,
            maxpost: Some(cfg.max_post_size.clone()),
            thumbsize: Some(cfg.thumb_size.clone()),
            knorapath: Some(cfg.knora_path.clone()),
            knoraport: Some(cfg.knora_port.clone()),
            // The Lua config schema has no log-level key.
            loglevel: None,
            jpeg_quality: Some(narrow(cfg.jpeg_quality, "sipi.jpeg_quality")?),
            scaling_quality: ScalingQuality {
                jpeg: cfg.scaling_quality.jpeg.clone(),
                tiff: cfg.scaling_quality.tiff.clone(),
                png: cfg.scaling_quality.png.clone(),
                j2k: cfg.scaling_quality.j2k.clone(),
            },
            hostname: Some(cfg.hostname.clone()),
            sslport: Some(narrow(cfg.ssl_port, "sipi.ssl_port")?),
        })
    }
}

/// Per-codec scaling-quality overrides ("high"|"medium"|"low"); `None` = engine
/// default. TOML-config-only — there is no CLI flag for these.
///
/// The engine reads the j2k slot under a legacy `"jpk"` map key (a pre-existing
/// quirk), so `j2k` currently has no effect on either the Lua or TOML path; it is
/// kept for schema completeness and alignment with the Lua `scaling_quality` table.
#[derive(Debug, Default, Clone)]
pub struct ScalingQuality {
    pub jpeg: Option<String>,
    pub tiff: Option<String>,
    pub png: Option<String>,
    pub j2k: Option<String>,
}

impl ServerOverrides {
    /// Layer `self` (higher precedence — CLI/env) over `base` (lower — a TOML
    /// config): per field, keep `self`'s value when set, else fall back to
    /// `base`. With clap's own `CLI > env`, this realises `config < env < CLI`
    /// with a TOML base, matching the Lua-config precedence.
    ///
    /// Fail-on-omission (DUNE-006): the returned struct literal names every field
    /// with no `..` rest, and the output type IS `ServerOverrides`, so a new
    /// field fails to compile here until it is explicitly merged
    /// (`self.new.or(base.new)`). No separate destructure of `self`/`base` is
    /// needed — the exhaustive literal already forces reading both sides (unlike
    /// `From<&ServerArgs>` / `OverridesHolder::new`, whose outputs are different
    /// structs and so destructure their source).
    #[must_use]
    pub fn layered_over(self, base: ServerOverrides) -> ServerOverrides {
        ServerOverrides {
            serverport: self.serverport.or(base.serverport),
            imgroot: self.imgroot.or(base.imgroot),
            scriptdir: self.scriptdir.or(base.scriptdir),
            initscript: self.initscript.or(base.initscript),
            tmpdir: self.tmpdir.or(base.tmpdir),
            maxtmpage: self.maxtmpage.or(base.maxtmpage),
            docroot: self.docroot.or(base.docroot),
            wwwroute: self.wwwroute.or(base.wwwroute),
            pathprefix: self.pathprefix.or(base.pathprefix),
            jwtkey: self.jwtkey.or(base.jwtkey),
            cache_dir: self.cache_dir.or(base.cache_dir),
            cache_size: self.cache_size.or(base.cache_size),
            cache_nfiles: self.cache_nfiles.or(base.cache_nfiles),
            memory_limit: self.memory_limit.or(base.memory_limit),
            admission_mode: self.admission_mode.or(base.admission_mode),
            tiles_memory_ratio: self.tiles_memory_ratio.or(base.tiles_memory_ratio),
            large_decode_threshold_bytes: self
                .large_decode_threshold_bytes
                .or(base.large_decode_threshold_bytes),
            decode_timeout_ms: self.decode_timeout_ms.or(base.decode_timeout_ms),
            maxpost: self.maxpost.or(base.maxpost),
            thumbsize: self.thumbsize.or(base.thumbsize),
            knorapath: self.knorapath.or(base.knorapath),
            knoraport: self.knoraport.or(base.knoraport),
            loglevel: self.loglevel.or(base.loglevel),
            jpeg_quality: self.jpeg_quality.or(base.jpeg_quality),
            scaling_quality: ScalingQuality {
                jpeg: self.scaling_quality.jpeg.or(base.scaling_quality.jpeg),
                tiff: self.scaling_quality.tiff.or(base.scaling_quality.tiff),
                png: self.scaling_quality.png.or(base.scaling_quality.png),
                j2k: self.scaling_quality.j2k.or(base.scaling_quality.j2k),
            },
            hostname: self.hostname.or(base.hostname),
            sslport: self.sslport.or(base.sslport),
        }
    }
}

/// The CLI/env override channel passed to `sipi_init` — hand-mirrored from
/// `sipi_ffi.h`'s `SipiServerConfig` (NOT bindgen; see the module docs). The
/// engine layers these over the Lua-parsed config before building its services.
///
/// Presence convention (matches the header):
/// - strings / the string array: a null pointer means "absent".
/// - scalars: a paired `has_*` flag (non-zero = present), because `0` is a valid
///   value for some (e.g. `cache_nfiles` `0` = unlimited).
///
/// Field order, widths, and `has_*` flags are lock-step with the header: the
/// [`layout`] test below and the C++ `static_assert`s in `sipi_ffi.h` pin the
/// layout against drift on either side. Constructed only via [`OverridesHolder`]
/// (a later slice), which owns the backing C strings; the fields are an FFI ABI
/// mirror, hence `#[allow(dead_code)]` until that wiring reads them.
#[repr(C)]
// `OverridesHolder` writes every field and C reads them across the seam; they
// are never read from Rust, so `dead_code` cannot see the use.
#[allow(dead_code)]
pub(crate) struct SipiServerConfig {
    // 8-byte: path / identity strings (null = absent)
    pub imgroot: *const c_char,
    pub scriptdir: *const c_char,
    pub initscript: *const c_char,
    pub tmpdir: *const c_char,
    pub jwtkey: *const c_char,
    pub cache_dir: *const c_char,
    pub cache_size: *const c_char, // raw "200M" — engine parses the suffix
    pub maxpost: *const c_char,    // raw "300M" — engine parses the suffix
    pub memory_limit: *const c_char, // raw "8G" RAM envelope — engine parses the suffix; "0"/absent = auto-detect
    pub admission_mode: *const c_char, // "basic" | "advanced"
    pub thumbsize: *const c_char,
    pub knorapath: *const c_char,
    pub knoraport: *const c_char,
    pub docroot: *const c_char,
    pub wwwroute: *const c_char,
    pub loglevel: *const c_char,
    // 8-byte: image scaling-quality per codec (null = engine default). TOML-only
    // (no CLI flag).
    pub scaling_quality_jpeg: *const c_char,
    pub scaling_quality_tiff: *const c_char,
    pub scaling_quality_png: *const c_char,
    pub scaling_quality_j2k: *const c_char,
    // 8-byte: 64-bit scalar values (presence via the has_ flags below)
    pub tiles_memory_ratio: f64, // fraction of the envelope reserved for tiles; full lane = envelope × (1 − ratio)
    pub large_decode_threshold_bytes: u64, // estimated peak >= this => full lane (charged); below => tile (bypass)
    // 4-byte scalar values (presence via the has_ flags below)
    pub serverport: i32,
    pub maxtmpage: i32,
    pub cache_nfiles: u32, // 0 = unlimited; a negative is rejected at the CLI (no wrap)
    pub pathprefix: i32,   // prefix_as_path, bool carried as 0/1
    pub jpeg_quality: i32, // JPEG output quality (1-100); TOML-only
    // 4-byte presence flags (non-zero = present)
    pub has_serverport: c_int,
    pub has_maxtmpage: c_int,
    pub has_cache_nfiles: c_int,
    pub has_pathprefix: c_int,
    pub has_jpeg_quality: c_int,
    pub has_tiles_memory_ratio: c_int,
    pub has_large_decode_threshold_bytes: c_int,
    // Appended after the 4-byte block: 8-aligned at 224, padded to 240.
    pub decode_timeout_ms: u64,
    pub has_decode_timeout_ms: c_int,
}

/// Owns the C storage backing a [`SipiServerConfig`] so its pointers stay valid
/// across the synchronous `sipi_init` call (seam contract: caller-owned inputs
/// outlive the call). Built from a [`ServerOverrides`]; the engine deep-copies
/// every present value during `sipi_init`, so the holder can drop right after.
///
/// `cfg`'s pointers reference heap buffers owned by `_strings`. A
/// `CString`'s buffer and a `Vec`'s buffer keep a stable address when the owning
/// struct moves, so the holder itself is safe to move; only [`Self::as_ptr`]'s
/// result is move-sensitive (it borrows `self.cfg`), and it is consumed inline
/// by the immediately-following `sipi_init` call.
///
/// Construction is fallible: a string value containing an interior NUL byte
/// cannot become a `CString`. Argv/env inputs are NUL-free (the OS forbids it),
/// but a TOML config string can carry one, so [`Self::new`] returns the error
/// instead of panicking — the caller surfaces it as a startup failure.
pub(crate) struct OverridesHolder {
    _strings: Vec<CString>,
    cfg: SipiServerConfig,
}

impl OverridesHolder {
    pub fn new(o: &ServerOverrides) -> Result<Self, NulError> {
        // Fail-on-omission (DUNE-006): destructure the whole `ServerOverrides`
        // (no `..` rest pattern) so a field that is never forwarded into the C
        // `SipiServerConfig` below fails to compile — an unused binding under the
        // crate's `-D warnings` clippy gate — instead of being silently dropped
        // before it reaches the engine. Cloned so the bindings are owned (a single
        // clone at startup); the scalar `Option`s are `Copy`, so the value +
        // `has_*` presence flag can each read the same binding.
        let ServerOverrides {
            serverport,
            imgroot,
            scriptdir,
            initscript,
            tmpdir,
            maxtmpage,
            docroot,
            wwwroute,
            pathprefix,
            jwtkey,
            cache_dir,
            cache_size,
            cache_nfiles,
            memory_limit,
            admission_mode,
            tiles_memory_ratio,
            large_decode_threshold_bytes,
            decode_timeout_ms,
            maxpost,
            thumbsize,
            knorapath,
            knoraport,
            loglevel,
            jpeg_quality,
            scaling_quality,
            // Lua-config-only: consumed Rust-side (the Lua `config` table);
            // they do not cross the seam.
            hostname: _,
            sslport: _,
        } = o.clone();

        let mut strings: Vec<CString> = Vec::new();

        let cfg = SipiServerConfig {
            imgroot: intern_cstr(&mut strings, &imgroot)?,
            scriptdir: intern_cstr(&mut strings, &scriptdir)?,
            initscript: intern_cstr(&mut strings, &initscript)?,
            tmpdir: intern_cstr(&mut strings, &tmpdir)?,
            jwtkey: intern_cstr(&mut strings, &jwtkey)?,
            cache_dir: intern_cstr(&mut strings, &cache_dir)?,
            cache_size: intern_cstr(&mut strings, &cache_size)?,
            maxpost: intern_cstr(&mut strings, &maxpost)?,
            memory_limit: intern_cstr(&mut strings, &memory_limit)?,
            admission_mode: intern_cstr(&mut strings, &admission_mode)?,
            thumbsize: intern_cstr(&mut strings, &thumbsize)?,
            knorapath: intern_cstr(&mut strings, &knorapath)?,
            knoraport: intern_cstr(&mut strings, &knoraport)?,
            docroot: intern_cstr(&mut strings, &docroot)?,
            wwwroute: intern_cstr(&mut strings, &wwwroute)?,
            loglevel: intern_cstr(&mut strings, &loglevel)?,
            scaling_quality_jpeg: intern_cstr(&mut strings, &scaling_quality.jpeg)?,
            scaling_quality_tiff: intern_cstr(&mut strings, &scaling_quality.tiff)?,
            scaling_quality_png: intern_cstr(&mut strings, &scaling_quality.png)?,
            scaling_quality_j2k: intern_cstr(&mut strings, &scaling_quality.j2k)?,
            tiles_memory_ratio: tiles_memory_ratio.unwrap_or(0.0),
            // Always sent with the shell-side default when unset: the shell owns
            // the single definition (DUNE-003), so the engine reads it from the
            // seam and never carries its own copy.
            large_decode_threshold_bytes: large_decode_threshold_bytes
                .unwrap_or(DEFAULT_LARGE_DECODE_THRESHOLD_BYTES),
            serverport: serverport.map(i32::from).unwrap_or(0),
            maxtmpage: maxtmpage.unwrap_or(0),
            cache_nfiles: cache_nfiles.unwrap_or(0),
            pathprefix: pathprefix.map(i32::from).unwrap_or(0),
            jpeg_quality: jpeg_quality.unwrap_or(0),
            has_serverport: serverport.is_some() as c_int,
            has_maxtmpage: maxtmpage.is_some() as c_int,
            has_cache_nfiles: cache_nfiles.is_some() as c_int,
            has_pathprefix: pathprefix.is_some() as c_int,
            has_jpeg_quality: jpeg_quality.is_some() as c_int,
            has_tiles_memory_ratio: tiles_memory_ratio.is_some() as c_int,
            // Always present: the shell always supplies the threshold (its own
            // default when unset), so the engine can rely on the seam value.
            has_large_decode_threshold_bytes: 1,
            decode_timeout_ms: decode_timeout_ms.unwrap_or(0),
            has_decode_timeout_ms: decode_timeout_ms.is_some() as c_int,
        };

        Ok(Self {
            _strings: strings,
            cfg,
        })
    }

    /// Pointer to the `SipiServerConfig` for `sipi_init`. Valid only while `self`
    /// is alive and unmoved — call it inline at the `sipi_init` call site.
    pub fn as_ptr(&self) -> *const SipiServerConfig {
        &self.cfg
    }
}

/// Push `s` (when present) into `strings` as a `CString` and return a pointer to
/// its buffer, or null when absent. The buffer keeps a stable address even if
/// `strings` later reallocates (the `CString` allocation does not move when the
/// `Vec`'s element slots do). Returns `Err` when the value has an interior NUL
/// byte (possible from a TOML config string; argv/env are NUL-free).
fn intern_cstr(strings: &mut Vec<CString>, s: &Option<String>) -> Result<*const c_char, NulError> {
    match s {
        Some(v) => {
            let c = CString::new(v.as_str())?;
            let p = c.as_ptr();
            strings.push(c);
            Ok(p)
        }
        None => Ok(std::ptr::null()),
    }
}

/// Lock-step layout guard — paired with the C++ `static_assert`/`offsetof`
/// checks in `src/ffi/sipi_ffi.h`. Any field reorder or width change on either
/// side breaks one of the two. LP64 on every supported target (darwin-aarch64,
/// linux-x86_64, linux-aarch64).
#[cfg(test)]
mod layout {
    use super::SipiServerConfig;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn repr_c_matches_sipi_ffi_h() {
        assert_eq!(size_of::<usize>(), 8, "layout assumes an LP64 target");
        assert_eq!(align_of::<SipiServerConfig>(), 8);
        assert_eq!(size_of::<SipiServerConfig>(), 240);

        assert_eq!(offset_of!(SipiServerConfig, imgroot), 0);
        assert_eq!(offset_of!(SipiServerConfig, scriptdir), 8);
        assert_eq!(offset_of!(SipiServerConfig, initscript), 16);
        assert_eq!(offset_of!(SipiServerConfig, tmpdir), 24);
        assert_eq!(offset_of!(SipiServerConfig, jwtkey), 32);
        assert_eq!(offset_of!(SipiServerConfig, cache_dir), 40);
        assert_eq!(offset_of!(SipiServerConfig, cache_size), 48);
        assert_eq!(offset_of!(SipiServerConfig, maxpost), 56);
        assert_eq!(offset_of!(SipiServerConfig, memory_limit), 64);
        assert_eq!(offset_of!(SipiServerConfig, admission_mode), 72);
        assert_eq!(offset_of!(SipiServerConfig, thumbsize), 80);
        assert_eq!(offset_of!(SipiServerConfig, knorapath), 88);
        assert_eq!(offset_of!(SipiServerConfig, knoraport), 96);
        assert_eq!(offset_of!(SipiServerConfig, docroot), 104);
        assert_eq!(offset_of!(SipiServerConfig, wwwroute), 112);
        assert_eq!(offset_of!(SipiServerConfig, loglevel), 120);
        assert_eq!(offset_of!(SipiServerConfig, scaling_quality_jpeg), 128);
        assert_eq!(offset_of!(SipiServerConfig, scaling_quality_tiff), 136);
        assert_eq!(offset_of!(SipiServerConfig, scaling_quality_png), 144);
        assert_eq!(offset_of!(SipiServerConfig, scaling_quality_j2k), 152);
        assert_eq!(offset_of!(SipiServerConfig, tiles_memory_ratio), 160);
        assert_eq!(
            offset_of!(SipiServerConfig, large_decode_threshold_bytes),
            168
        );
        assert_eq!(offset_of!(SipiServerConfig, serverport), 176);
        assert_eq!(offset_of!(SipiServerConfig, maxtmpage), 180);
        assert_eq!(offset_of!(SipiServerConfig, cache_nfiles), 184);
        assert_eq!(offset_of!(SipiServerConfig, pathprefix), 188);
        assert_eq!(offset_of!(SipiServerConfig, jpeg_quality), 192);
        assert_eq!(offset_of!(SipiServerConfig, has_serverport), 196);
        assert_eq!(offset_of!(SipiServerConfig, has_maxtmpage), 200);
        assert_eq!(offset_of!(SipiServerConfig, has_cache_nfiles), 204);
        assert_eq!(offset_of!(SipiServerConfig, has_pathprefix), 208);
        assert_eq!(offset_of!(SipiServerConfig, has_jpeg_quality), 212);
        assert_eq!(offset_of!(SipiServerConfig, has_tiles_memory_ratio), 216);
        assert_eq!(
            offset_of!(SipiServerConfig, has_large_decode_threshold_bytes),
            220
        );
        assert_eq!(offset_of!(SipiServerConfig, decode_timeout_ms), 224);
        assert_eq!(offset_of!(SipiServerConfig, has_decode_timeout_ms), 232);
    }
}

#[cfg(test)]
mod overrides_tests {
    use super::{OverridesHolder, ScalingQuality, ServerOverrides};

    #[test]
    fn layered_over_prefers_self_then_falls_back_to_base() {
        // Precedence direction: `self` (CLI/env) wins per field when set; `base`
        // (TOML config) fills the gaps. The exhaustive struct literal guarantees
        // every field is *merged*; this test guards the *direction* of each merge
        // — a swapped `self`/`base` or an `or` typo would still compile, so a
        // behavioural test is the only thing that catches it. One field per group.
        let base = ServerOverrides {
            serverport: Some(1000),                  // network
            imgroot: Some("/base/img".into()),       // paths (set in both)
            scriptdir: Some("/base/scripts".into()), // paths (self None → base)
            maxtmpage: Some(10),
            pathprefix: Some(false),
            jwtkey: Some("base-jwtkey".into()), // auth (self None → base)
            cache_nfiles: Some(1),              // cache
            maxpost: Some("100M".into()),       // limits
            knorapath: Some("base-knora".into()), // knora (self None → base)
            loglevel: Some("INFO".into()),      // logging
            jpeg_quality: Some(50),             // image quality
            scaling_quality: ScalingQuality {
                jpeg: Some("low".into()),
                tiff: Some("low".into()), // self None → base
                ..Default::default()
            },
            ..Default::default()
        };
        let this = ServerOverrides {
            serverport: Some(2000),
            imgroot: Some("/self/img".into()),
            maxtmpage: Some(20),
            pathprefix: Some(true),
            cache_nfiles: Some(2),
            maxpost: Some("200M".into()),
            loglevel: Some("DEBUG".into()),
            jpeg_quality: Some(90),
            scaling_quality: ScalingQuality {
                jpeg: Some("high".into()),
                ..Default::default()
            },
            ..Default::default()
        };
        let merged = this.layered_over(base);
        // self wins where set:
        assert_eq!(merged.serverport, Some(2000));
        assert_eq!(merged.imgroot.as_deref(), Some("/self/img"));
        assert_eq!(merged.maxtmpage, Some(20));
        assert_eq!(merged.pathprefix, Some(true));
        assert_eq!(merged.cache_nfiles, Some(2));
        assert_eq!(merged.maxpost.as_deref(), Some("200M"));
        assert_eq!(merged.loglevel.as_deref(), Some("DEBUG"));
        assert_eq!(merged.jpeg_quality, Some(90));
        assert_eq!(merged.scaling_quality.jpeg.as_deref(), Some("high"));
        // base fills where self is None:
        assert_eq!(merged.scriptdir.as_deref(), Some("/base/scripts"));
        assert_eq!(merged.jwtkey.as_deref(), Some("base-jwtkey"));
        assert_eq!(merged.knorapath.as_deref(), Some("base-knora"));
        assert_eq!(merged.scaling_quality.tiff.as_deref(), Some("low"));
    }

    #[test]
    fn new_rejects_interior_nul() {
        // A TOML config string can carry an interior NUL; it must surface as an
        // error, not panic in CString::new.
        let o = ServerOverrides {
            imgroot: Some("/img\0root".to_string()),
            ..Default::default()
        };
        assert!(OverridesHolder::new(&o).is_err());
    }

    #[test]
    fn new_accepts_nul_free_strings() {
        let o = ServerOverrides {
            imgroot: Some("/images".to_string()),
            scriptdir: Some("/scripts".to_string()),
            ..Default::default()
        };
        assert!(OverridesHolder::new(&o).is_ok());
    }
}

#[cfg(test)]
mod csv_list_tests {
    use super::parse_csv_list;

    #[test]
    fn unset_yields_empty_allowlist() {
        assert_eq!(parse_csv_list(None), Vec::<String>::new());
    }

    #[test]
    fn empty_string_yields_empty_allowlist() {
        assert_eq!(parse_csv_list(Some("")), Vec::<String>::new());
    }

    #[test]
    fn splits_trims_and_drops_empty_entries() {
        assert_eq!(
            parse_csv_list(Some(" https://a.example ,https://b.example,,")),
            vec!["https://a.example", "https://b.example"]
        );
        assert_eq!(
            parse_csv_list(Some(" iiif.example.org ,iiif2.example.org,,")),
            vec!["iiif.example.org", "iiif2.example.org"]
        );
    }
}

#[cfg(test)]
mod request_timeout_tests {
    use super::{request_timeout_from_env, DEFAULT_REQUEST_TIMEOUT_SECS};
    use std::sync::Mutex;
    use std::time::Duration;

    /// Serializes every test in this module: `std::env::var` reads
    /// process-global state, so tests that set/remove `SIPI_REQUEST_TIMEOUT`
    /// would otherwise race in parallel.
    static SERIAL: Mutex<()> = Mutex::new(());

    fn with_env<T>(value: Option<&str>, f: impl FnOnce() -> T) -> T {
        let _guard = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
        match value {
            Some(v) => std::env::set_var("SIPI_REQUEST_TIMEOUT", v),
            None => std::env::remove_var("SIPI_REQUEST_TIMEOUT"),
        }
        let result = f();
        std::env::remove_var("SIPI_REQUEST_TIMEOUT");
        result
    }

    #[test]
    fn unset_defaults_to_60s() {
        let got = with_env(None, request_timeout_from_env);
        assert_eq!(got, Duration::from_secs(DEFAULT_REQUEST_TIMEOUT_SECS));
    }

    #[test]
    fn valid_value_is_honored() {
        let got = with_env(Some("30"), request_timeout_from_env);
        assert_eq!(got, Duration::from_secs(30));
    }

    #[test]
    fn invalid_value_defaults_to_60s() {
        let got = with_env(Some("not-a-number"), request_timeout_from_env);
        assert_eq!(got, Duration::from_secs(DEFAULT_REQUEST_TIMEOUT_SECS));
    }

    #[test]
    fn zero_defaults_to_60s() {
        let got = with_env(Some("0"), request_timeout_from_env);
        assert_eq!(got, Duration::from_secs(DEFAULT_REQUEST_TIMEOUT_SECS));
    }
}

#[cfg(test)]
mod body_read_timeout_tests {
    use super::{body_read_timeout_from_env, DEFAULT_BODY_READ_TIMEOUT_SECS};
    use std::sync::Mutex;
    use std::time::Duration;

    /// Serializes every test in this module: `std::env::var` reads
    /// process-global state, so tests that set/remove `SIPI_BODY_READ_TIMEOUT`
    /// would otherwise race in parallel.
    static SERIAL: Mutex<()> = Mutex::new(());

    fn with_env<T>(value: Option<&str>, f: impl FnOnce() -> T) -> T {
        let _guard = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
        match value {
            Some(v) => std::env::set_var("SIPI_BODY_READ_TIMEOUT", v),
            None => std::env::remove_var("SIPI_BODY_READ_TIMEOUT"),
        }
        let result = f();
        std::env::remove_var("SIPI_BODY_READ_TIMEOUT");
        result
    }

    #[test]
    fn unset_defaults_to_10s() {
        let got = with_env(None, body_read_timeout_from_env);
        assert_eq!(got, Duration::from_secs(DEFAULT_BODY_READ_TIMEOUT_SECS));
    }

    #[test]
    fn valid_value_is_honored() {
        let got = with_env(Some("30"), body_read_timeout_from_env);
        assert_eq!(got, Duration::from_secs(30));
    }

    #[test]
    fn invalid_value_defaults_to_10s() {
        let got = with_env(Some("not-a-number"), body_read_timeout_from_env);
        assert_eq!(got, Duration::from_secs(DEFAULT_BODY_READ_TIMEOUT_SECS));
    }

    #[test]
    fn zero_defaults_to_10s() {
        let got = with_env(Some("0"), body_read_timeout_from_env);
        assert_eq!(got, Duration::from_secs(DEFAULT_BODY_READ_TIMEOUT_SECS));
    }
}

/// Pins the constant so a future edit cannot make the shipped default
/// unbounded. The finite-mapping behaviour of `max_post_size` itself is
/// covered by the body-limit e2e.
#[cfg(test)]
mod tests {
    use super::DEFAULT_MAX_POST_SIZE;

    #[test]
    fn default_max_post_size_is_finite() {
        // black_box defeats clippy's assertions_on_constants lint (the
        // comparison would otherwise be evaluated at compile time and
        // flagged as always-true) — this still catches a future edit that
        // sets the constant to 0.
        assert!(std::hint::black_box(DEFAULT_MAX_POST_SIZE) > 0);
    }
}

#[cfg(test)]
mod decode_timeout_tests {
    use super::{resolve_decode_timeout, ServerOverrides};
    use crate::config_file::Config;
    use std::time::Duration;

    #[test]
    fn unset_is_80_percent_of_the_request_timeout() {
        assert_eq!(
            resolve_decode_timeout(None, Duration::from_secs(60)),
            Ok(48_000)
        );
        assert_eq!(
            resolve_decode_timeout(None, Duration::from_secs(1)),
            Ok(800)
        );
        assert_eq!(
            resolve_decode_timeout(None, Duration::from_secs(5)),
            Ok(4_000)
        );
    }

    #[test]
    fn a_set_value_below_the_request_timeout_is_kept() {
        assert_eq!(
            resolve_decode_timeout(Some(2_000), Duration::from_secs(60)),
            Ok(2_000)
        );
        assert_eq!(
            resolve_decode_timeout(Some(59_999), Duration::from_secs(60)),
            Ok(59_999)
        );
    }

    #[test]
    fn a_set_value_not_below_the_request_timeout_refuses_and_names_both() {
        let err = resolve_decode_timeout(Some(60_000), Duration::from_secs(60)).unwrap_err();
        assert!(err.contains("60000 ms"), "{err}");
        assert!(err.contains("SIPI_REQUEST_TIMEOUT"), "{err}");
        assert!(resolve_decode_timeout(Some(120_000), Duration::from_secs(60)).is_err());
    }

    #[test]
    fn zero_refuses() {
        assert!(resolve_decode_timeout(Some(0), Duration::from_secs(60)).is_err());
    }

    /// The check runs on the merged value, so a TOML value refuses like a
    /// CLI/env one, and a CLI/env value overrides the TOML one.
    #[test]
    fn the_merged_toml_and_cli_value_is_checked() {
        let toml = "[paths]\nimg_root = \"/img\"\n[limits]\ndecode_timeout_ms = 90000\n";
        let cfg: Config = toml::from_str(toml).unwrap();
        let (from_toml, _) = cfg.resolve(ServerOverrides::default()).unwrap();
        assert!(
            resolve_decode_timeout(from_toml.decode_timeout_ms, Duration::from_secs(60)).is_err()
        );

        let cfg: Config = toml::from_str(toml).unwrap();
        let cli = ServerOverrides {
            decode_timeout_ms: Some(30_000),
            ..Default::default()
        };
        let (merged, _) = cfg.resolve(cli).unwrap();
        assert_eq!(
            resolve_decode_timeout(merged.decode_timeout_ms, Duration::from_secs(60)),
            Ok(30_000)
        );
    }
}
