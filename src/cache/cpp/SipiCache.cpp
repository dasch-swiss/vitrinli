/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


#ifdef HAVE_MALLOC_H
#include <malloc.h>
#else


#endif

#include <cassert>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/evp.h>

#include "SipiCache.h"
#include "error/SipiError.h"
#include "observability/metrics.h"
#include "observability/profiling.h"
#include "util/Global.h"
#include "logging/logger.h"

namespace Sipi {

using observability::Metrics;

namespace {

  // Fixed on-disk index header (ADR-0025): magic + format version +
  // `sizeof(FileCacheRecord)` at write time. A missing/mismatching header —
  // including a `sizeof(FileCacheRecord)` mismatch, which the mtime layout
  // difference between platforms and any future field addition both trigger —
  // means "start empty and log once", never "parse as a stream of records".
  struct CacheIndexHeader
  {
    char magic[8];
    std::uint32_t version;
    std::uint32_t record_size;
  };

  constexpr char kCacheIndexMagic[8] = { 'S', 'I', 'P', 'I', 'C', 'A', 'C', 'H' };
  constexpr std::uint32_t kCacheIndexVersion = 1;

  // SHA-256 hex digest of `s` (64 hex chars = 256 bits, well past the >=128-bit
  // floor): the persisted and in-memory index key derived from an entry's Cache key
  // (S2-26). A short/truncated key is forgeable and becomes a
  // content-substitution primitive on a restricted image.
  std::string sha256_hex(const std::string &s)
  {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(s.data(), s.size(), digest, &len, EVP_sha256(), nullptr) != 1) {
      throw SipiError("SHA-256 digest of cache key failed");
    }
    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i) {
      oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }
    return oss.str();
  }

  // Subtracts `amount` from `total` without letting the unsigned counter wrap
  // (S2-45): a forged/oversized fsize, a double-removal, or accounting drift
  // must clamp to 0 rather than underflow into a huge value that breaks
  // eviction.
  void subtractClamped(std::atomic<unsigned long long> &total, unsigned long long amount)
  {
    unsigned long long current = total.load();
    if (amount > current) {
      log_warn("Cache accounting underflow: subtracting %llu from %llu bytes used; clamping to 0", amount, current);
      total.store(0);
    } else {
      total.fetch_sub(amount);
    }
  }

}// namespace

// Owns a scandir() result: frees every entry and the array itself.
struct ScandirList
{
  struct dirent **namelist{ nullptr };
  int n{ -1 };

  explicit ScandirList(const std::string &dir) { n = scandir(dir.c_str(), &namelist, nullptr, alphasort); }

  ~ScandirList()
  {
    for (int i = 0; i < n; i++) { free(namelist[i]); }
    free(namelist);
  }

  ScandirList(const ScandirList &) = delete;
  ScandirList &operator=(const ScandirList &) = delete;
};

// Helper: remove all non-dotfiles from a directory, returns count removed
static int clearCacheDir(const std::string &dir)
{
  int removed = 0;
  ScandirList files(dir);
  for (int i = 0; i < files.n; i++) {
    if (files.namelist[i]->d_name[0] != '.') {
      std::string ff = dir + "/" + files.namelist[i]->d_name;
      ::remove(ff.c_str());
      removed++;
    }
  }
  return removed;
}

typedef struct _AListEle
{
  std::string cache_key;
  time_t access_time;
  off_t fsize;

  bool operator<(const _AListEle &str) const { return (difftime(access_time, str.access_time) < 0.); }

  bool operator>(const _AListEle &str) const { return (difftime(access_time, str.access_time) > 0.); }

  bool operator==(const _AListEle &str) const { return (difftime(access_time, str.access_time) == 0.); }
} AListEle;


