# Stage 1: firmware and sync

## 1. Goal
Turn the Thingy:53 (and the nRF5340-DK with breakouts) into a working puck:
- records Opus audio with no host present, encrypted at rest;
- follows the storage, time and feedback rules from PLAN.md §1;
- pairs with hosts and syncs recordings over BLE (L2CAP) and USB.

On the host side, build just enough of `libmicnote` (transport, protocol, crypto) for `micnote-cli pair` and `micnote-cli sync` to serve as the test harness.

**Exit criterion (from PLAN.md):** `micnote-cli sync` pulls a recording made with no host present; it decrypts and plays back.

## 2. Depends on / unblocks
- **Depends on:** 0b (skeletons, CI).
- **Hardware needed** (parts, links and prices: [`docs/hardware/prototype-parts.md`](../hardware/prototype-parts.md)):
  - Thingy:53 + nRF5340-DK (as the J-Link debugger).
  - DK breakouts: 2× IM73D122 or T5838 PDM mics, a 64 MB QSPI NOR (W25Q512NW or JV), a TMAG5273, a DRV2605L + LRA.
  - An adhesive MagSafe ring.
  - PPK2.
- **Unblocks:** 2 (real recordings to transcribe), 5 (mobile BLE bridge targets this protocol), 6a (validates the mic, flash, snap-detection and power choices before the PCB is designed).

## 3. Deliverables
```
firmware/app/src/
  main.c                    # init order, state machine owner
  recorder.c                # one recording: encoder thread -> ring -> storage thread -> recstore
  audio/{pdm_capture.c, preproc.c, encoder.c, pipeline.c}
  storage/{recstore.c, recstore.h, policy.c, hosts.c}
  crypto/{keys.c, wrap.c, aead.c, pairing.c}  # keys.c: fallback key; pairing.c: link key, SAS, proofs
  ble/{ble.c, gatt_svc.c, l2cap.c, adv.c}   # ble.c: security, connection parameters, link thread
  usb/{cdc_transport.c}
  proto_link/{framing.c, dispatch.c}   # shared by BLE L2CAP, GATT fallback and USB
  ui/{led.c, haptic.c, button.c, gestures.c, patterns.c}
  sensors/{snap.c, power.c}
  timekeeping.c
firmware/modules/opus/       # Zephyr module wrapper for xiph/opus (pinned in west.yml)
firmware/tests/              # twister suites (native_sim)
firmware/bsim/               # BLE end to end in BabbleSim (puck, host, stranger)
proto/micnote.proto          # full v1 message set (§4.3)
proto/testvectors/           # golden encrypted recording + keys, shared with core tests
core/src/transport/{transport.hpp, ble_simpleble.cpp, usb_serial.cpp}
core/src/protocol/{framing.cpp, client.cpp, client.hpp}
core/src/crypto/{psa.cpp, pairing.cpp}
core/src/recording/{mnr.cpp, reader.cpp, ogg_writer.cpp}
core/cli/{main.cpp, commands.cpp}
docs/protocol.md             # complete protocol spec (this doc's §4.3–4.6, kept authoritative)
```

## 4. Design detail

### 4.1 Firmware architecture
```
            ┌──────────── main state machine (main.c) ─────────────┐
 button ──► │ SLEEP ⇄ IDLE ⇄ RECORDING   ·  ADV_SLOW / ADV_FAST    │
 accel  ──► │ CONNECTED (BLE|USB) · PAIRING · DFU                  │
 snap   ──► └───────┬──────────────┬─────────────┬─────────────────┘
                    │              │             │
         audio pipeline     recstore (LittleFS)   link (framing+dispatch)
   PDM DMA → ring → Opus →  encrypt → append      ├─ L2CAP CoC
                                                   ├─ GATT fallback
                                                   └─ USB CDC ACM
```
**Threads and priorities:**
- PDM callback runs in ISR/workqueue context and only copies into a ring buffer.
- The `encoder` thread (priority 5) runs Opus and hands frames to `recstore`.
- The `storage` thread (priority 6) batches flash writes.
- The `link` thread (priority 7) handles transfers.
- UI runs on the system workqueue.

**Init order:** power/PMIC → settings (NVS) → timekeeping → flash + LittleFS mount → crypto keys → UI → sensors → BLE → USB.

### 4.2 Audio pipeline
- **Capture:** Zephyr DMIC API on the nRF5340 PDM peripheral.
  - 16 kHz, 16-bit, stereo (L/R mics on one bus).
  - 10 ms blocks (320 samples per channel), from a memory slab of 8 blocks.
  - The Thingy:53 has one mic, so it runs in mono there.
  - PDM clock about 1.28 MHz (ratio 80). *Verify* the IM73D122 and VM3011 supported clock ranges.
- **Pre-processing:**
  - DC-blocking high-pass (one-pole, about 80 Hz).
  - Fixed digital gain chosen per board in devicetree or Kconfig.
  - **v1 stores a mono mix (L+R)/2.** Stereo storage for host-side beamforming is a stage 6b decision (see §8).
