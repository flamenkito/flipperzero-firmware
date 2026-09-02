
## Task 1 reconnaissance (measurement/regression baseline)

- `web/protocol-harness.html`
  - Primary runner is `runAllProtocolTests()`; UI trigger is `#runBtn` labeled `Run All Tests`.
  - Pass gate is `PASS: ${passed}/${outcomes.length} protocol tests passed.`; current suite length is 17, so success is `17/17`.
  - Timing/evidence hook already exists in `runBootstrapStreamClient()` via `createTimingRecorder({ operation: 'deploy-stream', ... sink: record => harnessEvidenceRecords.push(record) })`.
  - Harness evidence sink is `window.__airbridgeHarnessEvidence`.
- `web/airbridge-protocol.js`
  - Lowest-level hook points for timing are `ItemSender.sendHello()`, `sendItemMeta()`, `sendItemData()`, `sendItemDone()`, `sendWithAck()`, and `waitForAck()`.
  - Receive-side event points are `ItemReceiver.onMessage()`, `handleHello()`, `handleMeta()`, `handleData()`, `handleDone()`, plus `emit('cancel'|'busy'|'error'|'item'|'ack'|...)`.
  - ACK payload shape is fixed to `[acked_type, acked_seq_be_hi, acked_seq_be_lo]`.
- `web/chat-usb.html`
  - Send path is `sendItem()`; attach timing there around `sendHello()`, `sendItemMetaWithAbort()`, `sendItemData()`, `sendItemDone()`.
  - Observability points for chat/file/cancel are `receiver.on('item')`, `receiver.on('cancel')`, `receiver.on('busy')`, `receiver.on('error')`, `handleInboundFrame()`, and `cancelActiveTransfer()`.
  - `sender.onAck(...)` already tracks chunk ACK progress; good place for per-chunk timing JSON emission without changing transport behavior.
- `web/chat-ble.html`
  - Same send/cancel observability as USB page: `sendItem()`, `sendItemMetaWithAbort()`, `sendBusy()`, `sendCancelFrame()`, `handleFrame()`, `handlePeerCancel()`, `receiver.on('item')`.
  - Transfer timing can be captured around `transferAbortController`, `transferPendingAckSeq`, and the `sender.onAck(...)` chunk tracker.
- `web/bootstrap.js`
  - Current bootstrap is a single-line deploy payload; it opens WebHID, requests the HP/Logitech/Dell/MSFT VID/PID list, receives reports, verifies checksum, then `document.write`s or falls back to `Blob` URL.
  - Deploy-stream timing can be observed around `x.sendReport(0, r)`, the `inputreport` listener, and the `await D` completion/verification path.
- `tools/build_bundle.py`
  - Bundle step inlines `airbridge-protocol.js`, `airbridge-transports.js`, and `airbridge-ui.js` into `dist/app-*.html`.
  - Production-behavior guard is the post-inline sanity checks that reject any remaining `import`, `type="module"`, or external UI references.
  - `bootstrap.js` is only length-checked here; bundle generation should stay untouched for timing-only work.
  - Task 1 fix: stale `airbridge-evidence.js` inlining was removed from the chat bundle path because the chat pages do not import that module.
- Current deploy/baseline risk notes
  - `web/protocol-harness.html` imports `./airbridge-evidence.js`, but that file is not present in the repo snapshot; timing JSON plumbing may need a new shared module or an in-page fallback.
  - `web/bootstrap.js` is minified and likely easiest to instrument by wrapping the report request / completion path in a separate evidence layer rather than expanding the payload.
## 2026-08-29 — Task 1 measurement baseline

- Added reusable browser evidence schema `airbridge-timing/v1` with bundle bytes, transfer bytes, first report latency, complete time, error state, KB/s, and mock/hardware markers.
- Protocol harness now emits mock evidence for deploy stream, bidirectional chat text, attachment SHA-256, cancel, and Flipper counter structure while preserving the 17/17 harness contract.
- Chat pages expose guarded evidence capture via `?evidence=1`; production URLs avoid evidence log/storage side effects.
- Repo-root harness serving exposed a stale path assumption: `/web/protocol-harness.html` must fetch `./bootstrap.js`, not `/bootstrap.js`.
- Mock post-change proof: `.omo/evidence/airbridge-roadmap-encryption/task-1/timing-mock-postchange.json` and screenshot `protocol-harness-post.png` show 17/17.
## 2026-08-29 — Task 1 measurement baseline

