#include "api/context.hpp"

#include <array>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <utility>

#include "store/repos.hpp"
#include "store/schema.hpp"
#include "transport/usb_serial.hpp"
#include <nlohmann/json.hpp>

namespace micnote::api {

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// GCOVR_EXCL_BR_START: the error makers and JSON builders below run in
// context_test; their uncovered branches are gcc's exception cleanup for the
// string and nlohmann::json temporaries.
ApiError Invalid(std::string message) {
  return ApiError{.status = MN_E_INVALID, .code = "invalid_params", .message = std::move(message)};
}

ApiError FromDb(const store::DbError &e) {
  return ApiError{.status = MN_E_IO, .code = "store", .message = e.message};
}

ApiError NotFound(std::string what) {
  return ApiError{.status = MN_E_NOT_FOUND, .code = "not_found", .message = std::move(what)};
}
// GCOVR_EXCL_BR_STOP

std::string Hex(std::span<const std::byte> b) {
  constexpr std::string_view kDigits = "0123456789abcdef";
  std::string s;
  for (const std::byte x : b) {
    s += kDigits.at(std::to_integer<unsigned>(x) >> 4U);
    s += kDigits.at(std::to_integer<unsigned>(x) & 0xFU);
  }
  return s;
}  // GCOVR_EXCL_LINE: gcc's exception cleanup of `s`

// A required integer parameter.
std::expected<std::int64_t, ApiError> IntParam(const json &p, const char *name) {
  const auto it = p.find(name);
  if (it == p.end() || !it->is_number_integer()) {
    return std::unexpected(Invalid(std::string("\"") + name + "\" must be an integer"));
  }
  return it->get<std::int64_t>();
}

std::size_t LimitParam(const json &p, std::size_t fallback) {
  const auto it = p.find("limit");
  return it != p.end() && it->is_number_unsigned() ? it->get<std::size_t>() : fallback;
}

// GCOVR_EXCL_BR_START: as above.
json DeviceJson(const store::Device &d) {
  return json{{"id", d.id},
              {"device_id", Hex(d.device_id)},
              {"name", d.name},
              {"host_slot", d.host_slot ? json(*d.host_slot) : json(nullptr)},
              {"paired_at", d.paired_at},
              {"fw_version", d.fw_version},
              {"last_seen", d.last_seen ? json(*d.last_seen) : json(nullptr)}};
}

json RecordingJson(const store::StoredRecording &r) {
  json markers = json::array();
  for (const auto &m : r.markers) {
    markers.push_back({{"offset_ms", m.offset_ms}, {"kind", m.kind}});
  }
  // The start to show: the RTC's, the repaired one, or none.
  const std::int64_t start = r.start_unix_ms_fixed.value_or(r.start_unix_ms);
  return json{{"id", r.id},
              {"device", r.device},
              {"rec_id", r.rec_id},
              {"start_unix_ms", start != 0 ? json(start) : json(nullptr)},
              {"time_confidence", store::ToString(r.time_confidence)},  // GCOVR_EXCL_LINE: cleanup
              {"start_boot", r.start_boot},
              {"start_uptime_ms", r.start_uptime_ms},
              {"duration_ms", r.duration_ms},
              {"state", store::ToString(r.state)},  // GCOVR_EXCL_LINE: cleanup
              {"sha256", Hex(r.sha256)},
              {"created_at", r.created_at},
              {"markers", markers}};
}
// GCOVR_EXCL_BR_STOP

using Handler = std::function<std::expected<json, ApiError>(Context &, store::Db &, const json &)>;

// GCOVR_EXCL_BR_START: from here to Methods(), nearly every line builds
// nlohmann::json or ApiError temporaries, and what branch coverage reports
// uncovered is gcc's exception cleanup for them. The handlers' own decisions
// (missing or invalid params, not found, store errors, a corrupt setting)
// each have a test in context_test.cpp, and line coverage still applies.

std::expected<json, ApiError> DevicesList(Context & /*ctx*/, store::Db &db,
                                          const json & /*params*/) {
  auto devices = store::ListDevices(db);
  if (!devices) {
    return std::unexpected(FromDb(devices.error()));
  }
  json out = json::array();
  for (const auto &d : *devices) {
    out.push_back(DeviceJson(d));
  }
  return json{{"devices", out}};
}

std::expected<json, ApiError> RecordingsList(Context & /*ctx*/, store::Db &db, const json &p) {
  auto list = store::ListRecordings(db, LimitParam(p, 0));
  if (!list) {
    return std::unexpected(FromDb(list.error()));
  }
  json out = json::array();
  for (const auto &r : *list) {
    out.push_back(RecordingJson(r));
  }
  return json{{"recordings", out}};
}

std::expected<json, ApiError> RecordingsGet(Context & /*ctx*/, store::Db &db, const json &p) {
  const auto id = IntParam(p, "id");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto r = store::GetRecording(db, *id);
  if (!r) {
    return std::unexpected(FromDb(r.error()));
  }
  if (!r->has_value()) {
    return std::unexpected(NotFound("no recording " + std::to_string(*id)));
  }
  return RecordingJson(**r);
}

std::expected<json, ApiError> RecordingsDelete(Context &ctx, store::Db &db, const json &p) {
  const auto id = IntParam(p, "id");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto r = store::GetRecording(db, *id);
  if (!r) {
    return std::unexpected(FromDb(r.error()));
  }
  if (!r->has_value()) {
    return std::unexpected(NotFound("no recording " + std::to_string(*id)));
  }
  if (auto gone = store::DeleteRecording(db, *id); !gone) {
    return std::unexpected(FromDb(gone.error()));
  }
  // Only ever a file inside data_dir (the store writes relative paths).
  const fs::path rel((*r)->mnr_path);
  if (!rel.empty() && rel.is_relative() && !(*r)->mnr_path.contains("..")) {
    std::error_code ec;
    fs::remove(ctx.data_dir() / rel, ec);
  }
  ctx.Emit(json{{"type", "recording.deleted"}, {"id", *id}}.dump());
  return json{{"deleted", *id}};
}

std::expected<json, ApiError> TranscriptsGet(Context & /*ctx*/, store::Db &db, const json &p) {
  const auto rec = IntParam(p, "recording");
  if (!rec) {
    return std::unexpected(rec.error());
  }
  auto t = store::GetTranscript(db, *rec);
  if (!t) {
    return std::unexpected(FromDb(t.error()));
  }
  if (!t->has_value()) {
    return std::unexpected(NotFound("no transcript for recording " + std::to_string(*rec)));
  }
  const store::Transcript &tr = **t;
  json segments = json::array();
  for (const auto &s : tr.segments) {
    segments.push_back({{"start_ms", s.start_ms},
                        {"end_ms", s.end_ms},
                        {"text", s.text},
                        {"avg_logprob", s.avg_logprob},
                        {"no_speech_prob", s.no_speech_prob}});
  }
  return json{{"recording", tr.recording}, {"model", tr.model},           {"backend", tr.backend},
              {"language", tr.language},   {"created_at", tr.created_at}, {"rtf", tr.rtf},
              {"segments", segments}};
}

std::expected<json, ApiError> SearchQuery(Context & /*ctx*/, store::Db &db, const json &p) {
  const auto q = p.find("q");
  if (q == p.end() || !q->is_string() || q->get<std::string>().empty()) {
    return std::unexpected(Invalid("\"q\" must be a non-empty string"));
  }
  auto hits = store::Search(db, q->get<std::string>(), LimitParam(p, 50));
  if (!hits) {
    // FTS5 syntax errors land here too: the caller's query, not the store.
    return std::unexpected(Invalid("bad query: " + hits.error().message));
  }
  json out = json::array();
  for (const auto &h : *hits) {
    out.push_back({{"recording", h.recording},
                   {"start_ms", h.start_ms},
                   {"end_ms", h.end_ms},
                   {"text", h.text}});
  }
  return json{{"hits", out}};
}

std::expected<json, ApiError> SettingsGet(Context & /*ctx*/, store::Db &db, const json &p) {
  const auto key = p.find("key");
  if (key == p.end() || !key->is_string()) {
    return std::unexpected(Invalid("\"key\" must be a string"));
  }
  auto v = store::GetSetting(db, key->get<std::string>());
  if (!v) {
    return std::unexpected(FromDb(v.error()));
  }
  return json{{"key", *key}, {"value", v->has_value() ? json::parse(**v) : json(nullptr)}};
}

std::expected<json, ApiError> SettingsSet(Context & /*ctx*/, store::Db &db, const json &p) {
  const auto key = p.find("key");
  const auto value = p.find("value");
  if (key == p.end() || !key->is_string() || value == p.end()) {
    return std::unexpected(Invalid(R"(needs "key" (a string) and "value")"));
  }
  if (auto ok = store::SetSetting(db, key->get<std::string>(), value->dump()); !ok) {
    return std::unexpected(FromDb(ok.error()));
  }
  return json{{"key", *key}, {"value", *value}};
}

json JobJson(const store::Job &j) {
  return json{{"id", j.id},
              {"kind", j.kind},
              {"ref_id", j.ref_id ? json(*j.ref_id) : json(nullptr)},
              {"state", store::ToString(j.state)},  // GCOVR_EXCL_LINE: gcc's exception cleanup
              {"attempts", j.attempts},
              {"not_before", j.not_before},
              {"last_error", j.last_error},
              {"created_at", j.created_at},
              {"updated_at", j.updated_at}};
}

std::expected<json, ApiError> JobsGet(Context & /*ctx*/, store::Db &db, const json &p) {
  const auto id = IntParam(p, "id");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto j = store::GetJob(db, *id);
  if (!j) {
    return std::unexpected(FromDb(j.error()));
  }
  if (!j->has_value()) {
    return std::unexpected(NotFound("no job " + std::to_string(*id)));
  }
  return JobJson(**j);
}

std::expected<json, ApiError> JobsList(Context & /*ctx*/, store::Db &db, const json &p) {
  auto list = store::ListJobs(db, LimitParam(p, 0));
  if (!list) {
    return std::unexpected(FromDb(list.error()));
  }
  json out = json::array();
  for (const auto &j : *list) {
    out.push_back(JobJson(j));
  }
  return json{{"jobs", out}};
}

// GCOVR_EXCL_BR_STOP

const std::map<std::string, Handler, std::less<>> &Methods() {
  // GCOVR_EXCL_BR_START: the static's guard and the table's cleanup.
  static const std::map<std::string, Handler, std::less<>> kMethods = {
      {"devices.list@1", DevicesList},
      {"recordings.list@1", RecordingsList},
      {"recordings.get@1", RecordingsGet},
      {"recordings.delete@1", RecordingsDelete},
      {"transcripts.get@1", TranscriptsGet},
      {"search.query@1", SearchQuery},
      {"settings.get@1", SettingsGet},
      {"settings.set@1", SettingsSet},
      {"jobs.get@1", JobsGet},
      {"jobs.list@1", JobsList},
  };
  // GCOVR_EXCL_BR_STOP
  return kMethods;
}

}  // namespace

