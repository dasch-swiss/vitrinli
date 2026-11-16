/*
 * Copyright © 2016 - 2026 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*!
 * `sipi_init` — the production server-mode engine install behind the FFI seam.
 *
 * The Rust shell calls this once at startup before serving: it takes every
 * configured value from `overrides` (the shell parses both config flavors —
 * TOML and Lua — Rust-side), builds the cache / decode-memory-budget services,
 * and points `engine_context()` at them. It is a production entry point, so it
 * lives in the seam package
 * (`src/ffi`), not in the CLI package (`src/cli`).
 */

#include <climits>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "util/Error.h"// shttps::Error

#include "cache/SipiCache.h"
#include "ffi/SipiConf.h"// Sipi::SipiConf, Sipi::parseSizeString
#include "image/SipiIO.h"// Sipi::ScalingMethod, Sipi::ScalingQuality
#include "throttling/SipiMemoryBudget.h"// Sipi::SipiMemoryBudget, AdmissionMode, parse_admission_mode
#include "logging/logger.h"// log_warn / log_err / log_info
#include "observability/metrics.h"// Sipi::observability::Metrics

#include "ffi/engine_context.h"// Sipi::ffi::set_engine_context, EngineContext
#include "ffi/sipi_ffi.h"// the extern "C" sipi_init contract + SipiServerConfig
#include "ffi/startup.h"// Sipi::ffi::LibraryInitialiser, detect_available_memory

namespace {

/*! Map a config scaling-quality string to a ScalingMethod; unknown/missing → HIGH. */
Sipi::ScalingMethod parse_scaling_method(const std::string &v)
{
  if (v == "medium") { return Sipi::ScalingMethod::MEDIUM; }
  if (v == "low") { return Sipi::ScalingMethod::LOW; }
  return Sipi::ScalingMethod::HIGH;
}

/*! Convert SipiConf's `map<string,string>` scaling-quality table into the
 *  `ScalingQuality` struct EngineContext holds (jk2 ← the "jpk" key). */
Sipi::ScalingQuality to_scaling_quality(const std::map<std::string, std::string> &m)
{
  const auto get = [&](const char *k) -> std::string {
    const auto it = m.find(k);
    return it != m.end() ? it->second : std::string();
  };
  Sipi::ScalingQuality q;
  q.jk2 = parse_scaling_method(get("jpk"));
  q.jpeg = parse_scaling_method(get("jpeg"));
  q.tiff = parse_scaling_method(get("tiff"));
  q.png = parse_scaling_method(get("png"));
  return q;
}

/*!
 * Process-wide server runtime installed by `sipi_init`.
 *
 * The Rust shell has no server object to own the engine services, so `sipi_init`
 * parks them here for the process lifetime.
 * `engine_context()` stores non-owning pointers into this holder, so it must
 * outlive every serve call — hence file-static.
 */
struct ServerRuntime
{
  Sipi::SipiConf conf;
  std::unique_ptr<Sipi::SipiCache> cache;
  std::unique_ptr<Sipi::SipiMemoryBudget> memory_budget;
};
std::unique_ptr<ServerRuntime> g_server_runtime;

}// namespace

/*!
 * Install the engine from scratch. The Rust shell calls it once at startup
 * before serving. Builds the cache / memory budget into `g_server_runtime` and
 * points `engine_context()` at them. Returns 0 on success or `EXIT_FAILURE`;
 * never lets a C++ exception cross the boundary.
 *
 * `overrides` carries the resolved config the Rust shell assembled (config file
 * base + CLI/env layered on top; null = none). Only engine-behaviour values are
 * forwarded; transport knobs the Rust shell owns (TLS, keep-alive, concurrency)
 * are not in the struct.
 */
