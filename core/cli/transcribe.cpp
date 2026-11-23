#include "transcribe.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "asr/transcribe.hpp"
#include "asr/whisper_engine.hpp"
#include "audio/opus_decode.hpp"
#include "crypto/psa.hpp"
#include "models.hpp"
#include "recording/reader.hpp"

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;

std::string Clock(std::int64_t ms) {
  return std::format("{:02}:{:02}.{:03}", ms / 60000, ms / 1000 % 60, ms % 1000);
}

std::vector<std::byte> ReadAll(const fs::path &path, bool &ok) {
  std::ifstream in(path, std::ios::binary);
  ok = static_cast<bool>(in);
  const std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
  std::vector<std::byte> out(chars.size());
  std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
  return out;
}

}  // namespace

int Transcribe(std::span<const std::string_view> args, const Env &env) {
  std::map<std::string, std::string, std::less<>> opts;
  for (std::size_t i = 1; i < args.size(); i += 2) {
    const std::string_view o = args[i];
    const bool known = o == "--file" || o == "--model" || o == "--models" || o == "--vad" ||
                       o == "--key" || o == "--language" || o == "--beam";
    if (!known || i + 1 == args.size() || !opts.emplace(o, args[i + 1]).second) {
      *env.err << "usage:\n" << kTranscribeUsage;
      return 2;
    }
  }
  if (!opts.contains("--file") || (opts.contains("--model") && opts.contains("--models"))) {
    *env.err << "usage:\n" << kTranscribeUsage;
    return 2;
  }

  // The host key that decrypts the recording.
  const fs::path key_path = opts.contains("--key") ? fs::path(opts.at("--key")) : env.default_key;
  bool ok = false;
  auto priv = ReadAll(key_path, ok);
  if (!ok || priv.size() != crypto::kX25519KeySize) {
    *env.err << std::format("micnote-cli: no host key at {}\n", key_path.string());
    return 1;
  }
  auto key = recording::HostKey::FromPrivate(std::span(priv).first<crypto::kX25519KeySize>());
  crypto::Wipe(priv);
  const auto file = ReadAll(opts.at("--file"), ok);
  if (!ok) {
    *env.err << std::format("micnote-cli: can't read {}\n", opts.at("--file"));
    return 1;
  }
  const auto rec = recording::ReadRecording(file, *key);
  if (!rec) {
    *env.err << std::format("micnote-cli: can't decrypt {}: {}\n", opts.at("--file"),
                            Describe(rec.error()));
    return 1;
  }
  const auto pcm = audio::DecodeOpus(rec->packets);
  if (!pcm) {
    *env.err << "micnote-cli: the recording's audio doesn't decode\n";
    return 1;
  }

  // --model, or else the registry's default for this machine from the models
  // directory, with its VAD model if that's there too.
  std::string model = opts.contains("--model") ? opts.at("--model") : "";
  std::string vad = opts.contains("--vad") ? opts.at("--vad") : "";
  if (model.empty()) {
    const fs::path dir = opts.contains("--models") ? fs::path(opts.at("--models")) : env.models_dir;
    const auto reg = CliRegistry(env);
    if (!reg) {
      return reg.error();
    }
    const models::Device device = env.device ? *env.device : models::ThisDevice();
    const models::Model *asr = reg->Default(models::Kind::kAsr, device);
    if (asr == nullptr || models::Check(*asr, dir) != models::FileState::kPresent) {
      *env.err << std::format(
          "micnote-cli: no speech model in {}; see `micnote-cli models list`, or pass --model\n",
          dir.string());
      return 1;
    }
    model = (dir / asr->file).string();
    const models::Model *v = reg->Default(models::Kind::kVad, device);
    if (vad.empty() && v != nullptr && models::Check(*v, dir) == models::FileState::kPresent) {
      vad = (dir / v->file).string();
    }
  }
  auto engine = asr::WhisperEngine::Load(model, vad);
  if (!engine) {
    *env.err << std::format("micnote-cli: {} isn't a whisper model{}\n", model,
                            vad.empty() ? "" : std::format(", or {} isn't a VAD model", vad));
    return 1;
  }
  asr::AsrOptions options;
  if (opts.contains("--language")) {
    options.language = opts.at("--language");
  }
  if (opts.contains("--beam")) {
    int beam = 0;
    const std::string &v = opts.at("--beam");
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), beam);
    if (ec != std::errc() || ptr != v.data() + v.size() || beam < 1) {
      *env.err << "micnote-cli: --beam takes a positive integer\n";
      return 2;
    }
    options.beam_size = beam;
  }
  const auto t0 = std::chrono::steady_clock::now();
  const auto segments = asr::Transcribe(**engine, *pcm, options, [&](int pct) {
    *env.err << std::format("\rtranscribing... {}%", pct) << std::flush;
    return true;
  });
  *env.err << "\n";
  if (!segments) {
    *env.err << "micnote-cli: transcription failed\n";
    return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double audio_secs = static_cast<double>(pcm->size()) / audio::kAsrRate;

  for (const auto &s : *segments) {
    *env.out << std::format("[{} --> {}] {}\n", Clock(s.start_ms), Clock(s.end_ms), s.text);
  }
  for (const auto &m : rec->markers) {
    *env.out << std::format("marker at {}\n", Clock(m.offset_ms));
  }
  *env.out << std::format("{} segments, {:.1f} s of audio in {:.1f} s (RTF {:.2f}, {} on {}{})\n",
                          segments->size(), audio_secs, secs,
                          audio_secs > 0 ? secs / audio_secs : 0.0, (*engine)->Model(),
                          (*engine)->Backend(), (*engine)->Vad() ? ", VAD" : "");
  return 0;
}

}  // namespace micnote::cli
