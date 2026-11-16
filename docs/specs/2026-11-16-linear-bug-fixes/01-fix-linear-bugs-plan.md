---
title: "fix: open Vitrinli bugs from Linear (response headers, decode deadline, metadata round-trip)"
type: fix
date: 2026-11-16
author: "Ivan Subotic"
status: implemented
repository: dasch-swiss/vitrinli
repositories: []
linear: DEV-7602, DEV-7600, DEV-7601, DEV-7599, DEV-7597, DEV-7598, DEV-7604, DEV-7603, DEV-7857, DEV-7606, DEV-7607, DEV-7621, DEV-6214, DEV-6215
---

# fix: open Vitrinli bugs from Linear

## Overview

This plan covers the 13 open Linear issues with a bug label for Vitrinli, plus DEV-7857: it fixes 12 of them and DEV-7857 in eight phases and seven parallel units; DEV-6215 is already fixed on `main` (see below). The security-advisory follow-ups DEV-7876 and DEV-7832 are not in scope. The research at `main` 5dcdb7e (release 10.1.0) found every issue still present, except one issue and one item:

- DEV-6215 is fixed on `main`. Both leaks it names are gone: the JPEG reader hands the pixel buffer to `img` before the longjmp window (`SipiIOJpeg.cpp:804-809`), and the writer's `FileBuffer` and `destmgr` are `unique_ptr`s declared before `setjmp` (`SipiIOJpeg.cpp:1095-1099`). It gets no phase; the issue is closed (H2).
- DEV-7599 item 1 (error responses carry no CORS headers) is fixed on `main`: `with_cors` wraps every IIIF response (`routes.rs:282-292`), and `cors_error_responses_with_origin` (`test/e2e/tests/iiif_compliance.rs:583`) covers 400, 401, 403 and 404.

## Problem Statement / Motivation

The issues fall into four groups:

- **Response headers and the cache (DEV-7602, DEV-7600, DEV-7601, DEV-7599).** Three of them can expose a restricted render or internal preflight data to the wrong caller: the cache key ignores which watermark was applied, image and `/file` responses can be stored by a shared cache although their bytes depend on the caller's credentials, and the 401 `info.json` copies every unknown preflight key into its public body.
- **Decode deadline and admission (DEV-7597, DEV-7598, DEV-7604, DEV-7603, DEV-7857).** A spinning codec pins a core until restart; the 60 s request timeout fires before the 120 s decode deadline, so the engine's own timeout path never runs; the Lua `SipiImage.dims` probe has no deadline; restricted region requests do full-image work in the Tile lane; and the default deadline exists twice.
- **Metadata round-trip (DEV-7606, DEV-7607, DEV-7621).** Vitrinli loses metadata it wrote itself (ExtendedXMP), loses rights metadata held only in IFD0 copies (in conflict with the owner rule of 2026-10-20 that embedded legal metadata is always returned), and writes the wrong TIFF YResolution.
- **C++ error path (DEV-6214).** `memTiffOpen` throws from inside an `extern "C"` block.

## Proposed Solution

One phase per coherent change, cut so that phases share no contract, except Phases 3 and 4: Phase 4's tests use the decode-timeout knob Phase 3 adds, so they form one chain.

| Phase | Issues | Module scopes |
|---|---|---|
| 1 Response headers | DEV-7600, DEV-7599, DEV-7601 | `server` |
| 2 Watermark cache | DEV-7602 | `ffi` |
| 3 Decode deadline | DEV-7857, DEV-7598, DEV-7604 | `ffi`, `cli`, `server` |
| 4 Wedged threads | DEV-7597 | `server` |
| 5 Restricted admission | DEV-7603 | `throttling`, `server` |
| 6 ExtendedXMP read | DEV-7606 | `metadata`, `format_handlers` |
| 7 Rights in IFD0 copies | DEV-7607 | `metadata` |
| 8 TIFF writer fixes | DEV-7621, DEV-6214 | `format_handlers` |

## Alternative Approaches Considered

| Option | Why not |
|---|---|
| DEV-7602: put the watermark identity (hash of resolved path plus mtime) into the cache key | It adds a stat and a hash per request and a key format change for a rarely used feature. Skipping the cache for watermarked output is what the code comment already promises, and it cannot serve one caller's watermark to another. |
| DEV-7600: add a `SipiServeRequest` field so C++ emits `private` and `Vary` | It changes an FFI layout. `apply_headers` (`sink.rs:318`) is last-write-wins, so the shell can set `Cache-Control` after the engine, and `with_cors` already adds to `Vary` with a check-then-append (`routes.rs:1959-1986`). |
| DEV-7597: cooperative cancellation through Kakadu `kdu_thread_env` | The known hang class wedges in the single-threaded box parse in `read_shape`, before any `kdu_thread_env` exists, and the operator docs state Kakadu has no clean abort (`docs/src/operation/health-endpoint.md:49-56`). |
| DEV-7597: decode in a killable worker process | A new process model, IPC and a pixel-transfer path. Far larger than the risk; the shed-and-alert option meets the issue's "Done when". |
| DEV-7598: a fixed 50 s server default with a startup refusal when it is not shorter than the request timeout | Every server with `SIPI_REQUEST_TIMEOUT` at or below 50 s would fail to start after the upgrade, the e2e servers at `test/e2e/tests/resource_limits.rs:117,339` (1 s and 5 s) included. |
| DEV-7857: the CLI reads `engine_context_or_default()` | The CLI uses the value as a `constexpr` struct default (`convert_service_file.h:27`, `convert_access_file.h:74`); a runtime lookup would turn defaults into calls. A shared `constexpr` is enough. |
| DEV-7603: classify restricted identifiers ahead of time through the preflight cache | It needs a cache the shell does not have for classification, and it is wrong on the first request for each identifier. |
| DEV-7603: keep the accepted residual in ADR-0028 | ADR-0028 itself names Full-lane routing as the follow-up, and a crawler on restricted non-pyramidal TIFFs can still fill the Tile lane's partition. |

