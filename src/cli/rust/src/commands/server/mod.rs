//! The `server` verb: parse the flags and run the `sipi` library's axum shell.
//!
//! The clap surface lives in [`args`] (assembled from per-domain flatten
//! groups); this module owns the verb handler and the
//! `From<&ServerArgs> for ServerOverrides` mapping — the binary knows the CLI
//! shape, the `sipi` library takes the Rust-native overrides bag.
//!
//! The override channel into the engine (the `repr(C)` struct + the `sipi_init`
//! apply block) lives in `server/rust/config.rs`.

mod args;

use args::{
    CacheArgs, ConcurrencyArgs, KnoraArgs, LimitsArgs, LoggingArgs, NetworkArgs, PathArgs,
    ServerArgs, TlsAuthArgs,
};
use clap::Parser;
use sipi::ServerOverrides;
use std::process::ExitCode;

impl From<&ServerArgs> for ServerOverrides {
    fn from(args: &ServerArgs) -> Self {
        // Fail-on-omission (DUNE-006): every clap flag group is destructured
        // exhaustively — no `..` rest pattern — so a NEW `server` flag fails to
        // compile here until it is either forwarded into `ServerOverrides` or
        // explicitly bound `field: _` (accepted-but-not-forwarded). Without this,
        // a forgotten forward is a silent drop the engine never sees.
        //
        // Deliberately NOT forwarded (bound `_`): sslport/sslcert/sslkey,
        // keepalive, hostname, logfile (the shell does not act on them);
        // `config` + `drain_timeout` + the whole concurrency group (`nthreads`,
        // `max_waiting`, `queue_timeout`, preflight-cache knobs) are Rust-owned
        // serve knobs handed straight to `sipi::run`, not layered onto the engine
        // config. The deprecated cache aliases (--cachedir/--cachesize/
        // --cachenfiles) collapse onto their canonical field — canonical wins if
        // both are set (the precedence between the two spellings is not a
        // contract).
        let ServerArgs {
            config: _,
            network,
            concurrency,
            limits,
            paths,
            cache,
            tls_auth,
            knora,
            logging,
            drain_timeout: _,
        } = args;
        let NetworkArgs {
            serverport,
            sslport: _,
            hostname: _,
            keepalive: _,
        } = network;
        let PathArgs {
            imgroot,
            docroot,
            wwwroute,
            scriptdir,
            tmpdir,
            maxtmpage,
            initscript,
            pathprefix,
        } = paths;
        let CacheArgs {
            cache_dir,
            cache_size,
            cache_nfiles,
            cachedir,
            cachesize,
            cachenfiles,
        } = cache;
        let LimitsArgs {
            maxpost,
            memory_limit,
            admission_mode,
            tiles_memory_ratio,
            large_decode_threshold_bytes,
            decode_timeout_ms,
            thumbsize,
        } = limits;
        let TlsAuthArgs {
            sslcert: _,
            sslkey: _,
            jwtkey,
        } = tls_auth;
        let KnoraArgs {
            knorapath,
            knoraport,
        } = knora;
        let LoggingArgs {
            logfile: _,
            loglevel,
        } = logging;
        let ConcurrencyArgs {
            nthreads: _,
            tiles_thread_ratio: _,
            max_waiting: _,
            queue_timeout: _,
            preflight_cache_ttl: _,
            preflight_cache_slots: _,
            max_wedged_threads: _,
        } = concurrency;

        ServerOverrides {
            serverport: *serverport,
            imgroot: imgroot.clone(),
            scriptdir: scriptdir.clone(),
            initscript: initscript.clone(),
            tmpdir: tmpdir.clone(),
            maxtmpage: *maxtmpage,
            docroot: docroot.clone(),
            wwwroute: wwwroute.clone(),
            pathprefix: *pathprefix,
            jwtkey: jwtkey.clone(),
            cache_dir: cache_dir.clone().or_else(|| cachedir.clone()),
            cache_size: cache_size.clone().or_else(|| cachesize.clone()),
            cache_nfiles: cache_nfiles.or(*cachenfiles),
            memory_limit: memory_limit.clone(),
            admission_mode: admission_mode.clone(),
            tiles_memory_ratio: *tiles_memory_ratio,
            large_decode_threshold_bytes: *large_decode_threshold_bytes,
            decode_timeout_ms: *decode_timeout_ms,
            maxpost: maxpost.clone(),
            thumbsize: thumbsize.clone(),
            knorapath: knorapath.clone(),
            knoraport: knoraport.clone(),
            loglevel: loglevel.clone(),
            // jpeg_quality + scaling_quality are TOML-config-only (no CLI flag),
            // so the clap path never sets them.
            jpeg_quality: None,
            scaling_quality: Default::default(),
            // Lua-config-only (the CLI --hostname/--sslport transport flags are
            // deliberately not forwarded; these fields feed the Lua `config`
            // table and are set by the Lua config parse alone).
            hostname: None,
            sslport: None,
        }
    }
}

/// Parse the `server` flags (argv from the "server" token onward) and run the
/// axum server. Blocks until shutdown; returns the process exit code.
pub fn run(server_argv: &[String]) -> ExitCode {
    let args = match ServerArgs::try_parse_from(server_argv) {
        Ok(args) => args,
        Err(e) => {
            // clap renders help/version to stdout and usage errors to stderr;
            // mirror its own exit codes — 0 for `--help`/`--version`, 2 for a
            // usage error. We never call clap's process-exiting `.exit()`.
            let _ = e.print();
            return ExitCode::from(e.exit_code() as u8);
        }
    };

    let overrides = ServerOverrides::from(&args);
    sipi::run(
        args.config,
        overrides,
        args.drain_timeout,
        args.concurrency.nthreads,
        args.concurrency.tiles_thread_ratio,
        args.concurrency.max_waiting,
        args.concurrency.queue_timeout,
        args.concurrency.preflight_cache_ttl,
        args.concurrency.preflight_cache_slots,
        args.concurrency.max_wedged_threads,
    )
}
