# Device ↔ host protocol

How the puck and a host (phone, tablet or desktop) talk: message framing, the control messages, the recording file format, the cryptography, pairing, and the BLE and USB transports. Firmware updates are not part of it: they use MCUmgr's standard SMP service.

**This page is the spec.** If the code and this page disagree, one of them is a bug. The schema is [`proto/micnote.proto`](../proto/micnote.proto) (package `micnote.v1`) with sizes in [`proto/micnote.options`](../proto/micnote.options). nanopb generates C for both sides into `proto/gen/` (committed; `proto/regen.py` regenerates it, and CI checks for drift).

Sections marked **(stage 1, PR n)** describe design that the named PR implements; until then the design in [stage 1 §4](stages/01-firmware-and-sync.md#4-design-detail) is the reference, and that PR moves it here.

## 1. Versioning

- `proto_version` is **1**. The host sends it in `Hello`; the puck reports its own in `HelloAck.device_info`.
- Within a version, changes are additive only: new fields, new messages, new enum values. Both sides ignore unknown fields (protobuf's default) and treat an unknown enum value as `*_UNSPECIFIED`.
- A puck that doesn't speak the host's version answers `Error{PROTO_VERSION}` and closes the link. The host then offers a firmware update (SMP, which doesn't depend on this protocol).
- Field numbers are never reused. A removed field's number is marked `reserved`.

## 2. Framing

Every message on every transport is:

```
varint length  |  Frame (length bytes, protobuf-encoded)
```

- **Varint:** protobuf's base-128 encoding, low 7 bits first, high bit set on every byte except the last.
- **Maximum frame:** `micnote_v1_Frame_size` = **4142** bytes (a `Chunk` with a full payload), so the prefix is at most **2 bytes**. A receiver rejects a prefix longer than 2 bytes, or a length above 4142, *before* reading the body.
- **A byte stream:** frames are not aligned to transport packets. A frame may span several L2CAP SDUs, GATT writes or USB transfers, and one packet may carry several frames. Receivers reassemble incrementally.
- **Errors are fatal to the link.** A bad prefix, an oversized length, or a body that isn't a valid `Frame` means the stream has lost sync: the receiver drops the link (the host reconnects). It does not try to resynchronize.
- A zero-length frame is a valid encoding of an empty `Frame`. The puck answers a `Frame` with no body with `Error{BAD_REQUEST}`.

Implementations: `core/src/protocol/framing.{hpp,cpp}` (host) and `firmware/app/src/proto_link/framing.{c,h}` (puck).

### `Frame`

```proto
message Frame { uint32 req_id = 1; oneof body { ... } }
```

- **`req_id`** is chosen by the host, non-zero, and unique among its outstanding requests. The puck copies it into every response to that request, including every `Chunk` of a `Fetch`.
- **`req_id = 0`** marks an unsolicited message from the puck (today only `DeviceStatus` on a state change). The puck never sends a request.
- **Body field numbers:** requests (host → puck) use 2–31, responses and events (puck → host) use 32 and up.
- **One request at a time.** In v1 the host waits for a request's final response before sending the next. A puck that receives a request while another is in progress answers `Error{BUSY}`.

## 3. Session

**(stage 1, PR 5)**

1. The host opens the transport and sends `Hello{proto_version, host_id, host_nonce}`. `host_id` is the first 8 bytes of `SHA-256(host_pub)`; `host_nonce` is 16 random bytes.
2. The puck answers `HelloAck{device_info, device_nonce}` (16 random bytes) if `host_id` is paired, or if the puck is in PAIRING. Otherwise it answers `Error{UNPAIRED}`.
3. The host sends `SetTime{unix_ms, tz_offset_min}` (§6).
4. Any request other than `Hello` before a successful `HelloAck` gets `Error{BAD_STATE}`.

> **Open, decide before PR 5 ([threat model §6.1](threat-model.md#6-open-issues)):** `host_id` alone doesn't authenticate the host. The likely fix is a challenge-response over both nonces, keyed from the pairing exchange, before the puck accepts any state-changing request (`Ack`, `Unpair`, `SetTime`). It adds fields to `Hello`/`HelloAck` (or one new request), which §1 allows.

## 4. Control messages

Sizes come from `micnote.options`. Strings are UTF-8, and their maximum counts the NUL terminator nanopb stores (`max_size: 32` holds 31 bytes).

| Request | Response(s) | Valid in | Errors |
|---|---|---|---|
| `Hello{proto_version, host_id[8], host_nonce[16]}` | `HelloAck{device_info, device_nonce[16]}` | any | `UNPAIRED`, `PROTO_VERSION` |
| `GetStatus{}` | `DeviceStatus{…}` | session | — |
| `SetTime{unix_ms, tz_offset_min}` | `Ok{}` | session | `BAD_REQUEST` (`unix_ms` = 0) |
| `ListRecordings{after_id}` | `RecordingList{entries[≤16], more}` | session | `STORAGE` |
| `Fetch{recording_id, from_seq}` | `Chunk{recording_id, seq, payload[≤4112]}` × n, then `FetchDone{recording_id, last_seq, header[≤512]}` | session | `NOT_FOUND`, `STORAGE` |
| `Ack{recording_id, sha256[32]}` | `Ok{}` | session | `NOT_FOUND`, `HASH_MISMATCH`, `BAD_STATE` (still recording) |
| `PairBegin{host_pub[32], host_name}` | `PairChallenge{device_pub[32], device_nonce[16]}` | PAIRING | `BAD_STATE`, `HOSTS_FULL` |
| `PairConfirm{user_confirmed}` | `PairDone{host_slot}` | PAIRING, after `PairBegin` | `BAD_STATE` |
| `ListHosts{}` | `HostList{hosts[≤4]}` | session | — |
| `Unpair{host_id[8]}` | `Ok{}` | session | `NOT_FOUND` |

Any request can also get `Error{BUSY}` or `Error{BAD_REQUEST}`.

**`Error{code, detail}`:** `detail` (≤ 63 bytes) is for logs only; hosts never parse it.

| Code | Meaning |
|---|---|
| `UNPAIRED` | `Hello.host_id` isn't paired, and the puck isn't in PAIRING |
| `BUSY` | another request or link is using the puck; retry later |
| `NOT_FOUND` | no recording or host with that ID |
| `HASH_MISMATCH` | `Ack.sha256` doesn't match the stored recording; nothing was deleted |
| `STORAGE` | flash or filesystem error |
| `BAD_STATE` | the request isn't valid in the current state |
| `PROTO_VERSION` | the puck doesn't speak `Hello.proto_version` |
| `BAD_REQUEST` | the frame decoded, but the body is missing or a field is invalid |
| `HOSTS_FULL` | all 4 host slots are taken; unpair one first |

**`DeviceStatus`:** `battery_pct`, `battery_mv`, `charge_source` (`NONE`/`USB`/`QI`), `storage_used_kb`, `storage_total_kb`, `recording_count`, `fw_version`, `boot_count`, `uptime_ms`, `rtc_valid`, `unix_ms` (0 while `rtc_valid` is false), `state` (`SLEEP`, `IDLE`, `RECORDING`, `CONNECTED`, `PAIRING`, `DFU`). Sent in reply to `GetStatus`, and unsolicited (`req_id` 0) whenever `state`, `charge_source` or `recording_count` changes.

**`RecordingInfo`:** `id` (`boot_count << 32 | per-boot counter`), `start_boot`, `start_uptime_ms`, `start_unix_ms` (0 if the RTC was invalid), `duration_ms`, `size_bytes`, `last_seq`, `marker_count`, `complete` (false while still recording).

## 5. Sync

**(stage 1, PRs 5–6)**

```
host                                   puck
 │ ListRecordings{after_id=0}  ───────►│
 │◄─────── RecordingList{entries, more}│   repeat with after_id = last id while more
 │ Fetch{id, from_seq=0}       ───────►│
 │◄───────────── Chunk{id, seq=0, …}   │
 │◄───────────── Chunk{id, seq=1, …}   │   …
 │◄───────────── FetchDone{id, last_seq, header}
 │   verify every chunk's tag, decrypt, write .opus
 │ Ack{id, sha256}             ───────►│   puck deletes the recording on a match
 │◄───────────────────────────── Ok{}  │
```

- **Resume:** if the link drops mid-fetch, the host keeps the chunks it has and sends `Fetch{id, from_seq = next seq}` on the next link.
- **Recording in progress:** a `Fetch` of an incomplete recording streams the chunks written so far; its `FetchDone.last_seq` is the last one flushed. It can't be acked until `complete` is true. With no chunk flushed yet, `FetchDone` carries just the header and `last_seq` is meaningless.
- **Checks the host makes** (`core/src/protocol/client.cpp`): every `Chunk` of a fetch has the requested `recording_id`, the next `seq` in order and a payload of at least 17 bytes; `FetchDone.last_seq` matches the last chunk received; a `RecordingList` page with `more` set must end past `after_id`. Anything else is a protocol error and the host drops the link.
- **Unsolicited messages:** only `DeviceStatus` (with `req_id` 0). Any other frame the host didn't ask for is a protocol error.
- **Hash:** `Ack.sha256` covers the header followed by every chunk record as stored (`seq | ct_len | ciphertext | tag`, §6), so the host acks exactly what the puck holds.
- **Deletion rule:** the puck deletes a recording only on a matching `Ack` or a factory reset. With several paired hosts, the first host to ack it takes it.

## 6. Recording file format (`.mnr`)

**(stage 1, PR 3)** — layout in [stage 1 §4.4](stages/01-firmware-and-sync.md#44-recording-file-format-recid-hexmnr-on-littlefs), code in `firmware/app/src/storage/mnr.{h,c}`. All integers are little-endian.

| Part | Bytes | Contents |
|---|---|---|
| Core | 40 | magic `MNR1`, version 1, `hdr_len`, flags, `rec_id`, codec, rate, frame length, the three start timestamps. Never changes; every chunk authenticates it |
| Wrap table | 1 + 4 × 88 | `n_wraps`, then 4 slots of `{host_id, eph_pub, wrapped_cek, tag}`; unused slots are zero. Rewritten in place when the fallback wrap is replaced at first pairing |
| Chunk records | 6 + ct_len + 16 each | `seq u32 \| ct_len u16 (1..4096) \| ciphertext \| tag`. The last one carries the trailer |

The first chunk starts at `hdr_len` (393 today). Readers accept `hdr_len` up to 512 and skip anything after the wrap table, so a later v1 writer can append header fields.

Time: `SetTime` sets the puck's RTC. Every recording also stores `start_boot` and `start_uptime_ms`, so the host can rebuild `start_unix_ms` for recordings made while `rtc_valid` was false, from a later `SetTime` in the same boot ([stage 1 §4.10](stages/01-firmware-and-sync.md#410-timekeeping)).

## 7. Cryptography

**(stage 1, PR 3)** — see [stage 1 §4.5](stages/01-firmware-and-sync.md#45-cryptography-psa-crypto-api-on-both-sides-ncs-on-firmware-mbed-tlstf-psa-crypto-via-fetchcontent-on-host): a random CEK per recording, wrapped to each host with its own ephemeral X25519 key + HKDF-SHA256 (`"micnote/v1/kek"`) + AES-256-CCM; chunks sealed with AES-256-CCM under the CEK, nonce `rec_id‖seq‖0x00`, AAD `SHA-256(header core)‖seq`.

## 8. Pairing

**(stage 1, PR 5)** — see [stage 1 §4.6](stages/01-firmware-and-sync.md#46-pairing-no-display-on-the-puck): a 60 s PAIRING window, `PairBegin`/`PairChallenge` exchanging X25519 keys, a 4-symbol LED color SAS (`"micnote/v1/sas"`), an RSSI gate, USB pairing without the SAS, and 4 host slots (`HOSTS_FULL` for a fifth).

## 9. BLE transport

**(stage 1, PR 5)** — see [stage 1 §4.7](stages/01-firmware-and-sync.md#47-ble-details). The framed byte stream of §2 runs over an L2CAP CoC (dynamic PSM, read from the `psm` characteristic), or over the GATT `ctrl` (write/indicate) and `data` (notify) characteristics when L2CAP isn't available.

## 10. USB transport

**(stage 1, PR 5)** — see [stage 1 §4.8](stages/01-firmware-and-sync.md#48-usb). The framed byte stream of §2 runs over the first CDC ACM interface; SMP uses a second.

## 11. Test vectors

[`proto/testvectors/`](../proto/testvectors/README.md): recordings written by the firmware's real recording store from fixed keys and fixed "randomness" (`firmware/tests/testvectors`, extracted by `proto/testvectors.py`), plus `vectors.json` with the keys and the values a host must get back. The core's `core/tests/recording_test.cpp` decrypts every one of them with its own implementation of §6–7. CI regenerates them and fails on any drift.
