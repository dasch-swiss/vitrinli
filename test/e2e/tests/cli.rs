use insta::assert_snapshot;
use sipi_e2e::{
    apply_sanitizer_env, cli_convert, cli_run, cli_tmp_path, exif_app1_with_user_comment,
    jpeg_with_app_segment, repo_root, sipi_bin_path, test_data_dir,
};
use std::process::Command;

// =============================================================================
// Lightweight CLI-mode tests (version, quality plumbing, help snapshot, query,
// compare, watermark). The heavy Kakadu JP2 conversions — decodes, round-trips,
// format-fidelity, and the verify pipeline — run in the sibling `cli_conversions`
// target, which fans them out in parallel (see `tests/cli_conversions.rs`).
// =============================================================================

#[test]
fn cli_version_flag() {
    let sipi_bin = sipi_bin_path();

    let result = Command::new(&sipi_bin)
        .arg("--version")
        .output()
        .unwrap_or_else(|e| panic!("Failed to run sipi --version: {}", e));

    assert!(
        result.status.success(),
        "sipi --version exited non-zero: {}",
        String::from_utf8_lossy(&result.stderr)
    );

    let expected_version = std::fs::read_to_string(repo_root().join("version.txt"))
        .expect("read version.txt")
        .trim()
        .to_string();
    let stdout = String::from_utf8_lossy(&result.stdout);
    // A stamped build (`just bazel-test-e2e` / `bazel-coverage`, CI) bakes
    // `STABLE_SIPI_VERSION` from version.txt. A plain `bazel test //...` is
    // unstamped, so `expand_template` in src/BUILD.bazel falls back to
    // `0.0.0-unstamped`. Accept either: the unstamped run still verifies the
    // `--version` plumbing and output format, while the stamped CI run pins
    // the actual version.txt value.
    let stamped = format!("sipi {}", expected_version);
    let unstamped = "sipi 0.0.0-unstamped";
    assert!(
        stdout.trim() == stamped || stdout.trim() == unstamped,
        "expected stdout to be {:?} (stamped) or {:?} (unstamped), got {:?}",
        stamped,
        unstamped,
        stdout
    );
}

// =============================================================================
// `convert -q/--quality` value plumbing (regression).
//
// `SipiCompressionParams` is `unordered_map<int, std::string>`. The CLI quality
// option (`int optJpegQuality`) must be stringified before it lands in that map;
// assigning the int directly bound to `std::string::operator=(char)`, storing
// the byte 0x50 ('P') so the JPEG writer's `stoi()` threw "JPEG quality argument
// must be integer between 0 and 100" for EVERY `convert -q ...` invocation.
//
// These tests run the real binary through the bare `convert` path (Access File
// output via the string-map params — distinct from `convert access-file`, which
// threads an int field). They guard both failure modes a mis-threaded numeric
// option can take: erroring out, and being silently ignored.
// =============================================================================

fn sipi_convert_quality(input: &str, output: &str, quality: u32) -> std::process::Output {
    let mut cmd = Command::new(sipi_bin_path());
    cmd.arg("convert")
        .arg("--quality")
        .arg(quality.to_string())
        .arg("--format")
        .arg("jpg")
        .arg(input)
        .arg(output)
        .current_dir(test_data_dir());
    apply_sanitizer_env(&mut cmd);
    cmd.output()
        .unwrap_or_else(|e| panic!("Failed to run sipi CLI: {}", e))
}

#[test]
fn cli_convert_quality_succeeds_and_emits_jpeg() {
    let input = test_data_dir().join("images/unit/lena512.tif");
    let output = cli_tmp_path("sipi_cli_quality_ok.jpg");
    let _ = std::fs::remove_file(&output);

    let result = sipi_convert_quality(input.to_str().unwrap(), output.to_str().unwrap(), 80);

    assert!(
        result.status.success(),
        "`convert --quality 80` must succeed; stderr:\n{}",
        String::from_utf8_lossy(&result.stderr)
    );
    assert!(
        output.exists(),
        "quality conversion should emit an output file"
    );

    // Valid JPEG: SOI marker 0xFFD8 followed by 0xFF.
    let bytes = std::fs::read(&output).expect("read output jpeg");
    assert!(
        bytes.len() > 2 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF,
        "output should be a valid JPEG (SOI marker), got first bytes {:?}",
        &bytes[..bytes.len().min(3)]
    );

    let _ = std::fs::remove_file(&output);
}

