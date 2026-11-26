// Only compile when the "docker" feature is enabled (or when run explicitly via --test docker_smoke).
// This prevents these tests from running in `cargo test` (used by zig-test-e2e)
// since they require a pre-built Docker image.
#![cfg(feature = "docker")]

/// Docker-image smoke tests — validates that the packaged Docker image works.
///
/// Uses docker CLI directly (no testcontainers) for simplicity and to avoid
/// async runtime issues. Each test starts a container, runs a request, and
/// cleans up via Drop.
///
/// Image-source resolution:
///   - If `SIPI_IMAGE_TAR` is set (Bazel `:docker_smoke` target),
///     `docker load -i $SIPI_IMAGE_TAR` runs once at suite startup and the
///     loaded image is referenced by the tag in `SIPI_IMAGE_TAG`
///     (default `sipi:e2e`). The tarball is the OCI bundle produced by
///     `//src:image_load`'s `tarball` output group.
///   - Otherwise the test falls back to whatever image is already loaded
///     under `daschswiss/sipi:latest` (the inner-loop `just test-smoke`
///     path, which builds the image into the local Docker daemon
///     beforehand).
///
/// Run via Bazel: `just bazel-test-smoke`.
/// Run via cargo (inner-loop): `just test-smoke` (builds the image first).
use reqwest::blocking::Client;
use std::process::Command;
use std::sync::OnceLock;
use std::time::{Duration, Instant};

/// Default image tag used when `SIPI_IMAGE_TAG` is unset (cargo inner-loop
/// path, where `just test-smoke` has built and tagged the image as
/// `daschswiss/sipi:latest` upstream of this test).
const DEFAULT_IMAGE_TAG: &str = "daschswiss/sipi:latest";

/// Resolve the image tag once and cache it. Performs a one-time
/// `docker load -i $SIPI_IMAGE_TAR` if the tarball env var is set
/// (Bazel-driven path).
fn image_tag() -> &'static str {
    static TAG: OnceLock<String> = OnceLock::new();
    TAG.get_or_init(|| {
        if let Ok(tar_path) = std::env::var("SIPI_IMAGE_TAR") {
            // Bazel path: load the OCI tarball into the daemon. Idempotent —
            // `docker load` is a no-op if the image is already present.
            let load = Command::new("docker")
                .args(["load", "-i", &tar_path])
                .output()
                .expect("docker load failed (is the daemon running?)");
            assert!(
                load.status.success(),
                "docker load failed for {}: {}",
                tar_path,
                String::from_utf8_lossy(&load.stderr)
            );
        }
        std::env::var("SIPI_IMAGE_TAG").unwrap_or_else(|_| DEFAULT_IMAGE_TAG.to_string())
    })
    .as_str()
}

struct DockerContainer {
    id: String,
    port: u16,
}

