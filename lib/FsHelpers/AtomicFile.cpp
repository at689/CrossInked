#include "AtomicFile.h"

#include <HalStorage.h>
#include <Logging.h>

// Crash-consistent whole-file write. See AtomicFile.h. (CrossInked)
//
// Mirrors EpubReaderUtils::saveProgress: write a .tmp sibling, flush+sync so the
// bytes are on the SD before we touch the live file, rotate the existing file to
// .bak, then rename the temp over the destination. Every failure path removes the
// temp and, where possible, restores the original so a torn write can never leave
// the destination missing when it previously existed.
namespace FsHelpers {

bool writeAtomically(const char* path, std::string_view content, const char* moduleName) {
  if (path == nullptr || path[0] == '\0') {
    LOG_ERR(moduleName, "writeAtomically: empty path");
    return false;
  }

  const std::string destPath = path;
  const std::string tmpPath = destPath + ".tmp";
  const std::string backupPath = destPath + ".bak";

  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR(moduleName, "Could not remove stale temp file: %s", tmpPath.c_str());
    return false;
  }

  FsFile f;
  if (!Storage.openFileForWrite(moduleName, tmpPath, f)) {
    LOG_ERR(moduleName, "Could not open temp file for write: %s", tmpPath.c_str());
    return false;
  }

  bool writeOk = true;
  if (!content.empty()) {
    const size_t written = f.write(content.data(), content.size());
    if (written != content.size()) {
      LOG_ERR(moduleName, "Short write to %s: %u/%u bytes", tmpPath.c_str(), static_cast<unsigned>(written),
              static_cast<unsigned>(content.size()));
      writeOk = false;
    }
  }

  if (writeOk) {
    f.flush();
    if (!f.sync()) {
      LOG_ERR(moduleName, "Failed to sync temp file: %s", tmpPath.c_str());
      writeOk = false;
    }
  }

  const bool closeOk = f.close();
  if (!closeOk) {
    LOG_ERR(moduleName, "Failed to close temp file: %s", tmpPath.c_str());
  }
  if (!writeOk || !closeOk) {
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR(moduleName, "Could not remove old backup: %s", backupPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  // Rotate the live file aside so a crash between the two renames still leaves a
  // recoverable copy at <path>.bak. Missing destination (first write) is fine.
  if (Storage.exists(destPath.c_str()) && !Storage.rename(destPath.c_str(), backupPath.c_str())) {
    LOG_ERR(moduleName, "Could not rotate backup for: %s", destPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), destPath.c_str())) {
    LOG_ERR(moduleName, "Could not move temp into place: %s", destPath.c_str());
    // Put the original back so we don't leave the destination missing.
    if (Storage.exists(backupPath.c_str()) && !Storage.exists(destPath.c_str())) {
      Storage.rename(backupPath.c_str(), destPath.c_str());
    }
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

}  // namespace FsHelpers