extern "C" int sipi_init(const SipiServerConfig *overrides)
{
  try {
    // Initialise the codec libraries (curl / Exiv2 / TIFF) the decode pipeline
    // needs. The Rust server path does not go through sipi_cli_main's
    // LibraryInitialiser, so sipi_init owns it here; the singleton is idempotent.
    Sipi::ffi::LibraryInitialiser::instance();

    auto runtime = std::make_unique<ServerRuntime>();

    // `runtime->conf` stays default-constructed (an all-defaults config via
    // SipiConf's in-class initializers); the overrides below supply every
    // configured value — the shell parses both config flavors (TOML and Lua)
    // and sends the result over this one channel.
    Sipi::SipiConf &conf = runtime->conf;

    // Apply the present values onto the default-constructed SipiConf BEFORE
    // the cache / memory-budget services below are built from `conf`.
    // Setter names are SipiConf's verbatim. Sized
    // strings (cache_size/maxpost/memory_limit) carry the raw "300M" text;
    // parseSizeString expands the suffix engine-side. A negative maxpost /
    // memory-limit clamps to 0 (matching the SipiConf ctor) so
    // it cannot become SIZE_MAX and skip the envelope auto-detect; cache_size keeps
    // -1 as its valid "unlimited" sentinel. cache_nfiles is `unsigned` (0 =
    // unlimited; a negative is rejected by CLI11 on both binaries / by clap u32),
    // so it forwards straight to setCacheNFiles(size_t) with no signed wrap.
    // The large-decode classifier threshold: shell-sourced, read from the seam
    // override, installed into the EngineContext below. 0 (shell did not set it)
    // conservatively classifies every decode as full-lane.
    std::size_t large_decode_threshold_bytes = 0;
    // The decode deadline is shell-resolved (shorter than the request timeout)
    // and, like the threshold above, lands only in the EngineContext.
    std::size_t decode_timeout_ms = Sipi::ffi::kDefaultDecodeTimeoutMs;
    if (overrides != nullptr) {
      const SipiServerConfig &o = *overrides;
      // Strings (null = absent).
      if (o.imgroot != nullptr) conf.setImgRoot(o.imgroot);
      if (o.scriptdir != nullptr) conf.setScriptDir(o.scriptdir);
      if (o.initscript != nullptr) conf.setInitScript(o.initscript);
      if (o.tmpdir != nullptr) conf.setTmpDir(o.tmpdir);
      if (o.jwtkey != nullptr) conf.setJwtSecret(o.jwtkey);
      if (o.cache_dir != nullptr) conf.setCacheDir(o.cache_dir);
      if (o.cache_size != nullptr) conf.setCacheSize(Sipi::parseSizeString(o.cache_size));
      if (o.maxpost != nullptr) {
        const long long v = Sipi::parseSizeString(o.maxpost);
        conf.setMaxPostSize(v > 0 ? static_cast<size_t>(v) : 0);// <=0 -> 0 (unlimited)
      }
      if (o.memory_limit != nullptr) {
        const long long v = Sipi::parseSizeString(o.memory_limit);
        conf.setMemoryLimit(v > 0 ? static_cast<size_t>(v) : 0);// <=0 -> 0 (auto-detect available RAM)
      }
      if (o.admission_mode != nullptr) conf.setAdmissionMode(o.admission_mode);
      if (o.has_tiles_memory_ratio) conf.setTilesMemoryRatio(o.tiles_memory_ratio);
      // The large-decode threshold is shell-sourced and engine-read-from-seam
      // (DUNE-003): it never lands in SipiConf, only in the EngineContext below.
      if (o.has_large_decode_threshold_bytes) {
        large_decode_threshold_bytes = o.large_decode_threshold_bytes;
      }
      if (o.has_decode_timeout_ms) decode_timeout_ms = o.decode_timeout_ms;
      if (o.thumbsize != nullptr) conf.setThumbSize(o.thumbsize);
      if (o.knorapath != nullptr) conf.setKnoraPath(o.knorapath);
      if (o.knoraport != nullptr) conf.setKnoraPort(o.knoraport);
      if (o.docroot != nullptr) conf.setDocRoot(o.docroot);
      if (o.wwwroute != nullptr) conf.setWWWRoute(o.wwwroute);
      if (o.loglevel != nullptr) conf.setLogLevel(o.loglevel);
      // Scaling-quality per codec (TOML-config-only — no CLI flag). Merge the
      // present codecs onto the base map so a partial override keeps the others.
      // The "j2k" key is stored as the config writes it; to_scaling_quality reads
      // it under "jpk" (a legacy engine quirk), so j2k scaling falls to the
      // default on both the Lua and TOML paths alike — parity, not a new bug.
      if (o.scaling_quality_jpeg != nullptr || o.scaling_quality_tiff != nullptr
          || o.scaling_quality_png != nullptr || o.scaling_quality_j2k != nullptr) {
        std::map<std::string, std::string> sq = conf.getScalingQuality();
        if (o.scaling_quality_jpeg != nullptr) sq["jpeg"] = o.scaling_quality_jpeg;
        if (o.scaling_quality_tiff != nullptr) sq["tiff"] = o.scaling_quality_tiff;
        if (o.scaling_quality_png != nullptr) sq["png"] = o.scaling_quality_png;
        if (o.scaling_quality_j2k != nullptr) sq["j2k"] = o.scaling_quality_j2k;
        conf.setScalingQuality(sq);
      }
      // Scalars (presence flag — 0 is a valid value, so gate on has_).
      if (o.has_serverport) conf.setPort(o.serverport);
      if (o.has_maxtmpage) conf.setMaxTempFileAge(o.maxtmpage);
      if (o.has_cache_nfiles) conf.setCacheNFiles(o.cache_nfiles);
      if (o.has_pathprefix) conf.setPrefixAsPath(o.pathprefix != 0);
      if (o.has_jpeg_quality) conf.setJpegQuality(o.jpeg_quality);
    }

    // Apply the resolved engine log level to the C++ logger gate (CLI/env/TOML;
    // LL_INFO when unset). Without this the configured level is silently ignored.
    set_log_level(parse_log_level(conf.getLoglevel(), LL_INFO));

    // Engine services built from the config values (with the CLI/env overrides
    // above already applied). A null service means the corresponding feature is
    // disabled.
    {
      const std::string cachedir = conf.getCacheDir();
      const long long cache_size = conf.getCacheSize();
      if (cache_size != 0 && !cachedir.empty()) {
        // Degrade to no-cache on a bad/unwritable cache dir rather than aborting
        // startup — cache init failure is non-fatal, so log the error and continue.
        try {
          runtime->cache = std::make_unique<Sipi::SipiCache>(cachedir, cache_size, conf.getCacheNFiles());
        } catch (const shttps::Error &e) {
          log_warn("sipi_init: caching disabled — %s", e.what());
          runtime->cache = nullptr;
        }
      }
    }
    // Admission config, resolved once here (the single authority): the shell
    // reads these back over the seam so its two-lane thread pool matches the
    // engine's memory budget. Declared at function scope for the EngineContext
    // install below.
    std::string admission_mode_resolved;
    double tiles_memory_ratio_resolved = 0.0;
    std::size_t memory_limit_resolved = 0;
    {
      // Admission mode: an unrecognized/legacy value (e.g. a stale "monitor"
      // from an old template) falls back to the basic default rather than
      // failing startup — same as the rest of config, "if it's wrong, too bad".
      const Sipi::AdmissionMode mode =
        Sipi::parse_admission_mode(conf.getAdmissionMode()).value_or(Sipi::AdmissionMode::BASIC);
      admission_mode_resolved = (mode == Sipi::AdmissionMode::ADVANCED) ? "advanced" : "basic";
      // The tile reserve fraction must leave a positive full-lane budget.
      const double ratio = conf.getTilesMemoryRatio();
      if (!(ratio > 0.0 && ratio < 1.0)) {
        log_err("sipi_init: tiles_memory_ratio %.4f out of range (0, 1)", ratio);
        return EXIT_FAILURE;
      }
      tiles_memory_ratio_resolved = ratio;
      // The RAM envelope: an explicit memory_limit, or the detected available RAM
      // (0 = auto). The full lane gets envelope × (1 − tiles_memory_ratio); the
      // reserve (envelope × tiles_memory_ratio) houses tile usage + the
      // non-decode floor and is never charged to the full lane.
      std::size_t envelope = conf.getMemoryLimit();
      if (envelope == 0) {
        const std::size_t detected = Sipi::ffi::detect_available_memory();
        envelope = (detected > 0) ? detected : (1ULL * 1024 * 1024 * 1024);
      }
      memory_limit_resolved = envelope;
      const auto full_mem = static_cast<std::size_t>(static_cast<double>(envelope) * (1.0 - ratio));
      runtime->memory_budget = std::make_unique<Sipi::SipiMemoryBudget>(full_mem, mode);
      Sipi::observability::Metrics::instance().decode_memory_budget_bytes.Set(static_cast<double>(full_mem));
    }

    // Resolve the image root (realpath) for path-traversal containment (R2).
    const std::string imgroot = conf.getImgRoot();
    char resolved[PATH_MAX];
    if (realpath(imgroot.c_str(), resolved) == nullptr) {
      log_err("sipi_init: image root '%s' does not resolve", imgroot.c_str());
      return EXIT_FAILURE;
    }
    const std::string resolved_imgroot(resolved);

    // Install the engine context — non-owning pointers into g_server_runtime.
    Sipi::ffi::set_engine_context(Sipi::ffi::EngineContext{
      .cache = runtime->cache.get(),
      .memory_budget = runtime->memory_budget.get(),
      .large_decode_threshold_bytes = large_decode_threshold_bytes,
      .admission_mode = admission_mode_resolved,
      .tiles_memory_ratio = tiles_memory_ratio_resolved,
      .memory_limit_bytes = memory_limit_resolved,
      .imgroot = imgroot,
      .resolved_imgroot = resolved_imgroot,
      .docroot = conf.getDocRoot(),
      .wwwroute = conf.getWWWRoute(),
      .prefix_as_path = conf.getPrefixAsPath(),
      .jpeg_quality = conf.getJpegQuality(),
      .scaling_quality = to_scaling_quality(conf.getScalingQuality()),
      .port = conf.getPort(),
      .max_post_size = conf.getMaxPostSize(),
      .decode_timeout_ms = decode_timeout_ms,
    });

    g_server_runtime = std::move(runtime);
    log_info("sipi_init: engine installed (imgroot resolved: %s)", resolved_imgroot.c_str());
    return EXIT_SUCCESS;
  } catch (const shttps::Error &e) {
    log_err("sipi_init failed: %s", e.what());
    return EXIT_FAILURE;
  } catch (const std::exception &e) {
    log_err("sipi_init failed: %s", e.what());
    return EXIT_FAILURE;
  } catch (...) {
    log_err("sipi_init failed: unknown error");
    return EXIT_FAILURE;
  }
}
