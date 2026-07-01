#include <gtest/gtest.h>

#include "NaturalSort.h"

using namespace FsHelpers;

// --- naturalCompare: numeric-aware ordering the whole library relies on ---

TEST(NaturalCompare, NumbersOrderByValueNotLexically) {
  EXPECT_LT(naturalCompare("4 A", "10 A"), 0);  // 4 < 10 (lexical would say "10" < "4")
  EXPECT_LT(naturalCompare("2 A", "10 A"), 0);
  EXPECT_GT(naturalCompare("10 A", "9 A"), 0);
  EXPECT_EQ(naturalCompare("7 A", "7 A"), 0);
}

TEST(NaturalCompare, OmnibusAndNovellaSlotCorrectly) {
  // 1 < 1-3 < 2   (space 0x20 < '-' 0x2D)
  EXPECT_LT(naturalCompare("1 X", "1-3 X"), 0);
  EXPECT_LT(naturalCompare("1-3 X", "2 X"), 0);
  // 16 < 16.5 < 17   (space 0x20 < '.' 0x2E)
  EXPECT_LT(naturalCompare("16 X", "16.5 X"), 0);
  EXPECT_LT(naturalCompare("16.5 X", "17 X"), 0);
}

TEST(NaturalCompare, LeadingZerosIgnored) {
  EXPECT_EQ(naturalCompare("01 A", "1 A"), 0);
  EXPECT_LT(naturalCompare("09 A", "10 A"), 0);
}

// --- parseSeriesNumber: the series-gap building block ---

TEST(ParseSeriesNumber, PlainInteger) {
  const auto s = parseSeriesNumber("4 Sharpe's Trafalgar - Bernard Cornwell.epub");
  EXPECT_TRUE(s.numbered);
  EXPECT_FALSE(s.decimal);
  EXPECT_EQ(s.start, 4);
  EXPECT_EQ(s.end, 4);
}

TEST(ParseSeriesNumber, MultiDigit) {
  const auto s = parseSeriesNumber("14 Sharpe's Command - Bernard Cornwell.epub");
  EXPECT_TRUE(s.numbered);
  EXPECT_EQ(s.start, 14);
  EXPECT_EQ(s.end, 14);
}

TEST(ParseSeriesNumber, DecimalNovella) {
  const auto s = parseSeriesNumber("16.5 Sharpe's Ransom - Bernard Cornwell.epub");
  EXPECT_TRUE(s.numbered);
  EXPECT_TRUE(s.decimal);
  EXPECT_EQ(s.start, 16);
  EXPECT_EQ(s.end, 16);
}

TEST(ParseSeriesNumber, OmnibusRange) {
  const auto s = parseSeriesNumber("1-3 First Law Trilogy Omnibus - Joe Abercrombie.epub");
  EXPECT_TRUE(s.numbered);
  EXPECT_EQ(s.start, 1);
  EXPECT_EQ(s.end, 3);
}

TEST(ParseSeriesNumber, Unnumbered) {
  EXPECT_FALSE(parseSeriesNumber("Norwegian Wood - Haruki Murakami.epub").numbered);
  EXPECT_FALSE(parseSeriesNumber(nullptr).numbered);
  EXPECT_FALSE(parseSeriesNumber("").numbered);
}

// A title that merely starts with a digit but has no separator still parses its
// leading digits (acceptable: gap logic only triggers between two numbered files).
TEST(ParseSeriesNumber, BareLeadingDigits) {
  const auto s = parseSeriesNumber("1984 - George Orwell.epub");
  EXPECT_TRUE(s.numbered);
  EXPECT_EQ(s.start, 1984);
}

// --- Series-gap semantics: mirrors the browser marker logic (b.start > a.end + 1) ---

static bool hasGap(const char* prev, const char* cur) {
  const auto a = parseSeriesNumber(prev);
  const auto b = parseSeriesNumber(cur);
  return a.numbered && b.numbered && b.start > a.end + 1;
}

TEST(SeriesGap, FlagsRealGaps) {
  EXPECT_TRUE(hasGap("6 A", "8 A"));    // missing 7
  EXPECT_TRUE(hasGap("12 A", "15 A"));  // missing 13-14
  EXPECT_TRUE(hasGap("1-3 A", "6 A"));  // omnibus covers 1-3, then 6: missing 4-5
}

TEST(SeriesGap, DoesNotFlagFalsePositives) {
  EXPECT_FALSE(hasGap("6 A", "7 A"));      // consecutive
  EXPECT_FALSE(hasGap("16 A", "16.5 A"));  // novella after 16
  EXPECT_FALSE(hasGap("16.5 A", "17 A"));  // next book after novella
  EXPECT_FALSE(hasGap("1-3 A", "4 A"));    // omnibus then 4: contiguous
  EXPECT_FALSE(hasGap("A", "8 A"));        // previous entry unnumbered
  EXPECT_FALSE(hasGap("8 A", "B"));        // current entry unnumbered
}
