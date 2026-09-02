# BLE HIDS advertising window learnings

- `GapSvc` stores the live advertised service list as `adv_svc_uuid` plus `adv_svc_uuid_len`; it is distinct from the profile's `GapConfig.adv_service` template. The initial 16-bit HIDS list is an `AD_TYPE_16_BIT_SERV_UUID` byte followed by little-endian `HUMAN_INTERFACE_DEVICE_SERVICE_UUID` bytes.
- `aci_gap_set_discoverable()` receives that list directly. A zero `Service_Uuid_length` omits it, while the local name, flags, and appearance continue through the existing GAP setup. The scan-response manufacturer data is set separately by `hci_le_set_scan_response_data()`.
- The vendor stack exposes `aci_gap_update_adv_data()`, but its raw payload API does not model the discoverable command's service-list argument. The toggle therefore uses the existing `gap_advertise_start()` stop/set-discoverable/start path when state is `GapStateAdvFast` or `GapStateAdvLowPower`, preserving the configured interval.
- `gap_set_adv_hids()` takes `state_mutex`, mutates only the live service-list bytes, and queues a GAP-thread refresh for active advertising. The refresh re-checks state before restarting, so a connection that completes before queue processing cannot restart advertising. Connected and idle states retain the new payload for their next advertising start.
# W3 — Web getDevices-first reconnect (2026-07-23)

## What landed

- `web/airbridge-transports.js` — `WebBluetoothAdapter.connect()` now tries
  `tryGrantedReconnect()` first: `navigator.bluetooth.getDevices?.()` → exact-name
  match against `identity.BLE_NAME` (`"HP 725 K+M"`) → `gatt.connect()` → verify via
  existing `findSerialService()`. 3 attempts, 500 ms backoff, then falls back to the
  unchanged `requestDevice` picker. GATT bring-up factored into `openGattSession()`
  shared by both paths; public contract (isConnected/getName/onReceive/onDisconnect)
  untouched. Retry rationale is commented in code per plan.
- `web/bootstrap-ble.js` — minimal getDevices-first block ahead of the picker:
  2 attempts (one retry), 500 ms backoff, early `break` when the device is not in the
  grant list (no pointless 500 ms picker delay). Exact-name match `'HP 725 K+M'`.
  Picker fallback byte-compatible (`namePrefix:'HP'` filter kept). 1874 → 2111 bytes
  (+237, ~+13%). ASCII-only verified (0 chars >127), `new Function` syntax check passes.

## Verification evidence

- `python3 tools/build_bundle.py` → `dist/app-usb.html` 72747 B, `dist/app-ble.html`
  73795 B, bootstrap.js 1200-char guard still green.
- SD deploy via `scripts/storage.py -p /dev/cu.usbmodemflip_Luwot1 send -f` to
  `/ext/apps_data/pocket_airbridge/`; `size` readback matched local bytes exactly:
  bootstrap-ble.js 2111, app-usb.html 72747, app-ble.html 73795. App was not running
  (CDC port present throughout).
- Protocol harness: `PASS: 17/17 protocol tests passed.` via permanent CDP Chrome
  (:9222, Chrome 137) on `http://localhost:8081/protocol-harness.html` — fresh reload,
  `runAllProtocolTests()` fired un-awaited, `#status` polled. Window stayed visible.

## Implementation notes for W4

- The granted-device path intentionally does NOT disconnect GATT after the
  bootstrap's verify probe (`getPrimaryService` inside the retry loop). The
  subsequent `x.gatt.connect()` in the shared flow resolves with the same server
  object when already connected, so the transfer continues on the verified link.
- `tryGrantedReconnect()` removes the stale `gattserverdisconnected` listener from
  the granted device before falling through to the picker, so a late disconnect event
  from the abandoned device cannot fire the caller's onDisconnect against the new
  picker session.
- `handleDisconnected` in the BLE adapter (unlike the WebHID one) does not check
  `event.device !== this.device` — pre-existing; left as-is (blast-radius discipline).
- Harness does not cover `bootstrap-ble.js` or `WebBluetoothAdapter` (protocol +
  `bootstrap.js` USB only), so 17/17 is a regression gate, not coverage of the new
  path. Real proof of the getDevices path is W4 hardware E2E.
- Chrome flags still required on the receiving origin:
  `#enable-experimental-web-platform-features` (+ `#enable-web-bluetooth-new-permissions-backend`
  for cross-restart persistence). With flags off, `getDevices` is undefined and both
  code paths skip straight to the picker (no error).

## W2: FAP wiring of furi_hal_bt_set_adv_hids (2026-07-23)

- All changes in `applications_user/pocket_airbridge/pocket_airbridge.c`; helper
  `app_set_hids_adv(app, enable)` + 6 call sites. Build exits 0; SDKCHK now
  reports **API 90.1** (W1 export live). FAP 21,568 bytes (was 21,224).
- `furi_hal_bt_is_connected` does NOT exist — guard uses `app->ble_connected`
  as the task's fallback.
- KEY API SEMANTICS (verified in `targets/f7/ble_glue/gap.c:527`): `gap_set_adv_hids`
  LATCHES the adv service-UUID list under the GAP state mutex in EVERY state and
  queues an AdvRefresh only if actively advertising (AdvFast/AdvLowPower). While
  connected or idle, the latched content applies at the next adv start. So a swap
  mid-link is state-safe and takes effect exactly when it matters.
- GUARD DECISION (deviates from the task letter, documented in-code): the ON swap
  is guarded by `!app->ble_connected` (pointless mid-link — adv is stopped, and
  the typing-end OFF re-latches name-only before adv could air again). OFF swaps
  are UNguarded because `bt_disconnect` does NOT wait for the
  disconnection-complete event (`bt_api.c:34-43` unlocks after the message is
  processed; `bt_close_connection` fires HCI disconnect async — that's why
  `app_restore_ble` adds its own 200 ms wait). `ble_connected` is therefore still
  true at the typing-end OFF site; a guarded OFF would be skipped and HIDS would
  leak into the Waiting advertising — the exact macOS-claim bug this plan fixes.
- Call sites: (1) ON in `app_start_typing` for BLE transport only, right before
  `screen = AirbridgeScreenTyping` (after bootstrap load, before first key
  report); (2) OFF in `app_typing_step` success path, BEFORE `bt_disconnect`;
  (3) OFF in `app_abort_typing` for BLE transport (covers BACK-abort, the
  KEYBOARD SEND ERROR abort at line ~590, and the exit-path abort);
  (4) OFF in `app_show_error` gated on `screen == Typing && transport == BLE`
  (covers the NOT-US-ASCII and send-error paths that skip abort); (5) OFF at
  startup after `app_configure_ble` when `ble_profile_installed` (boot default
  name-only; scrub is unconditional since GAP defaults are malloc'd, not
  guaranteed zeroed); (6) OFF on exit immediately before `app_restore_ble`.
- USB typing never touches adv state: every swap site is behind a BLE-transport
  check (startup/exit OFFs are transport-agnostic but idempotent name-only
  latches — USB deploy leaves adv exactly as bridge default).
- `furi_hal_bt_start_advertising()` after every swap per spec — no-op unless
  GapStateIdle; `gap_get_state()` returns GapStateUninitialized when `!gap`, and
  `gap_set_adv_hids` early-returns on `!gap`, so both calls are safe even with
  BLE never configured.
- Every Bridge-entry path is covered transitively: Typing-BACK via abort, Error
  via app_show_error, Waiting/Streaming inherit the typing-end OFF, DeployPrompt
  never had ON, boot via startup OFF.
- Not deployed/flashed (orchestrator handles hardware); W4 hardware E2E is the
  real proof (macOS picker stays populated outside the typing window).

## W2.1: prompt-phase window — pairing needs adv BEFORE OK (2026-07-23)

