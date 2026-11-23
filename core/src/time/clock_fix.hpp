// Timestamp repair (stage 2 §4.4). A recording made while the puck's clock
// was unset has start_unix_ms = 0 but a (boot, uptime) start. Any later
// observation from the same boot, (boot, uptime) <-> unix_ms, pins it:
//   fixed = obs.unix_ms - (obs.uptime_ms - start_uptime_ms)
// The observation nearest in uptime is used (the least clock drift).
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "store/repos.hpp"

namespace micnote::time {

struct Repair {
  std::optional<std::int64_t> fixed_unix_ms;
  store::TimeConfidence confidence = store::TimeConfidence::kUnknown;
};

// observations: from the recording's boot (any order).
Repair RepairStart(std::int64_t start_unix_ms, std::int64_t start_boot,
                   std::int64_t start_uptime_ms,
                   std::span<const store::ClockObservation> observations);

}  // namespace micnote::time
