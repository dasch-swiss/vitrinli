//! Native server config file — the Rust-side alternative to the Lua config
//! (`--config foo.toml`).
//!
//! **Experimental** (ADR-0017): this is one of the native alternatives SIPI is
//! introducing for everything Lua is used for. The schema may change without the
//! usual compatibility guarantees until it has been validated in production;
//! once stabilized, the experimental designation is dropped and Lua is
//! deprecated. The Lua config path stays fully supported in the meantime.
//!
//! [`Config`] is the format-agnostic representation (serde structs); TOML is just
//! the wire format the loader reads today (`Config::load` → `toml::from_str`) — a
//! future format is a new loader, not a new schema. The parsed config feeds the
//! override channel ([`ServerOverrides`]) with an empty Lua path, so the
//! engine default-constructs its config and these values layer on top. This lets
//! SIPI run without a Lua VM in the config path (the library-consumer story);
//! route *scripts* stay `.lua`, executed by the engine as usual.
//!
//! The schema mirrors the `server` clap argument groups as sections (`[network]`,
//! `[paths]`, `[cache]`, …) with idiomatic snake_case keys; the key→Lua-key
//! mapping is documented in the CLI docs. It carries only the keys the Rust shell
//! acts on: transport knobs the shell does not own (TLS, `hostname`,
//! `keep_alive`, thread count, `logfile`) have no key, so `deny_unknown_fields`
//! reports them as unsupported rather than silently ignoring them — and the same
//! guard turns a typo'd key into a startup error.
//!
//! Precedence is `config < env < CLI`: the parsed file is the base, and the
//! CLI/env overrides (already `CLI > env` from clap) layer over it via
//! [`ServerOverrides::layered_over`].

use crate::config::{ScalingQuality, ServerOverrides};
use crate::ffi::RouteEntry;
use serde::Deserialize;
use std::path::Path;

/// A parsed server config file (format-agnostic; the loader reads TOML today).
/// Sections mirror the `server` clap argument groups; every field is optional (an
/// omitted key falls through to the engine default).
#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    #[serde(default)]
    network: NetworkSection,
    #[serde(default)]
    paths: PathsSection,
    #[serde(default)]
    cache: CacheSection,
    #[serde(default)]
    limits: LimitsSection,
    #[serde(default)]
    image: ImageSection,
    #[serde(default)]
    tls_auth: TlsAuthSection,
    #[serde(default)]
    knora: KnoraSection,
    #[serde(default)]
    logging: LoggingSection,
    #[serde(default)]
    routes: Vec<RouteSpec>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct NetworkSection {
    port: Option<u16>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct PathsSection {
    img_root: Option<String>,
    script_dir: Option<String>,
    init_script: Option<String>,
    tmp_dir: Option<String>,
    max_temp_file_age: Option<i32>,
    doc_root: Option<String>,
    www_route: Option<String>,
    prefix_as_path: Option<bool>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct CacheSection {
    dir: Option<String>,
    /// Raw size string ("200M"); the engine parses the suffix.
    size: Option<String>,
    n_files: Option<u32>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct LimitsSection {
    /// Total RAM envelope as a raw size string ("8G"); the engine parses the
    /// suffix. "0"/absent = auto-detect available RAM.
    memory_limit: Option<String>,
    /// Admission mode: "basic" | "advanced".
    admission_mode: Option<String>,
    /// Fraction of the envelope reserved for tiles; the full lane gets
    /// envelope × (1 − ratio).
    tiles_memory_ratio: Option<f64>,
    /// Estimated peak-memory threshold (bytes): at/above it a decode is a
    /// full-lane decode charged against the budget; below it is a tile decode.
    large_decode_threshold_bytes: Option<u64>,
    /// Wall-clock decode deadline in ms; must be shorter than the request
    /// timeout. Unset: 80 % of the request timeout.
    decode_timeout_ms: Option<u64>,
    /// Raw size string ("300M"); the engine parses the suffix.
    max_post_size: Option<String>,
    thumb_size: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct ImageSection {
    jpeg_quality: Option<i32>,
    #[serde(default)]
    scaling_quality: ScalingQualitySection,
}

/// Per-codec scaling quality ("high"|"medium"|"low"). Maps to [`ScalingQuality`].
#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct ScalingQualitySection {
    jpeg: Option<String>,
    tiff: Option<String>,
    png: Option<String>,
    j2k: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct TlsAuthSection {
    jwt_secret: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct KnoraSection {
    path: Option<String>,
    port: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
#[serde(deny_unknown_fields)]
struct LoggingSection {
    level: Option<String>,
}

/// One `[[routes]]` entry — the same shape as a Lua `routes` table row. `script`
/// is a filename resolved against `paths.script_dir` (or an absolute path).
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RouteSpec {
    method: String,
    route: String,
    script: String,
}

/// A failure loading or validating a `.toml` config.
#[derive(Debug)]
pub enum ConfigError {
    Io(std::io::Error),
    Parse(toml::de::Error),
    /// No `paths.img_root` (and no `--imgroot`): the engine cannot resolve a root.
    MissingImgRoot,
    /// `[[routes]]` with a relative `script` but no `paths.script_dir` to resolve
    /// it against.
    MissingScriptDir,
    /// `[image].jpeg_quality` outside the valid 1-100 range. Caught at startup
    /// rather than as a per-request 500 at the first JPEG encode (the C++ CLI
    /// path range-checks the same flag).
    JpegQualityRange(i32),
    /// A `[[routes]]` entry whose HTTP method the shell does not serve — caught
    /// at startup rather than silently dropping the route at registration.
    UnknownRouteMethod(String),
}

impl std::fmt::Display for ConfigError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ConfigError::Io(e) => write!(f, "reading config: {e}"),
            // `.message()`, not `{e}`: `toml::de::Error`'s Display echoes the
            // offending source line verbatim (e.g. `1 | jwt_secret = "..."`) —
            // this error reaches `tracing::error!` in lib.rs, which
            // `sentry_tracing::layer()` forwards to Sentry. `.message()` is the
            // description alone, with no source-text echo.
            ConfigError::Parse(e) => write!(f, "parsing TOML: {}", e.message()),
            ConfigError::MissingImgRoot => {
                write!(f, "no image root: set [paths].img_root (or pass --imgroot)")
            }
            ConfigError::MissingScriptDir => write!(
                f,
                "[[routes]] has a relative script but [paths].script_dir is unset"
            ),
            ConfigError::JpegQualityRange(q) => {
                write!(f, "[image].jpeg_quality must be 1-100, got {q}")
            }
            ConfigError::UnknownRouteMethod(m) => write!(
                f,
                "[[routes]] method '{m}' is not supported (use GET, HEAD, POST, PUT, DELETE, or OPTIONS)"
            ),
        }
    }
}

impl std::error::Error for ConfigError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            ConfigError::Io(e) => Some(e),
            ConfigError::Parse(e) => Some(e),
            _ => None,
        }
    }
}

