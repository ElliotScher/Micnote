// What the C ABI's mn_context is (stage 2 §4.1, §4.7): the store, the
// secrets the host provides, and the event callback. The JSON methods live
// in context.cpp; abi.cpp is only the extern "C" shell around them.
#pragma once

#include <micnote.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "jobs/queue.hpp"
#include "jobs/sync_job.hpp"
#include "jobs/transcribe_job.hpp"
#include "models/registry.hpp"
#include "secrets/secrets.hpp"
#include "store/db.hpp"

namespace micnote::api {

struct ApiError {
  mn_status status = MN_E_INTERNAL;
  // Stable, for programs: "not_found", "invalid_params", ...
  std::string code;
  // For people and logs.
  std::string message;
};

// What a host can plug in beyond mn_config (C++ hosts: the CLI, tests).
struct ContextOptions {
  // How sync.run opens a port; USB serial when empty.
  jobs::Opener open;
  // A speech engine; without one, transcribe.run isn't available and synced
  // recordings stay `synced`.
  jobs::EngineFactory engine;
  asr::AsrOptions asr;
  // Unix ms; the system clock when empty.
  std::function<std::int64_t()> now;
  std::chrono::milliseconds link_timeout = std::chrono::seconds(5);
  // Where model files live; data_dir/models when empty.
  std::filesystem::path models_dir;
  // The models this context offers: the built-in manifest when empty.
  std::string manifest_json;
  // The device models are chosen for: this machine when unset.
  std::optional<models::Device> device;
};

class Context {
 public:
  // Opens (or creates) data_dir/micnote.db with the db_key secret, and
  // starts the job queue.
  static std::expected<std::unique_ptr<Context>, ApiError> Open(const mn_config &config);
  static std::expected<std::unique_ptr<Context>, ApiError> Open(
      const std::filesystem::path &data_dir, std::unique_ptr<secrets::Secrets> secrets,
      mn_event_fn on_event, void *user_data, ContextOptions options = {});

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Context(Context &&) = delete;
  Context &operator=(Context &&) = delete;
  ~Context();

  struct Response {
    mn_status status = MN_OK;
    std::string json;
  };
  // One method call; never throws (a C++ exception becomes MN_E_INTERNAL).
  Response Call(std::string_view method, std::string_view params_json);

  struct Submitted {
    mn_status status = MN_OK;
    std::uint64_t job = 0;
    std::string error_json;  // set when status != MN_OK
  };
  // Queues a job: "sync.run@1" {usb}, "transcribe.run@1" {recording}.
  Submitted Submit(std::string_view method, std::string_view params_json);
  // MN_OK, or MN_E_NOT_FOUND if the job isn't queued or running.
  mn_status Cancel(std::uint64_t job);
  // Wakes the queue's workers (tests that move the clock).
  void PokeJobs() { queue_->Poke(); }

  [[nodiscard]] const std::filesystem::path &data_dir() const { return data_dir_; }
  [[nodiscard]] const std::filesystem::path &models_dir() const { return options_.models_dir; }
  [[nodiscard]] const models::Registry &registry() const { return registry_; }
  [[nodiscard]] const models::Device &device() const { return device_; }
  void Emit(const std::string &event_json) const;

 private:
  Context(std::filesystem::path data_dir, std::unique_ptr<secrets::Secrets> secrets, store::Db db,
          models::Registry registry, mn_event_fn on_event, void *user_data, ContextOptions options);
  std::expected<std::string, ApiError> Dispatch(std::string_view method,
                                                std::string_view params_json);

  std::filesystem::path data_dir_;
  std::unique_ptr<secrets::Secrets> secrets_;
  // One connection, one caller at a time. (The writer thread and reader
  // pool of §4.1 come with the job queue.)
  std::mutex db_mutex_;
  store::Db db_;
  models::Registry registry_;
  models::Device device_;  // options' device, or this machine
  mn_event_fn on_event_;
  void *user_data_;
  ContextOptions options_;
  jobs::SyncDeps sync_deps_;
  jobs::TranscribeDeps transcribe_deps_;
  // Last: it stops (joining its workers) before the store goes away.
  std::unique_ptr<jobs::JobQueue> queue_;
};

// The JSON body of an error response.
std::string ErrorJson(const ApiError &e);

}  // namespace micnote::api