#[test]
fn cli_convert_quality_actually_affects_output() {
    // The strongest guard: the quality value must reach the encoder, not just
    // avoid erroring. A low-quality encode must be meaningfully smaller than a
    // high-quality one — false if the option is dropped or mis-threaded.
    let input = test_data_dir().join("images/unit/lena512.tif");
    let low = cli_tmp_path("sipi_cli_quality_low.jpg");
    let high = cli_tmp_path("sipi_cli_quality_high.jpg");
    let _ = std::fs::remove_file(&low);
    let _ = std::fs::remove_file(&high);

    let r_low = sipi_convert_quality(input.to_str().unwrap(), low.to_str().unwrap(), 10);
    let r_high = sipi_convert_quality(input.to_str().unwrap(), high.to_str().unwrap(), 95);

    assert!(
        r_low.status.success() && r_high.status.success(),
        "both quality conversions must succeed; low stderr:\n{}\nhigh stderr:\n{}",
        String::from_utf8_lossy(&r_low.stderr),
        String::from_utf8_lossy(&r_high.stderr)
    );

    let low_size = std::fs::metadata(&low).expect("low jpeg").len();
    let high_size = std::fs::metadata(&high).expect("high jpeg").len();
    assert!(
        low_size < high_size,
        "quality must affect output: -q 10 ({low_size} B) should be smaller than -q 95 ({high_size} B)"
    );

    let _ = std::fs::remove_file(&low);
    let _ = std::fs::remove_file(&high);
}

// =============================================================================
// `sipi server --help` snapshot.
//
// `ServerArgs` composes its ~40 flags from eight `#[command(flatten)]` groups
// (Network/Concurrency/Limits/Paths/Cache/TLS & Auth/Knora/Logging); clap
// renders each group under its own `next_help_heading` in
// declaration order. This snapshot locks that heading order (and the full
// rendered surface) against silent drift from a reordered/renamed group.
// `term_width = 0` (`args/mod.rs`) fixes the wrap width so the snapshot is
// stable across CI and local terminal widths.
//
// The spawn strips every `SIPI_` var from the child env: clap renders each
// bound var's *value* in its `[env: NAME=value]` annotation, so a `SIPI_*` var
// set in the caller's shell (e.g. a dev's `SIPI_LOGLEVEL`) would corrupt the
// capture. A prefix strip (not a full `env_clear`, which would also drop the
// loader/runfiles vars the binary needs to start) covers every clap-bound flag.
// =============================================================================

#[test]
fn sipi_server_help_heading_order() {
    let mut cmd = Command::new(sipi_bin_path());
    cmd.args(["server", "--help"]);
    for (key, _) in std::env::vars() {
        if key.starts_with("SIPI_") {
            cmd.env_remove(key);
        }
    }
    let result = cmd
        .output()
        .unwrap_or_else(|e| panic!("Failed to run sipi server --help: {}", e));

    assert!(
        result.status.success(),
        "sipi server --help exited non-zero: {}",
        String::from_utf8_lossy(&result.stderr)
    );

    let stdout = String::from_utf8_lossy(&result.stdout);
    assert_snapshot!("sipi-server-help", stdout);
}

// =============================================================================
// The offline verbs `query` and `compare`. Both delegate to the C++ CLI
// (`sipi_cli_main`), so this is plain e2e coverage. (`verify` runs in the
// `cli_conversions` batch; `convert`'s `-w/--watermark` lives below.)
// =============================================================================

