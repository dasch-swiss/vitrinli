use nix::sys::signal;
pub use nix::sys::signal::Signal;
use nix::unistd::Pid;
use std::io::{BufRead, BufReader};
use std::net::{TcpListener, TcpStream};
use std::os::unix::process::ExitStatusExt as _;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::{Arc, Mutex, Once, OnceLock};
use std::time::{Duration, Instant};

pub mod jwt;

/// Atomic port counter to avoid conflicts when tests run in parallel.
/// Start well above privileged ports (macOS restricts ports below 1024).
/// The base offset is derived from the PID so back-to-back test
/// binaries don't restart at the same port and collide on TIME_WAIT
/// state from each other's recently-closed sockets.
static NEXT_PORT: OnceLock<AtomicU16> = OnceLock::new();

fn next_port() -> &'static AtomicU16 {
    NEXT_PORT.get_or_init(|| {
        // 11024 + (PID mod 16k), keeping ports under 32k so the +2 SSL port never overflows u16.
        let offset = (std::process::id() % 16384) as u16;
        AtomicU16::new(11024 + offset)
    })
}

/// Allocate a pair of (http_port, ssl_port) for a test server instance.
///
/// A pair with a port already bound, by a stale server from an earlier run
/// or by anything else, is skipped and never freed. The harness signals only
/// processes it spawned: finding a PID by port also finds unrelated
/// processes, such as a remote-execution server whose client connection has
/// that port at its far end.
pub fn allocate_ports() -> (u16, u16) {
    loop {
        let http = next_port().fetch_add(2, Ordering::SeqCst);
        assert!(http < 65534, "no free port pair below 65534");
        let ssl = http + 1;
        if port_is_free(http) && port_is_free(ssl) {
            return (http, ssl);
        }
    }
}

/// Probes the address the server binds (`0.0.0.0`); the probe listener is
/// dropped at once, before the server starts.
fn port_is_free(port: u16) -> bool {
    TcpListener::bind(("0.0.0.0", port)).is_ok()
}

/// A spawned server's `Child` plus everything a shutdown check needs: the
/// captured output buffers (for a failure message) and the drain threads'
/// EOF signals (so the final lines -- e.g. a LeakSanitizer report printed
/// just before exit -- are captured before the buffers are read). Owned by
/// [`LIVE`] from [`spawn`] until [`stop_entry`] consumes it.
struct RegistryEntry {
    child: Child,
    stdout_buf: Arc<Mutex<String>>,
    stderr_buf: Arc<Mutex<String>>,
    stdout_done: std::sync::mpsc::Receiver<()>,
    stderr_done: std::sync::mpsc::Receiver<()>,
}

/// Every spawned server not yet stopped, keyed by nothing but linear scan
/// (test binaries run at most a handful of servers concurrently). A
/// `SipiServer` held in a test's `static OnceLock` is never dropped, so its
/// entry would otherwise never be stopped or checked for a sanitizer
/// finding; [`atexit_stop_remaining_servers`] sweeps whatever is still here
/// when the test binary exits.
static LIVE: Mutex<Vec<RegistryEntry>> = Mutex::new(Vec::new());

static ATEXIT_ONCE: Once = Once::new();

/// Register [`atexit_stop_remaining_servers`] with `libc::atexit` exactly
/// once per process.
fn ensure_atexit_registered() {
    ATEXIT_ONCE.call_once(|| unsafe {
        libc::atexit(atexit_stop_remaining_servers);
    });
}

/// Stop one registry entry: SIGTERM, wait up to [`GRACEFUL_STOP_TIMEOUT`],
/// then judge the exit. A server that had to be SIGKILLed after the
/// deadline is not judged (escalation means we gave up waiting, not that
/// the server misbehaved). A clean exit (status 0) or death by our own
/// SIGTERM passes. Anything else -- a non-zero exit, or death by any other
/// signal (e.g. SIGSEGV/SIGABRT/SIGBUS from a crash, or the shell aborting
/// on a sanitizer finding) -- is a failure, and the returned message
/// carries the captured stdout+stderr so the report is visible in the
/// failure.
fn stop_entry(mut entry: RegistryEntry) -> Result<(), String> {
    signal::kill(
        Pid::from_raw(i32::try_from(entry.child.id()).expect("PID overflows i32")),
        Signal::SIGTERM,
    )
    .ok();

    let deadline = Instant::now() + GRACEFUL_STOP_TIMEOUT;
    let status = loop {
        match entry.child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) if Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(100));
            }
            _ => break None,
        }
    };

    let Some(status) = status else {
        let pid = entry.child.id();
        entry.child.kill().ok();
        entry.child.wait().ok();
        eprintln!(
            "[test-harness] sipi (pid {pid}) did not exit within {:?} of SIGTERM; SIGKILLed",
            GRACEFUL_STOP_TIMEOUT
        );
        return Ok(());
    };

    // Give the drain threads a chance to reach EOF so a report printed just
    // before exit (e.g. LeakSanitizer's) lands in the buffer before it's
    // read below.
    let _ = entry.stdout_done.recv_timeout(Duration::from_secs(2));
    let _ = entry.stderr_done.recv_timeout(Duration::from_secs(2));

    if status.success() || status.signal() == Some(libc::SIGTERM) {
        Ok(())
    } else {
        Err(format!(
            "sipi exited with {} after our SIGTERM\nstdout:\n{}\nstderr:\n{}",
            status,
            dump(&entry.stdout_buf),
            dump(&entry.stderr_buf)
        ))
    }
}

/// Runs at process exit (via `libc::atexit`) to stop and judge every sipi
/// server still in [`LIVE`] -- the servers held in a test's `static
/// OnceLock`, whose `Drop` never runs. Must never panic: this runs outside
/// Rust's unwind machinery. Exits the process non-zero if any remaining
/// server had a failed exit (non-zero, or death by a signal other than our
/// SIGTERM), so the sanitizer finding or crash fails the test binary (and
/// thus the Bazel test) instead of going unnoticed.
extern "C" fn atexit_stop_remaining_servers() {
    let entries = {
        let mut reg = LIVE.lock().unwrap_or_else(|e| e.into_inner());
        std::mem::take(&mut *reg)
    };
    let mut any_failed = false;
    for entry in entries {
        if let Err(msg) = stop_entry(entry) {
            eprintln!("[test-harness] {msg}");
            any_failed = true;
        }
    }
    if any_failed {
        // SAFETY: called from `atexit`; skipping any later-registered
        // atexit handlers is fine since this is the only one this crate
        // registers.
        unsafe { libc::_exit(1) };
    }
}

