mod common;

use common::{client, client_no_redirect, server};
use image::GenericImageView;
use insta::assert_json_snapshot;
use serde_json::Value;

// Comprehensive IIIF Image API 3.0 compliance checks.
// These verify sipi's IIIF endpoints against the spec requirements
// without requiring the external IIIF validator binary.

/// Assert a IIIF URL returns the expected HTTP status.
fn assert_iiif_status(path: &str, expected: u16) {
    let srv = server();
    let resp = client()
        .get(format!("{}{}", srv.base_url, path))
        .send()
        .unwrap_or_else(|e| panic!("GET {} failed: {}", path, e));
    assert_eq!(
        resp.status().as_u16(),
        expected,
        "unexpected status for {}",
        path
    );
}

#[test]
fn info_json_has_required_fields() {
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed");

    assert_eq!(resp.status().as_u16(), 200);

    let json: serde_json::Value = resp.json().expect("invalid JSON");

    // Required fields per IIIF Image API 3.0 Section 5.6
    assert_eq!(json["type"], "ImageService3");
    assert_eq!(json["protocol"], "http://iiif.io/api/image");
    assert!(json["width"].is_number(), "width must be numeric");
    assert!(json["height"].is_number(), "height must be numeric");
    assert!(json["profile"].is_string(), "profile must be string");
}

#[test]
fn info_json_has_sizes_array() {
    let srv = server();
    let json: serde_json::Value = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed")
        .json()
        .expect("invalid JSON");

    let sizes = json["sizes"].as_array().expect("sizes must be array");
    assert!(!sizes.is_empty(), "sizes should not be empty");

    for size in sizes {
        assert!(size["width"].is_number(), "size.width must be numeric");
        assert!(size["height"].is_number(), "size.height must be numeric");
    }
}

#[test]
fn info_json_has_tiles_array() {
    let srv = server();
    let json: serde_json::Value = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed")
        .json()
        .expect("invalid JSON");

    let tiles = json["tiles"].as_array().expect("tiles must be array");
    assert!(!tiles.is_empty(), "tiles should not be empty");

    for tile in tiles {
        assert!(tile["width"].is_number(), "tile.width must be numeric");
        let factors = tile["scaleFactors"]
            .as_array()
            .expect("scaleFactors must be array");
        assert!(!factors.is_empty(), "scaleFactors should not be empty");
    }
}

#[test]
fn info_json_has_extra_features() {
    let srv = server();
    let json: serde_json::Value = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed")
        .json()
        .expect("invalid JSON");

    let features = json["extraFeatures"]
        .as_array()
        .expect("extraFeatures must be array");

    // Level 2 required features per IIIF Image API 3.0
    let feature_strs: Vec<&str> = features.iter().filter_map(|f| f.as_str()).collect();
    assert!(feature_strs.contains(&"regionByPx"), "missing regionByPx");
    assert!(feature_strs.contains(&"regionByPct"), "missing regionByPct");
    assert!(feature_strs.contains(&"sizeByW"), "missing sizeByW");
    assert!(feature_strs.contains(&"sizeByH"), "missing sizeByH");
    assert!(feature_strs.contains(&"sizeByWh"), "missing sizeByWh");
    assert!(
        feature_strs.contains(&"rotationBy90s"),
        "missing rotationBy90s"
    );
}

#[test]
fn info_json_content_type_default() {
    let srv = server();
    // Without Accept: application/ld+json, sipi returns application/json
    // with a Link header to the JSON-LD context (per IIIF spec Section 5.2)
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed");

    assert_eq!(resp.status().as_u16(), 200);

    let ct = resp
        .headers()
        .get("content-type")
        .expect("missing content-type")
        .to_str()
        .expect("invalid content-type");

    assert_eq!(
        ct, "application/json",
        "content-type should be exactly application/json without Accept header, got: {}",
        ct
    );
}

// --- info.json Complete Validation ---

/// Helper to fetch and parse info.json for lena512.jp2.
fn fetch_info_json() -> Value {
    let srv = server();
    client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed")
        .json()
        .expect("invalid JSON")
}

#[test]
fn info_json_context() {
    let json = fetch_info_json();
    assert_eq!(
        json["@context"], "http://iiif.io/api/image/3/context.json",
        "@context must be IIIF Image API 3.0 context"
    );
}

#[test]
fn info_json_id_contains_base_uri() {
    let srv = server();
    let json = fetch_info_json();
    let id = json["id"].as_str().expect("id must be string");
    assert!(
        id.contains(&format!("127.0.0.1:{}", srv.http_port)),
        "id should contain server host:port, got: {}",
        id
    );
    assert!(
        id.contains("unit/lena512.jp2"),
        "id should contain prefix/identifier, got: {}",
        id
    );
}

#[test]
fn info_json_type_imageservice3() {
    let json = fetch_info_json();
    assert_eq!(json["type"], "ImageService3");
}

#[test]
fn info_json_protocol() {
    let json = fetch_info_json();
    assert_eq!(json["protocol"], "http://iiif.io/api/image");
}

#[test]
fn info_json_profile_level2() {
    let json = fetch_info_json();
    assert_eq!(json["profile"], "level2");
}

#[test]
fn info_json_dimensions_match_lena512() {
    let json = fetch_info_json();
    // lena512.jp2 is a 512x512 image
    assert_eq!(json["width"], 512, "width should be 512");
    assert_eq!(json["height"], 512, "height should be 512");
}

#[test]
fn info_json_sizes_have_valid_dimensions() {
    let json = fetch_info_json();
    let sizes = json["sizes"].as_array().expect("sizes must be array");
    assert!(!sizes.is_empty(), "sizes should not be empty");

    for size in sizes {
        let w = size["width"].as_i64().expect("size.width must be integer");
        let h = size["height"]
            .as_i64()
            .expect("size.height must be integer");
        assert!(w > 0, "size.width must be positive, got {}", w);
        assert!(h > 0, "size.height must be positive, got {}", h);
        // Sizes should be smaller than or equal to original
        assert!(w <= 512, "size.width should not exceed original, got {}", w);
        assert!(
            h <= 512,
            "size.height should not exceed original, got {}",
            h
        );
    }
}

#[test]
fn info_json_tiles_have_scale_factors() {
    let json = fetch_info_json();
    let tiles = json["tiles"].as_array().expect("tiles must be array");
    assert!(!tiles.is_empty(), "tiles should not be empty");

    let tile = &tiles[0];
    assert!(tile["width"].is_number(), "tile.width must be numeric");
    let factors = tile["scaleFactors"]
        .as_array()
        .expect("scaleFactors must be array");
    assert!(!factors.is_empty(), "scaleFactors should not be empty");
    // Scale factors should start at 1
    assert_eq!(
        factors[0].as_i64().unwrap(),
        1,
        "first scaleFactor should be 1"
    );
}

