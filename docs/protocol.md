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

1. The host opens the transport and sends `Hello{proto_version, host_id, host_nonce}`. `host_id` is the first 8 bytes of `SHA-256(host_pub)`; `host_nonce` is 16 random bytes, fresh per link.
2. The puck answers `HelloAck{device_info, device_nonce, device_proof}` (`device_nonce`: 16 random bytes, fresh per link) if `host_id` is paired, or if the puck is in PAIRING. Otherwise it answers `Error{UNPAIRED}`.
3. A paired host checks `device_proof` and sends `Auth{host_proof}`; the puck answers `Ok`. A host that is pairing sends `PairBegin` instead (§8).
4. The host sends `SetTime{unix_ms, tz_offset_min}` (§4).

**Host authentication** (decided 2026-11-22, [threat model §6.1](threat-model.md#6-open-issues)). `host_id` alone doesn't authenticate a host, so the session is a challenge-response keyed by the **link key** both sides derived at pairing (§8):

- `device_proof = HMAC-SHA256(link_key, "puck" ‖ host_nonce ‖ device_nonce)`: the host knows it reached the puck it paired with. Empty when the host isn't paired (the puck is in PAIRING and doesn't know it).
- `host_proof = HMAC-SHA256(link_key, "host" ‖ host_nonce ‖ device_nonce)`: the puck knows the host holds the key. Both nonces are fresh per link, so a captured session can't be replayed.
- The labels are the 4 ASCII bytes, no terminator.

**Session states**, per link:

| State | Entered by | Answered | Everything else |
|---|---|---|---|
| none | link start; a wrong `host_proof` | `Hello` | `BAD_STATE` |
| hello | `HelloAck` | `Hello`, `GetStatus`, `Auth` (paired host), `PairBegin` (PAIRING) | `BAD_STATE` |
| pairing | `PairChallenge` | `PairConfirm`; while it waits for the button, every request gets `BUSY` | `BAD_STATE` |
| session | `Auth` → `Ok`, or `PairDone` | everything except `PairConfirm` | — |

A wrong `host_proof` gets `Error{UNPAIRED}` and drops the link back to "none". A `Hello` in any state starts over.

## 4. Control messages

Sizes come from `micnote.options`. Strings are UTF-8, and their maximum counts the NUL terminator nanopb stores (`max_size: 32` holds 31 bytes).

| Request | Response(s) | Valid in | Errors |
|---|---|---|---|
| `Hello{proto_version, host_id[8], host_nonce[16]}` | `HelloAck{device_info, device_nonce[16], device_proof[0 or 32]}` | any | `UNPAIRED`, `PROTO_VERSION` |
| `Auth{host_proof[32]}` | `Ok{}` | hello, paired host | `UNPAIRED` (wrong proof), `BAD_STATE` |
| `GetStatus{}` | `DeviceStatus{…}` | hello, session | — |
| `SetTime{unix_ms, tz_offset_min}` | `Ok{}` | session | `BAD_REQUEST` (`unix_ms` = 0) |
| `ListRecordings{after_id}` | `RecordingList{entries[≤16], more}` | session | `STORAGE` |
| `Fetch{recording_id, from_seq}` | `Chunk{recording_id, seq, payload[≤4112]}` × n, then `FetchDone{recording_id, last_seq, header[≤512]}` | session | `NOT_FOUND`, `STORAGE` |
| `Ack{recording_id, sha256[32]}` | `Ok{}` | session | `NOT_FOUND`, `HASH_MISMATCH`, `BAD_STATE` (still recording) |
| `PairBegin{host_pub[32], host_name}` | `PairChallenge{device_pub[32], device_nonce[16]}` | hello, puck in PAIRING | `BAD_STATE`, `HOSTS_FULL`, `BAD_REQUEST` (`host_pub` doesn't match `Hello.host_id`, or is a low-order point) |
| `PairConfirm{user_confirmed}` | `PairDone{host_slot}` after the button press, or `Ok{}` if `user_confirmed` is false (cancelled) | pairing | `BAD_STATE` (the PAIRING window closed first) |
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

**`RecordingInfo`:** `id` (`boot_count << 32 | per-boot counter`), `start_boot`, `start_uptime_ms`, `start_unix_ms` (0 if the RTC was invalid), `duration_ms`, `size_bytes`, `last_seq`, `marker_count`, `complete` (false while still recording). **`duration_ms` and `marker_count` are always 0 from v1 firmware:** they're in the recording's encrypted trailer, which the puck can't read once it closes. The host learns them by decrypting.

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

The puck has no screen, so the user compares a short colour sequence instead of a number ([stage 1 §4.6](stages/01-firmware-and-sync.md#46-pairing-no-display-on-the-puck)). The user opens a 60 s PAIRING window with a 5 s button hold; then:

1. `Hello` from the new host (unknown `host_id`) gets `HelloAck` with an empty `device_proof`.
2. `PairBegin{host_pub, host_name}`. The puck checks `SHA-256(host_pub)[:8] == Hello.host_id`, makes a **fresh X25519 key pair** `d` for this pairing, and answers `PairChallenge{device_pub = d_pub, device_nonce = n_d}` with a new 16-byte `n_d`. A fifth host gets `HOSTS_FULL`; a host that is already paired is paired again into its own slot.
3. Both sides compute, with `n_h` the `Hello.host_nonce` of this link:
   - `ss = X25519(d, host_pub)` (the host: `X25519(host_priv, d_pub)`); an all-zero result (low-order key) is refused.
   - `link_key = HKDF-SHA256(ikm = ss, salt = host_id ‖ device_id, info = "micnote/v1/link", L = 32)`, with `device_id` from `DeviceInfo` (8 bytes).
   - `sas = HKDF-SHA256(ikm = ss, salt = n_h ‖ n_d, info = "micnote/v1/sas" ‖ host_pub ‖ d_pub, L = 4)`, and symbol `i` = `sas[i] mod 6`, an index into the colours red, green, blue, yellow, cyan, magenta (0–5). That's 6⁴ = 1296 sequences, about 10.3 bits; `mod 6` on a byte favours 0–3 slightly (43 vs 42 of 256), which is acceptable at this size.
4. The puck plays the 4 colours on a loop; the app shows the same 4. Over USB the cable is the trust anchor: the CLI prints the colours but doesn't ask for a comparison.
5. `PairConfirm{user_confirmed}`. `false` cancels (`Ok`). `true` waits for a short press of the puck's button, then the puck stores `{host_id, host_pub, host_name, link_key}` in a slot, moves every recording still wrapped to its fallback key over to the new host (§7, `mn_recstore_rewrap_all`), destroys `d`, and answers `PairDone{host_slot}`. The link is then a session; there's no `Auth` on this link.
6. If the PAIRING window closes first (60 s), the pending `PairConfirm` gets `Error{BAD_STATE}` and `d` is destroyed.

The host stores `link_key` per puck (`micnote-cli`: `~/.config/micnote/pucks/<device_id>.link`). `Unpair{host_id}` frees the slot and destroys its link key; future recordings aren't wrapped to that host, past ones still are.

**Test vectors:** `firmware/tests/pairing` and `core/tests/pairing_test.cpp` check the link key, SAS and proofs against the same known answers, computed independently with pyca/cryptography.

## 9. BLE transport

**(stage 1, PR 5)** — see [stage 1 §4.7](stages/01-firmware-and-sync.md#47-ble-details) for the service, UUIDs, advertising and connection parameters. The framed byte stream of §2 runs over:

- **L2CAP CoC** (preferred): a dynamic PSM, read from the `psm` characteristic (u16 LE). The puck receives SDUs of up to one PDU and sends SDUs of up to the host's MTU (2048 at most), so the host should offer an MTU of 2048 and a generous credit window; requests and responses can split anywhere across SDUs, as on any byte stream.
- **GATT fallback**, when the host can't open an L2CAP channel: the host subscribes to `data` notifications and writes the request stream to `ctrl` (write without response is fine), in pieces of up to ATT MTU − 3; the puck sends every response as `data` notifications.

The first transport to carry bytes on a connection owns it; bytes on the other are ignored. Everything needs an encrypted, bonded link (LE Secure Connections only); the app-layer session of §3 runs on top of that.

## 10. USB transport

**(stage 1, PR 5)** — see [stage 1 §4.8](stages/01-firmware-and-sync.md#48-usb). The framed byte stream of §2 runs over the first CDC ACM interface; SMP uses a second.

## 11. Test vectors

[`proto/testvectors/`](../proto/testvectors/README.md): recordings written by the firmware's real recording store from fixed keys and fixed "randomness" (`firmware/tests/testvectors`, extracted by `proto/testvectors.py`), plus `vectors.json` with the keys and the values a host must get back. The core's `core/tests/recording_test.cpp` decrypts every one of them with its own implementation of §6–7. CI regenerates them and fails on any drift.