- Protocol harness is stable at `PASS: 17/17 protocol tests passed` from `http://localhost:8000/web/protocol-harness.html` when bootstrap fetches are relative to the harness directory.
- Mock timing evidence schema `airbridge-timing/v1` records `bundleBytes`, `transferBytes`, `firstReportLatencyMs`, `completeTimeMs`, `errorState`, `transferKbps`, `mock`, `hardware`, and `evidencePath`.
- Mock evidence coverage now includes deploy-stream, chat text in both directions, attachment, cancel, and Flipper counter records with `DROP=0`/`TXERR=0` fixture values.

## 2026-08-29 — AdversarialVerify Task 1

- Independent reviewer reran `git diff --check`, `python3 tools/build_bundle.py`, and the protocol harness through persistent Chrome CDP at `http://localhost:9222`; harness remained `PASS: 17/17 protocol tests passed` with zero console warnings/errors.
- Verified Task 1 evidence is mock-only and explicitly marked (`mock=true`, `hardware=false`, `marker="mock"`); hardware timing/counter measurements remain unclaimed.
- `python3 tools/build_bundle.py` regenerates `dist/app-usb.html`/`dist/app-ble.html` with inlined evidence helpers and no module imports; reviewer restored those generated artifacts afterward with `git restore -- dist/app-usb.html dist/app-ble.html`.

## 2026-08-29 — Task 1 adversarial gate review

- Confirmed Task 1 measurement baseline by inspecting source/evidence and running `git diff --check && python3 tools/build_bundle.py`; build output was `dist/app-usb.html`, `dist/app-ble.html`, and `web/bootstrap.js: 1104 chars`.
- Independently inspected `root-harness-result.json` (`PASS: 17/17 protocol tests passed.`), `root-console-errors.txt` (`Errors: 0, Warnings: 0`), chat mock evidence, Flipper counter mock evidence, and `hardware-blocker.json`.
- Evidence hooks are guarded behind `?evidence=1` on normal chat pages; harness mock evidence covers deploy-stream, bidirectional chat text, attachment SHA, cancel, and Flipper counter schema. Hardware timing remains explicitly unclaimed (`marker=mock`, `hardware=false`).
- Reviewer cleanup: rerunning `tools/build_bundle.py` touched generated `dist/app-*.html` and `tools/__pycache__/build_bundle.cpython-314.pyc`; restored those generated/cached artifacts with `git restore -- ...`. No server or browser was started by this review.
## 2026-08-29 — Task 1 measurement/regression baseline

- Added guarded browser evidence mode with `?evidence=1` for chat pages; normal production UI behavior remains uninstrumented except for available helper exports.
- Timing records use `airbridge-timing/v1` and include `bundleBytes`, `transferBytes`, `firstReportTimestampMs`, `firstReportLatencyMs`, `completeTimeMs`, `errorState`, `transferKbps`, `mock`, and `hardware`.
- Protocol harness final verification under persistent Chrome CDP passed `17/17` at `http://localhost:8000/web/protocol-harness.html`; evidence artifact: `.omo/evidence/airbridge-roadmap-encryption/task-1/protocol-harness-final.json`.
- Mock chat evidence for USB and BLE surfaces is captured with `?mock=1&evidence=1`; artifacts: `chat-usb-mock-evidence.json`, `chat-ble-mock-evidence.json`, and `flipper-counter-mock-evidence.json`.

## 2026-08-29 — Task 2 deploy safety hardening

