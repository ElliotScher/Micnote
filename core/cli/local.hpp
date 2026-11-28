// The micnote-cli commands that work on this machine's store, through the
// same api::Context (and so the same encrypted database, job queue and C
// ABI methods) the app uses:
//   db   recordings, transcripts and search;
//   run  the daemon: sync when the puck appears, transcribe what arrives.
// The store lives in a data directory ($XDG_DATA_HOME/micnote by default);
// its secrets (db_key, host.key, the pucks' link keys) are where the other
// commands keep theirs (keys.hpp), so it sees what `keygen` and `pair` wrote.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "api/context.hpp"
#include "commands.hpp"

namespace micnote::cli {

inline constexpr std::string_view kDbUsage =
    "       micnote-cli db recordings  [--data DIR]\n"
    "       micnote-cli db transcripts [--recording ID] [--data DIR]\n"
    "       micnote-cli db items       --recording ID [--data DIR]\n"
    "       micnote-cli db actions     [--recording ID] [--data DIR]\n"
    "       micnote-cli db search QUERY [--data DIR]\n";

inline constexpr std::string_view kCloudUsage =
    "       micnote-cli cloud  key                      (reads your Claude API key from stdin)\n"
    "       micnote-cli cloud  preview --item ID [--data DIR]\n"
    "       micnote-cli cloud  draft   --item ID --hash HASH [--data DIR]\n";

inline constexpr std::string_view kPlanUsage =
    "       micnote-cli plan   --recording ID [--llm MODEL.gguf] [--data DIR]\n";

#if defined(MICNOTE_HAVE_ASR)
inline constexpr std::string_view kRunUsage =
    "       micnote-cli run    --usb PORT [--data DIR] [--models DIR] [--backend B] "
    "[--llm MODEL.gguf]\n";
#else
inline constexpr std::string_view kRunUsage =
    "       micnote-cli run    --usb PORT [--data DIR] [--llm MODEL.gguf]\n";
#endif

// args without argv[0], args[0] being "db" / "run". Exit codes as Run().
int Db(std::span<const std::string_view> args, const Env &env);
int Plan(std::span<const std::string_view> args, const Env &env);
int Cloud(std::span<const std::string_view> args, const Env &env);
int RunDaemon(std::span<const std::string_view> args, const Env &env);

// $XDG_DATA_HOME/micnote, else ~/.local/share/micnote (%LOCALAPPDATA%\micnote
// on Windows); empty if none of those is set.
std::filesystem::path DefaultDataDir(const std::function<const char *(const char *)> &getenv);

// Opens the store in data_dir (env.data_dir if empty) with the CLI's secrets. Explains a failure on
// env.err and returns exit code 1.
std::expected<std::unique_ptr<api::Context>, int> OpenStore(const Env &env,
                                                            const std::filesystem::path &data_dir,
                                                            api::ContextOptions options,
                                                            mn_event_fn on_event = nullptr,
                                                            void *user_data = nullptr);

class EventInbox;

// Prints recording `id`'s transcript; false (after saying why) if there's none.
bool PrintTranscript(api::Context &ctx, std::int64_t id, const Env &env);

// Prints a recording's action drafts (all recordings' without an id), as the
// user would have to confirm them; false after saying why not.
bool PrintActions(api::Context &ctx, std::optional<std::int64_t> recording, const Env &env);

// Prints one action from actions.list or actions.get, as PrintActions does.
void PrintAction(const nlohmann::json &action, const Env &env);

// Submits a job and waits: true once it's done. A failure, or Ctrl-C, is
// explained and cancels it, so it isn't retried later.
bool RunJob(api::Context &ctx, EventInbox &inbox, std::string_view method,
            const nlohmann::json &params, std::string_view doing, const Env &env);

// What the CLI knows for the planner's lookups: contacts.csv ("Name,
// address; address" lines) and repos.txt (owner/name lines) in dir.
nlohmann::json LocalContext(const std::filesystem::path &dir);

// Minutes this machine's time zone is east of UTC at a time.
int LocalUtcOffset(std::int64_t unix_ms);

// The language model: env.llm, or the --llm file in builds with one. Explains
// a failure and returns its exit code.
std::expected<jobs::LlmFactory, int> CliLlm(const std::string *model, const Env &env);

// This machine's lookups and time zone, for a context that plans or drafts.
void SetLookups(api::ContextOptions &options, const Env &env);
// Lets a context plan, with those.
void SetPlanner(api::ContextOptions &options, jobs::LlmFactory llm, const Env &env);

// "mm:ss.mmm".
std::string Clock(std::int64_t ms);
// "2026-11-23 14:05" (UTC).
std::string Date(std::int64_t unix_ms);

// Collects the core's events, which arrive on its worker threads, for the
// command's own thread to read.
class EventInbox {
 public:
  static void Callback(void *user_data, const char *event_json);
  std::vector<std::string> Take();

 private:
  std::mutex mu_;
  std::vector<std::string> events_;
};

}  // namespace micnote::cli
