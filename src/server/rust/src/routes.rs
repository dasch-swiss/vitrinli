//! The IIIF request router.
//!
//! Replaces the C++ `iiif_handler` dispatch: a
//! catch-all axum handler classifies the path with [`iiif_parser`], validates it
//! at the edge with [`crate::path`], runs the Lua preflight hook (auth + path
//! resolution) through the seam, and dispatches to the engine
//! (`sipi_serve_image` / `sipi_serve_file`) or assembles JSON with
//! [`crate::info`]. The blocking FFI calls run on a `spawn_blocking` pool bounded
//! by a semaphore sized to the configured worker count, so the async runtime
//! never stalls on the C++ engine.

use std::ffi::CString;
use std::io::Write;
use std::num::NonZeroU32;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime};

use axum::body::{to_bytes, Body};
use axum::extract::{DefaultBodyLimit, FromRequest, Multipart, OriginalUri, Request, State};
use axum::http::{header, HeaderMap, HeaderValue, Method, StatusCode, Uri};
use axum::response::{IntoResponse, Response};
use axum::routing::{on, MethodFilter, MethodRouter};
use tempfile::NamedTempFile;
use tokio::sync::{mpsc, oneshot};

use admission::{
    Acquired, Admission, AdmissionConfig, AdmissionError, AdmissionKind, AdmissionMode, Permit,
};
use iiif_parser::{ParsedRequest, RequestKind};

use crate::ffi::{self, SipiPermType, SipiResponse, SipiServeRequest};
use crate::info::{self, Sidecar};
use crate::path::{self, Resolved};
use crate::preflight_cache;
use crate::sink::{self, Outcome};

/// Image MIME types that take the IIIF / image-dimension code path. The C++
/// engine's `detect_in_format` (`src/ffi/serve_image.cpp`) is the matching
/// server-side table; both must list the same image mimes.
const IMAGE_MIMES: [&str; 5] = [
    "image/tiff",
    "image/jpeg",
    "image/png",
    "image/jpx",
    "image/jp2",
];

/// Cached engine config (read once at startup, after `sipi_init`) plus the shared
/// two-lane [`Admission`] pool that bounds concurrent engine work.
#[derive(Clone)]
pub struct AppState {
    ready: bool,
    imgroot: String,
    resolved_imgroot: String,
    prefix_as_path: bool,
    has_preflight: bool,
    has_file_preflight: bool,
    /// The Rust-hosted Lua environment (hardened runtime + init script +
    /// config-table values) behind the preflight hooks. `None` when no config
    /// is installed.
    lua: Option<Arc<scripting::LuaEnv>>,
    /// The two-lane admission pool: classifies each request tile/full and bounds
    /// concurrency (global pool + full sub-pool). Owns the per-partition wait/shed
    /// counters. Shared (`Arc`) across all requests. See
    /// [`admission`](../../throttling/rust).
    admission: Arc<Admission>,
    /// Configured Lua routes (method/route/script), registered as axum routes by
    /// [`crate::app`]. Empty when the engine is uninstalled.
    pub routes: Vec<ffi::RouteEntry>,
    /// Max POST body size in bytes, always finite: a Lua config that
    /// leaves `max_post_size` unset falls back to
    /// [`crate::config::DEFAULT_MAX_POST_SIZE`] in [`AppState::load`]. The
    /// Lua-route and docroot handlers reject oversized bodies (413).
    max_post_size: usize,
    /// The `/server` docroot fileserver root (raw config value; the handler
    /// canonicalises it per request). Empty = no fileserver configured.
    docroot: String,
    /// The URL prefix the docroot fileserver is mounted at (e.g. "/server"). Read
    /// by [`crate::app`] to register the static route. Empty = fileserver off.
    pub wwwroute: String,
    /// Burst-coalescing cache for the `pre_flight` access decision, in front of the
    /// hook in [`iiif_access`]. `None` when disabled (`--preflight-cache-ttl 0`).
    /// See [`crate::preflight_cache`].
    preflight_cache: Option<Arc<preflight_cache::PreflightCache>>,
    /// CORS origin allowlist (`SIPI_ALLOWED_ORIGINS`). Empty (the default) is
    /// opt-out: every CORS site reflects any origin via [`cors_allow`].
    /// Non-empty switches the instance into allowlist mode.
    allowed_origins: Vec<String>,
    /// Public host allowlist (`SIPI_PUBLIC_HOSTS`). Empty (the default) is
    /// opt-out: [`forwarded`] returns the `X-Forwarded-Host`/`Host` header
    /// verbatim. Non-empty switches the instance into
    /// allowlist mode: a header host on the list passes through, any other
    /// host is replaced by the first allowlisted host — closing the
    /// canonical-`@id`/`Location`/`Link`/cache-key/`server.host` spoofing
    /// lever a hostile header would otherwise open.
    public_hosts: Vec<String>,
}

impl AppState {
    /// Read the cached config from the engine. Returns a not-ready state when
    /// `sipi_init` has not run (no `--config`); the serve routes then 503.
    ///
    /// `configured_routes` is `Some` for a TOML config (the shell parsed the
    /// `[[routes]]` table Rust-side and composed each script path); `None` for a
    /// Lua config, where the routes are read back from the engine via
    /// [`ffi::routes`] after `sipi_init` installed them.
    ///
    /// `lua` is the Rust-hosted Lua environment (hardened runtime + init
    /// script + config-table values); `has_preflight`/`has_file_preflight`
    /// are its boot-frozen hook probes.
    ///
    /// `nthreads`/`max_waiting`/`queue_timeout` are the
    /// `--nthreads`/`--max-waiting`/`--queue-timeout` serve knobs (`None` → the
    /// defaults below).
    ///
    /// `allowed_origins` is the `SIPI_ALLOWED_ORIGINS` CORS allowlist
    /// ([`crate::config::allowed_origins_from_env`]); empty = opt-out, every
    /// CORS site reflects any Origin.
    ///
    /// `public_hosts` is the `SIPI_PUBLIC_HOSTS` host allowlist
    /// ([`crate::config::public_hosts_from_env`]); empty = opt-out, `forwarded`
    /// trusts `X-Forwarded-Host`/`Host` verbatim.
    // Startup glue with one parameter per serve knob (like `serve()` in
    // lib.rs); a config struct would just relocate the list.
    #[allow(clippy::too_many_arguments)]
    pub fn load(
        configured_routes: Option<Vec<ffi::RouteEntry>>,
        lua: Option<Arc<scripting::LuaEnv>>,
        hook_probes: Option<scripting::HookProbes>,
        nthreads: Option<u32>,
        tiles_thread_ratio: Option<f64>,
        max_waiting: Option<u64>,
        queue_timeout: Option<u32>,
        preflight_cache_ttl: Option<u32>,
        preflight_cache_slots: Option<u64>,
        allowed_origins: Vec<String>,
        public_hosts: Vec<String>,
    ) -> Result<Self, AdmissionError> {
        // The thread-pool knobs are Rust-owned serve args. `nthreads` (unset or
        // 0 = auto) sizes the global pool from host parallelism; `max_waiting`
        // defaults to 2×workers and `queue_timeout` to 5s.
        let resolved_nthreads = nthreads
            .filter(|n| *n > 0)
            .map_or_else(admission::default_pool_size, |n| n as usize);
        let max_waiting =
            max_waiting.map_or_else(|| resolved_nthreads.saturating_mul(2), |n| n as usize);
        let queue_timeout = Duration::from_secs(u64::from(queue_timeout.unwrap_or(5)));
        // The memory-coupled admission config is read back from the engine (the
        // single authority — it parsed the Lua/override config and resolved the
        // envelope), so the shell's pool classifies and enforces against exactly
        // the values the engine's memory budget uses. Falls back to the code
        // defaults only when the engine is not installed (the not-ready arm).
        let mode = ffi::admission_mode()
            .ok()
            .and_then(|s| AdmissionMode::parse(&s))
            .unwrap_or(AdmissionMode::Basic);
        let admission = Arc::new(Admission::new(AdmissionConfig {
            nthreads: resolved_nthreads,
            tiles_thread_ratio: tiles_thread_ratio.unwrap_or(0.5),
            tiles_memory_ratio: ffi::tiles_memory_ratio().unwrap_or(0.25),
            mode,
            max_waiting,
            queue_timeout,
            large_decode_threshold_bytes: ffi::large_decode_threshold_bytes()
                .unwrap_or(32 * 1024 * 1024),
            memory_limit_bytes: ffi::memory_limit_bytes().unwrap_or(0),
        })?);
        // Preflight access-cache: opt-in, TTL default 0 = disabled (→ `None`, no
        // cache work per request); a TTL > 0 enables it, slots default 4096.
        let preflight_cache = preflight_cache::PreflightCache::new(
            preflight_cache_slots.map_or(preflight_cache::DEFAULT_SLOTS, |n| n as usize),
            Duration::from_secs(
                preflight_cache_ttl.map_or(preflight_cache::DEFAULT_TTL_SECS, u64::from),
            ),
        );
        Ok(
            match (
                ffi::imgroot(false),
                ffi::imgroot(true),
                ffi::prefix_as_path(),
            ) {
                (Ok(imgroot), Ok(resolved_imgroot), Ok(prefix_as_path)) => Self {
                    ready: true,
                    imgroot,
                    resolved_imgroot,
                    prefix_as_path,
                    // Boot-frozen hook probes (an init-script error already
                    // refused startup in lib.rs — fail closed).
                    has_preflight: hook_probes.is_some_and(|p| p.pre_flight),
                    has_file_preflight: hook_probes.is_some_and(|p| p.file_pre_flight),
                    lua,
                    admission,
                    // TOML config supplies routes directly; a Lua config has them
                    // read back from the engine via the seam.
                    routes: configured_routes.unwrap_or_default(),
                    // `0` from the engine means "unset" (Lua config never
                    // configured `max_post_size`), not "unlimited" — fall back to
                    // a finite default rather than reading an unbounded body into
                    // RAM before admission.
                    max_post_size: ffi::max_post_size()
                        .ok()
                        .filter(|&n| n > 0)
                        .unwrap_or(crate::config::DEFAULT_MAX_POST_SIZE),
                    // The fileserver docroot/wwwroute (empty when not configured →
                    // no static route registered).
                    docroot: ffi::docroot().unwrap_or_default(),
                    wwwroute: ffi::wwwroute().unwrap_or_default(),
                    preflight_cache,
                    allowed_origins,
                    public_hosts,
                },
                _ => Self {
                    ready: false,
                    imgroot: String::new(),
                    resolved_imgroot: String::new(),
                    prefix_as_path: true,
                    has_preflight: false,
                    has_file_preflight: false,
                    lua: None,
                    admission,
                    routes: Vec::new(),
                    max_post_size: crate::config::DEFAULT_MAX_POST_SIZE,
                    docroot: String::new(),
                    wwwroute: String::new(),
                    preflight_cache,
                    allowed_origins,
                    public_hosts,
                },
            },
        )
    }

    /// Register the OTel observable instruments for the engine + admission metrics
    /// against the global meter (a no-op when no meter provider is installed, so
    /// it is safe to call unconditionally). Hands the metrics module the shared
    /// [`Admission`] pool, whose snapshot feeds the concurrency + fingerprint gauges.
    pub(crate) fn register_metrics(&self) {
        crate::metrics::register(Arc::clone(&self.admission));
    }
}

/// The parsed result of a preflight hook: the permission and the open kv
/// channel (`infile` plus `watermark` / `size` / auth-service keys).
pub struct PreflightOutcome {
    pub permission: SipiPermType,
    pub kv: Vec<(String, String)>,
}

impl PreflightOutcome {
    /// The value of a kv key, if the hook emitted it.
    #[must_use]
    fn get(&self, key: &str) -> Option<&str> {
        self.kv
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }
}

/// The resolved access decision: the on-disk file the hook chose, the permission,
/// and the open kv channel (restrict `size`/`watermark`, auth-service urls).
struct Access {
    infile: String,
    permission: SipiPermType,
    kv: Vec<(String, String)>,
}

/// The catch-all IIIF handler (`GET|HEAD /…`). Classifies, validates, runs
/// preflight, and dispatches. Never leaks an internal path in an error body:
/// every failure this code renders is a bare status via [`sink::error_response`]
/// — the one exception is a `pre_flight` hook that answers the request itself
/// (`preflight_direct_response`), whose body is whatever the script wrote, by
/// the script's own choice.
///
/// Every response this handler produces leaves through [`with_cors`], whatever
/// its status: a cross-origin client only sees a response that carries
/// `Access-Control-Allow-Origin`, so an error without it reaches the browser as
/// status 0 instead of its real 400/401/403/404/503. Responses built outside
/// the handler get no CORS: the `TimeoutLayer` 408 and axum's 405 for a method
/// the route does not serve.
pub async fn iiif(
    State(state): State<Arc<AppState>>,
    method: Method,
    OriginalUri(uri): OriginalUri,
    headers: HeaderMap,
) -> Response {
    let origin = header_str(&headers, "origin");
    let cors_decision = cors_allow(origin.as_deref(), &state.allowed_origins);
    let response = iiif_response(state, method, uri, headers).await;
    with_cors(response, origin.as_deref(), cors_decision)
}

/// The body of [`iiif`]: every early return and every engine outcome, before
/// the CORS headers are applied.
async fn iiif_response(
    state: Arc<AppState>,
    method: Method,
    uri: Uri,
    headers: HeaderMap,
) -> Response {
    if !state.ready {
        return sink::error_response(StatusCode::SERVICE_UNAVAILABLE);
    }

    let parsed = match iiif_parser::parse_request(uri.path()) {
        Ok(p) => p,
        Err(_) => return sink::error_response(StatusCode::BAD_REQUEST),
    };

    // R7: an interior NUL in the decoded identifier/prefix is rejected outright
    // (matches the docroot guard's R7, `decode_path_suffix`). NUL only — not the
    // full control-char set: this path has no independent CRLF guard (unlike
    // `redirect()`, whose `HeaderValue::from_str` already rejects CRLF
    // regardless of this check), so broadening this check would reject
    // identifiers currently served — a behaviour change to weigh separately.
    if parsed.identifier.contains('\0') || parsed.prefix.contains('\0') {
        return sink::error_response(StatusCode::BAD_REQUEST);
    }

    // R1: string-level traversal check on the decoded identifier (and prefix when
    // it is a path component), before any path construction or preflight.
    if path::contains_traversal(&parsed.identifier)
        || (state.prefix_as_path && path::contains_traversal(&parsed.prefix))
    {
        tracing::warn!(identifier = %parsed.identifier, prefix = %parsed.prefix, "rejected path traversal");
        return sink::error_response(StatusCode::NOT_FOUND);
    }

    // Redirect is cheap and engine-free — answer it on the async path.
    if parsed.kind == RequestKind::Redirect {
        return redirect(&headers, &parsed, &state.public_hosts);
    }

    // Everything else drives the blocking C++ engine (the per-call preflight VM,
    // realpath, decode/encode). Classify the request tile/full and admit it to
    // that partition of the two-lane pool, running the work on a blocking thread
    // so the async runtime stays responsive; a shed/timeout returns 503 +
    // Retry-After. /health, /favicon, and OPTIONS are separate routes that never
    // reach here.
    let permit = match state
        .admission
        .acquire(state.admission.classify(&parsed))
        .await
    {
        Acquired::Admitted(permit) => permit,
        Acquired::Shed | Acquired::TimedOut => return busy_response(),
    };
    // The shell-set Content-Disposition of a /file Range request, derived from
    // the identifier before the request is moved onto the blocking thread.
    let content_disp = (parsed.kind == RequestKind::FileDownload
        && headers.contains_key(header::RANGE))
    .then(|| content_disposition(&parsed.identifier))
    .flatten();
    let credential_dependent = match parsed.kind {
        RequestKind::FileDownload => state.has_file_preflight,
        _ => state.has_preflight,
    };

    // The engine commits the head (status + headers) on the oneshot, then streams
    // body chunks on the bounded mpsc as it produces them. A slow client
    // stalls the engine thread; a disconnect drops the receiver, which
    // the engine's cancelled() poll and a failed body send both observe, aborting
    // the work. The pool permit is held for the whole dispatch — released when the
    // blocking task ends (after the last chunk), restoring the concurrency bound.
    // `serve_image` may swap it for a Full permit when the engine is about to
    // decode a size-restricted image (`reclassify_to_full`).
    let (outcome_tx, outcome_rx) = oneshot::channel::<Outcome>();
    let (body_tx, body_rx) = mpsc::channel::<sink::BodyItem>(sink::BODY_CHANNEL_CAP);
    // Carry the request span (created by OtelAxumLayer) onto the blocking thread
    // so the engine's coarse span nests under it and its trace context is what
    // stamps the C++ engine logs.
    let request_span = tracing::Span::current();
    tokio::task::spawn_blocking(move || {
        let mut permit = Some(permit);
        let _entered = request_span.enter();
        dispatch_engine(
            &state,
            &parsed,
            &method,
            &uri,
            &headers,
            &mut permit,
            outcome_tx,
            body_tx,
        );
    });
    let mut response = match outcome_rx.await {
        // JSON / redirect / error / HEAD: a complete response, returned as built.
        Ok(Outcome::Complete(response)) => response,
        // Image / file: stream the body as the engine produces it, adding the
        // shell-set Content-Disposition header to the committed head.
        Ok(Outcome::StreamHead {
            status,
            headers: head,
        }) => {
            let mut response = sink::stream_response(status, head, body_rx);
            if let Some(cd) = content_disp {
                if response.status().is_success() {
                    response
                        .headers_mut()
                        .insert(header::CONTENT_DISPOSITION, cd);
                }
            }
            response
        }
        // The blocking task dropped the sender without sending — a panic the
        // sink's catch_unwind somehow escaped — maps to a bare 500.
        Err(_) => sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
    };
    if credential_dependent {
        mark_private(response.headers_mut());
    }
    response
}

