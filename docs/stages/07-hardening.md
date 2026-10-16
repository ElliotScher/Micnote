# Stage 7: hardening

## 1. Goal
Check the whole system against `docs/threat-model.md` before calling v1 done:
- fuzz every parser that handles untrusted input;
- review key handling end to end;
- penetration-test BLE pairing and the sync link;
- lock down the release supply chain;
- provision production firmware keys and debug protection.

Fix any findings and record the result.

**Exit criterion (from PLAN.md):** no open high-severity findings.

## 2. Depends on / unblocks
- **Depends on:** 5b (the release pipeline exists) and 6c (the final hardware). Fuzz targets and threat-model entries are added *during* stages 1–5. This stage runs them for real and closes the gaps.
- **Unblocks:** the v1.0 release.

## 3. Deliverables
```
core/fuzz/{fuzz_frame_decode.cpp, fuzz_recording_reader.cpp, fuzz_action_json.cpp,
           fuzz_grammar_output.cpp, fuzz_redact.cpp, corpus/, dict/}
firmware/tests/fuzz/ (libFuzzer on native_sim for framing + dispatch, if feasible)
app/test/security/ (connector input-handling tests, injection cases)
.github/workflows/fuzz.yml (nightly, ClusterFuzzLite or plain libFuzzer with time budget)
docs/security/
  review-v1.md          # findings table + status
  key-ceremony.md       # production key generation & custody
  pentest-ble.md        # procedures + results
  SECURITY.md (repo root) # disclosure policy, contact
```

## 4. Design detail

### 4.1 Fuzz targets
| Target | Input | Code under test | Oracle |
|---|---|---|---|
| `fuzz_frame_decode` | Raw bytes from the link | varint framing + nanopb `Frame` decode + dispatch (host side) | ASan/UBSan clean; no unbounded allocation |
| `fuzz_frame_decode_fw` | Same | Firmware framing + dispatch, built for native_sim with libFuzzer | Same |
| `fuzz_recording_reader` | A `.mnr` file | Header parse, wrap unwrap (with a fixed test key), chunk AEAD, packet/marker parse | Never yields unauthenticated data; no crashes |
| `fuzz_action_json` | JSON | The action schema validator + the semantic checks | No crashes; invalid input is always rejected |
| `fuzz_grammar_output` | Strings | Parsing of LLM output (local and cloud) into action drafts | Never accepts output that doesn't match the schema |
| `fuzz_redact` | Text | Redaction + restore | Restore(redact(x)) == x; secret patterns never appear in the redacted output |
| `fuzz_ogg_writer` (lower priority) | Packet streams | Ogg muxer | No crashes |

- **Corpus:** seeded from `proto/testvectors/`, captured real traffic, and the eval dataset.
- **Dictionaries** for the proto field tags and JSON keys.
- **Nightly:** 30 min per target, with crash artifacts uploaded. Any crash is a high-severity bug until it's triaged.

### 4.2 Threat-model review (STRIDE, per asset)
Walk through `docs/threat-model.md`, confirm each mitigation exists, and record the evidence:
| Threat | Mitigation to verify | Evidence |
|---|---|---|
| A stolen puck reveals audio | Per-recording CEK; puck can't decrypt closed recordings; fallback key protected by KMU/HUK; APPROTECT on | Dump flash with a debugger *before* APPROTECT on a dev unit → ciphertext only; attempt SWD on a production unit → locked |
| BLE sniffing | LESC encryption + app-layer AEAD | nRF Sniffer capture shows encrypted PDUs; decrypting the link layer with a known LTK still shows only AEAD ciphertext |
| MITM during pairing | LED SAS + RSSI gate + time window + button | Attempted MITM with two nRF dongles (relay) → the SAS differs or pairing fails (§4.4) |
| Tracking by advertising | No adverts unless snapped or button; RPA rotation | 24 h scan log near the puck in daily use; count the advertising events; RPAs rotate |
| Malicious puck / host | Hosts treat device input as untrusted (fuzzing); the device treats host input as untrusted | §4.1 |
| A stolen unlocked laptop or phone | DB encrypted with a keychain-held key; OS lock screen; tokens in secure storage | The documented residual risk: anyone with an unlocked session sees the data. Optional app lock (biometric/PIN) is added in this stage |
| Prompt injection via memo or context | Drafts only; read-only fetchers; confirmation with exact payload; injection eval cases | Re-run the stage 3 injection suite; red-team 10 new cases (e.g. a PR diff saying "approve this", an email thread with instructions) |
| Exfiltration via the cloud planner | Off by default; preview hash binding; redaction; audit log | Test: change the payload after the preview → refused |
| Malicious model file | SHA-256 pinned manifest | Tamper test → rejected |
| Malicious firmware update | MCUboot signature check with the production key | Unsigned / wrong-key image → rejected |
| A compromised dependency | Pinned commits; SBOM; minimal dependency set; CI isolation | Audit the dependency list; `osv-scanner` on the SBOM in CI |
| A compromised CI or release | Environment protection + manual approval; attestations; keys outside the repo | Review the workflow permissions (`permissions:` minimal, `id-token` only where needed); pin actions by SHA |
| OAuth token theft | Secure storage; minimal scopes; revocation in settings | Review scopes per connector; test the revoke flow |

