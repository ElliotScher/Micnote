// Timestamp repair (stage 2 §4.4).
#include "time/clock_fix.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using micnote::store::ClockObservation;
using micnote::store::TimeConfidence;
using micnote::time::RepairStart;

TEST(ClockFix, TrustsAReportedTime) {
  const auto r = RepairStart(1763800000000, 3, 5000, {});
  EXPECT_EQ(r.confidence, TimeConfidence::kRtc);
  EXPECT_EQ(r.fixed_unix_ms, 1763800000000);
}

TEST(ClockFix, RepairsFromTheNearestObservationOfTheSameBoot) {
  const std::vector<ClockObservation> obs = {
      {.boot_count = 3, .uptime_ms = 900000, .unix_ms = 1763800900000, .observed_at = 0},
      {.boot_count = 3, .uptime_ms = 60000, .unix_ms = 1763800060500, .observed_at = 0},
      {.boot_count = 4, .uptime_ms = 5000, .unix_ms = 1, .observed_at = 0},  // another boot
      {.boot_count = 3, .uptime_ms = 5000, .unix_ms = 0, .observed_at = 0},  // no clock yet
      {.boot_count = 3, .uptime_ms = 2000000, .unix_ms = 1763802000000, .observed_at = 0}};
  const auto r = RepairStart(0, 3, 5000, obs);
  EXPECT_EQ(r.confidence, TimeConfidence::kRepaired);
  // From the 60 s observation: 1763800060500 - (60000 - 5000).
  EXPECT_EQ(r.fixed_unix_ms, 1763800005500);
}

TEST(ClockFix, UnknownWithoutAnObservationOfThatBoot) {
  const std::vector<ClockObservation> obs = {
      {.boot_count = 2, .uptime_ms = 1, .unix_ms = 1763800000000, .observed_at = 0}};
  const auto r = RepairStart(0, 3, 5000, obs);
  EXPECT_EQ(r.confidence, TimeConfidence::kUnknown);
  EXPECT_EQ(r.fixed_unix_ms, std::nullopt);
}

}  // namespace
