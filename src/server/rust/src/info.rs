//! IIIF `info.json` + SIPI `knora.json` assembly.
//!
//! The seam has no serve entry for these JSON responses — the Rust shell builds
//! them from the edge-probe results (`sipi_image_dims` / `sipi_mimetype`), a
//! `stat`, and the optional `.info` sidecar. Ported field-for-field from the
//! C++ `serve_info_json_file` / `serve_knora_json_file` builders. Key order is
//! irrelevant: both
//! `serde_json` and the e2e `insta` snapshots normalise to sorted keys.
//!
//! The builders are pure (they take already-fetched data), so they unit-test
//! without the engine; the handler does the FFI + filesystem I/O.

use serde_json::{json, Map, Value};

use crate::ffi::{ImageEssentials, SipiImageDims, SipiPermType};

const IMAGE_CONTEXT: &str = "http://iiif.io/api/image/3/context.json";
const FILE_CONTEXT: &str = "http://sipi.io/api/file/3/context.json";

/// The 17 IIIF `extraFeatures` SIPI advertises.
const EXTRA_FEATURES: [&str; 17] = [
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

/// IIIF Image API context URL — used for the info.json `Link` header.
#[must_use]
pub const fn image_context() -> &'static str {
    IMAGE_CONTEXT
}

/// SIPI file context URL — used for the non-image info.json `Link` header.
#[must_use]
pub const fn file_context() -> &'static str {
    FILE_CONTEXT
}

/// Reference tile size for the `sizes` ladder when the image is untiled.
const DEFAULT_TILE_SIZE: u32 = 512;

/// Contiguous powers of two `[2^0 .. 2^n]` (ascending) derived from the tile
/// grid, where `n` is the smallest level count that puts the whole image inside
/// one tile on both axes: `n = max(ceil(log2(w / tile_w)), ceil(log2(h / tile_h)))`.
/// `n` is clamped to 31 so every factor fits `u32` (`2^32` would truncate to 0 and
/// make `sizes_for` divide by zero); the clamp is unreachable for real images,
/// which need `n <= 31` unless a tile axis is `1` and the image exceeds `2^31` px.
/// This is the single source of truth feeding both `scaleFactors` and `sizes`, so
/// the two arrays describe the same pyramid. Assumes `tile_w`/`tile_h >= 1` (the
/// caller substitutes `DEFAULT_TILE_SIZE` for untiled images).
fn pyramid_scale_factors(width: u32, height: u32, tile_w: u32, tile_h: u32) -> Vec<u32> {
    let levels = |dim: u32, tile: u32| -> u32 {
        let mut n = 0u32;
        while (u64::from(tile) << n) < u64::from(dim) {
            n += 1;
        }
        n
    };
    let n = levels(width, tile_w).max(levels(height, tile_h)).min(31);
    (0..=n).map(|i| 1u32 << i).collect()
}

/// `sizes` for an ascending scale-factor list, emitted ascending (smallest →
/// native), native size included. A pure transform of the factor list, so
/// `sizes` and `scaleFactors` cannot describe different pyramids.
fn sizes_for(width: u32, height: u32, scale_factors: &[u32]) -> Vec<Value> {
    scale_factors
        .iter()
        .rev()
        .map(|&sf| json!({ "width": width.div_ceil(sf), "height": height.div_ceil(sf) }))
        .collect()
}

