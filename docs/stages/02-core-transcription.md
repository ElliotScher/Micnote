# Stage 2: core, transcription and store

## 1. Goal
Turn synced recordings into stored, timestamped transcripts, entirely on the host. `libmicnote` gains:
- an encrypted database;
- a persistent job queue: sync → transcribe → (plan, stage 3);
- whisper.cpp transcription with hardware acceleration;
- the C ABI that the Flutter app will use.

**Exit criterion (from PLAN.md):** a DK recording becomes a transcript, and the protocol and crypto unit tests pass.

## 2. Depends on / unblocks
- **Depends on:** 1 (transport, protocol, `RecordingReader`, real recordings).
- **Unblocks:** 3 (needs transcripts and the store), 4 (needs the C ABI).

## 3. Deliverables
```
core/include/micnote.h              # stable C ABI (v1)
core/src/api/                       # C ABI → C++ glue, JSON (de)serialization
core/src/store/{db.cpp, migrations/*.sql, repo_*.cpp}
core/src/secrets/{keychain.cpp}     # OS keychain wrapper
core/src/jobs/{queue.cpp, job_sync.cpp, job_transcribe.cpp}
core/src/audio/{opus_decode.cpp, resample.cpp, vad.cpp}
core/src/asr/{whisper_engine.cpp, model_registry.cpp}
core/src/time/{clock_fix.cpp}
core/cli/{transcribe.cpp, db.cpp, models.cpp}
core/tests/{store_*, audio_*, asr_*, abi_*}
core/tests/fixtures/{librispeech_subset/, reference_transcripts.json}
docs/measurements/stage2.md         # WER + speed per backend
```

## 4. Design detail

### 4.1 Library layout
```
mn_context (one per app process)
 ├─ Store (SQLite3MultipleCiphers, WAL, one writer thread + reader pool)
 ├─ Secrets (keychain)
 ├─ DeviceManager (paired pucks, transports, ProtocolClient)
 ├─ JobQueue (persistent, worker threads by job class)
 ├─ AsrEngine (whisper.cpp, lazily loaded model)
 └─ EventBus (→ C callbacks → Dart)
```
- **Threading:** one sync worker, one ASR worker (whisper uses its own internal thread pool), and one planner worker (stage 3).
- **Calls:** every public call is thread-safe and non-blocking. Long work is always a job.

### 4.2 Dependencies (added to `cmake/deps.cmake`, pinned by commit)
- whisper.cpp, built with these backend flags:
  | Platform | Flags |
  |---|---|
  | macOS / iOS | `GGML_METAL=ON`, `WHISPER_COREML=ON` (optional encoder) |
  | Linux / Windows | `GGML_VULKAN=ON` if the Vulkan SDK is found, `GGML_CUDA` opt-in |
  | Android | CPU NEON + `GGML_OPENMP=OFF` |
- opus (decode).
- speexdsp resampler, or the r8brain header; pick one (speexdsp is small, BSD).
- SQLite3MultipleCiphers amalgamation.
- `hrantzsch/keychain` (macOS Keychain, Windows Credential Manager, libsecret), plus custom Android and iOS paths via the Flutter side (§4.6).
- nlohmann/json, or glaze (C++23-friendly, fast). **Choose glaze** if it builds on all of the CI's compilers; otherwise nlohmann.