- GAP FOUND by flow analysis: ON at `app_start_typing` (OK press) is too late for
  a FIRST pairing. BLE typing needs macOS connected as a keyboard before the
  first key report. With the window OFF at Bridge, System Settings shows nothing
  to pair; by OK time macOS has no chance — the FAP types into the void and hits
  KEYBOARD SEND ERROR. The pre-window flow worked only because HIDS was always
  advertised.
- FIX: the window now opens at the DEPLOY PROMPT for BLE (DOWN on Bridge) and
  closes on every prompt-exit path:
  1. ON in the Bridge DOWN handler (BLE → DeployPrompt). ON kept in
     `app_start_typing` too (idempotent, covers any prompt-skipping path).
  2. OFF on BACK from DeployPrompt (BLE only) — window must not leak open when
     returning to Bridge without typing.
  3. `app_show_error` scrub gate widened from `screen == Typing` to
     `screen == Typing || screen == DeployPrompt` (BLE transport): the OK→
     `app_start_typing`→`app_load_bootstrap` failure path errors out while the
     screen is STILL DeployPrompt, with HIDS latched ON from the prompt — without
     the widening that leaks through Error → Bridge.
- Prompt text for BLE now instructs pairing during the prompt:
  y=31 `Pair "HP 725 K+M" in BT`, y=42 `settings, then press OK` (23 chars each,
  same width as the USB lines they replace; USB text unchanged). Device name
  verified against `lib/ble_profile/extra_profiles/airbridge_profile.c:122`
  (`.device_name = "HP 725 K+M"`) — the prompt matches what macOS shows.
- DESIGN RULE recorded: the HIDS-adv window spans the entire BLE deploy
  interaction (prompt → typing), not just active typing. Window-open points are
  user-facing screens where the host needs to discover/pair; window-close points
  are EVERY exit from that flow (BACK, error, typing-end success, abort, app
  exit). Any new screen added to the BLE deploy flow must re-audit open/close.
- Build exits 0, API 90.1. FAP 21,720 bytes (was 21,568).

## W3: deploy-scoped squatter kick in Waiting (2026-07-23)

- Problem: with bonding ON, the macOS HID daemon auto-reconnects the bonded
  keyboard within ~1 s of any disconnect and holds the link sitting on HIDS,
  never writing the vendor RX. Advertising stays stopped, so the Web Bluetooth
  picker (needs fresh adv) is always empty — blocking the FIRST grant on any new
  origin (blank.org deploy target).
- Mechanism: `ble_connected_since` (new struct field) stamped in
  `app_ble_status_changed_callback` on the rising edge only. In the existing
  Waiting pump block (`screen == AirbridgeScreenWaiting && transport == BLE`):
  if `ble_connected && now - ble_connected_since >= BLE_WAITING_SQUATTER_KICK_MS`
  (3000, new define next to `BLE_WAITING_PUMP_MS`) → `bt_disconnect` +
  `furi_hal_bt_start_advertising()`, then re-stamp `ble_connected_since = now` to
  defer re-check while the async disconnect lands.
- WHY IT CANNOT KICK CHROME: the bootstrap writes `0x42` to the vendor RX
  immediately after subscribing; `app_handle_relay` (line ~1148) routes a `0x42`
  in Waiting to `app_start_stream`, which sets `screen = AirbridgeScreenStreaming`
  (line ~690). Once 0x42 lands the screen is no longer Waiting, so the kick branch
  is unreachable — verified the assignment site, not just the intent. A central
  still connected in Waiting past the deadline is therefore definitionally one
  that never requested the bundle.
- Rate limit is structural: one kick per connection — each rising edge re-stamps
  the window, and macOS's auto-reconnect gets a fresh 3 s to prove itself (it
  never will; it also never gets to hold the link longer than that).
- Scope discipline: NO kick in Bridge/chat mode (chat clients may sit silently;
  that multiplexing is handled by the getDevices path, not disconnects). USB
  deploy, pump idle-restart, pairing, and typing are untouched — the kick lives
  entirely inside the pre-existing `Waiting + BLE` branch.
- Note the interplay with W2.1: during BLE typing the FAP itself disconnects at
  typing end, so the timestamp entering Waiting is always from a NEW connection
  made during Waiting (or a straggler teardown, which the re-stamp absorbs with at
  most one harmless redundant disconnect in a millisecond race window).
- Build exits 0, API 90.1. FAP 21,868 bytes (was 21,720).

## W3.1: kick timeout 3 s → 15 s — pairing-ceremony budget (2026-07-23, hardware evidence)

- HARDWARE FINDING: 3000 ms killed the FIRST bootstrap Connect on a FRESH origin
  mid-pairing ("Transfer failed - retry"; the retry succeeded because the bond
  then existed). The first Connect on a new origin runs a full pairing ceremony
  — numeric code shown on the Flipper plus human reaction time — BEFORE the
  bootstrap writes 0x42. The kick clock starts at the rising edge (connect), not
  at pairing completion, so the ceremony eats the window.
- Fix: `BLE_WAITING_SQUATTER_KICK_MS` 3000 → 15000 (same constant, no rename
  needed). 15 s covers the pairing ceremony while still cycling macOS HID
  squatters off the link every 15 s, so pickers keep catching advertising
  windows. Kick comment updated with this rationale so nobody "tightens" it back.
- Lesson: any connect-time deadline gated on application-layer proof-of-intent
  must budget for the security ceremony first — bond state changes the timing
  profile between first and subsequent connects on the same machine.
- Rebuild exits 0, API 90.1. FAP 21,868 bytes — unchanged (immediate constant
  only).

## W4: BLE typing pacing → USB parity (2026-07-24)

- Changed `BLE_TYPE_PRESS_DELAY_MS` 40 → 12, `BLE_TYPE_RELEASE_DELAY_MS` 60 → 18.
  `BLE_TYPE_MODIFIED_SETTLE_MS` (10) and `BLE_TYPING_RETRY_DELAY_MS` (20) unchanged.
- Rationale: macOS negotiates ~11.25–15 ms connection intervals for HID
  keyboards (our profile advertises 7.5–45 ms). At 12/18 ms, a press+release
  notification pair drains within one interval; the old 40/60 was sized for the
  worst-case 45 ms CI and is unnecessarily slow. USB typing already uses 12/18
  — BLE now has parity.
- Transient queue congestion is absorbed by `app_ble_kb_report_with_retry`
  (5 attempts, 20 ms apart), so the tighter cadence does not need extra slack
  there.
- RISK UNDER TEST: if a release notification is missed at 12/18 pacing, a
  modifier stays down and corrupts the typed bootstrap stream. The acceptance
  criterion for this change is a full-bootstrap BLE typing test with ZERO
  corruption (no stuck-modifier artifacts). The constant block comment now
  records this risk so nobody softens it without running that test.
- Rebuild exits 0, API 90.1. FAP 21,856 bytes (was 21,868).


## W4 prep: firmware flash + FAP deploy (2026-07-23 19:19)

- Full firmware flash via `./fbt flash_usb` — the gap.c `gap_set_adv_hids`
  change lives in `targets/f7/ble_glue/gap.c`, so FAP-only is not enough.
  Self-update package uploaded at ~150 kb/s; serial dropped, device rebooted
  into the updater, port `/dev/cu.usbmodemflip_Luwot1` returned after ~15 s.
- Storage responsive immediately after port return (`storage.py list /ext`
  works on first try).
- Pre-deploy stray check: exactly one line at `/ext/apps/USB/
  pocket_airbridge.fap` (old 21224-byte build still there) — nothing to remove.
- FAP upload OK at ~160 kb/s (3 chunks), size verified `21568` bytes (matches
  local build mtime `19:14`, +344 vs previous for the `app_set_hids_adv`
  helper + 6 call sites).
