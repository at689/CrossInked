#include "Epub.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {
constexpr int kDefaultThumbHeight = 180;
constexpr char kCrossInkLocationsPath[] = "META-INF/x-locations.json";
constexpr size_t kCrossInkLocationsMaxBytes = 64 * 1024;
constexpr uint32_t kDefaultReferenceWordsPerPage = 250;

float clampUnit(const float value) {
  if (value <= 0.0f) {
    return 0.0f;
  }
  if (value >= 1.0f) {
    return 1.0f;
  }
  return value;
}

int32_t readLe32(const uint8_t* data) {
  return static_cast<int32_t>(static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24));
}

void normalizeThumbDimensions(int& width, int& height) {
  if (height <= 0) {
    height = kDefaultThumbHeight;
  }
  if (width <= 0) {
    width = static_cast<int>((static_cast<int64_t>(height) * 3 + 2) / 5);
  }
}

bool cachedBmpMatchesDimensions(const std::string& path, const int width, const int height,
                                const bool allowContainedDimensions = false) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("EBP", path, file)) {
    return false;
  }

  uint8_t header[26] = {};
  const bool hasHeader = file.size() >= sizeof(header) && file.read(header, sizeof(header)) == sizeof(header);
  file.close();
  const bool isBmp = hasHeader && header[0] == 'B' && header[1] == 'M';
  const int32_t bmpWidth = isBmp ? readLe32(header + 18) : 0;
  const int32_t bmpHeight = isBmp ? readLe32(header + 22) : 0;
  const int32_t absHeight = bmpHeight < 0 ? -bmpHeight : bmpHeight;
  const bool exactMatch = isBmp && bmpWidth == width && absHeight == height;
  const bool containedMatch = allowContainedDimensions && isBmp && bmpWidth > 0 && absHeight > 0 && bmpWidth <= width &&
                              absHeight <= height && (bmpWidth == width || absHeight == height);
  const bool matches = exactMatch || containedMatch;
  if (!matches) {
    LOG_DBG("EBP", "Removing stale thumbnail dimensions: %s (%dx%d expected %dx%d)", path.c_str(), bmpWidth, absHeight,
            width, height);
    Storage.remove(path.c_str());
  }
  return matches;
}

std::string getThumbBmpPathForDimensions(const std::string& cachePath, int width, int height) {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

std::string getAdaptiveThumbBmpPathForDimensions(const std::string& cachePath, int width, int height) {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + "_fit.bmp";
}

std::string legacyCachePathForFilePath(const std::string& filepath, const std::string& cacheDir) {
  return cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(filepath));
}

class CoverImageRefScanner final : public Print {
 public:
  std::string imageRef;

  size_t write(uint8_t data) override { return write(&data, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size && imageRef.empty(); ++i) {
      consume(static_cast<char>(buffer[i]));
    }
    return size;
  }

 private:
  static constexpr size_t kMaxImageRefLen = 512;
  static constexpr const char* kXlinkPattern = "xlink:href=\"";
  static constexpr const char* kSrcPattern = "src=\"";

  size_t xlinkMatched = 0;
  size_t srcMatched = 0;
  bool collecting = false;
  std::string candidate;

  static bool isSupportedImageRef(const std::string& ref) {
    const auto view = std::string_view{ref};
    return FsHelpers::hasPngExtension(view) || FsHelpers::hasJpgExtension(view) || FsHelpers::hasGifExtension(view);
  }

  void consume(const char c) {
    if (collecting) {
      if (c == '"') {
        if (isSupportedImageRef(candidate)) {
          imageRef = candidate;
        }
        candidate.clear();
        collecting = false;
        xlinkMatched = 0;
        srcMatched = 0;
        return;
      }
      if (candidate.size() < kMaxImageRefLen) {
        candidate.push_back(c);
      } else {
        candidate.clear();
        collecting = false;
      }
      return;
    }

    const auto advance = [c](const char* pattern, size_t matched) {
      if (c == pattern[matched]) {
        return matched + 1;
      }
      return c == pattern[0] ? size_t{1} : size_t{0};
    };

    xlinkMatched = advance(kXlinkPattern, xlinkMatched);
    srcMatched = advance(kSrcPattern, srcMatched);

    if (kXlinkPattern[xlinkMatched] == '\0' || kSrcPattern[srcMatched] == '\0') {
      collecting = true;
      candidate.clear();
    }
  }
};

// content.key sidecar parsing/sizing (CrossInked). Defined here so both
// writeContentKeySidecar() and the adoption scan can use them.
//
// Sidecar format (line-based, backward-tolerant):
//   line 1: fnvHash64(title \x1f author)   (decimal)
//   line 2: source EPUB path
//   line 3 (optional, F5): source EPUB file size in bytes (decimal)
// A missing or unparseable size line yields sourceSize == 0 ("legacy" sidecar).
struct SidecarInfo {
  uint64_t key = 0;
  std::string sourcePath;
  uint64_t sourceSize = 0;  // 0 == absent/legacy
};

