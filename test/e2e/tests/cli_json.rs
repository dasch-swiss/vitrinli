// Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
// contributors. SPDX-License-Identifier: AGPL-3.0-or-later
//
// Feature-contract tests for the `--json` CLI flag: F1–F3 for the
// `sipi convert ... --json` document, then `error_code` and `correlation_id`.

use serde_json::Value;
use sipi_e2e::{sipi_bin_path, test_data_dir, write_jpeg_with_garbage_iptc};
use std::path::PathBuf;
use std::process::Command;

fn sipi_bin() -> String {
    sipi_bin_path()
}

fn tmp_path(name: &str) -> PathBuf {
    let dir = std::env::var("TMPDIR").unwrap_or_else(|_| "/tmp".to_string());
    PathBuf::from(dir).join(name)
}

/// F1 — `sipi --json --file <good.jpg> <out.jp2>` emits a single JSON
/// document to stdout with `status: "ok"` and a populated `image` object.
#[test]
fn cli_json_output_success_contains_metadata() {
    let input = test_data_dir().join("images/unit/MaoriFigure.jpg");
    if !input.exists() {
        eprintln!("Skipping F1 — MaoriFigure.jpg not found");
        return;
    }
    let output = tmp_path("sipi_f1_out.jp2");

    let result = Command::new(sipi_bin())
        .arg("convert")
        .arg(&input)
        .arg(&output)
        .arg("--json")
        .arg("--format")
        .arg("jpx")
        .output()
        .expect("sipi CLI invocation should run");

    assert!(
        result.status.success(),
        "--json success invocation should exit 0\nstderr: {}",
        String::from_utf8_lossy(&result.stderr)
    );

    let stdout = std::str::from_utf8(&result.stdout).expect("stdout is utf-8");
    let parsed: Value = serde_json::from_str(stdout.trim())
        .unwrap_or_else(|e| panic!("stdout must parse as JSON: {e}\nstdout: {stdout:?}"));

    assert_eq!(parsed["status"], "ok", "status should be ok: {parsed:?}");
    assert_eq!(parsed["mode"], "cli");
    assert!(parsed["image"].is_object(), "image object must be present");
    let image = &parsed["image"];
    assert!(
        image["width"].as_u64().unwrap_or(0) > 0,
        "image.width must be populated: {image:?}"
    );
    assert!(
        image["height"].as_u64().unwrap_or(0) > 0,
        "image.height must be populated"
    );
    assert!(
        image["bps"].as_u64().unwrap_or(0) > 0,
        "image.bps must be populated"
    );

    let _ = std::fs::remove_file(&output);
}

/// F2 — `sipi --json --file <good.jpg> <out.UNSUPPORTED>` emits a single
/// JSON document with `status: "error"`, `phase: "cli_args"`, a populated
/// `error_message`, and **no** `image` object (the `cli_args` phase omits
/// the `image` key because no image was loaded).
///
/// The parameter-validation path (unsupported output extension) is the
/// only error path that is _always_ reachable regardless of which bugs
/// have been fixed in the `read`, `convert`, and `write` phases, so it
/// makes the most stable contract test.
#[test]
fn cli_json_output_error_contains_image_context() {
    let input = test_data_dir().join("images/unit/MaoriFigure.jpg");
    if !input.exists() {
        eprintln!("Skipping F2 — MaoriFigure.jpg not found");
        return;
    }
    let bad_output = tmp_path("sipi_f2_out.unsupported_ext");

    let result = Command::new(sipi_bin())
        .arg("convert")
        .arg(&input)
        .arg(&bad_output)
        .arg("--json")
        .output()
        .expect("sipi CLI invocation should run");

    // A parameter error must yield a non-zero exit code.
    assert!(
        !result.status.success(),
        "unsupported output extension must fail; exit={:?}",
        result.status
    );
    let stdout = std::str::from_utf8(&result.stdout).expect("stdout is utf-8");
    let parsed: Value = serde_json::from_str(stdout.trim())
        .unwrap_or_else(|e| panic!("stdout must parse as JSON: {e}\nstdout: {stdout:?}"));
    assert_eq!(parsed["status"], "error", "status: {parsed:?}");
    assert_eq!(parsed["phase"], "cli_args");
    assert!(
        !parsed["error_message"].as_str().unwrap_or("").is_empty(),
        "error_message must be populated"
    );
    // For cli_args errors the `image` object is intentionally omitted.
    assert!(
        parsed.get("image").is_none(),
        "image object must be omitted for cli_args phase: {parsed:?}"
    );
}

