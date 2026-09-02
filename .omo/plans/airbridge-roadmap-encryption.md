# airbridge-roadmap-encryption - Work Plan

## TL;DR (For humans)

**What you'll get:** A staged AirBridge v2 roadmap that makes the demo faster, less fragile, harder to fingerprint, and encrypted end-to-end between the two browser pages. The Flipper remains a blind USB/BLE bridge; it never sees plaintext chat/file contents or keys.

**Why this approach:** First add measurement and failure visibility, then fix the exact deploy/BLE state bugs hit on hardware, then add compression and encryption, and only then tune BLE/protocol throughput. This avoids stacking timing, radio, and crypto changes into one un-debuggable batch.

**What it will NOT do:** It will not enable deploy streaming from Bridge mode, weaken the physical Deploy consent gate, or claim BLE stealth is perfect. Transfer resume is explicitly deferred; windowed ACKs are optional last.

**Effort:** XL
**Risk:** High - spans browser crypto, browser permissions, FAP state, BLE stack behavior, and hardware-only QA.
**Decisions to sanity-check:** Encryption is always-on; NACK is included now; transfer resume is deferred; windowed ACKs are last/optional; deploy code stays menu-gated.

Your next move: approve this plan for execution, or ask for a high-accuracy review first. Full execution detail follows below.

---

> TL;DR (machine): XL/high-risk architecture roadmap: measurement, deploy safety, compression, always-on E2E encryption, BLE tuning, stealth hardening, NACK, final hardware/security verification.

## Scope

### Must have
- Measurement harness for deploy/chat/file throughput, exact bytes, timing, errors, and regression evidence.
- Deploy hardening: browser timeout/error path; Flipper starts deploy stream only from menu-gated Waiting state after physical confirmation; explicit failure when the user/browser tries the wrong state.
- Bridge-screen BLE advertising/watchdog fix so the Web Bluetooth picker does not require app restart after long-running FAP state.
- Bundle compression for Deploy path: generated compressed app bundle, Flipper streams compressed bytes, browser inflates with `DecompressionStream("gzip")` when available.
- Always-on browser-to-browser encryption: ECDH P-256 public-key exchange, HKDF-SHA256 key derivation, AES-GCM-256 item encryption, 6-digit SAS code verification, lock indicator, mismatch abort.
- NACK implementation for recoverable missing/corrupt chunks after the crypto framing is stable.
- BLE radio tuning with measured runtime proof: MTU/DLE/2M PHY/connection interval evidence and throughput before/after.
- Stealth hardening: typing jitter, remove custom AirBridge serial UUID from advertising when feasible, per-device DIS serial, Windows USB tree compare where hardware is available.
- Regenerated firmware bundle and docs after firmware changes.

### Must NOT have (guardrails, anti-slop, scope boundaries)
- MUST NOT send keyboard reports in Bridge mode or any non-Deploy data path.
- MUST NOT make `0x42` start deploy streaming from `AirbridgeScreenBridge`; Bridge mode must remain pure relay.
- MUST NOT hide or bypass browser/Flipper physical consent gates.
- MUST NOT store plaintext, session keys, or decrypted files on the Flipper.
- MUST NOT weaken SHA-256 user-visible file integrity; after encryption, verify plaintext hash after decrypt and rely on AES-GCM for ciphertext authenticity.
- MUST NOT claim empirical throughput or stealth improvements without recorded evidence.
- MUST NOT make Windows `usbccgp` evidence a hard pass gate when no Windows host / real dongle capture is available.
- MUST NOT implement transfer resume in this plan; defer as a future plan.

### Protocol contract decisions

#### Crypto handshake v1 (must be documented before coding)
- Add protocol version constant `AIRBRIDGE_CRYPTO_V1 = 1` and reject peers that claim crypto support but cannot complete the v1 handshake.
- Add frame types after the existing `0x01-0x09` range: `KEY_OFFER = 0x0A`, `KEY_REPLY = 0x0B`, `KEY_CONFIRM = 0x0C`, `KEY_ABORT = 0x0D`.
- Public keys are WebCrypto P-256 ECDH raw uncompressed points: 65 bytes (`0x04 || X32 || Y32`). Because a frame payload is only 59 bytes, `KEY_OFFER` and `KEY_REPLY` are fragmented using normal outer frame `seq`:
  - fragment `seq=0` payload: `version:u8`, `role:u8` (`1=USB`, `2=BLE`), `total_len:u16be` (`65`), then up to 55 public-key bytes.
  - fragment `seq>0` payload: remaining public-key bytes, max 59 per frame, until `total_len` is satisfied.
  - receiver rejects duplicate role, unknown version, malformed raw P-256 point, wrong total length, or missing fragments after timeout.