SipiCache::SipiCache(const std::string &cachedir_p,
  long long max_cache_size_p,
  unsigned max_nfiles_p)
  : _cachedir(cachedir_p), cache_used_bytes(0), max_cache_size(max_cache_size_p), nfiles(0), max_nfiles(max_nfiles_p)
{

  if (access(_cachedir.c_str(), R_OK | W_OK | X_OK) != 0) {
    if (mkdir(_cachedir.c_str(), 0755) != 0) {
      throw SipiError("Cannot create cache directory: " + _cachedir, errno);
    }
    log_info("Created cache directory: %s", _cachedir.c_str());
  }

  std::string cachefilename = _cachedir + "/.sipicache";

  std::ifstream cachefile(cachefilename, std::ios::in | std::ios::binary);

  int skipped = 0;
  int orphans_removed = 0;

  if (cachefile.fail()) {
    //
    // No .sipicache file — crash recovery: clear all files in cache dir
    //
    orphans_removed = clearCacheDir(_cachedir);

    if (orphans_removed > 0) {
      log_warn("Cache index missing — cleared %d orphan files (crash recovery)", orphans_removed);
    }
  } else {
    //
    // .sipicache exists — validate the fixed header, then load
    //
    cachefile.seekg(0, cachefile.end);
    std::streampos length = cachefile.tellg();
    cachefile.seekg(0, cachefile.beg);

    bool header_valid = length >= 0 && static_cast<size_t>(length) >= sizeof(CacheIndexHeader);
    CacheIndexHeader header{};

    if (header_valid) {
      cachefile.read(reinterpret_cast<char *>(&header), sizeof(header));
      header_valid = std::memcmp(header.magic, kCacheIndexMagic, sizeof(kCacheIndexMagic)) == 0
                      && header.version == kCacheIndexVersion
                      && header.record_size == sizeof(SipiCache::FileCacheRecord);
    }

    size_t body_length = header_valid ? static_cast<size_t>(length) - sizeof(CacheIndexHeader) : 0;
    if (header_valid && (body_length % sizeof(SipiCache::FileCacheRecord) != 0)) { header_valid = false; }

    // Missing header, mismatched magic/version/record-size, or a body length
    // that isn't a whole number of records: never parse as a stream of
    // records — start empty and log once (ADR-0025).
    if (!header_valid) {
      log_warn("Cache index missing/invalid header or corrupted (size %lld) — clearing cache",
        static_cast<long long>(length));
      cachefile.close();

      // Clear all files in cache dir
      orphans_removed = clearCacheDir(_cachedir);
      // Remove the corrupted index file itself
      ::remove(cachefilename.c_str());
    } else {
      int nrecords = static_cast<int>(body_length / sizeof(SipiCache::FileCacheRecord));

      for (int i = 0; i < nrecords; i++) {
        SipiCache::FileCacheRecord fr;
        cachefile.read((char *)&fr, sizeof(SipiCache::FileCacheRecord));

        // Bound every char[] field before constructing a std::string — a
        // record without a NUL within its field is corrupt, not "long".
        size_t cache_key_len = strnlen(fr.cache_key, sizeof(fr.cache_key));
        size_t origpath_len = strnlen(fr.origpath, sizeof(fr.origpath));
        size_t cachepath_len = strnlen(fr.cachepath, sizeof(fr.cachepath));
        if (cache_key_len == sizeof(fr.cache_key) || origpath_len == sizeof(fr.origpath)
            || cachepath_len == sizeof(fr.cachepath)) {
          log_debug("Cache index record %d has an unterminated field, skipping", i);
          skipped++;
          continue;
        }

        std::string cache_key(fr.cache_key, cache_key_len);
        std::string origpath(fr.origpath, origpath_len);
        std::string cachepath(fr.cachepath, cachepath_len);

        // Reject forged records: an empty key, or a cachepath escaping the
        // cache directory (a path separator has no business in a basename
        // produced by getNewCacheFileName()).
        if (cache_key.empty() || cachepath.find('/') != std::string::npos) {
          log_debug("Cache index record %d has an invalid cache key/cachepath, skipping", i);
          skipped++;
          continue;
        }

        std::string accesspath = _cachedir + "/" + cachepath;
        struct stat cache_stat{};
        // lstat (not stat): a symlinked "cache file" is rejected outright,
        // not followed.
        if (lstat(accesspath.c_str(), &cache_stat) != 0 || !S_ISREG(cache_stat.st_mode)) {
          log_debug("Cache file \"%s\" not on disk, skipping", cachepath.c_str());
          skipped++;
          continue;
        }

        CacheRecord cr;
        cr.img_w = fr.img_w;
        cr.img_h = fr.img_h;
        cr.tile_w = fr.tile_w;
        cr.tile_h = fr.tile_h;
        cr.clevels = fr.clevels;
        cr.numpages = fr.numpages;
        cr.origpath = origpath;
        cr.cachepath = cachepath;
        cr.mtime = fr.mtime;
        cr.access_time = fr.access_time;
        // Recomputed from the actual cache file, never trusted from the
        // index: a forged fsize would otherwise thrash eviction / fill disk.
        cr.fsize = cache_stat.st_size;
        cr.source_size = fr.source_size;
        cache_used_bytes += cr.fsize;
        nfiles++;
        cachetable[cache_key] = cr;
        log_debug("Cache loaded file \"%s\"", cr.cachepath.c_str());
      }

      cachefile.close();

      //
      // Scan for orphan files not in the loaded index and delete them
      // Build a set of known cache filenames for O(1) lookup
      //
      std::unordered_set<std::string> known_cache_files;
      known_cache_files.reserve(cachetable.size());
      for (const auto &ele : cachetable) {
        known_cache_files.insert(ele.second.cachepath);
      }

      ScandirList files(_cachedir);
      for (int i = 0; i < files.n; i++) {
        if (files.namelist[i]->d_name[0] == '.') { continue; }
        std::string file_on_disk = files.namelist[i]->d_name;

        if (known_cache_files.find(file_on_disk) == known_cache_files.end()) {
          std::string ff = _cachedir + "/" + file_on_disk;
          log_debug("Orphan file \"%s\" not in cache index, removing", file_on_disk.c_str());
          ::remove(ff.c_str());
          orphans_removed++;
        }
      }
    }
  }

  // If over limits on startup, evict down to 80%
  int evicted = purge(false);

  // Single summary line at INFO level
  log_info("Cache loaded: %u files (%.1f MB), %d skipped, %d orphans removed, %d evicted",
    nfiles.load(), static_cast<double>(cache_used_bytes.load()) / (1024.0 * 1024.0), skipped, orphans_removed, evicted > 0 ? evicted : 0);

  // Update metrics gauges
  auto &metrics = Metrics::instance();
  metrics.cache_size_bytes.Set(static_cast<double>(cache_used_bytes));
  metrics.cache_files.Set(static_cast<double>(nfiles));
  metrics.cache_size_limit_bytes.Set(static_cast<double>(max_cache_size));
  metrics.cache_files_limit.Set(static_cast<double>(max_nfiles));
  if (evicted > 0) {
    metrics.cache_evictions_total.Increment(static_cast<double>(evicted));
  }
}

