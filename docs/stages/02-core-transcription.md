# Stage 2: core, transcription and store

> **In plain terms.** Once recordings reach your phone or computer, this stage turns them into searchable, timestamped text, entirely on that device. It builds the parts of the core library that:
> - keep everything in an **encrypted database** (recordings, transcripts, and later actions), whose key lives in the operating system's protected vault;
> - run long tasks in a **job queue** in the background (sync, then transcribe, then, in stage 3, draft actions), remembering them across restarts and retrying failures;
> - turn speech into text with **whisper.cpp**, free speech-recognition software that runs on your own hardware, using the graphics chip where possible for speed, skipping silence (*VAD*, voice activity detection) and working through long meetings in 10-minute pieces;
> - choose and check the AI **models** (large downloaded files) that suit each device;
> - provide the **C ABI**, the small set of functions the app uses to talk to the core.
>
> **Done when:** a recording made on the prototype becomes a transcript, and the protocol and encryption tests pass.
>
> *Unfamiliar words are explained in the [glossary](../glossary.md); the whole system is explained without jargon in [How Micnote works](../how-it-works.md).*

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
core/src/asr/whisper_engine.cpp
core/src/models/registry.cpp       # + models/manifest.json, built in
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
  | Linux / Windows | `GGML_VULKAN=ON` if the Vulkan SDK and SPIRV-Headers are found, `GGML_CUDA` opt-in |
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
3. **VAD:** use whisper.cpp's built-in VAD (Silero-based model; `params.vad` + `vad_model_path`, verified at v1.9.5) to drop long silences. Keep the mapping from VAD-trimmed time back to recording time, so segment timestamps still line up with markers.
4. **Chunking:** whisper processes 30 s windows internally. For long meetings, call `whisper_full` on about 10-minute slices with 2 s of overlap, and de-duplicate segments in the overlap. This bounds memory use and allows progress reports and cancellation.
5. **Parameters:**
   - `WHISPER_SAMPLING_BEAM_SEARCH` (beam 5) on desktop, greedy on mobile.
   - `language="en"` by default, or `auto` per settings.
   - `token_timestamps` on.
   - `initial_prompt` seeded with vocabulary from settings (names, repos, jargon). This prompt mechanism improves proper-noun accuracy, e.g. "Jeff", repo names.
6. **Output:** `segments` rows plus the transcript metadata, with RTF (real-time factor) recorded.