/// How long [`stop_entry`] waits for a graceful exit after SIGTERM before
/// escalating to SIGKILL. Covers the `--drain-timeout 2` shutdown plus the
/// up-to-2s wait for the drain threads to reach EOF, with margin under
/// ASan's runtime overhead.
const GRACEFUL_STOP_TIMEOUT: Duration = Duration::from_secs(15);

/// Manages a sipi server process for testing. Registered in [`LIVE`] from
/// [`spawn`] until stopped (by `Drop` or, for a server held in a `static
/// OnceLock`, by [`atexit_stop_remaining_servers`]).
pub struct SipiServer {
    pid: u32,
    pub http_port: u16,
    pub base_url: String,
    stdout_buf: Arc<Mutex<String>>,
    stderr_buf: Arc<Mutex<String>>,
    stopped: bool,
}

impl SipiServer {
    /// Start the Rust shell (the subject under test) with the given config
    /// file and extra CLI arguments.
    ///
    /// `config` — path to the config file, relative to `working_dir`.
    /// `working_dir` — the directory sipi should run in (where config/, scripts/, images/ are).
    /// `extra_args` — additional CLI arguments appended after standard ones.
    ///                 CLI args override config values (CLI precedence).
    pub fn start_with_args(config: &str, working_dir: &Path, extra_args: &[&str]) -> Self {
        Self::start_env(config, working_dir, extra_args, &[])
    }

    /// Like [`Self::start_with_args`], additionally setting `extra_env` in
    /// the spawned process's environment.
    pub fn start_env(
        config: &str,
        working_dir: &Path,
        extra_args: &[&str],
        extra_env: &[(&str, &str)],
    ) -> Self {
        Self::spawn(sipi_bin_path(), config, working_dir, extra_args, extra_env)
    }

