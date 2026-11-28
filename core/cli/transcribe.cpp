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
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "asr/transcribe.hpp"
#include "asr/whisper_engine.hpp"
#include "audio/opus_decode.hpp"
#include "crypto/psa.hpp"
#include "keys.hpp"
#include "local.hpp"
#include "models.hpp"
#include "recording/reader.hpp"
#include <nlohmann/json.hpp>

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using Opts = std::map<std::string, std::string, std::less<>>;
// What an engine factory makes.
using MadeEngine = std::expected<std::shared_ptr<asr::AsrEngine>, asr::AsrError>;

std::vector<std::byte> ReadAll(const fs::path &path, bool &ok) {
  std::ifstream in(path, std::ios::binary);
  ok = static_cast<bool>(in);
  const std::vector<char> chars{std::istreambuf_iterator<char>(in), {}};
  std::vector<std::byte> out(chars.size());
  std::ranges::transform(chars, out.begin(), [](char c) { return static_cast<std::byte>(c); });
  return out;
}

struct ModelFiles {
  std::string model;
  std::string vad;  // empty: no VAD
  std::string backend = "auto";
};

constexpr std::array<std::string_view, 5> kBackendNames = {"auto", "cpu", "metal", "vulkan",
                                                           "cuda"};

std::string Joined(const std::vector<std::string> &names) {
  std::string out;
  for (const std::string &n : names) {
    out += (out.empty() ? "" : ", ") + n;
  }
  return out;
}

// The registry's default speech model for this machine in dir, with its
// VAD model if that's downloaded too; nullopt if the speech model isn't.
std::optional<ModelFiles> RegistryDefaults(const models::Registry &reg, const fs::path &dir,
                                           const Env &env, std::string_view backend) {
  models::Device device = CliDevice(env);
  device.gpu = device.gpu && backend != "cpu";
  const models::Model *asr = reg.Default(models::Kind::kAsr, device);
  if (asr == nullptr || models::Check(*asr, dir) != models::FileState::kPresent) {
    return std::nullopt;
  }
  ModelFiles files{.model = (dir / asr->file).string(), .vad = {}, .backend = "auto"};
  const models::Model *v = reg.Default(models::Kind::kVad, device);
  if (v != nullptr && models::Check(*v, dir) == models::FileState::kPresent) {
    files.vad = (dir / v->file).string();
  }
  return files;
}

// --model (with --vad if given), or else the registry's defaults.
std::expected<ModelFiles, int> ResolveModels(const Opts &opts, const Env &env) {
  const std::string vad = opts.contains("--vad") ? opts.at("--vad") : "";
  const std::string backend = opts.contains("--backend") ? opts.at("--backend") : "auto";
  if (std::ranges::find(kBackendNames, backend) == kBackendNames.end()) {
    *env.err << "micnote-cli: --backend is auto, cpu, metal, vulkan or cuda\n";
    return std::unexpected(2);
  }
  if (opts.contains("--model")) {
    return ModelFiles{.model = opts.at("--model"), .vad = vad, .backend = backend};
  }
  const fs::path dir = opts.contains("--models") ? fs::path(opts.at("--models")) : env.models_dir;
  const auto reg = CliRegistry(env);
  if (!reg) {
    return std::unexpected(reg.error());
  }
  auto files = RegistryDefaults(*reg, dir, env, backend);
  if (!files) {
    *env.err << std::format(
        "micnote-cli: no speech model in {}; see `micnote-cli models list`, or pass --model\n",
        dir.string());
    return std::unexpected(1);
  }
  if (!vad.empty()) {
    files->vad = vad;
  }
  files->backend = backend;
  return *files;
}

std::expected<std::unique_ptr<asr::WhisperEngine>, int> LoadEngine(const ModelFiles &files,
                                                                   const Env &env) {
  auto engine = asr::WhisperEngine::Load(files.model, files.vad, files.backend);
  if (!engine && engine.error() == asr::AsrError::kBackend) {
    *env.err << std::format("micnote-cli: no {} backend here; this build and machine have: {}\n",
                            files.backend, Joined(asr::WhisperEngine::Backends()));
    return std::unexpected(1);
  }
  if (!engine) {
    *env.err << std::format(
        "micnote-cli: {} isn't a whisper model{}\n", files.model,
        files.vad.empty() ? "" : std::format(", or {} isn't a VAD model", files.vad));
    return std::unexpected(1);
  }
  return std::move(*engine);
}

std::expected<asr::AsrOptions, int> SpeechOptions(const Opts &opts, const Env &env) {
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
      return std::unexpected(2);
    }
    options.beam_size = beam;
  }
  return options;
}

