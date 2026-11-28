// Speech recognition for real (micnote_asr builds): whisper.cpp's JFK
// sample, straight, through the puck's codec settings and between long
// silences with VAD, and `micnote-cli transcribe`. Needs MICNOTE_TEST_MODEL
// (ggml-tiny.en.bin is enough); without it the tests skip. The VAD model is
// the Silero one in whisper.cpp's own tree (MICNOTE_WHISPER_VAD_MODEL).
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "asr/transcribe.hpp"
#include "asr/whisper_engine.hpp"
#include "audio/opus_decode.hpp"
#include "commands.hpp"
#include "local.hpp"
#include "secrets/secrets.hpp"
#include "store/db.hpp"
#include "store/repos.hpp"
#include "transcribe.hpp"
#include <nlohmann/json.hpp>
#include <opus.h>

namespace {

namespace fs = std::filesystem;
using micnote::asr::AsrError;
using micnote::asr::AsrOptions;
using micnote::asr::WhisperEngine;

std::string ModelPath() {
  const char *m = std::getenv("MICNOTE_TEST_MODEL");  // NOLINT(concurrency-mt-unsafe)
  return m != nullptr ? m : "";
}

// GTEST_SKIP returns from the test body, hence a macro.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define REQUIRE_MODEL()                                               \
  if (ModelPath().empty()) {                                          \
    GTEST_SKIP() << "set MICNOTE_TEST_MODEL to a ggml whisper model"; \
  }

// A 16-bit PCM mono WAV (jfk.wav is 16 kHz) as floats.
std::vector<float> ReadWav(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> b{std::istreambuf_iterator<char>(in), {}};
  const auto u32 = [&](std::size_t o) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(b[o])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[o + 1])) << 8U |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[o + 2])) << 16U |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b[o + 3])) << 24U;
  };
  std::vector<float> pcm;
  for (std::size_t o = 12; o + 8 <= b.size();) {
    const std::uint32_t size = u32(o + 4);
    if (std::string_view(&b[o], 4) == "data") {
      for (std::size_t i = o + 8; i + 1 < o + 8 + size && i + 1 < b.size(); i += 2) {
        const unsigned lo = static_cast<unsigned char>(b[i]);
        const unsigned hi = static_cast<unsigned char>(b[i + 1]);
        const auto s = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo | (hi << 8U)));
        pcm.push_back(static_cast<float>(s) / 32768.0F);
      }
      break;
    }
    o += 8 + size + (size & 1U);
  }
  return pcm;
}