- Bootstrap deploy now has a visible no-report/no-completion timeout: `Connecting` re-arms the Connect button with `Transfer timed out - retry` instead of hanging forever.
- `web/bootstrap.js` remains ASCII-only and under budget at 1,198 chars after adding timeout/retry reset behavior.
- FAP deploy stream gating is explicit: `0x42` streams only from `AirbridgeScreenWaiting`; Bridge mode leaves `0x42` as relay data; other non-waiting states show `DEPLOY NOT ARMED`.
- Protocol harness now passes `20/20`, adding bootstrap timeout, deploy state gate, and real-bootstrap timeout tests.
- Background-tab timer throttling made mock ACK tests flaky; replacing mock transport `setTimeout(0)` delivery with a microtask (`Promise.resolve()`) removed the timing artifact without changing protocol behavior.

## 2026-08-29 — Task 2 deploy timeout/state hardening

- `web/bootstrap.js` now composes a visible `Transfer timed out - retry` status when the HID bundle stream never sends reports; final size is exactly 1200 ASCII chars, so future deploy changes have zero slack unless they shrink first.
- Protocol harness grew from 17 to 20 tests for bootstrap timeout, deploy request state gating, and real bootstrap transfer-timeout UI; final persistent-CDP run passed `PASS: 20/20 protocol tests passed.`
- Cache-bust `./bootstrap.js` fetches inside harness eval tests; stale browser cache otherwise tested an old bootstrap and produced misleading timeout failures.
- Live FAP path for this machine is `/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c`; mirrored bundle copy is `firmware/pocket_airbridge/pocket_airbridge.c`.
- FAP deploy byte handling now explicitly starts stream only from `AirbridgeScreenWaiting`; `AirbridgeScreenBridge` still relays `0x42` as data, while other screens show `DEPLOY NOT ARMED` and do not silently hang.

## 2026-08-29 — Task 3 Bridge BLE advertising watchdog

- Bridge mode now has a BLE advertising watchdog at the existing Waiting pump cadence (`BLE_WAITING_PUMP_MS`), with its own timestamp so Bridge and Waiting state do not interfere.
- The Bridge watchdog is FAP-only: it checks the AirBridge BLE profile is installed, skips known connected sessions, requires `!furi_hal_bt_is_active()` before kicking, and then calls the HAL's existing idle-gated `furi_hal_bt_start_advertising()`.
- Bridge mode deliberately has no `bt_disconnect()` watchdog path; the only remaining disconnects are app teardown, BLE Deploy transition cleanup, and the existing deploy-scoped Waiting squatter kick.
- Task 2 deploy safety is preserved: `0x42` still starts streaming only from `AirbridgeScreenWaiting`; in `AirbridgeScreenBridge` it remains relay data.

## 2026-08-29 — Task 3 Bridge BLE advertising watchdog

- Existing BLE Waiting pump is deploy-scoped: it periodically calls `furi_hal_bt_start_advertising()` from `AirbridgeScreenWaiting` and only kicks silent HIDS squatters in that Waiting deploy state.
- Bridge mode had no equivalent idle advertising maintenance, so long-running Bridge/relaunch cycles could leave GAP idle until an app restart or disconnect callback restarted advertising.
- Added Bridge-only watchdog using the same `furi_hal_bt_start_advertising()` idle-gated helper, guarded by `!app->ble_connected`, and with no `bt_disconnect()` path; active BLE chat sessions should not be kicked by the watchdog.
- Task 2 deploy safety remains intact: `0x42` starts streaming only from `AirbridgeScreenWaiting`; Bridge mode still relays `0x42` as data.

## 2026-08-29 — Task 4 compressed Deploy bundle

- `tools/build_bundle.py` now writes deterministic gzip companions with `gzip.compress(..., mtime=0)` for both deploy bundles; final sizes are `dist/app-usb.html` 94,365 B → `dist/app-usb.html.gz` 23,718 B and `dist/app-ble.html` 96,220 B → `dist/app-ble.html.gz` 23,952 B.
- `web/bootstrap.js` stayed ASCII-only and under the inherited 1,200-character cap at 1,196 chars while adding `DecompressionStream("gzip")` inflation and a clear `Transfer unsupported - retry` path before WebHID selection.
- `web/bootstrap-ble.js` was updated because the FAP now streams `app-ble.html.gz` for BLE Deploy too; it inflates with the same browser API and reports `Transfer unsupported - retry` when unavailable.
- FAP stream source changed only in the deploy asset filenames: Waiting-state `0x42` opens `app-usb.html.gz` / `app-ble.html.gz`; Bridge-state `0x42` remains relay data and Bridge watchdog still has no disconnect path.
- Protocol harness final run on persistent Chrome CDP passed `PASS: 23/23 protocol tests passed`, including compressed-stream reassembly, unsupported `DecompressionStream`, and real `bootstrap.js` unsupported-path tests.

