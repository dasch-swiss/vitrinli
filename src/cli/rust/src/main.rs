//! Thin default entry point for the SIPI Rust shell.
//!
//! `cli/rust` owns `main` and the verb dispatch; all server behaviour lives in the
//! `sipi` library (`//src/server/rust`). The `server` verb runs the axum shell;
//! `health` is a Rust-native loopback probe (no FFI); every other argv (offline
//! subcommands, `--version`, `--help`) is handed to the C++ CLI (`sipi_cli_main`)
//! verbatim. A downstream crate can replace this binary with its own `main`
//! while reusing the `sipi` library.

// Fast unsafe check (CI `lint` gate): every `unsafe {}` block must carry a
// `// SAFETY:` comment. `allow`-by-default (clippy `restriction` group), so it
// is enabled here explicitly; CI's `-Dwarnings` promotes it to a hard error.
#![warn(clippy::undocumented_unsafe_blocks)]

mod commands;
mod ffi;

use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::process::ExitCode;

fn main() -> ExitCode {
    #[cfg(feature = "mimalloc")]
    allocator::init();

    // The `rustls-no-provider` feature on `sentry` (below) pulls in TLS support
    // without forcing a crypto provider — install `ring` explicitly. `sentry`'s
    // plain `rustls` feature would otherwise resolve to reqwest's aws-lc-rs
    // path, and aws-lc-sys's cmake build script panics inside a Bazel action
    // (banned outright in MODULE.bazel); this repo's other rustls consumers
    // (tonic/hyper-rustls, for OTLP) already resolve against `ring`.
    rustls::crypto::ring::default_provider()
        .install_default()
        .expect("failed to install the rustls ring crypto provider");

    // `sentry_rust_minidump::init` re-spawns/reconnects via a
    // `--crash-reporter-server=<socket>` argv entry (hardcoded by the
    // `minidumper-child` crate it wraps — verified against the exact resolved
    // version, 0.3.0: `is_crash_reporter_process` checks
    // `env::args().any(|a| a.starts_with("--crash-reporter-server"))`, and the
    // parent spawns the reporter as `current_exe()` with *only* that one arg;
    // not configurable through this wrapper). Handle it here, first,
    // unconditionally, and return without ever reaching verb dispatch:
    // `minidumper-child`'s own "force exit so app code doesn't run after this"
    // contract only fires on ITS success path (a bind/setup failure returns an
    // `Err` instead) — without this early return, a reporter that fails to
    // start would fall through to `run_cli(&argv)` with the
    // `--crash-reporter-server=...` flag as its sole argument, which
    // `sipi_cli_main` doesn't recognize as any option or subcommand.
    //
    // Gated by the `minidump` crate feature (BUILD.bazel) — a build without
    // it never spawns a reporter child, so this argv entry never appears.
    #[cfg(feature = "minidump")]
    if std::env::args().any(|a| a.starts_with("--crash-reporter-server")) {
        let _sentry_guard = init_sentry();
        if let Some(client) = sentry::Hub::current().client() {
            let _ = sentry_rust_minidump::init(&client);
        }
        return ExitCode::SUCCESS;
    }

    let argv: Vec<String> = std::env::args().collect();
    // The verb is the first non-flag token after argv[0].
    let verb_idx = argv
        .iter()
        .enumerate()
        .skip(1)
        .find(|(_, a)| !a.starts_with('-'))
        .map(|(i, _)| i);
    // Only read below when the `minidump` feature (BUILD.bazel) is on.
    #[cfg_attr(not(feature = "minidump"), allow(unused_variables))]
    let verb = verb_idx.map(|idx| argv[idx].as_str());

    // Sentry client — panics + handled events, uniformly for every verb
    // (`server`, `health`, and the offline verbs behind `sipi_cli_main`) once
    // a DSN is configured (`None` otherwise — see `init_sentry`). Held for
    // the life of `main`: `ClientInitGuard`'s `Drop` blocks to flush any
    // pending events before the process exits.
    let _sentry_guard = init_sentry();

    // Native-crash (minidump) reporting, `server` only. `sipi health` runs on a
    // short interval in prod and must never fork a reporter child of its own;
    // CLI `convert` crash coverage is out of scope here, since it never reaches
    // Rust code (see the module doc).
    // `Hub::current().client()` is `Some` only when `init_sentry` actually
    // called `sentry::init` (a valid DSN was configured) — a DSN-less run
    // never forks a reporter child or installs crash-signal handlers.
    //
    // Gated by the `minidump` crate feature (BUILD.bazel).
    #[cfg(feature = "minidump")]
    let _minidump_guard = if verb == Some("server") {
        sentry::Hub::current()
            .client()
            .and_then(|client| sentry_rust_minidump::init(&client).ok())
    } else {
        None
    };

    match verb_idx {
        // `server` → the Rust shell. Pass the slice from the verb onward; clap
        // treats argv[idx] ("server"/"health") as the binary name and skips it.
        Some(idx) if argv[idx] == "server" => commands::server::run(&argv[idx..]),
        Some(idx) if argv[idx] == "health" => commands::health::run(&argv[idx..]),
        _ => run_cli(&argv),
    }
}