- `runfap.py` launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x10001182b`). FAP is live on the HIDS-adv-window
  firmware, ready for W4 E2E.

## W4 prep follow-up: corrected FAP deploy (2026-07-23 19:37)

- User exited the previously-running FAP via BACK; port
  `/dev/cu.usbmodemflip_Luwot1` present without needing a restart.
- Pre-deploy stray check: exactly one line at `/ext/apps/USB/
  pocket_airbridge.fap` (previous 21568-byte build).
- Upload OK at ~150 kb/s (3 chunks), size verified `21720` bytes (matches
  local build mtime `19:23`, +152 vs previous — the W2 fix increment).
- `runfap.py` launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x100011cd5`).

## W4 prep: bonding firmware flash + FAP + W3c web assets (2026-07-23 22:05)

- User restart #3 today (LEFT+BACK after another wedged BACK-exit — line added
  to ble-impersonation/issues.md). Port `/dev/cu.usbmodemflip_Luwot1` back ~2 s
  after desktop.
- `./fbt flash_usb` uploaded the bonding-ON + serial-UUID-in-adv firmware
  (firmware.dfu only — update.fuf/updater.bin unchanged from prior flash and
  skipped by the build). Serial dropped, device rebooted, port returned ~15 s.
- Pre-deploy stray check: exactly one line at `/ext/apps/USB/
  pocket_airbridge.fap` (already the 21720-byte build, uploaded anyway for
  consistency).
- FAP upload + size verify: `21720` bytes (matches local mtime `19:23`).
- W3c web assets pushed to `/ext/apps_data/pocket_airbridge/` and size-verified
  byte-for-byte against local:
  - `bootstrap-ble.js` `2087` bytes (was `2111` in W3 — prompt-window change
    shrank it by 24)
  - `app-usb.html` `75949` bytes (was `72747` in W3 — +3202)
  - `app-ble.html` `76997` bytes (was `73795` in W3 — +3202)
- `runfap.py` launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x10001426b`). Device is on bonding firmware + W3c
  web assets, ready for W4 hardware E2E.

## W4 prep: squatter-kick FAP deploy (2026-07-23 23:10)

- User restart (LEFT+BACK), port `/dev/cu.usbmodemflip_Luwot1` back ~2 s.
- Pre-deploy stray check: exactly one line at `/ext/apps/USB/
  pocket_airbridge.fap` (previous 21720-byte build).
- Upload OK at ~160 kb/s (3 chunks), size verified `21868` bytes (matches
  local mtime `23:06`, +148 vs previous — squatter-kick increment).
- `runfap.py` launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x100015504`).

## W4 prep: final kick-window FAP deploy (2026-07-23 23:25)

- User exited previous FAP cleanly (BACK); port `/dev/cu.usbmodemflip_Luwot1`
  present without restart.
- Pre-deploy stray check: exactly one line at `/ext/apps/USB/
  pocket_airbridge.fap` (previous 21868-byte squatter-kick build).
- Upload OK at ~160 kb/s (3 chunks), size verified `21868` bytes — same byte
  count as the previous build (kick-window change is an immediate-constant
  bump to 15 s, no new code). Local mtime `23:21` supersedes `23:06`.
- `runfap.py` launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x100015b08`).

## Final-Wave gap-fix flash + F2 web assets (2026-07-24 12:44)

- First `./fbt flash_usb` attempt HUNG at `Installing "f7-update-local"` with
  zero chunks sent (killed at 600 s). `storage.py list /ext` also hung at that
  point (exit 124), and `lsof` showed no stale serial client. This is the
  documented hang mode; recovery = physical unplug/replug (user-gated).
- After replug, storage responded immediately and the retry `./fbt flash_usb`
  completed cleanly (firmware.dfu 99 chunks + update.fuf + updater.bin 15
  chunks at ~150 kb/s; `minusbinstall.flag` touched). Log at
  `/tmp/flash_final_wave2.log`.
- Port `/dev/cu.usbmodemflip_Luwot1` returned ~15 s after updater reboot.
- LESSON: `2>&1 | tail -N` on `./fbt flash_usb` is a footgun — when the
  process is killed by timeout, tail's buffered output is lost entirely and
  you can't tell whether the hang was in build or flash. Always `tee` to a
  file (or redirect to a file and tail after) for any long-running fbt call.
- Pre-deploy stray check: exactly one line at `/ext/apps/USB/
  pocket_airbridge.fap` (21868 bytes — already current, no re-upload).
- F2-remediation web assets pushed to `/ext/apps_data/pocket_airbridge/` and
  size-verified against local bytes:
  - `bootstrap-ble.js` `2119` bytes (task expected 2118 — task was off by
    one; local file IS 2119, on-device matches local)
  - `app-usb.html` `77436` bytes
  - `app-ble.html` `78484` bytes
- `runfap.py` launch clean, no trailing `Device not configured`.
- Post-launch: CDC gone, fresh `HP Wireless Keyboard and Mouse@01100000`
  enumeration (ioreg id `0x1000220a3`). Device on Final-Wave gap-fix firmware
  (advertise_hids init + gap_int.h), F2 web assets, current FAP.

## W3b: bonded AirBridge + serial service advertising (2026-07-23)

- `lib/ble_profile/extra_profiles/airbridge_profile.c:275` now sets
  `bonding_mode = true` while retaining `GapPairingPinCodeVerifyYesNo`; first
  pairing remains numeric comparison and subsequent reconnects use the stored
  bond. Its GAP template advertises the 128-bit serial service and explicitly
  moves the complete local name to scan response.
- UUID order is deliberate: `targets/f7/ble_glue/services/airbridge_serial_uuid.h:4-15`
  documents that the macro bytes are STM32WB controller order. The literal
  serial bytes `{A3,27,D6,13,26,9C,46,5F,B4,C5,F0,BA,28,12,87,7B}` therefore
  advertise as canonical Web Bluetooth UUID
  `7b871228-baf0-c5b4-5f46-9c2613d627a3`.
- `targets/f7/ble_glue/gap.c:16-26` asserts the concrete legacy-ADV budget:
  flags 3 + 128-bit UUID-list AD structure 18 + optional 16-bit HIDS-list AD
  structure 4 = 25 <= 31. `gap_advertise_start()` supplies the serial UUID as
  the base service list, removes the stack-added TX-power AD type for this
  profile, and adds HIDS with `aci_gap_update_adv_data()` only when
  `advertise_hids` is true. `gap_set_adv_hids()` now toggles that boolean and
  retains the W1 refresh queue; it no longer replaces the serial UUID list.
- `gap.c:630-641` builds scan response as complete local name then manufacturer
  data. The default HP identity is 12 bytes for the complete-name AD structure
  and 4 bytes for `{0x65,0x00}` manufacturer data, totaling 16 <= 31. The FAP's
  existing config validation (`pocket_airbridge.c:394-400`) rejects identities
  that would exceed the complete-name-plus-manufacturer scan-response budget.
- Bond persistence needs no extra key-storage change: `bt_keys_storage.c:38-59`
  stores IRK/ERK and `bt_keys_storage.c:270-301` reloads the `.bt.keys` NVM
  data; `bt.c:456-495` loads it before every profile change, while its
  key-storage callback at `bt.c:406-415` persists NVM updates. The static
  `ble_app_nvm` buffer (`ble_app.c:16`) survives the profile reinitialization;
  `BLE_NVM_DATA_TO_SRAM` reloads it into the radio stack, and `gap.c:366-368`
  writes the restored IRK/ERK to the controller. The prior non-persistence was
  solely AirBridge's disabled bonding flag.
- Verification: full `/Users/asutov/projects/flipperzero-firmware/./fbt` exited
  0 on 2026-07-23, compiled the changed AirBridge/GAP units, linked and packaged
  the DFU, and reported `API version 90.1 is up to date`. No firmware was
  flashed, no FAP/web sources changed, and no API-symbol edits were made by W3b.

## W3c: Web exact filters + reconnect backoff (2026-07-23)

### What landed

- `web/airbridge-transports.js` — `WebBluetoothAdapter.connect()` picker fallback
  is now `filters: [{ services: [identity.SERIAL_SERVICE_UUID] }]` (exact;
  `acceptAllDevices` removed), `optionalServices` kept; picker cancel still
  propagates as `NotFoundError`. Bounded auto-reconnect: unintentional
  `gattserverdisconnected` (i.e. `handleDisconnected` reached while
  `autoReconnect` is armed) now retries `openGattSessionSerialized()` at
  [500, 1000, 2000, 4000, 8000] ms, then disarms and fires the existing
  disconnect callback. `connect()` arms + cancels any prior chain;
  `disconnect()` disarms + cancels pending retries. New private machinery:
  `BLE_RECONNECT_DELAYS_MS`, `scheduleReconnect(delayIndex)`,
  `cancelReconnect()` (epoch bump + clearTimeout), `openGattSessionSerialized()`
  (shares an in-flight bring-up on the same device so a `connect()` racing a
  reconnect attempt never issues a concurrent `gatt.connect()`, which Chromium
  rejects). `tryGrantedReconnect` uses the serialized opener too.
- `web/bootstrap-ble.js` — picker is now `{filters:[{services:[U]}]}`;
  `namePrefix:'HP'` gone and `optionalServices` dropped (filter services are
  auto-granted). 2111 → 2087 bytes (−24). ASCII-only (0 chars >127),
  `new Function` syntax check passes.

### Verification evidence

- Node behavioral harness (mocked `navigator.bluetooth` + recorded fake timers;
  temp copy under `$TMPDIR/opencode/w3c-test/`), 7/7 scenarios:
  F exact-filter shape/no acceptAllDevices; A all-fail → delays exactly
  [500,1000,2000,4000,8000], 6 connect calls, callback ×1; B flaky-then-success
  → session restored, callback ×0; C disconnect during pending timer → no
  retry ran, callback ×0; D disconnect racing an in-flight attempt → the
  attempt's session is torn down (`gatt.disconnect`), callback ×0;
  E double-drop mid-attempt → no chain fork, single give-up callback,
  max 1 concurrent `gatt.connect`; G getDevices-first path still skips picker.
- `python3 tools/build_bundle.py` → `dist/app-usb.html` 75949 B,
  `dist/app-ble.html` 76997 B (both +3202; the whole transports module is
  inlined into each bundle, no tree-shaking), bootstrap.js 1200-char guard
  green. `acceptAllDevices` count in both bundles: 0.
- Protocol harness: `PASS: 17/17 protocol tests passed.` via permanent CDP
  Chrome (:9222, Chrome 137) on `http://localhost:8081/protocol-harness.html`,
  `runAllProtocolTests()` fired un-awaited, `#status` polled; window visible.
