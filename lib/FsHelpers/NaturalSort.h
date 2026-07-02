#pragma once

#include <cstddef>
#include <cstdint>

namespace FsHelpers {

int naturalCompare(const char* s1, const char* s2);
size_t naturalSortKey(const char* name, uint8_t* out, size_t cap);

// Parsed leading series number of a filename, used for series-gap detection.
// Handles "4 Title" (start=end=4), "16.5 Title" (start=end=16, decimal=true),
// and "1-3 Title" omnibus ranges (start=1, end=3). `numbered` is false when the
// name does not begin with a digit.
struct SeriesNumber {
  bool numbered = false;
  bool decimal = false;  // interstitial/novella like "16.5"
  long start = 0;        // leading integer
  long end = 0;          // range end (== start unless "N-M" omnibus)
};

// Parse the leading series number from a book filename. Only digits that are
// followed by a separator (space, '.', '-') or end-of-string are treated as a
// number, so titles that merely start with a digit word still parse their prefix.
SeriesNumber parseSeriesNumber(const char* name);

// Group key returned by firstSortChar for names whose first sort-relevant
// codepoint is non-ASCII and has no Latin base letter (CJK, Cyrillic, ß, etc.).
// 0x7F sorts after every ASCII letter, so these names form one stable trailing
// group instead of scattering under arbitrary letters. (CrossInked)
constexpr char NON_ASCII_GROUP = 0x7F;

// First sort-relevant character of a name, lowercased, for jump-to-letter-group
// navigation. Decodes the first UTF-8 codepoint (not raw bytes): the first ASCII
// letter (lowercased) or digit ('0', so all numbered entries form one group);
// accented Latin letters fold to their base letter (É->e, Ø->o, š->s); any other
// non-ASCII codepoint returns NON_ASCII_GROUP. Leading punctuation/space is
// skipped. Returns 0 for names with no sort-relevant char. (CrossInked)
char firstSortChar(const char* name);

}  // namespace FsHelpers