- Handshake ACK/retry: each `KEY_OFFER`, `KEY_REPLY`, and `KEY_CONFIRM` fragment uses existing ACK format `[acked_type, acked_seq]`. Sender retransmits a handshake fragment after 1000 ms without matching ACK, up to 3 retries per fragment; after that it sends `KEY_ABORT` reason `3=timeout` and clears handshake state. Receiver accepts duplicate already-ACKed handshake fragments idempotently and re-sends the ACK; conflicting duplicate payload for the same `(type, seq)` is terminal `KEY_ABORT` reason `4=conflict`.
- Handshake ordering/state: USB endpoint sends `KEY_OFFER` after transport connect; BLE endpoint sends `KEY_REPLY` only after validating full `KEY_OFFER`; both endpoints derive the transcript and enter `SasPending` after validating the peer public key. `KEY_CONFIRM` is the post-SAS mutual unlock signal, not a pre-SAS handshake marker: an endpoint sends `KEY_CONFIRM` only after its local user accepts the displayed matching SAS. `KEY_CONFIRM` is valid only in `SasPending`, must carry the current `transcript_hash16`, and is ACKed with the existing ACK format. Each endpoint enters `Unlocked` only after both conditions are true: local SAS accepted and peer `KEY_CONFIRM` for the same transcript received/validated. If one side accepts SAS earlier, it remains unable to send items until peer confirmation arrives. Any `ITEM_META`, `ITEM_DATA`, or `ITEM_DONE` sent or received before mutual `Unlocked` is terminal protocol error and clears keys/state.
- Transcript binding: `transcript = "PocketAirBridge-crypto-v1" || usbRoleByte || usbPubRaw || bleRoleByte || blePubRaw`. The USB endpoint is always role `1`; BLE endpoint is always role `2`, independent of who initiates.
- Key derivation: derive ECDH shared bits with `crypto.subtle.deriveBits({name:"ECDH", public: remotePub}, localPriv, 256)`, import as HKDF key material, then HKDF-SHA256 with `salt = SHA-256("PocketAirBridge salt v1" || transcript)`. Derive two non-extractable AES-GCM-256 keys with distinct `info` strings: `PocketAirBridge v1 USB->BLE item key` and `PocketAirBridge v1 BLE->USB item key`. Derive `keyId` as the first 16 bytes of `HKDF-SHA256(info="PocketAirBridge v1 key id")`, base64url encoded without padding in JSON metadata; receiver rejects any encrypted item whose `keyId` does not match the active session.
- IV/nonce uniqueness: for each direction, derive a 4-byte nonce prefix with HKDF info `PocketAirBridge v1 <direction> nonce prefix`; IV is `prefix32 || item_counter:u64be`. The 64-bit counter starts at zero per handshake/direction, increments once per encrypted item, and must abort on wrap or rollback.
- Direction wire mapping: JSON metadata `direction` is exactly one of `"usb-to-ble"` or `"ble-to-usb"`. Canonical AAD byte `direction:u8` is `1` for `"usb-to-ble"` and `2` for `"ble-to-usb"`. `"usb-to-ble"` selects the AES key derived with info `PocketAirBridge v1 USB->BLE item key` and nonce prefix info `PocketAirBridge v1 usb-to-ble nonce prefix`; `"ble-to-usb"` selects the AES key derived with info `PocketAirBridge v1 BLE->USB item key` and nonce prefix info `PocketAirBridge v1 ble-to-usb nonce prefix`. Receiver rejects any metadata whose JSON direction, AAD direction byte, sender transport role, selected key, or IV prefix do not match this table.
- SAS: compute `sasInt = first 20 bits of SHA-256("PocketAirBridge SAS v1" || transcript)`, then render `(sasInt % 1000000)` as exactly six decimal digits with leading zeros on both browser pages. Sending is blocked until both pages locally mark SAS accepted. Mismatch sends `KEY_ABORT`, clears keys, and returns both pages to locked state.
- `KEY_CONFIRM` payload: `version:u8`, `status:u8` (`1=SAS accepted`; value `2` is reserved and MUST NOT unlock), `transcript_hash16` (first 16 bytes of SHA-256 transcript). `KEY_ABORT` payload: `version:u8`, `reason:u8`, optional UTF-8 reason truncated to remaining payload.
- Downgrade prevention: immediately after transport connect the protocol state is `CryptoRequired`; no `ITEM_META`, `ITEM_DATA`, or `ITEM_DONE` may be sent or accepted until state is `Unlocked`. Plaintext item frames before unlock are terminal protocol errors. A peer that does not complete crypto v1 fails closed. There is no “unencrypted compatibility mode” in this plan.

