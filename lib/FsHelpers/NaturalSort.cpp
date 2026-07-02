#include "NaturalSort.h"

#include <cctype>

namespace FsHelpers {

int naturalCompare(const char* s1, const char* s2) {
  while (*s1 && *s2) {
    if (isdigit(static_cast<unsigned char>(*s1)) && isdigit(static_cast<unsigned char>(*s2))) {
      while (*s1 == '0') s1++;
      while (*s2 == '0') s2++;

      int len1 = 0;
      int len2 = 0;
      while (isdigit(static_cast<unsigned char>(s1[len1]))) len1++;
      while (isdigit(static_cast<unsigned char>(s2[len2]))) len2++;

      if (len1 != len2) return len1 - len2;

      for (int i = 0; i < len1; i++) {
        if (s1[i] != s2[i]) return s1[i] - s2[i];
      }

      s1 += len1;
      s2 += len2;
    } else {
      const int c1 = tolower(static_cast<unsigned char>(*s1));
      const int c2 = tolower(static_cast<unsigned char>(*s2));
      if (c1 != c2) return c1 - c2;
      s1++;
      s2++;
    }
  }

  if (*s1 == *s2) return 0;
  return (*s1 == '\0') ? -1 : 1;
}

size_t naturalSortKey(const char* name, uint8_t* out, size_t cap) {
  static constexpr uint8_t DIGIT_RUN_MARKER = 0x30;
  size_t n = 0;
  const char* s = name;

  while (*s && n < cap) {
    if (isdigit(static_cast<unsigned char>(*s))) {
      while (*s == '0') s++;

      size_t len = 0;
      while (isdigit(static_cast<unsigned char>(s[len]))) len++;

      out[n++] = DIGIT_RUN_MARKER;
      if (n >= cap) break;

      if (len == 0) {
        out[n++] = 1;
        if (n >= cap) break;
        out[n++] = '0';
      } else {
        out[n++] = static_cast<uint8_t>(len > 255 ? 255 : len);
        for (size_t i = 0; i < len && n < cap; i++) {
          out[n++] = static_cast<uint8_t>(s[i]);
        }
      }
      s += len;
    } else {
      out[n++] = static_cast<uint8_t>(tolower(static_cast<unsigned char>(*s)));
      s++;
    }
  }

  return n;
}

SeriesNumber parseSeriesNumber(const char* name) {
  SeriesNumber result;
  if (name == nullptr || !isdigit(static_cast<unsigned char>(*name))) {
    return result;
  }

  const char* s = name;
  long start = 0;
  while (isdigit(static_cast<unsigned char>(*s))) {
    start = start * 10 + (*s - '0');
    s++;
  }

  result.numbered = true;
  result.start = start;
  result.end = start;

  if (*s == '.' && isdigit(static_cast<unsigned char>(s[1]))) {
    // Decimal novella/interstitial such as "16.5" — counts as part of book 16.
    result.decimal = true;
  } else if (*s == '-' && isdigit(static_cast<unsigned char>(s[1]))) {
    // Omnibus range such as "1-3" — covers books start..end.
    const char* r = s + 1;
    long end = 0;
    while (isdigit(static_cast<unsigned char>(*r))) {
      end = end * 10 + (*r - '0');
      r++;
    }
    if (end >= start) {
      result.end = end;
    }
  }

  return result;
}

namespace {

// Decode one UTF-8 codepoint starting at `s` (which must be a lead byte >= 0xC0).
// Writes the number of bytes consumed to `len` (>= 1) and returns the codepoint,
// or 0 on a malformed/truncated sequence (len is then set past the bad byte). (CrossInked)
uint32_t decodeCodepoint(const char* s, int& len) {
  const unsigned char c0 = static_cast<unsigned char>(s[0]);
  int expect;
  uint32_t cp;
  if ((c0 & 0xE0) == 0xC0) {
    expect = 1;
    cp = c0 & 0x1F;
  } else if ((c0 & 0xF0) == 0xE0) {
    expect = 2;
    cp = c0 & 0x0F;
  } else if ((c0 & 0xF8) == 0xF0) {
    expect = 3;
    cp = c0 & 0x07;
  } else {
    len = 1;  // 0xC0/0xC1/0xF8.. or stray continuation byte — skip it
    return 0;
  }
  for (int i = 1; i <= expect; ++i) {
    const unsigned char cc = static_cast<unsigned char>(s[i]);
    if ((cc & 0xC0) != 0x80) {  // truncated
      len = i;
      return 0;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  len = expect + 1;
  return cp;
}

// Fold an accented Latin codepoint (Latin-1 Supplement U+00C0-U+00FF and
// Latin Extended-A U+0100-U+017F) to its base ASCII letter for grouping, e.g.
// É/è->e, Ø/ø->o, š/Š->s, Ç->c. Returns 0 when there is no sensible base letter
// (ß, ÷, ×, æ ligatures etc.), letting the caller bucket it with other
// non-ASCII names. Kept as a compact range-fold, not a Unicode library. (CrossInked)
char foldLatinToAscii(uint32_t cp) {
  // Latin-1 Supplement letters.
  if (cp >= 0x00C0 && cp <= 0x00FF) {
    static const char kLatin1[64] = {
        //   C0   C1   C2   C3   C4   C5   C6   C7   C8   C9   CA   CB   CC   CD   CE   CF
        'a', 'a', 'a', 'a', 'a', 'a', 0,   'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
        //   D0   D1   D2   D3   D4   D5   D6   D7   D8   D9   DA   DB   DC   DD   DE   DF
        'd', 'n', 'o', 'o', 'o', 'o', 'o', 0,   'o', 'u', 'u', 'u', 'u', 'y', 0,   0,
        //   E0   E1   E2   E3   E4   E5   E6   E7   E8   E9   EA   EB   EC   ED   EE   EF
        'a', 'a', 'a', 'a', 'a', 'a', 0,   'c', 'e', 'e', 'e', 'e', 'i', 'i', 'i', 'i',
        //   F0   F1   F2   F3   F4   F5   F6   F7   F8   F9   FA   FB   FC   FD   FE   FF
        'd', 'n', 'o', 'o', 'o', 'o', 'o', 0,   'o', 'u', 'u', 'u', 'u', 'y', 0,   'y'};
    return kLatin1[cp - 0x00C0];
  }
  // Latin Extended-A: each base letter occupies a run of accented variants. The
  // block is laid out in ASCII-alphabetical order, so map by sub-range.
  if (cp >= 0x0100 && cp <= 0x017F) {
    if (cp <= 0x0105) return 'a';  // Ā ā Ă ă Ą ą
    if (cp <= 0x010D) return 'c';  // Ć ć Ĉ ĉ Ċ ċ Č č
    if (cp <= 0x0111) return 'd';  // Ď ď Đ đ
    if (cp <= 0x011B) return 'e';  // Ē..ě
    if (cp <= 0x0123) return 'g';  // Ĝ..ģ
    if (cp <= 0x0127) return 'h';  // Ĥ..ħ
    if (cp <= 0x0133) return 'i';  // Ĩ..ı, IJ ligature Ĳ ĳ
    if (cp <= 0x0135) return 'j';  // Ĵ ĵ
    if (cp <= 0x0137) return 'k';  // Ķ ķ
    if (cp <= 0x0142) return 'l';  // ĸ Ĺ..ł
    if (cp <= 0x0148) return 'n';  // Ń..ň
    if (cp <= 0x0151) return 'o';  // ŉ Ŋ ŋ Ō..ő
    if (cp <= 0x0153) return 'o';  // OE ligature Œ œ
    if (cp <= 0x0159) return 'r';  // Ŕ..ř
    if (cp <= 0x0161) return 's';  // Ś..š
    if (cp <= 0x0167) return 't';  // Ţ..ŧ
    if (cp <= 0x0173) return 'u';  // Ũ..ų
    if (cp <= 0x0175) return 'w';  // Ŵ ŵ
    if (cp <= 0x0178) return 'y';  // Ŷ ŷ Ÿ
    return 'z';                    // Ź..ž
  }
  return 0;
}

}  // namespace

char firstSortChar(const char* name) {
  if (name == nullptr) return 0;
  const char* s = name;
  while (*s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c < 0x80) {
      if (isdigit(c)) return '0';  // all numbered entries collapse to one group
      if (isalpha(c)) return static_cast<char>(tolower(c));
      ++s;  // ASCII punctuation/space: skip to the first real sort char
      continue;
    }
    // Multibyte lead: decode and try to fold an accented Latin letter to its base.
    int len = 1;
    const uint32_t cp = decodeCodepoint(s, len);
    if (cp != 0) {
      const char base = foldLatinToAscii(cp);
      if (base != 0) return base;
      // Any other decodable non-ASCII (CJK, Cyrillic, ß, ligatures...) forms one
      // stable bucket so such names cluster together rather than under a garbage
      // ASCII letter picked from later in the name. (CrossInked)
      return NON_ASCII_GROUP;
    }
    s += len;  // malformed sequence: skip past it and keep looking
  }
  return 0;
}

}  // namespace FsHelpers
