// `micnote-cli db`: what the store holds, through the C ABI's own methods.
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <string>

#include "local.hpp"
#include <nlohmann/json.hpp>

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsageError = 2;

// The result of a method call, or nullopt after printing its error.
std::optional<json> CallOrSay(api::Context &ctx, std::string_view method, const json &params,
                              const Env &env) {
  const auto r = ctx.Call(method, params.dump());
  json body = json::parse(r.json);
  if (r.status != MN_OK) {
    *env.err << std::format("micnote-cli: {}\n", body["error"]["message"].get<std::string>());
    return std::nullopt;
  }
  return body;
}

// "m:ss"; a 90-minute meeting is "90:00".
std::string Length(std::int64_t ms) {
  const std::int64_t s = ms / 1000;
  return std::format("{}:{:02}", s / 60, s % 60);
}

// The states a recording has once it's been transcribed.
bool HasTranscript(const json &r) {
  constexpr std::array<std::string_view, 2> kStates = {"transcribed", "planned"};
  return std::ranges::find(kStates, r["state"].get<std::string>()) != kStates.end();
}

int Recordings(api::Context &ctx, const fs::path &dir, const Env &env) {
  const auto list = CallOrSay(ctx, "recordings.list@1", json::object(), env);
  if (!list) {
    return kFailed;
  }
  const json &recs = (*list)["recordings"];
  if (recs.empty()) {
    *env.out << std::format("no recordings in {} yet\n", dir.string());
    return kOk;
  }
  *env.out << std::format("{:>4}  {:<16}  {:<16}  {:<8}  {:>8}  {}\n", "ID", "RECORDING",
                          "STARTED (UTC)", "TIME", "LENGTH", "STATE");
  for (const json &r : recs) {
    const json &start = r["start_unix_ms"];
    // GCOVR_EXCL_BR_START: both sides run (db recordings tests); the rest is
    // gcc's exception cleanup of the temporary strings.
    const std::string started = start.is_null() ? "unknown" : Date(start.get<std::int64_t>());
    // GCOVR_EXCL_BR_STOP
    *env.out << std::format(
        "{:>4}  {:016x}  {:<16}  {:<8}  {:>8}  {}\n", r["id"].get<std::int64_t>(),
        r["rec_id"].get<std::uint64_t>(), started, r["time_confidence"].get<std::string>(),
        Length(r["duration_ms"].get<std::int64_t>()), r["state"].get<std::string>());
  }
  return kOk;
}

// --recording's value; nullopt after saying it isn't an ID.
std::optional<std::int64_t> RecordingId(const std::string &s, const Env &env) {
  std::int64_t id = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), id);
  if (ec != std::errc() || ptr != s.data() + s.size()) {
    *env.err << "micnote-cli: --recording takes a recording's ID (see `micnote-cli db "
                "recordings`)\n";
    return std::nullopt;
  }
  return id;
}

int Transcripts(api::Context &ctx, std::optional<std::int64_t> only, const Env &env) {
  if (only) {
    return PrintTranscript(ctx, *only, env) ? kOk : kFailed;
  }
  const auto list = CallOrSay(ctx, "recordings.list@1", json::object(), env);
  if (!list) {
    return kFailed;
  }
  bool any = false;
  for (const json &r : (*list)["recordings"]) {
    if (HasTranscript(r)) {
      // GCOVR_EXCL_START: the row just said it has a transcript.
      if (!PrintTranscript(ctx, r["id"].get<std::int64_t>(), env)) {
        return kFailed;
      }
      // GCOVR_EXCL_STOP
      any = true;
    }
  }
  if (!any) {
    *env.out << "no transcripts yet\n";
  }
  return kOk;
}

int Search(api::Context &ctx, const std::string &query, const Env &env) {
  const auto hits =
      CallOrSay(ctx, "search.query@1", json{{"q", query}}, env);  // GCOVR_EXCL_BR_LINE: json
  if (!hits) {
    return kFailed;
  }
  if ((*hits)["hits"].empty()) {
    *env.out << "no matches\n";
  }
  for (const json &h : (*hits)["hits"]) {
    *env.out << std::format("recording {} [{}] {}\n", h["recording"].get<std::int64_t>(),
                            Clock(h["start_ms"].get<std::int64_t>()), h["text"].get<std::string>());
  }
  return kOk;
}

}  // namespace

int Db(std::span<const std::string_view> args, const Env &env) {
  const auto usage = [&] {
    *env.err << "usage:\n" << kDbUsage;
    return kUsageError;
  };
  if (args.size() < 2) {
    return usage();
  }
  const std::string_view what = args[1];
  const bool search = what == "search";
  const bool actions = what == "actions";
  if (what != "recordings" && what != "transcripts" && !actions && !search) {
    return usage();
  }
  // search's query is the one positional argument.
  const std::size_t first_option = search ? 3 : 2;
  if (search && args.size() < 3) {
    return usage();
  }
  std::map<std::string, std::string, std::less<>> opts;
  for (std::size_t i = first_option; i < args.size(); i += 2) {
    const std::string_view o = args[i];
    const bool known = o == "--data" || ((what == "transcripts" || actions) && o == "--recording");
    if (!known || i + 1 == args.size() || !opts.emplace(o, args[i + 1]).second) {
      return usage();
    }
  }
  std::optional<std::int64_t> id;
  if (const auto it = opts.find("--recording"); it != opts.end()) {
    id = RecordingId(it->second, env);
    if (!id) {
      return kUsageError;
    }
  }
  api::ContextOptions options;
  options.run_jobs = false;  // only reading: leave queued jobs alone
  fs::path dir;
  if (const auto it = opts.find("--data"); it != opts.end()) {
    dir = it->second;
  }
  auto ctx = OpenStore(env, dir, std::move(options));
  if (!ctx) {
    return ctx.error();
  }
  if (what == "recordings") {
    return Recordings(**ctx, dir.empty() ? env.data_dir : dir, env);
  }
  if (search) {
    return Search(**ctx, std::string(args[2]), env);
  }
  if (actions) {
    return PrintActions(**ctx, id, env) ? kOk : kFailed;
  }
  return Transcripts(**ctx, id, env);
}

}  // namespace micnote::cli