// Returns false when the sidecar is missing/garbage or the key is 0 (see F12:
// strtoull yields 0 on garbage, and a stored 0 must never match a real key).
bool readContentKeySidecar(const std::string& keyFilePath, SidecarInfo& out) {
  if (!Storage.exists(keyFilePath.c_str())) return false;
  std::string content(Storage.readFile(keyFilePath.c_str()).c_str());
  const size_t nl = content.find('\n');
  if (nl == std::string::npos) return false;
  const uint64_t storedKey = strtoull(content.substr(0, nl).c_str(), nullptr, 10);
  if (storedKey == 0) return false;  // garbage or absent key -> never adoptable

  std::string rest = content.substr(nl + 1);
  std::string storedPath = rest;
  std::string sizeLine;
  const size_t nl2 = rest.find('\n');
  if (nl2 != std::string::npos) {
    storedPath = rest.substr(0, nl2);
    sizeLine = rest.substr(nl2 + 1);
    const size_t nl3 = sizeLine.find('\n');
    if (nl3 != std::string::npos) sizeLine = sizeLine.substr(0, nl3);
  }
  while (!storedPath.empty() && (storedPath.back() == '\n' || storedPath.back() == '\r')) storedPath.pop_back();

  uint64_t storedSize = 0;
  if (!sizeLine.empty()) {
    // Trim and require all-digits so a non-numeric line reads as legacy (0), not garbage.
    while (!sizeLine.empty() && (sizeLine.back() == '\n' || sizeLine.back() == '\r' || sizeLine.back() == ' '))
      sizeLine.pop_back();
    bool allDigits = !sizeLine.empty();
    for (const char c : sizeLine) {
      if (c < '0' || c > '9') {
        allDigits = false;
        break;
      }
    }
    if (allDigits) storedSize = strtoull(sizeLine.c_str(), nullptr, 10);
  }

  out.key = storedKey;
  out.sourcePath = std::move(storedPath);
  out.sourceSize = storedSize;
  return true;
}

// Size in bytes of the EPUB backing file at path, or 0 if it can't be read
// (a 0 result is treated as "unknown" and never blocks adoption).
uint64_t epubFileSize(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("EBP", path, f)) return 0;
  const uint64_t sz = f.fileSize64();
  f.close();
  return sz;
}
}  // namespace

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = cachePathForFilePath(this->filepath, cacheDir);
  migrateLegacyCachePath(cacheDir);
}

std::string Epub::cachePathForFilePath(const std::string& filepath, const std::string& cacheDir) {
  // Keep on-disk EPUB cache keys stable across standard library/toolchain changes.
  return cacheDir + "/epub_" + std::to_string(ZipFile::fnvHash64(filepath.c_str(), filepath.size()));
}

bool Epub::hasCache(const std::string& filepath, const std::string& cacheDir) {
  return BookMetadataCache::exists(cachePathForFilePath(filepath, cacheDir));
}