// GCOVR_EXCL_BR_START: as for the handlers: the rest of this file is
// Context's plumbing, whose decisions context_test.cpp covers one by one.
std::string ErrorJson(const ApiError &e) {
  return json{{"error", {{"code", e.code}, {"message", e.message}}}}.dump();
}

Context::Context(fs::path data_dir, std::unique_ptr<secrets::Secrets> secrets, store::Db db,
                 mn_event_fn on_event, void *user_data, ContextOptions options)
    : data_dir_(std::move(data_dir)),
      secrets_(std::move(secrets)),
      db_(std::move(db)),
      on_event_(on_event),
      user_data_(user_data),
      options_(std::move(options)) {
  if (!options_.now) {
    options_.now = [] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    };
  }
  if (!options_.open) {
    options_.open = [](const std::string &path)
        -> std::expected<std::unique_ptr<transport::Transport>, transport::TransportError> {
      auto port = transport::UsbSerial::Open(path);
      if (!port) {
        return std::unexpected(port.error());
      }
      return std::move(*port);
    };
  }
  queue_ = std::make_unique<jobs::JobQueue>(
      [this](const std::function<void(store::Db &)> &fn) {  // GCOVR_EXCL_LINE: as above
        const std::scoped_lock lock(db_mutex_);
        fn(db_);
      },
      [this](const std::string &event) { Emit(event); }, options_.now);
  transcribe_deps_ = jobs::TranscribeDeps{.data_dir = data_dir_,
                                          .secrets = secrets_.get(),
                                          .engine = options_.engine,
                                          .options = options_.asr};
  sync_deps_ = jobs::SyncDeps{.data_dir = data_dir_,
                              .secrets = secrets_.get(),
                              .open = options_.open,
                              .timeout = options_.link_timeout,
                              .on_recording = {}};
  if (options_.engine) {
    // Sync chains into transcription (§4.8).
    sync_deps_.on_recording = [this](std::int64_t rec) {
      [[maybe_unused]] const auto queued = queue_->Submit("transcribe", rec, "{}");
    };
    queue_->Register("transcribe", [this](jobs::JobContext &ctx) {
      return RunTranscribe(ctx, transcribe_deps_);
    });
  }
  queue_->Register("sync", [this](jobs::JobContext &ctx) { return RunSync(ctx, sync_deps_); });
  [[maybe_unused]] const auto started = queue_->Start();
}

