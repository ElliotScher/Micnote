#include "local.hpp"

#include <chrono>
#include <format>
#include <optional>
#include <utility>

#include "keys.hpp"
#include <nlohmann/json.hpp>

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

}  // namespace

std::filesystem::path DefaultDataDir(const std::function<const char *(const char *)> &getenv) {
  const auto set = [&](const char *name) -> std::optional<fs::path> {
    const char *value = getenv(name);
    if (value == nullptr || *value == '\0') {
      return std::nullopt;
    }
    return fs::path(value);
  };
#if defined(_WIN32)
  if (const auto local = set("LOCALAPPDATA")) {
    return *local / "micnote";
  }
#else
  if (const auto xdg = set("XDG_DATA_HOME")) {
    return *xdg / "micnote";
  }
  if (const auto home = set("HOME")) {
    return *home / ".local" / "share" / "micnote";
  }
#endif
  return {};
}

std::expected<std::unique_ptr<api::Context>, int> OpenStore(const Env &env,
                                                            const fs::path &data_dir,
                                                            api::ContextOptions options,
                                                            mn_event_fn on_event, void *user_data) {
  const fs::path data = data_dir.empty() ? env.data_dir : data_dir;
  if (data.empty()) {
    *env.err << "micnote-cli: no home directory for the store; pass --data DIR\n";
    return std::unexpected(1);
  }
  auto keys = OpenKeys(std::nullopt, env);
  if (!keys) {
    return std::unexpected(keys.error());
  }
  auto ctx =
      api::Context::Open(data, std::move(keys->store), on_event, user_data, std::move(options));
  if (!ctx) {
    *env.err << std::format("micnote-cli: can't open the store in {}: {}\n", data.string(),
                            ctx.error().message);
    return std::unexpected(1);
  }
  return std::move(*ctx);
}

bool PrintTranscript(api::Context &ctx, std::int64_t id, const Env &env) {
  const auto r =
      ctx.Call("transcripts.get@1", json{{"recording", id}}.dump());  // GCOVR_EXCL_BR_LINE: json
  const json t = json::parse(r.json);
  if (r.status != MN_OK) {
    *env.err << std::format("micnote-cli: {}\n", t["error"]["message"].get<std::string>());
    return false;
  }
  *env.out << std::format("recording {}: {} on {}, RTF {:.2f}\n", id, t["model"].get<std::string>(),
                          t["backend"].get<std::string>(), t["rtf"].get<double>());
  for (const json &s : t["segments"]) {
    *env.out << std::format("  [{} --> {}] {}\n", Clock(s["start_ms"].get<std::int64_t>()),
                            Clock(s["end_ms"].get<std::int64_t>()), s["text"].get<std::string>());
  }
  return true;
}

std::string Clock(std::int64_t ms) {
  return std::format("{:02}:{:02}.{:03}", ms / 60000, ms / 1000 % 60, ms % 1000);
}

std::string Date(std::int64_t unix_ms) {
  using namespace std::chrono;
  const sys_time<milliseconds> t{milliseconds(unix_ms)};
  const auto day = floor<days>(t);
  const year_month_day ymd{day};
  const hh_mm_ss hms{floor<minutes>(t - day)};
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}", static_cast<int>(ymd.year()),
                     static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                     hms.hours().count(), hms.minutes().count());
}

void EventInbox::Callback(void *user_data, const char *event_json) {
  auto *self = static_cast<EventInbox *>(user_data);
  const std::scoped_lock lock(self->mu_);
  self->events_.emplace_back(event_json);
}

std::vector<std::string> EventInbox::Take() {
  const std::scoped_lock lock(mu_);
  return std::exchange(events_, {});
}

}  // namespace micnote::cli
