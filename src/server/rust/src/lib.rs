//! SIPI Rust HTTP shell.
//!
//! This crate is the `sipi` **library**: it owns the axum + tokio server,
//! routing, the FFI wiring to the C++ image engine, config, and observability,
//! and it statically links the engine via `//src/ffi:sipi_ffi`. The default
//! `sipi` binary lives in the sibling `cli/rust` crate (`//src/cli/rust:sipi`): it
//! owns `main`, parses the CLI, and calls [`run`] for the `server` verb.
//! Shipping the server as a library is what lets SIPI be consumed as a
//! dependency: a downstream crate can own `main`, depend on
//! `sipi`, and inject its own behaviour.

// Fast unsafe check (CI `lint` gate): every `unsafe {}` block must carry a
// `// SAFETY:` comment. `allow`-by-default (clippy `restriction` group), so it
// is enabled here explicitly; CI's `-Dwarnings` promotes it to a hard error.
#![warn(clippy::undocumented_unsafe_blocks)]

pub mod config;
pub mod config_file;
pub mod ffi;
pub mod info;
pub mod malloc_stats;
pub mod metrics;
pub mod path;
pub mod preflight_cache;
pub mod routes;
pub mod sink;
pub mod telemetry;

pub use config::{ServeKnobs, ServerOverrides};

use axum::extract::State;
use axum::response::{IntoResponse, Response};
use axum::{routing::get, Router};
use std::future::IntoFuture;
use std::net::SocketAddr;
use std::process::ExitCode;
use std::sync::{Arc, OnceLock};
use std::time::{Duration, Instant};

/// Last-resort listen port, used only when the `serve()` port resolution below
/// (the single authority on port precedence) selects nothing else. A Lua config
/// that omits `port` entirely falls to `SipiConf`'s own in-class default (3333,
/// `SipiConf.h`) one tier before this — that tier is never 1024, so an operator
/// relying on this constant should set `port` explicitly (as every known config
/// does).
const DEFAULT_PORT: u16 = 1024;

/// The jwt_secret literal SIPI used to ship in `config/sipi.config.lua`. A
/// deployment that inherited it unchanged could forge JWTs, so it is rejected
/// at startup alongside empty and too-short secrets.
const SHIPPED_JWT_SECRET: &str = "UP 4888, nice 4-8-4 steam engine";

/// True when `key` is unsafe to use as a jwt_secret: empty, shorter than 32
/// bytes, or the literal SIPI used to ship as a default.
fn jwtkey_is_unsafe(key: &str) -> bool {
    key.is_empty() || key.len() < 32 || key == SHIPPED_JWT_SECRET
}

/// Lexically normalize `p`: canonicalize when the path exists (resolves
/// symlinks too), else fall back to a purely lexical `.`/`..` collapse so a
/// not-yet-created directory (e.g. a `cache_dir` sipi will `mkdir -p` on
/// first use) can still be compared.
fn normalize_path(p: &std::path::Path) -> std::path::PathBuf {
    if let Ok(canon) = std::fs::canonicalize(p) {
        return canon;
    }
    let mut result = std::path::PathBuf::new();
    for component in p.components() {
        match component {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                result.pop();
            }
            other => result.push(other.as_os_str()),
        }
    }
    result
}

/// True when `a` and `b` name the same directory, or one is an ancestor of the
/// other. The docroot fileserver executes `.lua`/`.elua` scripts, so overlap
/// with any root a request can write to (or the script dir) turns an
/// uploaded/planted file into remote code execution.
fn paths_overlap(a: &std::path::Path, b: &std::path::Path) -> bool {
    let a = normalize_path(a);
    let b = normalize_path(b);
    a == b || a.starts_with(&b) || b.starts_with(&a)
}

/// Returns the name of the first root in `roots` that overlaps `docroot`, or
/// `None` if none do.
fn docroot_overlap<'a>(
    docroot: &std::path::Path,
    roots: &[(&'a str, &std::path::Path)],
) -> Option<&'a str> {
    roots
        .iter()
        .find(|(_, root)| paths_overlap(docroot, root))
        .map(|(name, _)| *name)
}