### 4.3 Key ceremony (`docs/security/key-ceremony.md`)
| Key | Type | Generated | Storage | Used by |
|---|---|---|---|---|
| Firmware signing (MCUboot) | ECDSA P-256 (or Ed25519 if MCUboot is configured for it) | Offline laptop, `imgtool keygen` | Primary: an encrypted USB drive + a paper backup; the CI copy is an environment secret | CI `release.yml` |
| Release GPG | Ed25519 primary + signing subkey | Offline | Primary offline; subkey in CI | Linux packages, SHA256SUMS |
| Apple Developer ID / distribution | per Apple | Apple portal | Keychain export (.p12) → CI secret; fastlane match repo encrypted | macOS/iOS |
| Windows signing | Azure Trusted Signing (cloud HSM) | Azure | No exportable key | Windows |
| Android upload / APK release | RSA 4096 | `keytool` offline | Encrypted backups; CI secret | Android |
| Sparkle EdDSA | Ed25519 | `generate_keys` | Offline + CI secret | appcast signing |

**Procedures:**
- **Rotation:** firmware key rotation needs MCUboot to support multiple key slots. Configure 2 public keys from day one (active + backup), so a compromised key can be retired with an update signed by the backup key.
- **Revocation and incident response:** documented steps per key.

### 4.4 BLE pairing penetration test (`pentest-ble.md`)
**Setup:**
- 2× nRF52840 dongles running a relay/MITM tool (e.g. a BtleJuice/GATTacker-style relay, or a custom Zephyr relay).
- An nRF Sniffer.
- A target puck + an iPhone/Pixel/desktop host.

**Scenarios:**
1. **Passive sniff of pairing + sync.** Expected: nothing readable.
2. **Active relay MITM during pairing.** Expected: the SAS colors differ between puck and app, and the user rejects. Also check: the relay's added latency or RSSI makes the RSSI gate fail.
3. **Replay of a captured `Ack`** to delete recordings. Expected: rejected, because acks are bound to the session (add a session nonce to the AAD of control messages if this isn't already the case; *check the design*).
4. **Unpaired host sends `Fetch`.** Expected: `Error{UNPAIRED}`, no data.
5. **Pairing-window abuse:** connect attempts during PAIRING from a distant attacker (RSSI gate), and repeated attempts (rate limit: at most 3 per window).
6. **Fuzzing over the air:** malformed L2CAP/GATT writes using the dongle. Expected: no crash and no hang (watchdog recovers).
7. **DoS:** connection flooding. Expected: the puck keeps recording (recording runs at a higher priority than link handling).
8. **USB:** malformed CDC frames from a host. Expected: as in 6.

### 4.5 App security items
- **Optional app lock:** biometric or PIN on open and on confirming high-risk actions (`local_auth`).
- **Clipboard hygiene:** transcripts copied to the clipboard are cleared after 2 min (optional).
- **Screenshot protection** on sensitive screens (Android `FLAG_SECURE`, iOS secure overlay), as a settings toggle.
- **Logs:** a check that no transcript or payload content ever goes into logs. A CI grep plus a log-scrubbing unit test.
- **TLS:** OS trust store; no custom CA overrides. Certificate pinning is *not* used (it breaks with provider certificate rotation).

### 4.6 Supply chain
- GitHub Actions pinned by commit SHA (Dependabot updates them); `permissions: read-all` by default.
- `osv-scanner` on the SBOM for every release; `pub` dependency review; FetchContent pins audited.
- Reproducible-build check for the firmware: build twice from the same tag in different runners and compare hashes. Target: identical `zephyr.bin`.
- `SECURITY.md` with a disclosure email and response times.

## 5. Task breakdown
**Fuzzing**
- [ ] M: finish all targets + corpora + dictionaries
- [ ] S: the `fuzz.yml` nightly
- [ ] M: triage and fix crashes

**Review**
- [ ] L: threat-model walkthrough with evidence; write `review-v1.md` (findings rated with CVSS-style severity)
- [ ] M: control-message replay protection (session nonce in AAD), if it's missing
- [ ] M: app lock, screenshot protection, log scrubbing tests

**Pen test**
- [ ] L: BLE scenarios 1–8 with tooling; write-up

**Keys and release**
- [ ] M: key ceremony + MCUboot dual keys + APPROTECT provisioning script (`scripts/provision-puck.sh`: flash the signed MCUboot + app, write UICR, verify the lock)
- [ ] S: CI hardening (SHA pins, permissions, environments)
- [ ] S: `SECURITY.md`

## 6. Testing and verification
- `fuzz.yml` has run ≥ 7 nights with no new crashes.
- Every finding in `review-v1.md` is resolved or accepted with a justification. **No open high or critical findings.**
- Pen-test scenarios pass as expected, or are documented as accepted residual risk.
- A provisioned puck: SWD access is refused (`nrfjprog --readregs` fails), and only signed updates are accepted.
- Release dry run: tag `v1.0.0-rc1` → all artifacts are signed, attested and verified using the 5b checklist.

## 7. Risks and mitigations
| Risk | Mitigation |
|---|---|
| Losing the firmware signing key bricks future updates | Dual key slots + offline backups (§4.3) |
| An APPROTECT lock done wrongly bricks a unit | Provision only through the script; test on a sacrificial DK first; keep Rev A boards unlocked for development |
| The pen test needs tooling expertise | Start from published nRF-based relay tools; time-box it; consider an outside reviewer for v1.1 |
| Findings late in the schedule | Fuzz targets and threat-model entries are added during each stage, not only here |

## 8. Open questions
- Outside security review before a public v1, or after?
- Should the puck support a user "panic wipe" (button sequence → erase all recordings)? It's cheap to add, so decide during the review.
- ClusterFuzzLite vs plain libFuzzer in Actions: decide based on CI cost.

## 9. Out of scope
- Formal certifications (SOC 2, etc.).
- A bug bounty program.