std::string Joined(const std::vector<micnote::asr::AsrSegment> &segs) {
  std::string all;
  for (const auto &s : segs) {
    all += s.text + " ";
  }
  std::ranges::transform(all, all.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return all;
}

TEST(Whisper, RefusesSomethingThatIsntAModel) {
  const fs::path bogus = fs::temp_directory_path() / "micnote-not-a-model.bin";
  std::ofstream(bogus) << "not a model";
  EXPECT_EQ(WhisperEngine::Load(bogus).error(), AsrError::kModel);
  fs::remove(bogus);
}

TEST(Whisper, RefusesSomethingThatIsntAVadModel) {
  REQUIRE_MODEL();
  const fs::path bogus = fs::temp_directory_path() / "micnote-not-a-vad-model.bin";
  std::ofstream(bogus) << "not a model";
  EXPECT_EQ(WhisperEngine::Load(ModelPath(), bogus).error(), AsrError::kModel);
  fs::remove(bogus);
}

TEST(Whisper, TranscribesTheJfkSample) {
  REQUIRE_MODEL();
  const auto pcm = ReadWav(fs::path(MICNOTE_WHISPER_SAMPLES) / "jfk.wav");
  ASSERT_GT(pcm.size(), 16000U * 10);
  auto engine = WhisperEngine::Load(ModelPath());
  ASSERT_TRUE(engine.has_value());
  const auto segs = micnote::asr::Transcribe(**engine, pcm, AsrOptions{});
  ASSERT_TRUE(segs.has_value());
  const std::string text = Joined(*segs);
  EXPECT_NE(text.find("ask not what your country can do for you"), std::string::npos) << text;
  EXPECT_LE(segs->back().end_ms, 11100);
}

// Every backend this build and machine have gives the same words, and says
// which it was; one they don't have is refused.
TEST(Whisper, TranscribesOnEveryBackend) {
  REQUIRE_MODEL();
  const auto pcm = ReadWav(fs::path(MICNOTE_WHISPER_SAMPLES) / "jfk.wav");
  const auto backends = WhisperEngine::Backends();
  ASSERT_FALSE(backends.empty());
  EXPECT_EQ(backends.front(), "cpu");
  for (const std::string &b : backends) {
    auto engine = WhisperEngine::Load(ModelPath(), {}, b);
    ASSERT_TRUE(engine.has_value()) << b;
    EXPECT_EQ((*engine)->Backend(), b);
    const auto segs = micnote::asr::Transcribe(**engine, pcm, AsrOptions{});
    ASSERT_TRUE(segs.has_value()) << b;
    const std::string text = Joined(*segs);
    EXPECT_NE(text.find("ask not what your country can do for you"), std::string::npos)
        << b << ": " << text;
  }
  // auto: the first GPU if there is one, else the CPU.
  auto automatic = WhisperEngine::Load(ModelPath());
  ASSERT_TRUE(automatic.has_value());
  EXPECT_EQ((*automatic)->Backend(), backends.size() > 1 ? backends[1] : "cpu");
  EXPECT_EQ(WhisperEngine::Load(ModelPath(), {}, "abacus").error(), AsrError::kBackend);
}

// The puck's encoder settings (stage 1 §4.2: VOIP, 16 kHz, 20 ms, 16 kbps
// VBR, complexity 3), then the host's decoder, then whisper.
TEST(Whisper, TranscribesThroughThePucksCodec) {
  REQUIRE_MODEL();
  const auto pcm = ReadWav(fs::path(MICNOTE_WHISPER_SAMPLES) / "jfk.wav");
  int err = 0;
  OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
  ASSERT_EQ(err, OPUS_OK);
  // opus_encoder_ctl is variadic: libopus's API, not ours to change.
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
  ASSERT_EQ(opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000)), OPUS_OK);
  ASSERT_EQ(opus_encoder_ctl(enc, OPUS_SET_VBR(1)), OPUS_OK);
  ASSERT_EQ(opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3)), OPUS_OK);
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)
  std::vector<std::vector<std::byte>> packets;
  std::vector<unsigned char> buf(256);
  for (std::size_t i = 0; i + 320 <= pcm.size(); i += 320) {
    const int n = opus_encode_float(enc, &pcm[i], 320, buf.data(), static_cast<int>(buf.size()));
    ASSERT_GT(n, 0);
    const auto *b = reinterpret_cast<const std::byte *>(buf.data());
    packets.emplace_back(b, b + n);
  }
  opus_encoder_destroy(enc);
  const auto decoded = micnote::audio::DecodeOpus(packets);
  ASSERT_TRUE(decoded.has_value());

  auto engine = WhisperEngine::Load(ModelPath());
  ASSERT_TRUE(engine.has_value());
  const auto segs = micnote::asr::Transcribe(
      **engine, *decoded,
      AsrOptions{.language = "en", .initial_prompt = "", .beam_size = 1, .threads = 0});
  ASSERT_TRUE(segs.has_value());
  const std::string text = Joined(*segs);
  EXPECT_NE(text.find("your country"), std::string::npos) << text;
}

// Quiet room noise (about -60 dBFS), so the silence isn't digital zero.
std::vector<float> Hiss(std::size_t samples, std::uint32_t seed) {
  // A fixed seed on purpose: repeatable noise.
  // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
  std::minstd_rand rng(seed);
  std::uniform_real_distribution<float> d(-0.001F, 0.001F);
  std::vector<float> out(samples);
  std::ranges::generate(out, [&] { return d(rng); });
  return out;
}

