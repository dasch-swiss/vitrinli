mod common;

use common::{client, server};
use sipi_e2e::{http_client, poll_cache_file_count, test_data_dir, SipiServer};
use std::time::{Duration, Instant};

// =============================================================================
// Resource limits tests — verify server handles heavy load without crashes
// =============================================================================

/// DEV-7597: once the wedged decode threads reach `SIPI_MAX_WEDGED_THREADS`,
/// the server sheds every admitted request (IIIF and Lua routes) with 503
/// instead of letting each new hang pin another core, and `/health` fails so
/// the orchestrator restarts it. A 2 s decode deadline turns the DEV-7080
/// fixture into one wedged thread; with a cap of 1 the next request is shed.
///
/// A dedicated server (the shed state lasts until restart) on an isolated
/// cache dir, torn down by `Drop`; each request carries its own client timeout.
#[test]
fn wedged_threads_at_the_cap_shed_with_503() {
    let cache_dir = tempfile::tempdir().expect("create isolated cache dir");
    let cache_dir_str = cache_dir.path().to_string_lossy().into_owned();
    let srv = SipiServer::start_env(
        "config/sipi.e2e-test-config.lua",
        &test_data_dir(),
        &[],
        &[
            ("SIPI_DECODE_TIMEOUT_MS", "2000"),
            ("SIPI_MAX_WEDGED_THREADS", "1"),
            ("SIPI_REQUEST_TIMEOUT", "30"),
            ("SIPI_CACHE_DIR", cache_dir_str.as_str()),
        ],
    );
    let c = http_client();

    let start = Instant::now();
    let hung = c
        .get(format!(
            "{}/hang/dev7080_read_shape_hang.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .timeout(Duration::from_secs(25))
        .send()
        .expect("request for the hanging file failed");
    let elapsed = start.elapsed();
    assert_eq!(
        hung.status().as_u16(),
        500,
        "a decode past the deadline should answer the engine's 500"
    );
    assert!(
        elapsed < Duration::from_secs(20),
        "the 2s decode deadline should answer well before 20s (took {elapsed:?})"
    );

    let shed = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .timeout(Duration::from_secs(10))
        .send()
        .expect("request after the cap was reached failed");
    assert_eq!(
        shed.status().as_u16(),
        503,
        "every IIIF request should be shed once wedged threads reach the cap"
    );
    assert!(
        shed.headers().contains_key("retry-after"),
        "the shed 503 should carry Retry-After"
    );

    let lua_route = c
        .get(format!("{}/read_write_lua", srv.base_url))
        .timeout(Duration::from_secs(10))
        .send()
        .expect("Lua route request after the cap was reached failed");
    assert_eq!(
        lua_route.status().as_u16(),
        503,
        "a Lua route should be shed once wedged threads reach the cap"
    );

    let health = c
        .get(format!("{}/health", srv.base_url))
        .timeout(Duration::from_secs(10))
        .send()
        .expect("/health request after the cap was reached failed");
    assert_eq!(
        health.status().as_u16(),
        503,
        "/health should fail once wedged threads reach the cap, so the server is restarted"
    );
}

#[test]
fn sustained_load_memory_growth() {
    // Send 100+ sequential requests for large images and check the cache does not
    // grow unboundedly. Pinned on the on-disk file count under `cache_dir`, per
    // the DEV-6659 repin in `cache.rs`: the shell serves no `/metrics` route, so
    // the original scrape silently yielded no reading and the growth assertion
    // below never ran.
    let srv = server();
    let c = client();
    let cache_dir = test_data_dir().join("cache");

    let initial_cache_files = poll_cache_file_count(&cache_dir, |_| true);

    // Send 100 sequential requests alternating between info.json and image delivery.
    // The musl static binary can drop individual connections under sustained load
    // (independent of connection pooling). This test is about memory growth, not
    // 100% request success, so track failures instead of panicking.
    let total_requests = 100;
    let mut failures = 0u32;
    for i in 0..total_requests {
        let url = if i % 2 == 0 {
            format!("{}/unit/lena512.jp2/info.json", srv.base_url)
        } else {
            format!("{}/unit/lena512.jp2/full/max/0/default.jpg", srv.base_url)
        };

        match c.get(&url).send() {
            Ok(r) => {
                if r.status().as_u16() != 200 {
                    failures += 1;
                }
                let _ = r.bytes(); // consume body
            }
            Err(_) => {
                failures += 1;
            }
        }
    }

    let max_failures = total_requests / 20; // 5%
    assert!(
        failures <= max_failures,
        "{} of {} requests failed (max allowed: {})",
        failures,
        total_requests,
        max_failures
    );

    // Cache files should not grow unboundedly — the same two derivatives are
    // requested over and over, so the count stabilises rather than growing by 100.
    let final_cache_files = poll_cache_file_count(&cache_dir, |c| c > 0);
    // Guard the guard: with an empty cache dir the growth check below would pass
    // for the wrong reason, which is how the original `/metrics` version of this
    // test went silently vacuous.
    assert!(
        final_cache_files > 0,
        "100 image requests should have populated the cache at {}",
        cache_dir.display()
    );
    let growth = final_cache_files.saturating_sub(initial_cache_files);
    assert!(
        growth < 20,
        "cache files grew by {} over 100 requests (initial={}, final={}) — possible leak",
        growth,
        initial_cache_files,
        final_cache_files
    );

    // Verify server still responsive
    let health = c
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("server should respond after sustained load");
    assert_eq!(health.status().as_u16(), 200);
}

/// S2-18: a handler that exceeds `SIPI_REQUEST_TIMEOUT` answers 408, not
/// whatever the handler itself would eventually produce. Reuses the
/// hardening config's `/hardening/loop` route (an infinite pre-commit Lua
/// loop, see `lua_hardening.rs`) with the Lua deadline set far above the
/// request timeout, so the axum-level wall-clock timeout — not the Lua VM's
/// own deadline kill — is what fires and answers first.
///
/// Runs against its own empty cache dir (`tempfile::tempdir`), not the
/// hardening config's own `./cache`: that path is a relative literal shared
/// by several `_test_data/config/*.lua` files, and this file's `server()`
/// (a *second*, concurrently-running sipi process against the same working
/// directory) already owns it. A second process loading that shared
/// directory treats the first process's live cache files as orphans absent
/// from its own fresh index and deletes them (`SipiCache`'s crash-recovery
/// cleanup) — turning the shared server's next cache hit into a `500` for a
/// file that no longer exists. Same isolation pattern as
/// `admission_control.rs`'s `start()` helper.
#[test]
fn handler_exceeding_request_timeout_answers_408() {
    let cache_dir = tempfile::tempdir().expect("create isolated cache dir");
    let cache_dir_str = cache_dir.path().to_string_lossy().into_owned();
    let srv = SipiServer::start_env(
        "config/sipi.lua-hardening-config.lua",
        &test_data_dir(),
        &[],
        &[
            ("SIPI_LUA_TIMEOUT_MS", "10000"),
            ("SIPI_REQUEST_TIMEOUT", "1"),
            ("SIPI_CACHE_DIR", cache_dir_str.as_str()),
        ],
    );

    let start = Instant::now();
    let resp = http_client()
        .get(format!("{}/hardening/loop", srv.base_url))
        .send()
        .expect("request to the looping route failed");
    let elapsed = start.elapsed();

    assert_eq!(
        resp.status().as_u16(),
        408,
        "handler exceeding SIPI_REQUEST_TIMEOUT should answer 408"
    );
    assert!(
        elapsed < Duration::from_secs(9),
        "408 should fire near the 1s request timeout, not the 10s Lua deadline (took {elapsed:?})"
    );
}

#[test]
fn concurrent_large_image_decode() {
    // Send nthreads (4) simultaneous requests for the largest test image,
    // verify all succeed and server remains responsive.
    let srv = server();
    let nthreads = 4;

    let mut handles = vec![];
    let base_url = srv.base_url.clone();

    for i in 0..nthreads {
        let url = format!("{}/unit/lena512.jp2/full/max/0/default.jpg", base_url);
        let handle = std::thread::spawn(move || {
            let c = http_client();
            let resp = c.get(&url).send();
            match resp {
                Ok(r) => {
                    let status = r.status().as_u16();
                    let body = r.bytes().unwrap_or_default();
                    (i, status, body.len())
                }
                Err(e) => {
                    eprintln!("concurrent decode thread {} failed: {}", i, e);
                    (i, 0, 0)
                }
            }
        });
        handles.push(handle);
    }

    let mut success_count = 0;
    let mut first_size = 0;
    for handle in handles {
        let (idx, status, size) = handle.join().expect("thread panicked");
        if status == 200 {
            success_count += 1;
            if first_size == 0 {
                first_size = size;
            } else {
                // All responses for same image should be same size
                assert_eq!(
                    size, first_size,
                    "thread {} got different response size ({} vs {})",
                    idx, size, first_size
                );
            }
        } else {
            eprintln!("thread {} returned status {}", idx, status);
        }
    }

    assert_eq!(
        success_count, nthreads,
        "all {} concurrent decodes should succeed, only {} did",
        nthreads, success_count
    );

    // Verify server still responsive after concurrent load
    let c = client();
    let health = c
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("server should respond after concurrent decodes");
    assert_eq!(health.status().as_u16(), 200);
}

#[test]
fn transform_pipeline_memory() {
    // Request large image with region+size+rotation+quality transforms,
    // exercising the worst-case memory path (multiple intermediate buffers).
    // Verify server completes without crash.
    let srv = server();
    let c = client();

    // Full transform pipeline: region → size → rotation → quality
    // region: square crop from center, size: scale down, rotation: 90°, quality: default jpg
    let url = format!(
        "{}/unit/lena512.jp2/100,100,300,300/128,128/90/default.jpg",
        srv.base_url
    );

    let resp = c
        .get(&url)
        .send()
        .expect("transform pipeline request failed");

    assert_eq!(
        resp.status().as_u16(),
        200,
        "transform pipeline should succeed"
    );

    let body = resp.bytes().expect("read transform response body");
    assert!(!body.is_empty(), "transform response should not be empty");

    // Verify the JPEG is valid by checking magic bytes
    assert!(
        body.len() > 2 && body[0] == 0xFF && body[1] == 0xD8,
        "response should be a valid JPEG (starts with FF D8)"
    );

    // Now do a more aggressive transform: multiple transforms in sequence on same connection
    let transforms = [
        "0,0,256,256/64,64/0/default.jpg",
        "0,0,512,512/max/180/default.png",
        "256,256,256,256/128,/!0/default.jpg", // mirror
        "pct:10,10,80,80/256,256/270/default.jpg",
    ];

    for transform in &transforms {
        let url = format!("{}/unit/lena512.jp2/{}", srv.base_url, transform);
        let resp = c
            .get(&url)
            .send()
            .unwrap_or_else(|e| panic!("transform '{}' failed: {}", transform, e));
        let status = resp.status().as_u16();
        let _ = resp.bytes(); // consume body
        assert!(
            status == 200 || status == 400,
            "transform '{}' returned unexpected status {}",
            transform,
            status
        );
    }

    // Verify server still responsive after all transforms. If the musl
    // static binary hasn't recovered from the heavy transform work yet,
    // this fails and Bazel re-runs the whole test
    // (`--flaky_test_attempts` for this target in `.bazelrc`) — the single
    // retry mechanism for the suite.
    let health_url = format!("{}/unit/lena512.jp2/full/max/0/default.jpg", srv.base_url);
    let resp = c
        .get(&health_url)
        .send()
        .expect("health check request failed");
    assert_eq!(
        resp.status().as_u16(),
        200,
        "server not responsive after transform pipeline"
    );
}

/// S2-19: a multipart body with more than `MAX_MULTIPART_PARTS` (64) parts is
/// rejected with 400, independent of `max_post_size` — the cap on part count
/// guards against a request built from many tiny parts, which stays under the
/// byte-size limit while still costing a field-processing pass (and a temp
/// file, for file parts) per part.
#[test]
fn multipart_part_count_over_limit_answers_400() {
    let c = reqwest::blocking::Client::builder()
        .timeout(Duration::from_secs(30))
        .build()
        .expect("build client");

    let mut form = reqwest::blocking::multipart::Form::new();
    for i in 0..65 {
        form = form.text(format!("field{i}"), "x");
    }

    let resp = c
        .post(format!("{}/api/upload", server().base_url))
        .multipart(form)
        .send()
        .expect("65-part upload request failed");

    assert_eq!(
        resp.status().as_u16(),
        400,
        "a 65-part multipart body should be rejected before any part is processed"
    );
}

/// S2-19: a client that stops sending body bytes mid-request must not hold
/// the connection (and, on the Lua-route path, an eventual admission permit)
/// open indefinitely. `SIPI_BODY_READ_TIMEOUT=1` bounds the raw-body read in
/// `serve_lua_script`; a trickling client that goes silent for longer than
/// that must see the server cut the read — either by answering (whatever
/// status the body-read error maps to) or by closing/resetting the
/// connection. `SIPI_REQUEST_TIMEOUT=5` is a backstop far above the 1s body
/// timeout so the assertion below is exercising the body-read timeout, not
/// the handler wall-clock timeout.
///
/// Runs against its own empty cache dir for the same reason as
/// `handler_exceeding_request_timeout_answers_408`: this config's own
/// `./cache` is the literal this file's `server()` is concurrently using, and
/// a second process loading that shared directory deletes the first
/// process's live cache files as orphans.
#[test]
fn trickling_body_is_cut_off_by_body_read_timeout() {
    use std::io::{Read, Write};

    let cache_dir = tempfile::tempdir().expect("create isolated cache dir");
    let cache_dir_str = cache_dir.path().to_string_lossy().into_owned();
    let srv = SipiServer::start_env(
        "config/sipi.e2e-test-config.lua",
        &test_data_dir(),
        &[],
        &[
            ("SIPI_BODY_READ_TIMEOUT", "1"),
            ("SIPI_REQUEST_TIMEOUT", "5"),
            ("SIPI_CACHE_DIR", cache_dir_str.as_str()),
        ],
    );
    let port = srv.http_port;

    let mut sock = std::net::TcpStream::connect(("127.0.0.1", port)).expect("connect");
    sock.set_read_timeout(Some(Duration::from_secs(8)))
        .expect("set read timeout");
    sock.set_write_timeout(Some(Duration::from_secs(8)))
        .expect("set write timeout");

    write!(
        sock,
        "POST /api/upload HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/octet-stream\r\nContent-Length: 1000000\r\nConnection: close\r\n\r\n"
    )
    .expect("send request head");
    sock.write_all(b"trickle...").expect("send partial body");
    sock.flush().expect("flush partial body");

    // Go silent for longer than the 1s body-read timeout, then see whether the
    // server responded or cut the connection.
    std::thread::sleep(Duration::from_secs(2));

    let mut buf = [0u8; 512];
    match sock.read(&mut buf) {
        Ok(_) => {} // the server answered or closed cleanly — the read was cut off
        Err(e)
            if matches!(
                e.kind(),
                std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
            ) =>
        {
            // Our own 8s socket read timeout fired: the server never cut the
            // trickling read, i.e. SIPI_BODY_READ_TIMEOUT did not take effect.
            panic!("server did not cut off the trickling body read within the socket read timeout");
        }
        Err(_) => {} // connection reset by the server — also proves the read was cut off
    }
}