impl Config {
    /// Read and parse a `.toml` config file.
    pub fn load(path: &str) -> Result<Self, ConfigError> {
        let text = std::fs::read_to_string(path).map_err(ConfigError::Io)?;
        toml::from_str(&text).map_err(ConfigError::Parse)
    }

    /// Layer the CLI/env `overrides` over this TOML base (`TOML < env < CLI`),
    /// validate, and compose the configured routes. Returns the effective
    /// overrides for `sipi_init` and the route list for the shell to register.
    pub fn resolve(
        self,
        cli: ServerOverrides,
    ) -> Result<(ServerOverrides, Vec<RouteEntry>), ConfigError> {
        let effective = cli.layered_over(self.base());

        if effective.imgroot.is_none() {
            return Err(ConfigError::MissingImgRoot);
        }

        if let Some(q) = effective.jpeg_quality {
            if !(1..=100).contains(&q) {
                return Err(ConfigError::JpegQualityRange(q));
            }
        }

        let script_dir = effective.scriptdir.clone().unwrap_or_default();
        let has_relative = self
            .routes
            .iter()
            .any(|r| !Path::new(&r.script).is_absolute());
        if has_relative && script_dir.is_empty() {
            return Err(ConfigError::MissingScriptDir);
        }

        // The shell registers routes by exact-uppercase method (routes.rs
        // method_filter: GET/HEAD/POST/PUT/DELETE/OPTIONS). Normalize + reject an
        // unknown verb here with a clear startup error, instead of letting the
        // route be silently dropped at registration.
        const SUPPORTED_METHODS: [&str; 6] = ["GET", "HEAD", "POST", "PUT", "DELETE", "OPTIONS"];
        let routes = self
            .routes
            .into_iter()
            .map(|r| {
                let method = r.method.to_ascii_uppercase();
                if !SUPPORTED_METHODS.contains(&method.as_str()) {
                    return Err(ConfigError::UnknownRouteMethod(r.method));
                }
                Ok(RouteEntry {
                    method,
                    route: r.route,
                    script: compose_script_path(&script_dir, &r.script),
                })
            })
            .collect::<Result<Vec<_>, _>>()?;

        Ok((effective, routes))
    }