// 15 s of hiss, JFK (11 s), 15 s of hiss: VAD hands whisper only the speech,
// and the segments still come back in recording time.
TEST(Whisper, VadKeepsRecordingTimeAcrossSilence) {
  REQUIRE_MODEL();
  const auto jfk = ReadWav(fs::path(MICNOTE_WHISPER_SAMPLES) / "jfk.wav");
  constexpr std::size_t kPad = std::size_t{15} * 16000;
  std::vector<float> pcm = Hiss(kPad, 1);
  pcm.insert(pcm.end(), jfk.begin(), jfk.end());
  const auto tail = Hiss(kPad, 2);
  pcm.insert(pcm.end(), tail.begin(), tail.end());

  auto engine = WhisperEngine::Load(ModelPath(), MICNOTE_WHISPER_VAD_MODEL);
  ASSERT_TRUE(engine.has_value());
  EXPECT_TRUE((*engine)->Vad());
  const auto segs = micnote::asr::Transcribe(**engine, pcm, AsrOptions{});
  ASSERT_TRUE(segs.has_value());
  ASSERT_FALSE(segs->empty());
  const std::string text = Joined(*segs);
  EXPECT_NE(text.find("ask not what your country can do for you"), std::string::npos) << text;
  const auto jfk_ms = static_cast<std::int64_t>(jfk.size() / 16);
  // The leading silence still counts.
  EXPECT_GE(segs->front().start_ms, 14000);
  EXPECT_LE(segs->back().end_ms, 15000 + jfk_ms + 1000);
}

TEST(Whisper, VadGivesNothingForSilence) {
  REQUIRE_MODEL();
  const auto pcm = Hiss(std::size_t{20} * 16000, 3);
  auto engine = WhisperEngine::Load(ModelPath(), MICNOTE_WHISPER_VAD_MODEL);
  ASSERT_TRUE(engine.has_value());
  const auto segs = micnote::asr::Transcribe(**engine, pcm, AsrOptions{});
  ASSERT_TRUE(segs.has_value());
  EXPECT_TRUE(segs->empty()) << Joined(*segs);
}