/// The binary's side of the allocator contract: this target links mimalloc as
/// the process-wide malloc override on Linux (`_ALLOCATOR` in BUILD.bazel),
/// so it owns verifying the override took and supplying allocator stats to
/// the `sipi` library's gauges (`sipi::malloc_stats` deliberately stays
/// allocator-agnostic — a downstream crate with its own `main` chooses its
/// own allocator). The `mimalloc` crate feature is set by BUILD.bazel under
/// exactly the condition the dep is linked (Linux, non-ASan), so this module
/// and its `mi_*` extern references drop out together with the library.
/// See docs/adr/0019-mimalloc-production-allocator.md.
#[cfg(feature = "mimalloc")]
mod allocator {
    use std::ffi::c_void;

    // These `mi_*` / `sipi_mi_stats_read` symbols are declared here, inside the
    // `#[cfg(feature = "mimalloc")]` module, rather than in the shell's shared
    // seam bindings (`sipi::ffi`): the block is valid only when mimalloc is
    // actually linked, and colocating it under the same feature gate makes the
    // declarations vanish in lock-step with the dep (ASan / non-Linux builds
    // link no mimalloc, so nothing here must resolve). Kept out of a central FFI
    // file on purpose — a stray, always-compiled declaration would reference
    // symbols that need not exist. See docs/adr/0019-mimalloc-production-allocator.md.
    extern "C" {
        fn mi_version() -> core::ffi::c_int;
        fn mi_is_in_heap_region(p: *const c_void) -> bool;
        // `mi_stats_shim.c` (`:mi_stats_shim` in BUILD.bazel). Never re-declare
        // mimalloc's stats API in Rust: nothing verifies a hand-mirrored
        // declaration against the header.
        fn sipi_mi_stats_read(
            malloc_normal_current: *mut i64,
            malloc_huge_current: *mut i64,
            rss_current: *mut i64,
        ) -> bool;
    }

    /// Verify interposition (abort on failure) and register the mimalloc
    /// stats reader with the library's allocator gauges.
    pub(super) fn init() {
        verify_interposition();
        sipi::malloc_stats::set_source(stats);
    }

    /// Verify that the statically linked mimalloc interposes *libc-internal*
    /// allocations, not just the binary's own — the two bind through
    /// different mechanisms. Code linked into the binary resolves
    /// `malloc`/`free` at static link time and can never miss; allocations
    /// made inside libc.so.6 (`getcwd(NULL, 0)`, `scandir`, …) go through
    /// libc's own GOT and reach mimalloc only if the executable exports the
    /// override symbols to its dynamic table. A process where those two
    /// disagree is in a latent heap-corruption state — glibc-malloc'd
    /// pointers handed to mimalloc's `free` (e.g. the `scandir` entries the
    /// engine's cache frees) — so a failed probe aborts rather than serving.
    ///
    /// `getcwd(NULL, 0)` is the probe because glibc documents it as
    /// allocating the buffer itself, inside libc.so.6.
    fn verify_interposition() {
        // SAFETY: `getcwd(NULL, 0)` is the documented glibc extension that
        // allocates a sufficiently large buffer and returns it (NULL on error).
        let p = unsafe { libc::getcwd(std::ptr::null_mut(), 0) };
        if p.is_null() {
            // getcwd failure (e.g. deleted cwd) says nothing about the allocator.
            return;
        }
        // SAFETY: `p` is a valid pointer returned by getcwd; mi_is_in_heap_region
        // only inspects mimalloc's own region map and never dereferences `p`.
        if unsafe { mi_is_in_heap_region(p.cast()) } {
            // SAFETY: `p` was allocated by (interposed) malloc and is freed
            // exactly once, through the same allocator.
            unsafe { libc::free(p.cast()) };
        } else {
            // Deliberately leak `p`: freeing a glibc-malloc'd pointer through
            // the statically bound mimalloc `free` is the very defect being
            // detected.
            // SAFETY: mi_version takes no arguments and reads a constant.
            let version = unsafe { mi_version() };
            eprintln!(
                "fatal: mimalloc v{version} is linked, but libc-internal allocations \
                 bypass it (override symbols not exported to the dynamic table); \
                 refusing to run with mixed allocators"
            );
            std::process::abort();
        }
    }