## 2026-08-29 — Task 4 compressed Deploy bundle

- Build output now includes deterministic gzip deploy assets: `dist/app-usb.html.gz` and `dist/app-ble.html.gz`; gzip bytes are smaller than raw bundles and `gzip.decompress()` hashes match the raw app HTML exactly.
- Deploy bootstrap stays ASCII-only and within budget at 1196/1200 chars; it requires `DecompressionStream("gzip")`, requests the gzip bundle with the existing 64-byte `0x42` report, verifies compressed-byte checksum, inflates, then writes the app page.
- FAP deploy streaming now reads `app-usb.html.gz` / `app-ble.html.gz` from app data while preserving Task 2 gating: only `AirbridgeScreenWaiting` starts streaming; Bridge mode still relays `0x42`; other screens show `DEPLOY NOT ARMED`.
- Protocol harness passed `PASS: 23/23 protocol tests passed`, including compressed stream success, gzip unsupported mock failure, and real `bootstrap.js` no-`DecompressionStream` error path.

## 2026-08-29 — Task 5 always-on E2E encryption

- Browser protocol now defines crypto v1 frame IDs exactly after the existing range: `KEY_OFFER=0x0A`, `KEY_REPLY=0x0B`, `KEY_CONFIRM=0x0C`, `KEY_ABORT=0x0D`.
- Crypto handshake is browser-only over the existing relay: USB sends `KEY_OFFER`, BLE replies with `KEY_REPLY`, both display a six-digit SAS, and `KEY_CONFIRM` unlocks only after local SAS acceptance plus peer confirmation for the same transcript hash.
- Encrypted items wrap the whole plaintext item before chunking with AES-GCM-256. The Flipper still sees and forwards only ordinary 64-byte frames; no firmware relay semantics changed.
- Outer encrypted metadata is limited to transport fields and intentionally excludes user-visible name, MIME type, plaintext size/hash, `chunks`, and `itemCounter`; plaintext details live inside the authenticated encrypted envelope.
- Protocol harness grew to `PASS: 30/30 protocol tests passed`, adding encrypted text/attachment, SAS match/mismatch, AES-GCM tamper, plaintext-before-unlock fail-closed, and reconnect key-clearing coverage.
- `ItemSender` retry semantics now match the plan wording: one initial send plus up to three retries per fragment; crypto fragment timeout budget was adjusted to the same window.

## 2026-08-29 — Task 5 always-on browser E2E encryption

- Browser protocol now adds crypto frame types `KEY_OFFER`/`KEY_REPLY`/`KEY_CONFIRM`/`KEY_ABORT` and a WebCrypto P-256 ECDH + HKDF-SHA256 + AES-GCM-256 session that keeps the Flipper as a blind frame relay.
- Chat pages are fail-closed: connected endpoints stay locked until SAS is displayed/accepted locally and a matching peer `KEY_CONFIRM` arrives; send controls remain disabled before unlock.
- Plaintext user metadata/name/MIME/hash move into the encrypted `AB1` inner envelope; outer `ITEM_META` uses `kind:"encrypted"` plus crypto transport fields and `chunks` only when the page sends it through the legacy chunker.
- Protocol harness now passes `PASS: 30/30 protocol tests passed`, including encrypted text/file, SAS match/mismatch, AES-GCM tamper, plaintext pre-unlock failure, and reconnect key clearing.

## 2026-08-29 — Task 5 docs consistency fix

