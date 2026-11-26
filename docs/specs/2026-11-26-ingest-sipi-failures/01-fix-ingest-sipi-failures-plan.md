---
title: "fix: sipi failures seen in the rdu-13 bulk ingest (Exif charset, CLI error codes and ID, Kakadu errors, TIFF memory, query, release image)"
type: fix
date: 2026-11-26
author: "Ivan Subotic"
status: implemented
repository: dasch-swiss/vitrinli
repositories: []
linear: DEV-8089, DEV-8090, DEV-8091, DEV-8092, DEV-7236, DEV-8093, DEV-8113
---

# fix: sipi failures seen in the rdu-13 bulk ingest

## Overview

On 2026-11-21 and 2026-11-26 a bulk import into project 4001 on `rdu-13` (omeka2dsp, Moritz Mähr) failed for many files. DSP ingest runs `sipi convert <in> <out>.jpx --json --format jpx --topleft` and then `sipi query <out>.jpx`, and answers HTTP 500 when either fails. The post-mortem is dasch-swiss/infra#618. The ICC profile defect (DEV-8088/DEV-8099) is fixed in vitrinli#857 and is not part of this plan. This plan fixes the six remaining sipi-side issues:

| Issue | Failure | Cause |
|---|---|---|
| DEV-8089 | `sipi query` exits with "unhandled exception: Cannot convert text encoding from 'UCS-2LE' to 'UTF-8'" | The Linux Exiv2 build has no iconv (`bazel/exiv2.BUILD.bazel` sets `EXV_HAVE_ICONV` only on macOS); the throw comes from `Exif` `operator<<` (`exif.cpp:557-568`) when it prints `Exif.Photo.UserComment`, and nothing catches it below `cli_app.cpp:838` |
| DEV-8090 | The whole read fails with "No valid IPTC data!", and dsp-api answers HTTP 500 | The rejection is correct (`docs/src/development/error-model.md`: embedded metadata parse failures are fatal). The defect is the error: the message names neither the file nor the reason, and the `--json` payload has no code that tells dsp-api "the input file is invalid" (4xx) apart from "sipi failed" (5xx), and no ID to find the log lines |
| DEV-8091 | A 228×320 8-bit RGB PNG without an ICC profile fails to write as JPX with "Kakadu exception 1801745733" | Kakadu reports that the JPX has no colour description. Kakadu's text, found in Loki (rdu-13, 2026-11-26 13:18:16Z, trace `912c34af911b6c9d6dd59d460aab167c`): "No colour description found in JP2-family data source, or provided for generating a JP2-family file". Which `SipiIOJ2k::write` branch produces it is open; the Phase 3 unit reproduces it first. 1801745733 is `kduE`, Kakadu's generic code; Kakadu's text goes only to the log (`KduSipiError`, `SipiIOJ2k.cpp:325-350`), never into the error |
| DEV-8092, DEV-7236 | `sipi convert` is OOM-killed on 2–4 GB TIFFs | Partly DEV-7236: the TIFF reader holds two full-size pixel buffers at once (`SipiIOTiff.cpp:1534-1563`, `inbuf` plus the `pixdata` vector, then a `memcpy`) |
| DEV-8093 | `sipi query` hits the 120 s decode deadline on valid 1.75–2.3 GB JPX files | `run_query` (`cli_app.cpp:209-222`) decodes every pixel, then prints only header and metadata fields (`SipiImage.cpp:522-541`) |
| DEV-8113 | The released image is not the image the smoke tests ran on | `publish.yml`: `publish-docker` builds again and pushes. Also, `validate-docker` builds with `--config=release` (`bazel-docker-build-*`), but `just bazel-test-smoke` (`justfile:289`) runs `bazel test --stamp //test/e2e:docker_smoke` without it, so the smoke test loads a fastbuild image |

## Problem Statement / Motivation

DaSCH wants to scale ingest (bulk imports through the API are the path for migrating projects). Every failure above returned a bare HTTP 500 to an external user after minutes of work, and three of them fail a valid image because of one metadata field or one print statement. The released-image gap let a defective binary reach production for four months.

## Proposed Solution

Six independent changes, one unit each:

1. Exiv2 gets iconv on Linux, and the Exif printer drops a value it cannot convert with a warning.
2. Every `--json` error payload carries an `error_code`, and every payload a `correlation_id`. The ID comes from the caller (`SIPI_CORRELATION_ID`) or is generated, and every log line of the run carries it. An invalid IPTC block stays fatal, with a message that names the file and the reason.
3. Kakadu's error text goes into the error that sipi returns, for reads and writes, and the JPX writer returns its own error when it has no colour description.
4. The TIFF reader decodes into the image's own pixel buffer: one full-size buffer, not two.
5. `sipi query` reads header and metadata only, with the same output.
6. `publish-docker` pushes the exact image and debug file that `validate-docker` tested.

## Alternative Approaches Considered