    /// Read mimalloc's accounting into the library's allocator-neutral
    /// [`sipi::malloc_stats::MallocStats`] shape (field mapping documented
    /// there). `None` when the shim's size/version handshake with
    /// `mi_stats_get` fails — wrong-layout numbers are worse than no numbers.
    fn stats() -> Option<sipi::malloc_stats::MallocStats> {
        let mut malloc_normal: i64 = 0;
        let mut malloc_huge: i64 = 0;
        let mut rss: i64 = 0;
        // SAFETY: the three out-pointers are valid and distinct; the shim
        // writes all of them before returning true and touches nothing else.
        let ok = unsafe { sipi_mi_stats_read(&mut malloc_normal, &mut malloc_huge, &mut rss) };
        if !ok {
            return None;
        }
        let in_use = malloc_normal.saturating_add(malloc_huge);
        // `arena`/`retained` report the true resident set (`mi_process_info`
        // current_rss), not mimalloc's `committed` — which ratchets and reads
        // far above RSS, so it misled as an OOM proxy. `retained` is the
        // resident memory not currently handed out to the application.
        Some(sipi::malloc_stats::MallocStats {
            in_use_bytes: in_use,
            retained_bytes: rss.saturating_sub(in_use).max(0),
            mmap_bytes: malloc_huge,
            arena_bytes: rss,
        })
    }

    #[cfg(test)]
    mod tests {
        /// The reader must survive a real `mi_stats_get` round-trip against the
        /// linked mimalloc and report live allocations.
        /// `//src/cli/rust:sipi_unit_test` links mimalloc exactly like the
        /// binary, so this is the in-CI execution of the stats FFI.
        #[test]
        fn stats_reads_live_mimalloc_accounting() {
            // Hold 1 MiB of small binned blocks — the `malloc_normal` class.
            // A single large Vec would land in `malloc_huge` (> 512 KiB),
            // which mimalloc tracks even with binned accounting compiled out
            // (`MI_STAT=0`), and would miss a stats-level regression in
            // bazel/mimalloc.BUILD.bazel's `MI_STAT=1` define.
            let held: Vec<Vec<u8>> = (0..256).map(|_| vec![0u8; 4096]).collect();
            let held_bytes: i64 = held.iter().map(|b| b.len() as i64).sum();
            let stats = super::stats().expect("mi_stats_get size/version handshake");
            assert!(
                stats.in_use_bytes >= held_bytes,
                "in_use ({}) must cover {held_bytes} B of live binned allocations",
                stats.in_use_bytes
            );
            assert!(stats.arena_bytes > 0, "resident set backs the heap");
            assert!(stats.retained_bytes >= 0);
            assert!(stats.mmap_bytes >= 0);
            drop(std::hint::black_box(held));
        }
    }
}

/// `SIPI_SENTRY_DSN` empty/unset/unparseable ⇒ `None`, and `sentry::init` is
/// never called at all on an empty DSN. `release`/`environment` share their
/// source with the OTel resource attributes (`sipi::telemetry`), so both
/// observability backends agree. `traces_sample_rate: 0.0`: Sentry owns
/// crashes/panics/errors here, OTLP owns traces; a nonzero rate would duplicate
/// transactions across both backends.
fn init_sentry() -> Option<sentry::ClientInitGuard> {
    let dsn = std::env::var("SIPI_SENTRY_DSN")
        .ok()
        .filter(|s| !s.is_empty())
        .and_then(|raw| match raw.parse() {
            Ok(dsn) => Some(dsn),
            Err(e) => {
                eprintln!("sipi: SIPI_SENTRY_DSN is not a valid Sentry DSN ({e}); Sentry disabled");
                None
            }
        })?;
    Some(sentry::init(sentry::ClientOptions {
        dsn: Some(dsn),
        release: sipi::telemetry::service_version().map(Into::into),
        environment: Some(sipi::telemetry::deployment_environment().into()),
        attach_stacktrace: true,
        traces_sample_rate: 0.0,
        ..Default::default()
    }))
}