/// IIIF `info.json` for an image. The auth-service
/// block (preflight-driven) is added by the handler. A size-restricted image
/// (`size_restricted`, `dims` already its restricted (RW, RH)) also states
/// `maxWidth`/`maxHeight` = (RW, RH): the engine serves nothing larger, so
/// `^max` equals `max` (IIIF Image API 3.0 §5.2).
#[must_use]
pub fn image_info_json(id: &str, dims: &SipiImageDims, size_restricted: bool) -> Value {
    let mut root = Map::new();
    root.insert("@context".into(), json!(IMAGE_CONTEXT));
    root.insert("id".into(), json!(id));
    root.insert("type".into(), json!("ImageService3"));
    root.insert("protocol".into(), json!("http://iiif.io/api/image"));
    root.insert("profile".into(), json!("level2"));
    root.insert("width".into(), json!(dims.width));
    root.insert("height".into(), json!(dims.height));
    if size_restricted {
        root.insert("maxWidth".into(), json!(dims.width));
        root.insert("maxHeight".into(), json!(dims.height));
    }
    if dims.numpages > 0 {
        root.insert("numpages".into(), json!(dims.numpages));
    }
    // Derive one pyramid from the tile grid and feed both arrays from it, so
    // `sizes` and `scaleFactors` can never drift apart. Untiled images fall back
    // to a reference tile size. Depth comes from the tile grid, not a
    // resolution-level count.
    let (tw, th) = if dims.tile_width > 0 && dims.tile_height > 0 {
        (dims.tile_width, dims.tile_height)
    } else {
        (DEFAULT_TILE_SIZE, DEFAULT_TILE_SIZE)
    };
    let scale_factors = pyramid_scale_factors(dims.width, dims.height, tw, th);
    root.insert(
        "sizes".into(),
        json!(sizes_for(dims.width, dims.height, &scale_factors)),
    );
    if dims.tile_width > 0 && dims.tile_height > 0 {
        root.insert(
            "tiles".into(),
            json!([{ "width": dims.tile_width, "height": dims.tile_height, "scaleFactors": scale_factors }]),
        );
    }
    root.insert("extraFormats".into(), json!(["tif", "jp2"]));
    root.insert(
        "preferredFormats".into(),
        json!(["jpg", "tif", "jp2", "png"]),
    );
    root.insert("extraFeatures".into(), json!(EXTRA_FEATURES));
    Value::Object(root)
}

/// The Bitstream Information document (`info.json` for a non-image resource):
/// the SIPI file context, the id, the detected MIME type, and the byte size.
#[must_use]
pub fn bitstream_info_json(id: &str, mime: &str, file_size: u64) -> Value {
    json!({
        "@context": FILE_CONTEXT,
        "id": id,
        "internalMimeType": mime,
        "fileSize": file_size,
    })
}

/// The optional `.info` sidecar SIPI writes next to a derivative.
/// All fields are optional; a missing or
/// unparseable sidecar yields the default (everything `None`).
#[derive(Debug, Default, Clone)]
pub struct Sidecar {
    pub original_filename: Option<String>,
    pub checksum_original: Option<String>,
    pub checksum_derivative: Option<String>,
    pub duration: Option<f64>,
    pub fps: Option<f64>,
    pub height: Option<f64>,
    pub width: Option<f64>,
}

impl Sidecar {
    /// Parse a `.info` JSON document, tolerating missing keys and wrong types
    /// (matching the C++ key-by-key extraction). Invalid JSON → an empty sidecar.
    #[must_use]
    pub fn parse(text: &str) -> Self {
        let Ok(Value::Object(map)) = serde_json::from_str::<Value>(text) else {
            return Self::default();
        };
        let string = |k: &str| map.get(k).and_then(Value::as_str).map(str::to_owned);
        let number = |k: &str| map.get(k).and_then(Value::as_f64);
        Self {
            original_filename: string("originalFilename"),
            checksum_original: string("checksumOriginal"),
            checksum_derivative: string("checksumDerivative"),
            duration: number("duration"),
            fps: number("fps"),
            height: number("height"),
            width: number("width"),
        }
    }
}

/// The common `knora.json` prelude: context, id, and the sidecar checksums
/// (emitted for every file type).
fn knora_base(id: &str, sidecar: &Sidecar) -> Map<String, Value> {
    let mut root = Map::new();
    root.insert("@context".into(), json!(FILE_CONTEXT));
    root.insert("id".into(), json!(id));
    if let Some(c) = &sidecar.checksum_original {
        root.insert("checksumOriginal".into(), json!(c));
    }
    if let Some(c) = &sidecar.checksum_derivative {
        root.insert("checksumDerivative".into(), json!(c));
    }
    root
}

/// `knora.json` for an image. `originalMimeType` /
/// `originalFilename` come from the image's embedded Essentials packet
/// (`essentials`, via `sipi_image_essentials`) — present together iff
/// `read_shape` reports one (`success == ALL`), absent together otherwise (a
/// plain JPEG/PNG or a packet-less TIFF/JP2). Unlike the video/generic paths,
/// these do NOT come from the `.info` sidecar — SIPI sources image identity
/// from the embedded packet only.
#[must_use]
pub fn image_knora_json(
    id: &str,
    mime: &str,
    dims: &SipiImageDims,
    sidecar: &Sidecar,
    essentials: Option<&ImageEssentials>,
) -> Value {
    let mut root = knora_base(id, sidecar);
    root.insert("width".into(), json!(dims.width));
    root.insert("height".into(), json!(dims.height));
    if dims.numpages > 0 {
        root.insert("numpages".into(), json!(dims.numpages));
    }
    root.insert("internalMimeType".into(), json!(mime));
    if let Some(e) = essentials {
        root.insert("originalMimeType".into(), json!(e.original_mimetype));
        root.insert("originalFilename".into(), json!(e.original_filename));
    }
    Value::Object(root)
}