**Model registry (`core/src/models/registry.cpp`):** reads `models/manifest.json` (stage 5b §4.4's format), which is compiled into the library so the pinned URLs and hashes always ship with the code; the model files live in `mn_config.models_dir` (default `data_dir/models`) under their URLs' file names. Besides the models, the manifest holds an ordered `defaults` list, so the choice below is data, not code:
```json
{"version":1,
 "models":[{"id":"whisper-large-v3-turbo-q5_0","kind":"asr","url":"https://huggingface.co/…/resolve/<commit>/ggml-large-v3-turbo-q5_0.bin",
            "sha256":"…","bytes":574041195,"min_ram_mb":2048,"classes":["desktop"],"license":"MIT"}, …],
 "defaults":[{"class":"desktop","min_ram_mb":7168,"asr":"whisper-large-v3-turbo-q5_0","vad":"silero-v6.2.0"}, …]}
```
- **Default choice:** desktop with ≥ 8 GB RAM and a GPU whisper can use gets `large-v3-turbo` q5 (on a CPU it took 3.3 times the recording's length, decided 2026-11-24); otherwise `small.en` q5; mobile gets `base.en`, or `small.en` with ≥ 8 GB (stage 5 §4.4's table, which is more specific than "≥ 6 GB"). "8 GB" is `min_ram_mb: 7168`, because an 8 GB machine reports a little under 8192 MB; phones below 5120 MB are `mobile-small`. Every class gets the Silero VAD model.
- The CLI takes `--model PATH` to override.

### 4.6 Secrets
- `Secrets::get_or_create(name)` returns 32 random bytes stored in:
  - macOS: Keychain;
  - Windows: Credential Manager (DPAPI-protected);
  - Linux: libsecret (Secret Service). If no Secret Service is running (headless), fall back to a file in the CLI's config directory (`~/.config/micnote/`, as built) with mode 0600 and a loud warning.
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
micnote-cli transcribe --file FILE.mnr|--recording ID [--model PATH | --models DIR] [--vad PATH]
                       [--backend auto|cpu|metal|vulkan|cuda]
micnote-cli db {recordings,transcripts [--recording ID],search "query"} [--data DIR]
micnote-cli models {list,verify}    # list: files, states, defaults, download hints; verify: SHA-256
micnote-cli run --usb PORT # daemon: auto-sync when the puck appears, then transcribe
```

## 5. Task breakdown
**PR 1: store and secrets**
- [x] M: SQLite3MultipleCiphers via FetchContent; the `Db` wrapper (prepared statements, transactions, WAL). *Note:* v2.7.1 (SQLite 3.54.1), the release amalgamation pinned by SHA-256 (`core/cmake/deps.cmake`, built by `core/cmake/sqlite3mc.cmake`): SQLCipher v4 cipher (`legacy=4`, raw 32-byte key), FTS5, AES in software (no per-CPU intrinsics on the app's toolchains). `core/src/store/db.*`; `Db::FailAfter()` injects a failure at the Nth call so tests sweep every error path (`core/tests/store_test.cpp`, which also checks the file is ciphertext and a wrong key gives `SQLITE_NOTADB`)
- [x] M: migrations + schema + repositories. *Note:* `core/src/store/{schema,repos}.*`, `migrations/0001_init.sql` embedded through a CMake-generated header, `PRAGMA user_version`, a newer schema refused. Deviations from §4.3, written in the migration: `devices` keyed by `device_id` (pairing uses a fresh device key, so there is no long-term `device_pub`), `recordings.mnr_path` (the §4.3 decision keeps the `.mnr`), and triggers keeping the FTS index in step. Timestamp repair (§4.4) is `core/src/time/clock_fix.*`
- [x] M: Secrets (keychain for the CLI + provider callback). *Note:* `core/src/secrets/`: the C ABI's callback store, the file store (0600, atomic replace; the documented headless fallback), `GetOrCreate` refusing a stored secret of the wrong size, and `KeychainSecrets` (`keychain.*`), which keeps each secret in the OS keychain as hex over a `KeychainBackend` (tested with a fake). Every store has `Remove` (the callbacks can't delete: `kProvider`). The real backend is CLI-only (`core/cli/os_keychain.*`, hrantzsch/keychain at main past v1.3.1 for `isAvailable()`, items under `org.micnote` / `micnote-cli`), so libmicnote and the app never link libsecret; on Linux it needs libsecret-1 and glib-2.0, or `-DMICNOTE_KEYCHAIN=OFF` for files only. `micnote-cli` (`core/cli/keys.*`) uses the keychain unless `--key FILE` says files; with no keychain (a Linux machine with no Secret Service) it uses the files in its config directory with a loud warning on every command. Deviation from §4.6: the fallback files stay in the CLI's config directory (`~/.config/micnote/`, where `keygen` and `pair` always put them), not `$XDG_DATA_HOME`, so there's one place for keys. `micnote_tests`' `OsKeychain` test round-trips through the real keychain only with `MICNOTE_TEST_OS_KEYCHAIN=1` (CI sets it on macOS and Windows; it passed once on a Linux desktop's Secret Service)
- [x] S: host key migration from `~/.config/micnote/host.key` into the keychain. *Note:* `MoveFileSecrets` moves `host.key`, `db_key` and `pucks/*.link` the first time the CLI finds a keychain. Each file is overwritten with zeros and deleted only once the keychain reads back the same bytes; one the keychain already has identically is just deleted; one that differs from the keychain's is kept, with a warning, and the keychain's is used; and if any file can't be moved, that command uses the files and says so. `micnote-cli` reports each move on stderr

**PR 2: C ABI**
- [x] M: `micnote.h`, `mn_call`/`mn_submit` dispatcher, JSON library, event bus. *Note:* `mn_context_create`/`destroy`, `mn_call`, `mn_submit`, `mn_cancel`, `mn_free`, `mn_abi_version` (`core/src/api/`). Methods: `devices.list`, `recordings.{list,get,delete}`, `transcripts.get`, `search.query`, `settings.{get,set}`, `jobs.{get,list}`; jobs: `sync.run`, `transcribe.run`; all `@1`. JSON is nlohmann (glaze would need proving on MSVC and Apple first). `mn_status` is an `int32_t` with the codes as constants, not an enum: ffigen refuses enums, whose size is up to the compiler. Events can arrive on the job queue's threads. `device.seen`/`device.connected` come with BLE discovery (stage 5)
- [x] S: ABI tests in C (not C++), so the header is guaranteed to be pure C. *Note:* `core/tests/abi_test.c`, run by `abi_test.cpp`; the methods themselves in `core/tests/context_test.cpp`
- [x] S: regenerate the Flutter ffigen bindings

**PR 3: jobs and sync integration**
- [x] M: JobQueue + persistence + retry. *Note:* `core/src/jobs/queue.*` over the `jobs` table (`migrations/0002_job_params.sql` adds `params_json` and a `cancelled` state, triggers refuse any other state). One worker thread per kind; a worker holds the store lock only to claim and settle a job. A retryable failure is queued again after 2^attempts min, the 5th failure (or a fatal one) is `dead`; `running` jobs are requeued at start; a job stopped by shutdown goes back without counting the attempt; per-kind pause (§4.8) via `SetPaused`. Events `job.progress`, `job.done`, `job.failed` (`retry_at`), `job.cancelled`, `job.paused`. Running jobs on threads made two fixes necessary: TF-PSA-Crypto is now built thread-safe (`MBEDTLS_THREADING_C`: pthreads on POSIX, SRWLOCK on Windows, `core/cmake/tfpsa/`; ThreadSanitizer found the key-slot race), and a failed `COMMIT` now rolls back (it left the long-lived connection inside the transaction, so every later write failed)
- [x] M: sync job (reuses stage 1's ProtocolClient), `device_clock`, timestamp repair. *Note:* `core/src/jobs/sync_job.*`. The session check moved into the core (`core/src/protocol/session.*`, shared with `micnote-cli`). Hello, Auth with the stored link key, SetTime, GetStatus; records the device and a clock observation and repairs earlier undated recordings from the same boot; then for each finished recording: Fetch, check it decrypts with this host's key (one that doesn't is left on the puck), write `data_dir/rec/<device>/<id>.mnr`, store the row with its repaired time, Ack, queue its transcription. A recording stored but not acked is acked again. Secret names match the CLI's files (`host.key`, `pucks/<device_id>.link`), so a file store rooted at the CLI's config directory sees what `micnote-cli pair` stored. The transcribe job (`core/src/jobs/transcribe_job.*`) gets its engine from the host (`ContextOptions::engine`); without one, `transcribe.run` is refused and recordings stay `synced`

**PR 4: ASR**
- [x] M: whisper.cpp FetchContent with backend options per preset. *Note:* v1.9.5 pinned in `core/cmake/deps.cmake`, built only with `-DMICNOTE_ASR=ON` into its own `micnote_asr` target (decided 2026-11-23), so the app's builds and the coverage job don't carry it; stage 5 turns it on for phones. `MICNOTE_ASR_GPU` sets whisper's `GGML_*` options: `auto` (Metal on Apple; Vulkan on Linux and Windows if its headers, loader, glslc and SPIRV-Headers are found, else the CPU; the CPU without OpenMP on Android), `metal`, `vulkan`, `cuda` or `off`. whisper and ggml are linked statically (until 2026-11-23 they were shared by mistake). `WhisperEngine::Load` takes a backend and reports the one it chose; `micnote-cli transcribe` and `run` take `--backend`. Built and run on Linux with the CPU and Vulkan (Intel Arc); CI's `asr` matrix adds Metal (not run yet). CUDA and Windows are untested. Core ML's encoder (§4.2, optional) isn't enabled. On x86-64, AVX2 is asked for by name instead of `GGML_NATIVE`, which isn't portable and is silently dropped when `SOURCE_DATE_EPOCH` is set or under Nix, making the CPU about 6 times slower (found 2026-11-24). A CPU without AVX2 (before about 2013-2015) therefore can't run these builds: stage 5b decides whether to support one
- [x] M: decode → VAD → sliced `whisper_full` → segments. *Note:* Opus decoding (`core/src/audio/opus_decode.*`, libopus at the firmware's commit, in `micnote_impl` for playback too), slicing into ~10 min windows with 2 s overlap and de-duplication in recording time (`core/src/asr/transcribe.*`, behind an `AsrEngine` interface, tested with a fake), and the whisper engine (`core/src/asr/whisper_engine.*`: beam 5 or greedy, language, initial prompt, token timestamps, per-segment avg log-prob and no-speech probability). VAD is whisper's own (`params.vad`, Silero v6.2.0 ggml model) per slice, turned on by passing a VAD model to `WhisperEngine::Load` (checked there, so a bad file is `kModel` up front, not a failed job); whisper maps segment times back to the slice's timeline itself, so no separate `vad.cpp` or mapping table of ours, and a slice with no speech gives no segments. `micnote-cli transcribe --vad SILERO.bin`. `core/tests/asr_whisper_test.cpp` transcribes whisper.cpp's JFK sample, also after a round trip through the puck's Opus settings, and between 15 s silences with VAD (segments still in recording time; 20 s of hiss gives none), using the Silero model in whisper.cpp's tree. The model registry (next task) should list the VAD model too (`kind: "vad"`)
- [x] M: model registry + selection. *Note:* `core/src/models/registry.*` over `models/manifest.json` (embedded through a CMake-generated header, like the migrations): four pinned models (large-v3-turbo q5_0, small.en q5_1, base.en q5_1 from ggerganov/whisper.cpp, Silero v6.2.0 from ggml-org/whisper-vad, all MIT; hashes from Hugging Face's LFS metadata, tiny.en's matching CI's pin and Silero's matching the copy in whisper.cpp's tree), every field validated (https URLs whose last component is a plain file name, so nothing lands outside the models directory; 64-digit SHA-256; defaults that name a model of the right kind for that class). `Check` (present, wrong size, missing) is cheap; `Verify` streams SHA-256 in 64 KB blocks (`crypto::Sha256Stream`) with progress and cancel. RAM from the OS (`sysconf`, `sysctl hw.memsize`, `GlobalMemoryStatusEx`); phone builds are mobile. ABI: `models.list@1` (device, models with state, default and chosen flags, the models directory) and `models.select@1` (`{kind, id|null}`, stored as the setting `models.<kind>`; a choice the manifest no longer has falls back to the default); `mn_config.models_dir` is now used. CLI: `micnote-cli models list|verify [--models DIR] [--id ID]` (`core/cli/models.*`, default `$XDG_DATA_HOME/micnote/models`). Hashing a 574 MB model takes seconds, so the ABI has no verify method yet: it comes as a job with stage 5b's downloader, which verifies as it goes
- [x] S: `micnote-cli transcribe`, `run` daemon. *Note:* in ASR builds, `transcribe --file FILE.mnr [--key FILE]` transcribes a file here and stores nothing, and `transcribe --recording ID [--data DIR]` queues a transcription job on the store (as the app does), shows its progress and prints the stored transcript; both take `[--model MODEL.bin | --models DIR] [--vad SILERO.bin] [--language LANG] [--beam N]`, and without `--model` use the registry's defaults (`core/cli/transcribe.*`). Flags rather than §4.9's positional file, which keeps all ASR-only code out of the coverage-gated parser. `run --usb PORT [--data DIR] [--models DIR]` (`core/cli/run.cpp`) watches the port and queues a sync each time it appears, never two at once; the job queue chains transcription with the registry's model (loaded once, on the first job), and it reports stored recordings, transcripts and failures until Ctrl-C. BLE discovery waits for stage 5, so `run` finds the puck by its USB port for now. `db recordings | transcripts [--recording ID] | search QUERY [--data DIR]` (`core/cli/db.cpp`) reads the store through the C ABI's methods, opening it with `ContextOptions::run_jobs = false` so a read never starts a leftover job. The store lives in `$XDG_DATA_HOME/micnote` (`%LOCALAPPDATA%\micnote`), its secrets beside the host key, where `keygen` and `pair` put them. `--backend` (§4.9) picks the compute backend

**PR 5: accuracy and speed**
- [ ] M: WER harness: a LibriSpeech test-clean subset (about 100 utterances) + 5 of your own recorded memos with hand-checked transcripts. *Partial:* the harness, `micnote-wer` (`core/bench/`, in ASR builds): scores `--librispeech DIR` (an unpacked test-clean, 100 clips spread evenly, the same ones every time) or `--memos DIR` (each `memo.wav`/`.flac`/`.mp3` with its `memo.txt`), optionally through the puck's Opus settings (`--codec`), and prints a row for `docs/measurements/stage2.md` with the WER and the RTF (the first clip is run once untimed, so GPU shader compilation isn't counted). Numbers aren't spelled out before comparing, so "20" for "twenty" counts as an error. The scoring is tested in `core/tests/wer_test.cpp`. The data isn't in the repo: test-clean is a 350 MB download (https://www.openslr.org/12, `test-clean.tar.gz`, MD5 32fa31d27d2e1cad72775fee3f4849a9), and memos belong in `/recordings/`, which git ignores. LibriSpeech is measured (next task); there are no personal memos to use (2026-11-24), so that half waits
- [ ] M: measure RTF per backend on: Linux desktop (CPU, Vulkan), a Mac (Metal), and later a phone (stage 5). Record results in `docs/measurements/stage2.md`. *Partial:* Linux CPU and Vulkan are in [`docs/measurements/stage2.md`](../measurements/stage2.md). No Mac is available (2026-11-24), so Metal isn't measured. Open: large-v3-turbo, the desktop default, has an RTF of 3.3 on a laptop CPU, so since 2026-11-24 the manifest gives it only to desktops with a GPU (`"gpu": true` on the default)

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