/// Process start time, for the `/health` uptime field. Set once at server
/// startup; the handler reads `elapsed()`.
static START: OnceLock<Instant> = OnceLock::new();

/// Run the SIPI axum server. The `cli/rust` binary parses the `server` verb's
/// flags and calls this; a downstream crate can call it directly.
/// `config` is the bootstrap config file path — `.lua` (engine Lua VM) or `.toml`
/// (parsed Rust-side); it selects the base config the `overrides` layer onto.
/// `drain_timeout` is the Rust-owned graceful-drain deadline in seconds
/// (default 30). `knobs` are the Rust-owned serve knobs ([`ServeKnobs`]).
/// Blocks until shutdown; returns the process exit code. Telemetry init lives in
/// [`server_main`] (inside the runtime) because the OTLP batch exporter needs a
/// tokio runtime.
pub fn run(
    config: Option<String>,
    overrides: ServerOverrides,
    drain_timeout: Option<u64>,
    knobs: ServeKnobs,
) -> ExitCode {
    let mut builder = tokio::runtime::Builder::new_multi_thread();
    builder.enable_all();
    let rt = match builder.build() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("failed to build tokio runtime: {e}");
            return ExitCode::FAILURE;
        }
    };

    rt.block_on(server_main(config, overrides, drain_timeout, knobs))
}