### 4.3 Database schema (migration `0001_init.sql`)
```sql
CREATE TABLE devices (
  id INTEGER PRIMARY KEY, device_pub BLOB UNIQUE NOT NULL, name TEXT,
  host_slot INTEGER, paired_at INTEGER NOT NULL, fw_version TEXT, last_seen INTEGER);
CREATE TABLE device_clock (                      -- for timestamp repair
  device_id INTEGER REFERENCES devices, boot_count INTEGER, uptime_ms INTEGER,
  unix_ms INTEGER, observed_at INTEGER, PRIMARY KEY(device_id, boot_count, uptime_ms));
CREATE TABLE recordings (
  id INTEGER PRIMARY KEY, device_id INTEGER REFERENCES devices, rec_id INTEGER NOT NULL,
  start_boot INTEGER, start_uptime_ms INTEGER, start_unix_ms INTEGER,   -- as reported
  start_unix_ms_fixed INTEGER, time_confidence TEXT CHECK(time_confidence IN ('rtc','repaired','unknown')),
  duration_ms INTEGER, opus_path TEXT NOT NULL,        -- decrypted Ogg Opus, inside app data dir
  sha256 BLOB, state TEXT NOT NULL,                    -- synced|transcribing|transcribed|planned|error
  created_at INTEGER NOT NULL, UNIQUE(device_id, rec_id));
CREATE TABLE markers (recording_id INTEGER REFERENCES recordings ON DELETE CASCADE,
  offset_ms INTEGER, kind INTEGER, PRIMARY KEY(recording_id, offset_ms));
CREATE TABLE transcripts (
  id INTEGER PRIMARY KEY, recording_id INTEGER UNIQUE REFERENCES recordings ON DELETE CASCADE,
  model TEXT, backend TEXT, language TEXT, created_at INTEGER, rtf REAL);
CREATE TABLE segments (
  transcript_id INTEGER REFERENCES transcripts ON DELETE CASCADE, idx INTEGER,
  start_ms INTEGER, end_ms INTEGER, text TEXT, avg_logprob REAL, no_speech_prob REAL,
  PRIMARY KEY(transcript_id, idx));
CREATE VIRTUAL TABLE segments_fts USING fts5(text, content='segments', content_rowid='rowid');
-- stage 3/4 tables created now so the schema is reviewed once:
CREATE TABLE items (id INTEGER PRIMARY KEY, recording_id INTEGER REFERENCES recordings,
  kind TEXT, start_ms INTEGER, end_ms INTEGER, text TEXT, created_at INTEGER);
CREATE TABLE actions (id INTEGER PRIMARY KEY, item_id INTEGER REFERENCES items,
  connector TEXT NOT NULL, action_type TEXT NOT NULL, state TEXT NOT NULL,
  current_revision INTEGER, idempotency_key TEXT UNIQUE NOT NULL, created_at INTEGER, updated_at INTEGER);
CREATE TABLE action_revisions (action_id INTEGER REFERENCES actions, rev INTEGER,
  author TEXT CHECK(author IN ('planner_local','planner_cloud','user')),
  payload_json TEXT NOT NULL, created_at INTEGER, PRIMARY KEY(action_id, rev));
CREATE TABLE executions (id INTEGER PRIMARY KEY, action_id INTEGER REFERENCES actions,
  rev INTEGER, started_at INTEGER, finished_at INTEGER, outcome TEXT, result_json TEXT, error TEXT);
CREATE TABLE audit (id INTEGER PRIMARY KEY, at INTEGER NOT NULL, actor TEXT, event TEXT, detail_json TEXT);
CREATE TABLE jobs (id INTEGER PRIMARY KEY, kind TEXT, ref_id INTEGER, state TEXT, attempts INTEGER,
  not_before INTEGER, last_error TEXT, created_at INTEGER, updated_at INTEGER);
CREATE TABLE settings (key TEXT PRIMARY KEY, value_json TEXT);
```
- **Encryption:** SQLite3MultipleCiphers with `cipher=sqlcipher` (compatible with SQLCipher v4). The key is a 256-bit random value from the keychain, applied via `PRAGMA key` right after opening the database.
- **Audio files:** decrypted `.opus` files are stored on disk, encrypted with a file key (AES-256-GCM, streamed in 64 KB blocks) and decrypted only while ASR runs or while playing. Alternatively they can stay as the original `.mnr` plus wrapped keys. **Decision: keep the original `.mnr`** (already encrypted, with the CEK unwrapped through the host key on demand). This avoids a second encryption format; `opus_path` then points at the `.mnr`.
- **Migrations:** numbered `.sql` files embedded with `#embed` (C++26, clang ≥19) or a CMake-generated header. `PRAGMA user_version` tracks the applied migration.

