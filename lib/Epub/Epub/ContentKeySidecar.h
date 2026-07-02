#pragma once

// Pure, dependency-free parser/serializer for the content.key sidecar written
// by the cache-adoption feature (CrossInked). Kept separate from Epub.cpp so the
// parsing rules -- which guard reading progress against edition swaps and torn
// writes -- can be unit-tested on the host without the SD/Arduino HAL.
//
// Sidecar format (line-based, backward-tolerant):
//   line 1: fnvHash64(title \x1f author)          (decimal)
//   line 2: source EPUB path
//   line 3 (optional, F5): source EPUB file size in bytes (decimal)
// A missing or non-numeric size line yields sourceSize == 0 ("legacy" sidecar).

#include <cstdint>
#include <cstdlib>
#include <string>

namespace content_key_sidecar {

struct Info {
  uint64_t key = 0;
  std::string sourcePath;
  uint64_t sourceSize = 0;  // 0 == absent/legacy
};

// Parses raw sidecar file contents. Returns false when the content is malformed
// (no key line) or the key is 0 -- strtoull yields 0 on garbage, and a stored 0
// must never match a real content key (F12).
inline bool parse(const std::string& content, Info& out) {
  const size_t nl = content.find('\n');
  if (nl == std::string::npos) return false;
  const uint64_t storedKey = strtoull(content.substr(0, nl).c_str(), nullptr, 10);
  if (storedKey == 0) return false;

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

// Serializes the 3-line sidecar body (with trailing newline on each line).
inline std::string serialize(uint64_t key, const std::string& sourcePath, uint64_t sourceSize) {
  return std::to_string(key) + "\n" + sourcePath + "\n" + std::to_string(sourceSize) + "\n";
}

}  // namespace content_key_sidecar
