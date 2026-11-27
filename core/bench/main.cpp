// micnote-wer: accuracy and speed of speech recognition (stage 2 PR 5).
//
//   micnote-wer --model MODEL.bin (--librispeech DIR | --memos DIR) [--count N]
//               [--vad SILERO.bin] [--backend B] [--beam N] [--codec] [--show]
//
// --librispeech  a folder holding LibriSpeech's *.trans.txt and *.flac files,
//                at any depth (test-clean, from https://www.openslr.org/12)
// --memos        a folder of recordings (.wav, .flac, .mp3), each with its
//                checked transcript beside it: memo.wav and memo.txt
// --count        how many clips, evenly spread (default 100; 0 for all)
// --codec        first through Opus with the puck's settings, as a real
//                recording would be
// --show         print each clip that has errors, with both texts
//
// Prints a Markdown table row for docs/measurements/stage2.md.
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asr/transcribe.hpp"
#include "asr/whisper_engine.hpp"
#include "audio/opus_decode.hpp"
#include "audio_file.hpp"
#include "wer.hpp"
#include <opus.h>

namespace {

namespace fs = std::filesystem;
using micnote::bench::Wer;

struct Clip {
  fs::path audio;
  std::string reference;
};

std::string ReadText(const fs::path &path) {
  std::error_code ec;
  std::string out(static_cast<std::size_t>(fs::file_size(path, ec)), '\0');
  std::ifstream in(path, std::ios::binary);
  in.read(out.data(), static_cast<std::streamsize>(out.size()));
  out.resize(ec ? 0 : static_cast<std::size_t>(in.gcount()));
  return out;
}

// "1089-134686-0000 HE HOPED THERE WOULD BE STEW" beside 1089-134686-0000.flac.
void AddLibriSpeech(const fs::path &trans, std::vector<Clip> &out) {
  std::ifstream in(trans);
  for (std::string line; std::getline(in, line);) {
    const auto space = line.find(' ');
    if (space != std::string::npos) {
      out.push_back({.audio = trans.parent_path() / (line.substr(0, space) + ".flac"),
                     .reference = line.substr(space + 1)});
    }
  }
}

std::vector<Clip> FindClips(const fs::path &dir, bool librispeech) {
  std::vector<Clip> out;
  std::error_code ec;
  for (const auto &e : fs::recursive_directory_iterator(dir, ec)) {
    const fs::path &p = e.path();
    if (librispeech) {
      if (p.filename().string().ends_with(".trans.txt")) {
        AddLibriSpeech(p, out);
      }
      continue;
    }
    const std::string ext = p.extension().string();
    if (ext == ".wav" || ext == ".flac" || ext == ".mp3") {
      fs::path text = p;
      text.replace_extension(".txt");
      if (fs::exists(text, ec)) {
        out.push_back({.audio = p, .reference = ReadText(text)});
      }
    }
  }
  std::ranges::sort(out, {}, &Clip::audio);
  return out;
}

// Stage 1 §4.2's encoder settings: VOIP, 16 kHz, 20 ms, 16 kbps VBR, complexity 3.
std::optional<std::vector<float>> ThroughThePucksCodec(std::vector<float> pcm) {
  constexpr std::size_t kFrame = 320;
  pcm.resize((pcm.size() + kFrame - 1) / kFrame * kFrame);
  int err = 0;
  OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
  if (err != OPUS_OK) {
    return std::nullopt;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): libopus's API
  opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));
  opus_encoder_ctl(enc, OPUS_SET_VBR(1));
  opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3));
  // NOLINTEND(cppcoreguidelines-pro-type-vararg)
  std::vector<std::vector<std::byte>> packets;
  std::vector<unsigned char> buf(1276);
  for (std::size_t i = 0; i < pcm.size(); i += kFrame) {
    const int n = opus_encode_float(enc, &pcm[i], static_cast<int>(kFrame), buf.data(),
                                    static_cast<int>(buf.size()));
    if (n < 0) {
      opus_encoder_destroy(enc);
      return std::nullopt;
    }
    const auto bytes = std::as_bytes(std::span(buf).first(static_cast<std::size_t>(n)));
    packets.emplace_back(bytes.begin(), bytes.end());
  }
  opus_encoder_destroy(enc);
  auto out = micnote::audio::DecodeOpus(packets);
  if (!out) {
    return std::nullopt;
  }
  return std::move(*out);
}

std::optional<int> Number(const std::string &s) {
  int n = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (ec != std::errc() || ptr != s.data() + s.size() || n < 0) {
    return std::nullopt;
  }
  return n;
}

