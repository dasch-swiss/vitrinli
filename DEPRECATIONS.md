# Deprecations

## Cache Configuration Keys (deprecated in v4.0.0, removal in next major)

| Old Key | New Key | Notes |
|---------|---------|-------|
| `cachedir` (Lua) / `--cachedir` (CLI) / `SIPI_CACHEDIR` (env) | `cache_dir` / `--cache-dir` / `SIPI_CACHE_DIR` | |
| `cachesize` (Lua) / `--cachesize` (CLI) / `SIPI_CACHESIZE` (env) | `cache_size` / `--cache-size` / `SIPI_CACHE_SIZE` | |
| `cache_hysteresis` (Lua) / `--cachehysteresis` (CLI) / `SIPI_CACHEHYSTERESIS` (env) | _(removed)_ | Replaced by built-in 80% low-water mark |

Old keys are accepted during the grace period with a deprecation warning. If both old and new keys are specified for the same setting, Vitrinli will refuse to start with an error.