/// Keep a response whose bytes depend on the caller's credentials out of
/// shared caches: `private` leads `Cache-Control` (replacing `public`, keeping
/// the engine's other directives; `private` alone when there were none), and
/// `Vary` lists `Cookie` and `Authorization`. Browser caching stays as the
/// engine set it; `no-store` would end it for every public tile behind a hook.
fn mark_private(h: &mut HeaderMap) {
    let directives: Vec<String> = h
        .get_all(header::CACHE_CONTROL)
        .iter()
        .filter_map(|v| v.to_str().ok())
        .flat_map(|v| v.split(','))
        .map(str::trim)
        .filter(|d| {
            !d.is_empty() && !d.eq_ignore_ascii_case("public") && !d.eq_ignore_ascii_case("private")
        })
        .map(str::to_owned)
        .collect();
    let value = std::iter::once("private".to_owned())
        .chain(directives)
        .collect::<Vec<_>>()
        .join(", ");
    h.insert(
        header::CACHE_CONTROL,
        HeaderValue::from_str(&value).expect("joined from visible-ASCII header values"),
    );
    append_vary(h, "Cookie");
    append_vary(h, "Authorization");
}

/// The blocking engine dispatch, run on a `spawn_blocking` thread under a pool
/// permit: access resolution (the preflight VM), path validation (realpath), and
/// the serve call (decode/encode, or raw `/file`). Split out of [`iiif`] so the
/// async runtime never blocks on the C++ engine. Redirects are handled by the
/// caller before the pool; file downloads are handled here, ahead of the IIIF
/// JSON/image kinds.
// Single-caller engine-dispatch glue, as `serve_image`.
#[allow(clippy::too_many_arguments)]
fn dispatch_engine(
    state: &AppState,
    parsed: &ParsedRequest,
    method: &Method,
    uri: &Uri,
    headers: &HeaderMap,
    permit: &mut Option<Permit>,
    outcome_tx: oneshot::Sender<Outcome>,
    body_tx: mpsc::Sender<sink::BodyItem>,
) {
    // A coarse engine span under the request span. Low-cardinality name; the
    // identifier rides as an attribute (never in the name). Its trace
    // context stamps the C++ engine's log lines (the child span_id, so engine
    // logs nest under this step), cleared when the scope ends so a reused
    // blocking thread can't leak a stale id to the next request.
    let span = tracing::info_span!(
        "sipi.serve",
        kind = ?parsed.kind,
        identifier = %parsed.identifier,
        otel.status_code = tracing::field::Empty,
        error.type = tracing::field::Empty,
    );
    let _enter = span.enter();
    let _trace =
        crate::telemetry::current_trace_context().map(|(t, s)| ffi::LogTraceScope::set(&t, &s));

    if parsed.kind == RequestKind::FileDownload {
        match file_access(state, parsed, method, uri, headers)
            .and_then(|access| resolve(&access.infile, &state.resolved_imgroot))
        {
            Ok(resolved) => serve_file(&resolved, headers, outcome_tx, body_tx),
            Err(resp) => complete(outcome_tx, *resp),
        }
        return;
    }

    // IIIF image / info.json / knora.json: resolve infile + permission via the
    // preflight hook (or a default path when no hook is defined).
    let access = match iiif_access(state, parsed, method, uri, headers) {
        Ok(a) => a,
        Err(resp) => return complete(outcome_tx, *resp),
    };

    // The IIIF image serve enforces auth itself (401 for anything but
    // allow/restrict/stream);
    // info.json renders the auth-service block at 401 instead of gating here;
    // knora.json is a DSP-internal metadata surface with no auth-challenge
    // shape of its own, so an unauthorized caller gets a bare 404 rather than a
    // signal that the resource exists.
    if matches!(parsed.kind, RequestKind::Iiif | RequestKind::KnoraJson)
        && !matches!(
            access.permission,
            SipiPermType::Allow | SipiPermType::Restrict | SipiPermType::Stream
        )
    {
        let status = if parsed.kind == RequestKind::Iiif {
            StatusCode::UNAUTHORIZED
        } else {
            StatusCode::NOT_FOUND
        };
        return complete(outcome_tx, sink::error_response(status));
    }

    let resolved = match resolve(&access.infile, &state.resolved_imgroot) {
        Ok(r) => r,
        Err(resp) => return complete(outcome_tx, *resp),
    };

    match parsed.kind {
        RequestKind::Iiif => {
            serve_image(
                &resolved,
                parsed,
                // Raw, percent-encoded request path — log/report context
                // only; never re-parsed by the engine.
                uri.path(),
                headers,
                *method == Method::HEAD,
                &access,
                &state.admission,
                permit,
                &state.public_hosts,
                outcome_tx,
                body_tx,
            );
        }
        RequestKind::InfoJson => complete(
            outcome_tx,
            serve_info_json(
                &resolved,
                parsed,
                headers,
                &access,
                &state.allowed_origins,
                &state.public_hosts,
                state.has_preflight,
            ),
        ),
        RequestKind::KnoraJson => complete(
            outcome_tx,
            serve_knora_json(
                &resolved,
                parsed,
                headers,
                &access,
                &state.allowed_origins,
                &state.public_hosts,
                state.has_preflight,
            ),
        ),
        RequestKind::Redirect | RequestKind::FileDownload => {
            unreachable!("redirect handled by caller, file above")
        }
    }
}

/// Deliver a complete (non-streaming) response — JSON, redirect, or an error —
/// on the outcome channel, consuming the sender. On a 4xx/5xx, mark the engine
/// span errored so Grafana can correlate failures (the outer OtelAxumLayer span
/// carries the HTTP status separately; `error.type` carries the status code).
fn complete(outcome_tx: oneshot::Sender<Outcome>, response: Response) {
    let status = response.status();
    if status.is_client_error() || status.is_server_error() {
        let span = tracing::Span::current();
        span.record("otel.status_code", "ERROR");
        span.record("error.type", status.as_str());
    }
    let _ = outcome_tx.send(Outcome::Complete(response));
}

/// Pool-full Throttling: a bare 503 with `Retry-After: 1` — no body,
/// no internal detail. The client should retry shortly. The shed is already
/// counted per partition inside [`Admission`]; this only builds the response.
fn busy_response() -> Response {
    let mut response = sink::error_response(StatusCode::SERVICE_UNAVAILABLE);
    response
        .headers_mut()
        .insert(header::RETRY_AFTER, HeaderValue::from_static("1"));
    response
}

// ── Access resolution (preflight) ───────────────────────────────────────────

/// The permission vocabulary in discriminant order: the index into
/// [`DECISIONS`], and the value set the OTLP bridge fans the counter over.
pub(crate) const PERMISSIONS: [SipiPermType; 8] = [
    SipiPermType::Allow,
    SipiPermType::Login,
    SipiPermType::Clickthrough,
    SipiPermType::Kiosk,
    SipiPermType::External,
    SipiPermType::Restrict,
    SipiPermType::Deny,
    SipiPermType::Stream,
];

/// Resolved preflight decisions per permission (process-global; read by the OTel
/// bridge in `crate::metrics`). `stream` serves exactly as `allow` (ADR-0027), so
/// nothing in a response distinguishes that population — and a bare `stream` count
/// only says it is non-zero. Counting every permission gives it the denominator
/// that sizes an enforcement decision.
static DECISIONS: [AtomicU64; PERMISSIONS.len()] = [const { AtomicU64::new(0) }; PERMISSIONS.len()];

/// Cumulative decisions carrying `permission`, for the OTLP bridge.
pub(crate) fn decisions(permission: SipiPermType) -> u64 {
    DECISIONS[permission as usize].load(Ordering::Relaxed)
}

/// The hook's wire string for a permission — the inverse of
/// [`permission_from_str`], and the counter's attribute value. Exhaustive on
/// purpose: a new permission type does not compile until it has a string here.
pub(crate) fn permission_str(permission: SipiPermType) -> &'static str {
    match permission {
        SipiPermType::Allow => "allow",
        SipiPermType::Login => "login",
        SipiPermType::Clickthrough => "clickthrough",
        SipiPermType::Kiosk => "kiosk",
        SipiPermType::External => "external",
        SipiPermType::Restrict => "restrict",
        SipiPermType::Deny => "deny",
        SipiPermType::Stream => "stream",
    }
}

/// Resolve the infile + permission for an IIIF / info / knora request: run the
/// `pre_flight` hook when one is defined (it returns the infile), else build the
/// default `imgroot/prefix/identifier` path with `allow`.
fn iiif_access(
    state: &AppState,
    parsed: &ParsedRequest,
    method: &Method,
    uri: &Uri,
    headers: &HeaderMap,
) -> Result<Access, Box<Response>> {
    if state.has_preflight {
        // Opt-in burst-coalescing cache in front of the hook (`None` unless a TTL
        // was configured): a hit skips the whole FFI + Lua + dsp-api round-trip.
        // Keyed on (prefix, identifier, raw Cookie, raw Authorization) — distinct
        // credentials never share a decision. The key does NOT cover the rest of the
        // request the hook can read via `server.*` (the path, other headers such as
        // `X-Api-Key`, host, client IP); the cache is only correct for a hook whose
        // decision is a pure function of the keyed fields (see
        // `crate::preflight_cache` and `docs/src/lua/index.md`). The key is
        // query-free by design and never will cover the raw query string — that
        // would turn a query-string cache-buster into an unbounded amplifier
        // against dsp-api; `build_request_data` is the structural guard (its
        // explicit `RequestData` literal keeps the preflight view query- and
        // body-free). Only plain outcomes are cached (a hook `direct_response` is
        // request-specific).
        let cache_key = state.preflight_cache.as_ref().map(|_| {
            preflight_cache::make_key(
                &parsed.prefix,
                &parsed.identifier,
                header_str(headers, header::COOKIE.as_str()).as_deref(),
                header_str(headers, header::AUTHORIZATION.as_str()).as_deref(),
            )
        });
        if let (Some(cache), Some(key)) = (state.preflight_cache.as_ref(), cache_key.as_ref()) {
            if let Some(hit) = cache.get(key, Instant::now()) {
                return Ok(access_from(PreflightOutcome {
                    permission: hit.permission,
                    kv: hit.kv,
                }));
            }
        }

        let lua = state
            .lua
            .as_ref()
            .ok_or_else(|| Box::new(sink::error_response(StatusCode::INTERNAL_SERVER_ERROR)))?;
        // Run the hook under a `sipi.preflight` span; the outbound traceparent
        // rides on the request data (host-side injection, ADR-0017), so the
        // hook's dsp-api call nests under this span.
        let reply = {
            let _span = tracing::info_span!("sipi.preflight").entered();
            let req = build_request_data(method, uri, headers, &state.public_hosts);
            lua.preflight(req, &parsed.prefix, &parsed.identifier)
        };
        let outcome = match reply {
            Ok(scripting::PreflightReply::Decision { permission, kv }) => PreflightOutcome {
                permission: permission_from_str(&permission),
                kv,
            },
            Ok(scripting::PreflightReply::Direct {
                status,
                headers,
                body,
            }) => {
                return Err(Box::new(sink::direct_response(status, headers, body)));
            }
            // A killed or failed hook is a bare 500 and is never cached.
            Err(_) => {
                return Err(Box::new(sink::error_response(
                    StatusCode::INTERNAL_SERVER_ERROR,
                )));
            }
        };
        if let (Some(cache), Some(key)) = (state.preflight_cache.as_ref(), cache_key.as_ref()) {
            cache.insert(
                key,
                preflight_cache::CachedDecision {
                    permission: outcome.permission,
                    kv: outcome.kv.clone(),
                },
                Instant::now(),
            );
        }
        Ok(access_from(outcome))
    } else {
        Ok(Access {
            infile: path::build_request_path(
                &state.imgroot,
                &parsed.prefix,
                &parsed.identifier,
                state.prefix_as_path,
            ),
            permission: SipiPermType::Allow,
            kv: Vec::new(),
        })
    }
}

/// Resolve the infile + permission for a `/file` download: build the path, then
/// run `file_pre_flight` on it when defined.
/// A restrict decision is rejected here (403); any other non-allow
/// permission is rejected here (401).
fn file_access(
    state: &AppState,
    parsed: &ParsedRequest,
    method: &Method,
    uri: &Uri,
    headers: &HeaderMap,
) -> Result<Access, Box<Response>> {
    let built = path::build_request_path(
        &state.imgroot,
        &parsed.prefix,
        &parsed.identifier,
        state.prefix_as_path,
    );
    if !state.has_file_preflight {
        return Ok(Access {
            infile: built,
            permission: SipiPermType::Allow,
            kv: Vec::new(),
        });
    }
    let lua = state
        .lua
        .as_ref()
        .ok_or_else(|| Box::new(sink::error_response(StatusCode::INTERNAL_SERVER_ERROR)))?;
    // Run the hook under a `sipi.file_preflight` span; the outbound
    // traceparent rides on the request data (host-side injection, ADR-0017).
    let reply = {
        let _span = tracing::info_span!("sipi.file_preflight").entered();
        let req = build_request_data(method, uri, headers, &state.public_hosts);
        lua.file_preflight(req, &built)
    };
    let outcome = match reply {
        Ok(scripting::PreflightReply::Decision { permission, kv }) => PreflightOutcome {
            permission: permission_from_str(&permission),
            kv,
        },
        Ok(scripting::PreflightReply::Direct {
            status,
            headers,
            body,
        }) => {
            return Err(Box::new(sink::direct_response(status, headers, body)));
        }
        Err(_) => {
            return Err(Box::new(sink::error_response(
                StatusCode::INTERNAL_SERVER_ERROR,
            )));
        }
    };
    // Folded before the dispatch, not inside it: a refused decision is as much a
    // resolved decision as a served one, and the counter needs both.
    let access = access_from(outcome);
    match access.permission {
        // `stream` expresses "consume in place, do not hand over"; it carries no
        // restriction SIPI enforces today and serves exactly as `allow`
        // (ADR-0027).
        SipiPermType::Allow | SipiPermType::Stream => Ok(access),
        // A restrict decision cannot be applied to a raw `/file` download (no
        // clamp or watermark point exists on that path), so it is refused
        // rather than served at full fidelity.
        SipiPermType::Restrict => Err(Box::new(sink::error_response(StatusCode::FORBIDDEN))),
        _ => Err(Box::new(sink::error_response(StatusCode::UNAUTHORIZED))),
    }
}

/// Map a validated permission string (the hook's vocabulary) onto the seam
/// enum the serve paths dispatch on.
fn permission_from_str(s: &str) -> SipiPermType {
    match s {
        "allow" => SipiPermType::Allow,
        "login" => SipiPermType::Login,
        "clickthrough" => SipiPermType::Clickthrough,
        "kiosk" => SipiPermType::Kiosk,
        "external" => SipiPermType::External,
        "restrict" => SipiPermType::Restrict,
        "stream" => SipiPermType::Stream,
        _ => SipiPermType::Deny,
    }
}

/// The request as the Lua bindings see it: lowercase header names (axum
/// guarantees them), one entry per cookie with original-case names, and the
/// host-injected outbound traceparent.
fn build_request_data(
    method: &Method,
    uri: &Uri,
    headers: &HeaderMap,
    public_hosts: &[String],
) -> scripting::bindings::RequestData {
    let header_vec: Vec<(String, String)> = headers
        .iter()
        .filter_map(|(n, v)| {
            v.to_str()
                .ok()
                .map(|v| (n.as_str().to_owned(), v.to_owned()))
        })
        .collect();
    let cookies = parse_cookies(headers);
    let (_scheme, host) = forwarded(headers, public_hosts);
    // The preflight view is deliberately query- and body-free: `get_params`,
    // `post_params`, `request_params`, `uploads`, and `content` are always
    // empty, and `uri` carries the path only (never the query). The
    // preflight-cache key (`preflight_cache::make_key`) covers only
    // `(prefix, identifier, Cookie, Authorization)` — a hook decision that
    // depended on any field left out of this literal would be served a stale
    // cached decision for a different request. An explicit literal (no
    // `..Default::default()`) means a new `RequestData` field fails to
    // compile here until it is deliberately classified as keyed or excluded.
    scripting::bindings::RequestData {
        method: method.as_str().to_owned(),
        client_ip: client_ip(headers),
        client_port: 0,
        secure: false,
        host,
        uri: uri.path().to_owned(),
        headers: header_vec,
        cookies,
        get_params: Vec::new(),
        post_params: Vec::new(),
        request_params: Vec::new(),
        uploads: Vec::new(),
        content: Vec::new(),
        content_type: String::new(),
        jwt_secret: String::new(),
        docroot: None,
        traceparent: crate::telemetry::current_traceparent(),
    }
}

/// Fold a [`PreflightOutcome`] into an [`Access`], taking the hook's `infile`
/// (empty for `deny`, which then fails R2 → 404, matching the C++ `access()`).
fn access_from(outcome: PreflightOutcome) -> Access {
    // Counted here rather than at `permission_from_str` because a preflight-cache
    // hit never re-parses the hook's string: this is the one point every
    // resolved decision — hook or cache, IIIF or `/file` — passes through.
    DECISIONS[outcome.permission as usize].fetch_add(1, Ordering::Relaxed);
    let infile = outcome.get("infile").unwrap_or_default().to_owned();
    Access {
        infile,
        permission: outcome.permission,
        kv: outcome.kv,
    }
}

/// R2: realpath + image-root containment. Maps to a bare error response.
fn resolve(infile: &str, resolved_root: &str) -> Result<String, Box<Response>> {
    match path::validate_resolved_path(infile, resolved_root) {
        Resolved::Ok(p) => Ok(p),
        Resolved::NotFound => Err(Box::new(sink::error_response(StatusCode::NOT_FOUND))),
        Resolved::Traversal => Err(Box::new(sink::error_response(StatusCode::BAD_REQUEST))),
    }
}

