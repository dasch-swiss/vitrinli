/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef __defined_sipi_cache_h
#define __defined_sipi_cache_h

#include <algorithm>
#include <atomic>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/time.h>
#include <unordered_map>
#include <unordered_set>

#include "generated/SipiConfig.h"

namespace Sipi {

/*!
 * SipiCache handles all the caching of files. Whenever a request to the IIIF server is made
 * SIPI first looks in its cache if this version of the file is available. IF not, the file
 * is generated (= decoded) and simultaneously written to the server socket and the chache file.
 * If the file has already been cached, the cached version is sent (but only, if the "original"
 * is older than the cached file. In order to identify the different versions, the
 * Cache key (the IIIF Canonical URL plus watermark and restriction suffixes) is used.
 *
 * The on-disk `.sipicache` index is a fixed header (magic + version +
 * `sizeof(FileCacheRecord)`) followed by an array of `FileCacheRecord`s keyed
 * by a SHA-256 digest of the Cache key (not the raw key — a short or
 * truncated key is a forgery/collision primitive on a restricted image), and
 * every record is re-validated on load (bounded strings, no path separators,
 * `fsize` recomputed from the actual cache file). See
 * [ADR-0025](../../../docs/adr/0025-cache-index-header.md) for the header
 * layout, the version-mismatch-means-empty rule, and the documented residual
 * risk of a same-size, same-mtime file replacement.
 */
class SipiCache
{
public:
  typedef enum {
    SORT_ATIME_ASC,
    SORT_ATIME_DESC,
    SORT_FSIZE_ASC,
    SORT_FSIZE_DESC,
  } SortMethod;

  /*!
   * A struct which is used to read/write the file containing all cache information on
   * server start or server shutdown.
   */
  typedef struct
  {
    size_t img_w, img_h;
    size_t tile_w, tile_h;
    int clevels;
    int numpages;
    char cache_key[256];//!< hex SHA-256 digest of the Cache key (ADR-0025), not the raw key
    char origpath[256];
    char cachepath[256];
#if defined(HAVE_ST_ATIMESPEC)
    struct timespec mtime;//!< entry time into cache
#else
    time_t mtime;
#endif
    time_t access_time;//!< last access in seconds
    off_t fsize;//!< size of the CACHE file, used for eviction accounting
    off_t source_size;//!< size of the SOURCE file at cache-add time (S2-25 freshness gate)
  } FileCacheRecord;

  /*!
   * SipiRecord is used to form a in-memory list of all cached files. On startup of the server,
   * the cached files are read from a file (being a serialization of FileCacheRecord). On
   * server shutdown, the in-memory representation is again ritten to a file.
   */
  typedef struct _CacheRecord
  {
    size_t img_w, img_h;
    size_t tile_w, tile_h;
    int clevels;
    int numpages;
    std::string origpath;
    std::string cachepath;
#if defined(HAVE_ST_ATIMESPEC)
    struct timespec mtime;//!< entry time into cache
#else
    time_t mtime;
#endif
    time_t access_time;//!< last access in seconds
    off_t fsize;//!< size of the CACHE file, used for eviction accounting
    off_t source_size;//!< size of the SOURCE file at cache-add time (S2-25 freshness gate)
  } CacheRecord;

  // The pre-DEV-6537 `SizeRecord` typedef + `sizetable` map cached an image's
  // shape (width / height / tile_w / tile_h / clevels / numpages) keyed on its
  // original path. ADR-0004's `read_shape` fast path now reads the same shape
  // from the Essentials packet directly (DEV-6379), so the parasitic
  // shape memoization is dead weight. Deleted (DEV-6538).

  /*!
   * This is the prototype function to used as parameter for the method SipiCache::loop
   * which is applied to all cached files.
   */
  typedef void (*ProcessOneCacheFile)(int index, const std::string &, const SipiCache::CacheRecord &, void *userdata);

private:
  std::mutex locking;
  std::string _cachedir;//!< path to the cache directory
  std::unordered_map<std::string, CacheRecord> cachetable;//!< Internal map of all cached files
  std::unordered_map<std::string, int> blocked_files;
  std::atomic<unsigned long long> cache_used_bytes;//!< number of bytes in the cache
  long long max_cache_size;//!< maximum number of bytes that can be cached (-1=unlimited, 0=disabled, >0=limit)
  std::atomic<unsigned> nfiles;//!< number of files in cache
  unsigned max_nfiles;//!< maximum number of files that can be cached
public:
  /*!
   * Create a Cache instance and initialize it.
   *
   * Reads the cache index file if available, rebuilds from disk if missing (crash recovery).
   * The cache directory is created automatically if it does not exist.
   *
   * \param[in] cachedir_p Path to the cache directory (auto-created if missing).
   * \param[in] max_cache_size_p Maximum cache size in bytes (-1=unlimited, 0=disabled, >0=limit).
   * \param[in] max_nfiles_p Maximum number of files in the cache (0=no limit).
   */
  SipiCache(const std::string &cachedir_p,
    long long max_cache_size_p = -1,
    unsigned max_nfiles_p = 0);