// --file: decrypt, decode and transcribe one .mnr here, storing nothing.
int TranscribeFile(const Opts &opts, const Env &env) {
  std::optional<fs::path> key_file;
  if (opts.contains("--key")) {
    key_file = fs::path(opts.at("--key"));
  }
  const auto keys = OpenKeys(key_file, env);
  if (!keys) {
    return keys.error();
  }
  auto key = LoadHostKey(*keys, env);
  if (!key) {
    return 1;
  }
  bool ok = false;
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
  const auto files = ResolveModels(opts, env);
  if (!files) {
    return files.error();
  }
  auto engine = LoadEngine(*files, env);
  if (!engine) {
    return engine.error();
  }
  const auto options = SpeechOptions(opts, env);
  if (!options) {
    return options.error();
  }
  const auto t0 = std::chrono::steady_clock::now();
  const auto segments = asr::Transcribe(**engine, *pcm, *options, [&](int pct) {
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

// --recording: a transcription job for a stored recording, on the store's
// job queue (as the app runs it), then its transcript from the store.
int TranscribeStored(const Opts &opts, const Env &env) {
  std::int64_t id = 0;
  const std::string &v = opts.at("--recording");
  const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), id);
  if (ec != std::errc() || ptr != v.data() + v.size()) {
    *env.err << "micnote-cli: --recording takes a recording's ID (see `micnote-cli db "
                "recordings`)\n";
    return 2;
  }
  const auto files = ResolveModels(opts, env);
  if (!files) {
    return files.error();
  }
  auto loaded = LoadEngine(*files, env);
  if (!loaded) {
    return loaded.error();
  }
  const auto speech = SpeechOptions(opts, env);
  if (!speech) {
    return speech.error();
  }
  std::shared_ptr<asr::AsrEngine> engine = std::move(*loaded);
  api::ContextOptions options;
  options.engine = [engine] { return MadeEngine(engine); };
  options.asr = *speech;
  EventInbox inbox;
  const fs::path data = opts.contains("--data") ? fs::path(opts.at("--data")) : fs::path();
  auto ctx = OpenStore(env, data, std::move(options), &EventInbox::Callback, &inbox);
  if (!ctx) {
    return ctx.error();
  }
  if (!RunJob(**ctx, inbox, "transcribe.run@1", nlohmann::json{{"recording", id}}, "transcription",
              env)) {
    return 1;
  }
  return PrintTranscript(**ctx, id, env) ? 0 : 1;
}

}  // namespace

std::string CheckBackend(std::string_view backend) {
  if (std::ranges::find(kBackendNames, backend) == kBackendNames.end()) {
    return "--backend is auto, cpu, metal, vulkan or cuda";
  }
  const auto have = asr::WhisperEngine::Backends();
  if (backend != "auto" && std::ranges::find(have, backend) == have.end()) {
    return std::format("no {} backend here; this build and machine have: {}", backend,
                       Joined(have));
  }
  return {};
}

std::optional<jobs::EngineFactory> RegistryEngine(const Env &env, const fs::path &models_dir,
                                                  std::string_view backend) {
  const auto reg = CliRegistry(env);
  if (!reg) {
    return std::nullopt;
  }
  auto files = RegistryDefaults(*reg, models_dir, env, backend);
  if (!files) {
    return std::nullopt;
  }
  files->backend = std::string(backend);
  // Loaded on the first job and kept: a model takes seconds to load.
  auto cache = std::make_shared<std::pair<std::mutex, std::shared_ptr<asr::AsrEngine>>>();
  return [cache, f = *files] {
    const std::scoped_lock lock(cache->first);
    if (!cache->second) {
      auto engine = asr::WhisperEngine::Load(f.model, f.vad, f.backend);
      if (!engine) {
        return MadeEngine(std::unexpected(engine.error()));
      }
      cache->second = std::move(*engine);
    }
    return MadeEngine(cache->second);
  };
}

int Transcribe(std::span<const std::string_view> args, const Env &env) {
  Opts opts;
  for (std::size_t i = 1; i < args.size(); i += 2) {
    const std::string_view o = args[i];
    const bool known = o == "--file" || o == "--recording" || o == "--data" || o == "--model" ||
                       o == "--models" || o == "--vad" || o == "--backend" || o == "--key" ||
                       o == "--language" || o == "--beam";
    if (!known || i + 1 == args.size() || !opts.emplace(o, args[i + 1]).second) {
      *env.err << "usage:\n" << kTranscribeUsage;
      return 2;
    }
  }
  const bool file = opts.contains("--file");
  const bool stored = opts.contains("--recording");
  // Exactly one of --file and --recording; --key only with a file, --data
  // only with the store; --model or --models, not both.
  if (file == stored || (file && opts.contains("--data")) || (stored && opts.contains("--key")) ||
      (opts.contains("--model") && opts.contains("--models"))) {
    *env.err << "usage:\n" << kTranscribeUsage;
    return 2;
  }
  return file ? TranscribeFile(opts, env) : TranscribeStored(opts, env);
}

}  // namespace micnote::cli