| Option | Why not |
|---|---|
| DEV-8089: only enable iconv | glibc's iconv converts UCS-2 built in, but a charset that needs a gconv module (JIS) would still throw in the distroless image and fail the file |
| DEV-8089: only catch in the printer | The common Photoshop UCS-2 comment would then never print on Linux, while it prints on macOS |
| DEV-8093: give `query` a longer or configurable deadline | Needs a dsp-api change, and a 2 GB full decode also costs gigabytes of memory, the DEV-8092 failure again |
| DEV-8093: build `query` on `read_shape` | `SipiImgInfo` has no `photo`, extra samples or metadata objects, and the JPEG `read_shape` skips ICC; the output would change |
| DEV-8113: push from `validate-docker` after the smoke test | amd64 could push while arm64 fails, leaving per-arch tags without a manifest; the `release-gate` split exists to prevent that |
| DEV-8090: drop an invalid IPTC block with a warning | Reverses the owner's ruling that embedded metadata parse failures are fatal (we are a repository; no corruption is allowed). Owner decision 2026-11-26: keep rejecting, with a clearer error |
| Error ID: sipi always generates its own ID | dsp-api's logs and sipi's logs would carry two IDs for one request; an ID passed in by the caller joins them, and sipi generates one only when none is passed |
| Error ID: a `--correlation-id` flag | The Rust `main` (Sentry) and the C++ CLI (logger, JSON) both need it before CLI11 parses; an environment variable is read the same way in both, and needs no new option on every verb |

## Technical Considerations

### Decisions per issue

- **DEV-8089.** `bazel/exiv2.BUILD.bazel`: the Linux `select` branch adds `EXV_HAVE_ICONV=1` next to `EXV_STRERROR_R_CHAR_P=1`. glibc provides iconv in libc, so no linkopt is added (the macOS-only `-liconv` linkopts stay). The module docstring states the new token values. Only the Exif printer, `operator<<(std::ostream&, Exif&)` (`exif.cpp:557-568`), changes: it renders each value into a string inside a `try`. On `Exiv2::Error` or `std::exception` the line prints `<unconvertible>` in place of the value, and `log_warn` names the key. The `Iptc` printer and `assign_val` (`exif.cpp:62-72`) do not change: no failure is shown there. The read path does not change: it already succeeds. Rejected: dropping the key from the output. The line keeps the key, so a reader sees that the field exists.
- **DEV-8090 and the error ID.**
  - IPTC stays fatal (owner decision 2026-11-26). When `IptcParser::decode` returns non-zero, `Iptc::parse` (`iptc.cpp:18-25`) returns `kMetadataParseFailed` with `IPTC block does not parse: Exiv2 rejected the block (decode code <rc>)`, in place of `No valid IPTC data!` (`iptc.cpp:22`). `Iptc::parse_complete` (`iptc.cpp:28-60`) does not change. `parse` gets a `try`/`catch` only if a unit test shows that `decode` throws on garbage input. The four reader sites return a new `SipiValueError` with the same code and the message prefixed by `<path>: `.
  - `SipiReport.h` declares `[[nodiscard]] const char *error_code_name(ErrorCode)`. It is a total `switch` with no `default`, so a new `ErrorCode` fails the build until it has a name. The names: `decode_failed`, `unsupported_format`, `malformed_input`, `write_failed`, `client_abort`, `shape_probe_failed`, `metadata_parse_failed`, `invalid_request_parameter`.
  - `emit_json_report` (`SipiReport.h:39-42`) gets a new parameter `std::string error_code = "internal"` after `phase`. Every call site of `emit_json_report` in `cli_app.cpp` and in `src/cli/cpp/commands/convert_access_file.cpp` passes its code. A call site that holds a `SipiValueError` passes `error_code_name(r.error().code())`. `emit_json_cli_arg_error` passes `cli_args`. A `catch (SipiImageError &)`, a `catch (std::exception &)` and the decode-deadline path pass `internal`. `report_error` in `convert_access_file.cpp` (`:103-110`) gets an `error_code` parameter. `error_code` is emitted only when `error_message` is set; a success document has no `error_code` key.
  - The post-decode steps (`to8bps`, `convertToIcc`) change from phase `read` to phase `convert`: in `cli_app.cpp:343-364`, including the `catch (const std::exception &)` at `:360-364` that wraps only them, and in `convert_access_file.cpp:212`, `:217` and the catch at `:223`. So phase `read` holds only decode errors of the input file.
  - Caller rule, in `json-output.md`: in phase `read`, the codes `decode_failed`, `unsupported_format`, `malformed_input` and `metadata_parse_failed` mean the input file is invalid. If that file is the user's upload, the caller answers 4xx. If sipi created the file, the caller answers 5xx (owner rule 2026-11-26). Every other code is a 5xx.
  - `run_query` (`cli_app.cpp:209-222`) and `run_compare` (`:523-589`) emit the JSON error document when `--json` is set, and call `set_json_mode(true)`. `run_query`: both error paths use phase `read`; the deadline path uses code `internal`. `run_compare`: the two reads (`:535-551`) use phase `read` with the `SipiValueError` code, or `internal` for the deadline; the `subtract` failure (`:574`) uses phase `convert`; the `diff.tif` write failure (`:578`) uses phase `write`; both pass `error_code_name(r.error().code())`. The two file-not-found exits (`:525-532`) use phase and code `cli_args`, as an argument error does. Today they only log. On success, `query --json` and `compare --json` keep their text output on stdout; only the error path is JSON.
  - The catch-all in `sipi_cli_main` (`cli_app.cpp:838-846`) also emits an error document when `--json` is set, with phase `unknown` and code `internal`.
  - `error_message` keeps the full text, including Kakadu's (Phase 3).