void Epub::migrateLegacyCachePath(const std::string& cacheDir) const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  const std::string legacyCachePath = legacyCachePathForFilePath(filepath, cacheDir);
  if (legacyCachePath == cachePath || !Storage.exists(legacyCachePath.c_str())) {
    return;
  }

  if (Storage.rename(legacyCachePath.c_str(), cachePath.c_str())) {
    LOG_INF("EBP", "Migrated legacy EPUB cache: %s -> %s", legacyCachePath.c_str(), cachePath.c_str());
  } else {
    LOG_ERR("EBP", "Failed to migrate legacy EPUB cache: %s -> %s", legacyCachePath.c_str(), cachePath.c_str());
  }
}

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    CoverImageRefScanner scanner;
    if (readItemContentsToStream(opfParser.guideCoverPageHref, scanner, 512) && !scanner.imageRef.empty()) {
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }
      bookMetadata.coverItemHref =
          FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + scanner.imageRef));
      LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
    }
  }

  // Prefer an explicit guide "text" reference; fall back to a "start" reference
  // (common in EPUB2/Project Gutenberg files that omit "text") so the book opens
  // at its intended first page rather than spine 0 (title/legal front matter).
  bookMetadata.textReferenceHref =
      opfParser.textReferenceHref.empty() ? opfParser.startReferenceHref : opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  const auto tmpNcxPath = getCachePath() + "/toc.ncx";
  FsFile tempNcxFile;
  if (!Storage.openFileForWrite("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  readItemContentsToStream(tocNcxItem, tempNcxFile, 1024);
  // Explicitly close() file before reopening for reading
  tempNcxFile.close();
  if (!Storage.openFileForRead("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  const auto ncxSize = tempNcxFile.size();

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  const auto ncxBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!ncxBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc ncx parser");
    return false;
  }

  while (tempNcxFile.available()) {
    const auto readSize = tempNcxFile.read(ncxBuffer, 1024);
    if (readSize == 0) break;
    const auto processedSize = ncxParser.write(ncxBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc ncx data");
      free(ncxBuffer);
      return false;
    }
  }

  free(ncxBuffer);
  // Explicitly close() file before calling Storage.remove()
  tempNcxFile.close();
  Storage.remove(tmpNcxPath.c_str());

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  const auto tmpNavPath = getCachePath() + "/toc.nav";
  FsFile tempNavFile;
  if (!Storage.openFileForWrite("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  readItemContentsToStream(tocNavItem, tempNavFile, 1024);
  // Explicitly close() file before reopening for reading
  tempNavFile.close();
  if (!Storage.openFileForRead("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  const auto navSize = tempNavFile.size();

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  const auto navBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!navBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc nav parser");
    return false;
  }

  while (tempNavFile.available()) {
    const auto readSize = tempNavFile.read(navBuffer, 1024);
    const auto processedSize = navParser.write(navBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc nav data");
      free(navBuffer);
      return false;
    }
  }

  free(navBuffer);
  // Explicitly close() file before calling Storage.remove()
  tempNavFile.close();
  Storage.remove(tmpNavPath.c_str());

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

Epub::CssParseStatus Epub::parseCssFiles(const bool forceRebuild) const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache() && !forceRebuild) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return cssParser->isCachePartial() ? CssParseStatus::Partial : CssParseStatus::Complete;
  }

  // No cache yet - parse CSS files. If memory runs out partway through, keep
  // the rules already parsed and persist them as a marked partial cache so
  // chapter layout can still use most of the book's stylesheet.
  bool parsedAllCss = true;
  size_t parsedCssFileCount = 0;
  size_t failedCssFileIndex = 0;
  std::string failedCssPath;
  for (size_t cssFileIndex = 0; cssFileIndex < cssFiles.size(); ++cssFileIndex) {
    const auto& cssPath = cssFiles[cssFileIndex];
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      parsedAllCss = false;
      failedCssFileIndex = cssFileIndex + 1;
      failedCssPath = cssPath;
      break;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    FsFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    if (!cssParser->loadFromStream(tempCssFile)) {
      failedCssFileIndex = cssFileIndex + 1;
      failedCssPath = cssPath;
      LOG_ERR("EBP", "CSS parsing failed for file %zu/%zu after %zu parsed files: %s", failedCssFileIndex,
              cssFiles.size(), parsedCssFileCount, cssPath.c_str());
      parsedAllCss = false;
    } else {
      ++parsedCssFileCount;
    }
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
    if (!parsedAllCss) {
      break;
    }
  }

  if (!parsedAllCss && cssParser->empty()) {
    LOG_ERR("EBP", "CSS parsing failed for %s before any usable rules were loaded; CSS cache will not be written",
            failedCssPath.empty() ? "<unknown>" : failedCssPath.c_str());
    cssParser->clear();
    return CssParseStatus::Failed;
  }

  if (!parsedAllCss) {
    LOG_ERR("EBP", "Saving %zu partial CSS rules after parse stopped in %s", cssParser->ruleCount(),
            failedCssPath.empty() ? "<unknown>" : failedCssPath.c_str());
  }

  // Save to cache for next time
  if (!cssParser->saveToCache(parsedAllCss)) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
    cssParser->clear();
    return CssParseStatus::Failed;
  }

  LOG_DBG("EBP", "Loaded %zu %s CSS style rules from %zu/%zu files", cssParser->ruleCount(),
          parsedAllCss ? "complete" : "partial", parsedCssFileCount, cssFiles.size());
  cssParser->clear();
  return parsedAllCss ? CssParseStatus::Complete : CssParseStatus::Partial;
}

// load in the meta data for the epub file
void Epub::writeContentKeySidecar() const {
  if (bookMetadataCache == nullptr) return;
  const std::string keyFile = cachePath + "/content.key";
  const uint64_t currentSize = epubFileSize(filepath);
  // Skip the write only when the sidecar already records the current source path
  // AND already carries a size (so legacy sidecars get upgraded with a size
  // field on their next open, strengthening the F5 edition check over time).
  if (Storage.exists(keyFile.c_str())) {
    SidecarInfo existing;
    if (readContentKeySidecar(keyFile, existing) && existing.sourcePath == filepath && existing.sourceSize != 0) {
      return;
    }
  }
  const auto& m = bookMetadataCache->coreMetadata;
  const std::string ck = m.title + "\x1f" + m.author;
  const uint64_t key = ZipFile::fnvHash64(ck.c_str(), ck.size());
  String out(std::to_string(key).c_str());
  out += "\n";
  out += filepath.c_str();
  out += "\n";
  // Line 3 (F5): source file size for the edition check. Written as 0 only if
  // the file size can't be read; readers treat 0 as "legacy" and skip the guard.
  out += std::to_string(currentSize).c_str();
  out += "\n";
  Storage.writeFile(keyFile.c_str(), out);
}

namespace {
// Clears a pre-created destination cache dir so an orphan can be renamed into
// its place. SdFat's FAT rename() FAILS when the destination path already
// exists (unlike POSIX rename in the simulator), and several flows
// (prewarm/setupCacheDir, reader open, Clear Reading Cache) create the empty
// destination dir before load() runs — so without this, adoption silently
// no-ops on device and reading progress is stranded forever. (CrossInked)
//
// SAFETY: the caller has already verified !BookMetadataCache::exists(dstDir),
// so dstDir provably contains no book.bin. We only ever rmdir an empty dir
// (rmdir fails on non-empty dirs), and any stray files (e.g. stats*.bin /
// progress.bin restored in place by Clear Reading Cache) are relocated into
// the orphan we are about to adopt rather than deleted — except when the
// orphan already holds its own copy under the same name, in which case the
// orphan's copy (the real, longer reading history) wins and the stray is
// dropped. Returns true when dstDir was removed and the rename may proceed.
bool clearPreCreatedCacheDir(const std::string& dstDir, const std::string& orphanDir) {
  if (BookMetadataCache::exists(dstDir)) return false;  // never touch a dir holding a real cache

  auto dir = Storage.open(dstDir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  // Relocate any stray files into the orphan dir before removing the (now
  // empty) destination. The orphan's own files take precedence on collisions.
  char name[128];
  bool relocateOk = true;
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const bool isSubDir = f.isDirectory();
    f.close();
    const std::string entryName(name);
    if (entryName.empty() || entryName == "." || entryName == "..") continue;
    if (isSubDir) {
      // A sub-directory means this is not a freshly-created stats-only dir;
      // leave it untouched and abort rather than risk data.
      relocateOk = false;
      continue;
    }
    const std::string src = dstDir + "/" + entryName;
    const std::string dst = orphanDir + "/" + entryName;
    if (Storage.exists(dst.c_str())) {
      Storage.remove(src.c_str());  // orphan already has the authoritative copy
    } else if (!Storage.rename(src.c_str(), dst.c_str())) {
      LOG_ERR("EBP", "Could not relocate stray cache file %s -> %s during adoption", src.c_str(), dst.c_str());
      relocateOk = false;
    }
  }
  dir.close();

  if (!relocateOk) return false;  // leave the orphan intact rather than lose data
  if (!Storage.rmdir(dstDir.c_str())) {
    LOG_ERR("EBP", "Could not rmdir pre-created cache dir %s; leaving orphan unadopted", dstDir.c_str());
    return false;
  }
  return true;
}

// Session-scoped orphan-cache index (CrossInked). The original adoption scan
// walked all of /.crosspoint reading every content.key sidecar AND pre-parsed
// the OPF on EVERY cache miss -- O(N) SD path lookups per open and O(N^2)
// during a full prewarm at ~1,850 books. Orphans only ever shrink within a
// session (a cache is adopted, never spontaneously created), and the Arduino
// loop is single-threaded, so we scan /.crosspoint once and keep the small set
// of orphan candidates in RAM. Later misses consult the list; when it is empty
// (the common case) we skip even the expensive OPF pre-parse. On overflow we
// fall back to the per-miss directory walk so correctness never depends on the
// cap.
struct OrphanEntry {
  uint64_t key;         // fnvHash64(title \x1f author)
  std::string dirName;  // e.g. "epub_<hash>"
};

constexpr size_t kMaxOrphanIndex = 64;
bool s_orphansScanned = false;
bool s_orphansOverflowed = false;  // too many orphans -> fall back to full scan
std::vector<OrphanEntry> s_orphans;
}  // namespace

// Case-insensitive ASCII path compare (F6): FAT LFN lookups are
// case-insensitive/case-preserving, so a case-only rename ("abc.epub" ->
// "ABC.epub") leaves the old stored path matching the new file under
// Storage.exists(), which would defeat adoption. Treat paths that differ only
// by ASCII case as the same source so those renames adopt. (CrossInked)
bool Epub::pathsEqualIgnoreAsciiCase(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

// True when storedPath still refers to a live *other* file (so the orphan is
// not actually orphaned and must not be stolen). A case-only rename of this
// book leaves storedPath pointing at our own renamed file -- treat that as the
// book itself (an orphan we should adopt), not a live original. (CrossInked, F6)
bool Epub::storedPathIsLiveOriginal(const std::string& storedPath) const {
  if (storedPath == filepath) return false;                      // same source
  if (pathsEqualIgnoreAsciiCase(storedPath, filepath)) return false;  // case-only rename of this book
  return Storage.exists(storedPath.c_str());                     // some other file still holds this path
}

// Content check for adoption (F5): the title|author hash alone will match a
// different edition of the same book (a normal Calibre re-export). Compare the
// sidecar's recorded source size against the current file's size -- renames
// preserve size, different editions almost never share an exact byte count.
// A missing size field (legacy sidecar written before F5) is allowed with a
// log: requiring a rebuild would strand reading progress for every book cached
// before this flash, which is exactly the loss adoption exists to prevent.
bool Epub::sidecarSizeMatchesForAdoption(uint64_t sidecarSize) const {
  if (sidecarSize == 0) {
    LOG_DBG("EBP", "Adopting via legacy sidecar (no size field) for %s", filepath.c_str());
    return true;  // legacy sidecar -> allow (see rationale above)
  }
  const uint64_t currentSize = epubFileSize(filepath);
  if (currentSize == 0) {
    LOG_DBG("EBP", "Could not size %s; skipping size guard for adoption", filepath.c_str());
    return true;  // unknown current size -> don't block on a read failure
  }
  if (currentSize != sidecarSize) {
    LOG_INF("EBP", "Rejecting cache adoption: size mismatch (sidecar=%llu current=%llu) for %s",
            static_cast<unsigned long long>(sidecarSize), static_cast<unsigned long long>(currentSize),
            filepath.c_str());
    return false;
  }
  return true;
}

void Epub::buildOrphanIndex(const std::string& root, const std::string& ourName) {
  s_orphans.clear();
  s_orphansOverflowed = false;
  s_orphansScanned = true;

  auto dir = Storage.open(root.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[128];
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const bool isDir = f.isDirectory();
    f.close();
    if (!isDir) continue;
    const std::string entryName(name);
    if (entryName == ourName || entryName.rfind("epub_", 0) != 0) continue;

    SidecarInfo info;
    if (!readContentKeySidecar(root + "/" + entryName + "/content.key", info)) continue;
    // Keep a candidate when it looks adoptable, i.e. NOT a live book sitting at
    // its own canonical home. A live, correctly-homed book has both an existing
    // source file AND a cache dir whose name matches cachePathForFilePath of the
    // source -- skip those. This keeps the index tiny in a healthy library while
    // still retaining (a) source-gone orphans from renames/renumbers/moves, and
    // (b) dirs whose sidecar path is stale or case-mismatched (the F6 case-only
    // rename still resolves the old path via FAT's case-insensitive exists()).
    // The per-candidate storedPathIsLiveOriginal() check makes the final call.
    const bool sourceExists = Storage.exists(info.sourcePath.c_str());
    const std::string canonicalHome = cachePathForFilePath(info.sourcePath, root);
    const bool atCanonicalHome = canonicalHome == (root + "/" + entryName);
    if (sourceExists && atCanonicalHome) continue;  // live book at home -> not adoptable

    if (s_orphans.size() >= kMaxOrphanIndex) {
      LOG_INF("EBP", "Orphan-cache index overflow (>%zu); using full scan for adoption", kMaxOrphanIndex);
      s_orphansOverflowed = true;
      s_orphans.clear();
      break;
    }
    s_orphans.push_back(OrphanEntry{info.key, entryName});
  }
  dir.close();
  LOG_DBG("EBP", "Built orphan-cache index: %zu candidate(s)", s_orphans.size());
}

void Epub::tryAdoptOrphanCacheByContentKey() {
  if (BookMetadataCache::exists(cachePath)) return;  // our own cache already present

  const size_t slash = cachePath.find_last_of('/');
  if (slash == std::string::npos) return;
  const std::string root = cachePath.substr(0, slash);      // e.g. "/.crosspoint"
  const std::string ourName = cachePath.substr(slash + 1);  // e.g. "epub_<hash>"

  // Build the session orphan index on the first miss.
  if (!s_orphansScanned) {
    buildOrphanIndex(root, ourName);
  }

  // Fast path: no orphan candidates this session -> skip the OPF pre-parse and
  // the directory walk entirely. This is the common case at steady state and is
  // the main win of the index (a fresh open no longer pays the duplicate
  // parseContentOpf + O(N) sidecar walk when nothing was renamed). (F2)
  if (!s_orphansOverflowed && s_orphans.empty()) {
    return;
  }

  // Cheap metadata-only parse (no cache writes) to derive the content key. Only
  // reached when at least one orphan candidate exists, so the extra parse is
  // paid only when a rename may actually have happened.
  BookMetadataCache::BookMetadata meta;
  if (!parseContentOpf(meta, /*writeSpineEntries=*/false)) return;
  if (meta.title.empty() && meta.author.empty()) return;
  const std::string ck = meta.title + "\x1f" + meta.author;
  const uint64_t key = ZipFile::fnvHash64(ck.c_str(), ck.size());

  std::string adoptFrom;
  size_t adoptedIndexSlot = SIZE_MAX;

  if (s_orphansOverflowed) {
    // Fallback: bounded per-miss directory walk (index too large to hold).
    auto dir = Storage.open(root.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return;
    }
    char name[128];
    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      f.getName(name, sizeof(name));
      const bool isDir = f.isDirectory();
      f.close();
      if (!isDir) continue;
      const std::string entryName(name);
      if (entryName == ourName || entryName.rfind("epub_", 0) != 0) continue;
      SidecarInfo info;
      if (!readContentKeySidecar(root + "/" + entryName + "/content.key", info)) continue;
      if (info.key != key) continue;
      if (storedPathIsLiveOriginal(info.sourcePath)) continue;
      if (!sidecarSizeMatchesForAdoption(info.sourceSize)) continue;  // wrong edition -> don't adopt
      adoptFrom = root + "/" + entryName;
      break;
    }
    dir.close();
  } else {
    // Fast path: consult the in-RAM index, re-verifying the winning candidate's
    // sidecar before committing (the index only stored the key, so re-read to
    // confirm the source is still gone, the size still matches, and guard against
    // a case-only rename).
    for (size_t i = 0; i < s_orphans.size(); i++) {
      if (s_orphans[i].key != key) continue;
      SidecarInfo info;
      const std::string dirPath = root + "/" + s_orphans[i].dirName;
      if (!readContentKeySidecar(dirPath + "/content.key", info) || info.key != key) {
        continue;
      }
      if (storedPathIsLiveOriginal(info.sourcePath)) continue;
      if (!sidecarSizeMatchesForAdoption(info.sourceSize)) continue;  // wrong edition -> don't adopt
      adoptFrom = dirPath;
      adoptedIndexSlot = i;
      break;
    }
  }

  if (adoptFrom.empty()) return;
  LOG_INF("EBP", "Adopting orphaned cache %s for %s (content-key match, source gone)", adoptFrom.c_str(),
          filepath.c_str());

  // SdFat rename() fails if the destination path already exists, and several
  // flows pre-create the (empty, book.bin-less) destination dir before load().
  // Clear it first, relocating any stray stats/progress into the orphan. If it
  // still exists we cannot rename onto it, so bail rather than silently no-op. (CrossInked)
  if (Storage.exists(cachePath.c_str()) && !clearPreCreatedCacheDir(cachePath, adoptFrom)) {
    LOG_ERR("EBP", "Adoption aborted: destination cache dir %s exists and could not be cleared", cachePath.c_str());
    return;
  }

  if (!Storage.rename(adoptFrom.c_str(), cachePath.c_str())) {
    // Falls back to a fresh build; log so a stranded orphan is diagnosable.
    LOG_ERR("EBP", "Failed to adopt orphaned cache %s -> %s; will rebuild fresh", adoptFrom.c_str(), cachePath.c_str());
    return;
  }

  // Adoption succeeded: this orphan no longer exists, so drop it from the index.
  if (adoptedIndexSlot != SIZE_MAX && adoptedIndexSlot < s_orphans.size()) {
    s_orphans.erase(s_orphans.begin() + adoptedIndexSlot);
  }
}

bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));

  // Content-key cache survival (CrossInked): if our path-hashed cache is missing but an
  // orphaned cache for the same book (renamed/renumbered on the SD card) still exists,
  // adopt it so reading progress and layout cache survive the rename.
  if (buildIfMissing) {
    tryAdoptOrphanCacheByContentKey();
  }

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing or when cache version changed (loadFromCache removes stale file)
      bool rebuildCssCache = false;
      bool forceCssRebuild = false;
      bool retryingPartialCssCache = false;
      if (!cssParser->hasCache()) {
        LOG_DBG("EBP", "CSS rules cache missing, attempting to parse CSS files");
        rebuildCssCache = true;
      } else if (!cssParser->loadFromCache()) {
        LOG_DBG("EBP", "CSS rules cache stale, attempting to parse CSS files");
        cssParser->deleteCache();
        rebuildCssCache = true;
      } else if (cssParser->isCachePartial()) {
        LOG_DBG("EBP", "CSS rules cache is partial, attempting to rebuild complete CSS cache");
        cssParser->clear();
        rebuildCssCache = true;
        forceCssRebuild = true;
        retryingPartialCssCache = true;
      } else {
        cssParser->clear();
      }

      if (rebuildCssCache) {
        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        } else {
          discoverCssFilesFromZip();
        }
        bookMetadataCache.reset();
        const CssParseStatus cssStatus = parseCssFiles(forceCssRebuild);
        bookMetadataCache.reset(new BookMetadataCache(cachePath));
        if (!bookMetadataCache->load()) {
          LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
          return false;
        }
        if (cssStatus == CssParseStatus::Complete ||
            (cssStatus == CssParseStatus::Partial && !retryingPartialCssCache)) {
          // Invalidate section caches so they are rebuilt with the new CSS.
          Storage.removeDir((cachePath + "/sections").c_str());
        } else if (cssStatus == CssParseStatus::Partial) {
          LOG_ERR("EBP", "CSS cache is still partial after rebuild; preserving existing section caches");
        } else {
          LOG_ERR("EBP", "CSS cache rebuild failed; preserving existing section caches");
        }
      }
    }
    loadCrossInkLocations();
    writeContentKeySidecar();
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  discoverCssFilesFromZip();
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to keep heap as open as possible for rule-table growth.
    bookMetadataCache.reset();
    if (parseCssFiles() != CssParseStatus::Failed) {
      Storage.removeDir((cachePath + "/sections").c_str());
    } else {
      LOG_ERR("EBP", "CSS cache build failed; leaving any existing section caches in place");
    }
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  loadCrossInkLocations();
  writeContentKeySidecar();
  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverJpg, 1024)) {
      LOG_ERR("EBP", "Failed to read cover JPG item: %s", coverImageHref.c_str());
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    coverBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverPng, 1024)) {
      LOG_ERR("EBP", "Failed to read cover PNG item: %s", coverImageHref.c_str());
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    coverBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[WIDTH]x[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return getThumbBmpPath(0, height); }
std::string Epub::getThumbBmpPath(int width, int height) const {
  normalizeThumbDimensions(width, height);
  const std::string newPath = getThumbBmpPathForDimensions(cachePath, width, height);
  if (Storage.exists(newPath.c_str())) {
    return newPath;
  }
  const std::string legacyPath = cachePath + "/thumb_" + std::to_string(height) + ".bmp";
  if (Storage.exists(legacyPath.c_str())) {
    return legacyPath;
  }
  return newPath;
}

std::string Epub::getAdaptiveThumbBmpPath(int width, int height) const {
  normalizeThumbDimensions(width, height);
  return getAdaptiveThumbBmpPathForDimensions(cachePath, width, height);
}

bool Epub::generateThumbBmp(int height) const { return generateThumbBmp(0, height); }

bool Epub::generateThumbBmp(int width, int height) const { return generateThumbBmpInternal(width, height, false); }

bool Epub::generateAdaptiveThumbBmp(int width, int height) const {
  return generateThumbBmpInternal(width, height, true);
}

bool Epub::generateThumbBmpInternal(int width, int height, const bool adaptiveContain) const {
  if (height <= 0) {
    LOG_DBG("EBP", "Using default thumb BMP height for requested dimensions: %dx%d", width, height);
  }
  normalizeThumbDimensions(width, height);
  const std::string thumbPath = adaptiveContain ? getAdaptiveThumbBmpPathForDimensions(cachePath, width, height)
                                                : getThumbBmpPathForDimensions(cachePath, width, height);

  // Already generated with matching dimensions, return true
  if (cachedBmpMatchesDimensions(thumbPath, width, height, adaptiveContain)) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from JPG cover image");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    FsFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverJpg, 1024)) {
      LOG_ERR("EBP", "Failed to read thumbnail JPG item: %s", coverImageHref.c_str());
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    int THUMB_TARGET_WIDTH = width;
    int THUMB_TARGET_HEIGHT = height;
    const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverJpg, thumbBmp, THUMB_TARGET_WIDTH,
                                                                             THUMB_TARGET_HEIGHT, adaptiveContain);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    thumbBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
      Storage.remove(thumbPath.c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    FsFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    if (!readItemContentsToStream(coverImageHref, coverPng, 1024)) {
      LOG_ERR("EBP", "Failed to read thumbnail PNG item: %s", coverImageHref.c_str());
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    int THUMB_TARGET_WIDTH = width;
    int THUMB_TARGET_HEIGHT = height;
    const bool success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverPng, thumbBmp, THUMB_TARGET_WIDTH,
                                                                           THUMB_TARGET_HEIGHT, adaptiveContain);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    thumbBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
      Storage.remove(thumbPath.c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
  }

  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

bool Epub::loadCrossInkLocations() {
  locationSpine.clear();
  totalLocations = 0;
  totalWords = 0;
  wordsPerReferencePage = 0;
  totalReferencePages = 0;
  crossinkLocationsLoaded = false;

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return false;
  }

  const int spineCount = getSpineItemsCount();
  if (spineCount <= 0) {
    return false;
  }

  size_t manifestSize = 0;
  if (!getItemSize(kCrossInkLocationsPath, &manifestSize)) {
    return false;
  }
  if (manifestSize == 0 || manifestSize > kCrossInkLocationsMaxBytes) {
    LOG_ERR("EBP", "Ignoring CrossInk locations manifest with unsupported size: %zu bytes", manifestSize);
    return false;
  }

  size_t bytesRead = 0;
  uint8_t* manifestData = readItemContentsToBytes(kCrossInkLocationsPath, &bytesRead, true);
  if (!manifestData) {
    LOG_ERR("EBP", "Failed to read CrossInk locations manifest");
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(manifestData), bytesRead);
  free(manifestData);

  if (err) {
    LOG_ERR("EBP", "CrossInk locations parse error: %s", err.c_str());
    return false;
  }

  const char* format = doc["format"] | "";
  const int version = doc["version"] | 0;
  const uint32_t parsedTotalLocations = doc["totalLocations"] | 0;
  const uint32_t parsedTotalWords = doc["totalWords"] | 0;
  const uint32_t parsedWordsPerReferencePage = doc["wordsPerReferencePage"] | 0;
  const uint32_t parsedTotalReferencePages = doc["totalReferencePages"] | 0;
  JsonArrayConst spine = doc["spine"];

  if (std::strcmp(format, "crossink-locations") != 0 || version != 1 || parsedTotalLocations == 0 || spine.isNull()) {
    LOG_ERR("EBP", "Ignoring unsupported CrossInk locations manifest");
    return false;
  }

  locationSpine.assign(static_cast<size_t>(spineCount), {});
  bool hasValidEntry = false;
  size_t ordinal = 0;
  for (JsonObjectConst spineItem : spine) {
    const int index = spineItem["index"] | static_cast<int>(ordinal);
    ordinal++;
    if (index < 0 || index >= spineCount) {
      continue;
    }

    const uint32_t startLocation = spineItem["startLocation"] | 0;
    const uint32_t endLocation = spineItem["endLocation"] | 0;
    const uint32_t wordStart = spineItem["wordStart"] | 0;
    const uint32_t wordCount = spineItem["wordCount"] | 0;
    if (startLocation == 0 && endLocation == 0) {
      continue;
    }
    if (startLocation == 0 || endLocation < startLocation || endLocation > parsedTotalLocations) {
      LOG_ERR("EBP", "Ignoring invalid CrossInk location range at spine %d", index);
      continue;
    }

    locationSpine[static_cast<size_t>(index)] = {startLocation, endLocation, wordStart, wordCount};
    hasValidEntry = true;
  }

  if (!hasValidEntry) {
    locationSpine.clear();
    return false;
  }

  totalLocations = parsedTotalLocations;
  totalWords = parsedTotalWords;
  wordsPerReferencePage = parsedWordsPerReferencePage > 0 ? parsedWordsPerReferencePage : kDefaultReferenceWordsPerPage;
  totalReferencePages = parsedTotalReferencePages;
  if (totalReferencePages == 0 && totalWords > 0 && wordsPerReferencePage > 0) {
    totalReferencePages = (totalWords + wordsPerReferencePage - 1) / wordsPerReferencePage;
  }
  crossinkLocationsLoaded = true;
  LOG_INF("EBP", "Loaded CrossInk locations: %lu locations, %lu reference pages across %zu spine items",
          static_cast<unsigned long>(totalLocations), static_cast<unsigned long>(totalReferencePages),
          locationSpine.size());
  return true;
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getCumulativeSpineItemSize called but cache not loaded");
    return 0;
  }

  return bookMetadataCache->getSpineCumulativeSize(spineIndex);
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

float Epub::calculateSizeProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = clampUnit(currentSpineRead) * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  if (!crossinkLocationsLoaded || totalLocations == 0 || currentSpineIndex < 0 ||
      currentSpineIndex >= static_cast<int>(locationSpine.size())) {
    return calculateSizeProgress(currentSpineIndex, currentSpineRead);
  }

  const LocationSpineEntry& entry = locationSpine[static_cast<size_t>(currentSpineIndex)];
  if (entry.startLocation == 0 || entry.endLocation < entry.startLocation) {
    return calculateSizeProgress(currentSpineIndex, currentSpineRead);
  }

  const uint32_t locationCount = entry.endLocation - entry.startLocation + 1;
  const float completedBeforeSpine = static_cast<float>(entry.startLocation - 1);
  const float completedInSpine = clampUnit(currentSpineRead) * static_cast<float>(locationCount);
  return clampUnit((completedBeforeSpine + completedInSpine) / static_cast<float>(totalLocations));
}

