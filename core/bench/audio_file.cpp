// miniaudio (in whisper.cpp's tree) is built here, without its device code.
#include "audio_file.hpp"

#include <array>

#define MA_NO_DEVICE_IO
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace micnote::bench {

std::optional<std::vector<float>> ReadAudio(const std::filesystem::path &path) {
  const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 16000);
  ma_decoder decoder;
  if (ma_decoder_init_file(path.string().c_str(), &config, &decoder) != MA_SUCCESS) {
    return std::nullopt;
  }
  std::vector<float> out;
  std::array<float, 4096> buf{};
  for (;;) {
    ma_uint64 got = 0;
    const ma_result r = ma_decoder_read_pcm_frames(&decoder, buf.data(), buf.size(), &got);
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    if (r == MA_AT_END || got == 0) {
      break;
    }
    if (r != MA_SUCCESS) {
      ma_decoder_uninit(&decoder);
      return std::nullopt;
    }
  }
  ma_decoder_uninit(&decoder);
  return out;
}

}  // namespace micnote::bench