    /// Spawn the sipi process from the resolved `bin`, wait until it serves
    /// `/health`, and return the handle.
    fn spawn(
        bin: String,
        config: &str,
        working_dir: &Path,
        extra_args: &[&str],
        extra_env: &[(&str, &str)],
    ) -> Self {
        let (http_port, ssl_port) = allocate_ports();

        eprintln!(
            "[test-harness] Starting sipi: bin={} config={} ports={}/{} cwd={} extra_args={:?} extra_env={:?}",
            bin,
            config,
            http_port,
            ssl_port,
            working_dir.display(),
            extra_args,
            extra_env
        );

        // `--drain-timeout 2` keeps every test's sipi shutdown bounded to ~2s
        // (default is 30s). `stop()`'s 15s graceful deadline covers that
        // drain plus the up-to-2s wait for the drain threads to reach EOF
        // (so a report printed just before exit, e.g. LeakSanitizer's, lands
        // in the captured buffer), with ample margin even under ASan's
        // runtime overhead, so SIGKILL never fires on a still-draining
        // server. Placed before `extra_args` so an individual test can still
        // override it (last-wins).
        let mut cmd = Command::new(&bin);
        cmd.arg("server")
            .arg("--config")
            .arg(config)
            .arg("--serverport")
            .arg(http_port.to_string())
            // The Rust shell parses `--sslport` but serves plain HTTP behind
            // Traefik in production.
            .arg("--sslport")
            .arg(ssl_port.to_string());
        cmd.arg("--drain-timeout")
            .arg("2")
            .args(extra_args)
            .envs(extra_env.iter().copied())
            .current_dir(working_dir)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        apply_sanitizer_env(&mut cmd);

        // Coverage support: when sipi is built with --coverage (gcov/llvm-cov),
        // .gcda paths are embedded in the binary at compile time.
        //
        // CI (nix-clang): paths are absolute (/home/runner/.../build/CMakeFiles/...).
        // The gcda files write to the correct location regardless of cwd.
        // Do NOT set GCOV_PREFIX — it mangles absolute paths.
        //
        // Local (zig): paths are relative (CMakeFiles/...). When the test harness
        // changes cwd to test/_test_data/, gcda files would land there instead of
        // in build/. GCOV_PREFIX redirects them to the build dir.
        //
        // Derive `build_dir` from the *actually-resolved* sipi binary so a
        // `$SIPI_BIN` outside `repo_root/build` (static-musl CI, sanitizer CI,
        // or any custom binary) still computes the right dir.
        let build_dir = PathBuf::from(&bin)
            .parent()
            .expect("sipi binary should be in a build directory")
            .to_path_buf();
        if !build_dir.is_absolute() {
            cmd.env("GCOV_PREFIX", &build_dir)
                .env("GCOV_PREFIX_STRIP", "0");
        }

        let mut child = cmd.spawn().unwrap_or_else(|e| {
            panic!("Failed to start sipi at {}: {}", bin, e);
        });
        eprintln!("[test-harness] Spawned sipi PID={}", child.id());

        // Drain both child streams on background threads (keeps the pipe buffer
        // from filling and blocking the child) and feed the readiness channel on
        // the listen-log line.
        // Watching both streams keeps readiness independent of which sink a
        // given binary logs to.
        // server/rust logs this once `sipi::run` binds the axum listener. Used
        // as a fast readiness hint; the startup poll falls back to a TCP /
        // `/health` probe if it never appears (suppressed by log level, or
        // written to a stream we don't match), so this is an optimization, not
        // a hard requirement.
        let ready_signal = "SIPI Rust shell listening";
        let (tx, rx) = std::sync::mpsc::channel();
        let (stderr_buf, stderr_done) = spawn_log_drain(
            child.stderr.take().expect("stderr captured"),
            ready_signal,
            tx.clone(),
        );
        let (stdout_buf, stdout_done) = spawn_log_drain(
            child.stdout.take().expect("stdout captured"),
            ready_signal,
            tx,
        );

        // Wait for readiness: the listen-log signal (fast hint) or a successful
        // TCP connect. Polling both avoids a 30s stall when the signal is
        // suppressed or lands on a stream we don't match.
        let timeout = Duration::from_secs(30);
        let start = Instant::now();
        let mut connected = false;
        while start.elapsed() < timeout {
            if rx.try_recv().is_ok() {
                eprintln!(
                    "[test-harness] readiness signal after {:?}",
                    start.elapsed()
                );
                connected = true;
                break;
            }
            if TcpStream::connect(format!("127.0.0.1:{}", http_port)).is_ok() {
                connected = true;
                break;
            }
            if let Ok(Some(status)) = child.try_wait() {
                panic!(
                    "sipi exited with {} during startup\nstdout:\n{}\nstderr:\n{}",
                    status,
                    dump(&stdout_buf),
                    dump(&stderr_buf)
                );
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        if !connected {
            child.kill().ok();
            panic!(
                "sipi failed to start within {:?} on port {}\nstdout:\n{}\nstderr:\n{}",
                timeout,
                http_port,
                dump(&stdout_buf),
                dump(&stderr_buf)
            );
        }

        // The listen log / TCP-accept precedes the event loop actually
        // processing requests. Probe /health (Rust-native, engine-independent)
        // to confirm the server is serving.
        let base_url = format!("http://127.0.0.1:{}", http_port);
        let probe_client = reqwest::blocking::Client::builder()
            .danger_accept_invalid_certs(true)
            .timeout(Duration::from_secs(5))
            .build()
            .expect("probe client");

        let mut http_ready = false;
        let mut last_probe_err = String::new();
        let mut probe_count = 0u32;
        while start.elapsed() < timeout {
            probe_count += 1;
            match probe_client.get(format!("{}/health", base_url)).send() {
                Ok(resp) if resp.status().is_success() => {
                    http_ready = true;
                    eprintln!(
                        "[test-harness] HTTP ready after {} probes, {:?} total",
                        probe_count,
                        start.elapsed()
                    );
                    break;
                }
                Ok(resp) => {
                    last_probe_err = format!("HTTP {}", resp.status());
                }
                Err(e) => {
                    last_probe_err = format!("{}", e);
                }
            }
            if let Ok(Some(status)) = child.try_wait() {
                panic!(
                    "sipi exited with {} after {} /health probes\n\
                     last probe error: {}\nstdout:\n{}\nstderr:\n{}",
                    status,
                    probe_count,
                    last_probe_err,
                    dump(&stdout_buf),
                    dump(&stderr_buf)
                );
            }
            std::thread::sleep(Duration::from_millis(50));
        }

        if !http_ready {
            child.kill().ok();
            panic!(
                "sipi started but never served HTTP on port {} within {:?}\n\
                 probes attempted: {}, last error: {}\nstdout:\n{}\nstderr:\n{}",
                http_port,
                timeout,
                probe_count,
                last_probe_err,
                dump(&stdout_buf),
                dump(&stderr_buf)
            );
        }

        let pid = child.id();
        ensure_atexit_registered();
        LIVE.lock()
            .unwrap_or_else(|e| e.into_inner())
            .push(RegistryEntry {
                child,
                stdout_buf: stdout_buf.clone(),
                stderr_buf: stderr_buf.clone(),
                stdout_done,
                stderr_done,
            });

        SipiServer {
            pid,
            http_port,
            base_url,
            stdout_buf,
            stderr_buf,
            stopped: false,
        }
    }

    /// The server's captured stdout/stderr so far (accumulated on a
    /// background drain thread since the process was spawned). Useful for
    /// diagnosing a failure detected after startup (e.g. an unexpected exit
    /// code post-shutdown), where the log lines explaining it never reach
    /// the test's own captured output.
    pub fn captured_output(&self) -> (String, String) {
        (dump(&self.stdout_buf), dump(&self.stderr_buf))
    }

    /// Start a sipi server with the given config file (no extra args).
    pub fn start(config: &str, working_dir: &Path) -> Self {
        Self::start_with_args(config, working_dir, &[])
    }

    /// Start with the default test config.
    /// Sipi auto-creates the cache directory if missing.
    ///
    /// Sizes the engine-pool knobs so the concurrency tests (10 parallel requests)
    /// are served rather than shed, even under the ASan build's much slower image
    /// decodes: enough workers to drain the burst (`SIPI_NTHREADS`), a deep queue
    /// (`SIPI_MAX_WAITING`), and a wait long enough that a queued request is not
    /// shed before a worker frees (`SIPI_QUEUE_TIMEOUT`, well above the drain time
    /// and above the 30s client timeout that is the real hang guard). Also
    /// exercises the concurrency env wiring end-to-end.
    pub fn start_default() -> Self {
        let test_data = test_data_dir();
        Self::start_env(
            "config/sipi.e2e-test-config.lua",
            &test_data,
            &[],
            &[
                ("SIPI_NTHREADS", "8"),
                ("SIPI_MAX_WAITING", "256"),
                ("SIPI_QUEUE_TIMEOUT", "60"),
            ],
        )
    }

    /// Send `sig` to this server. Tests signal a process only through here, so
    /// the target is always a child this harness spawned.
    pub fn signal(&self, sig: Signal) -> nix::Result<()> {
        signal::kill(
            Pid::from_raw(i32::try_from(self.pid).expect("PID overflows i32")),
            sig,
        )
    }

    /// Check if the server process has exited. Returns the exit status if so.
    pub fn try_wait(&mut self) -> std::io::Result<Option<std::process::ExitStatus>> {
        let mut reg = LIVE.lock().unwrap_or_else(|e| e.into_inner());
        match reg.iter_mut().find(|e| e.child.id() == self.pid) {
            Some(entry) => entry.child.try_wait(),
            // Already stopped and removed from the registry.
            None => Ok(None),
        }
    }

    /// Gracefully stop the server via SIGTERM and judge its exit -- see
    /// [`stop_entry`] for the pass/fail rule. Idempotent: a second call (from
    /// `Drop`, after a test called this directly) is a no-op.
    pub fn stop(&mut self) -> Result<(), String> {
        if self.stopped {
            return Ok(());
        }
        self.stopped = true;

        let entry = {
            let mut reg = LIVE.lock().unwrap_or_else(|e| e.into_inner());
            reg.iter()
                .position(|e| e.child.id() == self.pid)
                .map(|idx| reg.remove(idx))
        };
        match entry {
            Some(entry) => stop_entry(entry),
            None => Ok(()),
        }
    }
}

impl Drop for SipiServer {
    /// Stops the server if a test hasn't already called [`Self::stop`]. A
    /// failed exit fails loudly (`panic!`) so a sanitizer finding or crash
    /// caught after the test body already returned isn't silently
    /// swallowed; during unwinding (a test already failed) it only
    /// `eprintln!`s, since panicking again would abort the process instead
    /// of reporting the original failure.
    fn drop(&mut self) {
        if let Err(msg) = self.stop() {
            if std::thread::panicking() {
                eprintln!("[test-harness] {msg}");
            } else {
                panic!("{msg}");
            }
        }
    }
}

/// Drain a child stream on a background thread: capture the whole stream
/// for diagnostics and signal `tx` on the first line containing
/// `ready_signal`. The capture is unbounded because a sanitizer report
/// printed at process exit must reach the failure output whole. Returns the
/// shared capture buffer and a receiver signalled once the stream reaches
/// EOF (the process exited and closed it), so a caller can bound how long
/// it waits for the final lines before reading the buffer.
fn spawn_log_drain(
    stream: impl std::io::Read + Send + 'static,
    ready_signal: &'static str,
    tx: std::sync::mpsc::Sender<()>,
) -> (Arc<Mutex<String>>, std::sync::mpsc::Receiver<()>) {
    let buf = Arc::new(Mutex::new(String::new()));
    let buf_clone = Arc::clone(&buf);
    let (done_tx, done_rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        let reader = BufReader::new(stream);
        for line in reader.lines() {
            let Ok(l) = line else { break };
            if let Ok(mut captured) = buf_clone.lock() {
                captured.push_str(&l);
                captured.push('\n');
            }
            if l.contains(ready_signal) {
                let _ = tx.send(());
            }
        }
        let _ = done_tx.send(());
    });
    (buf, done_rx)
}

/// Snapshot a capture buffer for inclusion in a panic message.
fn dump(buf: &Arc<Mutex<String>>) -> String {
    buf.lock().map(|s| s.clone()).unwrap_or_default()
}

/// The current process's `LSAN_OPTIONS`, with any `suppressions=<relative
/// path>` entry rewritten to an absolute path resolved against this
/// process's cwd, or `None` if `LSAN_OPTIONS` is unset.
///
/// `sipi_e2e_test.bzl`'s macro sets `LSAN_OPTIONS` to a runfiles-relative
/// path, valid only when resolved from the runfiles root — this test
/// process's cwd. A spawned sipi given a different `current_dir` can't
/// resolve that relative path; LeakSanitizer then refuses to run the leak
/// check and the process exits non-zero.
fn sanitizer_env() -> Option<String> {
    let options = std::env::var("LSAN_OPTIONS").ok()?;
    let cwd = std::env::current_dir().ok()?;
    Some(
        options
            .split(':')
            .map(|opt| match opt.strip_prefix("suppressions=") {
                Some(rel) if !Path::new(rel).is_absolute() => {
                    let abs = cwd.join(rel);
                    let abs = abs.canonicalize().unwrap_or(abs);
                    format!("suppressions={}", abs.display())
                }
                _ => opt.to_string(),
            })
            .collect::<Vec<_>>()
            .join(":"),
    )
}

/// Sets `LSAN_OPTIONS` on `cmd` from [`sanitizer_env`], if present. Apply to
/// every `Command` that spawns the sipi binary with a `current_dir` other
/// than the test's own.
pub fn apply_sanitizer_env(cmd: &mut Command) {
    if let Some(lsan_options) = sanitizer_env() {
        cmd.env("LSAN_OPTIONS", lsan_options);
    }
}

/// Path to the sipi repository root.
///
/// Resolution order:
///   1. `$SIPI_REPO_ROOT` if set — required when this crate is built in a
///      sandbox, because `CARGO_MANIFEST_DIR` is baked at compile time
///      and points at the (now-deleted) sandbox source path. Bazel
///      `rust_test` sets it to the `:test_fixtures` `copy_to_directory`
///      output (read-only).
///   2. `CARGO_MANIFEST_DIR/../..` — the inner-loop `cargo test` path
///      (writable source tree).
///
/// **Writability — Bazel-specific shim.** Bazel's `copy_to_directory`
/// output is read-only (`-r-xr-xr-x`), but the e2e tests write under
/// `test_data_dir()` for cache-isolation subdirs (`test/cache.rs`),
/// throw-away configs (`test/config.rs`), and uploaded blobs
/// (`test/upload.rs`). The first `repo_root()` call after Bazel sets
/// `SIPI_REPO_ROOT` to a read-only directory therefore performs a
/// one-shot recursive copy into `$TEST_TMPDIR/sipi-repo-root` and
/// caches the writable path in `WRITABLE_REPO_ROOT` for subsequent
/// calls. The copy is fast (the source is already laid out — Bazel
/// just doesn't grant write perms) and per-test-binary, so the cost
/// stays bounded.
///
/// The result is canonicalised to an absolute path so subsequent
/// `Command::current_dir(repo_root().join(...))` calls don't compound
/// with sipi's `--file`/`--config` resolution into path-prefix-doubled
/// lookups.
pub fn repo_root() -> PathBuf {
    static WRITABLE_REPO_ROOT: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();

    WRITABLE_REPO_ROOT
        .get_or_init(|| {
            let raw = if let Ok(p) = std::env::var("SIPI_REPO_ROOT") {
                PathBuf::from(p)
            } else {
                PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                    .parent()
                    .and_then(|p| p.parent())
                    .expect("e2e should be two levels below repo root")
                    .to_path_buf()
            };
            let canon = raw.canonicalize().unwrap_or_else(|_| raw.clone());

            // Probe writability with a temp-file probe rather than
            // checking permissions, so the cargo inner-loop (which
            // points at the source tree, fully writable) skips the
            // copy. The probe writes a tiny file in the target dir
            // and removes it; if that fails with PermissionDenied,
            // we know we need the TEST_TMPDIR copy.
            let probe = canon.join(".sipi-e2e-writable-probe");
            let writable = std::fs::File::create(&probe).is_ok();
            let _ = std::fs::remove_file(&probe);
            if writable {
                return canon;
            }

            // Read-only source (Bazel `copy_to_directory` output).
            // Copy into `$TEST_TMPDIR/sipi-repo-root` so tests can
            // write under `test_data_dir()` etc. `TEST_TMPDIR` is set
            // by Bazel for every test action and is per-binary, so
            // siblings can't collide.
            let tmpdir = std::env::var("TEST_TMPDIR")
                .map(PathBuf::from)
                .unwrap_or_else(|_| std::env::temp_dir());
            let dst = tmpdir.join("sipi-repo-root");
            // If a previous test in the same binary already populated
            // `dst`, the copy is a no-op (skip on `dst` exists).
            if !dst.exists() {
                copy_dir_recursive_writable(&canon, &dst).unwrap_or_else(|e| {
                    panic!(
                        "copy repo_root from {} to {} failed: {}",
                        canon.display(),
                        dst.display(),
                        e
                    );
                });
            }
            dst
        })
        .clone()
}

/// Recursive directory copy with `chmod u+w` applied to every output
/// (so the cache/config/upload tests can subsequently write under
/// it). Symlinks at the source are followed (the Bazel
/// `copy_to_directory` output already materialises everything as
/// real files, so there are no source symlinks; the cargo inner-loop
/// path doesn't enter this function in the first place).
fn copy_dir_recursive_writable(src: &Path, dst: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(dst)?;
    for entry in std::fs::read_dir(src)? {
        let entry = entry?;
        let src_path = entry.path();
        let dst_path = dst.join(entry.file_name());
        let ft = entry.file_type()?;
        if ft.is_dir() {
            copy_dir_recursive_writable(&src_path, &dst_path)?;
        } else {
            std::fs::copy(&src_path, &dst_path)?;
            // Add user-write so test write operations succeed.
            // Read perms are inherited from the source (already 644+).
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let mut perms = std::fs::metadata(&dst_path)?.permissions();
                let mode = perms.mode() | 0o200;
                perms.set_mode(mode);
                std::fs::set_permissions(&dst_path, perms)?;
            }
        }
    }
    Ok(())
}