bool Epub::resolveLocationPercentToSpineProgress(const int percent, int& spineIndex, float& spineProgress) const {
  if (!crossinkLocationsLoaded || totalLocations == 0 || locationSpine.empty()) {
    return false;
  }

  const int clampedPercent = std::max(0, std::min(100, percent));
  if (clampedPercent <= 0) {
    spineIndex = 0;
    spineProgress = 0.0f;
    return true;
  }

  if (clampedPercent >= 100) {
    for (int i = static_cast<int>(locationSpine.size()) - 1; i >= 0; i--) {
      const LocationSpineEntry& entry = locationSpine[static_cast<size_t>(i)];
      if (entry.startLocation > 0 && entry.endLocation >= entry.startLocation) {
        spineIndex = i;
        spineProgress = 1.0f;
        return true;
      }
    }
    return false;
  }

  const float targetCompletedLocations =
      static_cast<float>(totalLocations) * static_cast<float>(clampedPercent) / 100.0f;
  for (size_t i = 0; i < locationSpine.size(); i++) {
    const LocationSpineEntry& entry = locationSpine[i];
    if (entry.startLocation == 0 || entry.endLocation < entry.startLocation) {
      continue;
    }

    const uint32_t locationCount = entry.endLocation - entry.startLocation + 1;
    const float completedBeforeSpine = static_cast<float>(entry.startLocation - 1);
    const float completedThroughSpine = static_cast<float>(entry.endLocation);
    if (targetCompletedLocations > completedThroughSpine) {
      continue;
    }

    spineIndex = static_cast<int>(i);
    spineProgress = clampUnit((targetCompletedLocations - completedBeforeSpine) / static_cast<float>(locationCount));
    return true;
  }

  return false;
}

bool Epub::resolveReferencePage(const int currentSpineIndex, const float currentSpineRead, uint32_t& currentPage,
                                uint32_t& pageCount) const {
  currentPage = 0;
  pageCount = 0;
  if (!crossinkLocationsLoaded || totalWords == 0 || wordsPerReferencePage == 0 || totalReferencePages == 0 ||
      currentSpineIndex < 0 || currentSpineIndex >= static_cast<int>(locationSpine.size())) {
    return false;
  }

  const LocationSpineEntry& entry = locationSpine[static_cast<size_t>(currentSpineIndex)];
  if (entry.wordCount == 0 || entry.wordStart >= totalWords) {
    return false;
  }

  const float clampedProgress = clampUnit(currentSpineRead);
  const uint32_t completedWords =
      entry.wordStart + static_cast<uint32_t>(clampedProgress * static_cast<float>(entry.wordCount));
  currentPage = std::min<uint32_t>(completedWords / wordsPerReferencePage + 1, totalReferencePages);
  pageCount = totalReferencePages;
  return true;
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