### 4.4 Timestamp repair (`clock_fix.cpp`)
- On every `Hello` + `GetStatus`, the host writes a `device_clock` row: (boot, uptime) ↔ unix_ms.
- A recording with `start_unix_ms = 0` is repaired when a `device_clock` row with the same `boot_count` exists: `fixed = row.unix_ms − (row.uptime_ms − start_uptime_ms)`, and confidence becomes `repaired`.
- Otherwise confidence is `unknown`, and the UI shows "time unknown, synced at …".

### 4.5 Audio → ASR pipeline
1. `RecordingReader` (from stage 1) → Opus packets.
2. Opus decode at 16 kHz mono → float PCM. This is already whisper's rate, so no resampling is needed now; the resampler is only for future stereo or 48 kHz input.
3. **VAD:** use whisper.cpp's built-in VAD (Silero-based model, *verify* the current API: `whisper_vad_*` / `params.vad`) to drop long silences. Keep the mapping from VAD-trimmed time back to recording time, so segment timestamps still line up with markers.
4. **Chunking:** whisper processes 30 s windows internally. For long meetings, call `whisper_full` on about 10-minute slices with 2 s of overlap, and de-duplicate segments in the overlap. This bounds memory use and allows progress reports and cancellation.
5. **Parameters:**
   - `WHISPER_SAMPLING_BEAM_SEARCH` (beam 5) on desktop, greedy on mobile.
   - `language="en"` by default, or `auto` per settings.
   - `token_timestamps` on.
   - `initial_prompt` seeded with vocabulary from settings (names, repos, jargon). This prompt mechanism improves proper-noun accuracy, e.g. "Jeff", repo names.
6. **Output:** `segments` rows plus the transcript metadata, with RTF (real-time factor) recorded.

**Model registry (`model_registry.cpp`):** reads `models.json`, which stage 5b ships and downloads:
```json
{"id":"whisper-large-v3-turbo-q5_0","file":"ggml-large-v3-turbo-q5_0.bin",
 "sha256":"…","size":574000000,"min_ram_mb":2048,"platforms":["desktop"],"kind":"asr"}
```
- **Default choice:** desktop with ≥ 8 GB RAM gets `large-v3-turbo` q5; otherwise `small.en` q5; mobile gets `base.en` (or `small.en` on devices with ≥ 6 GB RAM).
- The CLI takes `--model PATH` to override.

### 4.6 Secrets
- `Secrets::get_or_create(name)` returns 32 random bytes stored in:
  - macOS: Keychain;
  - Windows: Credential Manager (DPAPI-protected);
  - Linux: libsecret (Secret Service). If no Secret Service is running (headless), fall back to a file in `$XDG_DATA_HOME/micnote/` with mode 0600 and a loud warning.
- **iOS/Android:** core can't reach the Keychain or Keystore cleanly from C++. The ABI therefore accepts secrets **from the host app**: `mn_context_create` takes a `mn_secret_provider` callback. The Flutter app implements it with `flutter_secure_storage` (iOS Keychain, Android Keystore-backed EncryptedSharedPreferences). Desktop apps use the same callback path for consistency, so the C++ keychain code is only used by the CLI.
- **Stored secrets:** `db_key`, `host_x25519_priv`, and later the connector OAuth tokens (stage 4, stored by the Dart side directly).

