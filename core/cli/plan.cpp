// `micnote-cli plan`: splits a stored recording's transcript into items and
// drafts their actions, on the store's job queue (as the app does it), then
// prints them. Nothing is sent or changed: drafts only.
#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>

#include "local.hpp"
#include <nlohmann/json.hpp>
#if defined(MICNOTE_HAVE_LLM)
#include "planner/llm/llama_backend.hpp"
#endif

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::string Trimmed(std::string_view s) {
  constexpr std::string_view kSpace = " \t\r";
  const auto a = s.find_first_not_of(kSpace);
  if (a == std::string_view::npos) {
    return {};
  }
  return std::string(s.substr(a, s.find_last_not_of(kSpace) - a + 1));
}

// `text` split at each `at`, trimmed, without empty parts.
// GCOVR_EXCL_BR_START: gcc's exception cleanup of the strings.
std::vector<std::string> Parts(std::string_view text, char at) {
  std::vector<std::string> out;
  while (!text.empty()) {
    const auto end = std::min(text.find(at), text.size());
    std::string part = Trimmed(text.substr(0, end));
    if (!part.empty()) {
      out.push_back(std::move(part));
    }
    text.remove_prefix(std::min(end + 1, text.size()));
  }
  return out;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup
// GCOVR_EXCL_BR_STOP

// The file's lines, without blank ones and # comments; none if it isn't there.
std::vector<std::string> FileLines(const fs::path &path) {
  std::vector<std::string> out;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (std::string t = Trimmed(line); !t.empty() && t.front() != '#') {
      out.push_back(std::move(t));
    }
  }
  return out;
}

}  // namespace

json LocalContext(const fs::path &dir) {
  // GCOVR_EXCL_BR_START: gcc's exception cleanup of the json temporaries.
  json contacts = json::array();
  for (const std::string &line : FileLines(dir / "contacts.csv")) {
    const auto fields = Parts(line, ',');
    if (fields.size() >= 2) {
      contacts.push_back({{"name", fields[0]}, {"emails", Parts(fields[1], ';')}});
    }
  }
  json repos = json::array();
  for (const std::string &line : FileLines(dir / "repos.txt")) {
    repos.push_back({{"full_name", line}});
  }
  return json{{"contacts", std::move(contacts)}, {"repos", std::move(repos)}};
  // GCOVR_EXCL_BR_STOP
}

int LocalUtcOffset(std::int64_t unix_ms) {
  const auto t = static_cast<std::time_t>(unix_ms / 1000);
  std::tm utc{};
  std::tm local{};
#if defined(_WIN32)
  const bool ok = gmtime_s(&utc, &t) == 0 && localtime_s(&local, &t) == 0;
#else
  const bool ok =
      gmtime_r(&t, &utc) != nullptr && localtime_r(&t, &local) != nullptr;  // GCOVR_EXCL_BR_LINE
#endif
  if (!ok) {   // GCOVR_EXCL_BR_LINE: only for a time the C library can't represent
    return 0;  // GCOVR_EXCL_LINE
  }
  // The two differ by less than a day, so the sign of the day's difference
  // is enough (new year included).
  int days = local.tm_yday - utc.tm_yday;
  if (local.tm_year != utc.tm_year) {  // GCOVR_EXCL_BR_LINE: needs a time zone and new year
    days = local.tm_year > utc.tm_year ? 1 : -1;  // GCOVR_EXCL_LINE
  }
  return (days * 1440) + ((local.tm_hour - utc.tm_hour) * 60) + local.tm_min - utc.tm_min;
}

std::expected<jobs::LlmFactory, int> CliLlm(const std::string *model, const Env &env) {
  if (env.llm) {
    return env.llm;
  }
#if defined(MICNOTE_HAVE_LLM)
  if (model == nullptr) {
    *env.err << "micnote-cli: pass --llm MODEL.gguf, an instruct language model\n";
    return std::unexpected(2);
  }
  auto loaded = planner::LlamaLlm::Load(*model);
  if (!loaded) {
    *env.err << std::format("micnote-cli: can't load {}\n", *model);
    return std::unexpected(1);
  }
  std::shared_ptr<planner::Llm> llm = std::move(*loaded);
  return [llm] { return std::expected<std::shared_ptr<planner::Llm>, planner::LlmError>(llm); };
#else
  static_cast<void>(model);
  *env.err << "micnote-cli: this build has no language model\n";
  return std::unexpected(1);
#endif
}

void SetPlanner(api::ContextOptions &options, jobs::LlmFactory llm, const Env &env) {
  options.llm = std::move(llm);
  const fs::path dir = env.default_key.parent_path();
  options.fetch = [dir](const planner::Item & /*item*/) { return LocalContext(dir); };
  options.utc_offset = LocalUtcOffset;
}

int Plan(std::span<const std::string_view> args, const Env &env) {
  std::map<std::string, std::string, std::less<>> opts;
  constexpr std::array<std::string_view, 3> kOptions = {"--recording", "--data", "--llm"};
  for (std::size_t i = 1; i < args.size(); i += 2) {
    const bool known = std::ranges::find(kOptions, args[i]) != kOptions.end();
    if (!known || i + 1 == args.size() || !opts.emplace(args[i], args[i + 1]).second) {
      *env.err << "usage:\n" << kPlanUsage;
      return 2;
    }
  }
  std::int64_t id = 0;
  const std::string recording = opts["--recording"];
  const auto [ptr, ec] = std::from_chars(recording.data(), recording.data() + recording.size(), id);
  if (ec != std::errc() || ptr != recording.data() + recording.size()) {
    *env.err << "usage:\n" << kPlanUsage;
    return 2;
  }
  const auto model = opts.find("--llm");
  auto llm = CliLlm(model != opts.end() ? &model->second : nullptr, env);
  if (!llm) {
    return llm.error();
  }
  api::ContextOptions options;
  SetPlanner(options, std::move(*llm), env);
  EventInbox inbox;
  const auto data = opts.find("--data");
  auto ctx = OpenStore(env, data != opts.end() ? fs::path(data->second) : fs::path(),
                       std::move(options), &EventInbox::Callback, &inbox);
  if (!ctx) {
    return ctx.error();
  }
  if (!RunJob(**ctx, inbox, "planner.plan@1", id, "planning", env)) {
    return 1;
  }
  return PrintActions(**ctx, id, env) ? 0 : 1;  // GCOVR_EXCL_BR_LINE: db actions tests 1
}

}  // namespace micnote::cli