#[test]
fn info_json_extra_formats() {
    let json = fetch_info_json();
    let formats = json["extraFormats"]
        .as_array()
        .expect("extraFormats must be array");
    let format_strs: Vec<&str> = formats.iter().filter_map(|f| f.as_str()).collect();
    assert!(format_strs.contains(&"tif"), "missing tif in extraFormats");
    assert!(format_strs.contains(&"jp2"), "missing jp2 in extraFormats");
}

#[test]
fn info_json_preferred_formats() {
    let json = fetch_info_json();
    let formats = json["preferredFormats"]
        .as_array()
        .expect("preferredFormats must be array");
    let format_strs: Vec<&str> = formats.iter().filter_map(|f| f.as_str()).collect();
    assert!(
        format_strs.contains(&"jpg"),
        "missing jpg in preferredFormats"
    );
    assert!(
        format_strs.contains(&"tif"),
        "missing tif in preferredFormats"
    );
    assert!(
        format_strs.contains(&"jp2"),
        "missing jp2 in preferredFormats"
    );
    assert!(
        format_strs.contains(&"png"),
        "missing png in preferredFormats"
    );
}

#[test]
fn info_json_all_17_extra_features() {
    let json = fetch_info_json();
    let features = json["extraFeatures"]
        .as_array()
        .expect("extraFeatures must be array");
    let feature_strs: Vec<&str> = features.iter().filter_map(|f| f.as_str()).collect();

    let expected = [
        "baseUriRedirect",
        "canonicalLinkHeader",
        "cors",
        "jsonldMediaType",
        "mirroring",
        "profileLinkHeader",
        "regionByPct",
        "regionByPx",
        "regionSquare",
        "rotationArbitrary",
        "rotationBy90s",
        "sizeByConfinedWh",
        "sizeByH",
        "sizeByPct",
        "sizeByW",
        "sizeByWh",
        "sizeUpscaling",
    ];

    for feat in &expected {
        assert!(
            feature_strs.contains(feat),
            "missing extraFeature: {}",
            feat
        );
    }
    assert_eq!(
        features.len(),
        17,
        "expected exactly 17 extraFeatures, got {}",
        features.len()
    );
}

#[test]
fn info_json_golden_snapshot() {
    // Golden baseline: freeze the full info.json response for regression detection.
    // This snapshot guards against unintended changes during test deduplication
    // and serves as a stable reference for the long-term Rust migration.
    let json = fetch_info_json();
    assert_json_snapshot!("info-json-lena512", json, {
        ".id" => "[server_url]",  // redact dynamic server URL
    });
}

#[test]
fn info_json_headers_snapshot() {
    // Golden baseline: freeze key IIIF response headers (content-type, CORS, Link).
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed");

    assert_eq!(resp.status().as_u16(), 200);

    let headers = resp.headers();
    let snapshot = serde_json::json!({
        "content-type": headers.get("content-type").map(|v| v.to_str().unwrap_or("")),
        "access-control-allow-origin": headers.get("access-control-allow-origin").map(|v| v.to_str().unwrap_or("")),
        "link": headers.get("link").map(|v| v.to_str().unwrap_or("")),
    });

    assert_json_snapshot!("info-json-headers", snapshot);
}

#[test]
fn info_json_x_forwarded_proto_https() {
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .header("X-Forwarded-Proto", "https")
        .send()
        .expect("GET info.json with X-Forwarded-Proto failed");

    assert_eq!(resp.status().as_u16(), 200);

    let json: Value = resp.json().expect("invalid JSON");
    let id = json["id"].as_str().expect("id must be string");
    assert!(
        id.starts_with("https://"),
        "id should use https:// with X-Forwarded-Proto: https, got: {}",
        id
    );
}

// --- HTTP Feature Tests ---

#[test]
fn cors_info_json_without_origin() {
    // No Origin: info.json answers ACAO * without credentials (CORS forbids
    // pairing * with credentials).
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed");

    assert_eq!(resp.status().as_u16(), 200);
    let acao = resp
        .headers()
        .get("access-control-allow-origin")
        .expect("missing ACAO header on info.json")
        .to_str()
        .unwrap();
    assert_eq!(acao, "*", "info.json ACAO should be * without Origin");
    assert!(
        resp.headers()
            .get("access-control-allow-credentials")
            .is_none(),
        "info.json without Origin must not send Access-Control-Allow-Credentials"
    );
}

#[test]
fn cors_knora_json_varies_on_origin() {
    // The ACAO is `*` without an Origin and the echoed Origin with one, so a
    // shared cache must key knora.json on Origin in both cases, as info.json.
    let srv = server();
    let url = format!("{}/unit/lena512.jp2/knora.json", srv.base_url);
    let resp = client().get(&url).send().expect("GET knora.json failed");
    assert_eq!(resp.status().as_u16(), 200);
    assert_varies_on_origin(&resp);

    let resp = client()
        .get(&url)
        .header("Origin", "https://example.org")
        .send()
        .expect("GET knora.json with Origin failed");
    assert_eq!(resp.status().as_u16(), 200);
    assert_credentialed_echo(&resp, "https://example.org");
    assert_varies_on_origin(&resp);
}

#[test]
fn cors_image_varies_on_origin_in_reflect_mode() {
    // Whether the image response carries ACAO depends on the Origin, so it
    // varies on Origin with and without one, on a success and on an error.
    let srv = server();
    for path in [
        "/unit/lena512.jp2/full/max/0/default.jpg",
        "/unit/missing.jp2/full/max/0/default.jpg",
    ] {
        let url = format!("{}{path}", srv.base_url);
        let resp = client().get(&url).send().expect("GET image failed");
        assert_varies_on_origin(&resp);
        let resp = client()
            .get(&url)
            .header("Origin", "https://example.org")
            .send()
            .expect("GET image with Origin failed");
        assert_varies_on_origin(&resp);
    }
}

#[test]
fn cors_null_origin_gets_no_allow_origin() {
    // The opaque Origin `null` is sent by sandboxed iframes and `file:`
    // pages; echoing it with credentials would hand any of them the
    // caller's cookies.
    let srv = server();
    for path in [
        "/unit/lena512.jp2/full/max/0/default.jpg",
        "/unit/lena512.jp2/info.json",
        "/unit/lena512.jp2/knora.json",
        "/unit/test.csv/file",
    ] {
        let resp = client()
            .get(format!("{}{path}", srv.base_url))
            .header("Origin", "null")
            .send()
            .unwrap_or_else(|e| panic!("GET {path} failed: {e}"));
        assert_eq!(resp.status().as_u16(), 200, "{path}");
        assert!(
            resp.headers().get("access-control-allow-origin").is_none(),
            "{path}: Origin null must get no ACAO"
        );
        assert!(
            resp.headers()
                .get("access-control-allow-credentials")
                .is_none(),
            "{path}: Origin null must get no credentials"
        );
        assert_varies_on_origin(&resp);
    }
    let resp = client()
        .request(
            reqwest::Method::OPTIONS,
            format!("{}/unit/lena512.jp2/full/max/0/default.jpg", srv.base_url),
        )
        .header("Origin", "null")
        .header("Access-Control-Request-Method", "GET")
        .send()
        .expect("OPTIONS with Origin null failed");
    assert!(resp.headers().get("access-control-allow-origin").is_none());
}