- **The error ID (`correlation_id`).** One term in code and docs: the field `correlation_id`. Callers show it to the user as the error ID.
  - Every JSON document sipi emits carries `correlation_id`: the `convert` and `convert access-file` success and error documents, and the error documents of every verb.
  - The Rust `run_cli` path in `src/cli/rust/src/main.rs` resolves it once, before it calls `sipi_cli_main`. The `server` and `health` verbs do not change. The value is `SIPI_CORRELATION_ID` when it matches `^[A-Za-z0-9._:-]{1,128}$`. Else it is 16 random bytes in lowercase hex, read with `libc::getrandom` (`libc` is already a dependency, `src/cli/rust/BUILD.bazel:170`); no crate is added. An invalid value is replaced, and a warning says so.
  - `run_cli` sets the Sentry scope tag `correlation_id` and calls a new C-ABI setter `sipi_cli_set_correlation_id(const char *)`. It is declared next to `sipi_cli_main` in `src/ffi/cpp/sipi_ffi.h` (`:669`), defined in `cli_app.cpp` next to `sipi_cli_main` (`:111`), and bound in `src/cli/rust/src/ffi.rs`. It calls `set_correlation_id(const std::string &)`, declared in `logger.h`. The `ffi.rs` `//! The one C-ABI symbol` doc comment is updated.
  - `logger.h` declares `get_correlation_id()`. The logger keeps the ID in a process-wide string. In CLI mode, the first line of each log call starts with `[<id>] `, so one Loki search finds every log call of the run. Server mode does not change. `emit_json_report` reads the ID with `get_correlation_id()`. Until the setter runs, `get_correlation_id()` returns an empty string: the C++-only `//src/cli:sipi` binary (`src/cli/cpp/sipi.cpp:20`, used by approval tests) emits `"correlation_id": ""` and no log prefix. The two hand-built fallback documents in `SipiReport.cpp` (`:65-66`, `:118-119`) also carry `error_code` `internal` and the ID.