/// `knora.json` for `video/mp4`: MIME + size, plus
/// the sidecar's filename and media metrics (each emitted only when present and
/// non-negative, as JSON reals).
#[must_use]
pub fn video_knora_json(id: &str, mime: &str, file_size: u64, sidecar: &Sidecar) -> Value {
    let mut root = knora_base(id, sidecar);
    root.insert("internalMimeType".into(), json!(mime));
    root.insert("fileSize".into(), json!(file_size));
    if let Some(name) = sidecar
        .original_filename
        .as_deref()
        .filter(|s| !s.is_empty())
    {
        root.insert("originalFilename".into(), json!(name));
    }
    for (key, value) in [
        ("duration", sidecar.duration),
        ("fps", sidecar.fps),
        ("height", sidecar.height),
        ("width", sidecar.width),
    ] {
        if let Some(v) = value.filter(|v| *v >= 0.0) {
            root.insert(key.into(), json!(v));
        }
    }
    Value::Object(root)
}

/// `knora.json` for any other file: MIME, size, and
/// the original filename (empty string when there is no sidecar).
#[must_use]
pub fn generic_knora_json(id: &str, mime: &str, file_size: u64, sidecar: &Sidecar) -> Value {
    let mut root = knora_base(id, sidecar);
    root.insert("internalMimeType".into(), json!(mime));
    root.insert("fileSize".into(), json!(file_size));
    root.insert(
        "originalFilename".into(),
        json!(sidecar.original_filename.clone().unwrap_or_default()),
    );
    Value::Object(root)
}

/// Whether a permission requires the IIIF Authentication service block (and a
/// 401 response). `allow` / `restrict` / `deny` do not.
#[must_use]
pub fn is_auth_type(permission: SipiPermType) -> bool {
    matches!(
        permission,
        SipiPermType::Login
            | SipiPermType::Clickthrough
            | SipiPermType::Kiosk
            | SipiPermType::External
    )
}

/// The preflight keys copied into the auth-service block: the IIIF Auth API v1
/// description properties. Every other key stays out of the public 401 body,
/// so a hook value meant for the shell (`size`, `watermark`, `infile`) or a
/// key that would override a built field (`@id`, `profile`, `service`) cannot
/// reach it.
const AUTH_SERVICE_KEYS: [&str; 6] = [
    "label",
    "header",
    "description",
    "confirmLabel",
    "failureHeader",
    "failureDescription",
];