//============================================================================

SipiCache::~SipiCache()
{
  log_debug("Closing cache...");
  std::string cachefilename = _cachedir + "/.sipicache";
  std::ofstream cachefile(cachefilename, std::ofstream::out | std::ofstream::binary | std::ofstream::trunc);

  if (!cachefile.fail()) {
    CacheIndexHeader header{};
    std::memcpy(header.magic, kCacheIndexMagic, sizeof(kCacheIndexMagic));
    header.version = kCacheIndexVersion;
    header.record_size = static_cast<std::uint32_t>(sizeof(SipiCache::FileCacheRecord));
    cachefile.write(reinterpret_cast<const char *>(&header), sizeof(header));

    for (const auto &ele : cachetable) {
      SipiCache::FileCacheRecord fr;
      fr.img_w = ele.second.img_w;
      fr.img_h = ele.second.img_h;
      fr.tile_w = ele.second.tile_w;
      fr.tile_h = ele.second.tile_h;
      fr.clevels = ele.second.clevels;
      fr.numpages = ele.second.numpages;
      // ele.first is already the SHA-256 digest (S2-26): computed once by
      // check()/add() on the raw Cache key, never re-hashed here.
      (void)snprintf(fr.cache_key, 256, "%s", ele.first.c_str());
      (void)snprintf(fr.origpath, 256, "%s", ele.second.origpath.c_str());
      (void)snprintf(fr.cachepath, 256, "%s", ele.second.cachepath.c_str());
      fr.mtime = ele.second.mtime;
      fr.fsize = ele.second.fsize;
      fr.source_size = ele.second.source_size;
      fr.access_time = ele.second.access_time;
      cachefile.write((char *)&fr, sizeof(SipiCache::FileCacheRecord));
      log_debug("Writing \"%s\" to cache file...", ele.second.cachepath.c_str());
    }
  }

  cachefile.close();
}