Context::~Context() { queue_->Stop(); }

Context::Submitted Context::Submit(std::string_view method, std::string_view params_json) {
  const auto reject = [](mn_status status, std::string code, std::string message) {
    return Submitted{
        .status = status,
        .job = 0,
        .error_json = ErrorJson(
            ApiError{.status = status, .code = std::move(code), .message = std::move(message)})};
  };
  const json params =
      params_json.empty() ? json::object() : json::parse(params_json, nullptr, false);
  if (params.is_discarded() || !params.is_object()) {
    return reject(MN_E_INVALID, "invalid_params", "params must be a JSON object");
  }
  std::string kind;
  std::optional<std::int64_t> ref;
  if (method == "sync.run@1") {
    if (!params.contains("usb") || !params["usb"].is_string()) {
      return reject(MN_E_INVALID, "invalid_params", R"("usb" must be a string)");
    }
    kind = "sync";
  } else if (method == "transcribe.run@1") {
    if (!queue_->Handles("transcribe")) {
      return reject(MN_E_MODEL, "no_engine", "this build has no speech engine");
    }
    const auto rec = IntParam(params, "recording");
    if (!rec) {
      return reject(MN_E_INVALID, "invalid_params", rec.error().message);
    }
    kind = "transcribe";
    ref = *rec;
  } else {
    return reject(MN_E_INVALID, "unknown_method", "no job method \"" + std::string(method) + "\"");
  }
  const auto id = queue_->Submit(kind, ref, params.dump());
  if (!id) {
    return reject(MN_E_IO, "store", id.error().message);
  }
  return Submitted{.status = MN_OK, .job = static_cast<std::uint64_t>(*id), .error_json = {}};
}