// ── Serve handlers ──────────────────────────────────────────────────────────

/// IIIF image via `sipi_serve_image`, honouring a `restrict` decision's
/// `size`/`watermark` from the preflight kv channel.
// Single-caller engine-dispatch glue; the arguments are the distinct FFI inputs,
// not a bundle worth a struct.
#[allow(clippy::too_many_arguments)]
fn serve_image(
    resolved: &str,
    parsed: &ParsedRequest,
    request_uri: &str,
    headers: &HeaderMap,
    is_head: bool,
    access: &Access,
    admission: &Admission,
    permit: &mut Option<Permit>,
    public_hosts: &[String],
    outcome_tx: oneshot::Sender<Outcome>,
    body_tx: mpsc::Sender<sink::BodyItem>,
) {
    let params: ffi::SipiIiifParams = parsed
        .params
        .expect("an Iiif request always carries parsed params")
        .into();
    let (scheme, host) = forwarded(headers, public_hosts);

    // Every C string must outlive the synchronous sipi_serve_image call.
    let (c_resolved, c_prefix, c_identifier) = match (
        CString::new(resolved),
        CString::new(parsed.prefix.as_str()),
        CString::new(parsed.identifier.as_str()),
    ) {
        (Ok(a), Ok(b), Ok(c)) => (a, b, c),
        _ => return complete(outcome_tx, sink::error_response(StatusCode::BAD_REQUEST)),
    };
    let c_host = CString::new(host).unwrap_or_default();
    let c_scheme = CString::new(scheme).unwrap_or_default();
    let c_uri = CString::new(request_uri).unwrap_or_default();
    // restrict size + watermark from the preflight kv (None when not restricted).
    let restricted = access.permission == SipiPermType::Restrict;
    let size = restricted.then(|| access_kv_str(access, "size")).flatten();
    let watermark = restricted.then(|| restrict_watermark(access)).flatten();
    // A restrict decision that supplies neither a size cap nor a watermark
    // arrives at the seam indistinguishable from `allow` (0, 0 and NULL) —
    // refuse it here rather than let the engine pass the original through.
    if restricted && size.is_none() && watermark.is_none() {
        return complete(outcome_tx, sink::error_response(StatusCode::FORBIDDEN));
    }
    // A watermark the seam cannot carry is a 500, never "no watermark": the
    // restriction below counts it as present.
    let Ok(c_watermark) = watermark.map(CString::new).transpose() else {
        return complete(
            outcome_tx,
            sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
        );
    };
    // The shell decides (RW, RH) from its own shape probe. A failed probe is a
    // 500: sending 0, 0 instead would serve the image unrestricted.
    let (restricted_width, restricted_height) = match size {
        None => (0, 0),
        Some(size) => {
            let decided = ffi::image_dims(resolved)
                .map_err(|_| RestrictRefusal::Misconfigured)
                .and_then(|native| {
                    restriction(
                        size,
                        c_watermark.is_some(),
                        native.width,
                        native.height,
                        &parsed.identifier,
                    )
                });
            match decided {
                Ok((w, h)) => (w.get(), h.get()),
                Err(RestrictRefusal::Refuse) => {
                    return complete(outcome_tx, sink::error_response(StatusCode::FORBIDDEN))
                }
                Err(RestrictRefusal::Misconfigured) => {
                    return complete(
                        outcome_tx,
                        sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
                    )
                }
            }
        }
    };

    // A size restriction makes the engine decode the whole image, so the lane
    // swap runs from the engine's before-decode hook: a cache hit or HEAD
    // request, which decodes nothing, keeps its lane.
    let size_restricted = restricted_width != 0;
    let mut lane_swap = LaneSwap {
        admission,
        permit: &mut *permit,
    };
    let req = SipiServeRequest {
        resolved_path: c_resolved.as_ptr(),
        prefix: c_prefix.as_ptr(),
        identifier: c_identifier.as_ptr(),
        params,
        restricted_width,
        restricted_height,
        watermark_path: c_watermark
            .as_ref()
            .map_or(std::ptr::null(), |c| c.as_ptr()),
        // The engine honours X-Forwarded-Proto for the canonical Link header
        // (SIPI serves plain HTTP behind Traefik); the cache key stays scheme-free.
        forwarded_proto: c_scheme.as_ptr(),
        forwarded_host: c_host.as_ptr(),
        request_uri: c_uri.as_ptr(),
        is_head: i32::from(is_head),
        // A handled decode/convert/write error is reported as a side-channel
        // (never part of the response); `report_ctx` reuses `c_uri` — the
        // request URI already lives on the request, so it isn't duplicated
        // into the flat `SipiImageErrorReport` struct.
        report_error: Some(ffi::report_image_error),
        report_ctx: c_uri.as_ptr() as *mut std::ffi::c_void,
        before_decode: size_restricted
            .then_some(swap_lane_before_decode as ffi::SipiBeforeDecodeFn),
        before_decode_ctx: std::ptr::from_mut(&mut lane_swap).cast(),
    };

    // Wall-clock anchor for the engine child spans: the per-phase offsets the
    // engine records (monotonic) are added onto this. Captured just before the
    // call, a hair earlier than the engine's own monotonic origin — an accepted,
    // sub-microsecond skew (one struct build, no I/O, in between).
    let engine_start = SystemTime::now();
    // SAFETY: every pointer in `req` outlives this synchronous call; the seam
    // guards C++ exceptions (→ status code, never an unwind into Rust). The
    // CORS headers are added by the caller (`iiif`).
    sink::serve_streaming(outcome_tx, body_tx, |resp: &SipiResponse| unsafe {
        ffi::sipi_serve_image(&req, resp)
    });
    // Read back what the engine observed about this call, once. The decode-memory
    // estimate feeds a metric and the per-phase timings feed child spans; metrics
    // and traces are independent pipelines, so the take is unconditional.
    let observed = ffi::serve_timings_take();
    crate::metrics::record_decode_estimate(observed.decode_estimate_bytes);
    // Compare the lane the request ran in (after any re-classification) against
    // the engine's precise verdict (estimate ≥ threshold), bumping the
    // disagreement counter on drift. A zero estimate (cache hit / HEAD /
    // passthrough) records nothing.
    // No permit: the lane swap failed and the engine refused before its
    // estimate, so there is nothing to compare.
    if let Some(held) = permit.as_ref().map(Permit::kind) {
        admission.record_classification(held, observed.decode_estimate_bytes);
    }
    // Break the opaque engine span open: one child span per phase (decode,
    // encode, …) under `sipi.serve`, from the timings the engine just recorded.
    emit_engine_phase_spans(engine_start, &observed);
}

/// The `before_decode_ctx` of a size-restricted serve: what
/// [`swap_lane_before_decode`] needs to swap the permit.
struct LaneSwap<'a> {
    admission: &'a Admission,
    permit: &'a mut Option<Permit>,
}

/// The engine's before-decode hook for a size-restricted serve: runs
/// [`reclassify_to_full`] and refuses the decode (non-zero, a 503) when the
/// Full lane sheds or times out. A panic refuses too; it never unwinds into C++.
extern "C" fn swap_lane_before_decode(ctx: *mut std::ffi::c_void) -> std::ffi::c_int {
    // SAFETY: `ctx` is the `LaneSwap` `serve_image` passes; it outlives the
    // synchronous `sipi_serve_image` call, and nothing else touches it meanwhile.
    let swap = unsafe { &mut *ctx.cast::<LaneSwap<'_>>() };
    let swapped = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        reclassify_to_full(swap.admission, swap.permit)
    }));
    std::ffi::c_int::from(!matches!(swapped, Ok(true)))
}

/// Move a size-restricted request from the Tile to the Full lane. A size
/// restriction makes the engine decode the whole image (ADR-0028), which is
/// Full-lane work (ADR-0022), but the lane was picked before the preflight.
/// The Tile permit is dropped before the Full acquire, so the request never
/// holds one permit while it waits for the other. Returns false when the Full
/// lane sheds or times out; `permit` is then empty. A permit that is already
/// Full is kept.
///
/// Runs on the `spawn_blocking` thread, so it blocks on the runtime handle;
/// the wait is bounded by the admission queue timeout.
fn reclassify_to_full(admission: &Admission, permit: &mut Option<Permit>) -> bool {
    if permit.as_ref().map(Permit::kind) != Some(AdmissionKind::Tile) {
        return true;
    }
    *permit = None;
    match tokio::runtime::Handle::current().block_on(admission.acquire(AdmissionKind::Full)) {
        Acquired::Admitted(full) => {
            *permit = Some(full);
            true
        }
        Acquired::Shed | Acquired::TimedOut => false,
    }
}

/// Mint an OTel child span under the current `sipi.serve` span for each engine
/// phase the just-returned `sipi_serve_image` recorded, placing it on the trace
/// timeline from the FFI-returned per-phase offsets (relative to `engine_start`).
/// Runs on the same blocking thread as the FFI call, so `Span::current()` is
/// `sipi.serve`. Returns immediately when trace export is off (no throwaway spans
/// on the hot path) or when the engine recorded nothing (cache hit / HEAD /
/// passthrough). A phase that exited via an exception (`failed`) is marked
/// `Status=Error`.
fn emit_engine_phase_spans(engine_start: SystemTime, timings: &ffi::SipiServeTimings) {
    use opentelemetry::trace::{Span as _, Status, Tracer as _};
    use tracing_opentelemetry::OpenTelemetrySpanExt as _;

    if !crate::telemetry::tracing_active() {
        return;
    }
    if !timings.present.iter().any(|&p| p != 0) {
        return;
    }
    let parent_cx = tracing::Span::current().context();
    let tracer = opentelemetry::global::tracer("sipi");
    for i in 0..ffi::PHASE_COUNT {
        if timings.present[i] == 0 {
            continue;
        }
        let start = engine_start + Duration::from_nanos(timings.start_ns[i]);
        let end = start + Duration::from_nanos(timings.dur_ns[i]);
        let mut span = tracer
            .span_builder(ffi::PHASE_SPAN_NAMES[i])
            .with_start_time(start)
            .start_with_context(&tracer, &parent_cx);
        if timings.failed[i] != 0 {
            span.set_status(Status::error("engine phase failed"));
        }
        span.end_with_timestamp(end);
    }
}

/// Raw `/file` passthrough via `sipi_serve_file` (Range/206). The CORS headers
/// and, on a Range request, the identifier-derived Content-Disposition are added
/// by the caller (`iiif`).
fn serve_file(
    resolved: &str,
    headers: &HeaderMap,
    outcome_tx: oneshot::Sender<Outcome>,
    body_tx: mpsc::Sender<sink::BodyItem>,
) {
    let c_resolved = match CString::new(resolved) {
        Ok(c) => c,
        Err(_) => return complete(outcome_tx, sink::error_response(StatusCode::BAD_REQUEST)),
    };
    let range = header_str(headers, header::RANGE.as_str());
    let c_range = range.as_deref().and_then(|r| CString::new(r).ok());
    let range_ptr = c_range.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());

    // SAFETY: `c_resolved`/`c_range` outlive the synchronous call; the seam guards.
    sink::serve_streaming(outcome_tx, body_tx, |resp: &SipiResponse| unsafe {
        ffi::sipi_serve_file(c_resolved.as_ptr(), range_ptr, resp)
    });
}

/// CORS preflight (`OPTIONS`): echo the Origin + credentials, advertise the
/// served methods (`GET, POST, PUT, DELETE`), and echo the requested headers.
/// Engine-independent, so it serves without the readiness gate.
pub async fn cors_preflight(State(state): State<Arc<AppState>>, headers: HeaderMap) -> Response {
    cors_preflight_response(&headers, &state.allowed_origins)
}

/// The CORS-preflight response (`OPTIONS`): `204`, the served methods, the
/// echoed Origin and credentials (gated by [`cors_allow`] against
/// `allowlist`), `Vary: Origin` on every decision, and the requested
/// headers. Shared by the [`cors_preflight`] handler and the docroot
/// fileserver's OPTIONS path. Answers `204` (not `200`) to keep the CORS
/// credentials contract intact.
fn cors_preflight_response(headers: &HeaderMap, allowlist: &[String]) -> Response {
    let mut builder = Response::builder().status(StatusCode::NO_CONTENT).header(
        header::ACCESS_CONTROL_ALLOW_METHODS,
        "GET, POST, PUT, DELETE",
    );
    let origin = header_str(headers, "origin");
    if cors_allow(origin.as_deref(), allowlist) == CorsDecision::Allow {
        if let Some(origin) = origin.and_then(|o| HeaderValue::from_str(&o).ok()) {
            builder = builder
                .header(header::ACCESS_CONTROL_ALLOW_ORIGIN, origin)
                .header(header::ACCESS_CONTROL_ALLOW_CREDENTIALS, "true");
        }
    }
    if let Some(req_headers) = header_str(headers, "access-control-request-headers")
        .and_then(|h| HeaderValue::from_str(&h).ok())
    {
        builder = builder.header(header::ACCESS_CONTROL_ALLOW_HEADERS, req_headers);
    }
    let mut response = builder
        .body(Body::empty())
        .unwrap_or_else(|_| sink::error_response(StatusCode::INTERNAL_SERVER_ERROR));
    append_vary(response.headers_mut(), "Origin");
    response
}

// ── Configured Lua routes ────────────────────────────────────────────────────

/// One parsed multipart file upload, spooled to a temp file (`server.uploads`).
/// The `NamedTempFile` handle is held alongside until the route call returns, so
/// the on-disk file the engine opens by `tmpname` survives the call and is then
/// auto-deleted.
struct UploadPart {
    fieldname: String,
    origname: String,
    tmpname: String,
    mime: String,
    size: u64,
}

/// Map an HTTP method name to the axum `MethodFilter` (the config's verbs).
fn method_filter(method: &str) -> Option<MethodFilter> {
    match method {
        "GET" => Some(MethodFilter::GET),
        "HEAD" => Some(MethodFilter::HEAD),
        "POST" => Some(MethodFilter::POST),
        "PUT" => Some(MethodFilter::PUT),
        "DELETE" => Some(MethodFilter::DELETE),
        "OPTIONS" => Some(MethodFilter::OPTIONS),
        _ => None,
    }
}

/// Build the axum `MethodRouter` for one configured Lua route: the route's method
/// dispatches to [`serve_lua_route`] with the route's script captured, capped at
/// the configured POST size (the transport's `Connection` body limit). Returns
/// `None` for an unsupported method verb.
#[must_use]
pub fn lua_route_method_router(
    state: Arc<AppState>,
    entry: &ffi::RouteEntry,
) -> Option<MethodRouter<Arc<AppState>>> {
    let filter = method_filter(&entry.method)?;
    let script = entry.script.clone();
    let max_post = state.max_post_size;
    let handler = on(filter, move |req: Request| {
        let state = Arc::clone(&state);
        let script = script.clone();
        // A configured route never injects `server.docroot` (docroot = None),
        // matching the C++ `script_handler` (only `file_handler` injects it).
        async move { serve_lua_script(state, script, None, req).await }
    });
    // Cap the request body at max_post_size (oversized → 413) and bound the
    // body read by time: a slow/trickling client is cut off before it
    // ever reaches `serve_lua_script`'s `admission.acquire`, so it never holds
    // a Full permit while still streaming its body.
    let handler: MethodRouter<Arc<AppState>> = handler.layer(DefaultBodyLimit::max(max_post));
    let handler: MethodRouter<Arc<AppState>> =
        handler.layer(tower_http::timeout::RequestBodyTimeoutLayer::new(
            crate::config::body_read_timeout_from_env(),
        ));
    Some(handler)
}

/// The maximum number of multipart parts (file or non-file fields combined)
/// `serve_lua_script` will process in one request; the request beyond
/// this is rejected with 400, independent of `max_post_size`.
const MAX_MULTIPART_PARTS: u32 = 64;