/// Default sipi binary path for the cmake inner-loop dev shell
/// (`<repo>/build/sipi`).
///
/// Test harness resolution prefers `$SIPI_BIN` (set by
/// `just bazel-test-e2e` and the Bazel `rust_test` env to
/// `$(rootpath //src/cli/rust:sipi)`); this fallback only fires when
/// `SIPI_BIN` is unset (cmake inner-loop dev shell path).
pub fn find_sipi_bin() -> PathBuf {
    repo_root().join("build").join("sipi")
}

/// Resolve a sipi binary path from `env_var`, falling back to `fallback`,
/// and canonicalise the result to an absolute path.
///
/// Why canonicalise: callers spawn sipi via `Command::new(&path).
/// current_dir(working_dir)`, and Rust resolves the binary path AFTER
/// applying `current_dir`. A relative path like `src/sipi`
/// (Bazel `$(rootpath …)` output) would be looked up under
/// `working_dir/src/sipi` — wrong. The canonicalisation runs in the
/// parent's cwd, which under Bazel `rust_test` is the runfiles
/// workspace root where `src/sipi` does resolve.
///
/// Returns the binary path as a String (kept as String rather than
/// PathBuf so callers can pass it straight to `Command::new` or to
/// derived `PathBuf::from(&bin)` arithmetic in `SipiServer::spawn`).
pub fn sipi_bin_path_from(env_var: &str, fallback: PathBuf) -> String {
    let raw = std::env::var(env_var).unwrap_or_else(|_| fallback.to_string_lossy().to_string());
    std::path::Path::new(&raw)
        .canonicalize()
        .map(|p| p.to_string_lossy().into_owned())
        // Fall back to the raw value if canonicalize fails (e.g. file
        // doesn't exist yet) so the downstream `Command::spawn` panic
        // surfaces the original user input rather than swallowing it.
        .unwrap_or(raw)
}