#### Encrypted item envelope v1
- Encryption wraps the whole user item before chunking. The Flipper and outer protocol carry ciphertext only.
- Inner plaintext envelope before AES-GCM: `magic="AB1"`, `itemId:u32be`, `kind:u8` (`1=text`, `2=attachment`), `nameLen:u16be`, `mimeLen:u16be`, `plainSize:u64be`, `plainSha256:32`, UTF-8 name bytes, UTF-8 MIME bytes, then raw item bytes.
- Outer `ITEM_META` uses `kind:"encrypted"` and contains only transport metadata: `cryptoVersion`, `alg:"AES-GCM-256"`, `keyId`, `direction`, `iv` (base64), `encryptedSize`, `encryptedSha256`, and original `itemId`. `encryptedSize` and `encryptedSha256` refer to the exact WebCrypto AES-GCM output bytes (`ciphertext || 16-byte tag`) after encryption and before chunking. User-visible name/MIME/plaintext size move inside the encrypted envelope.
- AES-GCM AAD is canonical bytes: `"AB1-AAD" || cryptoVersion:u8 || itemId:u32be || direction:u8 || iv12 || encryptedSize:u64be`. Receiver recomputes AAD exactly; auth failure sends `ERROR` and clears the current item.
- AES-GCM tag placement: use WebCrypto AES-GCM output as `ciphertext || 16-byte tag`; chunk that output unchanged through existing `ITEM_DATA`.
- Integrity semantics: outer SHA-256 verifies ciphertext reassembly only; AES-GCM authenticates ciphertext + AAD; inner plaintext SHA-256 is verified after decrypt for user-visible file/text integrity.
- Replay handling: reject duplicate `(keyId, direction, itemCounter)` within a session; clear replay cache on new handshake.

#### NACK v1
- NACK payload format mirrors ACK and adds a reason: `nacked_type:u8`, `nacked_seq:u16be`, `reason:u8` (`1=missing`, `2=malformed`, `3=auth-failed-retryable`, `4=busy-window`).
- NACK references the outer protocol frame sequence/type (`ITEM_META`, `ITEM_DATA`, or `ITEM_DONE`), not plaintext offsets. It never requests “resume from byte offset”.
- Receiver sends NACK for the next expected outer frame if it observes a sequence gap or malformed recoverable frame and no correct frame arrives within 750 ms. Sender treats 2000 ms without ACK/NACK for an in-flight data frame as timeout and retransmits the exact frame.
- Sender retains the current item’s sent frames until `ITEM_DONE` ACK or terminal `ERROR`; on NACK it retransmits only the exact referenced frame if retry budget remains. Retry budget is 3 retransmissions per `(frame_type, seq)` per item; counters reset for a new item, never mid-item.
- Receiver ACKs idempotent duplicate frames already accepted; NACKs only recoverable missing/malformed frames. AES-GCM auth failure after full ciphertext decrypt is terminal `ERROR`, not infinite NACK, unless the missing/corrupt outer chunk is identifiable before decrypt.
- Retry exhaustion becomes `ERROR`; transfer resume remains out of scope.

## Verification strategy