- Root verification found stale Consent Model wording in `docs/protocol.md` that still said there was no application-level authentication/encryption.
- Updated only the docs wording: platform WebHID/Web Bluetooth/BLE pairing remain transport consent gates, while app-level confidentiality/authenticity now comes from SAS-verified browser E2E crypto v1 with no plaintext compatibility mode.

## 2026-08-29 — Task 6 NACK v1

- NACK v1 is browser-endpoint only: the Flipper remains a blind relay, and NACK payloads name only outer frame type/sequence plus reason (`missing`, `malformed`, `auth-failed-retryable`, `busy-window`).
- `ItemSender` retains current-item frames until `ITEM_DONE` ACK or terminal ERROR, so a NACK for an already-sent prior frame can retransmit the exact bytes without implementing byte-range resume.
- `ItemReceiver` keeps expected outer-frame state and delays recoverable NACK emission by the configured 750 ms default; duplicate accepted frames are ACKed idempotently.
- Protocol harness NACK coverage exercises gap-observed recovery, retry exhaustion, wrong NACK ignore, encrypted plaintext/hash integrity after recovery, and cancel clearing pending NACK state.

## 2026-08-29 — Task 6 NACK support

- NACK v1 is browser-endpoint only; the Flipper remains a blind relay and firmware/FAP behavior is unchanged.
- NACK payloads now mirror ACK and add a reason byte: `[type, seq_hi, seq_lo, reason]`, with current receiver use limited to `missing` and `malformed` outer-frame recovery.
- `ItemSender` retains current-item frames and retry counts until `ITEM_DONE` ACK or terminal `ERROR`; matching NACKs and ACK/NACK timeouts retransmit only the exact in-flight frame.
- Encrypted outer metadata now carries `chunks` as transport-only ciphertext reassembly data; plaintext name/MIME/size/hash remain inside the AES-GCM-authenticated `AB1` envelope.
- Protocol harness NACK coverage targets single dropped chunk recovery, repeated-drop retry exhaustion, wrong-NACK ignore behavior, encrypted plaintext integrity after recovery, and cancel clearing pending NACK state.

## 2026-08-29 — Task 7 BLE radio static baseline

- Firmware repo is not CodeGraph-indexed in this environment, so BLE parameter evidence was collected with direct source reads/searches instead of CodeGraph.
- Static BLE baseline: `CFG_BLE_MAX_ATT_MTU` is 414 bytes, `CFG_BLE_DATA_LENGTH_EXTENSION` is enabled, `gap_init_svc()` sets default PHY preference to 2M TX/RX, and AirBridge requests connection interval `0x0006..0x24` (7.5..45 ms), latency 0.
- Runtime hooks already log parts of the negotiated state when hardware is available: MTU response logs `Rx MTU size`, connection events log accepted interval/latency/supervision timeout, and PHY update events read/log TX/RX PHY. Static config alone is not negotiated evidence.
- AirBridge serial is capped at 244-byte characteristic values with 243-byte extended-update fragments, but the relay intentionally sends 64-byte HID-sized records (`59` browser payload bytes) over BLE for protocol/backpressure symmetry.
- No tuning change was made: existing static settings are already throughput-oriented, and hardware was absent, so changing MTU/DLE/PHY/interval would be speculative.

## 2026-08-29 — Task 7 BLE radio static baseline

- Firmware CodeGraph was unavailable for `/Users/asutov/projects/flipperzero-firmware` (no `.codegraph/`), so Task 7 used direct source inspection.
- Static BLE config already requests a high ATT MTU (`CFG_BLE_MAX_ATT_MTU = 414`), enables DLE in stack init, sets default TX/RX PHY preference to 2M, and AirBridge profile requests 7.5-45 ms connection interval with latency 0.
- Runtime hooks already exist for negotiated MTU (`Rx MTU size` -> max packet size), connection interval logging/renegotiation, and PHY readback after PHY update; none were exercised because hardware was absent.
- AirBridge serial service permits 244-byte service data / 243-byte characteristic fragments, but the bridge relay remains 64-byte HID-frame based, so static MTU headroom alone does not prove higher chat throughput.