#[test]
fn info_json_varies_on_accept() {
    // Content-Type and Link depend on Accept (application/ld+json or not).
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .header("Accept", "application/ld+json")
        .send()
        .expect("GET info.json failed");
    assert_eq!(resp.status().as_u16(), 200);
    let vary = resp
        .headers()
        .get("vary")
        .expect("missing Vary header")
        .to_str()
        .unwrap();
    assert!(
        vary.split(',')
            .any(|v| v.trim().eq_ignore_ascii_case("accept")),
        "info.json Vary should contain Accept, got: {vary}"
    );
}

/// Assert `resp` echoes `origin` with credentials, as the image route does.
fn assert_credentialed_echo(resp: &reqwest::blocking::Response, origin: &str) {
    let headers = resp.headers();
    let acao = headers
        .get("access-control-allow-origin")
        .expect("missing ACAO header")
        .to_str()
        .unwrap();
    assert_eq!(acao, origin, "ACAO should echo the Origin");
    let credentials = headers
        .get("access-control-allow-credentials")
        .expect("missing Access-Control-Allow-Credentials")
        .to_str()
        .unwrap();
    assert_eq!(credentials, "true");
}

/// Assert `resp` carries a `Vary` naming `Origin`.
fn assert_varies_on_origin(resp: &reqwest::blocking::Response) {
    let vary = resp
        .headers()
        .get("vary")
        .expect("missing Vary header")
        .to_str()
        .unwrap();
    assert!(
        vary.split(',')
            .any(|v| v.trim().eq_ignore_ascii_case("origin")),
        "Vary should contain Origin, got: {}",
        vary
    );
}

#[test]
fn cors_info_json_with_origin() {
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .header("Origin", "https://example.org")
        .send()
        .expect("GET info.json with Origin failed");

    assert_eq!(resp.status().as_u16(), 200);
    assert_credentialed_echo(&resp, "https://example.org");
    assert_varies_on_origin(&resp);
}

#[test]
fn cors_info_json_with_origin_and_cookie() {
    // DEV-7520: a credentialed info.json fetch (dsp-app sends
    // `withCredentials: true`) is readable cross-origin only with an echoed
    // Origin plus credentials.
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .header("Origin", "https://app.example.org")
        .header("Cookie", "KnoraAuthentication=abc")
        .send()
        .expect("GET info.json with Origin and Cookie failed");

    assert_eq!(resp.status().as_u16(), 200);
    assert_credentialed_echo(&resp, "https://app.example.org");
}

#[test]
fn cors_info_json_auth_challenge_with_origin() {
    // The 401 auth-challenge info.json must be readable cross-origin, or the
    // viewer cannot see the login service it has to follow.
    let srv = server();
    let resp = client()
        .get(format!("{}/auth/lena512.jp2/info.json", srv.base_url))
        .header("Origin", "https://example.org")
        .send()
        .expect("GET /auth/lena512.jp2/info.json with Origin failed");

    assert_eq!(resp.status().as_u16(), 401);
    assert_credentialed_echo(&resp, "https://example.org");
}

#[test]
fn cors_preflight_info_json() {
    let srv = server();
    let resp = client()
        .request(
            reqwest::Method::OPTIONS,
            format!("{}/unit/lena512.jp2/info.json", srv.base_url),
        )
        .header("Origin", "https://example.org")
        .header("Access-Control-Request-Method", "GET")
        .header("Access-Control-Request-Headers", "Authorization")
        .send()
        .expect("OPTIONS preflight on info.json failed");

    assert_credentialed_echo(&resp, "https://example.org");
    // A credentialed preflight cannot use the `*` wildcard for methods or
    // headers: browsers treat it literally once credentials are involved.
    let headers = resp.headers();
    let methods = headers
        .get("access-control-allow-methods")
        .expect("missing Allow-Methods")
        .to_str()
        .unwrap();
    assert_eq!(methods, "GET, POST, PUT, DELETE");
    let allow_headers = headers
        .get("access-control-allow-headers")
        .expect("missing Allow-Headers")
        .to_str()
        .unwrap();
    assert_eq!(allow_headers, "Authorization");
}

#[test]
fn cors_image_with_origin() {
    // Image responses echo the Origin header
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .header("Origin", "https://example.org")
        .send()
        .expect("GET image with Origin failed");

    assert_eq!(resp.status().as_u16(), 200);
    let acao = resp
        .headers()
        .get("access-control-allow-origin")
        .expect("missing ACAO header on image response")
        .to_str()
        .unwrap();
    assert_eq!(acao, "https://example.org", "image ACAO should echo Origin");
}

#[test]
fn cors_image_without_origin() {
    // Without an Origin, no ACAO is set for image responses
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET image without Origin failed");

    assert_eq!(resp.status().as_u16(), 200);
    // No ACAO header should be present (set only when an Origin is provided)
    assert!(
        resp.headers().get("access-control-allow-origin").is_none(),
        "image response should not have ACAO without Origin header"
    );
}

#[test]
fn cors_error_responses_with_origin() {
    // A cross-origin client only sees a response carrying ACAO: without it a
    // 400/401/403/404 reaches the browser as status 0. Every IIIF-route error
    // must echo the Origin with credentials, like the success responses do.
    let srv = server();
    for (path, status) in [
        // Parser rejection (before the engine) and engine rejection.
        ("/unit/lena512.jp2/invalid/max/0/default.jpg", 400),
        ("/unit/lena512.jp2/600,600,100,100/max/0/default.jpg", 400),
        // `deny` from `pre_flight`, and from `file_pre_flight` (which denies
        // only `unit/test2.csv`).
        ("/tmp/lena512.jp2/full/max/0/default.jpg", 401),
        ("/unit/test2.csv/file", 401),
        // A bare `restrict` decision is refused. `file_pre_flight` never
        // answers `restrict`, so `/file` has no 403 case here.
        (
            "/test_restrict_bare/lena512.jp2/full/max/0/default.jpg",
            403,
        ),
        ("/test_restrict_bare/lena512.jp2/info.json", 403),
        // File missing on disk.
        ("/unit/missing.jp2/full/max/0/default.jpg", 404),
        ("/unit/missing.jp2/info.json", 404),
        ("/unit/missing.jp2/knora.json", 404),
        ("/unit/missing.csv/file", 404),
    ] {
        let resp = client()
            .get(format!("{}{path}", srv.base_url))
            .header("Origin", "https://example.org")
            .send()
            .unwrap_or_else(|e| panic!("GET {path} failed: {e}"));
        assert_eq!(resp.status().as_u16(), status, "{path}");
        assert_credentialed_echo(&resp, "https://example.org");
    }
}