    /// The TOML values as a [`ServerOverrides`] base (before the CLI/env layer).
    fn base(&self) -> ServerOverrides {
        ServerOverrides {
            serverport: self.network.port,
            imgroot: self.paths.img_root.clone(),
            scriptdir: self.paths.script_dir.clone(),
            initscript: self.paths.init_script.clone(),
            tmpdir: self.paths.tmp_dir.clone(),
            maxtmpage: self.paths.max_temp_file_age,
            docroot: self.paths.doc_root.clone(),
            wwwroute: self.paths.www_route.clone(),
            pathprefix: self.paths.prefix_as_path,
            jwtkey: self.tls_auth.jwt_secret.clone(),
            cache_dir: self.cache.dir.clone(),
            cache_size: self.cache.size.clone(),
            cache_nfiles: self.cache.n_files,
            memory_limit: self.limits.memory_limit.clone(),
            admission_mode: self.limits.admission_mode.clone(),
            tiles_memory_ratio: self.limits.tiles_memory_ratio,
            large_decode_threshold_bytes: self.limits.large_decode_threshold_bytes,
            decode_timeout_ms: self.limits.decode_timeout_ms,
            maxpost: self.limits.max_post_size.clone(),
            thumbsize: self.limits.thumb_size.clone(),
            knorapath: self.knora.path.clone(),
            knoraport: self.knora.port.clone(),
            loglevel: self.logging.level.clone(),
            jpeg_quality: self.image.jpeg_quality,
            scaling_quality: ScalingQuality {
                jpeg: self.image.scaling_quality.jpeg.clone(),
                tiff: self.image.scaling_quality.tiff.clone(),
                png: self.image.scaling_quality.png.clone(),
                j2k: self.image.scaling_quality.j2k.clone(),
            },
            // Lua-config-only fields (the TOML schema deliberately owns no
            // transport keys; these feed the Lua `config` table for scripts).
            hostname: None,
            sslport: None,
        }
    }
}