/// The Rust shell under test (`//src/cli/rust:sipi`), from `$SIPI_BIN` or the
/// cmake inner-loop fallback (`<repo>/build/sipi`).
pub fn sipi_bin_path() -> String {
    sipi_bin_path_from("SIPI_BIN", find_sipi_bin())
}

/// Path to the test data directory.
pub fn test_data_dir() -> PathBuf {
    repo_root().join("test").join("_test_data")
}

// =============================================================================
// CLI-mode test helpers (offline `sipi convert`/`query`/... verbs).
//
// Shared by the `cli*` e2e targets. The JP2 codec work is heavy under ASan, so
// the heavy conversions are split out of `//test/e2e:cli` into the sibling
// `//test/e2e:cli_conversions` target, which fans them out across threads via
// `run_conversions`; these helpers keep both files small and their assertions
// identical.
// =============================================================================

/// Run `sipi convert <input> <output> --format <format>` from the test-data
/// dir and return the process output.
pub fn cli_convert(input: &str, output: &str, format: &str) -> std::process::Output {
    let mut cmd = Command::new(sipi_bin_path());
    cmd.arg("convert")
        .arg(input)
        .arg(output)
        .arg("--format")
        .arg(format)
        .current_dir(test_data_dir());
    apply_sanitizer_env(&mut cmd);
    cmd.output()
        .unwrap_or_else(|e| panic!("Failed to run sipi CLI: {}", e))
}

/// Run `sipi <args...>` from the test-data dir and return the process output.
pub fn cli_run(args: &[&str]) -> std::process::Output {
    let mut cmd = Command::new(sipi_bin_path());
    cmd.args(args).current_dir(test_data_dir());
    apply_sanitizer_env(&mut cmd);
    cmd.output()
        .unwrap_or_else(|e| panic!("Failed to run sipi {:?}: {}", args, e))
}

/// A scratch path under `$TMPDIR` (or `/tmp`) for CLI conversion outputs.
pub fn cli_tmp_path(name: &str) -> PathBuf {
    let dir = std::env::var("TMPDIR").unwrap_or_else(|_| "/tmp".to_string());
    PathBuf::from(dir).join(name)
}