### 4.7 C ABI (`micnote.h`, v1 sketch)
```c
#define MN_ABI_VERSION 1
typedef struct mn_context mn_context;
typedef enum { MN_OK=0, MN_E_INVALID=1, MN_E_IO=2, MN_E_CRYPTO=3, MN_E_NOT_FOUND=4,
               MN_E_BUSY=5, MN_E_DEVICE=6, MN_E_MODEL=7, MN_E_CANCELLED=8, MN_E_INTERNAL=99 } mn_status;

typedef struct {                     /* all strings UTF-8, owned by caller unless noted */
  const char* data_dir;
  const char* models_dir;
  int  (*get_secret)(void* ud, const char* name, uint8_t* out, size_t cap, size_t* len);
  int  (*set_secret)(void* ud, const char* name, const uint8_t* data, size_t len);
  void (*on_event)(void* ud, const char* event_json);   /* called on core threads */
  void* user_data;
} mn_config;

uint32_t    mn_abi_version(void);
const char* mn_version(void);                          /* static string */
mn_status   mn_context_create(const mn_config*, mn_context** out);
void        mn_context_destroy(mn_context*);

/* Request/response: JSON in, JSON out. Out-string freed with mn_free. */
mn_status   mn_call(mn_context*, const char* method, const char* params_json, char** result_json);
/* Async jobs: returns job id immediately; progress/completion arrive via on_event */
mn_status   mn_submit(mn_context*, const char* method, const char* params_json, uint64_t* job_id);
mn_status   mn_cancel(mn_context*, uint64_t job_id);
void        mn_free(void*);

/* Mobile transport bridge (stage 5): native BLE code pushes/pulls bytes */
mn_status   mn_link_open(mn_context*, const char* device_id, uint64_t* link_id);
mn_status   mn_link_rx(mn_context*, uint64_t link_id, const uint8_t* data, size_t len);
/* tx bytes delivered via on_event {"type":"link_tx","link":id,"b64":...} or a registered tx callback */
mn_status   mn_link_close(mn_context*, uint64_t link_id, int reason);
```
- **Why one generic `mn_call` instead of many functions?** It keeps the FFI surface tiny and stable, while methods change freely through JSON. Methods are versioned, e.g. `recordings.list@1`.
- **Methods in this stage:**
  - `devices.list`, `devices.status`;
  - `recordings.list`, `recordings.get`, `recordings.delete`;
  - `transcripts.get`, `search.query` (FTS);
  - `models.list`, `models.select`;
  - jobs: `sync.run`, `transcribe.run`;
  - `settings.get`, `settings.set`.
- **Events:** `{"type":"job.progress","job":7,"kind":"transcribe","pct":42}`, `job.done`, `job.failed`, `device.seen`, `device.connected`, `recording.new`, `transcript.ready`.
- **Errors:** inside C++, everything returns `std::expected<T, mn::Error>`. At the ABI boundary, errors become `mn_status` plus `{"error":{"code":..,"message":..}}` in the result JSON. Exceptions never cross the ABI (every entry point is wrapped in try/catch → `MN_E_INTERNAL`).

### 4.8 Job queue
- Job kinds: `sync(device)`, `transcribe(recording)`, `plan(recording)` (stage 3).
- State machine: `queued → running → done | failed(retryable) → queued (backoff 2^n min, max 5) | dead`.
- **Persistence:** jobs are stored in the DB. On startup, `running` jobs go back to `queued`.
- **Chaining:** sync completion enqueues transcribe for each new recording; transcribe completion enqueues plan.
- **Mobile constraints:** stage 5 sets `not_before` or keeps transcribe jobs held until the OS grants background processing time. The queue supports a "paused" flag per job kind.

### 4.9 CLI additions
```
micnote-cli transcribe FILE.mnr|--recording ID [--model PATH] [--backend cpu|metal|vulkan|cuda]
micnote-cli db {recordings,transcripts,search "query"}
micnote-cli models {list,verify}
micnote-cli run            # daemon: auto-sync when the puck appears, then transcribe
```

