/*
 * libmicnote C ABI: the only interface the Flutter app (dart:ffi) and other
 * hosts use. Plain C, opaque handles and UTF-8 JSON payloads, so it stays
 * stable across compilers. ffigen generates the Dart bindings,
 * app/packages/micnote_core/lib/src/bindings.dart, from this file.
 *
 * Stage 2 §4.7: one context per process; requests are JSON method calls
 * ("recordings.list@1", ...). Nothing here throws, and every entry point is
 * thread-safe.
 */
#ifndef MICNOTE_H
#define MICNOTE_H

/* A C header: C++-only advice (constexpr, enum class, <cstdint>) can't apply. */
/* clang-format off */
/* NOLINTBEGIN(modernize-deprecated-headers,cppcoreguidelines-macro-usage,cppcoreguidelines-use-enum-class,modernize-use-using,performance-enum-size,cppcoreguidelines-macro-to-enum,modernize-macro-to-enum) */
/* clang-format on */

#include <stddef.h>
#include <stdint.h>

#if defined(MICNOTE_STATIC)
#define MN_API
#elif defined(_WIN32)
#if defined(MICNOTE_BUILDING)
#define MN_API __declspec(dllexport)
#else
#define MN_API __declspec(dllimport)
#endif
#else
#define MN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to this header. */
#define MN_ABI_VERSION 1

typedef struct mn_context mn_context;

/* Status codes. A fixed-width integer rather than an enum: the size of a C
 * enum is up to the compiler, and FFI callers (Dart) must know it. */
typedef int32_t mn_status;
enum {
  MN_OK = 0,
  MN_E_INVALID = 1,   /* bad arguments, unknown method, malformed JSON */
  MN_E_IO = 2,        /* the data directory, the database, a file */
  MN_E_CRYPTO = 3,    /* a secret or key problem, a wrong database key */
  MN_E_NOT_FOUND = 4, /* no such recording, device, ... */
  MN_E_BUSY = 5,
  MN_E_DEVICE = 6,
  MN_E_MODEL = 7,
  MN_E_CANCELLED = 8,
  MN_E_INTERNAL = 99 /* a bug: please report it */
};

/*
 * Secrets the core needs (db_key, host_x25519_priv, link_key/<device>) are
 * kept by the host app, in the platform's secure storage:
 * - get_secret copies the secret `name` into out[cap] and sets *len; returns
 *   0, 1 if there is no such secret, or anything else for an error.
 * - set_secret stores it; returns 0, or non-zero for an error.
 * They may be called from any core thread.
 */
typedef int (*mn_get_secret_fn)(void *user_data, const char *name, uint8_t *out, size_t cap,
                                size_t *len);
typedef int (*mn_set_secret_fn)(void *user_data, const char *name, const uint8_t *data, size_t len);
/* Events (JSON), called on core threads; may be NULL. */
typedef void (*mn_event_fn)(void *user_data, const char *event_json);

typedef struct mn_config {
  /* UTF-8 paths, copied by mn_context_create. data_dir is created if need
   * be and holds the database and recordings; models_dir holds the model
   * files (named as in models/manifest.json's URLs), data_dir/models if
   * NULL. */
  const char *data_dir;
  const char *models_dir;
  mn_get_secret_fn get_secret;
  mn_set_secret_fn set_secret;
  mn_event_fn on_event;
  void *user_data;
} mn_config;

/*
 * libmicnote's version as "MAJOR.MINOR.PATCH". The string has static storage
 * duration: never free it.
 */
MN_API const char *mn_version(void);

/* MN_ABI_VERSION as this library was built: a host checks it matches. */
MN_API uint32_t mn_abi_version(void);

/*
 * Opens the store in config->data_dir (creating the database key on first
 * use) and returns a context. On failure *out is NULL and the status says
 * why: MN_E_CRYPTO for a database the stored key can't open.
 */
MN_API mn_status mn_context_create(const mn_config *config, mn_context **out);

/* Closes the store. NULL is a no-op. */
MN_API void mn_context_destroy(mn_context *ctx);

/*
 * Calls `method` with `params_json` (NULL means {}). *result_json receives a
 * JSON object to free with mn_free: the result on MN_OK, or
 * {"error":{"code":"...","message":"..."}} otherwise.
 */
MN_API mn_status mn_call(mn_context *ctx, const char *method, const char *params_json,
                         char **result_json);

/*
 * Queues a long-running job and returns at once with its id in *job_id:
 *   "sync.run@1"        {"usb": PORT}      sync the puck on that port
 *   "transcribe.run@1"  {"recording": ID}  (builds with a speech engine)
 * Progress and the outcome arrive as events: job.progress, job.done,
 * job.failed (with retry_at), job.cancelled. On an error *job_id is 0 and
 * *error_json (if not NULL) receives an error object to free with mn_free.
 */
MN_API mn_status mn_submit(mn_context *ctx, const char *method, const char *params_json,
                           uint64_t *job_id, char **error_json);

/* Cancels a queued or running job. MN_E_NOT_FOUND if it's neither. */
MN_API mn_status mn_cancel(mn_context *ctx, uint64_t job_id);

/* Frees a string the library returned. NULL is a no-op. */
MN_API void mn_free(void *p);

#ifdef __cplusplus
}
#endif

/* clang-format off */
/* NOLINTEND(modernize-deprecated-headers,cppcoreguidelines-macro-usage,cppcoreguidelines-use-enum-class,modernize-use-using,performance-enum-size,cppcoreguidelines-macro-to-enum,modernize-macro-to-enum) */
/* clang-format on */

#endif /* MICNOTE_H */