/// `jpeg` with one more APPn segment (`marker` 0xE0..=0xEF) carrying
/// `payload`, placed after the existing APPn segments. The JPEG reader keeps
/// the last Exif APP1 it sees, so an inserted Exif segment wins over one the
/// file already has.
pub fn jpeg_with_app_segment(jpeg: &[u8], marker: u8, payload: &[u8]) -> Vec<u8> {
    assert!(
        (0xE0..=0xEF).contains(&marker),
        "not an APPn marker: {marker:#04x}"
    );
    assert_eq!(&jpeg[..2], &[0xFF, 0xD8], "input is not a JPEG");
    let seg_len = u16::try_from(payload.len() + 2).expect("APPn payload over 65533 bytes");
    let mut pos = 2;
    while jpeg[pos] == 0xFF && (0xE0..=0xEF).contains(&jpeg[pos + 1]) {
        pos += 2 + usize::from(u16::from_be_bytes([jpeg[pos + 2], jpeg[pos + 3]]));
    }
    let mut out = Vec::with_capacity(jpeg.len() + payload.len() + 4);
    out.extend_from_slice(&jpeg[..pos]);
    out.extend_from_slice(&[0xFF, marker]);
    out.extend_from_slice(&seg_len.to_be_bytes());
    out.extend_from_slice(payload);
    out.extend_from_slice(&jpeg[pos..]);
    out
}

/// An APP1 Exif payload (`Exif\0\0` and a little-endian TIFF structure) whose
/// only field is `Exif.Photo.UserComment` with the raw bytes `comment`: the
/// 8-byte charset id followed by the text in that charset.
pub fn exif_app1_with_user_comment(comment: &[u8]) -> Vec<u8> {
    const IFD0: u32 = 8;
    const EXIF_IFD: u32 = IFD0 + 18;
    const COMMENT: u32 = EXIF_IFD + 18;
    let count = u32::try_from(comment.len()).expect("comment too long");
    let mut out = b"Exif\0\0II".to_vec();
    out.extend_from_slice(&42u16.to_le_bytes());
    out.extend_from_slice(&IFD0.to_le_bytes());
    // IFD0: one entry, the ExifIFD pointer (tag 0x8769, LONG).
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&0x8769u16.to_le_bytes());
    out.extend_from_slice(&4u16.to_le_bytes());
    out.extend_from_slice(&1u32.to_le_bytes());
    out.extend_from_slice(&EXIF_IFD.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    // ExifIFD: one entry, UserComment (tag 0x9286, UNDEFINED).
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&0x9286u16.to_le_bytes());
    out.extend_from_slice(&7u16.to_le_bytes());
    out.extend_from_slice(&count.to_le_bytes());
    out.extend_from_slice(&COMMENT.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(comment);
    out
}

/// Write to `dest` a copy of the unit `MaoriFigure.jpg` with an APP13
/// Photoshop 3.0 segment whose IPTC resource (`8BIM` 0x0404) does not parse:
/// its one dataset claims 0x7fff length bytes, which Exiv2's
/// `IptcParser::decode` rejects.
pub fn write_jpeg_with_garbage_iptc(dest: &Path) {
    let source = test_data_dir().join("images/unit/MaoriFigure.jpg");
    let jpeg = std::fs::read(&source).unwrap_or_else(|e| panic!("read {}: {e}", source.display()));
    let iptc = [0x1c, 0x02, 0x05, 0xff, 0xff, 0x00, 0x00, 0x00];
    let mut payload = b"Photoshop 3.0\0".to_vec();
    payload.extend_from_slice(b"8BIM\x04\x04\x00\x00");
    payload.extend_from_slice(&(iptc.len() as u32).to_be_bytes());
    payload.extend_from_slice(&iptc);
    let out = jpeg_with_app_segment(&jpeg, 0xED, &payload);
    std::fs::write(dest, out).unwrap_or_else(|e| panic!("write {}: {e}", dest.display()));
}

/// Decode `file<i>.jp2` from the ISO 15444-4 conformance set to TIFF and assert
/// the output exists and is non-empty (port of `test_iso_15444_4_decode_jp2`).
pub fn assert_jp2_decode(i: u32) {
    let input = test_data_dir()
        .join("images/iso-15444-4/testfiles_jp2")
        .join(format!("file{}.jp2", i));
    let output = cli_tmp_path(&format!("sipi_cli_decode_{}.tif", i));

    let result = cli_convert(input.to_str().unwrap(), output.to_str().unwrap(), "tif");
    assert!(
        result.status.success(),
        "JP2→TIFF decode for file{}.jp2 failed: {}",
        i,
        String::from_utf8_lossy(&result.stderr)
    );
    assert!(output.exists(), "output TIFF should exist for file{}", i);
    let meta = std::fs::metadata(&output).expect("read output metadata");
    assert!(
        meta.len() > 0,
        "output TIFF should not be empty for file{}",
        i
    );

    let _ = std::fs::remove_file(&output);
}

/// TIFF → JP2 → TIFF round-trip for `reference_jp2/jp2_<i>.tif` (port of
/// `test_iso_15444_4_round_trip`): assert each step succeeds and the
/// round-tripped TIFF is a sane size. No-op if the fixture is absent.
pub fn assert_jp2_roundtrip(i: u32) {
    let reference_tif = test_data_dir()
        .join("images/iso-15444-4/reference_jp2")
        .join(format!("jp2_{}.tif", i));

    if !reference_tif.exists() {
        eprintln!("Skipping round-trip for jp2_{}.tif — not found", i);
        return;
    }

    let intermediate_jp2 = cli_tmp_path(&format!("sipi_cli_rt_{}.jp2", i));
    let output_tif = cli_tmp_path(&format!("sipi_cli_rt_{}.tif", i));

    let result1 = cli_convert(
        reference_tif.to_str().unwrap(),
        intermediate_jp2.to_str().unwrap(),
        "jpx",
    );
    assert!(
        result1.status.success(),
        "TIFF→JP2 for jp2_{}.tif failed: {}",
        i,
        String::from_utf8_lossy(&result1.stderr)
    );
    assert!(
        intermediate_jp2.exists(),
        "intermediate JP2 should exist for file {}",
        i
    );

    let result2 = cli_convert(
        intermediate_jp2.to_str().unwrap(),
        output_tif.to_str().unwrap(),
        "tif",
    );
    assert!(
        result2.status.success(),
        "JP2→TIFF round-trip for file {} failed: {}",
        i,
        String::from_utf8_lossy(&result2.stderr)
    );
    assert!(
        output_tif.exists(),
        "round-trip TIFF should exist for file {}",
        i
    );

    let ref_size = std::fs::metadata(&reference_tif).unwrap().len();
    let out_size = std::fs::metadata(&output_tif).unwrap().len();
    assert!(
        out_size > 0,
        "round-trip TIFF for file {} should not be empty",
        i
    );
    // Round-trip may not be byte-identical, but should be within ~2x size.
    assert!(
        out_size < ref_size * 3,
        "round-trip TIFF for file {} is suspiciously large ({} vs ref {})",
        i,
        out_size,
        ref_size
    );

    let _ = std::fs::remove_file(&intermediate_jp2);
    let _ = std::fs::remove_file(&output_tif);
}