/// Hand the full argv to the C++ CLI (`sipi_cli_main`) and return its exit code.
fn run_cli(argv: &[String]) -> ExitCode {
    // Marshal argv into a C `char**`. `sipi_cli_main` is synchronous and does
    // not retain the pointers, so the CStrings only need to outlive the call.
    let c_args: Vec<CString> = match argv
        .iter()
        .map(|a| CString::new(a.as_str()))
        .collect::<Result<_, _>>()
    {
        Ok(v) => v,
        Err(_) => {
            eprintln!("sipi: argument contains an interior NUL byte");
            return ExitCode::FAILURE;
        }
    };
    let mut c_ptrs: Vec<*mut c_char> = c_args.iter().map(|c| c.as_ptr() as *mut c_char).collect();

    let id = correlation_id(std::env::var("SIPI_CORRELATION_ID").ok().as_deref());
    sentry::configure_scope(|scope| scope.set_tag("correlation_id", &id));
    let c_id = CString::new(id).expect("a correlation ID holds no NUL byte");
    // SAFETY: `c_id` is a NUL-terminated string that outlives the call; the
    // setter copies it and retains no pointer.
    unsafe { ffi::sipi_cli_set_correlation_id(c_id.as_ptr()) };

    // SAFETY: `c_ptrs` is a valid argv-shaped array of `argc` NUL-terminated
    // strings that outlive the synchronous call; the seam guarantees no C++
    // exception unwinds across the boundary (it returns a status code).
    let code = unsafe { ffi::sipi_cli_main(c_ptrs.len() as c_int, c_ptrs.as_mut_ptr()) };
    // Process exit codes are a single byte; CLI11/command codes (0/1/105/106)
    // all fit.
    ExitCode::from(code as u8)
}

/// The correlation ID of an offline-verb run: `supplied` (`SIPI_CORRELATION_ID`)
/// when it is 1-128 characters of `[A-Za-z0-9._:-]`, else 16 random bytes in
/// lowercase hex. The C++ CLI splices the ID raw into its JSON fallback
/// document and its log prefix, so only this charset may reach it.
fn correlation_id(supplied: Option<&str>) -> String {
    match supplied.filter(|s| !s.is_empty()) {
        Some(id) if is_valid_correlation_id(id) => id.to_owned(),
        Some(_) => {
            eprintln!(
                "sipi: SIPI_CORRELATION_ID is not 1-128 characters of [A-Za-z0-9._:-]; \
                 using a generated ID"
            );
            random_correlation_id()
        }
        None => random_correlation_id(),
    }
}

fn is_valid_correlation_id(id: &str) -> bool {
    (1..=128).contains(&id.len())
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b':' | b'-'))
}

fn random_correlation_id() -> String {
    let mut bytes = [0u8; 16];
    // SAFETY: `bytes` is a valid, writable 16-byte buffer; getentropy accepts
    // up to 256 bytes and writes only into it.
    let rc = unsafe { libc::getentropy(bytes.as_mut_ptr().cast(), bytes.len()) };
    assert_eq!(
        rc,
        0,
        "getentropy failed: {}",
        std::io::Error::last_os_error()
    );
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

#[cfg(test)]
mod correlation_id_tests {
    use super::*;

    #[test]
    fn valid_supplied_id_is_kept() {
        assert_eq!(correlation_id(Some("e2e-123")), "e2e-123");
        assert_eq!(correlation_id(Some("a.b_c:d-E9")), "a.b_c:d-E9");
        let longest = "x".repeat(128);
        assert_eq!(correlation_id(Some(&longest)), longest);
    }

    #[test]
    fn invalid_or_missing_id_is_generated() {
        let too_long = "x".repeat(129);
        for supplied in [
            None,
            Some(""),
            Some("a b"),
            Some("a\"b"),
            Some(too_long.as_str()),
        ] {
            let id = correlation_id(supplied);
            assert_eq!(id.len(), 32, "{supplied:?} gave {id}");
            assert!(id
                .bytes()
                .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase()));
        }
    }

    #[test]
    fn generated_ids_differ() {
        assert_ne!(random_correlation_id(), random_correlation_id());
    }
}