impl DockerContainer {
    fn start() -> Self {
        let root = project_root();
        // Bind-mount paths must be absolute for the Docker daemon. Under
        // Bazel runfiles `repo_root()` returns "." (relative to the test
        // action's cwd), so canonicalise before formatting the bind specs.
        let root_abs = root.canonicalize().unwrap_or_else(|e| {
            panic!("repo_root canonicalize failed: {} ({})", root.display(), e)
        });

        // Fail fast if a bind-mount source is missing. Under Bazel
        // runfiles a misconfigured `:test_fixtures` produces an empty
        // `config/` (or similar) which would otherwise surface only as
        // `--config: File does not exist: /sipi/config/sipi.config.lua`
        // from sipi inside the container — opaque to triage without a
        // post-mortem. The required files are verified explicitly.
        let required = [
            ("config/sipi.config.lua", "config layer"),
            (
                "test/_test_data/images/unit/lena512.jp2",
                "test images layer",
            ),
            ("scripts/send_response.lua", "scripts layer"),
            ("server/test.html", "server layer"),
        ];
        for (rel, label) in required {
            let p = root_abs.join(rel);
            assert!(
                p.exists(),
                "bind-mount source missing for {label}: expected {} (root_abs={}, root={})\n\
                 Hint: `:test_fixtures` (`copy_to_directory` in `//:BUILD.bazel`) likely \
                 didn't include this file. Check the per-package `:all_*` filegroups.",
                p.display(),
                root_abs.display(),
                root.display(),
            );
        }
        // Run without `--rm` so a fast sipi crash leaves the container in
        // place for log inspection. We `docker rm -f` in `Drop` instead.
        // Use an explicit `-p 0:1024` to map a random host port to the
        // container's 1024 (independent of the image's `ExposedPorts`
        // config, which has been observed not to round-trip cleanly
        // through `docker load`).
        let output = Command::new("docker")
            .args([
                "run",
                "-d",
                "-v",
                &format!("{}:/sipi/config", root_abs.join("config").display()),
                "-v",
                &format!(
                    "{}:/sipi/images",
                    root_abs.join("test/_test_data/images").display()
                ),
                "-v",
                &format!("{}:/sipi/scripts", root_abs.join("scripts").display()),
                "-v",
                &format!("{}:/sipi/server", root_abs.join("server").display()),
                // The shipped `config/sipi.config.lua` no longer carries a
                // `jwt_secret` (it must come from the environment), and startup
                // fail-closes on an empty/default/short key, so the image cannot
                // serve without one. Supply a ≥32-byte non-default test key, as
                // a real deployment supplies `SIPI_JWTKEY`.
                "-e",
                "SIPI_JWTKEY=docker-smoke-test-only-jwt-secret-0123456789",
                "-p",
                "0:1024",
                image_tag(),
            ])
            .output()
            .expect("docker run failed");

        assert!(
            output.status.success(),
            "docker run failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        let id = String::from_utf8(output.stdout).unwrap().trim().to_string();

        // Get the mapped port for 1024. `docker port <id> 1024/tcp` may
        // emit multiple lines (IPv4 + IPv6 bindings, e.g.
        // `0.0.0.0:32768\n[::]:32768`); pick the IPv4 line and extract
        // its trailing port number.
        let port_output = Command::new("docker")
            .args(["port", &id, "1024/tcp"])
            .output()
            .expect("docker port failed");
        let port_str = String::from_utf8(port_output.stdout).unwrap();
        let port_stderr = String::from_utf8(port_output.stderr).unwrap();
        let port: u16 = port_str
            .lines()
            .find(|l| l.starts_with("0.0.0.0:"))
            .or_else(|| port_str.lines().next())
            .and_then(|line| line.rsplit(':').next())
            .and_then(|p| p.trim().parse().ok())
            .unwrap_or_else(|| {
                // Surface post-mortem state so a CI failure is debuggable
                // without re-running. `docker logs` works because we
                // dropped `--rm` from `docker run` above; capture stderr
                // (sipi's structured logs go there) and the recorded port
                // mapping (empty if sipi exited before the daemon flushed
                // mappings — useful signal in itself).
                let logs = Command::new("docker")
                    .args(["logs", &id])
                    .output()
                    .map(|o| {
                        format!(
                            "stdout={:?}\n  stderr={:?}",
                            String::from_utf8_lossy(&o.stdout),
                            String::from_utf8_lossy(&o.stderr)
                        )
                    })
                    .unwrap_or_else(|e| format!("<logs failed: {}>", e));
                let inspect = Command::new("docker")
                    .args([
                        "inspect",
                        "--format={{.State.Status}} exit={{.State.ExitCode}} oom={{.State.OOMKilled}} err={{.State.Error}} ports={{json .NetworkSettings.Ports}}",
                        &id,
                    ])
                    .output()
                    .map(|o| String::from_utf8_lossy(&o.stdout).into_owned())
                    .unwrap_or_else(|e| format!("<inspect failed: {}>", e));
                // Best-effort cleanup of the dangling container so
                // subsequent test runs don't pile up (only matters for
                // local re-runs; CI runners are ephemeral).
                let _ = Command::new("docker").args(["rm", "-f", &id]).status();
                panic!(
                    "failed to parse mapped port for container {}\n  port stdout: {:?}\n  port stderr: {:?}\n  inspect: {}\n  logs: {}",
                    id, port_str, port_stderr, inspect, logs
                );
            });

        let container = DockerContainer { id, port };
        container.wait_ready();
        container
    }

    fn base_url(&self) -> String {
        format!("http://127.0.0.1:{}", self.port)
    }

    fn wait_ready(&self) {
        let client = Client::builder()
            .timeout(Duration::from_secs(2))
            .build()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(30);
        let url = format!("{}/server/test.html", self.base_url());

        while Instant::now() < deadline {
            if let Ok(resp) = client.get(&url).send() {
                if resp.status().as_u16() < 500 {
                    return;
                }
            }
            std::thread::sleep(Duration::from_millis(500));
        }
        panic!("SIPI Docker container did not become ready in 30s");
    }
}

impl Drop for DockerContainer {
    fn drop(&mut self) {
        // `docker run` was invoked without `--rm` so a fast crash can be
        // post-mortem'd via `docker logs`. Force-remove the container
        // here on the happy path. `rm -f` covers both running and
        // already-stopped containers.
        let _ = Command::new("docker").args(["rm", "-f", &self.id]).output();
    }
}

fn project_root() -> std::path::PathBuf {
    sipi_e2e::repo_root()
}

fn smoke_client() -> Client {
    Client::builder()
        .timeout(Duration::from_secs(30))
        .build()
        .expect("HTTP client build failed")
}

#[test]
fn docker_image_serves_iiif_image() {
    let container = DockerContainer::start();
    let c = smoke_client();

    let resp = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            container.base_url()
        ))
        .send()
        .expect("IIIF image request failed");

    assert_eq!(resp.status().as_u16(), 200, "IIIF image should return 200");
    let body = resp.bytes().expect("read body");
    assert!(
        body.len() > 1000,
        "response should contain image data (got {} bytes)",
        body.len()
    );
}