- SD deploy NOT done (Flipper app RUNNING) — pending orchestrator push at the
  next app-exit window: `bootstrap-ble.js`, `app-usb.html`, `app-ble.html` to
  `/ext/apps_data/pocket_airbridge/`.

### Implementation notes for W4

- Epoch discipline: every new chain (fresh drop), `connect()`, and
  `disconnect()` bumps `reconnectEpoch`; stale chain continuations exit
  silently. This is what prevents parallel chains when a second drop lands
  while a retry's `gatt.connect()` is in flight (scenario E regression).
- A retry attempt that SUCCEEDS after `disconnect()` raced it is torn down
  inside the success continuation (`!autoReconnect` → clearGattState +
  removeEventListener + `gatt.disconnect()`); a success adopted by a newer
  `connect()` (armed, stale epoch) is left alone.
- During the backoff window the page sees `isConnected() === false` and
  `send()` throws, but the UI disconnect callback has NOT fired yet — it only
  fires on give-up. chat-ble.html therefore stays "connected" across silent
  reconnects; that is the intended demo behavior (brief RF blips self-heal).
- `handleDisconnected` still has no `event.device !== this.device` check
  (pre-existing, noted in W3); the armed flag makes a stale-device event
  slightly more consequential (it would start a chain on the CURRENT device),
  but every known path removes stale listeners (disconnect, tryGrantedReconnect
  give-up).
- Residual exotic race (accepted, documented): `openGattSessionSerialized`
  keys sharing by device object; a `connect()` for a DIFFERENT physical device
  while a retry is mid-flight would run two bring-ups concurrently. Unreachable
  in practice: same-device sharing means the picker can only appear after the
  in-flight attempt has settled.
- Harness 17/17 remains a regression gate only — it does not execute the BLE
  adapter. Real proof of the filter + backoff is W4 hardware E2E (W3b firmware
  must advertise the serial UUID for the exact filter to match in the picker;
  until W3b flashes, the picker path shows no devices — getDevices path
  unaffected).

## example.com → blank.org canonical target (2026-07-23)

- Standardized `https://blank.org` as the canonical bootstrap/deploy target across all
  docs and skills, replacing `https://example.com` everywhere.
- Rationale: blank.org is a real HTTPS origin (satisfies WebHID/Web Bluetooth secure-context
  requirement), a blank page (clean landing surface), and loads from browser cache offline
  (example.com NXDOMAINs on an offline machine; cached google.com tabs caused window-naming
  drift in the orchestrator).
- Files changed:
  - `AGENTS.md` — PLAYWRIGHT TEST SURFACE rule: `https://example.com` → `https://blank.org`,
    rationale added inline.
  - `docs/firmware-guide.md` — Deploy Flow step 2: `https://example.com` → `https://blank.org`,
    note added that blank.org loads from cache offline.
  - `README.md` — Deploy app steps: both `https://example.com` occurrences replaced with
    `https://blank.org`; surrounding guidance (about:blank warning, isSecureContext check tip)
    preserved intact.
- `.agents/skills/pocket-airbridge-hardware-qa/SKILL.md` and
  `.agents/skills/flipper-zero-dev/references/hid-composite-deploy.md` had no
  `example.com` references and were not modified.
- No code, firmware, or FAP files touched (bootstrap is target-agnostic).