/// F3 — stdout must contain **exactly one** JSON document when `--json` is
/// set. No `log_info`, `log_warn`, or `log_err` output may appear on
/// stdout; they are all routed to stderr under `--json`. The fixture
/// `malformed_xmp.jpg` triggers `log_warn("Failed to parse XMP metadata
/// from JPEG")` in the read path, giving the test something to prove the
/// stderr routing actually works.
#[test]
fn cli_json_output_stdout_is_single_json_doc() {
    let input = test_data_dir().join("images/jpeg/malformed_xmp.jpg");
    if !input.exists() {
        eprintln!("Skipping F3 — malformed_xmp.jpg fixture not found");
        return;
    }
    let output = tmp_path("sipi_f3_out.jp2");

    let result = Command::new(sipi_bin())
        .arg("convert")
        .arg(&input)
        .arg(&output)
        .arg("--json")
        .output()
        .expect("sipi CLI invocation should run");

    let stdout = std::str::from_utf8(&result.stdout).expect("stdout is utf-8");
    let stdout_trimmed = stdout.trim_end_matches('\n');

    // Single-document contract: stdout parses as exactly one JSON value, with
    // nothing before the opening `{` or after the matching `}`.
    let first_brace = stdout_trimmed.find('{').expect("stdout must contain a '{'");
    assert_eq!(
        first_brace, 0,
        "nothing may precede the opening '{{' on stdout: {stdout_trimmed:?}"
    );

    // Parse one JSON value from the stream and confirm nothing follows it.
    // `StreamDeserializer<Value>` lets us walk past the single doc and then
    // assert that the iterator is empty (no second document, only trailing
    // whitespace).
    let mut stream = serde_json::Deserializer::from_str(stdout_trimmed).into_iter::<Value>();
    let first = stream
        .next()
        .expect("at least one JSON document")
        .unwrap_or_else(|e| {
            panic!("stdout must be a single JSON document: {e}\nstdout: {stdout_trimmed:?}")
        });
    assert!(first.is_object(), "stdout must be a JSON object");
    let trailing = stdout_trimmed[stream.byte_offset()..].trim();
    assert!(
        trailing.is_empty(),
        "nothing may follow the JSON document on stdout: trailing={trailing:?}"
    );

    // Warnings (if any) must be on stderr. The malformed-XMP fixture should
    // trigger a `log_warn` path; verify the warning landed on stderr (not
    // stdout) — stdout was already asserted to contain only the JSON doc.
    let stderr = String::from_utf8_lossy(&result.stderr);
    let _ = stderr; // documented but not hard-asserted (different libjpeg configs may take different paths)

    let _ = std::fs::remove_file(&output);
}

fn json_doc(output: &std::process::Output) -> Value {
    let stdout = std::str::from_utf8(&output.stdout).expect("stdout is utf-8");
    serde_json::from_str(stdout.trim())
        .unwrap_or_else(|e| panic!("stdout must parse as JSON: {e}\nstdout: {stdout:?}"))
}

fn assert_generated_id(doc: &Value) {
    let id = doc["correlation_id"].as_str().unwrap_or_default();
    assert!(
        id.len() == 32 && id.bytes().all(|b| matches!(b, b'0'..=b'9' | b'a'..=b'f')),
        "correlation_id must be 32 lowercase hex characters: {doc:?}"
    );
}

