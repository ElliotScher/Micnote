#include "time/clock_fix.hpp"

#include <cstdlib>

namespace micnote::time {

Repair RepairStart(std::int64_t start_unix_ms, std::int64_t start_boot,
                   std::int64_t start_uptime_ms,
                   std::span<const store::ClockObservation> observations) {
  if (start_unix_ms != 0) {
    return Repair{.fixed_unix_ms = start_unix_ms, .confidence = store::TimeConfidence::kRtc};
  }
  const store::ClockObservation *best = nullptr;
  for (const auto &o : observations) {
    if (o.boot_count != start_boot || o.unix_ms == 0) {
      continue;
    }
    if (best == nullptr ||
        std::llabs(o.uptime_ms - start_uptime_ms) < std::llabs(best->uptime_ms - start_uptime_ms)) {
      best = &o;
    }
  }
  if (best == nullptr) {
    return Repair{.fixed_unix_ms = std::nullopt, .confidence = store::TimeConfidence::kUnknown};
  }
  return Repair{.fixed_unix_ms = best->unix_ms - (best->uptime_ms - start_uptime_ms),
                .confidence = store::TimeConfidence::kRepaired};
}

}  // namespace micnote::time
