// `micnote-cli run`: the daemon of stage 2 §4.9. It watches the puck's USB
// port and queues a sync each time the port appears (and at start, if it's
// already there); the core's job queue does the rest, as it will in the app:
// the sync stores each new recording and, in builds with speech recognition,
// queues its transcription. Until Ctrl-C, it reports what happens.
// BLE discovery comes with stage 5; until then the puck is found by its port.
#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <string>

#include "local.hpp"
#include <nlohmann/json.hpp>
#if defined(MICNOTE_HAVE_ASR)
#include "transcribe.hpp"
#endif

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr auto kPoll = std::chrono::seconds(1);

// One event, as a line for the person watching. `syncing` turns false
// when a sync job ends, however it ends, so the next appearance starts one.
void Report(const std::string &event_json, bool &syncing, const Env &env) {
  const json e = json::parse(event_json);
  const std::string type = e["type"].get<std::string>();
  const bool sync_job = e.value("kind", "") == "sync";
  if (type == "recording.new") {
    *env.out << std::format("stored recording {}\n", e["id"].get<std::int64_t>());
  } else if (type == "transcript.ready") {
    *env.out << std::format("transcribed recording {}\n", e["recording"].get<std::int64_t>());
  } else if (type == "items.ready") {
    *env.out << std::format("planned recording {}\n", e["recording"].get<std::int64_t>());
  } else if (type == "job.failed") {
    *env.out << std::format("{} failed: {}{}\n", e["kind"].get<std::string>(),
                            e["error"].get<std::string>(),
                            e["retry_at"].is_null() ? "" : " (it will be retried)");
  } else if (type == "job.done" && sync_job) {
    *env.out << "sync finished\n";
  }
  if (sync_job && type != "job.progress") {
    syncing = false;
  }
}

}  // namespace

int RunDaemon(std::span<const std::string_view> args, const Env &env) {
#if defined(MICNOTE_HAVE_ASR)
  constexpr bool kHaveAsr = true;
#else
  constexpr bool kHaveAsr = false;
#endif
#if defined(MICNOTE_HAVE_ASR)
  constexpr std::array<std::string_view, 5> kOptions = {"--usb", "--data", "--models", "--llm",
                                                        "--backend"};
#else
  constexpr std::array<std::string_view, 3> kOptions = {"--usb", "--data", "--llm"};
#endif
  std::map<std::string, std::string, std::less<>> opts;
  for (std::size_t i = 1; i < args.size(); i += 2) {
    const std::string_view o = args[i];
    const bool known = std::ranges::find(kOptions, o) != kOptions.end();
    if (!known || i + 1 == args.size() || !opts.emplace(o, args[i + 1]).second) {
      *env.err << "usage:\n" << kRunUsage;
      return 2;
    }
  }
  if (!opts.contains("--usb")) {
    *env.err << "usage:\n" << kRunUsage;
    return 2;
  }
  const std::string port = opts.at("--usb");
  api::ContextOptions options;
  options.open = env.open;
  options.engine = env.engine;
#if defined(MICNOTE_HAVE_ASR)
  fs::path models = env.models_dir;
  if (const auto it = opts.find("--models"); it != opts.end()) {
    models = it->second;
  }
  const auto backend = opts.find("--backend");
  if (backend != opts.end()) {
    if (const std::string why = CheckBackend(backend->second); !why.empty()) {
      *env.err << std::format("micnote-cli: {}\n", why);
      return 2;
    }
  }
  if (!options.engine) {
    if (auto engine =
            RegistryEngine(env, models, backend != opts.end() ? backend->second : "auto")) {
      options.engine = std::move(*engine);
    }
  }
#endif
  const auto llm = opts.find("--llm");
  if (env.llm || llm != opts.end()) {
    auto made = CliLlm(llm != opts.end() ? &llm->second : nullptr, env);
    if (!made) {
      return made.error();
    }
    SetPlanner(options, std::move(*made), env);
  }
  const bool transcribes = static_cast<bool>(options.engine);
  EventInbox inbox;
  fs::path data;
  if (const auto it = opts.find("--data"); it != opts.end()) {
    data = it->second;
  }
  auto ctx = OpenStore(env, data, std::move(options), &EventInbox::Callback, &inbox);
  if (!ctx) {
    return ctx.error();
  }
  *env.out << std::format("watching {} for the puck; Ctrl-C to stop\n", port);
  if (!transcribes) {
    *env.out << (kHaveAsr ? "no speech model (see `micnote-cli models list`): recordings are "
                            "synced but not transcribed\n"
                          : "this build has no speech recognition: recordings are synced but "
                            "not transcribed\n");
  }

  const auto present = [&] {
    if (env.port_present) {
      return env.port_present(port);
    }
    std::error_code ec;
    return fs::exists(port, ec);
  };
  bool was_present = false;
  bool syncing = false;
  // Polls until Ctrl-C; the first poll is straight away.
  for (bool more = true; more; more = env.wait(kPoll)) {
    for (const std::string &e : inbox.Take()) {
      Report(e, syncing, env);
    }
    const bool now_present = present();
    if (now_present && !was_present && !syncing) {
      const auto s =
          (*ctx)->Submit("sync.run@1", json{{"usb", port}}.dump());  // GCOVR_EXCL_BR_LINE: json
      if (s.status != MN_OK) {
        *env.err << std::format("micnote-cli: can't queue a sync: {}\n",
                                json::parse(s.error_json)["error"]["message"].get<std::string>());
        return 1;
      }
      syncing = true;
      *env.out << std::format("puck on {}: syncing\n", port);
    }
    was_present = now_present;
  }
  *env.out << "stopped\n";
  return 0;
}

}  // namespace micnote::cli