/// Resolve a route `script` against `script_dir`, matching the engine's
/// `script_dir + "/" + script` composition. An already-absolute script is used
/// verbatim.
pub(crate) fn compose_script_path(script_dir: &str, script: &str) -> String {
    if Path::new(script).is_absolute() {
        script.to_string()
    } else {
        format!("{}/{}", script_dir.trim_end_matches('/'), script)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = r#"
[network]
port = 1024

[paths]
img_root = "/imgroot"
script_dir = "/scripts"
doc_root = "/server"
www_route = "/server"

[cache]
dir = "/cache"
size = "200M"
n_files = 250

[limits]
max_post_size = "300M"
thumb_size = "!128,128"

[image]
jpeg_quality = 90
scaling_quality = { jpeg = "high", tiff = "medium", png = "low", j2k = "high" }

[tls_auth]
jwt_secret = "secret"

[knora]
path = "knora.example.org"
port = "443"

[logging]
level = "INFO"

[[routes]]
method = "POST"
route = "/api/upload"
script = "upload.lua"
"#;

    #[test]
    fn parses_and_maps_sections_to_overrides() {
        let cfg: Config = toml::from_str(SAMPLE).expect("valid TOML");
        let base = cfg.base();
        assert_eq!(base.serverport, Some(1024));
        assert_eq!(base.imgroot.as_deref(), Some("/imgroot"));
        assert_eq!(base.scriptdir.as_deref(), Some("/scripts"));
        assert_eq!(base.docroot.as_deref(), Some("/server"));
        assert_eq!(base.cache_dir.as_deref(), Some("/cache"));
        assert_eq!(base.cache_size.as_deref(), Some("200M"));
        assert_eq!(base.cache_nfiles, Some(250));
        assert_eq!(base.maxpost.as_deref(), Some("300M"));
        assert_eq!(base.thumbsize.as_deref(), Some("!128,128"));
        assert_eq!(base.jpeg_quality, Some(90));
        assert_eq!(base.scaling_quality.jpeg.as_deref(), Some("high"));
        assert_eq!(base.scaling_quality.png.as_deref(), Some("low"));
        // j2k maps through Rust-side; the engine currently ignores it (it reads
        // that slot under a legacy "jpk" key), but the value must still parse.
        assert_eq!(base.scaling_quality.j2k.as_deref(), Some("high"));
        assert_eq!(base.jwtkey.as_deref(), Some("secret"));
        assert_eq!(base.knorapath.as_deref(), Some("knora.example.org"));
        assert_eq!(base.loglevel.as_deref(), Some("INFO"));
    }

    #[test]
    fn resolve_composes_routes_against_scriptdir() {
        let cfg: Config = toml::from_str(SAMPLE).unwrap();
        let (eff, routes) = cfg.resolve(ServerOverrides::default()).unwrap();
        assert_eq!(eff.serverport, Some(1024));
        assert_eq!(routes.len(), 1);
        assert_eq!(routes[0].method, "POST");
        assert_eq!(routes[0].route, "/api/upload");
        assert_eq!(routes[0].script, "/scripts/upload.lua");
    }

    #[test]
    fn cli_overrides_win_over_toml() {
        let cfg: Config = toml::from_str(SAMPLE).unwrap();
        let cli = ServerOverrides {
            serverport: Some(9999),
            imgroot: Some("/cli-root".into()),
            ..Default::default()
        };
        let (eff, _) = cfg.resolve(cli).unwrap();
        // CLI wins where set; TOML fills the rest.
        assert_eq!(eff.serverport, Some(9999));
        assert_eq!(eff.imgroot.as_deref(), Some("/cli-root"));
        assert_eq!(eff.cache_dir.as_deref(), Some("/cache"));
    }

    #[test]
    fn absolute_route_script_kept_verbatim() {
        let toml = r#"
[paths]
img_root = "/imgroot"
[[routes]]
method = "GET"
route = "/x"
script = "/abs/x.lua"
"#;
        let cfg: Config = toml::from_str(toml).unwrap();
        let (_, routes) = cfg.resolve(ServerOverrides::default()).unwrap();
        assert_eq!(routes[0].script, "/abs/x.lua");
    }

    #[test]
    fn out_of_range_jpeg_quality_is_rejected() {
        let toml = "[paths]\nimg_root = \"/imgroot\"\n[image]\njpeg_quality = 200\n";
        let cfg: Config = toml::from_str(toml).unwrap();
        assert!(matches!(
            cfg.resolve(ServerOverrides::default()),
            Err(ConfigError::JpegQualityRange(200))
        ));
    }

    #[test]
    fn missing_img_root_is_an_error() {
        let cfg: Config = toml::from_str("[network]\nport = 1024\n").unwrap();
        assert!(matches!(
            cfg.resolve(ServerOverrides::default()),
            Err(ConfigError::MissingImgRoot)
        ));
    }

    #[test]
    fn img_root_from_cli_satisfies_the_requirement() {
        let cfg: Config = toml::from_str("[network]\nport = 1024\n").unwrap();
        let cli = ServerOverrides {
            imgroot: Some("/cli-root".into()),
            ..Default::default()
        };
        assert!(cfg.resolve(cli).is_ok());
    }

    #[test]
    fn relative_route_without_scriptdir_is_an_error() {
        let toml = r#"
[paths]
img_root = "/imgroot"
[[routes]]
method = "GET"
route = "/x"
script = "x.lua"
"#;
        let cfg: Config = toml::from_str(toml).unwrap();
        assert!(matches!(
            cfg.resolve(ServerOverrides::default()),
            Err(ConfigError::MissingScriptDir)
        ));
    }

    #[test]
    fn unknown_route_method_is_rejected() {
        let toml = r#"
[paths]
img_root = "/imgroot"
script_dir = "/scripts"
[[routes]]
method = "PATCH"
route = "/x"
script = "x.lua"
"#;
        let cfg: Config = toml::from_str(toml).unwrap();
        assert!(matches!(
            cfg.resolve(ServerOverrides::default()),
            Err(ConfigError::UnknownRouteMethod(_))
        ));
    }

    #[test]
    fn route_method_is_normalized_to_uppercase() {
        let toml = r#"
[paths]
img_root = "/imgroot"
script_dir = "/scripts"
[[routes]]
method = "post"
route = "/api/upload"
script = "upload.lua"
"#;
        let cfg: Config = toml::from_str(toml).unwrap();
        let (_, routes) = cfg.resolve(ServerOverrides::default()).unwrap();
        assert_eq!(routes[0].method, "POST");
    }

    #[test]
    fn unknown_key_is_rejected() {
        let err = toml::from_str::<Config>("[network]\nprot = 1024\n");
        assert!(
            err.is_err(),
            "deny_unknown_fields should reject a typo'd key"
        );
    }

    #[test]
    fn unsupported_transport_key_is_rejected() {
        // Transport knobs the Rust shell does not own (TLS / hostname / thread
        // count) are not in the schema; deny_unknown_fields reports them rather
        // than silently ignoring them.
        assert!(toml::from_str::<Config>("[network]\nssl_port = 443\n").is_err());
        assert!(toml::from_str::<Config>("[concurrency]\nn_threads = 8\n").is_err());
    }
}
