// Word error rate, for the accuracy harness (stage 2 PR 5): the edits
// (substitutions, deletions, insertions) that turn a transcript into its
// reference, over the reference's word count.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace micnote::bench {

// The words to compare: lowercase, ASCII punctuation dropped (an apostrophe
// between letters stays; typographic ones count as one), hyphens and dashes
// as spaces, and whisper's non-speech notes ("[MUSIC]", "(laughs)") removed.
// Numbers aren't spelled out, so "20" against "twenty" counts as an error.
std::vector<std::string> Words(std::string_view text);

struct Wer {
  std::size_t errors = 0;
  std::size_t words = 0;  // in the reference

  Wer &operator+=(const Wer &o);
  // 0 with nothing to compare; above 1 if there are more errors than words.
  [[nodiscard]] double Rate() const;
};

Wer Compare(std::string_view reference, std::string_view hypothesis);

// `count` of `total` indexes, evenly spread and always the same (all of them
// if count is 0 or more than there are).
std::vector<std::size_t> Spread(std::size_t total, std::size_t count);

}  // namespace micnote::bench
