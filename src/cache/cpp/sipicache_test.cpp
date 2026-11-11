/*
 * Copyright © 2016 - 2024 Swiss National Data and Service Center for the Humanities and/or DaSCH Service Platform
 * contributors. SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "gtest/gtest.h"

#include "cache/SipiCache.h"
#include "error/SipiError.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <vector>

namespace {

// Helper: create a temporary directory for cache tests
std::string makeTempCacheDir()
{
  char tmpl[] = "/tmp/sipi_cache_test_XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (!dir) { throw std::runtime_error("mkdtemp failed"); }
  return std::string(dir);
}

// Helper: remove a directory and all its contents
void removeDirRecursive(const std::string &path)
{
  struct dirent **namelist;
  int n = scandir(path.c_str(), &namelist, nullptr, alphasort);
  if (n >= 0) {
    while (n--) {
      if (std::string(namelist[n]->d_name) != "." && std::string(namelist[n]->d_name) != "..") {
        std::string full = path + "/" + namelist[n]->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
          removeDirRecursive(full);
        } else {
          ::remove(full.c_str());
        }
      }
      free(namelist[n]);
    }
    free(namelist);
  }
  rmdir(path.c_str());
}

// Helper: create a dummy file of a given size
void createDummyFile(const std::string &path, size_t size)
{
  std::ofstream f(path, std::ios::binary);
  std::string data(size, 'x');
  f.write(data.data(), data.size());
}

// Helper: create a dummy "original" image file for cache to stat
std::string createOrigFile(const std::string &dir, const std::string &name, size_t size = 100)
{
  std::string path = dir + "/" + name;
  createDummyFile(path, size);
  return path;
}

// Mirrors SipiCache.cpp's private CacheIndexHeader (magic/version/record_size).
// Not exposed via SipiCache.h — the on-disk header shape is verified here by
// hand-crafting index files the same way an attacker (or bit rot) would.
struct TestCacheIndexHeader
{
  char magic[8];
  std::uint32_t version;
  std::uint32_t record_size;
};

constexpr char kValidMagic[8] = { 'S', 'I', 'P', 'I', 'C', 'A', 'C', 'H' };
constexpr std::uint32_t kValidVersion = 1;

// Helper: write a `.sipicache` index consisting of the given header followed
// by zero or more raw FileCacheRecord byte blobs.
void writeIndexFile(const std::string &cachedir,
  const TestCacheIndexHeader &header,
  const std::vector<Sipi::SipiCache::FileCacheRecord> &records)
{
  std::ofstream f(cachedir + "/.sipicache", std::ios::binary);
  f.write(reinterpret_cast<const char *>(&header), sizeof(header));
  for (const auto &fr : records) { f.write(reinterpret_cast<const char *>(&fr), sizeof(fr)); }
}

// Helper: count non-hidden files in a directory
int countFiles(const std::string &dir)
{
  int count = 0;
  struct dirent **namelist;
  int n = scandir(dir.c_str(), &namelist, nullptr, alphasort);
  if (n >= 0) {
    while (n--) {
      if (namelist[n]->d_name[0] != '.') { count++; }
      free(namelist[n]);
    }
    free(namelist);
  }
  return count;
}

class SipiCacheTest : public ::testing::Test
{
protected:
  std::string cachedir;
  std::string origdir;

  void SetUp() override
  {
    cachedir = makeTempCacheDir();
    origdir = makeTempCacheDir();
  }

  void TearDown() override
  {
    removeDirRecursive(cachedir);
    removeDirRecursive(origdir);
  }
};


// -------------------------------------------------------------------
// Constructor / startup indexing tests
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, EmptyDirectoryStartsClean)
{
  Sipi::SipiCache cache(cachedir, 1024 * 1024, 100);
  EXPECT_EQ(cache.getNfiles(), 0u);
  EXPECT_EQ(cache.getCacheUsedBytes(), 0ull);
}

TEST_F(SipiCacheTest, UnlimitedCacheConstructs)
{
  Sipi::SipiCache cache(cachedir, -1, 0);
  EXPECT_EQ(cache.getMaxCacheSize(), -1);
  EXPECT_EQ(cache.getMaxNfiles(), 0u);
}

TEST_F(SipiCacheTest, InvalidCacheDirThrows)
{
  EXPECT_THROW(Sipi::SipiCache("/nonexistent/path", 1024, 10), Sipi::SipiError);
}


// -------------------------------------------------------------------
// Crash recovery: no .sipicache index
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, CrashRecoveryClearsOrphans)
{
  // Create some orphan files in the cache dir (no .sipicache index)
  createDummyFile(cachedir + "/orphan1.tmp", 100);
  createDummyFile(cachedir + "/orphan2.tmp", 200);
  ASSERT_EQ(countFiles(cachedir), 2);

  // Constructor should clear them during crash recovery
  Sipi::SipiCache cache(cachedir, 1024 * 1024, 100);
  EXPECT_EQ(cache.getNfiles(), 0u);
  EXPECT_EQ(countFiles(cachedir), 0);
}


// -------------------------------------------------------------------
// Corrupted index
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, CorruptedIndexClearsCache)
{
  // Write a .sipicache with invalid size (not divisible by record size)
  std::string indexFile = cachedir + "/.sipicache";
  createDummyFile(indexFile, 17);// 17 bytes: not a valid record
  createDummyFile(cachedir + "/stale_cache_file", 500);

  Sipi::SipiCache cache(cachedir, 1024 * 1024, 100);
  EXPECT_EQ(cache.getNfiles(), 0u);
  // The corrupted index and orphan files should be cleaned up
}


// -------------------------------------------------------------------
// Persistence: write index, reload
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, PersistenceRoundTrip)
{
  std::string origpath = createOrigFile(origdir, "test.tif", 500);

  {
    Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
    std::string cachefile = cache.getNewCacheFileName();
    createDummyFile(cachefile, 1024);
    cache.add(origpath, "/iiif/test/full/max/0/default.jpg", cachefile, 800, 600);
    EXPECT_EQ(cache.getNfiles(), 1u);
    // destructor writes .sipicache
  }

  // Re-open: should load the persisted entry
  {
    Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
    EXPECT_EQ(cache.getNfiles(), 1u);
    EXPECT_GE(cache.getCacheUsedBytes(), 1024ull);
  }
}


// -------------------------------------------------------------------
// Orphan cleanup on reload
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, OrphanFilesRemovedOnReload)
{
  std::string origpath = createOrigFile(origdir, "test.tif", 500);

  {
    Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
    std::string cachefile = cache.getNewCacheFileName();
    createDummyFile(cachefile, 1024);
    cache.add(origpath, "/iiif/test/full/max/0/default.jpg", cachefile, 800, 600);
  }

  // Add an orphan file not tracked by the index
  createDummyFile(cachedir + "/orphan_not_in_index", 200);

  {
    Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
    // The indexed file should remain, the orphan should be removed
    EXPECT_EQ(cache.getNfiles(), 1u);
    // orphan should have been deleted
    struct stat st;
    EXPECT_NE(stat((cachedir + "/orphan_not_in_index").c_str(), &st), 0);
  }
}


// -------------------------------------------------------------------
// Add, check, remove
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, AddCheckRemove)
{
  std::string origpath = createOrigFile(origdir, "img.tif", 100);
  Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);

  std::string cache_key = "/iiif/img/full/max/0/default.jpg";

  // Check: not in cache
  std::string result = cache.check(origpath, cache_key);
  EXPECT_TRUE(result.empty());

  // Add
  std::string cachefile = cache.getNewCacheFileName();
  createDummyFile(cachefile, 2048);
  cache.add(origpath, cache_key, cachefile, 640, 480);
  EXPECT_EQ(cache.getNfiles(), 1u);

  // Check: now in cache
  result = cache.check(origpath, cache_key);
  EXPECT_FALSE(result.empty());

  // Remove
  EXPECT_TRUE(cache.remove(cache_key));
  EXPECT_EQ(cache.getNfiles(), 0u);

  // Check: gone
  result = cache.check(origpath, cache_key);
  EXPECT_TRUE(result.empty());
}


// -------------------------------------------------------------------
// (Removed in DEV-6538): the `cache.getSize()` shape-memo lookup
// was deleted along with the `sizetable` it backed. `read_shape` now reads
// the same data from the Essentials packet directly (ADR-0004 / DEV-6379).
// -------------------------------------------------------------------


// -------------------------------------------------------------------
// LRU eviction: file count limit
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, EvictionByFileCount)
{
  // max 5 files — adding 6 should trigger eviction down to 80% = 4
  Sipi::SipiCache cache(cachedir, -1, 5);

  for (int i = 0; i < 5; i++) {
    std::string name = "img" + std::to_string(i) + ".tif";
    std::string origpath = createOrigFile(origdir, name, 100);
    std::string cachefile = cache.getNewCacheFileName();
    createDummyFile(cachefile, 100);
    std::string cache_key = "/iiif/" + name + "/full/max/0/default.jpg";
    cache.add(origpath, cache_key, cachefile, 100, 100);
    // Small sleep to ensure different access_time for LRU ordering
    sleep(1);// ensure different time_t for LRU ordering
  }

  EXPECT_EQ(cache.getNfiles(), 5u);

  // Adding one more should trigger purge (from 5+1=6 to 80% of 5=4)
  std::string origpath = createOrigFile(origdir, "img_extra.tif", 100);
  std::string cachefile = cache.getNewCacheFileName();
  createDummyFile(cachefile, 100);
  cache.add(origpath, "/iiif/img_extra/full/max/0/default.jpg", cachefile, 100, 100);

  // Should have evicted down to low-water (4) then added the new one = 5 or less
  EXPECT_LE(cache.getNfiles(), 5u);
}


// -------------------------------------------------------------------
// LRU eviction: size limit
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, EvictionBySize)
{
  // max 500 bytes cache — add 5 files × 100 = 500 (exactly at limit)
  Sipi::SipiCache cache(cachedir, 500, 0);

  for (int i = 0; i < 5; i++) {
    std::string name = "img" + std::to_string(i) + ".tif";
    std::string origpath = createOrigFile(origdir, name, 50);
    std::string cachefile = cache.getNewCacheFileName();
    createDummyFile(cachefile, 100);
    std::string cache_key = "/iiif/" + name + "/full/max/0/default.jpg";
    cache.add(origpath, cache_key, cachefile, 100, 100);
    sleep(1);
  }

  // 5 * 100 = 500, exactly at limit
  EXPECT_EQ(cache.getNfiles(), 5u);
  EXPECT_EQ(cache.getCacheUsedBytes(), 500ull);

  // Adding one more triggers purge (500 >= 500), evicts to 80% = 400, then adds 100 = 500
  std::string origpath = createOrigFile(origdir, "img_extra.tif", 50);
  std::string cachefile = cache.getNewCacheFileName();
  createDummyFile(cachefile, 100);
  cache.add(origpath, "/iiif/img_extra/full/max/0/default.jpg", cachefile, 100, 100);

  EXPECT_LE(cache.getCacheUsedBytes(), 500ull);
}


// -------------------------------------------------------------------
// Dual-limit eviction: both size and nfiles
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, EvictionDualLimit)
{
  // max 3 files and 500 bytes
  Sipi::SipiCache cache(cachedir, 500, 3);

  for (int i = 0; i < 3; i++) {
    std::string name = "img" + std::to_string(i) + ".tif";
    std::string origpath = createOrigFile(origdir, name, 50);
    std::string cachefile = cache.getNewCacheFileName();
    createDummyFile(cachefile, 100);
    std::string cache_key = "/iiif/" + name + "/full/max/0/default.jpg";
    cache.add(origpath, cache_key, cachefile, 100, 100);
    sleep(1);
  }

  EXPECT_EQ(cache.getNfiles(), 3u);

  // Adding one more triggers eviction (nfiles limit hit)
  std::string origpath = createOrigFile(origdir, "img_extra.tif", 50);
  std::string cachefile = cache.getNewCacheFileName();
  createDummyFile(cachefile, 100);
  cache.add(origpath, "/iiif/img_extra/full/max/0/default.jpg", cachefile, 100, 100);

  EXPECT_LE(cache.getNfiles(), 3u);
  EXPECT_LE(cache.getCacheUsedBytes(), 500ull);
}


// -------------------------------------------------------------------
// Blocked file skip during eviction
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, BlockedFilesSkippedDuringEviction)
{
  // max 2 files
  Sipi::SipiCache cache(cachedir, -1, 2);

  // Add first file and block it
  std::string orig1 = createOrigFile(origdir, "img1.tif", 50);
  std::string cachefile1 = cache.getNewCacheFileName();
  createDummyFile(cachefile1, 100);
  cache.add(orig1, "/iiif/img1/full/max/0/default.jpg", cachefile1, 100, 100);
  usleep(10000);

  // Block the first file via check
  std::string blocked = cache.check(orig1, "/iiif/img1/full/max/0/default.jpg", true);
  EXPECT_FALSE(blocked.empty());

  // Add second file
  std::string orig2 = createOrigFile(origdir, "img2.tif", 50);
  std::string cachefile2 = cache.getNewCacheFileName();
  createDummyFile(cachefile2, 100);
  cache.add(orig2, "/iiif/img2/full/max/0/default.jpg", cachefile2, 100, 100);
  usleep(10000);

  // Add third file — should try to evict, but first file is blocked
  std::string orig3 = createOrigFile(origdir, "img3.tif", 50);
  std::string cachefile3 = cache.getNewCacheFileName();
  createDummyFile(cachefile3, 100);
  cache.add(orig3, "/iiif/img3/full/max/0/default.jpg", cachefile3, 100, 100);

  // The blocked file (img1) should still be in cache
  std::string check1 = cache.check(orig1, "/iiif/img1/full/max/0/default.jpg");
  EXPECT_FALSE(check1.empty());

  // Deblock
  cache.deblock(blocked);
}


// -------------------------------------------------------------------
// Remove nonexistent entry
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, RemoveNonexistentReturnsFalse)
{
  Sipi::SipiCache cache(cachedir, 1024 * 1024, 100);
  EXPECT_FALSE(cache.remove("/iiif/nonexistent/full/max/0/default.jpg"));
}


// -------------------------------------------------------------------
// Deblock
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, BlockAndDeblock)
{
  std::string origpath = createOrigFile(origdir, "img.tif", 100);
  Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);

  std::string cachefile = cache.getNewCacheFileName();
  createDummyFile(cachefile, 512);
  cache.add(origpath, "/iiif/img/full/max/0/default.jpg", cachefile, 100, 100);

  // Block
  std::string path = cache.check(origpath, "/iiif/img/full/max/0/default.jpg", true);
  EXPECT_FALSE(path.empty());

  // Can't remove while blocked
  EXPECT_FALSE(cache.remove("/iiif/img/full/max/0/default.jpg"));
  EXPECT_EQ(cache.getNfiles(), 1u);

  // Deblock
  cache.deblock(path);

  // Now can remove
  EXPECT_TRUE(cache.remove("/iiif/img/full/max/0/default.jpg"));
  EXPECT_EQ(cache.getNfiles(), 0u);
}


// -------------------------------------------------------------------
// Replacing existing entry
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, AddDuplicateCacheKeyReplacesEntry)
{
  std::string origpath = createOrigFile(origdir, "img.tif", 100);
  Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
  std::string cache_key = "/iiif/img/full/max/0/default.jpg";

  // Add first version
  std::string cachefile1 = cache.getNewCacheFileName();
  createDummyFile(cachefile1, 500);
  cache.add(origpath, cache_key, cachefile1, 100, 100);
  EXPECT_EQ(cache.getNfiles(), 1u);
  // Add second version with same cache key
  std::string cachefile2 = cache.getNewCacheFileName();
  createDummyFile(cachefile2, 800);
  cache.add(origpath, cache_key, cachefile2, 200, 200);
  EXPECT_EQ(cache.getNfiles(), 1u);

  // Old file removed from disk
  struct stat st;
  // The old cache file (basename extracted) should be unlinked
  EXPECT_NE(stat(cachefile1.c_str(), &st), 0);
}


// -------------------------------------------------------------------
// Startup eviction: over limits on construction
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, StartupEvictsOverLimit)
{
  std::string origpath = createOrigFile(origdir, "test.tif", 50);

  // First, create a cache with generous limits and add files
  {
    Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
    for (int i = 0; i < 10; i++) {
      std::string name = "img" + std::to_string(i) + ".tif";
      std::string orig = createOrigFile(origdir, name, 50);
      std::string cachefile = cache.getNewCacheFileName();
      createDummyFile(cachefile, 100);
      cache.add(orig, "/iiif/" + name + "/full/max/0/default.jpg", cachefile, 100, 100);
    }
    EXPECT_EQ(cache.getNfiles(), 10u);
  }

  // Re-open with tighter limit: max 5 files
  {
    Sipi::SipiCache cache(cachedir, -1, 5);
    // Constructor should evict down to 80% of 5 = 4
    EXPECT_LE(cache.getNfiles(), 4u);
  }
}

// -------------------------------------------------------------------
// S2-25: size-aware freshness gate
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, ReplacedSourceOlderOrEqualMtimeDifferentSizeIsMiss)
{
  std::string origpath = createOrigFile(origdir, "img.tif", 100);

  // Backdate the source's mtime well before "now" so the cache mtime
  // (recorded at add() time) is always newer — isolates the SIZE gate from
  // the pre-existing mtime gate. On main (mtime-only), this test is red: the
  // replacement below has an unchanged, older mtime and would still hit.
  struct utimbuf old_time{ 1000000000, 1000000000 };
  ASSERT_EQ(utime(origpath.c_str(), &old_time), 0);

  Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
  std::string cache_key = "/iiif/img/full/max/0/default.jpg";

  std::string cachefile = cache.getNewCacheFileName();
  createDummyFile(cachefile, 500);
  cache.add(origpath, cache_key, cachefile, 100, 100);
  ASSERT_FALSE(cache.check(origpath, cache_key).empty());

  // Replace with a DIFFERENT size, keeping the SAME (still older) mtime.
  createDummyFile(origpath, 250);
  ASSERT_EQ(utime(origpath.c_str(), &old_time), 0);

  EXPECT_TRUE(cache.check(origpath, cache_key).empty());
}

TEST_F(SipiCacheTest, SameSizeSameMtimeReplacementIsHit)
{
  // Pins the documented residual (ADR-0025): a same-size, same-mtime
  // replacement is indistinguishable from the original without hashing file
  // bodies on every check(), which SIPI deliberately does not do.
  std::string origpath = createOrigFile(origdir, "img.tif", 100);
  struct utimbuf old_time{ 1000000000, 1000000000 };
  ASSERT_EQ(utime(origpath.c_str(), &old_time), 0);

  Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
  std::string cache_key = "/iiif/img/full/max/0/default.jpg";
  std::string cachefile = cache.getNewCacheFileName();
  createDummyFile(cachefile, 500);
  cache.add(origpath, cache_key, cachefile, 100, 100);

  // Replace with SAME size (different content) and SAME mtime.
  createDummyFile(origpath, 100);
  ASSERT_EQ(utime(origpath.c_str(), &old_time), 0);

  EXPECT_FALSE(cache.check(origpath, cache_key).empty());
}


// -------------------------------------------------------------------
// S2-26: forged index rejection
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, ForgedIndexBadMagicStartsEmpty)
{
  TestCacheIndexHeader header{};
  std::memcpy(header.magic, "XXXXXXXX", sizeof(header.magic));
  header.version = kValidVersion;
  header.record_size = sizeof(Sipi::SipiCache::FileCacheRecord);
  writeIndexFile(cachedir, header, {});

  Sipi::SipiCache cache(cachedir, 1024 * 1024, 100);
  EXPECT_EQ(cache.getNfiles(), 0u);
}

TEST_F(SipiCacheTest, ForgedIndexWrongRecordSizeStartsEmpty)
{
  TestCacheIndexHeader header{};
  std::memcpy(header.magic, kValidMagic, sizeof(header.magic));
  header.version = kValidVersion;
  // A record_size that doesn't match this binary's FileCacheRecord layout
  // (e.g. the cross-platform timespec/time_t divergence, or a future field
  // addition) must never be parsed as a stream of records.
  header.record_size = sizeof(Sipi::SipiCache::FileCacheRecord) + 8;
  writeIndexFile(cachedir, header, {});

  Sipi::SipiCache cache(cachedir, 1024 * 1024, 100);
  EXPECT_EQ(cache.getNfiles(), 0u);
}

TEST_F(SipiCacheTest, ForgedRecordCachepathWithSlashIsRejected)
{
  TestCacheIndexHeader header{};
  std::memcpy(header.magic, kValidMagic, sizeof(header.magic));
  header.version = kValidVersion;
  header.record_size = sizeof(Sipi::SipiCache::FileCacheRecord);

  Sipi::SipiCache::FileCacheRecord fr{};
  fr.img_w = 100;
  fr.img_h = 100;
  std::snprintf(fr.cache_key, sizeof(fr.cache_key), "%s", "deadbeef");
  std::snprintf(fr.origpath, sizeof(fr.origpath), "%s", "/tmp/orig.tif");
  // A legitimate cachepath is always a getNewCacheFileName() basename with
  // no directory component — this one tries to escape the cache directory.
  std::snprintf(fr.cachepath, sizeof(fr.cachepath), "%s", "../escaped_cachefile");
  fr.access_time = time(nullptr);
  fr.fsize = 100;
  fr.source_size = 100;
  writeIndexFile(cachedir, header, { fr });

  Sipi::SipiCache cache(cachedir, 1024 * 1024, 100);
  EXPECT_EQ(cache.getNfiles(), 0u);
}


// -------------------------------------------------------------------
// S2-26: long cache keys don't collide (SHA-256, not truncation)
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, LongCacheKeysRoundTripWithoutCollision)
{
  std::string origpath1 = createOrigFile(origdir, "img1.tif", 100);
  std::string origpath2 = createOrigFile(origdir, "img2.tif", 100);

  // Two cache keys sharing a prefix well past the old 255-byte truncation
  // boundary, differing only past it — these used to collide once persisted
  // through the raw-truncation `cache_key[256]` field.
  std::string shared_prefix(280, 'a');
  std::string cache_key1 = "/iiif/" + shared_prefix + "/suffix-one/full/max/0/default.jpg";
  std::string cache_key2 = "/iiif/" + shared_prefix + "/suffix-two/full/max/0/default.jpg";

  {
    Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
    std::string cachefile1 = cache.getNewCacheFileName();
    createDummyFile(cachefile1, 500);
    cache.add(origpath1, cache_key1, cachefile1, 100, 100);

    std::string cachefile2 = cache.getNewCacheFileName();
    createDummyFile(cachefile2, 600);
    cache.add(origpath2, cache_key2, cachefile2, 100, 100);

    EXPECT_EQ(cache.getNfiles(), 2u);
    // destructor persists the index
  }

  // Reload: both digests round-trip losslessly and remain distinct.
  {
    Sipi::SipiCache cache(cachedir, 10 * 1024 * 1024, 100);
    EXPECT_EQ(cache.getNfiles(), 2u);
    EXPECT_FALSE(cache.check(origpath1, cache_key1).empty());
    EXPECT_FALSE(cache.check(origpath2, cache_key2).empty());
  }
}


// -------------------------------------------------------------------
// S2-45: cache_used_bytes never underflows
// -------------------------------------------------------------------

TEST_F(SipiCacheTest, DuplicateAddDuringEvictionClampsUsedBytesInsteadOfUnderflowing)
{
  const std::string cacheKeySmall = "/iiif/small/full/max/0/default.jpg";
  const std::string cacheKeyBig = "/iiif/big/full/max/0/default.jpg";

  std::string origSmall = createOrigFile(origdir, "small.tif", 50);
  std::string origBig = createOrigFile(origdir, "big.tif", 50);

  // Size-limited cache (100 bytes): the first entry (90 bytes) fits under the
  // limit and is never evicted on its own; the second entry (200 bytes) is
  // admitted unconditionally too, since add()'s pre-insertion check only
  // looks at the pre-existing total (90, still under 100).
  Sipi::SipiCache cache(cachedir, 100, 0);

  std::string smallCachefile = cache.getNewCacheFileName();
  createDummyFile(smallCachefile, 90);
  cache.add(origSmall, cacheKeySmall, smallCachefile, 100, 100);
  sleep(1);// ensure "small" keeps the older access_time for LRU ordering

  std::string bigCachefile = cache.getNewCacheFileName();
  createDummyFile(bigCachefile, 200);
  cache.add(origBig, cacheKeyBig, bigCachefile, 100, 100);

  // Cache is now 290 bytes, well over the 100-byte limit, uncorrected until
  // the next add() call.

  // Re-adding "small"'s cache key first subtracts its own 90 bytes as part
  // of the duplicate-replace bookkeeping (leaving 200, still over limit),
  // then calls purge() — whose LRU scan still finds the same
  // not-yet-overwritten cachetable entry and subtracts its 90 bytes a SECOND
  // time (110 remains). Purge then evicts "big" too (still over limit),
  // subtracting its full 200 bytes from the doubly-diminished 110. Without
  // the underflow guard this wraps cache_used_bytes to a huge value; with
  // it, the subtraction clamps to 0 before the replacement's 15 bytes are
  // added back.
  std::string newSmallCachefile = cache.getNewCacheFileName();
  createDummyFile(newSmallCachefile, 15);
  cache.add(origSmall, cacheKeySmall, newSmallCachefile, 100, 100);

  EXPECT_EQ(cache.getCacheUsedBytes(), 15ull);
}

}// anonymous namespace
