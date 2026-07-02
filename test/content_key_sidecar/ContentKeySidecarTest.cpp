#include <gtest/gtest.h>

#include "Epub/ContentKeySidecar.h"

using content_key_sidecar::Info;
using content_key_sidecar::parse;
using content_key_sidecar::serialize;

// --- Happy path: full 3-line sidecar (key, path, size) ---

TEST(ContentKeySidecar, ParsesKeyPathAndSize) {
  Info info;
  ASSERT_TRUE(parse("12345678901234567890\n/books/Author/Title.epub\n524288\n", info));
  EXPECT_EQ(info.key, 12345678901234567890ull);
  EXPECT_EQ(info.sourcePath, "/books/Author/Title.epub");
  EXPECT_EQ(info.sourceSize, 524288u);
}

TEST(ContentKeySidecar, RoundTripsThroughSerialize) {
  const std::string body = serialize(9999u, "/books/A - B.epub", 4096u);
  Info info;
  ASSERT_TRUE(parse(body, info));
  EXPECT_EQ(info.key, 9999u);
  EXPECT_EQ(info.sourcePath, "/books/A - B.epub");
  EXPECT_EQ(info.sourceSize, 4096u);
}

// --- Legacy sidecars (F5): missing size line reads as size 0, still adoptable ---

TEST(ContentKeySidecar, LegacyTwoLineSidecarHasZeroSize) {
  Info info;
  ASSERT_TRUE(parse("42\n/books/Old.epub\n", info));
  EXPECT_EQ(info.key, 42u);
  EXPECT_EQ(info.sourcePath, "/books/Old.epub");
  EXPECT_EQ(info.sourceSize, 0u);  // 0 == legacy -> caller allows adoption with a log
}

TEST(ContentKeySidecar, LegacySidecarWithoutTrailingNewline) {
  Info info;
  ASSERT_TRUE(parse("42\n/books/Old.epub", info));
  EXPECT_EQ(info.sourcePath, "/books/Old.epub");
  EXPECT_EQ(info.sourceSize, 0u);
}

TEST(ContentKeySidecar, StripsCarriageReturnsFromPath) {
  Info info;
  ASSERT_TRUE(parse("7\r\n/books/CRLF.epub\r\n1024\r\n", info));
  EXPECT_EQ(info.sourcePath, "/books/CRLF.epub");
  EXPECT_EQ(info.sourceSize, 1024u);
}

// --- Robustness: a torn write must never defeat the anti-steal guard (F12) ---

TEST(ContentKeySidecar, RejectsEmptyOrKeyOnly) {
  Info info;
  EXPECT_FALSE(parse("", info));               // empty
  EXPECT_FALSE(parse("12345", info));          // key line with no newline
}

TEST(ContentKeySidecar, RejectsZeroAndGarbageKey) {
  Info info;
  // strtoull yields 0 on both an explicit 0 and non-numeric garbage; a stored
  // key of 0 must never match a real content key, so parse rejects it.
  EXPECT_FALSE(parse("0\n/books/X.epub\n10\n", info));
  EXPECT_FALSE(parse("not-a-number\n/books/X.epub\n10\n", info));
}

TEST(ContentKeySidecar, TornPathLineStillYieldsSomePathButZeroSize) {
  // "key line intact, path line truncated" is the dangerous torn-write shape.
  // parse() succeeds (key valid) and returns the partial path with size 0; the
  // caller's exists()/live-original guard then decides -- crucially the size is
  // NOT fabricated from the truncated bytes.
  Info info;
  ASSERT_TRUE(parse("555\n/books/Trunc", info));
  EXPECT_EQ(info.key, 555u);
  EXPECT_EQ(info.sourcePath, "/books/Trunc");
  EXPECT_EQ(info.sourceSize, 0u);
}

// --- Size-field validation: non-numeric size reads as legacy, not garbage ---

TEST(ContentKeySidecar, NonNumericSizeReadsAsLegacyZero) {
  Info info;
  ASSERT_TRUE(parse("8\n/books/X.epub\nNaN\n", info));
  EXPECT_EQ(info.sourceSize, 0u);  // not all-digits -> treated as absent
}

TEST(ContentKeySidecar, SizeWithTrailingSpaceParses) {
  Info info;
  ASSERT_TRUE(parse("8\n/books/X.epub\n2048 \n", info));
  EXPECT_EQ(info.sourceSize, 2048u);
}

TEST(ContentKeySidecar, ExtraTrailingLinesAreIgnored) {
  Info info;
  ASSERT_TRUE(parse("8\n/books/X.epub\n64\nunexpected extra\n", info));
  EXPECT_EQ(info.sourceSize, 64u);
}

// --- Paths with spaces (the user's "Title - Author.epub" convention) ---

TEST(ContentKeySidecar, PreservesSpacesInPath) {
  Info info;
  ASSERT_TRUE(parse("1\n/books/Bernard Cornwell/12 Sharpe's Battle - Bernard Cornwell.epub\n700000\n", info));
  EXPECT_EQ(info.sourcePath, "/books/Bernard Cornwell/12 Sharpe's Battle - Bernard Cornwell.epub");
  EXPECT_EQ(info.sourceSize, 700000u);
}