  /*!
   * Cleans up the cache, serializes the actual cache content into a file and closes all caching
   * activities.
   */
  ~SipiCache();

  /*!
   * Function used to compare last access times (for sorting the list of cache files).
   *
   * \param[in] t1 First timestamp
   * \param[in] t2 Second timestamp
   *
   * \freturns true or false,
   */
#if defined(HAVE_ST_ATIMESPEC)

  int tcompare(struct timespec &t1, struct timespec &t2);

#else
  int tcompare(time_t &t1, time_t &t2);
#endif

  /*!
   * Purge the cache using LRU eviction. Triggers at 100% of size/file-count limits
   * and evicts down to 80% (low-water mark).
   *
   * \param[in] use_lock Whether to acquire the cache mutex (false when caller already holds it).
   *
   * \returns Number of files purged, or -1 if eviction was blocked.
   */
  int purge(bool use_lock = true);

  /*!
   * check if a file is already in the cache and up-to-date
   *
   * Freshness is gated on the source file's mtime AND size (S2-25): a
   * same-mtime replacement with a DIFFERENT size is a miss. Residual risk
   * (see ADR-0025): a same-size replacement that preserves mtime still hits —
   * SIPI does not hash source file bodies on every check() call. Operators
   * replacing a Service File in place must touch(1) it or purge the cache
   * entry.
   *
   * \param[in] origpath_p The path to the Service File
   * \param[in] cache_key_p The Cache key (see UBIQUITOUS_LANGUAGE.md)
   *
   * \returns Returns an empty string if the file is not in the cache or if the file needs to be replaced.
   *          Otherwise returns tha path to the cached file.
   */
  std::string check(const std::string &origpath_p, const std::string &cache_key_p, bool block_file = false);

  void deblock(const std::string &res);


  /*!
   * Creates a new cache file with a unique name.
   *
   * \return the name of the file.
   */
  std::string getNewCacheFileName(void);

  /*!
   * Add (or replace) a file to the cache.
   *
   * \param[in] origpath_p Path to the Service File
   * \param[in] cache_key_p The Cache key
   * \param[in] cachepath_p Path of the cache file
   */
  void add(const std::string &origpath_p,
    const std::string &cache_key_p,
    const std::string &cachepath_p,
    size_t img_w_p,
    size_t img_h_p,
    size_t tile_w_p = 0,
    size_t tile_h_p = 0,
    int clevels_p = 0,
    int numpages_p = 0);

  /*!
   * Remove one file from the cache
   *
   * \param[in] cache_key_p Cache key of the entry to remove
   */
  bool remove(const std::string &cache_key_p);

  /*!
   * Get the current size of the cache in bytes
   * \returns Size of cache in bytes
   */
  inline unsigned long long getCacheUsedBytes(void) { return cache_used_bytes; }

  /*!
   * Get the maximal size of the cache
   * \returns Maximal size of cache in bytes (-1=unlimited, 0=disabled, >0=limit)
   */
  inline long long getMaxCacheSize(void) { return max_cache_size; }

  /*!
   * get the number of cached files
   * \returns Number of cached files
   */
  inline unsigned getNfiles(void) { return nfiles; }

  /*!
   * Get the maximal number of cached files
   * \returns The maximal number of files that can be cached.
   */
  inline unsigned getMaxNfiles(void) { return max_nfiles; }

  /*!
   * get the path to the cache directory
   * \returns Path of the cache directory
   */
  inline std::string getCacheDir(void) { return _cachedir; }

  /*!
   * Loop over all cached files and apply the worker function (e.g. to collect furter
   * information about the cached files.
   *
   * \param[in] worker Function to be called for each cached file
   * \param[in] userdata Arbitrary pointer to userdata gived as parameter to each call of worker
   * \param[in] Sort method used to determine the sequence how the cache is processed.
   */
  void loop(ProcessOneCacheFile worker, void *userdata, SortMethod sm = SORT_ATIME_ASC);
};
}// namespace Sipi

#endif