## Technical Considerations

### Decisions per issue

- **DEV-7600.** In the IIIF route, when a preflight hook is configured (`state.has_preflight` for image responses, `state.has_file_preflight` for `/file`), the shell adds `private` to the engine's `Cache-Control` value (`private, must-revalidate, …`; `/file`: `private` replaces `public`) and adds `Cookie` and `Authorization` to `Vary` with the check-then-append idiom `with_cors` uses for `Origin` (`routes.rs:1959-1986`: scan every `Vary` value, `h.append` a separate `Vary` header for each missing token). This applies to every status the route emits for those kinds (200, 206, 304 and errors); when the response has no `Cache-Control` (the engine's error responses), the shell sets `Cache-Control: private`. One helper, `fn mark_private(headers: &mut HeaderMap)`, runs in `iiif()` after `outcome_rx` on both the `Complete` and the `StreamHead` branch, so it covers every response the engine dispatch produces; the early traversal 404 (`routes.rs:327`) and the 503 shed responses are exempt. Without a hook, headers stay as they are. Rejected: `no-store`, as `info.json` sends. DSP always runs with a hook, so `no-store` would end browser caching of every public tile; `private` blocks only shared caches, which is the exposure the issue names.
- **DEV-7599.** Item 1 is already fixed (see Overview). Item 2: in reflect mode (empty allowlist) every response carries `Vary: Origin`, also one to a request without an Origin header, because the response varies on it; `cors_allow` returns `vary: true` in every branch. Item 3: the literal Origin `null` is denied on every route and in both modes, also when `SIPI_ALLOWED_ORIGINS` lists it; the deny carries `Vary: Origin`. The `CorsDecision` and `cors_allow` doc comments (`routes.rs:2270-2290`) state the new rules. Item 4: `info.json` adds `Accept` to `Vary`, because its Content-Type and `Link` depend on `Accept` (`routes.rs:2457`). Item 5 is accepted without a change: reflecting `Access-Control-Request-Headers` grants a cross-origin caller nothing more than the credentials the response already accepts, and Vitrinli reads no custom request header that changes authorization. The PR description records this acceptance.
- **DEV-7601.** `info::auth_service` (`info.rs:290-321`) builds `@context`, `@id` (from `cookieUrl`), `profile` and the nested `service` (from `tokenUrl` and `logoutUrl`) itself. From the preflight kv it copies only `label`, `header`, `description`, `confirmLabel`, `failureHeader` and `failureDescription`. Every other key, `size`, `watermark`, `@id`, `profile` and `service` included, is dropped with a `debug!` log naming the key, never its value, so the kv can no longer override a field the shell builds.
- **DEV-7602.** `build_image_response` skips the cache read (`serve_image.cpp:736-755`) and the cache write (`:1034-1038`) when a watermark is applied (non-empty resolved watermark; an empty string is no watermark and still uses the cache, as `restrict_watermark` already treats it). Entries with the `/1` marker already in the cache are never read again. The `/0`/`/1` marker stays in the canonical URL; only cache use changes. The comment at `serve_image.cpp:736` becomes true. Deploy note: no cache purge is needed, because no new key format is introduced; stale watermarked entries are never read again and age out by LRU.
- **DEV-7857.** One `inline constexpr std::size_t kDefaultDecodeTimeoutMs = 120000;` in `src/ffi/cpp/engine_context.h` (the type of `EngineContext::decode_timeout_ms` and of the CLI args fields) is the default for `EngineContext::decode_timeout_ms` and for every offline verb (`cli_app.cpp`, `convert_service_file.h`, `convert_access_file.h`); `kDecodeTimeoutMs` in `decode_deadline.h` is deleted. The literal in `serve_image_test.cpp:981` uses the constant too. The `cli -> ffi` dependency already exists (`src/cli/cpp/commands/BUILD.bazel:59`) and is legal per `ARCH-MAP.md`. The offline verbs keep 120 s: ingest converts very large masters offline, and no request timeout applies there.
- **DEV-7598.** The decode deadline is derived from the request timeout unless it is set. The issue's "Done when" asks for a knob ("the decode deadline is configurable (no knob exists today)"), so a new server knob `SIPI_DECODE_TIMEOUT_MS` (CLI `--decode-timeout-ms`, TOML `decode_timeout_ms`; the name the JP2 watchdog design gave this follow-up) sets it in milliseconds. Unset, the shell uses 80 % of the request timeout (48 s at the default 60 s, 800 ms at the e2e's 1 s). Set, it must be shorter than the request timeout, or the server refuses to start with an error that names both values. The check and the derivation live in `server_main` in `src/server/rust/src/lib.rs`, after the CLI, env and TOML overrides are merged; the request timeout is env-only (`request_timeout_from_env`, today read at `lib.rs:486`), so `server_main` reads it once and passes it on. This follows the shell-owned-default pattern of `large_decode_threshold_bytes` (`config.rs:515-533`, `init.cpp:145-148`). The shell always sends the resolved value with `has_decode_timeout_ms = 1`, for both config flavors, so `init.cpp` (`:121-149`, `:245-261`) always sets `EngineContext::decode_timeout_ms` in the server; `kDefaultDecodeTimeoutMs` (120 s) stays the default only for the offline verbs and for code that runs without an engine context. The JP2 watchdog design (`docs/specs/2026-09-04-02-jp2-decode-watchdog-design.md`) chose 120 s because "a legitimate large-JP2 decode completes in seconds; a 2-minute wall clock trips only on a true hang". That reason still holds at 48 s, and above 60 s the request timeout answers 408 first anyway. Model for the knob: `large_decode_threshold_bytes` (`src/cli/rust/src/commands/server/args/limits.rs`, `src/cli/rust/src/commands/server/mod.rs:79,121`, `config_file.rs`, `config.rs` `ServerOverrides` and `SipiServerConfig` with a `has_` flag, `sipi_ffi.h`, `init.cpp`). The value (`size_t`, offset 224) and its `has_decode_timeout_ms` flag (offset 232) are appended after `has_large_decode_threshold_bytes` (offset 220, `sipi_ffi.h:346`), with an `offset_of!` in `config.rs` and a `static_assert(offsetof…)` in `sipi_ffi.h` for each, and any `sizeof(SipiServerConfig)` assertion updated from 224 to 240. The Lua config flavor gets no key (as for the other admission knobs, `config.rs:278-283`). No ops-deploy change is needed. Residue, accepted: one request can run two deadlines in a row (the `image_dims` probe, then the decode), so in the worst case it still gets a 408; the check compares one deadline, because the probe only hangs on files that then also hang the decode.
- **DEV-7604.** `sipi_image_file_dims` (`image_handle.cpp:237-253`) runs `read_shape` under `run_with_deadline` with `engine_context_or_default().decode_timeout_ms`, mirroring the probe in `sipi_image_dims` (`sipi_ffi.cpp:188-207`) and capturing `path` as a `std::string` copy because the worker may be detached; on timeout it emits `image decode deadline exceeded` through `err` and returns non-zero, as `sipi_image_new` does (`image_handle.cpp:194`). The Lua binding (`src/scripting/rust/engine_ffi.rs:513-531`) already turns a non-zero code into `Err(String)`, so Lua gets a Lua error with that message. `run_with_deadline` increments `wedged_threads` (`decode_guard.h:50`), so these timeouts count toward the Phase 4 cap.
- **DEV-7597.** Shed and alert. A new shell-only knob `SIPI_MAX_WEDGED_THREADS` (CLI `--max-wedged-threads`, in `ConcurrencyArgs`, `src/cli/rust/src/commands/server/args/concurrency.rs`, next to `--nthreads`; no TOML key, like `nthreads`, `config_file.rs:22`) sets a cap; the default is `max(1, nthreads - 1)`, `nthreads` being the shell's resolved `--nthreads` serve knob (`routes.rs:143-145`, stored as `AppState.nthreads` at `:159`), which matches the restart point the operator docs already give (`health-endpoint.md:55`). The flag is an `Option<u32>` passed like `nthreads` (`sipi::run` → `server_main` → `AppState::load`, `routes.rs:131`) and resolved only in `AppState::load`, in both its ready and not-ready branches (`routes.rs:218`); it never crosses the seam. In `iiif()`, after the Redirect branch (`routes.rs:331-333`) and before `acquire` (`:341`), the shell reads `wedged_threads` from the metrics snapshot it already has over the seam (`SipiMetricsSnapshot`, `ffi.rs:299`); at or above the cap it answers with `busy_response()` (503, `Retry-After`) and logs one `warn!` per transition into the shed state. This sheds every request `iiif()` admits, cache hits, `/file`, `info.json` and `knora.json` included, because the shell cannot tell a cache hit before dispatch. The check runs once, at the front of `iiif()`; Phase 5's later Full acquire does not repeat it. Lua routes and docroot paths are not shed (out of scope). A `None` from `ffi::metrics_snapshot()` (`ffi.rs:848`) counts as not wedged. `/health` is not changed: the issue's "Done when" is met by shedding and alerting, and a liveness change would restart production on a condition operators now handle by alert (rejected as beyond scope; it can follow as its own issue). The health page's claim that `/health` reports `wedged_threads` is false today and is corrected to name the metric instead. `wedged_threads` never decreases, so the shed state lasts until restart. The `sipi.wedged_threads` gauge is already exported over OTLP (`metrics.rs:530`); the alert rule on it is an operator action (H5); the operator restarts on the alert. The operator docs replace "restart at `nthreads - 1`" with this behaviour. Wedged threads are still not reclaimed in-process.
- **DEV-7603.** Re-classify after the preflight. The Tile permit is acquired in async `iiif()` (`routes.rs:341-348`) and moved into `spawn_blocking`, and the preflight and `routes::restriction` run on that blocking thread, in `serve_image` (`routes.rs:906`), which `dispatch_engine` (`routes.rs:406`) calls at `:475`. So the permit is threaded as `&mut Option<Permit>` through `dispatch_engine` into `serve_image` instead of the closure holding it. Every size restriction triggers the swap, because a restricted decode is always a whole-image decode (ADR-0028), which is Full-class work by ADR-0022's own definition. A `Refuse` or `Misconfigured` result swaps nothing. A new `impl Permit { pub fn kind(&self) -> AdmissionKind }` in `src/throttling/rust/lib.rs` (Full when the private `_full` permit is present, else Tile; `Permit` has no kind accessor today, `lib.rs:128-131`) lets `reclassify_to_full` and the classification record read the held lane. The closure at `routes.rs:368-374` (`let _permit = permit;`) becomes `let mut permit = Some(permit);` and passes `&mut permit`. When `restriction` yields a size restriction and the held permit is a Tile permit, `serve_image` drops it first, then acquires a Full permit with `tokio::runtime::Handle::current().block_on(admission.acquire(AdmissionKind::Full))` (`serve_image` takes `admission: &Admission`, `routes.rs:850-857`), inside a small `fn reclassify_to_full(admission: &Admission, permit: &mut Option<Permit>) -> bool` that the unit tests call directly (legal on a `spawn_blocking` thread; the wait is bounded by the admission queue timeout, and the Tile permit is never held while it waits, as ADR-0022 requires). `Shed` or `TimedOut` completes the outcome with `busy_response()`. The parked blocking thread holds no permit, so the concurrency bound holds; tokio's blocking pool is far larger than `nthreads`. Rejected: returning to the async layer to re-acquire, which needs a second `spawn_blocking` and a split of `dispatch_engine` for one case. At `routes.rs:972` the call passes `permit.as_ref().map(Permit::kind)` as the shell kind instead of `admission.classify(parsed)` (the method, `throttling/rust/lib.rs:321`, keeps its signature; after a failed swap `serve_image` has already returned), so `classifier_disagreement_total` no longer counts these requests. ADR-0028's accepted-residual bullet (line 128-134) is replaced in place, since the ADR names it as a follow-up; ADR-0022 gets a dated `**Amendment (2026-11-…, DEV-7603):**` paragraph at the top, the convention its earlier amendments use.
- **DEV-7606.** The JPEG reader collects every APP1 segment with the ExtendedXMP signature (`http://ns.adobe.com/xmp/extension/\0`), groups them by GUID, reassembles the chunk whose GUID matches `xmpNote:HasExtendedXMP` in the StandardXMP, checks that offsets and full length agree, and merges the extension's `rdf:Description` properties into the StandardXMP packet. The reassembly and merge live in `src/metadata/cpp/xmp.cpp` next to `Xmp::split_extended`, as `Xmp::merge_extended(std::string_view standard, <the collected extension segments>) -> std::string`; `SipiIOJpeg::read()` and `read_shape()` call it. The scan is linear (no `std::regex`: a regex took about 7 s on a 440 KB packet in an earlier round). A malformed or incomplete extension is ignored and the StandardXMP is kept as it is, with a `debug` log. The merged packet drops `xmpNote:HasExtendedXMP`. The extension's declared full length is capped at 16 MiB (the JPEG reader's ceiling; larger declared lengths are ignored as malformed).
- **DEV-7607.** On output, when the image has no primary XMP and the EXIF holds `Exif.Image.XMLPacket`, the packet becomes the primary XMP; likewise `Exif.Image.IPTCNAA` becomes the primary IPTC when no primary IPTC exists. The promotion happens once, in a new `SipiImage::promote_ifd0_copies()` that `SipiImage::write` calls before it dispatches to the format writer (`src/image/cpp/SipiImage.cpp`), so all four writers get it; on the IIIF path it changes only the per-request decode owned by `ImageEncodeProducer` (`serve_image.cpp:324-334`, `:1044`); `SipiCache` stores encoded files, not images. The Lua `SipiImage` handle (`image_handle.cpp:676`, `:701`) can be written more than once; the promotion is idempotent there because it runs only while no primary block exists. A copy that does not parse fully (XMP: Exiv2 rejects the packet; IPTC: Exiv2 rejects the IIM block) is not promoted (fail closed, logged); no partial promotion. A promoted XMP larger than one APP1 segment goes through the existing `Xmp::split_extended` path on JPEG output. Restricted outputs apply the same rules to a promoted packet as to a primary one (preview drop and size rewrite, `restrict_image_resources` and the XMP thumbnail rule). When a primary block exists, the IFD0 copy is ignored.
- **DEV-7621.** `SipiIOTiff.cpp:2050` sets `TIFFTAG_YRESOLUTION` from `Exif.Image.YResolution`.
- **DEV-6214.** `memTiffOpen` moves out of the `extern "C"` block (its only caller, `SipiIOTiff.cpp:1904`, is C++). It keeps throwing `SipiImageError` on allocation failure; the C++ caller already handles that.

### Cross-cutting

- **Security.** Phases 1 and 2 close information leaks; the security reviewer is bound to `src/server/rust/**` and `src/ffi/**` in `eng.yaml`.
- **Paths.** ADR-0026 (status `proposed`) would rename `src/ffi` to `src/engine`. If it lands during the run, a unit uses the new path for the same file.
- **FFI layout.** Phase 3 adds fields to `SipiServerConfig`; both sides' `offset_of!` and `static_assert(offsetof…)` checks are updated in the same commit (`docs/learnings/architecture/ffi-seam-rust-mirrors-and-signature-drift.md`).
- **Timeout tests.** Every timeout test uses small values and is bounded on four axes: server timeout via env, client `.timeout`, `Drop` teardown of a spawned server, and a Bazel test size (`docs/learnings/testing/timeout-e2e-must-be-bounded.md`). Deadline tests use the `/hang/*.jp2` fixtures and run the real watchdog thread, also under ASan.
- **Hot path.** Phases 6, 7 and 8 change `src/format_handlers/*` read and write code, which the hot-path rule in `CLAUDE.md` covers. The changes are metadata-only, but a before/after `just bench` and `just bench-compare` on the same `-c opt` binary is still owed. It needs a macOS or Linux host with Bazel, so it is H6 (after ship, before merge).
- **Gates in the cloud.** Bazel cannot fetch dependencies in the cloud container. A unit thread runs `just bazel-rustfmt-check`, `just bazel-clippy-check` and the unit's tests where it can; where it cannot, it pushes its draft PR and reads the CI result, and the gate counts as met only when CI is green on the unit's head.
- **Approval goldens.** Phases 6 to 8 can change metadata bytes in outputs. Each runs `just bazel-test-approval`; a changed golden is re-approved with a `test/approval/CHANGELOG.approval.md` row that names the reason.
- **Stacking without `gh stack`.** The cloud container has no `gh stack`. Owner decision 2026-11-16: units stack with plain git (`git rebase --onto origin/<top> <started_from> <unit branch>`), push, and open a draft PR with the GitHub MCP whose base is the current top branch. The owner adopts the stack with `gh stack init`/`gh stack link` on a Mac before the merge (H4). A unit thread whose session assigns a different branch name uses that name and reports it in `done`.

## Implementation Phases

#### Phase 1: Response headers for hook-gated and cross-origin responses

depends on: none

- [ ] DEV-7600: in the IIIF route, add `private` to `Cache-Control` and add `Cookie` and `Authorization` to `Vary` (check-then-append, as `with_cors` does) on image responses when `state.has_preflight`, and on `/file` responses when `state.has_file_preflight`, for every status; leave headers unchanged without a hook
- [ ] DEV-7599 item 2: `cors_allow` returns `vary: true` in every branch, so in reflect mode every IIIF response carries `Vary: Origin`, with or without an Origin header
- [ ] DEV-7599 item 3: `cors_allow` denies the literal Origin `null` on every route and in both modes, also when allowlisted; the deny carries `Vary: Origin`; doc comments updated
- [ ] DEV-7599 item 4: `json_cors` adds `Accept` to `Vary` for `info.json`
- [ ] DEV-7601: `info::auth_service` copies from the kv only `label`, `header`, `description`, `confirmLabel`, `failureHeader` and `failureDescription`; it keeps building `@context`, `@id`, `profile` and `service` itself; every other kv key is dropped with a `debug!` log of the key name
- [ ] Rust unit tests in `routes.rs` (`cors_allow` for reflect, `null` and allowlist modes; header merge with and without a hook) and `info.rs` (an unknown key such as `size` or `secret` never reaches the 401 body; every allowlisted key survives; a kv `@id` or `profile` does not override the built one). Regression caught: a hook-gated response without `private`, a reflected `null` Origin, a leaked preflight key
- [ ] e2e in `test/e2e/tests/security.rs`, modelled on `restricted_info_json_hides_native_resolution_and_tiles`: an image and a `/file` response behind the preflight hook carry `Cache-Control` with `private` and `Vary` with Cookie and Authorization, on a 200 and on an engine error status (a preflight 401 or 403, or a 404 for a missing file); in `iiif_compliance.rs`: the image route echoes `Vary: Origin` in reflect mode, `Origin: null` gets no `Access-Control-Allow-Origin`, `info.json` varies on `Accept`
- [ ] `just bazel-rustfmt-check && just bazel-clippy-check` and `just bazel-test` pass (CI where the container cannot build); `bazel test //src/server/... //test/e2e:all_e2e --test_output=errors` is the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 2: Never cache watermarked output

depends on: none

- [ ] DEV-7602: `build_image_response` (`src/ffi/cpp/serve_image.cpp`) skips the cache read and the cache write when a non-empty watermark is applied; the comment states the rule
- [ ] `serve_image_test` case modelled on `RestrictedImage.CacheKeySeparatesRestrictions`: two requests for the same file and coordinates with two different watermarks return different bytes and leave `cache.getNfiles()` unchanged; an unwatermarked request and a request with an empty watermark string still write one cache file. Regression caught: one caller's watermark served to another from the cache
- [ ] `just bazel-test` passes (CI where the container cannot build); `bazel test //src/ffi:serve_image_test --test_output=errors` is the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 3: One decode deadline, shorter than the request timeout

depends on: none

- [ ] DEV-7857: add `kDefaultDecodeTimeoutMs` (`std::size_t`) to `src/ffi/cpp/engine_context.h`; `EngineContext::decode_timeout_ms`, every `kDecodeTimeoutMs` call site in `cli_app.cpp`, and the struct defaults in `convert_service_file.h` and `convert_access_file.h` use it; delete `kDecodeTimeoutMs` from `src/cli/cpp/commands/decode_deadline.h`; `serve_image_test.cpp:981` uses the constant
- [ ] DEV-7598: add `SIPI_DECODE_TIMEOUT_MS` / `--decode-timeout-ms` / TOML `decode_timeout_ms` through `src/cli/rust/src/commands/server/args/limits.rs`, `src/cli/rust/src/commands/server/mod.rs`, `config_file.rs`, `config.rs` (`ServerOverrides`, `SipiServerConfig` with `has_decode_timeout_ms`), `sipi_ffi.h` and `init.cpp` (sets `EngineContext::decode_timeout_ms`); append the value and its flag after `has_large_decode_threshold_bytes`, with an `offset_of!` in `config.rs` and a `static_assert(offsetof…)` in `sipi_ffi.h` for each
- [ ] DEV-7598: in a pure `resolve_decode_timeout(set: Option<u64>, request: Duration) -> Result<u64, String>` called by `server_main` (`src/server/rust/src/lib.rs`), resolve the decode deadline (set value, else 80 % of the request timeout), refuse to start when a set value is not shorter than the request timeout (error names both values), and always send the resolved value with `has_decode_timeout_ms = 1`
- [ ] DEV-7604: `sipi_image_file_dims` runs `read_shape` under `run_with_deadline` with `engine_context_or_default().decode_timeout_ms`; on timeout it emits `image decode deadline exceeded` through `err` and returns non-zero
- [ ] Tests: Rust unit tests for the derived default (60 s gives 48 s, 1 s gives 800 ms), a set value, and the startup refusal (set by CLI, by env, by TOML); the existing `resource_limits.rs` servers with `SIPI_REQUEST_TIMEOUT=1` and `=5` still start; a `seam_probe_test` case that sets a 2 s deadline with `set_engine_context` and calls `sipi_image_file_dims` on `test/_test_data/images/hang/dev7080_read_shape_hang.jp2`, expecting the error message within the deadline plus a 10 s margin, tagged and sized as `serve_image_test`'s existing hang cases are (`serve_image_test.cpp:943-971`), restoring the previous engine context in teardown, because `set_engine_context` is process-global. Regression caught: an unbounded Lua dims probe, a decode deadline at or above the request timeout, a server that no longer starts with a short request timeout
- [ ] Docs: `docs/src/guide/running.md` (flag and env tables, the "Decode deadline" section: 80 % of the request timeout for the server unless set, 120 s for offline verbs), `docs/src/operation/admission-control.md` (the timeout order), `ARCH-MAP.md` throttling entry (`:105` names `kDecodeTimeoutMs`)
- [ ] `just bazel-rustfmt-check && just bazel-clippy-check` and `just bazel-test` pass (CI where the container cannot build); `bazel test //src/ffi/... //src/cli/... //src/server/... //test/e2e:all_e2e --test_output=errors` is the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 4: Shed load when wedged decode threads accumulate

depends on: Phase 3 (this phase's e2e and shed test use the `SIPI_DECODE_TIMEOUT_MS` knob Phase 3 adds, to make a hang wedge in 2 s)

- [ ] DEV-7597: add `SIPI_MAX_WEDGED_THREADS` / `--max-wedged-threads` (`Option<u32>` like `nthreads`, unset by default; resolved in `AppState::load` to `max(1, nthreads - 1)` when unset) in `ConcurrencyArgs`, threaded like `nthreads` through `sipi::run`, `server_main` and `AppState::load` into `AppState`; no TOML key, no seam field
- [ ] DEV-7597: in `iiif()`, after the Redirect branch and before `acquire`, the shell reads `wedged_threads` from the metrics snapshot; at or above the cap it answers `busy_response()` and logs one `warn!` on entering the shed state
- [ ] Tests: a Rust unit test for the shed decision at cap minus one, at cap and above; an e2e in `test/e2e/tests/resource_limits.rs` that starts a server with `SIPI_DECODE_TIMEOUT_MS=2000` and `SIPI_MAX_WEDGED_THREADS=1`, uses a dedicated server torn down by `Drop`, requests `/hang/dev7080_read_shape_hang.jp2/full/max/0/default.jpg` and expects 500 (the deadline error) within 2 s plus a 10 s margin, then expects 503 with `Retry-After` for the next image request; fixture `test/_test_data/images/hang/dev7080_read_shape_hang.jp2`; bounded per the timeout-test learning. Regression caught: a spinning codec that takes every core
- [ ] Docs: `docs/src/operation/health-endpoint.md` (its claim that `/health` reports `wedged_threads` is corrected: the gauge is a metric only) and `docs/src/guide/running.md` describe the cap, the 503 and the `sipi.wedged_threads` alert; the gauge description in `metrics.rs:530` drops "restart when it reaches nthreads-1"
- [ ] `just bazel-rustfmt-check && just bazel-clippy-check` and `just bazel-test` pass (CI where the container cannot build); `bazel test //src/server/... //test/e2e:all_e2e --test_output=errors` is the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 5: Route size-restricted requests to the Full lane

depends on: none

- [ ] DEV-7603: add `impl Permit { pub fn kind(&self) -> AdmissionKind }` in `src/throttling/rust/lib.rs` (Full when the private `_full` permit is present, else Tile; `Permit` has no kind accessor today, `lib.rs:128-131`); thread the permit as `&mut Option<Permit>` (the closure at `routes.rs:368-374` becomes `let mut permit = Some(permit);`) through `dispatch_engine` into `serve_image` (`routes.rs:475`, `:857`; `Permit`, `AdmissionKind` and `Acquired` from `src/throttling/rust/lib.rs:75,117,128`); right after `restriction()` (`routes.rs:906`) returns a size restriction while a Tile permit is held, call `reclassify_to_full(admission, permit)`, which in one ordered step drops the Tile permit and then acquires a Full permit with `Handle::current().block_on(admission.acquire(AdmissionKind::Full))`; on `Shed`/`TimedOut` it returns false and `serve_image` answers `busy_response()`. A `Refuse` (403) or `Misconfigured` (500) result keeps the Tile permit and swaps nothing
- [ ] DEV-7603: at `routes.rs:972`, pass the held permit's `kind()` to `record_classification` instead of `admission.classify(parsed)`, so swapped requests no longer count in `classifier_disagreement_total`; a unit test covers it
- [ ] Tests: a unit test in `src/throttling/rust` for the re-classification (Tile permit released, Full permit held, ordering kept); a `routes.rs` unit test of `reclassify_to_full` (Tile permit dropped before the Full acquire; a shed or timed-out Full acquire returns false); an e2e in `security.rs` that sends a restricted region request and asserts only that it is served (the lane is covered by the unit tests). Regression caught: restricted full-image decodes that fill the Tile lane
- [ ] ADR-0028: replace the accepted-residual bullet (lines 128-134) in place; ADR-0022: add a dated `**Amendment (…, DEV-7603):**` paragraph at the top on re-classification after the preflight
- [ ] `just bazel-rustfmt-check && just bazel-clippy-check` and `just bazel-test` pass (CI where the container cannot build); `bazel test //src/throttling/... //src/server/... //test/e2e:all_e2e --test_output=errors` is the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 6: Read ExtendedXMP back from JPEG

depends on: none

- [ ] DEV-7606: add the linear ExtendedXMP reassembly and merge to `src/metadata/cpp/xmp.cpp` next to `Xmp::split_extended`; `SipiIOJpeg::read()` and `read_shape()` use it
- [ ] Tests in `jpeg_metadata_segments_test.cpp`: extend `LargeXmpIsSplitIntoExtendedXmpKeepingRightsInTheStandardPacket` so a write-then-read round-trip returns every property; cases for a missing chunk, a wrong GUID, overlapping offsets and a declared length over the cap (each keeps the StandardXMP unchanged). Regression caught: ExtendedXMP properties lost on re-read
- [ ] `just bazel-test` passes, approval tests included (CI where the container cannot build); any re-approved golden has a `test/approval/CHANGELOG.approval.md` row; `bazel test //src/metadata/... //src/format_handlers:formats_test --test_output=errors` and `just bazel-test-approval` are the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 7: Keep rights metadata held only in IFD0 copies

depends on: none

- [ ] DEV-7607: promote `Exif.Image.XMLPacket` to the primary XMP and `Exif.Image.IPTCNAA` to the primary IPTC when the primary block is absent, in `SipiImage::write` before the format writer runs; do not promote a copy that does not parse
- [ ] DEV-7607: restricted outputs apply the preview drop and size rewrite to a promoted packet
- [ ] Tests: a synthesized fixture (built in the test with `Exiv2::TiffParser::encode`, as `ExifImageResources.ExifBytesNeverWritesTheRawXmpOrIptcCopies` in `pixel_dimensions_test.cpp` does) whose rights exist only in the IFD0 copies; the rights survive in jpg, png, tif and jp2 output; a restricted output of the same fixture has no preview and states the delivered size; a primary block wins over a copy; a copy that does not parse is not promoted; a promoted XMP over one APP1 segment is written through `Xmp::split_extended`; a watermarked restricted request of the fixture still carries the promoted rights. Update `ExifBytesNeverWritesTheRawXmpOrIptcCopies` only if its assertion conflicts. Regression caught: rights metadata lost in every output
- [ ] `just bazel-test` passes, approval tests included (CI where the container cannot build); `bazel test //src/metadata/... //src/format_handlers:formats_test //src/ffi:serve_image_test --test_output=errors` and `just bazel-test-approval` are the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

#### Phase 8: TIFF writer fixes

depends on: none

- [ ] DEV-7621: `SipiIOTiff.cpp:2050` sets `TIFFTAG_YRESOLUTION` from `Exif.Image.YResolution`
- [ ] DEV-7621: a round-trip test in `tiff_exif_rational_test.cpp` with different X and Y resolutions asserts both survive write and read, and that XResolution is unchanged. Regression caught: YResolution written into XResolution
- [ ] DEV-6214: move `memTiffOpen` out of the `extern "C"` block unchanged; it stays `static`
- [ ] `just bazel-test` passes, approval tests included (CI where the container cannot build); `bazel test //src/format_handlers:formats_test --test_output=errors` and `just bazel-test-approval` are the fast local loop
- [ ] Phase review: adversarial review of this phase's commits; verified findings fixed before the next phase starts

## Threads

Up to 7 units in parallel. Units stack in the order they finish (`thread-coordination.md`); the stack top is the last stacked unit branch. Units `headers`, `deadline` and `admission` all edit `routes.rs`, and all three touch `iiif()` in different places (`headers` after `outcome_rx`, `deadline` before `acquire`, `admission` the `spawn_blocking` closure); a conflict is resolved in the unit's stack-on rebase, as the stack rules say.

```yaml
plan_branch: claude/project-thread-rd0r37
max_parallel: 7
units:
  - unit: headers
    phases: [1]
    depends_on: []
    base: plan
    branch: impl/bugs-headers
  - unit: watermark-cache
    phases: [2]
    depends_on: []
    base: plan
    branch: impl/bugs-watermark-cache
  - unit: deadline
    phases: [3, 4]
    depends_on: []
    base: plan
    branch: impl/bugs-deadline
  - unit: admission
    phases: [5]
    depends_on: []
    base: plan
    branch: impl/bugs-admission
  - unit: extended-xmp
    phases: [6]
    depends_on: []
    base: plan
    branch: impl/bugs-extended-xmp
  - unit: ifd0-rights
    phases: [7]
    depends_on: []
    base: plan
    branch: impl/bugs-ifd0-rights
  - unit: tiff-writer
    phases: [8]
    depends_on: []
    base: plan
    branch: impl/bugs-tiff-writer
  - unit: final
```

## Human Actions

| Id | Action | Who | When | Why not the agent |
|----|--------|-----|------|-------------------|
| H1 | Read the plan PR, then answer the start card | Ivan | before start | Accepting the plan is the owner's |
| H2 | Close DEV-6215 in Linear as fixed on `main` (see Overview), and tick DEV-7599 item 1 | Ivan | before start | Linear triage is the owner's |
| H3 | Bump `quinn-udp` to >=0.5.16 on `main` and repin `Cargo.Bazel.lock` (RUSTSEC-2026-0538 fails `dependency-audit` on every PR since 2026-11-16) | Ivan | before start | The repin needs Bazel, which cannot fetch in the cloud container |
| H4 | On a Mac with `gh stack`: `gh stack init --base main <plan branch> <unit branches, bottom to top>`, `gh stack link`, then `gh stack merge --rebase <top PR>` | Ivan | after ship | `gh stack` is not available in the cloud container; merging is the owner's |
| H5 | Add a Grafana alert on `sipi.wedged_threads` reaching the cap; confirm that no shared HTTP cache sits in front of Vitrinli in the DSP deployment | Ivan | after ship | Infrastructure changes are operator-only (`CLAUDE.md`, Infrastructure boundary) |
| H6 | Before the merge, run `just bench` and `just bench-compare` (decode and encode benchmarks, `-c opt`) on `main` and on the stack top on the same machine; accept the delta per `docs/src/development/benchmarking.md` | Ivan | after ship | Needs a host where Bazel builds; the cloud container cannot |

## Requirements

- REQ-1 [B] (State-driven): While a preflight hook is configured, Vitrinli shall send a `Cache-Control` header that contains `private` and a `Vary` header that lists `Cookie` and `Authorization` on every image and `/file` response the engine dispatch produces (the early traversal 404 and 503 shed responses are exempt). Check: e2e in `security.rs` (Phase 1).
- REQ-2 [B] (Ubiquitous): Vitrinli shall list `Origin` in the `Vary` header of every IIIF response, in reflect and allowlist mode, with or without an Origin header on the request. Check: e2e in `iiif_compliance.rs` (Phase 1).
- REQ-3 [B] (Unwanted behaviour): If a request carries the Origin `null`, then Vitrinli shall send no `Access-Control-Allow-Origin` header, also when `null` is on the allowlist. Check: unit test and e2e (Phase 1).
- REQ-4 [B] (Ubiquitous): Vitrinli shall list `Accept` in the `Vary` header of every `info.json` response. Check: e2e (Phase 1).
- REQ-5 [B] (Event-driven): When Vitrinli answers `info.json` with 401, the auth-service block shall contain only the fields the shell builds (`@context`, `@id`, `profile`, `service`) and the preflight keys `label`, `header`, `description`, `confirmLabel`, `failureHeader` and `failureDescription`. Check: `info.rs` unit tests (Phase 1).
- REQ-6 [B] (Event-driven): When a watermark is applied to an image response, Vitrinli shall neither read the response from the cache nor write it to the cache. Check: `serve_image_test` (Phase 2).
- REQ-7 (Ubiquitous): The server and the offline verbs shall take the default decode deadline from one constant. Check: `grep -rn "120000" src/` finds it only in `engine_context.h` (Phase 3).
- REQ-8 [B] (Event-driven): When the configured decode timeout, after the CLI, env and TOML layers merge, is not shorter than the request timeout, the server shall refuse to start with an error that names both values. Check: Rust unit test of `resolve_decode_timeout` (Phase 3).
- REQ-9 [B] (Event-driven): When `SipiImage.dims` probes a file that does not finish within the decode deadline, Vitrinli shall return an error within the deadline plus a 10 s margin. Check: `seam_probe_test` (Phase 3).
- REQ-10 [B] (State-driven): While the number of wedged decode threads is at or above the configured cap, Vitrinli shall answer every engine-bound IIIF request with 503 and `Retry-After`. Check: e2e in `resource_limits.rs` (Phase 4).
- REQ-11 (Event-driven): When the preflight decision restricts a Tile-lane request's size, Vitrinli shall run its decode under a Full-lane permit and release the Tile permit first. Check: `src/throttling/rust` and `routes.rs` unit tests (Phase 5).
- REQ-12 [B] (Event-driven): When Vitrinli reads a JPEG whose XMP is split into StandardXMP and ExtendedXMP, it shall return every property of the original packet. Check: round-trip test in `jpeg_metadata_segments_test.cpp` (Phase 6).
- REQ-13 [B] (Event-driven): When an input has no primary XMP or IPTC block and has a parsable IFD0 copy, Vitrinli shall write that copy as the primary block in jpg, png, tif and jp2 output, with the restricted-output rules applied. Check: fixture test (Phase 7).
- REQ-14 [B] (Ubiquitous): Vitrinli shall write a TIFF's YResolution from the source's YResolution. Check: `tiff_exif_rational_test.cpp` round-trip (Phase 8).
- REQ-15 (Ubiquitous): No function that can throw a C++ exception shall have C linkage in `SipiIOTiff.cpp`. Check: `memTiffOpen` is outside every `extern "C"` block (Phase 8).
- REQ-16 (Ubiquitous): The decode and encode benchmarks shall show no regression above the baseline CV between `main` and the stack top. Check: H6.

## Out of Scope

- DEV-7876 and DEV-7832 (security-advisory follow-ups, the owner's own tasks).
- DEV-6215 (fixed on `main`; closed in H2).
- DEV-7236 (TIFF decode holds two full-size buffers; a separate plan with its benchmark gate).
- DEV-7599 item 5 (accepted; see Decisions per issue).
- Reclaiming a wedged thread (killable worker process or cooperative cancellation).
- A size limit for embedded metadata in outputs (DEV-7608).
- Any ops-deploy or infrastructure change (H5 is the operator's).

## Normative Sources

1. The owner's decisions of 2026-11-16: the scope (13 bugs plus DEV-7857) and stacking with plain branches plus draft PRs.
2. The owner rule of 2026-10-20: embedded metadata, especially legal metadata, is always returned.
3. `CLAUDE.md` and `CONVENTIONS.md` (scope discipline, hot-path benchmark rule, lint gates, infrastructure boundary).
4. ADR-0022 (two-lane admission), ADR-0028 (restricted-view coordinate space), ADR-0026 (engine seam).
5. IIIF Image API 3.0 and IIIF Authorization Flow; XMP Specification Part 3 (ExtendedXMP); the Fetch standard (CORS).
6. The Linear issues, as quoted in this plan.

## Dependencies & Risks

- **Bazel in the cloud.** Unit threads may only get test results from CI. A slow or flaky CI leg slows every unit.
- **FFI layout.** Phase 3 changes `SipiServerConfig`; a missed assertion compiles but corrupts config at runtime. Both sides' assertions are part of the deliverable.
- **Permanent shed.** `wedged_threads` never decreases, so after the cap is reached every engine-bound request is shed until restart. One uploaded file that hangs the decoder, requested `cap` times, takes decoding down until an operator restarts the process on the H5 alert. That is the trade against pinned cores.
- **Shed default.** A cap that is too low sheds healthy traffic after a few slow-but-finishing decodes. The default follows the documented restart point, and wedged counts only decodes that missed the deadline.
- **Shorter server deadline.** A legitimate full decode of a very large non-pyramidal master that took between 48 and 60 s now fails with the engine's timeout instead of a 408. The client sees an error in both cases; the new one frees the permit sooner.
- **More Full-lane memory held by wedged decodes.** A wedged decode keeps its Full-lane memory charge (`decode_guard.h:95-105`). After Phase 5 more restricted traffic runs in the Full lane, so this exposure grows; the Phase 4 cap bounds it.
- **Restricted admission.** Re-classification adds a permit release and acquire on restricted requests; the Full lane is smaller, so restricted traffic can now be shed earlier. That is the intended trade.
- **Metadata goldens.** Phases 6 to 8 can change approval goldens; re-approval needs a stated reason.

## Success Metrics

- Each in-scope Linear issue's "Done when" holds, or the plan states why an item is accepted (DEV-7599 item 5).
- CI is green on every layer of the stack.
- H6 shows no benchmark regression beyond the baseline CV.

## References

- `src/ffi/cpp/serve_image.cpp:669,696,736-755,1034-1038` (cache key and cache use)
- `src/server/rust/src/routes.rs:282-292,341-348,1073-1077,2238-2256,2290-2299,2428-2457` (CORS, admission, `json_response`)
- `src/server/rust/src/info.rs:309-316` (auth-service block)
- `src/ffi/cpp/decode_guard.h:39-52`, `src/ffi/cpp/engine_context.h:62`, `src/ffi/cpp/image_handle.cpp:237-253`, `src/ffi/cpp/sipi_ffi.cpp:175-207`
- `src/server/rust/src/config.rs:65-86` (request timeout)
- `src/format_handlers/cpp/SipiIOJpeg.cpp:681-695,991-1003` (XMP read), `src/metadata/cpp/exif.cpp:205-237`, `src/format_handlers/cpp/SipiIOTiff.cpp:62-91,2048-2050`
- `docs/learnings/testing/timeout-e2e-must-be-bounded.md`, `docs/learnings/architecture/ffi-seam-rust-mirrors-and-signature-drift.md`, `docs/learnings/debugging/parallel-e2e-shared-state-and-primary-evidence.md`
- `docs/src/operation/health-endpoint.md:49-56`, `docs/src/operation/admission-control.md:61`
