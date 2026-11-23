#include "commands.hpp"

#include <micnote.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "crypto/pairing.hpp"
#include "protocol/session.hpp"
#if defined(MICNOTE_HAVE_ASR)
#include "transcribe.hpp"
#endif
#include "crypto/psa.hpp"
#include "models.hpp"
#include "recording/ogg_writer.hpp"
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
    "       micnote-cli sync   --usb PORT --out DIR [--no-ack] [--key FILE]\n"
    "       micnote-cli pair   --usb PORT [--name NAME] [--key FILE]\n"
    "       micnote-cli hosts  --usb PORT [--key FILE]\n"
    "       micnote-cli unpair --usb PORT --host HOST_ID [--key FILE]\n";

// How long `pair` waits for the button press: the PAIRING window.
constexpr auto kPairWait = std::chrono::seconds(60);

constexpr int kOk = 0;
constexpr int kFailed = 1;
constexpr int kUsageError = 2;

struct Options {
  std::string command;
  std::map<std::string, std::string, std::less<>> values;
  bool no_ack = false;
};

int Usage(const Env &env) {
  *env.err << kUsage << kModelsUsage;
#if defined(MICNOTE_HAVE_ASR)
  *env.err << kTranscribeUsage;
#endif
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
  if (command == "pair") {
    return option == "--name";
  }
  if (command == "unpair") {
    return option == "--host";
  }
  return command == "sync" && (option == "--out" || option == "--no-ack");
}

std::optional<Options> Parse(std::span<const std::string_view> args) {
  Options opts;
  opts.command = args.front();
  constexpr std::array<std::string_view, 7> kCommands{"keygen", "status", "list",  "sync",
                                                      "pair",   "hosts",  "unpair"};
  if (std::ranges::find(kCommands, opts.command) == kCommands.end()) {
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
      (opts.command == "sync" && !opts.values.contains("--out")) ||
      (opts.command == "unpair" && !opts.values.contains("--host"))) {
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

// Writes a secret (a key) readable only by this user. False if it can't.
bool WriteSecret(const fs::path &path, std::span<const std::byte> bytes) {
  std::error_code ec;
  // Fails harmlessly for a bare file name; the write below reports problems.
  fs::create_directories(path.parent_path(), ec);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
      return false;
    }
  }
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, ec);
  return true;
}

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
  // Only this user may read it: it decrypts every recording made for this host.
  const bool written = WriteSecret(*path, priv);
  crypto::Wipe(priv);
  if (!written) {
    *env.err << std::format("micnote-cli: can't write {}\n", path->string());
    return kFailed;
  }
  *env.out << std::format("wrote host key {}\nhost_id {}\n", path->string(), Hex(key->host_id()));
  return kOk;
}

std::optional<recording::HostKey> LoadKey(const fs::path &path, const Env &env) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *env.err << std::format("micnote-cli: no host key at {}; run: micnote-cli keygen\n",
                            path.string());
    return std::nullopt;
  }
  std::array<std::byte, crypto::kX25519KeySize + 1> buf{};
  file.read(reinterpret_cast<char *>(buf.data()), buf.size());
  if (std::cmp_not_equal(file.gcount(), crypto::kX25519KeySize)) {
    *env.err << std::format("micnote-cli: {} isn't a host key (expected 32 bytes)\n",
                            path.string());
    return std::nullopt;
  }
  auto key = recording::HostKey::FromPrivate(std::span(buf).first<crypto::kX25519KeySize>());
  crypto::Wipe(buf);
  return std::move(*key);
}

// Where the link key for a puck lives: beside the host key, in
// pucks/<device_id>.link (protocol section 8).
fs::path LinkPath(const fs::path &key_path, std::span<const std::byte> device_id) {
  return key_path.parent_path() / "pucks" / (Hex(device_id) + ".link");
}

std::optional<crypto::LinkKey> LoadLink(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  crypto::LinkKey key{};
  std::array<char, crypto::kLinkKeySize + 1> buf{};
  file.read(buf.data(), buf.size());
  if (std::cmp_not_equal(file.gcount(), crypto::kLinkKeySize)) {
    return std::nullopt;
  }
  std::ranges::transform(std::span(buf).first<crypto::kLinkKeySize>(), key.begin(),
                         [](char c) { return static_cast<std::byte>(c); });
  return key;
}

// The pieces of an open link.
struct Link {
  recording::HostKey key;
  fs::path key_path;
  std::unique_ptr<transport::Transport> transport;
  std::unique_ptr<protocol::Client> client;
  micnote_v1_HelloAck hello{};
  crypto::Nonce n_h{};
};