/// Run a Lua script against the request on the blocking pool. The script's own
/// Lua sets the response status/headers/body (no shell-injected CORS: the
/// script owns its headers). Shared by configured Lua routes (`docroot = None`)
/// and docroot
/// `.lua`/`.elua` scripts (`docroot = Some`, injecting `server.docroot`).
async fn serve_lua_script(
    state: Arc<AppState>,
    script: String,
    docroot: Option<String>,
    req: Request,
) -> Response {
    let method = req.method().clone();
    let uri = req.uri().clone();
    let headers = req.headers().clone();
    let content_type = header_str(&headers, header::CONTENT_TYPE.as_str()).unwrap_or_default();

    // Request fields, owned so they can move onto the blocking thread.
    let (_scheme, host) = forwarded(&headers, &state.public_hosts);
    let client_ip = client_ip(&headers);
    let header_vec: Vec<(String, String)> = headers
        .iter()
        .filter_map(|(n, v)| {
            v.to_str()
                .ok()
                .map(|v| (n.as_str().to_owned(), v.to_owned()))
        })
        .collect();
    let cookies = parse_cookies(&headers);
    let get_params = uri.query().map(parse_form_encoded).unwrap_or_default();

    // Body: multipart → spooled uploads; urlencoded → POST params; else raw bytes.
    // The DefaultBodyLimit layer caps multipart; to_bytes caps the raw path.
    let mut uploads: Vec<UploadPart> = Vec::new();
    let mut tempfiles: Vec<NamedTempFile> = Vec::new();
    let mut post_params: Vec<(String, String)> = Vec::new();
    let mut body: Vec<u8> = Vec::new();

    if content_type.starts_with("multipart/form-data") {
        let mut multipart = match Multipart::from_request(req, &()).await {
            Ok(m) => m,
            // 413 (over the body cap) or 400 (malformed) — the rejection's own status.
            Err(rej) => return rej.into_response(),
        };
        let mut part_count: u32 = 0;
        loop {
            match multipart.next_field().await {
                Ok(Some(mut field)) => {
                    // Cap the part count independent of the body-size limit:
                    // a request built from many tiny parts stays under
                    // `max_post_size` while still costing a field-processing pass
                    // (and a temp file, for file parts) per part.
                    part_count += 1;
                    if part_count > MAX_MULTIPART_PARTS {
                        return sink::error_response(StatusCode::BAD_REQUEST);
                    }
                    let fieldname = field.name().unwrap_or_default().to_owned();
                    let origname = field.file_name().unwrap_or_default().to_owned();
                    let mime = field.content_type().unwrap_or_default().to_owned();
                    if origname.is_empty() {
                        // A non-file field is a POST form parameter (server.post) —
                        // small, so buffer it in memory.
                        let data = match field.bytes().await {
                            Ok(b) => b,
                            Err(rej) => return rej.into_response(),
                        };
                        post_params.push((fieldname, String::from_utf8_lossy(&data).into_owned()));
                    } else {
                        // A file part: stream chunks straight to the temp file so a
                        // single upload never buffers fully in memory. The whole-request
                        // size is still bounded by the DefaultBodyLimit layer
                        // (max_post_size is always finite, see config::DEFAULT_MAX_POST_SIZE).
                        let Ok(mut tf) = NamedTempFile::new() else {
                            return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR);
                        };
                        let mut size: u64 = 0;
                        loop {
                            match field.chunk().await {
                                Ok(Some(chunk)) => {
                                    if tf.write_all(&chunk).is_err() {
                                        return sink::error_response(
                                            StatusCode::INTERNAL_SERVER_ERROR,
                                        );
                                    }
                                    size += chunk.len() as u64;
                                }
                                Ok(None) => break,
                                Err(rej) => return rej.into_response(),
                            }
                        }
                        if tf.flush().is_err() {
                            return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR);
                        }
                        let tmpname = tf.path().to_string_lossy().into_owned();
                        uploads.push(UploadPart {
                            fieldname,
                            origname,
                            tmpname,
                            mime,
                            size,
                        });
                        tempfiles.push(tf);
                    }
                }
                Ok(None) => break,
                Err(rej) => return rej.into_response(),
            }
        }
    } else if has_request_body(&method) {
        let bytes = match to_bytes(req.into_body(), state.max_post_size).await {
            Ok(b) => b,
            Err(_) => return sink::error_response(StatusCode::PAYLOAD_TOO_LARGE),
        };
        if content_type.starts_with("application/x-www-form-urlencoded") {
            post_params = parse_form_encoded(std::str::from_utf8(&bytes).unwrap_or_default());
        }
        body = bytes.to_vec();
    }

    // Lua routes admit via the full partition: a script can do full-image-class
    // work (upload decode+encode) and its cost is unknowable up front, so it is
    // classed like a full decode — capped at `full_max` in advanced mode, keeping
    // the `tile_min` floor reachable by tiles — matching the full-lane memory
    // budget its decodes are charged to engine-side. Then run the (blocking)
    // Lua VM off the async runtime; a shed/timeout returns 503 + Retry-After.
    let permit = match state.admission.acquire(AdmissionKind::Full).await {
        Acquired::Admitted(permit) => permit,
        Acquired::Shed | Acquired::TimedOut => return busy_response(),
    };
    let (outcome_tx, outcome_rx) = oneshot::channel::<Outcome>();
    let (body_tx, body_rx) = mpsc::channel::<sink::BodyItem>(sink::BODY_CHANNEL_CAP);
    let request_span = tracing::Span::current();
    let method_str = method.as_str().to_owned();
    let uri_path = uri.path().to_owned();
    let Some(lua) = state.lua.clone() else {
        return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR);
    };
    tokio::task::spawn_blocking(move || {
        let _permit = permit;
        let _entered = request_span.enter();
        // Temp files outlive the route call: the script opens them by path
        // during execution, and they are deleted when this Vec drops (after
        // the call — a killed script still drops them).
        let _tempfiles = tempfiles;
        run_lua_route_blocking(
            &lua,
            &script,
            docroot.as_deref(),
            LuaRequest {
                method: &method_str,
                client_ip: &client_ip,
                host: &host,
                uri_path: &uri_path,
                headers: &header_vec,
                cookies: &cookies,
                get_params: &get_params,
                post_params: &post_params,
                content_type: &content_type,
                body: &body,
                uploads: &uploads,
            },
            outcome_tx,
            body_tx,
        );
    });
    match outcome_rx.await {
        Ok(Outcome::Complete(response)) => response,
        Ok(Outcome::StreamHead {
            status,
            headers: head,
        }) => sink::stream_response(status, head, body_rx),
        Err(_) => sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
    }
}

/// The owned, borrow-only view of a request handed to the blocking Lua dispatch.
struct LuaRequest<'a> {
    method: &'a str,
    client_ip: &'a str,
    host: &'a str,
    uri_path: &'a str,
    headers: &'a [(String, String)],
    cookies: &'a [(String, String)],
    get_params: &'a [(String, String)],
    post_params: &'a [(String, String)],
    content_type: &'a str,
    body: &'a [u8],
    uploads: &'a [UploadPart],
}

/// Build the request data (full request: body, uploads, params) and run the
/// route's script in the Rust Lua runtime against the streaming channels. Runs
/// on a `spawn_blocking` thread (the Lua VM + any decode/encode is blocking).
fn run_lua_route_blocking(
    lua: &scripting::LuaEnv,
    script: &str,
    docroot: Option<&str>,
    req: LuaRequest,
    outcome_tx: oneshot::Sender<Outcome>,
    body_tx: mpsc::Sender<sink::BodyItem>,
) {
    // Log only the basename — the full resolved path leaks the server's
    // filesystem layout to the OTel backend, and the basename identifies the route.
    let script_name = std::path::Path::new(script)
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or(script);
    let span = tracing::info_span!("sipi.lua_route", route_script = %script_name);
    let _enter = span.enter();
    let _trace =
        crate::telemetry::current_trace_context().map(|(t, s)| ffi::LogTraceScope::set(&t, &s));

    // The merged get+post view (`server.request`), get params first.
    let mut request_params = req.get_params.to_vec();
    request_params.extend_from_slice(req.post_params);
    // secure = false: SIPI runs plain HTTP behind Traefik (matches conn.secure()).
    let data = scripting::bindings::RequestData {
        method: req.method.to_owned(),
        client_ip: req.client_ip.to_owned(),
        client_port: 0,
        secure: false,
        host: req.host.to_owned(),
        uri: req.uri_path.to_owned(),
        headers: req.headers.to_vec(),
        cookies: req.cookies.to_vec(),
        get_params: req.get_params.to_vec(),
        post_params: req.post_params.to_vec(),
        request_params,
        uploads: req
            .uploads
            .iter()
            .map(|u| scripting::bindings::Upload {
                fieldname: u.fieldname.clone(),
                origname: u.origname.clone(),
                tmpname: u.tmpname.clone(),
                mimetype: u.mime.clone(),
                filesize: u.size,
            })
            .collect(),
        content: req.body.to_vec(),
        content_type: req.content_type.to_owned(),
        jwt_secret: String::new(), // stamped by LuaEnv
        docroot: docroot.map(str::to_owned),
        // Any `server.http` the route makes carries this span's traceparent,
        // so a downstream service continues the trace (host-side, ADR-0017).
        traceparent: crate::telemetry::current_traceparent(),
    };

    // The head commits through the oneshot on the first body byte; the sender
    // is shared with the post-run completion path (a body-less route sends a
    // Complete response instead).
    let outcome_slot = std::rc::Rc::new(std::cell::RefCell::new(Some(outcome_tx)));
    let commit_slot = std::rc::Rc::clone(&outcome_slot);
    let commit: scripting::bindings::CommitFn = Box::new(move |status, headers| {
        if let Some(tx) = commit_slot.borrow_mut().take() {
            let _ = tx.send(Outcome::StreamHead { status, headers });
        }
    });
    // Every body-channel send is bounded by the request's Lua deadline: a
    // client that stops reading stalls the axum body stream, fills the
    // bounded channel, and would otherwise pin this blocking thread forever,
    // reachable by any unauthenticated slow reader. On expiry the write fails as
    // client-gone and the script aborts.
    let write_deadline = std::time::Instant::now() + lua.limits().timeout;
    let write_tx = body_tx.clone();
    let write: scripting::bindings::WriteFn = Box::new(move |chunk| {
        send_body_until(
            &write_tx,
            Ok(axum::body::Bytes::copy_from_slice(chunk)),
            write_deadline,
        )
        .map_err(|_| scripting::bindings::ClientGone)
    });

    let outcome = lua.run_route(data, std::path::Path::new(script), commit, write);

    let take_tx = || outcome_slot.borrow_mut().take();
    match outcome.result {
        Ok(()) => {
            if let Some(tx) = take_tx() {
                // Body-less completion: the buffered head as a complete response.
                let (status, headers) = outcome.head;
                let _ = tx.send(Outcome::Complete(sink::direct_response(
                    status,
                    headers,
                    Vec::new(),
                )));
            }
        }
        Err(scripting::RouteFailure::NotFound) => {
            if let Some(tx) = take_tx() {
                let _ = tx.send(Outcome::Complete(sink::error_response(
                    StatusCode::NOT_FOUND,
                )));
            }
        }
        Err(scripting::RouteFailure::Killed(_)) => {
            if outcome.committed {
                // Post-commit kill: abort the stream so the client never sees
                // a clean EOF (the ADR-0023 kill contract). Bounded send — a
                // stalled client must not re-pin the thread the kill just
                // freed; if the abort can't be delivered in time, freeing the
                // thread wins and the connection dies by keep-alive timeout.
                let _ = send_body_until(&body_tx, Err(sink::BodyAbort), abort_deadline());
            } else if let Some(tx) = take_tx() {
                let _ = tx.send(Outcome::Complete(sink::error_response(
                    StatusCode::INTERNAL_SERVER_ERROR,
                )));
            }
        }
        Err(scripting::RouteFailure::Error(_)) => {
            if outcome.committed {
                // Post-commit error: abort the stream exactly like a
                // post-commit kill — dropping the sender would read as a
                // clean EOF, indistinguishable from a complete body (the
                // sink contract), silently serving truncated content as 200.
                let _ = send_body_until(&body_tx, Err(sink::BodyAbort), abort_deadline());
            } else if let Some(tx) = take_tx() {
                let _ = tx.send(Outcome::Complete(sink::error_response(
                    StatusCode::INTERNAL_SERVER_ERROR,
                )));
            }
        }
    }
}

/// The budget for delivering a stream abort to an already-stalled client.
fn abort_deadline() -> std::time::Instant {
    std::time::Instant::now() + Duration::from_secs(2)
}

/// Bounded body-channel send from a blocking thread: `try_send` polled until
/// `deadline`. `Err` = the client is gone or stopped reading past the budget;
/// either way the caller must give up rather than block indefinitely.
fn send_body_until(
    tx: &tokio::sync::mpsc::Sender<sink::BodyItem>,
    mut item: sink::BodyItem,
    deadline: std::time::Instant,
) -> Result<(), ()> {
    use tokio::sync::mpsc::error::TrySendError;
    loop {
        match tx.try_send(item) {
            Ok(()) => return Ok(()),
            Err(TrySendError::Closed(_)) => return Err(()),
            Err(TrySendError::Full(returned)) => {
                if std::time::Instant::now() >= deadline {
                    return Err(());
                }
                item = returned;
                std::thread::sleep(Duration::from_millis(5));
            }
        }
    }
}

// ── /server docroot fileserver ───────────────────────────────────────────────

/// Build the `MethodRouter` for the `/server` docroot fileserver, or `None` when
/// no docroot+wwwroute is configured. GET/HEAD/POST serve a static file
/// (Range/206 + MIME) or run a docroot `.lua`/`.elua` script; OPTIONS is the
/// CORS preflight.
/// [`crate::app`] registers it at the wwwroute prefix.
#[must_use]
pub fn docroot_method_router(state: Arc<AppState>) -> Option<MethodRouter<Arc<AppState>>> {
    if state.docroot.is_empty() || state.wwwroute.is_empty() {
        return None;
    }
    let max_post = state.max_post_size;
    // Capture state in the closure (the `serve_lua_route` pattern) rather than a
    // `State` extractor: mixing `State` with the consuming `Request` extractor in
    // a free fn trips axum's handler-marker inference. OPTIONS is folded into the
    // one handler (dispatched inside `serve_docroot`) so the method router is a
    // single closure — chaining `.options()` here would under-constrain the
    // state-type inference.
    let handler = on(
        MethodFilter::GET
            .or(MethodFilter::HEAD)
            .or(MethodFilter::POST)
            .or(MethodFilter::OPTIONS),
        move |req: Request| {
            let state = Arc::clone(&state);
            async move { serve_docroot(state, req).await }
        },
    );
    // Cap docroot `.lua`/`.elua` POST bodies at the configured size (static GETs
    // ignore it) and bound the body read by time, matching
    // `lua_route_method_router`.
    let handler: MethodRouter<Arc<AppState>> = handler.layer(DefaultBodyLimit::max(max_post));
    let handler: MethodRouter<Arc<AppState>> =
        handler.layer(tower_http::timeout::RequestBodyTimeoutLayer::new(
            crate::config::body_read_timeout_from_env(),
        ));
    Some(handler)
}

/// The `/server` docroot handler: serve a static file (Range/206 + MIME) or
/// execute a docroot `.lua`/`.elua` script with `server.docroot` injected. The
/// path is containment-validated against the realpath'd docroot before any
/// open (an R1/R2 traversal guard). An
/// interior NUL or other control char in the decoded path → 400 (this handler's
/// own guard, `decode_path_suffix`, R7 — strictly wider than the IIIF
/// catch-all's own R7, which rejects NUL only; see that check's comment for
/// why).
async fn serve_docroot(state: Arc<AppState>, req: Request) -> Response {
    // OPTIONS is the CORS preflight (engine-independent), folded into this handler
    // so the method router stays a single closure.
    if req.method() == Method::OPTIONS {
        return cors_preflight_response(req.headers(), &state.allowed_origins);
    }
    // Strip the wwwroute prefix, then percent-decode the remainder to the on-disk
    // suffix (C++: `uri.substr(route.len())`, then `docroot + uri`).
    let www = state.wwwroute.trim_end_matches('/');
    let Some(suffix_enc) = req.uri().path().strip_prefix(www) else {
        return sink::error_response(StatusCode::NOT_FOUND);
    };
    let Some(suffix) = decode_path_suffix(suffix_enc) else {
        return sink::error_response(StatusCode::BAD_REQUEST);
    };
    // R1: traversal components in the decoded suffix → 400.
    if path::contains_traversal(suffix.trim_start_matches('/')) {
        return sink::error_response(StatusCode::BAD_REQUEST);
    }
    // A request to the bare wwwroute maps to the docroot dir, not a file → 404
    // (C++ stat → not a regular file).
    if suffix.is_empty() || suffix == "/" {
        return sink::error_response(StatusCode::NOT_FOUND);
    }
    let suffix = if suffix.starts_with('/') {
        suffix
    } else {
        format!("/{suffix}")
    };
    // Hardening: a hidden file (a path component starting with `.`, e.g. `.env`,
    // `.git`, `.htaccess`) is never a served asset, regardless of whether it
    // exists — 404 rather than distinguishing "not found" from "exists but
    // hidden".
    if suffix
        .split('/')
        .any(|component| !component.is_empty() && component.starts_with('.'))
    {
        return sink::error_response(StatusCode::NOT_FOUND);
    }

    // infile = raw docroot + suffix: the Content-Disposition path (206) and the
    // canonicalisation input.
    let infile = format!("{}{}", state.docroot, suffix);
    let docroot = state.docroot.clone();
    let ext = extension_of(&suffix);

    // Path resolution (realpath), the MIME sniff (libmagic, DB loaded once per
    // worker thread), and the docroot VM are all blocking, so they run off the
    // async executor — the same discipline the IIIF path applies via
    // `dispatch_engine`. Static serving admits via the Tile lane below (no
    // decode, but the admission control still bounds concurrency).
    if matches!(ext.as_deref(), Some("lua" | "elua")) {
        // Resolve + contain off-executor, then dispatch through the Lua-route seam
        // (server.docroot injected); serve_lua_script runs the VM on the pool.
        let infile2 = infile.clone();
        let docroot2 = docroot.clone();
        let resolved =
            match tokio::task::spawn_blocking(move || resolve_docroot_file(&docroot2, &infile2))
                .await
            {
                Ok(Ok(p)) => p,
                Ok(Err(status)) => return sink::error_response(status),
                Err(_) => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
            };
        return serve_lua_script(state, resolved, Some(docroot), req).await;
    }

    let is_head = req.method() == Method::HEAD;
    // Echo the Origin (no credentials), matching the IIIF success paths — gated
    // by the allowlist (empty = the unconditional echo default) via `push_cors`.
    // Validated downstream (`apply_headers` / `head_only` drop a header `http`
    // rejects).
    let origin = header_str(req.headers(), "origin");
    let range = header_str(req.headers(), header::RANGE.as_str());
    let allowed_origins = state.allowed_origins.clone();
    // Static docroot serving admits via the Tile lane, bounding the
    // realpath+stat+libmagic+stream cost.
    let permit = match state.admission.acquire(AdmissionKind::Tile).await {
        Acquired::Admitted(permit) => permit,
        Acquired::Shed | Acquired::TimedOut => return busy_response(),
    };
    let outcome = tokio::task::spawn_blocking(move || {
        let _permit = permit;
        serve_static_blocking(
            &docroot,
            &infile,
            ext.as_deref(),
            is_head,
            range.as_deref(),
            origin.as_deref(),
            &allowed_origins,
        )
    })
    .await
    .unwrap_or(StaticOutcome::Err(StatusCode::INTERNAL_SERVER_ERROR));
    let mut response = match outcome {
        StaticOutcome::Stream {
            status,
            headers,
            path,
            offset,
            length,
        } => stream_region(status, headers, path, offset, length),
        StaticOutcome::Head { status, headers } => head_only(status, &headers),
        StaticOutcome::Err(status) => return sink::error_response(status),
    };
    // The ACAO `push_cors` set depends on the request's Origin.
    append_vary(response.headers_mut(), "Origin");
    response
}