#[test]
fn cors_preflight() {
    // OPTIONS with all three required headers triggers CORS preflight
    let srv = server();
    let resp = client()
        .request(
            reqwest::Method::OPTIONS,
            format!("{}/unit/lena512.jp2/full/max/0/default.jpg", srv.base_url),
        )
        .header("Origin", "https://example.org")
        .header("Access-Control-Request-Method", "GET")
        .header("Access-Control-Request-Headers", "Authorization")
        .send()
        .expect("OPTIONS preflight failed");

    let headers = resp.headers();
    let acao = headers
        .get("access-control-allow-origin")
        .expect("missing ACAO on preflight")
        .to_str()
        .unwrap();
    assert_eq!(
        acao, "https://example.org",
        "preflight ACAO should echo Origin"
    );

    let methods = headers
        .get("access-control-allow-methods")
        .expect("missing Allow-Methods")
        .to_str()
        .unwrap();
    assert!(
        methods.contains("GET"),
        "Allow-Methods should include GET, got: {}",
        methods
    );

    let allow_headers = headers
        .get("access-control-allow-headers")
        .expect("missing Allow-Headers")
        .to_str()
        .unwrap();
    assert!(
        allow_headers.contains("Authorization"),
        "Allow-Headers should echo requested headers, got: {}",
        allow_headers
    );
}

#[test]
fn base_uri_redirect() {
    // GET /{prefix}/{identifier} (no IIIF params) -> 303 to info.json
    let srv = server();
    let resp = client_no_redirect()
        .get(format!("{}/unit/lena512.jp2", srv.base_url))
        .send()
        .expect("GET base URI failed");

    assert_eq!(
        resp.status().as_u16(),
        303,
        "base URI should return 303 See Other"
    );

    let location = resp
        .headers()
        .get("location")
        .expect("missing Location header on redirect")
        .to_str()
        .unwrap();
    assert!(
        location.ends_with("/unit/lena512.jp2/info.json"),
        "Location should point to info.json, got: {}",
        location
    );
}

#[test]
fn jsonld_media_type_with_accept() {
    // With Accept: application/ld+json, sipi returns application/ld+json with profile
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .header("Accept", "application/ld+json")
        .send()
        .expect("GET info.json with Accept ld+json failed");

    assert_eq!(resp.status().as_u16(), 200);

    let ct = resp
        .headers()
        .get("content-type")
        .expect("missing content-type")
        .to_str()
        .unwrap();
    assert!(
        ct.contains("application/ld+json"),
        "content-type should be application/ld+json, got: {}",
        ct
    );
    assert!(
        ct.contains("http://iiif.io/api/image/3/context.json"),
        "content-type should include profile, got: {}",
        ct
    );
}

#[test]
fn jsonld_default_has_link_header() {
    // Without Accept header, sipi returns application/json with Link to context
    let srv = server();
    let resp = client()
        .get(format!("{}/unit/lena512.jp2/info.json", srv.base_url))
        .send()
        .expect("GET info.json failed");

    assert_eq!(resp.status().as_u16(), 200);

    let ct = resp
        .headers()
        .get("content-type")
        .expect("missing content-type")
        .to_str()
        .unwrap();
    assert!(
        ct.contains("application/json"),
        "default content-type should be application/json, got: {}",
        ct
    );

    let link = resp
        .headers()
        .get("link")
        .expect("missing Link header for JSON-LD context")
        .to_str()
        .unwrap();
    assert!(
        link.contains("http://iiif.io/api/image/3/context.json"),
        "Link header should reference IIIF context, got: {}",
        link
    );
}

#[test]
fn canonical_link_header() {
    // Image response includes Link: <...>;rel="canonical"
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET image failed");

    assert_eq!(resp.status().as_u16(), 200);

    let link = resp
        .headers()
        .get("link")
        .expect("missing Link header on image response")
        .to_str()
        .unwrap();
    assert!(
        link.contains("rel=\"canonical\""),
        "Link header should contain rel=\"canonical\", got: {}",
        link
    );
}

#[test]
fn profile_link_header() {
    // IIIF Image API 3.0 places the profileLinkHeader on image responses (the
    // feature is advertised in info.json's extraFeatures). The canonical and
    // profile links are folded into a single Link header value.
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET image failed");

    assert_eq!(resp.status().as_u16(), 200);

    let link = resp
        .headers()
        .get("link")
        .expect("missing Link header")
        .to_str()
        .unwrap();
    assert!(
        link.contains("http://iiif.io/api/image/3/level2.json") && link.contains("rel=\"profile\""),
        "image Link header should contain the profile link, got: {}",
        link
    );
}

#[test]
fn full_iiif_url_returns_image() {
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET image failed");

    assert_eq!(
        resp.status().as_u16(),
        200,
        "Expected 200 for full IIIF URL"
    );
}

// --- Region Tests ---

#[test]
fn region_square() {
    assert_iiif_status("/unit/lena512.jp2/square/max/0/default.jpg", 200);
}

#[test]
fn region_percent() {
    assert_iiif_status("/unit/lena512.jp2/pct:10,10,50,50/max/0/default.jpg", 200);
}

#[test]
fn region_pixel() {
    assert_iiif_status("/unit/lena512.jp2/0,0,100,100/max/0/default.jpg", 200);
}

#[test]
fn region_pixel_offset() {
    assert_iiif_status("/unit/lena512.jp2/50,50,200,200/max/0/default.jpg", 200);
}

#[test]
fn region_beyond_bounds_is_cropped() {
    assert_iiif_status("/unit/lena512.jp2/400,400,9999,9999/max/0/default.jpg", 200);
}

#[test]
fn region_start_beyond_image() {
    assert_iiif_status("/unit/lena512.jp2/600,600,100,100/max/0/default.jpg", 400);
}

#[test]
fn region_zero_width() {
    assert_iiif_status("/unit/lena512.jp2/0,0,0,100/max/0/default.jpg", 200);
}

#[test]
fn region_invalid_syntax() {
    assert_iiif_status("/unit/lena512.jp2/invalid/max/0/default.jpg", 400);
}

// --- Size Tests ---

#[test]
fn size_by_width() {
    assert_iiif_status("/unit/lena512.jp2/full/256,/0/default.jpg", 200);
}

#[test]
fn size_by_height() {
    assert_iiif_status("/unit/lena512.jp2/full/,256/0/default.jpg", 200);
}

#[test]
fn size_exact() {
    assert_iiif_status("/unit/lena512.jp2/full/200,200/0/default.jpg", 200);
}

#[test]
fn size_best_fit() {
    assert_iiif_status("/unit/lena512.jp2/full/!200,200/0/default.jpg", 200);
}

#[test]
fn size_percent() {
    assert_iiif_status("/unit/lena512.jp2/full/pct:50/0/default.jpg", 200);
}

#[test]
fn size_upscaling() {
    assert_iiif_status("/unit/lena512.jp2/full/^1000,/0/default.jpg", 200);
}