int Usage() {
  std::cerr
      << "usage: micnote-wer --model MODEL.bin (--librispeech DIR | --memos DIR) [--count N]\n"
         "                   [--vad SILERO.bin] [--backend auto|cpu|metal|vulkan|cuda]\n"
         "                   [--beam N] [--codec] [--show]\n";
  return 2;
}

int Run(std::span<char *> args) {
  std::map<std::string, std::string, std::less<>> opts;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string o = args[i];
    if (o == "--codec" || o == "--show") {
      opts[o] = "1";
      continue;
    }
    const bool known = o == "--model" || o == "--librispeech" || o == "--memos" || o == "--count" ||
                       o == "--vad" || o == "--backend" || o == "--beam";
    if (!known || i + 1 == args.size()) {
      return Usage();
    }
    opts[o] = args[++i];
  }
  const auto get = [&](const char *name, const char *fallback) {
    const auto it = opts.find(name);
    return it != opts.end() ? it->second : std::string(fallback);
  };
  const bool librispeech = opts.contains("--librispeech");
  const auto count = Number(get("--count", "100"));
  const auto beam = Number(get("--beam", "5"));
  if (!opts.contains("--model") || librispeech == opts.contains("--memos") || !count || !beam ||
      *beam < 1) {
    return Usage();
  }
  const auto all =
      FindClips(librispeech ? get("--librispeech", "") : get("--memos", ""), librispeech);
  if (all.empty()) {
    std::cerr << "micnote-wer: no clips with transcripts there\n";
    return 1;
  }
  auto engine = micnote::asr::WhisperEngine::Load(get("--model", ""), get("--vad", ""),
                                                  get("--backend", "auto"));
  if (!engine) {
    std::cerr << (engine.error() == micnote::asr::AsrError::kBackend
                      ? "micnote-wer: that backend isn't available here\n"
                      : "micnote-wer: can't load that model\n");
    return 1;
  }
  micnote::asr::AsrOptions options;
  options.beam_size = *beam;
  Wer total;
  double audio_s = 0;
  double run_s = 0;
  bool warm = false;
  const auto chosen = micnote::bench::Spread(all.size(), static_cast<std::size_t>(*count));
  for (const std::size_t i : chosen) {
    const Clip &clip = all[i];
    auto pcm = micnote::bench::ReadAudio(clip.audio);
    if (pcm && opts.contains("--codec")) {
      pcm = ThroughThePucksCodec(std::move(*pcm));
    }
    if (!pcm) {
      // Skipping it would quietly change what's measured.
      std::cerr << std::format("micnote-wer: can't read {}\n", clip.audio.string());
      return 1;
    }
    if (!warm) {
      // Not timed: the first run compiles GPU shaders and pages the model in.
      warm = true;
      if (!micnote::asr::Transcribe(**engine, *pcm, options)) {
        std::cerr << "micnote-wer: transcription failed\n";
        return 1;
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto segments = micnote::asr::Transcribe(**engine, *pcm, options);
    run_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!segments) {
      std::cerr << std::format("micnote-wer: {} failed to transcribe\n", clip.audio.string());
      return 1;
    }
    audio_s += static_cast<double>(pcm->size()) / micnote::audio::kAsrRate;
    std::string text;
    for (const auto &s : *segments) {
      text += s.text + " ";
    }
    const Wer w = micnote::bench::Compare(clip.reference, text);
    total += w;
    if (w.errors > 0 && opts.contains("--show")) {
      std::cout << std::format("{}: {}/{}\n  ref: {}\n  hyp: {}\n", clip.audio.filename().string(),
                               w.errors, w.words, clip.reference, text);
    }
  }
  if (total.words == 0 || audio_s <= 0) {
    std::cerr << "micnote-wer: nothing to measure (empty transcripts or audio)\n";
    return 1;
  }
  std::cout << "| Model | Backend | Set | Clips | Audio (s) | Beam | VAD | Codec | WER | RTF |\n"
               "|---|---|---|---|---|---|---|---|---|---|\n"
            << std::format("| {} | {} | {} | {} | {:.0f} | {} | {} | {} | {:.2f}% | {:.3f} |\n",
                           (*engine)->Model(), (*engine)->Backend(),
                           librispeech ? "LibriSpeech" : "memos", chosen.size(), audio_s, *beam,
                           (*engine)->Vad() ? "yes" : "no",
                           opts.contains("--codec") ? "Opus 16k" : "none", total.Rate() * 100,
                           run_s / audio_s);
  return 0;
}

}  // namespace

int main(int argc, char **argv) try {
  return Run(std::span(argv, static_cast<std::size_t>(argc)).subspan(1));
} catch (...) {
  static_cast<void>(std::fputs("micnote-wer: failed\n", stderr));
  return 1;
}