- Test decision: tests-after for UI/firmware changes, with protocol harness expansion before risky protocol/crypto changes where possible.
- Evidence path: `.omo/evidence/airbridge-roadmap-encryption/task-<N>/` for screenshots, logs, harness output, timing JSON, hardware notes, and Flipper counter photos/notes.
- Hardware gates: use the `question` tool plus audible alert for physical actions. Verification is agent-observed after each gate: page state, console logs, transferred hashes, USB/BLE enumeration, and Flipper counters.
- Browser surface: persistent Chrome with CDP `http://localhost:9222`; never use ephemeral Playwright for gated flows.
- Required regression set after every firmware/FAP/web change: protocol harness 17/17 plus new tests, USB WebHID connect, BLE Web Bluetooth connect, text both directions, attachment SHA-256, cancel path, USB Deploy, Flipper `DROP 0` and `TXERR 0`.
- Exact browser setup command: `mkdir -p ~/.cache/airbridge-pw-chrome && open -na "Google Chrome" --args --remote-debugging-port=9222 --user-data-dir=$HOME/.cache/airbridge-pw-chrome "https://blank.org"`; verify with `curl -s http://localhost:9222/json/version`.
- Exact local server command for web QA: `python3 -m http.server 8000` from `/Users/asutov/projects/flipper-hid`, then use `http://localhost:8000/web/protocol-harness.html`, `chat-usb.html`, and `chat-ble.html`.
- Exact Playwright MCP convention: every browser call passes `cdp_url="http://localhost:9222"`; before user browser gates, verify `/json/list` has the intended visible tab URL and activate it with `/json/activate/<target-id>` if needed.
- Exact firmware verification commands: full firmware `./fbt`; FAP `./fbt build APPSRC=applications_user/pocket_airbridge`; deploy via `scripts/storage.py send -f ... /ext/apps/USB/pocket_airbridge.fap`; size via `scripts/storage.py size /ext/apps/USB/pocket_airbridge.fap`; launch via `scripts/runfap.py`.
- Protocol harness pass condition: use Playwright to load `http://localhost:8000/web/protocol-harness.html`, run all harness tests, assert visible pass count equals expected count and console has zero errors.

## Execution strategy

### Parallel execution waves
- Wave 1: Measurement harness + deploy timeout/gating tests + BLE advertising watchdog design can be implemented in parallel only if they do not touch the same files. Prefer sequential if both edit `pocket_airbridge.c`.
- Wave 2: Compression after deploy timeout/gating is stable.
- Wave 3: Crypto design checkpoint and implementation before NACK/windowing.
- Wave 4: NACK after encrypted transfer framing is stable.
- Wave 5: BLE radio tuning and stealth hardening, with hardware-only acceptance.
- Wave 6: Bundle/docs regeneration and full final hardware/security verification.

### Dependency matrix
| Todo | Depends on | Blocks | Can parallelize with |
| --- | --- | --- | --- |
| 1 | none | 2, 3, 4, 5, 6, 7, 8 | none |
| 2 | 1 | 3, 4, 8 | 7 if file conflicts avoided |
| 3 | 2 | 4, 8 | 7 if no FAP file conflict |
| 4 | 2 | 5, 6, 8 | none |
| 5 | 4 | 6, 8 | none |
| 6 | 5 | 8 | 7 after 5 if files separate |
| 7 | 1 | 8 | 2/3 only if no shared files |
| 8 | 2,3,4,5,6,7 | 9 | none |
| 9 | 8 | final wave | none |

## Todos