/// The `nx`, `ny` and `nc` lines of a `sipi query` dump.
fn query_header(path: &str) -> Vec<String> {
    let result = cli_run(&["query", path]);
    assert!(
        result.status.success(),
        "sipi query {path} failed: {}",
        String::from_utf8_lossy(&result.stderr)
    );
    String::from_utf8_lossy(&result.stdout)
        .lines()
        .filter(|l| l.starts_with("nx ") || l.starts_with("ny ") || l.starts_with("nc "))
        .map(str::to_string)
        .collect()
}

/// `sipi query` reads only the header and metadata of a JPX: on a JPX that
/// `sipi convert` wrote from `cmyk.tif`, it must report the source's
/// dimensions and channel count.
pub fn assert_query_jpx_header() {
    let input = test_data_dir().join("images/unit/cmyk.tif");
    let jpx = cli_tmp_path("sipi_cli_query_cmyk.jpx");
    let r = cli_convert(input.to_str().unwrap(), jpx.to_str().unwrap(), "jpx");
    assert!(
        r.status.success(),
        "TIFF→JPX for the query test failed: {}",
        String::from_utf8_lossy(&r.stderr)
    );
    let source = query_header(input.to_str().unwrap());
    assert_eq!(
        source.len(),
        3,
        "expected nx, ny and nc lines, got {source:?}"
    );
    assert_eq!(query_header(jpx.to_str().unwrap()), source);
    let _ = std::fs::remove_file(&jpx);
}

/// Convert a TIFF to JP2 and back, then to JPEG and PNG, asserting each output
/// carries valid magic bytes (port of the `cli_metadata_fidelity` test).
pub fn assert_metadata_fidelity() {
    let input = test_data_dir().join("images/unit/lena512.tif");
    if !input.exists() {
        eprintln!("Skipping metadata fidelity: lena512.tif not found");
        return;
    }

    // TIFF → JP2 → TIFF, then assert the round-trip is a valid TIFF.
    let jp2_output = cli_tmp_path("sipi_cli_meta.jp2");
    let r1 = cli_convert(input.to_str().unwrap(), jp2_output.to_str().unwrap(), "jpx");
    assert!(
        r1.status.success(),
        "TIFF→JP2 metadata test failed: {}",
        String::from_utf8_lossy(&r1.stderr)
    );
    let tif_output = cli_tmp_path("sipi_cli_meta_rt.tif");
    let r2 = cli_convert(
        jp2_output.to_str().unwrap(),
        tif_output.to_str().unwrap(),
        "tif",
    );
    assert!(
        r2.status.success(),
        "JP2→TIFF metadata test failed: {}",
        String::from_utf8_lossy(&r2.stderr)
    );
    let tif_bytes = std::fs::read(&tif_output).expect("read output TIFF");
    assert!(tif_bytes.len() > 4, "output TIFF too small");
    let is_tiff = (tif_bytes[0] == b'I' && tif_bytes[1] == b'I')
        || (tif_bytes[0] == b'M' && tif_bytes[1] == b'M');
    assert!(
        is_tiff,
        "output should be valid TIFF (starts with II or MM)"
    );

    // Format diversity: TIFF → JPEG and TIFF → PNG.
    let jpg_output = cli_tmp_path("sipi_cli_meta.jpg");
    let r3 = cli_convert(input.to_str().unwrap(), jpg_output.to_str().unwrap(), "jpg");
    assert!(
        r3.status.success(),
        "TIFF→JPEG conversion failed: {}",
        String::from_utf8_lossy(&r3.stderr)
    );
    let jpg_bytes = std::fs::read(&jpg_output).expect("read JPEG output");
    assert!(
        jpg_bytes.len() > 2 && jpg_bytes[0] == 0xFF && jpg_bytes[1] == 0xD8,
        "output should be valid JPEG"
    );

    let png_output = cli_tmp_path("sipi_cli_meta.png");
    let r4 = cli_convert(input.to_str().unwrap(), png_output.to_str().unwrap(), "png");
    assert!(
        r4.status.success(),
        "TIFF→PNG conversion failed: {}",
        String::from_utf8_lossy(&r4.stderr)
    );
    let png_bytes = std::fs::read(&png_output).expect("read PNG output");
    assert!(
        png_bytes.len() > 8 && &png_bytes[1..4] == b"PNG",
        "output should be valid PNG"
    );

    for path in [&jp2_output, &tif_output, &jpg_output, &png_output] {
        let _ = std::fs::remove_file(path);
    }
}

