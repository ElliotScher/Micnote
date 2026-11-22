# Test vectors

Recordings written by the firmware's real recording store, and the values a host must get back from them. The firmware writes them (`firmware/tests/testvectors`); the core's tests decrypt them. If both pass, the puck and the host agree on the `.mnr` format and the cryptography ([`docs/protocol.md`](../../docs/protocol.md) §6–7).

**Generated, never edited by hand.** Regenerate after changing the format, the crypto or the generator, and commit the result:

```sh
python3 proto/testvectors.py          # runs the generator with twister (firmware environment)
```

CI's firmware workflow fails if the committed files differ from a fresh generation.

| File | What it is |
|---|---|
| `rec_two_hosts.mnr` | 250 packets (5 s) with item markers at 1 s and 3 s, wrapped to `host_a` and `host_b`, closed normally |
| `rec_fallback.mnr` | 100 packets, wrapped to `device_fallback` only, as before any pairing |
| `rec_power_cut.mnr` | 300 packets written to `host_a`, then a power cut and recovery: the unsynced tail is gone and there is no trailer |
| `vectors.json` | the keys (private keys included: they're test keys), and per recording its size, chunk and packet counts, marker offsets, trailer, duration, `packet_stream_sha256` and `file_sha256` |

- **Keys:** `host_a` is RFC 7748 §6.1's "Bob"; the others are fixed byte patterns. All the generator's "randomness" (CEKs, ephemeral keys) is fixed too, so regenerating gives identical bytes.
- **`packet_stream_sha256`** is SHA-256 over every packet as `len u16 LE | bytes`, in order, after decryption.
- **`file_sha256`** is the hash a host sends in `Ack`: the header and every stored chunk record, i.e. the whole file.
- **Packets are synthetic** (a byte pattern of 30–49 bytes each), not real Opus, until the encoder lands in stage 1 PR 2. These vectors pin the container and the crypto, not the codec.