- [x] 1. Measurement and regression harness baseline
  What to do / Must NOT do: Add reusable timing/evidence hooks for Deploy stream, chat text, attachments, cancel, and Flipper counters. Preserve existing behavior; no protocol or firmware semantics change in this task.
  Parallelization: Wave 1 | Blocked by: none | Blocks: 2, 3, 4, 5, 6, 7, 8
  References (executor has NO interview context - be exhaustive): `web/protocol-harness.html:72-90`; `web/airbridge-protocol.js:1-15,117-157,252-642`; `web/chat-usb.html`; `web/chat-ble.html`; `web/bootstrap.js:1`; `tools/build_bundle.py:73-75`; `.agents/skills/pocket-airbridge-hardware-qa/SKILL.md`.
  Acceptance criteria (agent-executable): protocol harness current 17/17 still passes; new timing JSON records bundle bytes, first report, complete time, error state, and transfer KB/s for mock and hardware runs; no production behavior changes except diagnostics guarded behind test helpers or UI logs.
  QA scenarios (name the exact tool + invocation): Playwright persistent CDP loads `web/protocol-harness.html` and captures pass count; hardware QA records USB/BLE connect, text both ways, one attachment SHA, cancel, and counters. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-1/`.
  Commit: Y | `test(protocol): add AirBridge timing and regression harness evidence`

- [~] 2. Deploy state safety and timeout hardening
  What to do / Must NOT do: Add browser-side timeout/error UI for bootstrap `Connecting`; add explicit FAP handling so `0x42` outside `AirbridgeScreenWaiting` fails or is ignored visibly without hanging. MUST preserve menu-gated Deploy: no deploy streaming from Bridge mode.
  Parallelization: Wave 1/2 | Blocked by: 1 | Blocks: 3, 4, 8
  References: `web/bootstrap.js:1`; bundled FAP source `firmware/pocket_airbridge/pocket_airbridge.c:623-645,754-807,1347-1363`; live firmware source must be located before coding with `find ~/projects/flipperzero-firmware -path '*pocket_airbridge.c' -print` and recorded in evidence; `docs/protocol.md:206-225`.
  Acceptance criteria (agent-executable): bootstrap stays ASCII-only and ≤1200 chars or the plan records a deliberate budget raise; wrong-state `0x42` test fails visibly within timeout; Waiting-state `0x42` still streams app; Bridge-mode relay traffic with first byte `0x42` does not trigger deploy.
  QA scenarios: protocol harness adds wrong-state deploy test; Playwright deploy timeout test; hardware USB Deploy from `https://blank.org` still loads app. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-2/`.
  Commit: Y | `fix(deploy): fail visibly when bundle stream is not armed`

- [~] 3. Bridge-screen BLE advertising watchdog
  What to do / Must NOT do: Add Bridge-screen BLE advertising refresh/watchdog modeled on the existing Waiting-screen pump, but only when GAP is idle; do not disconnect active BLE chat sessions.
  Parallelization: Wave 1/2 | Blocked by: 1 | Blocks: 4, 8
  References: bundled FAP source `firmware/pocket_airbridge/pocket_airbridge.c:1467-1503`; live FAP path must be discovered with `find ~/projects/flipperzero-firmware -path '*pocket_airbridge.c' -print` before firmware edits; `/Users/asutov/projects/flipperzero-firmware/targets/f7/ble_glue/gap.c:144-214,548-586`; `/Users/asutov/projects/flipperzero-firmware/targets/f7/furi_hal/furi_hal_bt.c:256-273`.
  Acceptance criteria: after long-running Bridge screen and after app relaunch cycles, Web Bluetooth picker sees the device without app restart; active BLE chat is not kicked; logs/counters show no `TXERR` during healthy traffic.
  QA scenarios: persistent Chrome BLE picker before/after watchdog; text both directions after 10+ minutes idle; squatter/bonded macOS link does not require restart. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-3/`.
  Commit: Y | `fix(ble): refresh advertising from bridge idle state`

- [~] 4. Compressed Deploy bundle
  What to do / Must NOT do: Generate gzip bundle(s), deploy compressed app asset to SD, stream compressed bytes from FAP, and inflate in browser with `DecompressionStream("gzip")` when supported. Preserve clear fallback/error when unsupported.
  Parallelization: Wave 2 | Blocked by: 2, 3 | Blocks: 5, 8
  References: `tools/build_bundle.py:9-12,45-75`; `web/bootstrap.js:1`; `firmware/pocket_airbridge/pocket_airbridge.c:764-906`; MDN `DecompressionStream` docs: Baseline, gzip example `new DecompressionStream("gzip")`.
  Acceptance criteria: generated compressed bundle is smaller than raw `dist/app-usb.html`; bootstrap budget satisfied (≤1200 chars unless explicitly raised); decompressed app hash matches raw app; USB Deploy loads compressed app on hardware; unsupported browser path fails clearly.
  QA scenarios: build script size/hash check; protocol harness bootstrap compressed-stream test; hardware USB Deploy timing before/after compression. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-4/`.
  Commit: Y | `perf(deploy): stream compressed app bundle`

