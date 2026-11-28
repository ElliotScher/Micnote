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

bool PrintActions(api::Context &ctx, std::optional<std::int64_t> recording, const Env &env) {
  json params = json::object();
  if (recording) {
    params["recording"] = *recording;
  }
  const auto r = ctx.Call("actions.list@1", params.dump());
  const json body = json::parse(r.json);
  if (r.status != MN_OK) {
    *env.err << std::format("micnote-cli: {}\n", body["error"]["message"].get<std::string>());
    return false;
  }
  if (body["actions"].empty()) {
    *env.out << "no actions\n";
  }
  for (const json &a : body["actions"]) {
    PrintAction(a, env);
  }
  return true;
}

void PrintAction(const json &a, const Env &env) {
  // GCOVR_EXCL_BR_START: gcc's exception cleanup of the json and strings.
  *env.out << std::format("action {} (recording {}, item {}): {}.{}, {} by {}, revision {}\n",
                          a["id"].get<std::int64_t>(), a["recording"].get<std::int64_t>(),
                          a["item"].get<std::int64_t>(), a["connector"].get<std::string>(),
                          a["type"].get<std::string>(), a["state"].get<std::string>(),
                          a["author"].get<std::string>(), a["revision"].get<std::int64_t>());
  for (const auto &[key, value] : a["draft"]["params"].items()) {
    *env.out << std::format("  {}: {}\n", key,
                            value.is_string() ? value.get<std::string>() : value.dump());
  }
  if (!a["draft"]["missing_fields"].empty()) {
    *env.out << std::format("  still needed: {}\n", a["draft"]["missing_fields"].dump());
  }
  // GCOVR_EXCL_BR_STOP
}

bool RunJob(api::Context &ctx, EventInbox &inbox, std::string_view method, const json &params,
            std::string_view doing, const Env &env) {
  const auto job = ctx.Submit(method, params.dump());
  if (job.status != MN_OK) {
    *env.err << std::format("micnote-cli: {}\n",
                            json::parse(job.error_json)["error"]["message"].get<std::string>());
    return false;
  }
  for (;;) {
    for (const std::string &text : inbox.Take()) {
      const json e = json::parse(text);
      if (e.value("job", std::uint64_t{0}) != job.job) {
        continue;
      }
      const std::string type = e["type"].get<std::string>();
      if (type == "job.progress") {
        *env.err << std::format("\r{}... {}%", doing, e["pct"].get<int>()) << std::flush;
      } else if (type == "job.done") {
        *env.err << "\n";
        return true;
      } else {  // failed, or cancelled from elsewhere
        *env.err << std::format("\nmicnote-cli: {} failed: {}\n", doing, e.value("error", type));
        static_cast<void>(ctx.Cancel(job.job));
        return false;
      }
    }
    if (!env.wait(std::chrono::milliseconds(200))) {
      static_cast<void>(ctx.Cancel(job.job));
      *env.err << "\nmicnote-cli: stopped\n";
      return false;
    }
  }
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
