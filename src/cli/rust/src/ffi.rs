//! The C-ABI symbols `cli/rust` binds directly: `sipi_cli_main` and
//! `sipi_cli_set_correlation_id`.
//!
//! `cli/rust` does NOT route these through the `sipi` library — the two FFI
//! surfaces are disjoint (the server seam lives in `//src/server/rust`), so each
//! crate declares the symbols it calls. The symbols are provided by the
//! `//src/cli:cli_app` `cc_library` at link time.

use std::os::raw::{c_char, c_int};

extern "C" {
    /// Hands argv verbatim to the C++ CLI11 parser (`//src/cli:cli_app`) and
    /// returns the process exit code — no `exit()`/`abort()` from inside the
    /// FFI, so the Rust caller owns teardown. Drives the offline subcommands
    /// (`convert`/`verify`/`query`/`compare`) the shell does not own, plus
    /// top-level `--version`/`--help`. (`server` and `health` are Rust-native
    /// verbs — they never reach this.)
    pub fn sipi_cli_main(argc: c_int, argv: *mut *mut c_char) -> c_int;

    /// Sets the run's correlation ID for the C++ CLI's log prefix and `--json`
    /// documents. Called once, before `sipi_cli_main`, with a NUL-terminated
    /// string the callee copies.
    pub fn sipi_cli_set_correlation_id(id: *const c_char);
}