- **Out of this plan.** dsp-api passes `SIPI_CORRELATION_ID` and returns `correlation_id`, `error_code` and a message without server paths in its 4xx/5xx body (DEV-8105). dsp-tools prints and logs them (DEV-8114).
- **DEV-8091.** Two changes. First, the JPX writer never hands Kakadu a layer without a colour description. Both colour `switch` statements stay as they are. After the `if (image_icc != nullptr) {...} else {...}` block (`SipiIOJ2k.cpp:1645-1781`), one check is added: if `!jp2_family_colour.exists()`, `write` returns `kWriteFailed` with `nc`, `getEs().size()` and `bps`. An unsupported channel layout is sipi's own error, a 5xx. A plain 8-bit RGB PNG without ICC takes `case 3` (`:1772`) and gets sRGB, so this check alone may not explain DEV-8091; the unit reproduces DEV-8091 first and records the branch. Second, Kakadu's text reaches the returned error. The shared `static KduSipiError kdu_sipi_error` (`SipiIOJ2k.cpp:350`) appends to one non-atomic `std::string msg` from concurrent decodes, a data race. `KduSipiError` keeps its message buffer in a `thread_local std::string` (Kakadu calls the handler on the thread that raised the error, so one installed instance stays valid whether `kdu_customize_errors` is global or per thread), and `flush(end_of_message)` moves the text into a second `thread_local` "last error" string and resets the buffer, so text no longer accumulates. Every `catch (kdu_exception` in `SipiIOJ2k.cpp` (13 today; `grep -c 'catch (kdu_exception'`) that returns an error appends the last-error text to that error and clears it. A `catch` that recovers (the colour fallback at `:1757`) only clears it. The numeric code stays where the message has it today. All three `kdu_customize_errors` call sites (read `:389`, `read_shape` `:1112`, write `:1388`) keep installing the same instance; the teardown-order rule from `docs/learnings/architecture/kakadu-teardown-order-and-pod-templates-accepting-std-string.md` holds for any new Kakadu object.
- **DEV-8092 / DEV-7236.** For the contiguous 8- and 16-bit strip and tile paths, `read_standard_data<T>` and `read_tiled_data<T>` write into the buffer that `set_pixels` receives, so the `memcpy` at `:1551`/`:1560` and the second full-size vector go away. The caller allocates the pixel buffer once as `std::vector<byte>` with the existing checked size (`checked_buf_size_or_throw`). `read_standard_data<T>` and `read_tiled_data<T>` take a `std::span<T>` over it, made with `reinterpret_cast<T *>(inbuf.data())`, and return `Result<void>`. A `static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)` and a one-line comment state why the cast is safe. `set_pixels(std::move(inbuf), ...)` (`SipiImage.h:361`) does not change. For `bps` 1 the span holds the promoted 8-bit samples. The planar-separate branches (`SipiIOTiff.cpp:745-782`, `:822-863`, tiled `:1006`) keep a local `std::vector<T>` and `separateToContig`, then copy the result into the span; only the contiguous branches write straight into it. Each function checks `span.size() == roi_w * roi_h * nc` and returns `kMalformedInput` otherwise. The 1-bit to 8-bit promotion (`one2eight<T>`) and the region-of-interest path keep their own sizing and are checked by the pixel test. The planar (`separateToContig`) and palette paths are out of scope. `estimate_peak_memory` (`SipiPeakMemory.h`) does not model the TIFF-internal copy, so it needs no change; its comment is checked for a claim about the copy. This is a hot path: `decode_benchmark.cpp` has TIFF cases (`pyr-*.tif`, `flat.tif`), and the before/after run is H3.
- **DEV-8093.** A new pure virtual `SipiIO::read_metadata(SipiImage *img, const std::string &path)`, like `read_shape` (`SipiIO.h:237`), implemented by the four handlers (TIFF, JPEG, PNG, J2K; there are no others), sets `nx`, `ny`, `nc`, `bps`, `photo`, extra samples, orientation and the EXIF, XMP, IPTC and ICC objects to the values a full `read` leaves, and decodes no pixels. Each handler shares its header and metadata code with its `read` (one code path, not a copy), including the header effects of post-decode steps: palette to RGB (`nc`, `photo`), 1-bit to 8-bit (`bps`), and CMYK handling. The IFD0 copy promotion is not a read step (`SipiImage.cpp:503`, write only) and is not reproduced. The J2K override opens the codestream for its header and decodes no tile. `SipiImage::read_metadata(path)` follows `SipiImage::read`: extension dispatch (`SipiImage.cpp:296-304`), then every other handler, then `kUnsupportedFormat` with the same `Error reading file ... (tried: ...)` message (`:333`, not the `Could not read file` text at `:439`). It does not use `read_shape`'s `kShapeProbeFailed`. `read_with_deadline` (`src/cli/cpp/commands/decode_deadline.{h,cpp}`) gets a metadata-only variant; `run_query` uses it with the 120 s default, and `compare` (`cli_app.cpp:534`, `:543`) keeps the full read. ADR-0004 keeps governing shape: `read_shape` stays the shape probe, and `read_metadata` is a full header and metadata read for `query` only, so ADR-0004's "no new virtual method" (about renaming `getDim`) is not reversed. Metadata parse failures stay fatal in `read_metadata` as in `read` (`docs/src/development/error-model.md`).
- **DEV-8113.** First, the smoke test runs on the release build: a new recipe `bazel-test-smoke-release <arch>` runs the smoke test with `--config=release --stamp --platforms=<arch platform>`, the flags of `bazel-docker-build-<arch>`, so the `//src:image_load` `tarball` output group it consumes is the release image. `bazel-test-smoke` and its use in `ci.yml` do not change, so the `image-scan` cache rule (`justfile:622-623`) holds. A second new recipe builds the tarball with the same flags plus `--remote_download_toplevel --output_groups=tarball //src:image_load` and locates it with `bazel cquery --output_groups=tarball --output=files`, as `image-scan` does (`justfile:629-630`), and `validate-docker` uploads it and the `sipi-<arch>.debug` file it already extracts as workflow artifacts (named `sipi-image-<arch>` and `sipi-debug-<arch>`, `retention-days: 7`, so a re-run of a failed `publish-docker` still finds them). `publish-docker` downloads them with `actions/download-artifact`, and a new `justfile` recipe pushes the tarball with `crane push` to `daschswiss/sipi:v<version>-<arch>` and `latest-<arch>`, `<version>` read from `version.txt` as `bazel-docker-publish-manifest` does (`justfile:677-690`), (the thread checks `crane push --help`; the platform comes from the tarball's config). The recipe compares `crane digest --tarball <tar>` with `crane digest daschswiss/sipi:v<version>-<arch>` after the push and fails when they differ, printing both. `publish-docker` no longer builds: `ci-setup` (Bazel, Kakadu, RBE) and the arm64 `qemu-user-static` step go; `extractions/setup-just` and `imjasonh/setup-crane` are added, pinned as in the `manifest` job (`publish.yml:213-214`); `docker/login-action` and the Node used by `sentry-cli` stay; the Sentry step uploads the downloaded debug file. The `release-gate`, `sentry` and `manifest` jobs keep their `needs:`. The unused `image_push_*` `oci_push` targets and `bazel-docker-push-*` recipes are removed, and every reference to them is updated (`README.md`, `CLAUDE.md`, `docs/src/development/ci.md`, `bazel.md`, `building.md`, `allocator-replay.md`, `.bazelrc`).

### Cross-cutting

- **Gates in the cloud.** Bazel cannot fetch dependencies in the cloud container. A unit thread runs what it can (standalone compile of a changed C++ file with g++ 13, `rustfmt`), pushes its draft PR and reads the CI result; a gate counts as met only when CI is green on the unit's head.
- **Approval goldens.** Phase 1 can change metadata in outputs. Each runs `just bazel-test-approval`; a changed golden is re-approved with a `test/approval/CHANGELOG.approval.md` row that names the reason.
- **Build completeness.** Phase 1 changes `bazel/exiv2.BUILD.bazel`. CI builds Linux x86_64 and aarch64; the macOS `just bazel-build` and `just bazel-coverage` run is H4.
- **Stacking without `gh stack`.** The cloud container has no `gh stack`. Owner decision 2026-11-16: units stack with plain git (`git rebase --onto origin/<top> <started_from> <unit branch>`), push, and open a draft PR with the GitHub MCP whose base is the current top branch. The owner adopts the stack with `gh stack init`/`gh stack link` on a Mac before the merge (H5). A unit thread whose session assigns a different branch name uses that name and reports it in `done`.
- **Base.** The plan branch starts from `main` after vitrinli#859, without vitrinli#857. When #857 merges first, the final unit rebases the stack on `main`; the units touch none of #857's files except `test/e2e/tests/docker_smoke.rs`, which no unit changes.

## Implementation Phases

#### Phase 1: Exif values that cannot be converted

depends on: none

- [ ] DEV-8089: `bazel/exiv2.BUILD.bazel` Linux branch adds `EXV_HAVE_ICONV=1`; the docstring states the Linux token values
- [ ] DEV-8089: `operator<<` for `Exif` (`exif.cpp:557`) renders each value inside a `try`; on failure the line prints `<unconvertible>` and `log_warn` names the key
- [ ] Unit test in `src/metadata/cpp/` (new `exif_charset_test.cpp` or the closest existing suite): build an `Exiv2::ExifData` with `Exif.Photo.UserComment` = `charset=Unicode` plus text, and one with the 8-byte charset prefix only; stream it through `Exif` `operator<<`; expect no exception and, where `EXV_HAVE_ICONV` is set, the UTF-8 text. A second case sets `charset=Jis` with bytes that are not valid JIS and expects the line to end in `<unconvertible>` or in the converted text, never an exception (the assertion holds with and without iconv). Regression caught: a metadata print that throws and fails `sipi query`
- [ ] e2e in `test/e2e/tests/cli.rs`: a JPEG with a UCS-2LE `UserComment`, written by a byte-level helper in `test/e2e/src/lib.rs` (an APP1 Exif segment inserted into an existing unit JPEG; no new LFS fixture), passes `sipi query` with exit 0
- [ ] `just commit-lint && just bazel-rustfmt-check && just bazel-clippy-check && just bazel-test`, `just bazel-test-approval` and `just test-signal-lint` pass (CI where the container cannot build); `bazel test //src/metadata/... //test/e2e:cli --test_output=errors` is the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 2: Error codes and a correlation ID in CLI errors

depends on: none

- [ ] DEV-8090: `Iptc::parse` returns `kMetadataParseFailed` with `IPTC block does not parse: Exiv2 rejected the block (decode code <rc>)` when `IptcParser::decode` returns non-zero (`iptc.cpp:21-22`); `parse_complete` does not change; `parse` gets a `try`/`catch` only if a unit test shows that `decode` throws on garbage input. The four reader sites (`SipiIOTiff.cpp:1301-1305`, `SipiIOJpeg.cpp:482-486`, `SipiIOPng.cpp:404-409`, `SipiIOJ2k.cpp:494-498`) stay fatal: each returns a new `SipiValueError` with the same code and the message prefixed by `<path>: `; the PNG site keeps its `png_destroy_read_struct`
- [ ] `error_code_name` and the new `emit_json_report` parameter; every call site in `cli_app.cpp` and `convert_access_file.cpp` passes its code; `to8bps` and `convertToIcc` failures move to phase `convert` in both files; `run_query` and `run_compare` emit the error document; the catch-all at `:838-846` emits one with phase `unknown` and code `internal`
- [ ] `run_cli` in `main.rs` resolves the ID (`SIPI_CORRELATION_ID` validated, else `libc::getrandom`), sets the Sentry tag `correlation_id`, and calls `sipi_cli_set_correlation_id` (declared in `sipi_ffi.h`, defined in `cli_app.cpp`) before `sipi_cli_main`; in CLI mode the first line of each log call starts with `[<id>] `
- [ ] Tests: a new `src/cli/cpp/SipiReport_test.cpp` with a `cc_test` next to the target that builds `SipiReport.cpp`, for each `error_code` name, for `correlation_id` (set and empty), for the fallback documents, and that a success document has no `error_code` key; a logger test that a CLI-mode line starts with the ID and a server-mode line does not (`src/logging/cpp/logger_test.cpp`); a format-handler test that a JPEG with a garbage APP13 IPTC block fails the read with `kMetadataParseFailed`, and its message starts with `<path>: IPTC block does not parse: Exiv2 rejected the block (decode code ` and ends with `)` (the test does not hard-code `<rc>`; the garbage block is chosen so that `IptcParser::decode` returns non-zero). Regression caught: an error the caller cannot classify or find in the logs
- [ ] e2e in `test/e2e/tests/cli.rs`: `SIPI_CORRELATION_ID=e2e-123 sipi convert --json` on the garbage-IPTC JPEG (built by a byte-level helper in `test/e2e/src/lib.rs`) exits non-zero, its stdout has `error_code == "metadata_parse_failed"`, `phase == "read"` and `correlation_id == "e2e-123"`, and the first stderr line starts with `[e2e-123] `; `sipi query --json` on the same JPEG gives `error_code == "metadata_parse_failed"` too; `sipi convert access-file --json` on an undecodable input gives `phase == "read"` and a code from the input-file set; without the variable, `correlation_id` is 32 hex characters; an invalid value (`a b`) is replaced by 32 hex characters (the Rust warning about it uses `eprintln!` and has no prefix, so the first-line check applies only to the valid-ID case). The e2e runs the Rust binary. Regression caught: the dsp-api contract breaking
- [ ] `docs/src/guide/json-output.md`: both fields, the code table, the 4xx/5xx rule for callers, `SIPI_CORRELATION_ID`, the log prefix, the `unknown` phase, and that `query`/`compare` emit JSON only on error; lines 3-9 and 26-28 are corrected (they say `query` emits JSON on success and that `--json` is not on `convert`); its examples use the current verb syntax (`sipi convert ... --json`)
- [ ] `just commit-lint && just bazel-rustfmt-check && just bazel-clippy-check && just bazel-test` and `just test-signal-lint` pass (CI where the container cannot build)
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 3: Kakadu error text in the returned error

depends on: none

- [ ] DEV-8091: `KduSipiError` stores the flushed message text in a `thread_local std::string` and resets its buffer after each flush; the instance passed to `kdu_customize_errors` is safe for concurrent decodes
- [ ] DEV-8091: every `catch (kdu_exception` in `SipiIOJ2k.cpp` (13 sites) that returns an error appends the stored text and clears it; the recovering `catch` at `:1757` only clears it
- [ ] DEV-8091: the unit first reproduces DEV-8091 with a 228×320 8-bit RGB PNG that has no `sRGB`, `gAMA` or `iCCP` chunk (built in the test) and records the branch it takes; then `SipiIOJ2k::write` checks `jp2_family_colour.exists()` after the colour block (`:1645-1781`) and returns `kWriteFailed` with `nc`, the extra-sample count and `bps`; if the PNG writes cleanly, the unit says so in its PR, and H6 brings the source file
- [ ] Test: a 2-channel 8-bit image without extra samples and without ICC, written as JPX, returns `kWriteFailed` whose message contains `nc=2`. Regression caught: a write that reaches Kakadu without a colour description
- [ ] Test in `src/format_handlers/cpp/format_error_path_test.cpp` (or a new J2K error test): a read of a J2K codestream built in the test and then cut inside its main header (for example after the first 20 bytes of the SIZ marker) returns an error (the test first asserts that the read fails, so it fails for the right reason before the change), and that error's message holds more than the numeric code (the fixture calls `set_cli_mode(true)` and restores it in teardown, because only CLI mode logs errors to stderr; `testing::internal::CaptureStderr()` wraps the read; the test asserts that the error message contains the `Kakadu-library: ` lead-in (`SipiIOJ2k.cpp:350`) and that the captured stderr contains the same text); a second failing read returns only its own text, not the first one's. Regression caught: an opaque `kduE` error
- [ ] `just commit-lint && just bazel-test` passes (CI where the container cannot build)
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 4: One pixel buffer in the TIFF reader

depends on: none

- [ ] DEV-7236: `read_standard_data<T>` and `read_tiled_data<T>` take a `std::span<T>` over the one `std::vector<byte>` that `set_pixels` receives, for contiguous 8- and 16-bit images, and return `Result<void>`; the `memcpy` at `SipiIOTiff.cpp:1551`/`:1560` is removed
- [ ] Test in `src/format_handlers/cpp/` that reads a synthetic contiguous TIFF (strip and tiled, 8 and 16 bit, plus one planar-separate TIFF to prove it is unchanged) and compares its pixels to the samples the test used to generate the TIFF. Regression caught: wrong pixels or byte order from the new buffer handling
- [ ] Test that the peak heap during a TIFF read of a synthetic 64 MiB image stays below 1.5 times the pixel buffer: the test binary replaces global `operator new`/`operator delete` with counting versions that track live and peak bytes, writes the TIFF to a temp file first, resets the peak, reads it, and compares the peak to the buffer size. Regression caught: a second full-size buffer coming back
- [ ] `SipiPeakMemory.h` comments checked; no claim about the removed copy remains
- [ ] `just commit-lint && just bazel-test` passes (CI where the container cannot build)
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 5: `sipi query` without a pixel decode

depends on: none

- [ ] DEV-8093: add the pure virtual `SipiIO::read_metadata(SipiImage *img, const std::string &path)` and `SipiImage::read_metadata(path)` with `read`'s dispatch, fallback and error; implement it in the TIFF, JPEG, PNG and J2K handlers by sharing their `read` header and metadata code
- [ ] DEV-8093: a metadata-only variant of `read_with_deadline` in `src/cli/cpp/commands/decode_deadline.{h,cpp}`, with a unit test next to the existing one; `run_query` uses it; `compare` keeps the full read
- [ ] Test (unit, in `src/image/cpp/` or `src/format_handlers/cpp/`): `operator<<` of the `read_metadata` image equals `operator<<` of the fully read image for every image in `test/_test_data/images/unit/` that the four handlers read, and the set must include a palette TIFF, a 1-bit TIFF, a CMYK TIFF and JPEG, a 16-bit TIFF, a tiled TIFF, a multi-page TIFF, and a JPX (the unit lists the files it uses; a missing kind is built in the test); a file with no known format gives the same `ErrorCode` and message from `read` and `read_metadata`. Regression caught: `query` output that drifts from the full read
- [ ] e2e `cli_query_dumps_image_info` (`test/e2e/tests/cli.rs:190`) still passes; a new e2e case runs `sipi query` on a JPX that `sipi convert` wrote from `cmyk.tif` and checks `nx`, `ny` and `nc` against the source. Regression caught: `query` on a JPX that returns the wrong header
- [ ] `decode_benchmark.cpp` has cases for JPEG, PNG and J2K as well as TIFF; a missing format gets a case
- [ ] The CLI docs (`grep -rln "query" docs/src/guide/` names the pages; `json-output.md` at least) state that `query` reads header and metadata only
- [ ] `just commit-lint && just bazel-rustfmt-check && just bazel-clippy-check && just bazel-test` and `just test-signal-lint` pass (CI where the container cannot build)
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 6: Publish the image that was tested

depends on: none

- [ ] DEV-8113: `validate-docker` runs the smoke test with the release flags (`--config=release --stamp --platforms=<arch platform>`), through the new `bazel-test-smoke-release <arch>` recipe; `bazel-test-smoke` does not change
- [ ] DEV-8113: a recipe builds the tarball with the release flags plus `--remote_download_toplevel --output_groups=tarball` and locates it with `bazel cquery`; `validate-docker` uploads it and `sipi-<arch>.debug` as artifacts `sipi-image-<arch>` and `sipi-debug-<arch>` (`retention-days: 7`)
- [ ] DEV-8113: `publish-docker` downloads both, sets up `just` and `crane` as the `manifest` job does, runs a new `crane` push recipe (tags from `version.txt`, digest of tarball and of pushed tag compared and printed, failure on a mismatch), uploads the downloaded debug file to Sentry, and no longer runs `ci-setup` or the qemu step; the `Save Bazel repository cache` step (`publish.yml:195-200`), the `bep-publish` upload (`:147-153`), the `GH_TOKEN` env (`:117`) and the matrix `bazel-platform` go; the `validate-docker` comments at `:68-71` and `:123-124` ("publish-docker rebuilds it") are updated
- [ ] Remove the `image_push_*` targets and `bazel-docker-push-*` recipes; `grep -rn "image_push\|bazel-docker-push" . --exclude-dir=specs --exclude-dir=.git` returns nothing; `.bazelrc`, `README.md`, `CLAUDE.md` and `docs/src/development/{ci,bazel,building,allocator-replay}.md` describe the new flow
- [ ] `python3 -c 'import yaml,sys; yaml.safe_load(open(".github/workflows/publish.yml"))'` passes, `actionlint` passes where installed, and every `needs:` target and artifact name in `publish.yml` matches a job or an upload (checked by a grep in the PR description); `just commit-lint` passes
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

## Threads

Up to 6 units in parallel. Units stack in the order they finish (`thread-coordination.md`); the stack top is the last stacked unit branch. Units `cli-errors` and `query` both touch the four format handlers' read functions, `cli-errors`, `kakadu-errors` and `query` all touch `SipiIOJ2k.cpp`; `tiff-buffer` and `query` both touch `SipiIOTiff::read`. `exif-charset` and `cli-errors` both add byte-level JPEG helpers to `test/e2e/src/lib.rs` and cases to `test/e2e/tests/cli.rs`. `cli-errors` and `query` both edit `run_query` in `cli_app.cpp` and `json-output.md`. The later unit merges in its rebase and reuses a helper that an earlier unit added. A conflict is resolved in the unit's stack-on rebase, as the stack rules say; `query` resolves the larger conflicts, so when `cli-errors` or `tiff-buffer` is ready at the same time, the coordinator's arrival order applies and no unit waits for another.

```yaml
plan_branch: plan/2026-11-26-ingest-sipi-failures
max_parallel: 6
units:
  - unit: exif-charset
    phases: [1]
    depends_on: []
    base: plan
    branch: impl/ingest-exif-charset
  - unit: cli-errors
    phases: [2]
    depends_on: []
    base: plan
    branch: impl/ingest-cli-errors
  - unit: kakadu-errors
    phases: [3]
    depends_on: []
    base: plan
    branch: impl/ingest-kakadu-errors
  - unit: tiff-buffer
    phases: [4]
    depends_on: []
    base: plan
    branch: impl/ingest-tiff-buffer
  - unit: query
    phases: [5]
    depends_on: []
    base: plan
    branch: impl/ingest-query
  - unit: release-image
    phases: [6]
    depends_on: []
    base: plan
    branch: impl/ingest-release-image
  - unit: final
```

## Human Actions

| Id | Action | Who | When | Why not the agent |
|----|--------|-----|------|-------------------|
| H1 | Read the plan PR, then answer the start card | Ivan | before start | Accepting the plan is the owner's |
| H2 | Done 2026-11-26: vitrinli#859 fixed RUSTSEC-2026-0594/0592 on `main`; the plan branch is rebased on it | Ivan | before start | The repin needs Bazel |
| H3 | Before the merge, run `just bench` and `just bench-compare` (`decode_benchmark`, `-c opt`) on `main` and on the stack top on the same machine; accept the delta per `docs/src/development/benchmarking.md` | Ivan | after ship | Needs a host where Bazel builds |
| H4 | Run `just bazel-build` and `just bazel-coverage` on macOS on the stack top (Exiv2 build change) | Ivan | after ship | Build Completeness Invariant; needs a Mac |
| H5 | On a Mac with `gh stack`: `gh stack init --base main <plan branch> <unit branches, bottom to top>`, `gh stack link`, then `gh stack merge --rebase <top PR>` | Ivan | after ship | `gh stack` is not available in the cloud container; merging is the owner's |
| H6 | After the next release is deployed to `rdu-13`: get the DEV-8091 source PNG (`60jzjs2Zi3U-4uSDY0uYFRW`) from Moritz, re-ingest it, and record the result in DEV-8091; ask Moritz to re-run the UCS-2LE, IPTC and decode-deadline files | Ivan / Balduin | after ship | Needs a deployed release and the external user |

## Requirements

- REQ-1 [B] (unwanted behaviour): If an Exif value cannot be converted to text, then `sipi query` shall print `<unconvertible>` for that value, log a warning that names the key, and exit 0. Check: Phase 1 unit test and e2e.
- REQ-2 (ubiquitous): The Linux build of Exiv2 shall convert a UCS-2LE `Exif.Photo.UserComment` to UTF-8. Check: Phase 1 unit test passes on the Linux CI legs.
- REQ-3 [B] (unwanted behaviour): If an image's IPTC block does not parse, then `sipi convert --json` shall fail with `error_code` `metadata_parse_failed` and a message that names the file. Check: Phase 2 e2e.
- REQ-8 [B] (ubiquitous): Every JSON document that the sipi binary emits shall carry `correlation_id`, equal to a valid `SIPI_CORRELATION_ID` or else generated, and the first line of each CLI log call shall start with it. Check: Phase 2 e2e.
- REQ-9 [B] (unwanted behaviour): If the JPX writer has no colour description for an image, then sipi shall return a `kWriteFailed` error that names the channel layout, without calling Kakadu's write. Check: Phase 3 test.
- REQ-4 [B] (unwanted behaviour): If Kakadu raises an error during a JPEG 2000 read or write, then the error that sipi returns shall contain Kakadu's message text. Check: Phase 3 test.
- REQ-5 (ubiquitous): The TIFF reader shall hold at most one full-size pixel buffer for a contiguous 8- or 16-bit image. Check: Phase 4 peak-memory test below 1.5 times the buffer.
- REQ-6 [B] (event-driven): When `sipi query` runs on an image that the TIFF, JPEG, PNG or J2K handler reads, it shall print the same output as before without decoding pixels. Check: Phase 5 equality test over `test/_test_data/images/unit/`.
- REQ-7 (ubiquitous): The release workflow shall smoke-test the release build of the image and push that image's digest. Check: the Phase 6 release-flag smoke step and digest comparison step.

The `[B]` requirements are proven by the unit and e2e tests named in their check; the repository has no `bdd.yaml`, so there are no feature files.

## Out of Scope

- DEV-8088/DEV-8099 (fixed in vitrinli#857), the release and DSP pin that follow it.
- The dsp-api and infra issues from the same post-mortem: DEV-8094 to DEV-8098, DEV-8100 to DEV-8106, DEV-8112. They need their own plan in their repositories.
- The dsp-api side of the error contract (DEV-8105: pass `SIPI_CORRELATION_ID`, map codes to 4xx/5xx, return the fields) and the dsp-tools side (DEV-8114: print and log them).
- A memory check before `sipi convert` (DEV-8101, ingest side), the planar and palette TIFF copies, and the memory budget for offline verbs.
- A configurable decode deadline for the offline verbs.

## Normative Sources

1. The owner's decisions: 2026-11-26 decision card (all six vitrinli issues in scope); 2026-11-26 IPTC stays fatal with a clearer error; 2026-11-26 a corrupt header on a user's file is a 4xx, on a file sipi created a 5xx; 2026-11-26 errors carry an ID that is also in the logs; 2026-11-16 stacking without `gh stack`.
2. The Linear issues DEV-8089, DEV-8090, DEV-8091, DEV-8092, DEV-7236, DEV-8093, DEV-8113 and their comments.
3. `docs/src/development/error-model.md` (embedded metadata parse failures are fatal on read), ADR-0004, ADR-0024.
4. `CLAUDE.md`, `CONVENTIONS.md` (Scope Discipline, Build Reproducibility and Completeness Invariants, hot-path benchmark rule).
5. Post-mortem dasch-swiss/infra#618.

## Dependencies & Risks

- **iconv in the hermetic toolchain.** `<iconv.h>` must be in the Linux sysroot of the hermetic LLVM toolchain. If it is not, Phase 1 keeps the printer fix and the unit reports `blocked` for the build change with the error.
- **Distroless runtime.** glibc converts UCS-2 without gconv modules; other charsets may still fail, which REQ-1 handles.
- **`read_metadata` drift.** Sharing code with `read` is required to keep `query` output equal; the equality test is the guard.
- **Phase 6 cannot run before a release.** The workflow runs only on a `v*` tag. The first release after the merge is its real test; H5's merge must be followed by watching that run.
- **CI-only gates.** Every unit relies on CI for Bazel gates, which makes each review cycle slower.

## Success Metrics

- The DEV-8089, DEV-8090 and DEV-8093 files from project 4001 ingest on `rdu-13` after the release (H6).
- DEV-8091's next failure, if any, carries Kakadu's text or sipi's channel-layout error, and every failure carries a `correlation_id` that finds its log lines in Loki.
- `just bench-compare` shows no decode regression beyond noise for any `decode_benchmark` case (TIFF, JPEG, PNG, J2K) (H3).
- The first release after the merge logs equal tested and pushed digests.

## References

- `src/cli/cpp/cli_app.cpp:209-222` (`run_query`), `:838-846` (unhandled exception)
- `src/metadata/cpp/exif.cpp:557-568`, `src/metadata/cpp/iptc.cpp:18-60`
- `bazel/exiv2.BUILD.bazel:1-21`, `:56-65`
- `src/format_handlers/cpp/SipiIOJ2k.cpp:325-350`, `:1906-1916`
- `src/format_handlers/cpp/SipiIOTiff.cpp:1534-1563`, `:655`, `:903`
- `src/image/cpp/SipiImage.cpp:280`, `:386-425`, `:522-541`; `src/image/cpp/SipiIO.h:128-148`, `:194`
- `src/throttling/cpp/SipiPeakMemory.h:36-90`
- `.github/workflows/publish.yml`, `justfile` (`bazel-docker-*` recipes), `src/BUILD.bazel:732-780`
- `docs/learnings/architecture/kakadu-teardown-order-and-pod-templates-accepting-std-string.md`