- [~] 5. Always-on E2E encryption design and implementation
  What to do / Must NOT do: Add first-class key exchange frames and always-on encrypted item payloads. Use browser WebCrypto ECDH P-256, HKDF-SHA256, AES-GCM-256. Show a 6-digit SAS code on both pages; block sending until verified; mismatch destroys keys and restarts handshake. Flipper remains blind relay.
  Parallelization: Wave 3 | Blocked by: 4 | Blocks: 6, 8
  References: `web/airbridge-protocol.js:1-15,117-157,252-642`; `web/chat-usb.html:518-655,699-714`; `web/chat-ble.html:629-760`; MDN `SubtleCrypto.deriveKey()` ECDH/HKDF/AES-GCM examples; `docs/protocol.md:28-155`.
  Acceptance criteria: new protocol frame types documented; both pages derive matching keys; SAS match unlocks send; SAS mismatch aborts and clears keys; plaintext text/file bytes do not appear in bridged ITEM_DATA frames after handshake; plaintext SHA-256 verifies after decrypt; AES-GCM tamper fails.
  QA scenarios: protocol harness tests encrypted text, encrypted attachment, SAS match/mismatch, tamper failure, reload/reconnect behavior; hardware text both ways and attachment SHA with lock indicator visible. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-5/`.
  Commit: Y | `feat(protocol): encrypt AirBridge items end to end`

- [~] 6. NACK support for encrypted transfers
  What to do / Must NOT do: Implement real `NACK` semantics after crypto framing is stable: payload format, sender retry behavior, retry limit, and distinction from `ERROR`/`BUSY`. Do not implement transfer resume.
  Parallelization: Wave 4 | Blocked by: 5 | Blocks: 8
  References: `web/airbridge-protocol.js:1-11,283-318,368-409,463-503,536-612`; `docs/protocol.md:49,155`; `web/protocol-harness.html:72-90`.
  Acceptance criteria: receiver sends NACK for recoverable missing/corrupt chunks according to documented format; sender retries exact sequence; retry exhaustion becomes ERROR; encrypted payload integrity still enforced by AES-GCM and plaintext SHA after decrypt; existing BUSY/CANCEL behavior unchanged.
  QA scenarios: mock transport drops a chunk then NACK recovers; repeated drops hit retry limit; wrong NACK ignored/fails safely; hardware transfer under induced loss/cancel returns idle. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-6/`.
  Commit: Y | `feat(protocol): recover chunk loss with NACK retries`

- [~] 7. BLE radio tuning and runtime evidence
  What to do / Must NOT do: Measure and, only where needed, tune runtime BLE parameters: MTU, DLE, 2M PHY, connection interval. Current code already sets high ATT MTU, DLE enabled, and 2M preferred; do not assume negotiated values without runtime evidence.
  Parallelization: Wave 5 | Blocked by: 1 | Blocks: 8
  References: `/Users/asutov/projects/flipperzero-firmware/targets/f7/ble_glue/app_conf.h:28-34,63,91`; `/Users/asutov/projects/flipperzero-firmware/targets/f7/ble_glue/ble_app.c:53-56`; `/Users/asutov/projects/flipperzero-firmware/targets/f7/ble_glue/gap.c:73-127,179-193,243-249,408,450-456`; `/Users/asutov/projects/flipperzero-firmware/applications/services/bt/bt_service/bt.c:248,496`; AirBridge serial max payload `/Users/asutov/projects/flipperzero-firmware/targets/f7/ble_glue/services/airbridge_serial_service.h:12-13`.
  Acceptance criteria: evidence records negotiated MTU, PHY, packet size, and connection interval; if explicit `hci_le_set_data_length`/`hci_le_set_phy` calls are added, they are gated and logged; BLE attachment throughput improves or the bottleneck is identified honestly.
  QA scenarios: hardware BLE throughput before/after with same file sizes; browser logs and firmware logs captured; text both ways and attachment SHA pass; no persistent `TXERR`. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-7/`.
  Commit: Y | `perf(ble): record and tune AirBridge link parameters`

- [x] 8. Stealth hardening without breaking UX
  What to do / Must NOT do: Add typing jitter after timeout/compression are stable; remove custom serial UUID from BLE advertising if `acceptAllDevices:true` + `optionalServices` remains usable; derive per-device DIS serial; perform Windows USB tree compare when hardware is available. Do not weaken selection failure handling.
  Parallelization: Wave 5 | Blocked by: 2,3,4,5,6,7 | Blocks: 9
  References: typing constants in bundled FAP source `firmware/pocket_airbridge/pocket_airbridge.c:26-39,647-762`; live FAP path must be discovered with `find ~/projects/flipperzero-firmware -path '*pocket_airbridge.c' -print` before firmware edits; BLE adv config `/Users/asutov/projects/flipperzero-firmware/lib/ble_profile/extra_profiles/airbridge_profile.c:268-305`; scan response `/Users/asutov/projects/flipperzero-firmware/targets/f7/ble_glue/gap.c:476-514,632-657`; BLE identity config in bundled FAP source `firmware/pocket_airbridge/pocket_airbridge.c:288-302,416-489`; Web Bluetooth options `web/airbridge-transports.js:217-220`; MDN `Bluetooth.requestDevice()` acceptAllDevices/optionalServices constraints.
  Acceptance criteria: jitter keeps deploy reliable under worst-case timing; BLE picker can connect with UUID not advertised or the plan records why not; selecting wrong BLE device fails safely; DIS serial differs per Flipper; Windows tree compare attached if hardware available, otherwise marked deferred.
  QA scenarios: timed USB/BLE Deploy under jitter; Web Bluetooth picker with multiple nearby devices; negative non-AirBridge device selection; macOS/ioreg and optional Windows evidence. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-8/`.
  Commit: Y | `chore(hardening): reduce AirBridge device fingerprints`