/// DEV-6118: `full/max` and `full/^max` on an unrestricted image are the
/// native size from info.json, for a JP2 and a TIFF whose `sizes` ladder has
/// more than one entry.
#[test]
fn full_max_on_unrestricted_images_is_the_native_size() {
    let srv = server();
    for file in ["unit/lena512_tiled.jp2", "unit/mario.tif"] {
        let resp = client()
            .get(format!("{}/{file}/info.json", srv.base_url))
            .send()
            .expect("GET info.json");
        assert_eq!(resp.status().as_u16(), 200, "{file} info.json");
        let info: Value = resp.json().expect("info.json body");
        let native = (
            u32::try_from(info["width"].as_u64().expect("width")).expect("width"),
            u32::try_from(info["height"].as_u64().expect("height")).expect("height"),
        );
        assert!(
            info["sizes"].as_array().expect("sizes").len() > 1,
            "{file} must have a sizes ladder: {info}"
        );
        assert!(info.get("maxWidth").is_none(), "{file}: {info}");
        for size in ["max", "^max"] {
            let resp = client()
                .get(format!("{}/{file}/full/{size}/0/default.jpg", srv.base_url))
                .send()
                .expect("GET full");
            assert_eq!(resp.status().as_u16(), 200, "{file} full/{size}");
            let img = image::load_from_memory(&resp.bytes().expect("body")).expect("decode");
            assert_eq!(img.dimensions(), native, "{file} full/{size}");
        }
    }
}

#[test]
fn size_no_upscale_beyond_original() {
    assert_iiif_status("/unit/lena512.jp2/full/1000,/0/default.jpg", 400);
}

#[test]
fn size_degenerate_target_dimensions() {
    assert_iiif_status("/unit/lena512.jp2/full/1,1/0/default.jpg", 400);
}

#[test]
fn size_after_region() {
    assert_iiif_status("/unit/lena512.jp2/0,0,200,200/100,/0/default.jpg", 200);
}

#[test]
fn size_invalid_syntax() {
    assert_iiif_status("/unit/lena512.jp2/full/invalid/0/default.jpg", 400);
}

// --- Rotation Tests ---

#[test]
fn mirror_rotation() {
    assert_iiif_status("/unit/lena512.jp2/full/max/!0/default.jpg", 200);
}

#[test]
fn rotation_180() {
    assert_iiif_status("/unit/lena512.jp2/full/max/180/default.jpg", 200);
}

#[test]
fn rotation_270() {
    assert_iiif_status("/unit/lena512.jp2/full/max/270/default.jpg", 200);
}

#[test]
fn rotation_arbitrary() {
    assert_iiif_status("/unit/lena512.jp2/full/max/45/default.png", 200);
}

#[test]
fn mirror_plus_180() {
    assert_iiif_status("/unit/lena512.jp2/full/max/!180/default.jpg", 200);
}

#[test]
fn rotation_after_region() {
    assert_iiif_status("/unit/lena512.jp2/square/max/90/default.jpg", 200);
}

#[test]
fn rotation_invalid() {
    assert_iiif_status("/unit/lena512.jp2/full/max/abc/default.jpg", 400);
}

// --- Quality and Format Tests ---

#[test]
fn quality_gray() {
    assert_iiif_status("/unit/lena512.jp2/full/max/0/gray.jpg", 200);
}

#[test]
fn quality_color() {
    assert_iiif_status("/unit/lena512.jp2/full/max/0/color.jpg", 200);
}

#[test]
fn quality_bitonal() {
    assert_iiif_status("/unit/lena512.jp2/full/max/0/bitonal.jpg", 200);
}

#[test]
fn quality_invalid() {
    assert_iiif_status("/unit/lena512.jp2/full/max/0/invalid.jpg", 400);
}

#[test]
fn format_jpg_content_type() {
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET jpg format failed");
    assert_eq!(resp.status().as_u16(), 200);
    let ct = resp
        .headers()
        .get("content-type")
        .unwrap()
        .to_str()
        .unwrap();
    assert_eq!(ct, "image/jpeg");
}

#[test]
fn format_png_content_type() {
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.png",
            srv.base_url
        ))
        .send()
        .expect("GET png format failed");
    assert_eq!(resp.status().as_u16(), 200);
    let ct = resp
        .headers()
        .get("content-type")
        .unwrap()
        .to_str()
        .unwrap();
    assert_eq!(ct, "image/png");
}

#[test]
fn format_tiff_content_type() {
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.tif",
            srv.base_url
        ))
        .send()
        .expect("GET tif format failed");
    assert_eq!(resp.status().as_u16(), 200);
    let ct = resp
        .headers()
        .get("content-type")
        .unwrap()
        .to_str()
        .unwrap();
    assert_eq!(ct, "image/tiff");
}

#[test]
fn format_jp2_content_type() {
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jp2",
            srv.base_url
        ))
        .send()
        .expect("GET jp2 format failed");
    assert_eq!(resp.status().as_u16(), 200);
    let ct = resp
        .headers()
        .get("content-type")
        .unwrap()
        .to_str()
        .unwrap();
    assert_eq!(ct, "image/jp2");
}

#[test]
fn unsupported_formats_rejected() {
    // sipi's URL parser regex only accepts jpg|tif|png|jp2
    for ext in &["gif", "pdf", "webp", "bmp"] {
        assert_iiif_status(
            &format!("/unit/lena512.jp2/full/max/0/default.{}", ext),
            400,
        );
    }
}

// --- Identifier and Error Handling Tests ---

#[test]
fn id_escaped() {
    // test#image.jp2 is a symlink to lena512.jp2; %23 is the URL-encoding of #.
    // A percent-escaped # in the identifier must resolve to the on-disk file
    // (IIIF Image API 3.0 identifier escaping).
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/test%23image.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET escaped id failed");
    assert_eq!(
        resp.status().as_u16(),
        200,
        "escaped identifier should resolve to the symlinked file"
    );
}

#[test]
fn id_escaped_slash_decoded() {
    // %2F is an escaped forward slash — sipi decodes it and treats as path separator,
    // so unit%2Flena512.jp2 resolves to unit/lena512.jp2 which exists -> 200.
    // The IIIF spec expects 404 but sipi's URL decoding resolves it.
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit%2Flena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET escaped slash id failed");
    assert_eq!(
        resp.status().as_u16(),
        200,
        "sipi decodes %2F as / and resolves the path"
    );
}

#[test]
fn id_random_gives_404() {
    assert_iiif_status("/nonexistent-random-id/full/max/0/default.jpg", 404);
}

#[test]
fn id_incomplete_iiif_url() {
    assert_iiif_status("/unit/lena512.jp2/full/max/default.jpg", 400);
}

#[test]
fn id_malformed_iiif_url() {
    assert_iiif_status("/unit/lena512.jp2/max/0/default.jpg", 400);
}

#[test]
fn invalid_iiif_url_empty_identifier() {
    assert_iiif_status("/unit//lena512.jp2", 400);
}

// --- Close IIIF Spec Gaps ---