/// The ADR-0009 Service → Access pipeline through the CLI (port of
/// `cli_verify_pipeline_service_and_access_files`): `convert service-file`
/// stamps an Essentials packet that `verify service-file` accepts, then
/// `convert access-file` drops it and `verify access-file` accepts the result;
/// the cross-checks assert each verify mode rejects the wrong file kind.
pub fn assert_verify_pipeline() {
    let service = cli_tmp_path("sipi_cli_verify_service.jp2");
    let access = cli_tmp_path("sipi_cli_verify_access.jpg");
    let _ = std::fs::remove_file(&service);
    let _ = std::fs::remove_file(&access);

    let r1 = cli_run(&[
        "convert",
        "service-file",
        "images/unit/lena512.tif",
        service.to_str().unwrap(),
    ]);
    assert!(
        r1.status.success(),
        "convert service-file must succeed; stderr:\n{}",
        String::from_utf8_lossy(&r1.stderr)
    );

    let r2 = cli_run(&["verify", "service-file", service.to_str().unwrap()]);
    assert!(
        r2.status.success(),
        "verify service-file must accept its own convert service-file output; stderr:\n{}",
        String::from_utf8_lossy(&r2.stderr)
    );

    let r3 = cli_run(&[
        "convert",
        "access-file",
        service.to_str().unwrap(),
        access.to_str().unwrap(),
    ]);
    assert!(
        r3.status.success(),
        "convert access-file must succeed on a Service File input; stderr:\n{}",
        String::from_utf8_lossy(&r3.stderr)
    );

    let r4 = cli_run(&["verify", "access-file", access.to_str().unwrap()]);
    assert!(
        r4.status.success(),
        "verify access-file must accept its own convert access-file output; stderr:\n{}",
        String::from_utf8_lossy(&r4.stderr)
    );

    // Cross-checks: an Access File must fail verify service-file (no
    // Essentials) and a Service File must fail verify access-file (carries one).
    let r5 = cli_run(&["verify", "service-file", access.to_str().unwrap()]);
    assert!(
        !r5.status.success(),
        "verify service-file must reject an Access File (no Essentials packet)"
    );
    let r6 = cli_run(&["verify", "access-file", service.to_str().unwrap()]);
    assert!(
        !r6.status.success(),
        "verify access-file must reject a Service File (carries an Essentials packet)"
    );

    let _ = std::fs::remove_file(&service);
    let _ = std::fs::remove_file(&access);
}

/// A labelled CLI-conversion task for [`run_conversions`]. The `label` is what
/// the executor prints if the task fails, so it should name the conversion
/// precisely (e.g. `"jp2_roundtrip_7"`).
pub struct ConversionTask {
    label: String,
    run: Box<dyn FnOnce() + Send>,
}

/// Build a labelled [`ConversionTask`] from a closure (usually a call to one of
/// the `assert_*` conversion helpers).
pub fn conversion_task(
    label: impl Into<String>,
    run: impl FnOnce() + Send + 'static,
) -> ConversionTask {
    ConversionTask {
        label: label.into(),
        run: Box::new(run),
    }
}

/// Thread count for [`run_conversions`]: `SIPI_E2E_CONVERT_THREADS` if set and
/// parseable, otherwise the machine's available parallelism. A single-core
/// action degrades to serial execution (no regression versus the old one-test
/// form); a many-core RBE worker fans the conversions out.
pub fn conversion_threads() -> usize {
    std::env::var("SIPI_E2E_CONVERT_THREADS")
        .ok()
        .and_then(|v| v.parse::<usize>().ok())
        .map(|n| n.max(1))
        .unwrap_or_else(|| std::thread::available_parallelism().map_or(1, |n| n.get()))
}

/// Run `tasks` across up to `threads` OS threads. Each task spawns its own
/// `sipi` subprocess, so they are independent. Every task is isolated with
/// `catch_unwind`, so one failure never hides another: ALL failures are
/// collected across every task, then reported together — the resulting panic
/// lists each failed task by its label. (The individual panic messages, with
/// their `file:line`, still print to stderr as they occur.)
pub fn run_conversions(tasks: Vec<ConversionTask>, threads: usize) {
    let threads = threads.clamp(1, tasks.len().max(1));

    // Round-robin the tasks into `threads` buckets so the work spreads evenly.
    let mut buckets: Vec<Vec<ConversionTask>> = (0..threads).map(|_| Vec::new()).collect();
    for (i, task) in tasks.into_iter().enumerate() {
        buckets[i % threads].push(task);
    }

    let handles: Vec<_> = buckets
        .into_iter()
        .map(|bucket| {
            std::thread::spawn(move || {
                let mut failures: Vec<String> = Vec::new();
                for task in bucket {
                    let ConversionTask { label, run } = task;
                    if let Err(payload) =
                        std::panic::catch_unwind(std::panic::AssertUnwindSafe(run))
                    {
                        let msg = payload
                            .downcast_ref::<String>()
                            .map(|s| s.as_str())
                            .or_else(|| payload.downcast_ref::<&str>().copied())
                            .unwrap_or("<non-string panic>");
                        failures.push(format!("{label}: {msg}"));
                    }
                }
                failures
            })
        })
        .collect();

    let mut failures: Vec<String> = Vec::new();
    for handle in handles {
        match handle.join() {
            Ok(mut bucket_failures) => failures.append(&mut bucket_failures),
            Err(_) => failures.push("<a conversion worker thread itself panicked>".to_string()),
        }
    }

    assert!(
        failures.is_empty(),
        "{} conversion task(s) failed:\n  - {}",
        failures.len(),
        failures.join("\n  - ")
    );
}

/// Build a reqwest blocking client that accepts self-signed certs.
///
/// Connection pooling is disabled (`pool_max_idle_per_host(0)`) so that each
/// request opens a fresh TCP connection. This prevents stale keep-alive
/// connections from accumulating in both the client pool and the server's
/// poll set — the root cause of transient "error sending request" failures
/// on fast arm64 musl builds where the server's idle-connection sweep never
/// triggers because tests run back-to-back without a 5-second idle gap.
pub fn http_client() -> reqwest::blocking::Client {
    reqwest::blocking::Client::builder()
        .danger_accept_invalid_certs(true)
        .timeout(Duration::from_secs(30))
        .pool_max_idle_per_host(0)
        .build()
        .expect("Failed to build HTTP client")
}

/// Poll `dir`'s file count until `stop` is satisfied or 2s elapses (a cache
/// write may lag a hair behind the HTTP response), returning the last
/// sampled count either way.
/// SipiCache writes flat `cache_XXXXXXXXXX` files directly under `dir` (no
/// subdirectories) and only writes its `.sipicache` index at shutdown, so a
/// plain top-level file count is an accurate on-disk proxy for cache state
/// while the server is running.
pub fn poll_cache_file_count(dir: &Path, stop: impl Fn(usize) -> bool) -> usize {
    let deadline = Instant::now() + Duration::from_secs(2);
    loop {
        let count = std::fs::read_dir(dir)
            .map(|entries| {
                entries
                    .filter_map(Result::ok)
                    .filter(|e| e.path().is_file())
                    .count()
            })
            .unwrap_or(0);
        if stop(count) || Instant::now() >= deadline {
            return count;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}