TEST(Whisper, CliTranscribesARecording) {
  REQUIRE_MODEL();
  std::random_device rd;
  const fs::path dir = fs::temp_directory_path() / ("micnote-asr-cli-" + std::to_string(rd()));
  fs::create_directories(dir);
  std::ifstream vin(std::string(MICNOTE_TESTVECTORS_DIR) + "/vectors.json");
  const auto vectors = nlohmann::json::parse(vin);
  const std::string hex = vectors["keys"]["host_a"]["priv"].get<std::string>();
  {
    std::ofstream key(dir / "host.key", std::ios::binary);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      key.put(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
  }
  std::ostringstream out;
  std::ostringstream err;
  const micnote::cli::Env env{.out = &out,
                              .err = &err,
                              .open = {},
                              .now_unix_ms = [] { return std::uint64_t{0}; },
                              .default_key = dir / "host.key",
                              .models_dir = {},
                              .manifest_json = {},
                              .device = {},
                              .data_dir = {},
                              .port_present = {},
                              .wait = {},
                              .engine = {},
                              .llm = {},
                              .keychain = nullptr,
                              .in = nullptr,
                              .http = {},
                              .claude_base_url = {}};
  const std::string file = std::string(MICNOTE_TESTVECTORS_DIR) + "/rec_two_hosts.mnr";
  const std::string model = ModelPath();
  const std::vector<std::string_view> args = {"transcribe", "--file", file, "--model",
                                              model,        "--beam", "1"};
  EXPECT_EQ(micnote::cli::Run(args, env), 0) << err.str();
  EXPECT_NE(out.str().find("segments, 5.0 s of audio"), std::string::npos) << out.str();
  EXPECT_NE(out.str().find("marker at 00:01.000"), std::string::npos) << out.str();

  // With VAD.
  std::ostringstream vout;
  const micnote::cli::Env venv{.out = &vout,
                               .err = &err,
                               .open = {},
                               .now_unix_ms = [] { return std::uint64_t{0}; },
                               .default_key = dir / "host.key",
                               .models_dir = {},
                               .manifest_json = {},
                               .device = {},
                               .data_dir = {},
                               .port_present = {},
                               .wait = {},
                               .engine = {},
                               .llm = {},
                               .keychain = nullptr,
                               .in = nullptr,
                               .http = {},
                               .claude_base_url = {}};
  const std::vector<std::string_view> vad = {
      "transcribe", "--file", file, "--model", model, "--vad", MICNOTE_WHISPER_VAD_MODEL};
  EXPECT_EQ(micnote::cli::Run(vad, venv), 0) << err.str();
  EXPECT_NE(vout.str().find(", VAD)"), std::string::npos) << vout.str();
  const std::vector<std::string_view> bad_vad = {"transcribe", "--file", file, "--model",
                                                 model,        "--vad",  file};
  EXPECT_EQ(micnote::cli::Run(bad_vad, venv), 1);

  // No --model: the registry's default from the models directory. A
  // manifest naming tiny.en (as CI pins it) and the Silero model, both
  // linked into place under their URLs' names.
  const fs::path models = dir / "models";
  fs::create_directories(models);
  fs::create_symlink(fs::absolute(model), models / "ggml-tiny.en.bin");
  fs::create_symlink(MICNOTE_WHISPER_VAD_MODEL, models / "ggml-silero-v6.2.0.bin");
  const std::string manifest = R"({"version":1,"models":[
    {"id":"tiny","kind":"asr","url":"https://x/ggml-tiny.en.bin",
     "sha256":"921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f",
     "bytes":)" + std::to_string(fs::file_size(model)) +
                               R"(,"classes":["desktop"]},
    {"id":"silero","kind":"vad","url":"https://x/ggml-silero-v6.2.0.bin",
     "sha256":"2aa269b785eeb53a82983a20501ddf7c1d9c48e33ab63a41391ac6c9f7fb6987",
     "bytes":885098,"classes":["desktop"]}],
    "defaults":[{"class":"desktop","asr":"tiny","vad":"silero"}]})";
  std::ostringstream rout;
  std::ostringstream rerr;
  const micnote::cli::Env renv{.out = &rout,
                               .err = &rerr,
                               .open = {},
                               .now_unix_ms = [] { return std::uint64_t{0}; },
                               .default_key = dir / "host.key",
                               .models_dir = models,
                               .manifest_json = manifest,
                               .device = {},
                               .data_dir = {},
                               .port_present = {},
                               .wait = {},
                               .engine = {},
                               .llm = {},
                               .keychain = nullptr,
                               .in = nullptr,
                               .http = {},
                               .claude_base_url = {}};
  const std::vector<std::string_view> from_registry = {"transcribe", "--file", file, "--beam", "1"};
  EXPECT_EQ(micnote::cli::Run(from_registry, renv), 0) << rerr.str();
  EXPECT_NE(rout.str().find(", ggml-tiny.en on "), std::string::npos) << rout.str();
  EXPECT_NE(rout.str().find(", VAD)"), std::string::npos) << rout.str();
  // An empty models directory, or a bad manifest.
  const std::string dir_arg = dir.string();  // outlives the views below
  const std::vector<std::string_view> elsewhere = {"transcribe", "--file", file, "--models",
                                                   dir_arg};
  EXPECT_EQ(micnote::cli::Run(elsewhere, renv), 1);
  EXPECT_NE(rerr.str().find("no speech model in"), std::string::npos) << rerr.str();
  const micnote::cli::Env bad_manifest{.out = &rout,
                                       .err = &rerr,
                                       .open = {},
                                       .now_unix_ms = [] { return std::uint64_t{0}; },
                                       .default_key = dir / "host.key",
                                       .models_dir = models,
                                       .manifest_json = "{",
                                       .device = {},
                                       .data_dir = {},
                                       .port_present = {},
                                       .wait = {},
                                       .engine = {},
                                       .llm = {},
                                       .keychain = nullptr,
                                       .in = nullptr,
                                       .http = {},
                                       .claude_base_url = {}};
  EXPECT_EQ(micnote::cli::Run(from_registry, bad_manifest), 1);

  // Backends: an unknown name, one this machine lacks, and the CPU.
  EXPECT_EQ(micnote::cli::CheckBackend("auto"), "");
  EXPECT_EQ(micnote::cli::CheckBackend("cpu"), "");
  EXPECT_NE(micnote::cli::CheckBackend("abacus").find("--backend is"), std::string::npos);
  const std::vector<std::string_view> abacus = {"transcribe", "--file",    file,    "--model",
                                                model,        "--backend", "abacus"};
  EXPECT_EQ(micnote::cli::Run(abacus, env), 2);
  const auto have = WhisperEngine::Backends();
  for (const std::string_view b : {"metal", "vulkan", "cuda"}) {
    if (std::ranges::find(have, b) == have.end()) {
      EXPECT_NE(micnote::cli::CheckBackend(b).find("this build and machine have: cpu"),
                std::string::npos);
      const std::vector<std::string_view> missing_one = {"transcribe", "--file",    file, "--model",
                                                         model,        "--backend", b};
      EXPECT_EQ(micnote::cli::Run(missing_one, env), 1);
      EXPECT_NE(err.str().find("no " + std::string(b) + " backend here"), std::string::npos)
          << err.str();
    }
  }
  const std::vector<std::string_view> run_abacus = {"run", "--usb", "x", "--backend", "abacus"};
  EXPECT_EQ(micnote::cli::Run(run_abacus, env), 2);
  out.str({});
  const std::vector<std::string_view> on_cpu = {
      "transcribe", "--file", file, "--model", model, "--beam", "1", "--backend", "cpu"};
  EXPECT_EQ(micnote::cli::Run(on_cpu, env), 0) << err.str();
  EXPECT_NE(out.str().find(" on cpu)"), std::string::npos) << out.str();

  // Usage errors.
  const std::vector<std::string_view> both = {"transcribe", "--file",   file,   "--model",
                                              model,        "--models", dir_arg};
  EXPECT_EQ(micnote::cli::Run(both, env), 2);
  const std::vector<std::string_view> missing = {"transcribe", "--model", model};
  EXPECT_EQ(micnote::cli::Run(missing, env), 2);
  const std::vector<std::string_view> unknown = {"transcribe", "--bogus", "x"};
  EXPECT_EQ(micnote::cli::Run(unknown, env), 2);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// `transcribe --recording`: a stored recording, transcribed by the store's
