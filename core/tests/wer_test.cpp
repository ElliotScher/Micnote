// The accuracy harness's scoring (core/bench/wer.*).
#include "wer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using micnote::bench::Compare;
using micnote::bench::Spread;
using micnote::bench::Wer;
using micnote::bench::Words;
using V = std::vector<std::string>;

TEST(Wer, WordsIgnoreCaseAndPunctuation) {
  EXPECT_EQ(Words("Ask NOT, what your country..."), (V{"ask", "not", "what", "your", "country"}));
  EXPECT_EQ(Words("don't 'quote' the dogs'"), (V{"don't", "quote", "the", "dogs"}));
  EXPECT_EQ(Words("don\xe2\x80\x99t \xe2\x80\x9cwell\xe2\x80\x94"
                  "known\xe2\x80\x9d x-ray"),
            (V{"don't", "well", "known", "x", "ray"}));
  EXPECT_EQ(Words("[MUSIC] hi (laughs) there"), (V{"hi", "there"}));
  EXPECT_EQ(Words("an [open bracket"), (V{"an", "open", "bracket"}));
  EXPECT_EQ(Words("caf\xc3\xa9 20"), (V{"caf\xc3\xa9", "20"}));
  EXPECT_TRUE(Words(" ... ").empty());
}

TEST(Wer, CountsEveryKindOfEdit) {
  const auto errors = [](const char *ref, const char *hyp) { return Compare(ref, hyp).errors; };
  EXPECT_EQ(errors("a b c", "A, b c."), 0U);
  EXPECT_EQ(errors("a b c", "a x c"), 1U) << "substitution";
  EXPECT_EQ(errors("a b c", "a c"), 1U) << "deletion";
  EXPECT_EQ(errors("a b c", "a b b c"), 1U) << "insertion";
  EXPECT_EQ(errors("a b c", ""), 3U);
  EXPECT_EQ(errors("", "a b"), 2U);
  EXPECT_EQ(errors("the cat sat on the mat", "a cat sat the mat now"), 3U);
}

TEST(Wer, RatesPoolOverClips) {
  Wer total = Compare("a b c d", "a b c x");
  EXPECT_DOUBLE_EQ(total.Rate(), 0.25);
  total += Compare("a b c d e f", "a b c d e f");
  EXPECT_DOUBLE_EQ(total.Rate(), 0.1) << "by words, not an average of clips";
  EXPECT_DOUBLE_EQ(Compare("", "").Rate(), 0.0);
  EXPECT_DOUBLE_EQ(Compare("a", "x y").Rate(), 2.0);
}

TEST(Wer, SpreadIsEvenAndRepeatable) {
  EXPECT_EQ(Spread(10, 5), (std::vector<std::size_t>{0, 2, 4, 6, 8}));
  EXPECT_EQ(Spread(3, 0), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Spread(3, 9), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_TRUE(Spread(0, 5).empty());
}

}  // namespace
