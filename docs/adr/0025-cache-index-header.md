---
status: accepted
---

> Later rename: the `FileCacheRecord::canonical` field and the `canonical_p` parameters are now named `cache_key` and `cache_key_p`, after the glossary term *Cache key*. This record keeps the names it was written with.

# Cache index gets a fixed header, a SHA-256 canonical key, and a size-aware freshness gate

The `.sipicache` index (`src/cache/cpp/SipiCache.{h,cpp}`) — the on-disk serialization of `SipiCache`'s in-memory `cachetable` — changes in three ways, landing together as one atomic format change because they all touch the same on-disk `FileCacheRecord` layout:

1. A fixed **header** (magic bytes + format version + `sizeof(FileCacheRecord)`) is prepended to the index file.
2. The in-memory map key and the on-disk `canonical` field switch from the raw canonical URL (truncated into a 256-byte `char[]`) to a **SHA-256 hex digest** of the canonical URL.
3. `FileCacheRecord` gains a `source_size` field, and `SipiCache::check()` gates a cache hit on the source file's size in addition to its mtime.

## Why now

Two independent findings converged on the same file:

- **S2-26**: `canonical[256]` truncates the raw canonical IIIF URL. A short, non-cryptographic, truncatable key is forgeable — an attacker who can influence which bytes land in the first 255 characters of two different canonical URLs can make them collide in the index, or hand-craft an index record for one canonical URL that resolves to another server's cache file. On load, the previous code trusted every field of every record verbatim: `fsize` (fed straight into eviction accounting — a forged inflated value thrashes eviction, a forged deflated value under-accounts and can fill the disk), `cachepath` (no rejection of a value containing `/`, so a forged record could point outside the cache directory), and no bound on unterminated `char[]` fields before constructing a `std::string` from them.
- **S2-25**: `check()` treats "source mtime <= recorded mtime" as sufficient freshness evidence. A service file replaced in place with a preserved or backdated mtime but a different byte length (e.g. a corrupted re-upload, or a deliberately backdated replacement) was previously served from a stale cache entry with no signal at all.

Because `source_size` is a new field on `FileCacheRecord` and the SHA-256 digest changes what is stored in `canonical`, both land in the same record-layout bump — a second migration one release later would just repeat the same mechanical work in `SipiCache.cpp`'s load/save code for no distinguishable operational value. The mtime-layout divergence between platforms (`SipiCache.h`: `struct timespec` on `HAVE_ST_ATIMESPEC` platforms, plain `time_t` otherwise) already changes `sizeof(FileCacheRecord)` across macOS and Linux builds; the new header's `record_size` field cross-checks against exactly this, so a record-layout mismatch (from *either* source) is a clean, structural, on-load rejection rather than latent byte-shifted corruption silently accepted as a stream of records.

## Header layout

```cpp
struct CacheIndexHeader
{
  char magic[8];          // "SIPICACH", not NUL-terminated
  std::uint32_t version;  // format version, bumped on any FileCacheRecord layout change
  std::uint32_t record_size; // sizeof(FileCacheRecord) at write time
};
```

Written once at the start of `.sipicache`, immediately followed by zero or more `FileCacheRecord`s.

**The version-mismatch-means-empty rule.** On load, the header is validated as a whole: present (file at least `sizeof(CacheIndexHeader)` bytes), magic matches, `version == kCacheIndexVersion`, and `record_size == sizeof(FileCacheRecord)` for the currently-running binary. A missing header, a magic mismatch, a version bump, or a `record_size` mismatch (including the pre-existing cross-platform `timespec`/`time_t` divergence, and now this ADR's own `source_size` addition) is treated identically: **start empty and log once**, then run the same crash-recovery path that already exists for "no index file at all" (clear the cache directory of orphan files, remove the corrupted/incompatible index). This is a deliberate choice not to add a format-version migration reader: the cache is a disposable regenerate-on-miss structure, not a Service File — there is no preservation obligation to keep old cache entries readable across a binary upgrade, and a "start empty" fallback is strictly safer than a partial-field migration that could silently misinterpret bytes into different fields.

After the header, the remaining byte length must be an exact multiple of `sizeof(FileCacheRecord)`; a non-multiple remainder is corruption and takes the same "start empty" path.

## SHA-256 canonical key

`cachetable`'s key — both as the `std::unordered_map<std::string, CacheRecord>` key in memory and as the persisted `canonical` field on disk — is now `SHA256(canonical_url)` rendered as a 64-character lowercase hex string (256 bits, well past the 128-bit floor a forgery-resistant key needs). `check()`, `add()`, and `remove()` all hash their incoming canonical URL once before touching the map; the on-disk `canonical` field is written and read as this digest directly, with no re-hashing on load (a loaded record's `canonical` field already *is* the digest, by construction of everything that ever wrote it). Two distinct canonical URLs cannot collide short of a SHA-256 collision, and no truncation boundary exists to engineer one.