/// The async server lifecycle inside the tokio runtime: install telemetry, prove
/// the FFI seam, install the engine + Lua config, serve, then flush telemetry on
/// the way out.
async fn server_main(
    config: Option<String>,
    overrides: ServerOverrides,
    drain_timeout: Option<u64>,
    knobs: ServeKnobs,
) -> ExitCode {
    // Stamp the process start for /health uptime before anything else.
    let _ = START.set(Instant::now());

    // Telemetry first, so the startup logs + the FFI self-check are captured.
    // Fail-open: no OTEL_EXPORTER_OTLP_ENDPOINT → no exporter, so local dev needs
    // no collector. Inside the runtime because the OTLP batch exporter needs it.
    let otel = telemetry::init();

    // Prove the C++ engine links into this binary and the seam round-trips
    // before we accept traffic (cc_library → rust_binary under hermetic
    // LLVM/libc++). 404 is the expected status for a missing file.
    let status = ffi::link_self_check();
    tracing::info!(
        seam_status = status,
        "FFI link self-check: sipi_serve_file(bogus) → status"
    );
    debug_assert_eq!(
        status, 404,
        "FFI seam self-check should report 404 for a missing file"
    );

    // Read once: the request timeout bounds the decode deadline resolved below
    // and is the handler timeout `app` installs.
    let request_timeout = config::request_timeout_from_env();

    // Install the engine + config before serving. engine_context() hard-fails on
    // any serve call until this runs, so without --config only the engine-free
    // routes (/health, /favicon.ico) work. Both config flavors are parsed
    // Rust-side into the override channel and feed the engine a Lua-less init:
    // a `.toml` through `config_file`, anything else as a Lua config through
    // the scripting crate's config VM. `effective` is the overrides after the
    // config base + CLI/env merge (the listen port folds in here for both
    // flavors); the configured Lua routes are sourced Rust-side either way.
    let (mut effective, configured_routes): (ServerOverrides, Option<Vec<ffi::RouteEntry>>) =
        match config.as_deref() {
            Some(cfg) if cfg.ends_with(".toml") => {
                // Experimental (ADR-0017): the native config format may change
                // until it is validated in production.
                tracing::warn!(
                    "TOML config support is experimental; the schema may change \
                 until it is validated in production"
                );
                let parsed = match config_file::Config::load(cfg) {
                    Ok(c) => c,
                    Err(e) => {
                        tracing::error!(config = %cfg, error = %e, "invalid TOML config");
                        flush_telemetry(otel).await;
                        return ExitCode::FAILURE;
                    }
                };
                let (effective, routes) = match parsed.resolve(overrides) {
                    Ok(r) => r,
                    Err(e) => {
                        tracing::error!(config = %cfg, error = %e, "invalid TOML config");
                        flush_telemetry(otel).await;
                        return ExitCode::FAILURE;
                    }
                };
                (effective, Some(routes))
            }
            Some(cfg) => {
                // A Lua config, evaluated in the scripting crate's config VM
                // (whitelisted, unlimited — the trusted startup path). Parse errors
                // arrive pre-sanitized: chunk name + line, never a source echo (the
                // file carries `jwt_secret = '…'` literally), so logging `e` here
                // cannot leak it to tracing/Sentry.
                let parsed = match scripting::parse_config_file(std::path::Path::new(cfg)) {
                    Ok(p) => p,
                    Err(e) => {
                        tracing::error!(config = %cfg, error = %e, "invalid Lua config");
                        flush_telemetry(otel).await;
                        return ExitCode::FAILURE;
                    }
                };
                let base = match ServerOverrides::from_lua_config(&parsed) {
                    Ok(b) => b,
                    Err(e) => {
                        tracing::error!(config = %cfg, error = %e, "invalid Lua config");
                        flush_telemetry(otel).await;
                        return ExitCode::FAILURE;
                    }
                };
                let routes: Vec<ffi::RouteEntry> = parsed
                    .routes
                    .iter()
                    .map(|r| ffi::RouteEntry {
                        method: r.method.clone(),
                        route: r.route.clone(),
                        script: config_file::compose_script_path(&parsed.script_dir, &r.script),
                    })
                    .collect();
                (overrides.layered_over(base), Some(routes))
            }
            None => {
                tracing::warn!(
                    "no --config: engine uninitialised; only /health and /favicon.ico will serve"
                );
                (overrides, None)
            }
        };

    if let Some(cfg) = config.as_deref() {
        // The engine always receives the resolved deadline, so its own default
        // applies only to the offline verbs.
        match config::resolve_decode_timeout(effective.decode_timeout_ms, request_timeout) {
            Ok(ms) => effective.decode_timeout_ms = Some(ms),
            Err(e) => {
                tracing::error!(config = %cfg, error = %e, "invalid decode deadline");
                flush_telemetry(otel).await;
                return ExitCode::FAILURE;
            }
        }
        // The engine default-constructs its config; these overrides then
        // supply every value.
        if let Err(code) = ffi::init(&effective) {
            tracing::error!(config = %cfg, code, "sipi_init failed");
            flush_telemetry(otel).await;
            return ExitCode::FAILURE;
        }
        let flavor = if cfg.ends_with(".toml") {
            "TOML"
        } else {
            "Lua"
        };
        tracing::info!(
            config = %cfg,
            decode_timeout_ms = effective.decode_timeout_ms,
            "engine installed ({flavor} config)"
        );
    }

    // The docroot fileserver executes `.lua`/`.elua` scripts, so if it overlaps
    // any root a request can write to (imgroot, tmpdir, cache_dir) or the
    // script dir, an uploaded/planted file becomes remote code execution.
    // Refuse startup rather than trust operator config. Resolved with the same
    // defaults `lua_config_values` uses, so the comparison matches what the
    // engine actually serves from.
    if let Some(docroot) = effective.docroot.as_deref().filter(|d| !d.is_empty()) {
        let docroot_path = std::path::Path::new(docroot);
        let imgroot = effective.imgroot.clone().unwrap_or_else(|| ".".into());
        let tmpdir = effective.tmpdir.clone().unwrap_or_else(|| "/tmp".into());
        let scriptdir = effective
            .scriptdir
            .clone()
            .unwrap_or_else(|| "./scripts".into());
        let cache_dir = effective
            .cache_dir
            .clone()
            .unwrap_or_else(|| "./cache".into());
        let roots: [(&str, &std::path::Path); 4] = [
            ("imgroot", std::path::Path::new(&imgroot)),
            ("tmpdir", std::path::Path::new(&tmpdir)),
            ("scriptdir", std::path::Path::new(&scriptdir)),
            ("cache_dir", std::path::Path::new(&cache_dir)),
        ];
        if let Some(offending) = docroot_overlap(docroot_path, &roots) {
            tracing::error!(
                docroot = %docroot,
                overlaps = offending,
                "refusing startup — docroot overlaps {offending}; the docroot fileserver executes \
                 .lua/.elua scripts, so overlap with a writable root is remote code execution"
            );
            flush_telemetry(otel).await;
            return ExitCode::FAILURE;
        }
    }

    // The Rust-hosted Lua environment: hardened runtime + init script + the
    // config-table values, built from the resolved config. The boot probe runs
    // the init script under the request-VM limit profile; an init-script error
    // refuses startup, failing closed rather than silently disabling
    // authorization. A hook genuinely not defined is the legitimate
    // no-preflight mode. Probe results are boot-frozen.
    let (lua_env, hook_probes) = if configured_routes.is_some() {
        let limits = match scripting::LimitConfig::from_env() {
            Ok(l) => l,
            Err(e) => {
                tracing::error!(error = %e, "invalid Lua limit knob");
                flush_telemetry(otel).await;
                return ExitCode::FAILURE;
            }
        };
        let env = std::sync::Arc::new(scripting::LuaEnv::new(
            std::path::PathBuf::from(effective.scriptdir.clone().unwrap_or_default()),
            effective
                .initscript
                .clone()
                // "." is the schema default a Lua config without `initscript`
                // resolves to — a placeholder for "none", not a script path.
                .filter(|p| !p.is_empty() && p != ".")
                .map(std::path::PathBuf::from),
            effective.jwtkey.clone().unwrap_or_default(),
            limits,
            lua_config_values(&effective),
        ));
        let probes = match env.probe_hooks() {
            Ok(p) => p,
            Err(e) => {
                tracing::error!(error = %e, "init script failed — refusing startup (fail closed)");
                flush_telemetry(otel).await;
                return ExitCode::FAILURE;
            }
        };
        if jwtkey_is_unsafe(&effective.jwtkey.clone().unwrap_or_default()) {
            tracing::error!(
                "refusing startup — jwt_secret is empty, too short (<32 bytes), or the shipped default; set SIPI_JWTKEY"
            );
            flush_telemetry(otel).await;
            return ExitCode::FAILURE;
        }
        tracing::info!(
            pre_flight = probes.pre_flight,
            file_pre_flight = probes.file_pre_flight,
            "Lua runtime installed"
        );
        (Some(env), Some(probes))
    } else {
        (None, None)
    };

    let drain_deadline = Duration::from_secs(drain_timeout.unwrap_or(30));
    let result = serve(
        effective.serverport,
        drain_deadline,
        request_timeout,
        configured_routes,
        lua_env,
        hook_probes,
        knobs,
    )
    .await;

    // Flush pending spans before the guard drops; the OTLP export is blocking
    // I/O, so do it off the async runtime.
    flush_telemetry(otel).await;

    let code = match &result {
        Ok(()) => 0,
        Err(e) => {
            tracing::error!(error = %e, "server terminated with error");
            1
        }
    };

    if code == 0 {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}

/// Flush + shut down the OTel exporter off the async runtime — the flush
/// performs blocking I/O (the documented current-thread-shutdown deadlock is
/// avoided by running it on a blocking thread).
async fn flush_telemetry(otel: telemetry::Telemetry) {
    let _ = tokio::task::spawn_blocking(move || otel.shutdown()).await;
}

/// Build the axum application. `/health` + `/favicon.ico` are Rust-native and
/// never touch the FFI; every other path is the IIIF catch-all, which classifies,
/// validates, and drives the engine seam. The CORS, concurrency, OTel, and
/// request-duration layers wrap this.
pub fn app(state: Arc<routes::AppState>, request_timeout: Duration) -> Router {
    use axum_tracing_opentelemetry::middleware::{OtelAxumLayer, OtelInResponseLayer};
    let mut router = Router::new()
        // The bare root has no `*rest` capture, so register it explicitly; the
        // handler classifies it (→ 400, matching the C++ parser). OPTIONS is the
        // CORS preflight (engine-independent). These routes are traced: a span
        // named by the route template (low cardinality), continuing
        // the W3C traceparent; OtelInResponseLayer echoes it into the response.
        .route(
            "/",
            get(routes::iiif)
                .head(routes::iiif)
                .options(routes::cors_preflight),
        )
        .route(
            "/{*rest}",
            get(routes::iiif)
                .head(routes::iiif)
                .options(routes::cors_preflight),
        );

    // Configured Lua routes (the script_handler analogue), registered before the
    // OTel layer so they are traced too. axum/matchit prioritises a static route
    // over the IIIF `/{*rest}` catch-all; a `<route>/{*rest}` variant reproduces
    // the C++ longest-prefix match (a route at /api/upload also serves subpaths).
    for entry in &state.routes {
        if let Some(method_router) = routes::lua_route_method_router(Arc::clone(&state), entry) {
            router = router.route(&entry.route, method_router.clone());
            if entry.route.as_str() != "/" {
                let prefix = format!("{}/{{*rest}}", entry.route.trim_end_matches('/'));
                router = router.route(&prefix, method_router);
            }
        }
    }

    // The `/server` docroot fileserver (the C++ file_handler analogue): static
    // files with Range/206 + `.lua`/`.elua` execution. Registered before the OTel
    // layer (traced) and the IIIF catch-all; only when a docroot + wwwroute are
    // configured. axum/matchit prioritises the static `<wwwroute>` prefix over the
    // `/{*rest}` catch-all, and the `<wwwroute>/{*rest}` variant serves subpaths.
    if let Some(method_router) = routes::docroot_method_router(Arc::clone(&state)) {
        let www = state.wwwroute.trim_end_matches('/');
        if !www.is_empty() {
            router = router.route(&format!("{www}/{{*rest}}"), method_router.clone());
            router = router.route(www, method_router);
        }
    }

    router
        .layer(OtelInResponseLayer)
        .layer(OtelAxumLayer::default())
        // Outermost, so the recorded duration spans the whole in-router path
        // (tracing layers included) rather than just the handler.
        .layer(axum::middleware::from_fn(metrics::record_http_duration))
        // Handler wall-clock timeout: `tower_http::timeout::TimeoutLayer`
        // (not `tower::timeout::TimeoutLayer` — its `BoxError` error type is
        // incompatible with axum 0.8's `Infallible` router requirement) answers
        // 408 once `SIPI_REQUEST_TIMEOUT` elapses. This bounds the handler future
        // up to the response head (request-body read, preflight, engine
        // dispatch) and does NOT bound a streaming image body after the head is
        // sent; on the IIIF path the admission `Permit` is moved into
        // `spawn_blocking` and dropping the handler future does not cancel that
        // task, so the 408 bounds client-visible latency, not permit occupancy.
        .layer(tower_http::timeout::TimeoutLayer::with_status_code(
            axum::http::StatusCode::REQUEST_TIMEOUT,
            request_timeout,
        ))
        // Registered after the layers so liveness / asset probes never enter the
        // trace pipeline or the latency histogram — they also bypass the engine
        // pool.
        .route("/health", get(health))
        .route("/favicon.ico", get(favicon))
        .with_state(state)
}

async fn serve(
    port: Option<u16>,
    drain_timeout: Duration,
    request_timeout: Duration,
    configured_routes: Option<Vec<ffi::RouteEntry>>,
    lua_env: Option<std::sync::Arc<scripting::LuaEnv>>,
    hook_probes: Option<scripting::HookProbes>,
    knobs: ServeKnobs,
) -> std::io::Result<()> {
    // Read the cached engine config once (image root + prefix_as_path); a
    // not-ready state (no --config) leaves the serve routes returning 503.
    // `configured_routes` is `Some` whenever a config was loaded (both flavors
    // source the routes Rust-side), `None` without a config.
    // A degenerate admission ratio (`tiles_thread_ratio` ∉ (0,1)) is a hard
    // config error — fail loud at startup rather than serve with a broken pool.
    let state = Arc::new(
        routes::AppState::load(
            configured_routes,
            lua_env,
            hook_probes,
            knobs,
            // Read directly (like `SIPI_RS_PORT` below): CORS is a Rust-shell
            // knob, never a `ServerOverrides`/FFI concern.
            config::allowed_origins_from_env(),
            // Same rationale: the public-host allowlist is a Rust-shell knob,
            // never a `ServerOverrides`/FFI concern.
            config::public_hosts_from_env(),
        )
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidInput, e.to_string()))?,
    );
    // Bind the OTel observable instruments now that the engine pool exists (the
    // concurrency gauges read its permits). A no-op when no meter provider was
    // installed (no OTLP endpoint), so it is safe to call unconditionally.
    state.register_metrics();
    // Precedence: `SIPI_RS_PORT` (dev/test-only — lets the e2e
    // harness spawn parallel shells without a `--serverport`) beats
    // `--serverport`/`SIPI_SERVERPORT`/the config's port (`port` — the config
    // base folds `[network].port` / `sipi.port` under clap's own `CLI > env`
    // via `layered_over`), which beats the hardcoded default.
    let port = std::env::var("SIPI_RS_PORT")
        .ok()
        .and_then(|p| p.parse().ok())
        .or(port)
        .unwrap_or(DEFAULT_PORT);
    let addr = SocketAddr::from(([0, 0, 0, 0], port));
    let listener = tokio::net::TcpListener::bind(addr).await?;
    tracing::info!(%addr, "SIPI Rust shell listening");

    // Graceful shutdown: a SIGTERM/Ctrl-C stops accepting new
    // connections and lets in-flight requests finish, bounded by drain_timeout —
    // past the deadline the remaining requests are abandoned and the process
    // exits. A watch latches the signal so axum's drain and the deadline both
    // observe it from the single signal listener (a notify could be missed).
    let (drain_tx, drain_rx) = tokio::sync::watch::channel(false);
    let graceful = {
        let mut rx = drain_rx;
        async move {
            let _ = rx.wait_for(|started| *started).await;
        }
    };
    // `with_graceful_shutdown` yields an `IntoFuture`, not a `Future`; resolve it
    // so the select arm can poll `&mut server`.
    //
    // `axum::serve` exposes no hyper builder, so it cannot enforce a header-read
    // timeout or cap concurrent connections directly; both are enforced at the
    // edge in Traefik instead.
    let server = axum::serve(listener, app(state, request_timeout))
        .with_graceful_shutdown(graceful)
        .into_future();
    tokio::pin!(server);

    tokio::select! {
        res = &mut server => res,
        () = async {
            shutdown_signal().await;
            let _ = drain_tx.send(true);// begin draining in-flight requests
            tokio::time::sleep(drain_timeout).await;
        } => {
            tracing::warn!(
                drain_timeout_s = drain_timeout.as_secs(),
                "drain deadline exceeded; abandoning in-flight requests"
            );
            Ok(())
        }
    }
}