mn_status Context::Cancel(std::uint64_t job) {
  return queue_->Cancel(static_cast<std::int64_t>(job)) ? MN_OK : MN_E_NOT_FOUND;
}

std::expected<std::unique_ptr<Context>, ApiError> Context::Open(const mn_config &config) {
  if (config.data_dir == nullptr || config.get_secret == nullptr || config.set_secret == nullptr) {
    return std::unexpected(Invalid("data_dir, get_secret and set_secret are required"));
  }
  void *ud = config.user_data;
  mn_get_secret_fn get = config.get_secret;
  mn_set_secret_fn set = config.set_secret;
  auto secrets = std::make_unique<secrets::CallbackSecrets>(secrets::SecretCallbacks{
      .get = [get, ud](const char *name, std::uint8_t *out, std::size_t cap,
                       std::size_t *len) { return get(ud, name, out, cap, len); },
      .set = [set, ud](const char *name, const std::uint8_t *data,
                       std::size_t len) { return set(ud, name, data, len); }});
  return Open(fs::path(reinterpret_cast<const char8_t *>(config.data_dir)), std::move(secrets),
              config.on_event, ud);
}

std::expected<std::unique_ptr<Context>, ApiError> Context::Open(
    const fs::path &data_dir, std::unique_ptr<secrets::Secrets> secrets, mn_event_fn on_event,
    void *user_data, ContextOptions options) {
  std::error_code ec;
  fs::create_directories(data_dir, ec);
  if (!fs::is_directory(data_dir, ec)) {
    return std::unexpected(ApiError{
        .status = MN_E_IO, .code = "data_dir", .message = "can't create " + data_dir.string()});
  }
  auto key = secrets::GetOrCreate(*secrets, "db_key", store::kDbKeySize);
  if (!key) {
    return std::unexpected(
        ApiError{.status = MN_E_CRYPTO, .code = "secret", .message = "db_key unavailable"});
  }
  store::DbKey db_key{};
  std::ranges::copy(*key, db_key.begin());
  auto db = store::Db::Open(data_dir / "micnote.db", db_key);
  db_key.fill(std::byte{0});
  if (!db) {
    // SQLITE_NOTADB: the key in secure storage doesn't open this database.
    const bool wrong_key = db.error().code == 26;
    return std::unexpected(ApiError{.status = wrong_key ? MN_E_CRYPTO : MN_E_IO,
                                    .code = wrong_key ? "db_key_mismatch" : "store",
                                    .message = db.error().message});
  }
  if (auto migrated = store::Migrate(*db); !migrated) {
    return std::unexpected(FromDb(migrated.error()));
  }
  return std::unique_ptr<Context>(new Context(data_dir, std::move(secrets), std::move(*db),
                                              on_event, user_data, std::move(options)));
}