- [x] 9. Firmware bundle, docs, and final demo script
  What to do / Must NOT do: Regenerate `firmware/` bundle from live firmware repo, update docs/protocol/README with encryption/compression/NACK/BLE tuning behavior, and write a short demo script. Do not introduce new functional changes here.
  Parallelization: Wave 6 | Blocked by: 8 | Blocks: final wave
  References: `firmware/README.md`; `firmware/airbridge-firmware.patch`; `firmware/api-symbols-additions.patch`; `docs/protocol.md`; `docs/architecture.md`; `README.md`; `.agents/skills/pocket-airbridge-hardware-qa/SKILL.md`.
  Acceptance criteria: bundle dry-run applies to pristine firmware base; bundle contains all intended firmware changes and no stale deltas; docs describe always-on E2E encryption, SAS, compressed Deploy, NACK, BLE tuning, stealth caveats, and deferred transfer resume.
  QA scenarios: dry-run patch apply; full `./fbt` and FAP build; protocol harness; hardware demo: USB/BLE connect, encrypted text both ways, encrypted attachment SHA, cancel, USB Deploy, counters. Evidence `.omo/evidence/airbridge-roadmap-encryption/task-9/`.
  Commit: Y | `docs(firmware): document AirBridge v2 encrypted roadmap changes`

## Final verification wave
> Runs in parallel after ALL todos. ALL must APPROVE. Surface results and wait for the user's explicit okay before declaring complete.

- [x] F1. Plan compliance audit
  Tool/invocation: read-only Oracle reviewer plus `git diff --stat`, `git diff`, `git log --oneline -10`, and plan/evidence reads. Pass evidence: table mapping every changed file and commit to todos 1-9; explicit grep/read proof that no deploy stream can start from Bridge mode, no keyboard reports are sent outside Deploy, no transfer resume was added, and regenerated bundle/docs are present. Verdict must be `APPROVE` or `REJECT`.

- [x] F2. Security and crypto review
  Tool/invocation: read-only Oracle/security reviewer; inspect `web/airbridge-protocol.js`, `web/chat-usb.html`, `web/chat-ble.html`, `docs/protocol.md`, and protocol harness output. Pass evidence: checklist proving ECDH P-256 raw key fragmentation, HKDF salt/info/keyId, directional AES-GCM keys, IV uniqueness, exact AAD, six-digit SAS modulo rendering, mismatch/key clearing, replay rejection, fail-closed pre-unlock behavior, tamper/downgrade/MITM negative tests, and no plaintext item bytes visible to Flipper frames after unlock. Verdict must be `APPROVE` or `REJECT`.

