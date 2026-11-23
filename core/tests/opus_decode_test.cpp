// Decoding the test vectors' Opus (the firmware's encoder) back to PCM.
#include "audio/opus_decode.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <vector>

#include "fake_puck.hpp"
#include "recording/reader.hpp"

namespace {

using micnote::audio::DecodeError;
using micnote::audio::DecodeOpus;
using micnote::audio::DecodeStats;

TEST(OpusDecode, EveryVectorDecodesToItsDuration) {
  const auto vectors = micnote::test::Vectors();
  for (const auto &v : vectors["recordings"]) {
    const std::string file = v["file"].get<std::string>();
    SCOPED_TRACE(file);
    const auto rec = micnote::recording::ReadRecording(
        micnote::test::ReadFile(file),
        micnote::test::KeyNamed(v["recipients"][0].get<std::string>()));
    ASSERT_TRUE(rec.has_value());
    DecodeStats stats;
    const auto pcm = DecodeOpus(rec->packets, &stats);
    ASSERT_TRUE(pcm.has_value());
    // 20 ms packets: 320 samples each at 16 kHz.
    EXPECT_EQ(pcm->size(), rec->packets.size() * 320U);
    EXPECT_EQ(stats.packets, rec->packets.size());
    EXPECT_EQ(stats.samples, pcm->size());
    double energy = 0;
    for (const float s : *pcm) {
      ASSERT_TRUE(std::isfinite(s));
      ASSERT_LE(std::fabs(s), 1.5F);
      energy += static_cast<double>(s) * static_cast<double>(s);
    }
    EXPECT_GT(energy / static_cast<double>(pcm->size()), 1e-5) << "the voice came back as silence";
  }
}

TEST(OpusDecode, RejectsAPacketLibopusCantParse) {
  // TOC code 3 with a frame count of 0 is invalid.
  const std::vector<std::vector<std::byte>> bad = {{std::byte{0x4B}, std::byte{0x00}}};
  EXPECT_EQ(DecodeOpus(bad).error(), DecodeError::kBadPacket);
  EXPECT_TRUE(DecodeOpus({}).value().empty());
}

}  // namespace