## 2026-08-29 — Task 8 stealth hardening

- Deploy typing jitter is safest at the scheduler seam: add bounded delay to `app_typing_step()` press/release next-tick calculations only after `app_start_typing()` has entered `AirbridgeScreenTyping`; Bridge relay paths never see keyboard report calls.
- A per-device DIS serial can be derived without exposing raw Flipper identity by hashing `furi_hal_version_uid()` and formatting an HP-shaped serial (`HP` + eight hex digits); config `ble_dis_serial` still overrides the derived default.
- Web Bluetooth UUID hiding remains a hardware UX decision: MDN permits `acceptAllDevices:true` with `optionalServices`, but changing from service-filtered picker selection to broad picker selection must be proven with the real Chrome/BLE pairing flow before claiming stealth.

## 2026-08-29 — Task 8 stealth hardening

- Deploy typing jitter is scoped to the explicit typing state: `app_start_typing()` seeds `typing_jitter_state`, and `app_typing_step()` adds 0-7 ms only to press/release cadence. `app_service_input()` still runs before typing work each loop, and BLE retry boundaries service input, so BACK abort remains independent of stream completion.
- The mirrored FAP and live firmware FAP are byte-identical after Task 8 sync. The live build passed with `./fbt build APPSRC=applications_user/pocket_airbridge`.
- Default BLE DIS serial is now per-device without raw UID exposure: FNV-1a32 over `furi_hal_version_uid()` is formatted as `HP%08X`; `/ext/apps_data/pocket_airbridge/config` `ble_dis_serial` still overrides it.
- BLE serial UUID hiding was evidence-gated, not guessed: MDN allows `acceptAllDevices:true` with `optionalServices`, but Chromium picker UX, wrong-device handling, pairing, and reconnect need physical proof before changing the advertised UUID/filter behavior.

## 2026-08-29 — Task 9 firmware bundle and docs refresh

- Regenerated `firmware/airbridge-firmware.patch` and `firmware/api-symbols-additions.patch` from `/Users/asutov/projects/flipperzero-firmware` against pristine base `c9ab2b68`; dry-run apply passed with `patch --dry-run -p1` over a `git archive` copy.
- Mirrored the live FAP source from `/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c` to `firmware/pocket_airbridge/pocket_airbridge.c`; docs now state both live and mirrored paths.
- Current deploy bundle output is deterministic gzip: `app-usb.html` 132365 B to `app-usb.html.gz` 31707 B, `app-ble.html` 134578 B to `app-ble.html.gz` 32061 B, with gzip inflation matching raw SHA inputs.
- Persistent Chrome CDP harness passed `PASS: 35/35 protocol tests passed.` with zero console warnings/errors; this remains browser/mock proof, not physical Flipper proof.
- Docs and `docs/demo-script.md` now carry the conservative Task 9 state: browser-only E2E encryption, SAS unlock, compressed Deploy, exact-frame NACK retry, BLE static/runtime evidence limits, typing jitter, per-device DIS serial, UUID hiding deferment, Windows deferment, and no transfer resume.

## 2026-08-29 — Task 9 bundle/docs finalization

- Regenerated `firmware/airbridge-firmware.patch` and `firmware/api-symbols-additions.patch` from `/Users/asutov/projects/flipperzero-firmware` against base `c9ab2b68`; `patch --dry-run -p1` on a pristine `git archive` copy passed for both patches.
- Mirrored live FAP source remains byte-identical between `/Users/asutov/projects/flipperzero-firmware/applications_user/pocket_airbridge/pocket_airbridge.c` and `firmware/pocket_airbridge/pocket_airbridge.c`.
- Browser bundle build outputs deterministic gzip assets: `app-usb.html.gz` 31,707 B and `app-ble.html.gz` 32,061 B, with gzip decompression matching raw app HTML; `web/bootstrap.js` remains 1,196 ASCII chars.
- Protocol harness final Task 9 run used persistent Chrome CDP at `http://localhost:9222` and passed `PASS: 35/35 protocol tests passed.`
- Docs and `docs/demo-script.md` now state always-on browser E2E crypto, SAS fail-closed unlock, gzip Deploy with `DecompressionStream("gzip")`, exact-frame NACK retry only, static-only BLE tuning evidence, deploy typing jitter, per-device DIS serial, and deferred BLE UUID hiding, Windows USB tree compare, and transfer resume.

