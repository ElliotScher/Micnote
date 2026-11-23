/*
 * The C ABI from C (stage 2: "ABI tests in C, so the header is guaranteed
 * to be pure C"). abi_test.cpp runs these and reports any failure.
 */
#include <micnote.h>

#include <stdio.h>
#include <string.h>

/* A host's secure storage: one slot is enough for db_key. */
struct store {
  char name[64];
  uint8_t value[64];
  size_t len;
  int fail_get;
};

static int get_secret(void *ud, const char *name, uint8_t *out, size_t cap, size_t *len) {
  struct store *s = ud;
  if (s->fail_get) {
    return 7;
  }
  if (strcmp(s->name, name) != 0) {
    return 1;
  }
  if (s->len > cap) {
    return 2;
  }
  memcpy(out, s->value, s->len);
  *len = s->len;
  return 0;
}

static int set_secret(void *ud, const char *name, const uint8_t *data, size_t len) {
  struct store *s = ud;
  const size_t n = strlen(name);
  if (n >= sizeof(s->name) || len > sizeof(s->value)) {
    return 1;
  }
  memcpy(s->name, name, n + 1); /* not strcpy: MSVC deprecates it (C4996) */
  memcpy(s->value, data, len);
  s->len = len;
  return 0;
}

static int events;
static void on_event(void *ud, const char *json) {
  (void)ud;
  if (strstr(json, "\"type\"") != NULL) {
    events++;
  }
}

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      (void)snprintf(msg, cap, "%s:%d: %s", __FILE__, __LINE__, #cond); \
      return 1;                                                         \
    }                                                                   \
  } while (0)

int mn_abi_c_test(const char *data_dir, char *msg, size_t cap) {
  struct store s;
  mn_config config;
  mn_context *ctx = NULL;
  char *out = NULL;

  memset(&s, 0, sizeof(s));
  memset(&config, 0, sizeof(config));
  CHECK(mn_abi_version() == MN_ABI_VERSION);
  CHECK(strlen(mn_version()) >= 5);

  /* Bad arguments. */
  CHECK(mn_context_create(NULL, &ctx) == MN_E_INVALID && ctx == NULL);
  CHECK(mn_context_create(&config, NULL) == MN_E_INVALID);
  CHECK(mn_context_create(&config, &ctx) == MN_E_INVALID && ctx == NULL);

  config.data_dir = data_dir;
  config.get_secret = get_secret;
  config.set_secret = set_secret;
  config.on_event = on_event;
  config.user_data = &s;
  CHECK(mn_context_create(&config, &ctx) == MN_OK && ctx != NULL);
  CHECK(strcmp(s.name, "db_key") == 0 && s.len == 32);

  CHECK(mn_call(ctx, "recordings.list@1", NULL, &out) == MN_OK);
  CHECK(strcmp(out, "{\"recordings\":[]}") == 0);
  mn_free(out);

  CHECK(mn_call(ctx, "settings.set@1", "{\"key\":\"language\",\"value\":\"en\"}", &out) == MN_OK);
  mn_free(out);
  CHECK(mn_call(ctx, "settings.get@1", "{\"key\":\"language\"}", &out) == MN_OK);
  CHECK(strstr(out, "\"value\":\"en\"") != NULL);
  mn_free(out);

  CHECK(mn_call(ctx, "recordings.get@1", "{\"id\":42}", &out) == MN_E_NOT_FOUND);
  CHECK(strstr(out, "\"code\":\"not_found\"") != NULL);
  mn_free(out);
  CHECK(mn_call(ctx, "no.such@1", "{}", &out) == MN_E_INVALID);
  CHECK(strstr(out, "unknown_method") != NULL);
  mn_free(out);
  CHECK(mn_call(ctx, NULL, "{}", &out) == MN_E_INVALID && out != NULL);
  mn_free(out);
  CHECK(mn_call(NULL, "recordings.list@1", "{}", &out) == MN_E_INVALID);
  mn_free(out);
  CHECK(mn_call(ctx, "recordings.list@1", "{}", NULL) == MN_E_INVALID);
  /* Jobs: queued at once; refused with an error object. */
  {
    uint64_t job = 7;
    char *err = NULL;
    CHECK(mn_submit(ctx, "sync.run@1", "{\"usb\":\"/dev/micnote-nowhere\"}", &job, &err) == MN_OK);
    CHECK(job > 0 && err == NULL);
    CHECK(mn_submit(ctx, "sync.run@1", "{}", &job, &err) == MN_E_INVALID);
    CHECK(job == 0 && err != NULL && strstr(err, "invalid_params") != NULL);
    mn_free(err);
    CHECK(mn_submit(ctx, "sync.run@1", NULL, &job, NULL) == MN_E_INVALID);
    CHECK(mn_submit(ctx, NULL, "{}", &job, &err) == MN_E_INVALID && err != NULL);
    mn_free(err);
    CHECK(mn_submit(NULL, "sync.run@1", "{}", &job, NULL) == MN_E_INVALID && job == 0);
    CHECK(mn_submit(ctx, "sync.run@1", "{}", NULL, &err) == MN_E_INVALID);
    mn_free(err);
    CHECK(mn_submit(ctx, "sync.run@1", "{}", NULL, NULL) == MN_E_INVALID);
    CHECK(mn_cancel(ctx, 987654) == MN_E_NOT_FOUND);
    CHECK(mn_cancel(NULL, 1) == MN_E_INVALID);
  }
  mn_context_destroy(ctx);
  mn_context_destroy(NULL);
  mn_free(NULL);

  /* Reopened with the same stored key: the settings are still there. */
  CHECK(mn_context_create(&config, &ctx) == MN_OK);
  CHECK(mn_call(ctx, "settings.get@1", "{\"key\":\"language\"}", &out) == MN_OK);
  CHECK(strstr(out, "\"en\"") != NULL);
  mn_free(out);
  /* models_dir defaults to data_dir/models; given, it's where models are. */
  CHECK(mn_call(ctx, "models.list@1", NULL, &out) == MN_OK);
  CHECK(strstr(out, "/models\"") != NULL || strstr(out, "\\\\models\"") != NULL);
  mn_free(out);
  mn_context_destroy(ctx);
  config.models_dir = "/opt/micnote-test-models";
  CHECK(mn_context_create(&config, &ctx) == MN_OK);
  CHECK(mn_call(ctx, "models.list@1", NULL, &out) == MN_OK);
  CHECK(strstr(out, "\"models_dir\":\"/opt/micnote-test-models\"") != NULL);
  mn_free(out);
  mn_context_destroy(ctx);

  /* Another key for the same database: refused as a crypto error. */
  s.value[0] ^= 1U;
  CHECK(mn_context_create(&config, &ctx) == MN_E_CRYPTO && ctx == NULL);
  /* The host's storage failing. */
  s.fail_get = 1;
  CHECK(mn_context_create(&config, &ctx) == MN_E_CRYPTO && ctx == NULL);
  (void)events;
  return 0;
}