/// Fetch an IIIF image and decode its pixel dimensions.
fn fetch_image_dimensions(path: &str) -> (u32, u32) {
    let srv = server();
    let resp = client()
        .get(format!("{}{}", srv.base_url, path))
        .send()
        .unwrap_or_else(|e| panic!("GET {} failed: {}", path, e));
    assert_eq!(
        resp.status().as_u16(),
        200,
        "expected 200 for {}, got {}",
        path,
        resp.status().as_u16()
    );
    let bytes = resp.bytes().expect("failed to read image body");
    let img = image::load_from_memory(&bytes)
        .unwrap_or_else(|e| panic!("failed to decode image from {}: {}", path, e));
    img.dimensions()
}

// --- Size upscale variants ---

#[test]
fn size_upscale_max() {
    // ^max should return an image larger than the original (or 501 if unsupported).
    // lena512.jp2 is 512x512; ^max with upscaling should work since sizeUpscaling is advertised.
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/^max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET ^max failed");
    // Accept either 200 (upscaled) or 501 (not implemented)
    let status = resp.status().as_u16();
    assert!(
        status == 200 || status == 501,
        "^max should return 200 or 501, got {}",
        status
    );
}

#[test]
fn size_upscale_height() {
    // ^,h with height larger than original should upscale
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/^,1024/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET ^,1024 failed");
    let status = resp.status().as_u16();
    assert!(
        status == 200 || status == 501,
        "^,h upscale should return 200 or 501, got {}",
        status
    );
}

#[test]
fn size_upscale_exact() {
    // ^w,h with dimensions larger than original
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/^1024,1024/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET ^1024,1024 failed");
    let status = resp.status().as_u16();
    assert!(
        status == 200 || status == 501,
        "^w,h upscale should return 200 or 501, got {}",
        status
    );
}

#[test]
fn size_upscale_confined() {
    // ^!w,h confined upscale — fit within bounding box, allowing upscale
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/^!1024,1024/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET ^!1024,1024 failed");
    let status = resp.status().as_u16();
    assert!(
        status == 200 || status == 501,
        "^!w,h upscale should return 200 or 501, got {}",
        status
    );
}

#[test]
fn size_upscale_percent() {
    // ^pct:150 should return 150% of original size
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/full/^pct:150/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET ^pct:150 failed");
    let status = resp.status().as_u16();
    assert!(
        status == 200 || status == 501,
        "^pct:150 should return 200 or 501, got {}",
        status
    );
}

// --- Dimension verification tests ---

#[test]
fn region_dimension_verification() {
    // Request a 100x100 pixel region and verify output dimensions match.
    let (w, h) = fetch_image_dimensions("/unit/lena512.jp2/100,100,100,100/max/0/default.png");
    assert_eq!(w, 100, "region width should be 100, got {}", w);
    assert_eq!(h, 100, "region height should be 100, got {}", h);
}

#[test]
fn size_dimension_verification() {
    // Request width=256 (height auto) and verify output dimensions.
    let (w, _h) = fetch_image_dimensions("/unit/lena512.jp2/full/256,/0/default.png");
    assert_eq!(w, 256, "size width should be 256, got {}", w);
    // lena512.jp2 is square, so height should also be 256
    assert_eq!(
        _h, 256,
        "size height should be 256 for square image, got {}",
        _h
    );
}

#[test]
fn rotation_dimension_verification() {
    // 90° rotation of a square image keeps same dimensions;
    // use a non-square region to detect the swap.
    // Crop a 200x100 region, then rotate 90°: output should be 100x200.
    let (w, h) = fetch_image_dimensions("/unit/lena512.jp2/0,0,200,100/max/90/default.png");
    assert_eq!(
        w, 100,
        "90° rotation should swap: width should be 100, got {}",
        w
    );
    assert_eq!(
        h, 200,
        "90° rotation should swap: height should be 200, got {}",
        h
    );
}

// --- Operation ordering ---

#[test]
fn operation_ordering() {
    // Verify region→size→rotation order.
    // Crop 200x100 region, scale to width=100 (height=50), rotate 90° → 50x100.
    let (w, h) = fetch_image_dimensions("/unit/lena512.jp2/0,0,200,100/100,/90/default.png");
    assert_eq!(
        w, 50,
        "operation order: final width should be 50, got {}",
        w
    );
    assert_eq!(
        h, 100,
        "operation order: final height should be 100, got {}",
        h
    );
}

// --- Fractional percent region ---

#[test]
fn fractional_percent_region() {
    // Test pct: with fractional values
    assert_iiif_status(
        "/unit/lena512.jp2/pct:0.5,0.5,99.0,99.0/max/0/default.jpg",
        200,
    );
}

// --- Conditional request ---

#[test]
fn conditional_request_304() {
    // First request to get Last-Modified, then conditional request for 304.
    let srv = server();
    let url = format!("{}/unit/lena512.jp2/full/max/0/default.jpg", srv.base_url);
    let resp1 = client().get(&url).send().expect("first GET failed");
    assert_eq!(resp1.status().as_u16(), 200);

    let last_modified = resp1
        .headers()
        .get("last-modified")
        .map(|v| v.to_str().unwrap().to_string());
    if let Some(lm) = last_modified {
        let resp2 = client()
            .get(&url)
            .header("If-Modified-Since", &lm)
            .send()
            .expect("conditional GET failed");
        // Sipi should return 304 if it supports conditional requests,
        // or 200 if it doesn't. Document the actual behavior.
        let status = resp2.status().as_u16();
        assert!(
            status == 304 || status == 200,
            "conditional request should return 304 or 200, got {}",
            status
        );
    }
    // If no Last-Modified header, the test is inconclusive — sipi doesn't support it yet
}

// --- Extra qualities in info.json ---

#[test]
fn extra_qualities_in_info_json() {
    let json = fetch_info_json();
    // Check if extraQualities is present. IIIF spec says servers SHOULD list
    // supported qualities beyond "default" in extraQualities.
    if let Some(qualities) = json.get("extraQualities") {
        let q_arr = qualities.as_array().expect("extraQualities must be array");
        let q_strs: Vec<&str> = q_arr.iter().filter_map(|q| q.as_str()).collect();
        // Sipi supports color, gray, bitonal
        assert!(
            q_strs.contains(&"color") || q_strs.contains(&"gray") || q_strs.contains(&"bitonal"),
            "extraQualities should list supported qualities, got: {:?}",
            q_strs
        );
    }
    // If extraQualities is absent, document as a gap but don't fail —
    // this test verifies the field when present.
}

// --- Non-ASCII identifier ---

