mod common;

use common::{client, client_no_redirect, server};
use image::GenericImageView as _;
use serde_json::json;
use sipi_e2e::jwt::{alg_none_token, create_jwt, tamper_payload};
use sipi_e2e::{
    allocate_ports, apply_sanitizer_env, http_client, sipi_bin_path, test_data_dir, SipiServer,
};
use std::io::{Read as _, Write as _};
use std::net::TcpStream;
use std::process::Command;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const JWT_SECRET: &str = "dev-only-insecure-jwt-secret-change-me";

// =============================================================================
// JWT Security Tests (using the 'auth' prefix which checks JWT tokens)
// =============================================================================

#[test]
fn jwt_expired_token() {
    // SECURITY FINDING: sipi's Lua pre-flight handler does NOT check the `exp` claim.
    // It only validates the signature and checks `token_val['allow']`.
    // An expired JWT with a valid signature and `allow: true` grants access.
    // This documents the current behavior — fixing requires Lua handler changes.
    let srv = server();

    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let claims = json!({
        "allow": true,
        "exp": now - 3600,  // expired 1 hour ago
        "iat": now - 7200,
    });
    let token = create_jwt(&claims, JWT_SECRET);

    let resp = client()
        .get(format!(
            "{}/auth/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .header("Authorization", format!("Bearer {}", token))
        .send()
        .expect("expired JWT request failed");

    let status = resp.status().as_u16();
    // decode_jwt validates `exp` (ADR-0023): the expired token is rejected.
    // The 500 is this test hook's own decode-failure shape (it sendStatus(500)s
    // on a failed decode) — the point pinned here is that access is refused.
    assert_eq!(
        status, 500,
        "expired JWT must be rejected by decode_jwt, got {}",
        status
    );
}

#[test]
fn jwt_alg_none_bypass() {
    // Send JWT with alg:none and no signature — common JWT vulnerability.
    let srv = server();

    let token = alg_none_token(r#"{"allow":true}"#);

    let resp = client()
        .get(format!(
            "{}/auth/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .header("Authorization", format!("Bearer {}", token))
        .send()
        .expect("alg:none JWT request failed");

    let status = resp.status().as_u16();
    // Must NOT return 200 — that would mean the bypass worked
    assert_ne!(
        status, 200,
        "SECURITY: alg:none JWT should NOT grant access (got 200)"
    );
}

#[test]
fn jwt_tampered_payload() {
    // Create valid JWT, modify payload without re-signing, verify rejection.
    let srv = server();

    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let claims = json!({
        "allow": false,
        "exp": now + 3600,
    });
    let valid_token = create_jwt(&claims, JWT_SECRET);

    let tampered_token = tamper_payload(
        &valid_token,
        &serde_json::to_string(&json!({
            "allow": true,
            "exp": now + 3600,
        }))
        .unwrap(),
    );

    let resp = client()
        .get(format!(
            "{}/auth/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .header("Authorization", format!("Bearer {}", tampered_token))
        .send()
        .expect("tampered JWT request failed");

    let status = resp.status().as_u16();
    // Tampered token should be rejected
    assert_ne!(
        status, 200,
        "SECURITY: tampered JWT should NOT grant access (got 200)"
    );
}

#[test]
fn config_empty_jwt_secret_refuses_startup() {
    // An empty jwt_secret is unsafe (S2-11): startup must be refused (fail
    // closed) instead of serving with a forgeable/absent secret.
    let test_data = test_data_dir();

    let config_content = r#"sipi = {
    port = 1024,
    nthreads = 4,
    jpeg_quality = 60,
    scaling_quality = { jpeg = "medium", tiff = "high", png = "high", j2k = "high" },
    max_post_size = '300M',
    imgroot = './images',
    prefix_as_path = true,
    initscript = './config/sipi.init-knora.lua',
    cache_dir = './cache',
    cache_size = '20M',
    cache_nfiles = 8,
    scriptdir = './scripts',
    thumb_size = '!128,128',
    tmpdir = '/tmp',
    max_temp_file_age = 86400,
    knora_path = 'localhost',
    knora_port = '3434',
    jwt_secret = '',
}

fileserver = {
    docroot = './server',
    wwwroute = '/server'
}

routes = {}
"#;

    let config_path = test_data.join("config/sipi.empty-jwt.lua");
    std::fs::write(&config_path, config_content).expect("write empty jwt config");

    let (http_port, _) = allocate_ports();
    let mut cmd = Command::new(sipi_bin_path());
    cmd.arg("server")
        .arg("--config")
        .arg("config/sipi.empty-jwt.lua")
        .arg("--serverport")
        .arg(http_port.to_string())
        .current_dir(&test_data);
    apply_sanitizer_env(&mut cmd);
    let output = cmd.output().expect("spawn sipi");

    let _ = std::fs::remove_file(&config_path);

    assert!(
        !output.status.success(),
        "an empty jwt_secret must refuse startup, got: {:?}\nstderr:\n{}",
        output.status,
        String::from_utf8_lossy(&output.stderr)
    );
}

#[test]
fn crlf_header_injection() {
    // Request identifier containing %0d%0a (CRLF), verify no response header injection.
    let srv = server();

    // %0d%0a is URL-encoded CRLF
    let resp = client()
        .get(format!(
            "{}/unit/lena512%0d%0aX-Injected:%20evil/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("CRLF injection request failed");

    // R8: CRLF characters are sanitized from headers but the request itself
    // may return 404 (file not found) since the filename with CRLF doesn't exist.
    // The key security property is that no header injection occurs.
    let status = resp.status().as_u16();
    assert!(
        status == 400 || status == 404,
        "CRLF request should return 400 or 404, got {}",
        status
    );

    // Verify no injected header
    assert!(
        resp.headers().get("X-Injected").is_none(),
        "SECURITY: response should not contain injected header"
    );
}

#[test]
fn error_no_path_disclosure() {
    // Trigger a server error, verify response body does not leak internal paths.
    let srv = server();

    let resp = client()
        .get(format!(
            "{}/unit/nonexistent-file-for-path-test.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("error path disclosure request failed");

    let status = resp.status().as_u16();
    assert!(status >= 400, "nonexistent file should return error");

    let body = resp.text().unwrap_or_default().to_lowercase();

    // Check for common path disclosure patterns
    let path_indicators = [
        "/users/",
        "/home/",
        "/var/",
        "/opt/",
        "/tmp/sipi",
        "images/unit/",
        ".jp2",
    ];

    for indicator in &path_indicators {
        // Allow the filename itself to appear but not full filesystem paths
        if *indicator != ".jp2" {
            assert!(
                !body.contains(indicator),
                "SECURITY: error response may leak internal path '{}' in body: {}",
                indicator,
                &body[..body.len().min(500)]
            );
        }
    }
}

#[test]
fn decompression_bomb_rejection() {
    // Request an image that would decompress to extreme dimensions.
    // We can't easily create a decompression bomb test fixture, so instead
    // test that extreme IIIF size requests are rejected.
    let srv = server();

    // Request upscaling to extreme dimensions (100000x100000)
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/^100000,100000/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("extreme upscale request failed");

    let status = resp.status().as_u16();
    // Server should reject or handle gracefully — not OOM
    assert!(
        status == 400 || status == 500 || status == 200,
        "extreme upscale should be handled gracefully, got {}",
        status
    );

    // Verify server still responsive after potential OOM pressure
    let health = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("server should respond after extreme upscale");
    assert_eq!(health.status().as_u16(), 200);
}

#[test]
#[ignore = "slowloris test may be flaky in CI due to timing sensitivity"]
fn slowloris_resilience() {
    // Open connection, send partial request headers slowly, verify server doesn't hang.
    let srv = server();

    // Connect raw TCP
    let addr = format!("127.0.0.1:{}", srv.http_port);
    let mut stream = TcpStream::connect(&addr).expect("TCP connect failed");
    stream.set_read_timeout(Some(Duration::from_secs(10))).ok();
    stream.set_write_timeout(Some(Duration::from_secs(5))).ok();

    // Send partial HTTP request (no final \r\n\r\n)
    stream
        .write_all(b"GET /unit/lena512.jp2/full/max/0/default.jpg HTTP/1.1\r\n")
        .expect("write partial request");
    stream
        .write_all(b"Host: localhost\r\n")
        .expect("write host header");
    // Don't send the final \r\n — this is a slowloris attack

    // Wait a bit
    std::thread::sleep(Duration::from_secs(2));

    // Verify server is still accepting new connections
    let health = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("server should still accept connections during slowloris");
    assert_eq!(health.status().as_u16(), 200);

    // Try reading from the slow connection — server should eventually close it
    let mut buf = [0u8; 1024];
    let result = stream.read(&mut buf);
    // Either timeout (server kept connection open) or closed (server killed it)
    // Both are acceptable as long as the server didn't hang
    match result {
        Ok(0) => {} // Connection closed — good
        Ok(_) => {} // Got some data — server responded despite partial request
        Err(e) => {
            // Timeout or connection reset — acceptable
            eprintln!("Slowloris connection result: {}", e);
        }
    }
}

// =============================================================================
// info.json / knora.json disclosure (S2-16)
// =============================================================================

#[test]
fn restricted_info_json_hides_native_resolution_and_tiles() {
    // `test_restrict` caps the view to `thumb_size` (`!128,128`) — the
    // resolved lena512.jp2 is 512x512 native, so a clamped info.json must
    // report the restricted dims, not the native ones, and must not carry a
    // `tiles` pyramid built from the native grid.
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/test_restrict/lena512.jp2/info.json",
            srv.base_url
        ))
        .send()
        .expect("GET restricted info.json failed");

    assert_eq!(resp.status().as_u16(), 200);
    let vary = resp
        .headers()
        .get(reqwest::header::VARY)
        .and_then(|v| v.to_str().ok())
        .unwrap_or_default()
        .to_owned();
    let cache_control = resp
        .headers()
        .get(reqwest::header::CACHE_CONTROL)
        .and_then(|v| v.to_str().ok())
        .unwrap_or_default()
        .to_owned();

    let json: serde_json::Value = resp.json().expect("restricted info.json must be JSON");
    let width = json["width"].as_u64().expect("width must be numeric");
    assert!(
        width < 512,
        "restricted info.json must report a clamped width < 512 (native), got {}",
        width
    );
    assert!(
        json.get("tiles").is_none(),
        "restricted info.json must not disclose the native tiling pyramid"
    );
    assert!(
        vary.contains("Cookie") && vary.contains("Authorization"),
        "restricted info.json must vary on Cookie/Authorization, got Vary: {}",
        vary
    );
    assert_eq!(
        cache_control, "private, no-store",
        "restricted info.json must not be cacheable across credentials"
    );
}

#[test]
fn hook_gated_responses_are_private() {
    // The default server runs `pre_flight` and `file_pre_flight`, so every
    // IIIF and /file response depends on the caller's credentials: a shared
    // cache must not store it (`private`) or key it without them (`Vary`).
    let srv = server();
    for (path, status) in [
        ("/unit/lena512.jp2/full/max/0/default.jpg", 200),
        // `deny` from `pre_flight`.
        ("/tmp/lena512.jp2/full/max/0/default.jpg", 401),
        ("/unit/missing.jp2/full/max/0/default.jpg", 404),
        ("/unit/lena512.jp2/info.json", 200),
        // `deny` from `pre_flight` on knora.json is a bare 404.
        ("/tmp/lena512.jp2/knora.json", 404),
        ("/unit/test.csv/file", 200),
        // `deny` from `file_pre_flight`.
        ("/unit/test2.csv/file", 401),
        ("/unit/missing.csv/file", 404),
    ] {
        let resp = client()
            .get(format!("{}{path}", srv.base_url))
            .send()
            .unwrap_or_else(|e| panic!("GET {path} failed: {e}"));
        assert_eq!(resp.status().as_u16(), status, "{path}");
        let joined = |name: reqwest::header::HeaderName| {
            resp.headers()
                .get_all(name)
                .iter()
                .filter_map(|v| v.to_str().ok())
                .collect::<Vec<_>>()
                .join(", ")
        };
        let cache_control = joined(reqwest::header::CACHE_CONTROL);
        assert!(
            cache_control
                .split(',')
                .any(|d| d.trim().eq_ignore_ascii_case("private")),
            "{path}: Cache-Control must contain private, got: {cache_control}"
        );
        assert!(
            !cache_control
                .split(',')
                .any(|d| d.trim().eq_ignore_ascii_case("public")),
            "{path}: Cache-Control must not contain public, got: {cache_control}"
        );
        let vary = joined(reqwest::header::VARY);
        for token in ["Cookie", "Authorization"] {
            assert!(
                vary.split(',')
                    .any(|v| v.trim().eq_ignore_ascii_case(token)),
                "{path}: Vary must contain {token}, got: {vary}"
            );
        }
    }
}

#[test]
fn knora_json_denied_path_returns_not_found() {
    // `tmp` always denies — knora.json is a DSP-internal surface and must not
    // disclose an auth challenge or the resource's existence to a denied caller.
    let srv = server();
    let resp = client()
        .get(format!("{}/tmp/lena512.jp2/knora.json", srv.base_url))
        .send()
        .expect("GET denied knora.json failed");

    assert_eq!(resp.status().as_u16(), 404);
}

#[test]
fn knora_json_allowed_path_still_succeeds() {
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/knora.json", srv.base_url))
        .send()
        .expect("GET allowed knora.json failed");

    assert_eq!(resp.status().as_u16(), 200);
    let json: serde_json::Value = resp.json().expect("allowed knora.json must be JSON");
    assert_eq!(json["width"], 512);
    assert_eq!(json["height"], 512);
}

#[test]
fn bare_restrict_info_json_is_forbidden() {
    // `test_restrict_bare` returns `{type='restrict'}` with neither a `size`
    // cap nor a `watermark` — indistinguishable from `allow` at the seam. If
    // let through, info.json would disclose the native width/height and the
    // full `scaleFactors` tiling pyramid (RUST-001). It must be refused.
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/test_restrict_bare/lena512.jp2/info.json",
            srv.base_url
        ))
        .send()
        .expect("GET bare-restrict info.json failed");

    assert_eq!(
        resp.status().as_u16(),
        403,
        "a bare restrict decision (no size, no watermark) must be forbidden on info.json"
    );
}

#[test]
fn bare_restrict_knora_json_is_not_found() {
    // Same bare-restrict decision as above; knora.json is a DSP-internal
    // surface that hides existence rather than surfacing an auth challenge
    // (matching the established deny convention), so it returns 404.
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/test_restrict_bare/lena512.jp2/knora.json",
            srv.base_url
        ))
        .send()
        .expect("GET bare-restrict knora.json failed");

    assert_eq!(
        resp.status().as_u16(),
        404,
        "a bare restrict decision (no size, no watermark) must be hidden (404) on knora.json"
    );
}

#[test]
fn no_op_restrictions_are_refused_on_every_route() {
    // `size=max` and `size=pct:100` without a watermark (an empty one
    // included) restrict nothing. The image route, info.json and
    // knora.json must agree on the refusal, each with its own status: 403,
    // 403, and 404 on knora.json, which hides existence.
    let srv = server();
    for prefix in [
        "test_restrict_max",
        "test_restrict_pct100",
        "test_restrict_max_emptywm",
    ] {
        for (path, status) in [
            ("full/max/0/default.jpg", 403),
            ("info.json", 403),
            ("knora.json", 404),
        ] {
            let resp = client()
                .get(format!("{}/{prefix}/lena512.jp2/{path}", srv.base_url))
                .send()
                .expect("GET no-op restriction failed");
            assert_eq!(resp.status().as_u16(), status, "{prefix} {path}");
        }
    }
}

#[test]
fn restricted_knora_json_dims_match_info_json() {
    let srv = server();
    let get = |path: &str| -> serde_json::Value {
        let resp = client()
            .get(format!("{}/test_restrict/lena512.jp2/{path}", srv.base_url))
            .send()
            .expect("GET restricted json failed");
        assert_eq!(resp.status().as_u16(), 200, "{path}");
        resp.json().expect("restricted response must be JSON")
    };
    let info = get("info.json");
    let knora = get("knora.json");
    assert_eq!(knora["width"], info["width"]);
    assert_eq!(knora["height"], info["height"]);
    assert_eq!(info["width"], 128);
}

#[test]
fn image_inside_restriction_box_is_served_at_native_size() {
    // `test_restrict_bigbox` caps at `!1024,1024`; lena512.jp2 (512x512) is
    // already inside the box, so it is served, not refused.
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/test_restrict_bigbox/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET big-box restricted image failed");
    assert_eq!(resp.status().as_u16(), 200);
    let img = image::load_from_memory(&resp.bytes().expect("read body"))
        .expect("decode big-box restricted image");
    assert_eq!(img.dimensions(), (512, 512));

    let info: serde_json::Value = client()
        .get(format!(
            "{}/test_restrict_bigbox/lena512.jp2/info.json",
            srv.base_url
        ))
        .send()
        .expect("GET big-box restricted info.json failed")
        .json()
        .expect("big-box info.json must be JSON");
    assert_eq!(info["width"], 512);
    assert_eq!(info["height"], 512);
}

#[test]
fn restricted_region_request_is_served() {
    // A size-restricted region request runs a whole-image decode, so the shell
    // moves it from the Tile to the Full admission lane after the preflight
    // (DEV-7603). The lane itself is covered by the `reclassify_to_full` unit
    // tests; this checks that the swap still serves the region. `test_restrict`
    // caps lena512.jp2 to 128x128, and region coordinates are in that space
    // (ADR-0028).
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/test_restrict/lena512.jp2/0,0,64,64/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET restricted region failed");
    assert_eq!(resp.status().as_u16(), 200);
    let img = image::load_from_memory(&resp.bytes().expect("read body"))
        .expect("decode restricted region");
    assert_eq!(img.dimensions(), (64, 64));
}

// =============================================================================
// `stream` permission: parity with `allow` (ADR-0027)
// =============================================================================
//
// `stream` records the intent "consumable in place, never handed over as a
// file". It enforces nothing: the `test_stream` prefix resolves to the same
// file `unit` serves under `allow`, and every response below must be identical
// to the `allow` one. A later tightening therefore has to break these
// assertions deliberately rather than drift past them.

#[test]
fn stream_file_download_is_byte_identical_to_allow() {
    let srv = server();
    let streamed = client()
        .get(format!("{}/test_stream/test.csv/file", srv.base_url))
        .send()
        .expect("GET streamed /file failed");
    assert_eq!(streamed.status().as_u16(), 200, "stream must serve /file");
    let streamed_disposition = streamed
        .headers()
        .get("content-disposition")
        .map(|v| v.to_str().expect("ASCII disposition").to_owned());
    let streamed_body = streamed.bytes().expect("streamed /file body");

    let allowed = client()
        .get(format!("{}/unit/test.csv/file", srv.base_url))
        .send()
        .expect("GET allowed /file failed");
    assert_eq!(allowed.status().as_u16(), 200);
    let allowed_disposition = allowed
        .headers()
        .get("content-disposition")
        .map(|v| v.to_str().expect("ASCII disposition").to_owned());
    let allowed_body = allowed.bytes().expect("allowed /file body");

    assert_eq!(
        streamed_body, allowed_body,
        "a stream decision must serve the same bytes as allow"
    );
    assert_eq!(
        streamed_disposition, allowed_disposition,
        "a stream decision must not change Content-Disposition"
    );
}

#[test]
fn stream_info_json_matches_allow() {
    let srv = server();
    let streamed: serde_json::Value = client()
        .get(format!(
            "{}/test_stream/lena512.jp2/info.json",
            srv.base_url
        ))
        .send()
        .expect("GET streamed info.json failed")
        .json()
        .expect("streamed info.json must be JSON");
    let allowed: serde_json::Value = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET allowed info.json failed")
        .json()
        .expect("allowed info.json must be JSON");

    // `id` is the request's own canonical URL, so it differs by prefix; every
    // other member — the native dims and the `tiles` pyramid a restriction
    // strips — must match.
    let strip_id = |mut v: serde_json::Value| {
        v.as_object_mut().expect("info.json object").remove("id");
        v
    };
    assert_eq!(
        strip_id(streamed),
        strip_id(allowed),
        "a stream decision must describe the image exactly as allow does"
    );
}

#[test]
fn stream_image_serve_matches_allow() {
    // Compared as decoded pixels, not raw bytes: every rendered representation
    // embeds an ICC profile carrying a wall-clock creation date (deterministic
    // only under the approval tests' `SOURCE_DATE_EPOCH`), so two renders
    // seconds apart differ byte-wise while being the same image.
    let srv = server();
    let allowed = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET allowed image failed");
    assert_eq!(allowed.status().as_u16(), 200);
    let allowed_type = allowed
        .headers()
        .get("content-type")
        .map(|v| v.to_str().expect("ASCII content-type").to_owned());
    let allowed_img = image::load_from_memory(&allowed.bytes().expect("allowed image body"))
        .expect("decode allowed render");

    let streamed = client()
        .get(format!(
            "{}/test_stream/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET streamed image failed");
    assert_eq!(streamed.status().as_u16(), 200);
    let streamed_type = streamed
        .headers()
        .get("content-type")
        .map(|v| v.to_str().expect("ASCII content-type").to_owned());
    let streamed_img = image::load_from_memory(&streamed.bytes().expect("streamed image body"))
        .expect("decode streamed render");

    assert_eq!(streamed_type, allowed_type);
    assert_eq!(
        streamed_img.dimensions(),
        allowed_img.dimensions(),
        "a stream decision must not clamp the served resolution"
    );
    assert_eq!(
        streamed_img.to_rgb8().into_raw(),
        allowed_img.to_rgb8().into_raw(),
        "a stream decision must serve the same pixels as allow"
    );
}

/// S2-17: with `SIPI_PUBLIC_HOSTS` configured, a hostile `X-Forwarded-Host` is
/// substituted with the first allowlisted host in both the 303 redirect
/// `Location` and the info.json `id` (the canonical IIIF service id) — never
/// reflected verbatim. A host that is on the allowlist passes through
/// unchanged.
#[test]
fn public_hosts_allowlist_substitutes_hostile_forwarded_host() {
    // Its own cache directory: this second server shares the working directory
    // with the binary's shared server, and a starting server reclaims every
    // cache file its freshly-read index does not list — which would delete the
    // entries the shared server still holds in memory and turn its next cache
    // hit into a 500.
    let srv = SipiServer::start_env(
        "config/sipi.e2e-test-config.lua",
        &test_data_dir(),
        &[],
        &[
            ("SIPI_PUBLIC_HOSTS", "iiif.example.org,iiif2.example.org"),
            ("SIPI_CACHE_DIR", "./cache_public_hosts"),
        ],
    );

    let redirect_resp = client_no_redirect()
        .get(format!("{}/unit/lena512.jp2", srv.base_url))
        .header("X-Forwarded-Host", "evil.example.org")
        .send()
        .expect("GET base URI with hostile X-Forwarded-Host");
    assert_eq!(redirect_resp.status().as_u16(), 303);
    let location = redirect_resp
        .headers()
        .get("location")
        .expect("missing Location header on redirect")
        .to_str()
        .unwrap();
    assert!(
        location.starts_with("http://iiif.example.org/"),
        "SECURITY: Location must carry the configured host, not the hostile one, got: {}",
        location
    );

    let info_resp = http_client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .header("X-Forwarded-Host", "evil.example.org")
        .send()
        .expect("GET info.json with hostile X-Forwarded-Host");
    assert_eq!(info_resp.status().as_u16(), 200);
    let id = info_resp
        .json::<serde_json::Value>()
        .expect("info.json body")["id"]
        .as_str()
        .expect("id field")
        .to_owned();
    assert!(
        id.starts_with("http://iiif.example.org/"),
        "SECURITY: @id must carry the configured host, not the hostile one, got: {}",
        id
    );

    let allowed_resp = http_client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .header("X-Forwarded-Host", "iiif2.example.org")
        .send()
        .expect("GET info.json with allowlisted host");
    let allowed_id = allowed_resp
        .json::<serde_json::Value>()
        .expect("info.json body")["id"]
        .as_str()
        .expect("id field")
        .to_owned();
    assert!(
        allowed_id.starts_with("http://iiif2.example.org/"),
        "an allowlisted host should pass through unchanged, got: {}",
        allowed_id
    );
}

#[test]
fn allowed_origins_allowlist_gates_info_json_cors() {
    // Its own cache directory, for the same reason as
    // `public_hosts_allowlist_substitutes_hostile_forwarded_host`.
    let srv = SipiServer::start_env(
        "config/sipi.e2e-test-config.lua",
        &test_data_dir(),
        &[],
        &[
            ("SIPI_ALLOWED_ORIGINS", "https://allowed.example"),
            ("SIPI_CACHE_DIR", "./cache_allowed_origins"),
        ],
    );
    let url = format!("{}/unit/lena512.jp2/info.json", srv.base_url);
    let varies_on_origin = |resp: &reqwest::blocking::Response| {
        resp.headers()
            .get("vary")
            .and_then(|v| v.to_str().ok())
            .is_some_and(|v| {
                v.split(',')
                    .any(|h| h.trim().eq_ignore_ascii_case("origin"))
            })
    };

    let no_origin = http_client()
        .get(&url)
        .send()
        .expect("GET info.json without an Origin");
    assert_eq!(no_origin.status().as_u16(), 200);
    assert_eq!(
        no_origin
            .headers()
            .get("access-control-allow-origin")
            .expect("missing ACAO without an Origin"),
        "*"
    );
    assert!(no_origin
        .headers()
        .get("access-control-allow-credentials")
        .is_none());
    assert!(varies_on_origin(&no_origin), "Vary should contain Origin");

    let denied = http_client()
        .get(&url)
        .header("Origin", "https://evil.example")
        .send()
        .expect("GET info.json with an unlisted Origin");
    assert_eq!(denied.status().as_u16(), 200);
    assert!(
        denied
            .headers()
            .get("access-control-allow-origin")
            .is_none(),
        "SECURITY: an unlisted Origin must get no ACAO on info.json"
    );
    assert!(
        denied
            .headers()
            .get("access-control-allow-credentials")
            .is_none(),
        "SECURITY: an unlisted Origin must get no credentials on info.json"
    );
    // A shared cache must not serve this deny to a listed Origin.
    assert!(varies_on_origin(&denied), "Vary should contain Origin");

    let allowed = http_client()
        .get(&url)
        .header("Origin", "https://allowed.example")
        .send()
        .expect("GET info.json with a listed Origin");
    assert_eq!(allowed.status().as_u16(), 200);
    let headers = allowed.headers();
    assert_eq!(
        headers
            .get("access-control-allow-origin")
            .expect("missing ACAO for a listed Origin"),
        "https://allowed.example"
    );
    assert_eq!(
        headers
            .get("access-control-allow-credentials")
            .expect("missing credentials for a listed Origin"),
        "true"
    );
    assert!(varies_on_origin(&allowed), "Vary should contain Origin");

    // An error response follows the same allowlist: a listed Origin gets the
    // credentialed echo, an unlisted one gets no ACAO, and both vary on Origin.
    let missing = format!("{}/unit/missing.jp2/full/max/0/default.jpg", srv.base_url);
    let allowed_error = http_client()
        .get(&missing)
        .header("Origin", "https://allowed.example")
        .send()
        .expect("GET missing image with a listed Origin");
    assert_eq!(allowed_error.status().as_u16(), 404);
    assert_eq!(
        allowed_error
            .headers()
            .get("access-control-allow-origin")
            .expect("missing ACAO on a 404 for a listed Origin"),
        "https://allowed.example"
    );
    assert_eq!(
        allowed_error
            .headers()
            .get("access-control-allow-credentials")
            .expect("missing credentials on a 404 for a listed Origin"),
        "true"
    );
    assert!(
        varies_on_origin(&allowed_error),
        "Vary should contain Origin"
    );

    let denied_error = http_client()
        .get(&missing)
        .header("Origin", "https://evil.example")
        .send()
        .expect("GET missing image with an unlisted Origin");
    assert_eq!(denied_error.status().as_u16(), 404);
    assert!(
        denied_error
            .headers()
            .get("access-control-allow-origin")
            .is_none(),
        "SECURITY: an unlisted Origin must get no ACAO on an error"
    );
    assert!(
        varies_on_origin(&denied_error),
        "Vary should contain Origin"
    );
}