## 2026-08-29 — Final review docs rejection cleanup

- Updated `AGENTS.md` protocol instructions so NACK is active with payload `[nacked_type, nacked_seq, reason]`, crypto KEY frame types are listed, and Flipper blind-relay/browser-only E2E wording stays explicit.
- Updated `docs/firmware-guide.md` macOS USB enumeration guidance so current verification expects the selected impersonation profile, default HP `0x03F0:0x5341`, while `0x0483:0x5742` is labeled historical development-only.
- Verification grep found no stale active claims for `NACK(0x05, reserved/unused)`, `NACK reserved`, `No encryption`, or `No compression`; `git diff --check -- AGENTS.md docs/firmware-guide.md` passed.

## 2026-08-29 — Final review docs cleanup

- Updated `AGENTS.md` so the protocol list names active `NACK(0x05)` and crypto frame types `KEY_OFFER` through `KEY_ABORT`; NACK now documents `[nacked_type, nacked_seq, reason]`, exact current-item outer-frame retry, and no transfer resume.
- Updated `docs/firmware-guide.md` USB enumeration guidance so the running shipped FAP verifies the selected impersonation profile, default HP `0x03F0:0x5341`, while STM32 `0x0483:0x5742` is labeled historical development-only.
- Stale-claim grep over `AGENTS.md`, `README.md`, and `docs/*.md` found no `NACK(0x05, reserved/unused)`, `NACK reserved`, `No encryption`, `No compression`, or active `0x0483/0x5742` expectation matches. `git diff --check -- AGENTS.md docs/firmware-guide.md` passed with no output.

## 2026-08-31 — E2E crypto status pill (presentational only)

- Replaced the plain mono `E2E <state>` text in both chat pages with a `.status-pill`-pattern pill: `id="cryptoPill"` wrapper (`aria-live="polite"`), padlock glyph span `id="cryptoLock"` (`aria-hidden="true"`), and the existing `id="cryptoState"` text span now rendering `E2E locked|verify SAS|waiting peer|unlocked|aborted`.
- `updateCryptoPanel()` in `web/chat-usb.html` and `web/chat-ble.html` kept its signature and all control-gating logic untouched; it only gained lines setting the pill class (`crypto-unlocked` green / `crypto-pending` amber / `crypto-aborted`|`crypto-locked` red) and the glyph (U+1F512 closed lock when unlocked, U+1F513 open lock otherwise). Both pages edited identically — no drift.
- CSS lives once in shared `web/airbridge-ui.css` (`.crypto-pill` section after the `.status-*` rules); it recolors pill border+text via `var(--ok)`/`var(--warn)`/`var(--error)` and reuses the existing 999px radius / border tokens from `.status-pill`. No new fonts, icons, or dependencies.
- Rebuilt deploy bundles: `dist/app-usb.html` 133190 B (`app-usb.html.gz` 31898 B), `dist/app-ble.html` 135403 B (`app-ble.html.gz` 32249 B); `grep cryptoPill` confirms both bundles inlined the new markup. `node --check web/airbridge-protocol.js` clean.
- Verified on persistent Chrome CDP `http://localhost:9222` against `python3 -m http.server 8000` at repo root: protocol harness `PASS: 35/35 protocol tests passed.` with zero console errors; both `chat-usb.html` and `chat-ble.html` render the pill red (`rgb(248, 81, 73)` = `var(--error)`), open-lock glyph, text `E2E locked`, `aria-live="polite"`, radius 999px in default state, with Accept SAS / Abort Crypto still disabled. Zero console errors on both pages; screenshots `crypto-pill-usb-locked.png` / `crypto-pill-ble-locked.png` captured.
- The protocol-harness "Run All Tests" button requires a real click (`btn.click()` via `browser_evaluate` works); the suite does not auto-run on load.