- **Encoding:** libopus fixed-point (`FIXED_POINT`, `DISABLE_FLOAT_API`).
  - Settings: `OPUS_APPLICATION_VOIP`, 16 kHz (wideband), 20 ms frames, `OPUS_SET_BITRATE(16000)`, VBR on, `OPUS_SET_COMPLEXITY(3)`, then tuned down if CPU load is above 40% of the app core at 64 MHz.
  - Encoder state and stack, measured (`firmware/tests/opus`, Cortex-M33 under QEMU, libopus 1.6.1): 15.2 KB of encoder state; the encode uses 24.3 KB of stack at complexity 3 and 23.0 KB at complexity 0. Budget: 32 KB of state plus a **32 KB** encoder thread stack (the first estimate, 8 KB, was before any measurement). `VAR_ARRAYS` puts Opus's scratch on that stack; the other choice, the global pseudo-stack, moves the same memory to a static buffer and saves nothing.
- **Output:** Opus packets of about 40 bytes each, 50 per second. Each one is appended to the current plaintext chunk buffer as `u16 len | packet`.
- **Markers:** when the user marks an item (long press or double tap), a `0xFFFF` length sentinel goes into the chunk, followed by `u8 marker_type | u32 offset_ms`.

### 4.3 Protocol (`proto/micnote.proto`, package `micnote.v1`)

**Transport framing:** each message is a varint length followed by a `Frame`:
```proto
message Frame { uint32 req_id = 1; oneof body { ... all messages below ... } }
```

**Control messages:**
| Request (host → puck) | Response (puck → host) | Notes |
|---|---|---|
| `Hello{proto_version, host_id(bytes8), host_nonce(bytes16)}` | `HelloAck{device_info, device_nonce}` | First message on every link. Unknown `host_id` → `Error{UNPAIRED}` (except during pairing) |
| `GetStatus{}` | `DeviceStatus{battery_pct, battery_mv, charge_source(enum NONE/USB/QI), storage_used_kb, storage_total_kb, recording_count, fw_version, boot_count, uptime_ms, rtc_valid, unix_ms, state(enum)}` | Also sent unsolicited on state changes |
| `SetTime{unix_ms, tz_offset_min}` | `Ok{}` | Sent by the host after every `Hello` |
| `ListRecordings{after_id}` | `RecordingList{repeated RecordingInfo entries, bool more}` | Paginated, 16 per page |
| `Fetch{recording_id, from_seq}` | stream of `Chunk{recording_id, seq, bytes payload}` then `FetchDone{recording_id, last_seq, bytes header}` | `payload` is ciphertext + tag as stored |
| `Ack{recording_id, bytes32 sha256}` | `Ok{}` or `Error{HASH_MISMATCH}` | Hash covers header + all stored chunk records. On a match, the recording is deleted |
| `PairBegin{host_pub(bytes32), host_name}` | `PairChallenge{device_pub(bytes32), device_nonce}` | Only valid in PAIRING state |
| `PairConfirm{bool user_confirmed}` | `PairDone{host_slot}` | After the SAS comparison (§4.6) |
| `ListHosts{}` / `Unpair{host_id}` | `HostList{…}` / `Ok{}` | Unpairing re-wraps nothing; that host simply can't read *future* recordings |
| — | `Error{code, detail}` | Codes: UNPAIRED, BUSY, NOT_FOUND, HASH_MISMATCH, STORAGE, BAD_STATE, PROTO_VERSION, BAD_REQUEST, HOSTS_FULL (the last two added in PR 1) |

**`RecordingInfo` fields:**
- `id(u64)`;
- `start_boot(u32)`, `start_uptime_ms(u64)`, `start_unix_ms(u64, 0 if RTC invalid)`;
- `duration_ms`, `size_bytes`, `last_seq`, `marker_count`;
- `complete(bool)`: false while it is still recording.
- `duration_ms` and `marker_count` are 0 from the puck: they live in the encrypted trailer, which it can't read (see `docs/protocol.md` section 4).

**Firmware updates:** DFU uses the standard MCUmgr SMP service (separate GATT service; SMP over CDC on USB), not this protocol.

