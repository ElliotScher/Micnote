#include "wer.hpp"

#include <algorithm>
#include <cctype>

namespace micnote::bench {

namespace {

bool Alnum(char c) {
  // Bytes of a multi-byte UTF-8 letter count as letters.
  const auto u = static_cast<unsigned char>(c);
  return u >= 0x80U || std::isalnum(u) != 0;
}

void ReplaceAll(std::string &s, std::string_view from, std::string_view to) {
  for (auto at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
    s.replace(at, from.size(), to);
  }
}

// Drops bracketed notes; an opening bracket that never closes is kept.
void DropNotes(std::string &s, char open, char close) {
  for (auto at = s.find(open); at != std::string::npos; at = s.find(open, at)) {
    const auto end = s.find(close, at);
    if (end == std::string::npos) {
      return;
    }
    s.replace(at, end - at + 1, " ");
  }
}

}  // namespace

// UTF-8 bytes rather than \u escapes, which MSVC would put in its own charset.
std::vector<std::string> Words(std::string_view text) {
  std::string s(text);
  ReplaceAll(s, "’", "'");
  ReplaceAll(s, "‘", "'");
  ReplaceAll(s, "“", " ");
  ReplaceAll(s, "”", " ");
  ReplaceAll(s, "–", " ");
  ReplaceAll(s, "—", " ");
  ReplaceAll(s, "…", " ");
  DropNotes(s, '[', ']');
  DropNotes(s, '(', ')');
  std::vector<std::string> out;
  std::string word;
  const auto flush = [&] {
    if (!word.empty()) {
      out.push_back(std::move(word));
      word.clear();
    }
  };
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (Alnum(c)) {
      word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else if (c == '\'' && !word.empty() && i + 1 < s.size() && Alnum(s[i + 1])) {
      word += c;
    } else if (c == '-' || std::isspace(static_cast<unsigned char>(c)) != 0) {
      flush();
    }
  }
  flush();
  return out;
}

Wer &Wer::operator+=(const Wer &o) {
  errors += o.errors;
  words += o.words;
  return *this;
}

double Wer::Rate() const {
  return words == 0 ? 0.0 : static_cast<double>(errors) / static_cast<double>(words);
}

Wer Compare(std::string_view reference, std::string_view hypothesis) {
  const auto ref = Words(reference);
  const auto hyp = Words(hypothesis);
  // Levenshtein distance over words, one row at a time.
  std::vector<std::size_t> row(hyp.size() + 1);
  for (std::size_t h = 0; h < row.size(); ++h) {
    row[h] = h;
  }
  for (std::size_t r = 1; r <= ref.size(); ++r) {
    std::size_t diagonal = row[0];
    row[0] = r;
    for (std::size_t h = 1; h <= hyp.size(); ++h) {
      const std::size_t match = diagonal + (ref[r - 1] == hyp[h - 1] ? 0U : 1U);
      diagonal = row[h];
      row[h] = std::min({match, row[h] + 1, row[h - 1] + 1});
    }
  }
  return Wer{.errors = row.back(), .words = ref.size()};
}

std::vector<std::size_t> Spread(std::size_t total, std::size_t count) {
  const std::size_t n = count == 0 ? total : std::min(count, total);
  std::vector<std::size_t> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = i * total / n;
  }
  return out;
}

}  // namespace micnote::bench