- [~] F3. Hardware end-to-end QA
  Tool/invocation: load `pocket-airbridge-hardware-qa` and `playwright`; launch persistent Chrome with `mkdir -p ~/.cache/airbridge-pw-chrome && open -na "Google Chrome" --args --remote-debugging-port=9222 --user-data-dir=$HOME/.cache/airbridge-pw-chrome "https://blank.org"`; verify with `curl -s http://localhost:9222/json/version`; serve repo with `python3 -m http.server 8000`; use Playwright MCP with `cdp_url="http://localhost:9222"` for `http://localhost:8000/web/chat-usb.html`, `http://localhost:8000/web/chat-ble.html`, and `https://blank.org`. Every physical picker/button gate must first run the audible alert and then use `question` with the exact browser window/tab named.
  Pass procedure/evidence: capture `.omo/evidence/airbridge-roadmap-encryption/final-F3/` containing browser console logs, screenshots or DOM snapshots of lock indicator/SAS acceptance, transferred text transcript both directions, attachment plaintext SHA after decrypt, cancel recovery to idle, compressed USB Deploy success from `https://blank.org`, BLE picker visible after idle/watchdog interval without app restart, and Flipper-screen evidence showing green heartbeat plus counters `DROP 0` and `TXERR 0`. Verdict must be `APPROVE` or `REJECT`; reject if any step is self-reported without agent-observed browser/device evidence.

- [x] F4. Performance and evidence review
  Tool/invocation: read-only reviewer; inspect `.omo/evidence/airbridge-roadmap-encryption/task-*`, timing JSON, screenshots/logs, protocol harness output, and commit diffs. Pass evidence: before/after table for deploy compression, BLE throughput/link parameter evidence, timeout behavior, NACK recovery/retry exhaustion, and a list of any claimed speedup/stealth improvement with the exact supporting evidence path. Reject if any performance/stealth claim lacks measured evidence. Verdict must be `APPROVE` or `REJECT`.

- [x] F5. Scope fidelity and demo readiness
  Tool/invocation: read-only reviewer; run `git status --short` in `/Users/asutov/projects/flipper-hid` and `/Users/asutov/projects/flipperzero-firmware`, inspect README/docs/demo script, inspect `.omo/evidence/airbridge-roadmap-encryption/`, and compare docs to implemented behavior. Pass evidence: both repos clean; docs/demo script match actual encrypted/compressed/NACK behavior; physical consent gates documented and observed; Windows USB tree compare attached or explicitly deferred with reason; commit list is atomic and ordered. Verdict must be `APPROVE` or `REJECT`.

## Commit strategy

- Use multiple atomic commits; never one giant roadmap commit.
- Keep implementation+tests together for each todo.
- Firmware repo commits first for FAP/BLE/USB changes; then regenerate and commit `flipper-hid/firmware/` bundle.
- Suggested sequence:
  1. `test(protocol): add AirBridge timing and regression harness evidence`
  2. `fix(deploy): fail visibly when bundle stream is not armed`
  3. `fix(ble): refresh advertising from bridge idle state`
  4. `perf(deploy): stream compressed app bundle`
  5. `feat(protocol): encrypt AirBridge items end to end`
  6. `feat(protocol): recover chunk loss with NACK retries`
  7. `perf(ble): record and tune AirBridge link parameters`
  8. `chore(hardening): reduce AirBridge device fingerprints`
  9. `docs(firmware): document AirBridge v2 encrypted roadmap changes`
- Before every commit inspect `git status`, `git diff`, and `git log --oneline -10`; stage only intended files.

## Success criteria

- Current protocol harness 17/17 still passes, plus new compression/crypto/NACK tests pass.
- USB Deploy never hangs forever; wrong deploy state fails visibly; correct menu-gated Deploy still works.
- BLE picker works from Bridge screen after idle/relaunch cycles without manual app restart.
- Compressed Deploy transfers a smaller bundle and reconstructs exact app HTML.
- Encryption is always-on after SAS verification; plaintext user messages/files are not visible on bridged frames; tampering fails.
- Text succeeds in both directions and attachments verify SHA-256 after decrypt.
- NACK recovers a dropped chunk in mock tests and fails safely on retry exhaustion.
- BLE tuning has measured runtime evidence; any unachieved radio optimization is documented honestly.
- Stealth hardening is evidence-backed; USB/BLE corporate-detection caveats remain documented rather than overclaimed.
- Firmware bundle is regenerated and dry-run apply succeeds; both repos are clean after atomic commits.
