#include "asr/transcribe.hpp"

#include <algorithm>

namespace micnote::asr {

namespace {

constexpr std::int64_t kSamplesPerMs = 16;

}  // namespace

std::expected<std::vector<AsrSegment>, AsrError> Transcribe(AsrEngine &engine,
                                                            std::span<const float> pcm16k,
                                                            const AsrOptions &options,
                                                            const Progress &progress,
                                                            const SliceConfig &slices) {
  const auto total = static_cast<std::int64_t>(pcm16k.size());
  const std::int64_t slice = slices.slice_ms * kSamplesPerMs;
  const std::int64_t overlap = std::min(slices.overlap_ms * kSamplesPerMs, slice / 2);
  const std::int64_t step = slice - overlap;
  std::vector<AsrSegment> out;

  // Ends at the slice that reaches the end of the audio (the first one, for
  // empty audio, which the engine still sees once).
  for (std::int64_t begin = 0;; begin += step) {
    const std::int64_t end = std::min(begin + slice, total);
    const bool first = begin == 0;
    const bool last = end == total;
    auto got = engine.Run(
        pcm16k.subspan(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin)),
        options);
    if (!got) {
      return std::unexpected(got.error());
    }
    // Overlaps are split down the middle: each segment is kept from the one
    // slice whose half of the overlap it starts in.
    const std::int64_t offset_ms = begin / kSamplesPerMs;
    const std::int64_t keep_from_ms = first ? 0 : (overlap / 2) / kSamplesPerMs;
    const std::int64_t keep_until_ms =
        last ? INT64_MAX : (end - begin - (overlap / 2)) / kSamplesPerMs;
    for (AsrSegment &s : *got) {
      if (s.start_ms >= keep_from_ms && s.start_ms < keep_until_ms) {
        s.start_ms += offset_ms;
        s.end_ms += offset_ms;
        out.push_back(std::move(s));
      }
    }
    if (progress && !progress(static_cast<int>(total == 0 ? 100 : end * 100 / total))) {
      return std::unexpected(AsrError::kCancelled);
    }
    if (last) {
      return out;
    }
  }
}

}  // namespace micnote::asr
