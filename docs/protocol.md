# Device ↔ host protocol

> **Stub.** Stage 1 fills this in and from then on it is the authoritative spec: if the code and this page disagree, one of them is a bug. Until then, the design lives in [stage 1 §4.3–4.8](stages/01-firmware-and-sync.md#43-protocol-protomicnoteproto-package-micnotev1).

How the puck and a host (phone, tablet or desktop) talk: message framing, the control messages, the recording file format, the cryptography, pairing, and the BLE and USB transports. Firmware updates are not part of it: they use MCUmgr's standard SMP service.

## Current state (stage 0b)

[`proto/micnote.proto`](../proto/micnote.proto), package `micnote.v1`, defines only:

| Message | Fields | Used by |
|---|---|---|
| `Version` | `major`, `minor`, `patch` | firmware, protocol and core versions |
| `DeviceInfo` | `fw_version`, `proto_version`, `device_id` (8 bytes, the nRF5340's FICR ID), `board` (≤ 32 chars) | `HelloAck`, from stage 1 |

nanopb generates C for both sides into `proto/gen/` (committed; `proto/regen.py` regenerates it, and CI checks for drift). [`proto/micnote.options`](../proto/micnote.options) gives every `bytes`, `string` and `repeated` field a fixed maximum, so the firmware never needs the heap.

## Sections to write in stage 1

1. **Versioning:** `proto_version`, compatibility rules, what a host does with a newer or older puck.
2. **Framing:** a varint length followed by a `Frame{req_id, oneof body}`; maximum frame size per transport.
3. **Session:** `Hello` / `HelloAck`, host authentication (see [threat model §6.1](threat-model.md#6-open-issues)), `SetTime`.
4. **Control messages:** each request and response, its fields and size limits, the states it's valid in, and its errors (`UNPAIRED`, `BUSY`, `NOT_FOUND`, `HASH_MISMATCH`, `STORAGE`, `BAD_STATE`, `PROTO_VERSION`).
5. **Sync:** list → fetch (with resume from `last_seq`) → verify → ack; when the puck deletes a recording.
6. **Recording file format (`.mnr`):** header, key wraps, chunk records, trailer, crash recovery.
7. **Cryptography:** per-recording CEK, X25519 wrapping, HKDF labels, AES-256-CCM nonces and AAD, the device fallback key, host IDs.
8. **Pairing:** the pairing window, the app-layer key exchange, the LED color SAS, the RSSI gate, USB pairing, host slots.
9. **BLE transport:** GATT service and characteristics, L2CAP CoC, connection parameters, advertising states.
10. **USB transport:** CDC ACM interfaces and enumeration.
11. **Test vectors:** `proto/testvectors/`, produced by the firmware tests and checked by the core tests.
