## 2026-08-29 — Task 1 blockers / risks

- Hardware measurements were not captured in this task. Do not claim real Deploy stream timing, WebHID/BLE throughput, or Flipper `DROP/TXERR` values until a physical gated run records `marker=hardware` evidence.
- Initial repo-root baseline produced 15/17 because the old harness fetched `/bootstrap.js`; captured as `.omo/evidence/airbridge-roadmap-encryption/task-1/baseline-prechange-root.json` and fixed to relative fetch.
- Browser module cache briefly reported stale imports; hard reload/cache-bust was required before final Playwright CDP verification.
## 2026-08-29 — Task 1 measurement baseline

- Hardware throughput/counter measurements remain unclaimed for this task; only mock/browser artifacts were captured under `.omo/evidence/airbridge-roadmap-encryption/task-1/`.
- Preflight found a dirty worktree with partial measurement-hook edits before this task continued; final DoneClaim should call this out so later tasks do not confuse preexisting dirty state with hardware evidence.
## 2026-08-29 — Task 1 issues/blockers

- Baseline on the plan-specified URL (`python3 -m http.server 8000` from repo root, `http://localhost:8000/web/protocol-harness.html`) initially produced `15/17` because two bootstrap tests fetched `/bootstrap.js`; fixed to relative `./bootstrap.js`, after which final harness evidence passed `17/17`.
- Hardware USB/BLE picker and Flipper counter evidence were not run in this worker. No physical gate was requested, and this tool session does not expose the required `question` tool. Mock/checklist placeholders are in `.omo/evidence/airbridge-roadmap-encryption/task-1/manual-qa-surfaces.json`.

## 2026-08-29 — Task 2 issues/blockers

- Hardware USB Deploy from `https://blank.org` was not attempted because this session does not expose the required `question` tool for browser picker / Flipper physical gates; see `.omo/evidence/airbridge-roadmap-encryption/task-2/hardware-blocker.json`.
- LSP diagnostics are not authoritative here: Biome is not installed for JS/HTML and clang cannot resolve Flipper SDK headers from the main repo, while the exact FAP build command succeeds.
- Baseline dirty worktree included inherited Task 1 changes (`tools/build_bundle.py`, chat pages, protocol harness, `web/airbridge-evidence.js`) before Task 2 edits; see `baseline-worktree-status.txt`.

## 2026-08-29 — Task 2 issues/blockers

- Hardware USB Deploy was not run: `python3 tools/flipper_alive.py` reported `ABSENT: no usbmodem port`, and this tool session still does not expose the required blocking `question` tool for physical gates. Do not claim hardware Deploy success for Task 2.
- `web/bootstrap.js` is exactly at the 1200-character budget after timeout hardening. Task 4 compression work must shrink before adding new bootstrap behavior or explicitly record a budget raise.
- LSP diagnostics were not authoritative for this task: Biome LSP is unavailable/declined for JS/HTML, and the mirrored C source cannot resolve Flipper SDK headers in this repo. The live firmware FAP build passed and is the meaningful C gate.

## 2026-08-29 — Task 3 issues/blockers

- Hardware BLE picker / active-session retention evidence was not captured: `python3 tools/flipper_alive.py` reported `ABSENT: no usbmodem port`; see `.omo/evidence/airbridge-roadmap-encryption/task-3/hardware-blocker.json`.
- LSP diagnostics are still not authoritative for the mirrored C source from the main repo because clang cannot resolve Flipper SDK headers (`'furi.h' file not found`); the live firmware FAP build passed.
- Port `8000` was already occupied by another server during the first harness attempt, which loaded stale/cached harness behavior and failed. The passing evidence is the cache-busted run on `http://localhost:8765/web/protocol-harness.html?task3=20260829`.

## 2026-08-29 — Task 3 issues/blockers

- Hardware BLE picker and long-idle Bridge watchdog behavior require a real Flipper plus physical browser/BLE gates; do not claim this from build/static proof alone.
- The firmware and main repo were already dirty before Task 3. Task 3's intended code delta is limited to the Bridge advertising watchdog in the live FAP and mirrored bundle copy, plus task-3 evidence/notepad artifacts.
- Task 3 protocol harness verification did not pass in this session: persistent-CDP run reported `FAIL: 13/17 passed, 4 failed` with two ACK timeouts and two `bootstrap.js` fetch failures. Task 3 did not edit web files, but the full verification claim remains blocked until the harness is repaired/re-run.
- Hardware BLE evidence was blocked by `python3 tools/flipper_alive.py` returning `ABSENT: no usbmodem port`; no BLE picker or `TXERR` hardware claim was made.

## 2026-08-29 — Task 4 issues/blockers

