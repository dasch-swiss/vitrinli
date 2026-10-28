# Running Vitrinli

Vitrinli can be run either as a command-line image converter or as an IIIF media server.

## Quick Start with Docker

```bash
docker run -p 1024:1024 daschswiss/sipi
```

## Running Vitrinli as a Command-line Image Converter

Vitrinli now uses verb-noun subcommands (`convert`, `query`, `compare`,
`verify`, `server`, `health`) instead of the legacy `--convert` / `--query` /
`--compare` / `--file` / `--outf` flags. A bare `sipi` invocation
errors with a usage message — every operation needs an explicit
subcommand.

Convert an image file to another format:

```bash
sipi convert input.tif output.jpg --format jpg
```

Query image file information:

```bash
sipi query input.tif
```

Compare two image files pixel-wise:

```bash
sipi compare file1.tif file2.jpg
```

## Running Vitrinli as a Server

```bash
sipi server --config config/sipi.config.lua
```

### JWT secret (`SIPI_JWTKEY`)

Any Lua-configured deployment (a `--config`/`SIPI_CONFIGFILE` pointing at a
`.lua` file) must supply a jwt_secret of at least 32 bytes via the
`SIPI_JWTKEY` environment variable (or the equivalent `--jwtkey` flag /
`jwt_secret` config key). There is no shipped default — an empty, too-short,
or shipped-literal secret refuses startup (fail closed), since it would let a
deployment that inherited a known secret forge JWTs.

Example (documentation only — generate your own secret, never reuse this
value):

```bash
export SIPI_JWTKEY='a randomly generated secret, at least 32 bytes long'
```

The old Lua `admin = { user = '…', password = '…' }` block that used to ship
alongside `jwt_secret` in `config/sipi.config.lua` is documented for
reference only:

```lua
admin = {
    user = 'admin',
    password = 'Sipi-Admin'
}
```

### Configuration file format (`.lua` or `.toml`)

