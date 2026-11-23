// The engine-independent transcription logic, with a fake engine that
// reports one segment per second of audio it's given.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "asr/transcribe.hpp"

namespace {

using micnote::asr::AsrEngine;
using micnote::asr::AsrError;
using micnote::asr::AsrOptions;
using micnote::asr::AsrSegment;
using micnote::asr::SliceConfig;
using micnote::asr::Transcribe;

class FakeEngine final : public AsrEngine {
 public:
  std::expected<std::vector<AsrSegment>, AsrError> Run(std::span<const float> pcm,
                                                       const AsrOptions &options) override {
    calls.push_back(pcm.size());
    last_prompt = options.initial_prompt;
    if (std::cmp_equal(fail_on_call, calls.size())) {
      return std::unexpected(AsrError::kFailed);
    }
    // Each slice's audio is a ramp of absolute sample indexes, so a segment
    // can name the second of the recording it covers.
    std::vector<AsrSegment> out;
    for (std::size_t s = 0; s + 16000 <= pcm.size(); s += 16000) {
      const auto second = std::lround(pcm[s] / 16000.0F);
      out.push_back(AsrSegment{.start_ms = static_cast<std::int64_t>(s / 16),
                               .end_ms = static_cast<std::int64_t>((s + 16000) / 16),
                               .text = "s" + std::to_string(second),
                               .avg_logprob = -0.1,
                               .no_speech_prob = 0.0});
    }
    return out;
  }
  [[nodiscard]] std::string Model() const override { return "fake"; }
  [[nodiscard]] std::string Backend() const override { return "none"; }

  std::vector<std::size_t> calls;
  std::string last_prompt;
  int fail_on_call = 0;
};

std::vector<float> Ramp(std::size_t seconds) {
  std::vector<float> pcm(seconds * 16000);
  for (std::size_t i = 0; i < pcm.size(); ++i) {
    pcm[i] = static_cast<float>(i);
  }
  return pcm;
}

TEST(Asr, ShortAudioIsOneCall) {
  FakeEngine e;
  const auto pcm = Ramp(5);
  AsrOptions opt;
  opt.initial_prompt = "Jeff, micnote";
  const auto segs = Transcribe(e, pcm, opt).value();
  EXPECT_EQ(e.calls.size(), 1U);
  EXPECT_EQ(e.last_prompt, "Jeff, micnote");
  ASSERT_EQ(segs.size(), 5U);
  EXPECT_EQ(segs[4].start_ms, 4000);
  EXPECT_EQ(e.Model(), "fake");
  EXPECT_EQ(e.Backend(), "none");
}

TEST(Asr, LongAudioIsSlicedWithoutDuplicatesOrGaps) {
  FakeEngine e;
  const auto pcm = Ramp(25);
  // 10 s slices, 4 s overlap: slices at 0, 6, 12, 18 s.
  const auto segs =
      Transcribe(e, pcm, AsrOptions{}, {}, SliceConfig{.slice_ms = 10000, .overlap_ms = 4000})
          .value();
  EXPECT_EQ(e.calls.size(), 4U);
  ASSERT_EQ(segs.size(), 25U) << "one per second, each exactly once";
  for (std::size_t i = 0; i < segs.size(); ++i) {
    EXPECT_EQ(segs[i].start_ms, static_cast<std::int64_t>(i) * 1000) << i;
    EXPECT_EQ(segs[i].text, "s" + std::to_string(i)) << "recording time, not slice time";
  }
}

TEST(Asr, ProgressAndCancel) {
  FakeEngine e;
  const auto pcm = Ramp(25);
  std::vector<int> pct;
  const SliceConfig small{.slice_ms = 10000, .overlap_ms = 4000};
  ASSERT_TRUE(Transcribe(
      e, pcm, AsrOptions{},
      [&](int p) {
        pct.push_back(p);
        return true;
      },
      small));
  EXPECT_EQ(pct.back(), 100);
  EXPECT_TRUE(std::ranges::is_sorted(pct));

  FakeEngine e2;
  const auto cancelled = Transcribe(e2, pcm, AsrOptions{}, [](int) { return false; }, small);
  EXPECT_EQ(cancelled.error(), AsrError::kCancelled);
  EXPECT_EQ(e2.calls.size(), 1U);
}

TEST(Asr, EngineFailureStops) {
  FakeEngine e;
  e.fail_on_call = 2;
  const auto pcm = Ramp(25);
  EXPECT_EQ(Transcribe(e, pcm, AsrOptions{}, {}, SliceConfig{.slice_ms = 10000, .overlap_ms = 2000})
                .error(),
            AsrError::kFailed);
}

TEST(Asr, EmptyAudioStillAsksTheEngineOnce) {
  FakeEngine e;
  std::vector<int> pct;
  const auto segs = Transcribe(e, {}, AsrOptions{}, [&](int p) {
    pct.push_back(p);
    return true;
  });
  ASSERT_TRUE(segs);
  EXPECT_TRUE(segs->empty());
  EXPECT_EQ(e.calls.size(), 1U);
  EXPECT_EQ(pct, std::vector<int>{100});
}

}  // namespace