- Hardware USB Deploy timing/success was not claimed: `python3 tools/flipper_alive.py` returned `ABSENT: no usbmodem port`; see `.omo/evidence/airbridge-roadmap-encryption/task-4/hardware-blocker.json`.
- JS/HTML LSP diagnostics remain unavailable because Biome LSP is not installed/was declined; `web/bootstrap.js`, `web/bootstrap-ble.js`, and `web/protocol-harness.html` were instead verified through the persistent-CDP protocol harness.
- Mirrored C LSP diagnostics still cannot resolve Flipper SDK headers from the main repo (`'furi.h' file not found`); the live firmware `./fbt build APPSRC=applications_user/pocket_airbridge` passed and is the meaningful FAP gate.
- The main repo still has inherited dirty Task 1/2/3 files (`web/chat-*`, `web/airbridge-evidence.js`, and `docs/protocol.md`) outside Task 4 scope; Task 4 continuation removed its temporary broad docs edits and did not commit.

## 2026-08-29 — Task 4 issues/blockers

- Hardware USB Deploy timing before/after compression was not run: `python3 tools/flipper_alive.py` reported `ABSENT: no usbmodem port`; see `.omo/evidence/airbridge-roadmap-encryption/task-4/hardware-blocker.json`. No hardware Deploy success/timing claim is made.
- LSP diagnostics remain limited for browser/FAP files: Biome is unavailable/declined for JS/HTML, and clang in the main repo cannot resolve Flipper SDK headers (`'furi.h' file not found`). The live firmware FAP build is the meaningful C gate and passed.
- Post-write LOC checks flag pre-existing oversized files (`web/protocol-harness.html`, `firmware/pocket_airbridge/pocket_airbridge.c`, live FAP). Refactoring them is out of Task 4 scope/risk; the change is intentionally minimal and harness/build-verified.

## 2026-08-29 — Task 5 issues/blockers

- Hardware encrypted USB↔BLE chat/file proof was not run: `python3 tools/flipper_alive.py` returned `ABSENT: no usbmodem port`. Do not claim physical encrypted chat, file transfer, DROP/TXERR, or throughput evidence for Task 5.
- JS/HTML LSP diagnostics are still limited: Biome is unavailable/declined for HTML files and diagnostics timed out for `web/airbridge-protocol.js`; the meaningful browser gates are the persistent-CDP protocol harness and bundle build, both passing.
- Task 5 added crypto to oversized legacy browser files (`web/protocol-harness.html`, `web/airbridge-protocol.js`, chat pages). Splitting them is real cleanup work but out of scope for the encryption contract; tests now lock the crypto behavior before any future refactor.

## 2026-08-29 — Task 5 issues/blockers

- Hardware encrypted chat/file proof was not captured: `python3 tools/flipper_alive.py` returned `ABSENT: no usbmodem port`; no WebHID/Web Bluetooth picker or physical Flipper SAS run is claimed.
- JS/HTML LSP diagnostics remain unavailable/timed out in this repo; Task 5 browser changes are verified through `node --check`, `python3 tools/build_bundle.py`, and the persistent-CDP protocol harness.
- `web/airbridge-protocol.js` and `web/protocol-harness.html` are now oversized; Task 5 kept the implementation local to preserve the existing browser-only/no-dependency architecture, but follow-up refactoring should split crypto helpers and harness fixtures before adding Task 6 NACK work.

## 2026-08-29 — Task 6 issues/blockers

- Hardware USB↔BLE recovery proof still requires a real Flipper plus WebHID/Web Bluetooth permission gates; this worker must not claim physical DROP/TXERR or transfer recovery evidence unless `tools/flipper_alive.py` and gated browser runs are captured.
- `web/airbridge-protocol.js` and `web/protocol-harness.html` remain oversized legacy browser files; Task 6 kept NACK scoped to protocol semantics and harness proof rather than risky file splitting.

## 2026-08-29 — Task 6 issues/blockers

- Hardware recovery/cancel proof is not claimed in Task 6 unless a real Flipper is present; NACK behavior is browser-harness verified only when `tools/flipper_alive.py` reports no usbmodem port.
- JS/HTML LSP diagnostics remain unavailable in this repo, so the meaningful Task 6 gates are `node --check`, `tools/build_bundle.py`, deterministic gzip decompression/hash checks, and the persistent-CDP protocol harness.
- `web/airbridge-protocol.js`, `web/protocol-harness.html`, and chat pages are oversized legacy browser files. Task 6 keeps NACK changes local and tested; a later refactor should split protocol retry machinery and harness fixtures before more feature work.

## 2026-08-29 — Task 7 issues/blockers