//============================================================================

#if defined(HAVE_ST_ATIMESPEC)

int SipiCache::tcompare(struct timespec &t1, struct timespec &t2)
#else
int SipiCache::tcompare(time_t &t1, time_t &t2)
#endif
{
#if defined(HAVE_ST_ATIMESPEC)
  if (t1.tv_sec > t2.tv_sec) {
    return 1;
  } else if (t1.tv_sec < t2.tv_sec) {
    return -1;
  } else {
    if (t1.tv_sec == t2.tv_sec) {
      if (t1.tv_nsec > t2.tv_nsec) {
        return 1;
      } else if (t1.tv_nsec < t2.tv_nsec) {
        return -1;
      } else {
        return 0;
      }
    }
  }
#else
  if (t1 > t2) {
    return 1;
  } else if (t1 < t2) {
    return -1;
  } else {
    return 0;
  }

#endif
  return 0;// should never reached – just for the compiler to suppress a warning...
}

//============================================================================

static bool _compare_access_time_asc(const AListEle &e1, const AListEle &e2)
{
  double d = difftime(e1.access_time, e2.access_time);
  return (d < 0.0);
}

//============================================================================

static bool _compare_access_time_desc(const AListEle &e1, const AListEle &e2)
{
  double d = difftime(e1.access_time, e2.access_time);
  return (d > 0.0);
}

//============================================================================

static bool _compare_fsize_asc(const AListEle &e1, const AListEle &e2) { return (e1.fsize < e2.fsize); }
//============================================================================

static bool _compare_fsize_desc(const AListEle &e1, const AListEle &e2) { return (e1.fsize > e2.fsize); }
//============================================================================

int SipiCache::purge(bool use_lock)
{
  // Acquire lock first so threshold checks are consistent
  std::unique_lock<std::mutex> locking_mutex_guard(locking, std::defer_lock);
  if (use_lock) locking_mutex_guard.lock();

  if ((max_cache_size < 0) && (max_nfiles == 0)) return 0;// unlimited cache, no file limit

  bool size_over = (max_cache_size > 0)
    && (cache_used_bytes >= static_cast<unsigned long long>(max_cache_size));
  bool nfiles_over = (max_nfiles > 0) && (nfiles >= max_nfiles);

  if (!size_over && !nfiles_over) return 0;

  int n = 0;

  // Low-water marks: 80% of configured limits
  unsigned long long size_low = (max_cache_size > 0)
    ? static_cast<unsigned long long>(max_cache_size * 0.8) : 0;
  unsigned nfiles_low = (max_nfiles > 0)
    ? static_cast<unsigned>(max_nfiles * 0.8) : 0;

  // Build sorted list — use partial_sort to only sort the portion we need to evict
  std::vector<AListEle> alist;
  alist.reserve(cachetable.size());
  for (const auto &ele : cachetable) {
    alist.push_back({ ele.first, ele.second.access_time, ele.second.fsize });
  }

  std::sort(alist.begin(), alist.end(), _compare_access_time_asc);

  for (const auto &ele : alist) {
    // Check if BOTH limits are at or below low-water
    bool size_ok = (max_cache_size <= 0) || (cache_used_bytes <= size_low);
    bool nfiles_ok = (max_nfiles == 0) || (nfiles <= nfiles_low);
    if (size_ok && nfiles_ok) break;

    auto ct_it = cachetable.find(ele.cache_key);
    if (ct_it == cachetable.end()) continue;// already erased

    std::string delpath = _cachedir + "/" + ct_it->second.cachepath;

    auto blocked_it = blocked_files.find(delpath);
    if (blocked_it != blocked_files.end() && blocked_it->second > 0) {
      log_debug("Skipping blocked cache file for %s", ele.cache_key.c_str());
      continue;
    }

    log_debug("Purging from cache \"%s\"...", ct_it->second.cachepath.c_str());
    ::unlink(delpath.c_str());
    subtractClamped(cache_used_bytes, ct_it->second.fsize);
    --nfiles;
    ++n;
    cachetable.erase(ct_it);
  }

  // Update metrics after eviction
  if (n > 0) {
    auto &metrics = Metrics::instance();
    metrics.cache_evictions_total.Increment(static_cast<double>(n));
    metrics.cache_size_bytes.Set(static_cast<double>(cache_used_bytes));
    metrics.cache_files.Set(static_cast<double>(nfiles));
  }

  // Check if we couldn't free enough space (all remaining files blocked)
  bool size_ok = (max_cache_size <= 0) || (cache_used_bytes <= size_low);
  bool nfiles_ok = (max_nfiles == 0) || (nfiles <= nfiles_low);
  if (!size_ok || !nfiles_ok) {
    log_warn("Cache full and all remaining files are blocked. New file will not be cached.");
    return -1;
  }

  return n;
}