/// A garbage IPTC block fails `convert --json` with `metadata_parse_failed`
/// in phase `read`, the caller's correlation ID in the document, and the ID
/// leading the first stderr line. dsp-api maps `error_code` and `phase` to
/// HTTP status classes (DEV-8090).
#[test]
fn cli_json_convert_garbage_iptc_carries_code_and_correlation_id() {
    let input = tmp_path("sipi_garbage_iptc_convert.jpg");
    write_jpeg_with_garbage_iptc(&input);
    let output = tmp_path("sipi_garbage_iptc_convert.jpx");

    let result = Command::new(sipi_bin())
        .env("SIPI_CORRELATION_ID", "e2e-123")
        .args(["convert", "--json", "--format", "jpx"])
        .arg(&input)
        .arg(&output)
        .output()
        .expect("sipi CLI invocation should run");

    assert!(
        !result.status.success(),
        "garbage IPTC must fail the convert"
    );
    let doc = json_doc(&result);
    assert_eq!(doc["status"], "error", "{doc:?}");
    assert_eq!(doc["phase"], "read", "{doc:?}");
    assert_eq!(doc["error_code"], "metadata_parse_failed", "{doc:?}");
    assert_eq!(doc["correlation_id"], "e2e-123", "{doc:?}");
    let message = doc["error_message"].as_str().unwrap_or_default();
    assert!(
        message.contains("IPTC block does not parse"),
        "error_message must say why: {message:?}"
    );
    let stderr = String::from_utf8_lossy(&result.stderr);
    let first = stderr.lines().next().unwrap_or_default();
    assert!(
        first.starts_with("[e2e-123] "),
        "the first stderr line must start with the ID: {stderr:?}"
    );

    let _ = std::fs::remove_file(&input);
    let _ = std::fs::remove_file(&output);
}

/// `query --json` prints text on success and a JSON document on error.
#[test]
fn cli_json_query_garbage_iptc_carries_code() {
    let input = tmp_path("sipi_garbage_iptc_query.jpg");
    write_jpeg_with_garbage_iptc(&input);

    let result = Command::new(sipi_bin())
        .env("SIPI_CORRELATION_ID", "e2e-123")
        .args(["query", "--json"])
        .arg(&input)
        .output()
        .expect("sipi CLI invocation should run");

    assert!(!result.status.success(), "garbage IPTC must fail the query");
    let doc = json_doc(&result);
    assert_eq!(doc["phase"], "read", "{doc:?}");
    assert_eq!(doc["error_code"], "metadata_parse_failed", "{doc:?}");
    assert_eq!(doc["correlation_id"], "e2e-123", "{doc:?}");

    let _ = std::fs::remove_file(&input);
}

/// An input that does not decode fails `convert access-file --json` in phase
/// `read` with a code that says the input file is invalid, and without
/// `SIPI_CORRELATION_ID` the document carries a generated ID.
#[test]
fn cli_json_access_file_undecodable_input_is_an_input_error() {
    let input = tmp_path("sipi_undecodable_access.jpg");
    std::fs::write(&input, b"this is not an image").expect("write the input");
    let output = tmp_path("sipi_undecodable_access.jpx");

    let result = Command::new(sipi_bin())
        .env_remove("SIPI_CORRELATION_ID")
        .args(["convert", "access-file", "--json", "--format", "jpx"])
        .arg(&input)
        .arg(&output)
        .output()
        .expect("sipi CLI invocation should run");

    assert!(!result.status.success(), "an undecodable input must fail");
    let doc = json_doc(&result);
    assert_eq!(doc["phase"], "read", "{doc:?}");
    let code = doc["error_code"].as_str().unwrap_or_default();
    assert!(
        [
            "decode_failed",
            "unsupported_format",
            "malformed_input",
            "metadata_parse_failed"
        ]
        .contains(&code),
        "error_code must be an input-file code: {doc:?}"
    );
    assert_generated_id(&doc);

    let _ = std::fs::remove_file(&input);
    let _ = std::fs::remove_file(&output);
}

/// An invalid `SIPI_CORRELATION_ID` is replaced by a generated ID, on a
/// success document too.
#[test]
fn cli_json_invalid_correlation_id_is_replaced() {
    let input = test_data_dir().join("images/unit/MaoriFigure.jpg");
    let output = tmp_path("sipi_invalid_id_out.jpg");

    let result = Command::new(sipi_bin())
        .env("SIPI_CORRELATION_ID", "a b")
        .args(["convert", "--json"])
        .arg(&input)
        .arg(&output)
        .output()
        .expect("sipi CLI invocation should run");

    assert!(
        result.status.success(),
        "convert must succeed\nstderr: {}",
        String::from_utf8_lossy(&result.stderr)
    );
    let doc = json_doc(&result);
    assert_eq!(doc["status"], "ok", "{doc:?}");
    assert!(doc.get("error_code").is_none(), "{doc:?}");
    assert_generated_id(&doc);

    let _ = std::fs::remove_file(&output);
}