#[test]
fn cli_query_dumps_image_info() {
    // `sipi query <file>` streams SipiImage::operator<<, a fixed-field text
    // dump (SipiImage.cpp) — not a --json contract. lena512 is a known 512x512
    // fixture (see info_json_dimensions_match_lena512 in iiif_compliance.rs).
    let result = cli_run(&["query", "images/unit/lena512.tif"]);
    assert!(
        result.status.success(),
        "sipi query must succeed; stderr:\n{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let stdout = String::from_utf8_lossy(&result.stdout);
    assert!(
        stdout.contains("SipiImage with the following parameters"),
        "expected the SipiImage dump header, got:\n{stdout}"
    );
    assert!(
        stdout.contains("nx    = 512") && stdout.contains("ny    = 512"),
        "expected nx/ny = 512 for the lena512 fixture, got:\n{stdout}"
    );
}

#[test]
fn cli_query_prints_ucs2_user_comment() {
    // Photoshop writes Exif.Photo.UserComment as charset `Unicode` in UCS-2LE.
    // Printing it needs a charset conversion; `sipi query` must print the
    // converted text and exit 0.
    let source = std::fs::read(test_data_dir().join("images/unit/MaoriFigure.jpg"))
        .expect("read MaoriFigure.jpg");
    let mut comment = b"UNICODE\0".to_vec();
    for unit in "Grüße".encode_utf16() {
        comment.extend_from_slice(&unit.to_le_bytes());
    }
    let jpeg = jpeg_with_app_segment(&source, 0xE1, &exif_app1_with_user_comment(&comment));
    let path = cli_tmp_path("sipi_cli_query_ucs2_user_comment.jpg");
    std::fs::write(&path, jpeg).expect("write test JPEG");

    let result = cli_run(&["query", path.to_str().unwrap()]);
    let _ = std::fs::remove_file(&path);
    let stdout = String::from_utf8_lossy(&result.stdout);
    assert!(
        result.status.success(),
        "sipi query must succeed on a UCS-2LE UserComment; stderr:\n{}",
        String::from_utf8_lossy(&result.stderr)
    );
    let line = stdout
        .lines()
        .find(|l| l.starts_with("Exif.Photo.UserComment"))
        .unwrap_or_else(|| panic!("no UserComment line in:\n{stdout}"));
    assert!(
        line.ends_with("charset=Unicode Grüße"),
        "expected the converted comment, got: {line}"
    );
}

#[test]
fn cli_compare_identical_files_reports_match() {
    // Comparing a file against itself: img1 == img2, exit 0 ("Files identical!").
    let result = cli_run(&[
        "compare",
        "images/unit/lena512.tif",
        "images/unit/lena512.tif",
    ]);
    assert!(
        result.status.success(),
        "comparing a file to itself must succeed; stderr:\n{}",
        String::from_utf8_lossy(&result.stderr)
    );
}

#[test]
fn cli_compare_differing_files_reports_mismatch() {
    // Two genuinely different images: run_compare returns -1 (truncates to a
    // non-zero exit code on the wire) and writes diff.tif — assert only the
    // non-zero contract, not the platform-specific truncated value.
    let result = cli_run(&[
        "compare",
        "images/unit/lena512.tif",
        "images/unit/mario.tif",
    ]);
    assert!(
        !result.status.success(),
        "comparing two different images must NOT report success"
    );
}

#[test]
fn cli_convert_watermark_changes_output_bytes() {
    // `-w/--watermark` on the bare `convert` verb (attach_generic_transform_opts,
    // cli_app.cpp) is implemented but had no e2e coverage. Compare a plain
    // convert against a watermarked one of the same input/format: the bytes
    // must differ, and the watermarked output must still be a valid JPEG.
    let input = test_data_dir().join("images/unit/lena512.tif");
    let watermark = test_data_dir().join("images/unit/watermark_correct.tif");
    let plain = cli_tmp_path("sipi_cli_watermark_plain.jpg");
    let watermarked = cli_tmp_path("sipi_cli_watermark_applied.jpg");
    let _ = std::fs::remove_file(&plain);
    let _ = std::fs::remove_file(&watermarked);

    let r_plain = cli_convert(input.to_str().unwrap(), plain.to_str().unwrap(), "jpg");
    assert!(
        r_plain.status.success(),
        "plain convert must succeed; stderr:\n{}",
        String::from_utf8_lossy(&r_plain.stderr)
    );

    let mut r_wm_cmd = Command::new(sipi_bin_path());
    r_wm_cmd
        .arg("convert")
        .arg("--watermark")
        .arg(watermark.to_str().unwrap())
        .arg("--format")
        .arg("jpg")
        .arg(input.to_str().unwrap())
        .arg(watermarked.to_str().unwrap())
        .current_dir(test_data_dir());
    apply_sanitizer_env(&mut r_wm_cmd);
    let r_wm = r_wm_cmd
        .output()
        .unwrap_or_else(|e| panic!("Failed to run sipi CLI: {}", e));
    assert!(
        r_wm.status.success(),
        "watermarked convert must succeed; stderr:\n{}",
        String::from_utf8_lossy(&r_wm.stderr)
    );

    let plain_bytes = std::fs::read(&plain).expect("read plain output");
    let wm_bytes = std::fs::read(&watermarked).expect("read watermarked output");
    assert!(
        wm_bytes.len() > 2 && wm_bytes[0] == 0xFF && wm_bytes[1] == 0xD8,
        "watermarked output should be a valid JPEG"
    );
    assert_ne!(
        plain_bytes, wm_bytes,
        "a watermarked convert must produce different bytes than a plain convert"
    );

    let _ = std::fs::remove_file(&plain);
    let _ = std::fs::remove_file(&watermarked);
}