// Loads the host key, opens the port and says Hello.
std::optional<Link> Open(const Options &opts, const Env &env) {
  const auto key_path = KeyPath(opts, env);
  if (!key_path) {
    return std::nullopt;
  }
  auto key = LoadKey(*key_path, env);
  if (!key) {
    return std::nullopt;
  }
  const std::string &port = opts.values.at("--usb");
  auto transport = env.open(port);
  if (!transport) {
    *env.err << std::format("micnote-cli: can't open {}\n", port);
    return std::nullopt;
  }
  auto client = std::make_unique<protocol::Client>(**transport);
  crypto::Nonce n_h{};
  static_cast<void>(Random(n_h));
  auto hello = client->Hello(key->host_id(), n_h);
  if (!hello) {
    *env.err << std::format("micnote-cli: hello failed: {}\n", Describe(hello.error()));
    return std::nullopt;
  }
  // GCOVR_EXCL_BR_START: gcc's exception-cleanup branches for the moves.
  return Link{.key = std::move(*key),
              .key_path = *key_path,
              .transport = std::move(*transport),
              .client = std::move(client),
              .hello = *hello,
              .n_h = n_h};
  // GCOVR_EXCL_BR_STOP
}

crypto::Nonce DeviceNonce(const pb_byte_t (&bytes)[crypto::kNonceSize]) {
  crypto::Nonce n{};
  std::ranges::transform(bytes, n.begin(), [](pb_byte_t b) { return std::byte{b}; });
  return n;
}

// An authenticated session (protocol section 3) with the puck's clock set.
struct Session {
  recording::HostKey key;
  std::unique_ptr<transport::Transport> link;
  std::unique_ptr<protocol::Client> client;
  // This host's link key file for the puck.
  fs::path link_path;
};

std::optional<Session> Connect(const Options &opts, const Env &env) {
  auto link = Open(opts, env);
  if (!link) {
    return std::nullopt;
  }
  const auto device_id = std::as_bytes(std::span(link->hello.device_info.device_id));
  const fs::path link_path = LinkPath(link->key_path, device_id);
  const auto link_key = LoadLink(link_path);
  if (!link_key) {
    *env.err << std::format(
        "micnote-cli: not paired with puck {} (no {}); run: micnote-cli pair --usb {}\n",
        Hex(device_id), link_path.string(), opts.values.at("--usb"));
    return std::nullopt;
  }
  if (auto auth = protocol::Authenticate(*link->client, link->n_h, link->hello, *link_key); !auth) {
    if (auth.error().kind == protocol::AuthError::Kind::kWrongPuck) {
      *env.err << std::format(
          "micnote-cli: puck {} didn't prove it's the one paired with this host; pair again\n",
          Hex(device_id));
    } else {
      *env.err << std::format("micnote-cli: the puck refused this host: {}\n",
                              Describe(auth.error().client));
    }
    return std::nullopt;
  }
  if (auto set = link->client->SetTime(env.now_unix_ms(), 0); !set) {
    *env.err << std::format("micnote-cli: setting the clock failed: {}\n", Describe(set.error()));
    return std::nullopt;
  }
  // GCOVR_EXCL_BR_START: gcc's exception-cleanup branches for the moves.
  return Session{.key = std::move(link->key),
                 .link = std::move(link->transport),
                 .client = std::move(link->client),
                 .link_path = link_path};
  // GCOVR_EXCL_BR_STOP
}

int Pair(const Options &opts, const Env &env) {
  auto link = Open(opts, env);
  if (!link) {
    return kFailed;
  }
  protocol::Client &client = *link->client;
  const auto name_it = opts.values.find("--name");
  std::string name = "micnote-cli";
  if (name_it != opts.values.end()) {
    name = name_it->second;
  }
  const auto challenge = client.PairBegin(link->key.pub(), name);
  if (!challenge) {
    constexpr ClientError kNotPairing{.kind = ClientError::Kind::kDevice,
                                      .device = micnote_v1_ErrorCode_ERROR_CODE_BAD_STATE};
    if (challenge.error() == kNotPairing) {
      *env.err << "micnote-cli: the puck isn't in pairing mode; hold its button for 5 s, then "
                  "run this again\n";
    } else {
      *env.err << std::format("micnote-cli: pairing failed: {}\n", Describe(challenge.error()));
    }
    return kFailed;
  }
  const auto device_id = std::as_bytes(std::span(link->hello.device_info.device_id));
  std::array<std::byte, crypto::kX25519KeySize> device_pub{};
  std::ranges::transform(challenge->device_pub, device_pub.begin(),
                         [](pb_byte_t b) { return std::byte{b}; });
  const auto pairing =
      crypto::DerivePairing(link->key.key(), link->key.pub(), device_pub,
                            std::span<const std::byte, 8>(link->key.host_id()),
                            device_id.first<8>(), link->n_h, DeviceNonce(challenge->device_nonce));
  if (!pairing) {
    *env.err << "micnote-cli: the puck sent an invalid key; pairing cancelled\n";
    // Best effort: the puck also drops the pairing when its window closes.
    if (!client.PairCancel()) {
      *env.err << "micnote-cli: (the puck didn't acknowledge the cancel)\n";
    }
    return kFailed;
  }
  // Over USB the cable is the trust anchor (protocol section 8): the colours
  // are shown, not compared.
  std::string colours;
  for (const std::uint8_t symbol : pairing->sas) {
    colours += std::format("{}{}", colours.empty() ? "" : " ", crypto::SasColorName(symbol));
  }
  *env.out << std::format("the puck is showing: {}\npress the puck's button to confirm\n", colours);
  const auto slot = client.PairConfirm(kPairWait);
  if (!slot) {
    *env.err << std::format("micnote-cli: pairing not confirmed: {}\n", Describe(slot.error()));
    return kFailed;
  }
  const fs::path path = LinkPath(link->key_path, device_id);
  if (!WriteSecret(path, pairing->link_key)) {
    *env.err << std::format("micnote-cli: paired, but can't write {}; pair again\n", path.string());
    return kFailed;
  }
  *env.out << std::format("paired with puck {} as host {} (slot {}); link key {}\n", Hex(device_id),
                          Hex(link->key.host_id()), *slot, path.string());
  return kOk;
}

