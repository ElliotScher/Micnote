// `micnote-cli cloud`: drafting one item with Claude (stage 3 §4.5), the way
// the app will: look first, then send exactly that.
//   cloud key       reads your API key from standard input (so it isn't in
//                   your shell's history) and keeps it with the CLI's other
//                   secrets: the OS keychain, or a file;
//   cloud preview   prints the request that would be sent for an item: the
//                   redacted prompt, the schema, its size and its hash;
//   cloud draft     sends it, if it's still what that hash says, and prints
//                   the new draft. Builds without HTTPS can only preview.
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <string>

#include "keys.hpp"
#include "local.hpp"
#include <nlohmann/json.hpp>
#if defined(MICNOTE_HAVE_HTTPS)
#include "http/https_client.hpp"
#endif

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsageError = 2;

int Usage(const Env &env) {
  *env.err << "usage:\n" << kCloudUsage;
  return kUsageError;
}

int SaveKey(const Env &env) {
  std::string key;
  if (env.in != nullptr) {
    std::getline(*env.in, key);
  }
  const auto blank = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r'; };
  while (!key.empty() && blank(static_cast<unsigned char>(key.back()))) {
    key.pop_back();
  }
  key.erase(key.begin(), std::ranges::find_if_not(key, blank));
  if (key.empty()) {
    *env.err << "micnote-cli: no key on standard input (`micnote-cli cloud key < FILE`)\n";
    return kFailed;
  }
  auto keys = OpenKeys(std::nullopt, env);
  if (!keys) {
    return keys.error();
  }
  const auto bytes = std::as_bytes(std::span(key));
  if (!keys->store->Set(jobs::kClaudeKeySecret, bytes)) {
    *env.err << std::format("micnote-cli: can't save the key to {}\n",
                            keys->Where(jobs::kClaudeKeySecret));
    return kFailed;
  }
  *env.out << std::format("saved the Claude API key: {}\n", keys->Where(jobs::kClaudeKeySecret));
  return kOk;
}

}  // namespace

int Cloud(std::span<const std::string_view> args, const Env &env) {
  if (args.size() < 2) {
    return Usage(env);
  }
  const std::string_view what = args[1];
  if (what == "key") {
    return args.size() == 2 ? SaveKey(env) : Usage(env);
  }
  const bool draft = what == "draft";
  if (what != "preview" && !draft) {
    return Usage(env);
  }
  std::map<std::string, std::string, std::less<>> opts;
  for (std::size_t i = 2; i < args.size(); i += 2) {
    const std::string_view o = args[i];
    const bool known = o == "--item" || o == "--data" || (draft && o == "--hash");
    if (!known || i + 1 == args.size() || !opts.emplace(o, args[i + 1]).second) {
      return Usage(env);
    }
  }
  std::int64_t item = 0;
  const std::string id = opts["--item"];
  const auto [ptr, ec] = std::from_chars(id.data(), id.data() + id.size(), item);
  if (ec != std::errc() || ptr != id.data() + id.size() || (draft && !opts.contains("--hash"))) {
    if (ec != std::errc() || ptr != id.data() + id.size()) {
      *env.err << "micnote-cli: --item takes an item's ID (see `micnote-cli db items`)\n";
    }
    return Usage(env);
  }

  api::ContextOptions options;
  options.open = env.open;
  // The preview and the draft must see the same lookups, or the hashes differ.
  SetLookups(options, env);
  options.http = env.http;
#if defined(MICNOTE_HAVE_HTTPS)
  if (!options.http) {
    options.http = std::make_shared<http::CurlClient>();  // GCOVR_EXCL_LINE: not in tests
  }
#endif
  options.claude_base_url = env.claude_base_url;
  options.claude_wait = env.wait;
  options.run_jobs = draft;
  EventInbox inbox;
  fs::path data;
  if (const auto it = opts.find("--data"); it != opts.end()) {
    data = it->second;
  }
  auto ctx = OpenStore(env, data, std::move(options), &EventInbox::Callback, &inbox);
  if (!ctx) {
    return ctx.error();
  }
  const auto preview = (*ctx)->Call("planner.cloud_preview@1",
                                    json{{"item", item}}.dump());  // GCOVR_EXCL_BR_LINE: json
  const json p = json::parse(preview.json);
  if (preview.status != MN_OK) {
    *env.err << std::format("micnote-cli: {}\n", p["error"]["message"].get<std::string>());
    return kFailed;
  }
  if (!draft) {
    // GCOVR_EXCL_BR_START: gcc's exception cleanup of the json and strings.
    *env.out << p["body"].get<std::string>() << "\n\n";
    *env.out << std::format(
        "{} bytes to {}; {} things replaced by placeholders (names are not, unless "
        "planner.redact.names is on)\nhash: {}\n",
        p["bytes"].get<std::size_t>(), p["model"].get<std::string>(),
        p["redactions"].get<std::size_t>(), p["hash"].get<std::string>());
    if (p["can_send"].get<bool>()) {
      *env.out << std::format("to send it: micnote-cli cloud draft --item {} --hash {}\n", item,
                              p["hash"].get<std::string>());
    } else {
      *env.out << "this build can't send it (configure with -DMICNOTE_HTTPS=ON)\n";
    }
    // GCOVR_EXCL_BR_STOP
    return kOk;
  }
  if (!p["can_send"].get<bool>()) {
    *env.err << "micnote-cli: this build can't reach the Claude API (configure with "
                "-DMICNOTE_HTTPS=ON)\n";
    return kFailed;
  }
  const json params = {{"item", item},
                       {"preview_hash", opts["--hash"]}};  // GCOVR_EXCL_BR_LINE: json
  if (!RunJob(**ctx, inbox, "planner.cloud_draft@1", params, "drafting with Claude", env)) {
    return kFailed;
  }
  // Held: a loop over a temporary's member dangles before C++23 (gcc 14).
  const json list = json::parse((*ctx)->Call("actions.list@1", "{}").json);
  for (const json &a : list["actions"]) {
    if (a["item"] == item) {
      PrintAction(a, env);
    }
  }
  return kOk;
}

}  // namespace micnote::cli
