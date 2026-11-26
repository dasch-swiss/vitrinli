use sipi_e2e::{
    assert_jp2_decode, assert_jp2_roundtrip, assert_metadata_fidelity, assert_query_jpx_header,
    assert_verify_pipeline, conversion_task, conversion_threads, run_conversions, ConversionTask,
};

// =============================================================================
// The heavy Kakadu JP2 / format conversions, previously the serial bulk of the
// `cli` test (~4 min under ASan). They are independent — each spawns its own
// `sipi convert` subprocess — so they run as a labelled task list across
// `conversion_threads()` workers (see `run_conversions`). A failure names the
// exact task (e.g. `jp2_roundtrip_7: ...`) and never masks the others.
//
// Tune the fan-out with `SIPI_E2E_CONVERT_THREADS`; the default is the host's
// available parallelism, so a single-core action still runs correctly (serial).
// =============================================================================

#[test]
fn cli_conversions() {
    let mut tasks: Vec<ConversionTask> = vec![
        // JP2 → TIFF decodes of files 1 to 4 (sRGB, sYCC, sYCC with 2x2
        // subsampled chroma, greyscale); the round-trips below cover the
        // reference set 1 to 9. Part 1 of the former `cli_file_conversion`.
        conversion_task("jp2_decode_file1", || assert_jp2_decode(1)),
        conversion_task("jp2_decode_file2", || assert_jp2_decode(2)),
        conversion_task("jp2_decode_file3", || assert_jp2_decode(3)),
        conversion_task("jp2_decode_file4", || assert_jp2_decode(4)),
        // TIFF → JP2 → TIFF → JPEG → PNG format fidelity.
        conversion_task("metadata_fidelity", assert_metadata_fidelity),
        // ADR-0009 Service → Access pipeline (convert + verify, both kinds).
        conversion_task("verify_pipeline", assert_verify_pipeline),
        // `sipi query` on a JPX reports the header without a pixel decode.
        conversion_task("query_jpx_header", assert_query_jpx_header),
    ];

    // TIFF → JP2 → TIFF round-trips over the reference set (Part 2 of the
    // former `cli_file_conversion`), one task each so the slowest fixture — not
    // their sum — bounds the wall-clock.
    for i in 1..=9 {
        tasks.push(conversion_task(format!("jp2_roundtrip_{i}"), move || {
            assert_jp2_roundtrip(i)
        }));
    }

    run_conversions(tasks, conversion_threads());
}
