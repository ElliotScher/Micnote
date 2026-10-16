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
- **Hardware needed:**
  - Thingy:53 + nRF5340-DK (as the J-Link debugger).
  - DK breakouts: 2× IM73D122 or T5838 PDM mics, a 64 MB QSPI NOR (W25Q512NW or JV), a TMAG5273, a DRV2605L + LRA.
  - An adhesive MagSafe ring.
  - PPK2.
- **Unblocks:** 2 (real recordings to transcribe), 5 (mobile BLE bridge targets this protocol), 6a (validates the mic, flash, snap-detection and power choices before the PCB is designed).

## 3. Deliverables
```
firmware/app/src/
  main.c                    # init order, state machine owner
  audio/{pdm_capture.c, encoder.c, encoder.h}
  storage/{recstore.c, recstore.h, policy.c}
  crypto/{keys.c, wrap.c, aead.c}
  ble/{gatt_svc.c, l2cap.c, adv.c, pairing.c}
  usb/{cdc_transport.c}
  proto_link/{framing.c, dispatch.c}   # shared by BLE L2CAP, GATT fallback and USB
  ui/{led.c, haptic.c, button.c, gestures.c}
  sensors/{snap.c, power.c}
  timekeeping.c
firmware/modules/opus/       # Zephyr module wrapper for xiph/opus (pinned in west.yml)
firmware/tests/              # twister suites (native_sim)
proto/micnote.proto          # full v1 message set (§4.3)
proto/testvectors/           # golden encrypted recording + keys, shared with core tests
core/src/transport/{transport.hpp, ble_simpleble.cpp, usb_serial.cpp}
core/src/protocol/{framing.cpp, client.cpp, client.hpp}
core/src/crypto/{host_keys.cpp, unwrap.cpp, aead.cpp}
core/src/recording/{reader.cpp, ogg_writer.cpp}
core/cli/{pair.cpp, sync.cpp, status.cpp, list.cpp}
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
  - Measure the encoder state size and stack. Budget: 32 KB of state plus an 8 KB thread stack.
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
| — | `Error{code, detail}` | Codes: UNPAIRED, BUSY, NOT_FOUND, HASH_MISMATCH, STORAGE, BAD_STATE, PROTO_VERSION |

**`RecordingInfo` fields:**
- `id(u64)`;
- `start_boot(u32)`, `start_uptime_ms(u64)`, `start_unix_ms(u64, 0 if RTC invalid)`;
- `duration_ms`, `size_bytes`, `last_seq`, `marker_count`;
- `complete(bool)`: false while it is still recording.

**Firmware updates:** DFU uses the standard MCUmgr SMP service (separate GATT service; SMP over CDC on USB), not this protocol.

### 4.4 Recording file format (`/rec/<id-hex>.mnr` on LittleFS)
```
Header (plaintext, written once at start; authenticated via AAD)
  magic     "MNR1"            4
  version   u8 = 1            1
  flags     u8 (bit0 stereo)  1
  rec_id    u64               8   = (boot_count << 32) | per-boot counter
  codec     u8 = 1 (Opus)     1
  rate_hz   u16 = 16000       2
  frame_ms  u8 = 20           1
  start_boot u32, start_uptime_ms u64, start_unix_ms u64       20
  eph_pub   [32]                                               32   X25519 ephemeral public key
  n_wraps   u8                                                  1
  wraps[n]  { host_id[8], wrapped_cek[32], tag[16] }           56·n
  hdr_len   u16 (total header length, for forward compat)       2
Chunk records (repeated)
  seq       u32
  ct_len    u16               (≤ 4096)
  ciphertext[ct_len]
  tag       [16]