## Per-record hardening on load

A record loaded from disk is no longer trusted verbatim:

- Every `char[]` field (`canonical`, `origpath`, `cachepath`) is bounded with `strnlen` before a `std::string` is constructed from it; a field with no NUL within its array is corrupt and the record is skipped.
- A record with an empty `canonical` or a `cachepath` containing `/` is rejected outright — a legitimate `cachepath` is always a `getNewCacheFileName()` basename with the directory component stripped.
- `fsize` is **recomputed** from an `lstat()` of the actual cache file at load time, not trusted from the index; a symlink at that path is rejected (`S_ISREG` check), and a missing cache file drops the record. Trusting a forged `fsize` would otherwise let a crafted index thrash LRU eviction or misreport the cache's disk footprint.
- An unreadable, too-short, or otherwise invalid index (per the header rule above) falls back to an empty cache rather than attempting to parse anything.

## Symlink-safe cache reads

`serve_image.cpp`'s `full_file_body()` — the body constructor shared by the direct-passthrough and cache-hit response paths — now `open()`s the target with `O_NOFOLLOW` and verifies `S_ISREG` via `fstat()` before returning a `FileBody`, instead of merely `stat()`ing the path. A planted symlink at a cache path combined with a forged or stale index record would otherwise let the server read and serve any file the process has permission to read (e.g. a config file holding a JWT secret) under an unrelated IIIF URL.

## S2-25: source-size freshness gate

`FileCacheRecord`/`CacheRecord` gain `source_size` (the SOURCE file's size at cache-add time), kept distinct from the pre-existing `fsize` (the CACHE file's size, used for eviction accounting). `SipiCache::add()` records the source's `st_size` alongside its `st_mtime`-derived freshness baseline; `SipiCache::check()` — which already `stat()`s the source file for its mtime — reads `st_size` from the same `stat()` call and treats the entry as a miss if either the source is newer than the cache's recorded mtime **or** the source's current size differs from the recorded `source_size`.

**Accepted residual risk.** A replacement that preserves both the source file's mtime *and* its exact byte length still produces a cache hit against stale content. SIPI deliberately does not hash the full source file body on every `check()` call — that would turn every cache lookup into a full read of the (potentially large) service file, defeating the purpose of the cache. Operators who replace a Service File in place must `touch(1)` it (to change mtime) or explicitly purge the affected cache entry; this is a documented operational rule, not a code-enforced invariant.

## Considered options

- **Keep the raw canonical URL as the key, just widen the buffer** — rejected. A wider fixed buffer still truncates at some length and does nothing to prevent an attacker-chosen prefix collision; it only raises the bar, it doesn't close the primitive.
- **Full source-file content digest instead of the size+mtime heuristic** — rejected for `check()`'s hot path (every cache lookup would require reading the entire source file); the size+mtime combination catches the realistic "replaced with different bytes" case at O(1) stat cost, and the residual (same-size, same-mtime replacement) is accepted and documented rather than engineered around.
- **Migrate old-format indexes field-by-field on a version bump** — rejected; see the version-mismatch-means-empty rationale above. The cache has no preservation obligation.

## Consequences

- Every SIPI upgrade that changes `FileCacheRecord`'s layout (including this one) discards the existing `.sipicache` index and the cache files it referenced on first startup after the upgrade — a one-time, self-healing cold-cache period, not a correctness issue.
- `SipiCache` gains an `@openssl//:crypto` build dependency (already present transitively elsewhere in the tree via `//src/util`) for `EVP_Digest`/`EVP_sha256()`.
- `full_file_body()` in `serve_image.cpp` now performs an `open()` + `fstat()` + `close()` on every direct-passthrough and cache-hit response instead of a bare `stat()`; negligible cost relative to the file I/O that follows.