int Hosts(const Options &opts, const Env &env) {
  auto session = Connect(opts, env);
  if (!session) {
    return kFailed;
  }
  const auto hosts = session->client->ListHosts();
  if (!hosts) {
    *env.err << std::format("micnote-cli: hosts failed: {}\n", Describe(hosts.error()));
    return kFailed;
  }
  for (const auto &h : *hosts) {
    const auto id = std::as_bytes(std::span(h.host_id));
    // GCOVR_EXCL_START: every line runs; gcc pins the exception cleanup of
    // the format temporaries to one of them.
    *env.out << std::format("slot {}  {}  {}{}\n", h.slot, Hex(id), h.name,
                            std::ranges::equal(id, session->key.host_id()) ? "  (this host)" : "");
    // GCOVR_EXCL_STOP
  }
  return kOk;
}

int Unpair(const Options &opts, const Env &env) {
  const std::string &hex = opts.values.at("--host");
  std::array<std::byte, 8> host_id{};
  const bool valid = hex.size() == 16 && std::ranges::all_of(hex, [](char c) {
                       return std::isxdigit(static_cast<unsigned char>(c)) != 0;
                     });
  if (!valid) {
    *env.err << std::format("micnote-cli: {} isn't a host_id (16 hex digits)\n", hex);
    return kFailed;
  }
  for (std::size_t i = 0; i < host_id.size(); ++i) {
    host_id.at(i) = static_cast<std::byte>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
  }
  auto session = Connect(opts, env);
  if (!session) {
    return kFailed;
  }
  if (auto done = session->client->Unpair(host_id); !done) {
    *env.err << std::format("micnote-cli: unpair failed: {}\n", Describe(done.error()));
    return kFailed;
  }
  *env.out << std::format("unpaired host {}\n", hex);
  if (std::ranges::equal(host_id, session->key.host_id())) {
    // This host: its link key for the puck is useless now.
    std::error_code ec;
    fs::remove(session->link_path, ec);
    *env.out << "that was this host; pair again to use this puck\n";
  }
  return kOk;
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
    // The .mnr is the copy that matters (encrypted, and what the ack hash
    // covers); the .opus is a convenience, so a failure here doesn't stop
    // the ack.
    WriteOpusFile(*read, out / (name + ".opus"), name, env);
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

bool WriteOpusFile(const recording::Recording &rec, const fs::path &path, std::string_view name,
                   const Env &env) {
  const auto ogg = recording::WriteOggOpus(rec);
  if (!ogg) {
    *env.out << std::format("{}: no .opus: the audio isn't Opus this host can wrap\n", name);
    return false;
  }
  if (!WriteFile(path, *ogg)) {
    *env.err << std::format("micnote-cli: {}: can't write {}\n", name, path.string());
    return false;
  }
  *env.out << std::format("{}: -> {}\n", name, path.string());
  return true;
}

int Run(std::span<const std::string_view> args, const Env &env) {
  if (args.size() == 1 && (args.front() == "--version" || args.front() == "-V")) {
    *env.out << std::format("micnote-cli {}\n", mn_version());
    return kOk;
  }
  if (args.size() == 1 && (args.front() == "--help" || args.front() == "-h")) {
    *env.out << kUsage << kModelsUsage;
#if defined(MICNOTE_HAVE_ASR)
    *env.out << kTranscribeUsage;
#endif
    return kOk;
  }
  if (args.empty()) {
    return Usage(env);
  }
  if (args.front() == "models") {
    return Models(args, env);
  }
#if defined(MICNOTE_HAVE_ASR)
  if (args.front() == "transcribe") {
    return Transcribe(args, env);
  }
#endif
  const auto opts = Parse(args);
  if (!opts) {
    return Usage(env);
  }
  if (opts->command == "pair") {
    return Pair(*opts, env);
  }
  if (opts->command == "hosts") {
    return Hosts(*opts, env);
  }
  if (opts->command == "unpair") {
    return Unpair(*opts, env);
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