`--config` accepts either a Lua file (`.lua`) or a TOML file (`.toml`),
dispatched on the extension. The Lua file is parsed by the engine's Lua VM
(Vitrinli's historical format); a TOML file is parsed directly by the server and
needs no Lua VM in the config path. Route *scripts* are always Lua — TOML
replaces the config *file*, not the scripting.

> **TOML config is experimental.** It is the first of the native (Rust/TOML)
> alternatives Vitrinli is introducing for everything Lua is used for today (see
> [ADR-0017](https://github.com/dasch-swiss/vitrinli/blob/main/docs/adr/0017-extensibility-lua-and-rust.md)).
> The schema may change without the usual compatibility guarantees until it is
> validated in production; the server logs a warning on startup when a `.toml`
> config is loaded. The Lua config path stays fully supported. Once the native
> alternatives stabilize, Lua will be marked deprecated.

A TOML config groups keys into sections that mirror the `server` flag groups,
with idiomatic snake_case names:

```toml
[network]
port = 1024

[paths]
img_root = "/sipi/images"   # required
script_dir = "/sipi/scripts"
init_script = "/sipi/config/sipi.init.lua"

[cache]
dir = "/sipi/cache"
size = "200M"               # "0" disables the cache

[image]
jpeg_quality = 60
scaling_quality = { jpeg = "high", tiff = "high", png = "high", j2k = "high" }

[tls_auth]
jwt_secret = "…"

[[routes]]
method = "POST"
route = "/api/upload"
script = "upload.lua"       # resolved against [paths].script_dir
```

Precedence is `config file < environment variable < command-line flag`, the same
as for a Lua config: a CLI flag (or its `SIPI_*` env var) overrides the matching
TOML key. `img_root` is required (an empty image root has nothing to resolve);
routes with a relative `script` require `[paths].script_dir`. Unknown keys are
rejected (a typo is a startup error, not a silently-ignored value), so knobs
the Rust server does not own as config — TLS (`ssl_*`), `hostname`,
`keep_alive`, thread count, and `logfile` — have no TOML key and are reported
as unsupported; these are now CLI-flag/env-var-only knobs, accepted for
parse-only compatibility (see [Server Options](#server-options) below), not
Lua config or TOML keys.

The TOML key for each Lua config key:

| TOML key | Lua config key |
|----------|----------------|
| `[network] port` | `port` |
| `[paths] img_root` | `imgroot` |
| `[paths] script_dir` | `scriptdir` |
| `[paths] init_script` | `initscript` |
| `[paths] tmp_dir` | `tmpdir` |
| `[paths] max_temp_file_age` | `max_temp_file_age` |
| `[paths] doc_root` | `fileserver.docroot` |
| `[paths] www_route` | `fileserver.wwwroute` |
| `[paths] prefix_as_path` | `prefix_as_path` |
| `[cache] dir` | `cache_dir` |
| `[cache] size` | `cache_size` |
| `[cache] n_files` | `cache_nfiles` |
| `[limits] memory_limit` | `memory_limit` |
| `[limits] admission_mode` | `admission_mode` |
| `[limits] tiles_memory_ratio` | `tiles_memory_ratio` |
| `[limits] max_post_size` | `max_post_size` |
| `[limits] thumb_size` | `thumb_size` |
| `[image] jpeg_quality` | `jpeg_quality` |
| `[image] scaling_quality.{jpeg,tiff,png,j2k}` | `scaling_quality.{…}` (the `j2k` entry is accepted but currently has no effect — the engine reads that slot under a legacy key) |
| `[tls_auth] jwt_secret` | `jwt_secret` |
| `[knora] path` | `knora_path` |
| `[knora] port` | `knora_port` |
| `[logging] level` | `loglevel` |
| `[[routes]]` (`method`/`route`/`script`) | `routes` |

## Health Check

`sipi health` is a self-contained liveness probe. It performs an HTTP `GET` on
the local [`/health`](../operation/health-endpoint.md) endpoint and maps the
result to a process exit code, following the Docker/Swarm healthcheck
convention:

- **0** — the endpoint returned HTTP 200 (healthy).
- **1** — connection refused, timeout, or any non-200 response (unhealthy).

```bash
sipi health            # probes http://127.0.0.1:1024/health
sipi health --port 80  # probe a server running on a different port
```

The host is always `127.0.0.1` and the path is always `/health`; only the port
is configurable. A separate `sipi health` process cannot know whether the
running server took its port from a config file, an env var, or `--serverport`,
so the caller passes the port it configured (`--port`, default `1024`). The
probe uses a short (~2 s) timeout so a wedged server reports unhealthy rather
than hanging. Because it needs no `curl` in the image, it is the intended
container/orchestrator liveness command (see
[Health Endpoint](../operation/health-endpoint.md)).

## Logging

Vitrinli uses two logging modes depending on how it is running:

- **CLI mode** (`convert`, `compare`, `query`): Plain text output.
  Errors go to **stderr**, informational messages go to **stdout**.
  This is the standard Unix convention for command-line tools. When
  `--json` is also set (see [Structured JSON output](#structured-json-output-cli)),
  every level is routed to **stderr** so `stdout` stays reserved for the
  single JSON document.

- **Server mode** (`sipi server --config`): JSON-formatted log lines go to **stdout**.
  This follows container best practices — Docker, Kubernetes, and log
  collectors (Grafana Loki, Fluentd) expect structured logs on stdout.
  Each line is a JSON object: `{"level": "INFO", "message": "..."}`.

### Log Levels

Vitrinli supports the following log levels (in order of increasing severity):

| Level | Description |
| ----- | ----------- |
| `DEBUG` | Detailed diagnostic information. |
| `INFO` | Normal operational messages (routes added, server started, migrations). |
| `NOTICE` | Significant but normal events. |
| `WARNING` | Something unexpected but recoverable (e.g., failed XMP parse, incomplete metadata write). |
| `ERR` | Errors that affect a specific operation (e.g., image processing failure, ICC error). |
| `CRIT` | Critical errors. |
| `ALERT` | Conditions requiring immediate attention. |
| `EMERG` | System-wide emergencies. |

The log level controls which messages are emitted. Setting a level suppresses
all messages below it. For example, `WARNING` shows only WARNING, ERR, CRIT,
ALERT, and EMERG — suppressing DEBUG, INFO, and NOTICE.

The log level can be configured in three ways (in order of precedence):

1. **CLI option**: `--loglevel WARNING`
2. **Environment variable**: `SIPI_LOGLEVEL=WARNING`
3. **Lua config**: `loglevel = "WARNING"` (in the `sipi` block)

If none is specified, the default level is `INFO`.

## Command-line Options

### Image Conversion Options

These options are accepted by the `convert` subcommand (and where
documented in the per-subcommand matrix, by `convert access-file`
and `convert service-file`). Usage: `sipi convert <input> <output>
[options]`.

| Flag | Short | Description |
|------|-------|-------------|
| `--format <fmt>` | `-F` | Output format: `jpx`, `jp2`, `jpg`, `tif`, `png`, `webp`, `gif` |
| `--icc <profile>` | `-I` | Convert to ICC profile: `none`, `sRGB`, `AdobeRGB`, `GRAY` |
| `--quality <1-100>` | `-q` | JPEG compression quality (1 = highest compression, 100 = best quality) |
| `--pagenum <n>` | `-n` | Page number for multi-page PDF/TIFF input files |
| `--region <x,y,w,h>` | `-r` | Select a region of interest (4 integer values) |
| `--reduce <factor>` | `-R` | Reduce image size by factor (faster than `--scale`) |
| `--size <w,h>` | `-s` | Resize image to given dimensions |
| `--scale <percent>` | `-S` | Resize image by percentage |
| `--mirror <dir>` | `-m` | Mirror image: `none`, `horizontal`, `vertical` |
| `--rotate <angle>` | `-o` | Rotate image by degrees (0.0 - 360.0) |
| `--skipmeta` | `-k` | Strip all metadata from the output file |
| `--topleft` | | Enforce TOPLEFT orientation |
| `--watermark <file>` | `-w` | Overlay a watermark (single-channel grayscale TIFF) |
| `--Ctiff_pyramid` | | Store output in pyramidal TIFF format |

### Query and Compare

Use the `query` and `compare` subcommands. `--query` / `--compare`
flag forms are gone.

| Subcommand | Description |
|------------|-------------|
| `sipi query <file>` | Dump all information about the given file |
| `sipi compare <f1> <f2>` | Compare two files pixel-wise |

### Decode deadline

Every offline verb (`convert`, `convert access-file`, `convert service-file`,
`query`, `compare`) decodes its input under a 120 s wall-clock deadline, the
same default the server applies per request. A malformed JPEG 2000 file can
wedge the Kakadu decoder indefinitely; when the deadline passes the verb
reports `decode exceeded the 120000 ms deadline` and exits non-zero instead
of hanging the shell (or the ingest worker driving it). The wedged decode
thread is abandoned and ends with the process.

### Structured JSON output (CLI)

| Flag | Description |
|------|-------------|
| `--json` | Emit a single JSON document to `stdout` instead of human-readable output. Accepted by `convert`, `convert access-file`, `query`, and `compare`. Useful for programmatic consumers and for full diagnostics on failures, which the CLI never reports to Sentry (see [Structured Diagnostics (CLI Mode)](#structured-diagnostics-cli-mode)). |

Use cases:

- **Local debugging** — the primary RDU case. Every `ImageContext` field
  appears in the JSON document; CLI failures are never sent to Sentry.
- **CI pipelines** that need to assert on image properties
  (`jq '.image.bps'`) or on a specific failure mode (`jq '.phase'`).
- **Scripts** that consume sipi output pipeline-style.

See [`json-output.md`](json-output.md) for the full schema, worked
examples, and the `stdout` / `stderr` contract.

### JPEG2000 Options

| Flag | Description |
|------|-------------|
| `--Sprofile <val>` | J2K profile: `PROFILE0`, `PROFILE1`, `PROFILE2`, `PART2`, `CINEMA2K`, `CINEMA4K`, `BROADCAST`, `CINEMA2S`, `CINEMA4S`, `CINEMASS`, `IMF` |
| `--rates <string>` | Bit-rate(s) for quality layers (`-1` for lossless final layer) |
| `--Clayers <n>` | Number of quality layers (default: 8) |
| `--Clevels <n>` | Number of wavelet decomposition levels (default: 8) |
| `--Corder <val>` | Progression order: `LRCP`, `RLCP`, `RPCL`, `PCRL`, `CPRL` (default: `RPCL`) |
| `--Stiles <string>` | Tile dimensions `"{tx,ty}"` (default: `"{256,256}"`) |
| `--Cprecincts <string>` | Precinct dimensions `"{px,py}"` (default: `"{256,256}"`) |
| `--Cblk <string>` | Code-block dimensions `"{dx,dy}"` (default: `"{64,64}"`) |
| `--Cuse_sop <val>` | Include SOP markers (default: yes) |

### Server Options

These options are accepted by the `server` subcommand. Usage:
`sipi server [options]`.

| Flag | Short | Env Var | Default | Description |
|------|-------|---------|---------|-------------|
| `--config <file>` | `-c` | `SIPI_CONFIGFILE` | | Server config file — Lua (`.lua`) or TOML (`.toml`); see [Configuration file format](#configuration-file-format-lua-or-toml) |
| `--serverport <n>` | | `SIPI_SERVERPORT` | `80` | HTTP port |
| `--sslport <n>` | | `SIPI_SSLPORT` | `443` | HTTPS port |
| `--hostname <name>` | | `SIPI_HOSTNAME` | `localhost` | Public DNS hostname. Parse-only: accepted for compatibility, not read by the server |
| `--keepalive <sec>` | | `SIPI_KEEPALIVE` | `5` | HTTP keep-alive timeout in seconds. Parse-only: accepted for compatibility, unread |
| `--nthreads <n>` | `-t` | `SIPI_NTHREADS` | `0` (auto) | Worker threads (`0` = auto: available parallelism, container-aware, fallback 4) |
| `--tiles-thread-ratio <r>` | | `SIPI_TILES_THREAD_RATIO` | `0.5` | Fraction of workers guaranteed to tiles (0..1); the full partition is hard-capped at the rest |
| `--max-waiting <n>` | | `SIPI_MAX_WAITING` | `2×nthreads` | Max requests that may wait for a worker before HTTP 503 (`0` = no queue, shed immediately; `Retry-After: 1`) |
| `--queue-timeout <sec>` | | `SIPI_QUEUE_TIMEOUT` | `5` | Max seconds a request waits in queue before 503 |
| `--memory-limit <size>` | | `SIPI_MEMORY_LIMIT` | `0` (auto) | Total decode-memory envelope (`0` = auto-detect available RAM; `8G`, `500M`); the full partition's budget is envelope × (1 − tiles-memory-ratio) |
| `--admission-mode <mode>` | | `SIPI_ADMISSION_MODE` | `advanced` | `basic` (enforce the basic tier only; advanced tier observe-only) or `advanced` (also enforce the memory budget + 503/413) |
| `--tiles-memory-ratio <r>` | | `SIPI_TILES_MEMORY_RATIO` | `0.25` | Fraction of the envelope reserved for tiles + non-decode floor (0..1) |
| `--large-decode-threshold-bytes <n>` | | `SIPI_LARGE_DECODE_THRESHOLD_BYTES` | `33554432` (32 MiB) | Estimated peak-memory at/above which a decode is charged to the full partition; below it bypasses as a tile |
| `--preflight-cache-ttl <sec>` | | `SIPI_PREFLIGHT_CACHE_TTL` | `0` (disabled) | Seconds a `pre_flight` access decision is cached per `(prefix, identifier, credential)`. Opt-in; enable (`>0`) only if the hook decides purely on prefix/identifier/Cookie/Authorization (see the Preflight access-cache section in the Lua scripting guide) |
| `--preflight-cache-slots <n>` | | `SIPI_PREFLIGHT_CACHE_SLOTS` | `4096` | Slot count for the preflight access-cache |
| `--maxpost <size>` | | `SIPI_MAXPOSTSIZE` | `300M` | Maximum POST upload size |
| `--imgroot <path>` | | `SIPI_IMGROOT` | `./images` | Image repository root directory |
| `--docroot <path>` | | `SIPI_DOCROOT` | `./server` | Web server document root |
| `--wwwroute <path>` | | `SIPI_WWWROUTE` | `/server` | URL route for web server |
| `--scriptdir <path>` | | `SIPI_SCRIPTDIR` | `./scripts` | Directory for Lua route scripts |
| `--tmpdir <path>` | | `SIPI_TMPDIR` | `./tmp` | Temporary files directory |
| `--maxtmpage <sec>` | | `SIPI_MAXTMPAGE` | `86400` | Max age of temp files in seconds |
| `--initscript <path>` | | `SIPI_INITSCRIPT` | `./config/sipi.init.lua` | Path to Lua init script |
| `--cachedir <path>` | | `SIPI_CACHEDIR` | `./cache` | Cache directory |
| `--cachesize <size>` | | `SIPI_CACHESIZE` | `200M` | Maximum cache size (`-1`=unlimited, `0`=disabled) |
| `--cachenfiles <n>` | | `SIPI_CACHENFILES` | `200` | Maximum number of cached files (`0`=no limit) |
| `--thumbsize <size>` | | `SIPI_THUMBSIZE` | `!128,128` | Default thumbnail size (IIIF syntax) |
| `--sslcert <path>` | | `SIPI_SSLCERTIFICATE` | `./certificate/certificate.pem` | SSL certificate path. Parse-only: accepted for compatibility, unread |
| `--sslkey <path>` | | `SIPI_SSLKEY` | `./certificate/key.pem` | SSL key file path. Parse-only: accepted for compatibility, unread |
| `--jwtkey <string>` | | `SIPI_JWTKEY` | | JWT shared secret, required for any Lua-configured deployment (at least 32 bytes; the shipped default is rejected) |
| `--loglevel <level>` | | `SIPI_LOGLEVEL` | `DEBUG` | Sets the engine log level (`DEBUG`/`INFO`/…, see Logging section); applied via `set_log_level` |

### Sentry Error Reporting

No CLI flags — these are environment-variable only (see [Environment
Variables](#environment-variables) below): `SIPI_SENTRY_DSN`,
`SIPI_SENTRY_RELEASE`, `SIPI_SENTRY_ENVIRONMENT`. Read by the Rust shell's
`main` (`cli/rust/src/main.rs`), which owns Sentry init for every verb.

### Deprecated Options

| Flag | Description |
|------|-------------|
| `--pathprefix[=BOOL]` | Treat the IIIF URL prefix as a subdirectory under imgroot, i.e. `<imgroot>/<prefix>/<id>` (default: true; bare flag means true) |

## Environment Variables

All server options can be configured via environment variables. Environment variables
override Lua configuration file values but are themselves overridden by command-line
flags.

| Variable | CLI Flag | Default | Description |
|----------|----------|---------|-------------|
| `SIPI_CONFIGFILE` | `--config` | | Configuration file path |
| `SIPI_RS_PORT` | *(none)* | | Highest-precedence HTTP listen-port override (env-only, no CLI flag). Overrides `--serverport`/`SIPI_SERVERPORT` and the config's `port`. Primarily for parallel dev/test shells; safe to leave unset in production |
| `SIPI_SERVERPORT` | `--serverport` | `80` | HTTP port |
| `SIPI_SSLPORT` | `--sslport` | `443` | HTTPS port |
| `SIPI_HOSTNAME` | `--hostname` | `localhost` | Public hostname. Parse-only: accepted for compatibility, not read by the server |
| `SIPI_KEEPALIVE` | `--keepalive` | `5` | Keep-alive timeout (seconds). Parse-only: accepted for compatibility, unread |
| `SIPI_NTHREADS` | `--nthreads` | `0` (auto) | Worker threads (`0` = auto: available parallelism, container-aware, fallback 4) |
| `SIPI_TILES_THREAD_RATIO` | `--tiles-thread-ratio` | `0.5` | Fraction of workers guaranteed to tiles (0..1); the full partition is hard-capped at the rest |
| `SIPI_MAX_WAITING` | `--max-waiting` | `2×nthreads` | Max requests that may wait for a worker before 503 (`0` = no queue, shed immediately) |
| `SIPI_QUEUE_TIMEOUT` | `--queue-timeout` | `5` | Max seconds in queue before 503 |
| `SIPI_PREFLIGHT_CACHE_TTL` | `--preflight-cache-ttl` | `0` (disabled) | Seconds a `pre_flight` decision is cached per `(prefix, identifier, credential)`. Opt-in; see the Preflight access-cache section in the Lua scripting guide before enabling |
| `SIPI_PREFLIGHT_CACHE_SLOTS` | `--preflight-cache-slots` | `4096` | Slot count for the preflight access-cache |
| `SIPI_MAXPOSTSIZE` | `--maxpost` | `300M` | Max POST size |
| `SIPI_IMGROOT` | `--imgroot` | `./images` | Image root directory |
| `SIPI_DOCROOT` | `--docroot` | `./server` | Document root |
| `SIPI_WWWROUTE` | `--wwwroute` | `/server` | Web server route |
| `SIPI_SCRIPTDIR` | `--scriptdir` | `./scripts` | Lua scripts directory |
| `SIPI_TMPDIR` | `--tmpdir` | `./tmp` | Temporary directory |
| `SIPI_MAXTMPAGE` | `--maxtmpage` | `86400` | Max temp file age |
| `SIPI_INITSCRIPT` | `--initscript` | `./config/sipi.init.lua` | Init script path |
| `SIPI_CACHEDIR` | `--cachedir` | `./cache` | Cache directory |
| `SIPI_CACHESIZE` | `--cachesize` | `200M` | Max cache size (`-1`=unlimited, `0`=disabled) |
| `SIPI_CACHENFILES` | `--cachenfiles` | `200` | Max cached files (`0`=no limit) |
| `SIPI_THUMBSIZE` | `--thumbsize` | `!128,128` | Thumbnail size |
| `SIPI_SSLCERTIFICATE` | `--sslcert` | `./certificate/certificate.pem` | SSL certificate. Parse-only: accepted for compatibility, unread |
| `SIPI_SSLKEY` | `--sslkey` | `./certificate/key.pem` | SSL key. Parse-only: accepted for compatibility, unread |
| `SIPI_JWTKEY` | `--jwtkey` | | JWT secret, required for any Lua-configured deployment (at least 32 bytes; the shipped default is rejected) |
| `SIPI_ALLOWED_ORIGINS` | *(none)* | | Comma-separated browser Origins allowed to make credentialed cross-origin (CORS) requests; empty/unset = reflect any Origin (default, backward-compatible); set = restrict to the listed origins. Applies to every CORS response, `info.json` included; an `info.json` or `knora.json` request without an Origin still gets `*` |
| `SIPI_PUBLIC_HOSTS` | *(none)* | | Comma-separated hosts allowed in the canonical `@id`, the 303 `Location`, the `Link` header, the cache key, and Lua `server.host`; empty/unset = trust `X-Forwarded-Host`/`Host` verbatim (default, backward-compatible); set = a header host on the list passes through, any other host is replaced by the first listed host |
| `SIPI_REQUEST_TIMEOUT` | *(none)* | `60` | Handler wall-clock timeout in seconds; a request whose handler exceeds it answers 408 (`0`/unset/unparseable falls back to the default) |
| `SIPI_BODY_READ_TIMEOUT` | *(none)* | `10` | Lua-route/docroot request-body-read timeout in seconds; a body that takes longer to read is cut off before admission (`0`/unset/unparseable falls back to the default) |
| `SIPI_JPEGQUALITY` | `--quality` | `60` | JPEG quality |
| `SIPI_LOGLEVEL` | `--loglevel` | `DEBUG` | Sets the engine log level; applied via `set_log_level` |
| `SIPI_SENTRY_DSN` | | | Sentry DSN (no CLI flag) |
| `SIPI_SENTRY_RELEASE` | | | Sentry release (no CLI flag) |
| `SIPI_SENTRY_ENVIRONMENT` | | | Sentry environment (no CLI flag) |
| `SIPI_MEMORY_LIMIT` | `--memory-limit` | `0` (auto) | Total RAM envelope (`0`=auto-detect available RAM, `8G`, `500M`); the full lane gets envelope × (1 − `tiles_memory_ratio`) |
| `SIPI_ADMISSION_MODE` | `--admission-mode` | `advanced` | Admission mode: `basic` (enforce basic tier only), `advanced` (also enforce the advanced tier) |
| `SIPI_TILES_MEMORY_RATIO` | `--tiles-memory-ratio` | `0.25` | Fraction of the envelope reserved for tiles + non-decode floor (range 0..1) |
| `SIPI_LARGE_DECODE_THRESHOLD_BYTES` | `--large-decode-threshold-bytes` | `33554432` (32 MiB) | Estimated peak-memory at/above which a decode is charged to the full lane; below it bypasses as a tile |

**Configuration precedence** (highest to lowest):

1. Command-line flags
2. Environment variables
3. Lua configuration file

## Exit Codes and Error Handling

### Exit Codes

When running Vitrinli as a command-line image converter, the process exit code indicates
whether the conversion succeeded:

- **0** — Success. The output file was written correctly.
- **1** (`EXIT_FAILURE`) — Image processing error. The image could not be read,
  converted, or written.

**Important for calling services:** Always check the exit code. A non-zero exit code
means the output file was not produced (or is incomplete).

The `sipi health` subcommand follows the same convention: **0** = healthy
(`/health` returned 200), **1** = unhealthy (connection refused, timeout, or
non-200).

### Error Output

On failure, Vitrinli prints a short error message to **stderr** indicating the failure
phase and the specific error. The format is:

```
Error <phase> image: <details>
```

Where `<phase>` is one of `reading`, `converting`, or `writing`. Example:

```
Error reading image: Unsupported JPEG colorspace JCS_UNKNOWN (file=input.jpg, dimensions=2048x1536, components=4)
```

### Structured Diagnostics (CLI Mode)

CLI conversion failures are not sent to Sentry — only a crash (a panic in the
Rust shell) is. A handled read/convert/write failure logs to stderr and,
with `--json`, emits the full structured document described in
[Structured JSON output (CLI)](#structured-json-output-cli) above: the same
image-context fields (`input_file`, `output_file`, `width`, `height`,
`channels`, `bps`, `colorspace`, `icc_profile_type`, `orientation`,
`file_size_bytes`) plus the failing `phase` (`read`, `convert`, or `write`)
and the error message. This is the diagnostic surface for CLI failures —
`--json` gets you full context regardless of whether `SIPI_SENTRY_DSN` is set.

### Common Failure Causes

| Error | Meaning |
| ----- | ------- |
| Unsupported colorspace (JCS_UNKNOWN) | The JPEG uses a colorspace Vitrinli cannot convert. Re-encode the source image in sRGB. |
| Unsupported bits/sample | Only 8 and 16 bits/sample are supported. Images with other bit depths must be converted first. |
| Channel/colorspace mismatch | The number of channels does not match the declared colorspace (e.g., 4 channels but RGB). The file metadata may be corrupt. |
| ICC profile incompatible | The ICC profile does not match the channel count (e.g., CMYK profile on a 3-channel image). |
| Corrupt or truncated file | The input file is incomplete or damaged. |
| Unsupported TIFF tiling | The TIFF tile configuration is inconsistent or uses unsupported bit depths. |
| EXIF rational with zero denominator | A TIFF-format conversion embeds an EXIF rational tag (e.g. `ExposureTime`, `FNumber`) whose value is `n/0` with `n != 0`. The source metadata is corrupt; a `0/0` ("no value") rational is skipped rather than failing the write. |

### Integration Notes for Calling Services

If you call Vitrinli CLI from another service (e.g., a Java service):

1. **Check the exit code.** Non-zero means failure — do not assume the output file
   exists or is valid.
2. **Parse stderr** (optional). The first line of stderr contains a human-readable
   error message with the failure phase and details.
3. **Pass `--json`** to get full diagnostics without reproducing the failure —
   the document's `phase`, `colorspace`, `bps`, and `output_format` fields
   identify the failure pattern (see [Structured Diagnostics (CLI
   Mode)](#structured-diagnostics-cli-mode) above).

## Configuration Files

Vitrinli's configuration file is written in [Lua](https://www.lua.org/). You
can make your own configuration file by adapting
`config/sipi.config.lua`.

- Check that the port number is correct and that your operating
  system's firewall does not block it.
- Set `imgroot` to the directory containing the files to be served.
- Create the directory `cache` in the top-level directory of the
  source tree.

For more information, see the comments in `config/sipi.config.lua` and the
[Reference](sipi.md) page for all configuration parameters.

### HTTPS Support

Vitrinli supports SSL/TLS encryption if the
[OpenSSL](https://www.openssl.org/) library is installed. You will need
to install a certificate; see `config/sipi.config.lua` for instructions.

### IIIF Prefixes

Vitrinli supports [IIIF Image API
URLs](https://iiif.io/api/image/3.0/#21-image-request-uri-syntax).

If the configuration property `prefix_as_path` is set to `true`, the
IIIF `prefix` portion of the URL is interpreted as a subdirectory of
`imgroot`, and Vitrinli looks for the requested image file in that
subdirectory. Otherwise, it looks for the file in `imgroot`.