## W5 full regression (bonded build, FAP 21,868 b) — 2026-07-23
- Phase 1 PASS: protocol harness 17/17 (fresh tab, ~5 s, permanent CDP Chrome).
- Baseline anomaly: at regression start, ioreg showed STOCK identity ("Flipper Luwot", idVendor 1155), not HP composite — FAP did not appear to be running despite "live" handoff. Flagged at gate 1 for user to launch/confirm.
- Tab drift again: handoff said chat-usb/chat-ble tabs loaded; CDP showed only blank.org deployed chat + DevTools. Created all localhost tabs fresh.
- INCIDENT (pre-phase-2): FAP exited spontaneously at some point before regression start (stock identity on bus, no user action). Relaunched via runfap by user; HP composite stable 60+ s. No repro yet — watch for recurrence during phases and flag loudly if it happens mid-transfer.
- Phase 3 FINDING: silent getDevices connect did NOT engage on chat-ble (localhost:8081). Log: "[11:33:00 AM] Opening Web Bluetooth picker..." printed same-second as Pair click -> getDevices() returned no usable granted device despite handoff claiming a grant exists for localhost:8081 + "HP 725 K+M". Chooser auto-cancelled after ~57 s idle ("User cancelled the requestDevice() chooser"). Hypotheses: (a) grant never persisted for this origin/device, (b) W5 services-only picker filter created a grant keyed differently than getDevices expects, (c) Chrome requires the device to have been granted with optionalServices covering the serial UUID. NEXT: complete picker manually (should create the grant), then re-test silence via disconnect+reconnect after phase 4.
- Phase 3 RESOLUTION / FINDING: user resolved chat-ble connect by restarting the app. Root cause: macOS held the bonded keyboard link; getDevices service-discovery failed 3 times, picker was empty. After restart the link freed and chat-ble connected. IMPLICATION: bridge-mode squatter-kick is needed — disconnect centrals with zero serial-RX activity for N seconds in Bridge mode (chat-ble sends HELLO/messages, macOS HID never writes vendor RX). Risk: silent read-only chat users could be wrongly kicked; needs careful heuristic.
- Phase 4 PASS: bridge text both directions and 2 KB attachment on bonded build. Tab A (USB): you="bond-reg usb2ble", peer="bond-reg ble2usb". Tab B (BLE): peer="bond-reg usb2ble". Attachment card on tab B: "bond-reg-2kb.bin, 2.0 KB", "SHA-256 verified b2a8170614e23194..." (full b2a8170614e23194ae2951423d601987f518ce2f11205d7b0b708080103b9f76), download link present.
- Phase 5 (blank.org deployed chat reconnect): Pair clicked, status went to "connected / HP 725 K+M" within 5 s. Page log shows "Opening Web Bluetooth picker..." followed by "Connected" same-second — functionally SILENT (no visible picker, no orchestrator gate needed), consistent with an existing grant. The page log text is slightly misleading; it prints "Opening..." before the getDevices attempt. This validates the getDevices-first + W3c backoff reconnect path on blank.org.
- Phase 5 PASS: blank.org deployed chat-ble reconnected silently via getDevices (no visible picker, connected within 5 s). Both BLE centrals (localhost chat-ble and blank.org deployed chat) stayed connected simultaneously — W5 appears to support multiple GATT subscribers / broadcast TX. Sent "bond-reg deployed2usb" from blank.org; arrived as peer message on chat-usb (tab A) at 11:39:32 AM. Validates getDevices-first + W3c reconnect on the deployed origin.
- Phase 6 PASS: USB deploy E2E on bonded build. User typed bootstrap.js into fresh blank.org DevTools console (progress bar visible during typing), then clicked Connect + picked "HP Wireless Keyboard and Mouse". app-usb.html streamed and booted: title "Pocket AirBridge — USB Chat", full chat UI (#transcript, #textInput), log "WebHID mode ready.", status Disconnected post-stream as expected. Deployed app ready for its own WebHID connect.
- Phase 7 PASS: clean BACK exit (no wedge this time). After ~5 s enumeration, ioreg confirms stock identity: idVendor 1155 (0x0483), "Flipper Luwot" / "Flipper Devices Inc.", full IOUSB tree shows ZERO HP composite (no idVendor 1008, no "HP Wireless Keyboard and Mouse").

## W5 FULL REGRESSION VERDICT (ble-hids-adv-window bonded build, FAP 21,868 b)
| Phase | Result |
|-------|--------|
| 1. Protocol harness 17/17 | PASS |
| 2. chat-usb WebHID connect | PASS (one picker gate) |
| 3. chat-ble connect | CONDITIONAL PASS after manual app restart; silent getDevices FAILED when macOS held the bonded keyboard link (picker empty, service-discovery failed 3x). FINDING: bridge-mode squatter-kick still needed to kick centrals with zero serial-RX activity. |
| 4. Bridge text both directions | PASS |
| 5. 2 KB attachment USB→BLE SHA-256 | PASS |
| 6. blank.org deployed chat-ble reconnect | PASS — silent via getDevices, both BLE centrals stayed connected, message from blank.org arrived on chat-usb |
| 7. USB deploy E2E on blank.org | PASS — bootstrap typed with progress bar, Connect + picker, app-usb.html streamed and booted |
| 8. Exit identity restore | PASS — clean BACK exit, stock Flipper identity restored, zero HP composite |

Bottom line: all functional flows PASS on the bonded build. The remaining known gap is the bridge-mode squatter-kick for the bonded-keyboard macOS case; the deploy-Waiting squatter-kick and blank.org reconnect path work. No regressions attributable to the W5 bonding/adv-window changes.

## W6: publish record (2026-07-24)

- Regenerated `firmware/airbridge-firmware.patch` and
  `firmware/api-symbols-additions.patch` from the firmware tree relative to
  pristine upstream `c9ab2b68`; each patch dry-runs cleanly against a fresh
  archive of that base.
- Copied `applications_user/pocket_airbridge/pocket_airbridge.c` into the
  bundle and verified it is byte-identical to the live FAP source.
- Published the bonded advertising contract in the firmware guide, protocol,
  bundle README, and BLE QA scanner: serial UUID always advertised; HIDS only
  during BLE Deploy prompt/typing; first pairing numeric comparison with silent
  bonded reconnects; 15-second Deploy Waiting squatter-kick; getDevices-first
  Chrome reconnects and the Bridge-mode macOS residual.
- `python3 scripts/ble_qa_scan.py --selftest` passed both the serial-only
  Bridge advertisement and the serial-plus-HIDS Deploy-window advertisement.

## W7: Final-Wave F2/F4 remediation (2026-07-24)

- `Gap` is allocated with `malloc`, so `gap_init()` now explicitly sets
  `advertise_hids = false` before the GAP thread can reach its first advertising
  start. This prevents uninitialized heap data from advertising HIDS outside the
  explicit BLE Deploy window.
- The adv-window fields were audited: `service.scan_response_len` is already set
  to zero before scan-response construction, and `service.adv_name` continues to
  borrow the configured name unchanged.
- Adding `gap_set_adv_hids(bool)` to `gap.h` made the API extractor propose
  version 90.2 because public `furi_hal_bt.h` includes `gap.h`. The declaration
  therefore lives in the private `gap_int.h` included only by `gap.c` and
  `furi_hal_bt.c`; the ad-hoc forward declaration was removed. The API manifest
  remains at version 90.1 and contains no `gap_set_adv_hids` entry, so this does
  not extend the SDK surface.

## 2026-07-23 — W4/W5 RAW EVIDENCE BLOCK (Final-Wave F3 remediation)

### Live ble_qa_scan outputs (hardware, not selftest)

**Run 1 — Bridge screen, bonded firmware (pre-deploy):**
```
PASS | TARGET FOUND | HP 725 K+M | C099F161-A217-9B8E-3CB2-B80492E9284D
PASS | advertised local name is exact or scan-response-verified prefix | HP 725 K+M | exact advertised local name
PASS | "flipper" absent from name, advertising data, and scan response | absence | absent
FAIL*| advertised service UUIDs are HIDS only | 0x1812 | 7b871228-baf0-c5b4-5f46-9c2613d627a3
PASS | HP manufacturer data is present | 0x0065 | 0x0065
PASS | exact-name advertisement source is unique | 1 | 1
```
(* scanner assertion was written for the old HIDS-only design; actual = serial UUID in adv, HIDS ABSENT on Bridge — the intended new spec. Assertion updated in W6.)

**Run 2 — BLE deploy prompt (HIDS window OPEN, user holding on prompt):**
```
PASS | TARGET FOUND | HP 725 K+M
FAIL*| advertised service UUIDs are HIDS only | 0x1812 | 00001812, 7b871228-baf0-c5b4-5f46-9c2613d627a3
```
Window toggle proven on-air: Bridge = serial only; prompt = serial + HIDS.

**Run 3 — macOS holding the bonded keyboard link (picker-empty root cause):**
```
FAIL | TARGET NOT FOUND | advertisement local name 'HP 725 K+M' | no matching advertisement observed
```
Same moment, system_profiler SPBluetoothDataType showed "HP 725 K+M" in Connected (Address 02:00:00:82:52:3C) — device connected to macOS HID daemon, hence not advertising, hence empty Chrome picker.

**Run 4 — after Forget in BT settings:**
```
PASS | TARGET FOUND | HP 725 K+M
FAIL*| advertised service UUIDs ... | 7b871228-baf0-c5b4-5f46-9c2613d627a3
```
Link released, advertising restored.

### W4 gate artifacts (question-tool user confirmations, chronological)
1. "On the prompt" — user held Flipper on BLE deploy prompt while Run 2 scan proved the HIDS window live.
2. "Bonded ✓" — first pairing via System Settings completed, code confirmed on Flipper, device persisted in My Devices (bonding ON).
3. "Typed + executed OK" — BLE bootstrap typed into the Protocol Harness tab console (TYPING progress bar watched).
4. "picker is empty 'localhost wants to pair'" — first bootstrap Connect attempt: picker empty because macOS re-grabbed the bonded keyboard (Run 3 evidence).
5. getDevicesType "undefined" in CDP profile → user enabled #enable-web-bluetooth-new-permissions-backend and relaunched; re-check: getDevicesType "function", granted [].
6. "Done — forgotten" — device forgotten once in BT settings (bond desync reset); Run 4 proved advertising restored.
7. "Connected ✓" — chat-ble picker grant: "HP 725 K+M" picked, code confirmed on Flipper. getDevices then returned ["HP 725 K+M"] (grant artifact, CDP check at 22:38).
8. "Typed + executed OK" — bootstrap re-typed into blank.org (secure context verified: isSecureContext true, navigator.bluetooth/hid present).
9. "picker showed it! then 'Transfer failed - retry' -- but ble chat booted in a moment" — SQUATTER-KICK PROOF: device appeared in a fresh-origin picker after the kick freed the link; attempt 1 lost the post-kick race to macOS HID auto-reconnect (generic failure, button re-armed); attempt 2 connected, 0x42 landed, stream+checksum passed, app-ble.html booted.
10. "Connected" — deployed BLE chat on blank.org connected (getDevices path, no picker, no code).

### W5 gate artifacts
- "Done — connected" (WebHID picker, HP Wireless Keyboard and Mouse).
- chat-ble empty-picker finding: user-reported, resolved by app restart (macOS grip); recorded as bridge-mode squatter follow-up.
- "Typed + executed OK" (USB deploy on fresh blank.org tab) → USB bootstrap Connect → WebHID picker → app-usb.html streamed (progress bar) → booted (agent-verified page state: title "Pocket AirBridge — USB Chat", #connectBtn present).
- "Done — exited" — clean BACK exit; ioreg: idVendor 1155, "Flipper Luwot", zero "HP Wireless" on bus.

### Bond-reuse-no-code artifact
At steps 9/10 (blank.org, ~10 min after the 22:38 chat-ble bond): no pairing code appeared on the Flipper during the retry connect or the deployed-chat connect — the user answered "Connected" with no code-confirm gate requested by the orchestrator (every prior fresh pairing had required one). Bond was reused silently.

## 2026-07-23 — Patch application verification (F4 remediation)
- `patch --dry-run -p1` against a fresh `git archive c9ab2b68` tree: airbridge-firmware.patch exit 0, api-symbols-additions.patch exit 0. (unix patch tool used because the git-apply command is permission-denied here; equivalent hunk-level verification.)

## 2026-07-24 — F2 Final-Wave (W7) remediation: bootstrap-ble state scoping + BLE stale-session race

### Findings fixed (F2 oracle, both blocking)

1. **`web/bootstrap-ble.js` — global/single-use transfer state + enabled button during transfer.**
   `n`/`c`/`a`/`D`/`h` moved from module scope into the click handler (`let n,c,a=[],h,D=new Promise(...)`),
   so every click starts with a fresh accumulator, expected length, checksum, stream promise, and a
   **locally declared** notification handler `h` (was implicitly global). `b.disabled=1` is the first
   statement of the handler (browsers fire no click events on disabled buttons → double-request gone);
   re-armed with `b.disabled=0` in the success path right before the `replaceChild` attempt and kept in
   the catch. `x`/`k`/`r` deliberately stay module-scope: `x` persists the granted device for picker-free
   retry, `k` is reassigned before each use. Size 2,087 → **2,119 B** (+32), ASCII-only (verified
   byte-wise), `node --check` clean.

2. **`web/airbridge-transports.js` — `disconnect()` did not cancel an in-flight `openGattSession()`.**
   `openGattSession()` now captures `device` + `reconnectEpoch` at entry and checks staleness after every
   awaited GATT step (`gatt.connect`, `getPrimaryServices`, `findSerialService`, both `getCharacteristic`,
   `startNotifications`). Stale ⇒ tear down the partial session (remove `gattserverdisconnected` listener,
   `gatt.disconnect()` on that device, `clearGattState()` guarded by `this.server?.device === device` so a
   newer session's state is never clobbered) and throw `GATT session cancelled by disconnect`.
   KEY DESIGN POINT: staleness = `epoch changed && !autoReconnect`. `connect()` also bumps the epoch via
   `cancelReconnect()`, so an epoch-only check would stale-kill an in-flight bring-up that a fresh
   `connect()` legitimately shares (W3 device-keyed sharing) — epoch+flag encodes exactly "disconnect()
   ran". `disconnect()` itself unchanged (already bumps epoch via `cancelReconnect()`); public contract
   unchanged. `tryGrantedReconnect()` gained `if (!this.autoReconnect) throw error;` in its retry catch —
   without it a killed attempt-1 session would be torn down but attempt 2 would resurrect it 500 ms later,
   re-introducing the exact finding on the initial-connect path.

### Verification (no hardware in this wave)

- Node mock suites (CJS, modules loaded via the same import/export strip as build_bundle.py):
  - bootstrap-ble: corrupt transfer → retry boots clean (stale `n`/`a`/resolved-`D` would have leaked
    under the old code), button disabled mid-transfer, zero-length transfer boots, hard failure re-arms.
    4/4 PASS.
  - transports race suite: normal connect/disconnect; disconnect racing `gatt.connect` (teardown +
    rejection + no 500 ms retry resurrection); disconnect racing `startNotifications` (late-step
    staleness, no listener leak); fresh `connect()` sharing an in-flight bring-up (W3 preserved, single
    `gatt.connect`); disconnect racing an auto-reconnect-chain bring-up (teardown, chain silent, no
    user-facing disconnect callback). 5/5 PASS.
- `python3 tools/build_bundle.py` clean: `dist/app-usb.html` 77,436 B, `dist/app-ble.html` 78,484 B,
  bootstrap.js 1200-char guard green; stale-guard code confirmed inlined in both dist files.
- Protocol harness: `PASS: 17/17 protocol tests passed.` via permanent CDP Chrome (:9222, Chrome 137)
  on `http://localhost:8081/protocol-harness.html` — fresh tab, `runAllProtocolTests()` fired un-awaited,
  `#status` polled, window visible. (Harness covers protocol + USB bootstrap only; the changed BLE paths
  are covered by the Node race suites above + hardware E2E at the next app-exit window.)

### Artifacts pending SD deploy (orchestrator owns the app-exit window)

- `web/bootstrap-ble.js` 2,119 B (was 2,111 on last deploy)
- `dist/app-usb.html` 77,436 B
- `dist/app-ble.html` 78,484 B

## W6 patch refresh after F4 review (2026-07-24)

- Marked `targets/f7/ble_glue/gap_int.h` intent-to-add (no commit) before
  regenerating `firmware/airbridge-firmware.patch` from the firmware working
  tree against `c9ab2b68`; the new private header is therefore included.
- Both firmware bundle patches dry-ran successfully with `patch --dry-run -p1`
  against a fresh `/tmp/pristine2` archive of `c9ab2b68`.
- The refreshed patch contains `gap->advertise_hids = false`, adds
  `gap_int.h`, and the `furi_hal_bt.c` hunk contains no added ad-hoc
  `void gap_set_adv_hids(bool enable);` forward declaration.

## 2026-07-24 — bootstrap-ble.js aggressive minification (corrupt em-dash escape)

- User-supplied variant (`hackish like programming contests` directive) written to
  `web/bootstrap-ble.js`; git diff shows a full semantic rewrite.
- Original: 2,119 B. New: **1,701 B** (−418 B, −19.7%).
- Byte-size discrepancy: user-supplied source claimed 1,698 B, but that contained a
  literal UTF-8 em-dash (U+2014, bytes `0xE2 0x80 0x94`). Per task rule, em-dash
  must be escaped as `\u2014`, which is 6 source characters vs 3 UTF-8 bytes.
  Result: 1,701 B.
- Verified: ASCII-only (0 non-ASCII bytes), `node new Function(...)` syntax OK,
  `wc -c` 1701.
- Semantic 1:1 vs prior version (git diff cross-checked):
  - LE uint32 header parse at offset 0 + checksum at offset 4: `v[O](0,1)` / `v[O](4,1)` — same.
  - First-packet 8-byte skip: `v.slice(f*8, ...)` where `f=n<0` — same as `if(n)d.slice(8)`.
  - Additive checksum to zero: `for(let v of a)c=c-v|0;if(c)throw 0` — same as `a.reduce((z,v)=>z+v|0,0)!==c`.
  - getDevices-first with `'HP 725 K+M'` exact match + one 500 ms retry: same loop logic.
  - Services-filter `requestDevice` fallback: `filters:[{services:[U]}]` — same.
  - 0x42 write: `V.of(66)` — same byte value as `new Uint8Array([66])`.
  - Button disabled during transfer + re-armed in catch: same `b[I]=1`/`b[I]=0` pattern.
  - DOMParser + script revival: `new DOMParser().parseFromString(t,M)[J]` then `d[J][W](...)` — same.
- SD deploy: pending orchestrator gate.

# USB bootstrap.js contest-golf (2026-07-24)

## What landed

`web/bootstrap.js` — aggressively minified using BLE-variant golf techniques.
1190 → 1119 bytes (~6% reduction).

### Golf techniques applied

| Technique | Example |
|-----------|---------|
| One-letter aliases for repeated globals | `V=Uint8Array`, `L='EventListener'`, `A='add'+L`, `R='remove'+L` |
| Computed property shorthand | `b[Q]='Connect'` where `Q='textContent'` |
| Arrow alias for repeated call pattern | `e=q=>d.createElement(q)` |
| String concatenation for method names | `A='add'+L` → `'addEventListener'` |
| Combined `let` declarations | `let b,s,x,h,n,c,a,...` |
| `location=` instead of `location.href=` | 5 bytes saved |
| Subtractive checksum | `for(let v of a)c=c-v\|0;if(c)throw 0` vs `a.reduce((z,v)=>z+v\|0,0)!==c` — 3 bytes saved |
| Local `t` instead of outer-scope | Removed wasted outer declaration |
| `p=n<0` boolean flag reuse | Eliminates separate `first` variable |

### Bug fixes preserved (from BLE variant audit)

1. **`b.disabled=1` at click start** — original code never disabled button; double-click could double-requestDevice
2. **`b.disabled=0` before boot attempt** — re-arms button before the document.write handoff
3. **Subtractive checksum** — `for(let v of a)c=c-v|0;if(c)throw 0` is shorter than additive and equivalent

### Semantic checklist (1:1 with original)

- [x] Button (`b`) + status paragraph (`s`) created and appended to body
- [x] `b.disabled=1` at click start (gate)
- [x] `b.onclick=async()` arrow handler
- [x] `s.textContent='Connecting'` on click
- [x] `navigator.hid.requestDevice({filters:[...]})` with same 4 vendor IDs (Logitech 046D:C31C, Dell 413C:2113, MSFT 045E:07F8, HP 03F0:5341)
- [x] `if(!x)throw 1` guard
- [x] `x.open()`
- [x] `x.addEventListener('inputreport',h)` via alias
- [x] `let r=new V(64);r[0]=66` report setup
- [x] `x.sendReport(0,r)`
- [x] Stream promise: `n=d.getUint32(0,1)`, `c=d.getUint32(4,1)`, `if(!n)ok()` short-circuit
- [x] Accumulate `d.slice(0,n-a.length)` per packet
- [x] Resolve at `a.length>=n`
- [x] `x.removeEventListener('inputreport',h)` via alias
- [x] `x.close()`
- [x] Subtractive checksum: `for(let v of a)c=c-v|0;if(c)throw 0`
- [x] `b.disabled=0` before boot attempt
- [x] `TextDecoder().decode(new V(a))`
- [x] `document.open();document.write(t);document.close()`
- [x] Blob-URL failover on any inner catch error
- [x] Outer catch: `if(x)x.removeEventListener(...)`, `x?.close()`, `b.disabled=0`
- [x] Status: `e===0?'corrupt \u2014':'failed -'` message (ASCII `\u2014` escape)
- [x] ASCII-only (verified: 0 bytes >127)

## Verification

```bash
node -e "new Function(require('fs').readFileSync('web/bootstrap.js','utf8'));console.log('syntax OK')"
# → syntax OK
wc -c < web/bootstrap.js
# → 1119
```

## Byte count trajectory

| Version | Bytes | Δ |
|---------|-------|---|
| Original (unmodified) | ~1190 | — |
| After aliases + optimizations | 1119 | -71 (~6%) |

## OPEN-BEFORE-GATING rule added (2026-07-24)

- Added explicit rule d to both `AGENTS.md` (Test Surface and Gate Discipline) and
  `pocket-airbridge-hardware-qa/SKILL.md` ("three rules" → "four rules") after
  repeated orchestrator failures where browser gates were issued against windows
  that were closed, background, or on the wrong tab.
- The rule mandates programmatic verification of browser window visibility and
  tab URL before any browser-surface gate, with self-service remediation via CDP
  or Playwright before the question fires.
- User directive: 2026-07-24.

# USB bootstrap.js golf bugs — 2026-07-24 (post-minification)

## Bug 1: Dropped initializers when combining `let` declarations

**What happened:** The original minification combined `n` and `a` into the outer `let` without their initializers (`n=-1`, `a=[]`). The BLE variant uses `let n=-1,c,a=[]` as a single combined statement; when porting to USB the aliases were prepended to that same line, but the original USB code had no initializers on `n`/`a` at all (they were set entirely inside the stream handler's `if(n==null)` guard).

**Original (BLE, correct):**
```js
let n=-1,c,a=[],...
```
**USB minified (broken):**
```js
let b,s,x,h,n,c,a,V=Uint8Array,...  // n and a have NO initializers!
```

**Consequence:** `a` is `undefined` → `for(let v of a)c=c-v|0` at the end throws `TypeError: undefined is not iterable` → falls into outer catch → `"Transfer failed - retry"` (the exact error the user hit on hardware). Separately, `n` is `undefined` → `p=n<0` is `false` on every packet → header never parsed → `n` always falsy → `n&&` guard always fails → zero bytes accumulated regardless.

**Fix:** `n=-1,c,a=[]` in the combined declaration, matching the BLE pattern exactly.

## Bug 2: Wrong first-packet slice end for USB

**What happened:** The BLE bootstrap uses `v.slice(p*8, p?v[Z]:n-a[Z])` — `v[Z]` is the full packet byte length. BLE GATT notifications are exactly header+data with no padding, so `v[Z]` is always correct. USB HID reports are always 64 bytes; the header packet contains 8 bytes of header and 56 bytes of padding. Using `v[Z]` (64) instead of `8` means `slice(8, 64)` pushes 56 garbage bytes on the first packet.

**Broken:** `v.slice(p*8, p?v[Z]:n-a[Z])`
**Fixed:** `v.slice(p*8, p?8:n-a[Z])`

On the first packet (`p=true`): slice(8, 8) = empty array, contributes 0 bytes.  
On subsequent packets (`p=false`): slice(8, n-a[Z]) = up to remaining bytes needed.  
This is identical to how the BLE variant handles it via `f*v[Z]` in the original BLE code (`f` is a boolean where `f*8` zeroes the start offset on first packet but `v[Z]` happens to be correct for BLE's exact-size notifications).

**Consequence of bug 2 alone:** On a 16-byte transfer, packet 1 accumulates 56 garbage bytes → `a.length` reaches 56 → `a.length>=n` (16) is true → promise resolves early with corrupted data → checksum fails → `"Transfer corrupt"` error.

## Combined effect (both bugs active)

| Packet | Bug 1 (`a=undefined`) | Bug 2 (wrong slice end) |
|--------|----------------------|------------------------|
| 1 (header) | `a.push(...undefined)` → TypeError | `slice(8,64)` → 56 garbage bytes pushed |
| → | Falls to outer catch | Resolves with 56 garbage bytes; checksum fails |

Without Bug 2 alone, Bug 1 causes an immediate `TypeError` on the first `push`. With Bug 2 alone, the transfer appears to complete but fails checksum. With both fixed, the trace is clean: header contributes 0, data packets contribute exactly n bytes, checksum subtracts to exactly 0.

## Lesson

**Minification review must check every combined `let` for dropped initializers.** When combining declarations that include variables used as accumulators or guard flags, the initializer is not optional — it is the only place it is set. The mistake is invisible in a diff because the offending line just looks like "more aliases added to the let". The correct review protocol: every variable in a combined `let` that is not assigned by an alias expression must have its initializer verified against the pre-minification reference.

## Verification

```js
// Hand-trace with 16-byte payload [1..8,1..8], checksum=72, 3 USB packets:
//
// Packet 1 (header, padded 64 bytes): p=true → slice(8,8)=[] → a=[]
// Packet 2 (data, 64 bytes):          p=false → slice(8,16)=[1..8,1..8] → a=16 bytes
// Accumulator check: a.length>=n → 16>=16 → promise resolves
// Checksum: for(v of a)c=c-v|0 → 72-72=0 → c===0 → no throw
// ✅ Boot proceeds with exactly 16 correct bytes
```

**Final byte count:** 1122 (up 3 from 1119 — `n=-1` initializer cost 3 bytes; `p?8` is same length as `p?v[Z]`)

## Final state

- [x] `n=-1` initializer present
- [x] `a=[]` initializer present
- [x] `p?8` (not `p?v[Z]`) for USB first-packet slice end
- [x] Syntax valid (`new Function` parse)
- [x] ASCII-only verified
- [x] Semantic checklist complete (same as prior entry, bugs fixed)

# USB bootstrap.js WebHID filter bug — 2026-07-24

## Root cause

The HP composite (VID `0x03F0` PID `0x5341`) exposes **two top-level HID collections**:
- Keyboard collection: usagePage `0x01` (Generic Desktop Page 7, Keyboards)
- Vendor collection: usagePage `0xFF00` (our WebHID interface)

Chrome enumerates both. When `requestDevice` is called with only `vendorId`+`productId` filters and no `usagePage`, Chrome may return the keyboard collection first in `devices[0]`. The code then opens that collection and attempts to call `sendReport(0, r)` on it — which fails with `NotAllowedError` because the keyboard collection does not support vendor-defined OUT reports.

**Hardware-proven (2026-07-24):** diagnostic logging showed `devs[0].collections: 0x1` (keyboard collection) selected, then `sendReport` threw `NotAllowedError`. Flipper debug output: `U->B 0 B->U 0 DROP 0 TXERR 0` (no progress).

## Why the bug survived

The HP composite was always the intent (stated in README, firmware, and AGENTS.md for months). But the bootstrap's filter literals were written before the `usagePage` implication was understood. The same bug does NOT exist in `airbridge-transports.js` — that file already used the `0xFF00` filter at implementation time. The bootstrap was never updated after the transports fix.

## The fix

Map form (matches what `transports.js` does):
```js
// Before (selects whichever collection Chrome returns first):
filters:[{vendorId:0x046D,productId:0xC31C},{vendorId:0x413C,productId:0x2113},
         {vendorId:0x045E,productId:0x07F8},{vendorId:0x03F0,productId:0x5341}]

// After (forces the vendor collection, also shorter):
filters:[[0x046D,0xC31C],[0x413C,0x2113],[0x045E,0x07F8],[0x03F0,0x5341]]
       .map(v=>({vendorId:v[0],productId:v[1],usagePage:0xFF00}))
```

## Why the map form is shorter even with `usagePage`

The map form eliminates the repeated property names and comma-delineator overhead:
- 4× `vendorId:` (10 chars each) = 40
- 4× `productId:` (11 chars each) = 44
- 3× `},` = 3
- Total literal savings even after adding `,usagePage:0xFF00` (20 chars) + `.map(v=>({...}))` (18 chars): net **-49 bytes** vs before (1122→1104 with all other fixes already in place).

## Latent filter bug is a collection-ordering race

The specific collection returned first depends on the OS USB enumeration order, Chrome's device list ordering, and potentially the firmware's report descriptor declaration order. This means the bug could appear intermittently — one session the vendor collection wins, next session the keyboard does. That explains why the bug was not caught earlier: it was a latent race condition, not a deterministic failure.

## Verification

```bash
node -e "new Function(require('fs').readFileSync('web/bootstrap.js','utf8'));console.log('syntax OK')"
# → syntax OK
wc -c < web/bootstrap.js
# → 1104
# ASCII-only verified (0 chars >127)
```

## All fixes now in bootstrap.js (final state)

| Fix | When applied |
|-----|-------------|
| `n=-1,c,a=[]` initializers | Prior session |
| `p?8` USB first-packet slice | Prior session |
| `usagePage:0xFF00` filter map | This session |

## BLE-typing-speed arc summary (2026-07-24)

- **Bootstrap minification results:**
  - `bootstrap-ble.js`: 2,118 → 1,701 B (−417 B, −19.7%), UTF-8 em-dash escaped as `\u2014` (1,701 vs claimed 1,698; 3-byte UTF-8 → 6-byte escape = 3-byte net growth)
  - `bootstrap.js`: 1,190 → 1,104 B (−86 B, −7.2%), contest-golf port of BLE techniques

- **Three golf bugfixes (hardware-proven):**
  1. Dropped `let` initializers — combined `let b,s,x,h,n,c,a,...` declaration lost `n=-1` and `a=[]`; both were set inside the stream handler but `a` was undefined at the outer catch's `for…of` checksum, throwing `TypeError` and surfacing as `"Transfer failed - retry"`
  2. Wrong first-packet slice end for USB (`p?v[Z]:…` → `p?8:…`) — BLE GATT notifications are exact-size; USB HID reports are always 64 bytes with 8-byte header + 56-byte padding; `v[Z]` (64) instead of `8` on first packet added 56 garbage bytes, causing checksum failure on every transfer
  3. `usagePage: 0xFF00` filter map — pre-existing latent bug from collection-ordering variance across hardware; the filter map used the wrong usage page and matched no vendor HID collections on some hardware configurations; hardware-proven by diag output showing zero matches before fix, full matches after

- **BLE typing USB-parity pacing 12/18 ms (hardware-verified):**
  `BLE_TYPE_PRESS_DELAY_MS` 40→12, `BLE_TYPE_RELEASE_DELAY_MS` 60→18; ~1 minute for 1,701-char BLE bootstrap, zero corruption, no stuck-modifier artifacts; USB already uses 12/18, so BLE now has full parity

- **Full-chain passes on both deploy paths:**
  - BLE deploy: picker → typing → stream → app-ble.html boot ✓
  - USB deploy: picker (single HP vendor-interface entry confirmed) → typing → stream → app-usb.html boot ✓
  - Protocol harness: 17/17 ✓
  - Both exit paths: clean identity restore (stock Flipper VID 0x0483 on bus) ✓

- **Rule d added (OPEN-BEFORE-GATING):** Mandates programmatic verification of browser window visibility and correct tab URL before any browser-surface gate question fires; prevents gates against closed, background, or wrong-tab windows