/// The blocking outcome of resolving + describing a static docroot file, built
/// off the async executor (it does realpath + stat + libmagic). `serve_docroot`
/// turns it into a `Response` on the executor, so the streaming body channel is
/// created in the runtime context.
enum StaticOutcome {
    /// Stream `[offset, offset+length)` of `path` with the built headers.
    Stream {
        status: StatusCode,
        headers: Vec<(String, String)>,
        path: String,
        offset: u64,
        length: u64,
    },
    /// A HEAD response: the headers, no body.
    Head {
        status: StatusCode,
        headers: Vec<(String, String)>,
    },
    /// An error status (404 / 400 / 500).
    Err(StatusCode),
}

/// Resolve a docroot file off the executor: canonicalise the docroot (it may be
/// created after startup, unlike imgroot), realpath + contain the file, and check
/// it is a regular file. Missing → 404 (an existence check),
/// escape → 400 (the traversal guard), directory/device → 404.
fn resolve_docroot_file(docroot: &str, infile: &str) -> Result<String, StatusCode> {
    let canon_root = std::fs::canonicalize(docroot).map_err(|_| StatusCode::NOT_FOUND)?;
    let resolved = match path::validate_resolved_path(infile, &canon_root.to_string_lossy()) {
        Resolved::Ok(p) => p,
        Resolved::NotFound => return Err(StatusCode::NOT_FOUND),
        Resolved::Traversal => return Err(StatusCode::BAD_REQUEST),
    };
    match std::fs::metadata(&resolved) {
        Ok(m) if m.is_file() => Ok(resolved),
        _ => Err(StatusCode::NOT_FOUND),
    }
}

/// Resolve + describe a static docroot file (off the executor). `.html` (when the
/// sniff is `text/html`) / `.js` / `.css` get a hardcoded Content-Type and the
/// full file with no Range; everything else sniffs the MIME via the engine and
/// supports Range/206.
fn serve_static_blocking(
    docroot: &str,
    infile: &str,
    ext: Option<&str>,
    is_head: bool,
    range: Option<&str>,
    origin: Option<&str>,
    allowed_origins: &[String],
) -> StaticOutcome {
    let resolved = match resolve_docroot_file(docroot, infile) {
        Ok(p) => p,
        Err(status) => return StaticOutcome::Err(status),
    };

    // `.js` / `.css`: a hardcoded type, no sniff (the C++ branches don't sniff).
    let special = match ext {
        Some("js") => Some("application/javascript; charset=utf-8"),
        Some("css") => Some("text/css; charset=utf-8"),
        _ => None,
    };
    if let Some(ct) = special {
        return static_special(&resolved, ct, is_head, origin, allowed_origins);
    }
    // `.html` and binary need the sniffed MIME; the `.html` hardcoded type is used
    // only when the sniff is `text/html`.
    let mime = match ffi::mimetype(&resolved) {
        Ok(m) => m,
        Err(_) => return StaticOutcome::Err(StatusCode::INTERNAL_SERVER_ERROR),
    };
    if ext == Some("html") && mime == "text/html" {
        return static_special(
            &resolved,
            "text/html; charset=utf-8",
            is_head,
            origin,
            allowed_origins,
        );
    }
    static_binary(
        &resolved,
        infile,
        &mime,
        range,
        is_head,
        origin,
        allowed_origins,
    )
}

/// Branch A (`.html`/`.js`/`.css`): a full-file 200 with only Content-Type — no
/// Range, Cache-Control, or Last-Modified.
fn static_special(
    resolved: &str,
    content_type: &str,
    is_head: bool,
    origin: Option<&str>,
    allowed_origins: &[String],
) -> StaticOutcome {
    let Ok(meta) = std::fs::metadata(resolved) else {
        return StaticOutcome::Err(StatusCode::NOT_FOUND);
    };
    let mut headers = vec![(header::CONTENT_TYPE.to_string(), content_type.to_owned())];
    push_cors(&mut headers, origin, allowed_origins);
    if is_head {
        StaticOutcome::Head {
            status: StatusCode::OK,
            headers,
        }
    } else {
        StaticOutcome::Stream {
            status: StatusCode::OK,
            headers,
            path: resolved.to_owned(),
            offset: 0,
            length: meta.len(),
        }
    }
}

/// Branch B (binary/media): full caching headers + Range/206. Content-Length is
/// left to hyper, which streams chunked.
fn static_binary(
    resolved: &str,
    infile: &str,
    mime: &str,
    range: Option<&str>,
    is_head: bool,
    origin: Option<&str>,
    allowed_origins: &[String],
) -> StaticOutcome {
    let Ok(meta) = std::fs::metadata(resolved) else {
        return StaticOutcome::Err(StatusCode::NOT_FOUND);
    };
    let fsize = meta.len();
    let mut headers = vec![
        (header::CONTENT_TYPE.to_string(), mime.to_owned()),
        (
            header::CACHE_CONTROL.to_string(),
            "public, must-revalidate, max-age=0".to_owned(),
        ),
        (header::PRAGMA.to_string(), "no-cache".to_owned()),
        (header::ACCEPT_RANGES.to_string(), "bytes".to_owned()),
    ];
    if let Ok(mtime) = meta.modified() {
        headers.push((
            header::LAST_MODIFIED.to_string(),
            httpdate::fmt_http_date(mtime),
        ));
    }
    push_cors(&mut headers, origin, allowed_origins);

    match range {
        Some(raw) => {
            let Some((start, end)) = parse_byte_range(raw, fsize) else {
                // Malformed / inverted range / start past EOF → 500.
                return StaticOutcome::Err(StatusCode::INTERNAL_SERVER_ERROR);
            };
            headers.push((
                header::CONTENT_RANGE.to_string(),
                format!("bytes {start}-{end}/{fsize}"),
            ));
            // Basename only — the full infile path leaks the server's docroot
            // layout; `content_disposition` also closes the header-injection
            // surface (CR/LF/NUL/control/DEL stripped, RFC 2616/6266 escaping).
            // An unresolvable or empty basename (e.g. `file_name()` returns
            // `None`, or the name isn't valid UTF-8) hard-errors instead of
            // falling back to the full path, so the leak closure is structural.
            let Some(infile_basename) = std::path::Path::new(infile)
                .file_name()
                .and_then(|n| n.to_str())
                .filter(|name| !name.is_empty())
            else {
                return StaticOutcome::Err(StatusCode::INTERNAL_SERVER_ERROR);
            };
            if let Some(cd) = content_disposition(infile_basename) {
                if let Ok(cd_str) = cd.to_str() {
                    headers.push((header::CONTENT_DISPOSITION.to_string(), cd_str.to_owned()));
                }
            }
            // `end >= start` is guaranteed by parse_byte_range, so `end - start + 1`
            // cannot underflow.
            if is_head {
                StaticOutcome::Head {
                    status: StatusCode::PARTIAL_CONTENT,
                    headers,
                }
            } else {
                StaticOutcome::Stream {
                    status: StatusCode::PARTIAL_CONTENT,
                    headers,
                    path: resolved.to_owned(),
                    offset: start,
                    length: end - start + 1,
                }
            }
        }
        None => {
            if is_head {
                StaticOutcome::Head {
                    status: StatusCode::OK,
                    headers,
                }
            } else {
                StaticOutcome::Stream {
                    status: StatusCode::OK,
                    headers,
                    path: resolved.to_owned(),
                    offset: 0,
                    length: fsize,
                }
            }
        }
    }
}

/// Percent-decode a URL path suffix to an on-disk path fragment. `None` (→ the
/// caller 400s) on invalid UTF-8 or an interior NUL / control char — a strictly
/// wider guard than the IIIF catch-all's own R7 (NUL only; see its comment).
fn decode_path_suffix(enc: &str) -> Option<String> {
    let decoded = percent_encoding::percent_decode_str(enc)
        .decode_utf8()
        .ok()?;
    // `is_ascii_control` covers 0x00–0x1F and 0x7F, so the IIIF catch-all's
    // NUL-only R7 check is subsumed here (this guard is strictly wider).
    if decoded.bytes().any(|b| b.is_ascii_control()) {
        return None;
    }
    Some(decoded.into_owned())
}

/// The lowercased filename extension (after the last `.` in the last path
/// component), or `None` — drives the static-vs-Lua dispatch.
fn extension_of(path: &str) -> Option<String> {
    let name = path.rsplit('/').next().unwrap_or(path);
    name.rfind('.').map(|i| name[i + 1..].to_ascii_lowercase())
}

/// Parse a single `bytes=start-end` Range value (the C++ regex
/// `bytes=\s*(\d+)-(\d*)`): `start` is required, `end` defaults to EOF when
/// omitted and is clamped to the file. `None` (→ the caller 500s) on a malformed
/// range, `start >= fsize`, or an inverted range (`end < start`) — matching the
/// C++ throw, and guaranteeing the caller's `end - start + 1` cannot underflow.
fn parse_byte_range(raw: &str, fsize: u64) -> Option<(u64, u64)> {
    let body = raw.trim().strip_prefix("bytes=")?;
    let (s, e) = body.trim_start().split_once('-')?;
    let start = leading_u64(s.trim_start())?;
    let end = leading_u64(e.trim_start()).unwrap_or_else(|| fsize.saturating_sub(1));
    if start >= fsize {
        return None;
    }
    let end = end.min(fsize - 1);
    if end < start {
        return None;
    }
    Some((start, end))
}

/// Leading ASCII digits of `s` parsed as a `u64`, or `None` when there is no
/// leading digit (mirrors the C++ Range regex's `\d+` / `\d*` groups).
fn leading_u64(s: &str) -> Option<u64> {
    let digits: String = s.chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().ok()
}

/// Append the CORS `Access-Control-Allow-Origin` echo when [`cors_allow`]
/// allows `origin` against `allowlist` (ACAO only — no credentials, matching
/// today's fileserver behaviour in both modes). The caller adds `Vary: Origin`
/// to the built response on every decision. The raw value is validated downstream
/// (`apply_headers` / `head_only` drop a header `http` rejects), so a
/// control-char Origin can't inject a header.
fn push_cors(headers: &mut Vec<(String, String)>, origin: Option<&str>, allowlist: &[String]) {
    if let (CorsDecision::Allow, Some(o)) = (cors_allow(origin, allowlist), origin) {
        headers.push((
            header::ACCESS_CONTROL_ALLOW_ORIGIN.to_string(),
            o.to_owned(),
        ));
    }
}

/// Apply the IIIF route's CORS decision to `response`, whatever its status.
///
/// A response that already carries `Access-Control-Allow-Origin` is left as
/// built: info.json and knora.json set their own (`*` without an Origin, see
/// [`json_cors`]), and a `pre_flight` hook that answers the request itself may
/// set its own, which an echo would overwrite. Otherwise an allowed `origin` is echoed with
/// `Access-Control-Allow-Credentials: true` (cookie auth: without it the
/// browser withholds the DSP session cookie cross-origin), and `Origin` is
/// added to `Vary` on every decision, a deny included, so a shared cache does
/// not serve one origin's response to another.
fn with_cors(mut response: Response, origin: Option<&str>, decision: CorsDecision) -> Response {
    let h = response.headers_mut();
    append_vary(h, "Origin");
    if h.contains_key(header::ACCESS_CONTROL_ALLOW_ORIGIN) {
        return response;
    }
    if decision == CorsDecision::Allow {
        if let Some(acao) = origin.and_then(|o| HeaderValue::from_str(o).ok()) {
            h.insert(header::ACCESS_CONTROL_ALLOW_ORIGIN, acao);
            h.insert(
                header::ACCESS_CONTROL_ALLOW_CREDENTIALS,
                HeaderValue::from_static("true"),
            );
        }
    }
    response
}

/// Add `token` to the response's `Vary` unless a `Vary` value already lists
/// it (case-insensitive). Every `Vary` header is scanned, and the result is
/// one `Vary` header: a client or cache that reads only the first `Vary`
/// still sees every token.
fn append_vary(h: &mut HeaderMap, token: &str) {
    let values: Vec<String> = h
        .get_all(header::VARY)
        .iter()
        .filter_map(|v| v.to_str().ok())
        .map(str::to_owned)
        .collect();
    let listed = values
        .iter()
        .flat_map(|v| v.split(','))
        .any(|v| v.trim().eq_ignore_ascii_case(token));
    if listed {
        return;
    }
    let joined = values
        .iter()
        .map(String::as_str)
        .chain([token])
        .collect::<Vec<_>>()
        .join(", ");
    h.insert(
        header::VARY,
        HeaderValue::from_str(&joined).expect("joined from visible-ASCII header values"),
    );
}

/// A head-only response (HEAD) carrying the static-serve headers, no body.
fn head_only(status: StatusCode, headers: &[(String, String)]) -> Response {
    let mut builder = Response::builder().status(status);
    for (name, value) in headers {
        if let (Ok(n), Ok(v)) = (
            axum::http::HeaderName::from_bytes(name.as_bytes()),
            HeaderValue::from_str(value),
        ) {
            builder = builder.header(n, v);
        }
    }
    builder
        .body(Body::empty())
        .unwrap_or_else(|_| sink::error_response(StatusCode::INTERNAL_SERVER_ERROR))
}

/// Stream a `[offset, offset+length)` file region as the response body, reusing
/// the sink's bounded-memory blocking reader (no engine pool permit — static
/// serving does no FFI). The blocking `std::fs` read runs on `spawn_blocking`.
fn stream_region(
    status: StatusCode,
    headers: Vec<(String, String)>,
    path: String,
    offset: u64,
    length: u64,
) -> Response {
    let (body_tx, body_rx) = mpsc::channel::<sink::BodyItem>(sink::BODY_CHANNEL_CAP);
    tokio::task::spawn_blocking(move || {
        let _ = sink::stream_file_region(&body_tx, &path, offset, length);
    });
    sink::stream_response(status.as_u16(), headers, body_rx)
}

/// Whether an HTTP method carries a request body the Lua route should read.
fn has_request_body(method: &Method) -> bool {
    matches!(
        *method,
        Method::POST | Method::PUT | Method::PATCH | Method::DELETE
    )
}

/// Parse `application/x-www-form-urlencoded` (or a query string) into name/value
/// pairs via the `form_urlencoded` crate (WHATWG semantics: `&`-split, `=`-split,
/// `+` → space, percent-decode; empty pairs skipped).
///
/// This feeds the Lua `server.get`/`server.post` bindings, so its output must
/// match the documented query/POST parse the scripts rely on (exercised by the
/// Lua e2e tests).
fn parse_form_encoded(s: &str) -> Vec<(String, String)> {
    form_urlencoded::parse(s.as_bytes()).into_owned().collect()
}

fn serve_info_json(
    resolved: &str,
    parsed: &ParsedRequest,
    headers: &HeaderMap,
    access: &Access,
    allowed_origins: &[String],
    public_hosts: &[String],
    hook_configured: bool,
) -> Response {
    let (scheme, host) = forwarded(headers, public_hosts);
    let id = canonical_id(&scheme, &host, &parsed.prefix, &parsed.identifier);

    let mime = match ffi::mimetype(resolved) {
        Ok(m) => m,
        Err(_) => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
    };

    let (mut value, link_context) = if IMAGE_MIMES.contains(&mime.as_str()) {
        // A restrict decision that supplies neither a size cap nor a watermark
        // arrives here indistinguishable from `allow` — refuse it rather than
        // disclose the native dims and tile grid, matching the image-serve
        // path (routes.rs `serve_image`) and `/file`.
        if access.permission == SipiPermType::Restrict
            && access_kv_str(access, "size").is_none()
            && restrict_watermark(access).is_none()
        {
            return sink::error_response(StatusCode::FORBIDDEN);
        }
        let dims = match ffi::image_dims(resolved) {
            Ok(d) => d,
            Err(_) => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
        };
        let (dims, size_restricted) = match restricted_dims(&dims, access, &parsed.identifier) {
            Ok(d) => d,
            Err(RestrictRefusal::Refuse) => return sink::error_response(StatusCode::FORBIDDEN),
            Err(RestrictRefusal::Misconfigured) => {
                return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR)
            }
        };
        (
            info::image_info_json(&id, &dims, size_restricted),
            info::image_context(),
        )
    } else {
        let size = match std::fs::metadata(resolved) {
            Ok(m) => m.len(),
            Err(_) => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
        };
        (
            info::bitstream_info_json(&id, &mime, size),
            info::file_context(),
        )
    };

    // An auth-type permission adds the IIIF Auth service block at status 401.
    let status = if info::is_auth_type(access.permission) {
        match info::auth_service(access.permission, &access.kv) {
            Some(service) => {
                if let Some(obj) = value.as_object_mut() {
                    obj.insert("service".into(), serde_json::json!([service]));
                }
                StatusCode::UNAUTHORIZED
            }
            // Missing cookieUrl/tokenUrl in the hook result → 500.
            None => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
        }
    } else {
        StatusCode::OK
    };

    // The body depends on the caller's credentials (clamped dims, 401 auth
    // challenge), so a credentialed viewer must be able to read it: the same
    // CORS as knora.json and the image route.
    let (acao, vary) = json_cors(
        header_str(headers, "origin"),
        allowed_origins,
        hook_configured,
    );
    json_response(
        status,
        &value,
        headers,
        Some(link_context),
        acao.as_deref(),
        vary,
        hook_configured,
    )
}

