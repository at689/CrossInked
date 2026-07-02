#pragma once

#include <WString.h>

#include <string>
#include <string_view>

// Crash-consistent whole-file write for the small JSON/config stores. (CrossInked)
//
// Storage.writeFile() opens with O_WRONLY|O_CREAT|O_TRUNC and writes in place, so
// a power loss part-way through leaves the destination truncated/garbled. On a
// battery e-reader that dies at 0% this is a realistic event, and for state.json
// it silently resets ALL app state (open book, sleep prefs, lastBrowsePath).
//
// writeAtomically() instead writes "<path>.tmp", flushes+syncs it, rotates the old
// file aside to "<path>.bak", then renames the temp into place — so the destination
// is only ever replaced by a fully-written file. Modeled on the in-repo precedent
// EpubReaderUtils::saveProgress. Returns true only when <path> now holds the new
// content.
namespace FsHelpers {

bool writeAtomically(const char* path, std::string_view content, const char* moduleName = "AtomicFile");

inline bool writeAtomically(const char* path, const String& content, const char* moduleName = "AtomicFile") {
  return writeAtomically(path, std::string_view{content.c_str(), content.length()}, moduleName);
}

inline bool writeAtomically(const std::string& path, std::string_view content,
                            const char* moduleName = "AtomicFile") {
  return writeAtomically(path.c_str(), content, moduleName);
}

}  // namespace FsHelpers
