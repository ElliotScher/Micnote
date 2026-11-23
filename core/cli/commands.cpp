#include "commands.hpp"

#include <micnote.h>

#include <array>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "crypto/psa.hpp"
#include "transport/usb_serial.hpp"

namespace micnote::cli {

namespace {

namespace fs = std::filesystem;
using protocol::ClientError;

constexpr std::string_view kUsage =
    "usage: micnote-cli [--version | --help]\n"
    "       micnote-cli keygen [--key FILE]\n"
    "       micnote-cli status --usb PORT [--key FILE]\n"
    "       micnote-cli list   --usb PORT [--key FILE]\n"
    "       micnote-cli sync   --usb PORT --out DIR [--no-ack] [--key FILE]\n";

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsageError = 2;

struct Options {
  std::string command;
  std::map<std::string, std::string, std::less<>> values;
  bool no_ack = false;
};

int Usage(const Env &env) {
  *env.err << kUsage;
  return kUsageError;
}

// The options each command takes; --no-ack is the only flag.
bool Allowed(std::string_view command, std::string_view option) {
  if (option == "--key") {
    return true;
  }
  if (command == "keygen") {
    return false;
  }
  if (option == "--usb") {
    return true;
  }
  return command == "sync" && (option == "--out" || option == "--no-ack");
}

std::optional<Options> Parse(std::span<const std::string_view> args) {
  Options opts;
  opts.command = args.front();
  if (opts.command != "keygen" && opts.command != "status" && opts.command != "list" &&
      opts.command != "sync") {
    return std::nullopt;
  }
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view option = args[i];
    if (!Allowed(opts.command, option)) {
      return std::nullopt;
    }
    if (option == "--no-ack") {
      opts.no_ack = true;
      continue;
    }
    if (i + 1 == args.size() || !opts.values.emplace(option, args[i + 1]).second) {
      return std::nullopt;  // no value, or given twice
    }
    ++i;
  }
  const bool needs_usb = opts.command != "keygen";
  if ((needs_usb && !opts.values.contains("--usb")) ||
      (opts.command == "sync" && !opts.values.contains("--out"))) {
    return std::nullopt;
  }
  return opts;
}

std::string Hex(std::span<const std::byte> bytes) {
  std::string out;
  for (const std::byte b : bytes) {
    out += std::format("{:02x}", std::to_integer<unsigned>(b));
  }
  return out;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `out`

std::string RecordingName(std::uint64_t id) { return std::format("{:016x}", id); }

std::optional<fs::path> KeyPath(const Options &opts, const Env &env) {
  if (const auto it = opts.values.find("--key"); it != opts.values.end()) {
    return fs::path(it->second);
  }
  if (env.default_key.empty()) {
    *env.err << "micnote-cli: no home directory for the host key; pass --key FILE\n";
    return std::nullopt;
  }
  return env.default_key;
}

// GCOVR_EXCL_START: PSA's RNG fails only if the OS has no entropy source.
bool Random(std::span<std::byte> out) {
  return crypto::Init() == PSA_SUCCESS &&
         psa_generate_random(reinterpret_cast<std::uint8_t *>(out.data()), out.size()) ==
             PSA_SUCCESS;
}
// GCOVR_EXCL_STOP

int Keygen(const Options &opts, const Env &env) {
  const auto path = KeyPath(opts, env);
  if (!path) {
    return kFailed;
  }
  std::error_code ec;
  if (fs::exists(*path, ec)) {
    *env.err << std::format("micnote-cli: {} already exists; not overwriting it\n", path->string());
    return kFailed;
  }
  std::array<std::byte, crypto::kX25519KeySize> priv{};
  // GCOVR_EXCL_START: see Random().
  if (!Random(priv)) {
    *env.err << "micnote-cli: no random number source\n";
    return kFailed;
  }
  // GCOVR_EXCL_STOP
  const auto key = recording::HostKey::FromPrivate(priv);
  // Fails harmlessly for a bare file name; the write below reports problems.
  fs::create_directories(path->parent_path(), ec);
  {
    std::ofstream file(*path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(priv.data()), priv.size());
    crypto::Wipe(priv);
    if (!file) {
      *env.err << std::format("micnote-cli: can't write {}\n", path->string());
      return kFailed;
    }
  }
  // Only this user may read it: it decrypts every recording made for this host.
  fs::permissions(*path, fs::perms::owner_read | fs::perms::owner_write, ec);
  *env.out << std::format("wrote host key {}\nhost_id {}\n", path->string(), Hex(key->host_id()));
  return kOk;
}

std::optional<recording::HostKey> LoadKey(const Options &opts, const Env &env) {
  const auto path = KeyPath(opts, env);
  if (!path) {
    return std::nullopt;
  }
  std::ifstream file(*path, std::ios::binary);
  if (!file) {
    *env.err << std::format("micnote-cli: no host key at {}; run: micnote-cli keygen\n",
                            path->string());
    return std::nullopt;
  }
  std::array<std::byte, crypto::kX25519KeySize + 1> buf{};
  file.read(reinterpret_cast<char *>(buf.data()), buf.size());
  if (std::cmp_not_equal(file.gcount(), crypto::kX25519KeySize)) {
    *env.err << std::format("micnote-cli: {} isn't a host key (expected 32 bytes)\n",
                            path->string());
    return std::nullopt;
  }
  auto key = recording::HostKey::FromPrivate(std::span(buf).first<crypto::kX25519KeySize>());
  crypto::Wipe(buf);
  return std::move(*key);
}

// An open link that has said Hello and set the puck's clock.
struct Session {
  recording::HostKey key;
  std::unique_ptr<transport::Transport> link;
  std::unique_ptr<protocol::Client> client;
};

std::optional<Session> Connect(const Options &opts, const Env &env) {
  auto key = LoadKey(opts, env);
  if (!key) {
    return std::nullopt;
  }
  const std::string &port = opts.values.at("--usb");
  auto link = env.open(port);
  if (!link) {
    *env.err << std::format("micnote-cli: can't open {}\n", port);
    return std::nullopt;
  }
  auto client = std::make_unique<protocol::Client>(**link);
  std::array<std::byte, 16> nonce{};
  static_cast<void>(Random(nonce));
  auto hello = client->Hello(key->host_id(), nonce);
  if (!hello) {
    *env.err << std::format("micnote-cli: hello failed: {}\n", Describe(hello.error()));
    return std::nullopt;
  }
  if (auto set = client->SetTime(env.now_unix_ms(), 0); !set) {
    *env.err << std::format("micnote-cli: setting the clock failed: {}\n", Describe(set.error()));
    return std::nullopt;
  }
  // GCOVR_EXCL_BR_START: gcc's exception-cleanup branches for the moves.
  return Session{.key = std::move(*key), .link = std::move(*link), .client = std::move(client)};
  // GCOVR_EXCL_BR_STOP
}

std::string_view ChargeName(micnote_v1_ChargeSource source) {
  switch (source) {
    case micnote_v1_ChargeSource_CHARGE_SOURCE_USB:
      return "charging over USB";
    case micnote_v1_ChargeSource_CHARGE_SOURCE_QI:
      return "charging on Qi";
    default:
      return "on battery";
  }
}

int Status(const Options &opts, const Env &env) {
  auto session = Connect(opts, env);
  if (!session) {
    return kFailed;
  }
  const auto s = session->client->GetStatus();
  if (!s) {
    *env.err << std::format("micnote-cli: status failed: {}\n", Describe(s.error()));
    return kFailed;
  }
  *env.out << std::format(
      "battery   {}% ({} mV), {}\n"
      "storage   {} of {} KB used, {} recordings\n"
      "firmware  {}.{}.{}, boot {}, up {} s\n"
      "clock     {}\n",
      s->battery_pct, s->battery_mv, ChargeName(s->charge_source), s->storage_used_kb,
      s->storage_total_kb, s->recording_count, s->fw_version.major, s->fw_version.minor,
      s->fw_version.patch, s->boot_count, s->uptime_ms / 1000, s->rtc_valid ? "set" : "not set");
  return kOk;
}

int List(const Options &opts, const Env &env) {
  auto session = Connect(opts, env);
  if (!session) {
    return kFailed;
  }
  const auto list = session->client->ListRecordings();
  if (!list) {
    *env.err << std::format("micnote-cli: list failed: {}\n", Describe(list.error()));
    return kFailed;
  }
  if (list->empty()) {
    *env.out << "no recordings\n";
  }
  for (const auto &r : *list) {
    // Duration and markers are in the encrypted trailer: the puck reports 0
    // for them, and only sync (which decrypts) can show them.
    // GCOVR_EXCL_START: every line runs; gcc pins the exception cleanup of
    // the format temporaries to one of them.
    *env.out << std::format("{}  {:>8} bytes  {:>4} chunks{}\n", RecordingName(r.id), r.size_bytes,
                            r.last_seq + 1, r.complete ? "" : "  (recording)");
    // GCOVR_EXCL_STOP
  }
  return kOk;
}

bool WriteFile(const fs::path &path, std::span<const std::byte> bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(file);
}

int Sync(const Options &opts, const Env &env) {
  const fs::path out = opts.values.at("--out");
  std::error_code ec;
  fs::create_directories(out, ec);
  if (!fs::is_directory(out, ec)) {
    *env.err << std::format("micnote-cli: can't create {}\n", out.string());
    return kFailed;
  }
  auto session = Connect(opts, env);
  if (!session) {
    return kFailed;
  }
  protocol::Client &client = *session->client;
  const auto list = client.ListRecordings();
  if (!list) {
    *env.err << std::format("micnote-cli: list failed: {}\n", Describe(list.error()));
    return kFailed;
  }

  std::size_t synced = 0;
  for (const auto &info : *list) {
    const std::string name = RecordingName(info.id);
    if (!info.complete) {
      *env.out << std::format("{}: still recording, skipped\n", name);
      continue;
    }
    protocol::FetchedRecording rec;
    rec.id = info.id;
    if (auto fetched = client.Fetch(rec); !fetched) {
      // The link is in an unknown state now: stop, and resume next time.
      *env.err << std::format("micnote-cli: {}: fetch failed: {}\n", name,
                              Describe(fetched.error()));
      return kFailed;
    }
    const std::vector<std::byte> file = rec.File();
    const auto read = recording::ReadRecording(file, session->key);
    if (!read) {
      *env.out << std::format("{}: can't decrypt ({}); kept on the puck\n", name,
                              Describe(read.error()));
      continue;
    }
    const fs::path path = out / (name + ".mnr");
    if (!WriteFile(path, file)) {
      *env.err << std::format("micnote-cli: {}: can't write {}; kept on the puck\n", name,
                              path.string());
      continue;
    }
    *env.out << std::format("{}: {} packets, {:.1f} s, {} markers -> {}\n", name,
                            read->packets.size(), read->DurationMs() / 1000.0, read->markers.size(),
                            path.string());
    if (opts.no_ack) {
      *env.out << std::format("{}: kept on the puck (--no-ack)\n", name);
    } else if (auto acked = client.Ack(info.id, crypto::Sha256(file)); !acked) {
      *env.err << std::format("micnote-cli: {}: ack failed: {}; kept on the puck\n", name,
                              Describe(acked.error()));
      continue;
    } else {
      *env.out << std::format("{}: acked, deleted from the puck\n", name);
    }
    ++synced;
  }
  *env.out << std::format("synced {} of {} recordings\n", synced, list->size());
  return synced == list->size() ? kOk : kFailed;
}

}  // namespace

int Run(std::span<const std::string_view> args, const Env &env) {
  if (args.size() == 1 && (args.front() == "--version" || args.front() == "-V")) {
    *env.out << std::format("micnote-cli {}\n", mn_version());
    return kOk;
  }
  if (args.size() == 1 && (args.front() == "--help" || args.front() == "-h")) {
    *env.out << kUsage;
    return kOk;
  }
  if (args.empty()) {
    return Usage(env);
  }
  const auto opts = Parse(args);
  if (!opts) {
    return Usage(env);
  }
  if (opts->command == "keygen") {
    return Keygen(*opts, env);
  }
  if (opts->command == "status") {
    return Status(*opts, env);
  }
  if (opts->command == "list") {
    return List(*opts, env);
  }
  return Sync(*opts, env);
}

std::filesystem::path DefaultKeyPath(const std::function<const char *(const char *)> &getenv) {
  const auto set = [&](const char *name) -> std::optional<fs::path> {
    const char *value = getenv(name);
    if (value == nullptr || *value == '\0') {
      return std::nullopt;
    }
    return fs::path(value);
  };
#if defined(_WIN32)
  if (const auto appdata = set("APPDATA")) {
    return *appdata / "micnote" / "host.key";
  }
#else
  if (const auto xdg = set("XDG_CONFIG_HOME")) {
    return *xdg / "micnote" / "host.key";
  }
  if (const auto home = set("HOME")) {
    return *home / ".config" / "micnote" / "host.key";
  }
#endif
  return {};
}

Opener UsbOpener() {
  return [](const std::string &path)
             -> std::expected<std::unique_ptr<transport::Transport>, transport::TransportError> {
    auto port = transport::UsbSerial::Open(path);
    if (!port) {
      return std::unexpected(port.error());
    }
    return std::move(*port);
  };
}

std::uint64_t SystemNowMs() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string_view DeviceErrorName(micnote_v1_ErrorCode code) {
  switch (code) {
    case micnote_v1_ErrorCode_ERROR_CODE_UNPAIRED:
      return "this host isn't paired";
    case micnote_v1_ErrorCode_ERROR_CODE_BUSY:
      return "busy";
    case micnote_v1_ErrorCode_ERROR_CODE_NOT_FOUND:
      return "not found";
    case micnote_v1_ErrorCode_ERROR_CODE_HASH_MISMATCH:
      return "hash mismatch";
    case micnote_v1_ErrorCode_ERROR_CODE_STORAGE:
      return "storage error";
    case micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE:
      return "not allowed right now";
    case micnote_v1_ErrorCode_ERROR_CODE_PROTO_VERSION:
      return "protocol version not supported; update the firmware";
    case micnote_v1_ErrorCode_ERROR_CODE_BAD_REQUEST:
      return "bad request";
    case micnote_v1_ErrorCode_ERROR_CODE_HOSTS_FULL:
      return "all host slots are taken";
    default:
      return "unknown error";
  }
}

std::string Describe(const ClientError &error) {
  switch (error.kind) {
    case ClientError::Kind::kTransport:
      return "the link failed";
    case ClientError::Kind::kTimeout:
      return "the puck didn't answer";
    case ClientError::Kind::kFraming:
      return "garbled data on the link";
    case ClientError::Kind::kDevice:
      return std::format("the puck said: {}", DeviceErrorName(error.device));
    default:
      return "unexpected response from the puck";
  }
}

std::string Describe(recording::ReadError error) {
  using recording::ReadError;
  switch (error) {
    case ReadError::kFormat:
      return "not a recording";
    case ReadError::kNotARecipient:
      return "not encrypted to this host";
    case ReadError::kUnwrap:
      return "this host's key wrap doesn't verify";
    case ReadError::kChunkFraming:
      return "damaged chunk records";
    case ReadError::kChunkAuth:
      return "a chunk doesn't verify";
    case ReadError::kPlaintext:
      return "malformed audio data";
    default:
      return "crypto failure";
  }
}

}  // namespace micnote::cli