### 4.4 Recording file format (`/rec/<id-hex>.mnr` on LittleFS)
All integers are little-endian. Code: `firmware/app/src/storage/mnr.{h,c}`.
```
Header core (plaintext, written once, never changed; chunks authenticate it)
  magic      "MNR1"            4
  version    u8 = 1            1
  hdr_len    u16               2   total header length; the first chunk starts here
  flags      u8 (bit0 stereo)  1
  rec_id     u64               8   = (boot_count << 32) | per-boot counter
  codec      u8 = 1 (Opus)     1
  rate_hz    u16 = 16000       2
  frame_ms   u8 = 20           1
  start_boot u32, start_uptime_ms u64, start_unix_ms u64       20
                                                        core = 40
Wrap table (plaintext, fixed size, rewritten in place at first pairing)
  n_wraps    u8                                                  1
  4 slots    { host_id[8], eph_pub[32], wrapped_cek[32], tag[16] }  88 each; unused slots zero
                                               header = 393 (hdr_len)
Chunk records (repeated)
  seq       u32
  ct_len    u16               (1 ..= 4096)
  ciphertext[ct_len]
  tag       [16]
Trailer = last chunk with plaintext type byte 0x02 + {duration_ms, marker_count}
```
- **Why core + a fixed table** (changed in PR 3 and confirmed 2026-11-22; the first draft had one ephemeral key, a variable-length wrap list and `hdr_len` last): the first pairing replaces the device fallback wrap with the host's (§4.5). With the AAD covering only the core and the table a fixed size, that rewrite doesn't shift the chunks or break their tags. `hdr_len` comes before anything variable so a reader can skip fields a later v1 writer appends (up to 512 bytes, `FetchDone.header`'s limit).
- **Plaintext chunk:** `type u8 (0x01 audio, 0x02 trailer)`, then packets and markers as in §4.2. Flushed when it reaches 4 KB, which is about 2 s of audio, so a power loss loses at most about 2 s.
- **Crash recovery** (`mn_recstore_recover()`, run at boot): a recording is written as `<id>.part` and renamed to `<id>.mnr` when it closes. At boot, every `.part` is truncated after its last complete, in-sequence chunk record and renamed to `.mnr`. It has no trailer: the host reads that as "ended by a power cut" and counts the duration from the packets. The puck can't write a trailer itself, because the CEK was only ever in RAM (the first draft of this section assumed it could). A `.part` with no complete header holds no key wraps and can never be decrypted; it is deleted, the one exception to "never delete without an Ack". *Decided 2026-11-22.*

### 4.5 Cryptography (PSA Crypto API on both sides: NCS on firmware, Mbed TLS/TF-PSA-Crypto via FetchContent on host)
- **Per recording:**
  1. Generate a random 256-bit CEK (content encryption key).
  2. For each paired host `H_i` (or the device fallback key if no host is paired yet), with a fresh ephemeral X25519 keypair `e_i` per wrap:
     - `ss_i = X25519(e_i_priv, H_i_pub)`
     - `KEK_i = HKDF-SHA256(ikm=ss_i, salt=rec_id(8, LE)‖e_i_pub, info="micnote/v1/kek"‖host_id_i)`
     - `wrapped_i = AES-256-CCM(KEK_i, nonce=0^13, aad=host_id_i, CEK)`. A zero nonce is safe here because each KEK is used exactly once.
     - Store `{host_id_i, e_i_pub, wrapped_i, tag_i}` in slot i, and destroy `e_i_priv`.
  3. Write the header.
- **Chunks:** `AES-256-CCM(CEK, nonce = rec_id(8, LE)‖seq(4, LE)‖0x00, aad = SHA-256(header core)‖seq(4, LE), plaintext)`, with a 16-byte tag. The CEK stays in RAM only while the recording is open, then is zeroized. **After a recording closes, the puck can no longer decrypt it.**
- **Device fallback key (before any host is paired):** an X25519 keypair generated on first boot.
  - Stored as a PSA persistent key with the Hardware Unique Key / KMU protection that NCS offers on the nRF5340 (*verify* which mechanism: KMU-backed HUK + `trusted_storage`).
  - At first pairing, the device unwraps the CEKs of its pending recordings with this key, wraps each to the new host with a fresh ephemeral key, and writes that wrap over the fallback wrap's slot (one in-place header rewrite per recording; the chunks are untouched).
- **Host keys:** X25519 per host install. In this stage, `micnote-cli` stores it in `~/.config/micnote/host.key` (mode 0600). Stage 2 moves it into the OS keychain.
- **Host IDs:** `host_id` is the first 8 bytes of `SHA-256(host_pub)`.

### 4.6 Pairing (no display on the puck)
The puck has no screen or keypad, so standard BLE numeric comparison isn't possible. Instead:
1. **Enter pairing:** hold the button for 5 s. The LED breathes blue, and the PAIRING window lasts 60 s.
2. **BLE link:** LE Secure Connections "Just Works" bonding. This protects against passive sniffing but not against an active man-in-the-middle, which steps 3 and 4 cover.
3. **App-layer exchange:** `PairBegin`/`PairChallenge` swap X25519 public keys and nonces. The puck's key is **fresh for each pairing** (decided 2026-11-22) and destroyed once it's over; both sides also derive the **link key** that authenticates every later session (protocol §3).
4. **Short authentication string (SAS):** both sides compute `SAS = HKDF-SHA256(ikm = X25519(host, device), salt = host_nonce‖device_nonce, info = "micnote/v1/sas"‖host_pub‖device_pub, L = 4)` and map byte `i` to symbol `i` mod 6: **4 symbols from 6 LED colors** (about 10.3 bits; decided 2026-11-22, exact formulas in [protocol §8](../protocol.md#8-pairing)). The puck plays the color sequence on loop, the app shows the same sequence, and the user confirms the match in the app. The puck then requires a short button press as well.
5. **RSSI gate:** the host only allows pairing when RSSI is above about −50 dBm (*tune*), i.e. the puck is held near or snapped on.
6. **USB pairing (desktop):** over USB CDC the physical cable is the trust anchor. The SAS step is skipped, but the button press is still required.
7. **Limits:** up to 4 host slots. A 5th pairing is refused with a red LED until one is unpaired.

*Note:* this replaces the original "numeric code in app" idea; PLAN.md §1 has been updated to match.

### 4.7 BLE details
- **GATT service** `MICNOTE_SVC`, UUID `4442e606-0000-4e50-8c5e-40291fa0aab7` (generated 2026-11-23); the characteristics change the second group to `0001`–`0004`:
  | Char | UUID suffix | Props | Use |
  |---|---|---|---|
  | `ctrl` | `0001` | write, write without response, indicate | Fallback transport: the host writes the framed request stream here |
  | `data` | `0002` | notify | Fallback transport: every response, as notifications. (`ctrl`'s indicate is reserved; v1 doesn't use it.) |
  | `psm` | `0003` | read | L2CAP PSM, u16 LE (iOS discovers CoC PSMs this way) |
  | `status` | `0004` | read, notify | Compact status, 6 bytes: battery %, state, recording count (u16 LE), storage used %, flags (bit 0 sync_pending, bit 1 charging) |

  Every characteristic and the L2CAP channel need an encrypted link. Bonding is LE Secure Connections only (Just Works; legacy pairing is refused), and new bonds are accepted only while the PAIRING window is open; bonded hosts re-encrypt at any time.
- **L2CAP connection-oriented channel:** dynamic PSM, MTU/MPS 2048 / 247 (*tune*), credit-based flow control, framing as in §4.3.
- **Connection parameters:**
  - During transfer: 2M PHY, DLE 251, a 15 ms connection interval (iOS minimum), latency 0. Requested when the L2CAP channel opens; Zephyr holds a peripheral's requests for `CONFIG_BT_CONN_PARAM_UPDATE_TIMEOUT`, set to 1 s (default 5 s).
  - When idle: renegotiate to a 100–500 ms interval with latency 4, after 5 s without traffic.
- **Throughput target:** ≥ 50 KB/s to an iPhone. A 1-hour recording is about 7.2 MB, so that's about 2.5 min (*measure*).
- **Advertising:**
  | State | Interval | Duration | Payload |
  |---|---|---|---|
  | ADV_FAST (snapped on, or button) | 30 ms | 60 s | flags, service UUID, `sync_pending` bit in manufacturer data |
  | ADV_SLOW (after recording ends) | 1 s | 5 min | same |
  | none | — | — | default: no advertising |
- Advertising always uses a resolvable private address (RPA), rotated every 15 min.
- **The payload** (26 of 31 bytes): flags, the complete 128-bit service UUID, and manufacturer data with Bluetooth SIG's test company ID `0xFFFF` and one flags byte (bit 0: `sync_pending`). Service data keyed by the 128-bit UUID doesn't fit beside the UUID itself; the company ID gets replaced once Micnote has one (stage 5b).
- **Mbed TLS heap:** with Bluetooth enabled, Mbed TLS allocates from its own heap, 2 KB by default, which X25519 outgrows. The board builds need `CONFIG_MBEDTLS_HEAP_SIZE=8192` (found in BabbleSim; native_sim uses the host's `malloc`).

### 4.8 USB
- A USB device with CDC ACM (data, same framing) plus MCUmgr SMP over a second CDC interface. VID/PID: Nordic's test VID while developing, then the pid.codes ID in stage 5b.
- **On attach:** the puck advertises nothing over BLE and serves the USB link. Charging happens at the same time.

### 4.9 Storage policy (`policy.c`)
| State | Condition | Behavior |
|---|---|---|
| OK | used < 90% | normal |
| WARN | 90% ≤ used < 99% | amber flash after each record start/stop; status flag set |
| FULL | used ≥ 99%, or free < 64 KB | button press → red flash + long buzz, recording refused |
| (low battery) | battery < 5% | the active recording is finalized, then recording is refused |

**Hard rule:** recordings are never deleted except by a matching `Ack` or an explicit factory reset (hold the button 20 s while plugging in USB, then confirm with another press).

### 4.10 Timekeeping
- RTC timer from the 32.768 kHz LFXO. `unix_ms = rtc_base_unix + (uptime - rtc_base_uptime)` after the first `SetTime`.
- `boot_count` is kept in NVS and incremented at every boot. `rtc_valid` resets to false on a cold boot (the battery died).
- Each recording stores all three values (`start_boot`, `start_uptime_ms`, `start_unix_ms`), so the host can fix timestamps for recordings made while `rtc_valid=false`, using a later `SetTime` in the same boot.

### 4.11 UI tables
| Event | LED | Haptic |
|---|---|---|
| Recording start | solid red, then a slow red pulse every 3 s while recording | 1 short |
| Recording stop | green 0.5 s | 1 short |
| Item marker | white blink | 2 short |
| Sync complete | green double blink | 1 long |
| Storage WARN | amber 2 blinks | — |
| Storage FULL / refused | red 3 fast | 1 long |
| Pairing | blue breathe; then the SAS color loop | — |
| Low battery | red blink every 10 s | — |
| Status query (double-press) | battery: 1–5 green blinks; storage: 1–5 blue blinks | — |

**Button gestures:** tap = start/stop; long press 1 s (while recording) = marker; double-press = status; hold 5 s = pairing; hold 20 s + USB = factory reset. While recording, a double tap detected by the accelerometer is also a marker.

How `gestures.c` reports them (first pass, to be tried on the DK):
- A **tap** is reported 400 ms after the release, once no second press has come. Start and stop lag by that much, which is the cost of having a double-press at all.
- A **double** is reported at the second press, and that press can't also become a long press.
- **Long** fires at 1 s while the button is still down, so the marker buzz is immediate. Any longer hold reports it first.
- **Pair** fires on release between 5 s and 20 s. If it fired at 5 s, a user holding on for a factory reset would start pairing on the way.
- **Reset** fires at 20 s while the button is still down.
- The recognizer doesn't know the device state. `main.c` decides: long is a marker only while recording, pair is ignored while recording, and reset needs USB.

The status reply's blue blinks count *free* storage, so more blinks means more left for both groups. Durations in `patterns.c`: short buzz 40 ms, long 400 ms; marker blink 120 ms; status blinks 200 ms with 300 ms gaps and a 1 s pause between the groups.

The Thingy:53 has no LRA, so on that board haptic events are logged only. On the DK, the DRV2605L breakout is used.

### 4.12 Snap detection (`snap.c`)
1. **Wake:** the accelerometer's motion interrupt (ADXL362 on the Thingy, LIS2DW12 on the puck) wakes the sensor logic.
2. **Sample:** read the magnetometer (BMM150 / TMAG5273) at 20 Hz for 2 s.
3. **Baseline:** `B0` is an exponential moving average of the field, updated only while not snapped and not moving.
4. **SNAPPED** is declared when `|B − B0| > T_snap` for ≥ 300 ms **and** the accelerometer saw a shock above `G_snap` within the previous 500 ms. Starting values: T_snap = 1 mT and G_snap = 2 g, to be *tuned* with the MagSafe ring on an iPhone and a Pixel.
5. **UNSNAPPED** is declared when the field returns within `T_snap/2` of `B0` for 1 s.
6. **On SNAPPED:** ADV_FAST.
7. **Charger filter:** if the Qi power-good line is active, a snap is classified as "on charger" and does *not* trigger ADV_FAST (except Pixel Battery Share; distinguishing it is a stage 6b question). The Thingy:53 has no Qi, so this is only stubbed in stage 1.

Log each raw snap event over RTT, so the thresholds can be tuned from recorded traces.

How `snap.c` reads the steps above (first pass, before tuning):
- **The shock window** runs from 500 ms before the field first jumped up to the moment the snap is declared. The knock comes as the magnet grabs, so it can land just before the field moves or during the 300 ms confirmation. Each shock can confirm only one snap.
- **The baseline** moves 1/64 of the way per sample, a time constant of about 3 s at 20 Hz.
- **It freezes** while snapped, while moving, and for the 300 ms confirmation. Without that freeze, it absorbed about 9% of the ring's field before the snap was declared.
- **A change that outlasts the confirmation without a knock** is absorbed like drift, for example a laptop's magnets. The baseline never freezes for good.
- **Units:** fields in µT as integers, compared as the magnitude of the 3-axis difference, with no floating point. The arithmetic is safe up to TMAG5273's ±80 mT range.
- **The trace for tuning:** each sample's `dev_ut` (|B − B0|) and the baseline are exposed for RTT logging.

### 4.13 Power states
| State | Target current (puck estimates; measure on DK) |
|---|---|
| SLEEP (no adv, sensors in wake-on-motion) | ≤ 20 µA |
| ADV_SLOW | ≤ 60 µA average |
| RECORDING | ≤ 6 mA |
| CONNECTED transfer | ≤ 10 mA |

### 4.14 Host side (this stage only)
- **`ITransport`:** `connect()`, `send(span<const byte>)`, `on_receive(callback)`, `close()`. Implementations:
  - `BleSimpleBleTransport` uses an L2CAP channel where SimpleBLE supports it; otherwise it falls back to GATT ctrl/data. *Verify* SimpleBLE's L2CAP support per OS. If it's missing on Linux, use BlueZ L2CAP sockets directly via `socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP)`.
  - `UsbSerialTransport`.
- **`ProtocolClient`:** coroutine-free, using `std::expected`-returning blocking calls on a worker thread, plus an async wrapper for the app later. It does pagination, fetch resume from `last_seq`, hash calculation and acking.
- **`RecordingReader`:** parses the header, unwraps the CEK for this host, decrypts and authenticates chunks, and yields Opus packets and markers.
- **`OggOpusWriter`:** a minimal Ogg page writer (RFC 7845 over RFC 3533, no dependency), writing the puck's packets unchanged into a playable `.opus` file.
- **`micnote-cli`:**
  Implemented (USB only), each with `--usb PORT [--key FILE]` except `keygen`:
  ```
  micnote-cli keygen [--key FILE]        # the host key ($XDG_CONFIG_HOME/micnote/host.key, mode 0600)
  micnote-cli pair [--name NAME]         # protocol §8; link key -> <key dir>/pucks/<device_id>.link
  micnote-cli status
  micnote-cli list
  micnote-cli sync --out DIR [--no-ack]  # <id>.mnr and <id>.opus
  micnote-cli hosts                      # the puck's paired hosts
  micnote-cli unpair --host HOST_ID
  ```
  Planned: `scan` (nearby pucks over BLE, with RSSI and sync_pending). `settime` was dropped: every session sets the clock.

### 4.15 Shared test vectors (`proto/testvectors/`)
- `rec_two_hosts.mnr`: 250 packets with 2 markers, wrapped to `host_a` and `host_b`.
- `rec_fallback.mnr`: wrapped to the device fallback key only.
- `rec_power_cut.mnr`: cut off by a simulated power cut, then recovered (no trailer).
- `vectors.json`: the keys, and per recording the packet count, marker offsets, duration, SHA-256 of the decrypted packet stream and of the file (the `Ack` hash).

The firmware's `firmware/tests/testvectors` suite *produces* these deterministically (fixed keys and fixed "randomness") and prints them; `proto/testvectors.py` extracts them, and CI fails if the committed copy drifts. Core GoogleTest *consumes* them. The packets are real Opus from the firmware's encoder on a synthetic voice. See `proto/testvectors/README.md`.

## 5. Task breakdown
**PR 1: protocol and test vectors**
- [x] M: full `micnote.proto` + options, regenerate, `docs/protocol.md`. *Note:* `docs/protocol.md` fully specifies versioning, framing, control messages and sync; the sections for PRs 3 and 5 point back here until those PRs move them. Host authentication on `Hello` (threat model §6.1) is still open and must be decided before PR 5
- [x] M: framing (firmware C + host C++), with twister and GoogleTest round-trip tests. *Note:* host `core/src/protocol/framing.*` + `core/tests/framing_test.cpp` + `core/fuzz/fuzz_frame_decode.cpp`; firmware `firmware/app/src/proto_link/framing.*` + `firmware/tests/framing/`. The firmware module joins the app build with the dispatcher in PR 5

**PR 2: audio**
- [x] M: Opus west module + Zephyr CMake wrapper. *Note:* xiph/opus v1.6.1 pinned in `firmware/west.yml`; `firmware/zephyr/module.yml` makes `firmware/` a Zephyr module, so `CONFIG_OPUS=y` works in every build (`firmware/modules/opus/`: fixed point, `DISABLE_FLOAT_API`, `VAR_ARRAYS`, Arm DSP inline assembly). `firmware/tests/opus/` runs on native_sim and on `mps2/an521/cpu1` under QEMU (in CI too): bitrate, a decode round trip, stack use, and one packet-stream CRC that both platforms must give, which shows the inline assembly is bit-exact. Measurements in §4.2
- [ ] M: PDM capture on the Thingy (VM3011) and the DK (breakout mics); RTT dump of raw PCM for checking
- [ ] M: encoder thread; CPU profiling at complexity 0–5; pick the setting. *Partial:* the thread and everything it runs are done (`firmware/app/src/audio/{preproc,encoder,pipeline}.*`, `firmware/tests/audio/`, on native_sim and the Cortex-M33 under QEMU): the (L+R)/2 mix, the 80 Hz DC blocker and Q8 gain; an encoder that builds 20 ms frames from blocks of any size (its packets match `tests/opus` bit for bit), pads the last partial frame on stop and gives marker offsets; and the priority-5 thread over a pull source shaped like `dmic_read()`, ending on stop, end of input, a source fault or a sink error. CPU profiling needs the board (QEMU cycle counts mean nothing) and waits for the PDM task

**PR 3: storage and crypto**
- [ ] M: QSPI + LittleFS partitions (Thingy 8 MB, DK 64 MB)
- [x] L: `recstore`: header, chunking, trailer, crash recovery. *Note:* on native_sim (LittleFS over the simulated flash): `firmware/app/src/storage/{mnr,recstore}.*`, `firmware/tests/{mnr,recstore}/`. The header became core + fixed wrap table (§4.4), and recovery truncates and renames instead of writing a trailer (§4.4, decided 2026-11-22). Running on the boards' QSPI is the partitions task above
- [x] L: crypto: fallback key, CEK and wrap, chunk AEAD, zeroization. *Note:* wrap/unwrap (per-wrap ephemeral key, low-order key check), chunk seal/open and zeroization (`firmware/app/src/crypto/{wrap,aead}.*`, `firmware/tests/crypto/`, checked against known answers from pyca/cryptography). The device fallback key (`firmware/app/src/crypto/keys.*`, `firmware/tests/keys/`) is a PSA persistent X25519 key generated on first boot, usable for ECDH only (not exportable), kept across reboots and replaced by a factory reset; on native_sim it lives in Zephyr's secure storage (PSA ITS over settings/NVS). Moving recordings from it to the first host is `mn_recstore_rewrap()` / `mn_recstore_rewrap_all()` (same suite): the fallback slot is rewritten in place, chunks untouched. Which store backs it on the nRF5340 (KMU, or NCS trusted storage with the HUK) is the risk-table item, settled in stage 7. PSA note for the host side too: `psa_key_derivation_output_key()` needs the HKDF secret as a `PSA_KEY_TYPE_DERIVE` key, not raw bytes; and generating a key pair needs `PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE`, not just `_IMPORT`
- [x] M: storage policy and the low-battery finalize. *Note:* the rules (`firmware/app/src/storage/policy.*`, `firmware/tests/policy/`) and the recorder that applies them (`firmware/app/src/recorder.*`, `firmware/tests/recorder/`): it owns a recording from start to stop over the encoder thread, a ring buffer and the priority-6 storage thread (§4.1), refuses to start when FULL or under 5%, and finalizes the active recording (trailer and all) when the battery drops under 5%, storage turns FULL, or capture fails. The battery reading itself comes from `sensors/power.c` (hardware)
- [x] M: test vector generation (twister native_sim). *Note:* `firmware/tests/testvectors/` + `proto/testvectors.py` (extract, `--check` in CI); cross-checked by decrypting with pyca/cryptography. The packets are real Opus from `audio/encoder.c` on the shared test voice

**PR 4: UI, time and sensors**
- [ ] M: button gestures, LED patterns, haptic (DK DRV2605L). *Partial:* the logic is done (`firmware/app/src/ui/{gestures,patterns}.*`, `firmware/tests/ui/`): the gesture state machine, and the §4.11 LED and haptic tables with players that give the LED colour or motor state at any time. The drivers (`button.c` on gpio-keys, `led.c`, `haptic.c` on the DRV2605L), the SAS colour loop (PR 5) and trying the timings on the DK remain
- [ ] S: timekeeping + boot_count. *Partial:* done on native_sim (`firmware/app/src/timekeeping.*`, `firmware/tests/timekeeping/`): `boot_count` in settings on NVS, the SetTime clock (`unix_ms = base + uptime delta`, invalid until set and after every reset), recording IDs `(boot_count << 32) | n`. The simulator uses it. On the boards: the NVS partition, and keeping the clock across a warm reset (retained RAM) if we want `rtc_valid` to survive one, as §4.10 implies
- [ ] L: snap detection with trace logging; tuning sessions on an iPhone and a Pixel. *Partial:* the detector is done as a pure state machine (`firmware/app/src/sensors/snap.*`, `firmware/tests/snap/`, 19 cases on synthetic traces: snap and release, magnet without a knock, knock without a magnet, the shock window's edges, the charger, hysteresis, drift, full-scale fields). The sensor drivers, RTT trace logging and tuning on real phones remain

**PR 5: links**
- [ ] L: GATT service + L2CAP CoC + advertising states. *Partial:* done and tested in BabbleSim (`firmware/app/src/ble/`, `firmware/bsim/`, CI job `bsim`): the service and its four characteristics, the L2CAP channel on a dynamic PSM, the GATT fallback, the advertising modes and payload (`firmware/tests/adv/`), bonding policy, connection-parameter switching, and the link thread. A simulated host bonds, pairs, syncs (about 100 KB/s simulated), re-authenticates over L2CAP and GATT; a stranger is kept out. On the boards: the nRF5340 build (BLE joins `main.c`), throughput to real phones (PR 7)
- [ ] M: pairing flow + SAS LED sequence. *Partial:* the protocol side is done and runs end to end on the simulator: the pairing and session crypto (`firmware/app/src/crypto/pairing.*`, `core/src/crypto/pairing.*`, both checked against pyca/cryptography known answers in `firmware/tests/pairing/` and `core/tests/pairing_test.cpp`), the host slots with PSA persistent link keys (`firmware/app/src/storage/hosts.*`), and the dispatcher's session states (Hello → `device_proof` → `Auth`; PairBegin with a fresh key, PairConfirm waiting for the button, PairDone moving fallback recordings to the new host; Unpair; ListHosts; `firmware/tests/dispatch/`). It runs over BLE in BabbleSim too, bonding included. Remain on hardware: playing the SAS colours on the LED (`show_sas` → `led.c`), the button press from `button.c`, closing the 60 s window, and the host-side RSSI gate
- [ ] M: USB CDC transport + MCUmgr SMP. *Partial:* the transport-free request handler is done (`firmware/app/src/proto_link/dispatch.*`, `firmware/tests/dispatch/`: the authenticated session, pairing, SetTime, GetStatus, ListRecordings, Fetch with resume, Ack, ListHosts, Unpair), with recstore's read side (list, reader, hash-checked delete). `firmware/sim/` serves it on a native_sim pseudo-terminal, and `micnote-cli pair` then `sync` run end to end against it (`firmware/sim/e2e.py`, CI job `sim-e2e`). USB CDC and SMP on the boards remain
- [ ] S: MCUboot DFU check (`mcumgr image upload`)

**PR 6: host harness**
- [ ] M: `ITransport` (SimpleBLE + USB serial) and the BlueZ L2CAP fallback. *Partial:* the `Transport` interface and `UsbSerial` are done (`core/src/transport/`; raw termios + `poll` on Linux/macOS, tested over a pseudo-terminal; Win32 COM API on Windows, built by CI but not yet run against a device). BLE waits for hardware
- [x] M: `ProtocolClient` + resume + ack. *Note:* `core/src/protocol/client.*` (with `Auth`, `PairBegin`/`PairConfirm`/`PairCancel`, `ListHosts` and `Unpair` since PR 5) over the `Transport` interface (`core/src/transport/transport.hpp`), tested in `core/tests/client_test.cpp` against a fake puck serving the test vectors: a whole sync, pagination, resume after a dropped link, and every misbehaviour. Real transports are the `ITransport` task above
- [x] M: `RecordingReader` + `OggOpusWriter` + test-vector tests. *Note:* `RecordingReader` (`core/src/recording/{mnr,reader}.*`, `core/src/crypto/psa.*`, `core/tests/recording_test.cpp`, `core/fuzz/fuzz_recording_reader.cpp`) decrypts every vector the firmware wrote; host PSA comes from TF-PSA-Crypto 1.1.2 (release archive, pinned by SHA-256 in `core/cmake/deps.cmake`). `OggOpusWriter` (`core/src/recording/ogg_writer.*`, `core/tests/ogg_test.cpp`) wraps the packets as RFC 7845 Ogg Opus without re-encoding: pre-skip 312, pages of up to 1 s, granules from each packet's TOC, item markers as `CHAPTERnnn` comments; the test vectors (now real Opus) all convert
- [ ] M: `micnote-cli` commands. *Partial:* over USB, `keygen`, `pair` (prints the SAS colours, waits for the button, stores the link key in `pucks/<device_id>.link` beside the host key), `status`, `list`, `sync`, `hosts` and `unpair --host ID` (`core/cli/commands.*`, tested against the fake puck in `core/tests/cli_test.cpp`). Every command but `keygen` and `pair` opens an authenticated session. `sync` writes each recording as `<id>.mnr` after checking it decrypts, and as `<id>.opus`, then acks it (a failed `.opus` doesn't hold up the ack: the `.mnr` is the copy that counts). `scan` waits for BLE; there is no separate `settime`, since every session sets the clock

**PR 7: measurements**
- [ ] M: PPK2 profiles for every power state; throughput measurements to Linux, macOS and an iPhone (via a minimal iOS test app or the nRF Connect app); results recorded in `docs/measurements/stage1.md`

## 6. Testing and verification
- `west twister -T firmware/tests -p native_sim`: framing, recstore (including simulated power loss mid-chunk), policy, crypto, test vector generation, audio (pre-processing, encoder, encoder thread), the recorder, the fallback key and re-wrap (`keys`), pairing crypto and host slots (`pairing`), and the dispatcher's sessions and pairing. `tests/opus` and `tests/audio` also run on `mps2/an521/cpu1` (QEMU).
- **Simulator end to end** (`firmware/sim/e2e.py`, CI `sim-e2e`): pair a fresh simulated puck, refuse a second host, sync with and without ack across a restart.
- **BLE in BabbleSim** (`firmware/bsim/run.py`, CI `bsim`): a puck, a host and a stranger on one simulated radio channel; see `firmware/bsim/README.md`.
- `ctest --preset dev -R 'protocol|crypto|recording'`
- **Fuzz harnesses** (run in stage 7, added now): `fuzz_frame_decode` and `fuzz_recording_reader` (core).
- **Hardware end to end:**
  1. Unplug the Thingy from all hosts and record three memos with markers over 1 hour.
  2. Power-cycle it during the second memo.
  3. Run `micnote-cli sync --out /tmp/m`. All three `.opus` files play back; the second one ends cleanly at the power-cut point; marker offsets are printed.
  4. Re-run `sync`: nothing left on the puck, so the acks worked.
- **Pairing negative tests:** a wrong SAS → no bond; RSSI too low → refused; an unpaired host → `Error{UNPAIRED}`.
- **Sniffer:** an nRF Sniffer + Wireshark capture shows an encrypted link and RPA rotation.
- **Storage:** fill the flash with a test pattern and confirm the WARN and FULL behavior.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Opus CPU load too high at 64 MHz | Run the app core at 128 MHz while recording; lower the complexity; fallback codec: LC3 (in NCS, hardware-friendly) at 32 kbps, which costs storage |
| SimpleBLE lacks L2CAP on some OS | GATT notification fallback (slower), or native L2CAP code per OS in core |
| iOS L2CAP throughput below target | Tune connection interval/MTU; accept GATT notifications with 2M PHY + DLE |
| VM3011 on the Thingy differs from the puck mics | Thingy is for flow validation only; mic quality is judged on DK breakouts |
| KMU/HUK API complexity in NCS | Start with PSA persistent keys in internal flash with APPROTECT; harden in stage 7 |
| LED SAS is too weak (≈10 bits) | Keep the RSSI gate and the 60 s window; option to raise it to 5 symbols (≈13 bits) |

## 8. Open questions
- Stereo storage for meetings (24 kbps stereo Opus, roughly 1.5× storage) to allow beamforming and denoising on the host? Decide after the DK mic tests.
- LC3 vs Opus: Opus is the default. Re-check if CPU or power numbers disappoint.
- Exact SAS symbol count after usability testing.

## 9. Out of scope
- Transcription (stage 2).
- Mobile background wake-up (stage 5).
- Custom board files (stage 6b).
- Production signing keys (stage 7).