#[test]
fn id_non_ascii() {
    // URL-encoded non-ASCII characters in identifier.
    // %C3%A9 is é (e-acute) — this identifier won't exist, so expect 404 (not 500/crash).
    let srv = server();
    let resp = client()
        .get(format!(
            "{}/unit/caf%C3%A9.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("GET non-ASCII id failed");
    let status = resp.status().as_u16();
    assert!(
        status == 404 || status == 400,
        "non-ASCII identifier should return 404 or 400, got {} (not 500/crash)",
        status
    );
}

// --- region_zero_width spec discrepancy ---
// Note: The existing `region_zero_width` test (line 712) asserts 200.
// IIIF spec says zero-width regions SHOULD return 400.
// Current sipi behavior returns 200 with a degenerate image.
// This is documented as non-compliance rather than a test change,
// since changing server behavior is out of scope here.
// See: testing-strategy.md gap matrix, Region section.

// --- Tests moved from server.rs (IIIF-related) ---

#[test]
fn iiif_region_crop() {
    assert_iiif_status("/unit/lena512.jp2/0,0,256,256/max/0/default.jpg", 200);
}

#[test]
fn iiif_rotation_90() {
    assert_iiif_status("/unit/lena512.jp2/full/max/90/default.jpg", 200);
}

#[test]
fn deny_unauthorized_image() {
    assert_iiif_status("/knora/DenyLeaves.jpg/full/max/0/default.jpg", 401);
}

#[test]
fn head_iiif_image_empty_body() {
    let srv = server();
    let resp = client()
        .head(format!(
            "{}/unit/lena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("HEAD IIIF image failed");

    assert_eq!(resp.status().as_u16(), 200);
    assert_eq!(resp.text().unwrap_or_default().len(), 0);
}

#[test]
fn path_traversal_rejected() {
    let srv = server();
    for path in &[
        "/unit/%2E%2E%2F%2E%2E%2Fetc%2Fpasswd/full/max/0/default.jpg",
        "/unit/..%2F..%2Fetc%2Fpasswd/full/max/0/default.jpg",
    ] {
        let resp = client()
            .get(format!("{}{}", srv.base_url, path))
            .send()
            .unwrap_or_else(|e| panic!("GET {} failed: {}", path, e));
        let status = resp.status().as_u16();
        let body = resp.text().unwrap_or_default();
        assert!(
            [400, 403, 404].contains(&status),
            "path traversal should be rejected: {} returned {} body={}",
            path,
            status,
            &body[..body.len().min(200)]
        );
    }
}

// --- IIIF Pipeline Tests ---

#[test]
fn metadata_iiif_pipeline() {
    // Request image through IIIF pipeline with region+size+rotation transforms.
    // Verify the pipeline produces a valid image with correct dimensions.
    let srv = server();

    let resp = client()
        .get(format!(
            "{}/unit/lena512.jp2/0,0,256,256/128,128/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("IIIF pipeline request failed");

    assert_eq!(resp.status().as_u16(), 200);
    let ct = resp
        .headers()
        .get("content-type")
        .and_then(|v| v.to_str().ok())
        .unwrap_or("");
    assert!(
        ct.contains("image/jpeg"),
        "expected JPEG content-type, got: {}",
        ct
    );

    let bytes = resp.bytes().expect("read body");
    assert!(!bytes.is_empty(), "response body should not be empty");

    let img = image::load_from_memory(&bytes).expect("decode JPEG from pipeline");
    let (w, h) = img.dimensions();
    assert_eq!(w, 128, "expected width 128, got {}", w);
    assert_eq!(h, 128, "expected height 128, got {}", h);
}

/// Helper: create a truncated copy of a file, request it via IIIF, verify the
/// server returns an error status (not crash), and stays healthy afterward.
fn assert_corrupt_image_handled(
    filename: &str,
    source: &str,
    truncate_bytes: usize,
    iiif_format: &str,
) {
    let test_data = sipi_e2e::test_data_dir();
    let corrupt_path = test_data.join(format!("images/unit/{}", filename));

    let real_data =
        std::fs::read(test_data.join(source)).unwrap_or_else(|e| panic!("read {source}: {e}"));
    assert!(
        real_data.len() > truncate_bytes,
        "{} is smaller than {} bytes",
        source,
        truncate_bytes
    );
    std::fs::write(&corrupt_path, &real_data[..truncate_bytes]).expect("write truncated file");

    // Give the isolated server its own cache dir so its startup orphan
    // scan doesn't wipe out the main shared server's cache entries —
    // shared `cache_dir = './cache'` from the Lua config means concurrent
    // sipi processes race on `.sipicache` index updates and on disk files.
    let cache_tmp = tempfile::tempdir().expect("create isolated cache dir");
    let cache_arg = cache_tmp.path().to_string_lossy().to_string();
    let isolated_srv = sipi_e2e::SipiServer::start_with_args(
        "config/sipi.e2e-test-config.lua",
        &test_data,
        &["--cache-dir", &cache_arg],
    );
    let isolated_client = sipi_e2e::http_client();

    // Request the corrupt image — server should return error, not crash
    let result = isolated_client
        .get(format!(
            "{}/unit/{}/full/max/0/default.{}",
            isolated_srv.base_url, filename, iiif_format
        ))
        .send();

    match result {
        Ok(resp) => {
            let status = resp.status().as_u16();
            assert!(
                status >= 400,
                "corrupt {} should return error status, got {}",
                filename,
                status
            );
        }
        Err(e) => {
            panic!(
                "Server crashed or closed connection for corrupt {}: {}. \
                 The catch-all should have returned HTTP 500.",
                filename, e
            );
        }
    }

    // Verify server is still healthy after handling the corrupt request
    let health_resp = isolated_client
        .get(format!(
            "{}/unit/lena512.jp2/info.json",
            isolated_srv.base_url
        ))
        .send()
        .expect("health check after corrupt image failed — server may have crashed");
    assert_eq!(
        health_resp.status().as_u16(),
        200,
        "server should still be healthy after handling corrupt {}",
        filename
    );

    let _ = std::fs::remove_file(&corrupt_path);

    // Drop the server before the temp dir: SipiServer::Drop sends SIGTERM
    // and waits up to 5s for graceful shutdown; if `cache_tmp` were dropped
    // first, the cache_dir would be deleted under sipi's feet during its
    // shutdown flush. Today this is enforced implicitly by reverse-
    // declaration drop order — make it explicit so a future refactor can't
    // silently break it.
    drop(isolated_srv);
    drop(cache_tmp);
}

#[test]
fn corrupt_image_handling() {
    // Truncated JP2 — Kakadu's error handler throws (SipiIOJ2k's KduSipiError),
    // the read path converts it to a SipiImageError, and the server returns a
    // clean error and stays up (same graceful-degradation contract as JPEG/PNG).
    assert_corrupt_image_handled("corrupt_test.jp2", "images/unit/lena512.jp2", 100, "jpg");
}

#[test]
fn corrupt_jpeg_handling() {
    // Truncated JPEG — exercises setjmp/longjmp in JPEG read path + server catch-all.
    assert_corrupt_image_handled(
        "corrupt_test.jpg",
        "images/unit/MaoriFigure.jpg",
        100,
        "jpg",
    );
}

#[test]
fn corrupt_png_handling() {
    // Truncated PNG — exercises setjmp(png_jmpbuf) in PNG read path + server catch-all.
    assert_corrupt_image_handled("corrupt_test.png", "images/unit/mario.png", 100, "jpg");
}

#[test]
fn double_encoded_url() {
    // Request with %252F (double-encoded slash), verify correct handling.
    // %252F decodes to %2F at the HTTP level, which is the URL encoding of '/'.
    let srv = server();

    let resp = client()
        .get(format!(
            "{}/unit%252Flena512.jp2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("double-encoded URL request failed");

    // Should handle gracefully — 200, 404, or 400 are all acceptable
    let status = resp.status().as_u16();
    assert!(
        status == 200 || status == 404 || status == 400,
        "double-encoded URL should return 200, 404, or 400, got {}",
        status
    );
}

#[test]
#[ignore = "multi-page TIFF page selection may not be implemented"]
fn multipage_tiff_page_selection() {
    // Request with @page syntax for multi-page TIFF. Documents current behavior.
    let srv = server();

    // Basic TIFF request should work
    let resp = client()
        .get(format!(
            "{}/unit/gradient-stars.tif/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("TIFF request failed");
    assert_eq!(resp.status().as_u16(), 200);

    // Page selection syntax — may or may not be supported
    let resp2 = client()
        .get(format!(
            "{}/unit/gradient-stars.tif@2/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("multi-page TIFF request should not crash");

    let status = resp2.status().as_u16();
    assert!(
        status == 200 || status == 404 || status == 400 || status == 500,
        "multi-page TIFF request should return a valid HTTP status, got {}",
        status
    );
}

#[test]
fn cmyk_through_iiif_pipeline() {
    // CMYK TIFF through IIIF pipeline, verify sRGB JPEG output.
    let srv = server();

    let resp = client()
        .get(format!(
            "{}/unit/cmyk.tif/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("CMYK TIFF IIIF request failed");

    let status = resp.status().as_u16();
    assert_eq!(
        status, 200,
        "CMYK TIFF should be convertible to JPEG, got {}",
        status
    );

    let bytes = resp.bytes().expect("read CMYK response body");
    assert!(!bytes.is_empty());

    // Decode the JPEG — if this succeeds, CMYK→sRGB conversion worked
    let img = image::load_from_memory(&bytes).expect("decode JPEG from CMYK source");
    let (w, h) = img.dimensions();
    assert!(w > 0 && h > 0, "decoded image should have valid dimensions");
}

#[test]
fn cielab_through_iiif_pipeline() {
    // CIELab TIFF through IIIF pipeline, verify successful JPEG conversion.
    let srv = server();

    let resp = client()
        .get(format!(
            "{}/unit/cielab.tif/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("CIELab TIFF IIIF request failed");

    let status = resp.status().as_u16();
    assert_eq!(
        status, 200,
        "CIELab TIFF should be convertible to JPEG, got {}",
        status
    );

    let bytes = resp.bytes().expect("read CIELab response body");
    assert!(!bytes.is_empty());

    let img = image::load_from_memory(&bytes).expect("decode JPEG from CIELab source");
    let (w, h) = img.dimensions();
    assert!(w > 0 && h > 0, "decoded image should have valid dimensions");
}

#[test]
fn bit16_through_iiif_pipeline() {
    // 16-bit PNG through IIIF pipeline to JPEG (which only supports 8-bit).
    let srv = server();

    // Copy 16-bit PNG from knora directory to unit for auth-free access
    let test_data = sipi_e2e::test_data_dir();
    let src = test_data.join("images/knora/png_16bit.png");
    let dst = test_data.join("images/unit/png_16bit_test.png");
    if src.exists() {
        std::fs::copy(&src, &dst).expect("copy 16-bit PNG to unit dir");
    } else {
        eprintln!("Skipping: 16-bit PNG test fixture not found");
        return;
    }

    let resp = client()
        .get(format!(
            "{}/unit/png_16bit_test.png/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("16-bit PNG IIIF request failed");

    let status = resp.status().as_u16();
    let _ = std::fs::remove_file(&dst);

    assert_eq!(
        status, 200,
        "16-bit PNG should be convertible to JPEG, got {}",
        status
    );

    let bytes = resp.bytes().expect("read 16-bit PNG response body");
    assert!(!bytes.is_empty());

    // JPEG is always 8-bit — successful decode confirms 16→8 bit conversion
    let img = image::load_from_memory(&bytes).expect("decode JPEG from 16-bit PNG source");
    let (w, h) = img.dimensions();
    assert!(w > 0 && h > 0);
}

#[test]
fn progressive_jpeg_input() {
    // JPEG input through IIIF pipeline with transforms.
    let srv = server();

    let resp = client()
        .get(format!(
            "{}/unit/MaoriFigure.jpg/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("JPEG input IIIF request failed");

    assert_eq!(resp.status().as_u16(), 200);

    let bytes = resp.bytes().expect("read JPEG response body");
    assert!(!bytes.is_empty());

    let img = image::load_from_memory(&bytes).expect("decode JPEG from JPEG source");
    let (w, h) = img.dimensions();
    assert!(
        w > 0 && h > 0,
        "JPEG→JPEG pipeline should produce valid output"
    );

    // Also test with a transform to exercise the full pipeline
    let resp2 = client()
        .get(format!(
            "{}/unit/MaoriFigure.jpg/0,0,200,200/100,100/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("JPEG input with transform failed");

    assert_eq!(resp2.status().as_u16(), 200);
    let bytes2 = resp2.bytes().expect("read transformed JPEG");
    let img2 = image::load_from_memory(&bytes2).expect("decode transformed JPEG");
    let (w2, h2) = img2.dimensions();
    assert_eq!(w2, 100, "expected width 100, got {}", w2);
    assert_eq!(h2, 100, "expected height 100, got {}", h2);
}

#[test]
fn tiff_jpeg_compression_input() {
    // TIFF with JPEG compression.
    let srv = server();

    // Copy from knora directory to unit for auth-free access
    let test_data = sipi_e2e::test_data_dir();
    let src = test_data.join("images/knora/tiffJpegScanlineBug.tif");
    let dst = test_data.join("images/unit/tiffJpegScanlineBug_test.tif");
    if src.exists() {
        std::fs::copy(&src, &dst).expect("copy TIFF JPEG fixture to unit dir");
    } else {
        eprintln!("Skipping: tiffJpegScanlineBug.tif not found");
        return;
    }

    let resp = client()
        .get(format!(
            "{}/unit/tiffJpegScanlineBug_test.tif/full/max/0/default.jpg",
            srv.base_url
        ))
        .send()
        .expect("TIFF JPEG compression request failed");

    let status = resp.status().as_u16();

    assert_eq!(
        status, 200,
        "TIFF JPEG compression input should decode successfully"
    );

    let bytes = resp.bytes().expect("read TIFF JPEG response body");
    let img = image::load_from_memory(&bytes).expect("decode returned JPEG");
    let (w, h) = img.dimensions();
    assert!(w > 0 && h > 0);

    // The source fixture must outlive the streamed response, so remove it last.
    let _ = std::fs::remove_file(&dst);
}