/// Resolve when the process is asked to stop: a Unix `SIGTERM` (the container
/// stop signal) or `Ctrl-C` (`SIGINT`). On non-Unix only `Ctrl-C` is wired.
async fn shutdown_signal() {
    let ctrl_c = async {
        let _ = tokio::signal::ctrl_c().await;
    };
    #[cfg(unix)]
    let terminate = async {
        match tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()) {
            Ok(mut sig) => {
                sig.recv().await;
            }
            Err(e) => {
                tracing::error!(error = %e, "failed to install SIGTERM handler");
                std::future::pending::<()>().await;
            }
        }
    };
    #[cfg(not(unix))]
    let terminate = std::future::pending::<()>();

    tokio::select! {
        () = ctrl_c => {},
        () = terminate => {},
    }
    tracing::info!("shutdown signal received; draining");
}

/// Rust-native liveness probe — bypasses the engine pool. Mirrors the C++
/// health_handler's body: `{status, version, uptime_seconds}` as JSON. The
/// version is the deployed release (`SIPI_SENTRY_RELEASE`, the same source as the
/// OTel `service.version`), falling back to the crate version in local runs.
/// Once the wedged decode threads reach `--max-wedged-threads` the server
/// sheds every request until restart, so the probe answers 503 with status
/// `wedged` and the orchestrator restarts it.
async fn health(State(state): State<Arc<routes::AppState>>) -> Response {
    let uptime = START.get().map_or(0, |s| s.elapsed().as_secs());
    let version =
        telemetry::service_version().unwrap_or_else(|| env!("CARGO_PKG_VERSION").to_owned());
    let (code, status) = if state.wedged_cap_reached() {
        (axum::http::StatusCode::SERVICE_UNAVAILABLE, "wedged")
    } else {
        (axum::http::StatusCode::OK, "ok")
    };
    let body =
        serde_json::json!({ "status": status, "version": version, "uptime_seconds": uptime })
            .to_string();
    (
        code,
        [(axum::http::header::CONTENT_TYPE, "application/json")],
        body,
    )
        .into_response()
}