//============================================================================

std::string SipiCache::check(const std::string &origpath_p, const std::string &cache_key_p, bool block_file)
{
  SIPI_ZONE_N("SipiCache::check");
  struct stat fileinfo;
  SipiCache::CacheRecord fr;

  if (stat(origpath_p.c_str(), &fileinfo) != 0) {
    throw SipiError("Couldn't stat file \"" + origpath_p + "\"!", errno);
  }
#if defined(HAVE_ST_ATIMESPEC)
  struct timespec mtime = fileinfo.st_mtimespec;
#else
  time_t mtime = fileinfo.st_mtime;
#endif
  const off_t source_size = fileinfo.st_size;

  std::string res;
  const std::string digest = sha256_hex(cache_key_p);

  std::lock_guard<std::mutex> locking_mutex_guard(locking);
  auto it = cachetable.find(digest);
  if (it == cachetable.end()) {
    Metrics::instance().cache_misses_total.Increment();
    return res;// return empty string, because we didn't find the file in cache
  }
  fr = it->second;

  //
  // get the current time (seconds since Epoch)
  //
  time_t at;
  time(&at);
  it->second.access_time = at;// update the access time!

  // Freshness gate (S2-25): mtime alone is not sufficient replacement
  // evidence — a same-mtime replacement with a DIFFERENT size is still a
  // miss. Residual (ADR-0025): a same-size replacement that also preserves
  // mtime still hits; SIPI does not hash source file bodies on every check().
  if (tcompare(mtime, fr.mtime) > 0 || source_size != fr.source_size) {
    // original file is newer than cache, or its size changed — replace it...
    Metrics::instance().cache_misses_total.Increment();
    return res;// return empty string, means "replace the file in the cache!"
  } else {
    Metrics::instance().cache_hits_total.Increment();
    std::string res = _cachedir + "/" + fr.cachepath;
    if (block_file) { blocked_files[res]++; }
    return res;
  }
}

//============================================================================

void SipiCache::deblock(const std::string &res)
{
  std::lock_guard<std::mutex> locking_mutex_guard(locking);
  auto it = blocked_files.find(res);
  if (it == blocked_files.end()) return;
  --(it->second);
  if (it->second < 1) { blocked_files.erase(it); }
}

/*!
 * Creates a new cache file with a unique name.
 *
 * \return the name of the file.
 */
std::string SipiCache::getNewCacheFileName(void)
{
  std::string filename = _cachedir + "/cache_XXXXXXXXXX";
  char *c_filename = &filename[0];
  int tmp_fd = mkstemp(c_filename);

  if (tmp_fd == -1) {
    throw SipiError(std::string("Couldn't create cache file ") + filename, errno);
  }

  close(tmp_fd);
  return filename;
}

//============================================================================

