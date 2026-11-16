//! The `server` verb's argument set, assembled from per-domain flatten groups.
//!
//! [`ServerArgs`] is the clap `Parser`; each `#[derive(Args)]` group is
//! flattened in so `--help` renders sectioned. Group structs are crate-internal:
//! only `ServerArgs` is the public surface, because the binary owns the CLI and
//! the `sipi` library takes a Rust-native `ServerOverrides`.
//!
//! Each overridable flag is an `Option<T>` carrying its `SIPI_*` env var, with
//! NO `default_value`, so an unset flag stays `None` and falls through to the
//! loaded Lua config; precedence is `config < env < CLI`. Every engine-behaviour
//! flag forwards into `ServerOverrides` (see `mod.rs`'s `From<&ServerArgs>`);
//! the transport flags the Rust shell owns (`sslport`/`sslcert`/`sslkey`,
//! `keepalive`, `max-waiting`/`queue-timeout`, `hostname`, `nthreads`,
//! `logfile`) are accepted for CLI compatibility but stay unforwarded.
//!
//! Two fields stay top-level rather than in a group:
//! - `config` is bootstrap: it *selects* the base Lua config the overrides
//!   layer onto; it is not itself an override.
//! - `drain_timeout` is a Rust-owned serve knob (the graceful-drain deadline),
//!   not a config override, so it is handed straight to `sipi::run`.
//!
//! `term_width = 0` pins `--help`'s wrapping to clap's fixed default rather
//! than the invoking terminal's width, so the heading-order snapshot in
//! `test/e2e/tests/cli.rs::sipi_server_help_heading_order` is stable across
//! CI and local dev.

mod cache;
mod concurrency;
mod knora;
mod limits;
mod logging;
mod network;
mod paths;
mod tls_auth;

pub use cache::CacheArgs;
pub use concurrency::ConcurrencyArgs;
pub use knora::KnoraArgs;
pub use limits::LimitsArgs;
pub use logging::LoggingArgs;
pub use network::NetworkArgs;
pub use paths::PathArgs;
pub use tls_auth::TlsAuthArgs;

use clap::Parser;

#[derive(Parser, Debug)]
#[command(name = "sipi server", term_width = 0)]
pub struct ServerArgs {
    /// Path to the SIPI Lua config (installed by `sipi_init` before serving).
    /// Also accepts the `-c` short form.
    #[arg(long, short = 'c', env = "SIPI_CONFIGFILE", value_name = "FILE")]
    pub config: Option<String>,

    #[command(flatten)]
    pub network: NetworkArgs,
    #[command(flatten)]
    pub concurrency: ConcurrencyArgs,
    #[command(flatten)]
    pub limits: LimitsArgs,
    #[command(flatten)]
    pub paths: PathArgs,
    #[command(flatten)]
    pub cache: CacheArgs,
    #[command(flatten)]
    pub tls_auth: TlsAuthArgs,
    #[command(flatten)]
    pub knora: KnoraArgs,
    #[command(flatten)]
    pub logging: LoggingArgs,

    /// Graceful-drain deadline in seconds (default 30): on SIGTERM/Ctrl-C,
    /// in-flight requests get this long to finish before a forced shutdown.
    #[arg(long, env = "SIPI_DRAIN_TIMEOUT", value_name = "SECS")]
    pub drain_timeout: Option<u64>,
}

#[cfg(test)]
mod tests {
    use super::ServerArgs;
    use clap::Parser;

    /// Every override-bearing field must parse to `None` when neither CLI nor
    /// env provides it, so it falls through to the loaded Lua config: a
    /// `default_value` on an overridable field would silently clobber it. The
    /// argv starts at `"server"` because that is what `commands::server::run`
    /// hands `try_parse_from` after the verb dispatch.
    #[test]
    fn bare_server_invocation_sets_no_overrides() {
        let args = ServerArgs::try_parse_from(["server"]).unwrap();
        assert_eq!(args.config, None);
        assert_eq!(args.drain_timeout, None);
        assert_eq!(args.network.serverport, None);
        assert_eq!(args.network.sslport, None);
        assert_eq!(args.concurrency.nthreads, None);
        assert_eq!(args.limits.maxpost, None);
        assert_eq!(args.limits.admission_mode, None);
        assert_eq!(args.paths.imgroot, None);
        assert_eq!(args.paths.pathprefix, None);
        assert_eq!(args.cache.cache_dir, None);
        assert_eq!(args.tls_auth.jwtkey, None);
        assert_eq!(args.knora.knorapath, None);
        assert_eq!(args.logging.loglevel, None);
    }

    /// The flatten wiring and the long-flag names hold across all eight groups,
    /// including the `-c`/`-t` short forms on the two top-level fields.
    #[test]
    fn server_flags_parse_into_their_groups() {
        let args = ServerArgs::try_parse_from([
            "server",
            "-c",
            "/etc/sipi.lua",
            "--serverport",
            "1024",
            "-t",
            "8",
            "--maxpost",
            "300M",
            "--imgroot",
            "/img",
            "--cache-dir",
            "/c",
            "--jwtkey",
            "secret",
            "--knoraport",
            "3434",
            "--loglevel",
            "INFO",
            // An optional-value flag, placed last so no following token can be
            // read as its value: it must resolve to `Some(true)`.
            "--pathprefix",
        ])
        .unwrap();
        assert_eq!(args.config.as_deref(), Some("/etc/sipi.lua"));
        assert_eq!(args.network.serverport, Some(1024));
        assert_eq!(args.concurrency.nthreads, Some(8));
        assert_eq!(args.limits.maxpost.as_deref(), Some("300M"));
        assert_eq!(args.paths.imgroot.as_deref(), Some("/img"));
        assert_eq!(args.cache.cache_dir.as_deref(), Some("/c"));
        assert_eq!(args.tls_auth.jwtkey.as_deref(), Some("secret"));
        assert_eq!(args.knora.knoraport.as_deref(), Some("3434"));
        assert_eq!(args.logging.loglevel.as_deref(), Some("INFO"));
        assert_eq!(args.paths.pathprefix, Some(true));
    }

    /// `--decode-timeout-ms` lands in the limits group; the server checks the
    /// merged CLI/env/TOML value against the request timeout
    /// (`sipi::config::resolve_decode_timeout`).
    #[test]
    fn decode_timeout_ms_parses_into_limits() {
        let args = ServerArgs::try_parse_from(["server", "--decode-timeout-ms", "2000"]).unwrap();
        assert_eq!(args.limits.decode_timeout_ms, Some(2000));
    }
}