/// Rust-native favicon — 200 + `image/x-icon`, serving the crate-local
/// `favicon.ico` bytes embedded via `include_bytes!`.
async fn favicon() -> impl IntoResponse {
    (
        [(axum::http::header::CONTENT_TYPE, "image/x-icon")],
        include_bytes!("favicon.ico").as_slice(),
    )
}

/// The Lua `config`-table values, resolved from the merged config with the
/// engine-config defaults (what the deleted C++ `SipiConf` getters exposed).
fn lua_config_values(effective: &ServerOverrides) -> scripting::bindings::ConfigValues {
    let size_bytes = |raw: &Option<String>, default: i64| -> i64 {
        raw.as_deref()
            .and_then(|s| scripting::entry::parse_size_string(s).ok())
            .unwrap_or(default)
    };
    scripting::bindings::ConfigValues {
        hostname: effective
            .hostname
            .clone()
            .unwrap_or_else(|| "localhost".into()),
        port: effective.serverport.map_or(3333, i64::from),
        sslport: effective.sslport.map_or(-1, i64::from),
        imgroot: effective.imgroot.clone().unwrap_or_else(|| ".".into()),
        max_temp_file_age: effective.maxtmpage.map_or(86400, i64::from),
        prefix_as_path: effective.pathprefix.unwrap_or(true),
        init_script: effective.initscript.clone().unwrap_or_else(|| ".".into()),
        cache_dir: effective
            .cache_dir
            .clone()
            .unwrap_or_else(|| "./cache".into()),
        cache_size: size_bytes(&effective.cache_size, 200 * 1024 * 1024),
        jpeg_quality: effective.jpeg_quality.map_or(80, i64::from),
        thumb_size: effective
            .thumbsize
            .clone()
            .unwrap_or_else(|| "!128,128".into()),
        cache_n_files: effective.cache_nfiles.map_or(200, i64::from),
        max_post_size: size_bytes(&effective.maxpost, 0).max(0),
        tmpdir: effective.tmpdir.clone().unwrap_or_else(|| "/tmp".into()),
        scriptdir: effective
            .scriptdir
            .clone()
            .unwrap_or_else(|| "./scripts".into()),
        knora_path: effective
            .knorapath
            .clone()
            .unwrap_or_else(|| "localhost".into()),
        knora_port: effective.knoraport.clone().unwrap_or_else(|| "3333".into()),
        docroot: effective.docroot.clone().unwrap_or_default(),
    }
}