- Runtime BLE tuning evidence is blocked: `python3 tools/flipper_alive.py` returned `ABSENT: no usbmodem port`. No Web Bluetooth picker, serial log, PHY/DLE/MTU negotiation capture, throughput measurement, or Flipper `DROP/TXERR` counter evidence was collected.
- Do not claim negotiated BLE values from Task 7 static artifacts. `static-params.json` records configured/requested values only; runtime values remain deferred until a physical gated run uses persistent Chrome CDP at `http://localhost:9222`.
- Product code currently has no explicit `hci_le_set_data_length()` or per-connection `hci_le_set_phy()` tuning call. Add either only with hardware evidence, return-status logging, negotiated-value logging, and a passing firmware/FAP build.

## 2026-08-29 — Task 7 issues/blockers

- Hardware BLE tuning evidence was not captured: `python3 tools/flipper_alive.py` returned `ABSENT: no usbmodem port`; no negotiated MTU/PHY/DLE packet size/connection interval or BLE attachment throughput claim is made.
- No BLE parameter change was justified without runtime evidence. Static configuration is recorded under `.omo/evidence/airbridge-roadmap-encryption/task-7/static-params.json`; runtime evidence remains blocked under `hardware-blocker.json`.
- Build was not run because Task 7 made no firmware/config code changes. The existing main and firmware worktrees were already dirty from prior roadmap tasks; Task 7 intended writes are evidence/notepad only.

## 2026-08-29 — Task 8 issues/blockers

- Hardware deploy/BLE evidence remains blocked: `python3 tools/flipper_alive.py` returned `ABSENT: no usbmodem port`, so Task 8 does not claim deploy reliability under jitter, BLE picker behavior, wrong-device behavior, or Flipper `DROP/TXERR` counters.
- BLE advertising UUID removal is deferred rather than guessed. Current firmware still advertises the AirBridge serial UUID and current Web Bluetooth still uses a service filter; changing this needs a physical Chrome picker run proving `acceptAllDevices:true` UX and safe wrong-device failure.
- Windows USB tree compare is deferred because no Windows host or real HP dongle capture is available in this environment.
- LSP diagnostics remain limited for changed non-web files: mirrored C cannot resolve Flipper SDK headers from the main repo, Markdown has no configured LSP, and the live firmware file is outside the current LSP workspace. The live firmware FAP build passed and is the meaningful C gate.

## 2026-08-29 — Task 8 issues/blockers

- Hardware evidence remains blocked: `python3 tools/flipper_alive.py` reported `ABSENT: no usbmodem port`, so Task 8 does not claim deploy reliability under jitter, hidden-UUID BLE picker success, wrong-device behavior, or Flipper `DROP/TXERR` counters.
- BLE serial service UUID is still advertised continuously. Removing it is deferred until a physical Chromium run proves `acceptAllDevices:true` + `optionalServices` preserves selection/reconnect UX and wrong-device selection fails safely.
- Windows USB tree compare is deferred because this session ran on macOS/Darwin without a Windows host or usbccgp capture.
- LSP diagnostics remain non-authoritative for the mirrored C source (`furi.h`/SDK include resolution) and unavailable for Markdown; the live FAP build is the meaningful C gate and passed.

## 2026-08-29 — Task 9 issues/blockers

- Hardware final demo QA was not run: `python3 tools/flipper_alive.py` reported `ABSENT: no usbmodem port`. No Task 9 physical USB Deploy, WebHID, Web Bluetooth, encrypted chat/file, counter, or BLE runtime claim is made.
- BLE service UUID hiding remains deferred. The AirBridge serial UUID is still advertised and browser discovery still uses the service-filtered picker until a physical Chromium run proves the broad picker path.
- Windows USB tree comparison remains deferred because this session ran on macOS without a Windows host or real usbccgp capture.
- Markdown has no configured LSP, and mirrored C diagnostics in the main repo still fail on missing Flipper SDK headers. The live firmware `./fbt build APPSRC=applications_user/pocket_airbridge` passed and is the meaningful C gate.

## 2026-08-29 — Task 9 issues/blockers

- Hardware QA was not run: `python3 tools/flipper_alive.py` reported `ABSENT: no usbmodem port`. No Task 9 claim is made for WebHID picker, Web Bluetooth picker, USB Deploy, negotiated BLE MTU/PHY/DLE/interval, throughput, or Flipper `DROP/TXERR` counters.
- Markdown has no configured LSP, and main-repo C diagnostics cannot resolve Flipper SDK headers (`furi.h` missing); the live firmware `./fbt build APPSRC=applications_user/pocket_airbridge` passed and remains the meaningful C verification gate.
- BLE service UUID hiding and Windows USB tree comparison remain deferred until physical Chrome/BLE and Windows host evidence exists. Transfer resume remains intentionally absent; NACK retries exact current-item outer frames only.