## 5. Task breakdown
**PR 1: store and secrets**
- [x] M: SQLite3MultipleCiphers via FetchContent; the `Db` wrapper (prepared statements, transactions, WAL). *Note:* v2.7.1 (SQLite 3.54.1), the release amalgamation pinned by SHA-256 (`core/cmake/deps.cmake`, built by `core/cmake/sqlite3mc.cmake`): SQLCipher v4 cipher (`legacy=4`, raw 32-byte key), FTS5, AES in software (no per-CPU intrinsics on the app's toolchains). `core/src/store/db.*`; `Db::FailAfter()` injects a failure at the Nth call so tests sweep every error path (`core/tests/store_test.cpp`, which also checks the file is ciphertext and a wrong key gives `SQLITE_NOTADB`)
- [x] M: migrations + schema + repositories. *Note:* `core/src/store/{schema,repos}.*`, `migrations/0001_init.sql` embedded through a CMake-generated header, `PRAGMA user_version`, a newer schema refused. Deviations from §4.3, written in the migration: `devices` keyed by `device_id` (pairing uses a fresh device key, so there is no long-term `device_pub`), `recordings.mnr_path` (the §4.3 decision keeps the `.mnr`), and triggers keeping the FTS index in step. Timestamp repair (§4.4) is `core/src/time/clock_fix.*`
- [ ] M: Secrets (keychain for the CLI + provider callback). *Partial:* `core/src/secrets/` with the C ABI's callback store and the file store (0600, atomic replace; the documented headless fallback), `GetOrCreate` refusing a stored secret of the wrong size. The OS keychain backend for the CLI (hrantzsch/keychain, libsecret on Linux) remains
- [ ] S: host key migration from `~/.config/micnote/host.key` into the keychain

**PR 2: C ABI**
- [x] M: `micnote.h`, `mn_call`/`mn_submit` dispatcher, JSON library, event bus. *Note:* `mn_context_create`/`destroy`, `mn_call`, `mn_submit`, `mn_cancel`, `mn_free`, `mn_abi_version` (`core/src/api/`). Methods: `devices.list`, `recordings.{list,get,delete}`, `transcripts.get`, `search.query`, `settings.{get,set}`, `jobs.{get,list}`; jobs: `sync.run`, `transcribe.run`; all `@1`. JSON is nlohmann (glaze would need proving on MSVC and Apple first). `mn_status` is an `int32_t` with the codes as constants, not an enum: ffigen refuses enums, whose size is up to the compiler. Events can arrive on the job queue's threads. `device.seen`/`device.connected` come with BLE discovery (stage 5)
- [x] S: ABI tests in C (not C++), so the header is guaranteed to be pure C. *Note:* `core/tests/abi_test.c`, run by `abi_test.cpp`; the methods themselves in `core/tests/context_test.cpp`
- [x] S: regenerate the Flutter ffigen bindings

**PR 3: jobs and sync integration**
- [x] M: JobQueue + persistence + retry. *Note:* `core/src/jobs/queue.*` over the `jobs` table (`migrations/0002_job_params.sql` adds `params_json` and a `cancelled` state, triggers refuse any other state). One worker thread per kind; a worker holds the store lock only to claim and settle a job. A retryable failure is queued again after 2^attempts min, the 5th failure (or a fatal one) is `dead`; `running` jobs are requeued at start; a job stopped by shutdown goes back without counting the attempt; per-kind pause (§4.8) via `SetPaused`. Events `job.progress`, `job.done`, `job.failed` (`retry_at`), `job.cancelled`, `job.paused`. Running jobs on threads made two fixes necessary: TF-PSA-Crypto is now built thread-safe (`MBEDTLS_THREADING_C`: pthreads on POSIX, SRWLOCK on Windows, `core/cmake/tfpsa/`; ThreadSanitizer found the key-slot race), and a failed `COMMIT` now rolls back (it left the long-lived connection inside the transaction, so every later write failed)
- [x] M: sync job (reuses stage 1's ProtocolClient), `device_clock`, timestamp repair. *Note:* `core/src/jobs/sync_job.*`. The session check moved into the core (`core/src/protocol/session.*`, shared with `micnote-cli`). Hello, Auth with the stored link key, SetTime, GetStatus; records the device and a clock observation and repairs earlier undated recordings from the same boot; then for each finished recording: Fetch, check it decrypts with this host's key (one that doesn't is left on the puck), write `data_dir/rec/<device>/<id>.mnr`, store the row with its repaired time, Ack, queue its transcription. A recording stored but not acked is acked again. Secret names match the CLI's files (`host.key`, `pucks/<device_id>.link`), so a file store rooted at the CLI's config directory sees what `micnote-cli pair` stored. The transcribe job (`core/src/jobs/transcribe_job.*`) gets its engine from the host (`ContextOptions::engine`); without one, `transcribe.run` is refused and recordings stay `synced`

**PR 4: ASR**
- [ ] M: whisper.cpp FetchContent with backend options per preset. *Partial:* v1.9.5 pinned in `core/cmake/deps.cmake`, built only with `-DMICNOTE_ASR=ON` into its own `micnote_asr` target (decided 2026-11-23), so the app's builds (mobile included) and the coverage job don't carry it; stage 5 turns it on for phones. The `asr` preset (RelWithDebInfo) and CI job `asr` build it CPU-only. Per-platform GPU flags (Metal, Vulkan, CUDA) are whisper's own `GGML_*` options, still to be set per preset
- [ ] M: decode → VAD → sliced `whisper_full` → segments. *Partial:* Opus decoding (`core/src/audio/opus_decode.*`, libopus at the firmware's commit, in `micnote_impl` for playback too), slicing into ~10 min windows with 2 s overlap and de-duplication in recording time (`core/src/asr/transcribe.*`, behind an `AsrEngine` interface, tested with a fake), and the whisper engine (`core/src/asr/whisper_engine.*`: beam 5 or greedy, language, initial prompt, token timestamps, per-segment avg log-prob and no-speech probability). `core/tests/asr_whisper_test.cpp` transcribes whisper.cpp's JFK sample, also after a round trip through the puck's Opus settings. VAD (whisper's Silero model) remains
- [ ] M: model registry + selection
- [ ] S: `micnote-cli transcribe`, `run` daemon. *Partial:* `micnote-cli transcribe --file FILE.mnr --model MODEL.bin [--key FILE] [--language LANG] [--beam N]` in ASR builds (`core/cli/transcribe.*`): decrypts, decodes, transcribes, prints segments, markers and the real-time factor. A flag rather than §4.9's positional file, which keeps all ASR-only code out of the coverage-gated parser. `--recording ID` (from the store) and `run`, both on the job queue, remain

**PR 5: accuracy and speed**
- [ ] M: WER harness: a LibriSpeech test-clean subset (about 100 utterances) + 5 of your own recorded memos with hand-checked transcripts
- [ ] M: measure RTF per backend on: Linux desktop (CPU, Vulkan), a Mac (Metal), and later a phone (stage 5). Record results in `docs/measurements/stage2.md`

## 6. Testing and verification
- `ctest --preset dev`:
  - store CRUD + migrations;
  - wrong DB key → `MN_E_CRYPTO`;
  - timestamp repair cases;
  - job retry/backoff with a fake clock;
  - ABI round-trips from C;
  - transcription of a 10 s fixture against a golden transcript (tolerant match: WER ≤ 5%).
- `ctest --preset asan` is clean.
- **Manual end to end:** `micnote-cli run`, then record a memo on the DK/Thingy, bring it in range, and the transcript appears (`micnote-cli db transcripts`).
- **Targets:** WER ≤ 8% on the LibriSpeech subset with large-v3-turbo; RTF ≤ 0.1 on a desktop GPU and ≤ 0.5 on a laptop CPU (*targets, adjust after measuring*).

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Whisper hallucinates on silence or noise | VAD trimming; drop segments with high `no_speech_prob`; flag low-confidence segments in the UI |
| A whisper.cpp backend fails to build on some CI OS | Backend flags are opt-in per preset; CPU is always built and tested |
| Long meetings use too much memory on mobile | Slicing (§4.5) + smaller models on mobile |
| SQLite3MC encryption scheme confusion | Pin `cipher=sqlcipher` + `legacy=4` explicitly; add a test that opens a fixture DB |
| The JSON ABI gets sloppy | A JSON schema per method in `core/api/schemas/`, validated in debug builds and in tests |

## 8. Open questions
- Speaker diarization for meetings (who said what)? Not in v1. Possible later with pyannote-like ONNX models via whisper.cpp's tinydiarize, or a separate model.
- Keep decoded audio for in-app playback, or decrypt on the fly? Decided: decrypt on the fly from `.mnr`. Revisit if playback seeking feels slow.
- glaze vs nlohmann: decide in PR 2 based on compiler support.

## 9. Out of scope
- Planning and actions (stages 3 and 4).
- The Flutter UI (stage 4).
- Model download UX (stage 5b; the CLI uses local paths).
