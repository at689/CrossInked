#pragma once

#include <Print.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Stable cache path based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  struct LocationSpineEntry {
    uint32_t startLocation = 0;
    uint32_t endLocation = 0;
    uint32_t wordStart = 0;
    uint32_t wordCount = 0;
  };
  std::vector<LocationSpineEntry> locationSpine;
  uint32_t totalLocations = 0;
  uint32_t totalWords = 0;
  uint32_t wordsPerReferencePage = 0;
  uint32_t totalReferencePages = 0;
  bool crossinkLocationsLoaded = false;
  enum class CssParseStatus : uint8_t {
    Failed,
    Partial,
    Complete,
  };

  void migrateLegacyCachePath(const std::string& cacheDir) const;
  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  CssParseStatus parseCssFiles(bool forceRebuild = false) const;
  void discoverCssFilesFromZip();
  // Content-key cache survival (CrossInked): a small "content.key" sidecar records
  // fnvHash64(title|author) + source path per cache dir. On a cache miss, adopt an
  // orphaned cache (matching key, source file gone) so manual SD renames/renumbering
  // don't reset reading progress. See ROADMAP.md #5.
  void writeContentKeySidecar() const;
  void tryAdoptOrphanCacheByContentKey();
  // Scan /.crosspoint once per session, caching orphan-cache candidates in RAM so
  // subsequent cache misses avoid the O(N) sidecar walk + duplicate OPF parse. (F2)
  void buildOrphanIndex(const std::string& root, const std::string& ourName);
  // True when storedPath still refers to a live *other* file (so the orphan is not
  // orphaned and must not be stolen). Handles case-only renames of this book. (F6)
  bool storedPathIsLiveOriginal(const std::string& storedPath) const;
  // Edition guard for adoption (F5): the sidecar's recorded source size must match
  // the current file. A 0 (legacy sidecar) or unreadable size is allowed. (CrossInked)
  bool sidecarSizeMatchesForAdoption(uint64_t sidecarSize) const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() = default;
  static std::string cachePathForFilePath(const std::string& filepath, const std::string& cacheDir);
  // Rewrites the content.key sidecar in cacheDir to point at newSourcePath (and
  // refreshes the F5 size field) after the backing file + cache dir were moved.
  // Keeps the existing content key. No-op if the sidecar is missing/unparseable.
  // Used by the move-to-/Read flow so a finished book's live cache can't be
  // adopted (stolen) by a same-title duplicate via a stale source path. (F13)
  static void rewriteContentKeySourcePath(const std::string& cacheDir, const std::string& newSourcePath);
  // Drops the session-scoped orphan-cache index so the next cache miss rescans
  // /.crosspoint. Call after the SD card is (re)mounted or when orphan dirs are
  // created out of band -- otherwise a stale "already scanned, empty" flag would
  // skip adoption for the rest of the session. (CrossInked, F2)
  static void invalidateOrphanIndex();
  // True when a metadata cache already exists for this book, i.e. load() will
  // hit the fast path instead of rebuilding. Cheap: no parsing, just a stat.
  static bool hasCache(const std::string& filepath, const std::string& cacheDir);
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  // Deprecated compatibility wrapper; forwards to getThumbBmpPath(0, height).
  [[deprecated("use getThumbBmpPath(int width, int height)")]]
  std::string getThumbBmpPath(int height) const;
  // Returns the thumbnail cache path. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  std::string getThumbBmpPath(int width, int height) const;
  // Returns a Minimal-style adaptive thumbnail path. Normal cover ratios fill
  // the requested box; unusual ratios are contained inside the box.
  std::string getAdaptiveThumbBmpPath(int width, int height) const;
  // Deprecated compatibility wrapper; forwards to generateThumbBmp(0, height).
  [[deprecated("use generateThumbBmp(int width, int height)")]]
  bool generateThumbBmp(int height) const;
  // Writes a thumbnail BMP to cache. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  // Returns false on missing cache/cover, unsupported image format, or conversion failure.
  bool generateThumbBmp(int width, int height) const;
  // Writes a thumbnail that can either crop-to-fill or contain unusual cover
  // ratios, depending on the source image dimensions.
  bool generateAdaptiveThumbBmp(int width, int height) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  bool hasCrossInkLocations() const { return crossinkLocationsLoaded; }
  bool hasStablePageNumbers() const {
    return crossinkLocationsLoaded && totalWords > 0 && wordsPerReferencePage > 0 && totalReferencePages > 0;
  }
  float calculateSizeProgress(int currentSpineIndex, float currentSpineRead) const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  bool resolveLocationPercentToSpineProgress(int percent, int& spineIndex, float& spineProgress) const;
  bool resolveReferencePage(int currentSpineIndex, float currentSpineRead, uint32_t& currentPage,
                            uint32_t& pageCount) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;

 private:
  bool loadCrossInkLocations();
  bool generateThumbBmpInternal(int width, int height, bool adaptiveContain) const;
  // ASCII case-insensitive path compare used by orphan-cache adoption. (CrossInked, F6)
  static bool pathsEqualIgnoreAsciiCase(const std::string& a, const std::string& b);
};