void SipiCache::add(const std::string &origpath_p,
  const std::string &cache_key_p,
  const std::string &cachepath_p,
  size_t img_w_p,
  size_t img_h_p,
  size_t tile_w_p,
  size_t tile_h_p,
  int clevels_p,
  int numpages_p)
{
  SIPI_ZONE_N("SipiCache::add");
  size_t pos = cachepath_p.rfind('/');
  std::string cachepath;

  if (pos != std::string::npos) {
    cachepath = cachepath_p.substr(pos + 1);
  } else {
    cachepath = cachepath_p;
  }

  struct stat fileinfo;
  SipiCache::CacheRecord fr;

  fr.img_w = img_w_p;
  fr.img_h = img_h_p;
  fr.tile_w = tile_w_p;
  fr.tile_h = tile_h_p;
  fr.clevels = clevels_p;
  fr.numpages = numpages_p;
  fr.origpath = origpath_p;
  fr.cachepath = cachepath;

  if (stat(cachepath_p.c_str(), &fileinfo) != 0) {
    throw SipiError("Couldn't stat file \"" + origpath_p + "\"!", errno);
  }
#if defined(HAVE_ST_ATIMESPEC)
  fr.mtime = fileinfo.st_mtimespec;
#else
  fr.mtime = fileinfo.st_mtime;
#endif

  // Source file size (S2-25 freshness gate), distinct from fr.fsize below
  // (the CACHE file's size, used for eviction).
  struct stat origstat;
  if (stat(origpath_p.c_str(), &origstat) != 0) {
    throw SipiError("Couldn't stat file \"" + origpath_p + "\"!", errno);
  }
  fr.source_size = origstat.st_size;

  //
  // get the current time (seconds since Epoch)
  //
  time_t at;
  time(&at);
  fr.access_time = at;
  fr.fsize = fileinfo.st_size;

  const std::string digest = sha256_hex(cache_key_p);

  //
  // we check if there is already a file with the same cache key. If so,
  // we remove it
  //
  std::lock_guard<std::mutex> locking_mutex_guard(locking);
  auto existing = cachetable.find(digest);
  if (existing != cachetable.end()) {
    std::string toremove = _cachedir + "/" + existing->second.cachepath;
    ::unlink(toremove.c_str());
    subtractClamped(cache_used_bytes, existing->second.fsize);
    --nfiles;
  }

  int purge_result = purge(false);

  if (purge_result == -1) {
    // All files are blocked and cache is full — don't add this file
    std::string toremove = _cachedir + "/" + cachepath;
    ::unlink(toremove.c_str());
    return;
  }

  cachetable[digest] = fr;
  cache_used_bytes += fr.fsize;

  ++nfiles;

  auto &metrics = Metrics::instance();
  metrics.cache_size_bytes.Set(static_cast<double>(cache_used_bytes));
  metrics.cache_files.Set(static_cast<double>(nfiles));
}

//============================================================================

bool SipiCache::remove(const std::string &cache_key_p)
{
  std::lock_guard<std::mutex> locking_mutex_guard(locking);

  auto it = cachetable.find(sha256_hex(cache_key_p));
  if (it == cachetable.end()) {
    log_warn("Couldn't remove cache for %s: not existing!", cache_key_p.c_str());
    return false;
  }

  std::string delpath = _cachedir + "/" + it->second.cachepath;
  auto blocked_it = blocked_files.find(delpath);
  if (blocked_it != blocked_files.end() && blocked_it->second > 0) {
    log_warn("Couldn't remove cache for %s: file in use (%d)!", cache_key_p.c_str(), blocked_it->second);
    return false;
  }
  log_debug("Delete from cache \"%s\"...", it->second.cachepath.c_str());
  ::remove(delpath.c_str());
  subtractClamped(cache_used_bytes, it->second.fsize);
  cachetable.erase(it);
  --nfiles;

  return true;
}

//============================================================================

void SipiCache::loop(ProcessOneCacheFile worker, void *userdata, SortMethod sm)
{
  std::lock_guard<std::mutex> locking_mutex_guard(locking);
  std::vector<AListEle> alist;

  for (const auto &ele : cachetable) {
    AListEle al = { ele.first, ele.second.access_time, ele.second.fsize };
    alist.push_back(al);
  }

  switch (sm) {
  case SORT_ATIME_ASC: {
    sort(alist.begin(), alist.end(), _compare_access_time_asc);
    break;
  }

  case SORT_ATIME_DESC: {
    sort(alist.begin(), alist.end(), _compare_access_time_desc);
    break;
  }

  case SORT_FSIZE_ASC: {
    sort(alist.begin(), alist.end(), _compare_fsize_asc);
    break;
  }

  case SORT_FSIZE_DESC: {
    sort(alist.begin(), alist.end(), _compare_fsize_desc);
    break;
  }
  }

  int i = 1;

  for (const auto &ele : alist) {
    worker(i, ele.cache_key, cachetable[ele.cache_key], userdata);
    i++;
  }
}

//============================================================================
}