fn serve_knora_json(
    resolved: &str,
    parsed: &ParsedRequest,
    headers: &HeaderMap,
    access: &Access,
    allowed_origins: &[String],
    public_hosts: &[String],
    hook_configured: bool,
) -> Response {
    let (scheme, host) = forwarded(headers, public_hosts);
    let id = canonical_id(&scheme, &host, &parsed.prefix, &parsed.identifier);

    let mime = match ffi::mimetype(resolved) {
        Ok(m) => m,
        Err(_) => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
    };
    let sidecar = read_sidecar(resolved);

    let value = if IMAGE_MIMES.contains(&mime.as_str()) {
        // A restrict decision that supplies neither a size cap nor a watermark
        // arrives here indistinguishable from `allow` — refuse it (hiding
        // existence, per knora.json's deny convention) rather than disclose
        // the native dims and tile grid, matching the image-serve path
        // (routes.rs `serve_image`) and `/file`.
        if access.permission == SipiPermType::Restrict
            && access_kv_str(access, "size").is_none()
            && restrict_watermark(access).is_none()
        {
            return sink::error_response(StatusCode::NOT_FOUND);
        }
        let (dims, essentials) = match ffi::image_dims_and_essentials(resolved) {
            Ok(result) => result,
            Err(_) => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
        };
        let (dims, _) = match restricted_dims(&dims, access, &parsed.identifier) {
            Ok(d) => d,
            Err(RestrictRefusal::Refuse) => return sink::error_response(StatusCode::NOT_FOUND),
            Err(RestrictRefusal::Misconfigured) => {
                return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR)
            }
        };
        info::image_knora_json(&id, &mime, &dims, &sidecar, essentials.as_ref())
    } else {
        let size = match std::fs::metadata(resolved) {
            Ok(m) => m.len(),
            Err(_) => return sink::error_response(StatusCode::INTERNAL_SERVER_ERROR),
        };
        if mime == "video/mp4" {
            info::video_knora_json(&id, &mime, size, &sidecar)
        } else {
            info::generic_knora_json(&id, &mime, size, &sidecar)
        }
    };

    let (acao, vary) = json_cors(
        header_str(headers, "origin"),
        allowed_origins,
        hook_configured,
    );
    json_response(
        StatusCode::OK,
        &value,
        headers,
        None,
        acao.as_deref(),
        vary,
        hook_configured,
    )
}

/// 303 redirect from a bare identifier to its canonical info.json.
fn redirect(headers: &HeaderMap, parsed: &ParsedRequest, public_hosts: &[String]) -> Response {
    let (scheme, host) = forwarded(headers, public_hosts);
    let target = if parsed.prefix.is_empty() {
        format!("{scheme}://{host}/{}/info.json", parsed.identifier)
    } else {
        format!(
            "{scheme}://{host}/{}/{}/info.json",
            parsed.prefix, parsed.identifier
        )
    };
    match HeaderValue::from_str(&target) {
        Ok(location) => {
            let mut response = Response::builder()
                .status(StatusCode::SEE_OTHER)
                .body(Body::from(format!("Redirect to {target}")))
                .unwrap_or_else(|_| sink::error_response(StatusCode::INTERNAL_SERVER_ERROR));
            response.headers_mut().insert(header::LOCATION, location);
            response
                .headers_mut()
                .insert(header::CONTENT_TYPE, HeaderValue::from_static("text/plain"));
            response
        }
        // A Location with control chars (CRLF injection) is rejected, not emitted.
        Err(_) => sink::error_response(StatusCode::BAD_REQUEST),
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────

/// The `Access-Control-Allow-Origin` and the `Vary` list of info.json and
/// knora.json. No Origin answers `*`; a presented Origin is echoed when
/// [`cors_allow`] allows it against `allowlist`, else no ACAO is emitted.
/// [`json_response`] adds credentials to an echoed Origin. The ACAO depends on
/// the Origin in every mode, so the response always varies on `Origin`; with a
/// preflight hook the body depends on the credentials too (clamped dims, 401
/// auth challenge), so an intermediary cache must not share it across them.
fn json_cors(
    origin: Option<String>,
    allowlist: &[String],
    hook_configured: bool,
) -> (Option<String>, &'static [&'static str]) {
    let acao = match origin {
        None => Some("*".to_owned()),
        Some(o) => match cors_allow(Some(&o), allowlist) {
            CorsDecision::Allow => Some(o),
            CorsDecision::Deny => None,
        },
    };
    let vary: &'static [&'static str] = if hook_configured {
        &["Origin", "Cookie", "Authorization"]
    } else {
        &["Origin"]
    };
    (acao, vary)
}

/// A CORS decision for one request Origin against the configured allowlist.
/// The single chokepoint every CORS site ([`with_cors`] for every `iiif`
/// response, [`json_cors`] for info.json and knora.json, [`cors_preflight_response`],
/// [`push_cors`]) reads through [`cors_allow`] — the four sites differ only in
/// which headers they attach on `Allow`, not in whether they attach them.
///
/// Every site emits `Vary: Origin` on both decisions, in both modes, with or
/// without an Origin header: whether the response carries
/// `Access-Control-Allow-Origin` depends on the request's Origin, so a shared
/// cache must not serve one origin's response (or a deny) to another.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CorsDecision {
    /// Emit no CORS headers for this request, only `Vary: Origin`.
    Deny,
    /// Emit the site's CORS headers for the presented Origin.
    Allow,
}

/// Decide whether `origin` may be echoed back, given the configured
/// `allowlist` (`SIPI_ALLOWED_ORIGINS`, [`crate::config::allowed_origins_from_env`]).
///
/// An empty `allowlist` is the opt-out default: any presented Origin is
/// allowed. A non-empty `allowlist` switches the instance into allowlist mode:
/// only an exact, listed origin is allowed; anything else — including no
/// Origin header at all — is denied. The opaque Origin `null` (sandboxed
/// iframes, `file:` pages, some redirects) is denied in both modes, also when
/// the allowlist lists it: any such context can send it, so echoing it with
/// credentials would grant every one of them the caller's cookies.
fn cors_allow(origin: Option<&str>, allowlist: &[String]) -> CorsDecision {
    match origin {
        None | Some("null") => CorsDecision::Deny,
        Some(_) if allowlist.is_empty() => CorsDecision::Allow,
        Some(o) if allowlist.iter().any(|a| a == o) => CorsDecision::Allow,
        Some(_) => CorsDecision::Deny,
    }
}

/// Parse the `Cookie` header into name/value pairs (the parsed map the Lua
/// `server.cookies` binding reads) via the `cookie` crate's RFC 6265 splitter.
///
/// This feeds the Lua `server.cookies` binding, so its output must match the
/// documented cookie parse the scripts rely on (exercised by the Lua e2e tests).
fn parse_cookies(headers: &HeaderMap) -> Vec<(String, String)> {
    let Some(raw) = header_str(headers, header::COOKIE.as_str()) else {
        return Vec::new();
    };
    cookie::Cookie::split_parse(raw.as_str())
        .filter_map(Result::ok)
        .map(|c| (c.name().to_owned(), c.value().to_owned()))
        .collect()
}

fn access_kv_str<'a>(access: &'a Access, key: &str) -> Option<&'a str> {
    access
        .kv
        .iter()
        .find(|(k, _)| k == key)
        .map(|(_, v)| v.as_str())
}

/// A `restrict` decision's watermark. An empty one is no watermark: the engine
/// applies none, so counting it would serve a no-op restriction unrefused.
fn restrict_watermark(access: &Access) -> Option<&str> {
    access_kv_str(access, "watermark").filter(|w| !w.is_empty())
}

/// Why a `restrict` decision's `size` yields no restricted dims.
#[derive(Debug, PartialEq, Eq)]
enum RestrictRefusal {
    /// A restriction that restricts nothing (a size kind that never reduces,
    /// without a watermark) or a restricted axis below 2 px, which
    /// the codecs cannot render. Each route answers with its refusal status.
    Refuse,
    /// The `size` does not parse: a preflight misconfiguration, 500.
    Misconfigured,
}

/// The restricted dims `(RW, RH)` of a `restrict` decision's `size` on an
/// image of native `width` x `height`. info.json, knora.json and the image
/// route all decide through here, so they agree on refusals and dims.
///
/// The refusal is decided on the size's kind, not on the resulting dims: a
/// box kind caps at its box, and an image already inside the box is served at
/// native size (a warning makes the over-large box visible).
fn restriction(
    size: &str,
    watermarked: bool,
    width: u32,
    height: u32,
    identifier: &str,
) -> Result<(NonZeroU32, NonZeroU32), RestrictRefusal> {
    let never_reduces =
        iiif_parser::size_never_reduces(size).map_err(|_| RestrictRefusal::Misconfigured)?;
    if never_reduces && !watermarked {
        return Err(RestrictRefusal::Refuse);
    }
    let (w, h) = iiif_parser::clamp_dims_to_size(size, width, height)
        .map_err(|_| RestrictRefusal::Misconfigured)?;
    // The engine refuses the same minimum at the seam (serve_image.cpp).
    if w < 2 || h < 2 {
        return Err(RestrictRefusal::Refuse);
    }
    if !watermarked && (w, h) == (width, height) {
        tracing::warn!(
            identifier,
            size,
            "restrict size does not reduce the image; served at native size"
        );
    }
    match (NonZeroU32::new(w), NonZeroU32::new(h)) {
        (Some(w), Some(h)) => Ok((w, h)),
        _ => Err(RestrictRefusal::Refuse),
    }
}

/// Clamp an image's dims for a restricted permission so info.json / knora.json
/// never discloses the native tiling pyramid. A `Restrict` decision with a
/// `size` reports its [`restriction`] dims, or refuses as the image route
/// does; every other restricted permission keeps the native width/height (the
/// caller renders an auth challenge, not the image) but still loses the tile
/// grid. The flag is true when the dims are a size restriction's (RW, RH).
///
/// `Stream` is not a restriction: it restricts no pixel and must describe the
/// image exactly as `Allow` does, tile grid included (ADR-0027).
fn restricted_dims(
    dims: &ffi::SipiImageDims,
    access: &Access,
    identifier: &str,
) -> Result<(ffi::SipiImageDims, bool), RestrictRefusal> {
    if matches!(
        access.permission,
        SipiPermType::Allow | SipiPermType::Stream
    ) {
        return Ok((*dims, false));
    }
    let mut restricted = *dims;
    let mut size_restricted = false;
    if access.permission == SipiPermType::Restrict {
        if let Some(size) = access_kv_str(access, "size") {
            let watermarked = restrict_watermark(access).is_some();
            let (width, height) =
                restriction(size, watermarked, dims.width, dims.height, identifier)?;
            restricted.width = width.get();
            restricted.height = height.get();
            size_restricted = true;
        }
    }
    restricted.tile_width = 0;
    restricted.tile_height = 0;
    Ok((restricted, size_restricted))
}

/// Serialise a JSON value with the standard IIIF headers: a CORS
/// `Access-Control-Allow-Origin` (`acao`, from [`json_cors`]: the echoed
/// Origin, `*` when no Origin was sent, or `None` when the allowlist denies
/// the Origin, so no ACAO is emitted at all) and either `application/ld+json`
/// (when the client `Accept`s it) or `application/json` + a `Link` to the
/// JSON-LD context. A concretely echoed origin also carries
/// `Access-Control-Allow-Credentials: true` (cookie auth); `*` does not, as
/// CORS forbids the pairing. `vary`, from [`json_cors`], names every header
/// the response varies on and is emitted as one joined `Vary` header when
/// non-empty; with a `link_context` the Content-Type and `Link` depend on
/// `Accept`, so `Accept` is added to that list. `private`, set by the caller
/// whenever the body is credential-dependent, emits
/// `Cache-Control: private, no-store` so an intermediary cache never shares
/// one caller's response with another.
fn json_response(
    status: StatusCode,
    value: &serde_json::Value,
    headers: &HeaderMap,
    link_context: Option<&str>,
    acao: Option<&str>,
    vary: &[&str],
    private: bool,
) -> Response {
    let body = serde_json::to_vec(value).unwrap_or_default();
    let mut builder = Response::builder().status(status);
    if let Some(acao) = acao {
        if let Ok(acao_value) = HeaderValue::from_str(acao) {
            builder = builder.header(header::ACCESS_CONTROL_ALLOW_ORIGIN, acao_value);
            // A concrete echoed origin also needs credentials so the browser
            // accepts the cookie-authenticated cross-origin response. `*` (no
            // Origin sent) must not pair with credentials: CORS forbids it.
            if acao != "*" {
                builder = builder.header(header::ACCESS_CONTROL_ALLOW_CREDENTIALS, "true");
            }
        }
    }

    let wants_ldjson =
        header_str(headers, header::ACCEPT.as_str()).as_deref() == Some("application/ld+json");
    match (link_context, wants_ldjson) {
        (Some(ctx), true) => {
            builder = builder.header(
                header::CONTENT_TYPE,
                format!("application/ld+json;profile=\"{ctx}\""),
            );
        }
        (Some(ctx), false) => {
            builder = builder.header(header::CONTENT_TYPE, "application/json").header(
                header::LINK,
                format!("<{ctx}>; rel=\"http://www.w3.org/ns/json-ld#context\"; type=\"application/ld+json\""),
            );
        }
        (None, _) => {
            builder = builder.header(header::CONTENT_TYPE, "application/json");
        }
    }
    let mut response = builder
        .body(Body::from(body))
        .unwrap_or_else(|_| sink::error_response(StatusCode::INTERNAL_SERVER_ERROR));
    let h = response.headers_mut();
    let accept = link_context.map(|_| "Accept");
    for token in vary.iter().copied().chain(accept) {
        append_vary(h, token);
    }
    if private {
        h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
        mark_private(h);
    }
    response
}

/// Synthesise the request scheme + host from the forwarded headers (SIPI runs
/// plain HTTP behind Traefik, so the real scheme is in `X-Forwarded-Proto`). Host
/// falls back from `X-Forwarded-Host` to `Host`.
///
/// The host is validated against `public_hosts` (`SIPI_PUBLIC_HOSTS`) before it
/// reaches the canonical `@id`, the 303 `Location`, the `Link` header, the
/// cache key, or the Lua `server.host`: an empty allowlist is the opt-out
/// sentinel and the candidate host is returned verbatim; a non-empty allowlist
/// returns the candidate unchanged when it is on the list, else substitutes the
/// first allowlisted host.
fn forwarded(headers: &HeaderMap, public_hosts: &[String]) -> (String, String) {
    let scheme = if header_str(headers, "x-forwarded-proto").as_deref() == Some("https") {
        "https"
    } else {
        "http"
    }
    .to_owned();
    let candidate = header_str(headers, "x-forwarded-host")
        .or_else(|| header_str(headers, header::HOST.as_str()))
        .unwrap_or_default();
    let host = if public_hosts.is_empty() || public_hosts.iter().any(|h| h == &candidate) {
        candidate
    } else {
        public_hosts[0].clone()
    };
    (scheme, host)
}

/// The canonical service id: `scheme://host/[prefix/]identifier`.
fn canonical_id(scheme: &str, host: &str, prefix: &str, identifier: &str) -> String {
    if prefix.is_empty() {
        format!("{scheme}://{host}/{identifier}")
    } else {
        format!("{scheme}://{host}/{prefix}/{identifier}")
    }
}

/// The client IP: the rightmost `X-Forwarded-For` value, else empty. Feeds the
/// IIIF request's `client_ip` field and the Lua request context's
/// `server.client_ip`.
///
/// SECURITY: this trusts `X-Forwarded-For`, which is sound only when SIPI sits
/// behind a reverse proxy (Traefik) that overwrites the header with the real
/// peer. The shell must never be directly reachable by clients — a directly
/// connected client could forge `X-Forwarded-For` on every request to spoof
/// its reported client identity.
fn client_ip(headers: &HeaderMap) -> String {
    header_str(headers, "x-forwarded-for")
        .and_then(|v| v.rsplit(',').next().map(|s| s.trim().to_owned()))
        .unwrap_or_default()
}

/// `Content-Disposition` for a `/file` download: strip CR/LF/NUL/control/DEL
/// bytes (R8,
/// preserving bytes ≥ 0x80), then either an RFC 2616 quoted `filename="…"` for an
/// ASCII name, or the RFC 6266 `filename*=UTF-8''…` percent-encoded form for a
/// non-ASCII one. Operates on the already-URL-decoded identifier.
fn content_disposition(identifier: &str) -> Option<HeaderValue> {
    // R8: drop CR/LF/NUL, other controls (< 0x20) and DEL (0x7F); bytes ≥ 0x80
    // survive so a non-ASCII name routes to the RFC 6266 branch.
    let safe: Vec<u8> = identifier
        .bytes()
        .filter(|&b| b >= 0x20 && b != 0x7F)
        .collect();
    let value = if safe.iter().all(|&b| b < 0x7F) {
        // ASCII printable → RFC 2616 quoted-string, escaping '"' and '\'.
        let mut quoted = String::with_capacity(safe.len());
        for &b in &safe {
            if b == b'"' || b == b'\\' {
                quoted.push('\\');
            }
            quoted.push(b as char);
        }
        format!("inline; filename=\"{quoted}\"")
    } else {
        // Non-ASCII → RFC 6266, percent-encoding every byte except the RFC 3986
        // unreserved set (uppercase hex).
        let mut enc = String::with_capacity(safe.len() * 3);
        for &b in &safe {
            if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'.' | b'_' | b'~') {
                enc.push(b as char);
            } else {
                enc.push_str(&format!("%{b:02X}"));
            }
        }
        format!("inline; filename*=UTF-8''{enc}")
    };
    HeaderValue::from_str(&value).ok()
}

/// Read + parse the `[path-sans-ext].info` sidecar; missing/invalid → empty.
fn read_sidecar(infile: &str) -> Sidecar {
    let sidecar_path = match infile.rfind('.') {
        Some(pos) => format!("{}.info", &infile[..pos]),
        None => format!("{infile}.info"),
    };
    std::fs::read_to_string(sidecar_path)
        .map(|t| Sidecar::parse(&t))
        .unwrap_or_default()
}