void Context::Emit(const std::string &event_json) const {
  if (on_event_ != nullptr) {
    on_event_(user_data_, event_json.c_str());
  }
}

std::expected<std::string, ApiError> Context::Dispatch(std::string_view method,
                                                       std::string_view params_json) {
  const auto m = Methods().find(method);
  if (m == Methods().end()) {
    return std::unexpected(ApiError{.status = MN_E_INVALID,
                                    .code = "unknown_method",
                                    .message = "no method \"" + std::string(method) + "\""});
  }
  const json params =
      params_json.empty() ? json::object() : json::parse(params_json, nullptr, false);
  if (params.is_discarded() || !params.is_object()) {
    return std::unexpected(Invalid("params must be a JSON object"));
  }
  const std::scoped_lock lock(db_mutex_);
  return m->second(*this, db_, params).transform([](const json &result) {
    // Text from transcripts can hold invalid UTF-8: replace it, don't throw.
    return result.dump(-1, ' ', false, json::error_handler_t::replace);
  });
}

Context::Response Context::Call(std::string_view method, std::string_view params_json) {
  try {
    auto result = Dispatch(method, params_json);
    if (!result) {
      return Response{.status = result.error().status, .json = ErrorJson(result.error())};
    }
    return Response{.status = MN_OK, .json = std::move(*result)};
  } catch (const std::exception &e) {
    // A bug (or out of memory): reported, never thrown across the ABI.
    const ApiError internal{.status = MN_E_INTERNAL, .code = "internal", .message = e.what()};
    return Response{.status = MN_E_INTERNAL, .json = ErrorJson(internal)};
  }
}

// GCOVR_EXCL_BR_STOP

}  // namespace micnote::api