// `cmyk.tif` has no embedded ICC profile, so decoding it loads the built-in
// USWebCoatedSWOP profile that `//tools:bin2c` compiled into the binary. A
// binary that embedded something else fails the CMYK to sRGB conversion.
#[test]
fn docker_image_converts_cmyk_with_builtin_profile() {
    let container = DockerContainer::start();
    let c = smoke_client();

    let resp = c
        .get(format!(
            "{}/unit/cmyk.tif/full/max/0/default.jpg",
            container.base_url()
        ))
        .send()
        .expect("CMYK IIIF request failed");

    assert_eq!(
        resp.status().as_u16(),
        200,
        "CMYK TIFF should convert to JPEG with the built-in profile"
    );
    let body = resp.bytes().expect("read body");
    assert!(
        body.starts_with(&[0xFF, 0xD8]),
        "response should be a JPEG (got {} bytes)",
        body.len()
    );
}

#[test]
fn docker_image_serves_static_html() {
    let container = DockerContainer::start();
    let c = smoke_client();

    let resp = c
        .get(format!("{}/server/test.html", container.base_url()))
        .send()
        .expect("static HTML request failed");

    assert_eq!(resp.status().as_u16(), 200, "static HTML should return 200");
}

#[test]
fn docker_image_health_endpoint() {
    let container = DockerContainer::start();
    let c = smoke_client();

    let resp = c
        .get(format!("{}/health", container.base_url()))
        .send()
        .expect("health endpoint request failed");

    assert_eq!(resp.status().as_u16(), 200, "health should return 200");

    let body: serde_json::Value = resp.json().expect("health should return JSON");
    assert_eq!(body["status"], "ok");
    assert!(body["version"].is_string(), "version should be present");
    assert!(
        body["uptime_seconds"].is_number(),
        "uptime_seconds should be present"
    );
}

// The `sipi health` subcommand run *inside* the container — the exact command
// the orchestration-layer healthcheck uses (no `curl` dependency). The server
// listens on 1024 (the image default), so zero-arg `health` probes it and
// exits 0. `docker exec` of an explicit binary path needs no shell, so the
// distroless image is fine.
#[test]
fn docker_image_health_subcommand() {
    let container = DockerContainer::start();
    let status = Command::new("docker")
        .args(["exec", &container.id, "/sbin/sipi", "health"])
        .status()
        .expect("docker exec sipi health failed");
    assert!(
        status.success(),
        "in-container `sipi health` should exit 0 against a healthy server"
    );
}