/// First value of a header as an owned `String`, if present and valid UTF-8.
fn header_str(headers: &HeaderMap, name: &str) -> Option<String> {
    headers
        .get(name)
        .and_then(|v| v.to_str().ok())
        .map(str::to_owned)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn headers(pairs: &[(&str, &str)]) -> HeaderMap {
        let mut h = HeaderMap::new();
        for (k, v) in pairs {
            h.insert(
                axum::http::HeaderName::from_bytes(k.as_bytes()).unwrap(),
                HeaderValue::from_str(v).unwrap(),
            );
        }
        h
    }

    #[test]
    fn preflight_request_data_is_query_and_body_free() {
        // The preflight-cache key covers only (prefix, identifier, Cookie,
        // Authorization); this asserts the request view the hook actually sees
        // stays query- and body-free, so a future edit can't silently widen it
        // without the cache serving stale decisions for distinct requests.
        let uri: Uri = "/prefix/identifier/full/0/default.jpg?x=random"
            .parse()
            .unwrap();
        let h = headers(&[
            ("host", "iiif.example.org"),
            ("cookie", "session=abc"),
            ("authorization", "Bearer token"),
        ]);
        let data = build_request_data(&Method::GET, &uri, &h, &[]);

        assert_eq!(data.uri, "/prefix/identifier/full/0/default.jpg");
        assert!(data.get_params.is_empty());
        assert!(data.post_params.is_empty());
        assert!(data.request_params.is_empty());
        assert!(data.content.is_empty());

        assert_eq!(data.method, "GET");
        assert_eq!(data.host, "iiif.example.org");
        assert!(!data.headers.is_empty());
        assert_eq!(data.cookies, vec![("session".to_owned(), "abc".to_owned())]);
    }

    #[test]
    fn forwarded_synthesises_https_scheme() {
        let h = headers(&[("x-forwarded-proto", "https"), ("host", "iiif.example.org")]);
        assert_eq!(
            forwarded(&h, &[]),
            ("https".into(), "iiif.example.org".into())
        );
    }

    #[test]
    fn forwarded_defaults_http_and_prefers_forwarded_host() {
        let h = headers(&[
            ("host", "internal:1024"),
            ("x-forwarded-host", "iiif.example.org"),
        ]);
        assert_eq!(
            forwarded(&h, &[]),
            ("http".into(), "iiif.example.org".into())
        );
    }

    #[test]
    fn forwarded_substitutes_first_allowlisted_host_for_hostile_header() {
        let h = headers(&[("x-forwarded-host", "evil.example.org")]);
        let allowlist = vec![
            "iiif.example.org".to_owned(),
            "iiif2.example.org".to_owned(),
        ];
        assert_eq!(
            forwarded(&h, &allowlist),
            ("http".into(), "iiif.example.org".into())
        );
    }

    #[test]
    fn forwarded_passes_through_allowlisted_host() {
        let h = headers(&[("x-forwarded-host", "iiif2.example.org")]);
        let allowlist = vec![
            "iiif.example.org".to_owned(),
            "iiif2.example.org".to_owned(),
        ];
        assert_eq!(
            forwarded(&h, &allowlist),
            ("http".into(), "iiif2.example.org".into())
        );
    }

    #[test]
    fn cors_preflight_echoes_origin_credentials_and_methods() {
        // Cookie auth: with an Origin, the preflight echoes it + credentials and
        // advertises the served method list.
        let resp = cors_preflight_response(
            &headers(&[
                ("origin", "https://example.org"),
                ("access-control-request-headers", "authorization"),
            ]),
            &[],
        );
        assert_eq!(resp.status(), StatusCode::NO_CONTENT);
        let h = resp.headers();
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_METHODS).unwrap(),
            "GET, POST, PUT, DELETE"
        );
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).unwrap(),
            "https://example.org"
        );
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).unwrap(),
            "true"
        );
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_HEADERS).unwrap(),
            "authorization"
        );
    }

    #[test]
    fn cors_preflight_without_origin_omits_origin_and_credentials() {
        // No Origin → no credentialed CORS, but the method list still advertises.
        let resp = cors_preflight_response(&headers(&[]), &[]);
        assert_eq!(resp.status(), StatusCode::NO_CONTENT);
        let h = resp.headers();
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_METHODS).unwrap(),
            "GET, POST, PUT, DELETE"
        );
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).is_none());
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).is_none());
        assert_eq!(h.get(header::VARY).unwrap(), "Origin");
    }

    #[test]
    fn json_response_sets_credentials_only_for_echoed_origin() {
        let value = serde_json::json!({ "ok": true });
        // A concrete echoed origin → ACAO + credentials.
        let resp = json_response(
            StatusCode::OK,
            &value,
            &headers(&[]),
            None,
            Some("https://example.org"),
            &[],
            false,
        );
        assert_eq!(
            resp.headers()
                .get(header::ACCESS_CONTROL_ALLOW_ORIGIN)
                .unwrap(),
            "https://example.org"
        );
        assert_eq!(
            resp.headers()
                .get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS)
                .unwrap(),
            "true"
        );
        // `*` (no Origin sent) → ACAO but never credentials (CORS forbids it).
        let resp = json_response(
            StatusCode::OK,
            &value,
            &headers(&[]),
            None,
            Some("*"),
            &[],
            false,
        );
        assert_eq!(
            resp.headers()
                .get(header::ACCESS_CONTROL_ALLOW_ORIGIN)
                .unwrap(),
            "*"
        );
        assert!(resp
            .headers()
            .get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS)
            .is_none());
    }

    #[test]
    fn cors_allow_reflects_any_origin_when_allowlist_empty() {
        // Default (opt-out) posture: any presented Origin is allowed.
        assert_eq!(
            cors_allow(Some("https://example.org"), &[]),
            CorsDecision::Allow
        );
        assert_eq!(cors_allow(None, &[]), CorsDecision::Deny);
    }

    #[test]
    fn cors_allow_permits_only_listed_origin_when_allowlist_set() {
        let allowlist = vec!["https://a.example".to_string()];
        assert_eq!(
            cors_allow(Some("https://a.example"), &allowlist),
            CorsDecision::Allow
        );
        assert_eq!(
            cors_allow(Some("https://evil.example"), &allowlist),
            CorsDecision::Deny
        );
        assert_eq!(cors_allow(None, &allowlist), CorsDecision::Deny);
    }

    #[test]
    fn cors_allow_denies_the_null_origin_in_both_modes() {
        assert_eq!(cors_allow(Some("null"), &[]), CorsDecision::Deny);
        let allowlist = vec!["null".to_string(), "https://a.example".to_string()];
        assert_eq!(cors_allow(Some("null"), &allowlist), CorsDecision::Deny);
    }

    #[test]
    fn cors_sites_vary_on_origin_in_reflect_mode() {
        for origin in [None, Some("https://example.org"), Some("null")] {
            let resp = with_cors(
                sink::error_response(StatusCode::NOT_FOUND),
                origin,
                cors_allow(origin, &[]),
            );
            assert_eq!(
                resp.headers().get(header::VARY).unwrap(),
                "Origin",
                "{origin:?}"
            );

            let pairs: Vec<_> = origin.map(|o| ("origin", o)).into_iter().collect();
            let resp = cors_preflight_response(&headers(&pairs), &[]);
            assert_eq!(
                resp.headers().get(header::VARY).unwrap(),
                "Origin",
                "{origin:?}"
            );
        }
    }

    #[test]
    fn null_origin_gets_no_cors_headers_on_any_site() {
        let allowlist = ["null".to_string()];
        for list in [&[][..], &allowlist[..]] {
            let resp = with_cors(
                sink::error_response(StatusCode::NOT_FOUND),
                Some("null"),
                cors_allow(Some("null"), list),
            );
            assert!(resp
                .headers()
                .get(header::ACCESS_CONTROL_ALLOW_ORIGIN)
                .is_none());
            assert!(resp
                .headers()
                .get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS)
                .is_none());

            let resp = cors_preflight_response(&headers(&[("origin", "null")]), list);
            assert!(resp
                .headers()
                .get(header::ACCESS_CONTROL_ALLOW_ORIGIN)
                .is_none());

            let mut pushed = Vec::new();
            push_cors(&mut pushed, Some("null"), list);
            assert!(pushed.is_empty());

            let (acao, _) = json_cors(Some("null".to_owned()), list, false);
            assert!(acao.is_none());
        }
    }

    #[test]
    fn cors_preflight_listed_origin_gets_vary() {
        let allowlist = vec!["https://a.example".to_string()];
        let resp =
            cors_preflight_response(&headers(&[("origin", "https://a.example")]), &allowlist);
        let h = resp.headers();
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).unwrap(),
            "https://a.example"
        );
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).unwrap(),
            "true"
        );
        assert_eq!(h.get(header::VARY).unwrap(), "Origin");
    }

    #[test]
    fn cors_preflight_unlisted_origin_gets_no_cors_headers() {
        let allowlist = vec!["https://a.example".to_string()];
        let resp =
            cors_preflight_response(&headers(&[("origin", "https://evil.example")]), &allowlist);
        let h = resp.headers();
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).is_none());
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).is_none());
        // Allowlist mode still varies by Origin on a deny, so a shared cache
        // doesn't serve this deny to a later allowed origin.
        assert_eq!(h.get(header::VARY).unwrap(), "Origin");
    }

    #[test]
    fn with_cors_echoes_allowed_origin_on_error() {
        for status in [
            StatusCode::BAD_REQUEST,
            StatusCode::UNAUTHORIZED,
            StatusCode::FORBIDDEN,
            StatusCode::NOT_FOUND,
            StatusCode::SERVICE_UNAVAILABLE,
        ] {
            let resp = with_cors(
                sink::error_response(status),
                Some("https://example.org"),
                CorsDecision::Allow,
            );
            let h = resp.headers();
            assert_eq!(resp.status(), status);
            assert_eq!(
                h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).unwrap(),
                "https://example.org",
                "{status}"
            );
            assert_eq!(
                h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).unwrap(),
                "true",
                "{status}"
            );
            assert_eq!(h.get(header::VARY).unwrap(), "Origin", "{status}");
        }
    }

    #[test]
    fn with_cors_varies_on_origin_on_every_decision() {
        let allowed = with_cors(
            sink::error_response(StatusCode::NOT_FOUND),
            Some("https://a.example"),
            CorsDecision::Allow,
        );
        assert_eq!(allowed.headers().get(header::VARY).unwrap(), "Origin");

        let denied = with_cors(
            sink::error_response(StatusCode::NOT_FOUND),
            Some("https://evil.example"),
            CorsDecision::Deny,
        );
        let h = denied.headers();
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).is_none());
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).is_none());
        assert_eq!(h.get(header::VARY).unwrap(), "Origin");

        let no_origin = with_cors(
            sink::error_response(StatusCode::NOT_FOUND),
            None,
            CorsDecision::Deny,
        );
        let vary: Vec<_> = no_origin.headers().get_all(header::VARY).iter().collect();
        assert_eq!(vary, ["Origin"]);
        assert!(no_origin
            .headers()
            .get(header::ACCESS_CONTROL_ALLOW_ORIGIN)
            .is_none());
    }

    #[test]
    fn with_cors_keeps_a_json_response_cors_as_built() {
        let value = serde_json::json!({ "ok": true });
        // No Origin: info.json answers `*` without credentials, varying on
        // Origin, Cookie and Authorization. `with_cors` must not add to it.
        let resp = with_cors(
            json_response(
                StatusCode::OK,
                &value,
                &headers(&[]),
                None,
                Some("*"),
                &["Origin", "Cookie", "Authorization"],
                true,
            ),
            None,
            CorsDecision::Deny,
        );
        let h = resp.headers();
        assert_eq!(h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).unwrap(), "*");
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).is_none());
        assert_eq!(h.get_all(header::VARY).iter().count(), 1);
    }

    #[test]
    fn with_cors_does_not_duplicate_an_existing_vary_origin() {
        let mut resp = sink::error_response(StatusCode::NOT_FOUND);
        resp.headers_mut()
            .insert(header::VARY, HeaderValue::from_static("Cookie, origin"));
        let resp = with_cors(resp, Some("https://evil.example"), CorsDecision::Deny);
        let vary: Vec<_> = resp.headers().get_all(header::VARY).iter().collect();
        assert_eq!(vary, ["Cookie, origin"]);
    }

    #[test]
    fn json_cors_always_varies_on_origin() {
        let allowlist = ["https://a.example".to_owned()];
        for (origin, list, acao) in [
            (None, &[][..], Some("*")),
            (
                Some("https://b.example"),
                &[][..],
                Some("https://b.example"),
            ),
            (
                Some("https://a.example"),
                &allowlist[..],
                Some("https://a.example"),
            ),
            (Some("https://b.example"), &allowlist[..], None),
        ] {
            let (got, vary) = json_cors(origin.map(str::to_owned), list, false);
            assert_eq!(got.as_deref(), acao, "{origin:?}");
            assert_eq!(vary, ["Origin"], "{origin:?}");
        }
        let (_, vary) = json_cors(None, &[], true);
        assert_eq!(vary, ["Origin", "Cookie", "Authorization"]);
    }

    #[test]
    fn json_response_listed_origin_gets_credentials_and_vary() {
        let value = serde_json::json!({ "ok": true });
        let resp = json_response(
            StatusCode::OK,
            &value,
            &headers(&[]),
            None,
            Some("https://a.example"),
            &["Origin"],
            false,
        );
        let h = resp.headers();
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).unwrap(),
            "https://a.example"
        );
        assert_eq!(
            h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).unwrap(),
            "true"
        );
        assert_eq!(h.get(header::VARY).unwrap(), "Origin");
    }

    #[test]
    fn json_response_unlisted_origin_emits_no_cors_headers() {
        let value = serde_json::json!({ "ok": true });
        let resp = json_response(
            StatusCode::OK,
            &value,
            &headers(&[]),
            None,
            None,
            &[],
            false,
        );
        let h = resp.headers();
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).is_none());
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_CREDENTIALS).is_none());
        assert!(h.get(header::VARY).is_none());
        assert!(h.get(header::CACHE_CONTROL).is_none());

        // `vary` is emitted independent of `acao`: a deny still varies.
        let resp = json_response(
            StatusCode::OK,
            &value,
            &headers(&[]),
            None,
            None,
            &["Origin"],
            false,
        );
        let h = resp.headers();
        assert!(h.get(header::ACCESS_CONTROL_ALLOW_ORIGIN).is_none());
        assert_eq!(h.get(header::VARY).unwrap(), "Origin");
    }

    #[test]
    fn json_response_private_and_multi_vary() {
        let value = serde_json::json!({ "ok": true });
        let resp = json_response(
            StatusCode::OK,
            &value,
            &headers(&[]),
            None,
            Some("*"),
            &["Cookie", "Authorization"],
            true,
        );
        let h = resp.headers();
        assert_eq!(h.get(header::VARY).unwrap(), "Cookie, Authorization");
        assert_eq!(h.get(header::CACHE_CONTROL).unwrap(), "private, no-store");
    }

    #[test]
    fn json_response_with_a_link_context_varies_on_accept() {
        let value = serde_json::json!({ "ok": true });
        let resp = json_response(
            StatusCode::OK,
            &value,
            &headers(&[]),
            Some("http://iiif.io/api/image/3/context.json"),
            Some("*"),
            &["Origin"],
            false,
        );
        assert_eq!(resp.headers().get(header::VARY).unwrap(), "Origin, Accept");
    }

    #[test]
    fn mark_private_prefixes_the_engine_cache_control() {
        for (engine, expected) in [
            (
                Some("must-revalidate, post-check=0, pre-check=0"),
                "private, must-revalidate, post-check=0, pre-check=0",
            ),
            (
                Some("public, must-revalidate, max-age=0"),
                "private, must-revalidate, max-age=0",
            ),
            (Some("private, max-age=0"), "private, max-age=0"),
            (Some("private, no-store"), "private, no-store"),
            (None, "private"),
        ] {
            let mut h = HeaderMap::new();
            if let Some(cc) = engine {
                h.insert(header::CACHE_CONTROL, HeaderValue::from_static(cc));
            }
            mark_private(&mut h);
            let cc: Vec<_> = h.get_all(header::CACHE_CONTROL).iter().collect();
            assert_eq!(cc, [expected], "{engine:?}");
        }
    }

    #[test]
    fn mark_private_adds_cookie_and_authorization_to_vary_once() {
        let mut h = HeaderMap::new();
        h.insert(header::VARY, HeaderValue::from_static("Origin, cookie"));
        mark_private(&mut h);
        let vary: Vec<_> = h.get_all(header::VARY).iter().collect();
        assert_eq!(vary, ["Origin, cookie, Authorization"]);
        mark_private(&mut h);
        let vary: Vec<_> = h.get_all(header::VARY).iter().collect();
        assert_eq!(vary, ["Origin, cookie, Authorization"]);

        // Separate `Vary` headers become one.
        let mut h = HeaderMap::new();
        h.append(header::VARY, HeaderValue::from_static("Accept"));
        h.append(header::VARY, HeaderValue::from_static("Cookie"));
        mark_private(&mut h);
        let vary: Vec<_> = h.get_all(header::VARY).iter().collect();
        assert_eq!(vary, ["Accept, Cookie, Authorization"]);
    }

    #[test]
    fn push_cors_echoes_only_a_listed_origin() {
        let allowlist = vec!["https://a.example".to_string()];
        let mut headers = Vec::new();
        push_cors(&mut headers, Some("https://a.example"), &allowlist);
        assert!(headers.contains(&(
            header::ACCESS_CONTROL_ALLOW_ORIGIN.to_string(),
            "https://a.example".to_string()
        )));

        let mut headers = Vec::new();
        push_cors(&mut headers, Some("https://evil.example"), &allowlist);
        assert!(!headers.contains(&(
            header::ACCESS_CONTROL_ALLOW_ORIGIN.to_string(),
            "https://evil.example".to_string()
        )));
        assert!(headers.is_empty());
    }

    #[test]
    fn canonical_id_with_and_without_prefix() {
        assert_eq!(
            canonical_id("https", "h", "iiif/2", "a.jp2"),
            "https://h/iiif/2/a.jp2"
        );
        assert_eq!(canonical_id("http", "h", "", "a.jp2"), "http://h/a.jp2");
    }

    #[test]
    fn client_ip_takes_rightmost_xff() {
        let h = headers(&[("x-forwarded-for", "1.1.1.1, 2.2.2.2, 3.3.3.3")]);
        assert_eq!(client_ip(&h), "3.3.3.3");
        assert_eq!(client_ip(&HeaderMap::new()), "");
    }

    #[test]
    fn parse_cookies_splits_pairs() {
        let h = headers(&[("cookie", "sid=abc; theme=dark; lone")]);
        let c = parse_cookies(&h);
        assert_eq!(
            c,
            vec![
                ("sid".to_owned(), "abc".to_owned()),
                ("theme".to_owned(), "dark".to_owned())
            ]
        );
    }

    #[test]
    fn content_disposition_escapes_quotes_and_strips_controls() {
        // Controls (here `\n`) are stripped (R8); a `"` is backslash-escaped, not
        // stripped (RFC 6266 quoted-string escaping).
        let v = content_disposition("a\"b\nc.tif").unwrap();
        assert_eq!(v.to_str().unwrap(), "inline; filename=\"a\\\"bc.tif\"");
    }

    #[test]
    fn content_disposition_non_ascii_uses_rfc6266() {
        // A non-ASCII name routes to `filename*=UTF-8''` with percent-encoded
        // UTF-8 bytes (uppercase hex).
        let v = content_disposition("café.tif").unwrap();
        assert_eq!(
            v.to_str().unwrap(),
            "inline; filename*=UTF-8''caf%C3%A9.tif"
        );
    }

    #[test]
    fn busy_response_is_503_with_retry_after() {
        // The pool-full Throttling contract: bare 503 + Retry-After: 1.
        let resp = busy_response();
        assert_eq!(resp.status(), StatusCode::SERVICE_UNAVAILABLE);
        assert_eq!(resp.headers().get(header::RETRY_AFTER).unwrap(), "1");
    }

    // The two-lane pool's acquire/shed/classify behaviour is covered by the
    // engine-free crate tests in `//src/throttling/rust:admission_test`.

    // ── Re-classification of size-restricted requests ───────────────────────

    fn swap_admission(nthreads: usize, max_waiting: usize, timeout_ms: u64) -> Admission {
        Admission::new(AdmissionConfig {
            nthreads,
            tiles_thread_ratio: 0.5,
            tiles_memory_ratio: 0.25,
            mode: AdmissionMode::Advanced,
            max_waiting,
            queue_timeout: Duration::from_millis(timeout_ms),
            large_decode_threshold_bytes: 32 * 1024 * 1024,
            memory_limit_bytes: 0,
        })
        .expect("admission config")
    }

    /// `reclassify_to_full` blocks on the runtime handle, as it does on a
    /// `spawn_blocking` thread; a multi-thread runtime drives the timer.
    fn swap_runtime() -> tokio::runtime::Runtime {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(1)
            .enable_all()
            .build()
            .unwrap()
    }

    fn take(acquired: Acquired) -> Permit {
        match acquired {
            Acquired::Admitted(p) => p,
            Acquired::Shed | Acquired::TimedOut => panic!("expected a permit"),
        }
    }

    #[test]
    fn reclassify_drops_the_tile_permit_before_the_full_acquire() {
        // One worker: the tile holds the only global permit, so the Full
        // acquire succeeds only when the tile permit is dropped first.
        let a = swap_admission(1, 4, 200);
        let rt = swap_runtime();
        let mut permit = Some(take(rt.block_on(a.acquire(AdmissionKind::Tile))));
        let _ctx = rt.enter();
        assert!(reclassify_to_full(&a, &mut permit));
        assert_eq!(permit.as_ref().map(Permit::kind), Some(AdmissionKind::Full));
        let s = a.snapshot();
        assert_eq!(s.permits_in_use, 1);
        assert_eq!(s.full_in_use, 1);
    }

    #[test]
    fn reclassify_keeps_a_full_permit() {
        let a = swap_admission(4, 0, 50);
        let rt = swap_runtime();
        let mut permit = Some(take(rt.block_on(a.acquire(AdmissionKind::Full))));
        let _ctx = rt.enter();
        assert!(reclassify_to_full(&a, &mut permit));
        assert_eq!(permit.as_ref().map(Permit::kind), Some(AdmissionKind::Full));
        assert_eq!(a.snapshot().full_in_use, 1);
    }

    #[test]
    fn reclassify_fails_when_the_full_lane_sheds_or_times_out() {
        // nthreads 2 → full_max 1, held by another request. Queue disabled: shed;
        // queue enabled: the wait times out. Either way the tile permit is gone.
        for (max_waiting, timeout_ms) in [(0, 50), (4, 20)] {
            let a = swap_admission(2, max_waiting, timeout_ms);
            let rt = swap_runtime();
            let _other = take(rt.block_on(a.acquire(AdmissionKind::Full)));
            let mut permit = Some(take(rt.block_on(a.acquire(AdmissionKind::Tile))));
            let _ctx = rt.enter();
            assert!(
                !reclassify_to_full(&a, &mut permit),
                "max_waiting {max_waiting}"
            );
            assert!(permit.is_none());
            assert_eq!(a.snapshot().permits_in_use, 1, "only the other Full holds");
        }
    }

    #[test]
    fn the_before_decode_hook_refuses_only_a_failed_swap() {
        // nthreads 2 → full_max 1. Free: the hook swaps and lets the decode go
        // on (0). Held by another request with the queue disabled: it refuses (1).
        for (other_holds_full, answer) in [(false, 0), (true, 1)] {
            let a = swap_admission(2, 0, 50);
            let rt = swap_runtime();
            let _other =
                other_holds_full.then(|| take(rt.block_on(a.acquire(AdmissionKind::Full))));
            let mut permit = Some(take(rt.block_on(a.acquire(AdmissionKind::Tile))));
            let _ctx = rt.enter();
            let mut swap = LaneSwap {
                admission: &a,
                permit: &mut permit,
            };
            assert_eq!(
                swap_lane_before_decode(std::ptr::from_mut(&mut swap).cast()),
                answer
            );
            let expected = (!other_holds_full).then_some(AdmissionKind::Full);
            assert_eq!(permit.as_ref().map(Permit::kind), expected);
        }
    }

    #[test]
    fn a_swapped_request_records_no_classifier_disagreement() {
        // A small restricted region classifies as Tile, but the engine decodes
        // the whole image (estimate ≥ threshold). Recording the held lane after
        // the swap agrees with the engine; the pre-dispatch class would not.
        let a = swap_admission(4, 0, 50);
        let threshold = a.snapshot().large_decode_threshold_bytes;
        let rt = swap_runtime();
        let mut permit = Some(take(rt.block_on(a.acquire(AdmissionKind::Tile))));
        let _ctx = rt.enter();
        assert!(reclassify_to_full(&a, &mut permit));
        a.record_classification(permit.as_ref().unwrap().kind(), threshold);
        assert_eq!(a.snapshot().classifier_disagreement_total, 0);
        a.record_classification(AdmissionKind::Tile, threshold);
        assert_eq!(a.snapshot().classifier_disagreement_total, 1);
    }

    // ── /server docroot fileserver pure helpers ──────────────────────────────

    #[test]
    fn parse_byte_range_well_formed() {
        assert_eq!(parse_byte_range("bytes=0-99", 1000), Some((0, 99)));
        assert_eq!(parse_byte_range("bytes=200-299", 1000), Some((200, 299)));
        assert_eq!(parse_byte_range("bytes=999-999", 1000), Some((999, 999)));
    }

    #[test]
    fn parse_byte_range_open_ended_defaults_to_eof() {
        assert_eq!(parse_byte_range("bytes=0-", 1000), Some((0, 999)));
        assert_eq!(parse_byte_range("bytes=500-", 1000), Some((500, 999)));
    }

    #[test]
    fn parse_byte_range_clamps_end_to_eof() {
        // end beyond the file is clamped to fsize-1 (RFC 7233 §2.1).
        assert_eq!(parse_byte_range("bytes=0-99999", 1000), Some((0, 999)));
    }

    #[test]
    fn parse_byte_range_rejects_inverted_and_out_of_bounds() {
        // Inverted range (end < start) → None, so the caller 500s instead of
        // underflowing `end - start + 1`.
        assert_eq!(parse_byte_range("bytes=100-50", 1000), None);
        assert_eq!(parse_byte_range("bytes=999-0", 1000), None);
        // start at/after EOF → None (C++ sendFile throws → 500).
        assert_eq!(parse_byte_range("bytes=1000-1000", 1000), None);
        assert_eq!(parse_byte_range("bytes=5000-6000", 1000), None);
    }

    #[test]
    fn parse_byte_range_rejects_malformed() {
        assert_eq!(parse_byte_range("0-99", 1000), None); // no `bytes=`
        assert_eq!(parse_byte_range("bytes=-500", 1000), None); // suffix range unsupported
        assert_eq!(parse_byte_range("bytes=abc", 1000), None); // no `-`
    }

    #[test]
    fn leading_u64_parses_leading_digits_only() {
        assert_eq!(leading_u64("123"), Some(123));
        assert_eq!(leading_u64("123abc"), Some(123));
        assert_eq!(leading_u64(""), None);
        assert_eq!(leading_u64("abc"), None);
    }

    #[test]
    fn extension_of_lowercases_last_component_suffix() {
        assert_eq!(extension_of("/foo.HTML").as_deref(), Some("html"));
        assert_eq!(extension_of("/a/b/c.LUA").as_deref(), Some("lua"));
        assert_eq!(extension_of("/dir.with.dot/file").as_deref(), None);
        assert_eq!(extension_of("/no-extension").as_deref(), None);
    }

    #[test]
    fn decode_path_suffix_rejects_null_and_control() {
        assert_eq!(decode_path_suffix("/test%00.html"), None);
        assert_eq!(decode_path_suffix("/a%0db"), None); // CR
        assert_eq!(
            decode_path_suffix("/sub/file.bin").as_deref(),
            Some("/sub/file.bin")
        );
        assert_eq!(
            decode_path_suffix("/a%20b.bin").as_deref(),
            Some("/a b.bin") // %20 → space (a valid path char)
        );
    }

    #[test]
    fn permission_vocabulary_is_identical_on_both_gates() {
        for permission in PERMISSIONS {
            let wire = permission_str(permission);
            assert_eq!(permission_from_str(wire), permission, "{wire} round-trip");
            // The runtime gate splits the vocabulary: clickthrough/kiosk/external
            // are IIIF-only, the rest are the base set `file_pre_flight` shares.
            let extended_only = matches!(
                permission,
                SipiPermType::Clickthrough | SipiPermType::Kiosk | SipiPermType::External
            );
            assert!(
                scripting::valid_permission(wire, true),
                "{wire} must pass the IIIF gate"
            );
            assert_eq!(
                scripting::valid_permission(wire, false),
                !extended_only,
                "{wire} base-set gate"
            );
        }
    }

    fn preflight_state(lua: Arc<scripting::LuaEnv>) -> AppState {
        let admission = Arc::new(
            Admission::new(AdmissionConfig {
                nthreads: 2,
                tiles_thread_ratio: 0.5,
                tiles_memory_ratio: 0.25,
                mode: AdmissionMode::Basic,
                max_waiting: 2,
                queue_timeout: Duration::from_secs(1),
                large_decode_threshold_bytes: 32 * 1024 * 1024,
                memory_limit_bytes: 0,
            })
            .expect("admission config"),
        );
        AppState {
            ready: true,
            imgroot: "/img".to_string(),
            resolved_imgroot: "/img".to_string(),
            prefix_as_path: true,
            has_preflight: true,
            has_file_preflight: true,
            lua: Some(lua),
            admission,
            routes: Vec::new(),
            max_post_size: crate::config::DEFAULT_MAX_POST_SIZE,
            docroot: String::new(),
            wwwroute: String::new(),
            preflight_cache: preflight_cache::PreflightCache::new(16, Duration::from_secs(60)),
            allowed_origins: Vec::new(),
            public_hosts: Vec::new(),
        }
    }

    #[test]
    fn every_resolved_decision_is_counted_once_under_its_permission() {
        // The decision counters are process-global, so all three paths are walked
        // in one test: a sibling test resolving a decision in parallel would make
        // the deltas unreadable.
        let dir = tempfile::tempdir().expect("tempdir");
        let init = dir.path().join("init.lua");
        std::fs::write(
            &init,
            "function pre_flight(prefix, identifier, cookie)\n\
             return 'stream', '/img/' .. identifier\n\
             end\n\
             function file_pre_flight(filepath, cookie)\n\
             return 'login', filepath\n\
             end\n",
        )
        .expect("write init");
        let state = preflight_state(Arc::new(scripting::LuaEnv::new(
            dir.path().to_path_buf(),
            Some(init),
            String::new(),
            scripting::LimitConfig::default(),
            scripting::bindings::ConfigValues::default(),
        )));
        let parsed = ParsedRequest {
            kind: RequestKind::Iiif,
            prefix: "unit".to_string(),
            identifier: "lena.jp2".to_string(),
            params: None,
        };
        let uri: Uri = "/unit/lena.jp2/full/max/0/default.jpg".parse().unwrap();
        let hdrs = headers(&[]);
        let before: Vec<u64> = PERMISSIONS.iter().map(|p| decisions(*p)).collect();

        // 1. IIIF route, hook run fresh (the cache is empty, so this is a miss).
        let hits_before = preflight_cache::hits();
        let access = iiif_access(&state, &parsed, &Method::GET, &uri, &hdrs)
            .unwrap_or_else(|_| panic!("hook decision"));
        assert_eq!(access.permission, SipiPermType::Stream);
        assert_eq!(access.infile, "/img/lena.jp2");
        assert_eq!(decisions(SipiPermType::Stream), before[7] + 1);

        // 2. IIIF route again, same key: the cache answers and the hook never runs.
        let access = iiif_access(&state, &parsed, &Method::GET, &uri, &hdrs)
            .unwrap_or_else(|_| panic!("cached decision"));
        assert_eq!(access.permission, SipiPermType::Stream);
        assert_eq!(
            preflight_cache::hits(),
            hits_before + 1,
            "second call must be served from the cache"
        );
        assert_eq!(decisions(SipiPermType::Stream), before[7] + 2);

        // 3. `/file`, whose hook refuses: a refused decision is resolved too.
        let file_uri: Uri = "/unit/lena.jp2/file".parse().unwrap();
        let refused = file_access(&state, &parsed, &Method::GET, &file_uri, &hdrs)
            .err()
            .expect("login is refused on /file");
        assert_eq!(refused.status(), StatusCode::UNAUTHORIZED);
        assert_eq!(decisions(SipiPermType::Login), before[1] + 1);

        // Nothing else moved, and nothing moved twice.
        let deltas: Vec<u64> = PERMISSIONS
            .iter()
            .enumerate()
            .map(|(i, p)| decisions(*p) - before[i])
            .collect();
        assert_eq!(deltas, vec![0, 1, 0, 0, 0, 0, 0, 2]);
    }

    fn dims(w: u32, h: u32) -> Result<(u32, u32), RestrictRefusal> {
        restriction_of("!128,128", false, w, h)
    }

    fn restriction_of(
        size: &str,
        watermarked: bool,
        w: u32,
        h: u32,
    ) -> Result<(u32, u32), RestrictRefusal> {
        restriction(size, watermarked, w, h, "id").map(|(w, h)| (w.get(), h.get()))
    }

    #[test]
    fn restrict_watermark_treats_empty_as_absent() {
        let access = |wm: &str| Access {
            infile: String::new(),
            permission: SipiPermType::Restrict,
            kv: vec![("watermark".into(), wm.into())],
        };
        assert_eq!(restrict_watermark(&access("")), None);
        assert_eq!(restrict_watermark(&access("wm.tif")), Some("wm.tif"));
    }

    #[test]
    fn restriction_unparsable_size_is_misconfigured() {
        assert_eq!(
            restriction_of("nonsense", false, 512, 512),
            Err(RestrictRefusal::Misconfigured)
        );
    }

    #[test]
    fn restriction_refuses_an_axis_below_two_pixels() {
        assert_eq!(dims(2000, 10), Err(RestrictRefusal::Refuse));
    }

    #[test]
    fn restriction_never_yields_zero() {
        assert_eq!(
            iiif_parser::clamp_dims_to_size("!128,128", 0, 0).unwrap(),
            (1, 1)
        );
        assert_eq!(dims(0, 0), Err(RestrictRefusal::Refuse));
    }

    #[test]
    fn restriction_refuses_never_reducing_kinds_without_a_watermark() {
        for size in ["max", "pct:100", "pct:150", "^pct:200", "red:0"] {
            assert_eq!(
                restriction_of(size, false, 512, 512),
                Err(RestrictRefusal::Refuse),
                "{size}"
            );
            assert_eq!(
                restriction_of(size, true, 512, 512),
                Ok((512, 512)),
                "{size} with a watermark"
            );
        }
    }

    #[test]
    fn restriction_caps_box_kinds() {
        assert_eq!(restriction_of("^!128,128", false, 512, 512), Ok((128, 128)));
        assert_eq!(dims(2368, 1792), Ok((128, 97)));
    }

    #[test]
    fn restriction_serves_an_image_inside_the_box_at_native_size() {
        assert_eq!(dims(100, 80), Ok((100, 80)));
        assert_eq!(restriction_of("5000,", false, 2000, 1000), Ok((2000, 1000)));
    }
}