/// Build the IIIF Auth API v1 `service` object for an auth-type info.json.
/// Requires `cookieUrl` (and `tokenUrl`); a
/// missing required key → `None` (a 500). `@context`, `@id`, `profile` and the
/// nested `service` are built here; of the kv pairs only [`AUTH_SERVICE_KEYS`]
/// are copied. `logoutUrl` is optional.
pub fn auth_service(permission: SipiPermType, kv: &[(String, String)]) -> Option<Value> {
    let get = |key: &str| kv.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str());
    let profile = match permission {
        SipiPermType::Login => "http://iiif.io/api/auth/1/login",
        SipiPermType::Clickthrough => "http://iiif.io/api/auth/1/clickthrough",
        SipiPermType::Kiosk => "http://iiif.io/api/auth/1/kiosk",
        SipiPermType::External => "http://iiif.io/api/auth/1/external",
        _ => return None,
    };
    let cookie_url = get("cookieUrl")?;
    let token_url = get("tokenUrl")?;

    let mut service = Map::new();
    service.insert(
        "@context".into(),
        json!("http://iiif.io/api/auth/1/context.json"),
    );
    service.insert("@id".into(), json!(cookie_url));
    service.insert("profile".into(), json!(profile));
    for (k, v) in kv {
        if AUTH_SERVICE_KEYS.contains(&k.as_str()) {
            service.insert(k.clone(), json!(v));
        } else if !matches!(k.as_str(), "cookieUrl" | "tokenUrl" | "logoutUrl") {
            tracing::debug!(key = %k, "preflight key left out of the auth-service block");
        }
    }
    let mut sub = vec![json!({ "@id": token_url, "profile": "http://iiif.io/api/auth/1/token" })];
    if let Some(logout) = get("logoutUrl") {
        sub.push(json!({ "@id": logout, "profile": "http://iiif.io/api/auth/1/logout" }));
    }
    service.insert("service".into(), json!(sub));
    Some(Value::Object(service))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dims(width: u32, height: u32, tile: u32) -> SipiImageDims {
        SipiImageDims {
            width,
            height,
            numpages: 0,
            tile_width: tile,
            tile_height: tile,
        }
    }

    #[test]
    fn info_json_matches_lena512_golden() {
        // The exact shape the e2e golden snapshot pins (iiif_compliance__info-json-lena512):
        // 512x512, tile 512 → the whole image already fits one tile, so there is no
        // pyramid: scaleFactors [1] and sizes [{512,512}] (native only).
        let v = image_info_json("http://h/unit/lena512.jp2", &dims(512, 512, 512), false);
        assert_eq!(v["type"], "ImageService3");
        assert_eq!(v["protocol"], "http://iiif.io/api/image");
        assert_eq!(v["profile"], "level2");
        assert_eq!(v["width"], 512);
        assert_eq!(v["height"], 512);
        assert_eq!(v["sizes"], json!([{ "width": 512, "height": 512 }]));
        assert_eq!(v["tiles"][0]["width"], 512);
        assert_eq!(v["tiles"][0]["scaleFactors"], json!([1]));
        assert_eq!(v["extraFeatures"].as_array().unwrap().len(), 17);
        assert_eq!(v["extraFormats"], json!(["tif", "jp2"]));
        assert!(v.get("numpages").is_none());
        assert!(v.get("maxWidth").is_none());
        assert!(v.get("maxHeight").is_none());
    }

    #[test]
    fn size_restricted_image_states_its_max_size() {
        // A restricted (RW, RH) of 96x128 arrives untiled: one sizes entry,
        // and maxWidth/maxHeight bound every `^` request at full/max.
        let v = image_info_json("http://h/id", &dims(96, 128, 0), true);
        assert_eq!(v["width"], 96);
        assert_eq!(v["height"], 128);
        assert_eq!(v["maxWidth"], 96);
        assert_eq!(v["maxHeight"], 128);
        assert_eq!(v["sizes"], json!([{ "width": 96, "height": 128 }]));
        assert!(v.get("tiles").is_none());
    }

    #[test]
    fn untiled_image_omits_tiles() {
        // tile_width == 0 → derive the sizes ladder from DEFAULT_TILE_SIZE (512),
        // tiles stays omitted. 1000x800 / 512 → scaleFactors [1,2], so
        // sizes ascending are [{500,400},{1000,800}].
        let v = image_info_json("http://h/id", &dims(1000, 800, 0), false);
        assert!(v.get("tiles").is_none());
        assert_eq!(
            v["sizes"],
            json!([{ "width": 500, "height": 400 }, { "width": 1000, "height": 800 }])
        );
    }

    #[test]
    fn descriptor_conformance() {
        // Each case pins scaleFactors and sizes as literals (not a re-derivation of
        // sizes_for's own formula) so a shared-formula bug can't pass. sizes are
        // ascending (smallest → native), one per scale factor.
        struct Case {
            w: u32,
            h: u32,
            tw: u32,
            th: u32,
            sf: &'static [u32],
            sizes: &'static [(u32, u32)],
        }
        let cases = [
            // deep pyramid
            Case {
                w: 3505,
                h: 5156,
                tw: 1024,
                th: 1024,
                sf: &[1, 2, 4, 8],
                sizes: &[(439, 645), (877, 1289), (1753, 2578), (3505, 5156)],
            },
            // single tile, no pyramid
            Case {
                w: 512,
                h: 512,
                tw: 512,
                th: 512,
                sf: &[1],
                sizes: &[(512, 512)],
            },
            // non-square tiles: levels_h (256px axis) drives depth, not levels_w
            Case {
                w: 4096,
                h: 4096,
                tw: 1024,
                th: 256,
                sf: &[1, 2, 4, 8, 16],
                sizes: &[
                    (256, 256),
                    (512, 512),
                    (1024, 1024),
                    (2048, 2048),
                    (4096, 4096),
                ],
            },
        ];
        for c in cases {
            let d = SipiImageDims {
                width: c.w,
                height: c.h,
                numpages: 0,
                tile_width: c.tw,
                tile_height: c.th,
            };
            let v = image_info_json("http://h/id", &d, false);

            // §5.4: exactly one tile object; width/height are the stored tile
            // size, passed through unchanged.
            let tiles = v["tiles"].as_array().unwrap();
            assert_eq!(tiles.len(), 1, "single tile object (§5.4)");
            assert_eq!(tiles[0]["width"], c.tw);
            assert_eq!(tiles[0]["height"], c.th);

            // §5.4 uniqueness: the exact power-of-two, ascending, once-each
            // factor list.
            assert_eq!(
                tiles[0]["scaleFactors"],
                json!(c.sf),
                "scaleFactors {}x{}/{}x{}",
                c.w,
                c.h,
                c.tw,
                c.th
            );

            // The same pyramid: one size per factor, ascending, native included.
            let expected_sizes: Vec<Value> = c
                .sizes
                .iter()
                .map(|&(w, h)| json!({ "width": w, "height": h }))
                .collect();
            assert_eq!(
                v["sizes"],
                json!(expected_sizes),
                "sizes {}x{}/{}x{}",
                c.w,
                c.h,
                c.tw,
                c.th
            );
            assert_eq!(
                c.sizes.len(),
                c.sf.len(),
                "sizes.length == scaleFactors.length"
            );

            // The deepest scale factor puts the whole image within a single tile
            // on both axes.
            let deepest = *c.sf.last().unwrap();
            assert!(
                c.w.div_ceil(deepest) <= c.tw,
                "image fits one tile horizontally"
            );
            assert!(
                c.h.div_ceil(deepest) <= c.th,
                "image fits one tile vertically"
            );
        }
    }

    #[test]
    fn pyramid_exponent_clamped_no_panic() {
        // Degenerate FFI input: a 1px tile axis with an image wider than 2^31 drives
        // the exponent to 32, where 2^32 would truncate to 0 and make sizes_for
        // divide by zero. The clamp to 31 keeps every factor a valid nonzero u32.
        let d = SipiImageDims {
            width: u32::MAX,
            height: 512,
            numpages: 0,
            tile_width: 1,
            tile_height: 512,
        };
        let v = image_info_json("http://h/id", &d, false);
        let sf = v["tiles"][0]["scaleFactors"].as_array().unwrap();
        assert_eq!(sf.len(), 32, "exponent clamped to 31 → 2^0..2^31");
        assert!(
            sf.iter().all(|f| f.as_u64().unwrap() > 0),
            "no factor truncated to 0"
        );
        assert_eq!(
            v["sizes"].as_array().unwrap().len(),
            sf.len(),
            "sizes stay in step with the clamped factor list"
        );
    }

    #[test]
    fn bitstream_info_json_shape() {
        let v = bitstream_info_json("http://h/doc.pdf", "application/pdf", 1234);
        assert_eq!(v["@context"], FILE_CONTEXT);
        assert_eq!(v["internalMimeType"], "application/pdf");
        assert_eq!(v["fileSize"], 1234);
    }

    #[test]
    fn sidecar_parse_extracts_known_keys() {
        let s = Sidecar::parse(
            r#"{"originalFilename":"Dummy.mp4","fps":30,"width":320,"height":240,"duration":4.7,"checksumOriginal":"abc"}"#,
        );
        assert_eq!(s.original_filename.as_deref(), Some("Dummy.mp4"));
        assert_eq!(s.fps, Some(30.0));
        assert_eq!(s.checksum_original.as_deref(), Some("abc"));
        // Invalid JSON → empty.
        assert!(Sidecar::parse("not json").original_filename.is_none());
    }

    #[test]
    fn video_knora_json_from_sidecar() {
        let s =
            Sidecar::parse(r#"{"originalFilename":"Dummy.mp4","fps":30,"width":320,"height":240}"#);
        let v = video_knora_json("http://h/v.mp4", "video/mp4", 999, &s);
        assert_eq!(v["internalMimeType"], "video/mp4");
        assert_eq!(v["fileSize"], 999);
        assert_eq!(v["originalFilename"], "Dummy.mp4");
        assert_eq!(v["width"].as_f64().unwrap() as i64, 320);
        assert_eq!(v["fps"].as_f64().unwrap() as i64, 30);
    }

    #[test]
    fn image_knora_json_required_fields() {
        let v = image_knora_json(
            "http://h/i.jp2",
            "image/jp2",
            &dims(512, 512, 512),
            &Sidecar::default(),
            None,
        );
        assert_eq!(v["@context"], FILE_CONTEXT);
        assert_eq!(v["width"], 512);
        assert_eq!(v["internalMimeType"], "image/jp2");
        // No Essentials packet → neither identity field is emitted.
        assert!(v.get("originalMimeType").is_none());
        assert!(v.get("originalFilename").is_none());
    }

    #[test]
    fn image_knora_json_with_essentials() {
        let essentials = ImageEssentials {
            original_mimetype: "image/tiff".to_string(),
            original_filename: "lena512.tif".to_string(),
        };
        let v = image_knora_json(
            "http://h/i.jp2",
            "image/jp2",
            &dims(512, 512, 512),
            &Sidecar::default(),
            Some(&essentials),
        );
        assert_eq!(v["originalMimeType"], "image/tiff");
        assert_eq!(v["originalFilename"], "lena512.tif");
    }

    #[test]
    fn auth_service_block_for_login() {
        let kv = vec![
            ("cookieUrl".to_string(), "https://auth/cookie".to_string()),
            ("tokenUrl".to_string(), "https://auth/token".to_string()),
            ("logoutUrl".to_string(), "https://auth/logout".to_string()),
            ("infile".to_string(), "/srv/x.jp2".to_string()),
        ];
        let svc = auth_service(SipiPermType::Login, &kv).expect("login service");
        assert_eq!(svc["@context"], "http://iiif.io/api/auth/1/context.json");
        assert_eq!(svc["@id"], "https://auth/cookie");
        assert_eq!(svc["profile"], "http://iiif.io/api/auth/1/login");
        // infile/cookieUrl/tokenUrl/logoutUrl are structural, not passed through.
        assert!(svc.get("infile").is_none());
        let sub = svc["service"].as_array().unwrap();
        assert_eq!(sub[0]["profile"], "http://iiif.io/api/auth/1/token");
        assert_eq!(sub[1]["profile"], "http://iiif.io/api/auth/1/logout");
    }

    #[test]
    fn auth_service_copies_only_the_description_keys() {
        let mut kv: Vec<(String, String)> = [
            ("cookieUrl", "https://auth/cookie"),
            ("tokenUrl", "https://auth/token"),
            ("size", "!512,512"),
            ("watermark", "/srv/wm.tif"),
            ("secret", "s3cr3t"),
            ("type", "login"),
            ("@id", "https://evil/id"),
            ("@context", "https://evil/context"),
            ("profile", "https://evil/profile"),
            ("service", "https://evil/service"),
        ]
        .iter()
        .map(|(k, v)| ((*k).to_owned(), (*v).to_owned()))
        .collect();
        let described = [
            "label",
            "header",
            "description",
            "confirmLabel",
            "failureHeader",
            "failureDescription",
        ];
        for key in described {
            kv.push((key.to_owned(), format!("{key} text")));
        }
        let svc = auth_service(SipiPermType::Login, &kv).expect("login service");
        let obj = svc.as_object().unwrap();
        for key in described {
            assert_eq!(svc[key], format!("{key} text"), "{key}");
        }
        for key in [
            "size",
            "watermark",
            "secret",
            "type",
            "cookieUrl",
            "tokenUrl",
        ] {
            assert!(obj.get(key).is_none(), "{key} leaked");
        }
        assert_eq!(svc["@id"], "https://auth/cookie");
        assert_eq!(svc["@context"], "http://iiif.io/api/auth/1/context.json");
        assert_eq!(svc["profile"], "http://iiif.io/api/auth/1/login");
        assert_eq!(svc["service"][0]["@id"], "https://auth/token");
        // The four built fields plus the six description keys.
        assert_eq!(obj.len(), 4 + described.len());
    }

    #[test]
    fn auth_service_missing_cookie_url_errors() {
        assert!(auth_service(SipiPermType::Login, &[]).is_none());
        assert!(!is_auth_type(SipiPermType::Allow));
        assert!(is_auth_type(SipiPermType::Kiosk));
    }
}