#[cfg(test)]
mod app_tests {
    use super::*;
    use axum::body::Body;
    use axum::http::{Request, StatusCode};
    use tower::ServiceExt; // `oneshot`

    // No `sipi_init` runs in this test binary, so the engine is uninstalled and
    // AppState::load() reports not-ready. The engine-free routes still serve; the
    // serve routes 503. The full serve path (real images via the FFI) is covered
    // by the manual smoke run and the reqwest e2e suite targeting //src/cli/rust:sipi.
    fn test_app() -> Router {
        // Defaults everywhere (ratios fall back to valid values), so `load` cannot
        // return the degenerate-ratio error here.
        app(
            Arc::new(
                routes::AppState::load(
                    None,
                    None,
                    None,
                    ServeKnobs::default(),
                    Vec::new(),
                    Vec::new(),
                )
                .expect("default admission config is valid"),
            ),
            config::request_timeout_from_env(),
        )
    }

    async fn status_of(uri: &str) -> StatusCode {
        let resp = test_app()
            .oneshot(Request::get(uri).body(Body::empty()).unwrap())
            .await
            .unwrap();
        resp.status()
    }

    #[tokio::test]
    async fn health_is_rust_native() {
        assert_eq!(status_of("/health").await, StatusCode::OK);
    }

    #[tokio::test]
    async fn favicon_is_served_as_icon() {
        let resp = test_app()
            .oneshot(Request::get("/favicon.ico").body(Body::empty()).unwrap())
            .await
            .unwrap();
        assert_eq!(resp.status(), StatusCode::OK);
        assert_eq!(
            resp.headers()
                .get(axum::http::header::CONTENT_TYPE)
                .unwrap(),
            "image/x-icon"
        );
        let body = axum::body::to_bytes(resp.into_body(), usize::MAX)
            .await
            .unwrap();
        assert_eq!(body.len(), 1406, "favicon must match the favicon_ico bytes");
    }

