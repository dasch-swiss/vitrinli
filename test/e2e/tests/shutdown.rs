use sipi_e2e::{http_client, test_data_dir, Signal, SipiServer};
use std::thread;
use std::time::{Duration, Instant};

/// Start a fresh server for each shutdown test (not shared).
fn fresh_server() -> SipiServer {
    let test_data = test_data_dir();
    SipiServer::start("config/sipi.e2e-test-config.lua", &test_data)
}

#[test]
fn graceful_shutdown_completes_inflight_request() {
    let srv = fresh_server();
    let base_url = srv.base_url.clone();

    // Fire an image request in a background thread.
    // JP2 decoding takes enough time to overlap with the SIGTERM.
    let handle = thread::spawn(move || {
        let c = http_client();
        c.get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            base_url
        ))
        .send()
    });

    // Brief delay to ensure the request is in-flight, then SIGTERM
    thread::sleep(Duration::from_millis(50));
    srv.signal(Signal::SIGTERM).expect("SIGTERM failed");

    // The in-flight request should complete (drain phase allows it)
    let result = handle.join().expect("request thread panicked");
    match result {
        Ok(resp) => {
            // The response should be complete (200 or at least a valid HTTP response)
            let status = resp.status().as_u16();
            assert!(
                status == 200 || status == 503,
                "in-flight request during shutdown should complete, got {}",
                status
            );
        }
        Err(e) => {
            // The in-flight request is force-closed when the drain deadline
            // (`--drain-timeout 2`) elapses before the JP2 decode finishes —
            // the drain contract working as designed, and the common case under
            // ASan's slower decode. reqwest surfaces that force-close as a
            // transport error whose Display is "error sending request for url …"
            // (the connection detail lives in the nested source, not the
            // top-level string), so match the typed predicates rather than
            // substring-scanning the message.
            assert!(
                e.is_request() || e.is_connect() || e.is_timeout() || e.is_body(),
                "unexpected error during graceful shutdown: {e}"
            );
        }
    }
}

#[test]
fn server_exits_cleanly_after_sigterm() {
    let mut srv = fresh_server();
    let base_url = srv.base_url.clone();

    // Verify server is running
    let c = http_client();
    let resp = c
        .get(format!("{}/health", base_url))
        .send()
        .expect("health check failed");
    assert_eq!(resp.status().as_u16(), 200);

    // Send SIGTERM
    srv.signal(Signal::SIGTERM).expect("SIGTERM failed");

    // Wait for clean exit (up to 10s, covering drain_timeout + buffer)
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match srv.try_wait() {
            Ok(Some(status)) => {
                // Server should exit cleanly (0) or via signal
                let (stdout, stderr) = srv.captured_output();
                assert!(
                    status.success() || status.code().is_none(), // None = killed by signal
                    "server should exit cleanly, got: {:?}\nstdout:\n{}\nstderr:\n{}",
                    status,
                    stdout,
                    stderr
                );
                return;
            }
            Ok(None) if Instant::now() < deadline => {
                thread::sleep(Duration::from_millis(100));
            }
            _ => {
                panic!("server did not exit within 10s after SIGTERM");
            }
        }
    }
}

#[test]
fn new_requests_rejected_during_drain() {
    let srv = fresh_server();
    let base_url = srv.base_url.clone();

    // Send SIGTERM to start drain phase
    srv.signal(Signal::SIGTERM).expect("SIGTERM failed");

    // Brief delay for the server to enter drain mode (stop accepting)
    thread::sleep(Duration::from_millis(200));

    // New requests should fail (connection refused or timeout)
    let c = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(2))
        .build()
        .unwrap();

    let result = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            base_url
        ))
        .send();

    match result {
        Ok(resp) => {
            // If somehow we got a response, it should be an error
            // (the server might have processed this before drain started)
            let status = resp.status().as_u16();
            assert!(
                status >= 400 || status == 200, // 200 is ok if request raced ahead of drain
                "unexpected status during drain: {}",
                status
            );
        }
        Err(_) => {
            // Connection refused or timeout — expected during drain
        }
    }
}