Trailer = last chunk with plaintext type byte 0x02 + {duration_ms, marker_count}
```
- **Plaintext chunk:** `type u8 (0x01 audio, 0x02 trailer)`, then packets and markers as in §4.2. Flushed when it reaches 4 KB, which is about 2 s of audio, so a power loss loses at most about 2 s.
- **Crash recovery:** at boot, any `.mnr` without a trailer gets a trailer synthesized from the last valid chunk, and is marked complete.

### 4.5 Cryptography (PSA Crypto API on both sides: NCS on firmware, Mbed TLS/TF-PSA-Crypto via FetchContent on host)
- **Per recording:**
  1. Generate a random 256-bit CEK (content encryption key) and an ephemeral X25519 keypair `e`.
  2. For each paired host `H_i` (plus the device fallback key if no host is paired yet):
     - `ss_i = X25519(e_priv, H_i_pub)`
     - `KEK_i = HKDF-SHA256(ikm=ss_i, salt=rec_id‖e_pub, info="micnote/v1/kek"‖host_id_i)`
     - `wrapped_i = AES-256-CCM(KEK_i, nonce=0^13, aad=host_id_i, CEK)`. A zero nonce is safe here because each KEK is used exactly once.
  3. Destroy `e_priv` right after wrapping, then write the header.
- **Chunks:** `AES-256-CCM(CEK, nonce = rec_id(8)‖seq(4)‖0x00, aad = SHA-256(header)‖seq, plaintext)`, with a 16-byte tag. The CEK stays in RAM only while the recording is open, then is zeroized. **After a recording closes, the puck can no longer decrypt it.**
- **Device fallback key (before any host is paired):** an X25519 keypair generated on first boot.
  - Stored as a PSA persistent key with the Hardware Unique Key / KMU protection that NCS offers on the nRF5340 (*verify* which mechanism: KMU-backed HUK + `trusted_storage`).
  - At first pairing, the device unwraps the CEKs of its pending recordings with this key and re-wraps them to the new host, then deletes the fallback wraps.
- **Host keys:** X25519 per host install. In this stage, `micnote-cli` stores it in `~/.config/micnote/host.key` (mode 0600). Stage 2 moves it into the OS keychain.
- **Host IDs:** `host_id` is the first 8 bytes of `SHA-256(host_pub)`.

### 4.6 Pairing (no display on the puck)
The puck has no screen or keypad, so standard BLE numeric comparison isn't possible. Instead:
1. **Enter pairing:** hold the button for 5 s. The LED breathes blue, and the PAIRING window lasts 60 s.
2. **BLE link:** LE Secure Connections "Just Works" bonding. This protects against passive sniffing but not against an active man-in-the-middle, which steps 3 and 4 cover.
3. **App-layer exchange:** `PairBegin`/`PairChallenge` swap X25519 public keys and nonces.
4. **Short authentication string (SAS):** both sides compute `SAS = HKDF(X25519(host,device), info="micnote/v1/sas", host_nonce‖device_nonce‖both pubs)` and map it to **4 symbols from 6 LED colors** (about 10 bits). The puck plays the color sequence on loop, the app shows the same sequence, and the user confirms the match in the app. The puck then requires a short button press as well.
5. **RSSI gate:** the host only allows pairing when RSSI is above about −50 dBm (*tune*), i.e. the puck is held near or snapped on.
6. **USB pairing (desktop):** over USB CDC the physical cable is the trust anchor. The SAS step is skipped, but the button press is still required.
7. **Limits:** up to 4 host slots. A 5th pairing is refused with a red LED until one is unpaired.

*Note:* this replaces the original "numeric code in app" idea; PLAN.md §1 has been updated to match.

### 4.7 BLE details
- **GATT service** `MICNOTE_SVC` (128-bit base UUID `4d4e0000-7a1c-4b8e-9c3a-6d696e6f7465`, *generate fresh at implementation*):
  | Char | UUID suffix | Props | Use |
  |---|---|---|---|
  | `ctrl` | `…0001` | write, indicate | Fallback transport for `Frame`s (small messages) |
  | `data` | `…0002` | notify | Fallback bulk transfer if L2CAP is unavailable |
  | `psm` | `…0003` | read | L2CAP PSM (iOS discovers CoC PSMs this way) |
  | `status` | `…0004` | read, notify | Compact `DeviceStatus` |
- **L2CAP connection-oriented channel:** dynamic PSM, MTU/MPS 2048 / 247 (*tune*), credit-based flow control, framing as in §4.3.
- **Connection parameters:**
  - During transfer: 2M PHY, DLE 251, a 15 ms connection interval (iOS minimum), latency 0.
  - When idle: renegotiate to a 100–500 ms interval with latency 4.
- **Throughput target:** ≥ 50 KB/s to an iPhone. A 1-hour recording is about 7.2 MB, so that's about 2.5 min (*measure*).
- **Advertising:**
  | State | Interval | Duration | Payload |
  |---|---|---|---|
  | ADV_FAST (snapped on, or button) | 30 ms | 60 s | flags, service UUID, `sync_pending` bit in service data |
  | ADV_SLOW (after recording ends) | 1 s | 5 min | same |
  | none | — | — | default: no advertising |
- Advertising always uses a resolvable private address (RPA), rotated every 15 min.

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
- **`OggOpusWriter`:** uses libopusenc (FetchContent), or a minimal Ogg page writer, to write a playable `.opus` file.
- **`micnote-cli`:**
  ```
  micnote-cli scan                       # list nearby pucks (RSSI, sync_pending)
  micnote-cli pair [--usb PORT]          # runs SAS flow, saves host key + device record
  micnote-cli status
  micnote-cli list
  micnote-cli sync [--out DIR] [--no-ack] [--usb PORT]
  micnote-cli settime
  micnote-cli unpair HOST_ID
  ```

### 4.15 Shared test vectors (`proto/testvectors/`)
- `host_a.key`, `host_b.key`, `device_fallback.key`
- `rec_two_hosts.mnr`: 10 s of a known tone with 2 markers, wrapped to A and B.
- `rec_fallback.mnr`
- `expected.json`: packet count, marker offsets, SHA-256 of the decrypted Opus stream.

Firmware twister tests *produce* these files deterministically (with a fixed RNG), and core GoogleTest *consumes* them. Running both sides in CI guarantees the two implementations agree.

## 5. Task breakdown
**PR 1: protocol and test vectors**
- [ ] M: full `micnote.proto` + options, regenerate, `docs/protocol.md`
- [ ] M: framing (firmware C + host C++), with twister and GoogleTest round-trip tests

**PR 2: audio**
- [ ] M: Opus west module + Zephyr CMake wrapper
- [ ] M: PDM capture on the Thingy (VM3011) and the DK (breakout mics); RTT dump of raw PCM for checking
- [ ] M: encoder thread; CPU profiling at complexity 0–5; pick the setting

**PR 3: storage and crypto**
- [ ] M: QSPI + LittleFS partitions (Thingy 8 MB, DK 64 MB)
- [ ] L: `recstore`: header, chunking, trailer, crash recovery
- [ ] L: crypto: fallback key, CEK and wrap, chunk AEAD, zeroization
- [ ] M: storage policy and the low-battery finalize
- [ ] M: test vector generation (twister native_sim)

**PR 4: UI, time and sensors**
- [ ] M: button gestures, LED patterns, haptic (DK DRV2605L)
- [ ] S: timekeeping + boot_count
- [ ] L: snap detection with trace logging; tuning sessions on an iPhone and a Pixel

**PR 5: links**
- [ ] L: GATT service + L2CAP CoC + advertising states
- [ ] M: pairing flow + SAS LED sequence
- [ ] M: USB CDC transport + MCUmgr SMP
- [ ] S: MCUboot DFU check (`mcumgr image upload`)

**PR 6: host harness**
- [ ] M: `ITransport` (SimpleBLE + USB serial) and the BlueZ L2CAP fallback
- [ ] M: `ProtocolClient` + resume + ack
- [ ] M: `RecordingReader` + `OggOpusWriter` + test-vector tests
- [ ] M: `micnote-cli` commands

**PR 7: measurements**
- [ ] M: PPK2 profiles for every power state; throughput measurements to Linux, macOS and an iPhone (via a minimal iOS test app or the nRF Connect app); results recorded in `docs/measurements/stage1.md`

## 6. Testing and verification
- `west twister -T firmware/tests -p native_sim`: framing, recstore (including simulated power loss mid-chunk), policy, crypto, test vector generation.
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