    #[tokio::test]
    async fn serve_routes_503_without_engine() {
        // The catch-all guards on engine readiness before touching the seam.
        assert_eq!(
            status_of("/unit/lena512.jp2/info.json").await,
            StatusCode::SERVICE_UNAVAILABLE
        );
        assert_eq!(
            status_of("/unit/lena512.jp2/full/max/0/default.jpg").await,
            StatusCode::SERVICE_UNAVAILABLE
        );
    }
}

#[cfg(test)]
mod jwtkey_tests {
    use super::*;

    #[test]
    fn empty_is_unsafe() {
        assert!(jwtkey_is_unsafe(""));
    }

    #[test]
    fn short_is_unsafe() {
        assert!(jwtkey_is_unsafe("a".repeat(20).as_str()));
    }

    #[test]
    fn shipped_default_is_unsafe() {
        assert!(jwtkey_is_unsafe(SHIPPED_JWT_SECRET));
    }

    #[test]
    fn distinct_long_key_is_safe() {
        assert!(!jwtkey_is_unsafe("a".repeat(38).as_str()));
    }
}

#[cfg(test)]
mod docroot_overlap_tests {
    use super::*;
    use std::path::Path;

    #[test]
    fn equal_paths_overlap() {
        assert!(paths_overlap(
            Path::new("/data/www"),
            Path::new("/data/www")
        ));
    }

    #[test]
    fn nested_paths_overlap_docroot_inside_other() {
        assert!(paths_overlap(
            Path::new("/data/images/public"),
            Path::new("/data/images")
        ));
    }

    #[test]
    fn nested_paths_overlap_other_inside_docroot() {
        assert!(paths_overlap(
            Path::new("/data/www"),
            Path::new("/data/www/scripts")
        ));
    }

    #[test]
    fn disjoint_siblings_do_not_overlap() {
        assert!(!paths_overlap(
            Path::new("/data/www"),
            Path::new("/data/images")
        ));
    }

    #[test]
    fn docroot_overlap_reports_first_offending_root_name() {
        let roots: [(&str, &Path); 2] = [
            ("imgroot", Path::new("/data/images")),
            ("tmpdir", Path::new("/tmp")),
        ];
        assert_eq!(
            docroot_overlap(Path::new("/data/images"), &roots),
            Some("imgroot")
        );
        assert_eq!(docroot_overlap(Path::new("/data/www"), &roots), None);
    }
}