// job queue (as the app does it) and printed from the store; and `run`
// picking the registry's model.
TEST(Whisper, CliTranscribesAStoredRecordingAndRunFindsTheModel) {
  REQUIRE_MODEL();
  std::random_device rd;
  const fs::path dir = fs::temp_directory_path() / ("micnote-asr-store-" + std::to_string(rd()));
  fs::create_directories(dir / "data" / "rec");
  std::ifstream vin(std::string(MICNOTE_TESTVECTORS_DIR) + "/vectors.json");
  const auto vectors = nlohmann::json::parse(vin);
  const std::string hex = vectors["keys"]["host_a"]["priv"].get<std::string>();
  {
    std::ofstream key(dir / "host.key", std::ios::binary);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      key.put(static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
  }
  fs::copy_file(std::string(MICNOTE_TESTVECTORS_DIR) + "/rec_two_hosts.mnr",
                dir / "data" / "rec" / "a.mnr");
  std::ostringstream out;
  std::ostringstream err;
  int waits = 0;
  bool stop_waiting = false;
  const micnote::cli::Env env{.out = &out,
                              .err = &err,
                              .open = {},
                              .now_unix_ms = [] { return std::uint64_t{0}; },
                              .default_key = dir / "host.key",
                              .models_dir = {},
                              .manifest_json = {},
                              .device = {},
                              .data_dir = dir / "data",
                              .port_present = {},
                              .wait =
                                  [&](std::chrono::milliseconds) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                    ++waits;
                                    return !stop_waiting && waits < 6000;
                                  },
                              .engine = {},
                              .llm = {},
                              .keychain = nullptr,
                              .in = nullptr,
                              .http = {},
                              .claude_base_url = {}};
  const auto run = [&](std::vector<std::string> args) {
    out.str({});
    err.str({});
    const std::vector<std::string_view> views(args.begin(), args.end());
    return micnote::cli::Run(views, env);
  };
  // The store, then a recording in it, as a sync would have left it.
  ASSERT_EQ(run({"db", "recordings"}), 0) << err.str();
  {
    micnote::secrets::FileSecrets secrets(dir);
    const auto key = secrets.Get("db_key").value();
    micnote::store::DbKey k{};
    std::ranges::copy(key, k.begin());
    auto db = micnote::store::Db::Open(dir / "data" / "micnote.db", k).value();
    micnote::store::Device d;
    d.device_id.fill(std::byte{1});
    d.paired_at = 1;
    const auto dev = micnote::store::UpsertDevice(db, d).value();
    micnote::store::StoredRecording r;
    r.device = dev;
    r.rec_id = 1;
    r.duration_ms = 5000;
    r.mnr_path = "rec/a.mnr";
    r.created_at = 1;
    ASSERT_TRUE(micnote::store::InsertRecording(db, r));
  }
  const std::string model = ModelPath();
  EXPECT_EQ(run({"transcribe", "--recording", "1", "--model", model, "--beam", "1"}), 0)
      << err.str();
  EXPECT_NE(out.str().find("recording 1: ggml-tiny.en on "), std::string::npos) << out.str();
  EXPECT_NE(out.str().find("  [00:00.000 --> "), std::string::npos) << out.str();
  EXPECT_NE(err.str().find("transcription... "), std::string::npos) << err.str();
  // It's in the store now.
  EXPECT_EQ(run({"db", "transcripts", "--recording", "1"}), 0) << err.str();
  EXPECT_NE(out.str().find("recording 1: ggml-tiny.en"), std::string::npos);

  // A recording that isn't there fails the job, which isn't left to retry.
  EXPECT_EQ(run({"transcribe", "--recording", "99", "--model", model}), 1);
  EXPECT_NE(err.str().find("transcription failed: no recording 99"), std::string::npos)
      << err.str();
  // Ctrl-C while it runs.
  stop_waiting = true;
  EXPECT_EQ(run({"transcribe", "--recording", "1", "--model", model}), 1);
  EXPECT_NE(err.str().find("stopped"), std::string::npos) << err.str();
  stop_waiting = false;
  // Usage.
  for (const std::vector<std::string> &bad :
       std::vector<std::vector<std::string>>{{"transcribe", "--recording", "x", "--model", model},
                                             {"transcribe", "--recording", "1", "--file", "f.mnr"},
                                             {"transcribe", "--recording", "1", "--key", "k"},
                                             {"transcribe", "--file", "f.mnr", "--data", "d"},
                                             {"transcribe", "--model", model}}) {
    EXPECT_EQ(run(bad), 2) << bad.size();
  }

  // run: without a model it says so; with the registry's model downloaded,
  // it transcribes.
  stop_waiting = true;
  EXPECT_EQ(run({"run", "--usb", (dir / "no-port").string(), "--models", (dir / "none").string()}),
            0);
  EXPECT_NE(out.str().find("no speech model"), std::string::npos) << out.str();
  const fs::path models = dir / "models";
  fs::create_directories(models);
  fs::create_symlink(fs::absolute(model), models / "ggml-tiny.en.bin");
  micnote::cli::Env with_models = env;
  with_models.manifest_json = R"({"version":1,"models":[
    {"id":"tiny","kind":"asr","url":"https://x/ggml-tiny.en.bin",
     "sha256":"921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f",
     "bytes":)" + std::to_string(fs::file_size(model)) +
                              R"(,"classes":["desktop"]}],
    "defaults":[{"class":"desktop","asr":"tiny"}]})";
  out.str({});
  const std::string models_arg = models.string();  // outlives the views below
  const std::vector<std::string_view> run_args = {"run", "--usb", "no-port", "--models",
                                                  models_arg};
  EXPECT_EQ(micnote::cli::Run(run_args, with_models), 0);
  EXPECT_EQ(out.str().find("no speech model"), std::string::npos) << out.str();
  // The engine it built loads the model, once, on the first job.
  const auto engine = micnote::cli::RegistryEngine(with_models, models);
  ASSERT_TRUE(engine.has_value());
  const auto make = engine.value_or(micnote::jobs::EngineFactory{});
  const auto first = make();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)->Model(), "ggml-tiny.en");
  EXPECT_EQ(make().value().get(), first->get()) << "loaded once";
  stop_waiting = false;
  std::error_code ec;
  fs::remove_all(dir, ec);
}

}  // namespace
